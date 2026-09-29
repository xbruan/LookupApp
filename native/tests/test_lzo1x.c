/* 内核单元测试 · LZO1X 解压（src/compress/dsh_lzo1x.c）：解码器错一个分支就是整块词条读到垃圾，
 * 所以不只测「不崩」—— 期望输出一律取自参考实现（参考实现的 Lzo1x.cs = js-mdict 的 lzo1x.js）逐字节比，期望字节不许手改。
 * ⚠️ test.mdx 里没有 LZO 块（词块与记录块都是 zlib），真样本只能取自参考实现/tools/MdxProbe/variants 下的变体词典。 */

#include "compress/dsh_lzo1x.h"

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

/* 十六进制向量 → 字节：向量固化在源码里，不依赖测试用词典文件在场；解出来的缓冲也走
 * dsh_mem_alloc —— 这一组最后要断言活分配表回到基线，测试自己的临时缓冲同样得记账。 */
static int hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static uint8_t *hex_to_bytes(const char *hex, size_t *out_len) {
  const size_t n = strlen(hex);
  const size_t bytes = n / 2;
  uint8_t *buf = (uint8_t *)dsh_mem_alloc(bytes);
  if (buf == NULL) {
    fprintf(stderr, "测试自身的内存分配失败（环境问题）\n");
    *out_len = 0;
    return NULL;
  }
  for (size_t i = 0; i < bytes; i++) {
    buf[i] = (uint8_t)((hex_nibble(hex[2 * i]) << 4) | hex_nibble(hex[2 * i + 1]));
  }
  *out_len = bytes;
  return buf;
}

/* 向量表：_in 是喂给解压器的 LZO 流，_out 是参考实现给出的期望输出。 */

/* 真实词典的 LZO 块：输入已去掉 8 字节头，期望输出由 js-mdict 与 C# 两版一致给出。 */

/* 真实词典 v12-lzo.mdx 的第 0 个词块（首词 「apple」）；输入 92 字节 → 期望输出 94 字节 */
static const char *const k_v12_lzo_mdx_key0_in =
    "06000000006170706C65600109736170706C69636174696F6E7D01CD7C010C7900000001246170707265636961746005"
    "05018062616E616E61640303C6636F6C6F7264010001D5636F6C6F75720000000224E6B58BE8AF9500110000";
static const char *const k_v12_lzo_mdx_key0_out =
    "000000006170706C6500000000736170706C69636174696F6E00000000CD6170706C7900000001246170707265636961"
    "7465000000018062616E616E6100000001C6636F6C6F7200000001D5636F6C6F75720000000224E6B58BE8AF9500";

/* 真实词典 v12-lzo.mdx 的第 0 个记录块；输入 356 字节 → 期望输出 662 字节 */
static const char *const k_v12_lzo_mdx_rec0_in =
    "00163C64697620636C6173733D22656E747279223E3C68313E6170706C653C2F68313E3C7370616E2063A4040D70686F"
    "6E223E2FCB88C3A6706C2F3C2F6D033E2A08010B646566223EE88BB9E69E9CEFBC9BA00106E6A0913C2F6469763EB500"
    "002ABC002DC8010469636174696F6EA30F646976FC0E980A0003E794B3E8AFB7EFBC9BE5BA94E794A8E7A88BE5BA8FAC"
    "0AB4003A65017920044C0106EFBC9BE98082E794A8A00AB40039580103726563696174C0252EBC020CE6ACA3E8B58FEF"
    "BC9BE6849FE6BF806C0B02A286E4BC9AB40AB400366C010362616E616E6134CC0203E9A699E89589BC07B4000C004040"
    "404C494E4B3D636F6C6F7572365001B40335500102A29CE889B26C360289B2E5BDA9BC0AB40036380103E6B58BE8AF95"
    "34380109E4B8ADE69687E8AF8DE69DA1BC04001B20E28094E2809420E9AA8CE8AF81E5AD97E7ACA6E99B86E4B88EE994"
    "AEE5908D3C2F6469763E3C2F6469763E00110000";
