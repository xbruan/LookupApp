/* 词典取样（`dsh_speech_dict_samples` 与它那层纯策略）钉四件事：撒点单位是**词条**不是
 * 词块（1 块 9 条的词典按块撒只给 1 个候选，量中位数就退化成 n=1）、粗筛只放像词条的键去
 * 解正文、清洗装不下要如实拒、每条样本回验都必须是 `kind=entry`；用临时目录且先清再建。*/

#include "dsh_lookup.h"
#include "audio/dsh_dictsample.h"
#include "mem_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    fprintf(stderr, "FAIL %s：实际=%lld 期望=%lld\n", what, (long long)actual, (long long)expected);
  }
}

static void ok_has(const char *text, const char *needle, const char *what) {
  g_checks++;
  if (text == NULL || strstr(text, needle) == NULL) {
    g_failed++;
    fprintf(stderr, "FAIL %s\n      实际=%s\n      里面没有=%s\n", what, text ? text : "(null)",
            needle);
  }
}

/** 从平坦 JSON 里取一个字符串字段（够用就行）*/
static void field(const char *json, const char *key, char *buf, size_t cap) {
  char pat[96];
  const char *p;
  size_t k = 0;
  buf[0] = '\0';
  if (json == NULL) return;
  snprintf(pat, sizeof(pat), "\"%s\":\"", key);
  p = strstr(json, pat);
  if (p == NULL) return;
  p += strlen(pat);
  while (*p != '\0' && *p != '"' && k + 1 < cap) buf[k++] = *p++;
  buf[k] = '\0';
}

/** 从平坦 JSON 里取一个**数字**字段（`field` 只认字符串，`count` 那种数字要它）*/
static int64_t num_field(const char *json, const char *key) {
  char pat[96];
  const char *p;
  if (json == NULL) return -1;
  snprintf(pat, sizeof(pat), "\"%s\":", key);
  p = strstr(json, pat);
  if (p == NULL) return -1;
  p += strlen(pat);
  return (int64_t)strtoll(p, NULL, 10);
}

/** 取 `samples[]` 第 index 项的原文：按层数走、跳过字符串，字段值里可能有 `}`（壳那边 `ShellBridge.SplitArray` 是同一条纪律）*/
static void sample_item(const char *json, int index, char *buf, size_t cap) {
  const char *open;
  const char *p;
  int item = -1;
  int brace = 0;
  int in_string = 0;
  size_t k = 0;

  buf[0] = '\0';
  if (json == NULL) return;
  open = strstr(json, "\"samples\":[");
  if (open == NULL) return;
  for (p = open + strlen("\"samples\":["); *p != '\0'; p++) {
    char c = *p;
    if (in_string) {
      if (c == '\\') {
        /* ⚠️ 转义要连反斜杠带后一个字符一起抄：键名里带 `\` 是常态（`.mdd` 里的路径
         *    就是 `\beep__gb_1.wav`），只 `p++` 跳过会让 `audioKey` 少那个反斜杠、
         *    6 条样本全红，看着像内核两次给的键名不一样 */
        if (item == index && k + 1 < cap) buf[k++] = c;
        p++;
        if (*p == '\0') break;
        if (item == index && k + 1 < cap) buf[k++] = *p;
        continue;
      }
      if (c == '"') in_string = 0;
      if (item == index && k + 1 < cap) buf[k++] = c;
      continue;
    }
    if (c == '"') {
      in_string = 1;
      if (item == index && k + 1 < cap) buf[k++] = c;
      continue;
    }
    if (c == '{') {
      brace++;
      if (brace == 1) item++;
      if (item == index && k + 1 < cap) buf[k++] = c;
      continue;
    }
    if (c == '}') {
      brace--;
      if (item == index && k + 1 < cap) buf[k++] = c;
      if (brace == 0 && item == index) break;
      continue;
    }
    if (item == index && k + 1 < cap) buf[k++] = c;
  }
  buf[k < cap ? k : cap - 1] = '\0';
}

/* ① 均匀撒点（纯函数）*/

