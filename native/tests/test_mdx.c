/* 内核单元测试 · MDict 解析器
 * 检查标准来自**真测试用词典**（参考实现/testdata 下那几本），不是自己造的期望值。
 * ⚠️ 光断言「查询返回 1」不够 —— 一个永远返回命中的实现会全绿：所以每条键都比**规范键名**
 *    （词典里的写法），并且另有一条「不存在的键必须返回 0」的反例。
 */

#include "dict/dsh_mdx.h"
#include "dsh_internal.h"
#include "dsh_lookup.h"
#include "mem_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 测试用词典目录由编译期宏传入（Makefile 里 -DDSH_TESTDATA_DIR=...） */
#ifndef DSH_TESTDATA_DIR
#error "需要 -DDSH_TESTDATA_DIR=<参考实现/testdata 的路径>（见 Makefile）"
#endif

static int g_checks = 0;
static int g_failed = 0;

static void ok(int condition, const char *what) {
  g_checks++;
  if (!condition) {
    g_failed++;
    fprintf(stderr, "FAIL %s\n", what);
  }
}

/**
 * 检查活分配表有没有回到基线，并**把是哪一段漏的写进失败信息**。
 * ⚠️ 只在「所有对象都已关闭」之后调用：词典还开着时它自己持有的键块索引会被当成泄漏报出来。
 * 检查标准是「关闭之后回到基线」，不是「任何时刻都是 0」。
 */
static void ok_no_leak(const char *stage) {
  g_checks++;
  const size_t live = dsh_mem_live_count();
  if (live != 0) {
    g_failed++;
    fprintf(stderr, "FAIL 泄漏于「%s」之后：活分配表还剩 %zu 条（期望 0）\n", stage, live);
  }
}

static void ok_eq_str(const char *actual, const char *expected, const char *what) {
  g_checks++;
  if (actual == NULL || strcmp(actual, expected) != 0) {
    g_failed++;
    fprintf(stderr, "FAIL %s：实际=%s 期望=%s\n", what,
            actual ? actual : "(null)", expected);
  }
}

static void ok_eq_i64(int64_t actual, int64_t expected, const char *what) {
  g_checks++;
  if (actual != expected) {
    g_failed++;
    fprintf(stderr, "FAIL %s：实际=%lld 期望=%lld\n", what,
            (long long)actual, (long long)expected);
  }
}

static char *fixture(const char *name) {
  const size_t n = strlen(DSH_TESTDATA_DIR) + strlen(name) + 2;
  char *p = (char *)dsh_mem_alloc(n);
  if (p == NULL) return NULL;
  snprintf(p, n, "%s/%s", DSH_TESTDATA_DIR, name);
  return p;
}

/* ── 一本词典的头信息 ── */

static void check_info(const char *file, double want_version, int64_t want_keys,
                       int64_t want_blocks, const char *want_encoding,
                       const char *want_title) {
  char *path = fixture(file);
  ok(path != NULL, "测试用词典路径拼接失败");
  if (path == NULL) return;

  dsh_mdx *mdx = NULL;
  const int rc = dsh_mdx_open(path, &mdx);
  if (rc != 0) {
    g_failed++;
    g_checks++;
    fprintf(stderr, "FAIL 打不开 %s：%s\n", file, dsh_last_error_message());
    dsh_release(path);
    return;
  }

  char label[128];
  snprintf(label, sizeof(label), "%s 的版本", file);
  ok_eq_i64((int64_t)(dsh_mdx_version(mdx) * 10.0 + 0.5), (int64_t)(want_version * 10.0 + 0.5), label);
  snprintf(label, sizeof(label), "%s 的词条数", file);
  ok_eq_i64(dsh_mdx_key_count(mdx), want_keys, label);
  snprintf(label, sizeof(label), "%s 的词块数", file);
  ok_eq_i64(dsh_mdx_key_block_count(mdx), want_blocks, label);
  snprintf(label, sizeof(label), "%s 的编码", file);
  ok_eq_str(dsh_mdx_encoding_name(mdx), want_encoding, label);
  snprintf(label, sizeof(label), "%s 的书名", file);
  ok_eq_str(dsh_mdx_title(mdx), want_title, label);
  snprintf(label, sizeof(label), "%s 的加密标志", file);
  ok_eq_i64(dsh_mdx_encrypted(mdx), 0, label);
  snprintf(label, sizeof(label), "%s 的键块序应当单调", file);
  ok(dsh_mdx_block_order_monotone(mdx) == 1, label);

  dsh_mdx_close(mdx);
  dsh_release(path);
}

