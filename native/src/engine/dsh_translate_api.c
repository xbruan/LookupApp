/* 机器翻译接口的内核侧收尾：dsh_translate_status / plan / accept / clear_cache ——
 * 内核出「该发什么 / 回包是什么意思」，平台层只负责把这段字节发出去。
 * ⚠️ 译文缓存只在进程内、按内容寻址、无淘汰；一次只翻一条文本，超长切分没做（服务端回 45000130）。*/

#include "crypto/dsh_sha256.h"
#include "dict/dsh_entry_doc.h"
#include "dsh_internal.h"
#include "engine/dsh_engine_internal.h"
#include "engine/dsh_settings.h"
#include "json_reader.h"
#include "json_writer.h"
#include "platform/dsh_time.h"
#include "text/dsh_html.h"
#include "text/dsh_language.h"
#include "translate/dsh_translate.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ── 译文缓存（进程内，内容寻址）────────────────────────────────────────── */

#define MT_CACHE_MAX 64

typedef struct {
  char key[65];
  char *translation;
  char *source;     /* MT 码 */
  char *target;     /* MT 码 */
  char *detected;   /* 服务端识别到的（可空）*/
  int64_t tokens;
} mt_cache_entry;

static mt_cache_entry g_cache[MT_CACHE_MAX];
static int g_cache_count;
static int g_cache_next; /* 满了就从头覆盖（FIFO；条目都很小，不值得做 LRU）*/
static int64_t g_cache_hits;
static int64_t g_cache_misses;

static const mt_cache_entry *cache_find(const char *key) {
  int i;
  if (key == NULL || key[0] == 0) return NULL;
  for (i = 0; i < g_cache_count; i++) {
    if (strcmp(g_cache[i].key, key) == 0) return &g_cache[i];
  }
  return NULL;
}

static void cache_put(const char *key, const char *translation, const char *source,
                      const char *target, const char *detected, int64_t tokens) {
  mt_cache_entry *slot;
  if (key == NULL || key[0] == 0 || translation == NULL) return;

  slot = (mt_cache_entry *)cache_find(key); /* 同一句再翻一次：覆盖（不动计数器）*/
  if (slot == NULL) {
    if (g_cache_count < MT_CACHE_MAX) slot = &g_cache[g_cache_count++];
    else {
      slot = &g_cache[g_cache_next];
      g_cache_next = (g_cache_next + 1) % MT_CACHE_MAX;
      if (slot->translation != NULL) dsh_release(slot->translation);
      if (slot->source != NULL) dsh_release(slot->source);
      if (slot->target != NULL) dsh_release(slot->target);
      if (slot->detected != NULL) dsh_release(slot->detected);
      memset(slot, 0, sizeof(*slot));
    }
    snprintf(slot->key, sizeof(slot->key), "%s", key);
  } else {
    if (slot->translation != NULL) dsh_release(slot->translation);
    if (slot->source != NULL) dsh_release(slot->source);
    if (slot->target != NULL) dsh_release(slot->target);
    if (slot->detected != NULL) dsh_release(slot->detected);
  }

  slot->translation = dsh_mem_strdup(translation);
  slot->source = dsh_mem_strdup(source == NULL ? "" : source);
  slot->target = dsh_mem_strdup(target == NULL ? "" : target);
  slot->detected = (detected == NULL) ? NULL : dsh_mem_strdup(detected);
  slot->tokens = tokens;
}

static int cache_clear_all(void) {
  int i;
  int n = 0;
  for (i = 0; i < g_cache_count; i++) {
    if (g_cache[i].translation != NULL) dsh_release(g_cache[i].translation);
    if (g_cache[i].source != NULL) dsh_release(g_cache[i].source);
    if (g_cache[i].target != NULL) dsh_release(g_cache[i].target);
    if (g_cache[i].detected != NULL) dsh_release(g_cache[i].detected);
    memset(&g_cache[i], 0, sizeof(g_cache[i]));
    n++;
  }
  g_cache_count = 0;
  g_cache_next = 0;
  return n;
}

/* ── 设置那一小撮（翻译开关 / 方向 / 自动翻译 / 账号级凭据）──────────────── */

typedef struct {
  int enabled;
  int auto_translate;
  char target_mode[16];
} mt_settings;

/** 读 `translate` 那一节：归一化只在设置模型那处做（`merge_translate`），这里只读不算。 */
static void read_translate_settings(const struct dsh_settings *s, mt_settings *out) {
  const dsh_translate_settings *tr = dsh_settings_translate(s);
  out->enabled = (tr != NULL) ? tr->enabled : 0;
  out->auto_translate = (tr != NULL) ? tr->auto_translate : 1;
  snprintf(out->target_mode, sizeof(out->target_mode), "%s",
           (tr != NULL && tr->target_mode != NULL) ? tr->target_mode : "auto");
}

/** 账号级凭据（语音与翻译共用同一把 API Key）—— 没有就回空串 */
static const char *api_key_of(const struct dsh_settings *s) {
  const char *key = dsh_settings_volcengine_api_key(s);
  return (key == NULL) ? "" : key;
}

