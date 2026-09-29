/* ==========================================================================
 * 内核单元测试 · 机器翻译（纯逻辑层）
 *
 * 这一层一条网络请求都不发（内核零依赖）：它只管「该发什么」（语种映射 / 目标语种 /
 * 请求体）与「回包是什么意思」（译文 / 计费 token / 错误码翻成的人话）/ 缓存键。
 *
 * 四条**容易不报错地出错**的约定（约定全文见 docs/design/不带mdd的词典与机器翻译开发指导.md）：
 *   ① `no`→`nb`、繁体→`zh-Hant`、菲律宾→`tl` 三处与本机对不上，少一条只会翻出奇怪的东西；
 *   ② MT 不支持的语言必须回 NULL，**绝不退化成英语** —— 方向被偷偷改掉是最糟的失败方式；
 *   ③ 错误码表与 TTS **不是同一张**（`20000000` 在这儿是成功），混起来会把成功当失败；
 *   ④ 非 200 也算「答上来了」：403 要翻成「没开通 volc.speech.mt（与语音是两个独立权限）」，
 *      那是最容易被误判成「Key 填错了」的一档。
 * ========================================================================== */

#include "dsh_lookup.h"
#include "translate/dsh_translate.h"

#include "dsh_lookup.h"
#include "dsh_internal.h"
#include "mem_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks = 0;
static int g_failed = 0;

/** 从 JSON 里取 `key` 那一项的字符串值（够用就行，不做通用解析） */
static void str_field(const char *json, const char *key, char *buf, size_t cap) {
  char pat[96];
  const char *p;
  size_t k = 0;
  buf[0] = '\0';
  if (json == NULL) return;
  snprintf(pat, sizeof(pat), "\"%s\":\"", key);
  p = strstr(json, pat);
  if (p == NULL) return;
  p += strlen(pat);
  while (*p != '\0' && *p != '"' && k + 1 < cap) buf[k++] = *p++;
  buf[k] = '\0';
}

static void ok(int cond, const char *what) {
  g_checks++;
  if (!cond) {
    g_failed++;
    fprintf(stderr, "FAIL %s\n", what);
  }
}

static void ok_eq_str(const char *actual, const char *expected, const char *what) {
  g_checks++;
  if (actual == NULL || expected == NULL || strcmp(actual, expected) != 0) {
    g_failed++;
    fprintf(stderr, "FAIL %s\n      实际=%s\n      期望=%s\n", what, actual ? actual : "(null)",
            expected ? expected : "(null)");
  }
}

static void ok_eq_i64(int64_t actual, int64_t expected, const char *what) {
  g_checks++;
  if (actual != expected) {
    g_failed++;
    fprintf(stderr, "FAIL %s：实际=%lld 期望=%lld\n", what, (long long)actual, (long long)expected);
  }
}

/** 子串检查标准（人话里必须带上的关键词）*/
static void ok_has(const char *text, const char *needle, const char *what) {
  g_checks++;
  if (text == NULL || strstr(text, needle) == NULL) {
    g_failed++;
    fprintf(stderr, "FAIL %s\n      实际=%s\n      里面没有=%s\n", what, text ? text : "(null)",
            needle);
  }
}

/* ══════════════════════════════════════════════════════════════════════════
   ① 语种映射：32 种 + 三处对不上的 + 不支持的
   ══════════════════════════════════════════════════════════════════════════ */

