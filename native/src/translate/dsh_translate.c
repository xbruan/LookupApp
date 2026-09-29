/* ==========================================================================
 * 机器翻译 · 纯逻辑层 —— 请求构造 / 语种映射 / 回包解析 / 缓存键全在内核，没有任何 I/O。
 *
 * 为什么这样切：内核零依赖，但「该发什么、回包是什么意思」必须在内核里，所以边界只切在
 * I/O 上：`plan` 给出发送所需的一切，外壳原样 POST 并把状态与回包原样拿回来，`accept`
 * 说回包是什么意思。
 *
 * ⚠️ 外壳**不许在中间做任何判断**：不许改 target、不许把不支持的语种退化成英语、
 *    不许把非 200 当失败、不许自己拼错误提示。
 * ⚠️ 语种短码必须与在线服务认的一致；不支持的语种回 NULL，绝不退化成英语。
 * ⚠️ 错误码表**与语音合成不是同一张表**，别复用；HTTP 状态与业务 code 要分开判。
 * 约定出处（动这些表之前先对齐）：`docs/design/不带mdd的词典与机器翻译开发指导.md`。
 * ========================================================================== */

#include "translate/dsh_translate.h"

#include "crypto/dsh_sha256.h"
#include "dsh_internal.h"
#include "json_reader.h"
#include "json_writer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* MT 认的 32 种短码：逐字照服务端的写法，顺序也照抄（便于对账）*/
static const char *const k_mt_languages[] = {
    "zh",   "zh-Hant", "en", "ja", "ko", "fr", "de", "es", "pt", "ru", "ar",
    "it",   "nl",      "pl", "ro", "sv", "da", "nb", "fi", "hu", "cs", "hr",
    "el",   "he",      "tr", "uk", "th", "vi", "id", "ms", "tl", "hi",
};
static const size_t k_mt_language_count = sizeof(k_mt_languages) / sizeof(k_mt_languages[0]);

/** 大小写不敏感比较（标签一律 ASCII）*/
static int ci_eq(const char *a, const char *b) {
  if (a == NULL || b == NULL) return 0;
  while (*a && *b) {
    char ca = *a, cb = *b;
    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
    if (ca != cb) return 0;
    a++;
    b++;
  }
  return *a == 0 && *b == 0;
}

/** 大小写不敏感比较前 n 个字符 */
static int ci_eq_n(const char *a, const char *b, size_t n) {
  size_t i;
  if (a == NULL || b == NULL) return 0;
  for (i = 0; i < n; i++) {
    char ca = a[i], cb = b[i];
    if (ca == 0 || cb == 0) return 0;
    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
    if (ca != cb) return 0;
  }
  return 1;
}

/** 把主语言子标签拷进 buf（`zh-CN` → `zh`、`pt-BR` → `pt`）；返回长度 */
static size_t primary_subtag(const char *tag, char *buf, size_t cap) {
  size_t n = 0;
  if (tag == NULL || cap == 0) return 0;
  while (tag[n] && tag[n] != '-' && tag[n] != '_' && n + 1 < cap) {
    char c = tag[n];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    buf[n] = c;
    n++;
  }
  buf[n] = 0;
  return n;
}

const char *dsh_translate_language(const char *tag) {
  char primary[16];
  size_t n;

  if (tag == NULL || tag[0] == 0) return NULL;
  n = primary_subtag(tag, primary, sizeof(primary));
  if (n == 0) return NULL;

  /* 三处本机写法与 MT 对不上的（少一条就会不报错地出错）：繁体要 `zh-Hant`、
     本机 `no` / `nn` 都归 `nb`、菲律宾语要用 `tl`（`fil` 是三字母写法）。 */
  if (ci_eq(primary, "zh")) {
    if (ci_eq_n(tag, "zh-hant", 7) || ci_eq_n(tag, "zh-tw", 5) || ci_eq_n(tag, "zh-hk", 5) ||
        ci_eq_n(tag, "zh-mo", 5)) {
      return "zh-Hant";
    }
    return "zh";
  }
  if (ci_eq(primary, "no") || ci_eq(primary, "nb") || ci_eq(primary, "nn")) return "nb";
  if (ci_eq(primary, "fil") || ci_eq(primary, "tl")) return "tl";

  {
    size_t i;
    for (i = 0; i < k_mt_language_count; i++) {
      if (ci_eq(primary, k_mt_languages[i])) return k_mt_languages[i];
    }
  }

  /* 本机认识而 MT 不支持的（`faurtaskslbgltlvcaswla` …）→ 回 NULL。
     ⚠️ **绝不许在这里退化成英语**：用户会以为译错了，其实是方向被偷偷改了 ——
     调用方拿到 NULL 就该如实说「机器翻译暂不支持〈波斯语〉」，而不是照译。 */
  return NULL;
}

