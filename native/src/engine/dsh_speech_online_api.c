/* ── 在线语音（豆包）：plan 说该发什么、accept 说回包是什么意思，平台层只管 POST 与整段收 SSE ──
   音色怎么选（中英混排必须走中文音色）、模型版本、SSE 怎么解、code 0 但没音频算失败、错误码翻人话
   一律在核心里判，一个字都不许挪到平台层；接口名见 `abi/lookup.abi.json`。 */

#include "audio/dsh_doubao.h"
#include "crypto/dsh_sha256.h"
#include "dsh_internal.h"
#include "engine/dsh_engine_internal.h"
#include "engine/dsh_settings.h"
#include "engine/dsh_speech_overrides.h"
#include "json_reader.h"
#include "json_writer.h"
#include "platform/dsh_time.h"
#include "text/dsh_language.h"

#include <stdio.h>
#include <string.h>

/** 请求 id：服务端排错要用（形状按 UUID，只用时间戳 + 文本哈希生成，不引随机源）*/
static void make_request_id(const char *seed, char out[37]) {
  char payload[256];
  char hex[65];
  snprintf(payload, sizeof(payload), "%lld|%s", (long long)dsh_now_ms(), seed == NULL ? "" : seed);
  dsh_sha256_hex(payload, strlen(payload), hex);
  snprintf(out, 37, "%.8s-%.4s-%.4s-%.4s-%.12s", hex, hex + 8, hex + 12, hex + 16, hex + 20);
}

/** 失败那一档的统一出口（人话 + 机读码；形状与成功那份同键，外壳不必分支认识它）*/
static enum dsh_error online_fail(char **out_json, const char *why, const char *reason) {
  dsh_json *j = dsh_json_new();
  char *out;
  if (j == NULL) {
    dsh_set_last_error("dsh_speech_online_plan：内存不足");
    return DSH_E_OOM;
  }
  dsh_json_object_begin(j);
  dsh_json_kv_bool(j, "ok", 0);
  dsh_json_kv_bool(j, "needsHttp", 0);
  dsh_json_kv_str(j, "why", why == NULL ? "" : why);
  dsh_json_kv_str(j, "reason", reason == NULL ? "" : reason);
  dsh_json_object_end(j);
  out = dsh_json_take(j);
  dsh_json_free(j);
  if (out == NULL) return DSH_E_OOM;
  *out_json = out;
  return DSH_OK;
}

/* ── 共用：一次在线请求里内核算得出来的那几样 ──────────────────────────────────
   `plan` 与 `test_plan` 发的是**同一份东西**，所以拼请求只写一遍：
   语速换算 → 模型版本 → 请求体 → 请求 id。**同一约定不许有两个来源。 */

typedef struct {
  char *body;           /**< 请求体（新分配；调用方 `dsh_release`）*/
  const char *resource; /**< 模型版本（设置里填过就用它，否则按音色后缀推）*/
  char request_id[37];
} online_request;

/** 拼一次请求。语速要**换算**：设置里是 -10..10、接口收 -50..100，约定 clamp(rate * 5, -50, 100) ——
 * 直接把 -10..10 递过去服务端当「几乎没变速」，用户拖到两端会听不出差别。
 * @return 0 = 成功（`out->body` 要还）；-1 = 失败（last_error 已写） */
static int build_request(const dsh_settings *s, const char *text, const char *speaker,
                         const dsh_speech_overrides *ov, online_request *out) {
  const dsh_speech_settings *speech = dsh_settings_speech(s);
  int rate;
  int loudness;

  out->body = NULL;
  out->resource = NULL;
  out->request_id[0] = 0;

  /* 模型版本必须与音色配套 —— 配错了服务端回 40000001。
   * ⚠️ 这一格今天**表示不出「没填」**：设置读盘时就把默认值 `seed-tts-2.0` 落进去了，
   * 所以下面「按音色后缀推」那一支在 `plan` 这条路上走不到 —— 是刻意的现状，不是漏移植。 */
  out->resource = (speech != NULL && speech->doubao_resource_id != NULL &&
                   speech->doubao_resource_id[0] != 0)
                      ? speech->doubao_resource_id
                      : dsh_doubao_resource_for(speaker);

  {
    int setting_rate = (speech != NULL) ? speech->rate : 0;
    rate = setting_rate * 5;
    if (rate < -50) rate = -50;
    if (rate > 100) rate = 100;
  }

  /* 响度补偿：**设置里那两个滑块优先，没设过才用内置表**（两个默认音色 -50）。
   * ⚠️ 它与语速那条不是一回事：响度直接用官方刻度（滑块值就是 `loudness_rate` 本身），
   * 不做乘 5 换算；且**覆盖优先** —— 界面「平衡音量」要拿 `loudness: 0` 量中性电平。 */
  loudness = (ov != NULL && ov->has_loudness)
                 ? ov->loudness
                 : dsh_doubao_loudness_for(speaker,
                                           speech != NULL ? speech->doubao_speaker_en : NULL,
                                           speech != NULL ? speech->has_loudness_en : 0,
                                           speech != NULL ? speech->loudness_en : 0,
                                           speech != NULL ? speech->doubao_speaker_zh : NULL,
                                           speech != NULL ? speech->has_loudness_zh : 0,
                                           speech != NULL ? speech->loudness_zh : 0);

  out->body = dsh_doubao_request_body(text, speaker, rate, loudness);
  if (out->body == NULL) return -1; /* dsh_doubao_request_body 已把人话写进 last_error */
  make_request_id(text, out->request_id);
  return 0;
}

