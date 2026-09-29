/* 内核单元测试 · 文本编解码层（UTF-16LE / UTF-8 / GB18030 / Big5 → 内部统一的 UTF-8）；不引任何测试框架，
 * 失败非零退出。钉三件事：四种编码各自解对（含 83 组由 python3 现算后固化、测试里不跑 python 的用例）；
 * 坏字节 / 残缺序列 / GB18030 四字节区都不许让 dsh_text_decode 返回非零；每个返回值都 dsh_release。 */

#include "dsh_internal.h"
#include "dsh_lookup.h"
#include "mem_registry.h"
#include "text/dsh_textcodec.h"

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

static void ok_eq_size(size_t actual, size_t expected, const char *what) {
  g_checks++;
  if (actual != expected) {
    g_failed++;
    fprintf(stderr, "FAIL %s：实际=%zu 期望=%zu\n", what, actual, expected);
  }
}

static void ok_eq_str(const char *actual, const char *expected, const char *what) {
  g_checks++;
  if (actual == NULL || strcmp(actual, expected) != 0) {
    g_failed++;
    fprintf(stderr, "FAIL %s：实际=%s 期望=%s\n", what, actual ? actual : "(null)", expected);
  }
}

/* 小工具 */

/** 把「B2 E2」这样的十六进制串转成字节（用例里的输入一律这么写，免得转义踩坑）。 */
static size_t hex_to_bytes(const char *hex, uint8_t *out, size_t cap) {
  size_t i = 0;
  size_t n = 0;
  while (hex[i] != '\0') {
    unsigned value = 0;
    int k;
    while (hex[i] == ' ') i++;
    if (hex[i] == '\0') break;
    for (k = 0; k < 2 && hex[i] != '\0'; k++) {
      const char c = hex[i++];
      unsigned digit = 0;
      if (c >= '0' && c <= '9') {
        digit = (unsigned)(c - '0');
      } else if (c >= 'A' && c <= 'F') {
        digit = (unsigned)(c - 'A' + 10);
      } else if (c >= 'a' && c <= 'f') {
        digit = (unsigned)(c - 'a' + 10);
      }
      value = value * 16u + digit;
    }
    if (n < cap) out[n++] = (uint8_t)value;
  }
  return n;
}

/** 把一段字节摆成十六进制，供失败信息用（检查标准是字节，所以失败时直接摆字节）。 */
static void bytes_to_hex(const uint8_t *data, size_t len, char *out, size_t cap) {
  static const char k_digits[] = "0123456789ABCDEF";
  size_t used = 0;
  size_t i;
  for (i = 0; i < len && used + 3 < cap; i++) {
    out[used++] = k_digits[(data[i] >> 4) & 0x0Fu];
    out[used++] = k_digits[data[i] & 0x0Fu];
    out[used++] = ' ';
  }
  out[used] = '\0';
}

/* 解码 + 比对 */

/** 解码一段字节，按「字节 + 长度」逐字节比对：词典正文里可能有内嵌 U+0000，
 * 用 strcmp 会在那里提前收工、把差异咽掉。 */
static void check_bytes(dsh_text_encoding enc, const uint8_t *in, size_t in_len, const char *want,
                        size_t want_len, const char *what) {
  char *text = NULL;
  size_t out_len = 0;
  int rc;

  dsh_clear_last_error();
  rc = dsh_text_decode(enc, in, in_len, &text, &out_len);

  g_checks++;
  if (rc != 0) {
    g_failed++;
    const char *msg = dsh_last_error_message();
    fprintf(stderr, "FAIL %s：解码返回非零 %d（%s）\n", what, rc, msg != NULL ? msg : "");
    dsh_release((void *)msg);
    dsh_release(text);
    return;
  }

  if (text == NULL || out_len != want_len || memcmp(text, want, want_len) != 0) {
    char got_hex[512];
    char want_hex[512];
    g_failed++;
    bytes_to_hex((const uint8_t *)text, text != NULL ? out_len : 0, got_hex, sizeof got_hex);
    bytes_to_hex((const uint8_t *)want, want_len, want_hex, sizeof want_hex);
    fprintf(stderr, "FAIL %s：\n     实际(%zu 字节)=%s\n     期望(%zu 字节)=%s\n", what, out_len,
            got_hex, want_len, want_hex);
  }

  /* 接口定义：缓冲保证以 \0 结尾（真值是 out_len，但 out_len 那一格必须在） */
  ok(text != NULL && text[out_len] == '\0', "解码结果的 out_len 处必须是结尾的 \\0");

  dsh_release(text);
}

static void check_hex(dsh_text_encoding enc, const char *in_hex, const char *want,
                      const char *what) {
  uint8_t in[256];
  const size_t in_len = hex_to_bytes(in_hex, in, sizeof in);
  check_bytes(enc, in, in_len, want, strlen(want), what);
}

static void check_str(dsh_text_encoding enc, const char *in, const char *want, const char *what) {
  check_bytes(enc, (const uint8_t *)in, strlen(in), want, strlen(want), what);
}

/* 与 python3 参考实现对照测试 */

/** 一条对照测试用例：输入写成十六进制、期望写成 C 字符串字面量。 */
typedef struct {
  dsh_text_encoding enc;
  const char *what;
  const char *in_hex;
  const char *want;
} ref_case;