const char *dsh_translate_target(const char *language, const char *target_mode) {
  /* 用户固定了方向就听用户的（auto / zh / en 三档）*/
  if (target_mode != NULL && target_mode[0] != 0 && !ci_eq(target_mode, "auto")) {
    if (ci_eq(target_mode, "zh")) return "zh";
    if (ci_eq(target_mode, "en")) return "en";
    return NULL; /* 认不出来的模式**不猜** —— 让调用方如实报错 */
  }
  /* 汉字为主 → 英文，其余 → 中文 */
  if (language != NULL && ci_eq(language, "zh")) return "en";
  return "zh";
}

char *dsh_translate_request_body(const char *source, const char *target, const char *const *texts,
                                 size_t count) {
  dsh_json *w;
  size_t i;

  if (target == NULL || target[0] == 0) {
    dsh_set_last_error("机器翻译：没有目标语种");
    return NULL;
  }
  if (texts == NULL || count == 0) {
    dsh_set_last_error("机器翻译：没有要翻的文本");
    return NULL;
  }
  if (count > DSH_MT_MAX_ITEMS) {
    /* `text_list` 最多 16 条：这里**如实拒**，不悄悄截断 —— 截断会让用户以为整段都翻了。
       切分是调用方的事。 */
    dsh_set_last_error("机器翻译：一次最多 %d 条（调用方要先切分）", DSH_MT_MAX_ITEMS);
    return NULL;
  }

  w = dsh_json_new();
  if (w == NULL) return NULL;

  dsh_json_object_begin(w);
  /* 判不出源语种时**不编一个**：空串就不写这个键，让服务端自己检测 */
  if (source != NULL && source[0] != 0) dsh_json_kv_str(w, "source_language", source);
  dsh_json_kv_str(w, "target_language", target);
  dsh_json_key(w, "text_list");
  dsh_json_array_begin(w);
  for (i = 0; i < count; i++) dsh_json_str(w, texts[i] == NULL ? "" : texts[i]);
  dsh_json_array_end(w);
  dsh_json_object_end(w);

  {
    char *out = dsh_json_take(w);
    dsh_json_free(w);
    return out;
  }
}

char *dsh_translate_cache_key(const char *source, const char *target, const char *text) {
  /* 键 = 引擎版本 | source | target | 文本，再哈希。版本号进键是为了「换了请求约定之后
     旧缓存自动失效」；按 token 计费，所以缓存是必需项。 */
  size_t head_len;
  size_t text_len = (text == NULL) ? 0 : strlen(text);
  char *payload;
  char *hex;

  head_len = (size_t)snprintf(NULL, 0, "%s|%s|%s|", DSH_MT_ENGINE_VERSION,
                              source == NULL ? "" : source, target == NULL ? "" : target);
  payload = (char *)dsh_mem_alloc(head_len + text_len + 1);
  if (payload == NULL) return NULL;
  snprintf(payload, head_len + 1, "%s|%s|%s|", DSH_MT_ENGINE_VERSION, source == NULL ? "" : source,
           target == NULL ? "" : target);
  if (text_len > 0) memcpy(payload + head_len, text, text_len);
  payload[head_len + text_len] = 0;

  hex = (char *)dsh_mem_alloc(65);
  if (hex != NULL) dsh_sha256_hex(payload, head_len + text_len, hex);
  dsh_release(payload);
  return hex;
}

/* 错误码 → 人话（**与语音合成不是同一张表**，别复用）*/

static const char *mt_error_sentence(int64_t code) {
  switch (code) {
    case 20000000:
      return "";
    case 45000001:
      /* 「目标语种没指定」这类 —— 通常是**代码 bug**，不是用户的问题 */
      return "翻译请求的参数不对（多半是目标语种没给）—— 这是程序的问题，不是你的操作问题。";
    case 45000130:
      return "这段文本太长，翻译服务退回来了。";
    case 55000001:
      return "翻译服务出错，稍后再试一次。";
    case 45000002:
      return "这段文本里有翻译服务不接受的内容。";
    default:
      return NULL; /* 由调用方拼上「服务端原话 + code」*/
  }
}

/** 回包里 `message` 字段（服务端原话，排错时要带上）*/
static char *body_message(const dsh_json_node *root) {
  const dsh_json_node *m = dsh_json_object_get(root, "message");
  size_t len = 0;
  const char *s;
  char *copy;
  if (m == NULL || !dsh_json_is_string(m)) return NULL;
  s = dsh_json_str_value(m, &len);
  if (s == NULL) return NULL;
  copy = (char *)dsh_mem_alloc(len + 1);
  if (copy == NULL) return NULL;
  memcpy(copy, s, len);
  copy[len] = 0;
  return copy;
}