/** 释放 `build_request` 拼出来的东西 */
static void drop_request(online_request *req) {
  if (req->body != NULL) {
    dsh_release(req->body);
    req->body = NULL;
  }
}

/** 那四个头 + 一个计费开关（两个接口共用）。
 * ⚠️ 最后一个头少一个字符，回包就没有 `usage.text_words`（计费字数）—— 不是错，只是读不到。 */
static void write_headers(dsh_json *j, const char *api_key, const char *resource,
                          const char *request_id) {
  const char *names[5];
  const char *values[5];
  size_t i;
  char *key_copy = dsh_mem_strdup(api_key);
  char *id_copy = dsh_mem_strdup(request_id);

  names[0] = "X-Api-Key";
  values[0] = key_copy;
  names[1] = "X-Api-Resource-Id";
  values[1] = resource;
  names[2] = "X-Api-Request-Id";
  values[2] = id_copy;
  names[3] = "Content-Type";
  values[3] = "application/json";
  names[4] = "X-Control-Require-Usage-Tokens-Return";
  values[4] = "*";

  dsh_json_key(j, "headers");
  dsh_json_array_begin(j);
  for (i = 0; i < 5; i++) {
    dsh_json_array_begin(j);
    dsh_json_str(j, names[i]);
    dsh_json_str(j, values[i]);
    dsh_json_array_end(j);
  }
  dsh_json_array_end(j);

  if (key_copy != NULL) dsh_release(key_copy);
  if (id_copy != NULL) dsh_release(id_copy);
}

