/* 词典的内容哈希 id —— 硬决定：**id 认内容，不认路径**（用路径当身份时，挪目录 / 改名
 * 会让设置里跟这本词典绑定的东西**全部失联**，而磁盘上那本词典一个字都没变）。
 *
 * 约定：id = **整份文件内容的 SHA-256**，64 个小写十六进制；同名不同内容 = 两本词典，
 * 同内容不同名 = 同一本。⚠️ 必须算**整份**：只看头部会让两本不同词典**不报错地**撞成
 * 同一个 id，比用路径更坏。代价（一本 100MB 要读完）由「只在导入时算一次」抵消。 */

#ifndef DSH_DICT_ID_H
#define DSH_DICT_ID_H

#include "crypto/dsh_sha256.h"

#include <stddef.h>

/** id 的字符数（SHA-256 十六进制），以及缓冲要多大（含结尾 \0） */
#define DSH_DICT_ID_HEX_LEN 64
#define DSH_DICT_ID_BUF_LEN 65

/* 读文件的分块大小：64KB —— 大方块读盘与别占内存两头兼顾（与哈希算法无关）。 */
#define DSH_DICT_ID_CHUNK (64u * 1024u)

/**
 * 算一个文件的内容哈希 id。
 *
 * @param path    文件路径（UTF-8）
 * @param out_hex 出参：至少 DSH_DICT_ID_BUF_LEN 字节；成功时是 64 个小写十六进制 + \0
 * @return 0 成功；非零失败（打不开 / 读不动 / 空文件），原因写进 last_error。
 *
 * ⚠️ **空文件算失败**（不是「空内容的哈希」）：一本 0 字节的 .mdx 不是词典，
 *    给它一个合法 id 只会让「导入」把垃圾记成一本词库。
 */
int dsh_dict_id_of_file(const char *path, char out_hex[DSH_DICT_ID_BUF_LEN]);

/**
 * 算一段内存的内容哈希 id（测试与"给字符串取稳定键"用）。
 *
 * @param out_hex 出参：至少 DSH_DICT_ID_BUF_LEN 字节
 */
void dsh_dict_id_of_bytes(const void *data, size_t len, char out_hex[DSH_DICT_ID_BUF_LEN]);

#endif /* DSH_DICT_ID_H */
