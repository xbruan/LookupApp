/* ==========================================================================
 * 内核单元测试 · 查词历史（`history.jsonl` 那一层）
 *
 * 契约对在 `store/dsh_history.h` 顶上那段。这一组钉五类事：① 存取（记一条 / 按页取 / 清空，
 * 关掉再打开数据还在）；② 队头 5 分钟去重窗口 + 超上限从最旧那头砍；③ 去重键是 (词, 词典)；
 * ④ **查到了才记**（走 `dsh_engine_lookup`），跟过 `@@@LINK` 记的是**落点的规范键名**；
 * ⑤ 落盘形态：写放大 O(1)（记一条写出去的字节数不随历史长短变）、进程被杀不丢、
 * 不留 `.tmp` 残骸、一行坏行不毒死整份历史且跳过的条数要说出来、压缩真的会把文件收回去。
 *
 * ⚠️ 这一组**不碰用户的配置目录**：全程在 `DSH_TESTDATA_DIR` 旁边的临时目录里建文件。
 * ========================================================================== */

#include "dsh_lookup.h"
#include "dsh_internal.h"
#include "mem_registry.h"
#include "platform/dsh_file.h"
#include "store/dsh_history.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_checks = 0;
static int g_failed = 0;

static void ok(int cond, const char *what) {
  g_checks++;
  if (cond) return;
  g_failed++;
  fprintf(stderr, "FAIL %s\n", what);
}

static void ok_eq_str(const char *actual, const char *expected, const char *what) {
  g_checks++;
  if (actual != NULL && expected != NULL && strcmp(actual, expected) == 0) return;
  g_failed++;
  fprintf(stderr, "FAIL %s\n      实际=%s\n      期望=%s\n", what, actual != NULL ? actual : "(null)",
          expected != NULL ? expected : "(null)");
}

static int has(const char *haystack, const char *needle) {
  return haystack != NULL && strstr(haystack, needle) != NULL;
}

/** 建目录（测试的临时库要放在一个真的存在的目录里 —— 打开文件不会替你建目录）*/
static void make_dir(const char *path) { mkdir(path, 0777); }

/**
 * 把这一组用过的临时文件**擦干净**。
 *
 * ⚠️ 测试用词典目录（`testdata/`）是**冻结**的（那儿有 SHA256 清单），测试不许在它里面留东西。
 * 一并清几个老名字只是防残留；`history.jsonl.tmp` 是压缩被打断时会留下的那一个。
 */
static void cleanup_dir(const char *dir) {
  static const char *names[] = {"history.jsonl",  "history.jsonl.tmp", "lookup.db",
                                "lookup.db-wal",  "lookup.db-shm",     "settings.json"};
  char path[2048];
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
    remove(path);
  }
  rmdir(dir); /* 空目录才删得掉；里面还有别的东西就留着（如实，不递归删）*/
}

/** 文件在不在 */
static int file_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

/** 文件多少字节（不在就是 -1）*/
static long file_size(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) return -1;
  return (long)st.st_size;
}

/** 文件里有多少行（数 `\n`；空文件是 0） */
static long count_lines(const char *path) {
  FILE *fp = fopen(path, "rb");
  if (fp == NULL) return -1;
  long lines = 0;
  int c;
  while ((c = fgetc(fp)) != EOF) {
    if (c == '\n') lines++;
  }
  fclose(fp);
  return lines;
}

/** 往文件尾巴上追加一段原样字节（造「坏行」现场用）*/
static void append_raw(const char *path, const char *text) {
  FILE *fp = fopen(path, "ab");
  if (fp == NULL) return;
  fputs(text, fp);
  fclose(fp);
}

/** 取一个字符串字段的值（找该键名后那对引号，读到下一个引号为止）。
 *  只给**断言失败时把实际值打出来**用 —— 检查标准仍然钉 JSON 里那一段原文。 */
static void json_field(const char *json, const char *key, char *out, size_t cap) {
  out[0] = '\0';
  if (json == NULL || cap == 0) return;
  char pat[64];
  snprintf(pat, sizeof(pat), "\"%s\":\"", key);
  const char *at = strstr(json, pat);
  if (at == NULL) return;
  at += strlen(pat);
  size_t n = 0;
  while (at[n] != '\0' && at[n] != '"' && n + 1 < cap) n++;
  memcpy(out, at, n);
  out[n] = '\0';
}

/** 临时目录（`DSH_TESTDATA_DIR` 是测试用词典目录，测试的临时文件放在它旁边的 `tmp-hist/` 下） */
static char *tmp_db(const char *name) {  const char *fixtures = DSH_TESTDATA_DIR;
  const size_t need = strlen(fixtures) + strlen(name) + 32;
  char *dir = (char *)malloc(need);
  if (dir == NULL) return NULL;
  snprintf(dir, need, "%s/tmp-hist", fixtures);
  make_dir(dir); /* 已经有了也不会出错（mkdir 回 EEXIST，这里不看返回值）*/
  /* 目录可能不存在：`mkdir -p` 由测试脚本保证 —— 这里只拼路径，失败会由 open 报出来 */
  char *path = (char *)malloc(need + 16);
  if (path != NULL) snprintf(path, need + 16, "%s/%s", dir, name);
  free(dir);
  return path;
}

