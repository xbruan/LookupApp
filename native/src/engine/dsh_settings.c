/* 见 dsh_settings.h —— 设置模型的实现。
 * 内存形状：**一个 arena + 一块字典数组**，所有字符串（id / 名字 / 路径）都在 arena 里 ⇒
 * 「整份设置」只算**两次**活分配（句柄 + arena 块），`dsh_settings_free` 一次还清。
 */
#include "engine/dsh_settings.h"
#include "dict/dsh_dict_id.h"
#include "dsh_internal.h"
#include "json_reader.h"
#include "json_writer.h"
#include "platform/dsh_time.h"

#include <stdio.h>
#include <string.h>

#define DSH_SETTINGS_CHUNK 4096u
/* 词库里最多认这么多本（挡恶意/损坏输入；正常用户几十本） */
#define DSH_SETTINGS_MAX_DICTS 4096u
/* 内核不认识的顶层键最多原样保留这么多个 */
#define DSH_SETTINGS_MAX_FOREIGN 64u

typedef struct settings_arena {
  struct settings_arena *next;
  size_t used;
  size_t cap;
  uint8_t bytes[];
} settings_arena;

/*
 * 一层「原样搬运」的条目（未知顶层键、以及 `speech` / `volcengine` / `translate` 里没建模的子键）。
 * 名字与值都指向 arena（生存期跟着设置走）。⚠️ 子键也要搬：声明一个对象是「内核认识的键」= **接管它整个对象**，
 * 里面任何没建模的子键都会在下次存盘时**凭空消失** —— 那正是「用户设置莫名丢了」。
 */
typedef struct {
  const char *key;
  const char *raw;
  size_t raw_len;
} foreign_pair;

struct dsh_settings {
  settings_arena *arena;
  dsh_stored_dict *dicts;
  int64_t dict_count;
  char *current_dict_id; /* 空串 = 没指定 */
  dsh_close_behavior close_behavior;
  int show_floating_on_startup; /* 启动时显示悬浮窗；默认 1 */
  dsh_speech_settings speech;
  char *volcengine_api_key; /* 借用arena；NULL = 没填 */
  dsh_translate_settings translate;
  foreign_pair foreign[DSH_SETTINGS_MAX_FOREIGN];
  int64_t foreign_count;
  /* `speech` / `volcengine` 里**没建模**的子键（原样搬运，绝不丢）*/
  foreign_pair speech_extra[DSH_SETTINGS_MAX_FOREIGN];
  int64_t speech_extra_count;
  foreign_pair volcengine_extra[DSH_SETTINGS_MAX_FOREIGN];
  int64_t volcengine_extra_count;
  /* `translate` 里**没建模**的子键（同上；老配置残留的 `dictionaryFirst` 就走这条）*/
  foreign_pair translate_extra[DSH_SETTINGS_MAX_FOREIGN];
  int64_t translate_extra_count;
};

/* ── arena ─────────────────────────────────────────────────────────────── */

static void *arena_alloc(dsh_settings *s, size_t size, size_t align) {
  if (align > 8) align = 8;
  settings_arena *a = s->arena;
  size_t off = (a != NULL) ? ((a->used + (align - 1)) & ~(align - 1)) : 0;
  if (a == NULL || off + size > a->cap) {
    const size_t need = (size + align > DSH_SETTINGS_CHUNK) ? (size + align) : DSH_SETTINGS_CHUNK;
    settings_arena *n = (settings_arena *)dsh_mem_alloc(sizeof(*n) + need);
    if (n == NULL) return NULL;
    n->next = s->arena;
    n->used = 0;
    n->cap = need;
    s->arena = n;
    a = n;
    off = 0;
  }
  void *p = a->bytes + off;
  a->used = off + size;
  return p;
}

static char *arena_strndup(dsh_settings *s, const char *text, size_t len) {
  char *p = (char *)arena_alloc(s, len + 1, 1);
  if (p == NULL) return NULL;
  if (len > 0 && text != NULL) memcpy(p, text, len);
  p[len] = '\0';
  return p;
}

static char *arena_strdup(dsh_settings *s, const char *text) {
  if (text == NULL) text = "";
  return arena_strndup(s, text, strlen(text));
}

static void arena_release_all(dsh_settings *s) {
  settings_arena *a = s->arena;
  while (a != NULL) {
    settings_arena *next = a->next;
    dsh_release(a);
    a = next;
  }
  s->arena = NULL;
}

/* ── 小工具 ─────────────────────────────────────────────────────────────── */

/** 去掉首尾空白；两头都空时返回 NULL（与参考实现的 IsNullOrWhiteSpace 同一约定） */
static const char *trim_or_null(const char *text) {
  if (text == NULL) return NULL;
  while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') text++;
  if (*text == '\0') return NULL;
  const char *end = text + strlen(text);
  while (end > text) {
    const char c = end[-1];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
    end--;
  }
  /* 需要截断时用静态缓冲不安全 —— 调用方在本文件里都是「先 trim 判断、再 copy 长度」，
   * 所以这里只做判断用途；真正落盘的值走 `trim_copy`。 */
  return (*end == '\0') ? text : text;
}

/** 把一段（可带首尾空白的）文本复制进 arena，并返回去掉空白之后的长度。
 * ⚠️ 返回 `char *` 而不是 `const char *`：它指向**我们自己分配**的可写内存，调用方常直接赋给
 *    `char *` 字段；返回 const 会在那些赋值点触发 `-Wdiscarded-qualifiers`，而硬转掉 const 更坏。 */
static char *trim_copy(dsh_settings *s, const char *text, size_t *out_len) {
  if (out_len != NULL) *out_len = 0;
  if (text == NULL) return NULL;
  const char *b = text;
  while (*b == ' ' || *b == '\t' || *b == '\n' || *b == '\r') b++;
  const char *e = b + strlen(b);
  while (e > b) {
    const char c = e[-1];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
    e--;
  }
  const size_t len = (size_t)(e - b);
  if (len == 0) return NULL; /* 全空白 = 没填 */
  if (out_len != NULL) *out_len = len;
  return arena_strndup(s, b, len);
}

/* ── 解析 ───────────────────────────────────────────────────────────────── */

/** 内核认识的那些顶层键（用于「其余的原样搬运」） */
static int is_known_key(const char *key, size_t len) {
  static const char *known[] = {"version",   "dictionaries",  "currentDictId",
                                "closeBehavior", "showFloatingOnStartup",
                                "speech",    "volcengine",    "translate"};
  for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
    if (strlen(known[i]) == len && memcmp(known[i], key, len) == 0) return 1;
  }
  return 0;
}

/** `translate` 里内核**建模了**的子键（其余的走原样搬运）*/
static int is_known_translate_key(const char *key, size_t len) {
  static const char *known[] = {"enabled", "targetMode", "autoTranslate"};
  for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
    if (strlen(known[i]) == len && memcmp(known[i], key, len) == 0) return 1;
  }
  return 0;
}

/* ── 枚举写法的比较（`auto` / `zh` / `en` 这些值的大小写不敏感比对）────────── */

/** 大小写不敏感的整串比较 */
static int eq_nocase(const char *a, size_t a_len, const char *b) {
  const size_t b_len = strlen(b);
  if (a_len != b_len) return 0;
  for (size_t i = 0; i < a_len; i++) {
    char ca = a[i];
    char cb = b[i];
    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
    if (ca != cb) return 0;
  }
  return 1;
}

/** `speech` 里内核**建模了**的子键（其余的走原样搬运）*/
static int is_known_speech_key(const char *key, size_t len) {
  static const char *known[] = {
      "rate",         "voiceId",           "accent",          "defaultLanguage",
      "doubaoApiKey", "doubaoResourceId",  "doubaoFormat",    "doubaoSpeakerEn",
      "doubaoSpeakerZh", "doubaoLoudnessEn", "doubaoLoudnessZh"};
  for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
    if (strlen(known[i]) == len && memcmp(known[i], key, len) == 0) return 1;
  }
  return 0;
}

static int is_known_volcengine_key(const char *key, size_t len) {
  return (len == 6 && memcmp("apiKey", key, 6) == 0) ? 1 : 0;
}

/**
 * 把对象里**没建模**的子键原样记下来（原因见 `foreign_pair` 那段）。
 * `merge`：0 = 读设置文件（`count` 一开始是 0，直接追加）；1 = **打补丁**（同名的**替换**、不同的才追加）。
 * ⚠️ 少了 merge 这一步，补丁里出现的那个未建模子键会在存盘时**出现两遍**，下次读回来谁赢全看解析器。
 */
