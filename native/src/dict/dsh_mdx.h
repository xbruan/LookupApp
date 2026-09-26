/* ==========================================================================
 * MDict（.mdx / .mdd）解析器 —— 内核的内核。
 *
 * 这一层只做「把一个 MDict 文件读懂」，**不含任何产品约定**：不判语种、不做联想、
 * 不决定「查不到该怎么办」（那些在 engine / lookup 层）。
 *
 * 布局（v2.0）：头部长度(u32) → 头部正文(UTF-16LE XML) → 头部 adler32 → 键区头部
 *   → 键信息块 → 词块数据 → 记录区头部 → 记录信息块 → 记录块数据。
 *
 * 两条容易搞错的约定（都抄自参考实现，别自己发挥）：
 *   1. `numWidth`：v2.0 是 **8** 字节、v1.2 是 4；词条名的**长度字段**又只有
 *      `numWidth/4` 字节（v2.0 是 2 字节）。
 *   2. 词条名长度要 AdjustWordSize：v2.0 的长度里已经含了结尾的 \0（UTF-16 再 +2 字节），
 *      v1.2 不含。少这一步首尾词会整体串位，症状是「索引区间全错、什么词都查不到」。
 * ========================================================================== */

#ifndef DSH_MDX_H
#define DSH_MDX_H

#include <stddef.h>
#include <stdint.h>

/** 一个词块在键信息里的条目 */
typedef struct {
  int64_t entry_count;     /* 这一块有几个词条 */
  int64_t pack_size;       /* 压缩后字节数 */
  int64_t unpack_size;     /* 解压后字节数 */
  int64_t pack_offset;     /* 相对词块数据区的偏移（累加得到） */
  int64_t unpack_offset;   /* 相对全部解压后词块流的偏移（累加得到） */
  int64_t entry_offset;    /* 这是第几个词条（全局序，累加得到） */
  char *first_key;         /* UTF-8，已 TrimNul */
  char *last_key;          /* UTF-8，已 TrimNul */
} dsh_mdx_key_block;

/** 一个记录块在记录信息里的条目 */
typedef struct {
  int64_t pack_size;
  int64_t unpack_size;
  int64_t pack_offset;
  int64_t unpack_offset;
} dsh_mdx_record_block;

typedef struct dsh_mdx dsh_mdx;

/**
 * 打开一个 .mdx / .mdd。
 * @return 0 成功；非 0 失败（原因写进 dsh_set_last_error）
 */
int dsh_mdx_open(const char *path, dsh_mdx **out);

void dsh_mdx_close(dsh_mdx *mdx);

/* ── 元信息 ─────────────────────────────────────────────────────────────── */

const char *dsh_mdx_path(const dsh_mdx *mdx);
int dsh_mdx_is_mdd(const dsh_mdx *mdx);
/** 头部 GeneratedByEngineVersion；解析不出来返回 0（v1.2 分支就按 0 走） */
double dsh_mdx_version(const dsh_mdx *mdx);
/** 0=不加密，1=记录块加密，2=键信息块加密 */
int dsh_mdx_encrypted(const dsh_mdx *mdx);
/** 头里 Encoding 归一之后的名字：UTF-16 / UTF-8 / GB18030 / BIG5 */
const char *dsh_mdx_encoding_name(const dsh_mdx *mdx);
/** .mdx 头里的 Title（书名）；没有就是空串 */
const char *dsh_mdx_title(const dsh_mdx *mdx);
int64_t dsh_mdx_key_count(const dsh_mdx *mdx);
int64_t dsh_mdx_key_block_count(const dsh_mdx *mdx);
/** 相邻词块的首尾词在序数序上是否单调（不单调时查询要退化成全块扫描） */
int dsh_mdx_block_order_monotone(const dsh_mdx *mdx);
/** 打开过程中记下的诊断（最多 64 条）。没有就是 0。 */
int dsh_mdx_warning_count(const dsh_mdx *mdx);
const char *dsh_mdx_warning_at(const dsh_mdx *mdx, int index);

/* ── 键 ─────────────────────────────────────────────────────────────────── */

int64_t dsh_mdx_key_block_at(const dsh_mdx *mdx, int64_t index, dsh_mdx_key_block *out);

/** 记录块索引（逐字节对照与诊断用）：解压后大小与累加偏移决定了记录的字节范围。 */
int64_t dsh_mdx_record_block_count(const dsh_mdx *mdx);
/** 全部记录块的解压后大小之和（= 记录区头里记的 TotalRecordUnpackedSize） */
int64_t dsh_mdx_total_record_unpacked(const dsh_mdx *mdx);

/**
 * 一条记录在**记录区内**的字节范围 [start, end)。
 * 逐字节对照用：C# 那边是同一条规则（下一条的偏移、跨块取块首条、最后一条取总解压大小）。
 * @return 0 成功 / -1 出错（原因写进 last_error）
 */
int dsh_mdx_record_range(dsh_mdx *mdx, int64_t global_index, int64_t *out_start, int64_t *out_end);

/** 取记录区某个字节区间解出来的 UTF-8 文本（按当前编码）；跨块时自动拼接。
 *  这是逐字节对照与诊断的底层入口 —— `dsh_mdx_read_record` 就是它的一层便捷包装。 */