static void test_language_map(void) {
  /* 直通的那些（本机标签 → MT 码）*/
  ok_eq_str(dsh_translate_language("en"), "en", "① en → en");
  ok_eq_str(dsh_translate_language("en-US"), "en", "① en-US 取主标签 → en");
  ok_eq_str(dsh_translate_language("ja"), "ja", "① ja → ja");
  ok_eq_str(dsh_translate_language("ko"), "ko", "① ko → ko");
  ok_eq_str(dsh_translate_language("ru"), "ru", "① ru → ru");
  ok_eq_str(dsh_translate_language("pt-BR"), "pt", "① pt-BR → pt");

  /* ★ 三处与本机对不上的（§B4 那张表）—— 少一条只会不报错地出错 */
  ok_eq_str(dsh_translate_language("no"), "nb", "①★ 挪威语 no → nb");
  ok_eq_str(dsh_translate_language("nn"), "nb", "①★ 新挪威语 nn → nb（MT 只有这一栏）");
  ok_eq_str(dsh_translate_language("zh-TW"), "zh-Hant", "①★ 繁体（zh-TW）→ zh-Hant");
  ok_eq_str(dsh_translate_language("zh-HK"), "zh-Hant", "①★ 繁体（zh-HK）→ zh-Hant");
  ok_eq_str(dsh_translate_language("zh-Hant"), "zh-Hant", "①★ 繁体（zh-Hant）→ zh-Hant");
  ok_eq_str(dsh_translate_language("fil"), "tl", "①★ 菲律宾语 fil → tl");

  /* 简体中文留在 zh（别被上面那条繁体规则带走）*/
  ok_eq_str(dsh_translate_language("zh"), "zh", "① 简体 zh → zh");
  ok_eq_str(dsh_translate_language("zh-CN"), "zh", "① 简体 zh-CN → zh");

  /* ★ 本机有、MT 没有的 → NULL（**绝不退化成英语**）*/
  ok(dsh_translate_language("fa") == NULL, "①★ 波斯语不在 MT 的 32 种里 → NULL");
  ok(dsh_translate_language("ur") == NULL, "①★ 乌尔都语 → NULL");
  ok(dsh_translate_language("sk") == NULL, "①★ 斯洛伐克语 → NULL");
  ok(dsh_translate_language("la") == NULL, "①★ 拉丁语 → NULL");
  ok(dsh_translate_language("") == NULL, "① 空标签 → NULL");
  ok(dsh_translate_language(NULL) == NULL, "① NULL → NULL");

  /* 大小写不敏感（页面/设置里什么写法都可能出现）*/
  ok_eq_str(dsh_translate_language("EN"), "en", "① 大写 EN → en");
  ok_eq_str(dsh_translate_language("Zh-TW"), "zh-Hant", "① 混合大小写 Zh-TW → zh-Hant");
}

/* ══════════════════════════════════════════════════════════════════════════
   ② 目标语种：auto 按字形判，固定档听用户的
   ══════════════════════════════════════════════════════════════════════════ */

static void test_target(void) {
  /* auto：中文 → en，其余 → zh（§B5）*/
  ok_eq_str(dsh_translate_target("zh", "auto"), "en", "② 中文 → 译英");
  ok_eq_str(dsh_translate_target("zh", NULL), "en", "② 中文（没给 mode）→ 译英");
  ok_eq_str(dsh_translate_target("zh", ""), "en", "② 中文（空 mode）→ 译英");
  ok_eq_str(dsh_translate_target("en", "auto"), "zh", "② 英语 → 译中");
  ok_eq_str(dsh_translate_target("ja", "auto"), "zh", "② 日语 → 译中");
  ok_eq_str(dsh_translate_target(NULL, "auto"), "zh", "② 判不出语种 → 译中（不猜英语）");

  /* 用户固定了方向就听用户的 —— 哪怕与「按语种判」相反 */
  ok_eq_str(dsh_translate_target("zh", "zh"), "zh", "② 固定译中：中文也译中");
  ok_eq_str(dsh_translate_target("en", "en"), "en", "② 固定译英：英语也译英");

  /* 认不出来的模式**不猜**（返回 NULL，让调用方如实报错）*/
  ok(dsh_translate_target("zh", "de") == NULL, "② 认不出的 targetMode → NULL");
}

/* ══════════════════════════════════════════════════════════════════════════
   ③ 请求体
   ══════════════════════════════════════════════════════════════════════════ */