static int keep_extra(dsh_settings *s, const dsh_json_node *obj, foreign_pair *slots,
                      int64_t *count, int (*known)(const char *, size_t), const char *what,
                      int merge) {
  if (obj == NULL || !dsh_json_is_object(obj)) return 0;
  const int64_t n = dsh_json_object_len(obj);
  for (int64_t i = 0; i < n; i++) {
    const char *key = NULL;
    size_t key_len = 0;
    const dsh_json_node *v = dsh_json_object_at(obj, i, &key, &key_len);
    if (v == NULL || key == NULL) continue;
    if (known(key, key_len)) continue;
    size_t raw_len = 0;
    const char *raw = dsh_json_node_raw(v, &raw_len);
    int replaced = 0;
    if (merge) {
      for (int64_t k = 0; k < *count; k++) {
        if (strlen(slots[k].key) == key_len && memcmp(slots[k].key, key, key_len) == 0) {
          slots[k].raw = arena_strndup(s, raw, raw_len);
          slots[k].raw_len = raw_len;
          if (slots[k].raw == NULL) {
            dsh_set_last_error("内存不足：替换 %s 的子键（%s）", what, key);
            return -1;
          }
          replaced = 1;
          break;
        }
      }
    }
    if (replaced) continue;
    if (*count >= (int64_t)DSH_SETTINGS_MAX_FOREIGN) {
      dsh_set_last_error("设置里 %s 的子键太多（超过 %u 个），拒绝解析以免丢数据", what,
                         DSH_SETTINGS_MAX_FOREIGN);
      return -1;
    }
    foreign_pair *fp = &slots[*count];
    fp->key = arena_strndup(s, key, key_len);
    fp->raw = arena_strndup(s, raw, raw_len);
    fp->raw_len = raw_len;
    if (fp->key == NULL || fp->raw == NULL) {
      dsh_set_last_error("内存不足：保存 %s 的子键（%s）", what, key);
      return -1;
    }
    (*count)++;
  }
  return 0;
}

/** 取一个字符串子键（trim 之后写进 arena）；不存在返回 NULL。
 * ⚠️ **「写了空串」与「没写」必须分开**：`doubaoSpeakerEn` 那一档空串是用户的明确选择（「这个场景不要在线念」），
 *    字段从没设过才该填内置默认音色 —— 所以只对「没有这个键 / 不是字符串」回 NULL，「键在但是空串」回空串。 */
static const char *speech_str(dsh_settings *s, const dsh_json_node *obj, const char *name,
                              int *present) {
  const dsh_json_node *v = dsh_json_object_get(obj, name);
  if (present != NULL) *present = (v != NULL) ? 1 : 0;
  if (v == NULL || !dsh_json_is_string(v)) return NULL;
  const char *text = dsh_json_str_value(v, NULL);
  const char *trimmed = trim_copy(s, text, NULL);
  return (trimmed != NULL) ? trimmed : arena_strdup(s, "");
}

/**
 * 把 `speech` / `volcengine` 两节应用到 `s` 上（约定照参考实现的 `NormalizeSpeech`）：
 * `rate` 夹到 -10..10；`accent` 只认 uk / us，其余一律 auto；`voiceId` / `doubaoApiKey` trim 之后空的当没填；
 * `defaultLanguage` **一律清成 NULL**；响度补偿夹到 -50..100（没设过就保持没设过）；Key 两个字段**镜像**同值。
 * `reset_defaults`：**1 = 从零开始**（读设置文件那条路，先摆默认值再按文件里的键覆盖）；
 * **0 = 从现值开始**（打补丁那条路，**只改补丁里出现的键**）。⚠️ 少了 merge 语义就会**不报错地丢弃** ——
 * 第一版把这两个顶层键当「内核认识的键」跳过，于是改口音 / 改音色 / 填 Key 一条都存不进去。
 */
static int merge_speech(dsh_settings *s, const dsh_json_node *speech,
                        const dsh_json_node *volcengine, int reset_defaults) {
  dsh_speech_settings *out = &s->speech;
  if (reset_defaults) {
    out->rate = 0;
    out->accent = "auto";
    out->doubao_resource_id = DSH_DOUBAO_DEFAULT_RESOURCE_ID;
    out->doubao_format = DSH_DOUBAO_DEFAULT_FORMAT;
    out->doubao_speaker_en = DSH_DOUBAO_DEFAULT_SPEAKER_EN;
    out->doubao_speaker_zh = DSH_DOUBAO_DEFAULT_SPEAKER_ZH;
    out->voice_id = NULL;
    out->doubao_api_key = NULL;
    out->has_loudness_en = 0;
    out->has_loudness_zh = 0;
    out->loudness_en = 0;
    out->loudness_zh = 0;
  }

  int speech_present = 0;      /* 补丁里到底提到了哪几个会动到「镜像」的键 */
  int api_key_present = 0;

  if (speech != NULL && dsh_json_is_object(speech)) {
    int64_t rate = 0;
    if (dsh_json_i64_value(dsh_json_object_get(speech, "rate"), &rate) == 0) {
      if (rate < -10) rate = -10;
      if (rate > 10) rate = 10;
      out->rate = (int)rate;
    }
    {
      int present = 0;
      const char *accent = speech_str(s, speech, "accent", &present);
      if (present || reset_defaults) {
        /* 只认 uk / us；其余（含 auto / 空 / 脏值）一律 auto */
        if (accent != NULL && strcmp(accent, "uk") == 0) out->accent = "uk";
        else if (accent != NULL && strcmp(accent, "us") == 0) out->accent = "us";
        else out->accent = "auto";
      }
    }
    {
      int present = 0;
      const char *voice = speech_str(s, speech, "voiceId", &present);
      if (present || reset_defaults) {
        out->voice_id = voice;
        if (out->voice_id != NULL && out->voice_id[0] == '\0') out->voice_id = NULL;
      }
    }
    /* `defaultLanguage` 读进来直接丢掉（这是**有意的行为变更**，见 `merge_speech` 那段的约定）*/
    {
      int present = 0;
      const char *key = speech_str(s, speech, "doubaoApiKey", &present);
      if (present) {
        api_key_present = 1;
        out->doubao_api_key = key;
        if (out->doubao_api_key != NULL && out->doubao_api_key[0] == '\0') {
          out->doubao_api_key = NULL;
        }
      } else if (reset_defaults) {
        out->doubao_api_key = NULL;
      }
    }
    {
      int present = 0;
      const char *rid = speech_str(s, speech, "doubaoResourceId", &present);
      if (present || reset_defaults) {
        if (rid != NULL && rid[0] != '\0') out->doubao_resource_id = rid;
      }
      present = 0;
      const char *fmt = speech_str(s, speech, "doubaoFormat", &present);
      if (present || reset_defaults) {
        if (fmt != NULL && fmt[0] != '\0') out->doubao_format = fmt;
      }
    }
    {
      int present = 0;
      const char *en = speech_str(s, speech, "doubaoSpeakerEn", &present);
      if (present) {
        if (en != NULL) out->doubao_speaker_en = en; /* NULL → 默认值；空串 → 留着 */
      } else if (reset_defaults) {
        out->doubao_speaker_en = DSH_DOUBAO_DEFAULT_SPEAKER_EN;
      }
      present = 0;
      const char *zh = speech_str(s, speech, "doubaoSpeakerZh", &present);
      if (present) {
        if (zh != NULL) out->doubao_speaker_zh = zh;
      } else if (reset_defaults) {
        out->doubao_speaker_zh = DSH_DOUBAO_DEFAULT_SPEAKER_ZH;
      }
    }
    {
      int64_t v = 0;
      if (dsh_json_i64_value(dsh_json_object_get(speech, "doubaoLoudnessEn"), &v) == 0) {
        if (v < -50) v = -50;
        if (v > 100) v = 100;
        out->has_loudness_en = 1;
        out->loudness_en = (int)v;
      } else if (reset_defaults) {
        out->has_loudness_en = 0;
      }
      if (dsh_json_i64_value(dsh_json_object_get(speech, "doubaoLoudnessZh"), &v) == 0) {
        if (v < -50) v = -50;
        if (v > 100) v = 100;
        out->has_loudness_zh = 1;
        out->loudness_zh = (int)v;
      } else if (reset_defaults) {
        out->has_loudness_zh = 0;
      }
    }
  }

  /* 账号级的 Key */
  const char *account_key = NULL;
  int account_present = 0;
  if (volcengine != NULL && dsh_json_is_object(volcengine)) {
    account_key = speech_str(s, volcengine, "apiKey", &account_present);
    if (account_key != NULL && account_key[0] == '\0') account_key = NULL;
    if (account_present) speech_present = 1;
  }

  /* ⚠️ 补丁语义：**两个 Key 字段都没在补丁里出现时，镜像不能动** ——
   * 第一版无条件走 `else { s->volcengine_api_key = NULL; }`，「只改语速」的补丁会把已填好的 Key 抹掉。 */
  const int touch_mirror = reset_defaults || account_present || api_key_present;
  if (touch_mirror) {
    if (account_key != NULL) {
      s->volcengine_api_key = (char *)account_key;
      out->doubao_api_key = account_key;
    } else if (out->doubao_api_key != NULL) {
      s->volcengine_api_key = (char *)out->doubao_api_key;
    } else {
      s->volcengine_api_key = NULL;
    }
  }
  (void)speech_present;

  if (keep_extra(s, speech, s->speech_extra, &s->speech_extra_count, is_known_speech_key,
                 "speech", !reset_defaults) != 0) {
    return -1;
  }
  if (keep_extra(s, volcengine, s->volcengine_extra, &s->volcengine_extra_count,
                 is_known_volcengine_key, "volcengine", !reset_defaults) != 0) {
    return -1;
  }
  return 0;
}

