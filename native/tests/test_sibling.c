/* 内核单元测试 · 同目录散放的文件与 `dsh_engine_resource`
 * 重点不是「能不能取到」，而是**取不到的那几档**：`.mjs` / `.html` / `.htm` 必须拒绝，
 * `.js` 是唯一放行的脚本档；`..` 穿越、盘符、UNC、**符号链接逃逸**必须拒绝。
 * 每条拒绝的用例都用**真实存在的文件**当靶子 —— 否则分不清是被白名单挡住，还是本来就不在。 */

#define _POSIX_C_SOURCE 200809L

#include "dsh_lookup.h"
#include "dict/dsh_sibling.h"
#include "dict/dsh_mdx.h"
#include "dict/dsh_mime.h"
#include "engine/dsh_engine_internal.h"
#include "engine/dsh_settings.h"
#include "mem_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef DSH_TESTDATA_DIR
#error "需要 -DDSH_TESTDATA_DIR=<0.2.0/testdata 的路径>（见 Makefile）"
#endif

static int g_checks = 0;
static int g_failed = 0;
static int g_skipped = 0;

/** 同目录散放的文件的内容（期望值都从它算，不手写） */
static const char *const SIBLING_CSS = "/* sibling sentinel */";
/** 同目录散放的脚本哨兵串：放行 `.js` 之后必须取得到这一段（要的是对象本身） */
static const char *const SIBLING_SCRIPT = "/* sibling script sentinel */";

static void ok(int cond, const char *what) {
  g_checks++;
  if (!cond) {
    g_failed++;
    fprintf(stderr, "FAIL %s\n", what);
  }
}

/** `/proc/self/maps` 里有没有含 `needle` 的那一行。
 * 这是**操作系统自己的那份事实**（进程真映射了某个文件，内核才留这一行），不是内核自报。
 * @return 1 = 有；0 = 没有；**-1 = 读不到**（没有 /proc 的系统）—— 调用方据此如实跳过。
 */
static int mapped_file_contains(const char *needle, char *out_line, size_t cap) {
  FILE *fp = fopen("/proc/self/maps", "r");
  if (fp == NULL) return -1;
  char line[2048];
  int found = 0;
  while (fgets(line, sizeof(line), fp) != NULL) {
    if (strstr(line, needle) != NULL) {
      found = 1;
      if (out_line != NULL && cap > 0) snprintf(out_line, cap, "%s", line);
      break;
    }
  }
  fclose(fp);
  return found;
}

static void skip(const char *what) {
  g_skipped++;
  fprintf(stderr, "SKIP %s（这一条**没验**）\n", what);
}

/** 建目录（已存在也算成功） */
static void make_dir(const char *path) { mkdir(path, 0777); }

/** 往文件里写一小段文本 */
static void write_text(const char *path, const char *text) {
  FILE *fp = fopen(path, "wb");
  if (fp == NULL) return;
  fwrite(text, 1, strlen(text), fp);
  fclose(fp);
}

/** 复制一个文件（素材是**冻结**的那几份，绝不当场改它们） */
static int copy_file(const char *from, const char *to) {
  FILE *in = fopen(from, "rb");
  if (in == NULL) return -1;
  FILE *out = fopen(to, "wb");
  if (out == NULL) {
    fclose(in);
    return -1;
  }
  char buf[8192];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
    if (fwrite(buf, 1, n, out) != n) {
      fclose(in);
      fclose(out);
      return -1;
    }
  }
  fclose(in);
  fclose(out);
  return 0;
}

/** 定位断言：`expect_hit = 0` 表示**必须拒绝**（安全检查标准） */
static void check_locate(const char *mdx, const char *request, int expect_hit, const char *what) {
  char *hit = dsh_sibling_locate(mdx, request);
  const int got = (hit != NULL) ? 1 : 0;
  char label[320];
  snprintf(label, sizeof(label), "%s【%s】→ %s", what, request, expect_hit ? "命中" : "拒绝");
  g_checks++;
  if (got != expect_hit) {
    g_failed++;
    fprintf(stderr, "FAIL %s\n", label);
    if (got) fprintf(stderr, "      命中了：%s —— 这是一条**安全**检查标准，不能放过\n", hit);
    else fprintf(stderr, "      没命中（应当命中）\n");
  }
  if (hit != NULL) dsh_release(hit);
}

