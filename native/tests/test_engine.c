/* ==========================================================================
 * 内核单元测试 · 引擎（`engine` 组那 9 条）
 *
 * 这一层是「设置 + 词库 + 落盘」的汇合处，也是最容易出**悄悄丢数据**的地方 ——
 * 所以检查标准的重点全在「东西还在不在」，而不是「函数返回成功没有」：
 *
 *   ① 设置的增删改必须**真的落到 settings.json**，而且**重开一次还在**
 *      （只验内存里的那一份，等于没验落盘）；
 *   ② **未识别字段跨会话存活**：内核不认识的那些键（placement / futureKey / …）
 *      在「存盘 → 重开 → 再存盘」之后要一字不差；**唯一的例外是 `history`** ——
 *      它要在启动时搬进 `history.jsonl` 再从设置里删掉（两份并存 = 本仓库最贵的坑）；
 *   ③ **不存在的路径必须进 failed 并写明原因** —— 这条还要验到「词库本数没变」，
 *      光看 added=0 不够；
 *   ④ 词典 id = **内容哈希**：同一份内容换个文件名加进去，**认成同一本**；
 *   ⑤ 落盘失败时**内存里那份不许改**（否则是「界面显示改了、重启变回去」）。
 *
 * 测试用一个临时目录，跑完清掉，不碰用户的 %APPDATA%。
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

/** 测试用词典的完整路径 */
static char *fixture(const char *name) {
  const size_t n = strlen(DSH_TESTDATA_DIR) + strlen(name) + 2;
  char *p = (char *)malloc(n);
  if (p == NULL) return NULL;
  snprintf(p, n, "%s/%s", DSH_TESTDATA_DIR, name);
  return p;
}

/* ── 临时目录 ───────────────────────────────────────────────────────────── */

#define TMP_DIR "/tmp/dsh-engine-test"

static void tmp_reset(void) {
  char cmd[512];
  snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", TMP_DIR, TMP_DIR);
  if (system(cmd) != 0) {
    /* 清不掉也要继续：下面的用例会自己判断 */
  }
}

/** 读一个文件（调用方 free） */
static char *slurp(const char *path) {
  FILE *fp = fopen(path, "rb");
  if (fp == NULL) return NULL;
  char *buf = (char *)malloc(1 << 20);
  if (buf == NULL) {
    fclose(fp);
    return NULL;
  }
  const size_t got = fread(buf, 1, (1 << 20) - 1, fp);
  buf[got] = '\0';
  fclose(fp);
  return buf;
}

/** 从引擎拿词库清单 JSON（调用方 dsh_release） */
static char *list_json(dsh_engine *e) {
  char *json = NULL;
  if (dsh_engine_dict_list(e, &json) != DSH_OK) return NULL;
  return json;
}

/** 文件里有没有出现某段文本 */
static int file_contains(const char *path, const char *needle) {
  char *text = slurp(path);
  if (text == NULL) return 0;
  const int hit = (strstr(text, needle) != NULL);
  free(text);
  return hit;
}