static const char *const k_v12_lzo_mdx_rec0_out =
    "3C64697620636C6173733D22656E747279223E3C68313E6170706C653C2F68313E3C7370616E20636C6173733D227068"
    "6F6E223E2FCB88C3A6706C2F3C2F7370616E3E3C64697620636C6173733D22646566223EE88BB9E69E9CEFBC9BE88BB9"
    "E69E9CE6A0913C2F6469763E3C2F6469763E003C64697620636C6173733D22656E747279223E3C68313E6170706C6963"
    "6174696F6E3C2F68313E3C64697620636C6173733D22646566223EE794B3E8AFB7EFBC9BE5BA94E794A8E7A88BE5BA8F"
    "3C2F6469763E3C2F6469763E003C64697620636C6173733D22656E747279223E3C68313E6170706C793C2F68313E3C64"
    "697620636C6173733D22646566223EE794B3E8AFB7EFBC9BE5BA94E794A8EFBC9BE98082E794A83C2F6469763E3C2F64"
    "69763E003C64697620636C6173733D22656E747279223E3C68313E617070726563696174653C2F68313E3C6469762063"
    "6C6173733D22646566223EE6ACA3E8B58FEFBC9BE6849FE6BF80EFBC9BE9A286E4BC9A3C2F6469763E3C2F6469763E00"
    "3C64697620636C6173733D22656E747279223E3C68313E62616E616E613C2F68313E3C64697620636C6173733D226465"
    "66223EE9A699E895893C2F6469763E3C2F6469763E004040404C494E4B3D636F6C6F7572003C64697620636C6173733D"
    "22656E747279223E3C68313E636F6C6F75723C2F68313E3C64697620636C6173733D22646566223EE9A29CE889B2EFBC"
    "9BE889B2E5BDA93C2F6469763E3C2F6469763E003C64697620636C6173733D22656E747279223E3C68313EE6B58BE8AF"
    "953C2F68313E3C64697620636C6173733D22646566223EE4B8ADE69687E8AF8DE69DA1E6B58BE8AF9520E28094E28094"
    "20E9AA8CE8AF81E5AD97E7ACA6E99B86E4B88EE994AEE5908D3C2F6469763E3C2F6469763E00";

/* 真实词典 v2-lzo.mdx 的第 0 个词块（首词 「apple<NUL>」）；输入 94 字节 → 期望输出 126 字节 */
static const char *const k_v2_lzo_mdx_key0_in =
    "0A00000000000000006170706C65F1017374010469636174696F6EED02CD6D0279D401080124617070726563696174F0"
    "0605018062616E616E61E40403C6636F6C6F72F501D574010F7572000000000000000224E6B58BE8AF9500110000";
static const char *const k_v2_lzo_mdx_key0_out =
    "00000000000000006170706C650000000000000000736170706C69636174696F6E0000000000000000CD6170706C7900"
    "00000000000001246170707265636961746500000000000000018062616E616E610000000000000001C6636F6C6F7200"
    "00000000000001D5636F6C6F7572000000000000000224E6B58BE8AF9500";

/* 合成向量：用参考实现的压缩器压出来的；小的一组连期望输出一起固化。 */

/* 合成 小文本；输入 73 字节 → 期望输出 123 字节（roundTrip=true） */
static const char *const k_synth_text_in =
    "001C4C5A4F315820E5AFB9E68B8DEFBC9A6170706C65202F2062616E616E61202F20E6B58BE8AF95E380824C5A4F3158"
    "201BA0000E616E616E61202F20E6B58BE8AF95E38082110000";
static const char *const k_synth_text_out =
    "4C5A4F315820E5AFB9E68B8DEFBC9A6170706C65202F2062616E616E61202F20E6B58BE8AF95E380824C5A4F315820E5"
    "AFB9E68B8DEFBC9A6170706C65202F2062616E616E61202F20E6B58BE8AF95E380824C5A4F315820E5AFB9E68B8DEFBC"
    "9A6170706C65202F2062616E616E61202F20E6B58BE8AF95E38082";

