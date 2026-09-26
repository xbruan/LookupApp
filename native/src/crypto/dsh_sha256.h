/* SHA-256（FIPS 180-4）—— 词典的**内容哈希 id** 用它，也留给将来核对资源卷。
 * 流式（update 多次 + final 一次）是为了直接吃映射进来的分块：词典动辄上百 MB，
 * 不能先拷一份到堆上 —— 所以它一个 malloc 都不做，这一层没有失败模式（返回 void）。
 * 与 dsh_ripemd128 不许互相替代：那个是 MDict 加密键块必须用的，这个是自己选来标识词典的。
 * 调用约定：init → update* → final 的顺序；final 之后 ctx 就废了（除非重新 init）。 */

#ifndef DSH_CRYPTO_SHA256_H
#define DSH_CRYPTO_SHA256_H

#include <stddef.h>
#include <stdint.h>

/** 流式上下文（大小与内容由实现决定，调用方只当它是不透明的一块） */
typedef struct {
  uint32_t state[8];
  uint64_t bitlen;
  uint8_t buf[64];
  size_t buflen;
} dsh_sha256_ctx;

/** 初始化（等价于「还没有喂任何字节」） */
void dsh_sha256_init(dsh_sha256_ctx *ctx);

/** 喂一段数据（可以多次调用，等价于把它们接起来一次喂）。
 * @param data 数据；len 为 0 时可以是 NULL（空串也是合法输入）
 * @param len 字节数 */
void dsh_sha256_update(dsh_sha256_ctx *ctx, const void *data, size_t len);

/** 收尾，把 32 字节摘要写进 out_digest（**不做任何分配**）。
 * 调用之后 ctx 不可再用（除非重新 init）。 */
void dsh_sha256_final(dsh_sha256_ctx *ctx, uint8_t out_digest[32]);

/** 一段内存的 SHA-256 十六进制摘要（小写，64 个字符 + 结尾 \0）。
 * @param out_hex 出参：**至少 65 字节** */
void dsh_sha256_hex(const void *data, size_t len, char out_hex[65]);

#endif /* DSH_CRYPTO_SHA256_H */