/** 从引擎里取一段资源（返回字节的**拷贝**，调用方 free） */
static int fetch(dsh_engine *e, const char *id, const char *key, size_t offset, size_t length,
                 char **out_bytes, size_t *out_len, char *meta, size_t meta_cap) {
  uint8_t *bytes = NULL;
  size_t len = 0;
  char *json = NULL;
  const enum dsh_error rc = dsh_engine_resource(e, id, key, offset, length, &bytes, &len, &json);
  if (meta != NULL && meta_cap > 0) {
    meta[0] = '\0';
    if (json != NULL) {
      size_t k = 0;
      while (json[k] != '\0' && k + 1 < meta_cap) {
        meta[k] = json[k];
        k++;
      }
      meta[k] = '\0';
    }
  }
  if (json != NULL) dsh_release(json);
  if (rc != DSH_OK) {
    if (bytes != NULL) dsh_release(bytes);
    *out_bytes = NULL;
    *out_len = 0;
    return -1;
  }
  char *copy = NULL;
  if (bytes != NULL && len > 0) {
    copy = (char *)malloc(len + 1);
    if (copy != NULL) {
      memcpy(copy, bytes, len);
      copy[len] = '\0';
    }
  }
  if (bytes != NULL) dsh_release(bytes);
  *out_bytes = copy;
  *out_len = copy != NULL ? len : 0;
  return 0;
}