/* 本块由 python3 tools/make-textcodec-tables.py --print-cases 生成后粘入（种子 20260901）：正常序列取它的
 * 严格解码结果再编成 UTF-8；坏序列取 errors=replace 约定（实测等于每个坏字节一个 U+FFFD），只有 GB18030
 * 四字节区按本版约定整段替换（python3 会真把它们解出来）。不要手改 —— 要加用例就改脚本再重新生成。 */
static const ref_case k_ref_cases[] = {
  { DSH_TXT_UTF16LE, "utf-16-le：ASCII 单词", "61 00 70 00 70 00 6C 00 65 00", "apple" },
  { DSH_TXT_UTF16LE, "utf-16-le：中文二字词", "4B 6D D5 8B", "测试" },
  { DSH_TXT_UTF16LE, "utf-16-le：中文三字词", "49 6C ED 8B CD 8B 78 51", "汉语词典" },
  { DSH_TXT_UTF16LE, "utf-16-le：拉丁 + 中文混排", "4F 00 78 00 66 00 6F 00 72 00 64 00 20 00 5B 72 25 6D", "Oxford 牛津" },
  { DSH_TXT_UTF16LE, "utf-16-le：生僻字与全角标点", "98 9F 08 FF 4B 6D D5 8B 09 FF 01 30 0C 30 49 6C ED 8B 0D 30", "龘（测试）、「汉语」" },
  { DSH_TXT_UTF16LE, "utf-16-le：补充平面（代理对）emoji", "3D D8 00 DE", "😀" },
  { DSH_TXT_UTF16LE, "utf-16-le：代理对夹在中文里", "4B 6D D5 8B 3D D8 00 DE 49 6C ED 8B", "测试😀汉语" },
  { DSH_TXT_UTF16LE, "utf-16-le：西里尔与希腊字母", "4F 04 31 04 3B 04 3E 04 3A 04 3E 04 20 00 B1 03 B2 03 B3 03", "яблоко αβγ" },
  { DSH_TXT_UTF16LE, "utf-16-le：随机 BMP 抽样 1", "10 76 75 05 1F D6 A4 81 0D BA 9F AD 7A 89 2D 4F", "瘐յ혟膤먍궟襺伭" },
  { DSH_TXT_UTF16LE, "utf-16-le：随机 BMP 抽样 2", "7C 98 AF 51 86 C8 63 2C", "顼冯좆Ᵽ" },
  { DSH_TXT_UTF16LE, "utf-16-le：随机 BMP 抽样 3", "04 66 89 55 A6 46 EB 18 81 5F 64 57 9D 40 E8 94", "昄喉䚦ᣫ征坤䂝铨" },
  { DSH_TXT_UTF16LE, "utf-16-le：随机 BMP 抽样 4", "3C 5D B4 61 6D 9E 4B 32 5E 19", "崼憴鹭㉋ᥞ" },
  { DSH_TXT_UTF16LE, "utf-16-le：随机 BMP 抽样 5", "DF B1 62 79 70 00 CF C0 35 B5 EC 12", "뇟祢p샏딵ዬ" },
  { DSH_TXT_UTF16LE, "utf-16-le：随机 BMP 抽样 6", "DB F4 62 FC 86 6C 7C E3 16 CD 0D 62", "\xEF\x93\x9B\xEF\xB1\xA2\xE6\xB2\x86\xEE\x8D\xBC\xEC\xB4\x96\xE6\x88\x8D" },
  { DSH_TXT_UTF16LE, "utf-16-le：随机补充平面（代理对）1", "EB DB F3 DC 25 DB 88 DD 07 D8 B5 DE", "\xF4\x8A\xB3\xB3\xF3\x99\x96\x88\xF0\x91\xBA\xB5" },
  { DSH_TXT_UTF16LE, "utf-16-le：随机补充平面（代理对）2", "CA D9 4B DE", "\xF2\x82\xA9\x8B" },
  { DSH_TXT_UTF16LE, "utf-16-le：随机补充平面（代理对）3", "62 DB 5B DD 03 D8 C4 DD 69 DA A9 DE", "\xF3\xA8\xA5\x9B\xF0\x90\xB7\x84\xF2\xAA\x9A\xA9" },
  { DSH_TXT_UTF16LE, "utf-16-le：随机补充平面（代理对）4", "79 DB 24 DE 32 DA 23 DC 50 D8 0E DC", "\xF3\xAE\x98\xA4\xF2\x9C\xA0\xA3\xF0\xA4\x80\x8E" },
  { DSH_TXT_GB18030, "gb18030：常用二字词（测）", "B2 E2 CA D4", "测试" },
  { DSH_TXT_GB18030, "gb18030：常用二字词（汉）", "BA BA D3 EF", "汉语" },
  { DSH_TXT_GB18030, "gb18030：三字词", "B4 CA B5 E4 D1 A7", "词典学" },
  { DSH_TXT_GB18030, "gb18030：全角标点与括号", "A3 A8 B2 E2 CA D4 A3 A9 A3 BA A1 B8 BA BA D3 EF A1 B9 A1 A2 A3 BB", "（测试）：「汉语」、；" },
  { DSH_TXT_GB18030, "gb18030：生僻字", "81 40", "丂" },
  { DSH_TXT_GB18030, "gb18030：生僻字与常用字混排", "FD 93 EC 68 FD 51", "龘靐齉" },
  { DSH_TXT_GB18030, "gb18030：数字与拉丁混排", "47 42 32 33 31 32 2D 31 39 38 30 20 B2 E2 CA D4", "GB2312-1980 测试" },
  { DSH_TXT_GB18030, "gb18030：纯 ASCII", "48 65 6C 6C 6F 2C 20 77 6F 72 6C 64 21", "Hello, world!" },
  { DSH_TXT_GB18030, "gb18030：ASCII 与汉字混排", "61 62 63 B2 E2 CA D4 78 79 7A", "abc测试xyz" },
  { DSH_TXT_GB18030, "gb18030：随机双字节序列 1（C3BE CE72…）", "C3 BE CE 72 BC 92 EF 72 AF 5D AD EE", "\xE9\x95\x81\xE8\x9D\xA6\xE7\xB4\xA5\xE9\xA3\x8F\xE7\x97\x8C\xEE\x85\xA7" },
  { DSH_TXT_GB18030, "gb18030：随机双字节序列 2（A565 B2ED…）", "A5 65 B2 ED 93 E5 B1 F5 99 5B", "\xEE\x99\xAB\xE5\xB2\x94\xE6\x92\xB3\xE6\xBB\xA8\xE6\xA9\xBB" },
  { DSH_TXT_GB18030, "gb18030：随机双字节序列 3（C5F2 CF8F…）", "C5 F2 CF 8F", "膨蠌" },
  { DSH_TXT_GB18030, "gb18030：随机双字节序列 4（84BD C8B1…）", "84 BD C8 B1", "劷缺" },
  { DSH_TXT_GB18030, "gb18030：随机双字节序列 5（AED7 CB7C…）", "AE D7 CB 7C C4 E7", "\xEE\x86\xAE\xE8\x97\x8E\xE6\xBA\xBA" },
  { DSH_TXT_GB18030, "gb18030：随机双字节序列 6（AE49 EAB4…）", "AE 49 EA B4 98 C7 C2 9C", "甀甏樓聹" },
  { DSH_TXT_GB18030, "gb18030：随机双字节序列 7（E0AD F056…）", "E0 AD F0 56 A5 72", "\xE5\x96\xB9\xE9\xA4\xA0\xEE\x99\xB8" },
  { DSH_TXT_GB18030, "gb18030：随机双字节序列 8（D98C F092…）", "D9 8C F0 92 CF B8 B1 B7", "賹饞细狈" },
  { DSH_TXT_BIG5, "big5：繁體二字詞（測）", "B4 FA B8 D5", "測試" },
  { DSH_TXT_BIG5, "big5：繁體二字詞（漢）", "BA 7E BB 79", "漢語" },
  { DSH_TXT_BIG5, "big5：繁體三字詞", "B5 FC A8 E5 BE C7", "詞典學" },
  { DSH_TXT_BIG5, "big5：全形標點", "A1 5D B4 FA B8 D5 A1 5E A1 47 A1 75 BA 7E BB 79 A1 76 A1 42 A1 46", "（測試）：「漢語」、；" },
  { DSH_TXT_BIG5, "big5：中日韓罕用字", "F9 D5", "龘" },
  { DSH_TXT_BIG5, "big5：純 ASCII", "48 65 6C 6C 6F 2C 20 77 6F 72 6C 64 21", "Hello, world!" },
  { DSH_TXT_BIG5, "big5：ASCII 與繁體混排", "61 62 63 B4 FA B8 D5 78 79 7A", "abc測試xyz" },
  { DSH_TXT_BIG5, "big5：隨機雙位元組序列 1（EACB C556…）", "EA CB C5 56 D0 5F BB BF", "糑顥胙遛" },
  { DSH_TXT_BIG5, "big5：隨機雙位元組序列 2（B575 C0B5…）", "B5 75 C0 B5", "短懇" },
  { DSH_TXT_BIG5, "big5：隨機雙位元組序列 3（E570 ED7D…）", "E5 70 ED 7D E9 47 D5 51", "幠瞷圜悊" },
  { DSH_TXT_BIG5, "big5：隨機雙位元組序列 4（D8D9 F4BC…）", "D8 D9 F4 BC", "崷蘛" },
  { DSH_TXT_BIG5, "big5：隨機雙位元組序列 5（CABE E6E1…）", "CA BE E6 E1 E6 E7 BF B8", "尨糌緗蕈" },
  { DSH_TXT_BIG5, "big5：隨機雙位元組序列 6（DFC3 E0FC…）", "DF C3 E0 FC B1 EC", "葃僯桿" },
  { DSH_TXT_BIG5, "big5：隨機雙位元組序列 7（CA7E BCB6…）", "CA 7E BC B6 F8 7A B4 F0", "吘撰鼸湘" },
  { DSH_TXT_BIG5, "big5：隨機雙位元組序列 8（CC6F DCF4…）", "CC 6F DC F4 F4 DE E7 6E C0 E8", "帙嫄轘蓽燮" },
};
#define K_REF_CASE_COUNT ((int)(sizeof(k_ref_cases) / sizeof(k_ref_cases[0])))

