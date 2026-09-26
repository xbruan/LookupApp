/* ==========================================================================
 * 机器翻译 · 纯逻辑层 —— 约定与理由见 `dsh_translate.c` 顶部。
 *
 * 分工：**内核说「该发什么、回包是什么意思」，外壳只管把那段字节发出去** ——
 * 所以这一层没有任何 I/O，全是纯函数，诊断能直接调、不必联网。
 * ========================================================================== */

#ifndef DSH_TRANSLATE_DSH_TRANSLATE_H
#define DSH_TRANSLATE_DSH_TRANSLATE_H

#include <stddef.h>
#include <stdint.h>

/** 端点与 resource id（resource id 是固定值，不设字段、不给用户改）*/
#define DSH_MT_ENDPOINT \
  "https://openspeech.bytedance.com/api/v3/machine_translation/matx_translate"
#define DSH_MT_RESOURCE_ID "volc.speech.mt"

/** `text_list` 的条数上限（服务端的硬约束；切分是调用方的事）*/
#define DSH_MT_MAX_ITEMS 16

/** 进了缓存键的引擎版本号 —— 请求约定一变就改它，旧缓存自动失效 */
#define DSH_MT_ENGINE_VERSION "mt-v1"

/** 成功码（**与语音合成不是同一张表**，别复用）*/
#define DSH_MT_CODE_OK 20000000

/** 本机语种标签 → MT 认的 32 种之一。
 *
 * @return 静态字符串（不要 free）；**这个语种 MT 不支持时返回 NULL** ——
 *    NULL 的含义只有一个：**这种语言不提供翻译**，调用方要如实说
 *    「机器翻译暂不支持〈波斯语〉」，**绝不许退化成英语**。
 */
const char *dsh_translate_language(const char *tag);

/**
 * 目标语种。
 *
 * @param language    `dsh_language_decide` 判出来的**语种主代码**（`zh` / `en` / `ja` …）
 * @param target_mode 用户设置：`auto` / `zh` / `en`（NULL 或空串 = auto）
 * @return `en` / `zh`；`target_mode` 认不出来时 **NULL**（不猜）
 *
 * ⚠️ 收的是**语种**而不是字形：语种判定本身已包含字形线索，还多一条服务端没有的（词典标题，
 *    例如「牛津高阶英汉双解」）—— 收窄成字形等于把那条线索丢掉。
 */
const char *dsh_translate_target(const char *language, const char *target_mode);

/**
 * 拼请求体（`{source_language?, target_language, text_list[]}`）。
 *
 * @param source 可以为 NULL / 空串 —— 那就不写这个键（= 让服务端自己检测）
 * @return 新分配的 JSON 串（调用方 `dsh_release`）；参数不合法返回 NULL（last_error 已写）
 */
char *dsh_translate_request_body(const char *source, const char *target, const char *const *texts,
                                 size_t count);

/**
 * 缓存键：`sha256(引擎版本 | source | target | 文本)`（按 token 计费，缓存是必需项）。
 *
 * @return 64 个十六进制字符（调用方 `dsh_release`）
 */
char *dsh_translate_cache_key(const char *source, const char *target, const char *text);

/** 一次翻译的回包读出来的东西 */
typedef struct {
  int ok;                 /**< 业务码是成功码，且真拿到了译文 */
  int http_status;        /**< 外壳报告的状态码（原样带着，排错要用） */
  int64_t code;           /**< 业务码（`20000000` = 成功） */
  char *translation;      /**< 译文（`dsh_release` 还给内核；失败时 NULL） */
  char *detected_source;  /**< 服务端识别到的源语种（可空：请求指定了 source 时不会有） */
  int64_t prompt_tokens;
  int64_t completion_tokens;
  int64_t total_tokens;
  char *reason;           /**< 失败时那句人话（`dsh_release` 还给内核；成功时 NULL） */
} dsh_mt_result;

/**
 * 解析回包（§B2 的结构 + §B9 的错误码）。
 *
 * ⚠️ **非 200 也算"答上来了"**：这一版 MT 是"HTTP 200 + 业务 code"，所以 403/401
 *    各自翻成对应的那句人话（403 = 没开通 `volc.speech.mt`，它与语音是两个独立权限）。
 *
 * @return 0 = 解析过程本身成功（**看 `out->ok` 判断翻译成没成**）；-1 = 入参不合法
 */
int dsh_translate_parse(int http_status, const char *body, dsh_mt_result *out);

/** 释放 `dsh_translate_parse` 填出来的那些串 */
void dsh_translate_result_free(dsh_mt_result *result);

#endif /* DSH_TRANSLATE_DSH_TRANSLATE_H */
