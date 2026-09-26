/* 见 audio/dsh_audio.h —— 词条音频引用 + 键名策略。 */
#include "audio/dsh_audio.h"
#include "text/dsh_textutil.h"

#include "dsh_internal.h"

#include <stdio.h>
#include <string.h>

/* ── 小工具 ─────────────────────────────────────────────────────────────── */

static int is_space_char(char c) {
  return (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') ? 1 : 0;
}

static int eq_ci(const char *a, const char *b) {
  while (*a != '\0' && *b != '\0') {
    char x = *a;
    char y = *b;
    if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
    if (x != y) return 0;
    a++;
    b++;
  }
  return (*a == '\0' && *b == '\0') ? 1 : 0;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

/* ── 键名归一化 ─────────────────────────────────────────────────────────── */

char *dsh_audio_normalize(const char *raw, size_t len) {
  if (raw == NULL) len = 0;
  /* ① 两端 trim */
  size_t b = 0;
  size_t e = len;
  while (b < e && is_space_char(raw[b])) b++;
  while (e > b && is_space_char(raw[e - 1])) e--;
  /*
   * ② 解码（`%20` / `%E6%B5%8B` 这类；解不动就保留原样）—— 算法只有一份，在
   * `text/dsh_textutil.c` 的 `dsh_text_percent_decode`，别在这里另写一遍。
   */
  char *decoded = dsh_text_percent_decode(raw + b, e - b);
  if (decoded == NULL) {
    dsh_set_last_error("内存不足：音频键");
    return NULL;
  }
  /* ⚠️ `at` 必须是**解码后**的长度（`strlen`）：写成 `e - b` 会把 NUL 之后的字节也算进去。 */
  const size_t at = strlen(decoded);
  /* ③ 去前导 `\` 与 `/`，再两端 trim 一次 */
  size_t start = 0;
  while (decoded[start] == '\\' || decoded[start] == '/') start++;
  size_t end = at;
  while (end > start && is_space_char(decoded[end - 1])) end--;
  while (start < end && is_space_char(decoded[start])) start++;
  const size_t n = end - start;
  char *out = (char *)dsh_mem_alloc(n + 1);
  if (out == NULL) {
    dsh_release(decoded);
    dsh_set_last_error("内存不足：音频键");
    return NULL;
  }
  memcpy(out, decoded + start, n);
  out[n] = '\0';
  dsh_release(decoded);
  return out;
}

/* ── 口音 / 例句 / 扩展名 ───────────────────────────────────────────────── */

/**
 * 按分隔符 `[^0-9A-Za-z]+` 把键名切成 token、转小写再比对。
 *
 * 为什么要切 token 而不是直接找子串：`us` 这种两字母标记在长名字里随手就能撞上
 * （`apple__plus_1` 里就有）。
 */
static int has_token(const char *key, const char *want) {
  if (key == NULL) return 0;
  const size_t want_len = strlen(want);
  size_t i = 0;
  while (key[i] != '\0') {
    /* 跳过分隔符 */
    while (key[i] != '\0' && !((key[i] >= '0' && key[i] <= '9') ||
                               (key[i] >= 'A' && key[i] <= 'Z') ||
                               (key[i] >= 'a' && key[i] <= 'z'))) {
      i++;
    }
    const size_t start = i;
    while (key[i] != '\0' && ((key[i] >= '0' && key[i] <= '9') ||
                              (key[i] >= 'A' && key[i] <= 'Z') ||
                              (key[i] >= 'a' && key[i] <= 'z'))) {
      i++;
    }
    const size_t len = i - start;
    if (len == want_len) {
      int hit = 1;
      for (size_t k = 0; k < len; k++) {
        if (lower(key[start + k]) != want[k]) {
          hit = 0;
          break;
        }
      }
      if (hit) return 1;
    }
  }
  return 0;
}

const char *dsh_audio_classify_accent(const char *key) {
  static const char *const UK[] = {"gb", "bre", "brs", "uk", "brit"};
  static const char *const US[] = {"us", "uss", "ams", "nam", "ame"};
  for (size_t i = 0; i < sizeof(UK) / sizeof(UK[0]); i++) {
    if (has_token(key, UK[i])) return "uk";
  }
  for (size_t i = 0; i < sizeof(US) / sizeof(US[0]); i++) {
    if (has_token(key, US[i])) return "us";
  }
  return NULL;
}

int dsh_audio_is_example_key(const char *key) {
  if (key == NULL || key[0] == '\0') return 0;
  /* LDOCE5 的例句键名：`^p\d+__` */
  if (key[0] == 'p' || key[0] == 'P') {
    size_t i = 1;
    while (key[i] >= '0' && key[i] <= '9') i++;
    if (i > 1 && key[i] == '_' && key[i + 1] == '_') return 1;
  }
  static const char *const MARKS[] = {"gbs", "uss", "brs", "ams", "eps", "exa"};
  for (size_t i = 0; i < sizeof(MARKS) / sizeof(MARKS[0]); i++) {
    if (has_token(key, MARKS[i])) return 1;
  }
  return 0;
}

const char *dsh_audio_accent_label(const char *accent) {
  if (accent != NULL && eq_ci(accent, "uk")) return "英音";
  if (accent != NULL && eq_ci(accent, "us")) return "美音";
  return "发音";
}

static const char *const PLAYABLE_EXTENSIONS[] = {
    ".mp3", ".m4a", ".aac", ".wav", ".ogg", ".oga", ".opus", ".flac",
};

int dsh_audio_playable_extension_count(void) {
  return (int)(sizeof(PLAYABLE_EXTENSIONS) / sizeof(PLAYABLE_EXTENSIONS[0]));
}

const char *dsh_audio_playable_extension_at(int index) {
  if (index < 0 || index >= dsh_audio_playable_extension_count()) return NULL;
  return PLAYABLE_EXTENSIONS[index];
}

const char *dsh_audio_extension_of(const char *key) {
  if (key == NULL) return "";
  const char *dot = NULL;
  for (const char *p = key; *p != '\0'; p++) {
    if (*p == '.') dot = p;
    if (*p == '/' || *p == '\\') dot = NULL;
  }
  return (dot != NULL) ? dot : "";
}

/**
 * 这个键**看起来是不是音频**（扩展名 ∈ 能播的那几种 ∪ `.spx`）。
 *
 * 用它把「音频键的扩展名变体」那一轮**限定住**：请求 `theme.css` 而卷里恰好只有
 * `theme.wav` 时不该把音频取回去 —— 取到**错的东西**比取不到更坏，而且 `.spx` 也必须
 * 算进去：它正是「链接写 `.spx`、卷里其实是别的」那一整类情况的入口。
 */
int dsh_audio_is_audio_name(const char *key) {
  const char *ext = dsh_audio_extension_of(key);
  if (ext[0] == '\0') return 0;
  if (eq_ci(ext, ".spx")) return 1;
  for (int i = 0; i < dsh_audio_playable_extension_count(); i++) {
    if (eq_ci(ext, PLAYABLE_EXTENSIONS[i])) return 1;
  }
  return 0;
}

int dsh_audio_is_speex_name(const char *key) {
  const char *ext = dsh_audio_extension_of(key);
  return eq_ci(ext, ".spx") ? 1 : 0;
}

int dsh_audio_looks_playable(const char *key) {
  if (key == NULL || key[0] == '\0') return 0;
  const char *ext = dsh_audio_extension_of(key);
  for (size_t i = 0; i < sizeof(PLAYABLE_EXTENSIONS) / sizeof(PLAYABLE_EXTENSIONS[0]); i++) {
    if (eq_ci(ext, PLAYABLE_EXTENSIONS[i])) return 1;
  }
  if (dsh_audio_is_speex_name(key)) return 1;
  /* 认不出的扩展名也不急着否定（mdd 里什么怪名字都有）*/
  return 1;
}

int dsh_audio_candidate_keys(const char *key, char ***out, int64_t *out_count) {
  if (out != NULL) *out = NULL;
  if (out_count != NULL) *out_count = 0;
  if (out == NULL || out_count == NULL) {
    dsh_set_last_error("dsh_audio_candidate_keys：出参不能为空");
    return -1;
  }
  if (key == NULL || key[0] == '\0') return 0;

  const char *trimmed = key;
  while (*trimmed == '\\' || *trimmed == '/') trimmed++;
  /* 词干 = 去掉最后一个扩展名（点不在开头才算）*/
  const char *dot = NULL;
  for (const char *p = trimmed; *p != '\0'; p++) {
    if (*p == '.') dot = p;
    if (*p == '/' || *p == '\\') dot = NULL;
  }
  const size_t stem_len = (dot != NULL && dot != trimmed) ? (size_t)(dot - trimmed)
                                                          : strlen(trimmed);
  const int max = dsh_audio_playable_extension_count() + 1;
  char **list = (char **)dsh_mem_alloc((size_t)max * sizeof(char *));
  if (list == NULL) {
    dsh_set_last_error("内存不足：音频候选键名");
    return -1;
  }
  int64_t count = 0;

  /* `Push`：空值丢掉、**大小写不敏感去重**（保留先出现的写法）*/
  for (int pass = 0; pass < 2; pass++) {
    char value[1024];
    if (pass == 0) {
      for (int e = 0; e < dsh_audio_playable_extension_count(); e++) {
        if (stem_len + strlen(PLAYABLE_EXTENSIONS[e]) + 1 > sizeof(value)) continue;
        memcpy(value, trimmed, stem_len);
        value[stem_len] = '\0';
        strncat(value, PLAYABLE_EXTENSIONS[e], sizeof(value) - strlen(value) - 1);
        int dup = 0;
        for (int64_t i = 0; i < count; i++) {
          if (eq_ci(list[i], value)) {
            dup = 1;
            break;
          }
        }
        if (dup) continue;
        list[count] = dsh_mem_strdup(value);
        if (list[count] == NULL) {
          dsh_audio_keys_free(list, count);
          dsh_set_last_error("内存不足：音频候选键名");
          return -1;
        }
        count++;
      }
    } else {
      /* 原始键垫底 */
      int dup = 0;
      for (int64_t i = 0; i < count; i++) {
        if (eq_ci(list[i], trimmed)) {
          dup = 1;
          break;
        }
      }
      if (dup) continue;
      list[count] = dsh_mem_strdup(trimmed);
      if (list[count] == NULL) {
        dsh_audio_keys_free(list, count);
        dsh_set_last_error("内存不足：音频候选键名");
        return -1;
      }
      count++;
    }
  }

  if (count == 0) {
    dsh_release(list);
    return 0;
  }
  *out = list;
  *out_count = count;
  return 0;
}

void dsh_audio_keys_free(char **keys, int64_t count) {
  if (keys == NULL) return;
  for (int64_t i = 0; i < count; i++) {
    if (keys[i] != NULL) dsh_release(keys[i]);
  }
  dsh_release(keys);
}

/* ── 挑哪一条念 ─────────────────────────────────────────────────────────── */

int64_t dsh_audio_pick(const dsh_audio_candidate *candidates, int64_t count,
                       const char *accent_preference) {
  if (candidates == NULL || count <= 0) return -1;
  /* 第一遍：只认**词目发音**（例句不算）*/
  int64_t first_headword = -1;
  for (int64_t i = 0; i < count; i++) {
    if (!candidates[i].example && candidates[i].matched_key != NULL) {
      first_headword = i;
      break;
    }
  }
  if (first_headword < 0) return -1;
  if (accent_preference != NULL &&
      (eq_ci(accent_preference, "uk") || eq_ci(accent_preference, "us"))) {
    for (int64_t i = 0; i < count; i++) {
      if (candidates[i].example || candidates[i].matched_key == NULL) continue;
      if (candidates[i].accent != NULL && eq_ci(candidates[i].accent, accent_preference)) return i;
    }
  }
  return first_headword;
}

/* ── 从词条正文里抠音频引用 ─────────────────────────────────────────────── */

/** `sound://` 或 `snd://`（大小写不敏感）；命中时给出 scheme 的长度 */
static int scheme_at(const char *p, size_t avail, size_t *out_prefix_len) {
  static const char SOUND[] = "sound://";
  static const char SND[] = "snd://";
  if (avail >= 8) {
    int hit = 1;
    for (int i = 0; i < 8; i++) {
      if (lower(p[i]) != SOUND[i]) {
        hit = 0;
        break;
      }
    }
    if (hit) {
      *out_prefix_len = 8;
      return 1;
    }
  }
  if (avail >= 6) {
    int hit = 1;
    for (int i = 0; i < 6; i++) {
      if (lower(p[i]) != SND[i]) {
        hit = 0;
        break;
      }
    }
    if (hit) {
      *out_prefix_len = 6;
      return 1;
    }
  }
  return 0;
}

/** 键名字符类：不是半角引号 / 单引号 / 尖括号 / 右括号 / 逗号，也不是空白 */
static int is_key_char(char c) {
  if (c == '"' || c == '\'' || c == '>' || c == ')' || c == '<' || c == ',') return 0;
  return is_space_char(c) ? 0 : 1;
}

typedef struct {
  dsh_audio_ref *items;
  int64_t count;
  int64_t cap;
} ref_list;

static int ref_push(ref_list *list, char *key) {
  if (key == NULL || key[0] == '\0') {
    if (key != NULL) dsh_release(key);
    return 1;
  }
  for (int64_t i = 0; i < list->count; i++) {
    if (eq_ci(list->items[i].key, key)) {
      dsh_release(key);
      return 1;
    }
  }
  if (list->count == list->cap) {
    const int64_t cap = (list->cap == 0) ? 8 : list->cap * 2;
    dsh_audio_ref *next = (dsh_audio_ref *)dsh_mem_alloc((size_t)cap * sizeof(dsh_audio_ref));
    if (next == NULL) {
      dsh_set_last_error("内存不足：音频清单");
      dsh_release(key);
      return 0;
    }
    if (list->count > 0) memcpy(next, list->items, (size_t)list->count * sizeof(dsh_audio_ref));
    if (list->items != NULL) dsh_release(list->items);
    list->items = next;
    list->cap = cap;
  }
  dsh_audio_ref *ref = &list->items[list->count++];
  ref->key = key;
  const char *accent = dsh_audio_classify_accent(key);
  ref->accent = (accent != NULL) ? dsh_mem_strdup(accent) : NULL;
  ref->example = dsh_audio_is_example_key(key);
  return 1;
}

static void ref_list_dispose(ref_list *list) {
  if (list->items != NULL) dsh_release(list->items);
  list->items = NULL;
  list->count = 0;
  list->cap = 0;
}

int dsh_audio_extract(const char *html, size_t len, dsh_audio_ref **out, int64_t *out_count) {
  if (out != NULL) *out = NULL;
  if (out_count != NULL) *out_count = 0;
  if (out == NULL || out_count == NULL) {
    dsh_set_last_error("dsh_audio_extract：出参不能为空");
    return -1;
  }
  if (html == NULL || len == 0) return 0;

  ref_list list;
  memset(&list, 0, sizeof(list));

  /* ① `sound://x` / `snd://x` —— **按出现顺序**（顺序有意义：认不出英/美时取先出现的）*/
  for (size_t i = 0; i + 6 <= len; i++) {
    size_t prefix = 0;
    if (!scheme_at(html + i, len - i, &prefix)) continue;
    size_t end = i + prefix;
    while (end < len && is_key_char(html[end])) end++;
    if (end == i + prefix) continue; /* scheme 后面必须有东西 */
    char *key = dsh_audio_normalize(html + i + prefix, end - (i + prefix));
    if (!ref_push(&list, key)) goto fail;
    i = end - 1;
  }

  /* ② `<audio … name=…>`（牛津高阶那种「音频占位元素」）*/
  for (size_t i = 0; i + 6 <= len; i++) {
    if (html[i] != '<') continue;
    if (len - i < 6) break;
    char tag[6];
    for (int k = 0; k < 5; k++) tag[k] = lower(html[i + 1 + k]);
    tag[5] = '\0';
    if (strncmp(tag, "audio", 5) != 0) continue;
    /* `\b`：标签名后面不许是词字符 */
    const char after = (i + 6 < len) ? html[i + 6] : '\0';
    const int word_char = (after >= 'a' && after <= 'z') || (after >= 'A' && after <= 'Z') ||
                          (after >= '0' && after <= '9') || after == '_';
    if (word_char) continue;
    size_t tag_end = i + 5;
    while (tag_end < len && html[tag_end] != '>') tag_end++;
    if (tag_end >= len) continue;
    for (size_t p = i; p + 5 < tag_end; p++) {
      char probe[4];
      for (int k = 0; k < 4; k++) probe[k] = lower(html[p + k]);
      if (strncmp(probe, "name", 4) != 0) continue;
      size_t q = p + 4;
      while (q < tag_end && (html[q] == ' ' || html[q] == '\t')) q++;
      if (q >= tag_end || html[q] != '=') continue;
      q++;
      while (q < tag_end && (html[q] == ' ' || html[q] == '\t')) q++;
      if (q >= tag_end) continue;
      const char quote = html[q];
      if (quote != '"' && quote != '\'') continue;
      const size_t v = q + 1;
      size_t vend = v;
      while (vend < tag_end && html[vend] != quote) vend++;
      if (vend > v) {
        char *key = dsh_audio_normalize(html + v, vend - v);
        if (!ref_push(&list, key)) goto fail;
      }
      break;
    }
    i = tag_end;
  }

  *out = list.items;
  *out_count = list.count;
  return 0;

fail:
  for (int64_t i = 0; i < list.count; i++) {
    if (list.items[i].key != NULL) dsh_release(list.items[i].key);
    if (list.items[i].accent != NULL) dsh_release(list.items[i].accent);
  }
  ref_list_dispose(&list);
  return -1;
}

void dsh_audio_refs_free(dsh_audio_ref *refs, int64_t count) {
  if (refs == NULL) return;
  for (int64_t i = 0; i < count; i++) {
    if (refs[i].key != NULL) dsh_release(refs[i].key);
    if (refs[i].accent != NULL) dsh_release(refs[i].accent);
  }
  dsh_release(refs);
}