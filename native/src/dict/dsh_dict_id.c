/* 见 dsh_dict_id.h —— 内容哈希 id 的实现。 */
#include "dict/dsh_dict_id.h"
#include "dsh_internal.h"
#include "platform/dsh_file.h" /* 读文件走它（UTF-8 → 宽字符 API），别自己 fopen */

#include <stdio.h>
#include <string.h>

void dsh_dict_id_of_bytes(const void *data, size_t len, char out_hex[DSH_DICT_ID_BUF_LEN]) {
  if (out_hex == NULL) return;
  dsh_sha256_hex(data, len, out_hex);
}

int dsh_dict_id_of_file(const char *path, char out_hex[DSH_DICT_ID_BUF_LEN]) {
  if (out_hex == NULL) {
    dsh_set_last_error("dsh_dict_id_of_file：出参不能为空");
    return -1;
  }
  out_hex[0] = '\0';
  if (path == NULL) {
    dsh_set_last_error("dsh_dict_id_of_file：路径不能为空");
    return -1;
  }

  FILE *fp = dsh_file_open_read(path);
  if (fp == NULL) {
    dsh_set_last_error("算词典 id 时打不开文件：%s", path);
    return -1;
  }

  dsh_sha256_ctx ctx;
  dsh_sha256_init(&ctx);
  /* 分块读（64KB）：算一本 100MB 词典的 id，常驻内存只多这 64KB。
   * ⚠️ 别整块读进内存 —— 那会让「导入一本大词典」的峰值内存翻一倍。 */
  static uint8_t chunk[DSH_DICT_ID_CHUNK];
  size_t total = 0;
  for (;;) {
    const size_t got = fread(chunk, 1, sizeof(chunk), fp);
    if (got > 0) {
      dsh_sha256_update(&ctx, chunk, got);
      total += got;
    }
    if (got < sizeof(chunk)) {
      if (ferror(fp)) {
        fclose(fp);
        dsh_set_last_error("算词典 id 时读不动文件（读到 %zu 字节时失败）：%s", total, path);
        return -1;
      }
      break; /* EOF */
    }
  }
  fclose(fp);

  if (total == 0) {
    dsh_set_last_error("算词典 id 时发现文件是空的（0 字节，不是一本词典）：%s", path);
    return -1;
  }

  uint8_t digest[32];
  dsh_sha256_final(&ctx, digest);
  static const char digits[] = "0123456789abcdef";
  for (int i = 0; i < 32; i++) {
    out_hex[i * 2] = digits[(digest[i] >> 4) & 0xFu];
    out_hex[i * 2 + 1] = digits[digest[i] & 0xFu];
  }
  out_hex[DSH_DICT_ID_HEX_LEN] = '\0';
  return 0;
}
