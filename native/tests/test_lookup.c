/* ==========================================================================
 * 内核单元测试 · lookup 组（把零件接起来的那一层：resolve / suggest / probe / lookup）
 *
 * 四条判据：
 *   · `resolve` 与 `suggest` 的第一条**必须落到同一个落点**（`apples` 跟 `@@@LINK=` 落到 `apple`）
 *     —— 界面拿它判"选中的是不是当前词条"，判错会把同一篇正文重载一遍；
 *   · `probe` 问完一句之后**不许**把词典留在内存里（`dict_list` 里 `loaded` 必须仍是 false）；
 *   · **单字符查询不做拼写纠正**：编辑距离 ≤1 对单字符恒真，会退化成列出整块单字符词条；
 *   · **timeout / error 绝不许并成 missing**（接口定义里点名的那条）。
 * ========================================================================== */

#include "dsh_lookup.h"
#include "engine/dsh_engine_internal.h"
#include "engine/dsh_settings.h"
#include "mem_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DSH_TESTDATA_DIR
#error "需要 -DDSH_TESTDATA_DIR=<0.2.0/testdata 的路径>（见 Makefile）"
#endif

static int g_checks = 0;
static int g_failed = 0;

static void ok(int cond, const char *what) {
  g_checks++;
  if (!cond) {
    g_failed++;
    fprintf(stderr, "FAIL %s\n", what);
  }
}

static void ok_eq_i64(int64_t actual, int64_t expected, const char *what) {
  g_checks++;
  if (actual != expected) {
    g_failed++;
    fprintf(stderr, "FAIL %s：实际=%lld 期望=%lld\n", what, (long long)actual,
            (long long)expected);
  }
}

static void ok_eq_str(const char *actual, const char *expected, const char *what) {
  g_checks++;
  if (actual == NULL || expected == NULL || strcmp(actual, expected) != 0) {
    g_failed++;
    fprintf(stderr, "FAIL %s\n      实际=%s\n      期望=%s\n", what, actual ? actual : "(null)",
            expected ? expected : "(null)");
  }
}

/** 文本里有没有出现某段子串 */
static int has(const char *haystack, const char *needle) {
  return haystack != NULL && strstr(haystack, needle) != NULL;
}

/**
 * 子串断言：失败时把整份 JSON 打出来 —— 这一组的出参是一整份 EntryPayload（几百字节），
 * 只说"没命中"没法排查，而"到底交出来的是什么"恰恰是这一层唯一要验的东西。
 */
static void ok_has(const char *json, const char *needle, const char *what) {
  g_checks++;
  if (has(json, needle)) return;
  g_failed++;
  fprintf(stderr, "FAIL %s\n      缺：%s\n      实际：%.700s\n", what, needle,
          json != NULL ? json : "(null)");
}

/** 从 JSON 里取一个字符串字段的值（够用的极简取值器；找不到返回空串） */
static const char *json_str_field(const char *json, const char *key, char *buf, size_t cap) {
  buf[0] = '\0';
  if (json == NULL) return buf;
  char pat[64];
  snprintf(pat, sizeof(pat), "\"%s\":\"", key);
  const char *p = strstr(json, pat);
  if (p == NULL) return buf;
  p += strlen(pat);
  size_t k = 0;
  while (*p != '\0' && *p != '"' && k + 1 < cap) buf[k++] = *p++;
  buf[k] = '\0';
  return buf;
}

static char *fixture(const char *name) {
  const size_t n = strlen(DSH_TESTDATA_DIR) + strlen(name) + 2;
  char *p = (char *)malloc(n);
  if (p == NULL) return NULL;
  snprintf(p, n, "%s/%s", DSH_TESTDATA_DIR, name);
  return p;
}

/** 建一个引擎并把若干测试用词典加进词库；返回引擎（失败返回 NULL） */
static dsh_engine *engine_with(const char *const *names, int count, char *out_ids[4]) {
  dsh_engine *e = NULL;
  if (dsh_engine_create(NULL, &e) != DSH_OK || e == NULL) return NULL;
  for (int i = 0; i < count; i++) {
    char *p = fixture(names[i]);
    if (p == NULL) continue;
    char paths[1200];
    snprintf(paths, sizeof(paths), "[\"%s\"]", p);
    char *out = NULL;
    if (dsh_engine_dict_add(e, paths, &out) != DSH_OK) {
      free(p);
      dsh_engine_destroy(e);
      return NULL;
    }
    if (has(out, "\"added\":1") && out_ids != NULL) {
      /* 从清单里取**第 i 条** id：清单按加入顺序排，取最后一条会让两本的 id 变成同一个。
       * （id 由内核算，测试不必自己算哈希。） */
      char *list = NULL;
      if (dsh_engine_dict_list(e, &list) == DSH_OK && list != NULL) {
        const char *q = NULL;
        int at = 0;
        for (const char *p = list; (p = strstr(p, "\"id\":\"")) != NULL; p += 6) {
          if (at == i) {
            q = p;
            break;
          }
          at++;
        }
        if (q != NULL) {
          q += 6;
          size_t k = 0;
          while (q[k] != '\0' && q[k] != '"' && k < 64) {
            out_ids[i][k] = q[k];
            k++;
          }
          out_ids[i][k] = '\0';
        }
        dsh_release(list);
      }
    }
    if (out != NULL) dsh_release(out);
    free(p);
  }
  return e;
}