/* ── 键名逐条比对 ── */

static void check_keys(const char *file, const char *const *want, int64_t want_count) {
  char *path = fixture(file);
  dsh_mdx *mdx = NULL;
  if (path == NULL || dsh_mdx_open(path, &mdx) != 0) {
    g_failed++;
    g_checks++;
    fprintf(stderr, "FAIL 打不开 %s：%s\n", file, dsh_last_error_message());
    if (path != NULL) dsh_release(path);
    return;
  }

  char **keys = NULL;
  int64_t count = 0;
  char label[128];
  snprintf(label, sizeof(label), "%s 枚举全部键", file);
  if (dsh_mdx_list_keys(mdx, &keys, &count) != 0) {
    g_failed++;
    g_checks++;
    fprintf(stderr, "FAIL %s：%s\n", label, dsh_last_error_message());
    dsh_mdx_close(mdx);
    dsh_release(path);
    return;
  }

  snprintf(label, sizeof(label), "%s 的键条数", file);
  ok_eq_i64(count, want_count, label);
  for (int64_t i = 0; i < want_count && i < count; i++) {
    snprintf(label, sizeof(label), "%s 第 %lld 个键", file, (long long)i);
    ok_eq_str(keys[i], want[i], label);
  }
  /* ★ **逐块取键**（`dsh_mdx_block_keys`）与「一次枚举全部」必须是同一份东西：两条路各解析一遍词块
   * 记录格式，迟早在某个编码分支上分叉（大词典不能先物化全书键表，取样只能按块取）；检查标准钉
   * **拼起来逐字相等**，不是「条数对得上」。⚠️ 本节必须排在 `free_keys` 之前，写在释放后会读已还掉的
   * 内存。 */
  {
    const int64_t blocks = dsh_mdx_key_block_count(mdx);
    int64_t seen = 0;
    int same = 1;
    int64_t bi;
    char label2[128];
    ok(blocks > 0, "★ 这本词典至少有一个词块（逐块取键的前提）");
    for (bi = 0; bi < blocks && same; bi++) {
      char **block_keys = NULL;
      int64_t block_count = 0;
      int64_t k;
      if (dsh_mdx_block_keys(mdx, bi, &block_keys, &block_count) != 0) {
        same = 0;
        break;
      }
      for (k = 0; k < block_count; k++) {
        if (seen >= count || strcmp(block_keys[k], keys[seen]) != 0) same = 0;
        seen++;
      }
      dsh_mdx_free_keys(block_keys, block_count);
    }
    snprintf(label2, sizeof(label2), "%s 逐块取键拼起来 == 一次枚举全部", file);
    ok(same && seen == count, label2);
    /* 越界要**如实拒**，不许悄悄给空 */
    {
      char **bad = NULL;
      int64_t bad_count = 0;
      ok(dsh_mdx_block_keys(mdx, blocks, &bad, &bad_count) != 0, "★ 词块下标越界 → 拒");
      ok(dsh_mdx_block_keys(mdx, -1, &bad, &bad_count) != 0, "★ 词块下标 -1 → 拒");
    }
  }
  dsh_mdx_free_keys(keys, count);
  dsh_mdx_close(mdx);
  dsh_release(path);
}

/* ── 键查找 + 内容（含 @@@LINK 重定向）── */