/** 从 JSON 里取一个字符串字段的第 n 次出现（够用的极简取值器） */
static void nth_field(const char *json, const char *key, int n, char *buf, size_t cap) {
  buf[0] = '\0';
  if (json == NULL) return;
  char pat[64];
  snprintf(pat, sizeof(pat), "\"%s\":\"", key);
  const char *p = json;
  for (int i = 0; i <= n; i++) {
    p = strstr(p, pat);
    if (p == NULL) return;
    p += strlen(pat);
  }
  size_t k = 0;
  while (*p != '\0' && *p != '"' && k + 1 < cap) buf[k++] = *p++;
  buf[k] = '\0';
}

int main(void) {
  const size_t base = dsh_mem_live_count();
  /* ⚠️ 这个文件**不能叫 `history.jsonl`**：⑤ 会在同一个目录里建引擎，而引擎用的正是
   *    `<配置目录>/history.jsonl`，同名就会被当成「已有的历史」读到（断言当场红）。
   *    存储层测试用**它自己挑的路径**，换个名字就与产品路径互不干扰。 */
  char *hist_path = tmp_db("store.jsonl");
  ok(hist_path != NULL, "① 拼出临时历史文件路径");
  if (hist_path == NULL) return 1;
  remove(hist_path);

  /* ══ ① 存取：记、取、清 ═════════════════════════════════════════════════ */
  {
    dsh_history *h = NULL;
    ok(dsh_hist_open(hist_path, &h) == 0 && h != NULL, "① 打开（库不存在就建）");
    if (h != NULL) {
      int64_t total = -1;
      ok(dsh_hist_count(h, &total) == 0 && total == 0, "① 新库是空的");

      const int64_t t0 = 1700000000000LL;
      ok(dsh_hist_push(h, "apple", "d-one", "第一本", t0) == 0, "① 记一条");
      ok(dsh_hist_push(h, "banana", "d-one", "第一本", t0 + 1000) == 0, "① 再记一条");
      ok(dsh_hist_count(h, &total) == 0 && total == 2, "① 两条都在");

      char *json = NULL;
      ok(dsh_hist_query(h, 0, 0, &json) == 0 && json != NULL, "① 按页取");
      {
        char word[64];
        nth_field(json, "word", 0, word, sizeof(word));
        ok_eq_str(word, "banana", "① ★ 最近查的排在最前面（不是最早那条）");
        nth_field(json, "word", 1, word, sizeof(word));
        ok_eq_str(word, "apple", "① 第二条是早些时候那条");
        ok(has(json, "\"total\":2"), "① 总条数给了");
        ok(has(json, "\"hasMore\":false"), "① 一页装得下 → hasMore 为假");
      }
      if (json != NULL) dsh_release(json);

      /* 关掉再打开：**真的落盘了**（这一条把「只在内存里活」那种误报通过挡在门外） */
      dsh_hist_close(h);
      h = NULL;
      ok(dsh_hist_open(hist_path, &h) == 0 && h != NULL, "① 关掉再打开");
      if (h != NULL) {
        ok(dsh_hist_count(h, &total) == 0 && total == 2, "① ★ 重开之后数据还在（真的落盘）");
        json = NULL;
        ok(dsh_hist_query(h, 0, 0, &json) == 0 && has(json, "apple") && has(json, "第一本"),
           "① 词典名快照也在");
        if (json != NULL) dsh_release(json);

        /* 清空 */
        int64_t after = -1;
        ok(dsh_hist_clear(h, &after) == 0 && after == 0, "① 清空");
        ok(dsh_hist_count(h, &total) == 0 && total == 0, "① 清空之后是 0 条");
        dsh_hist_close(h);
      }
    }
  }

  /* ══ ② 两条产品规则（去重窗口 / 上限）═══════════════════════════════════ */
  {
    dsh_history *h = NULL;
    ok(dsh_hist_open(hist_path, &h) == 0 && h != NULL, "② 打开（复用一个库）");
    if (h != NULL) {
      int64_t total = 0;
      (void)dsh_hist_clear(h, NULL);

      const int64_t t0 = 1700000000000LL;
      ok(dsh_hist_push(h, "apple", "d-one", "第一本", t0) == 0, "② 记一条");
      ok(dsh_hist_push(h, "banana", "d-one", "第一本", t0 + 1000) == 0, "② 换一个词再记");
      /* ① 队头去重窗口：**同一个 (词, 词典) 又回到队头**、而且不到 5 分钟 → 不记 */
      ok(dsh_hist_push(h, "banana", "d-one", "第一本", t0 + 2000) == 0 &&
         dsh_hist_count(h, &total) == 0 && total == 2,
         "② ★ 同一个词在 5 分钟之内又落到队头 → **不重复记**");
      /* 过了窗口 → 记下来。⚠️ 总数**还是 2**：记一条不是「追加一条」，而是把同 (词, 词典)
       * 的旧记录换成新的那一条（参考实现先 `RemoveAll` 再 `Insert(0)`）。 */
      ok(dsh_hist_push(h, "banana", "d-one", "第一本", t0 + 6 * 60 * 1000) == 0 &&
         dsh_hist_count(h, &total) == 0 && total == 2,
         "② ★ 过了 5 分钟窗口 → 记下来（去重是对**窗口**说的，不是永久的）");
      {
        char *json = NULL;
        ok(dsh_hist_query(h, 0, 0, &json) == 0 && json != NULL, "② 取一页看顺序");
        char word[64];
        nth_field(json, "word", 0, word, sizeof(word));
        ok_eq_str(word, "banana", "② 重新记的那条回到了最前面");
        if (json != NULL) dsh_release(json);
      }

      /* 上限压到 3，再记到**第 4 条**（`cherry` 是第 3 条、`date` 是第 4 条）。
       * ⚠️ 要**真的超过**上限才会砍 —— 正好等于上限那一下一条都不砍。 */
      dsh_hist_set_limit(h, 3);
      ok(dsh_hist_push(h, "cherry", "d-one", "第一本", t0 + 7 * 60 * 1000) == 0 &&
         dsh_hist_count(h, &total) == 0 && total == 3,
         "② 记到第 3 条（正好等于上限 → 一条都不砍）");
      ok(dsh_hist_push(h, "date", "d-one", "第一本", t0 + 8 * 60 * 1000) == 0, "② 记第 4 条");
      ok(dsh_hist_count(h, &total) == 0 && total == 3, "② ★ 超过上限 → 砍到上限条数");
      {
        char *json = NULL;
        ok(dsh_hist_query(h, 0, 0, &json) == 0 && json != NULL, "② 取一页");
        ok(!has(json, "\"word\":\"apple\""), "② ★ 砍掉的是**最旧**那条（apple 没了）");
        ok(has(json, "\"word\":\"date\"") && has(json, "\"word\":\"cherry\""),
           "② 新的那两条都还在");
        if (json != NULL) dsh_release(json);
      }
      dsh_hist_set_limit(h, 0); /* 恢复默认（这一条只是测试在改，产品路径不许调）*/
      dsh_hist_close(h);
    }
  }

  /* ══ ③ 去重键是 (词, 词典)：同一个词在两本里并存 ═════════════════════════ */
  {
    dsh_history *h = NULL;
    ok(dsh_hist_open(hist_path, &h) == 0 && h != NULL, "③ 打开");
    if (h != NULL) {
      int64_t total = 0;
      (void)dsh_hist_clear(h, NULL);
      const int64_t t0 = 1700000000000LL;
      ok(dsh_hist_push(h, "apple", "d-one", "第一本", t0) == 0, "③ 在 d-one 里查 apple");
      ok(dsh_hist_push(h, "apple", "d-two", "第二本", t0 + 1000) == 0, "③ 在 d-two 里也查 apple");
      ok(dsh_hist_count(h, &total) == 0 && total == 2,
         "③ ★ 同一个词在两本词典里**并存**（去重键是 (词, 词典)，不是词）");
      /* 同一个 (词, 词典) 再过 5 分钟 → 只留一条 */
      ok(dsh_hist_push(h, "apple", "d-one", "第一本", t0 + 10 * 60 * 1000) == 0 &&
         dsh_hist_count(h, &total) == 0 && total == 2,
         "③ ★ 同一个 (词, 词典) 不会变成两条（旧的那条被换掉）");
      {
        char *json = NULL;
        ok(dsh_hist_query(h, 0, 0, &json) == 0 && json != NULL, "③ 取一页");
        char word[64];
        int hits = 0;
        for (const char *p = json; (p = strstr(p, "\"word\":\"apple\"")) != NULL; p++) hits++;
        nth_field(json, "word", 0, word, sizeof(word));
        ok_eq_str(word, "apple", "③ 刚查的那条在最前面");
        ok(hits == 2, "③ ★ 库里正好两条 apple（两本各一条）");
        if (json != NULL) dsh_release(json);
      }
      dsh_hist_close(h);
    }
  }

  /* ══ ④ 分页 ════════════════════════════════════════════════════════════ */
  {
    dsh_history *h = NULL;
    ok(dsh_hist_open(hist_path, &h) == 0 && h != NULL, "④ 打开");
    if (h != NULL) {
      (void)dsh_hist_clear(h, NULL);
      const int64_t t0 = 1700000000000LL;
      char word[32];
      for (int i = 0; i < 5; i++) {
        snprintf(word, sizeof(word), "w%d", i);
        (void)dsh_hist_push(h, word, "d-one", "第一本", t0 + i * 1000);
      }
      char *page1 = NULL;
      char *page2 = NULL;
      ok(dsh_hist_query(h, 0, 2, &page1) == 0 && page1 != NULL, "④ 第一页（2 条）");
      ok(dsh_hist_query(h, 2, 2, &page2) == 0 && page2 != NULL, "④ 第二页（2 条）");
      ok(has(page1, "\"hasMore\":true"), "④ ★ 还有下一页 → hasMore 为真");
      {
        char first[64];
        char second[64];
        char third[64];
        nth_field(page1, "word", 0, first, sizeof(first));
        nth_field(page2, "word", 0, second, sizeof(second));
        nth_field(page2, "word", 1, third, sizeof(third));
        ok_eq_str(first, "w4", "④ 第一页头一条是最新的");
        ok_eq_str(second, "w2", "④ ★ 第二页接着往下（不是从头再来）");
        ok_eq_str(third, "w1", "④ 第二页第二条也对");
      }
      ok(has(page1, "\"total\":5"), "④ 每一页都带总条数（界面据此显示「共 N 条」）");
      if (page1 != NULL) dsh_release(page1);
      if (page2 != NULL) dsh_release(page2);
      /* 越界的 offset：给空数组 + hasMore 为假（不是错误）*/
      char *beyond = NULL;
      ok(dsh_hist_query(h, 99, 2, &beyond) == 0 && beyond != NULL, "④ offset 越界也成功返回");
      ok(has(beyond, "\"items\":[]") && has(beyond, "\"hasMore\":false"),
         "④ ★ 越界那一页是空数组（不是错误、也不是最后一条）");
      if (beyond != NULL) dsh_release(beyond);
      dsh_hist_close(h);
    }
  }

  /* ══ ⑤ 引擎那条路：**查到了才记**，而且记的是落点的规范键名 ═════════════ */
  {
    char *cfg = tmp_db("cfg");
    /* 配置目录（引擎会在里面建它自己的落盘文件）*/
    {
      char *slash = strrchr(cfg, '/');
      if (slash != NULL) *slash = '\0';
    }
    dsh_engine *e = NULL;
    ok(dsh_engine_create(cfg, &e) == DSH_OK && e != NULL, "⑤ 建引擎（临时配置目录）");
    if (e != NULL) {
      /* link.mdx：`apples` 被 `@@@LINK` 重定向到 `apple` */
      char *p = NULL;
      {
        char paths[1200];
        char *link = NULL;
        const char *fx = DSH_TESTDATA_DIR;
        const size_t need = strlen(fx) + 32;
        link = (char *)malloc(need);
        if (link != NULL) snprintf(link, need, "%s/link.mdx", fx);
        snprintf(paths, sizeof(paths), "[\"%s\"]", link != NULL ? link : "");
        if (link != NULL) free(link);
        char *added = NULL;
        ok(dsh_engine_dict_add(e, paths, &added) == DSH_OK && has(added, "\"added\":1"),
           "⑤ 加一本测试用词典（link.mdx）");
        if (added != NULL) dsh_release(added);
      }
      /* 查一个**存在**的词 */
      {
        char *json = NULL;
        ok(dsh_engine_lookup(e, "run", DSH_ORIGIN_INPUT, NULL, &json) == DSH_OK,
           "⑤ 查 run（link.mdx 里有）");
        ok(has(json, "\"found\":true"), "⑤ 命中了");
        if (json != NULL) dsh_release(json);
      }
      {
        char *hist = NULL;
        ok(dsh_history_query(e, 0, 0, &hist) == DSH_OK && hist != NULL, "⑤ 读历史");
        ok(has(hist, "\"word\":\"run\"") && has(hist, "\"total\":1"),
           "⑤ ★ 查到的那一条进了历史");
        if (hist != NULL) dsh_release(hist);
      }
      /* 查一个**查不到**的词：不进历史（参考实现四个调用点全在 `if (payload.Found)` 里）*/
      {
        char *json = NULL;
        ok(dsh_engine_lookup(e, "zzz-not-a-word", DSH_ORIGIN_INPUT, NULL, &json) == DSH_OK,
           "⑤ 查一个不存在的词");
        ok(has(json, "\"found\":false"), "⑤ 如实说没查到");
        if (json != NULL) dsh_release(json);
      }
      {
        char *hist = NULL;
        ok(dsh_history_query(e, 0, 0, &hist) == DSH_OK && hist != NULL, "⑤ 再读历史");
        ok(has(hist, "\"total\":1") && !has(hist, "zzz-not-a-word"),
           "⑤ ★ **查不到不进历史**（总数还是 1）");
        if (hist != NULL) dsh_release(hist);
      }
      /* 跟过 `@@@LINK` 的：记的是**落点**（apple），不是输入（apples）*/
      {
        char *json = NULL;
        ok(dsh_engine_lookup(e, "apples", DSH_ORIGIN_INPUT, NULL, &json) == DSH_OK,
           "⑤ 查 apples（会被重定向到 apple）");
        ok(has(json, "\"keyText\":\"apple\""), "⑤ 落点是 apple");
        if (json != NULL) dsh_release(json);
      }
      {
        char *hist = NULL;
        ok(dsh_history_query(e, 0, 0, &hist) == DSH_OK && hist != NULL, "⑤ 再读历史");
        ok(has(hist, "\"word\":\"apple\""), "⑤ ★ 记的是**落点的规范键名**（apple）");
        ok(!has(hist, "\"word\":\"apples\""), "⑤ ★ 不是用户打进去的那个写法");
        if (hist != NULL) dsh_release(hist);
      }
      /* 清空那条接口 */
      {
        char *cleared = NULL;
        ok(dsh_history_clear(e, &cleared) == DSH_OK && has(cleared, "\"total\":0"),
           "⑤ 清空历史（接口回的形状与接口定义一致）");
        if (cleared != NULL) dsh_release(cleared);
        char *hist = NULL;
        ok(dsh_history_query(e, 0, 0, &hist) == DSH_OK && has(hist, "\"total\":0"),
           "⑤ 清空之后是 0 条");
        if (hist != NULL) dsh_release(hist);
      }

      /* ── ⑤b ★ **每一行都带「那一本还在不在」+ 一句人话** ─────────────────────
       * 三种情形都要**真的造出来**（不是拿字段非空糊过去）：正常那本 → `unavailable`/`note`
       * 空串；**不在词库里** → `removed`，书名用**历史里那份快照**；**在词库里、文件没了** →
       * `missing`，书名用**当前显示名**（改过名要看新名字）。
       * ⚠️ `dict_id` 要声明在这三个块**之外**：③ 是 ①② 的兄弟块，却要用 ① 取出的那个 id。 */
      char dict_id[128] = "";
      {
        /* ① 正常：先查一个词（那本在、文件也在），顺便把它那一本的 id 取出来 */
        char *json = NULL;
        ok(dsh_engine_lookup(e, "run", DSH_ORIGIN_INPUT, NULL, &json) == DSH_OK,
           "⑤b 查 run（link.mdx 在词库里、文件也在）");
        ok(json != NULL && has(json, "\"found\":true"), "⑤b 命中了");
        if (json != NULL) {
          json_field(json, "dictId", dict_id, sizeof(dict_id));
          dsh_release(json);
        }
        ok(dict_id[0] != '\0', "⑤b 从落点取出那一本的 id（后面要拿它做两件事）");
        json = NULL;
        ok(dsh_history_query(e, 0, 0, &json) == DSH_OK && json != NULL, "⑤b 读历史");
        ok(has(json, "\"unavailable\":\"\"") && has(json, "\"note\":\"\""),
           "⑤b ★ 那一本好好的 → `unavailable` 与 `note` 都是空串（界面没什么要说的）");
        if (json != NULL) dsh_release(json);

        /* ② 那本**被移出词库**了：记录还在，词典没了 */
        {
          char *removed = NULL;
          ok(dsh_engine_dict_remove(e, dict_id, &removed) == DSH_OK, "⑤b 把它移出词库");
          if (removed != NULL) dsh_release(removed);
        }
        json = NULL;
        ok(dsh_history_query(e, 0, 0, &json) == DSH_OK && json != NULL, "⑤b 再读历史");
        ok(has(json, "\"unavailable\":\"removed\""),
           "⑤b ★★ 那一本不在词库里了 → `unavailable=removed`");
        ok(has(json, "》已不在词库中 —— 重新导入到原路径即可恢复"),
           "⑤b ★★ 而且那句人话**由内核给**（参考实现里这句是界面拼的）");
        if (json != NULL) dsh_release(json);
      }

      /* ③ 那本**在词库里、文件不在了** —— 与「被移出」**不是同一句话**。
       * 造法：把测试用词典**复制**一份进临时目录再加进词库（不动冻结的 `testdata/`），
       * 然后把那份文件删掉；Linux 上删一个已映射的文件没问题。
       * ⚠️ 副本与 `link.mdx` 字节相同、而词典 id 是**内容哈希** → 它加回来就是**同一个 id**；
       *    拿**路径**当 id 去查是查不到的（只认 id），改名那一步会被不报错地跳过。 */
      {
        char src[1200];
        snprintf(src, sizeof(src), "%s/link.mdx", DSH_TESTDATA_DIR);
        char *copy = tmp_db("gone-dict.mdx");
        ok(copy != NULL, "⑤b 临时副本的路径");
        if (copy != NULL) {
          FILE *in = fopen(src, "rb");
          FILE *out = (in != NULL) ? fopen(copy, "wb") : NULL;
          ok(in != NULL && out != NULL, "⑤b 复制一份测试用词典出来当「文件会丢」的那本");
          if (in != NULL && out != NULL) {
            char buf[8192];
            size_t n = 0;
            while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
          }
          if (out != NULL) fclose(out);
          if (in != NULL) fclose(in);

          char paths[1400];
          snprintf(paths, sizeof(paths), "[\"%s\"]", copy);
          char *added = NULL;
          ok(dsh_engine_dict_add(e, paths, &added) == DSH_OK && has(added, "\"added\":1"),
             "⑤b 把它加进词库");
          if (added != NULL) dsh_release(added);
          /* 内容相同 → 同一个 id：这一条顺便把「id 是内容哈希、不认路径」再钉一次 */
          {
            char *renamed = NULL;
            ok(dsh_engine_dict_rename(e, dict_id, "改过名的词典", &renamed) == DSH_OK,
               "⑤b ★ 用**同一个 id** 就能给这本改名（副本与原来那本字节相同 = 同一本）");
            if (renamed != NULL) dsh_release(renamed);
          }
          remove(copy);
          char *json = NULL;
          ok(dsh_history_query(e, 0, 0, &json) == DSH_OK && json != NULL, "⑤b 文件删掉之后读历史");
          ok(has(json, "\"unavailable\":\"missing\""),
             "⑤b ★★ 在词库里、但文件没了 → `unavailable=missing`（与 removed **不是**同一档）");
          {
            /* 把那一句话取出来看**逐字**内容：这一条断的是「书名用的是当前显示名」 */
            char note[512];
            json_field(json, "note", note, sizeof(note));
            char label[768];
            snprintf(label, sizeof(label),
                     "⑤b ★★ 而且书名用的是**当前显示名**（改过名就要看新名字）—— 实际：%s",
                     note[0] != '\0' ? note : "(空)");
            ok(has(note, "《改过名的词典》的文件不在了 —— 把它放回原来的位置，或重新导入一次"),
               label);
          }
          ok(!has(json, "不在词库中"),
             "⑤b ★★ 这一档**不许**说「不在词库中」（两件事、两句话，参考实现的 D8）");
          if (json != NULL) dsh_release(json);
          free(copy); /* `tmp_db` 给的是 malloc 出来的路径（漏了它 ASan 会报一条泄漏）*/
        }
      }

      dsh_engine_destroy(e);
      if (p != NULL) free(p);
    }
    free(cfg);
  }

  /* ══ ⑥ 没有配置目录的引擎：**如实说用不了**，不许装成空历史 ═════════════ */
  {
    dsh_engine *e = NULL;
    ok(dsh_engine_create(NULL, &e) == DSH_OK && e != NULL, "⑥ 建一个只在内存里活的引擎");
    if (e != NULL) {
      char *json = NULL;
      const enum dsh_error rc = dsh_history_query(e, 0, 0, &json);
      ok(rc != DSH_OK && json == NULL,
         "⑥ ★ 没有配置目录时**报错**（不是回一张空历史 —— 那会让人以为历史被清空了）");
      const char *why = dsh_last_error_message();
      ok(why != NULL && why[0] != '\0', "⑥ 而且说清为什么（人话）");
      if (why != NULL) dsh_release((void *)why);
      dsh_engine_destroy(e);
    }
  }

  /* ══ ⑦ 写放大必须是 O(1) ════════════════════════════════════════════════
   * 「代码里看起来是追加」证明不了它不是整份重写 —— 只有数**真的交给操作系统的字节数**才算数。
   * 钉法：同一个 push 在历史很短与已有几百条两种情况下，写出去的字节数必须**一模一样**。 */
  {
    char *path = tmp_db("amplify.jsonl");
    remove(path);
    dsh_history *h = NULL;
    ok(dsh_hist_open(path, &h) == 0 && h != NULL, "⑦ 打开一个空历史");
    if (h != NULL) {
      const int64_t t0 = 1700000000000LL;
      int64_t short_bytes = 0;
      int64_t long_bytes = 0;
      {
        dsh_file_stats_reset();
        ok(dsh_hist_push(h, "probe", "d-one", "第一本", t0) == 0, "⑦ 空历史里记一条");
        short_bytes = dsh_file_bytes_written();
      }
      ok(short_bytes > 0, "⑦ 数得到写出去的字节数（钩子真的在工作）");
      ok(short_bytes < 200, "⑦ ★ 记一条只写**一行**（不是整份文件）");
      {
        /* 把它撑到 301 条：如果实现是「整份重写」，第 302 条的字节数会立刻涨到十几 KB */
        char word[32];
        for (int i = 0; i < 300; i++) {
          snprintf(word, sizeof(word), "filler%d", i);
          (void)dsh_hist_push(h, word, "d-one", "第一本", t0 + (i + 1) * 1000);
        }
        int64_t total = 0;
        ok(dsh_hist_count(h, &total) == 0 && total == 301, "⑦ 现在有 301 条（撑长了）");
        dsh_file_stats_reset();
        /* ⚠️ 这一条必须与上面那条**逐字相同**（同样的词、词典、时间）：要比的是「同一件事在不同
         *    的历史长度下写多少字节」。`at` 也用同一个值 —— 队头已经不是它了，不会撞上去重窗口。 */
        ok(dsh_hist_push(h, "probe", "d-one", "第一本", t0) == 0,
           "⑦ 长历史里再记**同一条**");
        long_bytes = dsh_file_bytes_written();
      }
      ok(long_bytes == short_bytes,
         "⑦ ★★ 写出去的字节数与历史长短**完全无关**（这条挂了就说明又回到整份重写）");
      dsh_hist_close(h);
    }
    remove(path);
    free(path);
  }

  /* ══ ⑧ 进程被杀不丢 + 不留 `.tmp` 残骸 ══════════════════════════════════
   * 每条都必须是 `fflush` 落的（不靠关闭时补写），打开时还要顺手清残骸。
   * 检查标准的做法：**真的 fork 一个子进程**，写完 30 条就 `_exit(0)`（不关句柄、不跑清理），
   * 然后父进程重新打开看数据在不在 —— 比「忘了关」更像被杀现场。 */
  {
    char *path = tmp_db("killed.jsonl");
    remove(path);
    {
      char tmp[2048];
      snprintf(tmp, sizeof(tmp), "%s.tmp", path);
      append_raw(tmp, "上次压缩写到一半留下的垃圾");
      ok(file_exists(tmp), "⑧ 先伪造一个 `.tmp` 残骸（模拟「写到一半被杀」）");
    }
    const pid_t pid = fork();
    if (pid == 0) {
      /* 子进程：写 30 条，然后**不关就退出**（`_exit` 不跑任何清理、不刷任何缓冲）*/
      dsh_history *child = NULL;
      if (dsh_hist_open(path, &child) != 0 || child == NULL) _exit(2);
      char word[32];
      for (int i = 0; i < 30; i++) {
        snprintf(word, sizeof(word), "killed%d", i);
        if (dsh_hist_push(child, word, "d-one", "第一本", 1700000000000LL + i * 1000) != 0) _exit(3);
      }
      _exit(0);
    }
    {
      int status = 0;
      (void)waitpid(pid, &status, 0);
      ok(WIFEXITED(status) && WEXITSTATUS(status) == 0, "⑧ 子进程写完之后直接退出（没有关句柄）");
    }
    {
      dsh_history *h = NULL;
      ok(dsh_hist_open(path, &h) == 0 && h != NULL, "⑧ 父进程重新打开");
      if (h != NULL) {
        int64_t total = 0;
        ok(dsh_hist_count(h, &total) == 0 && total == 30,
           "⑧ ★★ 上一个进程是被 `_exit` 干掉的，30 条一条不少");
        ok(count_lines(path) == 30, "⑧ ★ 文件里就是 30 行（每条一行，没有半行）");
        dsh_hist_close(h);
      }
    }
    {
      char tmp[2048];
      snprintf(tmp, sizeof(tmp), "%s.tmp", path);
      ok(!file_exists(tmp), "⑧ ★ 打开时把那个 `.tmp` 残骸清掉了（不留孤儿）");
    }
    remove(path);
    free(path);
  }

  /* ══ ⑨ 一行坏行不许毒死整份历史，而且跳过的条数要说出来 ═════════════════ */
  {
    char *path = tmp_db("broken.jsonl");
    remove(path);
    {
      dsh_history *h = NULL;
      if (dsh_hist_open(path, &h) == 0 && h != NULL) {
        (void)dsh_hist_push(h, "apple", "d-one", "第一本", 1700000000000LL);
        (void)dsh_hist_push(h, "banana", "d-one", "第一本", 1700000001000LL);
        (void)dsh_hist_push(h, "cherry", "d-one", "第一本", 1700000002000LL);
        dsh_hist_close(h);
      }
    }
    /* 往文件里插一行不是 JSON 的垃圾（模拟「手改坏了 / 写到一半断电」）*/
    append_raw(path, "这不是 JSON，只是一行垃圾\n");
    append_raw(path, "{\"word\":\"date\",\"dictId\":\"d-one\",\"dictTitle\":\"第一本\",\"at\":1700000003000}\n");
    {
      dsh_history *h = NULL;
      ok(dsh_hist_open(path, &h) == 0 && h != NULL, "⑨ 带一行坏行也能打开");
      if (h != NULL) {
        int64_t total = 0;
        int64_t skipped = -1;
        ok(dsh_hist_count(h, &total) == 0 && total == 4,
           "⑨ ★★ 坏行被跳过，其余 4 条**一条不少**");
        ok(dsh_hist_skipped(h, &skipped) == 0 && skipped == 1,
           "⑨ ★ 而且**如实报出来**跳过了 1 行（不许悄悄丢）");
        {
          char *json = NULL;
          ok(dsh_hist_query(h, 0, 0, &json) == 0 && json != NULL, "⑨ 取一页");
          ok(has(json, "\"word\":\"apple\"") && has(json, "\"word\":\"date\""),
             "⑨ 坏行**前后**的记录都在（不是只保住前半段）");
          if (json != NULL) dsh_release(json);
        }
        dsh_hist_close(h);
      }
    }
    remove(path);
    free(path);
  }

  /* ══ ⑩ 压缩：文件长到上限的 2 倍时收回去一次 ═══════════════════════════ */
  {
    char *path = tmp_db("compact.jsonl");
    remove(path);
    dsh_history *h = NULL;
    ok(dsh_hist_open(path, &h) == 0 && h != NULL, "⑩ 打开");
    if (h != NULL) {
      const int64_t t0 = 1700000000000LL;
      char word[32];
      /* 上限压到 10（**只给测试用**，见 dsh_hist_set_limit）：压缩会在第 20 行之后触发 */
      dsh_hist_set_limit(h, 10);
      for (int i = 0; i < 25; i++) {
        snprintf(word, sizeof(word), "c%d", i);
        (void)dsh_hist_push(h, word, "d-one", "第一本", t0 + i * 1000);
      }
      int64_t total = 0;
      ok(dsh_hist_count(h, &total) == 0 && total == 10, "⑩ 25 条之后只剩上限那 10 条");
      ok(count_lines(path) <= 20, "⑩ ★ 文件被压缩过（行数回到上限附近，不是一路涨到 25+）");
      {
        char *json = NULL;
        ok(dsh_hist_query(h, 0, 0, &json) == 0 && json != NULL, "⑩ 压缩之后取一页");
        ok(has(json, "\"word\":\"c24\""), "⑩ ★ 最新的那条还在（压缩没把新的压没）");
        ok(!has(json, "\"word\":\"c0\""), "⑩ 最旧的那些被砍掉了（上限说了算）");
        if (json != NULL) dsh_release(json);
      }
      dsh_hist_close(h);
      /* 重开一次：压缩写出来的文件必须**每一行都能读回来**（压缩没写坏）。
       * ⚠️ 重开之后上限回到默认（上限是**测试注入**的、不落盘），所以断言钉的是「行数与条数
       *    一致 + 没有坏行」，而不是「还是 10 条」（那要等下面把上限再设一次）。 */
      h = NULL;
      ok(dsh_hist_open(path, &h) == 0 && h != NULL, "⑩ 压缩之后重开");
      if (h != NULL) {
        int64_t reopened = -1;
        int64_t skipped = -1;
        ok(dsh_hist_count(h, &reopened) == 0 && reopened == count_lines(path),
           "⑩ ★★ 压缩写出去的文件**每一行都读得回来**（条数 == 行数）");
        ok(dsh_hist_skipped(h, &skipped) == 0 && skipped == 0,
           "⑩ ★ 压缩之后一行坏行都没有（跳过的条数是 0）");
        {
          char *json = NULL;
          ok(dsh_hist_query(h, 0, 0, &json) == 0 && has(json, "\"word\":\"c24\""),
             "⑩ ★ 重开之后最新的那条还在");
          if (json != NULL) dsh_release(json);
        }
        /* 上限压回 10：**立刻**收口（不是「下次 push 才生效」），而且落了盘 */
        dsh_hist_set_limit(h, 10);
        ok(dsh_hist_count(h, &reopened) == 0 && reopened == 10,
           "⑩ ★★ 设完上限当场砍到 10 条");
        dsh_hist_close(h);
      }
      h = NULL;
      ok(dsh_hist_open(path, &h) == 0 && h != NULL, "⑩ 收了上限之后再重开");
      if (h != NULL) {
        int64_t reopened = -1;
        ok(dsh_hist_count(h, &reopened) == 0 && reopened <= 10,
           "⑩ ★ 落盘之后条数不超过上限（上限是产品约定，重启也要成立）");
        dsh_hist_close(h);
      }
    }
    remove(path);
    free(path);
  }

  /* ══ ⑪ 满了 5000 条时文件多大（同时钉住「紧凑、不缩进」）═════════════════ */
  {
    char *path = tmp_db("full.jsonl");
    remove(path);
    dsh_history *h = NULL;
    ok(dsh_hist_open(path, &h) == 0 && h != NULL, "⑪ 打开");
    if (h != NULL) {
      const int64_t t0 = 1700000000000LL;
      char word[32];
      for (int i = 0; i < 5000; i++) {
        snprintf(word, sizeof(word), "word%d", i);
        if (dsh_hist_push(h, word, "0123456789abcdef", "牛津高阶英汉双解词典", t0 + i * 1000) != 0) {
          break;
        }
      }
      int64_t total = 0;
      ok(dsh_hist_count(h, &total) == 0 && total == 5000, "⑪ 记满 5000 条");
      const long size = file_size(path);
      ok(size > 0 && size < 1024 * 1024,
         "⑪ ★ 满额时文件**不到 1 MB**（JSON 是紧凑写的，没有缩进那 37% 的体积）");
      printf("      [实测结果] 5000 条（词 wordN + 16 字节词典 id + 中文词典名）时 %ld 字节 = %.0f KB\n",
             size, (double)size / 1024.0);
      dsh_hist_close(h);
    }
    remove(path);
    free(path);
  }

  remove(hist_path);
  free(hist_path);
  /* 把临时目录擦干净（测试用词典目录是冻结的，测试不许在这儿留东西，见 `cleanup_dir`）*/
  {
    const size_t need = strlen(DSH_TESTDATA_DIR) + 32;
    char *dir = (char *)malloc(need);
    if (dir != NULL) {
      snprintf(dir, need, "%s/tmp-hist", DSH_TESTDATA_DIR);
      cleanup_dir(dir);
      free(dir);
    }
  }

  ok((int64_t)dsh_mem_live_count() == (int64_t)base,
     "全部用例跑完，活分配表必须回到基线");

  printf("history：%d 项，失败 %d\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
