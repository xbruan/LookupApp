/* 临时诊断（不进交付物）：并排打印库算出的偏移与这里**独立解析**出的偏移，看是谁错了。 */
#include "dict/dsh_mdx.h"
#include "dsh_internal.h"
#include "dsh_lookup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int64_t rd(const unsigned char *b, size_t off, int w) {
  int64_t v = 0;
  for (int i = 0; i < w; i++) v = (v << 8) | b[off + i];
  return v;
}

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1]
      : "testdata/test.mdx";   /* 相对当前目录；也可以给一个路径参数 */

  /* ── 独立解析（算法同 tools/probe-mdx-layout.mjs，但这里独立写成 C，不调库）── */
  FILE *fp = fopen(path, "rb");
  if (fp == NULL) { printf("打不开 %s\n", path); return 1; }
  fseek(fp, 0, SEEK_END);
  const long size = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  unsigned char *raw = malloc((size_t)size);
  if (fread(raw, 1, (size_t)size, fp) != (size_t)size) { printf("读失败\n"); return 1; }
  fclose(fp);

  const int64_t hlen = rd(raw, 0, 4);
  const int64_t header_end = hlen + 8;
  const int w = 8; /* 由版本决定；下面从头部属性读 */
  const int64_t kbc = rd(raw, header_end, w);
  const int64_t kc = rd(raw, header_end + w, w);
  const int64_t kiu = rd(raw, header_end + 2 * w, w);
  const int64_t kip = rd(raw, header_end + 3 * w, w);
  const int64_t kbp = rd(raw, header_end + 4 * w, w);
  const int64_t key_info_start = header_end + 8 * 5 + 4;
  const int64_t key_block_start = key_info_start + kip;
  const int64_t record_header_start = key_block_start + kbp;

  printf("── 独立解析 ──\n");
  printf("header_len=%lld header_end=%lld\n", (long long)hlen, (long long)header_end);
  printf("key_block_count=%lld key_count=%lld key_info_unpack=%lld key_info_packed=%lld key_block_packed=%lld\n",
         (long long)kbc, (long long)kc, (long long)kiu, (long long)kip, (long long)kbp);
  printf("key_info_start=%lld  key_block_start=%lld  record_header_start=%lld  文件大小=%ld\n",
         (long long)key_info_start, (long long)key_block_start,
         (long long)record_header_start, size);
  printf("key_info 头 8 字节: %02x %02x %02x %02x | %02x %02x %02x %02x\n",
         raw[key_info_start], raw[key_info_start + 1], raw[key_info_start + 2], raw[key_info_start + 3],
         raw[key_info_start + 4], raw[key_info_start + 5], raw[key_info_start + 6], raw[key_info_start + 7]);
  printf("key_block 头 8 字节: %02x %02x %02x %02x | %02x %02x %02x %02x\n",
         raw[key_block_start], raw[key_block_start + 1], raw[key_block_start + 2], raw[key_block_start + 3],
         raw[key_block_start + 4], raw[key_block_start + 5], raw[key_block_start + 6], raw[key_block_start + 7]);
  printf("key_block 头 16 字节: ");
  for (int i = 0; i < 16; i++) printf("%02x ", raw[key_block_start + i]);
  printf("\n");

  /* ── 库 ── */
  dsh_mdx *m = NULL;
  printf("\n── 库 ──\n");
  if (dsh_mdx_open(path, &m) != 0) {
    printf("打开失败：%s\n", dsh_last_error_message());
    free(raw);
    return 1;
  }
  printf("version=%.2f keys=%lld blocks=%lld enc=%s\n", dsh_mdx_version(m),
         (long long)dsh_mdx_key_count(m), (long long)dsh_mdx_key_block_count(m),
         dsh_mdx_encoding_name(m));

  dsh_mdx_key_block info;
  dsh_mdx_key_block_at(m, 0, &info);
  printf("key_block[0]: entry_count=%lld first=%s last=%s pack=%lld unpack=%lld\n",
         (long long)info.entry_count, info.first_key, info.last_key,
         (long long)info.pack_size, (long long)info.unpack_size);

  uint8_t *buf = NULL;
  size_t blen = 0;
  if (dsh_mdx_read_key_block(m, 0, &buf, &blen) != 0) {
    printf("读词块失败：%s\n", dsh_last_error_message());
  } else {
    printf("库读到的词块前 16 字节: ");
    for (size_t i = 0; i < 16 && i < blen; i++) printf("%02x ", buf[i]);
    printf("  (%zu 字节)\n", blen);
    /* 与文件里那个位置的字节比一比 —— 如果一致，说明读的位置对但解错了 */
    printf("文件里该位置的前 16 字节: ");
    for (int i = 0; i < 16; i++) printf("%02x ", raw[key_block_start + i]);
    printf("\n");
    dsh_release(buf);
  }

  /* 头部属性（看 Encoding 与版本是怎么解出来的） */
  printf("title=[%s]\n", dsh_mdx_title(m));
  printf("warnings=%d\n", dsh_mdx_warning_count(m));
  for (int i = 0; i < dsh_mdx_warning_count(m); i++) printf("  warn: %s\n", dsh_mdx_warning_at(m, i));
  dsh_mdx_close(m);
  free(raw);
  return 0;
}