/** 读设置文件那条路：从默认值开始解析（`merge_speech` 的 reset 用法） */
static int parse_speech(dsh_settings *s, const dsh_json_node *speech,
                        const dsh_json_node *volcengine) {
  return merge_speech(s, speech, volcengine, 1);
}


/**
 * 机器翻译那一节（约定写在 `dsh_settings.h` 的 `dsh_translate_settings` 那一节）。
 * `reset_defaults` 的语义与 `merge_speech` **逐字相同**：1 = 读设置文件（缺的落默认值）；
 * 0 = 打补丁（**只改出现的子键** —— 「只关掉开关」不许把目标语种打回 auto）。
 * ⚠️ 同样要**留下没建模的子键**：声明 `translate` 是内核认识的键 = 接管整个对象，
 * 老配置里残留的 `dictionaryFirst` 以及将来平台层往里塞的字段都不能因为内核不认识就被抹掉。
 */
static int merge_translate(dsh_settings *s, const dsh_json_node *tr, int reset_defaults) {
  const dsh_json_node *enabled = (tr != NULL) ? dsh_json_object_get(tr, "enabled") : NULL;
  const dsh_json_node *auto_tr = (tr != NULL) ? dsh_json_object_get(tr, "autoTranslate") : NULL;
  const dsh_json_node *mode = (tr != NULL) ? dsh_json_object_get(tr, "targetMode") : NULL;

  /* 目标语种**永远非空**（认不出就落 auto）：缺值那条路一律落默认 ——
   * 「取消设置」对目标语种的含义就是「回到自动判断」。 */
  if (s->translate.target_mode == NULL) s->translate.target_mode = arena_strdup(s, "auto");
  if (s->translate.target_mode == NULL) {
    dsh_set_last_error("内存不足：翻译目标语种");
    return -1;
  }
  if (reset_defaults) {
    /* 总开关**默认关**（要联网、要把文本发给第三方）；自动翻译**默认开** */
    s->translate.enabled = 0;
    s->translate.auto_translate = 1;
  }

  if (enabled != NULL && !dsh_json_is_null(enabled)) {
    if (dsh_json_is_bool(enabled)) {
      s->translate.enabled = dsh_json_bool_value(enabled) ? 1 : 0;
    } else {
      dsh_set_last_error("translate.enabled 不是布尔值，已按原值处理");
    }
  }
  if (auto_tr != NULL && !dsh_json_is_null(auto_tr)) {
    if (dsh_json_is_bool(auto_tr)) {
      s->translate.auto_translate = dsh_json_bool_value(auto_tr) ? 1 : 0;
    } else {
      dsh_set_last_error("translate.autoTranslate 不是布尔值，已按原值处理");
    }
  }
  if (mode != NULL) {
    /*
     * ⚠️ **只认 auto / zh / en**（大小写不敏感）：它是从设置文件里读出来的，
     * 脏数据不许变成「发一个乱码目标语种给服务端」。认不出**不报错**（其余设置仍然可用），落回 `auto` 并说出原因。
     */
    const char *raw = dsh_json_is_string(mode) ? dsh_json_str_value(mode, NULL) : NULL;
    if (raw == NULL || raw[0] == '\0') {
      s->translate.target_mode = arena_strdup(s, "auto");
    } else if (eq_nocase(raw, strlen(raw), "zh")) {
      s->translate.target_mode = arena_strdup(s, "zh");
    } else if (eq_nocase(raw, strlen(raw), "en")) {
      s->translate.target_mode = arena_strdup(s, "en");
    } else if (eq_nocase(raw, strlen(raw), "auto")) {
      s->translate.target_mode = arena_strdup(s, "auto");
    } else {
      dsh_set_last_error("translate.targetMode 只认 auto / zh / en，已落回 auto");
      s->translate.target_mode = arena_strdup(s, "auto");
    }
    if (s->translate.target_mode == NULL) {
      dsh_set_last_error("内存不足：翻译目标语种");
      return -1;
    }
  }

  if (tr != NULL && keep_extra(s, tr, s->translate_extra, &s->translate_extra_count,
                               is_known_translate_key, "translate", !reset_defaults) != 0) {
    return -1;
  }
  return 0;
}

static int parse_dicts(dsh_settings *s, const dsh_json_node *arr) {
  const int64_t n = dsh_json_array_len(arr);
  if (n <= 0) return 0;
  if (n > (int64_t)DSH_SETTINGS_MAX_DICTS) {
    dsh_set_last_error("设置里的词库太多（%lld 本，上限 %u）", (long long)n,
                       DSH_SETTINGS_MAX_DICTS);
    return -1;
  }
  dsh_stored_dict *dicts = (dsh_stored_dict *)arena_alloc(s, (size_t)n * sizeof(*dicts), 8);
  if (dicts == NULL) {
    dsh_set_last_error("内存不足：词库清单需要 %lld 项", (long long)n);
    return -1;
  }
  memset(dicts, 0, (size_t)n * sizeof(*dicts));

  int64_t out = 0;
  for (int64_t i = 0; i < n; i++) {
    const dsh_json_node *item = dsh_json_array_at(arr, i);
    if (!dsh_json_is_object(item)) continue; /* 脏项直接跳过（参考实现也是这么做的） */
    const char *id = NULL;
    size_t id_len = 0;
    if (dsh_json_is_string(dsh_json_object_get(item, "id"))) {
      id = trim_copy(s, dsh_json_str_value(dsh_json_object_get(item, "id"), NULL), &id_len);
    }
    const char *mdx = NULL;
    size_t mdx_len = 0;
    if (dsh_json_is_string(dsh_json_object_get(item, "mdxPath"))) {
      mdx = trim_copy(s, dsh_json_str_value(dsh_json_object_get(item, "mdxPath"), NULL), &mdx_len);
    }
    /* ⚠️ 没有路径的条目**丢掉**：留下它就会在界面上出现一本点不开的词典 */
    if (mdx == NULL || mdx_len == 0) continue;

    dsh_stored_dict *d = &dicts[out];
    d->id = (id != NULL) ? id : arena_strdup(s, "");
    d->mdx_path = mdx;
    if (dsh_json_is_string(dsh_json_object_get(item, "title"))) {
      d->title = trim_copy(s, dsh_json_str_value(dsh_json_object_get(item, "title"), NULL), NULL);
    }
    if (d->title == NULL) d->title = arena_strdup(s, "");
    d->custom_title =
        dsh_json_is_string(dsh_json_object_get(item, "customTitle"))
            ? trim_copy(s, dsh_json_str_value(dsh_json_object_get(item, "customTitle"), NULL),
                        NULL)
            : NULL;
    {
      int64_t at = 0;
      if (dsh_json_i64_value(dsh_json_object_get(item, "addedAt"), &at) == 0 && at > 0) {
        d->added_at = at;
      } else {
        d->added_at = dsh_now_ms(); /* 老配置里没有这个字段 / 是 0 → 补当前时间 */
      }
    }
    /* 资源卷：数组；每一项必须是字符串 */
    {
      const dsh_json_node *mdd = dsh_json_object_get(item, "mddPaths");
      const int64_t m = dsh_json_array_len(mdd);
      if (m > 0) {
        const char **paths = (const char **)arena_alloc(s, (size_t)m * sizeof(char *), 8);
        if (paths == NULL) {
          dsh_set_last_error("内存不足：资源卷列表需要 %lld 项", (long long)m);
          return -1;
        }
        int64_t kept = 0;
        for (int64_t k = 0; k < m; k++) {
          const dsh_json_node *e = dsh_json_array_at(mdd, k);
          if (!dsh_json_is_string(e)) continue;
          const char *p = trim_copy(s, dsh_json_str_value(e, NULL), NULL);
          if (p == NULL) continue;
          paths[kept++] = p;
        }
        d->mdd_paths = paths;
        d->mdd_count = kept;
      }
    }
    out++;
  }
  s->dicts = dicts;
  s->dict_count = out;
  return 0;
}

