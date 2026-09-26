/* RIPEMD-128：按原始论文实现，MDict 的 Encrypted=2 词典用它派生密钥。
 * 密钥错一个 bit 解出来就是垃圾，所以必须与参考实现逐位一致 —— 连「长度按
 * 32 位截断」的填充写法也照抄（标准是 64 位拆两半，两者只在 len ≥ 2^29 时不同）。
 * 摘要 = 4 个小端 32 位字；不做任何分配，len 为 0 时 data 可以是 NULL。 */

#ifndef DSH_CRYPTO_RIPEMD128_H
#define DSH_CRYPTO_RIPEMD128_H

#include <stddef.h>
#include <stdint.h>

/** 算一段数据的 RIPEMD-128 摘要；16 字节摘要写进 out_digest。
 * @param data 数据（len 为 0 时可以是 NULL）
 * @param len 字节数 · @param out_digest 出参：16 字节（4 个小端 32 位字）
 */
void dsh_ripemd128(const uint8_t *data, size_t len, uint8_t out_digest[16]);

#endif /* DSH_CRYPTO_RIPEMD128_H */