/* 坏字节 / 残缺序列（同样逐条与 python3 的 replace 约定对照测试）。 */
static const ref_case k_bad_cases[] = {
  { DSH_TXT_GB18030, "gb18030：单个 0x80（没用的首字节）", "80", "\xEF\xBF\xBD" },
  { DSH_TXT_GB18030, "gb18030：单个 0xFF", "FF", "\xEF\xBF\xBD" },
  { DSH_TXT_GB18030, "gb18030：末尾只剩一个首字节", "81", "\xEF\xBF\xBD" },
  { DSH_TXT_GB18030, "gb18030：次字节 0x7F（表里的空槽）", "B2 7F", "\xEF\xBF\xBD\x7F" },
  { DSH_TXT_GB18030, "gb18030：四字节区只给两字节（残缺）", "81 30", "\xEF\xBF\xBD" },
  { DSH_TXT_GB18030, "gb18030：四字节区只给三字节（残缺）", "81 30 81", "\xEF\xBF\xBD" },
  { DSH_TXT_GB18030, "gb18030：首字节后面跟 ASCII 空格", "81 20", "\xEF\xBF\xBD\x20" },
  { DSH_TXT_GB18030, "gb18030：次字节越界 0xFF", "A1 FF", "\xEF\xBF\xBD\xEF\xBF\xBD" },
  { DSH_TXT_GB18030, "gb18030：汉字 + 四字节区（完整）", "B2 E2 81 30 81 30", "\xE6\xB5\x8B\xEF\xBF\xBD" },
  { DSH_TXT_GB18030, "gb18030：四字节区 + 汉字（完整）", "81 30 81 30 B2 E2", "\xEF\xBF\xBD\xE6\xB5\x8B" },
  { DSH_TXT_GB18030, "gb18030：四字节区形状不对（第三字节是 ASCII）", "81 30 41 42", "\xEF\xBF\xBD" },
  { DSH_TXT_GB18030, "gb18030：双字节 + 双字节 + 孤立的 0x80", "B2 E2 FE FE 80", "\xE6\xB5\x8B\xEE\x93\x85\xEF\xBF\xBD" },
  { DSH_TXT_GB18030, "gb18030：全角标点 + 坏字节 + ASCII", "A3 A1 FF 41", "\xEF\xBC\x81\xEF\xBF\xBD\x41" },
  { DSH_TXT_BIG5, "big5：单个 0x80", "80", "\xEF\xBF\xBD" },
  { DSH_TXT_BIG5, "big5：单个 0xA0（首字节不够）", "A0", "\xEF\xBF\xBD" },
  { DSH_TXT_BIG5, "big5：首字节 0xFA（越界）+ 合法次字节", "FA 40", "\xEF\xBF\xBD\x40" },
  { DSH_TXT_BIG5, "big5：次字节 0x7F（越界）", "A1 7F", "\xEF\xBF\xBD\x7F" },
  { DSH_TXT_BIG5, "big5：次字节 0x80（落在两段之间）", "A1 80", "\xEF\xBF\xBD\xEF\xBF\xBD" },
  { DSH_TXT_BIG5, "big5：次字节 0xA0（落在两段之间）", "A1 A0", "\xEF\xBF\xBD\xEF\xBF\xBD" },
  { DSH_TXT_BIG5, "big5：末尾只剩一个首字节", "B4", "\xEF\xBF\xBD" },
  { DSH_TXT_BIG5, "big5：末尾只剩一个首字节（0xA1）", "A1", "\xEF\xBF\xBD" },
  { DSH_TXT_BIG5, "big5：漢 + 坏次字节 + ASCII", "B4 FA A1 7F", "\xE6\xB8\xAC\xEF\xBF\xBD\x7F" },
  { DSH_TXT_BIG5, "big5：越界首字节 + 越界次字节 + 合法词", "FA FE A1 40", "\xEF\xBF\xBD\xEF\xBF\xBD\xE3\x80\x80" },
  { DSH_TXT_BIG5, "big5：合法词 + 0xFF + 0x80", "A1 40 FF 80", "\xE3\x80\x80\xEF\xBF\xBD\xEF\xBF\xBD" },
  { DSH_TXT_UTF16LE, "utf-16-le：末尾剩 1 个字节", "41", "\xEF\xBF\xBD" },
  { DSH_TXT_UTF16LE, "utf-16-le：落单的高代理", "00 D8", "\xEF\xBF\xBD" },
  { DSH_TXT_UTF16LE, "utf-16-le：落单的低代理", "00 DC", "\xEF\xBF\xBD" },
  { DSH_TXT_UTF16LE, "utf-16-le：落单的高代理 + ASCII", "00 D8 41 00", "\xEF\xBF\xBD\x41" },
  { DSH_TXT_UTF16LE, "utf-16-le：ASCII + 落单的高代理", "41 00 00 D8", "\x41\xEF\xBF\xBD" },
  { DSH_TXT_UTF16LE, "utf-16-le：合法代理对 + 落单的高代理", "3D D8 00 DE 00 D8", "\xF0\x9F\x98\x80\xEF\xBF\xBD" },
  { DSH_TXT_UTF16LE, "utf-16-le：合法汉字 + 末尾剩 1 个字节", "B4 FA A1", "\xEF\xAA\xB4\xEF\xBF\xBD" },
  { DSH_TXT_UTF16LE, "utf-16-le：连续两个落单的高代理", "00 D8 00 D8", "\xEF\xBF\xBD\xEF\xBF\xBD" },
  { DSH_TXT_UTF16LE, "utf-16-le：落单的低代理夹在汉字之间", "B2 E2 00 DC B2 E2", "\xEE\x8A\xB2\xEF\xBF\xBD\xEE\x8A\xB2" },
};
#define K_BAD_CASE_COUNT ((int)(sizeof(k_bad_cases) / sizeof(k_bad_cases[0])))
/* 正常序列 50 组 + 坏序列 33 组，共 83 组对照测试用例。 */

