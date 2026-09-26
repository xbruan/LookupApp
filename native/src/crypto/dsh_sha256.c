/* SHA-256（FIPS 180-4）的实现；调用约定见 dsh_sha256.h。
 * 所有算术都在 uint32_t 上做、靠显式移位拆字节，不碰任何主机字节序假设 ——
 * 同一份代码在大端机器上也不用改，别在这里埋一个只在 x86 上对的假设。 */
#include "crypto/dsh_sha256.h"

#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void compress(dsh_sha256_ctx *ctx, const uint8_t block[64]) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++) {
    w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
           ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
  }
  for (int i = 16; i < 64; i++) {
    const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
  uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
  for (int i = 0; i < 64; i++) {
    const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const uint32_t ch = (e & f) ^ ((~e) & g);
    const uint32_t t1 = h + S1 + ch + K[i] + w[i];
    const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t t2 = S0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  ctx->state[0] += a;
  ctx->state[1] += b;
  ctx->state[2] += c;
  ctx->state[3] += d;
  ctx->state[4] += e;
  ctx->state[5] += f;
  ctx->state[6] += g;
  ctx->state[7] += h;
}

void dsh_sha256_init(dsh_sha256_ctx *ctx) {
  if (ctx == NULL) return;
  ctx->state[0] = 0x6a09e667u;
  ctx->state[1] = 0xbb67ae85u;
  ctx->state[2] = 0x3c6ef372u;
  ctx->state[3] = 0xa54ff53au;
  ctx->state[4] = 0x510e527fu;
  ctx->state[5] = 0x9b05688cu;
  ctx->state[6] = 0x1f83d9abu;
  ctx->state[7] = 0x5be0cd19u;
  ctx->bitlen = 0;
  ctx->buflen = 0;
  memset(ctx->buf, 0, sizeof(ctx->buf));
}

void dsh_sha256_update(dsh_sha256_ctx *ctx, const void *data, size_t len) {
  if (ctx == NULL || len == 0) return;
  const uint8_t *p = (const uint8_t *)data;
  if (p == NULL) return;
  ctx->bitlen += (uint64_t)len * 8u;
  /* 先把上次剩下的补齐一块，再整块整块地吃，最后把尾巴留下 */
  while (len > 0) {
    const size_t space = 64u - ctx->buflen;
    const size_t take = (len < space) ? len : space;
    memcpy(ctx->buf + ctx->buflen, p, take);
    ctx->buflen += take;
    p += take;
    len -= take;
    if (ctx->buflen == 64u) {
      compress(ctx, ctx->buf);
      ctx->buflen = 0;
    }
  }
}

void dsh_sha256_final(dsh_sha256_ctx *ctx, uint8_t out_digest[32]) {
  if (ctx == NULL || out_digest == NULL) return;
  /* 填充：一个 0x80，补 0 到 56 mod 64，最后 8 字节大端比特长度。
   * ⚠️ 长度必须在**填第一个字节之前**就抄下来：update 会把填充字节也算进
   *    ctx->bitlen，而尾 8 字节要写的是**消息本身**的长度。 */
  const uint64_t bitlen = ctx->bitlen;
  const uint8_t pad = 0x80u;
  dsh_sha256_update(ctx, &pad, 1);
  { /* while 循环只对 buflen 负责，len==1 的 update 不会一次跨过 56 */
    const uint8_t zero = 0;
    while (ctx->buflen != 56u) {
      dsh_sha256_update(ctx, &zero, 1);
    }
  }
  uint8_t tail[8];
  for (int i = 0; i < 8; i++) {
    tail[i] = (uint8_t)((bitlen >> (56 - i * 8)) & 0xFFu);
  }
  dsh_sha256_update(ctx, tail, 8);

  for (int i = 0; i < 8; i++) {
    out_digest[i * 4] = (uint8_t)((ctx->state[i] >> 24) & 0xFFu);
    out_digest[i * 4 + 1] = (uint8_t)((ctx->state[i] >> 16) & 0xFFu);
    out_digest[i * 4 + 2] = (uint8_t)((ctx->state[i] >> 8) & 0xFFu);
    out_digest[i * 4 + 3] = (uint8_t)(ctx->state[i] & 0xFFu);
  }
}

void dsh_sha256_hex(const void *data, size_t len, char out_hex[65]) {
  static const char digits[] = "0123456789abcdef";
  if (out_hex == NULL) return;
  dsh_sha256_ctx ctx;
  dsh_sha256_init(&ctx);
  dsh_sha256_update(&ctx, data, len);
  uint8_t digest[32];
  dsh_sha256_final(&ctx, digest);
  for (int i = 0; i < 32; i++) {
    out_hex[i * 2] = digits[(digest[i] >> 4) & 0xFu];
    out_hex[i * 2 + 1] = digits[digest[i] & 0xFu];
  }
  out_hex[64] = '\0';
}
