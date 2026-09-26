/* ==========================================================================
 * DEFLATE（RFC 1951）解压 —— 内核自带，零依赖。
 *
 * 为什么自己写：接口定义承诺「内核零系统依赖」；MDict 的词块只用到**解压**（从不压缩）；
 * 而本仓库全部测试用词典的键信息块与词块都是 zlib 压的，所以这条路是解析器的**硬依赖**。
 *
 * 对齐的检查标准只有一个：**解出来的字节与 .NET 的 DeflateStream 逐字节相同**
 *
 * ========================================================================== */

#ifndef DSH_INFLATE_H
#define DSH_INFLATE_H

#include <stddef.h>
#include <stdint.h>

/**
 * 解压一段 zlib 流（含 2 字节头；末尾的 adler32 **不校验** —— 调用方另行校验）。
 *
 * @param input         zlib 数据（头 2 字节 + deflate 流）
 * @param input_len     字节数
 * @param expected_size 期望的解压后大小（MDict 的键信息块/词块都在头里记了这个数）。
 *                      传 0 表示未知，内部按 4 倍输入长度估初值、按需增长。
 * @param out_bytes     成功时写入新分配的缓冲（dsh_mem_alloc；调用方用 dsh_release 还给内核）
 * @param out_len       写入实际解出的字节数
 * @return 0 成功；非 0 失败（原因写进 dsh_set_last_error）
 */
int dsh_zlib_inflate(const uint8_t *input, size_t input_len, size_t expected_size,
                     uint8_t **out_bytes, size_t *out_len);

#endif /* DSH_INFLATE_H */