int dsh_settings_parse(const char *json, size_t len, dsh_settings **out) {
  if (out == NULL) {
    dsh_set_last_error("dsh_settings_parse：out 不能为空");
    return -1;
  }
  *out = NULL;

  dsh_settings *s = (dsh_settings *)dsh_mem_alloc(sizeof(*s));
  if (s == NULL) {
    dsh_set_last_error("内存不足：设置句柄");
    return -1;
  }
  memset(s, 0, sizeof(*s));
  s->current_dict_id = NULL;
  s->close_behavior = DSH_CLOSE_ASK; /* 非法值一律退回 ask */
  /*
   * ⚠️ 窗口级那几个开关的默认值**必须在这里落**（不是在「没有设置文件」那条分支里）：
   *    一份设置文件里**没有**这个键（老版本写的、手写的）走的是**下面**那条正常解析路 ——
   *    只 memset 成 0 的话，「没设过」会被当成 `false`，「启动时不显示悬浮窗」就凭空生效。
   */
  s->show_floating_on_startup = 1;

  /* 没有设置文件 = 全默认，这不是错误 */
  if (json == NULL || len == 0) {
    s->close_behavior = DSH_CLOSE_ASK;
    /* ⚠️ 发音那一节的默认值也**必须**在这里补上：它是「归一化之后的值」的唯一来源。
     * 漏了这一步，一份空设置存出去的 accent 与 doubaoFormat 都会是空串 —— 看着像解析失败。 */
    if (parse_speech(s, NULL, NULL) != 0) {
      dsh_settings_free(s);
      return -1;
    }
    /* 机器翻译那一节的默认值（同上）*/
    if (merge_translate(s, NULL, 1) != 0) {
      dsh_settings_free(s);
      return -1;
    }
    *out = s;
    return 0;
  }

  dsh_json_doc *doc = NULL;
  if (dsh_json_parse(json, len, &doc) != 0) {
    const char *saved = dsh_last_error_message();
    dsh_settings_free(s);
    dsh_set_last_error("设置文件读不动：%s", saved ? saved : "");
    if (saved != NULL) dsh_release((void *)saved);
    return -1;
  }
  const dsh_json_node *root = dsh_json_doc_root(doc);
  if (!dsh_json_is_object(root)) {
    dsh_json_doc_free(doc);
    dsh_settings_free(s);
    dsh_set_last_error("设置文件的最外层必须是一个 JSON 对象");
    return -1;
  }

  /* 认识的字段 */
  {
    const dsh_json_node *dicts = dsh_json_object_get(root, "dictionaries");
    if (dicts != NULL) {
      /* ⚠️ 类型不对要**报错**，不是「当空的」：把一份坏配置当空的读进来，下一次存盘就会用它覆盖掉
       * 用户原来的词库清单（「我的词库全没了」，而且没有任何提示）。报错至少能让宿主把原文件留着。 */
      if (!dsh_json_is_array(dicts)) {
        dsh_json_doc_free(doc);
        dsh_settings_free(s);
        dsh_set_last_error("设置里的 dictionaries 必须是数组");
        return -1;
      }
      if (parse_dicts(s, dicts) != 0) {
        dsh_json_doc_free(doc);
        dsh_settings_free(s);
        return -1;
      }
    }
    const dsh_json_node *cur = dsh_json_object_get(root, "currentDictId");
    if (dsh_json_is_string(cur)) {
      s->current_dict_id = trim_copy(s, dsh_json_str_value(cur, NULL), NULL);
    }
    const dsh_json_node *cb = dsh_json_object_get(root, "closeBehavior");
    if (dsh_json_is_string(cb)) {
      const char *v = dsh_json_str_value(cb, NULL);
      if (v != NULL && strcmp(v, "quit") == 0) s->close_behavior = DSH_CLOSE_QUIT;
      else if (v != NULL && strcmp(v, "tray") == 0) s->close_behavior = DSH_CLOSE_TRAY;
      else s->close_behavior = DSH_CLOSE_ASK;
    }
    /* ⚠️ 类型不对**不报错**，落回默认值并把原因写进 last_error —— 它与 `closeBehavior` 同一档
     * （一个开关值而已）。上面那几节要报错是因为它们承载**用户的清单 / 凭据**（丢了没法重建）。 */
    {
      const dsh_json_node *sf = dsh_json_object_get(root, "showFloatingOnStartup");
      if (sf != NULL) {
        if (dsh_json_is_bool(sf)) {
          s->show_floating_on_startup = dsh_json_bool_value(sf) ? 1 : 0;
        } else {
          s->show_floating_on_startup = 1;
          if (!dsh_json_is_null(sf)) {
            dsh_set_last_error("showFloatingOnStartup 不是布尔值，已按默认值（显示）处理");
          }
        }
      }
    }
    /* 发音与账号级凭据（见 `merge_speech` 顶上那段约定）*/
    if (parse_speech(s, dsh_json_object_get(root, "speech"),
                     dsh_json_object_get(root, "volcengine")) != 0) {
      dsh_json_doc_free(doc);
      dsh_settings_free(s);
      return -1;
    }
    /* 机器翻译（类型不对同样要报错：把一份坏配置当默认的读进来，下次存盘就把它抹了）*/
    {
      const dsh_json_node *tr = dsh_json_object_get(root, "translate");
      if (tr != NULL && !dsh_json_is_object(tr)) {
        dsh_json_doc_free(doc);
        dsh_settings_free(s);
        dsh_set_last_error("设置里的 translate 必须是一个对象");
        return -1;
      }
      if (merge_translate(s, tr, 1) != 0) {
        dsh_json_doc_free(doc);
        dsh_settings_free(s);
        return -1;
      }
    }
  }

  /* 不认识的顶层键：连名字带**原文片段**记下来，序列化时原样搬回去。
   * ⚠️ 片段的生存期是**这段输入文本**的 —— 必须把片段内容也复制进 arena，否则调用方一释放 json 就悬空。 */
  {
    const int64_t n = dsh_json_object_len(root);
    for (int64_t i = 0; i < n; i++) {
      const char *key = NULL;
      size_t key_len = 0;
      const dsh_json_node *v = dsh_json_object_at(root, i, &key, &key_len);
      if (v == NULL || key == NULL) continue;
      if (is_known_key(key, key_len)) continue;
      if (s->foreign_count >= (int64_t)DSH_SETTINGS_MAX_FOREIGN) {
        /* 如实报错，别悄悄丢 —— 丢了就是「用户设置莫名消失」那个坑 */
        dsh_json_doc_free(doc);
        dsh_settings_free(s);
        dsh_set_last_error("设置里不认识的顶层键太多（超过 %u 个），拒绝解析以免丢数据",
                           DSH_SETTINGS_MAX_FOREIGN);
        return -1;
      }
      size_t raw_len = 0;
      const char *raw = dsh_json_node_raw(v, &raw_len);
      foreign_pair *fp = &s->foreign[s->foreign_count];
      fp->key = arena_strndup(s, key, key_len);
      fp->raw = arena_strndup(s, raw, raw_len);
      fp->raw_len = raw_len;
      if (fp->key == NULL || fp->raw == NULL) {
        dsh_json_doc_free(doc);
        dsh_settings_free(s);
        dsh_set_last_error("内存不足：保存未识别字段（%s）", key);
        return -1;
      }
      s->foreign_count++;
    }
  }

  dsh_json_doc_free(doc);
  *out = s;
  return 0;
}

int dsh_settings_default(dsh_settings **out) { return dsh_settings_parse(NULL, 0, out); }

void dsh_settings_free(dsh_settings *s) {
  if (s == NULL) return;
  arena_release_all(s);
  dsh_release(s);
}

/* ── 读 ─────────────────────────────────────────────────────────────────── */

int64_t dsh_settings_dict_count(const dsh_settings *s) { return (s == NULL) ? 0 : s->dict_count; }

const dsh_stored_dict *dsh_settings_dict_at(const dsh_settings *s, int64_t index) {
  if (s == NULL || index < 0 || index >= s->dict_count) return NULL;
  return &s->dicts[index];
}

const dsh_stored_dict *dsh_settings_dict_by_id(const dsh_settings *s, const char *id) {
  if (s == NULL || id == NULL) return NULL;
  for (int64_t i = 0; i < s->dict_count; i++) {
    if (strcmp(s->dicts[i].id, id) == 0) return &s->dicts[i];
  }
  return NULL;
}

const char *dsh_settings_current_dict_id(const dsh_settings *s) {
  if (s == NULL) return "";
  return (s->current_dict_id != NULL) ? s->current_dict_id : "";
}

dsh_close_behavior dsh_settings_close_behavior(const dsh_settings *s) {
  return (s == NULL) ? DSH_CLOSE_ASK : s->close_behavior;
}

const char *dsh_settings_dict_display_name(const dsh_stored_dict *d, const char *header_title) {
  if (d == NULL) return "";
  if (d->custom_title != NULL && d->custom_title[0] != '\0') return d->custom_title;
  if (header_title != NULL) {
    const char *t = trim_or_null(header_title);
    if (t != NULL && t[0] != '\0') return header_title;
  }
  return (d->title != NULL) ? d->title : "";
}

