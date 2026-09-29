/* 临时：独立复现 —— 只读文件 + 调 inflate，不经过任何解析器代码；
 * 期望值用系统 zlib 现算（python3）给出。 */
#include "compress/dsh_inflate.h"
#include "dsh_lookup.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1]
      : "testdata/test.mdx";
  const long off = argc > 2 ? atol(argv[2]) : 700;
  const long len = argc > 3 ? atol(argv[3]) : 73;
  const long want = argc > 4 ? atol(argv[4]) : 97;

  FILE *fp = fopen(path, "rb");
  if (fp == NULL) { printf("打不开\n"); return 1; }
  unsigned char *raw = malloc((size_t)len);
  fseek(fp, off, SEEK_SET);
  if ((long)fread(raw, 1, (size_t)len, fp) != len) { printf("读失败\n"); return 1; }
  fclose(fp);

  printf("磁盘[%ld..%ld) 前 12 字节: ", off, off + len);
  for (int i = 0; i < 12; i++) printf("%02x ", raw[i]);
  printf("\n");

  unsigned char *out = NULL;
  size_t n = 0;
  const int rc = dsh_zlib_inflate(raw + 8, (size_t)(len - 8), (size_t)want, &out, &n);
  printf("rc=%d n=%zu（期望 %ld）\n", rc, n, want);
  if (rc != 0) { printf("last_error=%s\n", dsh_last_error_message()); return 1; }
  printf("前 16 字节: ");
  for (size_t i = 0; i < 16 && i < n; i++) printf("%02x ", out[i]);
  printf("\n");
  printf("第 8..24 字节: ");
  for (size_t i = 8; i < 24 && i < n; i++) printf("%02x ", out[i]);
  printf("\n");
  dsh_release(out);
  free(raw);
  return 0;
}
