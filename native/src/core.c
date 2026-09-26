/* 内核内存层 —— 「内核分配、内核释放」那条规矩的唯一落点。
 * C 里没有可移植的办法给任意指针验身份：想在每次分配前面放个魔数头、靠读指针前面
 * 那几字节来判断这块是不是内核给的，可**外来指针前面那段内存不属于我们**，读它就是越界。
 * 所以改用活分配登记表（mem_registry.c）：分配时登记，`dsh_release` 先查册、
 * 在册才 free 并注销，不在册就拒绝并记下原因 —— 全程不读任何不属于自己的字节。 */

#include "dsh_lookup.h"
#include "dsh_internal.h"
#include "mem_registry.h"
#include "portability.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── 错误信息 ─────────────────────────────────────────────────────────────
 * 目前是每线程一份。所有接口都跑在宿主的一个线程上，所以先用最简单的形式；
 * 将来若要真并发，这里换成线程局部存储即可 —— 业务代码一个字都不用改。 */
static char g_last_error[512];

void dsh_set_last_error(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_last_error, sizeof(g_last_error), fmt, ap);
  va_end(ap);
}

void dsh_clear_last_error(void) { g_last_error[0] = '\0'; }

const char *dsh_last_error_message(void) {
  const char *text = g_last_error[0] ? g_last_error : "";
  char *copy = dsh_mem_strdup(text);
  if (copy == NULL) dsh_set_last_error("内存不足：无法复制错误信息");
  return copy;
}

/* ── 分配与释放 ─────────────────────────────────────────────────────────── */

void *dsh_mem_alloc(size_t size) {
  const size_t n = size ? size : 1; /* malloc(0) 的行为各平台不同，统一给 1 字节 */
  void *p = malloc(n);
  if (p == NULL) return NULL;
  /* ⚠️ 登记失败必须**当成分配失败**：没登记上的指针在 dsh_release 那边会被
   * 当成外来的而拒绝释放 —— 那就是一次悄悄泄漏。 */
  if (!dsh_mem_register(p, n)) {
    free(p);
    dsh_set_last_error("内存不足：活分配登记表登记不上（分配的块已回收）");
    return NULL;
  }
  return p;
}

char *dsh_mem_strdup(const char *text) {
  if (text == NULL) text = "";
  const size_t len = strlen(text);
  char *copy = (char *)dsh_mem_alloc(len + 1);
  if (copy == NULL) return NULL;
  memcpy(copy, text, len + 1);
  return copy;
}

void dsh_release(void *ptr) {
  if (ptr == NULL) return; /* 接口定义：传 NULL 是合法的空操作 */

  size_t size = 0;
  if (!dsh_mem_unregister(ptr, &size)) {
    /* 不在册 —— 不是内核给的（或已经被释放过一次）。**绝不去读它**。
     * 拒绝释放并如实记下原因：这比段错误好查一个数量级，尤其宿主是隔着 P/Invoke 的场景。 */
    dsh_set_last_error("dsh_release 收到的指针不是内核当前分配的（不在活分配表里），已拒绝释放");
    return;
  }
  free(ptr);
}

/* ── core 组的其余接口 ──────────────────────────────────────────────────── */

int32_t dsh_abi_version(void) { return DSH_ABI_VERSION; }

const char *dsh_version(void) { return dsh_mem_strdup(DSH_VERSION_STRING); }