static void test_request_body(void) {
  const char *one[1];

  one[0] = "苹果";
  {
    char *body = dsh_translate_request_body("zh", "en", one, 1);
    ok(body != NULL, "③ 拼得出请求体");
    ok_has(body, "\"target_language\":\"en\"", "③ 带上 target_language");
    ok_has(body, "\"source_language\":\"zh\"", "③ 带上 source_language");
    ok_has(body, "\"text_list\":[\"苹果\"]", "③ text_list 是数组、一条文本");
    dsh_release(body);
  }

  /* source 为空 = **不写这个键**（让服务端自己检测），而不是写一个空串 */
  {
    char *body = dsh_translate_request_body(NULL, "en", one, 1);
    ok(body != NULL, "③ source 为 NULL 也拼得出来");
    ok(strstr(body, "source_language") == NULL, "③ source 为空 → 不写这个键（不是空串）");
    dsh_release(body);
  }
  {
    char *body = dsh_translate_request_body("", "en", one, 1);
    ok(strstr(body, "source_language") == NULL, "③ source 为空串 → 同样不写");
    dsh_release(body);
  }

  /* 词条名里出现半角双引号是真实存在的：不转义就会拼出一份坏 JSON */
  {
    const char *tricky[1];
    char *body;
    tricky[0] = "he said \"hi\"";
    body = dsh_translate_request_body(NULL, "zh", tricky, 1);
    ok_has(body, "he said \\\"hi\\\"", "③ 文本里的半角双引号被转义了");
    dsh_release(body);
  }

  /* 16 条是上限，超了**如实拒**（不悄悄截断）*/
  {
    const char *many[17];
    int i;
    char *body;
    for (i = 0; i < 17; i++) many[i] = "x";
    body = dsh_translate_request_body(NULL, "zh", many, 17);
    ok(body == NULL, "③ 17 条 → 拒（上限 16）");
    ok_has(dsh_last_error_message(), "16", "③ 拒的时候那句话里写了上限");
  }

  ok(dsh_translate_request_body(NULL, NULL, one, 1) == NULL, "③ 没有 target → 拒");
  ok(dsh_translate_request_body(NULL, "en", NULL, 1) == NULL, "③ 没有文本 → 拒");
  ok(dsh_translate_request_body(NULL, "en", one, 0) == NULL, "③ 文本条数为 0 → 拒");
}

/* ══════════════════════════════════════════════════════════════════════════
   ④ 缓存键
   ══════════════════════════════════════════════════════════════════════════ */

static void test_cache_key(void) {
  char *a = dsh_translate_cache_key("zh", "en", "苹果");
  char *b = dsh_translate_cache_key("zh", "en", "苹果");
  char *c = dsh_translate_cache_key("zh", "en", "苹果 ");
  char *d = dsh_translate_cache_key("zh", "zh", "苹果");

  ok(a != NULL && strlen(a) == 64, "④ 键是 64 个十六进制字符");
  ok_eq_str(a, b, "④ 同样的输入 → 同样的键（缓存能命中）");
  ok(c != NULL && strcmp(a, c) != 0, "④ 文本差一个字符 → 键不同");
  ok(d != NULL && strcmp(a, d) != 0, "④ 目标语种不同 → 键不同");

  /* 引擎版本进键：改了请求约定之后旧缓存自动失效 */
  ok(strstr(a, DSH_MT_ENGINE_VERSION) == NULL, "④ 键里不出现明文版本号（是哈希过的）");

  dsh_release(a);
  dsh_release(b);
  dsh_release(c);
  dsh_release(d);
}

/* ══════════════════════════════════════════════════════════════════════════
   ⑤ 回包解析：成功 / 各档错误 / 非 200
   ══════════════════════════════════════════════════════════════════════════ */

