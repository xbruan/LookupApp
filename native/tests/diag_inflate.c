/* 临时：直接解压词块那 73 字节，看解出来是不是 97 字节的键数据。 */
#include "compress/dsh_inflate.h"
#include "dsh_lookup.h"
#include <stdio.h>
#include <string.h>

int main(void) {
  const char *path = "testdata/test.mdx";
  FILE *fp = fopen(path, "rb");
  if (fp == NULL) { printf("打不开\n"); return 1; }
  /* 词块在 700，长 73；前 8 字节是 {压缩类型, adler32} */
  unsigned char raw[73];
  fseek(fp, 700, SEEK_SET);
  if (fread(raw, 1, 73, fp) != 73) { printf("读失败\n"); return 1; }
  fclose(fp);
  printf("磁盘前 12 字节: ");
  for (int i = 0; i < 12; i++) printf("%02x ", raw[i]);
  printf("\n");

  unsigned char *out = NULL;
  size_t n = 0;
  const int rc = dsh_zlib_inflate(raw + 8, 73 - 8, 97, &out, &n);
  printf("rc=%d out=%p n=%zu\n", rc, (void *)out, n);
  if (rc != 0) { printf("last_error=%s\n", dsh_last_error_message()); return 1; }
  printf("解出前 24 字节: ");
  for (size_t i = 0; i < 24 && i < n; i++) printf("%02x ", out[i]);
  printf("\n");
  printf("可读形式: %.96s\n", (const char *)out);
  int nonzero = 0;
  for (size_t i = 0; i < n; i++) if (out[i] != 0) nonzero++;
  printf("非零字节数=%d / %zu\n", nonzero, n);
  dsh_release(out);
  return 0;
}