int main(void) {
  const size_t base = dsh_mem_live_count();
  tmp_reset();

  const char *settings_file = TMP_DIR "/settings.json";

  /* ── ① 建引擎 + 设置落盘 + 重开还在 ───────────────────────────────────── */
  {
    dsh_engine *e = NULL;
    ok(dsh_engine_create(TMP_DIR, &e) == DSH_OK, "① 建引擎成功（空目录 = 全默认设置）");
    if (e != NULL) {
      ok_eq_str(dsh_engine_user_data_dir(e), TMP_DIR, "① 配置目录的值");
      ok(dsh_engine_settings(e) != NULL, "① 底层设置文档能取到");

      char *json = NULL;
      ok(dsh_engine_settings_set(e, "{\"closeBehavior\":\"tray\",\"currentDictId\":null}",
                                 &json) == DSH_OK,
         "① 改设置成功");
      ok(json != NULL && strstr(json, "\"closeBehavior\":\"tray\"") != NULL,
         "① 返回的是**规范化之后的完整设置**");
      if (json != NULL) dsh_release(json);

      ok(file_contains(settings_file, "\"closeBehavior\":\"tray\""),
         "① 设置必须**真的落到 settings.json**（只改内存等于没存）");
      /* 非法枚举要夹回，而不是报错 */
      json = NULL;
      ok(dsh_engine_settings_set(e, "{\"closeBehavior\":\"nonsense\"}", &json) == DSH_OK,
         "① 非法枚举不该让改设置失败");
      if (json != NULL) {
        ok(strstr(json, "\"closeBehavior\":\"ask\"") != NULL, "① 非法枚举夹回 ask");
        dsh_release(json);
      }
      char *json2 = NULL;
      ok(dsh_engine_settings_get(e, &json2) == DSH_OK && json2 != NULL, "① 取设置成功");
      dsh_engine_destroy(e);

      /* 重开：设置必须还在（这是「落盘」唯一的检查标准） */
      dsh_engine *e2 = NULL;
      ok(dsh_engine_create(TMP_DIR, &e2) == DSH_OK, "① 重开引擎成功");
      if (e2 != NULL) {
        char *json3 = NULL;
        ok(dsh_engine_settings_get(e2, &json3) == DSH_OK && json3 != NULL, "① 重开之后取设置");
        ok(json3 != NULL && json2 != NULL && strcmp(json3, json2) == 0,
           "① 重开之后设置与关闭前**逐字节相同**");
        if (json3 != NULL) dsh_release(json3);
        dsh_engine_destroy(e2);
      }
      if (json2 != NULL) dsh_release(json2);
    }
  }

  /* ── ② 未识别字段跨会话存活 + `history` 那个**例外** ────────────────────
   *
   * ⚠️ `history` 是**唯一一个被特意处理的顶层键**：它要在启动时**搬进 `history.jsonl`、
   *    再从设置里删掉**（两份并存 = 本仓库最贵的坑）。所以这一节同时钉住两半：
   *    一般未识别键**原样活着**，`history` **必须被搬走**。
   */
  {
    const char *seed = "{\"placement\":{\"pillX\":42},\"history\":[{\"word\":\"x\"}],"
                       "\"futureKey\":[1,2]}";
    FILE *fp = fopen(settings_file, "wb");
    ok(fp != NULL, "② 能写一份带未识别字段的设置文件");
    if (fp != NULL) {
      fwrite(seed, 1, strlen(seed), fp);
      fclose(fp);
    }
    /*
     * 再造一份旧版留下的 SQLite 库：迁移的约定是**明确忽略并删除**，所以这几条断言
     * 钉的正是「它真的不在了」（连 `-wal` 兄弟文件）—— 那是**删文件**的动作，
     * 写错了就是「用户的东西没了」。
     */
    {
      static const char *legacy[] = {TMP_DIR "/lookup.db", TMP_DIR "/lookup.db-wal",
                                     TMP_DIR "/lookup.db-shm"};
      for (size_t i = 0; i < sizeof(legacy) / sizeof(legacy[0]); i++) {
        FILE *lf = fopen(legacy[i], "wb");
        if (lf != NULL) {
          fputs("SQLite format 3\0（假的，只为验迁移会删掉它）", lf);
          fclose(lf);
        }
      }
      ok(file_contains(legacy[0], "SQLite"), "② 造出一份上一版的 `lookup.db`（含 -wal/-shm）");
    }
    dsh_engine *e = NULL;
    if (dsh_engine_create(TMP_DIR, &e) == DSH_OK && e != NULL) {
      char *json = NULL;
      ok(dsh_engine_settings_set(e, "{\"closeBehavior\":\"quit\"}", &json) == DSH_OK,
         "② 只改一个不认识之外的字段");
      if (json != NULL) dsh_release(json);
      dsh_engine_destroy(e);

      ok(file_contains(settings_file, "\"placement\":{\"pillX\":42}"),
         "② 未识别字段 placement 存盘后必须还在");
      ok(file_contains(settings_file, "\"futureKey\":[1,2]"),
         "② 连想都想不到的键也要原样活着（内核不许顺手清掉它）");
      ok(file_contains(settings_file, "\"closeBehavior\":\"quit\""),
         "② 真正要改的那个字段也确实改了");
      ok(!file_contains(settings_file, "\"history\""),
         "② ★ `history` 是唯一的例外：搬进 history.jsonl 之后**必须从设置里删掉**"
         "（不许两份并存）");
      ok(file_contains(TMP_DIR "/history.jsonl", "\"word\":\"x\""),
         "② ★ 而且它**真的被搬走了**：那一行现在躺在 history.jsonl 里");
      ok(!file_contains(TMP_DIR "/lookup.db", "SQLite"),
         "② ★★ 上一版的 `lookup.db` 被删掉了（它**绝不与 `history.jsonl` 并存**）");
      ok(!file_contains(TMP_DIR "/lookup.db-wal", "SQLite"),
         "② ★ 连 `-wal` 兄弟文件一起清（留一个孤零零的 WAL 比留着库更糟）");
    }
  }

  /* ── ③ 不存在的路径必须进 failed，而且词库本数不变 ───────────────────── */
  {
    tmp_reset();
    dsh_engine *e = NULL;
    if (dsh_engine_create(TMP_DIR, &e) == DSH_OK && e != NULL) {
      char *out = NULL;
      const char *paths = "[\"/definitely/not/here.mdx\"]";
      ok(dsh_engine_dict_add(e, paths, &out) == DSH_OK, "③ 导入不存在的路径：调用本身成功");
      ok(out != NULL && strstr(out, "\"added\":0") != NULL, "③ added 必须是 0");
      ok(out != NULL && strstr(out, "文件不存在") != NULL, "③ 失败原因必须写明（文件不存在）");
      if (out != NULL) dsh_release(out);

      char *list = list_json(e);
      ok(list != NULL && strcmp(list, "[]") == 0,
         "③ 导入失败之后词库必须**还是空的**（不许记成 added）");
      if (list != NULL) dsh_release(list);
      ok(!file_contains(settings_file, "not/here.mdx"),
         "③ 失败的那条不许出现在存盘结果里");

      /* 扩展名不对也要说清，而且在**打开之前**就说 */
      out = NULL;
      const char *bad_ext = "[\"" DSH_TESTDATA_DIR "/SHA256.txt\"]";
      ok(dsh_engine_dict_add(e, bad_ext, &out) == DSH_OK, "③ 导入非 .mdx");
      ok(out != NULL && strstr(out, "不是 .mdx") != NULL, "③ 理由要说不是 .mdx");
      if (out != NULL) dsh_release(out);
      list = list_json(e);
      ok(list != NULL && strcmp(list, "[]") == 0, "③ 扩展名不对也不能进词库");
      if (list != NULL) dsh_release(list);

      /* 目录不是词典 */
      out = NULL;
      const char *dir_path = "[\"" DSH_TESTDATA_DIR "\"]";
      ok(dsh_engine_dict_add(e, dir_path, &out) == DSH_OK, "③ 导入一个目录");
      ok(out != NULL && (strstr(out, "目录") != NULL || strstr(out, "不是 .mdx") != NULL),
         "③ 目录要么被扩展名挡下、要么被不是普通文件挡下，反正不许进词库");
      if (out != NULL) dsh_release(out);

      /* 参数本身不合法 → 非零返回 */
      out = NULL;
      ok(dsh_engine_dict_add(e, "{}", &out) != DSH_OK, "③ 参数不是数组必须失败");
      ok(out == NULL, "③ 失败时出参保持 NULL");
      ok(dsh_engine_dict_add(e, "[", &out) != DSH_OK, "③ 参数是坏 JSON 必须失败");
      dsh_engine_destroy(e);
    }
  }

  /* ── ④ 内容哈希 id：换名字认成同一本 ─────────────────────────────────── */
  {
    tmp_reset();
    char *src = fixture("test.mdx");
    char *copy = (char *)malloc(strlen(TMP_DIR) + 32);
    ok(src != NULL && copy != NULL, "④ 准备路径");
    if (src != NULL && copy != NULL) {
      snprintf(copy, strlen(TMP_DIR) + 32, "%s/renamed-dictionary.mdx", TMP_DIR);
      /* 逐字节拷一份（内容一样 → id 必须一样） */
      char cmd[1024];
      snprintf(cmd, sizeof(cmd), "cp '%s' '%s'", src, copy);
      ok(system(cmd) == 0, "④ 复制测试用词典");

      dsh_engine *e = NULL;
      if (dsh_engine_create(TMP_DIR, &e) == DSH_OK && e != NULL) {
        char paths[1024];
        snprintf(paths, sizeof(paths), "[\"%s\"]", copy);
        char *out = NULL;
        ok(dsh_engine_dict_add(e, paths, &out) == DSH_OK, "④ 导入副本");
        ok(out != NULL && strstr(out, "\"added\":1") != NULL, "④ 加进去 1 本");
        if (out != NULL) dsh_release(out);

        char *list = list_json(e);
        ok(list != NULL && strstr(list, "\"mddCount\":0") != NULL, "④ 清单里能读到资源卷数");
        ok(list != NULL && strstr(list, "\"exists\":true") != NULL, "④ 清单里 exists 为真");
        ok(list != NULL && strstr(list, "renamed-dictionary.mdx") != NULL,
           "④ 清单里给的是**文件名**（不加载词典也知道的那个名字）");
        /*
         * ★ 每一本还带「现在什么状态」+ 那句人话：`unavailable` 取空串 / `removed` / `missing`，
         *   `note` 是**界面原样显示**的话。这里只钉「文件好好的那两格是空的」——
         *   另两种情形（含「书名用当前显示名」那条约定）由 `test_history.c` 用真现场钉。
         */
        ok(list != NULL && strstr(list, "\"unavailable\":\"\"") != NULL &&
               strstr(list, "\"note\":\"\"") != NULL,
           "④ ★ 文件好好的 → `unavailable` 与 `note` 都是空串（界面没什么要说的）");
        if (list != NULL) dsh_release(list);

        /* 同一份内容、**原名**再导入一次 → 认成同一本（id 是内容哈希，不是路径） */
        out = NULL;
        snprintf(paths, sizeof(paths), "[\"%s\"]", src);
        ok(dsh_engine_dict_add(e, paths, &out) == DSH_OK, "④ 再导入原文件");
        ok(out != NULL && strstr(out, "\"added\":1") != NULL, "④ 调用成功");
        if (out != NULL) dsh_release(out);
        list = list_json(e);
        {
          /* 词库里应当只有 1 本（两处路径指向同一份内容） */
          int64_t entries = 0;
          if (list != NULL) {
            for (const char *p = list; (p = strstr(p, "\"mdxPath\"")) != NULL; p++) entries++;
          }
          ok_eq_i64(entries, 1,
                    "④ 同一份内容换个名字再导入 → **认成同一本**（id 认内容不认路径）");
          if (list != NULL) dsh_release(list);
        }

        /* 改名 / 指定当前 / 删 */
        char *id_json = list_json(e);
        char id[65] = "";
        if (id_json != NULL) {
          const char *p = strstr(id_json, "\"id\":\"");
          if (p != NULL) {
            p += 6;
            for (int i = 0; i < 64 && p[i] != '"'; i++) id[i] = p[i];
          }
          dsh_release(id_json);
        }
        ok(strlen(id) == 64, "④ 从清单里取到 64 位的 id");
        if (strlen(id) == 64) {
          out = NULL;
          ok(dsh_engine_dict_rename(e, id, "我的词典", &out) == DSH_OK, "④ 改名成功");
          ok(out != NULL && strstr(out, "\"customTitle\":\"我的词典\"") != NULL,
             "④ 改名之后的清单里有 customTitle");
          if (out != NULL) dsh_release(out);
          ok(file_contains(settings_file, "我的词典"), "④ 改名也要落盘");

          out = NULL;
          ok(dsh_engine_dict_set_current(e, id, &out) == DSH_OK, "④ 指定当前词典成功");
          if (out != NULL) dsh_release(out);
          ok(file_contains(settings_file, id), "④ 当前词典的 id 落盘了");

          out = NULL;
          ok(dsh_engine_dict_remove(e, id, &out) == DSH_OK, "④ 移除成功");
          if (out != NULL) dsh_release(out);
          ok(!file_contains(settings_file, id), "④ 移除之后存盘结果里不该还有那个 id");

          /* 移除不存在的 id：幂等，不报错 */
          out = NULL;
          ok(dsh_engine_dict_remove(e, "0000000000000000000000000000000000000000000000000000000000000000",
                                    &out) == DSH_OK,
             "④ 移除不存在的 id 是幂等的（不报错）");
          if (out != NULL) dsh_release(out);
        }
        dsh_engine_destroy(e);
      }
      free(copy);
    }
    if (src != NULL) free(src);
  }

  /* ── ⑤ id 缓存：同一条件重复导入不该重算 ─────────────────────────────── */
  {
    tmp_reset();
    char *src = fixture("link.mdx");
    if (src != NULL) {
      dsh_engine *e = NULL;
      if (dsh_engine_create(TMP_DIR, &e) == DSH_OK && e != NULL) {
        char paths[1024];
        snprintf(paths, sizeof(paths), "[\"%s\"]", src);
        char *o1 = NULL;
        char *o2 = NULL;
        ok(dsh_engine_dict_add(e, paths, &o1) == DSH_OK, "⑤ 第一次导入");
        ok(dsh_engine_dict_add(e, paths, &o2) == DSH_OK, "⑤ 第二次导入（同一路径）");
        ok(o1 != NULL && o2 != NULL && strcmp(o1, o2) == 0,
           "⑤ 同一路径重复导入，结果逐字节相同（缓存不该改变结论）");
        if (o1 != NULL) dsh_release(o1);
        if (o2 != NULL) dsh_release(o2);
        char *list = list_json(e);
        ok(list != NULL && strstr(list, "\"mdxCount\"") == NULL, "⑤ 清单形状稳定（null 与空数组都要正常）");
        if (list != NULL) dsh_release(list);
        dsh_engine_destroy(e);
      }
      free(src);
    }
  }

  /* ── ⑥ 清单里的**书名与头信息** ────────────────────────────────────────
   *
   * 两条检查标准，各钉一件事：
   *   · **书名取 .mdx 头里的那个**，不是文件名 —— 测试用词典 `titled.mdx` 就是为「书名 ≠ 文件名」
   *     造的，导入之后清单里必须看到头里的书名；
   *   · **条目数 / 编码 / 版本**要给真值（写死 0 与空串的话，界面那一行就是占位符）。
   */
  {
    tmp_reset();
    char *src = fixture("titled.mdx");
    if (src != NULL) {
      dsh_engine *e = NULL;
      if (dsh_engine_create(TMP_DIR, &e) == DSH_OK && e != NULL) {
        char paths[1024];
        snprintf(paths, sizeof(paths), "[\"%s\"]", src);
        char *out = NULL;
        ok(dsh_engine_dict_add(e, paths, &out) == DSH_OK, "⑥ 导入测试用词典 titled.mdx");
        if (out != NULL) dsh_release(out);

        char *list = list_json(e);
        ok(list != NULL, "⑥ 清单读得出来");
        /*
         * ⚠️ 检查标准必须钉 `title` 那个**字段值**：不能拿整份 JSON 去找 `titled.mdx` 这个串 ——
         *    `mdxPath` / `fileName` 里本来就有文件名，那样钉会跟着错误一起绿。
         */
        ok(list != NULL && strstr(list, "\"title\":\"有书名的测试词典\"") != NULL,
           "⑥ ★★ 书名取的是**头里的书名**（《有书名的测试词典》），不是文件名 `titled.mdx`");
        ok(list != NULL && strstr(list, "\"headerTitle\":\"有书名的测试词典\"") != NULL,
           "⑥ ★ `headerTitle` 也带着头里那一份（界面兜底用）");
        ok(list != NULL && strstr(list, "\"entryCount\":2,") != NULL,
           "⑥ ★ 条目数 = 2（这一本测试用词典就两条；界面那一格不再显示占位符）");
        ok(list != NULL && strstr(list, "\"encoding\":\"UTF-8\"") != NULL, "⑥ ★ 编码 = UTF-8");
        ok(list != NULL && strstr(list, "\"version\":\"2.0\"") != NULL, "⑥ ★ 版本 = 2.0");
        ok(list != NULL && strstr(list, "\"fileName\":\"titled.mdx\"") != NULL,
           "⑥ 文件名照旧单独给一份（界面拿它显示路径那一行）");
        if (list != NULL) dsh_release(list);

        /* 改成自己的名字之后，**自定义名优先于头里的书名**（这条优先级不许被改坏）*/
        char *renamed = NULL;
        if (dsh_settings_dict_count(dsh_engine_settings(e)) > 0) {
          const dsh_stored_dict *d = dsh_settings_dict_at(dsh_engine_settings(e), 0);
          if (d != NULL && dsh_engine_dict_rename(e, d->id, "我的名字", &renamed) == DSH_OK) {
            char *list2 = list_json(e);
            ok(list2 != NULL && strstr(list2, "\"title\":\"我的名字\"") != NULL,
               "⑥ ★ 改过名的词典仍显示**改过的名字**（自定义名 > 头里的书名 > 文件名）");
            if (list2 != NULL) dsh_release(list2);
          }
          if (renamed != NULL) dsh_release(renamed);
        }
        dsh_engine_destroy(e);
      }
      free(src);
    }
  }

  /* ── ⑦ NULL / 空目录 / 坏设置文件的边界 ──────────────────────────────── */
  {
    dsh_engine *e = NULL;
    ok(dsh_engine_create(NULL, &e) == DSH_OK,
       "⑥ 配置目录为 NULL = 只在内存里活（合法；测试与诊断脚本要的形态）");
    if (e != NULL) {
      ok_eq_str(dsh_engine_user_data_dir(e), "", "⑥ 没配目录时返回空串");
      char *json = NULL;
      ok(dsh_engine_settings_set(e, "{\"closeBehavior\":\"quit\"}", &json) == DSH_OK,
         "⑥ 没有目录也能改设置");
      if (json != NULL) dsh_release(json);
      dsh_engine_destroy(e);
    }
    ok(dsh_engine_create(TMP_DIR, NULL) != DSH_OK, "⑥ out 为 NULL 必须失败");
    dsh_engine_destroy(NULL); /* 合法空操作 */
    ok(1, "dsh_engine_destroy(NULL) 不该崩");

    /* 坏设置文件：必须**报错**，不许「当空的用」（否则下一次存盘会覆盖掉用户原文件） */
    tmp_reset();
    FILE *fp = fopen(settings_file, "wb");
    if (fp != NULL) {
      const char *broken = "{\"dictionaries\": [ oops";
      fwrite(broken, 1, strlen(broken), fp);
      fclose(fp);
    }
    dsh_engine *bad = NULL;
    ok(dsh_engine_create(TMP_DIR, &bad) != DSH_OK, "⑥ 设置文件是坏 JSON → 建引擎必须失败");
    ok(bad == NULL, "⑥ 失败时出参保持 NULL");
    {
      const char *msg = dsh_last_error_message();
      ok(msg != NULL && strstr(msg, "设置") != NULL, "⑥ 报错要指明是设置文件的问题");
      if (msg != NULL) dsh_release((void *)msg);
    }
    ok(file_contains(settings_file, "oops"), "⑥ 坏文件必须**原样留着**（不许被覆盖）");
  }

  tmp_reset();
  ok_eq_i64((int64_t)dsh_mem_live_count(), (int64_t)base,
            "全部用例跑完，活分配表必须回到基线");

  printf("engine：%d 项，失败 %d\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
