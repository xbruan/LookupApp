/* 内核单元测试 · SHA-256
 * 期望值用公开测试向量（FIPS 180-4 / NIST），不自己编 —— 自算出来的期望只能证明「和昨天一样」。
 * 另钉三件易错事：①流式与一次性在每个分块边界上都等价；②跨块的长度域填充；③hex 的小写与缓冲长度约定。
 */

#include "crypto/dsh_sha256.h"
#include "dsh_lookup.h"

#include <stdio.h>
#include <string.h>

static int g_checks = 0;
static int g_failed = 0;

static void ok(int cond, const char *what) {
  g_checks++;
  if (!cond) {
    g_failed++;
    fprintf(stderr, "FAIL %s\n", what);
  }
}

static void ok_eq_hex(const char *actual, const char *expected, const char *what) {
  g_checks++;
  if (actual == NULL || strcmp(actual, expected) != 0) {
    g_failed++;
    fprintf(stderr, "FAIL %s\n      实际=%s\n      期望=%s\n", what, actual ? actual : "(null)",
            expected);
  }
}

/** 对一段内存算十六进制摘要 */
static void hex_of(const void *data, size_t len, char out[65]) { dsh_sha256_hex(data, len, out); }

int main(void) {
  char hex[65];

  /* ── ① 公开测试向量 ── */
  hex_of("", 0, hex);
  ok_eq_hex(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            "空串的 SHA-256");

  hex_of("abc", 3, hex);
  ok_eq_hex(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "\"abc\" 的 SHA-256");

  hex_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, hex);
  ok_eq_hex(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
            "NIST 那条 56 字节的向量（正好踩在填充边界上）");

  {
    /* 一百万个 'a'（NIST 经典向量），同时压「多次 update」那条路 */
    static char million[1000000];
    memset(million, 'a', sizeof(million));
    hex_of(million, sizeof(million), hex);
    ok_eq_hex(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
              "一百万个 'a' 的 SHA-256");

    /* 同一份数据分块喂，结果必须一样 —— 在每个可能的分块边界上都试一遍 */
    int mismatched = 0;
    for (size_t cut = 0; cut <= 128; cut++) {
      dsh_sha256_ctx ctx;
      dsh_sha256_init(&ctx);
      dsh_sha256_update(&ctx, million, cut);
      dsh_sha256_update(&ctx, million + cut, sizeof(million) - cut);
      uint8_t d[32];
      dsh_sha256_final(&ctx, d);
      char h[65];
      static const char digits[] = "0123456789abcdef";
      for (int i = 0; i < 32; i++) {
        h[i * 2] = digits[(d[i] >> 4) & 0xF];
        h[i * 2 + 1] = digits[d[i] & 0xF];
      }
      h[64] = '\0';
      if (strcmp(h, hex) != 0) mismatched++;
    }
    ok(mismatched == 0, "流式分块喂（0..128 每个切点）必须与一次性喂等价");

    /* 极端分块：一次一字节喂前 300 字节，再一起喂剩下的 */
    {
      dsh_sha256_ctx ctx;
      dsh_sha256_init(&ctx);
      for (size_t i = 0; i < 300; i++) dsh_sha256_update(&ctx, million + i, 1);
      dsh_sha256_update(&ctx, million + 300, sizeof(million) - 300);
      uint8_t d[32];
      dsh_sha256_final(&ctx, d);
      static const char digits[] = "0123456789abcdef";
      for (int i = 0; i < 32; i++) {
        hex[i * 2] = digits[(d[i] >> 4) & 0xF];
        hex[i * 2 + 1] = digits[d[i] & 0xF];
      }
      hex[64] = '\0';
      ok_eq_hex(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
                "逐字节喂 300 次再喂完，结果不变");
    }
  }

  /* ── ② 填充边界：55 / 56 / 63 / 64 / 65 字节各一条 ──
   * 走全「0x80 + 补零 + 8 字节长度」里跨块的分支。期望值用 python3 的 hashlib 现算
   * （见 tools/golden/sha-vectors.py）：长散列手抄错一位，就成了「测试在验证一个错的东西」。 */
  {
    struct { size_t len; const char *want; } cases[] = {
        /* 期望值 = python3 的 hashlib.sha256(b'x'*N).hexdigest()，N 为字节数 */
        {55, "d5e285683cd4efc02d021a5c62014694958901005d6f71e89e0989fac77e4072"},
        {56, "04c26261370ee7541549d16dee320c723e3fd14671e66a099afe0a377c16888e"},
        {63, "75220b47218278e656f2013bb8f0c455a25eaf01e86c64924e9d48d89776d6f2"},
        {64, "7ce100971f64e7001e8fe5a51973ecdfe1ced42befe7ee8d5fd6219506b5393c"},
        {65, "9537c5fdf120482f7d58d25e9ed583f52c02b4e304ea814db1633ad565aed7e9"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      char buf[65];
      memset(buf, 'x', cases[i].len);
      char got[65];
      hex_of(buf, cases[i].len, got);
      char label[96];
      snprintf(label, sizeof(label), "%zu 个 'x' 的 SHA-256（填充边界）", cases[i].len);
      ok_eq_hex(got, cases[i].want, label);
    }
  }

  /* ── ③ 参数边界与便捷函数 ── */
  {
    /* len 为 0 且 data 为 NULL：合法（空串有确定的摘要） */
    hex_of(NULL, 0, hex);
    ok_eq_hex(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
              "data=NULL,len=0 等价于空串");

    /* 缓冲约定：hex 写满 64 个字符、第 65 个是 \0 */
    hex_of("abc", 3, hex);
    ok(strlen(hex) == 64, "hex 的长度必须是 64（不含结尾 \\0）");
    ok(hex[64] == '\0', "hex 的第 65 字节必须是 \\0");
    int lower_only = 1;
    for (int i = 0; i < 64; i++) {
      const char c = hex[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) lower_only = 0;
    }
    ok(lower_only, "hex 必须是**小写**十六进制（id 会进设置文件，大小写不能飘）");

    /* update(NULL, n) 不许崩：宿主可能传任何东西过来 */
    dsh_sha256_ctx ctx;
    dsh_sha256_init(&ctx);
    dsh_sha256_update(&ctx, NULL, 0);
    uint8_t d[32];
    dsh_sha256_final(&ctx, d);
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
      hex[i * 2] = digits[(d[i] >> 4) & 0xF];
      hex[i * 2 + 1] = digits[d[i] & 0xF];
    }
    hex[64] = '\0';
    ok_eq_hex(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
              "一次 update(NULL,0) 之后仍是空串摘要");

    /* 空指针容错（接口定义里「内核不该抛」那条的同类要求） */
    dsh_sha256_init(NULL);
    dsh_sha256_update(NULL, "x", 1);
    dsh_sha256_final(NULL, NULL);
    dsh_sha256_hex("x", 1, NULL);
    ok(1, "对 NULL 上下文/出参调用不许崩");
  }

  /* ── ④ 流式与一次性在「一次喂很大一块」时也一致 ── */
  {
    static char big[5000];
    for (size_t i = 0; i < sizeof(big); i++) big[i] = (char)(i * 7 + 3);
    char once[65];
    hex_of(big, sizeof(big), once);

    dsh_sha256_ctx ctx;
    dsh_sha256_init(&ctx);
    size_t off = 0;
    const size_t steps[] = {1, 63, 64, 65, 100, 1000};
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]) && off < sizeof(big); i++) {
      size_t n = steps[i];
      if (off + n > sizeof(big)) n = sizeof(big) - off;
      dsh_sha256_update(&ctx, big + off, n);
      off += n;
    }
    if (off < sizeof(big)) dsh_sha256_update(&ctx, big + off, sizeof(big) - off);
    uint8_t d[32];
    dsh_sha256_final(&ctx, d);
    static const char digits[] = "0123456789abcdef";
    char got[65];
    for (int i = 0; i < 32; i++) {
      got[i * 2] = digits[(d[i] >> 4) & 0xF];
      got[i * 2 + 1] = digits[d[i] & 0xF];
    }
    got[64] = '\0';
    ok_eq_hex(got, once, "不规则分块（1/63/64/65/100/1000）与一次性喂等价");
  }

  printf("sha256：%d 项，失败 %d\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
