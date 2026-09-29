/* ==========================================================================
 * 【生成，不许手改】词条正文的标准答案表 —— 由 0.1.3 的参考实现
 * `EntryDocument.Build` 产出（那份参考实现不随本仓库发布；接回来的步骤见
 * `tools/make-entry-assets.ps1` 找不到它时给出的提示）。
 *
 * 生成：powershell -File tools/make-entry-assets.ps1
 * 消费方：native/tests/test_entry_doc.c（逐字节对照测试）
 *
 * ⚠️ 每份文档**不是**整份抄在这里（那会是一兆的十六进制），而是拆成三段：
 *      prefix + DSH_ENTRY_BASE_STYLE + mid + DSH_ENTRY_BRIDGE_SCRIPT + suffix
 *    那两段资产在 native/src/dict/entry_assets.h 里（同一份参考实现产出）。
 *    生成器**当场断言**过：参考实现那份完整文档里，款式与脚本各只出现一次，
 *    而且按这个拆法能**逐字节reconstruct**回原样 —— 所以这里还带着整份文档的
 *    SHA-256：测试那边拼完先核哈希，证明"拼出来的就是参考实现那份"，
 *    再去比对 C 版。少了这一步，这套拆法就成了自说自话。
 * ========================================================================== */

#ifndef DSH_ENTRY_DOC_VECTORS_H
#define DSH_ENTRY_DOC_VECTORS_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
  const char *label;
  const char *dict_id;
  const unsigned char *definition;
  size_t definition_len;
  const unsigned char *notice;
  size_t notice_len;
  int has_resources;
  const unsigned char *prefix;
  size_t prefix_len;
  const unsigned char *mid;
  size_t mid_len;
  const unsigned char *suffix;
  size_t suffix_len;
  const unsigned char *expected_sha256; /* 32 字节，参考实现那份完整文档的哈希 */
} dsh_entry_doc_vector;