static void test_slots(void) {
  dsh_dictsample_slot slots[64];

  /* ★ 1 本书只有 1 块 9 条，要撒满 6 个点：按块撒的话这里只会得到 1 个候选 */
  {
    const int64_t counts[1] = {9};
    int64_t n = dsh_dictsample_slots(counts, 1, 6, slots);
    ok_eq_i64(n, 6, "①★ 1 块 9 条 → **6 个**候选（单位是词条，不是词块）");
    ok_eq_i64(slots[0].block_index, 0, "① 全落在第 0 块");
    ok_eq_i64(slots[0].entry_index, 0, "① 第一个点在块首");
    /* 9 条分 6 段：0,1,3,4,6,7 */
    ok_eq_i64(slots[1].entry_index, 1, "① 第二个点");
    ok_eq_i64(slots[2].entry_index, 3, "① 第三个点");
    ok_eq_i64(slots[5].entry_index, 7, "① 最后一个点在**尾部那一带**（不是挤在开头）");
  }

  /* 两块 3 + 7 条 = 全书 10 条 → 段首序号 0,1,3,5,6,8 → (块,条) 如下
   * ⚠️ 序号 3 正好是第二块第一条（offsets[1] == 3），二分落在第 1 块：写成 (0,2) 是断言错 */
  {
    const int64_t counts[2] = {3, 7};
    int64_t n = dsh_dictsample_slots(counts, 2, 6, slots);
    ok_eq_i64(n, 6, "① 两块共 10 条 → 6 个候选");
    ok_eq_i64(slots[0].block_index, 0, "① 第 1 个在第 0 块");
    ok_eq_i64(slots[0].entry_index, 0, "① 第 1 个是 (0,0)");
    ok_eq_i64(slots[1].entry_index, 1, "① 第 2 个是 (0,1)");
    ok_eq_i64(slots[2].block_index, 1, "①★ 第 3 个跨到第 1 块（序号 3 = 第二块的首条）");
    ok_eq_i64(slots[2].entry_index, 0, "①★ 第 3 个是 (1,0)");
    ok_eq_i64(slots[3].entry_index, 2, "① 第 4 个是 (1,2)");
    ok_eq_i64(slots[5].entry_index, 5, "① 最后一个在块内偏后");
  }

  /* 全书词条比 max_scan 少 → 一条一个，别空转 */
  {
    const int64_t counts[1] = {4};
    int64_t n = dsh_dictsample_slots(counts, 1, 10, slots);
    ok_eq_i64(n, 4, "① 只有 4 条词条时给 4 个点（不重复、不空转）");
  }

  /* 退化输入：不许崩、不许给出点了却没词条 */
  {
    const int64_t empty[1] = {0};
    const int64_t counts[1] = {5};
    ok_eq_i64(dsh_dictsample_slots(NULL, 1, 6, slots), 0, "① 块计数为空 → 0 个点");
    ok_eq_i64(dsh_dictsample_slots(counts, 0, 6, slots), 0, "① 没有块 → 0 个点");
    ok_eq_i64(dsh_dictsample_slots(counts, 1, 0, slots), 0, "① max_scan=0 → 0 个点");
    ok_eq_i64(dsh_dictsample_slots(empty, 1, 6, slots), 0, "① 全书 0 条 → 0 个点");
    ok_eq_i64(dsh_dictsample_slots(counts, 1, 6, NULL), 0, "① 出参为空 → 0 个点");
  }
}

/* ② 粗筛：像词条的才拿去解正文 */

