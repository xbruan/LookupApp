/* 豆包在线语音（单向流式 HTTP / SSE）—— 约定、理由与三条实测硬约束见 `dsh_doubao.h` 顶上那段。 */

#include "audio/dsh_doubao.h"

#include "crypto/dsh_sha256.h"
#include "dsh_internal.h"
#include "json_reader.h"
#include "json_writer.h"
#include "platform/dsh_time.h"
#include "text/dsh_base64.h"

#include <stdio.h>
#include <string.h>

/* ══════════════════════════════════════════════════════════════════════════
   静态判断：模型版本、用哪个音色
   ══════════════════════════════════════════════════════════════════════════ */

/** `s` 是不是以 `suffix` 结尾（大小写敏感 —— 音色 id 是服务端定的字面）*/
static int ends_with(const char *s, const char *suffix) {
  size_t n, m;
  if (s == NULL || suffix == NULL) return 0;
  n = strlen(s);
  m = strlen(suffix);
  return n >= m && strcmp(s + (n - m), suffix) == 0;
}

const char *dsh_doubao_resource_for(const char *speaker) {
  /* `_mars_bigtts` / `_moon_bigtts` 结尾的走 1.0，其余走 2.0（别自己加规则） */
  if (ends_with(speaker, "_mars_bigtts") || ends_with(speaker, "_moon_bigtts")) {
    return DSH_TTS_RESOURCE_1_0;
  }
  return DSH_TTS_RESOURCE_2_0;
}

const char *dsh_doubao_speaker_for(int mixed, const char *language, const char *speaker_en,
                                   const char *speaker_zh) {
  int want_zh = 0;

  /* 中英混排（或文本本来就是中文）走中文音色 —— 英文音色念混排服务端会不报错地回
   * 空句子，所以这条不是「哪个好听」的偏好，是「不照做就出不了声」。 */
  if (mixed) want_zh = 1;
  else if (language != NULL && strcmp(language, "zh") == 0) want_zh = 1;

  if (want_zh) {
    if (speaker_zh != NULL && speaker_zh[0] != 0) return speaker_zh;
    /* 中文音色没配：**回落到英文音色**（用户只配一个也能出声，功能不缺）*/
    if (speaker_en != NULL && speaker_en[0] != 0) return speaker_en;
    return NULL;
  }

  if (speaker_en != NULL && speaker_en[0] != 0) return speaker_en;
  if (speaker_zh != NULL && speaker_zh[0] != 0) return speaker_zh;
  return NULL;
}

/* ══════════════════════════════════════════════════════════════════════════
   检测用的样本词
   ══════════════════════════════════════════════════════════════════════════ */

/* 表逐条照参考实现的 SampleWords（32 个语种，顺序也照抄）。
 * ⚠️ 它只服务一件事：**「检测凭据」那次请求念哪个词** —— 所以检查标准是「这个语种的人
 * 一眼认得、而且短」（按字符计费），不是「覆盖多少词汇」。 */
static const struct {
  const char *language;
  const char *word;
} DSH_SAMPLE_WORDS[] = {
    {"en", "apple"},     {"zh", "苹果"},      {"ja", "りんご"},    {"ko", "사과"},
    {"fr", "bonjour"},   {"de", "Hallo"},     {"es", "manzana"},   {"it", "mela"},
    {"pt", "maçã"},      {"ru", "яблоко"},    {"ar", "تفاحة"},     {"th", "แอปเปิล"},
    {"vi", "táo"},       {"hi", "सेब"},       {"tr", "elma"},      {"pl", "jabłko"},
    {"nl", "appel"},     {"sv", "äpple"},     {"el", "μήλο"},      {"he", "תפוח"},
    {"uk", "яблуко"},    {"id", "apel"},      {"ur", "سیب"},       {"ta", "ஆப்பிள்"},
    {"bn", "আপেল"},      {"cs", "jablko"},    {"da", "æble"},      {"fi", "omena"},
    {"hu", "alma"},      {"no", "eple"},      {"ro", "măr"},       {"fa", "سیب"},
};

