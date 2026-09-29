/* ==========================================================================
 * 内核单元测试 · RIPEMD-128（src/crypto/dsh_ripemd128.c）
 *
 * 检查标准分两半，两边都必须过：
 *   ① **公开测试向量**：官方 RIPEMD-160 页面里 RIPEMD-128 那一列
 *      （https://homes.esat.kuleuven.be/~bosselae/ripemd160.html），逐字抄来 ——
 *      不用自己造的期望值，另外补了表里额外给的两条与 100 万个 a；
 *   ② **与 C# 参考实现对照**（参考实现的 Ripemd128.cs，即 js-mdict 的 ripemd128.js）：
 *      不在公开向量里的期望值（长度边界那 17 条、中文串、MDict 密钥）就是那一版的输出。
 *
 * ⚠️ 100 万个 a 的 RIPEMD-128 是 4a7f5723f954eba1216c9d8f6320431f。
 *    常见的 52783243c1697bdbe16d37f97f68f083 是 **RIPEMD-160** 的结果被截了前 16 字节
 *    （官方表里那一格完整值是 52783243c1697bdbe16d37f97f68f08325dc1528），
 *    不是 RIPEMD-128 —— 这两个值由「官方表 + C# 参考实现」两条独立来源核对过。
 *
 * 不用任何测试框架（本仓库不引框架），失败非零退出，能直接给脚本用。
 * ========================================================================== */

#include "crypto/dsh_ripemd128.h"

#include "dsh_internal.h"
#include "mem_registry.h"

#include <stdio.h>
#include <string.h>

static int g_checks = 0;
static int g_failed = 0;

static void ok(int condition, const char *what) {
  g_checks++;
  if (!condition) {
    g_failed++;
    fprintf(stderr, "FAIL %s\n", what);
  }
}

/** size_t 版的相等断言 —— 直接拿 size_t 与 int 比会触发 -Wsign-compare（-Werror 下即错误） */
static void ok_eq_size(size_t actual, size_t expected, const char *what) {
  g_checks++;
  if (actual != expected) {
    g_failed++;
    fprintf(stderr, "FAIL %s：实际=%zu 期望=%zu\n", what, actual, expected);
  }
}

/* 摘要转成小写十六进制（32 字符 + 结尾），失败信息里直接给人看 */
static void digest_to_hex(const uint8_t digest[16], char out[33]) {
  static const char *const digits = "0123456789abcdef";
  for (int i = 0; i < 16; i++) {
    out[2 * i] = digits[digest[i] >> 4];
    out[2 * i + 1] = digits[digest[i] & 0x0f];
  }
  out[32] = '\0';
}

/** 算一遍并与期望的十六进制串比。这里钉的是**摘要本身**，不是别的替代指标。 */
static void ok_digest(const uint8_t *data, size_t len, const char *expected_hex, const char *what) {
  uint8_t digest[16];
  char hex[33];
  dsh_ripemd128(data, len, digest);
  digest_to_hex(digest, hex);
  g_checks++;
  if (strcmp(hex, expected_hex) != 0) {
    g_failed++;
    fprintf(stderr, "FAIL %s：实际=%s 期望=%s\n", what, hex, expected_hex);
  }
}

/* ── 检查标准一：公开测试向量（官方表里 RIPEMD-128 那一列）─────────────────
 * 表里的三条「a...z」「abcdbcde...」「A...Za...z0...9」在这里写全。 */
typedef struct {
  const char *text;
  const char *digest;
  const char *what;
} string_vector;

static const string_vector kStringVectors[] = {
    {"", "cdf26213a150dc3ecb610f18f6b38b46", "空串：RIPEMD-128 的公开向量"},
    {"a", "86be7afa339d0fc7cfc785e72f578d33", "「a」：RIPEMD-128 的公开向量"},
    {"abc", "c14a12199c66e4ba84636b0f69144c77", "「abc」：RIPEMD-128 的公开向量"},
    {"message digest", "9e327b3d6e523062afc1132d7df9d1b8", "「message digest」：公开向量"},
    {"abcdefghijklmnopqrstuvwxyz", "fd2aa607f71dc8f510714922b371834e", "「a…z」：公开向量"},
    {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
     "a1aa0689d0fafa2ddc22e88b49133a06", "「abcdbcde…nopq」：公开向量"},
    {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
     "d1e959eb179c911faea4624c60c5c702", "「A…Za…z0…9」：公开向量"},
    {"12345678901234567890123456789012345678901234567890123456789012345678901234567890",
     "3f45ef194732c2dbb2c4a2c769795fa3", "「1234567890」重复八次：公开向量"},
};

static void test_published_vectors(void) {
  const size_t n = sizeof kStringVectors / sizeof kStringVectors[0];
  for (size_t i = 0; i < n; i++) {
    const char *text = kStringVectors[i].text;
    ok_digest((const uint8_t *)text, strlen(text), kStringVectors[i].digest, kStringVectors[i].what);
  }

  /* 空表的另一种写法：data 传 NULL、长度 0 也必须算出同一个摘要 */
  ok_digest(NULL, 0, "cdf26213a150dc3ecb610f18f6b38b46",
            "data=NULL 且长度 0：必须与空串同值（这条路不许解引用空指针）");

  /* UTF-8 多字节（中文）也要跟着走一遍 */
  const char *chinese = "MDict 对照测试：中文词条 / mixed ASCII";
  ok_digest((const uint8_t *)chinese, strlen(chinese), "5535f4073759bdd88ecf7ff5cbfac25a",
            "中文串（UTF-8 多字节）：与 C# 参考实现的输出一致");

  /* 一百万个 a —— 这条钉的是「跨很多个块」的累加路径。
   * ⚠️ 期望值取自官方表（见文件顶部那段说明），不是 RIPEMD-160 那个截断值。 */
  const size_t million = 1000000;
  uint8_t *buf = (uint8_t *)dsh_mem_alloc(million);
  ok(buf != NULL, "这一条要先分得出一兆字节（环境问题）");
  if (buf != NULL) {
    memset(buf, 'a', million);
    ok_digest(buf, million, "4a7f5723f954eba1216c9d8f6320431f",
              "一百万个 a：公开向量（RIPEMD-128，不是 RIPEMD-160 的截断）");
    dsh_release(buf);
  }
}

