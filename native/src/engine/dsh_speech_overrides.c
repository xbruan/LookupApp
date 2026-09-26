/* 见 engine/dsh_speech_overrides.h —— 两条语音接口共用的那一份「这一次的覆盖」。 */

#include "engine/dsh_speech_overrides.h"

#include "dsh_internal.h"
#include "json_reader.h"

#include <string.h>

/** 把 JSON 里那个键的字符串抄进定长缓冲（超长就截断 —— 音色 id / 语种码不该有那么长）*/
static void copy_bounded(char *dst, size_t cap, const char *src) {
  size_t i = 0;
  if (cap == 0) return;
  if (src != NULL) {
    for (; src[i] != '\0' && i + 1 < cap; i++) dst[i] = src[i];
  }
  dst[i] = '\0';
}

int dsh_speech_overrides_source_known(const char *source) {
  if (source == NULL) return 0;
  return strcmp(source, "dict") == 0 || strcmp(source, "online") == 0 ||
         strcmp(source, "system") == 0;
}

int dsh_speech_overrides_parse(const char *overrides_json, dsh_speech_overrides *out,
                               const char *what) {
  if (out == NULL) return -1;
  memset(out, 0, sizeof(*out));
  if (overrides_json == NULL || overrides_json[0] == '\0') return 0;

  dsh_json_doc *doc = NULL;
  if (dsh_json_parse(overrides_json, strlen(overrides_json), &doc) != 0 || doc == NULL) {
    dsh_set_last_error("%s：overrides_json 不是合法 JSON", (what != NULL) ? what : "语音规划");
    return -1;
  }
  const dsh_json_node *root = dsh_json_doc_root(doc);
  int64_t n = 0;

  /* ⚠️ **认不出的键一律忽略**（不报错）：向前兼容 —— 将来接口定义加了新键，
   *    老的壳多传一个键不该让发音整个失败。 */
  const char *src = dsh_json_str_value(dsh_json_object_get(root, "source"), NULL);
  if (src != NULL && src[0] != '\0') {
    out->has_source = 1;
    copy_bounded(out->source, sizeof(out->source), src);
  }
  const char *voice = dsh_json_str_value(dsh_json_object_get(root, "voiceId"), NULL);
  if (voice != NULL && voice[0] != '\0') {
    out->has_voice = 1;
    copy_bounded(out->voice, sizeof(out->voice), voice);
  }
  const char *lang = dsh_json_str_value(dsh_json_object_get(root, "language"), NULL);
  if (lang != NULL && lang[0] != '\0') {
    out->has_language = 1;
    copy_bounded(out->language, sizeof(out->language), lang);
  }
  if (dsh_json_i64_value(dsh_json_object_get(root, "loudness"), &n) == 0) {
    out->has_loudness = 1;
    out->loudness = (int)n;
    /* 夹到官方范围（与 `dsh_doubao_loudness_for` 同一条约定）*/
    if (out->loudness < -50) out->loudness = -50;
    if (out->loudness > 100) out->loudness = 100;
  }
  if (dsh_json_i64_value(dsh_json_object_get(root, "gainTenthsDb"), &n) == 0) {
    out->has_gain = 1;
    out->gain_tenths = (int)n;
  }
  dsh_json_doc_free(doc);
  return 0;
}