/* ── 复制 / 打补丁 ──────────────────────────────────────────────────────── */

/** 把一份设置**深拷**进另一个只换了 id/paths 的副本里（补丁与增删改都用它当底座） */
static dsh_settings *clone_settings(const dsh_settings *src) {
  dsh_settings *s = (dsh_settings *)dsh_mem_alloc(sizeof(*s));
  if (s == NULL) {
    dsh_set_last_error("内存不足：设置副本");
    return NULL;
  }
  memset(s, 0, sizeof(*s));
  s->close_behavior = src->close_behavior;
  s->show_floating_on_startup = src->show_floating_on_startup;
  if (src->current_dict_id != NULL) s->current_dict_id = arena_strdup(s, src->current_dict_id);

  if (src->dict_count > 0) {
    dsh_stored_dict *dicts =
        (dsh_stored_dict *)arena_alloc(s, (size_t)src->dict_count * sizeof(*dicts), 8);
    if (dicts == NULL) {
      dsh_settings_free(s);
      dsh_set_last_error("内存不足：词库清单副本");
      return NULL;
    }
    memset(dicts, 0, (size_t)src->dict_count * sizeof(*dicts));
    for (int64_t i = 0; i < src->dict_count; i++) {
      const dsh_stored_dict *d = &src->dicts[i];
      dsh_stored_dict *o = &dicts[i];
      o->id = arena_strdup(s, d->id);
      o->title = arena_strdup(s, d->title);
      o->custom_title = (d->custom_title != NULL) ? arena_strdup(s, d->custom_title) : NULL;
      o->mdx_path = arena_strdup(s, d->mdx_path);
      o->added_at = d->added_at;
      if (d->mdd_count > 0) {
        const char **paths =
            (const char **)arena_alloc(s, (size_t)d->mdd_count * sizeof(char *), 8);
        if (paths == NULL) {
          dsh_settings_free(s);
          dsh_set_last_error("内存不足：资源卷列表副本");
          return NULL;
        }
        for (int64_t k = 0; k < d->mdd_count; k++) paths[k] = arena_strdup(s, d->mdd_paths[k]);
        o->mdd_paths = paths;
        o->mdd_count = d->mdd_count;
      }
    }
    s->dicts = dicts;
    s->dict_count = src->dict_count;
  }

  for (int64_t i = 0; i < src->foreign_count; i++) {
    foreign_pair *fp = &s->foreign[i];
    fp->key = arena_strdup(s, src->foreign[i].key);
    fp->raw = arena_strndup(s, src->foreign[i].raw, src->foreign[i].raw_len);
    fp->raw_len = src->foreign[i].raw_len;
    s->foreign_count++;
  }

  /* 发音那一节（含**没建模的子键**的原样搬运块）—— 漏掉这一步的后果是「改词库/改当前词典之后，
   * 发音设置就没了」：那一条路走的就是这份副本。 */
  s->speech = src->speech;
  s->speech.rate = src->speech.rate;
  s->speech.voice_id = (src->speech.voice_id != NULL) ? arena_strdup(s, src->speech.voice_id) : NULL;
  s->speech.accent = arena_strdup(s, src->speech.accent);
  s->speech.doubao_api_key =
      (src->speech.doubao_api_key != NULL) ? arena_strdup(s, src->speech.doubao_api_key) : NULL;
  s->speech.doubao_resource_id = arena_strdup(s, src->speech.doubao_resource_id);
  s->speech.doubao_format = arena_strdup(s, src->speech.doubao_format);
  s->speech.doubao_speaker_en =
      (src->speech.doubao_speaker_en != NULL) ? arena_strdup(s, src->speech.doubao_speaker_en)
                                              : NULL;
  s->speech.doubao_speaker_zh =
      (src->speech.doubao_speaker_zh != NULL) ? arena_strdup(s, src->speech.doubao_speaker_zh)
                                              : NULL;
  s->volcengine_api_key =
      (src->volcengine_api_key != NULL) ? arena_strdup(s, src->volcengine_api_key) : NULL;
  s->speech_extra_count = 0;
  for (int64_t i = 0; i < src->speech_extra_count; i++) {
    foreign_pair *fp = &s->speech_extra[s->speech_extra_count];
    fp->key = arena_strdup(s, src->speech_extra[i].key);
    fp->raw = arena_strndup(s, src->speech_extra[i].raw, src->speech_extra[i].raw_len);
    fp->raw_len = src->speech_extra[i].raw_len;
    s->speech_extra_count++;
  }
  s->volcengine_extra_count = 0;
  for (int64_t i = 0; i < src->volcengine_extra_count; i++) {
    foreign_pair *fp = &s->volcengine_extra[s->volcengine_extra_count];
    fp->key = arena_strdup(s, src->volcengine_extra[i].key);
    fp->raw = arena_strndup(s, src->volcengine_extra[i].raw, src->volcengine_extra[i].raw_len);
    fp->raw_len = src->volcengine_extra[i].raw_len;
    s->volcengine_extra_count++;
  }

  /* 机器翻译那一节（含没建模的子键）—— 同一条理由：漏掉它，任何一次
   * 「改词库/改当前词典」都会把用户的翻译开关与目标语种打回默认。 */
  s->translate.enabled = src->translate.enabled;
  s->translate.auto_translate = src->translate.auto_translate;
  s->translate.target_mode =
      (src->translate.target_mode != NULL) ? arena_strdup(s, src->translate.target_mode) : NULL;
  s->translate_extra_count = 0;
  for (int64_t i = 0; i < src->translate_extra_count; i++) {
    foreign_pair *fp = &s->translate_extra[s->translate_extra_count];
    fp->key = arena_strdup(s, src->translate_extra[i].key);
    fp->raw = arena_strndup(s, src->translate_extra[i].raw, src->translate_extra[i].raw_len);
    fp->raw_len = src->translate_extra[i].raw_len;
    s->translate_extra_count++;
  }
  return s;
}

/* ── 窗口级设置（借用出参；见 dsh_settings.h）── */

const dsh_speech_settings *dsh_settings_speech(const dsh_settings *s) {
  return (s != NULL) ? &s->speech : NULL;
}

int dsh_settings_show_floating_on_startup(const dsh_settings *s) {
  return (s != NULL) ? s->show_floating_on_startup : 1;
}

const dsh_translate_settings *dsh_settings_translate(const dsh_settings *s) {
  return (s != NULL) ? &s->translate : NULL;
}

const char *dsh_settings_volcengine_api_key(const dsh_settings *s) {
  if (s == NULL || s->volcengine_api_key == NULL) return NULL;
  return s->volcengine_api_key;
}

int dsh_settings_speech_online_ready(const dsh_settings *s) {
  if (s == NULL) return 0;
  /* 检查标准只有一条：填没填 Key（**不许**再引入一个开关，见头文件那段）*/
  return (s->volcengine_api_key != NULL && s->volcengine_api_key[0] != '\0') ? 1 : 0;
}

/** 取补丁里某个键的文本值；返回 NULL 表示「键不存在或不是字符串」 */
static const char *patch_str(const dsh_json_node *patch, const char *key, int *present) {
  const dsh_json_node *v = dsh_json_object_get(patch, key);
  if (present != NULL) *present = (v != NULL) ? 1 : 0;
  if (v == NULL) return NULL;
  if (dsh_json_is_null(v)) return NULL; /* 显式 null = 清空 */
  if (!dsh_json_is_string(v)) return NULL;
  return dsh_json_str_value(v, NULL);
}