/* ── 检查标准二：长度边界（与 C# 参考实现逐条对照）──────────────────────────
 * 填充那两条路（补到 56 还是 120）只在特定长度上分叉，所以挑的是边界两侧：
 * 0 / 1 / 2 / 3、55…57、63…65、119…121、127…129、200。
 * 输入是固定的 200 字节模式（第 i 字节 = (i * 7 + 13) & 0xff）。 */
typedef struct {
  size_t size;
  const char *digest;
} size_vector;

static const size_vector kSizeVectors[] = {
    {0, "cdf26213a150dc3ecb610f18f6b38b46"},
    {1, "dd59cd21c18be542ecc784318dd85b88"},
    {2, "462ade5d0936ccef9cf1da54ac8279b6"},
    {3, "8542c595eae488060274816c86ecd1cc"},
    {55, "111e8bcb3ba426843baa1bcceb2bc39b"},   /* 尾部还差 1 字节就到 56 → 补一块 */
    {56, "d83e1bb8ffbca577b0d2b1979599eb35"},   /* 刚好 56 → 必须补两块 */
    {57, "4cb39b7eb63978bb371f77a85fa26648"},
    {63, "21f93af2b1571d189ff466a0613259b5"},   /* 差 1 字节整块 */
    {64, "75caf8109550e096f298b76c8fdd6af5"},   /* 整块：填充要到下一个块的 56 */
    {65, "ce99c5ae866f4612ca1f538450ade0a5"},
    {119, "5605be488d52fde0d03f579987ad68db"},
    {120, "8be1712254761a79b89a4790a1fe711b"},  /* 第二个边界：56 + 64 */
    {121, "f0a6451b3bf1a1e9084714ef9bf98280"},
    {127, "c1829a5c0d0745a30b33418c6aed0cd6"},
    {128, "cad1bba037a7faa186ecd9b203f288ff"},
    {129, "97cfe2faa5a8fbe880ca6ded1ad96aa1"},
    {200, "b983ff8a0af324fa18bbbacdae305e16"},
};

static void test_length_boundaries(void) {
  uint8_t pattern[200];
  for (size_t i = 0; i < sizeof pattern; i++) {
    pattern[i] = (uint8_t)((i * 7 + 13) & 0xff);
  }

  const size_t n = sizeof kSizeVectors / sizeof kSizeVectors[0];
  for (size_t i = 0; i < n; i++) {
    char what[128];
    snprintf(what, sizeof what, "长度 %zu 字节：与 C# 参考实现逐字节相同（填充边界）",
             kSizeVectors[i].size);
    ok_digest(pattern, kSizeVectors[i].size, kSizeVectors[i].digest, what);
  }
}

/* ── 检查标准三：MDict 真正用到它的那条路 ───────────────────────────────────
 * 密钥 = RIPEMD128(压缩块[4..8] + 95 36 00 00)。一个 bit 不同，MDict 解出来的
 * 密钥块就是垃圾 —— 所以拿一组固定的 8 字节输入把这条用法钉住。 */
static void test_mdict_key_derivation(void) {
  const uint8_t key_input[8] = {0x10, 0x11, 0x12, 0x13, 0x95, 0x36, 0x00, 0x00};
  ok_digest(key_input, sizeof key_input, "5286d7f33791be8786607e4ec3463997",
            "MDict 密钥派生（块[4..8] + 95 36 00 00）：与 C# 参考实现一致");

  /* 改动输入里的一个 bit，摘要必须不同（同一份输入的复算要稳定、不同输入不许撞）
   * —— 这条是防「摘要函数退化成常量」这类低级错误。 */
  uint8_t flipped[8];
  memcpy(flipped, key_input, sizeof flipped);
  flipped[0] ^= 0x01;
  uint8_t a[16];
  uint8_t b[16];
  dsh_ripemd128(key_input, sizeof key_input, a);
  dsh_ripemd128(flipped, sizeof flipped, b);
  ok(memcmp(a, b, sizeof a) != 0, "改一个 bit 之后摘要必须变（防退化成常量）");

  /* 同一个输入算两次必须一样（函数不许有隐藏状态） */
  uint8_t c[16];
  dsh_ripemd128(key_input, sizeof key_input, c);
  ok(memcmp(a, c, sizeof a) == 0, "同一输入算两次必须完全相同（不许有隐藏状态）");
}

static void test_no_leak(void) {
  /* RIPEMD-128 是「给定缓冲、就地写摘要」，内部不该有任何分配 ——
   * 跑完整组之后活分配表必须还是 0。 */
  ok_eq_size(dsh_mem_live_count(), 0,
             "跑完这一组之后不该还有内核分配的内存没还（RIPEMD-128 内部不分配）");
}

int main(void) {
  test_published_vectors();
  test_length_boundaries();
  test_mdict_key_derivation();
  test_no_leak();

  printf("ripemd128：%d 项，失败 %d\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