/* ── 译文伪词条：译文要能像真词条一样朗读、复制、进返回栈、能回退 ──────────
 * 复用 `dsh_entry_doc_build` 那层壳；`dictId` 取虚拟 id（真词典是内容哈希），令牌是正文哈希。
 * ⚠️ 正文是机器生成的文本，可能带 `<`、`&`：必须转义后再拼进 HTML。*/

#define DSH_MT_ENTRY_DICT_ID "translate"
#define DSH_MT_ENTRY_TITLE "机器翻译"

/** 把一段文本转义成 HTML（借用内核唯一那处实现，别在这儿再写一份）*/
static char *mt_escape(const char *text) {
  size_t len = 0;
  char *out = dsh_html_escape(text == NULL ? "" : text, strlen(text == NULL ? "" : text), &len);
  (void)len;
  return out;
}

/** 拼 `a` + `b`（两块都是内核分配的串，函数**吃掉**它们；失败时两块都还掉）*/
static char *join_and_free(char *a, const char *b) {
  const size_t na = (a != NULL) ? strlen(a) : 0;
  const size_t nb = (b != NULL) ? strlen(b) : 0;
  char *out = (char *)dsh_mem_alloc(na + nb + 1);
  if (out != NULL) {
    if (na > 0) memcpy(out, a, na);
    if (nb > 0) memcpy(out + na, b, nb);
    out[na + nb] = '\0';
  }
  if (a != NULL) dsh_release(a);
  return out;
}

/** 拼译文正文：译文 + 方向 + 用量 + 原文；用量**只说真花掉的那些**，命中缓存是 0。
 * ⚠️ 比较服务端识别语种必须拿**代码**比（`en` vs `en`）：拿中文名比永远不等，
 * 于是每次翻译都会多印一句交叉验证的噪音。*/
static char *mt_entry_definition(const char *source_text, const char *translation,
                                 const char *source_code, const char *source_label,
                                 const char *target_label, const char *detected, int64_t tokens,
                                 int from_cache) {
  char *esc_label = NULL;
  char *esc_target = NULL;
  char *esc_text = NULL;
  char *esc_source = NULL;
  char head[128];
  char usage[64];
  char *body = NULL;
  char *out = NULL;

  esc_label = mt_escape(source_label);
  esc_target = mt_escape(target_label);
  esc_text = mt_escape(translation);
  esc_source = mt_escape(source_text);
  if (esc_label == NULL || esc_target == NULL || esc_text == NULL || esc_source == NULL) goto done;

  if (detected != NULL && detected[0] != '\0' && source_code != NULL &&
      strcmp(detected, source_code) != 0) {
    snprintf(head, sizeof(head), "%s → %s（服务端识别为 %s）", esc_label, esc_target, detected);
  } else {
    snprintf(head, sizeof(head), "%s → %s", esc_label, esc_target);
  }
  /* 用量那一行：**只说真花掉的那些**（命中缓存 = 不产生费用，不能虚报 token）*/
  if (from_cache) {
    snprintf(usage, sizeof(usage), "命中缓存，本次不产生费用");
  } else {
    snprintf(usage, sizeof(usage), "本次 %lld tokens", (long long)tokens);
  }

  body = dsh_mem_strdup(
      "<style>"
      ".mt-head{display:flex;flex-wrap:wrap;gap:6px 10px;align-items:baseline;"
      "font-size:12.5px;color:#6b7280;margin-bottom:10px}"
      ".mt-head b{font-weight:600;color:#374151}"
      ".mt-text{white-space:pre-wrap;font-size:15px;line-height:1.75}"
      ".mt-src{margin-top:14px;padding-top:10px;border-top:1px solid rgba(20,24,40,.08);"
      "font-size:13px;color:#6b7280;white-space:pre-wrap}"
      "</style>");
  if (body == NULL) goto done;
  body = join_and_free(body, "<div class=\"mt-head\"><b>");
  body = join_and_free(body, head);
  body = join_and_free(body, "</b><span>");
  body = join_and_free(body, usage);
  body = join_and_free(body, "</span></div><div class=\"mt-text\">");
  body = join_and_free(body, esc_text);
  body = join_and_free(body, "</div>");
  if (source_text != NULL && source_text[0] != '\0') {
    body = join_and_free(body, "<div class=\"mt-src\">原文：");
    body = join_and_free(body, esc_source);
    body = join_and_free(body, "</div>");
  }
  out = body;
  body = NULL;

done:
  if (esc_label != NULL) dsh_release(esc_label);
  if (esc_target != NULL) dsh_release(esc_target);
  if (esc_text != NULL) dsh_release(esc_text);
  if (esc_source != NULL) dsh_release(esc_source);
  if (body != NULL) dsh_release(body);
  return out;
}

/** 把译文 + 方向 + 用量整成一份完整的词条正文，并给一个内容寻址的令牌。
 * @param out_json 出参 `{dictId, dictTitle, fromCache, entryToken, entryHtml}`，调用方负责释放。
 */