int dsh_translate_parse(int http_status, const char *body, dsh_mt_result *out) {
  dsh_json_doc *doc = NULL;
  const dsh_json_node *root;
  int64_t code = 0;
  char *message = NULL;

  if (out == NULL) return -1;
  memset(out, 0, sizeof(*out));
  out->http_status = http_status;

  if (http_status != 200) {
    /* 这一版 MT 是「HTTP 200 + 业务 code」，非 200 就是没答上来 —— 按服务失败说，
       并把状态码原样带上；403 / 401 另给话（见下）。 */
    if (http_status == 403) {
      out->reason = dsh_mem_strdup(
          "翻译：这个 Key 没有调用机器翻译的权限 —— 去控制台确认「机器翻译大模型」"
          "（volc.speech.mt）已开通。它与语音合成是**两个独立权限**。");
    } else if (http_status == 401) {
      out->reason = dsh_mem_strdup("翻译：API Key 不对或已失效。");
    } else {
      char buf[160];
      snprintf(buf, sizeof(buf), "翻译服务没答上来（HTTP %d）。", http_status);
      out->reason = dsh_mem_strdup(buf);
    }
    return 0;
  }

  if (body == NULL || dsh_json_parse(body, strlen(body), &doc) != 0) {
    out->reason = dsh_mem_strdup("翻译：回包不是合法的 JSON。");
    return 0;
  }
  root = dsh_json_doc_root(doc);
  message = body_message(root);

  if (root == NULL || !dsh_json_is_object(root)) {
    out->reason = dsh_mem_strdup("翻译：回包不是一个对象。");
    goto done;
  }

  if (dsh_json_i64_value(dsh_json_object_get(root, "code"), &code) != 0) {
    out->reason = dsh_mem_strdup("翻译：回包里没有 code。");
    goto done;
  }
  out->code = code;

  if (code != 20000000) {
    const char *known = mt_error_sentence(code);
    if (known != NULL) {
      out->reason = dsh_mem_strdup(known);
    } else {
      char buf[256];
      snprintf(buf, sizeof(buf), "翻译服务出错（code %lld）：%s", (long long)code,
               message == NULL ? "服务端没有给出说明" : message);
      out->reason = dsh_mem_strdup(buf);
    }
    goto done;
  }

  {
    const dsh_json_node *data = dsh_json_object_get(root, "data");
    const dsh_json_node *list = data == NULL ? NULL : dsh_json_object_get(data, "translation_list");
    const dsh_json_node *first = list == NULL ? NULL : dsh_json_array_at(list, 0);
    const dsh_json_node *usage;
    const dsh_json_node *translation;
    size_t len = 0;
    const char *s;

    if (first == NULL || !dsh_json_is_object(first)) {
      out->reason = dsh_mem_strdup("翻译：回包里没有译文。");
      goto done;
    }
    translation = dsh_json_object_get(first, "translation");
    if (translation == NULL || !dsh_json_is_string(translation)) {
      out->reason = dsh_mem_strdup("翻译：回包里没有译文。");
      goto done;
    }
    s = dsh_json_str_value(translation, &len);
    out->translation = (char *)dsh_mem_alloc(len + 1);
    if (out->translation == NULL) goto done;
    memcpy(out->translation, s, len);
    out->translation[len] = 0;

    /* `detected_source_language` 只在请求没指定 source 时才有；留着它做一次交叉验证 ——
       与我们的判定不同就如实标明。 */
    {
      const dsh_json_node *detected = dsh_json_object_get(first, "detected_source_language");
      size_t dlen = 0;
      const char *ds;
      if (detected != NULL && dsh_json_is_string(detected)) {
        ds = dsh_json_str_value(detected, &dlen);
        out->detected_source = (char *)dsh_mem_alloc(dlen + 1);
        if (out->detected_source != NULL) {
          memcpy(out->detected_source, ds, dlen);
          out->detected_source[dlen] = 0;
        }
      }
    }

    usage = dsh_json_object_get(first, "usage");
    if (usage != NULL) {
      int64_t v = 0;
      if (dsh_json_i64_value(dsh_json_object_get(usage, "prompt_tokens"), &v) == 0) out->prompt_tokens = v;
      if (dsh_json_i64_value(dsh_json_object_get(usage, "completion_tokens"), &v) == 0) {
        out->completion_tokens = v;
      }
      if (dsh_json_i64_value(dsh_json_object_get(usage, "total_tokens"), &v) == 0) out->total_tokens = v;
    }
    out->ok = 1;
  }

done:
  if (message != NULL) dsh_release(message);
  dsh_json_doc_free(doc);
  return 0;
}

void dsh_translate_result_free(dsh_mt_result *result) {
  if (result == NULL) return;
  if (result->translation != NULL) dsh_release(result->translation);
  if (result->detected_source != NULL) dsh_release(result->detected_source);
  if (result->reason != NULL) dsh_release(result->reason);
  memset(result, 0, sizeof(*result));
}