enum dsh_error dsh_speech_online_plan(dsh_engine *engine, const char *text, const char *dict_id,
                                      const char *overrides_json, char **out_json) {
  const struct dsh_settings *s;
  const dsh_speech_settings *speech;
  const char *api_key;
  const char *speaker_en;
  const char *speaker_zh;
  const char *speaker;
  const char *language = NULL;
  const char *basis = NULL;
  const char *basis_text = NULL;
  int mixed;
  dsh_speech_overrides ov;
  online_request req;
  dsh_json *j;
  char *out;

  dsh_clear_last_error();
  if (out_json == NULL) {
    dsh_set_last_error("dsh_speech_online_plan：out_json 不能为空");
    return DSH_E_INVALID_ARG;
  }
  if (engine == NULL) {
    dsh_set_last_error("dsh_speech_online_plan：引擎无效");
    return DSH_E_INVALID_ARG;
  }
  /* 「这一次的覆盖」：与 `dsh_speech_plan` 看到的是**同一份东西**（解析器也共用）*/
  if (dsh_speech_overrides_parse(overrides_json, &ov, "dsh_speech_online_plan") != 0) {
    return DSH_E_INVALID_ARG;
  }
  if (text == NULL || text[0] == 0) {
    return online_fail(out_json, "empty", "没有要念的文本。");
  }

  s = dsh_engine_settings(engine);
  speech = dsh_settings_speech(s);
  api_key = dsh_settings_volcengine_api_key(s);
  if (api_key == NULL || api_key[0] == 0) {
    return online_fail(out_json, "no-key",
                       "在线发音需要火山引擎凭据（与机器翻译共用同一把 API Key）—— "
                       "先去「语音」那一页填上。");
  }

  speaker_en = (speech != NULL) ? speech->doubao_speaker_en : NULL;
  speaker_zh = (speech != NULL) ? speech->doubao_speaker_zh : NULL;

  /* 语种与「混不混」由内核那套统一判定给（字形 + 词典标题两条线索）。
   * ⚠️ 硬约定：中英混排**必须**走中文音色 —— 英文音色念混排服务端不报错、直接回空句子。 */
  {
    const char *title = NULL;
    if (dict_id != NULL && dict_id[0] != 0) {
      const dsh_stored_dict *d = dsh_settings_dict_by_id(s, dict_id);
      if (d != NULL) title = d->title;
    }
    dsh_language_decide(text, title, NULL, NULL, &language, &basis, &basis_text);
  }
  mixed = dsh_language_is_mixed(text);

  /* 覆盖里的语种/音色优先（与 `dsh_speech_plan` 同一条约定：见 dsh_speech_overrides.h）*/
  if (ov.has_language) {
    language = ov.language;
    basis = "override";
    basis_text = NULL;
  }
  speaker = ov.has_voice
                ? ov.voice
                : dsh_doubao_speaker_for(mixed, language, speaker_en, speaker_zh);
  if (speaker == NULL || speaker[0] == 0) {
    return online_fail(out_json, "no-speaker",
                       "在线发音还没配音色 —— 去「语音」那一页填英文音色（中文音色可选，"
                       "中英混排要用它）。");
  }

  /* 模型版本、语速换算、请求体、请求 id 全在 `build_request` 里（两个接口共用）。 */
  if (build_request(s, text, speaker, &ov, &req) != 0) {
    return online_fail(out_json, "bad-request", dsh_last_error_message());
  }

  j = dsh_json_new();
  if (j == NULL) {
    drop_request(&req);
    return DSH_E_OOM;
  }
  dsh_json_object_begin(j);
  dsh_json_kv_bool(j, "ok", 1);
  dsh_json_kv_bool(j, "needsHttp", 1);
  dsh_json_kv_str(j, "url", DSH_TTS_ENDPOINT);
  write_headers(j, api_key, req.resource, req.request_id);
  dsh_json_kv_str(j, "body", req.body);
  dsh_json_kv_str(j, "speaker", speaker);
  dsh_json_kv_str(j, "resourceId", req.resource);
  dsh_json_kv_str(j, "language", language == NULL ? "" : language);
  dsh_json_kv_bool(j, "mixed", mixed);
  dsh_json_kv_str(j, "basis", basis == NULL ? "" : basis);
  dsh_json_object_end(j);

  out = dsh_json_take(j);
  dsh_json_free(j);
  drop_request(&req);
  if (out == NULL) {
    dsh_set_last_error("dsh_speech_online_plan：内存不足");
    return DSH_E_OOM;
  }
  *out_json = out;
  return DSH_OK;
}

/* ── 检测凭据：拿一个样本词真发一次请求 ────────────────────────────────────────
   音色取**界面正在填的那两个**（用户改了 ID、还没写盘时，要测的必须是即将存下去的那个）；
   一次测两项 —— 「本来就没东西可测」与「服务端说不能用」必须逐项分开说，且不受在线总开关限制。 */

/** 一次检测最多测几项（英文 + 中文）*/
#define DSH_TEST_TARGETS 2

/** 全是空白 = 没填（界面上留空的格子就是这一档）*/
static int is_blank(const char *s) {
  size_t i;
  if (s == NULL) return 1;
  for (i = 0; s[i] != 0; i++) {
    if (s[i] != ' ' && s[i] != '\t' && s[i] != '\r' && s[i] != '\n') return 0;
  }
  return 1;
}

/** 没填凭据时那句人话（与 `plan` 的 no-key 那一档**逐字同一句**）*/
#define DSH_NO_KEY_SENTENCE                                                          \
  "在线发音需要火山引擎凭据（与机器翻译共用同一把 API Key）—— 先去「语音」那一页填上。"

