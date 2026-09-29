/* 最小复现：大量分配 + 释放之后，活分配表必须回到 0。 */
#include "dsh_internal.h"
#include "dsh_lookup.h"
#include "mem_registry.h"

#include <stdio.h>
#include <string.h>

int main(void) {
  const size_t base = dsh_mem_live_count();
  printf("基线 %zu\n", base);

  const int N = 250000;
  char *arr[250000];
  for (int i = 0; i < N; i++) {
    arr[i] = dsh_mem_strdup("apples");
    if (arr[i] == NULL) { printf("第 %d 条分配失败\n", i); return 1; }
  }
  printf("分配 %d 条之后在册 %zu\n", N, dsh_mem_live_count());

  for (int i = 0; i < N; i++) dsh_release(arr[i]);
  printf("全部释放之后在册 %zu（期望 0）\n", dsh_mem_live_count());
  if (dsh_mem_live_count() != 0) return 1;
  return 0;
}