static void run_ref_cases(const ref_case *cases, int count, const char *group) {
  int i;
  const size_t before = dsh_mem_live_count();
  for (i = 0; i < count; i++) {
    uint8_t in[256];
    const size_t in_len = hex_to_bytes(cases[i].in_hex, in, sizeof in);
    char label[256];
    snprintf(label, sizeof label, "%s · %s", group, cases[i].what);
    check_bytes(cases[i].enc, in, in_len, cases[i].want, strlen(cases[i].want), label);
  }
  ok_eq_size(dsh_mem_live_count(), before, "对照测试跑完活分配表必须回到基线（每组都 dsh_release 了）");
}

/* ① Encoding 名字 → 枚举 */

static void test_encoding_from_name(void) {
  ok(dsh_text_encoding_from_name(NULL) == DSH_TXT_UTF8,
     "名字为 NULL → UTF-8（头里没写 Encoding 时与 参考实现的默认分支一致）");
  ok(dsh_text_encoding_from_name("") == DSH_TXT_UTF8, "空串 → UTF-8");
  ok(dsh_text_encoding_from_name("UTF-8") == DSH_TXT_UTF8, "UTF-8 → UTF-8");
  ok(dsh_text_encoding_from_name("utf-8") == DSH_TXT_UTF8, "utf-8 → UTF-8（连字符忽略）");
  ok(dsh_text_encoding_from_name("UTF8") == DSH_TXT_UTF8, "UTF8 → UTF-8");

  ok(dsh_text_encoding_from_name("GBK") == DSH_TXT_GB18030, "GBK → GB18030（GBK 是它的子集）");
  ok(dsh_text_encoding_from_name("gbk") == DSH_TXT_GB18030, "gbk 小写也认");
  ok(dsh_text_encoding_from_name("GB2312") == DSH_TXT_GB18030, "GB2312 → GB18030");
  ok(dsh_text_encoding_from_name("gb2312") == DSH_TXT_GB18030, "gb2312 小写也认");
  ok(dsh_text_encoding_from_name("GB18030") == DSH_TXT_GB18030,
     "GB18030 → GB18030（比 参考实现多认的一档，见头文件说明）");

  ok(dsh_text_encoding_from_name("big5") == DSH_TXT_BIG5, "big5 → BIG5");
  ok(dsh_text_encoding_from_name("BIG5") == DSH_TXT_BIG5, "BIG5 大小写不敏感");
  ok(dsh_text_encoding_from_name("Big5") == DSH_TXT_BIG5, "Big5 混写也认");

  ok(dsh_text_encoding_from_name("utf16") == DSH_TXT_UTF16LE, "utf16 → UTF-16LE");
  ok(dsh_text_encoding_from_name("UTF-16") == DSH_TXT_UTF16LE, "utf-16 → UTF-16LE");
  ok(dsh_text_encoding_from_name("UTF-16LE") == DSH_TXT_UTF16LE, "utf-16le → UTF-16LE");
  ok(dsh_text_encoding_from_name("utf_16_le") == DSH_TXT_UTF16LE, "下划线写法也认");
  ok(dsh_text_encoding_from_name("  UTF-16LE  ") == DSH_TXT_UTF16LE, "两头的空格忽略");

  ok(dsh_text_encoding_from_name("Shift_JIS") == DSH_TXT_UTF8, "认不出来的 Shift_JIS → UTF-8");
  ok(dsh_text_encoding_from_name("UTF-16BE") == DSH_TXT_UTF8,
     "UTF-16BE 本版不支持 → UTF-8（与 参考实现同一档）");
  ok(dsh_text_encoding_from_name("unicode") == DSH_TXT_UTF8, "unicode 这种写法认不出来 → UTF-8");
  ok(dsh_text_encoding_from_name("gb") == DSH_TXT_UTF8, "只有 gb 两个字不算 GBK → UTF-8");
}