enum dsh_error dsh_speech_online_test_plan(dsh_engine *engine, const char *speaker,
                                           const char *language, char **out_json) {
  const struct dsh_settings *s;
  const dsh_speech_settings *speech;
  const char *api_key;
  const char *target_speaker[DSH_TEST_TARGETS];
  const char *target_language[DSH_TEST_TARGETS];
  int count = 0;
  int i;
  dsh_json *j;
  char *out;

  dsh_clear_last_error();
  if (out_json == NULL) {
    dsh_set_last_error("dsh_speech_online_test_plan：out_json 不能为空");
    return DSH_E_INVALID_ARG;
  }
  if (engine == NULL) {
    dsh_set_last_error("dsh_speech_online_test_plan：引擎无效");
    return DSH_E_INVALID_ARG;
  }

  s = dsh_engine_settings(engine);
  speech = dsh_settings_speech(s);
  api_key = dsh_settings_volcengine_api_key(s);

  if (!is_blank(speaker)) {
    /* 指定了音色：只测它；语种认不出就按英文（样本词表里的 apple 失败概率最低）。 */
    const char *code = dsh_language_primary(language);
    target_speaker[0] = speaker;
    target_language[0] = (code[0] != 0) ? code : "en";
    count = 1;
  } else {
    /* 没指定：**英文 + 中文各测一次** —— 两个音色的 Key/Resource 配套关系虽一样，
     * 音色 ID 写错只有真发一次才发现，所以中文那一项也得测。 */
    target_speaker[0] = (speech != NULL) ? speech->doubao_speaker_en : NULL;
    target_language[0] = "en";
    target_speaker[1] = (speech != NULL) ? speech->doubao_speaker_zh : NULL;
    target_language[1] = "zh";
    count = DSH_TEST_TARGETS;
  }

  j = dsh_json_new();
  if (j == NULL) {
    dsh_set_last_error("dsh_speech_online_test_plan：内存不足");
    return DSH_E_OOM;
  }
  dsh_json_object_begin(j);
  dsh_json_kv_bool(j, "ok", 1);
  dsh_json_key(j, "plans");
  dsh_json_array_begin(j);
  for (i = 0; i < count; i++) {
    const char *who = target_speaker[i];
    const char *lang = target_language[i];
    const char *text = dsh_doubao_sample_word(lang);
    const char *reason = NULL;
    online_request req;
    int ready = 0;

    if (is_blank(who)) {
      /* 留空的格子：**没东西可测**，不是「服务端说它不能用」—— 两档不许混。 */
      reason = "这个音色没填（这一项测不了）";
    } else if (api_key == NULL || api_key[0] == 0) {
      reason = DSH_NO_KEY_SENTENCE;
    } else if (build_request(s, text, who, NULL, &req) != 0) {
      reason = dsh_last_error_message();
    } else {
      ready = 1;
    }

    dsh_json_object_begin(j);
    dsh_json_kv_bool(j, "ok", ready);
    dsh_json_kv_bool(j, "needsHttp", ready);
    dsh_json_kv_str(j, "speaker", is_blank(who) ? "" : who);
    dsh_json_kv_str(j, "language", lang);
    dsh_json_kv_str(j, "text", text);
    /* 这一项是**请求体的事实**：`explicit_language` 一律不传，所以恒为空串；
     * 界面那份 `DoubaoTest` 有这个字段，别让它去猜。 */
    dsh_json_kv_str(j, "explicitLanguage", "");
    dsh_json_kv_str(j, "reason", reason == NULL ? "" : reason);
    if (ready) {
      dsh_json_kv_str(j, "url", DSH_TTS_ENDPOINT);
      write_headers(j, api_key, req.resource, req.request_id);
      dsh_json_kv_str(j, "body", req.body);
      dsh_json_kv_str(j, "resourceId", req.resource);
      drop_request(&req);
    }
    dsh_json_object_end(j);
  }
  dsh_json_array_end(j);
  dsh_json_object_end(j);

  out = dsh_json_take(j);
  dsh_json_free(j);
  if (out == NULL) {
    dsh_set_last_error("dsh_speech_online_test_plan：内存不足");
    return DSH_E_OOM;
  }
  *out_json = out;
  return DSH_OK;
}