int dsh_settings_apply_patch(const dsh_settings *base, const char *patch_json, size_t len,
                             dsh_settings **out) {
  if (out == NULL) {
    dsh_set_last_error("dsh_settings_apply_patch：out 不能为空");
    return -1;
  }
  *out = NULL;
  if (base == NULL) {
    dsh_set_last_error("dsh_settings_apply_patch：base 不能为空");
    return -1;
  }
  if (patch_json == NULL) {
    dsh_set_last_error("dsh_settings_apply_patch：补丁不能为空");
    return -1;
  }

  dsh_json_doc *doc = NULL;
  if (dsh_json_parse(patch_json, len, &doc) != 0) {
    const char *saved = dsh_last_error_message();
    dsh_set_last_error("设置补丁读不动：%s", saved ? saved : "");
    if (saved != NULL) dsh_release((void *)saved);
    return -1;
  }
  const dsh_json_node *patch = dsh_json_doc_root(doc);
  if (!dsh_json_is_object(patch)) {
    dsh_json_doc_free(doc);
    dsh_set_last_error("设置补丁必须是一个 JSON 对象（局部补丁：只写要改的字段）");
    return -1;
  }

  dsh_settings *s = clone_settings(base);
  if (s == NULL) {
    dsh_json_doc_free(doc);
    return -1;
  }

  /* 只改补丁里**出现**的键 */
  int present = 0;
  {
    const char *v = patch_str(patch, "currentDictId", &present);
    if (present) {
      s->current_dict_id = (v != NULL) ? arena_strdup(s, v) : NULL;
    }
  }
  {
    const char *v = patch_str(patch, "closeBehavior", &present);
    if (present) {
      if (v != NULL && strcmp(v, "quit") == 0) s->close_behavior = DSH_CLOSE_QUIT;
      else if (v != NULL && strcmp(v, "tray") == 0) s->close_behavior = DSH_CLOSE_TRAY;
      else s->close_behavior = DSH_CLOSE_ASK; /* 非法值一律退回 ask */
    }
  }
  {
    /* 启动时显不显示悬浮窗：与 `closeBehavior` 同一档（顶层、只有一个开关值）。
     * ⚠️ 只有补丁里**真的带了这个键**才动它，否则「改词库」那种补丁会把用户关掉的显示又打开。
     * 显式 `null` = 回到默认；类型不对同样落回默认并留下原因。 */
    const dsh_json_node *sf = dsh_json_object_get(patch, "showFloatingOnStartup");
    if (sf != NULL) {
      if (dsh_json_is_bool(sf)) {
        s->show_floating_on_startup = dsh_json_bool_value(sf) ? 1 : 0;
      } else {
        s->show_floating_on_startup = 1;
        if (!dsh_json_is_null(sf)) {
          dsh_set_last_error("设置补丁里的 showFloatingOnStartup 不是布尔值，已按默认值（显示）处理");
        }
      }
    }
  }
  {
    const dsh_json_node *v = dsh_json_object_get(patch, "dictionaries");
    if (v != NULL) {
      if (dsh_json_is_null(v)) {
        s->dicts = NULL;
        s->dict_count = 0;
      } else if (dsh_json_is_array(v)) {
        /* ⚠️ 词库清单**整份替换**（不按 id 合并）：它是引擎自己维护的数组，补丁里给了就以补丁为准 ——
         * 逐项合并的语义表达不出「用户删了两本」，删除只能靠整份给。 */
        dsh_settings *empty = clone_settings(s);
        if (empty == NULL) {
          dsh_settings_free(s);
          dsh_json_doc_free(doc);
          return -1;
        }
        empty->dicts = NULL;
        empty->dict_count = 0;
        if (parse_dicts(empty, v) != 0) {
          dsh_settings_free(empty);
          dsh_settings_free(s);
          dsh_json_doc_free(doc);
          return -1;
        }
        /* 迁移 id/paths 到 s：直接把 s 的清单换掉（两边都是arena内存，s 那份就此废弃） */
        s->dicts = empty->dicts;
        s->dict_count = empty->dict_count;
        /* ⚠️ empty 的 arena 不能在这里 free —— `s->dicts` 指向它的内存：
         * 把 empty 的整条 arena 链**接到 s 上**，由 s 一起管到底。 */
        if (empty->arena != NULL) {
          settings_arena *tail = empty->arena;
          while (tail->next != NULL) tail = tail->next;
          tail->next = s->arena;
          s->arena = empty->arena;
          empty->arena = NULL; /* 交给 s 了 */
        }
        dsh_settings_free(empty); /* 只还句柄 */
      } else {
        dsh_settings_free(s);
        dsh_json_doc_free(doc);
        dsh_set_last_error("设置补丁里的 dictionaries 必须是数组或 null");
        return -1;
      }
    }
  }

  /*
   * 发音与账号级凭据（`speech` / `volcengine`）**必须走 merge 语义**（`reset_defaults = 0`）：
   * `settingsSet` 收的是局部补丁，「只改语速」的补丁不许把 Key / 音色 / 响度一起打回默认值 ——
   * 第一版跳过这两个顶层键，于是界面上改口音 / 改音色 / 填 Key **一条都存不进去**且不报错。
   */
  {
    const dsh_json_node *sp = dsh_json_object_get(patch, "speech");
    const dsh_json_node *ve = dsh_json_object_get(patch, "volcengine");
    if (sp != NULL && !dsh_json_is_object(sp)) {
      dsh_settings_free(s);
      dsh_json_doc_free(doc);
      dsh_set_last_error("设置补丁里的 speech 必须是一个对象");
      return -1;
    }
    if (ve != NULL && !dsh_json_is_object(ve)) {
      dsh_settings_free(s);
      dsh_json_doc_free(doc);
      dsh_set_last_error("设置补丁里的 volcengine 必须是一个对象");
      return -1;
    }
    if ((sp != NULL || ve != NULL) && merge_speech(s, sp, ve, 0) != 0) {
      dsh_settings_free(s);
      dsh_json_doc_free(doc);
      return -1;
    }
  }

  /* 机器翻译（同一条 merge 语义：只改补丁里出现的子键 ——
   * 「只关掉总开关」绝不许把用户固定的目标语种打回 auto）*/
  {
    const dsh_json_node *tr = dsh_json_object_get(patch, "translate");
    if (tr != NULL && !dsh_json_is_object(tr)) {
      dsh_settings_free(s);
      dsh_json_doc_free(doc);
      dsh_set_last_error("设置补丁里的 translate 必须是一个对象");
      return -1;
    }
    if (merge_translate(s, tr, 0) != 0) {
      dsh_settings_free(s);
      dsh_json_doc_free(doc);
      return -1;
    }
  }

  /* 补丁里**内核不认识**的键：也要原样活下来（否则宿主改一处、内核存盘就丢了别的） */
  {
    const int64_t n = dsh_json_object_len(patch);
    for (int64_t i = 0; i < n; i++) {
      const char *key = NULL;
      size_t key_len = 0;
      const dsh_json_node *v = dsh_json_object_at(patch, i, &key, &key_len);
      if (v == NULL || key == NULL) continue;
      if (is_known_key(key, key_len)) continue;
      size_t raw_len = 0;
      const char *raw = dsh_json_node_raw(v, &raw_len);
      int replaced = 0;
      for (int64_t k = 0; k < s->foreign_count; k++) {
        if (strlen(s->foreign[k].key) == key_len &&
            memcmp(s->foreign[k].key, key, key_len) == 0) {
          s->foreign[k].raw = arena_strndup(s, raw, raw_len);
          s->foreign[k].raw_len = raw_len;
          replaced = 1;
          break;
        }
      }
      if (replaced) continue;
      if (s->foreign_count >= (int64_t)DSH_SETTINGS_MAX_FOREIGN) {
        dsh_settings_free(s);
        dsh_json_doc_free(doc);
        dsh_set_last_error("设置里不认识的顶层键太多（超过 %u 个）", DSH_SETTINGS_MAX_FOREIGN);
        return -1;
      }
      foreign_pair *fp = &s->foreign[s->foreign_count];
      fp->key = arena_strndup(s, key, key_len);
      fp->raw = arena_strndup(s, raw, raw_len);
      fp->raw_len = raw_len;
      if (fp->key == NULL || fp->raw == NULL) {
        dsh_settings_free(s);
        dsh_json_doc_free(doc);
        dsh_set_last_error("内存不足：保存未识别字段（%s）", key);
        return -1;
      }
      s->foreign_count++;
    }
  }

  dsh_json_doc_free(doc);
  *out = s;
  return 0;
}

/* ── 词库增删改 ─────────────────────────────────────────────────────────── */

/** 用一个新的清单替换副本里的清单（长度由参数给；元素由调用方填好） */
static int replace_dicts(dsh_settings *s, dsh_stored_dict *dicts, int64_t count) {
  s->dicts = dicts;
  s->dict_count = count;
  return 0;
}