/* ② UTF-16LE */

static void test_utf16le(void) {
  check_hex(DSH_TXT_UTF16LE, "61 00 62 00 63 00", "abc", "utf-16-le：纯 ASCII");
  check_hex(DSH_TXT_UTF16LE, "4B 6D D5 8B", "测试", "utf-16-le：中文两字（测 试）");
  check_hex(DSH_TXT_UTF16LE, "3D D8 00 DE", "😀",
            "utf-16-le：代理对要合成 U+1F600，编成 UTF-8 就是 F0 9F 98 80 四个字节");
  check_hex(DSH_TXT_UTF16LE, "41 00 3D D8 00 DE 42 00", "A😀B",
            "utf-16-le：代理对夹在 ASCII 之间");

  /* 奇数长度：末尾剩 1 个字节 → 一个 U+FFFD */
  {
    const size_t before = dsh_mem_live_count();
    char *text = NULL;
    size_t out_len = 0;
    const uint8_t in[] = {0x4B, 0x6D, 0xD5, 0x8B, 0x41};
    dsh_clear_last_error();
    ok(dsh_text_decode(DSH_TXT_UTF16LE, in, sizeof in, &text, &out_len) == 0,
       "utf-16-le：奇数长度**不是**失败（坏字节不改返回值）");
    ok_eq_size(out_len, 9, "utf-16-le：测试（6 字节）+ 一个 U+FFFD（3 字节）＝ 9 字节");
    ok(text != NULL && memcmp(text, "测试\xEF\xBF\xBD", 9) == 0,
       "utf-16-le：末尾那半个码元要变成一个 U+FFFD");
    dsh_release(text);
    ok_eq_size(dsh_mem_live_count(), before, "utf-16-le：奇数长度这条不许漏内存");
  }

  /* BOM：开头的 U+FEFF 吃掉（与参考实现的 Decode / TextDecoder 默认行为一致） */
  check_hex(DSH_TXT_UTF16LE, "FF FE 4B 6D D5 8B", "测试", "utf-16-le：开头的 BOM 要吃掉");
  check_hex(DSH_TXT_UTF16LE, "4B 6D FF FE D5 8B", "\xE6\xB5\x8B\xEF\xBB\xBF\xE8\xAF\x95",
            "utf-16-le：中间那个 U+FEFF（小端是 FF FE）不是 BOM，必须原样保留");

  /* 内嵌 U+0000：解出来就是 UTF-8 里的一个 0x00，长度必须照数 */
  {
    const uint8_t in[] = {0x41, 0x00, 0x00, 0x00, 0x42, 0x00};
    const char want[] = {0x41, 0x00, 0x42};
    char *text = NULL;
    size_t out_len = 0;
    dsh_clear_last_error();
    ok(dsh_text_decode(DSH_TXT_UTF16LE, in, sizeof in, &text, &out_len) == 0,
       "utf-16-le：含内嵌 NUL 的记录要能解出来");
    ok_eq_size(out_len, 3, "utf-16-le：内嵌的 U+0000 也算一个字节（out_len 才是真值）");
    ok(text != NULL && memcmp(text, want, 3) == 0 && text[3] == '\0',
       "utf-16-le：内嵌 NUL 原样保留，结尾再加一个 \\0");
    dsh_release(text);
  }
}

