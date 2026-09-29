/* 临时：把 mdx 内部偏移暴露出来（查完删）。 */
#include "dict/dsh_mdx.h"
#include "dsh_internal.h"
#include <stdio.h>

/* dsh_mdx.c 的结构体定义不在头里，这里按**同样字段顺序**镜像一份，只为读偏移。
 * ⚠️ 字段顺序必须与 dsh_mdx.c 一致，错位读出来的就是别人的内存。 */
typedef struct {
  FILE *fp;
  char *path;
  int is_mdd;
  char *raw_header;
  double version;
  int encrypted;
  char *encoding_name;
  int encoding;
  char *title;
  int num_width;
  int64_t key_block_count;
  int64_t key_count;
  int64_t key_info_unpack_size;
  int64_t key_info_packed_size;
  int64_t key_block_packed_size;
  int64_t record_block_count;
  int64_t record_entries_num;
  int64_t record_info_comp_size;
  int64_t record_block_comp_size;
  dsh_mdx_key_block *key_blocks;
  dsh_mdx_record_block *record_blocks;
  int64_t key_info_offset;
  int64_t key_block_offset;
  int64_t record_info_offset;
  int64_t record_block_offset;
  int block_order_monotone;
  char *warnings[64];
  int warning_count;
} mirror;

int main(void) {
  dsh_mdx *m = NULL;
  if (dsh_mdx_open("testdata/test.mdx", &m) != 0) {
    printf("打开失败：%s\n", dsh_last_error_message());
    return 1;
  }
  mirror *v = (mirror *)m;
  printf("镜像结果：\n");
  printf("  num_width=%d\n", v->num_width);
  printf("  key_info_offset=%lld key_info_packed=%lld\n",
         (long long)v->key_info_offset, (long long)v->key_info_packed_size);
  printf("  key_block_offset=%lld key_block_packed=%lld\n",
         (long long)v->key_block_offset, (long long)v->key_block_packed_size);
  printf("  record_info_offset=%lld\n", (long long)v->record_info_offset);
  printf("  （独立解析说：key_info=658 key_block=700 record_header=773）\n");
  dsh_mdx_close(m);
  return 0;
}