static enum dsh_error build_entry(const char *source_text, const char *translation,
                                  const char *source_code, const char *source_label,
                                  const char *target_label, const char *detected, int64_t tokens,
                                  int from_cache, char **out_json) {
  char *definition = NULL;
  char *html = NULL;
  size_t html_len = 0;
  char token[65];
  dsh_json *j = NULL;
  char *out = NULL;

  definition = mt_entry_definition(source_text, translation, source_code, source_label, target_label,
                                   detected, tokens, from_cache);
  if (definition == NULL) {
    dsh_set_last_error("机器翻译：内存不足（拼正文）");
    return DSH_E_OOM;
  }
  /* 伪词条没有资源卷可挂，也没有「未找到」的提示 —— 那两种提示都是给真词条用的 */
  html = dsh_entry_doc_build(DSH_MT_ENTRY_DICT_ID, definition, strlen(definition), NULL, 0,
                            &html_len);
  dsh_release(definition);
  if (html == NULL) return DSH_E_OOM;
  dsh_sha256_hex(html, html_len, token);

  j = dsh_json_new();
  if (j == NULL) {
    dsh_release(html);
    return DSH_E_OOM;
  }
  dsh_json_object_begin(j);
  dsh_json_kv_str(j, "dictId", DSH_MT_ENTRY_DICT_ID);
  dsh_json_kv_str(j, "dictTitle", DSH_MT_ENTRY_TITLE);
  dsh_json_kv_bool(j, "fromCache", from_cache);
  dsh_json_kv_str(j, "entryToken", token);
  dsh_json_kv_str(j, "entryHtml", html);
  dsh_json_object_end(j);
  out = dsh_json_take(j);
  dsh_json_free(j);
  dsh_release(html);
  if (out == NULL) return DSH_E_OOM;
  *out_json = out;
  return DSH_OK;
}

/** 从一段 JSON 里读字符串字段（借用出参指针需要释放时由调用方 dsh_release）*/
static char *str_field_of(const char *json, const char *key) {
  dsh_json_doc *doc = NULL;
  char *out = NULL;
  if (json == NULL || key == NULL) return NULL;
  if (dsh_json_parse(json, strlen(json), &doc) != 0) return NULL;
  {
    const dsh_json_node *root = dsh_json_doc_root(doc);
    const dsh_json_node *node = (root != NULL) ? dsh_json_object_get(root, key) : NULL;
    const char *s = (node == NULL) ? NULL : dsh_json_str_value(node, NULL);
    if (s != NULL) out = dsh_mem_strdup(s);
  }
  dsh_json_doc_free(doc);
  return out;
}

/** 从一段 JSON 里读布尔字段（没有 = fallback）*/
static int bool_field_of(const char *json, const char *key, int fallback) {
  dsh_json_doc *doc = NULL;
  int out = fallback;
  if (json == NULL || key == NULL) return fallback;
  if (dsh_json_parse(json, strlen(json), &doc) != 0) return fallback;
  {
    const dsh_json_node *root = dsh_json_doc_root(doc);
    const dsh_json_node *node = (root != NULL) ? dsh_json_object_get(root, key) : NULL;
    if (node != NULL && dsh_json_is_bool(node)) out = dsh_json_bool_value(node) ? 1 : 0;
  }
  dsh_json_doc_free(doc);
  return out;
}

/** 从一段 JSON 里读整数字段（没有 = fallback）*/
static int64_t num_field_of(const char *json, const char *key, int64_t fallback) {
  dsh_json_doc *doc = NULL;
  int64_t out = fallback;
  if (json == NULL || key == NULL) return fallback;
  if (dsh_json_parse(json, strlen(json), &doc) != 0) return fallback;
  {
    const dsh_json_node *root = dsh_json_doc_root(doc);
    const dsh_json_node *node = (root != NULL) ? dsh_json_object_get(root, key) : NULL;
    if (node != NULL && dsh_json_is_number(node)) {
      int64_t v = fallback;
      if (dsh_json_i64_value(node, &v) == 0) out = v;
    }
  }
  dsh_json_doc_free(doc);
  return out;
}

/* ── dsh_translate_status ──────────────────────────────────────────────── */

enum dsh_error dsh_translate_status(dsh_engine *engine, char **out_json) {
  const struct dsh_settings *s;
  mt_settings cfg;
  dsh_json *j;
  char *out;

  dsh_clear_last_error();
  if (out_json == NULL) {
    dsh_set_last_error("dsh_translate_status：out_json 不能为空");
    return DSH_E_INVALID_ARG;
  }
  if (engine == NULL) {
    dsh_set_last_error("dsh_translate_status：引擎无效");
    return DSH_E_INVALID_ARG;
  }

  s = dsh_engine_settings(engine);
  read_translate_settings(s, &cfg);

  j = dsh_json_new();
  if (j == NULL) {
    dsh_set_last_error("dsh_translate_status：内存不足");
    return DSH_E_OOM;
  }
  dsh_json_object_begin(j);
  dsh_json_kv_bool(j, "hasApiKey", api_key_of(s)[0] != '\0');
  dsh_json_kv_bool(j, "enabled", cfg.enabled);
  dsh_json_kv_str(j, "targetMode", cfg.target_mode);
  dsh_json_kv_bool(j, "autoTranslate", cfg.auto_translate);
  /* 两个常量一起给：排错时一眼看出请求被指到哪儿去了 */
  dsh_json_kv_str(j, "endpoint", DSH_MT_ENDPOINT);
  dsh_json_kv_str(j, "resourceId", DSH_MT_RESOURCE_ID);
  dsh_json_key(j, "cache");
  dsh_json_object_begin(j);
  dsh_json_kv_i64(j, "count", g_cache_count);
  dsh_json_kv_i64(j, "hits", g_cache_hits);
  dsh_json_kv_i64(j, "misses", g_cache_misses);
  dsh_json_object_end(j);
  dsh_json_object_end(j);

