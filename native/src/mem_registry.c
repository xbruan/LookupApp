/* 活分配登记表 —— 实现「内核分配、内核释放」那条承诺的唯一手段。
 * 为什么需要它：C 里没有可移植的办法给任意指针验身份 —— 想在指针前面放个魔数，
 * 可**外来指针**前面那段内存不属于我们，读它就是越界，所以只能登记在册。
 * 为什么是可增长的链式哈希：表一满就悄悄不登记，那些指针释放时会被当成外来的而拒绝释放
 * → 全部泄漏，且不报错、不崩、只是内存一直涨；所以按需扩容，并把登记失败变成可见的失败。
 * 代价是每次分配/释放各一次 O(1) 哈希 + 一把锁。`dsh_release` 只跑在对外接口的返回值上、
 * 不在热路径，可以接受；将来若真成瓶颈，正解是只登记要交给宿主的那些，而不是去掉登记。 */

#include "mem_registry.h"
#include "portability.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct dsh_reg_node {
  void *ptr;
  size_t size;
  struct dsh_reg_node *next;
} dsh_reg_node;

static dsh_reg_node **g_buckets; /* 桶数组，每个桶是一条链 */
static size_t g_bucket_count;
static size_t g_live;
static int g_initialized;
static dsh_mutex g_lock;

#define DSH_REG_INIT_BUCKETS 1024u
#define DSH_REG_LOAD_NUM 3u /* 装填因子 3/4 就扩容 */
#define DSH_REG_LOAD_DEN 4u

/** 32 位 FNV-1a。地址低位的对齐规律很强，先混一遍再掩码。 */
static size_t hash_of(const void *ptr) {
  uintptr_t v = (uintptr_t)ptr;
  uint32_t h = 2166136261u;
  for (int i = 0; i < (int)sizeof(void *); i++) {
    h ^= (uint32_t)((v >> (i * 8)) & 0xFFu);
    h *= 16777619u;
  }
  return (size_t)h;
}

/**
 * 惰性初始化。
 *
 * ⚠️ **必须在 `dsh_mutex_lock` 之前调用** —— 互斥量本身要先初始化才能上锁。
 * 顺序写反时 Linux 恰好照常工作（全零就是 `PTHREAD_MUTEX_INITIALIZER`），
 * Windows 上却是一调用就 AccessViolation —— 改动加锁顺序时先看这里。
 */
static void ensure_initialized_locked(void) {
  dsh_mutex_init(&g_lock);
  g_bucket_count = DSH_REG_INIT_BUCKETS;
  g_buckets = (dsh_reg_node **)calloc(g_bucket_count, sizeof(dsh_reg_node *));
  if (g_buckets == NULL) g_bucket_count = 0;
  g_live = 0;
  g_initialized = 1;
}

/** 进任何登记表操作前调它：保证锁已初始化。幂等、且不必持锁（初始化只发生一次）。 */
static void ensure_initialized(void) {
  if (g_initialized) return;
  ensure_initialized_locked();
}

/** 扩容并重挂所有节点。调用方必须持锁。返回 0 表示失败（保持原样）。 */
static int grow_locked(void) {
  const size_t next_count = g_bucket_count * 2;
  dsh_reg_node **next = (dsh_reg_node **)calloc(next_count, sizeof(dsh_reg_node *));
  if (next == NULL) return 0;
  for (size_t i = 0; i < g_bucket_count; i++) {
    dsh_reg_node *n = g_buckets[i];
    while (n != NULL) {
      dsh_reg_node *next_node = n->next;
      const size_t b = hash_of(n->ptr) & (next_count - 1);
      n->next = next[b];
      next[b] = n;
      n = next_node;
    }
  }
  free(g_buckets);
  g_buckets = next;
  g_bucket_count = next_count;
  return 1;
}

int dsh_mem_register(void *ptr, size_t size) {
  if (ptr == NULL) return 1; /* 不登记 NULL —— 释放 NULL 是接口定义里的空操作 */
  ensure_initialized();       /* ⚠️ 必须在 lock 之前（见 ensure_initialized 的注释） */
  dsh_mutex_lock(&g_lock);
  if (g_buckets == NULL) {
    dsh_mutex_unlock(&g_lock);
    return 0;
  }
  /* 装填因子到 3/4 就扩。扩不动（内存不足）不致命：继续用旧表，链会变长但正确。 */
  if ((g_live + 1) * DSH_REG_LOAD_DEN > g_bucket_count * DSH_REG_LOAD_NUM) {
    (void)grow_locked();
  }
  const size_t b = hash_of(ptr) & (g_bucket_count - 1);
  for (dsh_reg_node *n = g_buckets[b]; n != NULL; n = n->next) {
    if (n->ptr == ptr) {
      /* 同一地址登记两次：说明上一次同地址的释放没走注销，或者 malloc 复用了地址。
       * 两种都是内核的 bug —— 但**不要**在这里悄悄改大小，那会把确定的错误变成飘忽的错误。 */
      dsh_mutex_unlock(&g_lock);
      return 1;
    }
  }
  dsh_reg_node *node = (dsh_reg_node *)malloc(sizeof(dsh_reg_node));
  if (node == NULL) {
    dsh_mutex_unlock(&g_lock);
    return 0; /* 登记不上 → 调用方必须把这次分配**当成失败**（见 mem_registry.h） */
  }
  node->ptr = ptr;
  node->size = size;
  node->next = g_buckets[b];
  g_buckets[b] = node;
  g_live++;
  dsh_mutex_unlock(&g_lock);
  return 1;
}

int dsh_mem_unregister(void *ptr, size_t *out_size) {
  if (ptr == NULL) return 0;
  ensure_initialized();       /* ⚠️ 必须在 lock 之前 */
  dsh_mutex_lock(&g_lock);
  if (g_buckets == NULL) {
    dsh_mutex_unlock(&g_lock);
    return 0;
  }
  const size_t b = hash_of(ptr) & (g_bucket_count - 1);
  dsh_reg_node **link = &g_buckets[b];
  while (*link != NULL) {
    dsh_reg_node *n = *link;
    if (n->ptr == ptr) {
      if (out_size != NULL) *out_size = n->size;
      *link = n->next;
      free(n);
      if (g_live > 0) g_live--;
      dsh_mutex_unlock(&g_lock);
      return 1;
    }
    link = &n->next;
  }
  dsh_mutex_unlock(&g_lock);
  return 0;
}

size_t dsh_mem_live_count(void) {
  ensure_initialized();       /* ⚠️ 必须在 lock 之前 */
  dsh_mutex_lock(&g_lock);
  const size_t n = g_live;
  dsh_mutex_unlock(&g_lock);
  return n;
}
