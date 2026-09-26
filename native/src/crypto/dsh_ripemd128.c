/* RIPEMD-128：按原始论文实现，供 MDict 的 Encrypted=2 词典派生密钥用。
 * 它是密钥派生的地基，错一个 bit 解出来就是垃圾 —— 常数表、轮次顺序、填充
 * 与长度域都不许做「看起来等价」的化简；长度按 32 位截断那一处见头文件说明。 */

#include "crypto/dsh_ripemd128.h"

#include <string.h>

/* 循环左移位数：8 行 × 16 个 = 128 项，下标 = 16 * (t / 16) + t % 16 ——
 * 「第 t 步」直接查表，所以必须铺成 128 项（主轮前 64、并行轮后 64），不是 64 项。 */
static const unsigned char RIPEMD_S[128] = {
    11, 14, 15, 12, 5, 8, 7, 9, 11, 13, 14, 15, 6, 7, 9, 8,       /* round 1 */
    7, 6, 8, 13, 11, 9, 7, 15, 7, 12, 15, 9, 11, 7, 13, 12,       /* round 2 */
    11, 13, 6, 7, 14, 9, 13, 15, 14, 8, 13, 6, 5, 12, 7, 5,       /* round 3 */
    11, 12, 14, 15, 14, 15, 9, 8, 9, 14, 5, 6, 8, 6, 5, 12,       /* round 4 */
    8, 9, 9, 11, 13, 15, 15, 5, 7, 7, 8, 11, 14, 14, 12, 6,       /* parallel round 1 */
    9, 13, 15, 7, 12, 8, 9, 11, 7, 7, 12, 7, 6, 15, 13, 11,       /* parallel round 2 */
    9, 7, 15, 11, 8, 6, 6, 14, 12, 13, 5, 14, 13, 13, 7, 5,       /* parallel round 3 */
    15, 5, 8, 11, 14, 14, 6, 14, 6, 9, 12, 9, 12, 5, 15, 8,       /* parallel round 4 */
};

/* 消息字的取用顺序（同样 8 行 × 16 个 = 128 项） */
static const unsigned char RIPEMD_X[128] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,         /* round 1 */
    7, 4, 13, 1, 10, 6, 15, 3, 12, 0, 9, 5, 2, 14, 11, 8,         /* round 2 */
    3, 10, 14, 4, 9, 15, 8, 1, 2, 7, 0, 6, 13, 11, 5, 12,         /* round 3 */
    1, 9, 11, 10, 0, 8, 12, 4, 13, 3, 7, 15, 14, 5, 6, 2,         /* round 4 */
    5, 14, 7, 0, 9, 2, 11, 4, 13, 6, 15, 8, 1, 10, 3, 12,         /* parallel round 1 */
    6, 11, 3, 7, 0, 13, 5, 10, 14, 15, 8, 12, 4, 9, 1, 2,         /* parallel round 2 */
    15, 5, 1, 3, 7, 14, 6, 9, 11, 8, 12, 2, 10, 0, 4, 13,         /* parallel round 3 */
    8, 6, 4, 1, 3, 11, 15, 0, 5, 12, 2, 13, 9, 7, 10, 14,         /* parallel round 4 */
};

/* 每轮的加数：前 4 个是主轮，后 4 个是并行轮（并行轮的顺序是反的） */
static const uint32_t RIPEMD_K[8] = {
    0x00000000u, /* FF  */
    0x5a827999u, /* GG  */
    0x6ed9eba1u, /* HH  */
    0x8f1bbcdcu, /* II  */
    0x50a28be6u, /* III */
    0x5c4dd124u, /* HHH */
    0x6d703ef3u, /* GGG */
    0x00000000u, /* FFF */
};

/* 循环左移；与 31 兜底是为了 n == 0 时不做 x >> 32（C 里那是未定义行为，
 * S 表里没有 0，纯属保险）。 */
static uint32_t rotl32(uint32_t x, unsigned n) {
  return (x >> ((32u - n) & 31u)) | (x << (n & 31u));
}

static uint32_t read_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_le32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xffu);
  p[1] = (uint8_t)((v >> 8) & 0xffu);
  p[2] = (uint8_t)((v >> 16) & 0xffu);
  p[3] = (uint8_t)((v >> 24) & 0xffu);
}