  out = dsh_json_take(j);
  dsh_json_free(j);
  if (out == NULL) {
    dsh_set_last_error("dsh_translate_status：内存不足");
    return DSH_E_OOM;
  }
  *out_json = out;
  return DSH_OK;
}

/* ── dsh_translate_plan —— 「该发什么」────────────────────────────────── */

/** 请求 id：服务端排错要用，形状按 UUID 那一套；**只用时间戳 + 键**生成（够用且不引随机源）*/
static void make_request_id(const char *seed, char out[37]) {
  char payload[256];
  char hex[65];
  snprintf(payload, sizeof(payload), "%lld|%s", (long long)dsh_now_ms(), seed == NULL ? "" : seed);
  dsh_sha256_hex(payload, strlen(payload), hex);
  snprintf(out, 37, "%.8s-%.4s-%.4s-%.4s-%.12s", hex, hex + 8, hex + 12, hex + 16, hex + 20);
}

/** 失败那一档的统一出口（人话 + 一个 `why` 机读码）*/
static enum dsh_error plan_fail(char **out_json, const char *text, const char *source_label,
                                const char *target_label, const char *why, const char *reason) {
  dsh_json *j = dsh_json_new();
  char *out;
  if (j == NULL) {
    dsh_set_last_error("dsh_translate_plan：内存不足");
    return DSH_E_OOM;
  }
  dsh_json_object_begin(j);
  dsh_json_kv_bool(j, "ok", 0);
  dsh_json_kv_bool(j, "needsHttp", 0);
  dsh_json_kv_bool(j, "cached", 0);
  dsh_json_kv_str(j, "why", why == NULL ? "" : why);
  dsh_json_kv_str(j, "message", reason == NULL ? "" : reason);
  dsh_json_kv_str(j, "text", text == NULL ? "" : text);
  dsh_json_kv_str(j, "translation", "");
  dsh_json_kv_str(j, "sourceLanguage", "");
  dsh_json_kv_str(j, "targetLanguage", "");
  dsh_json_kv_str(j, "sourceLabel", source_label == NULL ? "" : source_label);
  dsh_json_kv_str(j, "targetLabel", target_label == NULL ? "" : target_label);
  dsh_json_kv_str(j, "detected", "");
  dsh_json_kv_i64(j, "tokens", 0);
  dsh_json_kv_i64(j, "cachedItems", 0);
  dsh_json_kv_i64(j, "fetchedItems", 0);
  dsh_json_kv_i64(j, "elapsedMs", 0);
  dsh_json_object_end(j);
  out = dsh_json_take(j);
  dsh_json_free(j);
  if (out == NULL) return DSH_E_OOM;
  *out_json = out;
  return DSH_OK;
}