static void test_looks_like(void) {
  ok(dsh_dictsample_looks_like_headword("apple") == 1, "② 普通英文词 → 收");
  ok(dsh_dictsample_looks_like_headword("测试") == 1, "② 中文词 → 收");
  ok(dsh_dictsample_looks_like_headword("りんご") == 1, "② 假名 → 收");
  ok(dsh_dictsample_looks_like_headword("café") == 1, "② 带音标的拉丁词 → 收");
  ok(dsh_dictsample_looks_like_headword("  apple  ") == 1, "②★ 首尾空白先去掉（词典里真有这种键）");
  ok(dsh_dictsample_looks_like_headword("a") == 0, "② 单个字符 → 不收（太短）");
  ok(dsh_dictsample_looks_like_headword("") == 0, "② 空串 → 不收");
  ok(dsh_dictsample_looks_like_headword(NULL) == 0, "② NULL → 不收");
  ok(dsh_dictsample_looks_like_headword("apple pie") == 0, "②★ 带空格（词组）→ 不收");
  ok(dsh_dictsample_looks_like_headword("123") == 0, "②★ 不是字母开头（数字）→ 不收");
  ok(dsh_dictsample_looks_like_headword("-abc") == 0, "②★ 不是字母开头（符号）→ 不收");
  ok(dsh_dictsample_looks_like_headword("@@@LINK=apple") == 0, "②★ @@@LINK 重定向不是词条名 → 不收");
  ok(dsh_dictsample_looks_like_headword("a\\b") == 0, "② 含反斜杠（资源路径）→ 不收");
  ok(dsh_dictsample_looks_like_headword("a:b") == 0, "② 含冒号（命名空间）→ 不收");
  ok(dsh_dictsample_looks_like_headword("abcdefghijklmnopqrstuvwxyz") == 0,
     "②★ 超过 24 个字符 → 不收（检查标准刻意便宜且宽，宁可漏）");
  ok(dsh_dictsample_looks_like_headword("abcdefghijklmnopqrstuvwx") == 1, "② 24 个字符 → 收（边界）");
}

/* ③ 键名清洗 */

static void test_clean(void) {
  char buf[32];
  ok(dsh_dictsample_clean_key("  apple  ", buf, sizeof(buf)) == 0 && strcmp(buf, "apple") == 0,
     "③ 首尾空白去掉");
  ok(dsh_dictsample_clean_key("apple", buf, sizeof(buf)) == 0 && strcmp(buf, "apple") == 0,
     "③ 干净的键原样");
  ok(dsh_dictsample_clean_key("", buf, sizeof(buf)) == 0 && strcmp(buf, "") == 0,
     "③ 空串 → 空串（不是失败）");
  ok(dsh_dictsample_clean_key(NULL, buf, sizeof(buf)) != 0, "③ NULL → 拒");
  {
    /* 缓冲小到装不下：**如实拒**，不许悄悄截断成一个别的词（那会让取样拿去解一条不相干的词条）*/
    char small[8];
    ok(dsh_dictsample_clean_key("abcdefghijklmnopqrstuvwxyz", small, sizeof(small)) != 0,
       "③★ 装不下 → **如实拒**（不许悄悄截断成一个别的词）");
    ok(dsh_dictsample_clean_key("apple", small, sizeof(small)) == 0 && strcmp(small, "apple") == 0,
       "③ 小缓冲够用时照样成功");
  }
}

/* ④ 走引擎：真词典里挑出来的每一条都必须是词目发音 */

/** 建一个引擎、加一本词典、问出它的 id（每档各用一个干净目录：检查标准互不牵连）*/
static char *one_dict_engine(const char *dir, const char *fixture, dsh_engine **out_engine,
                             char *id_buf, size_t id_cap) {
  char path[1024];
  char paths[1200];
  char mk[1024];
  char *added = NULL;
  char *list = NULL;
  dsh_engine *engine = NULL;

  id_buf[0] = '\0';
  snprintf(mk, sizeof(mk), "rm -rf '%s' && mkdir -p '%s'", dir, dir);
  if (system(mk) != 0) { /* 清不掉也继续：建引擎会自己判断 */
  }
  if (dsh_engine_create(dir, &engine) != DSH_OK || engine == NULL) {
    *out_engine = NULL;
    return NULL;
  }
  snprintf(path, sizeof(path), "%s/%s", DSH_TESTDATA_DIR, fixture);
  snprintf(paths, sizeof(paths), "[\"%s\"]", path);
  if (dsh_engine_dict_add(engine, paths, &added) != DSH_OK) {
    if (added != NULL) dsh_release(added);
    *out_engine = engine;
    return NULL;
  }
  if (added != NULL) dsh_release(added);
  if (dsh_engine_dict_list(engine, &list) == DSH_OK && list != NULL) {
    const char *at = strstr(list, "\"id\":\"");
    if (at != NULL) {
      const char *p = at + strlen("\"id\":\"");
      size_t k = 0;
      while (*p != '\0' && *p != '"' && k + 1 < id_cap) id_buf[k++] = *p++;
      id_buf[k] = '\0';
    }
    dsh_release(list);
  }
  *out_engine = engine;
  return (id_buf[0] != '\0') ? id_buf : NULL;
}