enum dsh_error dsh_speech_online_accept(dsh_engine *engine, const char *plan_json, int http_status,
                                        const char *response_body, int elapsed_ms, uint8_t **out_bytes,
                                        size_t *out_len, char **out_json) {
  dsh_json_doc *plan_doc = NULL;
  const dsh_json_node *plan_root = NULL;
  char *speaker = NULL;
  char *language = NULL;
  char *message = NULL;
  char *why = NULL;
  dsh_tts_result tts;
  dsh_json *j;
  char *out;

  (void)engine; /* 目前不需要引擎状态；签名留着是为了将来把音频缓存挂到引擎上 */

  dsh_clear_last_error();
  if (out_bytes == NULL || out_len == NULL || out_json == NULL) {
    dsh_set_last_error("dsh_speech_online_accept：出参不能为空");
    return DSH_E_INVALID_ARG;
  }
  *out_bytes = NULL;
  *out_len = 0;

  /* 从 plan 里把「回话要用」的几样捡出来（都是 plan 自己给的，不重新判一遍）；
   * 用循环 + 小工具做，免得每个键都写一遍分配 / 拷贝。 */
  if (plan_json != NULL && plan_json[0] != 0) {
    if (dsh_json_parse(plan_json, strlen(plan_json), &plan_doc) == 0) {
      plan_root = dsh_json_doc_root(plan_doc);
    }
  }
  {
    const char *keys[3];
    char **slots[3];
    size_t k;
    keys[0] = "speaker";
    slots[0] = &speaker;
    keys[1] = "language";
    slots[1] = &language;
    keys[2] = "why";
    slots[2] = &why;
    for (k = 0; k < 3; k++) {
      const dsh_json_node *node =
          (plan_root == NULL) ? NULL : dsh_json_object_get(plan_root, keys[k]);
      size_t len = 0;
      const char *text_value = (node == NULL) ? NULL : dsh_json_str_value(node, &len);
      if (text_value != NULL) {
        *slots[k] = (char *)dsh_mem_alloc(len + 1);
        if (*slots[k] != NULL) {
          memcpy(*slots[k], text_value, len);
          (*slots[k])[len] = 0;
        }
      }
    }
  }

  memset(&tts, 0, sizeof(tts));
  if (dsh_doubao_parse(http_status, response_body == NULL ? "" : response_body,
                       response_body == NULL ? 0 : strlen(response_body), &tts) != 0) {
    if (plan_doc != NULL) dsh_json_doc_free(plan_doc);
    if (speaker != NULL) dsh_release(speaker);
    if (language != NULL) dsh_release(language);
    if (why != NULL) dsh_release(why);
    return DSH_E_OOM;
  }

  if (tts.ok) {
    *out_bytes = tts.audio;
    *out_len = tts.audio_len;
    tts.audio = NULL; /* 所有权交给调用方（宿主用 dsh_release 还给内核）*/
    tts.audio_len = 0;
  } else {
    message = (tts.reason != NULL) ? dsh_mem_strdup(tts.reason) : dsh_mem_strdup("在线合成失败。");
  }

  j = dsh_json_new();
  if (j == NULL) {
    dsh_doubao_result_free(&tts);
    if (*out_bytes != NULL) {
      dsh_release(*out_bytes);
      *out_bytes = NULL;
      *out_len = 0;
    }
    if (message != NULL) dsh_release(message);
    if (plan_doc != NULL) dsh_json_doc_free(plan_doc);
    if (speaker != NULL) dsh_release(speaker);
    if (language != NULL) dsh_release(language);
    if (why != NULL) dsh_release(why);
    dsh_set_last_error("dsh_speech_online_accept：内存不足");
    return DSH_E_OOM;
  }
  dsh_json_object_begin(j);
  dsh_json_kv_bool(j, "ok", tts.ok);
  dsh_json_kv_str(j, "mime", tts.mime == NULL ? DSH_TTS_FORMAT_MIME : tts.mime);
  dsh_json_kv_i64(j, "bytes", (int64_t)(tts.ok ? *out_len : 0));
  dsh_json_kv_i64(j, "textWords", tts.text_words);
  dsh_json_kv_i64(j, "elapsedMs", elapsed_ms < 0 ? 0 : elapsed_ms);
  dsh_json_kv_str(j, "message", message == NULL ? "" : message);
  dsh_json_kv_str(j, "speaker", speaker == NULL ? "" : speaker);
  dsh_json_kv_str(j, "language", language == NULL ? "" : language);
  dsh_json_kv_str(j, "why", why == NULL ? "" : why);
  dsh_json_object_end(j);

  out = dsh_json_take(j);
  dsh_json_free(j);
  dsh_doubao_result_free(&tts);
  if (message != NULL) dsh_release(message);
  if (plan_doc != NULL) dsh_json_doc_free(plan_doc);
  if (speaker != NULL) dsh_release(speaker);
  if (language != NULL) dsh_release(language);
  if (why != NULL) dsh_release(why);

  if (out == NULL) {
    if (*out_bytes != NULL) {
      dsh_release(*out_bytes);
      *out_bytes = NULL;
      *out_len = 0;
    }
    dsh_set_last_error("dsh_speech_online_accept：内存不足");
    return DSH_E_OOM;
  }
  *out_json = out;
  return DSH_OK;
}