enum dsh_error dsh_translate_plan(dsh_engine *engine, const char *text, const char *dict_title,
                                  char **out_json) {
  const struct dsh_settings *s;
  mt_settings cfg;
  const char *key;
  const char *language = NULL;
  const char *basis = NULL;
  const char *basis_text = NULL;
  const char *mt_source;
  const char *target;
  char *cache_key;
  const mt_cache_entry *hit;
  dsh_json *j;
  char *body;
  char *out;
  const char *texts[1];
  char request_id[37];

  dsh_clear_last_error();
  if (out_json == NULL) {
    dsh_set_last_error("dsh_translate_plan：out_json 不能为空");
    return DSH_E_INVALID_ARG;
  }
  if (engine == NULL) {
    dsh_set_last_error("dsh_translate_plan：引擎无效");
    return DSH_E_INVALID_ARG;
  }
  if (text == NULL || text[0] == 0) {
    /* 四种原因**分开说**：合并成一句会让用户去改错的地方 */
    return plan_fail(out_json, text, NULL, NULL, "empty", "没有要翻的文字。");
  }

  s = dsh_engine_settings(engine);
  read_translate_settings(s, &cfg);
  key = api_key_of(s);
  /* 总开关必须在**最低这一层**也挡：这个开关的语义是隐私（把文本发给第三方得用户主动打开），
   * 关着就不许有任何一条路把文本发出去 —— 平台上再挡一道就是第二处实现。
   * ⚠️ 措辞与查词通道那句（`dsh_fallback_translate_why`）**逐字相同**，改一处要两处一起改。*/
  if (!cfg.enabled) {
    return plan_fail(out_json, text, NULL, NULL, "disabled",
                     "机器翻译的总开关关着（选项 → 翻译）");
  }
  if (key[0] == 0) {
    return plan_fail(out_json, text, NULL, NULL, "no-key",
                     "翻译需要火山引擎凭据，它与语音共用同一把 API Key —— "
                     "先去「语音」那一页填上。");
  }
  if (cfg.target_mode[0] == 0) {
    return plan_fail(out_json, text, NULL, NULL, "bad-mode", "设置里的翻译方向认不出来。");
  }

  /* 源语种只走内核那套**统一判定**（字形 + 词典标题）：别在这儿另写一份「看有没有汉字」，
   * 那就是同一条规则的第二处实现。 */
  dsh_language_decide(text, dict_title, NULL, NULL, &language, &basis, &basis_text);

  mt_source = dsh_translate_language(language);
  if (mt_source == NULL) {
    /* ★ 该语种 MT 不支持：**如实说不支持，绝不退化成英语** */
    char reason[256];
    const char *label = dsh_language_name(language);
    if (label[0] == '\0') label = language;
    snprintf(reason, sizeof(reason), "机器翻译暂不支持%s（不会拿别的语种顶替）。", label);
    return plan_fail(out_json, text, dsh_language_name(language)[0] ? dsh_language_name(language) : language, NULL, "unsupported", reason);
  }

  target = dsh_translate_target(language, cfg.target_mode);
  if (target == NULL) {
    return plan_fail(out_json, text, NULL, NULL, "bad-mode", "设置里的翻译方向认不出来。");
  }

  cache_key = dsh_translate_cache_key(mt_source, target, text);
  if (cache_key == NULL) {
    dsh_set_last_error("dsh_translate_plan：内存不足");
    return DSH_E_OOM;
  }

  hit = cache_find(cache_key);
  if (hit != NULL) {
    g_cache_hits++;
    j = dsh_json_new();
    if (j == NULL) {
      dsh_release(cache_key);
      return DSH_E_OOM;
    }
    dsh_json_object_begin(j);
    dsh_json_kv_bool(j, "ok", 1);
    dsh_json_kv_bool(j, "needsHttp", 0);
    dsh_json_kv_bool(j, "cached", 1); /* ★ 命中缓存就不必发请求了：不重复计费 */
    dsh_json_kv_str(j, "cacheKey", cache_key);
    dsh_json_kv_str(j, "text", text);
    dsh_json_kv_str(j, "sourceLanguage", mt_source);
    dsh_json_kv_str(j, "targetLanguage", target);
    dsh_json_kv_str(j, "sourceLabel",
                    dsh_language_name(language)[0] ? dsh_language_name(language) : language);
    dsh_json_kv_str(j, "targetLabel", dsh_language_name(target == NULL ? "" : target));
    dsh_json_kv_str(j, "translation", hit->translation);
    dsh_json_kv_str(j, "detected", hit->detected == NULL ? "" : hit->detected);
    /* ⚠️ 命中缓存也要把结果那几个数给全（界面按 `TranslateResult` 的形状读）：少了它们界面拿到的
     * 是 `undefined`；命中则 cachedItems=1 / fetchedItems=0 / tokens=0 —— 这一次真没花钱。 */
    dsh_json_kv_str(j, "message", "");
    dsh_json_kv_i64(j, "tokens", 0);
    dsh_json_kv_i64(j, "cachedItems", 1);
    dsh_json_kv_i64(j, "fetchedItems", 0);
    dsh_json_kv_i64(j, "elapsedMs", 0);
    /* 伪词条：命中缓存也算翻出来了，正文文档同样要给（否则「翻译这个词」点下去是空页）。*/
    {
      char *entry = NULL;
      const char *src_label = dsh_language_name(language);
      if (src_label[0] == '\0') src_label = language;
      if (build_entry(text, hit->translation, mt_source, src_label,
                      dsh_language_name(target == NULL ? "" : target), hit->detected, 0, 1,
                      &entry) == DSH_OK &&
          entry != NULL) {
        dsh_json_key(j, "entry");
        dsh_json_value_raw(j, entry, strlen(entry));
        dsh_release(entry);
      }
    }
    dsh_json_object_end(j);
    out = dsh_json_take(j);
    dsh_json_free(j);
    dsh_release(cache_key);
    if (out == NULL) return DSH_E_OOM;
    *out_json = out;
    return DSH_OK;
  }

  g_cache_misses++;
  texts[0] = text;
  body = dsh_translate_request_body(mt_source, target, texts, 1);
  if (body == NULL) {
    dsh_release(cache_key);
    /* dsh_translate_request_body 已经把人话写进 last_error 了 */
    return plan_fail(out_json, text, NULL, NULL, "bad-request", dsh_last_error_message());
  }
  make_request_id(cache_key, request_id);

  j = dsh_json_new();
  if (j == NULL) {
    dsh_release(body);
    dsh_release(cache_key);
    return DSH_E_OOM;
  }
  dsh_json_object_begin(j);
  dsh_json_kv_bool(j, "ok", 1);
  dsh_json_kv_bool(j, "cached", 0);
  dsh_json_kv_str(j, "cacheKey", cache_key);
  dsh_json_kv_bool(j, "needsHttp", 1);
  dsh_json_kv_str(j, "url", DSH_MT_ENDPOINT);
  dsh_json_key(j, "headers");
  dsh_json_array_begin(j);
  {
    /* 三个头**由内核定**：resource id 是固定值、不给用户改 */
    const char *names[4];
    const char *values[4];
    size_t i;
    char *key_copy = dsh_mem_strdup(key);
    char *id_copy = dsh_mem_strdup(request_id);
    names[0] = "X-Api-Key";
    values[0] = key_copy;
    names[1] = "X-Api-Resource-Id";
    values[1] = DSH_MT_RESOURCE_ID;
    names[2] = "X-Api-Request-Id";
    values[2] = id_copy;
    names[3] = "Content-Type";
    values[3] = "application/json";
    for (i = 0; i < 4; i++) {
      dsh_json_array_begin(j);
      dsh_json_str(j, names[i]);
      dsh_json_str(j, values[i]);
      dsh_json_array_end(j);
    }
    if (key_copy != NULL) dsh_release(key_copy);
    if (id_copy != NULL) dsh_release(id_copy);
  }
  dsh_json_array_end(j);
  dsh_json_kv_str(j, "body", body);
  dsh_json_kv_str(j, "text", text);
  dsh_json_kv_str(j, "sourceLanguage", mt_source);
  dsh_json_kv_str(j, "targetLanguage", target);
  dsh_json_kv_str(j, "sourceLabel",
                  dsh_language_name(language)[0] ? dsh_language_name(language) : language);
  dsh_json_kv_str(j, "targetLabel", dsh_language_name(target));
  dsh_json_object_end(j);

  out = dsh_json_take(j);
  dsh_json_free(j);
  dsh_release(body);
  dsh_release(cache_key);
  if (out == NULL) {
    dsh_set_last_error("dsh_translate_plan：内存不足");
    return DSH_E_OOM;
  }
  *out_json = out;
  return DSH_OK;
}