static void check_fetch(const char *file, const char *key, int want_hit,
                        const char *want_landed, const char *want_text_contains) {
  char *path = fixture(file);
  dsh_mdx *mdx = NULL;
  if (path == NULL || dsh_mdx_open(path, &mdx) != 0) {
    g_failed++;
    g_checks++;
    fprintf(stderr, "FAIL 打不开 %s：%s\n", file, dsh_last_error_message());
    if (path != NULL) dsh_release(path);
    return;
  }

  char *landed = NULL;
  char *text = NULL;
  int64_t text_len = 0;
  const int hit = dsh_mdx_fetch(mdx, key, &landed, &text, &text_len);

  char label[192];
  snprintf(label, sizeof(label), "%s 查「%s」的命中结果", file, key);
  ok_eq_i64(hit, want_hit, label);
  if (hit < 0) {
    /* 失败时把内核报出来的原因打出来：只报「实际=-1」没法查，回话要能自己说出现场长什么样。 */
    fprintf(stderr, "      %s 查「%s」时内核报的原因：%s\n", file, key, dsh_last_error_message());
  }

  if (want_hit == 1) {
    snprintf(label, sizeof(label), "%s 查「%s」的规范键名", file, key);
    ok_eq_str(landed, want_landed, label);
    if (want_text_contains != NULL) {
      snprintf(label, sizeof(label), "%s 查「%s」的内容里应包含「%s」", file, key, want_text_contains);
      ok(text != NULL && strstr(text, want_text_contains) != NULL, label);
      if (text == NULL || strstr(text, want_text_contains) == NULL) {
        fprintf(stderr, "      实际内容前 120 字节：%.120s\n", text ? text : "(null)");
      }
    }
  }

  if (landed != NULL) dsh_release(landed);
  if (text != NULL) dsh_release(text);
  dsh_mdx_close(mdx);
  dsh_release(path);
}