/** 小写 ASCII（语种码是 ASCII；`EN` 与 `en` 必须命中同一条）*/
static int ascii_lower(int c) { return (c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c; }

static int same_language(const char *a, const char *b) {
  size_t i;
  if (a == NULL || b == NULL) return 0;
  for (i = 0;; i++) {
    /* 区域标记只看到 `-` 为止（`zh-CN` 就是 `zh`），与 `dsh_language_primary` 同一条约定。
     * ⚠️ 少了这一条 `zh-CN` 会落到「表外」那一档去。 */
    int ca = (a[i] == '-') ? 0 : ascii_lower((unsigned char)a[i]);
    int cb = (b[i] == '-') ? 0 : ascii_lower((unsigned char)b[i]);
    if (ca != cb) return 0;
    if (ca == 0) return 1;
  }
}

const char *dsh_doubao_sample_word(const char *language) {
  size_t i;
  for (i = 0; i < sizeof(DSH_SAMPLE_WORDS) / sizeof(DSH_SAMPLE_WORDS[0]); i++) {
    if (same_language(language, DSH_SAMPLE_WORDS[i].language)) return DSH_SAMPLE_WORDS[i].word;
  }
  /* 表外的语种：参考实现的兜底是 `"hello"`（不是空串 —— 空串会让请求体拼不出来）*/
  return "hello";
}

/** 两个默认音色在界面上的官网名（参考实现的 `DefaultSpeakerEnName/ZhName`）*/
#define DSH_DOUBAO_DEFAULT_SPEAKER_EN_NAME "Dacey"
#define DSH_DOUBAO_DEFAULT_SPEAKER_ZH_NAME "Vivi"

/** 两个音色 id 是不是同一个（大小写不敏感 —— 音色 id 是用户手打进去的）*/
static int same_speaker(const char *a, const char *b) {
  size_t i;
  if (a == NULL || b == NULL) return 0;
  for (i = 0; a[i] != '\0' && b[i] != '\0'; i++) {
    char ca = a[i];
    char cb = b[i];
    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
    if (ca != cb) return 0;
  }
  return a[i] == '\0' && b[i] == '\0';
}

const char *dsh_doubao_speaker_label(const char *speaker) {
  if (speaker == NULL || speaker[0] == '\0') return "";
  if (same_speaker(speaker, DSH_DOUBAO_DEFAULT_SPEAKER_EN)) return DSH_DOUBAO_DEFAULT_SPEAKER_EN_NAME;
  if (same_speaker(speaker, DSH_DOUBAO_DEFAULT_SPEAKER_ZH)) return DSH_DOUBAO_DEFAULT_SPEAKER_ZH_NAME;
  return speaker;
}

/* ══════════════════════════════════════════════════════════════════════════
   响度补偿（官方 `loudness_rate`）
   ══════════════════════════════════════════════════════════════════════════ */

/** 设置里的值是用户填的，夹到官方范围（与参考实现的 `ClampLoudness` 逐字同约定）*/
static int clamp_loudness(int rate) {
  if (rate < -50) return -50;
  if (rate > 100) return 100;
  return rate;
}

int dsh_doubao_loudness_for(const char *speaker, const char *speaker_en, int has_loud_en, int loud_en,
                            const char *speaker_zh, int has_loud_zh, int loud_zh) {
  if (speaker == NULL || speaker[0] == 0) return 0;
  /* ① 设置里的滑块优先，但只对**它对应的那个音色**生效（参考实现同一条）*/
  if (same_speaker(speaker, speaker_en) && has_loud_en) return clamp_loudness(loud_en);
  if (same_speaker(speaker, speaker_zh) && has_loud_zh) return clamp_loudness(loud_zh);
  /* ② 内置表：两个默认音色压到下限（它们是最响的两个）。③ 表外的音色 = 0（不补偿）。 */
  if (same_speaker(speaker, DSH_DOUBAO_DEFAULT_SPEAKER_EN) ||
      same_speaker(speaker, DSH_DOUBAO_DEFAULT_SPEAKER_ZH)) {
    return DSH_DOUBAO_BUILTIN_LOUDNESS;
  }
  return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
   请求体
   ══════════════════════════════════════════════════════════════════════════ */

char *dsh_doubao_request_body(const char *text, const char *speaker, int speech_rate,
                              int loudness_rate) {
  dsh_json *j;
  char additions[192];

  if (text == NULL || text[0] == 0) {
    dsh_set_last_error("豆包语音：没有要念的文本");
    return NULL;
  }
  if (speaker == NULL || speaker[0] == 0) {
    dsh_set_last_error("豆包语音：没有音色（去「语音」那一页配一个）");
    return NULL;
  }
  /* `speech_rate ∈ [-50, 100]` —— 越界**如实拒**，不悄悄夹 */
  if (speech_rate < -50 || speech_rate > 100) {
    dsh_set_last_error("豆包语音：语速必须在 -50~100 之间（给的是 %d）", speech_rate);
    return NULL;
  }

  /* `additions` 是一个**JSON 串**（不是对象）—— 服务端就是这么收的。
   * ⚠️ 这里**刻意不写 `explicit_language`**：它的语义是「只念这个语种」，而词典正文中英混排
   * 是常态；传错了（或传了表外的值）会让请求失败或直接出不了声。 */
  snprintf(additions, sizeof(additions),
           "{\"post_process\":{\"pitch\":0},\"disable_markdown_filter\":true}");

  j = dsh_json_new();
  if (j == NULL) {
    dsh_set_last_error("豆包语音：内存不足");
    return NULL;
  }

  dsh_json_object_begin(j);
  dsh_json_key(j, "user");
  dsh_json_object_begin(j);
  dsh_json_kv_str(j, "uid", "lookup");
  dsh_json_object_end(j);

  dsh_json_key(j, "req_params");
  dsh_json_object_begin(j);
  dsh_json_kv_str(j, "text", text);
  dsh_json_kv_str(j, "speaker", speaker);
  dsh_json_key(j, "audio_params");
  dsh_json_object_begin(j);
  dsh_json_kv_str(j, "format", DSH_TTS_FORMAT);
  dsh_json_kv_int(j, "sample_rate", DSH_TTS_SAMPLE_RATE);
  dsh_json_kv_int(j, "speech_rate", speech_rate);
  /* 响度补偿：设置里那两个滑块 + 两个默认音色的内置值都靠它落地。**总是发**（0 也发）——
   * 少一个字段与服务端「默认 0」虽然等价，但少一个字段就少一条可核对的证据。 */
  dsh_json_kv_int(j, "loudness_rate", clamp_loudness(loudness_rate));
  dsh_json_kv_int(j, "bit_rate", DSH_TTS_BIT_RATE);
  dsh_json_object_end(j);
  dsh_json_kv_str(j, "additions", additions);
  dsh_json_object_end(j);
  dsh_json_object_end(j);

  {
    char *out = dsh_json_take(j);
    dsh_json_free(j);
    return out;
  }
}

/* ══════════════════════════════════════════════════════════════════════════
   错误码 → 人话
   ══════════════════════════════════════════════════════════════════════════ */

const char *dsh_doubao_error_sentence(int64_t code) {
  switch (code) {
    case 0:
    case 20000000:
      return "";
    case 40000001:
      return "音色 ID 与模型版本不配套（或者文本、语速这些参数不对）—— 去「语音」那一页核对音色。";
    case 40300001:
      return "API Key 不对，或者语音服务没开通 / 实名认证没做完。";
    case 40402003:
      return "这段文本太长，语音服务退回来了（正常不该出现，我们是按段发的）。";
    case 45000000:
      return "语音服务说这次请求有问题（参数或文本）。";
    case 55000000:
      return "火山引擎服务端出错，稍后再试一次。";
    default:
      return NULL;
  }
}

/* ══════════════════════════════════════════════════════════════════════════
   SSE 回包 → 音频字节
   ══════════════════════════════════════════════════════════════════════════ */

/** 把一段字节接到输出缓冲后面（不够就按 2 倍长）*/
static int append(uint8_t **buffer, size_t *len, size_t *capacity, const uint8_t *data, size_t n) {
  if (n == 0) return 0;
  if (*len + n > *capacity) {
    size_t want = (*capacity == 0) ? (n + 64) : *capacity;
    uint8_t *grown;
    while (want < *len + n) want *= 2;
    grown = (uint8_t *)dsh_mem_alloc(want);
    if (grown == NULL) return -1;
    if (*len > 0 && *buffer != NULL) memcpy(grown, *buffer, *len);
    if (*buffer != NULL) dsh_release(*buffer);
    *buffer = grown;
    *capacity = want;
  }
  memcpy(*buffer + *len, data, n);
  *len += n;
  return 0;
}

/** 从一行 `data: {...}` 里取 JSON 那一截（前导空白也吃掉）*/
static const char *payload_of(const char *line, size_t line_len, size_t *out_len) {
  size_t i = 0;
  /* `data:` 后面可以有一个空格（SSE 的规矩）*/
  while (i < line_len && (line[i] == ' ' || line[i] == '\t')) i++;
  if (i + 5 > line_len || strncmp(line + i, "data:", 5) != 0) return NULL;
  i += 5;
  while (i < line_len && (line[i] == ' ' || line[i] == '\t')) i++;
  *out_len = line_len - i;
  return line + i;
}

int dsh_doubao_parse(int status, const char *body, size_t len, dsh_tts_result *out) {
  size_t at = 0;
  uint8_t *audio = NULL;
  size_t audio_len = 0;
  size_t capacity = 0;
  int64_t last_bad_code = 0;
  char *last_bad_message = NULL;
  int64_t words = 0;
  int saw_any_event = 0;

  if (out == NULL) return -1;
  memset(out, 0, sizeof(*out));
  out->mime = "audio/mpeg";

  if (status != 200) {
    /* 平台层报告「没答上来」或「服务端给了非 200」 —— 这一档与业务码无关 */
    char buf[160];
    if (status == 0) {
      out->reason = dsh_mem_strdup("语音请求没发出去（断网或者超时了）。");
    } else {
      snprintf(buf, sizeof(buf), "语音服务没答上来（HTTP %d）。", status);
      out->reason = dsh_mem_strdup(buf);
    }
    return 0;
  }

  if (body == NULL || len == 0) {
    out->reason = dsh_mem_strdup("语音服务回了空内容。");
    return 0;
  }

  /* 一行一行地看（SSE）*/
  while (at < len) {
    size_t line_end = at;
    size_t payload_len = 0;
    const char *payload;
    dsh_json_doc *doc = NULL;

    while (line_end < len && body[line_end] != '\n') line_end++;
    payload = payload_of(body + at, line_end - at, &payload_len);
    at = line_end + 1; /* 跳过 `\n`；行尾的 `\r` 由 base64 那一步忽略 */

    if (payload == NULL || payload_len == 0) continue;
    if (dsh_json_parse(payload, payload_len, &doc) != 0) {
      /* ⚠️ 单行解析不了**不算致命**（SSE 里可能混着 `event:` 之类的行）；但**一行都不是
       * JSON** 最后会走「没拿到音频」那一档，所以坏包不会被当成成功。 */
      continue;
    }
    saw_any_event = 1;
    {
      const dsh_json_node *root = dsh_json_doc_root(doc);
      int64_t code = 0;
      const dsh_json_node *data_node;
      const dsh_json_node *usage;

      if (root != NULL && dsh_json_is_object(root)) {
        if (dsh_json_i64_value(dsh_json_object_get(root, "code"), &code) == 0) {
          if (code != DSH_TTS_CODE_OK && code != DSH_TTS_CODE_OK_ALT) {
            /* 非成功码：记下来（最后那句人话按它给），但**继续看后面的行** ——
               服务端有时在错误码之后还会补一行说明 */
            const dsh_json_node *m = dsh_json_object_get(root, "message");
            size_t mlen = 0;
            const char *ms = (m == NULL) ? NULL : dsh_json_str_value(m, &mlen);
            last_bad_code = code;
            if (last_bad_message != NULL) dsh_release(last_bad_message);
            last_bad_message = NULL;
            if (ms != NULL) {
              last_bad_message = (char *)dsh_mem_alloc(mlen + 1);
              if (last_bad_message != NULL) {
                memcpy(last_bad_message, ms, mlen);
                last_bad_message[mlen] = 0;
              }
            }
          }
        }

        data_node = dsh_json_object_get(root, "data");
        if (data_node != NULL && dsh_json_is_string(data_node)) {
          size_t b64_len = 0;
          const char *b64 = dsh_json_str_value(data_node, &b64_len);
          uint8_t *chunk = NULL;
          size_t chunk_len = 0;
          if (b64 != NULL && b64_len > 0) {
            if (dsh_base64_decode(b64, b64_len, &chunk, &chunk_len) != 0) {
              dsh_json_doc_free(doc);
              if (audio != NULL) dsh_release(audio);
              if (last_bad_message != NULL) dsh_release(last_bad_message);
              out->reason = dsh_mem_strdup("语音回包里有一段音频解不出来（base64 坏了）。");
              return 0;
            }
            if (append(&audio, &audio_len, &capacity, chunk, chunk_len) != 0) {
              if (chunk != NULL) dsh_release(chunk);
              dsh_json_doc_free(doc);
              if (audio != NULL) dsh_release(audio);
              if (last_bad_message != NULL) dsh_release(last_bad_message);
              dsh_set_last_error("豆包语音：内存不足");
              return -1;
            }
            if (chunk != NULL) dsh_release(chunk);
          }
        }

        /* 计费字数（要请求头里点亮那个 `*` 才有）*/
        usage = dsh_json_object_get(root, "usage");
        if (usage != NULL) {
          int64_t v = 0;
          if (dsh_json_i64_value(dsh_json_object_get(usage, "text_words"), &v) == 0) words = v;
        }
      }
    }
    dsh_json_doc_free(doc);
  }

  out->code = last_bad_code;
  out->text_words = words;

  if (last_bad_code != 0) {
    const char *known = dsh_doubao_error_sentence(last_bad_code);
    if (audio != NULL) dsh_release(audio);
    if (known != NULL && known[0] != 0) {
      out->reason = dsh_mem_strdup(known);
    } else {
      char buf[320];
      snprintf(buf, sizeof(buf), "语音服务出错（code %lld）：%s", (long long)last_bad_code,
               last_bad_message == NULL ? "服务端没有给出说明" : last_bad_message);
      out->reason = dsh_mem_strdup(buf);
    }
    if (last_bad_message != NULL) dsh_release(last_bad_message);
    return 0;
  }

  if (last_bad_message != NULL) dsh_release(last_bad_message);

  if (audio == NULL || audio_len == 0) {
    /* ★ **这一档是实测踩出来的**：服务端回 `code 0`、**一个字节的音频都没有**，最常见
     * 的成因是「拿英文音色去念中英混排」。所以这里**必须单独判成失败**，而且那句话要把
     * 原因说清 —— 否则用户看到的就是「点了没声音」，无处可查。 */
    if (audio != NULL) dsh_release(audio);
    out->reason = dsh_mem_strdup(
        saw_any_event ? "语音服务没念出这段文本（回的是空音频）。最常见的原因是音色与文本不匹配 —— "
                        "中英混排要用支持混读的中文音色。"
                      : "语音服务的回包里没有音频（也不是认识的错误格式）。");
    return 0;
  }

  out->ok = 1;
  out->audio = audio;
  out->audio_len = audio_len;
  return 0;
}

void dsh_doubao_result_free(dsh_tts_result *result) {
  if (result == NULL) return;
  if (result->audio != NULL) dsh_release(result->audio);
  if (result->reason != NULL) dsh_release(result->reason);
  memset(result, 0, sizeof(*result));
}