/* ③ UTF-8 */

static void test_utf8(void) {
  check_str(DSH_TXT_UTF8, "abc", "abc", "utf-8：ASCII 原样透传");
  check_str(DSH_TXT_UTF8, "测试", "测试", "utf-8：汉字原样透传");
  check_str(DSH_TXT_UTF8, "😀", "😀", "utf-8：四字节 emoji 原样透传");
  check_str(DSH_TXT_UTF8, "测试😀abc", "测试😀abc", "utf-8：混排原样透传");

  check_str(DSH_TXT_UTF8, "\xEF\xBB\xBF" "abc", "abc", "utf-8：开头的 BOM 要吃掉");
  check_str(DSH_TXT_UTF8, "a\xEF\xBB\xBF" "b", "a\xEF\xBB\xBF" "b",
            "utf-8：中间的 U+FEFF 不是 BOM，必须原样保留");

  /* 非法 / 残缺：每个坏字节一个 U+FFFD，而且**返回 0** */
  check_str(DSH_TXT_UTF8, "abc\x80" "def", "abc\xEF\xBF\xBD" "def",
            "utf-8：孤立的后续字节 → 一个 U+FFFD，前后照旧解出来");
  check_str(DSH_TXT_UTF8, "\xC0\xAF", "\xEF\xBF\xBD\xEF\xBF\xBD",
            "utf-8：过长编码（C0 AF）→ 两个 U+FFFD（每个坏字节一个）");
  check_str(DSH_TXT_UTF8, "\xE0\x80\xAF", "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD",
            "utf-8：过长的三字节（E0 80 AF）→ 三个 U+FFFD");
  check_str(DSH_TXT_UTF8, "\xED\xA0\x80", "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD",
            "utf-8：UTF-8 里不许出现代理区（ED A0 80）→ 三个 U+FFFD");
  check_str(DSH_TXT_UTF8, "\xF5\x80\x80\x80", "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD",
            "utf-8：首字节 0xF5 越界 → 四个 U+FFFD");
  check_str(DSH_TXT_UTF8, "测\xE6\xB5",
            "\xE6\xB5\x8B\xEF\xBF\xBD\xEF\xBF\xBD",
            "utf-8：末尾残缺的 E6 B5 → 两个 U+FFFD（本层是「每个坏字节一个」，不是"
            "浏览器那种「最长大子串一个」；这一条是本层与 python 参考实现唯一的约定差异）");
}

/* ④ GB18030 / GBK */

static void test_gb18030(void) {
  check_hex(DSH_TXT_GB18030, "B2 E2 CA D4", "测试", "gb18030：测试");
  check_hex(DSH_TXT_GB18030, "BA BA D3 EF", "汉语", "gb18030：汉语");
  check_hex(DSH_TXT_GB18030, "B4 CA B5 E4 D1 A7", "词典学", "gb18030：词典学");
  check_hex(DSH_TXT_GB18030, "A3 A8 B2 E2 CA D4 A3 A9 A3 BA A1 B8 BA BA D3 EF A1 B9 A1 A2 A3 BB",
            "（测试）：「汉语」、；", "gb18030：全角标点与括号");
  check_hex(DSH_TXT_GB18030, "A3 A1", "！", "gb18030：全角感叹号（A3 A1 是 GBK 的典型全角标点）");
  check_hex(DSH_TXT_GB18030, "81 40", "丂",
            "gb18030：生僻字 丂（U+4E02，GB2312 里没有，只有 GBK/GB18030 才有）");
  check_hex(DSH_TXT_GB18030, "FD 93 EC 68 FD 51", "龘靐齉", "gb18030：三个生僻字连排");
  check_hex(DSH_TXT_GB18030, "61 62 63", "abc", "gb18030：ASCII 段原样透传");
  check_hex(DSH_TXT_GB18030, "61 62 63 B2 E2 CA D4", "abc测试", "gb18030：ASCII 与汉字混排");
}