/* 合成 64KB 重复模式；输入 375 字节 → 期望输出 65536 字节（roundTrip=true） */
static const char *const k_synth_pattern_64k_in =
    "00126162636465666768696A6B6C6D6E6F707172737475767778797A3031323334353637383920000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000006B8C00002236373839"
    "6162636465666768696A6B6C6D6E6F707172737475767778797A303132333435363738396162636465666768696A6B6C"
    "200000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00000000000000000000000000000000EA8C000D6162636465666768696A6B6C6D6E6F70110000";
/* 期望输出 65536 字节太长不固化，由测试自己按已知模式生成后逐字节比对。 */

/* 合成 70000 字节长零串；输入 328 字节 → 期望输出 70000 字节（roundTrip=true） */
static const char *const k_synth_zeros_70k_in =
    "030100000000002000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00000000000000008B00000C000000000000000000000000000000200000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000008C0000000100000000000000000000000000000000000002110000";
/* 期望输出 70000 字节太长不固化，由测试自己按已知模式生成后逐字节比对。 */

/* 分节一：真实词典里的 LZO 块，必须与参考实现逐字节相同 */
static void check_vector(const char *const in_hex, const char *const out_hex, const char *what) {
  size_t in_len = 0;
  size_t want_len = 0;
  uint8_t *in = hex_to_bytes(in_hex, &in_len);
  uint8_t *want = hex_to_bytes(out_hex, &want_len);
  if (in == NULL || want == NULL) return;

  uint8_t *got = NULL;
  size_t got_len = 0;
  const int rc = dsh_lzo1x_decompress(in, in_len, &got, &got_len);

  ok(rc == DSH_LZO1X_OK, what);
  ok_eq_size(got_len, want_len, what);
  ok(got != NULL && want_len > 0 && memcmp(got, want, want_len) == 0, what);

  dsh_release(got);
  dsh_release(in);
  dsh_release(want);
}

static void test_real_dictionary_blocks(void) {
  /* v1.2 格式词典的词块（首词 apple） */
  check_vector(k_v12_lzo_mdx_key0_in, k_v12_lzo_mdx_key0_out,
               "v12-lzo.mdx 词块：解出来必须与参考实现逐字节相同（v1.2 的 LZO 词块）");
  /* v2.0 格式词典的词块 —— 与上面同一批词条，但填充与令牌序列不同 */
  check_vector(k_v2_lzo_mdx_key0_in, k_v2_lzo_mdx_key0_out,
               "v2-lzo.mdx 词块：解出来必须与参考实现逐字节相同（v2.0 的 LZO 词块）");
  /* 记录块（词条正文）：三本 LZO 测试用词典的这一个块逐字节相同（输入的 sha256 前 12 位都是 7037548523c6），
   * 所以一条就够 —— 别以为漏测了另外两本。 */
  check_vector(k_v12_lzo_mdx_rec0_in, k_v12_lzo_mdx_rec0_out,
               "记录块（三本 LZO 测试用词典共用同一串字节）：必须与参考实现逐字节相同");
  /* 合成：一小段 UTF-8 文本，期望输出也一起固化 */
  check_vector(k_synth_text_in, k_synth_text_out,
               "合成小文本：必须与参考实现逐字节相同");
}

/* 分节二：需要扩容缓冲的大块 —— 参考实现的初始容量只有 4096（输入长度补齐到 4096 的整数倍），
 * 这两组的期望输出是 65536 / 70000 字节，必然走过扩容那条路；输出太长不固化，按已知模式生成后逐字节比。 */