int main(void) {
  const size_t base = dsh_mem_live_count();

  /* ── 头信息（对标参考实现测试用词典的实测值）── */
  /* test.mdx 那 6 条是仓库级常量（不许改测试用词典去凑场景），期望值一律对着它写死 */
  static const char *const TEST_KEYS[] = {"apple", "application", "apply", "appreciate",
                                          "banana", "测试"};
  static const char *const LINK_KEYS[] = {"apple", "apples", "ran", "run"};
  static const char *const TITLED_KEYS[] = {"apple", "banana"};
  static const char *const KANA_KEYS[] = {"りんご"};
  static const char *const TALL_KEYS[] = {"tall-a", "tall-b", "tall-c"};

  check_info("test.mdx", 2.0, 6, 1, "UTF-8", "");
  check_info("link.mdx", 2.0, 4, 1, "UTF-8", "");
  check_info("titled.mdx", 2.0, 2, 1, "UTF-8", "有书名的测试词典");
  check_info("kana.mdx", 2.0, 1, 1, "UTF-8", "");
  check_info("tall.mdx", 2.0, 3, 1, "UTF-8", "");
  check_info("audio.mdx", 2.0, 9, 1, "UTF-8", "");
  ok_no_leak("读六本词典的头信息（open + close）");

  check_keys("test.mdx", TEST_KEYS, 6);
  ok_no_leak("枚举 test.mdx 的键");
  check_keys("link.mdx", LINK_KEYS, 4);
  check_keys("titled.mdx", TITLED_KEYS, 2);
  check_keys("kana.mdx", KANA_KEYS, 1);
  check_keys("tall.mdx", TALL_KEYS, 3);
  ok_no_leak("枚举其余四本的键");

  /* ── 逐条取内容 ── */
  check_fetch("test.mdx", "apple", 1, "apple", NULL);
  ok_no_leak("按内容取一条词条（fetch）");
  check_fetch("test.mdx", "banana", 1, "banana", NULL);
  check_fetch("test.mdx", "测试", 1, "测试", NULL);
  /* 大小写：**解析层是大小写敏感的** —— 传 APPLE 查不到 apple。归一化（去标点 + 小写）只用来
   * **挑词块**，块内命中仍按原词逐字符比对；「大小写变体算命中」是产品层 ResolveKey 的事。
   * 这一条钉的就是这个分工 —— 顺手在解析层加上不敏感，会改变「查到」与「查不到」的边界。 */
  check_fetch("test.mdx", "APPLE", 0, NULL, NULL);
  /* 首尾空白：应当能命中（参考实现的两步里第二步） */
  check_fetch("test.mdx", "  apple  ", 1, "apple", NULL);
  /* 不存在的键：必须**如实返回没有**，而不是报错 */
  check_fetch("test.mdx", "zzzz-not-there", 0, NULL, NULL);
  check_fetch("test.mdx", "appl", 0, NULL, NULL);

  /* @@@LINK 重定向：apples 与 ran 是重定向记录（link.mdx 专门为它造的）。
   * 解析层**只如实给出记录内容**（内容就是 `@@@LINK=apple`），「跟到 apple 去」是产品层的事 ——
   * 在解析层抢先跟随会把「记录内容」这个契约弄丢。 */
  check_fetch("link.mdx", "apples", 1, "apples", "@@@LINK=apple");
  check_fetch("link.mdx", "ran", 1, "ran", "@@@LINK=run");
  check_fetch("link.mdx", "apple", 1, "apple", NULL);
  /* 两条重定向记录末尾的 \0 形态不同（一条带、一条不带）—— 这正是测试用词典的设计意图，
   * 两条都要能查到，才说明剥 \0 的处理没写死一种形态。 */
  check_fetch("link.mdx", "run", 1, "run", NULL);

  /* 书名与文件名不同的那本（titled.mdx）：书名只能来自头里的 Title */
  check_fetch("titled.mdx", "apple", 1, "apple", NULL);

  /* UTF-8 的日文键（kana.mdx） */
  check_fetch("kana.mdx", "りんご", 1, "りんご", NULL);
  ok_no_leak("按内容取词条（全部 fetch 用例）");

  /* ── 大词典：big.mdx 的 20 万词条 ── */
  {
    char *path = fixture("big.mdx");
    dsh_mdx *mdx = NULL;
    if (path != NULL && dsh_mdx_open(path, &mdx) == 0) {
      ok_eq_i64(dsh_mdx_key_count(mdx), 200000, "big.mdx 的词条数");
      ok(dsh_mdx_key_block_count(mdx) > 1, "big.mdx 应当有多个词块（否则验证不到二分定位）");

      /* 挑第一个、中间一个、最后一个查回来：这三条一次验掉二分定位 + 块内扫描 + 边界块 */
      char **keys = NULL;
      int64_t count = 0;
      const int list_rc = dsh_mdx_list_keys(mdx, &keys, &count);
      if (list_rc != 0) {
        fprintf(stderr, "      big.mdx 枚举键失败（rc=%d，已枚举 %lld 条）：%s\n",
                list_rc, (long long)count, dsh_last_error_message());
      }
      if (list_rc == 0 && count > 2) {
        ok_eq_i64(count, 200000, "big.mdx 枚举出的键条数");
        const int64_t picks[3] = {0, count / 2, count - 1};
        for (int i = 0; i < 3; i++) {
          char *landed = NULL;
          const int hit = dsh_mdx_lookup_key(mdx, keys[picks[i]], &landed, NULL);
          char label[160];
          snprintf(label, sizeof(label), "big.mdx 第 %lld 个键要能查回来（%s）",
                   (long long)picks[i], keys[picks[i]]);
          ok(hit == 1, label);
          snprintf(label, sizeof(label), "big.mdx 查 %s 的规范键名", keys[picks[i]]);
          ok_eq_str(landed, keys[picks[i]], label);
          if (landed != NULL) dsh_release(landed);
        }
        /* 还清 20 万条键之后，活分配表应当只剩「这本词典自己持有的」那些，**不是 0** —— mdx 还开着。
         * 断言「必须为 0」会误报失败（实测报过 208 条假泄漏）。 */
        const size_t before_free = dsh_mem_live_count();
        dsh_mdx_free_keys(keys, count);
        const size_t after_free = dsh_mem_live_count();
        ok_eq_i64((int64_t)(before_free - after_free), 200001,
                  "还清键数组应当释放 20 万条键 + 那个数组本身");
      } else {
        ok(0, "big.mdx 应该能枚举出 20 万个键");
      }

      /* 不存在的键在大词典里也要如实返回 0 */
      char *landed = NULL;
      ok(dsh_mdx_lookup_key(mdx, "this-key-does-not-exist-12345", &landed, NULL) == 0,
         "big.mdx 里不存在的键必须返回「没有」");
      dsh_mdx_close(mdx);
    } else {
      ok(0, "big.mdx 打不开");
    }
    if (path != NULL) dsh_release(path);
    /* ✅ 真正的泄漏检查标准：**这本词典关掉之后**才该回到 0 */
    ok_no_leak("打开、枚举、查询并关闭 big.mdx（20 万词条）");
  }

  /* ── 横跨记录块边界的记录：**必须拼接两块** —— v2-multiblock 的记录块按字节数均匀切分，参考实现只从
   * 记录起点那一块取字节，截断出来的内容停在半个标签里（没闭合的 entry 开标签），是明确的坏数据。
   * ⚠️ 这一条**故意与参考实现不一致**，别顺手改成跟它一样；检查标准钉「两端完整」：长度 + 闭标签 + 自带 \0。
   */
  {
    char *path = fixture("v2-multiblock.mdx");
    dsh_mdx *mdx = NULL;
    if (path == NULL || dsh_mdx_open(path, &mdx) != 0) {
      ok(0, "v2-multiblock.mdx 应该能打开");
      fprintf(stderr, "      %s\n", dsh_last_error_message());
    } else {
      ok_eq_i64(dsh_mdx_key_block_count(mdx), 3, "v2-multiblock.mdx 的词块数");
      ok_eq_i64(dsh_mdx_record_block_count(mdx), 3, "v2-multiblock.mdx 的记录块数");

      /* (记录下标, 键名, 解码后**字节**数, 是否横跨块) —— 第三个数是 `dsh_mdx_read_record` 的
       * `out_len`，是 **UTF-8 字节数**、不是字符数（中文一个字 3 字节，两种口径会差很多）；标准答案
       * 文件里这两种口径各出现过一次、第一版把它们互换了，所以期望值一律以**运行时实测结果**为准。 */
      struct { int64_t idx; const char *key; int64_t len; int crosses; } cases[] = {
          {2, "apply", 87, 1},
          {4, "banana", 70, 1},
          {1, "application", 90, 0},
          {6, "colour", 79, 0},
      };
      for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *text = NULL;
        int64_t len = 0;
        char label[192];
        const int rc = dsh_mdx_read_record(mdx, cases[i].idx, &text, &len);
        snprintf(label, sizeof(label), "v2-multiblock.mdx 读第 %lld 条（%s）",
                 (long long)cases[i].idx, cases[i].key);
        ok(rc == 0, label);
        if (rc != 0) {
          fprintf(stderr, "      %s\n", dsh_last_error_message());
          continue;
        }
        snprintf(label, sizeof(label),
                 "v2-multiblock.mdx 第 %lld 条（%s）%s的解码后字节数",
                 (long long)cases[i].idx, cases[i].key,
                 cases[i].crosses ? "横跨记录块边界、" : "");
        ok_eq_i64(len, cases[i].len, label);
        /* 两端都完整：开头是完整的标签、结尾是闭合标签 + 记录自带的 \0。
         * 截断的实现会停在半个标签里（参考实现就是那样）。 */
        snprintf(label, sizeof(label), "v2-multiblock.mdx 第 %lld 条（%s）开头完整",
                 (long long)cases[i].idx, cases[i].key);
        ok(strncmp(text, "<div class=\"entry\">", 19) == 0, label);
        snprintf(label, sizeof(label), "v2-multiblock.mdx 第 %lld 条（%s）结尾完整（</div></div> 加 \\0）",
                 (long long)cases[i].idx, cases[i].key);
        ok(len >= 13 && memcmp(text + len - 13, "</div></div>\0", 13) == 0, label);
        dsh_release(text);
      }
      dsh_mdx_close(mdx);
    }
    if (path != NULL) dsh_release(path);
    ok_no_leak("读完整本多块词典（含跨块记录）");
  }

  /* ── 反例：打不开的东西必须**报错**，不能安静地成功 ── */
  {
    dsh_mdx *mdx = NULL;
    ok(dsh_mdx_open("/definitely/not/here.mdx", &mdx) != 0, "不存在的路径必须报错");
    ok(mdx == NULL, "失败时句柄必须保持 NULL");
    ok(dsh_mdx_open(NULL, &mdx) != 0, "空路径必须报错");
    /* 目录不是词典 */
    ok(dsh_mdx_open(DSH_TESTDATA_DIR, &mdx) != 0, "传一个目录必须报错");
  }

  /* 全部还清：活分配表必须回到基线（这一条能暴露解析器的泄漏） */
  ok_eq_i64((int64_t)dsh_mem_live_count(), (int64_t)base,
            "解析器全部关闭之后活分配表必须回到基线（基线应为 0）");

  printf("mdx：%d 项，失败 %d\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
