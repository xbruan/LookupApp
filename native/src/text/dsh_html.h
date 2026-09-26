/* ==========================================================================
 * 极小的 HTML 文本工具（对应参考实现的 HtmlUtils）——「正文里什么算文本」是业务逻辑，
 * 必须在 C 内核里，界面不许自己再写一份。
 * 进出都带长度（不是 C 字符串）：正文里**可能有内嵌的 U+0000**（`&#0;` 解码出来就是），
 * `char *` 会在那里不报错地截断。
 * ⚠️ `&#xD800;`（孤立代理项）参考实现会抛异常；C 版**如实保留原文**（不崩、也不给出半个错字）。
 * ========================================================================== */

#ifndef DSH_HTML_H
#define DSH_HTML_H

#include <stddef.h>

/** 把词条 HTML 转成便于复制的纯文本（去脚本/样式、块级标签换行、实体还原、空白与空行收敛、
 *  逐行 trim）。出参由内核分配，用 `dsh_release` 还给内核。 */
char *dsh_html_strip(const char *html, size_t len, size_t *out_len);

/** 只做实体还原（`&amp;` / `&#65;` / `&#x4e2d;`；认不出来的原样留着） */
char *dsh_html_decode_entities(const char *text, size_t len, size_t *out_len);

/** HTML 转义（`&<>` 以及半角引号）—— 把词典原文塞进我们拼的 HTML 时用 */
char *dsh_html_escape(const char *text, size_t len, size_t *out_len);

/** 防止词典内容里的 `</script>` 把注入脚本截断（`</script` → `<\/script`） */
char *dsh_html_escape_for_inline_script(const char *text, size_t len, size_t *out_len);

#endif /* DSH_HTML_H */
