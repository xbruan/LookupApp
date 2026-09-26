/* ==========================================================================
 * Base64 解码（标准字母表 + `=` 填充 + 忽略空白）—— 内核承诺零依赖，所以自己写。
 * ⚠️ 必须忽略空白：SSE 每行后面都可能带 `\r`，而拼接约定是「把每段解出来的字节顺序接起来」。
 * ⚠️ 不认识的字符**如实失败**（返回非 0），不要跳过继续：坏音频交到播放器只是更难查。
 * ========================================================================== */

#ifndef DSH_TEXT_DSH_BASE64_H
#define DSH_TEXT_DSH_BASE64_H

#include <stddef.h>
#include <stdint.h>

/**
 * 解一段 base64。
 *
 * @param text    输入（UTF-8 / ASCII；空白与换行会被忽略）
 * @param len     字节数（**必须给**，段是从 SSE 行里切出来的，不一定以 \0 结尾）
 * @param out     出参：新分配的字节（调用方 `dsh_release`）；**没有内容时**写出一块长度为 0 的缓冲（不是 NULL）
 * @param out_len 出参：字节数
 * @return 0 成功；非 0 失败（last_error 已写，含出错位置）
 */
int dsh_base64_decode(const char *text, size_t len, uint8_t **out, size_t *out_len);

#endif /* DSH_TEXT_DSH_BASE64_H */
