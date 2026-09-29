/* 内核单元测试 · 内存接口（谁分配、谁释放、放错会怎样）
 * 这是跨语言最容易出人命的地方。不引测试框架（与参考实现的 B 级诊断脚本同一约定），失败非零退出。
 */

#include "dsh_internal.h"
#include "dsh_lookup.h"
#include "mem_registry.h"

#include <stdio.h>
#include <stdlib.h>
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

static void ok_eq_int(long actual, long expected, const char *what) {
  g_checks++;
  if (actual != expected) {
    g_failed++;
    fprintf(stderr, "FAIL %s：实际=%ld 期望=%ld\n", what, actual, expected);
  }
}

static void ok_eq_str(const char *actual, const char *expected, const char *what) {
  g_checks++;
  if (actual == NULL || strcmp(actual, expected) != 0) {
    g_failed++;
    fprintf(stderr, "FAIL %s：实际=%s 期望=%s\n", what, actual ? actual : "(null)", expected);
  }
}

/** size_t 版的相等断言 —— 直接拿 size_t 与 long 比会触发 -Wsign-compare（-Werror 下即错误） */
static void ok_eq_size(size_t actual, size_t expected, const char *what) {
  g_checks++;
  if (actual != expected) {
    g_failed++;
    fprintf(stderr, "FAIL %s：实际=%zu 期望=%zu\n", what, actual, expected);
  }
}

/* ── ABI 与版本 ── */
static void test_version_and_abi(void) {
  ok_eq_int(dsh_abi_version(), DSH_ABI_VERSION, "dsh_abi_version 必须等于头文件里的 DSH_ABI_VERSION");

  const char *v = dsh_version();
  ok(v != NULL, "dsh_version 不能返回 NULL");
  ok_eq_str(v, DSH_VERSION_STRING, "dsh_version 与头文件里的 DSH_VERSION_STRING 必须一致");
  dsh_release((void *)v);
}

/* ── 分配 / 释放 ── */
static void test_alloc_release(void) {
  /* 传 NULL 是合法的空操作，而且**不许**把错误状态改坏 */
  dsh_clear_last_error();
  dsh_release(NULL);
  ok_eq_str(dsh_last_error_message(), "", "dsh_release(NULL) 之后不该有错误");

  /* 普通分配与释放，反复多轮（ASan 下能暴露越界与泄漏） */
  for (int round = 0; round < 3; round++) {
    char *p = dsh_mem_strdup("牛津高阶英汉双解词典（第9版）");
    ok(p != NULL, "dsh_mem_strdup 不该返回 NULL");
    if (p != NULL) {
      ok_eq_str(p, "牛津高阶英汉双解词典（第9版）", "strdup 的内容必须一字不差（UTF-8 多字节要原样保留）");
      dsh_release(p);
    }
  }

  /* 零长度分配也要能用（内部会至少给 1 字节，免得 malloc(0) 的移植差异） */
  void *z = dsh_mem_alloc(0);
  ok(z != NULL, "dsh_mem_alloc(0) 不该返回 NULL");
  dsh_release(z);
}

static void test_release_rejects_foreign_pointer(void) {
  /* 宿主自己 malloc 的内存误传给 dsh_release：必须被**安全拒绝**、也不能动活分配表，而不是段错误。
   * ⚠️ 第一版靠读指针前面的魔数头判断身份，ASan 在这里报了 heap-buffer-overflow（读到了不属于我们的内存）。 */
  void *foreign = malloc(64);
  ok(foreign != NULL, "测试自身的 malloc 失败（环境问题）");
  if (foreign == NULL) return;

  const size_t live_before = dsh_mem_live_count();
  dsh_clear_last_error();
  dsh_release(foreign); /* 必须不崩、也不许动这块内存 */
  const char *msg = dsh_last_error_message();
  ok(msg != NULL && strstr(msg, "不是内核当前分配的") != NULL,
     "释放外来指针时必须记下『不是内核当前分配的』这个原因");
  dsh_release((void *)msg);
  ok_eq_size(dsh_mem_live_count(), live_before,
             "拒绝释放外来指针不该改动活分配表");
  free(foreign); /* 它仍归测试所有，由测试自己释放 */

  /* 而且这个拒绝不许污染后续的正常释放 */
  char *mine = dsh_mem_strdup("x");
  ok(mine != NULL, "外来指针被拒之后，正常分配仍要成功");
  dsh_release(mine);
}

static void test_double_release_is_refused(void) {
  /* 同一块释放两次：第二次必须被拒绝并记原因，**不能**二次 free（glibc 下多半直接 abort）。 */
  char *p = dsh_mem_strdup("双重释放");
  ok(p != NULL, "分配失败（环境问题）");
  if (p == NULL) return;
  dsh_release(p);
  dsh_clear_last_error();
  dsh_release(p); /* 同一地址再一次 */
  const char *msg = dsh_last_error_message();
  ok(msg != NULL && strstr(msg, "不在活分配表里") != NULL,
     "重复释放必须被拒绝，并说清『不在活分配表里』");
  dsh_release((void *)msg);
}

static void test_no_leak_from_api_results(void) {
  /* 接口定义那句「内核分配、宿主用 dsh_release 还」的可执行版本：还清之后活分配表必须回到 0。 */
  const size_t base = dsh_mem_live_count();
  const char *v = dsh_version();
  const char *e = dsh_last_error_message();
  ok(v != NULL && e != NULL, "两个返回值都不该是 NULL");
  ok(dsh_mem_live_count() > base, "取回结果之后活分配表里应该有它们");
  dsh_release((void *)v);
  dsh_release((void *)e);
  ok_eq_size(dsh_mem_live_count(), base,
             "还清之后活分配表必须回到基线（否则就是泄漏）");
}

/* ── 错误信息 ── */
static void test_last_error(void) {
  /* 每次取回来的都是**副本**：宿主释放它不该影响内核里那份 */
  dsh_clear_last_error();
  const char *a = dsh_last_error_message();
  ok_eq_str(a, "", "清掉之后必须返回空串，而不是上一次的旧错误");
  dsh_release((void *)a);

  dsh_set_last_error("测试错误 %d", 42);
  const char *b = dsh_last_error_message();
  ok_eq_str(b, "测试错误 42", "错误信息要按格式串渲染");
  dsh_release((void *)b);

  /* 两次取回是两份独立内存（各自可释放） */
  const char *c = dsh_last_error_message();
  const char *d = dsh_last_error_message();
  ok(c != d, "两次取回必须是各自独立的内存（否则宿主会重复释放同一块）");
  dsh_release((void *)c);
  dsh_release((void *)d);

  dsh_clear_last_error();
}

int main(void) {
  test_version_and_abi();
  test_alloc_release();
  test_release_rejects_foreign_pointer();
  test_double_release_is_refused();
  test_no_leak_from_api_results();
  test_last_error();

  printf("core：%d 项，失败 %d\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