/* ── dsh_translate_accept —— 「回包是什么意思」─────────────────────────── */

enum dsh_error dsh_translate_accept(dsh_engine *engine, const char *plan_json, int http_status,
                                    const char *response_body, int elapsed_ms, char **out_json) {
  dsh_json_doc *plan_doc = NULL;
  const dsh_json_node *plan_root = NULL;
  char *cache_key = NULL;
  char *text = NULL;
  char *mt_source = NULL;
  char *mt_target = NULL;
  char *source_label = NULL;
  char *target_label = NULL;
  dsh_mt_result result;
  dsh_json *j;
  char *out;

  (void)engine; /* 目前不需要引擎状态；签名留着是为了将来把缓存挂到引擎上 */

  dsh_clear_last_error();
  if (out_json == NULL) {
    dsh_set_last_error("dsh_translate_accept：out_json 不能为空");
    return DSH_E_INVALID_ARG;
  }
  if (plan_json != NULL && plan_json[0] != 0) {
    if (dsh_json_parse(plan_json, strlen(plan_json), &plan_doc) == 0) {
      plan_root = dsh_json_doc_root(plan_doc);
    }
  }

  {
    /* 从 plan 里把回话要用的几样捡出来（都是 plan 自己给的，不重新判一遍）*/
    const dsh_json_node *node;
    size_t len = 0;
    const char *s;
    node = (plan_root == NULL) ? NULL : dsh_json_object_get(plan_root, "cacheKey");
    s = (node == NULL) ? NULL : dsh_json_str_value(node, &len);
    if (s != NULL) {
      cache_key = dsh_mem_alloc(len + 1);
      if (cache_key != NULL) {
        memcpy(cache_key, s, len);
        cache_key[len] = 0;
      }
    }
    node = (plan_root == NULL) ? NULL : dsh_json_object_get(plan_root, "text");
    s = (node == NULL) ? NULL : dsh_json_str_value(node, &len);
    if (s != NULL) {
      text = dsh_mem_alloc(len + 1);
      if (text != NULL) {
        memcpy(text, s, len);
        text[len] = 0;
      }
    }
    node = (plan_root == NULL) ? NULL : dsh_json_object_get(plan_root, "sourceLanguage");
    s = (node == NULL) ? NULL : dsh_json_str_value(node, &len);
    if (s != NULL) {
      mt_source = dsh_mem_alloc(len + 1);
      if (mt_source != NULL) {
        memcpy(mt_source, s, len);
        mt_source[len] = 0;
      }
    }
    node = (plan_root == NULL) ? NULL : dsh_json_object_get(plan_root, "targetLanguage");
    s = (node == NULL) ? NULL : dsh_json_str_value(node, &len);
    if (s != NULL) {
      mt_target = dsh_mem_alloc(len + 1);
      if (mt_target != NULL) {
        memcpy(mt_target, s, len);
        mt_target[len] = 0;
      }
    }
    node = (plan_root == NULL) ? NULL : dsh_json_object_get(plan_root, "sourceLabel");
    s = (node == NULL) ? NULL : dsh_json_str_value(node, &len);
    if (s != NULL) {
      source_label = dsh_mem_alloc(len + 1);
      if (source_label != NULL) {
        memcpy(source_label, s, len);
        source_label[len] = 0;
      }
    }
    node = (plan_root == NULL) ? NULL : dsh_json_object_get(plan_root, "targetLabel");
    s = (node == NULL) ? NULL : dsh_json_str_value(node, &len);
    if (s != NULL) {
      target_label = dsh_mem_alloc(len + 1);
      if (target_label != NULL) {
        memcpy(target_label, s, len);
        target_label[len] = 0;
      }
    }
  }

  memset(&result, 0, sizeof(result));
  if (http_status == 0) {
    /* 外壳报告「根本没发出去」（断网 / 超时）—— 这不是服务端的错，话也不一样 */
    result.reason = dsh_mem_strdup("翻译请求没发出去（断网或者超时了）。");
  } else {
    dsh_translate_parse(http_status, response_body == NULL ? "" : response_body, &result);
  }

  if (result.ok) {
    cache_put(cache_key, result.translation, mt_source, mt_target, result.detected_source,
              result.total_tokens);
  }

  j = dsh_json_new();
  if (j == NULL) {
    dsh_translate_result_free(&result);
    if (cache_key != NULL) dsh_release(cache_key);
    if (text != NULL) dsh_release(text);
    if (mt_source != NULL) dsh_release(mt_source);
    if (mt_target != NULL) dsh_release(mt_target);
    if (source_label != NULL) dsh_release(source_label);
    if (target_label != NULL) dsh_release(target_label);
    if (plan_doc != NULL) dsh_json_doc_free(plan_doc);
    dsh_set_last_error("dsh_translate_accept：内存不足");
    return DSH_E_OOM;
  }

  dsh_json_object_begin(j);
  dsh_json_kv_bool(j, "ok", result.ok);
  dsh_json_kv_str(j, "message", result.ok ? "" : (result.reason == NULL ? "翻译失败" : result.reason));
  dsh_json_kv_str(j, "text", text == NULL ? "" : text);
  dsh_json_kv_str(j, "translation", result.ok ? result.translation : "");
  dsh_json_kv_str(j, "sourceLanguage", mt_source == NULL ? "" : mt_source);
  dsh_json_kv_str(j, "targetLanguage", mt_target == NULL ? "" : mt_target);
  dsh_json_kv_str(j, "sourceLabel", source_label == NULL ? "" : source_label);
  dsh_json_kv_str(j, "targetLabel", target_label == NULL ? "" : target_label);
  dsh_json_kv_str(j, "detected", result.detected_source == NULL ? "" : result.detected_source);
  /* 命中缓存的那些**不计费**，所以两个数分开给（界面据此算这次真花了多少）*/
  dsh_json_kv_i64(j, "tokens", result.total_tokens);
  dsh_json_kv_i64(j, "cachedItems", 0);
  dsh_json_kv_i64(j, "fetchedItems", result.ok ? 1 : 0);
  dsh_json_kv_i64(j, "elapsedMs", elapsed_ms < 0 ? 0 : elapsed_ms);
  /* 伪词条（`TranslateEntry`）：译文要能像真词条一样进正文框、进返回栈。
   * ⚠️ 只有**真翻出来**了才给（失败时给一份空文档等于让用户看到一张空白页）。*/
  if (result.ok) {
    char *entry = NULL;
    if (build_entry(text, result.translation, mt_source, source_label, target_label,
                    result.detected_source, result.total_tokens, 0, &entry) == DSH_OK &&
        entry != NULL) {
      dsh_json_key(j, "entry");
      dsh_json_value_raw(j, entry, strlen(entry));
      dsh_release(entry);
    }
  }
  dsh_json_object_end(j);

  out = dsh_json_take(j);
  dsh_json_free(j);
  dsh_translate_result_free(&result);
  if (cache_key != NULL) dsh_release(cache_key);
  if (text != NULL) dsh_release(text);
  if (mt_source != NULL) dsh_release(mt_source);
  if (mt_target != NULL) dsh_release(mt_target);
  if (source_label != NULL) dsh_release(source_label);
  if (target_label != NULL) dsh_release(target_label);
  if (plan_doc != NULL) dsh_json_doc_free(plan_doc);

  if (out == NULL) {
    dsh_set_last_error("dsh_translate_accept：内存不足");
    return DSH_E_OOM;
  }
  *out_json = out;
  return DSH_OK;
}

