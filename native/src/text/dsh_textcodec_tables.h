/* GENERATED — DO NOT EDIT
 * 本文件由脚本生成，手改会在下一次生成时被覆盖。
 * 生成命令：python3 tools/make-textcodec-tables.py
 * 码表来源：python3 的 gb18030 / big5 编解码器（逐码位 bytes([hi,lo]).decode(...)）
 * 约定：只收 BMP（u16，超出 BMP 的槽一律填 0）；不可映射的槽填 0，由解码器转成 U+FFFD。
 */
#ifndef DSH_TEXTCODEC_TABLES_H
#define DSH_TEXTCODEC_TABLES_H
#include <stddef.h>
#include <stdint.h>
/* ── GB18030 双字节区（GBK 是它的子集）────────────────────────────────
 * 首字节 0x81–0xFE（126 行）、次字节 0x40–0xFE（191 列，含恒不可映射的 0x7F）。
 * 下标 = (首 - DSH_GBK_LEAD_FIRST) * DSH_GBK_TRAIL_COUNT + (次 - DSH_GBK_TRAIL_FIRST)
 * 注意：次字节 0x30–0x39 属于 GB18030 的四字节区，本版不支持，
 *       解码器在那之前就拦下来了，不会走到这张表。 */
#define DSH_GBK_LEAD_FIRST 129
#define DSH_GBK_LEAD_LAST  254
#define DSH_GBK_TRAIL_FIRST 64
#define DSH_GBK_TRAIL_COUNT 191
#define DSH_GBK_TRAIL_LAST  (DSH_GBK_TRAIL_FIRST + DSH_GBK_TRAIL_COUNT - 1)
#define DSH_GBK_TABLE_SIZE 24066
#define DSH_GBK_MAPPED 23940
#define DSH_GBK_INDEX(lead, trail) \
  ((size_t)((lead) - DSH_GBK_LEAD_FIRST) * DSH_GBK_TRAIL_COUNT + \
   (size_t)((trail) - DSH_GBK_TRAIL_FIRST))

extern const uint16_t dsh_gbk_bmp[DSH_GBK_TABLE_SIZE];

/* ── Big5 ────────────────────────────────────────────────────────────
 * 首字节 0xA1–0xF9（89 行）；次字节只有 0x40–0x7E 与 0xA1–0xFE 两段有效，
 * 中间 0x7F–0xA0 恒不可映射（在表里就是 0）。 */
#define DSH_BIG5_LEAD_FIRST 161
#define DSH_BIG5_LEAD_LAST  249
#define DSH_BIG5_TRAIL_FIRST 64
#define DSH_BIG5_TRAIL_COUNT 191
#define DSH_BIG5_TRAIL_LAST  (DSH_BIG5_TRAIL_FIRST + DSH_BIG5_TRAIL_COUNT - 1)
#define DSH_BIG5_TABLE_SIZE 16999
#define DSH_BIG5_MAPPED 13710
#define DSH_BIG5_INDEX(lead, trail) \
  ((size_t)((lead) - DSH_BIG5_LEAD_FIRST) * DSH_BIG5_TRAIL_COUNT + \
   (size_t)((trail) - DSH_BIG5_TRAIL_FIRST))

extern const uint16_t dsh_big5_bmp[DSH_BIG5_TABLE_SIZE];

#endif /* DSH_TEXTCODEC_TABLES_H */