int dsh_settings_dict_add(const dsh_settings *base, const char *id, const char *mdx_path,
                          const char *title, const char *const *mdd_paths, int64_t mdd_count,
                          int64_t added_at, dsh_settings **out) {
  if (out == NULL) {
    dsh_set_last_error("dsh_settings_dict_add：out 不能为空");
    return -1;
  }
  *out = NULL;
  if (base == NULL) {
    dsh_set_last_error("dsh_settings_dict_add：base 不能为空");
    return -1;
  }
  /* ⚠️ 这两个判断是**产品规则**：不许把「没有路径的东西」记成一本书，也不许给一本没有 id 的书。
   * 宁可当场失败、让用户看到原因。 */
  if (mdx_path == NULL || mdx_path[0] == '\0') {
    dsh_set_last_error("加词典失败：.mdx 路径是空的");
    return -1;
  }
  if (id == NULL || id[0] == '\0') {
    dsh_set_last_error("加词典失败：内容哈希 id 是空的（调用方要先算它）");
    return -1;
  }
  if (base->dict_count >= (int64_t)DSH_SETTINGS_MAX_DICTS) {
    dsh_set_last_error("词库已达上限（%u 本）", DSH_SETTINGS_MAX_DICTS);
    return -1;
  }

  dsh_settings *s = clone_settings(base);
  if (s == NULL) return -1;

  /* 已经有一本同样 id 的：**不重复加**，但把路径更新成这次给的
   * （同一本词典换目录之后，用户是用「再导入一次」表达这个意思的）。 */
  for (int64_t i = 0; i < s->dict_count; i++) {
    if (strcmp(s->dicts[i].id, id) == 0) {
      s->dicts[i].mdx_path = arena_strdup(s, mdx_path);
      if (title != NULL && title[0] != '\0') s->dicts[i].title = arena_strdup(s, title);
      *out = s;
      return 0;
    }
  }

  dsh_stored_dict *dicts =
      (dsh_stored_dict *)arena_alloc(s, (size_t)(s->dict_count + 1) * sizeof(*dicts), 8);
  if (dicts == NULL) {
    dsh_settings_free(s);
    dsh_set_last_error("内存不足：词库清单");
    return -1;
  }
  memset(dicts, 0, (size_t)(s->dict_count + 1) * sizeof(*dicts));
  if (s->dict_count > 0) memcpy(dicts, s->dicts, (size_t)s->dict_count * sizeof(*dicts));

  dsh_stored_dict *d = &dicts[s->dict_count];
  d->id = arena_strdup(s, id);
  d->mdx_path = arena_strdup(s, mdx_path);
  d->title = (title != NULL) ? arena_strdup(s, title) : arena_strdup(s, "");
  d->custom_title = NULL;
  d->added_at = (added_at > 0) ? added_at : dsh_now_ms();
  if (mdd_count > 0 && mdd_paths != NULL) {
    const char **paths = (const char **)arena_alloc(s, (size_t)mdd_count * sizeof(char *), 8);
    if (paths == NULL) {
      dsh_settings_free(s);
      dsh_set_last_error("内存不足：资源卷列表");
      return -1;
    }
    int64_t kept = 0;
    for (int64_t k = 0; k < mdd_count; k++) {
      if (mdd_paths[k] == NULL || mdd_paths[k][0] == '\0') continue;
      paths[kept++] = arena_strdup(s, mdd_paths[k]);
    }
    d->mdd_paths = paths;
    d->mdd_count = kept;
  }
  replace_dicts(s, dicts, s->dict_count + 1);
  /*
   * ⚠️ 还没指定过当前词典时，新加的这一本就是当前。少了它的症状很具体：用户第一次加了一本词典、
   * 回面板查词，得到的是一句「还没指定当前词典」—— 而「当前」这件事他刚刚用「加进来」表达过了。
   */
  if (s->current_dict_id == NULL || s->current_dict_id[0] == '\0') {
    s->current_dict_id = arena_strdup(s, id);
    if (s->current_dict_id == NULL) {
      dsh_settings_free(s);
      dsh_set_last_error("内存不足：当前词典");
      return -1;
    }
  }
  *out = s;
  return 0;
}

int dsh_settings_dict_remove(const dsh_settings *base, const char *id, dsh_settings **out) {
  if (out == NULL) {
    dsh_set_last_error("dsh_settings_dict_remove：out 不能为空");
    return -1;
  }
  *out = NULL;
  if (base == NULL || id == NULL) {
    dsh_set_last_error("dsh_settings_dict_remove：参数不能为空");
    return -1;
  }
  dsh_settings *s = clone_settings(base);
  if (s == NULL) return -1;

  int64_t found = -1;
  for (int64_t i = 0; i < s->dict_count; i++) {
    if (strcmp(s->dicts[i].id, id) == 0) { found = i; break; }
  }
  if (found < 0) {
    *out = s; /* 找不到就原样返回（幂等）—— 重复删同一本不该报错 */
    return 0;
  }
  for (int64_t i = found; i + 1 < s->dict_count; i++) s->dicts[i] = s->dicts[i + 1];
  s->dict_count--;
  /*
   * 当前词典正好被删掉：**顺位给剩下的第一本**，一本都不剩才清空。
   * ⚠️ 一律清成 NULL 的后果是「删掉当前那本之后查什么都不动」，而用户手上明明还有别的词典。
   * ⚠️ 留着一个不在清单里的 id，界面会去找一本已删掉的词典（标题栏空着、点什么都没反应）。
   */
  if (s->current_dict_id != NULL && strcmp(s->current_dict_id, id) == 0) {
    s->current_dict_id =
        (s->dict_count > 0) ? arena_strdup(s, s->dicts[0].id) : NULL;
  }
  *out = s;
  return 0;
}

int dsh_settings_dict_rename(const dsh_settings *base, const char *id, const char *name,
                             dsh_settings **out) {
  if (out == NULL) {
    dsh_set_last_error("dsh_settings_dict_rename：out 不能为空");
    return -1;
  }
  *out = NULL;
  if (base == NULL || id == NULL) {
    dsh_set_last_error("dsh_settings_dict_rename：参数不能为空");
    return -1;
  }
  dsh_settings *s = clone_settings(base);
  if (s == NULL) return -1;

  int64_t found = -1;
  for (int64_t i = 0; i < s->dict_count; i++) {
    if (strcmp(s->dicts[i].id, id) == 0) { found = i; break; }
  }
  if (found < 0) {
    /* ⚠️ 报错里用**调用方传进来的 id**，不是 arena 里的副本 ——
     * 下面那行 free 会把 arena 全部释放，之后再用它就是 use-after-free。 */
    dsh_set_last_error("改名失败：词库里没有这本词典（id=%s）", id);
    dsh_settings_free(s);
    return -1;
  }
  {
    /* 空串/空白 = 恢复默认名（接口定义里写着） */
    const char *trimmed = trim_copy(s, name, NULL);
    s->dicts[found].custom_title = trimmed;
  }
  *out = s;
  return 0;
}

/*
 * 把一本在清单里挪 delta 位。
 * ⚠️ 两个容易写错的地方：① **夹住而不是报错**（第一本再往前、最后一本再往后都返回「原样的一份新设置」
 * 并且成功），夹住是防脏输入的兜底；② **只搬结构体本身**（浅拷贝，指针仍指向同一块 arena）——
 * 与 `dict_remove` 里那句 `s->dicts[i] = s->dicts[i + 1]` 同一条约定，改成深拷贝会漏掉 `mdd_paths`。
 */
int dsh_settings_dict_move(const dsh_settings *base, const char *id, int64_t delta,
                           dsh_settings **out) {
  if (out == NULL) {
    dsh_set_last_error("dsh_settings_dict_move：out 不能为空");
    return -1;
  }
  *out = NULL;
  if (base == NULL || id == NULL) {
    dsh_set_last_error("dsh_settings_dict_move：参数不能为空");
    return -1;
  }
  dsh_settings *s = clone_settings(base);
  if (s == NULL) return -1;

  int64_t found = -1;
  for (int64_t i = 0; i < s->dict_count; i++) {
    if (strcmp(s->dicts[i].id, id) == 0) { found = i; break; }
  }
  if (found < 0) {
    /* 与 rename 同一条：报错里用**调用方传进来的 id**（arena 里的东西马上要被 free 掉） */
    dsh_set_last_error("移动失败：词库里没有这本词典（id=%s）", id);
    dsh_settings_free(s);
    return -1;
  }
  if (delta == 0 || s->dict_count < 2) {
    *out = s; /* 不用挪 */
    return 0;
  }
  int64_t target = found + delta;
  if (target < 0) target = 0;
  if (target > s->dict_count - 1) target = s->dict_count - 1;
  if (target == found) {
    *out = s; /* 已经在头/尾 */
    return 0;
  }
  dsh_stored_dict moving = s->dicts[found];
  if (target > found) {
    for (int64_t i = found; i < target; i++) s->dicts[i] = s->dicts[i + 1];
  } else {
    for (int64_t i = found; i > target; i--) s->dicts[i] = s->dicts[i - 1];
  }
  s->dicts[target] = moving;
  *out = s;
  return 0;
}

int dsh_settings_dict_set_current(const dsh_settings *base, const char *id, dsh_settings **out) {
  if (out == NULL) {
    dsh_set_last_error("dsh_settings_dict_set_current：out 不能为空");
    return -1;
  }
  *out = NULL;
  if (base == NULL) {
    dsh_set_last_error("dsh_settings_dict_set_current：base 不能为空");
    return -1;
  }
  dsh_settings *s = clone_settings(base);
  if (s == NULL) return -1;

  int found = 0;
  const char *trimmed = trim_copy(s, id, NULL);
  if (trimmed == NULL) {
    s->current_dict_id = NULL; /* 空串/空白 = 取消指定 */
    *out = s;
    return 0;
  }
  for (int64_t i = 0; i < s->dict_count; i++) {
    if (strcmp(s->dicts[i].id, trimmed) == 0) { found = 1; break; }
  }
  if (!found) {
    /* ⚠️ 顺序要紧：**先写报错、再释放**。`trimmed` 住在 `s` 的 arena 里，先 free 再用它格式化就是
     * use-after-free；而 release 构建下通常什么都不报，只是一个偶尔乱码的错误信息。 */
    dsh_set_last_error("指定当前词典失败：词库里没有这本词典（id=%s）", trimmed);
    dsh_settings_free(s);
    return -1;
  }
  s->current_dict_id = (char *)trimmed;
  *out = s;
  return 0;
}