/* 四个非线性函数（按 r & 3 选：主轮传 t/16、并行轮传 (63 - t%64)/16） */
static uint32_t f_round(unsigned r, uint32_t x, uint32_t y, uint32_t z) {
  switch (r & 3u) {
    case 0:
      return x ^ y ^ z;
    case 1:
      return (x & y) | (~x & z);
    case 2:
      return (x | ~y) ^ z;
    default:
      return (x & z) | (y & ~z);
  }
}

/* 压一个 64 字节块，把结果并回 hash[4] */
static void ripemd128_compress(uint32_t hash[4], const uint8_t *block) {
  uint32_t a = hash[0];
  uint32_t b = hash[1];
  uint32_t c = hash[2];
  uint32_t d = hash[3];
  uint32_t aa = a;
  uint32_t bb = b;
  uint32_t cc = c;
  uint32_t dd = d;
  unsigned t;

  /* 主轮 0..63 */
  for (t = 0; t < 64; ++t) {
    const unsigned r = t / 16;
    const uint32_t v = a + f_round(r, b, c, d) + read_le32(block + 4 * RIPEMD_X[16 * r + t % 16]) + RIPEMD_K[r];
    a = rotl32(v, RIPEMD_S[16 * r + t % 16]);
    const uint32_t tmp = d;
    d = c;
    c = b;
    b = a;
    a = tmp;
  }

  /* 并行轮 64..127：非线性函数与加数都按 (63 - t%64)/16 反着取 */
  for (; t < 128; ++t) {
    const unsigned r = t / 16;
    const unsigned rr = (63 - t % 64) / 16;
    const uint32_t v = aa + f_round(rr, bb, cc, dd) + read_le32(block + 4 * RIPEMD_X[16 * r + t % 16]) + RIPEMD_K[r];
    aa = rotl32(v, RIPEMD_S[16 * r + t % 16]);
    const uint32_t tmp = dd;
    dd = cc;
    cc = bb;
    bb = aa;
    aa = tmp;
  }

  /* 两条线在这里汇合：取的是**旧**的 hash 值，所以必须先算完四个结果再整体写入，
   * 一个一个写就会串味。 */
  const uint32_t next0 = hash[1] + c + dd;
  const uint32_t next1 = hash[2] + d + aa;
  const uint32_t next2 = hash[3] + a + bb;
  const uint32_t next3 = hash[0] + b + cc;
  hash[0] = next0;
  hash[1] = next1;
  hash[2] = next2;
  hash[3] = next3;
}

void dsh_ripemd128(const uint8_t *data, size_t len, uint8_t out_digest[16]) {
  uint32_t hash[4];
  hash[0] = 0x67452301u;
  hash[1] = 0xefcdab89u;
  hash[2] = 0x98badcfeu;
  hash[3] = 0x10325476u;

  /* 整块直接吃输入（不复制） */
  const size_t full_blocks = len / 64;
  size_t i;
  for (i = 0; i < full_blocks; ++i) {
    ripemd128_compress(hash, data + i * 64);
  }

  /* 尾巴补成一个或两个块：补到 56 或 120（mod 64），再接 8 字节长度 ——
   * 尾部总共 64 或 128 字节，栈上一块数组就够。 */
  const size_t rem = len % 64;
  const size_t pad_len = (rem < 56 ? 56 : 120) - rem;
  uint8_t tail[128];
  memset(tail, 0, sizeof tail);
  if (rem > 0) memcpy(tail, data + len - rem, rem);
  tail[rem] = 0x80;

  /* 长度域：低 4 字节是 (len * 8) 截断到 32 位，高 4 字节写那个截断值的 bit31。
   * 这是参考实现自己的写法（标准是 64 位拆两半），不许改成标准写法 —— 见头文件。 */
  const uint32_t bits = (uint32_t)(len << 3);
  write_le32(tail + rem + pad_len, bits);
  write_le32(tail + rem + pad_len + 4, bits >> 31);

  for (i = 0; i < (rem + pad_len + 8) / 64; ++i) {
    ripemd128_compress(hash, tail + i * 64);
  }

  /* 摘要 = 4 个小端 32 位字 */
  for (i = 0; i < 4; ++i) {
    write_le32(out_digest + i * 4, hash[i]);
  }
}