static void test_gb18030_four_byte_area(void) {
  const size_t before = dsh_mem_live_count();
  char *text = NULL;
  size_t out_len = 0;
  const char *msg = NULL;

  /* 完整四字节序列：整段换成一个 U+FFFD，并记一条诊断 —— 但**返回 0** */
  {
    const uint8_t in[] = {0x81, 0x30, 0x81, 0x30};
    dsh_clear_last_error();
    ok(dsh_text_decode(DSH_TXT_GB18030, in, sizeof in, &text, &out_len) == 0,
       "gb18030：四字节区**不许**报成硬失败（否则整本词典读不出来）");
    ok_eq_size(out_len, 3, "gb18030：四字节区整段换成一个 U+FFFD（3 字节）");
    ok(text != NULL && memcmp(text, "\xEF\xBF\xBD", 3) == 0,
       "gb18030：四字节区按原样替换成 U+FFFD，不许悄悄丢掉");
    msg = dsh_last_error_message();
    ok(msg != NULL && strstr(msg, "四字节区") != NULL && strstr(msg, "不支持") != NULL,
       "gb18030：四字节区要在 last_error 里留下一句「四字节区…不支持」供诊断");
    dsh_release((void *)msg);
    dsh_release(text);
  }

  /* 四字节区夹在汉字中间：前面的汉字照解，四字节段换一个 U+FFFD */
  check_hex(DSH_TXT_GB18030, "B2 E2 81 30 81 30", "\xE6\xB5\x8B\xEF\xBF\xBD",
            "gb18030：测 + 四字节区 → 测 + 一个 U+FFFD");
  check_hex(DSH_TXT_GB18030, "81 30 81 30 B2 E2", "\xEF\xBF\xBD\xE6\xB5\x8B",
            "gb18030：四字节区 + 测 → 一个 U+FFFD + 测");
  check_hex(DSH_TXT_GB18030, "84 31 A4 37", "\xEF\xBF\xBD",
            "gb18030：四字节区里超出 BMP 的那一段（U+10000 以上）同样整段替换");
  check_hex(DSH_TXT_GB18030, "81 30", "\xEF\xBF\xBD",
            "gb18030：残缺的四字节区只给两字节 → 同样一个 U+FFFD（消费约定与 python 一致）");
  check_hex(DSH_TXT_GB18030, "81 30 81", "\xEF\xBF\xBD",
            "gb18030：残缺的四字节区只给三字节 → 同样一个 U+FFFD");

  /* 诊断信息与替换字符是两回事：没有坏字节时 last_error 必须是空的 */
  {
    const uint8_t in[] = {0xB2, 0xE2};
    char *clean = NULL;
    size_t clean_len = 0;
    const char *clean_msg = NULL;
    dsh_clear_last_error();
    ok(dsh_text_decode(DSH_TXT_GB18030, in, sizeof in, &clean, &clean_len) == 0,
       "gb18030：干净输入照旧成功");
    clean_msg = dsh_last_error_message(); /* 取回来的是副本，必须还回去（否则就是漏一块） */
    ok_eq_str(clean_msg, "", "gb18030：没有坏字节时不该留下错误信息");
    dsh_release((void *)clean_msg);
    dsh_release(clean);
  }

  ok_eq_size(dsh_mem_live_count(), before, "四字节区这几条不许漏内存");
}

/* ⑤ Big5 */

static void test_big5(void) {
  check_hex(DSH_TXT_BIG5, "B4 FA B8 D5", "測試", "big5：測試");
  check_hex(DSH_TXT_BIG5, "BA 7E BB 79", "漢語", "big5：漢語");
  check_hex(DSH_TXT_BIG5, "B5 FC A8 E5 BE C7", "詞典學", "big5：詞典學");
  check_hex(DSH_TXT_BIG5, "A1 40", "\xE3\x80\x80", "big5：A1 40 是全角空格 U+3000");
  check_hex(DSH_TXT_BIG5, "61 62 63", "abc", "big5：ASCII 段原样透传");
  check_hex(DSH_TXT_BIG5, "61 62 63 B4 FA B8 D5", "abc測試", "big5：ASCII 与繁体混排");

  /* 坏字节：次字节 0x7F 与 0x80–0xA0 都是空的，首字节越界也不认 */
  check_hex(DSH_TXT_BIG5, "A1 7F", "\xEF\xBF\xBD\x7F",
            "big5：次字节 0x7F 越界 → 首字节换 U+FFFD，0x7F 自己还是 ASCII");
  check_hex(DSH_TXT_BIG5, "A1 A0", "\xEF\xBF\xBD\xEF\xBF\xBD",
            "big5：次字节 0xA0 落在两段之间 → 两个坏字节、两个 U+FFFD");
  check_hex(DSH_TXT_BIG5, "FA 40", "\xEF\xBF\xBD\x40", "big5：首字节 0xFA 越界（只到 0xF9）");
  check_hex(DSH_TXT_BIG5, "B4", "\xEF\xBF\xBD", "big5：末尾只剩一个首字节 → 一个 U+FFFD");
}