/* ── 序列化 ─────────────────────────────────────────────────────────────── */

char *dsh_settings_to_json(const dsh_settings *s) {
  if (s == NULL) {
    dsh_set_last_error("dsh_settings_to_json：参数为 NULL");
    return NULL;
  }
  dsh_json *j = dsh_json_new();
  if (j == NULL) return NULL;

  dsh_json_object_begin(j);
  /* ⚠️ 顺序固定：同一份设置在两次存盘之间必须**逐字节相同**（可 diff、可逐字节对照），
   * 顺序一旦随机，「设置文件变了没有」就没法用比对来判断。 */
  dsh_json_kv_int(j, "version", 1);
  dsh_json_key(j, "dictionaries");
  dsh_json_array_begin(j);
  for (int64_t i = 0; i < s->dict_count; i++) {
    const dsh_stored_dict *d = &s->dicts[i];
    dsh_json_object_begin(j);
    dsh_json_kv_str(j, "id", d->id);
    dsh_json_kv_str(j, "title", d->title);
    dsh_json_key(j, "customTitle");
    if (d->custom_title != NULL) dsh_json_str(j, d->custom_title);
    else dsh_json_null(j);
    dsh_json_kv_str(j, "mdxPath", d->mdx_path);
    dsh_json_key(j, "mddPaths");
    dsh_json_array_begin(j);
    for (int64_t k = 0; k < d->mdd_count; k++) dsh_json_str(j, d->mdd_paths[k]);
    dsh_json_array_end(j);
    dsh_json_kv_i64(j, "addedAt", d->added_at);
    dsh_json_object_end(j);
  }
  dsh_json_array_end(j);
  dsh_json_key(j, "currentDictId");
  if (s->current_dict_id != NULL) dsh_json_str(j, s->current_dict_id);
  else dsh_json_null(j);
  dsh_json_kv_str(j, "closeBehavior",
                  s->close_behavior == DSH_CLOSE_QUIT
                      ? "quit"
                      : (s->close_behavior == DSH_CLOSE_TRAY ? "tray" : "ask"));
  /* 窗口级的另一条：**总是**写出来（只有一个布尔值，缺了就得让壳去猜默认）*/
  dsh_json_kv_bool(j, "showFloatingOnStartup", s->show_floating_on_startup);

  /* 发音那一节：**只写归一化之后的值**（写回去的永远是内核认可的形状） */
  {
    const dsh_speech_settings *sp = &s->speech;
    dsh_json_key(j, "speech");
    dsh_json_object_begin(j);
    dsh_json_kv_int(j, "rate", sp->rate);
    dsh_json_key(j, "voiceId");
    if (sp->voice_id != NULL) dsh_json_str(j, sp->voice_id);
    else dsh_json_null(j);
    dsh_json_kv_str(j, "accent", sp->accent);
    /* `defaultLanguage` **不写**（永远没有值，见 parse_speech 顶上那段）*/
    dsh_json_key(j, "doubaoApiKey");
    if (sp->doubao_api_key != NULL) dsh_json_str(j, sp->doubao_api_key);
    else dsh_json_null(j);
    dsh_json_kv_str(j, "doubaoResourceId", sp->doubao_resource_id);
    dsh_json_kv_str(j, "doubaoFormat", sp->doubao_format);
    dsh_json_key(j, "doubaoSpeakerEn");
    if (sp->doubao_speaker_en != NULL) dsh_json_str(j, sp->doubao_speaker_en);
    else dsh_json_null(j);
    dsh_json_key(j, "doubaoSpeakerZh");
    if (sp->doubao_speaker_zh != NULL) dsh_json_str(j, sp->doubao_speaker_zh);
    else dsh_json_null(j);
    if (sp->has_loudness_en) dsh_json_kv_int(j, "doubaoLoudnessEn", sp->loudness_en);
    if (sp->has_loudness_zh) dsh_json_kv_int(j, "doubaoLoudnessZh", sp->loudness_zh);
    /* `speech` 里**没建模**的子键原样搬回去（少一个就是「用户设置莫名丢了」）*/
    for (int64_t i = 0; i < s->speech_extra_count; i++) {
      dsh_json_key(j, s->speech_extra[i].key);
      dsh_json_value_raw(j, s->speech_extra[i].raw, s->speech_extra[i].raw_len);
    }
    dsh_json_object_end(j);

    /* 账号级凭据：与 `speech.doubaoApiKey` 同值（镜像，见头文件那段）*/
    dsh_json_key(j, "volcengine");
    dsh_json_object_begin(j);
    dsh_json_key(j, "apiKey");
    if (s->volcengine_api_key != NULL) dsh_json_str(j, s->volcengine_api_key);
    else dsh_json_null(j);
    for (int64_t i = 0; i < s->volcengine_extra_count; i++) {
      dsh_json_key(j, s->volcengine_extra[i].key);
      dsh_json_value_raw(j, s->volcengine_extra[i].raw, s->volcengine_extra[i].raw_len);
    }
    dsh_json_object_end(j);
  }

  /* 机器翻译：**只写归一化之后的值**（`targetMode` 一定是 auto / zh / en 之一，脏数据不会跟着传下去）*/
  {
    dsh_json_key(j, "translate");
    dsh_json_object_begin(j);
    dsh_json_kv_bool(j, "enabled", s->translate.enabled);
    dsh_json_kv_str(j, "targetMode",
                    s->translate.target_mode != NULL ? s->translate.target_mode : "auto");
    dsh_json_kv_bool(j, "autoTranslate", s->translate.auto_translate);
    for (int64_t i = 0; i < s->translate_extra_count; i++) {
      dsh_json_key(j, s->translate_extra[i].key);
      dsh_json_value_raw(j, s->translate_extra[i].raw, s->translate_extra[i].raw_len);
    }
    dsh_json_object_end(j);
  }

  /* 内核不认识的顶层键：**原样**搬回去。
   * 放在最后是刻意的：认识的那批位置固定、可 diff，不认识的那批不影响前面。 */
  for (int64_t i = 0; i < s->foreign_count; i++) {
    dsh_json_key(j, s->foreign[i].key);
    dsh_json_value_raw(j, s->foreign[i].raw, s->foreign[i].raw_len);
  }
  dsh_json_object_end(j);

  char *text = dsh_json_take(j);
  dsh_json_free(j);
  if (text == NULL) dsh_set_last_error("内存不足：设置 JSON");
  return text;
}

/* ── 对「内核不认识的顶层键」的两件操作（约定见 dsh_settings.h 那一节）──
 *
 * 唯一的用户是**历史迁移**（把老版本留在设置里的 `history` 数组搬进 `history.jsonl` 再删掉那个键）：
 * 「原样搬运」意味着那个数组会永远跟着设置走，而每次存设置都要把它整份重写一遍。
 */

const char *dsh_settings_foreign_raw(const dsh_settings *s, const char *key, size_t *out_len) {
  if (out_len != NULL) *out_len = 0;
  if (s == NULL || key == NULL) return NULL;
  for (int64_t i = 0; i < s->foreign_count; i++) {
    if (strcmp(s->foreign[i].key, key) == 0) {
      if (out_len != NULL) *out_len = s->foreign[i].raw_len;
      return s->foreign[i].raw;
    }
  }
  return NULL;
}

int dsh_settings_drop_foreign(const dsh_settings *s, const char *key, dsh_settings **out) {
  if (out == NULL) {
    dsh_set_last_error("dsh_settings_drop_foreign：出参不能为空");
    return -1;
  }
  *out = NULL;
  if (s == NULL || key == NULL || key[0] == '\0') {
    dsh_set_last_error("dsh_settings_drop_foreign：参数不能为空");
    return -1;
  }
  dsh_settings *n = clone_settings(s);
  if (n == NULL) return -1; /* last_error 已由 clone_settings 写好 */
  /* 就地压紧：跳过那一个键，其余的相对顺序一个不变（序列化顺序决定了可 diff）*/
  int64_t kept = 0;
  for (int64_t i = 0; i < n->foreign_count; i++) {
    if (strcmp(n->foreign[i].key, key) == 0) continue;
    if (kept != i) n->foreign[kept] = n->foreign[i];
    kept++;
  }
  n->foreign_count = kept;
  *out = n;
  return 0;
}