static const char DOC_ID_0[] = "abc123";
static const unsigned char DOC_DEF_0[] = {0x3C,0x70,0x3E,0x61,0x70,0x70,0x6C,0x65,0x3C,0x2F,0x70,0x3E};
static const unsigned char DOC_NOTICE_0[] = {0x00};
static const unsigned char DOC_PREFIX_0[] = "<!doctype html>\012<html lang=\"zh-CN\">\012<head>\012<meta charset=\"utf-8\">\012<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; img-src https://*.dictres.invalid data: blob:; media-src https://*.dictres.invalid data: blob:; style-src 'unsafe-inline' https://*.dictres.invalid; font-src https://*.dictres.invalid data:; script-src 'unsafe-inline' https://*.dictres.invalid; connect-src 'none'; frame-src 'none'; object-src 'none'; form-action 'none'; base-uri https://*.dictres.invalid;\">\012<base href=\"https://abc123.dictres.invalid/\">\012<style>";
static const unsigned char DOC_MID_0[] = "</style>\012</head>\012<body class=\"lookup-entry\" data-has-resources=\"1\">\012<p>apple</p>\012<div class=\"lookup-chips\" id=\"lookupChips\" hidden></div>\012<script>";
static const unsigned char DOC_SUFFIX_0[] = "</script>\012</body>\012</html>";
static const unsigned char DOC_HASH_0[32] = {0x0F,0x24,0xB0,0x63,0xCB,0x2B,0x45,0x51,0x08,0x86,0x65,0x5A,0xC8,0xE4,0x6F,0x8A,0x83,0xEE,0xD6,0x61,0xCB,0x0D,0xB8,0xA0,0x6A,0xF9,0xC8,0x0F,0xA8,0x58,0xA0,0xD3};
static const char DOC_ID_1[] = "abc123";
static const unsigned char DOC_DEF_1[] = {0x3C,0x70,0x3E,0x61,0x70,0x70,0x6C,0x65,0x3C,0x2F,0x70,0x3E};
static const unsigned char DOC_NOTICE_1[] = {0x00};
static const unsigned char DOC_PREFIX_1[] = "<!doctype html>\012<html lang=\"zh-CN\">\012<head>\012<meta charset=\"utf-8\">\012<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; img-src https://*.dictres.invalid data: blob:; media-src https://*.dictres.invalid data: blob:; style-src 'unsafe-inline' https://*.dictres.invalid; font-src https://*.dictres.invalid data:; script-src 'unsafe-inline' https://*.dictres.invalid; connect-src 'none'; frame-src 'none'; object-src 'none'; form-action 'none'; base-uri https://*.dictres.invalid;\">\012<base href=\"https://abc123.dictres.invalid/\">\012<style>";
static const unsigned char DOC_MID_1[] = "</style>\012</head>\012<body class=\"lookup-entry\" data-has-resources=\"0\">\012<p>apple</p>\012<div class=\"lookup-chips\" id=\"lookupChips\" hidden></div>\012<script>";
static const unsigned char DOC_SUFFIX_1[] = "</script>\012</body>\012</html>";
static const unsigned char DOC_HASH_1[32] = {0xAF,0xA8,0x7D,0xBA,0x91,0xAB,0xA0,0x80,0x86,0x90,0x61,0x2F,0xD9,0x37,0x8F,0x77,0x11,0xCD,0x05,0xF1,0x24,0x2C,0x55,0x61,0xD4,0x1D,0xE7,0x53,0x50,0x8B,0x63,0xEE};
static const char DOC_ID_2[] = "d1";
static const unsigned char DOC_DEF_2[] = {0x00};
static const unsigned char DOC_NOTICE_2[] = {0x3C,0x62,0x3E,0xE6,0xB2,0xA1,0xE6,0x9C,0x89,0xE8,0xBF,0x99,0xE4,0xB8,0xAA,0xE8,0xAF,0x8D,0x3C,0x2F,0x62,0x3E};
static const unsigned char DOC_PREFIX_2[] = "<!doctype html>\012<html lang=\"zh-CN\">\012<head>\012<meta charset=\"utf-8\">\012<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; img-src https://*.dictres.invalid data: blob:; media-src https://*.dictres.invalid data: blob:; style-src 'unsafe-inline' https://*.dictres.invalid; font-src https://*.dictres.invalid data:; script-src 'unsafe-inline' https://*.dictres.invalid; connect-src 'none'; frame-src 'none'; object-src 'none'; form-action 'none'; base-uri https://*.dictres.invalid;\">\012<base href=\"https://d1.dictres.invalid/\">\012<style>";
static const unsigned char DOC_MID_2[] = "</style>\012</head>\012<body class=\"lookup-entry\" data-has-resources=\"1\">\012<div class=\"lookup-notice\"><b>\346\262\241\346\234\211\350\277\231\344\270\252\350\257\215</b></div>\012<div class=\"lookup-chips\" id=\"lookupChips\" hidden></div>\012<script>";
static const unsigned char DOC_SUFFIX_2[] = "</script>\012</body>\012</html>";
static const unsigned char DOC_HASH_2[32] = {0xC6,0x03,0xBB,0xF9,0x8D,0x28,0x18,0x90,0xE9,0xD9,0x1A,0xC8,0x7B,0xD7,0xCA,0x38,0xE7,0xC1,0xF0,0x4C,0xAF,0x2B,0xB7,0xAB,0x68,0xCE,0x41,0x47,0x77,0x73,0xB6,0x52};
static const char DOC_ID_3[] = "d1";
static const unsigned char DOC_DEF_3[] = {0x3C,0x64,0x69,0x76,0x3E,0x78,0x3C,0x2F,0x64,0x69,0x76,0x3E};
static const unsigned char DOC_NOTICE_3[] = {0xE6,0x8F,0x90,0xE7,0xA4,0xBA};
static const unsigned char DOC_PREFIX_3[] = "<!doctype html>\012<html lang=\"zh-CN\">\012<head>\012<meta charset=\"utf-8\">\012<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; img-src https://*.dictres.invalid data: blob:; media-src https://*.dictres.invalid data: blob:; style-src 'unsafe-inline' https://*.dictres.invalid; font-src https://*.dictres.invalid data:; script-src 'unsafe-inline' https://*.dictres.invalid; connect-src 'none'; frame-src 'none'; object-src 'none'; form-action 'none'; base-uri https://*.dictres.invalid;\">\012<base href=\"https://d1.dictres.invalid/\">\012<style>";
static const unsigned char DOC_MID_3[] = "</style>\012</head>\012<body class=\"lookup-entry\" data-has-resources=\"0\">\012<div class=\"lookup-notice\">\346\217\220\347\244\272</div>\012<div class=\"lookup-chips\" id=\"lookupChips\" hidden></div>\012<script>";
static const unsigned char DOC_SUFFIX_3[] = "</script>\012</body>\012</html>";
static const unsigned char DOC_HASH_3[32] = {0xBB,0x2D,0x81,0xB5,0x95,0x62,0xE4,0x63,0xEE,0x3F,0x44,0x4E,0x2A,0x92,0x38,0xE5,0x6A,0x51,0xC2,0xFD,0xBA,0x16,0x5E,0xC8,0xF6,0xDF,0x80,0x34,0x7A,0xBF,0x4B,0x37};
static const char DOC_ID_4[] = "d2";
static const unsigned char DOC_DEF_4[] = {0x00};
static const unsigned char DOC_NOTICE_4[] = {0x00};
static const unsigned char DOC_PREFIX_4[] = "<!doctype html>\012<html lang=\"zh-CN\">\012<head>\012<meta charset=\"utf-8\">\012<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; img-src https://*.dictres.invalid data: blob:; media-src https://*.dictres.invalid data: blob:; style-src 'unsafe-inline' https://*.dictres.invalid; font-src https://*.dictres.invalid data:; script-src 'unsafe-inline' https://*.dictres.invalid; connect-src 'none'; frame-src 'none'; object-src 'none'; form-action 'none'; base-uri https://*.dictres.invalid;\">\012<base href=\"https://d2.dictres.invalid/\">\012<style>";
static const unsigned char DOC_MID_4[] = "</style>\012</head>\012<body class=\"lookup-entry\" data-has-resources=\"0\">\012\012<div class=\"lookup-chips\" id=\"lookupChips\" hidden></div>\012<script>";
static const unsigned char DOC_SUFFIX_4[] = "</script>\012</body>\012</html>";
static const unsigned char DOC_HASH_4[32] = {0x67,0xF0,0xF7,0xEC,0x14,0xDE,0x73,0xD2,0x72,0xD7,0x43,0x74,0x03,0x3C,0xE6,0x3A,0xDF,0x61,0xA5,0x6E,0xD9,0x84,0x72,0x36,0xA4,0x69,0x54,0x4A,0xD8,0xB9,0x94,0x14};
static const char DOC_ID_5[] = "539a599f3d30819489d19024504647d0c6874a692c97b3539a294a37c195672b";
static const unsigned char DOC_DEF_5[] = {0x3C,0x64,0x69,0x76,0x20,0x63,0x6C,0x61,0x73,0x73,0x3D,0x22,0x65,0x6E,0x74,0x72,0x79,0x22,0x3E,0x3C,0x73,0x70,0x61,0x6E,0x3E,0xE8,0x8B,0xB9,0xE6,0x9E,0x9C,0x3C,0x2F,0x73,0x70,0x61,0x6E,0x3E,0x3C,0x2F,0x64,0x69,0x76,0x3E};
static const unsigned char DOC_NOTICE_5[] = {0x00};
static const unsigned char DOC_PREFIX_5[] = "<!doctype html>\012<html lang=\"zh-CN\">\012<head>\012<meta charset=\"utf-8\">\012<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; img-src https://*.dictres.invalid data: blob:; media-src https://*.dictres.invalid data: blob:; style-src 'unsafe-inline' https://*.dictres.invalid; font-src https://*.dictres.invalid data:; script-src 'unsafe-inline' https://*.dictres.invalid; connect-src 'none'; frame-src 'none'; object-src 'none'; form-action 'none'; base-uri https://*.dictres.invalid;\">\012<base href=\"https://539a599f3d30819489d19024504647d0c6874a692c97b3539a294a37c195672b.dictres.invalid/\">\012<style>";
static const unsigned char DOC_MID_5[] = "</style>\012</head>\012<body class=\"lookup-entry\" data-has-resources=\"1\">\012<div class=\"entry\"><span>\350\213\271\346\236\234</span></div>\012<div class=\"lookup-chips\" id=\"lookupChips\" hidden></div>\012<script>";
static const unsigned char DOC_SUFFIX_5[] = "</script>\012</body>\012</html>";
static const unsigned char DOC_HASH_5[32] = {0xDE,0x43,0x57,0xCD,0xCA,0x0E,0x8E,0x96,0xB3,0x2E,0x5B,0x3B,0x35,0xBB,0x16,0xDA,0x9A,0x52,0xAD,0xDC,0xF6,0xD0,0x1E,0xEF,0xF9,0xBB,0x1E,0xFB,0x62,0x1A,0x23,0x6B};
static const char DOC_ID_6[] = "d3";
static const unsigned char DOC_DEF_6[] = {0x3C,0x61,0x20,0x68,0x72,0x65,0x66,0x3D,0x22,0x65,0x6E,0x74,0x72,0x79,0x3A,0x2F,0x2F,0x61,0x70,0x70,0x6C,0x65,0x22,0x3E,0x61,0x70,0x70,0x6C,0x65,0x3C,0x2F,0x61,0x3E,0x0D,0x0A,0x3C,0x64,0x69,0x76,0x20,0x69,0x64,0x3D,0x27,0x78,0x27,0x3E,0x22,0xE5,0xBC,0x95,0xE5,0x8F,0xB7,0x22,0x3C,0x2F,0x64,0x69,0x76,0x3E};
static const unsigned char DOC_NOTICE_6[] = {0x00};
static const unsigned char DOC_PREFIX_6[] = "<!doctype html>\012<html lang=\"zh-CN\">\012<head>\012<meta charset=\"utf-8\">\012<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; img-src https://*.dictres.invalid data: blob:; media-src https://*.dictres.invalid data: blob:; style-src 'unsafe-inline' https://*.dictres.invalid; font-src https://*.dictres.invalid data:; script-src 'unsafe-inline' https://*.dictres.invalid; connect-src 'none'; frame-src 'none'; object-src 'none'; form-action 'none'; base-uri https://*.dictres.invalid;\">\012<base href=\"https://d3.dictres.invalid/\">\012<style>";
static const unsigned char DOC_MID_6[] = "</style>\012</head>\012<body class=\"lookup-entry\" data-has-resources=\"1\">\012<a href=\"entry://apple\">apple</a>\015\012<div id='x'>\"\345\274\225\345\217\267\"</div>\012<div class=\"lookup-chips\" id=\"lookupChips\" hidden></div>\012<script>";
static const unsigned char DOC_SUFFIX_6[] = "</script>\012</body>\012</html>";
static const unsigned char DOC_HASH_6[32] = {0x03,0x20,0xFC,0xB6,0xC7,0xB7,0x20,0xC8,0x06,0x8D,0x12,0x17,0x2A,0x01,0x45,0x27,0x2D,0x9B,0xD4,0xD7,0x59,0xB5,0x87,0xEE,0x9F,0xB0,0x36,0x7A,0x84,0x0A,0x30,0x99};
static const char DOC_ID_7[] = "d4";
static const unsigned char DOC_DEF_7[] = {0x61,0x00,0x62};
static const unsigned char DOC_NOTICE_7[] = {0x00};
static const unsigned char DOC_PREFIX_7[] = "<!doctype html>\012<html lang=\"zh-CN\">\012<head>\012<meta charset=\"utf-8\">\012<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; img-src https://*.dictres.invalid data: blob:; media-src https://*.dictres.invalid data: blob:; style-src 'unsafe-inline' https://*.dictres.invalid; font-src https://*.dictres.invalid data:; script-src 'unsafe-inline' https://*.dictres.invalid; connect-src 'none'; frame-src 'none'; object-src 'none'; form-action 'none'; base-uri https://*.dictres.invalid;\">\012<base href=\"https://d4.dictres.invalid/\">\012<style>";
static const unsigned char DOC_MID_7[] = "</style>\012</head>\012<body class=\"lookup-entry\" data-has-resources=\"1\">\012a\000b\012<div class=\"lookup-chips\" id=\"lookupChips\" hidden></div>\012<script>";
static const unsigned char DOC_SUFFIX_7[] = "</script>\012</body>\012</html>";
static const unsigned char DOC_HASH_7[32] = {0xC3,0xF6,0x32,0xF0,0x2A,0xF1,0x71,0xD9,0x51,0x07,0x4C,0xF7,0x86,0xA1,0x24,0x0E,0x2A,0x9B,0xAE,0x01,0x2E,0x65,0x31,0x22,0xEB,0xAB,0x63,0x2F,0x80,0xDB,0x01,0xAE};
static const dsh_entry_doc_vector ENTRY_DOC_VECTORS[] = {
  {"plain", "abc123", DOC_DEF_0, 12, DOC_NOTICE_0, 0, 1,
   DOC_PREFIX_0, 551, DOC_MID_0, 146, DOC_SUFFIX_0, 25, DOC_HASH_0},
  {"no-resources", "abc123", DOC_DEF_1, 12, DOC_NOTICE_1, 0, 0,
   DOC_PREFIX_1, 551, DOC_MID_1, 146, DOC_SUFFIX_1, 25, DOC_HASH_1},
  {"notice", "d1", DOC_DEF_2, 0, DOC_NOTICE_2, 22, 1,
   DOC_PREFIX_2, 547, DOC_MID_2, 189, DOC_SUFFIX_2, 25, DOC_HASH_2},
  {"notice-and-def", "d1", DOC_DEF_3, 12, DOC_NOTICE_3, 6, 0,
   DOC_PREFIX_3, 547, DOC_MID_3, 173, DOC_SUFFIX_3, 25, DOC_HASH_3},
  {"both-empty", "d2", DOC_DEF_4, 0, DOC_NOTICE_4, 0, 0,
   DOC_PREFIX_4, 547, DOC_MID_4, 134, DOC_SUFFIX_4, 25, DOC_HASH_4},
  {"hash-id", "539a599f3d30819489d19024504647d0c6874a692c97b3539a294a37c195672b", DOC_DEF_5, 44, DOC_NOTICE_5, 0, 1,
   DOC_PREFIX_5, 609, DOC_MID_5, 178, DOC_SUFFIX_5, 25, DOC_HASH_5},
  {"quotes-and-crlf", "d3", DOC_DEF_6, 61, DOC_NOTICE_6, 0, 1,
   DOC_PREFIX_6, 547, DOC_MID_6, 195, DOC_SUFFIX_6, 25, DOC_HASH_6},
  {"definition-with-nul", "d4", DOC_DEF_7, 3, DOC_NOTICE_7, 0, 1,
   DOC_PREFIX_7, 547, DOC_MID_7, 137, DOC_SUFFIX_7, 25, DOC_HASH_7}
};
#define ENTRY_DOC_VECTOR_COUNT 8

#endif /* DSH_ENTRY_DOC_VECTORS_H */