static void test_engine(void) {
  const char *fixtures = DSH_TESTDATA_DIR;

  /* 甲：一本词典都没有 → 那一档说「还没有添加词典」*/
  {
    const size_t need = strlen(fixtures) + 40;
    char *dir = (char *)malloc(need);
    dsh_engine *engine = NULL;
    char *json = NULL;
    if (dir == NULL) {
      ok(0, "④ 分配不出临时目录名");
      return;
    }
    snprintf(dir, need, "%s/tmp-dictsample/cfg-none", fixtures);
    {
      char mk[1024];
      snprintf(mk, sizeof(mk), "rm -rf '%s' && mkdir -p '%s'", dir, dir);
      if (system(mk) != 0) { /* 同上 */
      }
    }
    ok(dsh_engine_create(dir, &engine) == DSH_OK && engine != NULL, "④ 建引擎（空词库）");
    if (engine != NULL) {
      ok(dsh_speech_dict_samples(engine, NULL, &json) == DSH_OK && json != NULL,
         "④ 一本词典都没有时也**成功返回**（原因写在 JSON 里）");
      ok_has(json, "\"ok\":false", "④ ok=false");
      ok_has(json, "还没有添加词典", "④★ 那句话说「还没有添加词典」");
      ok_has(json, "\"samples\":[]", "④ 样本是空数组（不是缺键）");
      if (json != NULL) dsh_release(json);
      json = NULL;
      dsh_engine_destroy(engine);
      engine = NULL;
    }
    free(dir);
  }

  /* 乙：一本没有资源卷的词典（kana.mdx，旁边没有 kana.mdd）
   * ⚠️ 别拿 test.mdx 当这种例子：`testdata/` 里躺着 `test.mdd`，它算有资源卷 ——
   *    否则现场走的是「扫不到」那一档，红的是断言的前提，不是内核 */
  {
    const size_t need = strlen(fixtures) + 40;
    char *dir = (char *)malloc(need);
    dsh_engine *engine = NULL;
    char id[256];
    char *json = NULL;
    if (dir == NULL) {
      ok(0, "④ 分配不出临时目录名");
      return;
    }
    snprintf(dir, need, "%s/tmp-dictsample/cfg-plain", fixtures);
    if (one_dict_engine(dir, "kana.mdx", &engine, id, sizeof(id)) == NULL || engine == NULL) {
      ok(0, "④ 建引擎并加 kana.mdx");
      free(dir);
      return;
    }
    ok(dsh_speech_dict_samples(engine, id, &json) == DSH_OK, "④ 问 kana.mdx（没有资源卷那本）");
    ok_has(json, "\"ok\":false", "④ 那本挑不出样本 → ok=false");
    ok_has(json, "没有资源卷", "④★ 那句话说清是「没有资源卷（.mdd）」（与「扫不到」分开说）");
    if (json != NULL) dsh_release(json);
    json = NULL;
    dsh_engine_destroy(engine);
    free(dir);
  }

  /* 丙：一本带资源卷的词典（audio.mdx + audio.mdd，9 条里有词的录音）*/
  {
    const size_t need = strlen(fixtures) + 40;
    char *dir = (char *)malloc(need);
    dsh_engine *engine = NULL;
    char id[256];
    char *json = NULL;
    int i;
    int count = 0;
    if (dir == NULL) {
      ok(0, "④ 分配不出临时目录名");
      return;
    }
    snprintf(dir, need, "%s/tmp-dictsample/cfg-audio", fixtures);
    if (one_dict_engine(dir, "audio.mdx", &engine, id, sizeof(id)) == NULL || engine == NULL) {
      ok(0, "④ 建引擎并加 audio.mdx");
      free(dir);
      return;
    }

    ok(dsh_speech_dict_samples(engine, id, &json) == DSH_OK && json != NULL,
       "④ 问 audio.mdx（带资源卷那本）");
    ok_has(json, "\"ok\":true", "④★ 挑得出样本（这本里有词的录音）");
    count = (int)num_field(json, "count");
    ok(count >= 1 && count <= 6, "④★ 条数在 1..6 之间（上限是 6）");

    /* ★ 每一条都拿内核自己回验一遍：必须是**词目发音**，而且键名一致 */
    for (i = 0; i < count; i++) {
      char item[512];
      char word[128];
      char key[256];
      char language[32];
      char label[64];
      char *verify = NULL;
      char where[320];
      sample_item(json, i, item, sizeof(item));
      field(item, "word", word, sizeof(word));
      field(item, "audioKey", key, sizeof(key));
      field(item, "language", language, sizeof(language));
      field(item, "languageLabel", label, sizeof(label));
      snprintf(where, sizeof(where), "④ ★ 第 %d 条（%s）四个字段都给全了", i + 1, word);
      ok(word[0] != '\0' && key[0] != '\0' && language[0] != '\0' && label[0] != '\0', where);
      if (dsh_speech_dict_audio(engine, id, word, &verify) == DSH_OK && verify != NULL) {
        char vkey[256];
        field(verify, "audioKey", vkey, sizeof(vkey));
        ok(strstr(verify, "\"kind\":\"entry\"") != NULL,
           "④★★ 每一条样本都真的是**词目发音**（kind=entry，不是例句）");
        ok(strcmp(vkey, key) == 0, "④ 回验拿到的键名与样本里那条一致");
        dsh_release(verify);
      } else {
        ok(0, "④ 回验那一条时 dsh_speech_dict_audio 失败了");
      }
    }

    /* 名字不许重复（均匀撒点 + 兜底补扫两趟走同一个去重表）*/
    {
      int dup = 0;
      int a;
      int b;
      for (a = 0; a < count; a++) {
        char ia[512];
        char wa[128];
        sample_item(json, a, ia, sizeof(ia));
        field(ia, "word", wa, sizeof(wa));
        for (b = a + 1; b < count; b++) {
          char ib[512];
          char wb[128];
          sample_item(json, b, ib, sizeof(ib));
          field(ib, "word", wb, sizeof(wb));
          if (wa[0] != '\0' && strcmp(wa, wb) == 0) dup++;
        }
      }
      ok(dup == 0, "④★ 样本里没有重复的词（两趟扫描共用一个去重表）");
    }

    /* 6 条时那句人话是空的（扫不满才说话）*/
    if (count == 6) {
      char message[256];
      field(json, "message", message, sizeof(message));
      ok(message[0] == '\0', "④ 挑满 6 条时 message 是空串");
    }
    /* 扫描条数与耗时都报出来了（调用方要拿它说清为什么只有 N 条）*/
    ok_has(json, "\"scanned\":", "④ 报了扫过几个候选");
    ok_has(json, "\"elapsedMs\":", "④ 报了耗时");
    if (json != NULL) dsh_release(json);
    json = NULL;

    /* 指定一本不存在的词典：如实拒，不许悄悄换成当前词典 */
    ok(dsh_speech_dict_samples(engine, "d-不存在", &json) != DSH_OK,
       "④★ 指定一本不存在的词典 → 拒（不许悄悄换成当前词典）");
    if (json != NULL) dsh_release(json);
    json = NULL;

    dsh_engine_destroy(engine);
    free(dir);
  }
}

int main(void) {
  const size_t base = dsh_mem_live_count();

  test_slots();
  test_looks_like();
  test_clean();
  test_engine();

  /* ⚠️ 汇总必须打在最后一条断言之后：`g_failed` 是在断言里累加的，
   *    先 printf 再断言会出现那行写着失败 0 而进程退出码是 1 */
  ok((size_t)dsh_mem_live_count() == base, "⑤ 这一组没漏内存（内核记账数回到起点）");
  printf("词典取样：%d 项，失败 %d\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
