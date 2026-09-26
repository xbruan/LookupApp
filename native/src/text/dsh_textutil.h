/* ==========================================================================
 * 文本小工具 —— 音节分隔点、字形、词数。约定与那两条警告见 .c 顶部。
 * ========================================================================== */

#ifndef DSH_TEXT_TEXTUTIL_H
#define DSH_TEXT_TEXTUTIL_H

#include "text/dsh_language.h"

#include <stddef.h>
#include <stdint.h>

/**
 * UTF-8 解码一个码位，`*i` 前进若干字节。
 *
 * 坏字节按 U+FFFD 处理并前进 1 字节 —— 与 `dsh_text_decode` 同一约定：一个坏字节不该让
 * 整段判定失败。
 *
 * ⚠️ 调用方要保证 `*i < len`（函数一进来就读 `s[*i]`）。
 */
uint32_t dsh_text_next_cp(const char *s, size_t len, size_t *i);

/**
 * 百分号转义：只留 `A-Za-z0-9-_.~`，其余按 UTF-8 字节写成 `%XX`。
 *
 * @return 内核分配（调用方 `dsh_release`）；**内存不足回 NULL 且不设 last_error**
 *         （两处调用方各有自己的人话，由它们自己设）。
 */
char *dsh_text_uri_escape(const char *text);

/**
 * 百分号解码：把 `[begin, begin+len)` 解成一条 C 串，**解不动的原样留着**。
 *
 * @return 内核分配（调用方 `dsh_release`）；**内存不足回 NULL 且不设 last_error**（理由同上）。
 */
char *dsh_text_percent_decode(const char *begin, size_t len);

/**
 * 这个码位是不是「音节分隔点」。
 *
 * 认且**只认**四个：`·` U+00B7 / `‧` U+2027 / `・` U+30FB / 软连字符 U+00AD。
 *
 * ⚠️ **不许把连字符（`-` U+002D）算进来**：那会把 `well-known` 变成 `wellknown`，
 *    而它是词典里真实存在的词形。
 */
int dsh_text_is_separator_dot(uint32_t cp);

/** 文本里有没有分隔点 */
int dsh_text_has_separator_dots(const char *text);

/**
 * 去掉全部音节分隔点。
 *
 * @return 新分配的 UTF-8 串（调用方 `dsh_release`）；整段只有点 → **空串**（不是 NULL）；
 *         失败返回 NULL（last_error 已写好）
 *
 * ⚠️ **调用方必须遵守顺序**：先拿**原样**文本查一次，查不到才用这个返回值重查。
 *    内核不替你改语义 —— `ResolveKey` 永远只认原样文本。
 */
char *dsh_text_strip_separator_dots(const char *text);

/**
 * 按词计数：**词数 = 被 Unicode 空白分隔开的非空段数**。对中文这种不用空格分词的语言，
 * 它是「段数」而不是语言学意义上的词数 —— 它与「用户输入了几个词」这个界面语义对得上。
 */
int64_t dsh_text_count_words(const char *text);

/**
 * 按词计数，**中日韩逐字算一个词**。
 *
 * ⚠️ 与上面那个**不是同一套约定**，两个都留着是有意的（别合并）：
 *   · `dsh_text_count_words` = 空白分隔的**段数**（`text.analyze` 的 `wordCount` 用它）；
 *   · 这一个：拉丁文按空白切词、**中日韩字符逐字计数**（`苹果` = 2）。它只有**一个**用途
 *     —— 选区那条路「没查到」那句提示里「≤4 个词就把词写出来、否则只说『所选文本』」。
 *     中日韩不用空格分词，拿「段数」去判会把一整句话当成 1 个词、原样抄进提示里。
 *
 * 「什么算中日韩」由 `dsh_script_classify` 判（汉字区间只有一处来源）。
 */
int64_t dsh_text_count_words_cjk_aware(const char *text);

/** 整段只有一个字符（**按码点算**，不是按字节 —— 「中」 是 1 个字符、3 个字节） */
int dsh_text_is_single_char(const char *text);

/** 文本分析的原始结果（不含 JSON 那一层，方便别的模块直接取） */
typedef struct {
  dsh_script_counts scripts;
  const char *script;         /* `han` / `latin` / `other` */
  int has_separator_dots;
  int64_t word_count;
  int is_single_char;
} dsh_text_analysis;

/**
 * 算一遍文本分析（不分配、不出 JSON）。
 *
 * @param text  UTF-8 文本（NULL 当空串 —— 空串也有一套确定的结果）
 * @param out   出参
 */
void dsh_text_analyze_plain(const char *text, dsh_text_analysis *out);

#endif /* DSH_TEXT_TEXTUTIL_H */