int main(void) {
  const size_t base = dsh_mem_live_count();
  char *ids[4] = {(char[65]){0}, (char[65]){0}, (char[65]){0}, (char[65]){0}};

  /* ── ① dsh_engine_resolve：落点、大小写变体、@@@LINK 重定向 ───────────── */
  {
    const char *names[] = {"test.mdx", "link.mdx"};
    dsh_engine *e = engine_with(names, 2, ids);
    ok(e != NULL, "① 建引擎并把两本测试用词典加进词库");
    if (e != NULL) {
      char *json = NULL;
      /* 没指定当前词典时：不该崩，而且理由要说清。
       * ⚠️ 这个状态得**显式造出来** —— 加第一本词典时内核会自动把它设成当前。 */
      {
        char *cleared = NULL;
        ok(dsh_engine_dict_set_current(e, NULL, &cleared) == DSH_OK && cleared != NULL,
           "① 先把当前词典清掉（造出「没指定」那个状态）");
        if (cleared != NULL) dsh_release(cleared);
      }
      ok(dsh_engine_resolve(e, NULL, "apple", &json) == DSH_OK, "① 没指定当前词典也不报错");
      ok(has(json, "\"landed\":null"), "① 没有当前词典时落点是 null");
      ok(has(json, "\"enabled\":false"), "① 没有当前词典时 enabled 为假");
      if (json != NULL) dsh_release(json);

      /* 指定当前词典 = link.mdx（它里面 apple / apples / ran / run） */
      char *out = NULL;
      ok(dsh_engine_dict_set_current(e, ids[1], &out) == DSH_OK, "① 指定当前词典");
      if (out != NULL) dsh_release(out);

      json = NULL;
      ok(dsh_engine_resolve(e, NULL, "apple", &json) == DSH_OK, "① 查 apple 成功");
      ok(has(json, "\"landed\":\"apple\""), "① apple 的落点就是 apple");
      ok(has(json, "\"enabled\":true"), "① 词典可用时 enabled 为真");
      if (json != NULL) dsh_release(json);

      /* ⚠️ 最关键的一条：`apples` 在词典里是 `@@@LINK=apple`，
       *    落点必须是 **apple**（跟到底），不是 apples 自己。 */
      json = NULL;
      ok(dsh_engine_resolve(e, NULL, "apples", &json) == DSH_OK, "① 查 apples 成功");
      ok(has(json, "\"landed\":\"apple\""),
         "① apples 的落点必须是 apple（跟 @@@LINK 重定向到底）——这是那个坑的保护措施");
      ok(has(json, "重定向"), "① 跟过重定向时理由里要说明白");
      if (json != NULL) dsh_release(json);

      /* ran → @@@LINK=run */
      json = NULL;
      ok(dsh_engine_resolve(e, NULL, "ran", &json) == DSH_OK, "① 查 ran 成功");
      ok(has(json, "\"landed\":\"run\""), "① ran 的落点必须是 run");
      if (json != NULL) dsh_release(json);

      /* 大小写变体：APPLE 也应当落到 apple（词典 lookup 本身是大小写敏感的，
       * 变体是这一层补的）。 */
      json = NULL;
      ok(dsh_engine_resolve(e, NULL, "APPLE", &json) == DSH_OK, "① 查 APPLE 成功");
      ok(has(json, "\"landed\":\"apple\""), "① APPLE 也落到 apple（大小写变体）");
      if (json != NULL) dsh_release(json);

      /* 首尾空白要先 trim */
      json = NULL;
      ok(dsh_engine_resolve(e, NULL, "  apple  ", &json) == DSH_OK, "① 带空白的查询");
      ok(has(json, "\"landed\":\"apple\""), "① 首尾空白要被 trim 掉");
      if (json != NULL) dsh_release(json);

      /* 查不到：landed=null，但 enabled 仍然为真（词典是好的） */
      json = NULL;
      ok(dsh_engine_resolve(e, NULL, "zzzz-no-such", &json) == DSH_OK, "① 查不到的查询也成功");
      ok(has(json, "\"landed\":null"), "① 查不到 → landed 为 null");
      ok(has(json, "\"enabled\":true"),
         "① 查不到时 enabled 仍为真（「词没有」与「词典坏了」是两件事）");
      if (json != NULL) dsh_release(json);

      /* 用 dict_id 指定另一本（不靠"当前词典"） */
      json = NULL;
      ok(dsh_engine_resolve(e, ids[0], "banana", &json) == DSH_OK, "① 用 dict_id 指定另一本");
      ok(has(json, "\"landed\":\"banana\""), "① 落点来自指定的那一本");
      if (json != NULL) dsh_release(json);

      /* 参数边界 */
      ok(dsh_engine_resolve(e, NULL, "x", NULL) != DSH_OK, "① out 为 NULL 必须失败");
      ok(dsh_engine_resolve(e, NULL, NULL, &json) != DSH_OK, "① text 为 NULL 必须失败");
      ok(dsh_engine_resolve(NULL, NULL, "x", &json) != DSH_OK, "① 引擎为 NULL 必须失败");
      dsh_engine_destroy(e);
    }
  }
  ok_eq_i64((int64_t)dsh_mem_live_count(), (int64_t)base, "① 全部释放之后活分配表回到基线");

  /* ── ② dsh_engine_suggest：精确 → 前缀 → 纠正，单字符不纠正 ───────────── */
  {
    const char *names[] = {"test.mdx"};
    char *one_id[4] = {(char[65]){0}, NULL, NULL, NULL};
    dsh_engine *e = engine_with(names, 1, one_id);
    ok(e != NULL, "② 建引擎（test.mdx：apple/application/apply/appreciate/banana/测试）");
    if (e != NULL) {
      char *out = NULL;
      ok(dsh_engine_dict_set_current(e, one_id[0], &out) == DSH_OK, "② 指定当前词典");
      if (out != NULL) dsh_release(out);

      /* 精确命中排第一，而且 kind 是 exact */
      char *json = NULL;
      ok(dsh_engine_suggest(e, "apple", &json) == DSH_OK, "② 联想 apple 成功");
      ok(has(json, "{\"word\":\"apple\",\"kind\":\"exact\"}"), "② 第一条是精确命中");
      if (json != NULL) dsh_release(json);

      /* 前缀补全：`app` → apple / application / apply / appreciate */
      json = NULL;
      ok(dsh_engine_suggest(e, "app", &json) == DSH_OK, "② 联想 app 成功");
      {
        int prefix_hits = 0;
        for (const char *p = json; (p = strstr(p, "\"kind\":\"prefix\"")) != NULL; p++) {
          prefix_hits++;
        }
        ok(prefix_hits >= 3, "② app 应当补出至少 3 条前缀候选");
      }
      ok(has(json, "\"word\":\"apple\""), "② 前缀候选里有 apple");
      ok(has(json, "\"word\":\"application\""), "② 前缀候选里有 application");
      if (json != NULL) dsh_release(json);

      /* ⚠️ 单字符：**不许**出现 fuzzy（编辑距离 ≤1 对单字符恒真） */
      json = NULL;
      ok(dsh_engine_suggest(e, "a", &json) == DSH_OK, "② 单字符联想成功");
      ok(!has(json, "\"kind\":\"fuzzy\""),
         "② 单字符**不许**做拼写纠正（编辑距离 ≤1 对单字符恒真，参考实现那个坑）");
      ok(has(json, "\"word\":\"apple\""), "② 单字符仍然给前缀补全");
      if (json != NULL) dsh_release(json);

      /* 行数上限：DSH_MAX_LIST_ROWS = 8 */
      json = NULL;
      ok(dsh_engine_suggest(e, "a", &json) == DSH_OK, "② 再联想一次");
      {
        int rows = 0;
        for (const char *p = json; (p = strstr(p, "\"kind\":")) != NULL; p++) rows++;
        ok(rows <= DSH_MAX_LIST_ROWS, "② 联想的行数不许超过 DSH_MAX_LIST_ROWS");
      }
      if (json != NULL) dsh_release(json);

      /* 查不到的东西：空数组（不是报错） */
      json = NULL;
      ok(dsh_engine_suggest(e, "zzzz-nothing", &json) == DSH_OK, "② 查不到时联想也成功");
      ok_eq_str(json, "[]", "② 查不到 → 空数组");
      if (json != NULL) dsh_release(json);

      /* 空查询：空数组 */
      json = NULL;
      ok(dsh_engine_suggest(e, "   ", &json) == DSH_OK, "② 空查询成功");
      ok_eq_str(json, "[]", "② 空查询 → 空数组");
      if (json != NULL) dsh_release(json);

      ok(dsh_engine_suggest(e, "x", NULL) != DSH_OK, "② out 为 NULL 必须失败");
      ok(dsh_engine_suggest(e, NULL, &json) != DSH_OK, "② text 为 NULL 必须失败");
      dsh_engine_destroy(e);
    }
  }

  /* ── ③ dsh_engine_probe：问一句就走，词典不许留下 ─────────────────────── */
  {
    const char *names[] = {"test.mdx", "link.mdx"};
    dsh_engine *e = engine_with(names, 2, ids);
    ok(e != NULL, "③ 建引擎并加两本");
    if (e != NULL) {
      char *out = NULL;
      ok(dsh_engine_dict_set_current(e, ids[0], &out) == DSH_OK, "③ 当前词典 = test.mdx");
      if (out != NULL) dsh_release(out);

      /* 先借问**另一本**（link.mdx）——这是 probe 的典型用法 */
      char *json = NULL;
      ok(dsh_engine_probe(e, "apples", ids[1], 0, &json) == DSH_OK, "③ 探路成功");
      ok(has(json, "\"status\":\"ok\""), "③ 问到的状态是 ok");
      ok(has(json, "\"landed\":\"apple\""), "③ 探路也要跟重定向（落在 apple）");
      if (json != NULL) dsh_release(json);

      /* ⚠️ 接口定义的纪律：问完之后那一本**不许**留在内存里 */
      {
        char *list = NULL;
        ok(dsh_engine_dict_list(e, &list) == DSH_OK, "③ 取词库清单");
        if (list != NULL) {
          /* 两本都应当是 loaded=false：test.mdx 从没查过、link.mdx 是借问的 */
          int loaded_true = 0;
          for (const char *p = list; (p = strstr(p, "\"loaded\":true")) != NULL; p++) {
            loaded_true++;
          }
          ok(loaded_true == 0,
             "③ 探路之后**没有**任何一本留在内存里（参考实现那个坑：问一句与加载一本必须分得开）");
          dsh_release(list);
        }
      }

      /* 查不到的东西：status = missing（不是 error） */
      json = NULL;
      ok(dsh_engine_probe(e, "zzzz-no-such", ids[1], 0, &json) == DSH_OK, "③ 探不存在的词");
      ok(has(json, "\"status\":\"missing\""),
         "③ 没有那一条 → missing（接口定义点名：**不许**与 error 混起来）");
      if (json != NULL) dsh_release(json);

      /* 词库里没有那本：missing（那是"没有这本"而不是"读不动"） */
      json = NULL;
      ok(dsh_engine_probe(e, "apple", "no-such-id", 0, &json) == DSH_OK, "③ 探不存在的词典");
      ok(has(json, "\"status\":\"missing\""), "③ 词库里没有这本 → missing");
      if (json != NULL) dsh_release(json);

      /* 文件被挪走 → error（**不许**说成 missing）——把路径改成一个不存在的 */
      {
        dsh_engine *e2 = NULL;
        if (dsh_engine_create(NULL, &e2) == DSH_OK && e2 != NULL) {
          /* 直接往设置里塞一本路径不存在的：走 dict_add 会被挡下（那是设计），
           * 所以这里用 resolve 那条路验不了 —— 换成 probe 一个空 id。 */
          json = NULL;
          ok(dsh_engine_probe(e2, "apple", "", 0, &json) != DSH_OK,
             "③ dict_id 为空必须失败（不能拿空 id 去探）");
        }
        dsh_engine_destroy(e2);
      }

      /* 参数边界 */
      ok(dsh_engine_probe(e, "x", ids[0], 0, NULL) != DSH_OK, "③ out 为 NULL 必须失败");
      ok(dsh_engine_probe(e, NULL, ids[0], 0, &json) != DSH_OK, "③ text 为 NULL 必须失败");
      ok(dsh_engine_probe(NULL, "x", ids[0], 0, &json) != DSH_OK, "③ 引擎为 NULL 必须失败");

      dsh_engine_destroy(e);
    }
  }

  /* ── ④ resolve 与 suggest 的落点必须一致（接口定义里那条不变式）────────── */
  {
    const char *names[] = {"link.mdx"};
    char *xid[4] = {(char[65]){0}, NULL, NULL, NULL};
    dsh_engine *e = engine_with(names, 1, xid);
    if (e != NULL) {
      char *out = NULL;
      ok(dsh_engine_dict_set_current(e, xid[0], &out) == DSH_OK, "④ 指定当前词典");
      if (out != NULL) dsh_release(out);
      /* 逐个词核对：resolve 的 landed 必须等于 suggest 的第一条 word */
      const char *words[] = {"apple", "apples", "ran", "run"};
      for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        char *rj = NULL;
        char *sj = NULL;
        ok(dsh_engine_resolve(e, NULL, words[i], &rj) == DSH_OK, "④ resolve 成功");
        ok(dsh_engine_suggest(e, words[i], &sj) == DSH_OK, "④ suggest 成功");
        char landed[128];
        json_str_field(rj, "landed", landed, sizeof(landed));
        char first[128] = "";
        const char *p = (sj != NULL) ? strstr(sj, "\"word\":\"") : NULL;
        if (p != NULL) {
          p += 8;
          size_t k = 0;
          while (p[k] != '\0' && p[k] != '"' && k + 1 < sizeof(first)) {
            first[k] = p[k];
            k++;
          }
          first[k] = '\0';
        }
        char label[512];
        snprintf(label, sizeof(label),
                 "④ 「%s」的落点必须与联想第一条相同（resolve=%s suggest=%s）", words[i],
                 landed, first);
        ok(strcmp(landed, first) == 0, label);
        if (rj != NULL) dsh_release(rj);
        if (sj != NULL) dsh_release(sj);
      }
      dsh_engine_destroy(e);
    }
  }

  /* ── ⑤ dsh_engine_lookup：一条链走到底（决策表在 engine/dsh_fallback.c）────
   * 这一组验**接线**：按决策表的结论真去干活之后，交出来的 EntryPayload 对不对。 */
  {
    const char *names[] = {"link.mdx", "test.mdx"};
    char *yid[4] = {(char[65]){0}, (char[65]){0}, NULL, NULL};
    dsh_engine *e = engine_with(names, 2, yid);
    ok(e != NULL, "⑤ 建引擎（link.mdx + test.mdx）");
    if (e != NULL) {
      char *out = NULL;
      /* 当前词典 = link.mdx（id 在清单末尾那一本 = test.mdx，所以取 ids[0] 那本手动设） */
      ok(dsh_engine_dict_set_current(e, yid[0], &out) == DSH_OK,
         "⑤ 当前词典 = link.mdx（第一本）");
      if (out != NULL) dsh_release(out);

      /* ① 选区那条路：直接查当前词典，命中 */
      char *json = NULL;
      ok(dsh_engine_lookup(e, "apple", DSH_ORIGIN_SELECTION, NULL, &json) == DSH_OK,
         "⑤ 选区查命中词");
      ok(has(json, "\"found\":true"), "⑤ 命中 → found 为真");
      ok(has(json, "\"keyText\":\"apple\""), "⑤ 落点键名就是 apple");
      ok(has(json, "\"via\":\"current\""), "⑤ 当前词典命中 → via=current");
      ok(has(json, "\"stage\":\"afterLookup\""), "⑤ stage 说的是「哪一步给出的答复」");
      ok(has(json, "\"surface\":\"none\""), "⑤ 这一档没有候选面板");
      {
        char url[256];
        json_str_field(json, "entryUrl", url, sizeof(url));
        char want[256];
        snprintf(want, sizeof(want), "https://%s.dictres.invalid/__entry__?word=apple", yid[0]);
        ok_eq_str(url, want, "⑤ 词条地址与参考实现同一个形状（宿主直接喂 iframe）");
      }
      ok(has(json, "\"plainText\":\""), "⑤ 纯文本（复制释义用）已经在核内剥好了");
      ok(!has(json, "<"), "⑤ 纯文本里不许留标签");
      if (json != NULL) dsh_release(json);

      /* ② 落点：变形形式被 @@@LINK 重定向到原型（与 resolve 同一条解析） */
      json = NULL;
      ok(dsh_engine_lookup(e, "apples", DSH_ORIGIN_SELECTION, NULL, &json) == DSH_OK,
         "⑤ 选区查 apples");
      ok(has(json, "\"keyText\":\"apple\""), "⑤ 落点必须跟到 apple（不是 apples）");
      ok(has(json, "\"linkedTo\":\"apple\""), "⑤ 跟着重定向时要把目标如实报出来");
      if (json != NULL) dsh_release(json);

      /* ③ 输入框那条路 · 精确命中：**接着往下查**。
       * 联想回来之后要先找 `kind == 'exact'` 的那条、有就直接查它 —— 漏掉这一步，
       * 输入框里打一个存在的词按回车只会出候选列表、正文永远空着。 */
      json = NULL;
      ok(dsh_engine_lookup(e, "apple", DSH_ORIGIN_INPUT, NULL, &json) == DSH_OK,
         "⑤ 输入框查 apple（link.mdx 里确实有这一条）");
      ok(has(json, "\"found\":true") && has(json, "\"keyText\":\"apple\""),
         "⑤ ★ 候选里有精确命中 → 一路查到词条（不是停在候选列表上）");
      ok(has(json, "\"via\":\"current\"") && has(json, "\"stage\":\"afterLookup\""),
         "⑤ 这一页是「当前词典那一步」给出来的");
      ok(!has(json, "\"surface\":\"list\""), "⑤ 命中这一档不摆候选列表");
      if (json != NULL) dsh_release(json);

      /* ③b 只有前缀候选、**没有**精确命中 → 摆候选列表并停下 */
      json = NULL;
      ok(dsh_engine_lookup(e, "app", DSH_ORIGIN_INPUT, NULL, &json) == DSH_OK,
         "⑤ 输入框查 app（link.mdx 里只有 apple / apples 是它的前缀）");
      ok(has(json, "\"via\":\"current\"") && has(json, "\"surface\":\"list\""),
         "⑤ 英文输入且没有精确命中：摆候选列表（surface=list）");
      ok(has(json, "\"stage\":\"afterSuggest\""), "⑤ 停在联想那一步");
      ok(has(json, "\"word\":\"apple\"") && !has(json, "\"kind\":\"exact\""),
         "⑤ 候选里给出 apple，而且**没有**标成精确命中");
      /* ③b2 ★ **"没打完的半个词"那一档不许给提示页地址**（与下面"查不到"那一档是一对）：
       * `surface=list` 的意思是"列表摆出来、交给用户挑"，所以这一档 `entryUrl` 必须是空 ——
       * 给了的话正文框会被换成一张「未在《…》中找到」的提示页，而那正是不许发生的行为。 */
      ok(has(json, "\"entryUrl\":\"\"") && has(json, "\"keyText\":\"\""),
         "⑤ ★ 没打完的半个词那一档**不给**提示页地址（列表摆出来就是终点，正文一步不动）");
      /* …朗读那一档同一条例外：没查过这个词就**没有要念的东西**，
       * 否则面板上那个「朗读」会亮着去念一个没打完的半个词（系统语音认得任何字符串）。 */
      ok(has(json, "\"speakText\":null"),
         "⑤ ★ 没打完的半个词那一档也不给朗读文本（`speakText` 是 null）");
      if (json != NULL) dsh_release(json);

      /* ③b3 ★ **朗读念的是"词典命中的那个词"**，不是用户打进去的写法。
       * 检查标准钉的是**那个字段本身等于词典的规范键名**：只断 `speakText` 非空的话，
       * 把 `query` 塞进去（打 `Apple` 就念 `Apple`）也照样绿。 */
      json = NULL;
      ok(dsh_engine_lookup(e, "Apple", DSH_ORIGIN_INPUT, NULL, &json) == DSH_OK,
         "⑤ 输入框查 Apple（大写写法）");
      ok(has(json, "\"keyText\":\"apple\""), "⑤ 落点是词典的规范键名（小写 apple）");
      ok(has(json, "\"speakText\":\"apple\""),
         "⑤ ★★ 朗读念的也是**命中的那个词**（不是用户打的 Apple）");
      if (json != NULL) dsh_release(json);

      /* ③c 同一件事换成 test.mdx */
      json = NULL;
      ok(dsh_engine_lookup(e, "apple", DSH_ORIGIN_INPUT, yid[1], &json) == DSH_OK,
         "⑤ 从 test.mdx 出发，输入框查 apple");
      ok(has(json, "\"found\":true"), "⑤ ★ test.mdx 里 apple 也直接出词条");
      if (json != NULL) dsh_release(json);

      /* ③d ★ **链接那条路**（`entry://` 点一下）：在**起点那一本**里精确查一次，
       *     **不走查词通道**（不借查、不翻译）。
       * ⚠️ 起点那一步要是直接给一张 SHOW（什么都没查、`entryUrl` 空），点词条里的链接
       *    就会得到一张空白页。这里"当前词典"是 link.mdx，正文必须来自指定的 test.mdx。 */
      json = NULL;
      ok(dsh_engine_lookup(e, "banana", DSH_ORIGIN_LINK, yid[1], &json) == DSH_OK,
         "⑤ 链接那条路：在 test.mdx 里查 banana（当前词典 link.mdx 里没有它）");
      ok(has(json, "\"found\":true") && has(json, "\"keyText\":\"banana\""),
         "⑤ ★ 链接命中 → **出词条**（不再是一张什么都没有的页）");
      {
        char id[256];
        json_str_field(json, "dictId", id, sizeof(id));
        ok_eq_str(id, yid[1], "⑤ ★ 正文来自**指定的那一本**（不是当前词典）");
      }
      ok(has(json, "\"via\":\"current\""), "⑤ 链接命中 → via=current（这一页不写「借查」）");
      ok(has(json, "dictres.invalid/__entry__?word=banana"),
         "⑤ 链接命中也要给出词条地址（壳直接喂 iframe）");
      if (json != NULL) dsh_release(json);

      /* ③e 链接指向的词那一本里没有 → **到此为止**（不借查、不翻译、不给按钮） */
      json = NULL;
      ok(dsh_engine_lookup(e, "zzz-no-such-link", DSH_ORIGIN_LINK, yid[1], &json) == DSH_OK,
         "⑤ 链接指向的词在那一本里没有");
      ok(has(json, "\"via\":\"terminal\"") && has(json, "\"found\":false"),
         "⑤ ★ 链接没查到 → 终态页（**不是**借查，也不是翻译）");
      ok(!has(json, "借查"), "⑤ ★ 链接那条路绝不借查（D11：退了就退不回来）");
      ok(has(json, "\"offerRecheck\":false") && has(json, "\"offerTranslate\":false"),
         "⑤ ★ 也不给「再问一遍」「翻译」两个按钮");
      ok(has(json, "\"chips\":[]"), "⑤ ★ 一条出路都给不出（这条路上一次探路都没发生）");
      if (json != NULL) dsh_release(json);

      /* ④ 汉字输入：跳过联想直接查 */
      json = NULL;
      ok(dsh_engine_lookup(e, "测试", DSH_ORIGIN_INPUT, yid[1], &json) == DSH_OK,
         "⑤ 汉字输入查 test.mdx（它里面有「测试」）");
      ok(has(json, "\"found\":true") && has(json, "\"keyText\":\"测试\""),
         "⑤ 汉字输入直接命中（没有停在联想那一步）");
      ok(has(json, "\"stage\":\"afterLookup\""),
         "⑤ ★ 汉字输入**跳过**联想：stage 直接到 afterLookup");
      ok(!has(json, "\"surface\":\"list\""), "⑤ 汉字输入不会摆英文词典的候选列表");
      if (json != NULL) dsh_release(json);

      /* ⑤ 借查：当前词典（link）没有、另一本（test）有 */
      json = NULL;
      ok(dsh_engine_lookup(e, "banana", DSH_ORIGIN_SELECTION, NULL, &json) == DSH_OK,
         "⑤ link 里没有 banana → 借查");
      ok(has(json, "\"via\":\"borrow\""), "⑤ 借查命中 → via=borrow");
      {
        char id[256];
        json_str_field(json, "dictId", id, sizeof(id));
        ok_eq_str(id, yid[1], "⑤ 正文是**借来那一本**给的（dictId 是它，且不切当前词典）");
      }
      ok(has(json, "已用《test.mdx》借查") && has(json, "当前词典没有"),
         "⑤ 解释行照参考实现的原话写（「当前词典没有 · 已用《test.mdx》借查」）");
      ok(has(json, "\"stage\":\"afterProbe\""), "⑤ 停在借查那一步");
      if (json != NULL) dsh_release(json);

      /* ⑥ 连续借查：起点是借来的那一本、绕回当前词典时**不是**借查 */
      json = NULL;
      ok(dsh_engine_lookup(e, "apple", DSH_ORIGIN_SELECTION, yid[1], &json) == DSH_OK,
         "⑤ 从 test.mdx 出发查 apple（它没有，而当前词典 link 有）");
      ok(has(json, "\"via\":\"current\""),
         "⑤ ★ 借查绕回**当前词典** → via=current（不是 borrow，）");
      ok(!has(json, "借查"), "⑤ ★ 这一页不写「借查」那行解释（两处都说反了是 那个 bug）");
      if (json != NULL) dsh_release(json);

      /* ⑦ 三档都没有：终态页 + 「翻译为什么没用上」 */
      json = NULL;
      ok(dsh_engine_lookup(e, "zzz-nothing-here", DSH_ORIGIN_SELECTION, NULL, &json) == DSH_OK,
         "⑤ 两本都没有 → 终态页（不是报错）");
      ok(has(json, "\"via\":\"terminal\""), "⑤ 终态页 via=terminal");
      ok(has(json, "\"found\":false"), "⑤ 终态页 found 为假");
      /* ★ 选区那条路的终态说的是**用户看得懂的那句话**「X未在《…》中查到，别的词典里也没有」，
       * 而且 `surface = toast` —— 界面据此**一个字都不动正文**（把正在读的正文换成一张
       * "没找到"的页，等于惩罚用户点了一下）。那句文案由内核给，界面只画不拼。 */
      ok(has(json, "zzz-nothing-here未在《link.mdx》中查到") && has(json, "别的词典里也没有"),
         "⑤ ★ 选区终态那句解释：把词与书名写出来（≤4 个词），并说清「都没有」");
      ok(has(json, "\"surface\":\"toast\""),
         "⑤ ★★ 而且 `surface = toast` —— 界面据此**不替换正文**（只在正文框底部说一句）");
      ok(has(json, "translateWhy") && has(json, "总开关"),
         "⑤ ★ 翻译为什么没用上：这一版如实说「总开关关着」（设置模型里还没有翻译一节）");
      ok(has(json, "\"offerTranslate\":false"),
         "⑤ 总开关关着时不给那条可点出路（该指去选项）");
      ok(has(json, "\"offerRecheck\":false"),
         "⑤ 两本都问完了 → 不给「再问一遍」，而且这时才可以说「都没有」");
      ok(has(json, "\"unconfirmed\":[]"), "⑤ 没问完的清单是空的（真的都问完了）");
      ok(has(json, "\"chips\":[]"),
         "⑤ ★ 一条出路都给不出时，chips 是空数组（点了也没用的按钮不许出现）");
      /* ⑦c ★ **没命中也要把提示页那个地址给出来**：`entryUrl` 只在命中时才填的话，
       * "查不到"时正文框是空的，而那张提示页上的候选正是**可点的 `entry://` 链接**
       * ——"点候选接着查"整条路都靠它。地址用**起点那一本**，词用**查询词本身**。 */
      {
        char url[512];
        json_str_field(json, "entryUrl", url, sizeof(url));
        char want[512];
        snprintf(want, sizeof(want),
                 "https://%s.dictres.invalid/__entry__?word=zzz-nothing-here", yid[0]);
        ok_eq_str(url, want, "⑤ ★ 没命中时 entryUrl 指向**那一本的提示页**（不是空串）");
        ok(has(json, "\"keyText\":\"zzz-nothing-here\""),
           "⑤ ★ 没命中时 keyText 就是查询词本身");
        {
          char id[256];
          json_str_field(json, "dictId", id, sizeof(id));
          ok_eq_str(id, yid[0], "⑤ ★ 没命中时也如实说是**哪一本**没查到（当前词典）");
        }
        ok(!has(json, "\"dictTitle\":\"\""), "⑤ 没命中时词典名也照给（标题栏那一格不许空）");
        /* …而朗读那一档**照给**（与"没打完的半个词"那条正好是一对）：查不到不等于念不出 ——
         * 系统语音认得任何词，用户要的就是"听一下它怎么念"。 */
        ok(has(json, "\"speakText\":\"zzz-nothing-here\""),
           "⑤ ★ 查不到时朗读文本仍然给（就是查询词本身）—— 与「没打完的半个词」那一档不是一回事");
      }
      if (json != NULL) dsh_release(json);

      /* ⑦d ★★ **查词通道真的会走到"该翻译"那一档**：四个条件（总开关 + 自动翻译 + Key + 语种支持）
       * 全为真时，链给 `via = "translate"` + `needsTranslate = true`，由外壳把请求发出去。
       * ⚠️ 这个动作以前返回「没接」（那时四条件恒假）—— 四条件可能为真之后，那返回值就是
       *    "用户一查生词就报错"，所以必须在这里钉住。 */
      {
        char *set_out = NULL;
        ok(dsh_engine_settings_set(e,
                                   "{\"translate\":{\"enabled\":true,\"autoTranslate\":true},"
                                   "\"volcengine\":{\"apiKey\":\"k-test\"}}",
                                   &set_out) == DSH_OK,
           "⑤ 把翻译开关打开并填上 Key");
        if (set_out != NULL) dsh_release(set_out);

        json = NULL;
        ok(dsh_engine_lookup(e, "zzz-nothing-here", DSH_ORIGIN_SELECTION, NULL, &json) == DSH_OK,
           "⑤ ★ 四个条件都满足时，查询**照常成功**（不是 NOT_IMPLEMENTED）");
        ok(has(json, "\"needsTranslate\":true"),
           "⑤ ★★ 链说「该翻译」：needsTranslate=true（外壳据此接着把翻译做完）");
        ok(has(json, "\"via\":\"translate\""), "⑤ ★ 而且 via=translate（界面据此写解释行）");
        ok(has(json, "\"translateWhy\":\"\""),
           "⑤ 这一档**没有**「为什么没翻译」那句话（不是没用上，是正要翻）");
        ok(has(json, "\"found\":false"),
           "⑤ 这一刻还没翻出来，所以 found 仍是假（外壳补上译文才为真）");
        if (json != NULL) dsh_release(json);

        /* 语言不支持那一档：`via` 落回 terminal，那句话点明是语种的事（四档分开说） */
        ok(dsh_engine_settings_set(e, "{\"translate\":{\"autoTranslate\":false}}", &set_out) ==
               DSH_OK,
           "⑤ 关掉自动翻译（用户自己关的）");
        if (set_out != NULL) dsh_release(set_out);
        json = NULL;
        ok(dsh_engine_lookup(e, "zzz-nothing-here", DSH_ORIGIN_SELECTION, NULL, &json) == DSH_OK,
           "⑤ 自动翻译关着 → 回到终态页");
        ok(has(json, "\"via\":\"terminal\""), "⑤ 这一档是 terminal");
        ok(has(json, "\"offerTranslate\":true"),
           "⑤ ★★ 但给出一条**可点的**「翻译这个词」（关掉自动 ≠ 不要翻译 —— D12）");
        ok(has(json, "\"needsTranslate\":false"), "⑤ 而且这一档不需要外壳做什么");

        /* 收尾：把翻译设置关回去，后面几条仍然按"没配翻译"的前提走 */
        ok(dsh_engine_settings_set(e,
                                   "{\"translate\":{\"enabled\":false,\"autoTranslate\":true},"
                                   "\"volcengine\":{\"apiKey\":null}}",
                                   &set_out) == DSH_OK,
           "⑤ 把翻译设置关回去");
        if (set_out != NULL) dsh_release(set_out);
        if (json != NULL) dsh_release(json);
      }

      json = NULL;
      ok(dsh_engine_lookup(e, "applz", DSH_ORIGIN_SELECTION, yid[1], &json) == DSH_OK,
         "⑤ 一个拼错的词（两本都没有，但 test.mdx 能给出纠正候选）");
      ok(has(json, "\"surface\":\"toast\""),
         "⑤ ★ 选区那条路：候选**并进正文框底部提示**（不摆列表顶掉正文）");
      ok(has(json, "\"via\":\"terminal\""),
         "⑤ 这一档的结局仍然是 terminal（界面据此**不替换正文**）");
      ok(has(json, "\"word\":\"apple\""), "⑤ 候选真的给出来了（界面照着画那几个可点的词）");
      /* ⑦b2 ★ 那个地址上的**提示页**里，候选真的是可点的 `entry://` 链接 ──
       *      上一组 `test_entry_doc.c` 钉的是"提示页怎么拼"，这里钉的是"lookup 给的地址
       *      **真的通到**那一页"：地址 → 取文档 → 文档里有 `entry://`。
       *      少了这一半，前面那条 `entryUrl` 断言可能指向一张空页而没人发现。 */
      {
        char url[512];
        json_str_field(json, "entryUrl", url, sizeof(url));
        char want[512];
        snprintf(want, sizeof(want), "https://%s.dictres.invalid/__entry__?word=applz", yid[1]);
        ok_eq_str(url, want, "⑤ ★ 起点了哪一本，提示页地址就是哪一本（这里是 test.mdx）");
        char *doc = NULL;
        ok(dsh_engine_entry_document(e, yid[1], "applz", &doc) == DSH_OK && doc != NULL,
           "⑤ 照着那个地址取提示页");
        ok(has(doc, "未在") && has(doc, "applz"), "⑤ 提示页里有那句「未在《…》中找到「applz」」");
        ok(has(doc, "entry://apple"),
           "⑤ ★ 提示页里的候选是**可点的** entry:// 链接（点它就能接着查）");
        if (doc != NULL) dsh_release(doc);
      }
      if (json != NULL) dsh_release(json);

      /*
       * ── ⑤d ★★ **音节分隔点：先原样问，问不到再去掉点问一遍**（参考实现）────
       *
       * 词典自己的排版会把音节切开：`pro·gress` / `dic·tion·ar·y`。用户从正文里
       * **选中**这样一个词来查时，原样去问多半问不到 —— 而词典里明明有 `progress`。
       * 用户的报法就是这一句：「选中的文本中含有『·』…请先去掉这种符号后再查」，
       * 而且把顺序说死了：**① 先按原样搜一遍；② 没命中才修剪掉分隔符再搜一遍；
       * ③ 再没有才进机器翻译兜底。**
       *
       * 参考实现修在 `LookupText` + `App.cs`（在 `FallbackPlan.Decide` **之前**做那一步），
       * 0.2.0 原来只在界面那侧对**链接**做过，内核这条链漏了 ——
       * 症状正是那样：**明明有词条，却一路走到机器翻译**（出了译文）。
       *
       * 这一节钉四件事（决策表那半在 `test_fallback.c` ①）：
       *   ① 去掉点之后**真查到了那一条**（`keyText` 是 apple，不是 app·le）；
       *   ② 这一页由**当前词典**给出（不是借查、不是翻译、也不是底部的 toast）；
       *   ③ 四个翻译条件全为真时**也不许被翻译抢走**；
       *   ④ 去掉点之后**还是**没有的词，**照旧**落到机器翻译那一档（第 ③ 步不许被吃掉）。
       */
      {
        char *set_out = NULL;
        char *j = NULL;

        /*
         * 摆位：`sameAsShown` 那条检查标准拿"上次交出去的那条"比（见上面 ⑤z）——
         * 先把"现在显示的是哪条"挪到 **banana**（它在 test.mdx 里，走借查命中），
         * 免得下面查 apple 时被"就在眼前"那一档截住、验不到"真的去点重问了一遍"。
         */
        ok(dsh_engine_lookup(e, "banana", DSH_ORIGIN_INPUT, NULL, &j) == DSH_OK && j != NULL,
           "⑤d 摆位：先让界面显示 banana（把「上次交出去的那条」挪开）");
        ok(has(j, "\"keyText\":\"banana\"") && has(j, "\"found\":true"),
           "⑤d 摆位成功（借查命中 test.mdx 的 banana）");
        if (j != NULL) dsh_release(j);
        j = NULL;

        /* ① 原样问一遍（带点）→ ② 没命中 → 去掉点再问一遍 → 命中 */
        ok(dsh_engine_lookup(e, "app·le", DSH_ORIGIN_SELECTION, NULL, &json) == DSH_OK,
           "⑤d ★★ 选区查「app·le」（link.mdx 里只有 apple，没有带点的那条）");
        ok(has(json, "\"found\":true"),
           "⑤d ★★ 去点之后**查到了**（原样那一次没命中，这一步是第二次问）");
        ok(has(json, "\"keyText\":\"apple\""),
           "⑤d ★★ 落点键名是 apple —— 也就是说第二次问用的是**去掉点之后的词**");
        ok(has(json, "\"via\":\"current\""),
           "⑤d ★ 这一页由**当前词典**给出（不是借查、不是终态页）");
        ok(has(json, "\"stage\":\"afterLookup\""),
           "⑤d 而且停在「当前词典查完了」那一步（去点重问回到起点再走一遍，回到的还是这一步）");
        ok(has(json, "\"surface\":\"none\""),
           "⑤d ★ 正文照常替换（不是只在底部说一句的 toast —— 用户要的是「查到那一条」）");
        if (json != NULL) dsh_release(json);
        json = NULL;

        /*
         * ★ 重音符那两个字（2026-09 加的 `ˈ` / `ˌ`）：走的是**同一条**去点重问，
         * 所以这里只钉"链真的接上了" —— 免得将来有人把新码点加进接口定义、却忘了内核要读生成表。
         * （码点本身那几条检查标准在 `test_text.c` ⑤，接口定义表在 `abi_compile_check.c` 钉。）
         */
        ok(dsh_engine_lookup(e, "\xCB\x88" "apple", DSH_ORIGIN_SELECTION, NULL, &json) == DSH_OK,
           "⑤d 选区查「ˈapple」（U+02C8 主重音）");
        ok(has(json, "\"found\":true") && has(json, "\"keyText\":\"apple\""),
           "⑤d ★ 重音符也一样去得掉：查得到《apple》");
        ok(has(json, "\"needsTranslate\":false"),
           "⑤d 而且没被翻译抢走（与「app·le」同一条路）");
        if (json != NULL) dsh_release(json);
        json = NULL;

        /* 输入框那条路同样先问词典：拿带点的词去拼前缀只会得到一串噪音候选 */
        ok(dsh_engine_lookup(e, "app·le", DSH_ORIGIN_INPUT, NULL, &json) == DSH_OK,
           "⑤d 输入框里打「app·le」再回车");
        ok(has(json, "\"found\":true") && has(json, "\"keyText\":\"apple\""),
           "⑤d ★ 一样查得到（带点的那次不进联想：先原样问，问不到去点再问）");
        ok(!has(json, "\"surface\":\"list\""),
           "⑤d ★★ 而且**不会停在候选列表上**（那正是「明明有词条却只出候选」的老症状）");
        if (json != NULL) dsh_release(json);
        json = NULL;

        /* ③ 四个翻译条件全为真时也不许被翻译抢走 */
        ok(dsh_engine_settings_set(e,
                                   "{\"translate\":{\"enabled\":true,\"autoTranslate\":true},"
                                   "\"volcengine\":{\"apiKey\":\"k-test\"}}",
                                   &set_out) == DSH_OK,
           "⑤d 把翻译四个条件都摆成真（不摆真，这一条验不出「没被翻译抢走」）");
        if (set_out != NULL) dsh_release(set_out);
        set_out = NULL;
        ok(dsh_engine_lookup(e, "app·le", DSH_ORIGIN_SELECTION, NULL, &json) == DSH_OK,
           "⑤d ★★ 四个条件全为真时再查一次「app·le」");
        ok(has(json, "\"found\":true") && has(json, "\"keyText\":\"apple\""),
           "⑤d ★★ 仍然是**词典里那一条**（就是这一格：有词条却出了译文）");
        ok(has(json, "\"needsTranslate\":false") && has(json, "\"via\":\"current\""),
           "⑤d ★★ 翻译**没被用上**（needsTranslate 为假、via 也不是 translate）");
        if (json != NULL) dsh_release(json);
        json = NULL;

        /*
         * ④ 反向对照：去点之后**还是**没有的词，**照旧**落到机器翻译那一档 ——
         *    这条顺序是"先修剪、后翻译"，不是"带点就不翻译"（用户的第 ③ 步）。
         */
        /*
         * ⚠️ `zzz·nothing` 去掉点之后是 **`zzznothing`**（点是被**删掉**，不是换成连字符）
         *    —— `dic·tion·ar·y` → `dictionary` 就是这条约定，别照"点变短横"去写期望值。
         */
        ok(dsh_engine_lookup(e, "zzz·nothing", DSH_ORIGIN_SELECTION, NULL, &json) == DSH_OK,
           "⑤d 查一个去点之后也没有的词（zzz·nothing → zzznothing）");
        ok(has(json, "\"needsTranslate\":true") && has(json, "\"via\":\"translate\""),
           "⑤d ★★ 两次都没命中 → **照旧进机器翻译兜底**（去点不许把第 ③ 步吃掉）");
        /*
         * 而且这一整条链用的是**去掉点之后**的那个词：提示页地址、`keyText`、
         * 借查用的词都是它（`RELOOKUP` 换的是整条链的 `query`，见内核那段注释）。
         * 这一格比"found 为假"值钱得多 —— 它钉住"换词"这件事**真的传下去了**，
         * 而不是只换了判断用的那一格。
         */
        {
          char url[512];
          json_str_field(json, "entryUrl", url, sizeof(url));
          char want[512];
          snprintf(want, sizeof(want),
                   "https://%s.dictres.invalid/__entry__?word=zzznothing",
                   yid[0] != NULL ? yid[0] : "?");
          ok_eq_str(url, want,
                    "⑤d ★★ 没命中的那一页按**去点之后**的词给地址（带点的写法不许漏进链里）");
        }
        ok(has(json, "\"keyText\":\"zzznothing\""),
           "⑤d 交出去的 keyText 也是去点之后的那份");
        if (json != NULL) dsh_release(json);
        json = NULL;

        /* 收尾：把翻译设置关回去，后面几条仍按"没配翻译"的前提走 */
        ok(dsh_engine_settings_set(e,
                                   "{\"translate\":{\"enabled\":false,\"autoTranslate\":true},"
                                   "\"volcengine\":{\"apiKey\":null}}",
                                   &set_out) == DSH_OK,
           "⑤d 把翻译设置关回去（后面的用例仍按「没配翻译」走）");
        if (set_out != NULL) dsh_release(set_out);
      }

      /* ⑧ 空文字：不查、如实说 */
      json = NULL;
      ok(dsh_engine_lookup(e, "   ", DSH_ORIGIN_INPUT, NULL, &json) == DSH_OK,
         "⑤ 空文字也成功返回（不是报错）");
      ok(has(json, "没有要查的文字") && has(json, "\"found\":false"), "⑤ 空文字如实说");
      if (json != NULL) dsh_release(json);

      /* ⑨ 参数边界 */
      ok(dsh_engine_lookup(e, "x", DSH_ORIGIN_INPUT, NULL, NULL) != DSH_OK,
         "⑤ out 为 NULL 必须失败");
      ok(dsh_engine_lookup(e, NULL, DSH_ORIGIN_INPUT, NULL, &json) != DSH_OK,
         "⑤ text 为 NULL 必须失败（选中内容由宿主传进来）");
      ok(dsh_engine_lookup(NULL, "x", DSH_ORIGIN_INPUT, NULL, &json) != DSH_OK,
         "⑤ 引擎为 NULL 必须失败");
      ok(dsh_engine_lookup(e, "x", DSH_ORIGIN_INPUT, "no-such-id", &json) == DSH_OK,
         "⑤ 指一本词库里没有的词典：成功返回（错误写在 reason 里）");
      ok(has(json, "词库里没有这本词典") && has(json, "\"found\":false"),
         "⑤ 并且如实说「词库里没有这本」");
      if (json != NULL) dsh_release(json);

      /*
       * ── ⑤z ★ **"落点就是当前那条词条"**（第三十六轮）───────────────────────────
       *
       * 约定出处：`docs/design/查词兜底通道与历史记录开发指导.md`  C 第 2 条 ——
       * 在正文里选中一段文字、点「查这个词」时，如果落点**就是正在读的那条**，
       * 那就**不跳、也不进查词通道**，只在正文框底部说一句。
       * 跳过去的后果是把同一篇正文重载一遍、阅读位置回到顶部
       * （用户 2026-09 报的"看起来就是跳转到本词条顶部（不是滚上去）"）。
       *
       * ⚠️ 检查标准必须是**解析之后的落点**，不能拿输入的文字比：
       *    词典把 `apples` 重定向到 `apple`，正在看 `apple` 时选中 `apples`，
       *    字面上并不相等 —— 而它们就是同一条词条。下面专门钉了这一条。
       *
       * ⚠️ 这一组**必须以 `origin = input` 开头把"现在显示的是哪条"摆好**：
       *    引擎记的是"上一次交出去的那条"，而这几个用例是靠它判的。
       */
      {
        char *j = NULL;
        /* 先让界面"显示" apple —— 走输入那条路（那条路**永远**是照常跳的，见下）*/
        ok(dsh_engine_lookup(e, "apple", DSH_ORIGIN_INPUT, NULL, &j) == DSH_OK && j != NULL,
           "⑤z 输入框查 apple（把「现在显示的是 apple」摆好）");
        ok(has(j, "\"sameAsShown\":false") && !has(j, "\"entryUrl\":\"\""),
           "⑤z ★ `input` 那条路**不判**这件事：同一个词再查一遍要真的重载一次"
           "（那是「再问一遍」的语义），所以照旧给地址");
        if (j != NULL) dsh_release(j);

        /* ① 选区查**同一个词**：落点就在眼前 → 不跳，给一句人话 */
        j = NULL;
        ok(dsh_engine_lookup(e, "apple", DSH_ORIGIN_SELECTION, NULL, &j) == DSH_OK && j != NULL,
           "⑤z 选区再查一次 apple");
        ok(has(j, "\"sameAsShown\":true"),
           "⑤z ★★ 落点就是当前那条 → `sameAsShown` 为真");
        ok(has(j, "\"entryUrl\":\"\""),
           "⑤z ★★ 而且**一个地址都不给**（界面据此一个字都不动正文 —— 给了地址就会"
           "加 `&t=` 把同一篇正文重载一遍、阅读位置回到顶部）");
        {
          /* 那句话**逐字**钉住（`ok_eq_str` 会把实际值打出来，红了就能直接看差在哪）*/
          char got_reason[256];
          json_str_field(j, "reason", got_reason, sizeof(got_reason));
          ok_eq_str(got_reason, "「apple」就是当前词条（apple）",
                    "⑤z ★ 那句话**由内核给**（参考实现里是界面拼的，0.2.0 按硬规则 1 搬进来）");
        }
        ok(has(j, "\"found\":true"),
           "⑤z 词条是真找到了（只是它就在眼前）—— 不许说成 found:false");
        if (j != NULL) dsh_release(j);

        /*
         * ② ★★ 变形形式：看 `apple`、选 `apples` —— **字符串比不出来，落点比得出来**。
         *    这一条是这次检查标准的核心：参考实现当年就是拿字符串比出来的那个 bug。
         */
        j = NULL;
        ok(dsh_engine_lookup(e, "apples", DSH_ORIGIN_SELECTION, NULL, &j) == DSH_OK && j != NULL,
           "⑤z 看 apple 时选区查 apples（link.mdx 把它重定向到 apple）");
        ok(has(j, "\"keyText\":\"apple\"") && has(j, "\"sameAsShown\":true"),
           "⑤z ★★ `apples` 的落点是 `apple`、而 `apple` 正在显示 → 也算「就是当前词条」"
           "（**不许**拿 `\"apples\" != \"apple\"` 去比）");
        ok(has(j, "\"entryUrl\":\"\""), "⑤z 所以同样不给地址");
        if (j != NULL) dsh_release(j);

        /* ③ 链接那条路**同一条约定**（参考实现里两处各写了一遍，这里只有一份实现）*/
        j = NULL;
        ok(dsh_engine_lookup(e, "apple", DSH_ORIGIN_LINK, NULL, &j) == DSH_OK && j != NULL,
           "⑤z 词条里的链接也指向 apple");
        ok(has(j, "\"sameAsShown\":true") && has(j, "\"entryUrl\":\"\""),
           "⑤z ★ `link` 那条路判得一样（同一份实现，不是两处各写一遍）");
        if (j != NULL) dsh_release(j);

        /* ④ 落点是**别的**词条：照常跳，而且给地址 */
        j = NULL;
        ok(dsh_engine_lookup(e, "banana", DSH_ORIGIN_SELECTION, NULL, &j) == DSH_OK && j != NULL,
           "⑤z 选区查一个**别的**词（banana，借到 test.mdx 那本）");
        ok(has(j, "\"sameAsShown\":false"),
           "⑤z ★ 落点不是当前那条 → 照常跳（不许一概说「就是当前词条」）");
        ok(has(j, "\"entryUrl\":\"https://") && !has(j, "\"entryUrl\":\"\""),
           "⑤z 而且地址照给（界面靠它换正文）");
        if (j != NULL) dsh_release(j);

        /*
         * ⑤ **同一本**才算：`apple` 在 test.mdx 里也有，但界面显示的是 link.mdx 那条。
         *    起点换成 test.mdx 时落点是"另一本里的 apple" —— 那是**另一条词条**，
         *    该跳（正文要换成那一本的）。
         */
        j = NULL;
        ok(dsh_engine_lookup(e, "apple", DSH_ORIGIN_SELECTION, yid[1], &j) == DSH_OK && j != NULL,
           "⑤z 从 test.mdx 那本出发选 apple（显示着的那个 apple 来自 link.mdx）");
        ok(has(j, "\"sameAsShown\":false") && has(j, "\"dictId\":\"") && has(j, "\"entryUrl\":\"https://"),
           "⑤z ★ **哪一本也要对得上**：另一本里的同名词条是另一条，照常跳");
        if (j != NULL) dsh_release(j);

        /*
         * ⑥ 查不到时**什么都没显示**（正文框那会儿摆的是提示页）→ 记忆要清掉，
         *    否则"先查一个查不到的词、再选一个本来就在眼前的词"会被误判成"不用跳"。
         */
        j = NULL;
        (void)dsh_engine_lookup(e, "apple", DSH_ORIGIN_INPUT, NULL, &j);
        if (j != NULL) dsh_release(j);
        j = NULL;
        ok(dsh_engine_lookup(e, "zzz-nothing-here", DSH_ORIGIN_INPUT, NULL, &j) == DSH_OK && j != NULL,
           "⑤z 查一个两本都没有的词（落点是空的）");
        if (j != NULL) dsh_release(j);
        j = NULL;
        ok(dsh_engine_lookup(e, "apple", DSH_ORIGIN_SELECTION, NULL, &j) == DSH_OK && j != NULL,
           "⑤z 再选区查 apple");
        ok(has(j, "\"sameAsShown\":false") && has(j, "\"entryUrl\":\"https://"),
           "⑤z ★★ 上一次查不到 → 「现在显示的是谁」已经清空，这一条要**照常跳**"
           "（把没查到当成「还在显示」是错的）");
        if (j != NULL) dsh_release(j);
      }

      dsh_engine_destroy(e);
    }
  }

  /* ── ⑥ 加载不了 / 没问完：都不许说成"没有"（那个坑）────────────────────────
   * 造这种事实只能绕过 `dict_add`（它本身会挡下不存在的路径 —— 那是设计）。
   * 做法：手写一份设置 JSON（里面有两条，一条路径是假的），
   * 用核内的 `dsh_engine_replace_settings` 直接换掉。 */
  {
    dsh_engine *e = NULL;
    if (dsh_engine_create(NULL, &e) == DSH_OK && e != NULL) {
      char good[1200];
      char *good_path = fixture("test.mdx");
      snprintf(good, sizeof(good), "%s", good_path != NULL ? good_path : "");
      free(good_path);
      char json[4096];
      snprintf(json, sizeof(json),
               "{\"version\":1,\"currentDictId\":\"d-good\",\"dictionaries\":["
               "{\"id\":\"d-good\",\"title\":\"好词典\",\"mdxPath\":\"%s\"},"
               "{\"id\":\"d-gone\",\"title\":\"丢了的词典\",\"mdxPath\":\"/no/such/place.mdx\"}]}",
               good);
      dsh_settings *st = NULL;
      ok(dsh_settings_parse(json, strlen(json), &st) == 0, "⑥ 手写设置解析成功");
      if (st != NULL) {
        ok(dsh_engine_replace_settings(e, st) == 0, "⑥ 换上这份设置");

        /* 起点那本读不动：如实说"加载不了"，不许说成"这本里没有" */
        char *out = NULL;
        ok(dsh_engine_lookup(e, "apple", DSH_ORIGIN_SELECTION, "d-gone", &out) == DSH_OK,
           "⑥ 起点那本读不动时仍然成功返回（错误写在 reason 里）");
        ok(has(out, "加载不了"), "⑥ ★ 读不动就是读不动：reason 说「加载不了」");
        ok(!has(out, "这本词典里没有"), "⑥ ★ 绝不许把它说成「这本词典里没有这一条」");
        if (out != NULL) dsh_release(out);

        /* 借查那一本读不动 → **没问完**，绝不许并成"都没有"（那个坑） */
        out = NULL;
        ok(dsh_engine_lookup(e, "apple-no-such", DSH_ORIGIN_SELECTION, "d-good", &out) == DSH_OK,
           "⑥ 好词典里没有 → 去问那本丢了的");
        ok_has(out, "\"via\":\"terminal\"", "⑥ 两本都问不出结果 → 终态页");
        ok_has(out, "丢了的词典", "⑥ ★ 「没问完」要把那本的名字说出来");
        ok_has(out, "没能确认", "⑥ ★ 「没问完」不许被说成「都没有」（那个坑）");
        ok_has(out, "\"offerRecheck\":true",
               "⑥ ★ 有没问完的词典就**必须**给「再问一遍」（那个坑唯一不许让步的一条）");
        /*
         * ★ 出路按钮：**连按钮上那行字都由内核给**（参考实现里那行文案在前端的
         *   `refreshEntryChips` 里，2026-09 起按硬规则 1 搬进内核）。逐字钉住 ——
         *   这一条红了就说明"界面只画不拼"这条纪律被破坏了。
         */
        ok_has(out,
               "\"chips\":[{\"action\":\"recheck\",\"label\":\"还有 1 本没查完\","
               "\"hint\":\"点一下把这（几）本也找一遍：丢了的词典\","
               "\"word\":\"apple-no-such\"}]",
               "⑥ ★ 终态页那排按钮由内核连**文字**一起给出（界面一个字都不拼）");
        if (out != NULL) dsh_release(out);
      }
      dsh_engine_destroy(e);
    }
  }

  /* ── ⑦ dsh_engine_borrow：「去别的词典里问一遍」（终态页那条「再问一遍」）────
   *
   * 这一节钉的是这条接口**自己的**约定（链里那次自动借查在 ⑥ 已经验过）：
   *   ① 命中：回"在哪一本、落在哪条"，而且**不加载**那一本（那个坑）；
   *   ② 跳过起点那一本（它已经答过"没有"）；
   *   ③ ★ **「没问完」绝不许并成「没有」**（那个坑）—— 读不动的那本要进 `unconfirmed`；
   *   ④ 两档预算的实测结果（自动 300 ms / 「再问一遍」按词长折算）钉在回包里。 */
  {
    const char *names[] = {"test.mdx", "link.mdx"};
    /*
     * ⚠️ `engine_with` 是**往调用方给的缓冲里写** id（`out_ids[i][k] = …`），
     *    不是分配一个字符串给你 —— 所以这里必须给 65 字节的缓冲（与 main 顶上那一行同一条约定）。
     *    第一版写成 `{NULL, NULL, NULL, NULL}`，`engine_with` 往 NULL 里写，第一次调用就段错误。
     */
    char *ids[4] = {(char[65]){0}, (char[65]){0}, (char[65]){0}, (char[65]){0}};
    dsh_engine *e = engine_with(names, 2, ids);
    ok(e != NULL, "⑦ 建引擎并加两本（test.mdx + link.mdx）");
    if (e != NULL) {
      char *out = NULL;
      char *json = NULL;
      ok(dsh_engine_dict_set_current(e, ids[0], &out) == DSH_OK, "⑦ 当前词典 = test.mdx");
      if (out != NULL) dsh_release(out);
      out = NULL;

      /* ① 命中：`apples` 在 link.mdx 里（重定向到 apple）—— 起点那本被跳过 */
      ok(dsh_engine_borrow(e, "apples", NULL, 0, &json) == DSH_OK && json != NULL,
         "⑦ 借查「apples」");
      ok_has(json, "\"hitLanded\":\"apple\"",
             "⑦★ 命中了，而且落点是**规整后的键名**（apples → apple）");
      {
        char want[256];
        snprintf(want, sizeof(want), "\"hitId\":\"%s\"", ids[1] != NULL ? ids[1] : "?");
        ok_has(json, want, "⑦★ 命中的是**另一本**（起点那本被跳过）");
      }
      ok_has(json, "\"unconfirmed\":[]", "⑦ 问到了就没有「没问完」那一项");
      ok_has(json, "\"asked\":1", "⑦ 只问了 1 本（问一句就走，命中就停）");
      if (json != NULL) dsh_release(json);
      json = NULL;

      /* ①b 命中的那本**不许**留在内存里（那个坑：借查 ≠ 加载） */
      {
        char *list = NULL;
        ok(dsh_engine_dict_list(e, &list) == DSH_OK, "⑦ 取词库清单");
        if (list != NULL) {
          int loaded_true = 0;
          for (const char *p = list; (p = strstr(p, "\"loaded\":true")) != NULL; p++) loaded_true++;
          ok(loaded_true == 0,
             "⑦★ 借查之后**没有任何一本留在内存里**（问一句与加载一本必须分得开）");
          dsh_release(list);
        }
      }

      /* ② 词在起点那本里、别的本没有 → 确定"别的词典里也没有"（两档都空） */
      json = NULL;
      ok(dsh_engine_borrow(e, "banana", NULL, 0, &json) == DSH_OK, "⑦ 借查一个只在起点那本里的词");
      ok_has(json, "\"hitId\":\"\"", "⑦ 别的本里没有 → hitId 空");
      ok_has(json, "\"unconfirmed\":[]",
             "⑦★ 而且**没有**「没问完」（答得干脆利落时不许含糊其辞）");
      if (json != NULL) dsh_release(json);
      json = NULL;

      /* ③ ★ 没问完 ≠ 没有：来一本**读不动**的（路径是假的），只能绕过 dict_add 造 */
      {
        dsh_engine *e3 = NULL;
        if (dsh_engine_create(NULL, &e3) == DSH_OK && e3 != NULL) {
          char good[1200];
          char *good_path = fixture("test.mdx");
          snprintf(good, sizeof(good), "%s", good_path != NULL ? good_path : "");
          free(good_path);
          {
            char settings[4096];
            snprintf(settings, sizeof(settings),
                     "{\"version\":1,\"currentDictId\":\"d-good\",\"dictionaries\":["
                     "{\"id\":\"d-good\",\"title\":\"好词典\",\"mdxPath\":\"%s\"},"
                     "{\"id\":\"d-gone\",\"title\":\"丢了的词典\",\"mdxPath\":\"/no/such/place.mdx\"}]}",
                     good);
            dsh_settings *st = NULL;
            ok(dsh_settings_parse(settings, strlen(settings), &st) == 0, "⑦ 手写设置解析成功");
            if (st != NULL) {
              ok(dsh_engine_replace_settings(e3, st) == 0, "⑦ 换上这份设置");
              json = NULL;
              ok(dsh_engine_borrow(e3, "apple-no-such", "d-good", 0, &json) == DSH_OK,
                 "⑦ 借查一个两本都没有的词");
              ok_has(json, "\"hitId\":\"\"", "⑦ 没命中 → hitId 空");
              ok_has(json, "丢了的词典",
                     "⑦★★ 读不动的那本进「没问完」（**绝不许**并成「别的词典里也没有」，那个坑）");
              ok_has(json, "\"unconfirmed\":[\"丢了的词典\"]",
                     "⑦★ 「没问完」报的是**名字**（界面直接列给人看）");
              if (json != NULL) dsh_release(json);
              json = NULL;
            }
          }
          dsh_engine_destroy(e3);
        }
      }

      /* ④ 两档预算的实测结果：自动 300 ms；「再问一遍」= 词长 × 1500 + 2000 */
      json = NULL;
      ok(dsh_engine_borrow(e, "banana", NULL, 0, &json) == DSH_OK, "⑦ 自动那一档（recheck=0）");
      ok_has(json, "\"perDictMs\":120", "⑦ 自动档单本用接口定义常量（120 ms）");
      ok_has(json, "\"totalMs\":300", "⑦ 自动档总预算 300 ms（用户正等着看结果）");
      if (json != NULL) dsh_release(json);
      json = NULL;
      ok(dsh_engine_borrow(e, "banana", NULL, 1, &json) == DSH_OK, "⑦ 「再问一遍」那一档（recheck=1）");
      ok_has(json, "\"perDictMs\":1500", "⑦★ 「再问一遍」单本 1500 ms（用户要答案，可以等）");
      ok_has(json, "\"totalMs\":11000", "⑦★ 总预算 = 6 个字符 × 1500 + 2000 = 11000");
      if (json != NULL) dsh_release(json);
      json = NULL;

      /* ⑤ 一本别的词典都没有（起点那本是唯一一本）→ 两档都空、一本没问 */
      /* ⑤ 一本别的词典都没有（起点那本是唯一一本）→ 两档都空、一本没问。
       * ⚠️ 必须单开一个引擎：上面那个引擎里有**两本**，跳过一本还剩一本可问 ——
       *    第一版就是这么写的，于是"一本都没问"当场红了（现场是命中了另一本）。 */
      {
        const char *one[] = {"test.mdx"};
        char *oid[4] = {(char[65]){0}, (char[65]){0}, (char[65]){0}, (char[65]){0}};
        dsh_engine *e2 = engine_with(one, 1, oid);
        ok(e2 != NULL, "⑦ 建一个只有一本词典的引擎");
        if (e2 != NULL) {
          json = NULL;
          ok(dsh_engine_borrow(e2, "apple", oid[0], 0, &json) == DSH_OK,
             "⑦ 没有别的词典可问时也回得来");
          ok_has(json, "\"asked\":0", "⑦ 一本都没问");
          ok_has(json, "\"hitId\":\"\"", "⑦ hitId 空");
          ok_has(json, "\"unconfirmed\":[]",
                 "⑦★ 也没有「没问完」—— 「确定都没有」与「没问完」是**两档**，绝不许混");
          if (json != NULL) dsh_release(json);
          json = NULL;
          dsh_engine_destroy(e2);
        }
      }

      /* ⑥ 参数边界 */
      ok(dsh_engine_borrow(e, "apple", NULL, 0, NULL) != DSH_OK, "⑦ out 为 NULL 必须失败");
      ok(dsh_engine_borrow(e, NULL, NULL, 0, &json) != DSH_OK, "⑦ text 为 NULL 必须失败");
      ok(dsh_engine_borrow(e, "", NULL, 0, &json) != DSH_OK, "⑦ text 为空必须失败");
      ok(dsh_engine_borrow(e, "   ", NULL, 0, &json) != DSH_OK, "⑦ text 只有空白必须失败");
      ok(dsh_engine_borrow(NULL, "apple", NULL, 0, &json) != DSH_OK, "⑦ 引擎为 NULL 必须失败");

      dsh_engine_destroy(e);
    }
  }

  ok_eq_i64((int64_t)dsh_mem_live_count(), (int64_t)base, "全部用例跑完，活分配表必须回到基线");

  printf("lookup：%d 项，失败 %d\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