static void test_parse_ok(void) {
  const char *body =
      "{\"code\":20000000,\"message\":\"ok\",\"data\":{\"translation_list\":["
      "{\"translation\":\"Apple\",\"detected_source_language\":\"zh\","
      "\"usage\":{\"prompt_tokens\":19,\"completion_tokens\":7,\"total_tokens\":26}}]}}";
  dsh_mt_result r;

  ok(dsh_translate_parse(200, body, &r) == 0, "⑤ 解析返回 0（看 ok 判成败）");
  ok(r.ok == 1, "⑤ ★ 20000000 在这张表里是**成功**（TTS 那边不是，别混）");
  ok_eq_i64(r.code, 20000000, "⑤ 业务码原样带着");
  ok_eq_str(r.translation, "Apple", "⑤ 译文");
  ok_eq_str(r.detected_source, "zh", "⑤ 服务端识别到的源语种（交叉验证用）");
  ok_eq_i64(r.total_tokens, 26, "⑤ 计费 token 总数");
  ok_eq_i64(r.prompt_tokens, 19, "⑤ prompt token");
  ok_eq_i64(r.completion_tokens, 7, "⑤ completion token");
  ok(r.reason == NULL, "⑤ 成功时没有人话原因");
  dsh_translate_result_free(&r);

  /* 没指定 source 时才返回 detected_source_language —— 缺它也不算错 */
  {
    const char *no_detect =
        "{\"code\":20000000,\"data\":{\"translation_list\":[{\"translation\":\"苹果\"}]}}";
    ok(dsh_translate_parse(200, no_detect, &r) == 0, "⑤ 没有 detected/usage 也解析得动");
    ok(r.ok == 1, "⑤ 仍然算成功");
    ok_eq_str(r.translation, "苹果", "⑤ 译文照旧");
    ok(r.detected_source == NULL, "⑤ 没有识别语种就是 NULL");
    ok_eq_i64(r.total_tokens, 0, "⑤ 没有 usage 就是 0（不是瞎猜的数）");
    dsh_translate_result_free(&r);
  }
}

static void test_parse_errors(void) {
  dsh_mt_result r;

  /* 45000130 载荷过大 */
  ok(dsh_translate_parse(200, "{\"code\":45000130,\"message\":\"payload too large\"}", &r) == 0,
     "⑤ 载荷过大解析得动");
  ok(r.ok == 0, "⑤ ★ 业务码不是成功码 → 不算成功（回包里 code 0 那种坑在 TTS 那边，别混）");
  ok_eq_i64(r.code, 45000130, "⑤ 码原样带着");
  ok_has(r.reason, "太长", "⑤ 载荷过大的那句话");
  dsh_translate_result_free(&r);

  /* 45000001 参数错 —— 是**代码 bug**，人话里要说清不是用户的问题 */
  ok(dsh_translate_parse(200, "{\"code\":45000001,\"message\":\"bad target\"}", &r) == 0,
     "⑤ 参数错解析得动");
  ok(r.ok == 0, "⑤ 参数错不算成功");
  ok_has(r.reason, "程序的问题", "⑤ ★ 参数错要说明是程序的问题，不是用户操作错");
  dsh_translate_result_free(&r);

  /* 55000001 服务内部错误 */
  ok(dsh_translate_parse(200, "{\"code\":55000001,\"message\":\"oops\"}", &r) == 0, "⑤ 内部错解析得动");
  ok_has(r.reason, "稍后", "⑤ 内部错让人稍后重试");
  dsh_translate_result_free(&r);

  /* 认不出来的码：把人话 + 服务端原话 + code 一起给（排错用）*/
  ok(dsh_translate_parse(200, "{\"code\":12345678,\"message\":\"who knows\"}", &r) == 0,
     "⑤ 陌生码解析得动");
  ok_has(r.reason, "12345678", "⑤ 陌生码的说明里带上 code");
  ok_has(r.reason, "who knows", "⑤ 陌生码的说明里带上服务端原话");
  dsh_translate_result_free(&r);

  /* 回包结构不对：一句话说清，不要崩 */
  ok(dsh_translate_parse(200, "not json at all", &r) == 0, "⑤ 坏 JSON 也回 0（不崩）");
  ok(r.ok == 0, "⑤ 坏 JSON 不算成功");
  ok_has(r.reason, "JSON", "⑤ 坏 JSON 的说明");
  dsh_translate_result_free(&r);

  ok(dsh_translate_parse(200, "{\"message\":\"no code\"}", &r) == 0, "⑤ 没有 code 也解析得动");
  ok(r.ok == 0, "⑤ 没有 code 不算成功");
  ok_has(r.reason, "code", "⑤ 缺 code 的说明");
  dsh_translate_result_free(&r);

  ok(dsh_translate_parse(200, "{\"code\":20000000,\"data\":{}}", &r) == 0, "⑤ 成功码但没有译文");
  ok(r.ok == 0, "⑤ ★ 成功码但拿不到译文 → **不算成功**（宁可报错也不给空译文）");
  ok_has(r.reason, "译文", "⑤ 缺译文的说明");
  dsh_translate_result_free(&r);
}