int dsh_mdx_read_record_bytes(dsh_mdx *mdx, int64_t start, int64_t end, char **out_text,
                              int64_t *out_len);

/**
 * 精确查一个键（先按原样、再按去首尾空白，与参考实现一致）。
 * 命中时写 *out_key（词典里的规范键名，调用方用 dsh_release 还给内核）与 *out_block_index。
 * @return 1 命中 / 0 没有这个键 / -1 出错（原因写进 last_error）
 */
int dsh_mdx_lookup_key(dsh_mdx *mdx, const char *key, char **out_key, int64_t *out_block_index);

/**
 * 取某个词块的**原始内容**（已解压、未解码成 UTF-8）。
 * 每词条 = {记录偏移(numWidth 字节) + 键名 + \0}，**没有长度字段**（UTF-16LE / .mdd 是
 * 两字节的 00 00）；⚠️ 别照键信息块把「长度」当第一个字段。
 * 调用方用 dsh_release 还给内核。
 */
int dsh_mdx_read_key_block(dsh_mdx *mdx, int64_t block_index, uint8_t **out_bytes, size_t *out_len);

/**
 * 枚举全部键名（UTF-8 数组，调用方用 dsh_release 还给内核）。
 * 这是逐字节对照与诊断的主力接口：拿它跟参考实现 / js-mdict 逐条比。
 */
int dsh_mdx_list_keys(dsh_mdx *mdx, char ***out_keys, int64_t *out_count);

/**
 * 取**某一个词块**里的键名（UTF-8 数组，调用方用 `dsh_mdx_free_keys` 还给内核）。
 *
 * 为什么不做成「list_keys 再切片」：那要先把整本书的键物化出来（几十万条就是几兆内存），
 * 而「取样」要的只是第 N 块里那几条（用 `entry_offset` / `entry_count` 就能定位）。
 *
 * @return 0 成功（**这一块一个键都没有也算成功**）；-1 失败（last_error 已写，
 *         下标越界也走这一档）
 */
int dsh_mdx_block_keys(dsh_mdx *mdx, int64_t block_index, char ***out_keys, int64_t *out_count);

/** 释放 dsh_mdx_list_keys 给出的数组（含每一条字符串） */
void dsh_mdx_free_keys(char **keys, int64_t count);

/**
 * 取某条记录的内容（UTF-8）。
 * @param global_index 词条的全局序（0 起）
 */
int dsh_mdx_read_record(dsh_mdx *mdx, int64_t global_index, char **out_text, int64_t *out_len);

/**
 * 取一条记录的**原始字节**（不解码）。
 *
 * ⚠️ 资源（`.mdd` 里的图片 / 音频 / 字体）必须走这一条：`.mdd` 的文件编码是 UTF-16LE，
 *    而资源是**二进制** —— 按文本解一遍就毁了（`.mdd` 里的图片与音频全读不出来，
 *    而 CSS 那种纯文本看着「没坏」）。参考实现里这也是两个入口：MdxReader.Lookup 与
 *    MddReader.Locate → GetRecordRaw。
 */
int dsh_mdx_read_record_raw(dsh_mdx *mdx, int64_t global_index, uint8_t **out_bytes,
                            size_t *out_len);

/** 按键取**原始字节**（资源用；文本用 `dsh_mdx_fetch`）。hit 语义同 `dsh_mdx_fetch`。 */
int dsh_mdx_fetch_raw(dsh_mdx *mdx, const char *key, char **out_key, uint8_t **out_bytes,
                      size_t *out_len);

/**
 * 通过键直接取这一条的内容（UTF-8）—— 把「查键 → 算全局序 → 读记录」三步收在一处。
 *
 * ⚠️ 别让调用方自己串这三步：命中块里的**块内下标**要加上该块的 `entry_offset` 才是全局序，
 * 而块内下标只有在解析词块时才知道 —— 自己算就得把词块的记录格式再实现一遍。
 *
 * @param out_key  命中时写回词典里的规范键名（调用方用 dsh_release 还给内核）
 * @return 1 命中 / 0 没有这个键 / -1 出错
 */
int dsh_mdx_fetch(dsh_mdx *mdx, const char *key, char **out_key, char **out_text,
                  int64_t *out_len);

/**
 * 前缀查询：把所有**以 prefix 开头**的键名按序数序列出来（最多 max_count 条）。
 *
 * 词块索引里存了每块的**首词/尾词**，所以先用二分定位起点块、再顺着往后扫，遇到第一个
 * 不再以 prefix 开头的键就停 —— 20 万词条的词典查 `app` 只走 `app*` 那一小段。
 * 联想是**热路径**（每敲一个字都要跑），这个差别很实在。
 *
 * @param out_keys  出参：新分配的指针数组（调用方用 dsh_mdx_free_keys 还给内核）
 * @param out_count 出参：实际找到几个（<= max_count）
 * @return 0 成功（**一个都没找到也算成功**，`*out_count = 0` 且 `*out_keys = NULL`）；
 *         非零失败
 */
int dsh_mdx_prefix_search(dsh_mdx *mdx, const char *prefix, int64_t max_count, char ***out_keys,
                          int64_t *out_count);

#endif /* DSH_MDX_H */