static void check_pattern_vector(const char *const in_hex, const uint8_t *want, size_t want_len,
                                const char *what) {
  size_t in_len = 0;
  uint8_t *in = hex_to_bytes(in_hex, &in_len);
  if (in == NULL) return;

  uint8_t *got = NULL;
  size_t got_len = 0;
  const int rc = dsh_lzo1x_decompress(in, in_len, &got, &got_len);

  ok(rc == DSH_LZO1X_OK, what);
  ok_eq_size(got_len, want_len, what);
  ok(got != NULL && memcmp(got, want, want_len) == 0, what);

  dsh_release(got);
  dsh_release(in);
}

static void test_growth_repeating_pattern(void) {
  /* 64KB 的重复模式：压缩后只有 375 字节，解出来 65536 字节 —— 容量 4096 远远不够 */
  const char *const pattern = "abcdefghijklmnopqrstuvwxyz0123456789";
  const size_t pattern_len = 36;
  const size_t want_len = 64 * 1024;
  uint8_t *want = (uint8_t *)dsh_mem_alloc(want_len);
  if (want == NULL) {
    fprintf(stderr, "测试自身的内存分配失败（环境问题）\n");
    return;
  }
  for (size_t i = 0; i < want_len; i++) want[i] = (uint8_t)pattern[i % pattern_len];

  check_pattern_vector(k_synth_pattern_64k_in, want, want_len,
                       "64KB 重复模式（必须扩容：初始容量只有 4096）：与参考实现逐字节相同");
  dsh_release(want);
}

static void test_growth_long_zero_run(void) {
  /* 70000 字节的长零串：同时逼出「长度逃逸串」（t == 0 → 每遇一个 0 字节加 255） */
  const size_t want_len = 70000;
  uint8_t *want = (uint8_t *)dsh_mem_alloc(want_len);
  if (want == NULL) {
    fprintf(stderr, "测试自身的内存分配失败（环境问题）\n");
    return;
  }
  memset(want, 0, want_len);
  want[0] = 1;
  want[want_len - 1] = 2;

  check_pattern_vector(k_synth_zeros_70k_in, want, want_len,
                       "70000 字节长零串（逃逸长度 + 扩容）：与参考实现逐字节相同");
  dsh_release(want);
}

/* 分节三：损坏数据「不崩、不挂、报错」—— 这些输入参考实现是不会终止的（实测：C# 版在单个 0x00 上
 * 死循环、10 秒没返回，JS 原版跑满 60 秒也没回），所以这一组同时钉住本移植新增的这条保护：
 * 越界读写一律当 0（参考语义），但「长度逃逸串读到越界」直接报损坏。 */
static void expect_rejected(const uint8_t *input, size_t input_len, const char *what,
                            const char *must_mention) {
  uint8_t *got = (uint8_t *)0x1; /* 故意放个非 NULL 的哨兵：失败时它必须被置回 NULL */
  size_t got_len = 12345;
  dsh_clear_last_error();
  const int rc = dsh_lzo1x_decompress(input, input_len, &got, &got_len);

  ok(rc != DSH_LZO1X_OK, what);
  ok(got == NULL, what);
  ok_eq_size(got_len, 0, what);
  const char *msg = dsh_last_error_message();
  ok(msg != NULL && msg[0] != '\0', what);
  if (must_mention != NULL) {
    ok(msg != NULL && strstr(msg, must_mention) != NULL, what);
  }
  dsh_release((void *)msg);
  dsh_release(got);
}

static void test_corrupt_inputs_do_not_hang(void) {
  /* 空输入：参考实现的长度逃逸串在这里会读到越界（那边永远读到 0 → 死循环） */
  const uint8_t empty[1] = {0};
  expect_rejected(empty, 0, "空输入必须报损坏（参考实现会死循环）", "损坏");

  /* 单个 0x00：同上，只是多了一个 0 字节 */
  const uint8_t one_zero[1] = {0x00};
  expect_rejected(one_zero, sizeof one_zero, "单字节 0x00 必须报损坏（参考实现会死循环）", "损坏");

  /* 单个 0x20：先走一段字面量（越界读全当 0），再落到越界的那条路上 —— 少了「越界读当 0」这条语义，
   * ASan 当场就会报越界读。 */
  const uint8_t one_20[1] = {0x20};
  expect_rejected(one_20, sizeof one_20, "单字节 0x20 必须报损坏，且越界读不许真去读内存", "损坏");

  /* 合法流被拦腰截断：前半段仍然解得动，但收不到流结束标记 */
  size_t in_len = 0;
  uint8_t *full = hex_to_bytes(k_v12_lzo_mdx_rec0_in, &in_len);
  if (full != NULL) {
    expect_rejected(full, in_len / 2, "合法流被截断必须报损坏（不许当成解完了）", "损坏");
    dsh_release(full);
  }
}