static void test_parse_http(void) {
  dsh_mt_result r;

  /* ★ 403：最常见的误判 —— 「没开通 volc.speech.mt」与「Key 填错了」症状几乎一样 */
  ok(dsh_translate_parse(403, "", &r) == 0, "⑤ 403 解析得动");
  ok(r.ok == 0, "⑤ 403 不算成功");
  ok_eq_i64(r.http_status, 403, "⑤ 状态码原样带着");
  ok_has(r.reason, "volc.speech.mt", "⑤ ★ 403 要说清是哪个权限");
  ok_has(r.reason, "独立权限", "⑤ ★ 还要说清它与语音是两个独立权限");
  dsh_translate_result_free(&r);

  ok(dsh_translate_parse(401, "", &r) == 0, "⑤ 401 解析得动");
  ok_has(r.reason, "Key", "⑤ 401 说的是 Key 不对");
  dsh_translate_result_free(&r);

  ok(dsh_translate_parse(502, "", &r) == 0, "⑤ 502 解析得动");
  ok_has(r.reason, "502", "⑤ 其它状态码带上状态码本身");
  dsh_translate_result_free(&r);
}

/**
 * ⑥ **接口那一层**（`status` / `plan` / `accept` / `payload`）—— 「设置怎么读、请求怎么拼、
 * 回包怎么变成界面那一页」这三件事真正发生的地方。
 *
 * 它要真引擎（设置要落盘、缓存要跨调用），所以用测试用词典旁边的临时配置目录，**不碰用户配置**。
 */