/* ── dsh_translate_clear_cache ─────────────────────────────────────────── */

enum dsh_error dsh_translate_clear_cache(dsh_engine *engine, char **out_json) {
  dsh_json *j;
  char *out;

  (void)engine;
  dsh_clear_last_error();
  if (out_json == NULL) {
    dsh_set_last_error("dsh_translate_clear_cache：out_json 不能为空");
    return DSH_E_INVALID_ARG;
  }

  j = dsh_json_new();
  if (j == NULL) {
    dsh_set_last_error("dsh_translate_clear_cache：内存不足");
    return DSH_E_OOM;
  }
  dsh_json_object_begin(j);
  dsh_json_kv_i64(j, "count", cache_clear_all());
  dsh_json_object_end(j);
  out = dsh_json_take(j);
  dsh_json_free(j);
  if (out == NULL) return DSH_E_OOM;
  *out_json = out;
  return DSH_OK;
}

/* ── dsh_translate_payload —— 译文当成一个词条（查词通道自动翻译那一档）─────
 * 把译文拼成与真词条同形状的载荷，界面于是不必认识「自动翻译」这件事。
 * ⚠️ `found` 只在真翻出来时才 true（否则 `via` 落回 `terminal`，不许给空页）；reason 这一层不拼。*/
enum dsh_error dsh_translate_payload(dsh_engine *engine, const char *translate_json,
                                     const char *entry_url, char **out_json) {
  int ok;
  int from_cache;
  char *text = NULL;
  char *translation = NULL;
  char *message = NULL;
  char *source_language = NULL;
  char *target_language = NULL;
  int64_t tokens;
  dsh_json *j = NULL;
  char *out = NULL;

  dsh_clear_last_error();
  if (out_json == NULL || translate_json == NULL) {
    dsh_set_last_error("dsh_translate_payload：出参不能为空");
    return DSH_E_INVALID_ARG;
  }
  if (engine == NULL) {
    dsh_set_last_error("dsh_translate_payload：引擎无效");
    return DSH_E_INVALID_ARG;
  }
  *out_json = NULL;
  (void)engine; /* 这一条只用入参里那份 JSON：它**已经是内核自己算出来的** */

  ok = bool_field_of(translate_json, "ok", 0);
  from_cache = bool_field_of(translate_json, "cached", 0);
  text = str_field_of(translate_json, "text");
  translation = str_field_of(translate_json, "translation");
  message = str_field_of(translate_json, "message");
  source_language = str_field_of(translate_json, "sourceLanguage");
  target_language = str_field_of(translate_json, "targetLanguage");
  tokens = num_field_of(translate_json, "tokens", 0);

  j = dsh_json_new();
  if (j == NULL) goto oom;

  dsh_json_object_begin(j);
  dsh_json_kv_str(j, "query", text != NULL ? text : "");
  dsh_json_kv_str(j, "keyText", text != NULL ? text : "");
  dsh_json_kv_str(j, "dictId", ok ? DSH_MT_ENTRY_DICT_ID : "");
  dsh_json_kv_str(j, "dictTitle", ok ? DSH_MT_ENTRY_TITLE : "");
  /* 地址由外壳给（令牌在它那儿）。没地址时**不给正文** —— 界面见空地址不会去导航。*/
  dsh_json_kv_str(j, "entryUrl", (ok && entry_url != NULL) ? entry_url : "");
  dsh_json_kv_str(j, "plainText", (ok && translation != NULL) ? translation : "");
  dsh_json_kv_bool(j, "found", ok);
  dsh_json_kv_bool(j, "sameAsShown", 0);
  dsh_json_key(j, "linkedTo");
  dsh_json_null(j);
  dsh_json_kv_str(j, "via", ok ? "translate" : "terminal");
  dsh_json_key(j, "unconfirmed");
  dsh_json_array_begin(j);
  dsh_json_array_end(j);
  dsh_json_kv_str(j, "reason",
                  ok ? ""
                     : ((message != NULL && message[0] != '\0') ? message : "翻译没成功。"));
  dsh_json_kv_str(j, "translateWhy", ok ? "" : (message != NULL ? message : ""));
  dsh_json_kv_bool(j, "offerTranslate", 0);
  dsh_json_kv_bool(j, "offerRecheck", 0);
  dsh_json_key(j, "chips");
  dsh_json_array_begin(j);
  dsh_json_array_end(j);
  dsh_json_key(j, "suggestions");
  dsh_json_array_begin(j);
  dsh_json_array_end(j);
  /* `speakText` 念的是**答案**（译文），不是问题；翻译没成时退回原文（系统语音认得任何词）。 */
  dsh_json_kv_str(j, "speakText",
                  (ok && translation != NULL && translation[0] != '\0')
                      ? translation
                      : (text != NULL ? text : ""));
  dsh_json_kv_str(j, "stage", "done");
  dsh_json_kv_str(j, "surface", "none");
  /* 界面不读下面这几个，排错时有用（这一页是不是缓存给的、译到哪个语种）*/
  dsh_json_kv_bool(j, "fromCache", from_cache);
  dsh_json_kv_i64(j, "tokens", tokens);
  dsh_json_kv_str(j, "sourceLanguage", source_language != NULL ? source_language : "");
  dsh_json_kv_str(j, "targetLanguage", target_language != NULL ? target_language : "");
  dsh_json_object_end(j);

  out = dsh_json_take(j);
  dsh_json_free(j);
  j = NULL;

  if (text != NULL) dsh_release(text);
  if (translation != NULL) dsh_release(translation);
  if (message != NULL) dsh_release(message);
  if (source_language != NULL) dsh_release(source_language);
  if (target_language != NULL) dsh_release(target_language);
  if (out == NULL) {
    dsh_set_last_error("dsh_translate_payload：内存不足");
    return DSH_E_OOM;
  }
  *out_json = out;
  return DSH_OK;

oom:
  if (j != NULL) dsh_json_free(j);
  if (text != NULL) dsh_release(text);
  if (translation != NULL) dsh_release(translation);
  if (message != NULL) dsh_release(message);
  if (source_language != NULL) dsh_release(source_language);
  if (target_language != NULL) dsh_release(target_language);
  dsh_set_last_error("dsh_translate_payload：内存不足");
  return DSH_E_OOM;
}