/* ⑥ 「坏字节不算失败」这条约定本身 */

static void test_bad_bytes_are_not_failures(void) {
  /* 盯最容易改错的一处：别把「解出过替换字符」当成失败 —— 四档各一个坏字节，全须返回 0 且留下诊断。 */
  struct {
    dsh_text_encoding enc;
    const char *in_hex;
    const char *what;
  } cases[] = {
      {DSH_TXT_UTF8, "E6 B5", "utf-8 残缺"},
      {DSH_TXT_UTF16LE, "00 D8", "utf-16-le 落单代理"},
      {DSH_TXT_GB18030, "81 30 81 30", "gb18030 四字节区"},
      {DSH_TXT_BIG5, "A1 A0", "big5 坏次字节"},
  };
  size_t i;
  for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    uint8_t in[64];
    const size_t in_len = hex_to_bytes(cases[i].in_hex, in, sizeof in);
    char *text = NULL;
    size_t out_len = 0;
    const char *msg = NULL;
    dsh_clear_last_error();
    ok(dsh_text_decode(cases[i].enc, in, in_len, &text, &out_len) == 0,
       "坏字节不算失败：解码必须返回 0（这条约定写在 dsh_textcodec.h 顶部）");
    ok(text != NULL && out_len > 0, "坏字节也要给出结果（替换字符），不能返回空");
    msg = dsh_last_error_message();
    ok(msg != NULL && msg[0] != '\0', "解出过替换字符时要记一条诊断（供诊断用，不是错误）");
    dsh_release((void *)msg);
    dsh_release(text);
  }
}

/* ⑦ 参数与入参自相矛盾（这些**才**是非零返回） */

static void test_arguments(void) {
  const uint8_t in[] = {0x41, 0x00};
  char *text = (char *)0x1; /* 故意给个非空垃圾值，验失败时会被置成 NULL */
  size_t out_len = 99;
  const char *msg = NULL;

  dsh_clear_last_error();
  ok(dsh_text_decode(DSH_TXT_UTF16LE, in, sizeof in, NULL, &out_len) != 0,
     "out_text 为 NULL → 非零返回");
  dsh_clear_last_error();
  ok(dsh_text_decode(DSH_TXT_UTF16LE, in, sizeof in, &text, NULL) != 0,
     "out_len 为 NULL → 非零返回");

  text = (char *)0x1;
  out_len = 99;
  dsh_clear_last_error();
  ok(dsh_text_decode(DSH_TXT_UTF16LE, NULL, 5, &text, &out_len) != 0,
     "字节指针为 NULL 但长度不是 0 → 非零返回");
  ok(text == NULL && out_len == 0, "失败时出参必须被清干净（NULL / 0）");
  msg = dsh_last_error_message();
  ok(msg != NULL && msg[0] != '\0', "失败必须留下人话说明");

  dsh_release((void *)msg);

  text = (char *)0x1;
  out_len = 99;
  dsh_clear_last_error();
  ok(dsh_text_decode((dsh_text_encoding)7, in, sizeof in, &text, &out_len) != 0,
     "枚举值越界（7）→ 非零返回（这是调用方的 bug，不是文件内容的问题）");
  ok(text == NULL && out_len == 0, "枚举越界时出参也要清干净");

  /* 空输入是合法的：给一个空串，长度 0 */
  text = NULL;
  out_len = 99;
  dsh_clear_last_error();
  ok(dsh_text_decode(DSH_TXT_GB18030, NULL, 0, &text, &out_len) == 0,
     "空输入（NULL + 长度 0）是合法的，不是错误");
  ok(text != NULL && text[0] == '\0', "空输入要给出一个可释放的空串");
  ok_eq_size(out_len, 0, "空输入的 out_len 是 0");
  dsh_release(text);

  text = NULL;
  out_len = 99;
  ok(dsh_text_decode(DSH_TXT_UTF8, (const uint8_t *)"", 0, &text, &out_len) == 0,
     "长度 0 的非 NULL 指针同样合法");
  dsh_release(text);
}

/* ⑧ 与参考实现对照测试 */

static void test_reference_cases(void) {
  run_ref_cases(k_ref_cases, K_REF_CASE_COUNT, "正常序列");
  run_ref_cases(k_bad_cases, K_BAD_CASE_COUNT, "坏序列");
}

/* main */

int main(void) {
  const size_t base = dsh_mem_live_count();

  test_encoding_from_name();
  test_utf16le();
  test_utf8();
  test_gb18030();
  test_gb18030_four_byte_area();
  test_big5();
  test_bad_bytes_are_not_failures();
  test_arguments();
  test_reference_cases();

  ok_eq_size(base, 0, "跑测试之前活分配表应当是空的（有早就说明别处漏了）");
  ok_eq_size(dsh_mem_live_count(), base,
             "全部跑完之后活分配表必须回到基线（每个返回值都 dsh_release 了）");

  printf("textcodec：%d 项（对照测试 %d + %d 组），失败 %d\n", g_checks,
         K_REF_CASE_COUNT, K_BAD_CASE_COUNT, g_failed);
  return g_failed == 0 ? 0 : 1;
}