static void test_output_limit(void) {
  /* 造一份「声称要解出 64MB 以上」的流：一个 0 字节开头（t == 0）加一长串 0（每个 +255），
   * 最后 0xff 收尾 —— t = 255 * 264000 + 15 + 255 + 3 ≈ 67.3MB > 64MB，必须在扩容前就报错。
   * 同一份输入交给 C# 版抛的也是「LZO 解压输出超过 64MB，输入数据可能已损坏」，两边判定一致。 */
  const size_t zeros = 264000;
  const size_t len = 1 + zeros + 1;
  uint8_t *input = (uint8_t *)dsh_mem_alloc(len);
  if (input == NULL) {
    fprintf(stderr, "测试自身的内存分配失败（环境问题）\n");
    return;
  }
  memset(input, 0, len);
  input[len - 1] = 0xff;

  expect_rejected(input, len, "输出超过 64MB 必须按损坏数据报错，不许继续吃内存", "64MB");
  dsh_release(input);
}

/* 分节四：参数与内存接口定义 */
static void test_arg_validation(void) {
  uint8_t *got = NULL;
  size_t got_len = 0;
  const uint8_t one = 0x00;

  dsh_clear_last_error();
  ok(dsh_lzo1x_decompress(&one, 1, NULL, &got_len) != DSH_LZO1X_OK,
     "out_bytes 传 NULL 必须被拒绝");
  ok(dsh_lzo1x_decompress(&one, 1, &got, NULL) != DSH_LZO1X_OK,
     "out_len 传 NULL 必须被拒绝");
  ok(dsh_lzo1x_decompress(NULL, 4, &got, &got_len) != DSH_LZO1X_OK,
     "input 是 NULL 而长度不为 0 必须被拒绝");

  /* 成功那次的出参是「恰好 out_len 字节」的一块内存，而且必须是内核登记过的（dsh_release 认得出它）。
   * 这一点拿 dsh_mem_live_count 的涨落来钉：工作缓冲没自己还掉，这里就会多出一块。 */
  size_t in_len = 0;
  uint8_t *in = hex_to_bytes(k_v12_lzo_mdx_key0_in, &in_len);
  if (in != NULL) {
    const size_t base = dsh_mem_live_count();
    uint8_t *out = NULL;
    size_t out_len = 0;
    ok(dsh_lzo1x_decompress(in, in_len, &out, &out_len) == DSH_LZO1X_OK,
       "合法输入必须解压成功");
    ok(dsh_mem_live_count() == base + 1,
       "成功时只该多出「解压结果」这一块（工作缓冲必须自己还掉）");
    dsh_release(out);
    ok_eq_size(dsh_mem_live_count(), base, "还掉结果之后活分配表回到基线");
    dsh_release(in);
  }
}

static void test_no_leak(void) {
  /* 全部用例跑完之后活分配表必须回到基线：成功、报损坏、超限三条路都不许漏。 */
  const size_t live = dsh_mem_live_count();
  ok_eq_size(live, 0, "跑完这一组之后不该还有内核分配的内存没还（活分配表必须是 0）");
}

int main(void) {
  test_real_dictionary_blocks();
  test_growth_repeating_pattern();
  test_growth_long_zero_run();
  test_corrupt_inputs_do_not_hang();
  test_output_limit();
  test_arg_validation();
  test_no_leak();

  printf("lzo1x：%d 项，失败 %d\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