static void test_api(void) {
  const size_t base = dsh_mem_live_count();
  const char *fixtures = DSH_TESTDATA_DIR;
  char dir[1024];
  char mk[2200]; /* 两条路径 + 固定片段：留够，别让 gcc 报 format-truncation */
  dsh_engine *engine = NULL;
  char *json = NULL;

  snprintf(dir, sizeof(dir), "%s/tmp-translate-api", fixtures);
  snprintf(mk, sizeof(mk), "rm -rf '%s' && mkdir -p '%s'", dir, dir);
  if (system(mk) != 0) { /* 清不掉也要继续：下面建引擎会自己判断 */ }
  if (dsh_engine_create(dir, &engine) != DSH_OK || engine == NULL) {
    ok(0, "⑥ 建引擎");
    return;
  }

  /* ① 默认（什么都没设过）：开关关着、自动翻译开着、方向 auto、没有 Key */
  ok(dsh_translate_status(engine, &json) == DSH_OK, "⑥ 取翻译状态");
  ok_has(json, "\"hasApiKey\":false", "⑥ 默认没有 Key");
  ok_has(json, "\"enabled\":false", "⑥ ★ 默认总开关**关着**（要联网 + 把文本发给第三方）");
  ok_has(json, "\"autoTranslate\":true", "⑥ 默认「查不到时自动翻译」开着");
  ok_has(json, "\"targetMode\":\"auto\"", "⑥ 默认方向 auto");
  ok_has(json, "\"resourceId\":\"volc.speech.mt\"", "⑥ resourceId 恒为 volc.speech.mt");
  dsh_release(json);
  json = NULL;

  /*
   * ② 还没配齐的时候 plan：**一次请求都不发**，而且缺什么就说什么。
   *
   * ⚠️ 顺序有讲究：开关关着时先说**开关**（那是隐私检查，用户得先自己打开），
   *    打开了才轮到「缺凭据」—— 两种原因分开说，用户才知道去改哪儿。
   *    而且开关关着时**哪一条通道都不许发请求**（界面上那条「翻译这个词」还摆着，点它也一样）。
   */
  ok(dsh_translate_plan(engine, "apple", "test.mdx", &json) == DSH_OK, "⑥ 没配齐时也问得动");
  ok_has(json, "\"ok\":false", "⑥ 没配齐 → ok=false");
  ok_has(json, "\"needsHttp\":false", "⑥ ★ 没配齐就不该发请求（needsHttp=false）");
  ok_has(json, "\"why\":\"disabled\"", "⑥ ★ 关着时给的原因是 disabled");
  ok_has(json, "总开关", "⑥ ★ 而且说的是「总开关」（不是「缺凭据」——先让用户打开它）");
  dsh_release(json);
  json = NULL;

  /* ③ 打开开关、**还没填 Key** → 这一档该说的是「凭据」*/
  ok(dsh_engine_settings_set(engine, "{\"translate\":{\"enabled\":true}}", &json) == DSH_OK,
     "⑥ 只打开开关");
  dsh_release(json);
  json = NULL;
  ok(dsh_translate_plan(engine, "apple", "test.mdx", &json) == DSH_OK, "⑥ 开着但没凭据");
  ok_has(json, "\"needsHttp\":false", "⑥ 没凭据仍然不发请求");
  ok_has(json, "\"why\":\"no-key\"", "⑥ ★ 这一档给的原因是 no-key");
  ok_has(json, "凭据", "⑥ ★ 说的是「凭据」（两种原因分开说，用户才知道去改哪儿）");
  dsh_release(json);
  json = NULL;

  /* ④ 填上 Key（账号级那一把，与语音共用）+ 固定方向：归一化与「该发什么」一起验 */
  ok(dsh_engine_settings_set(engine,
                             "{\"translate\":{\"targetMode\":\"ZH\"},"
                             "\"volcengine\":{\"apiKey\":\"k-123\"}}",
                             &json) == DSH_OK,
     "⑥ 填 Key 并固定方向");
  dsh_release(json);
  json = NULL;
  ok(dsh_translate_status(engine, &json) == DSH_OK, "⑥ 再取一次状态");
  ok_has(json, "\"hasApiKey\":true", "⑥ Key 读到了");
  ok_has(json, "\"enabled\":true", "⑥ 开关开着");
  ok_has(json, "\"targetMode\":\"zh\"", "⑥ ★ 大写 ZH 归一化成 zh（同一个设置模型）");
  dsh_release(json);
  json = NULL;

  /* ⑤ 开着 + 有 Key：plan 给出该发什么（url / 三个头 / 请求体）*/
  ok(dsh_translate_plan(engine, "apple", "test.mdx", &json) == DSH_OK, "⑥ 开着时 plan");
  ok_has(json, "openspeech.bytedance.com", "⑥ 端点给出来了");
  ok_has(json, "\"needsHttp\":true", "⑥ ★ 该发请求");
  ok_has(json, "volc.speech.mt", "⑥ 请求头里带 ResourceId");
  ok_has(json, "X-Api-Key", "⑥ 请求头里带 Key");
  {
    /* 请求体里目标语种必须是**用户固定的那个**（zh），不是 auto 推出来的 */
    ok_has(json, "\\\"target_language\\\":\\\"zh\\\"", "⑥ 请求体的目标语种 = 用户固定的 zh");
  }
  {
    char *plan = dsh_mem_strdup(json);
    char *accept = NULL;
    dsh_release(json);
    json = NULL;
    /* ⑤ accept：拿一份**成功**回包 → 译文 + 计费 + **伪词条**（虚拟词典那一份）*/
    {
      const char *body =
          "{\"code\":20000000,\"message\":\"ok\",\"data\":{\"translation_list\":["
          "{\"translation\":\"苹果 <b>派</b>\",\"detected_source_language\":\"en\"}],"
          "\"source_language\":\"en\",\"target_language\":\"zh\"}}";
      ok(dsh_translate_accept(engine, plan, 200, body, 120, &accept) == DSH_OK, "⑥ accept 成功回包");
      ok_has(accept, "\"ok\":true", "⑥ 成功");
      ok_has(accept, "\"translation\":\"苹果 <b>派</b>\"", "⑥ 译文原样带回来");
      ok_has(accept, "\"targetLanguage\":\"zh\"", "⑥ 目标语种是 zh");
      ok_has(accept, "\"tokens\":", "⑥ 计费 token 给出来了");
      /* ★ 伪词条三件套：虚拟 id / 中文名 / 文档正文 */
      ok_has(accept, "\"dictId\":\"translate\"", "⑥ ★ 伪词条的虚拟词典 id 是 translate");
      ok_has(accept, "\"dictTitle\":\"机器翻译\"", "⑥ ★ 标题栏上写「机器翻译」（用户一眼看出不是词典给的）");
      ok_has(accept, "\"fromCache\":false", "⑥ 这次不是缓存给的");
      ok_has(accept, "\"entryToken\":", "⑥ ★ 给了内容寻址的令牌（外壳拿它拼地址）");
      ok_has(accept, "\"entryHtml\":", "⑥ ★ 给了整份词条正文（沙箱、桥、样式都在内核这一处）");
      /* 正文里的译文是**机器生成的文本**：`<` 必须被转义，否则那是一处注入 */
      ok(strstr(accept, "&lt;b&gt;派&lt;/b&gt;") != NULL,
         "⑥ ★★ 译文里的 `<` `>` 被转义（机器生成的文本不许当 HTML 塞进去）");
      ok_has(accept, "英语 → 中文", "⑥ 方向那一行写着「英语 → 中文」（§B5）");
      ok_has(accept, "本次 0 tokens", "⑥ 用量那行说的是**真花掉的那些**（这里回包没给用量就是 0）");
      ok_has(accept, "原文：apple", "⑥ 正文底部带原文（对照用）");
      /* ⑥ payload：拿 accept 的结果 + 外壳拼好的地址 → 一份与真词条同形状的载荷 */
      {
        char *payload = NULL;
        ok(dsh_translate_payload(engine, accept, "https://translate.dictres.invalid/__entry__?tok=abc",
                                 &payload) == DSH_OK,
           "⑥ 把译文包成词条载荷");
        ok_has(payload, "\"via\":\"translate\"", "⑥ ★ via=translate（界面据此写解释行）");
        ok_has(payload, "\"found\":true", "⑥ ★ 翻出来了就是「找到了词条」");
        ok_has(payload, "\"dictId\":\"translate\"", "⑥ 虚拟词典 id");
        ok_has(payload, "\"dictTitle\":\"机器翻译\"", "⑥ 标题是机器翻译");
        ok_has(payload, "\"entryUrl\":\"https://translate.dictres.invalid/__entry__?tok=abc\"",
               "⑥ 地址用的就是外壳给的那一条");
        ok_has(payload, "\"plainText\":\"苹果 <b>派</b>\"", "⑥ 正文文本是译文（朗读/复制都用它）");
        ok_has(payload, "\"speakText\":\"苹果 <b>派</b>\"", "⑥ ★ 朗读念的是**答案**（译文），不是问题");
        ok_has(payload, "\"offerTranslate\":false", "⑥ 已经在译文页上了，不再给「翻译这个词」");
        ok_has(payload, "\"reason\":\"\"", "⑥ 成功时没有解释行");
        dsh_release(payload);
      }
      /* ⑦ 同一个地址再问一次：令牌是**内容寻址**的，所以两次相同 */
      {
        char token1[128];
        char token2[128];
        char *again = NULL;
        const char *body2 =
            "{\"code\":20000000,\"data\":{\"translation_list\":[{\"translation\":\"苹果 <b>派</b>\"}],"
            "\"source_language\":\"en\",\"target_language\":\"zh\"}}";
        str_field(accept, "entryToken", token1, sizeof(token1));
        if (dsh_translate_accept(engine, plan, 200, body2, 90, &again) == DSH_OK) {
          str_field(again, "entryToken", token2, sizeof(token2));
          ok(token1[0] != '\0' && strcmp(token1, token2) == 0,
             "⑦ ★ 同一段译文 → 同一个令牌（内容寻址：外壳那张表天然幂等）");
          dsh_release(again);
        }
      }
      /* ⑧ 失败回包：**不许**装作翻过了 —— 没有 entry，载荷落回终态页 */
      {
        char *failed = NULL;
        const char *bad = "{\"code\":45000130,\"message\":\"payload too large\"}";
        ok(dsh_translate_accept(engine, plan, 200, bad, 50, &failed) == DSH_OK, "⑥ 失败回包也解析得动");
        ok_has(failed, "\"ok\":false", "⑥ 失败就是失败");
        ok(strstr(failed, "entryHtml") == NULL, "⑥ ★★ 没翻出来就**不给文档**（不给一张空白译文页）");
        {
          char *payload = NULL;
          ok(dsh_translate_payload(engine, failed, "https://x/y", &payload) == DSH_OK,
             "⑥ 把失败的翻译包成载荷");
          ok_has(payload, "\"found\":false", "⑥ ★ 没翻出来 → found=false（不许给 found:true 的空页）");
          ok_has(payload, "\"via\":\"terminal\"", "⑥ ★ 而且落回终态页（via=terminal）");
          ok_has(payload, "\"entryUrl\":\"\"", "⑥ 没有要跳的地址");
          /*
           * ⚠️ `reason` 里是**内核自己翻成人话的那句**，不是服务端原文
           *    （英文串直接上屏没有意义）—— 所以这里钉「给了一句人话」，而不是某个具体字面量。
           */
          ok_has(payload, "\"reason\":\"", "⑥ 失败也有一句人话（原样来自内核）");
          ok(strstr(payload, "\"reason\":\"\"") == NULL, "⑥ ★ 而且那句人话**不是空的**");
          dsh_release(payload);
        }
        dsh_release(failed);
      }
      dsh_release(accept);
    }
    dsh_release(plan);
  }

  /* 收尾：译文缓存是**进程级**的（同一段译文反复翻不重复计费），所以清一次再算账 */
  {
    char *cleared = NULL;
    if (dsh_translate_clear_cache(engine, &cleared) == DSH_OK && cleared != NULL) {
      ok_has(cleared, "\"count\":", "⑥ 清缓存给出清掉几条");
      dsh_release(cleared);
    }
  }
  dsh_engine_destroy(engine);
  ok((size_t)dsh_mem_live_count() == base, "⑥ 这一组没漏内存（清掉进程级缓存之后回到基线）");
}

int main(void) {
  test_language_map();
  test_target();
  test_request_body();
  test_cache_key();
  test_parse_ok();
  test_parse_errors();
  test_parse_http();
  test_api();

  printf("翻译：%d 项，失败 %d\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