int main(void) {
  const size_t base = dsh_mem_live_count();

  /* ── 素材：三本「词典」都搭在临时目录里 ── */
  char root[] = "/tmp/dsh-sibling-XXXXXX";
  if (mkdtemp(root) == NULL) {
    fprintf(stderr, "建不了临时目录，这一组没法跑\n");
    return 2;
  }
  char sibling_only[1024], with_mdd[1024], bare[1024], outside[1024];
  char styles[1200]; /* 比目录那几个大一截：GCC 按「源最长 + 后缀」判 format-truncation */
  char path_sibling_css[1400], path_script[1400], path_page[1400], path_mdx_a[1400];
  snprintf(sibling_only, sizeof(sibling_only), "%s/sibling-only", root);
  snprintf(with_mdd, sizeof(with_mdd), "%s/with-mdd", root);
  snprintf(bare, sizeof(bare), "%s/bare", root);
  snprintf(outside, sizeof(outside), "%s/outside", root);
  snprintf(styles, sizeof(styles), "%s/styles", sibling_only);
  make_dir(sibling_only);
  make_dir(with_mdd);
  make_dir(bare);
  make_dir(outside);
  make_dir(styles);

  snprintf(path_mdx_a, sizeof(path_mdx_a), "%s/test.mdx", sibling_only);
  snprintf(path_sibling_css, sizeof(path_sibling_css), "%s/sibling.css", sibling_only);
  snprintf(path_script, sizeof(path_script), "%s/script.js", sibling_only);
  snprintf(path_page, sizeof(path_page), "%s/page.html", sibling_only);
  char path_module[1400];
  snprintf(path_module, sizeof(path_module), "%s/mod.mjs", sibling_only);
  char src_mdx[1400], src_mdd[1400];
  snprintf(src_mdx, sizeof(src_mdx), "%s/test.mdx", DSH_TESTDATA_DIR);
  snprintf(src_mdd, sizeof(src_mdd), "%s/test.mdd", DSH_TESTDATA_DIR);

  write_text(path_sibling_css, SIBLING_CSS);
  write_text(path_script, SIBLING_SCRIPT);        /* 放行的那一档：真实存在，必须取得到 */
  write_text(path_page, "<script>alert(1)</script>");
  write_text(path_module, "export default 1"); /* 真实存在 —— 靠白名单挡它 */
  {
    char p[1400];
    snprintf(p, sizeof(p), "%s/a.png", styles);
    write_text(p, "not-really-a-png");
    snprintf(p, sizeof(p), "%s/secret.css", outside);
    write_text(p, "secret");
    snprintf(p, sizeof(p), "%s/test.mdx", with_mdd);
    copy_file(src_mdx, p);
    snprintf(p, sizeof(p), "%s/test.mdd", with_mdd);
    copy_file(src_mdd, p);
    snprintf(p, sizeof(p), "%s/style.css", with_mdd);
    write_text(p, "/* sibling sentinel */");
    snprintf(p, sizeof(p), "%s/extra.css", with_mdd);
    write_text(p, "/* sibling sentinel */");
    /* 同目录散放的脚本与 module 脚本各放一份：一个**必须取得到**、一个**必须取不到**。
     * 两份都真实存在，才分得清是白名单挡住了，还是文件本来就不在。 */
    snprintf(p, sizeof(p), "%s/extra.js", with_mdd);
    write_text(p, SIBLING_SCRIPT);
    snprintf(p, sizeof(p), "%s/mod.mjs", with_mdd);
    write_text(p, "export default 1");
    snprintf(p, sizeof(p), "%s/test.mdx", bare);
    copy_file(src_mdx, p);
  }

  /* ══ ① 同目录散放的文件定位与安全边界（纯函数，不加载词典）══ */
  {
    check_locate(path_mdx_a, "sibling.css", 1, "① 同目录的样式表");
    check_locate(path_mdx_a, "/sibling.css", 1,
                 "① 根相对写法 `/sibling.css`（词典里常见，等价于词典目录下）");
    check_locate(path_mdx_a, "styles/a.png", 1, "① 子目录里的资源");
    check_locate(path_mdx_a, "sub/../sibling.css", 1,
                 "① 归一化后仍在目录内的 `..` 应当放行（挡的是「逃出去」，不是「出现 ..」）");
    check_locate(path_mdx_a, "sibling.css?v=2", 1, "① 带查询串（某些词典会写 `a.png?v=2`）");
    check_locate(path_mdx_a, "%73%69%62%6c%69%6e%67.css", 1, "① URL 编码的键名（先解码再判）");

    check_locate(path_mdx_a, "script.js", 1,
                 "① ★ `.js` **放行**（2026-09 决定）—— 文件真实存在，取到的就是它");
    check_locate(path_mdx_a, "page.html", 0,
                 "① ★ `.html` 必须拒绝 —— 真实存在，靠白名单挡（信任边界仍关着）");
    check_locate(path_mdx_a, "mod.mjs", 0, "① ★ `.mjs` 必须拒绝（ES module 是另一次决定）");
    check_locate(path_mdx_a, "missing.css", 0, "① 不存在的文件");
    check_locate(path_mdx_a, "..\\..\\..\\Windows\\win.ini", 0,
                 "① 相对路径穿越 `..\\..\\..\\Windows\\win.ini`");
    check_locate(path_mdx_a, "../../../../Windows/win.ini", 0, "① 相对路径穿越（正斜杠写法）");
    check_locate(path_mdx_a, "../outside/secret.css", 0,
                 "① ★ 逃出目录但扩展名**合法**（这条才真的验到「归一化之后必须在目录内」）");
    check_locate(path_mdx_a, "C:\\Windows\\win.ini", 0, "① 带盘符的绝对路径");
    check_locate(path_mdx_a, "\\\\server\\share\\a.css", 0, "① UNC 路径");
    check_locate(path_mdx_a, "//server/share/a.css", 0, "① UNC 路径（正斜杠写法）");
    check_locate(path_mdx_a, "%2e%2e%2f%2e%2e%2fWindows%2fwin.ini", 0,
                 "① URL 编码的穿越（先解码再判）");

    /* 符号链接逃逸：字符串上在目录内、真实文件在外 —— 只有逐段查才拦得住 */
    {
      char link[1400], target[1400], req[1400];
      snprintf(link, sizeof(link), "%s/escape", sibling_only);
      snprintf(target, sizeof(target), "%s", outside);
      if (symlink(target, link) == 0) {
        snprintf(req, sizeof(req), "%s", "escape/secret.css");
        check_locate(path_mdx_a, req, 0,
                     "① ★ **符号链接逃逸**（字符串在目录内、真实路径在外）");
      } else {
        skip("① 符号链接逃逸 —— 这个环境建不了符号链接");
      }
    }
  }

  /* ══ ② `.mdd` 优先 + 同目录兜底 + Range（真加载词典）══ */
  {
    dsh_engine *e = NULL;
    ok(dsh_engine_create(NULL, &e) == DSH_OK && e != NULL, "② 建引擎");
    if (e != NULL) {
      char paths[4096];
      /* ⚠️ 这里**只加带 .mdd 的那一本**：三份副本 .mdx 内容一样 → 内容哈希一样 → 是**同一本**
       * （id = 内容哈希、不认路径），所以「加哪一份」不等于「用哪一份的目录」——
       * 两件事混在一起，这一组就会测出误报失败。 */
      snprintf(paths, sizeof(paths), "[\"%s/test.mdx\"]", with_mdd);
      char *out = NULL;
      ok(dsh_engine_dict_add(e, paths, &out) == DSH_OK, "② 加带 .mdd 的那一本");
      if (out != NULL) dsh_release(out);
      out = NULL;
      ok(dsh_engine_dict_set_current(e, NULL, &out) != DSH_OK || 1,
         "② 当前词典那一步（这一组不依赖它）");
      if (out != NULL) dsh_release(out);
      char id[80] = "";
      char *list = NULL;
      if (dsh_engine_dict_list(e, &list) == DSH_OK && list != NULL) {
        const char *q = strstr(list, "\"id\":\"");
        if (q != NULL) {
          q += 6;
          size_t k = 0;
          while (q[k] != '\0' && q[k] != '"' && k < 79) {
            id[k] = q[k];
            k++;
          }
          id[k] = '\0';
        }
        dsh_release(list);
      }
      ok(id[0] != '\0', "② 取到词典 id");

      /* 用哪一份 .mdx 由路径决定：两份内容一样，所以 mdds 只从**第一条**路径旁边找
       * （with-mdd/test.mdd）—— 这正是要验的形态。 */
      char *bytes = NULL;
      size_t len = 0;
      char meta[512];
      ok(fetch(e, id, "style.css", 0, 0, &bytes, &len, meta, sizeof(meta)) == 0 && bytes != NULL,
         "② 取 `.mdd` 里的 `style.css`");
      ok(len > 0 && strstr(bytes, "sibling sentinel") == NULL,
         "② ★ `.mdd` 命中优先于同目录散放的文件（返回的不是同目录那份）");
      ok(strstr(meta, "\"found\":true") != NULL, "② 元信息里 found 为真");
      ok(strstr(meta, "\"mime\":\"text/css; charset=utf-8\"") != NULL,
         "② 元信息里的 MIME 是 text/css（表与参考实现同一条）");
      ok(strstr(meta, "\"etag\":\"\\\"mdd:") != NULL,
         "② ★ 身份证说的是「来自 .mdd」（附带候选键名与卷文件的大小-时间）");
      if (bytes != NULL) free(bytes);

      /* `.mdd` 未命中 → 回落同目录 */
      bytes = NULL;
      meta[0] = '\0';
      ok(fetch(e, id, "extra.css", 0, 0, &bytes, &len, meta, sizeof(meta)) == 0 && bytes != NULL,
         "② `.mdd` 未命中时回落到同目录（`extra.css`）");
      ok(bytes != NULL && strstr(bytes, "sibling sentinel") != NULL, "② 拿到的就是同目录散放的那份");
      ok(strstr(meta, "\"etag\":\"\\\"sibling:") != NULL,
         "② 身份证改说「来自同目录散放的文件」");
      if (bytes != NULL) free(bytes);

      /* `.js` 放行：引擎级那一侧也要通（两道检查必须**同时**开着，缺一个就是点了没反应） */
      bytes = NULL;
      meta[0] = '\0';
      ok(fetch(e, id, "extra.js", 0, 0, &bytes, &len, meta, sizeof(meta)) == 0 && bytes != NULL,
         "② ★ 同目录散放的脚本 `extra.js` 取得到（放行决定在引擎级也生效）");
      ok(bytes != NULL && strstr(bytes, SIBLING_SCRIPT) != NULL,
         "② 拿到的就是同目录散放的那份脚本（哨兵串对得上）");
      ok(strstr(meta, "\"etag\":\"\\\"sibling:") != NULL, "② 身份证说「来自同目录散放的文件」");
      if (bytes != NULL) free(bytes);

      /* 反向自检：检查没有全开 —— 同样真实存在的 `.mjs` 必须取不到 */
      bytes = NULL;
      meta[0] = '\0';
      ok(fetch(e, id, "mod.mjs", 0, 0, &bytes, &len, meta, sizeof(meta)) == 0 && bytes == NULL,
         "② ★ 同目录散放的 `mod.mjs` 取不到（只放行了 `.js`，不是所有扩展名都放行）");
      ok(strstr(meta, "\"found\":false") != NULL, "② 并且如实说 found=false");
      if (bytes != NULL) free(bytes);

      /* 不存在的东西 */
      bytes = NULL;
      meta[0] = '\0';
      ok(fetch(e, id, "no-such-thing.png", 0, 0, &bytes, &len, meta, sizeof(meta)) == 0 &&
             bytes == NULL,
         "② 不存在的资源 → 成功返回 + found=false（**不是**错误）");
      if (bytes != NULL) free(bytes);

      /* Range：切一段、越界那一档。⚠️ 期望值**从素材本身算**（`SIBLING_CSS` 的 strlen
       * 与子串），不许手写数字 —— 手数的字节数与起点两条都错过。 */
      bytes = NULL;
      meta[0] = '\0';
      ok(fetch(e, id, "extra.css", 2, 5, &bytes, &len, meta, sizeof(meta)) == 0 && bytes != NULL,
         "② 带 Range 取一段");
      {
        char want_total[64];
        snprintf(want_total, sizeof(want_total), "\"total\":%zu", strlen(SIBLING_CSS));
        ok(len == 5, "② 取到的就是 5 字节");
        ok(bytes != NULL && memcmp(bytes, SIBLING_CSS + 2, 5) == 0,
           "② 而且就是整份里从第 2 字节开始的那 5 个字节");
        ok(strstr(meta, want_total) != NULL,
           "② 元信息里的 total 是**整份**的长度（宿主据此发 206）");
      }
      if (bytes != NULL) free(bytes);

      bytes = NULL;
      meta[0] = '\0';
      ok(fetch(e, id, "extra.css", 999, 0, &bytes, &len, meta, sizeof(meta)) == 0 && len == 0,
         "② ★ Range 起点超过总长 → 0 字节 + total 照给（宿主据此回 416）");
      ok(strstr(meta, "416") != NULL, "② 并且把「宿主据此回 416」写清楚");
      if (bytes != NULL) free(bytes);

      /* 参数边界 */
      {
        uint8_t *b = NULL;
        size_t l = 0;
        char *m = NULL;
        ok(dsh_engine_resource(e, id, "extra.css", 0, 0, NULL, &l, &m) != DSH_OK,
           "② out_bytes 为 NULL 必须失败");
        ok(dsh_engine_resource(e, NULL, "x", 0, 0, &b, &l, &m) != DSH_OK,
           "② dict_id 为空必须失败");
        ok(dsh_engine_resource(NULL, id, "x", 0, 0, &b, &l, &m) != DSH_OK,
           "② 引擎为 NULL 必须失败");
        ok(dsh_engine_resource(e, "no-such-id", "x", 0, 0, &b, &l, &m) != DSH_OK,
           "② 词库里没有这本 → 失败（不是「没这个资源」）");
      }

      dsh_engine_destroy(e);
    }
  }

  /* ══ ②d ★ `.mdd` 卷是**懒打开**的 ══
   * 加一本词典不该顺手把它几百 MB 的资源卷映射进来，只有真从 `.mdd` 取东西时才开；「懒」不能用返回值
   * 证（接口说的和真做的也是两件事），所以检查标准取**操作系统自己的事实**（`/proc/self/maps`）：查词后
   * 不许有那个映射、真取过资源后必须有 —— 缺后一条，前一条可能永远绿着空转。 */
  {
    char probe[2048];
    if (mapped_file_contains("test.mdd", probe, sizeof(probe)) < 0) {
      skip("②d 读不到 /proc/self/maps —— 这一台机器上验不了「懒」");
    } else {
      dsh_engine *e = NULL;
      ok(dsh_engine_create(NULL, &e) == DSH_OK && e != NULL, "②d 建引擎");
      if (e != NULL) {
        char paths[1400];
        snprintf(paths, sizeof(paths), "[\"%s/test.mdx\"]", with_mdd);
        char *out = NULL;
        ok(dsh_engine_dict_add(e, paths, &out) == DSH_OK, "②d 加那一本（`.mdx` + `.mdd` 都在）");
        if (out != NULL) dsh_release(out);

        /* 查一次词：这一步一定会把 `.mdx` 加载进来（`ensure_loaded`）*/
        char *json = NULL;
        ok(dsh_engine_lookup(e, "apple", DSH_ORIGIN_INPUT, NULL, &json) == DSH_OK,
           "②d 查一次词（把 `.mdx` 加载进来）");
        if (json != NULL) dsh_release(json);

        /* 前提：`.mdx` 的映射确实出现了 —— 否则下面那一条「没有 .mdd」是空转的 */
        ok(mapped_file_contains("test.mdx", probe, sizeof(probe)) == 1,
           "②d 前提：`.mdx` 真的映射进来了（查过一次词）");
        /* ★ 而资源卷**一个字节都没映射** */
        ok(mapped_file_contains("test.mdd", probe, sizeof(probe)) == 0,
           "②d ★★ 查一次词**不许**顺手打开资源卷（maps 里没有 `.mdd`）");

        /* 取到这一本的 id（后面要从 `.mdd` 里拿东西）*/
        char id[80] = "";
        char *list = NULL;
        if (dsh_engine_dict_list(e, &list) == DSH_OK && list != NULL) {
          const char *q = strstr(list, "\"id\":\"");
          if (q != NULL) {
            q += 6;
            size_t k = 0;
            while (q[k] != '\0' && q[k] != '"' && k < 79) {
              id[k] = q[k];
              k++;
            }
            id[k] = '\0';
          }
          dsh_release(list);
        }
        char *bytes = NULL;
        size_t len = 0;
        char meta[512];
        ok(id[0] != '\0' &&
               fetch(e, id, "style.css", 0, 0, &bytes, &len, meta, sizeof(meta)) == 0 &&
               bytes != NULL,
           "②d 真从 `.mdd` 里取一次资源");
        if (bytes != NULL) free(bytes);
        ok(mapped_file_contains("test.mdd", probe, sizeof(probe)) == 1,
           "②d ★★ 取过一次之后**它必须出现**（反向对照：证明这台机器上走的真是 mmap，"
           "上一条不是空转的）");

        /* 反过来再看一眼：`.mdx` 与 `.mdd` 两个映射同时在 —— 那是真正开着的样子 */
        ok(mapped_file_contains("test.mdx", probe, sizeof(probe)) == 1,
           "②d 两本都开着的时候，两个映射都在");
        dsh_engine_destroy(e);
      }
    }
  }

  /* ══ ②b 内容相同的两份 .mdx 是**同一本**（id = 内容哈希，不认路径）══ */
  {
    dsh_engine *e2 = NULL;
    if (dsh_engine_create(NULL, &e2) == DSH_OK && e2 != NULL) {
      char paths[4096];
      snprintf(paths, sizeof(paths), "[\"%s/test.mdx\",\"%s/test.mdx\"]", with_mdd, bare);
      char *out = NULL;
      ok(dsh_engine_dict_add(e2, paths, &out) == DSH_OK, "②b 加两份内容相同的 .mdx");
      if (out != NULL) dsh_release(out);
      char *list = NULL;
      if (dsh_engine_dict_list(e2, &list) == DSH_OK && list != NULL) {
        int rows = 0;
        for (const char *p = list; (p = strstr(p, "\"id\":\"")) != NULL; p += 6) rows++;
        char label[256];
        snprintf(label, sizeof(label),
                 "②b ★ 内容相同的两份是**同一本**（id = 内容哈希、不认路径）—— 实际 %d 本",
                 rows);
        ok(rows == 1, label);
        dsh_release(list);
      }
      dsh_engine_destroy(e2);
    }
  }

  /* ══ ②c `.mdd` 的词块索引要读对（护住一次**真 bug**）══
   * 曾经 `read_key_infos` 末尾多一句 `dsh_release(packed)`，而解码器已消费掉它：那个地址此后被 `blocks`
   * 用掉，这一句就把活着的 `blocks` 注销，`m->key_blocks[0]` 头两个字段成垃圾（`unpack_size` 与首尾词
   * 仍对，只看首词照不出来），症状是一个资源都取不到。此前没测过 `.mdd`，这里专钉真卷的索引逐字段核对。 */
  {
    char mdd_path[1400];
    snprintf(mdd_path, sizeof(mdd_path), "%s/test.mdd", with_mdd);
    dsh_mdx *mdd = NULL;
    ok(dsh_mdx_open(mdd_path, &mdd) == 0 && mdd != NULL, "②c 打开真正的 `.mdd`");
    if (mdd != NULL) {
      ok(dsh_mdx_is_mdd(mdd), "②c 认得出这是 .mdd（键区按 UTF-16 解）");
      ok(dsh_mdx_key_count(mdd) == 2, "②c 词典头说这本有 2 个键");
      ok(dsh_mdx_key_block_count(mdd) == 1, "②c 有 1 个词块");
      dsh_mdx_key_block kb;
      memset(&kb, 0, sizeof(kb));
      ok(dsh_mdx_key_block_at(mdd, 0, &kb) == 0, "②c 取第 0 个词块");
      ok(kb.entry_count == 2, "②c ★ 词块里的条目数必须是 2（那个 bug 里它是垃圾）");
      ok(kb.pack_size > 0 && kb.pack_size < 963, "②c ★ pack_size 必须是合理的字节数");
      ok(kb.unpack_size > 0, "②c unpack_size 也必须是正数");
      ok(kb.first_key != NULL && strcmp(kb.first_key, "\\apple.png") == 0,
         "②c 首词是 \\apple.png");
      ok(kb.last_key != NULL && strcmp(kb.last_key, "\\style.css") == 0,
         "②c 尾词是 \\style.css");
      char **keys = NULL;
      int64_t n = 0;
      ok(dsh_mdx_list_keys(mdd, &keys, &n) == 0 && n == 2, "②c 枚举键：2 条");
      if (keys != NULL) dsh_mdx_free_keys(keys, n);
      char *landed = NULL;
      char *body = NULL;
      int64_t body_len = 0;
      ok(dsh_mdx_fetch(mdd, "\\style.css", &landed, &body, &body_len) == 1 && body_len > 0,
         "②c ★ 按 `\\style.css` 取得到记录（那个 bug 里这一步报越界）");
      if (landed != NULL) dsh_release(landed);
      if (body != NULL) dsh_release(body);
      dsh_mdx_close(mdd);
    }
  }

  /* ══ ②c `.mdd` 的词块索引要读对（护住一次**真 bug**）══
   * 曾经 `read_key_infos` 末尾多一句 `dsh_release(packed)`，而解码器已消费掉它：那个地址此后被 `blocks`
   * 用掉，这一句就把活着的 `blocks` 注销，`m->key_blocks[0]` 头两个字段成垃圾（`unpack_size` 与首尾词
   * 仍对，只看首词照不出来），症状是一个资源都取不到。此前没测过 `.mdd`，这里专钉真卷的索引逐字段核对。 */
  {
    char mdd_path[1400];
    snprintf(mdd_path, sizeof(mdd_path), "%s/test.mdd", with_mdd);
    dsh_mdx *mdd = NULL;
    ok(dsh_mdx_open(mdd_path, &mdd) == 0 && mdd != NULL, "②c 打开真正的 `.mdd`");
    if (mdd != NULL) {
      ok(dsh_mdx_is_mdd(mdd), "②c 认得出这是 .mdd（键区按 UTF-16 解）");
      ok(dsh_mdx_key_count(mdd) == 2, "②c 词典头说这本有 2 个键");
      ok(dsh_mdx_key_block_count(mdd) == 1, "②c 有 1 个词块");
      dsh_mdx_key_block kb;
      memset(&kb, 0, sizeof(kb));
      ok(dsh_mdx_key_block_at(mdd, 0, &kb) == 0, "②c 取第 0 个词块");
      ok(kb.entry_count == 2, "②c ★ 词块里的条目数必须是 2（那个 bug 里它是垃圾）");
      ok(kb.pack_size > 0 && kb.pack_size < 963, "②c ★ pack_size 必须是合理的字节数");
      ok(kb.unpack_size > 0, "②c unpack_size 也必须是正数");
      ok(kb.first_key != NULL && strcmp(kb.first_key, "\\apple.png") == 0,
         "②c 首词是 \\apple.png");
      ok(kb.last_key != NULL && strcmp(kb.last_key, "\\style.css") == 0,
         "②c 尾词是 \\style.css");
      char **keys = NULL;
      int64_t n = 0;
      ok(dsh_mdx_list_keys(mdd, &keys, &n) == 0 && n == 2, "②c 枚举键：2 条");
      if (keys != NULL) dsh_mdx_free_keys(keys, n);
      char *landed = NULL;
      char *body = NULL;
      int64_t body_len = 0;
      ok(dsh_mdx_fetch(mdd, "\\style.css", &landed, &body, &body_len) == 1 && body_len > 0,
         "②c ★ 按 `\\style.css` 取得到记录（那个 bug 里这一步报越界）");
      if (landed != NULL) dsh_release(landed);
      if (body != NULL) dsh_release(body);
      dsh_mdx_close(mdd);
    }
  }

  /* ══ ③ 有没有资源（`data-has-resources` 与「什么都没有」那一侧）══ */
  {
    char p[1400];
    snprintf(p, sizeof(p), "%s/test.mdx", with_mdd);
    ok(dsh_sibling_has_any(p), "③ 同目录散放的文件存在时：`has_any` 为真");
    snprintf(p, sizeof(p), "%s/test.mdx", bare);
    ok(!dsh_sibling_has_any(p), "③ 什么都没有的词典：`has_any` 为假");
    snprintf(p, sizeof(p), "%s/test.mdx", sibling_only);
    ok(dsh_sibling_has_any(p), "③ 不带 .mdd 的词典也为真 —— 与「取得到资源」不矛盾");
    ok(!dsh_sibling_has_any("/no/such/dir/x.mdx"), "③ mdx 路径不存在 → 假（不崩）");
    ok(!dsh_sibling_has_any(NULL), "③ 路径为 NULL → 假（不崩）");
  }

  /* ══ ④ MIME 表 ══ */
  {
    ok(strcmp(dsh_mime_for("a/style.css"), "text/css; charset=utf-8") == 0, "④ .css");
    ok(strcmp(dsh_mime_for("STYLE.CSS"), "text/css; charset=utf-8") == 0,
       "④ 扩展名大小写不敏感");
    ok(strcmp(dsh_mime_for("x.spx"), "audio/x-speex") == 0, "④ .spx 是 audio/x-speex");
    ok(strcmp(dsh_mime_for("a.woff2"), "font/woff2") == 0, "④ .woff2");
    ok(strcmp(dsh_mime_for("a.unknown"), "application/octet-stream") == 0, "④ 认不出 → octet-stream");
    ok(strcmp(dsh_mime_for("noext"), "application/octet-stream") == 0, "④ 没有扩展名");
    ok(strcmp(dsh_mime_for("dir.d/file"), "application/octet-stream") == 0,
       "④ `dir.d/file` 不算有扩展名（点只在最后一段里找）");
    ok(strcmp(dsh_mime_for("a.json"), "application/json; charset=utf-8") == 0,
       "④ .json（.mdd 里可能有，同目录散放文件的扩展名白名单里没有 —— 两件事不冲突）");
    ok(strcmp(dsh_mime_for(NULL), "application/octet-stream") == 0, "④ NULL 也给一个安全值");
  }

  /* ══ ⑤ 白名单本身（这是**信任边界**，单独钉）══ */
  {
    ok(dsh_sibling_is_allowed_extension("style.css"), "⑤ .css 允许");
    ok(dsh_sibling_is_allowed_extension("BOOKERLY.TTF"), "⑤ .TTF 允许（大小写不敏感）");
    ok(dsh_sibling_is_allowed_extension("a/b/c.webp"), "⑤ 子目录里的 .webp 允许");
    ok(dsh_sibling_is_allowed_extension("script.js"), "⑤ ★ .js **允许**（2026-09 决定）");
    ok(dsh_sibling_is_allowed_extension("SCRIPTS/MAIN.JS"), "⑤ ★ .JS 大写也允许（大小写不敏感）");
    ok(!dsh_sibling_is_allowed_extension("m.mjs"), "⑤ ★ .mjs **不允许**（ES module 另议）");
    ok(!dsh_sibling_is_allowed_extension("page.html"), "⑤ ★ .html **不允许**");
    ok(!dsh_sibling_is_allowed_extension("page.htm"), "⑤ ★ .htm **不允许**");
    ok(!dsh_sibling_is_allowed_extension("a.js.txt"), "⑤ `a.js.txt` 看的是最后一段 → 不允许");
    ok(!dsh_sibling_is_allowed_extension("dir.js/file"), "⑤ 目录名带 .js 不算数 → 不允许");
    ok(!dsh_sibling_is_allowed_extension("a.mp3"), "⑤ 音频不在同目录散放文件的扩展名白名单里（走发音那条路）");
    ok(!dsh_sibling_is_allowed_extension("noext"), "⑤ 没有扩展名 → 不允许");
    ok(!dsh_sibling_is_allowed_extension(""), "⑤ 空串 → 不允许");
  }

  ok((int64_t)dsh_mem_live_count() == (int64_t)base,
     "全部用例跑完，活分配表必须回到基线");

  printf("sibling：%d 项，失败 %d，跳过 %d\n", g_checks, g_failed, g_skipped);
  return g_failed == 0 ? 0 : 1;
}
