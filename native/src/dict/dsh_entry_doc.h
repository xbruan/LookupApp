/* 词条正文文档：产出一个**自包含的 HTML 文档**，宿主直接赋给跨源 iframe 的 `src`。资源协议
 * `https://<词典 id>.dictres.invalid/`（`.invalid` 永不解析）由宿主在**发出网络请求之前**就地
 * 应答；id 放在 **host** 位置，词条里 `/images/a.png` 这类根相对路径经 `<base>` 解析后仍
 * 落在同一本词典上。⚠️ 文档的头与桥接脚本必须在同一处产生，那两段资产由
 * `tools/make-entry-assets.ps1` 生成到 `entry_assets.h`，**不许手抄**。 */

#ifndef DSH_ENTRY_DOC_H
#define DSH_ENTRY_DOC_H

#include <stddef.h>
#include <stdint.h>

/** `https://<词典 id>.dictres.invalid`（新分配，调用方 `dsh_release`） */
char *dsh_entry_doc_origin(const char *dict_id);

/** `https://<词典 id>.dictres.invalid/`（新分配，调用方 `dsh_release`） */
char *dsh_entry_doc_base(const char *dict_id);

/** 词条正文文档的地址（直接赋给 `iframe.src`）：
 * `https://<词典 id>.dictres.invalid/__entry__?word=<转义后的键名>`
 * 路径放在 host 上，所以资源路径不会与它冲突（与参考实现同一条）。 */
char *dsh_entry_doc_entry_url(const char *dict_id, const char *word);

/**
 * 拼一份完整的词条正文。
 *
 * @param dict_id       词典 id（内容哈希）
 * @param definition    词条正文（**可信 HTML**，词典里那一段原文）
 * @param definition_len 正文长度（**带长度**：正文里可能有内嵌 U+0000）
 * @param notice        未命中 / 出错的提示（**可信 HTML**，由内核生成；非空时**顶替**正文）
 * @param has_resources 这本词典有没有资源（写进 `data-has-resources`）
 * @param out_len       出参：文档字节数（末尾那个 `\0` 不算）
 * @return 新分配的 UTF-8 文档（调用方 `dsh_release`）；失败回 NULL 并写 last_error
 */
char *dsh_entry_doc_build(const char *dict_id, const char *definition, size_t definition_len,
                          const char *notice, int has_resources, size_t *out_len);

/** 从词条正文里抠出全部音频键（`sound://` / `snd://` 与 `<audio name=…>`）。
 *
 * 与参考实现同一条约定：**按出现顺序、去重（大小写不敏感）**，每个键都过一遍
 * `Normalize`（URL 解码 + 去掉前导斜杠 + trim）。⚠️ 顺序有意义：认不出英 / 美时按
 * 「先出现的那个」挑（那是 `speech` 组的事）。
 *
 * @param out_keys 出参：新分配的**指针数组**（调用方 `dsh_release` 数组与每一项）
 * @param out_count 出参：抠出几个
 * @return 0 成功（一个都没有也算成功：`*out_count = 0`、`*out_keys = NULL`）
 */
int dsh_entry_doc_audio_keys(const char *html, size_t len, char ***out_keys, int64_t *out_count);

/** 释放 `dsh_entry_doc_audio_keys` 给出来的清单（每一项与数组本身都 `dsh_release`）*/
void dsh_entry_doc_audio_keys_free(char **keys, int64_t count);

/** RFC 3986 的转义（与参考实现同一条约定：unreserved 之外一律 `%XX`）。
 *
 * 为什么单独给出来：未命中提示页里那排候选链接要写成 `entry://<转义后的词>`
 * （桥接脚本按这个前缀拦截），而「怎么转义一个词」只许有一处实现。 */
char *dsh_entry_doc_uri_escape(const char *text);

#endif /* DSH_ENTRY_DOC_H */
