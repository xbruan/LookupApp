/* ==========================================================================
 * 文本编解码层 —— 词典里那四种 Encoding 到内核统一编码（UTF-8）的转换。
 *
 * MDict 的词条与正文按头部 Encoding 属性编码，实测四类：UTF-16LE（默认）、UTF-8、
 * GBK/GB2312（按 GB18030 解）、big5。纯 C11 零依赖的内核里没有 .NET 那两张码表，
 * 所以自己带（`dsh_textcodec_tables.c`，由 tools/make-textcodec-tables.py 生成，不许手抄）。
 *
 * 调用约定：
 *   · 解码结果用 dsh_mem_alloc 分配，**调用方用 dsh_release 还给内核**；
 *   · 成功时 *out_text 非 NULL（长度为 0 时是空串），*out_len 不含结尾的 \0；缓冲保证以
 *     \0 结尾，但**正文里可能有内嵌的 U+0000** —— 当真值请一律用 *out_len；
 *   · 失败（非零）时 *out_text 置 NULL、*out_len 置 0，并留下人话原因。
 *
 * ⚠️ 最容易被后人改错的一条：**坏字节不算失败**
 *   非法序列、残缺序列、GB18030 的四字节区**都不让 dsh_text_decode 失败**：每个坏字节
 *   产出一个 U+FFFD 继续往下解（四字节区整段一个）。只有「宿主传错参数」与「内存不够」
 *   才非零返回 —— 文件里有坏字节与内核解不了是两件事，前者不该让整本词典打不开。
 *   解出过替换字符照样记进 last_error 供诊断，**但返回 0**：想知道有没有解不干净的东西
 *   看 last_error，不要看返回值。⚠️ 「每个坏字节」是字面意思：UTF-8 末尾残缺的 E6 B5 给
 *   **两个** U+FFFD，而浏览器 TextDecoder 的「最长大子串」约定只给一个 —— 本层按前者。
 *
 * 另外两条约定（别自己发明）：
 *   · 开头的 BOM 会被吃掉（UTF-8 的 EF BB BF、UTF-16LE 的 FF FE），与 TextDecoder 的默认
 *     行为一致 —— 所以**别把一条记录的中段切出来单独解码**（那会吃掉本该保留的 U+FEFF），
 *     按记录整段解。
 *   · `.mdd` 一律按 UTF-16LE —— 但那个判断**不在这一层**，由调用方强制传 DSH_TXT_UTF16LE。
 * ========================================================================== */

#ifndef DSH_TEXT_CODEC_H
#define DSH_TEXT_CODEC_H

#include <stddef.h>
#include <stdint.h>

/** 词典头部 Encoding 属性对应的四种编码（内核内部一律存 UTF-8）。 */
typedef enum {
  DSH_TXT_UTF8 = 0,    /**< 原样校验 / 透传（认不出来的名字也走这条） */
  DSH_TXT_UTF16LE = 1, /**< 小端 UTF-16 */
  DSH_TXT_GB18030 = 2, /**< 头里写 GBK / GB2312 也走这条（GBK 是 GB18030 的子集） */
  DSH_TXT_BIG5 = 3     /**< 繁体 Big5 */
} dsh_text_encoding;

/**
 * 头部那个 Encoding 属性字符串 → 枚举。
 *
 * 大小写不敏感，中间的连字符与下划线也忽略：
 *   GBK / GB2312 / GB18030        → DSH_TXT_GB18030
 *   big5                          → DSH_TXT_BIG5
 *   utf16 / utf-16 / utf-16le     → DSH_TXT_UTF16LE
 *   NULL、空串、utf8、以及其它认不出来的名字 → DSH_TXT_UTF8
 *
 * 注：比参考实现多认一个 gb18030（它在那一档会退回 UTF-8）—— 有意的宽松：按 UTF-8 去解
 * 一份 GB18030 词典只会得到乱码。
 */
dsh_text_encoding dsh_text_encoding_from_name(const char *name);

/**
 * 把一段字节按指定编码解成 UTF-8。
 *
 * @param enc      源编码（枚举值；0–3 之外视为调用方 bug，返回非零）
 * @param bytes    源字节（可为 NULL，此时 len 必须为 0）
 * @param len      源字节数
 * @param out_text 出参：新分配的 UTF-8 缓冲（以 \0 结尾）
 * @param out_len  出参：UTF-8 字节数（不含结尾的 \0）
 * @return 0 成功；非零失败（出参为空、入参自相矛盾、内存不足）。
 *
 * ⚠️ 再说一遍：**内容里有坏字节不算失败**（每个坏字节一个 U+FFFD，并记进 last_error），
 * 只有参数错误与 OOM 才非零。
 */
int dsh_text_decode(dsh_text_encoding enc, const uint8_t *bytes, size_t len,
                    char **out_text, size_t *out_len);

#endif /* DSH_TEXT_CODEC_H */
