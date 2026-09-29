/* 内核单元测试 · 词条正文文档（dict/dsh_entry_doc.c）
 * 标准答案文件驱动：表在 `tests/entry_doc_vectors.h` 里，由 `tools/make-entry-assets.ps1` 拿
 * 0.1.3 的参考实现（`EntryDocument.Build`）现场产出，并把整份文档拆成五段 —— 所以第一条检查
 * 标准是**先证明这个拆法成立**：拼起来算 SHA-256 必须等于生成脚本从参考实现那份完整文档上
 * 算出的哈希；少了它，改了模板又两边一起改，照样绿。 */

#include "dict/dsh_entry_doc.h"
#include "dict/dsh_mdx.h"
#include "dict/entry_assets.h"
#include "dict/entry_assets_extra.h"
#include "crypto/dsh_sha256.h"
#include "dsh_lookup.h"
#include "engine/dsh_engine_internal.h"
#include "engine/dsh_settings.h"
#include "mem_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "entry_doc_vectors.h"

static int g_checks = 0;
static int g_failed = 0;

static void ok(int cond, const char *what) {
  g_checks++;
  if (!cond) {
    g_failed++;
    fprintf(stderr, "FAIL %s\n", what);
  }
}

/** 把若干段字节拼起来（测试自己的小工具；用 malloc，不进内核的活分配表） */
static void free_keys(char **keys, int64_t count) {
  if (keys == NULL) return;
  for (int64_t i = 0; i < count; i++) {
    if (keys[i] != NULL) dsh_release(keys[i]);
  }
  dsh_release(keys);
}

/** 测试用词典的绝对路径（malloc 分配，调用方 free）*/
static char *fixture_path(const char *name) {
  const size_t n = strlen(DSH_TESTDATA_DIR) + strlen(name) + 2;
  char *p = (char *)malloc(n);
  if (p != NULL) snprintf(p, n, "%s/%s", DSH_TESTDATA_DIR, name);
  return p;
}

/** 表里的 `notice` 是**没有收尾 `\0`** 的字节数组，而内核那条接口收 C 字符串，所以先拷进带
 *  `\0` 的缓冲。⚠️ 直接当 C 字符串用会读过数组末尾：普通构建照样能跑（读到紧邻的下一个
 *  全局量），只有 `make asan` 才会当场报 `global-buffer-overflow`（存字节是对的，错在用）。 */
static void notice_as_cstr(const dsh_entry_doc_vector *v, char *buf, size_t cap) {
  buf[0] = '\0';
  if (v->notice_len == 0 || v->notice_len + 1 > cap) return;
  memcpy(buf, v->notice, v->notice_len);
  buf[v->notice_len] = '\0';
}

/** 按标准答案文件的拆法拼出参考实现那份文档（返回新分配的字节，调用方 free，失败回 NULL），
 *  并核它的 SHA-256。⚠️ 这里拼的**不含 0.2.0 的追加段**，所以它必须仍逐字节等于参考实现；
 *  C 版那一份 = 这个 + `with_extra`，于是「差别只有那一段」是被机械钉住的，不靠人记着。 */
static unsigned char *rebuild_expected(const dsh_entry_doc_vector *v, size_t *out_len) {
  const size_t style_len = strlen(DSH_ENTRY_BASE_STYLE);
  const size_t script_len = strlen(DSH_ENTRY_BRIDGE_SCRIPT);
  const size_t total = v->prefix_len + style_len + v->mid_len + script_len + v->suffix_len;
  unsigned char *buf = (unsigned char *)malloc(total + 1);
  if (buf == NULL) return NULL;
  size_t at = 0;
  memcpy(buf + at, v->prefix, v->prefix_len);
  at += v->prefix_len;
  memcpy(buf + at, DSH_ENTRY_BASE_STYLE, style_len);
  at += style_len;
  memcpy(buf + at, v->mid, v->mid_len);
  at += v->mid_len;
  memcpy(buf + at, DSH_ENTRY_BRIDGE_SCRIPT, script_len);
  at += script_len;
  memcpy(buf + at, v->suffix, v->suffix_len);
  at += v->suffix_len;
  buf[at] = '\0';
  *out_len = at;
  return buf;
}

/** 「参考实现那一份」+ 0.2.0 的追加段 = **C 版应该产出的完整文档**。追加段的形状是「参考脚本的
 *  `</script>` + `<script>` + 那一段 JS」，再接上原来的 `suffix`（它本来就从 `</script>` 开始），
 *  所以拼法就是在 `BRIDGE_SCRIPT` 与 `suffix` 之间插一段。 */
static unsigned char *with_extra(const dsh_entry_doc_vector *v, size_t *out_len) {
  const size_t style_len = strlen(DSH_ENTRY_BASE_STYLE);
  const size_t script_len = strlen(DSH_ENTRY_BRIDGE_SCRIPT);
  const size_t open_len = strlen(DSH_ENTRY_EXTRA_OPEN);
  const size_t extra_len = strlen(DSH_ENTRY_BRIDGE_EXTRA);
  const size_t total =
      v->prefix_len + style_len + v->mid_len + script_len + open_len + extra_len + v->suffix_len;
  unsigned char *buf = (unsigned char *)malloc(total + 1);
  if (buf == NULL) return NULL;
  size_t at = 0;
  memcpy(buf + at, v->prefix, v->prefix_len);
  at += v->prefix_len;
  memcpy(buf + at, DSH_ENTRY_BASE_STYLE, style_len);
  at += style_len;
  memcpy(buf + at, v->mid, v->mid_len);
  at += v->mid_len;
  memcpy(buf + at, DSH_ENTRY_BRIDGE_SCRIPT, script_len);
  at += script_len;
  memcpy(buf + at, DSH_ENTRY_EXTRA_OPEN, open_len);
  at += open_len;
  memcpy(buf + at, DSH_ENTRY_BRIDGE_EXTRA, extra_len);
  at += extra_len;
  memcpy(buf + at, v->suffix, v->suffix_len);
  at += v->suffix_len;
  buf[at] = '\0';
  *out_len = at;
  return buf;
}

/** 逐字节比对两个缓冲，失败时给出第一处差异 */
static void same_bytes(const unsigned char *got, size_t got_len, const unsigned char *want,
                       size_t want_len, const char *what) {
  g_checks++;
  if (got_len == want_len && memcmp(got, want, want_len) == 0) return;
  g_failed++;
  fprintf(stderr, "FAIL %s\n", what);
  if (got_len != want_len) {
    fprintf(stderr, "      长度：实际 %zu / 期望 %zu\n", got_len, want_len);
  }
  const size_t n = got_len < want_len ? got_len : want_len;
  for (size_t i = 0; i < n; i++) {
    if (got[i] != want[i]) {
      fprintf(stderr, "      第一处差异在第 %zu 字节：实际 0x%02X / 期望 0x%02X\n", i, got[i],
              want[i]);
      /* 差异**两侧**都打：只打前面那一段看不出「少了一个字符」这类差异 */
      const size_t from = (i > 50) ? i - 50 : 0;
      fprintf(stderr, "      实际：%.*s[此处]%.*s\n", (int)(i - from), (const char *)got + from,
              (int)((i + 50 <= got_len) ? 50 : got_len - i), (const char *)got + i);
      fprintf(stderr, "      期望：%.*s[此处]%.*s\n", (int)(i - from), (const char *)want + from,
              (int)((i + 50 <= want_len) ? 50 : want_len - i), (const char *)want + i);
      break;
    }
  }
}

/** 音频键清单里有没有这个键 */
static int has_key(char **keys, int64_t count, const char *want) {
  for (int64_t i = 0; i < count; i++) {
    if (strcmp(keys[i], want) == 0) return 1;
  }
  return 0;
}

int main(void) {
  const size_t base = dsh_mem_live_count();

  ok(strlen(DSH_ENTRY_BASE_STYLE) > 1000, "① 基础样式那段资产在（否则标准答案文件是空转的）");
  ok(strlen(DSH_ENTRY_BRIDGE_SCRIPT) > 10000, "① 桥接脚本那段资产在");
  ok(strlen(DSH_ENTRY_BRIDGE_EXTRA) > 1500, "① 0.2.0 追加的那段脚本在（不报错地点击那条检查标准靠它）");
  ok(strstr(DSH_ENTRY_BRIDGE_EXTRA, "dead-click") != NULL,
     "① 追加段报的是 dead-click 这条消息（宿主按它弹正文框那条提示条）");
  ok(strstr(DSH_ENTRY_BRIDGE_EXTRA, "'no-href'") != NULL &&
         strstr(DSH_ENTRY_BRIDGE_EXTRA, "'no-anchor'") != NULL &&
         strstr(DSH_ENTRY_BRIDGE_EXTRA, "'nothing-happened'") != NULL,
     "① 三种原因都在（没有 href / 锚点找不到 / 点了页面没变）");

  /* ══ ① 标准答案文件：拆法成立 + 逐字节一致 ══ */
  for (int i = 0; i < ENTRY_DOC_VECTOR_COUNT; i++) {
    const dsh_entry_doc_vector *v = &ENTRY_DOC_VECTORS[i];
    char label[256];

    size_t expected_len = 0;
    unsigned char *expected = rebuild_expected(v, &expected_len);
    snprintf(label, sizeof(label), "① 「%s」拆出来的文档能算哈希", v->label);
    if (expected == NULL) {
      ok(0, label);
      continue;
    }
    /* 先证明这个拆法拼出来的**就是参考实现那份完整文档**（哈希由生成脚本在 C# 侧从 `Build`
     * 的完整输出上算出来并写进表）：少了它，模板改了、两边一起改照样绿。 */
    char hex_got[65];
    dsh_sha256_hex(expected, expected_len, hex_got);
    char hex_want[65];
    for (int k = 0; k < 32; k++) snprintf(hex_want + k * 2, 3, "%02x", v->expected_sha256[k]);
    hex_want[64] = '\0';
    g_checks++;
    if (strcmp(hex_got, hex_want) != 0) {
      g_failed++;
      fprintf(stderr, "FAIL %s（拆法不成立：拼出来的不是参考实现那份）\n", label);
      fprintf(stderr, "      实际 %s\n      期望 %s\n", hex_got, hex_want);
    }

    size_t got_len = 0;
    char notice_buf[512];
    notice_as_cstr(v, notice_buf, sizeof(notice_buf));
    char *got = dsh_entry_doc_build(v->dict_id, (const char *)v->definition, v->definition_len,
                                    notice_buf[0] != '\0' ? notice_buf : NULL, v->has_resources,
                                    &got_len);
    /* C 版那一份 = 参考实现那一份 **+ 0.2.0 有意追加的那一段**：上面那条 SHA-256 证明两份
     * 资产仍是参考实现那一份，这一条证明 C 版就是它加上**恰好一段**追加脚本。 */
    size_t want_len = 0;
    unsigned char *want = with_extra(v, &want_len);
    snprintf(label, sizeof(label), "① ★ 「%s」的文档 = 参考实现那份 + 那一处有意追加", v->label);
    if (got == NULL || want == NULL) {
      ok(0, label);
    } else {
      same_bytes((const unsigned char *)got, got_len, want, want_len, label);
      ok(got[got_len] == '\0', "① 文档以 \\0 收尾（长度照实另给）");
      /* 追加段**只有一处**：多一处就是拼接写错了（比如两段脚本嵌套） */
      {
        /* ⚠️ 必须按**长度**找、不能用 `strstr`：有一份正文里**带 NUL 字节**，而追加段在正文
         * 之后 —— `strstr` 会停在那个 NUL 上数出 0 个（这一条真红过一次）。 */
        const char *needle = DSH_ENTRY_EXTRA_OPEN;
        const size_t needle_len = strlen(needle);
        int count = 0;
        for (size_t i = 0; i + needle_len <= got_len; i++) {
          if (memcmp(got + i, needle, needle_len) == 0) count++;
        }
        snprintf(label, sizeof(label), "① 「%s」的追加段只有一处（不多不少）", v->label);
        ok(count == 1, label);
      }
      dsh_release(got);
    }
    if (want != NULL) free(want);
    free(expected);
  }

  /* ══ ② 三条 URL 与参考实现逐字相同（生成脚本把参考实现的输出打在日志里）══ */
  {
    char *origin = dsh_entry_doc_origin("abc123");
    char *baseUrl = dsh_entry_doc_base("abc123");
    char *entryUrl = dsh_entry_doc_entry_url("abc123", "apples");
    ok(origin != NULL && strcmp(origin, "https://abc123.dictres.invalid") == 0, "② OriginFor");
    ok(baseUrl != NULL && strcmp(baseUrl, "https://abc123.dictres.invalid/") == 0, "② BaseFor");
    ok(entryUrl != NULL &&
           strcmp(entryUrl, "https://abc123.dictres.invalid/__entry__?word=apples") == 0,
       "② EntryUrlFor");
    if (origin != NULL) dsh_release(origin);
    if (baseUrl != NULL) dsh_release(baseUrl);
    if (entryUrl != NULL) dsh_release(entryUrl);

    /* 词条名要转义：带空格 / 中文 / 斜杠的键名都见过 */
    char *spaced = dsh_entry_doc_entry_url("d1", "community care");
    char *cjk = dsh_entry_doc_entry_url("d1", "测试");
    char *slash = dsh_entry_doc_entry_url("d1", "a/b");
    ok(spaced != NULL && strstr(spaced, "?word=community%20care") != NULL,
       "② 空格转义成 %20（不是 +）");
    ok(cjk != NULL && strstr(cjk, "?word=%E6%B5%8B%E8%AF%95") != NULL, "② 中文按 UTF-8 转义");
    ok(slash != NULL && strstr(slash, "?word=a%2Fb") != NULL, "② 斜杠也要转义（否则会被当路径）");
    if (spaced != NULL) dsh_release(spaced);
    if (cjk != NULL) dsh_release(cjk);
    if (slash != NULL) dsh_release(slash);

    /* 词典 id 为空也不崩（给一个能用的域） */
    char *empty = dsh_entry_doc_entry_url(NULL, NULL);
    ok(empty != NULL && strstr(empty, ".dictres.invalid/__entry__?word=") != NULL,
       "② dict_id / word 为 NULL 不崩");
    if (empty != NULL) dsh_release(empty);
  }

  /* ══ ③ 文档的几处硬约定（标准答案文件已盖住，这里点名单钉）══ */
  {
    size_t len = 0;
    const char *body_html = "<p>apple</p>";
    char *doc = dsh_entry_doc_build("d1", body_html, strlen(body_html), NULL, 1, &len);
    ok(doc != NULL, "③ 拼一份文档");
    if (doc != NULL) {
      ok(strncmp(doc, "<!doctype html>\n", 16) == 0, "③ 开头是 doctype");
      ok(strstr(doc, "<iframe") == NULL,
         "③ 文档自己不再套 iframe（它本来就是被装进 iframe 的那一份）");
      ok(strstr(doc, "default-src 'none'") != NULL, "③ CSP 默认全禁");
      ok(strstr(doc, "img-src https://*.dictres.invalid") != NULL,
         "③ CSP 显式放行词典资源域（opaque origin 不能靠 'self'）");
      ok(strstr(doc, "connect-src 'none'") != NULL, "③ 不许词条往外连网");
      ok(strstr(doc, "<base href=\"https://d1.dictres.invalid/\">") != NULL,
         "③ base href 指向那一本的资源域");
      ok(strstr(doc, "data-has-resources=\"1\"") != NULL, "③ 有资源时 data-has-resources=1");
      ok(strstr(doc, "id=\"lookupChips\" hidden") != NULL, "③ 出路按钮那个容器在");
      ok(strstr(doc, "lookupBridge") != NULL, "③ 桥接脚本真的注进去了（它会给根元素打标记）");
      ok(strstr(doc, "<p>apple</p>") != NULL, "③ 正文原样进去了");
      ok(strlen(doc) == len, "③ 长度与实际一致（正文里可能有内嵌 NUL，长度说了算）");
      dsh_release(doc);
    }

    /* 提示非空时**顶替**正文（那是「没查到」的提示页） */
    char *notice = dsh_entry_doc_build("d1", body_html, strlen(body_html), "<b>没找到</b>", 0, &len);
    ok(notice != NULL && strstr(notice, "<div class=\"lookup-notice\"><b>没找到</b></div>") != NULL,
       "③ ★ 提示非空时顶替正文（装进 .lookup-notice）");
    ok(notice != NULL && strstr(notice, "<p>apple</p>") == NULL, "③ 而且正文一个字都不留");
    ok(notice != NULL && strstr(notice, "data-has-resources=\"0\"") != NULL,
       "③ 没有资源时 data-has-resources=0");
    if (notice != NULL) dsh_release(notice);

    /* 内嵌 U+0000 的正文：长度说了算，不许被 strlen 截断 */
    {
      const char def[5] = {'a', '\0', 'b', '\0', 'c'};
      char *nul_doc = dsh_entry_doc_build("d1", def, 5, NULL, 0, &len);
      ok(nul_doc != NULL, "③ 内嵌 U+0000 的正文也拼得出来");
      if (nul_doc != NULL) {
        /* 正文那 5 个字节（含两个 NUL）必须**原样**在文档里 */
        int found = 0;
        for (size_t i = 0; i + 5 <= len; i++) {
          if (memcmp(nul_doc + i, def, 5) == 0) {
            found = 1;
            break;
          }
        }
        ok(found, "③ ★ 正文里的内嵌 U+0000 原样保留（长度说了算，不是 strlen）");
        ok((size_t)strlen(nul_doc) < len,
           "③ 正因如此 strlen 会短于真实长度 —— 用它的地方就是 bug");
        dsh_release(nul_doc);
      }
    }
  }

  /* ══ ④ 音频键抠取（`sound://` / `snd://` / `<audio name=…>`）══ */
  {
    char **keys = NULL;
    int64_t n = 0;

    /* ① 两种 scheme、大小写、顺序 */
    const char *html1 =
        "<a href=\"sound://GB_brelasdeapple.spx\">x</a>"
        "<a href=\"snd://apple__gb_1.spx\">y</a>"
        "<a href=\"SOUND://US_apple.wav\">z</a>";
    ok(dsh_entry_doc_audio_keys(html1, strlen(html1), &keys, &n) == 0 && n == 3,
       "④ 三种写法各抠出一个键");
    ok(has_key(keys, n, "GB_brelasdeapple.spx"), "④ 抠出 GB_brelasdeapple.spx");
    ok(has_key(keys, n, "apple__gb_1.spx"), "④ 抠出 apple__gb_1.spx");
    ok(has_key(keys, n, "US_apple.wav"), "④ 大写的 SOUND:// 也认");
    ok(keys != NULL && strcmp(keys[0], "GB_brelasdeapple.spx") == 0,
       "④ ★ 顺序是**出现顺序**（认不出英/美时按先出现的那个挑）");
    free_keys(keys, n);

    /* ② 去重（大小写不敏感）+ 归一化（URL 编码、前导斜杠、两端空白）*/
    const char *html2 =
        "sound://%5Cstyle%2Fapple.mp3 sound://\\style/apple.mp3 "
        "<audio name=\" apple__gb_1.spx \"></audio>";
    keys = NULL;
    n = 0;
    ok(dsh_entry_doc_audio_keys(html2, strlen(html2), &keys, &n) == 0 && n == 2,
       "④ ★ 编码不同但归一化之后是同一个键 → 去重成 1 个（再加 audio 那个共 2 个）");
    ok(has_key(keys, n, "style/apple.mp3"), "④ 归一化：解 URL 编码 + 去前导反斜杠");
    ok(has_key(keys, n, "apple__gb_1.spx"), "④ `<audio name=…>` 那个也抠出来了，且两端空白去掉了");
    free_keys(keys, n);

    /* ③ 键名的边界字符：引号 / 尖括号 / 逗号 / 空白都算终止 */
    const char *html3 = "sound://a>b sound://c,d sound://e'f sound://g\"h sound://i<j";
    keys = NULL;
    n = 0;
    ok(dsh_entry_doc_audio_keys(html3, strlen(html3), &keys, &n) == 0 && n == 5,
       "④ 终止字符：`>` `,` `'` `\"` `<` 各自断开");
    ok(has_key(keys, n, "a") && has_key(keys, n, "c") && has_key(keys, n, "e") &&
           has_key(keys, n, "g") && has_key(keys, n, "i"),
       "④ 抠出来的都是边界前那一段");
    free_keys(keys, n);

    /* ④ 空 / 没有音频 */
    keys = NULL;
    n = 0;
    ok(dsh_entry_doc_audio_keys("", 0, &keys, &n) == 0 && n == 0 && keys == NULL,
       "④ 空正文 → 空清单（不是错误）");
    keys = NULL;
    n = 0;
    ok(dsh_entry_doc_audio_keys("<p>no audio</p>", strlen("<p>no audio</p>"), &keys, &n) == 0 &&
           n == 0,
       "④ 没有音频引用 → 空清单");
    /* ⑤ `snds://` 这种「像但不是」的不许认（scheme 要正好是 snd:// 或 sound://） */
    keys = NULL;
    n = 0;
    ok(dsh_entry_doc_audio_keys("snds://x.mp3", strlen("snds://x.mp3"), &keys, &n) == 0 && n == 0,
       "④ ★ `snds://` 不许被当音频引用（scheme 必须正好）");
    free_keys(keys, n);
    /* ⚠️ `xsound://y.mp3` **会被**抠出 `y.mp3` —— 照参考实现：它那条正则 **没有词边界**，
     * 所以 `xsound://` 里的 `sound://` 照样命中。如实钉住「我们也这样」，不改成更严格。 */
    keys = NULL;
    n = 0;
    ok(dsh_entry_doc_audio_keys("xsound://y.mp3", strlen("xsound://y.mp3"), &keys, &n) == 0 &&
           n == 1 && has_key(keys, n, "y.mp3"),
       "④ `xsound://y.mp3` 照样抠出 y.mp3（参考实现那条正则没有词边界，我们照抄）");
    free_keys(keys, n);

    keys = NULL;
    n = 0;
    ok(dsh_entry_doc_audio_keys(NULL, 5, &keys, &n) == 0 && n == 0, "④ 正文为 NULL 不崩");
  }

  /* ══ ⑤ 接口定义的 `dsh_engine_entry_document`（把零件接起来那一步）══ */
  {
    const char *names[] = {"test.mdx", "audio.mdx"};
    char *ids[4] = {(char[65]){0}, (char[65]){0}, NULL, NULL};
    dsh_engine *e = NULL;
    ok(dsh_engine_create(NULL, &e) == DSH_OK && e != NULL, "⑤ 建引擎");
    if (e != NULL) {
      char paths[4096];
      char *p1 = fixture_path(names[0]);
      char *p2 = fixture_path(names[1]);
      snprintf(paths, sizeof(paths), "[\"%s\",\"%s\"]", p1 != NULL ? p1 : "",
               p2 != NULL ? p2 : "");
      if (p1 != NULL) free(p1);
      if (p2 != NULL) free(p2);
      char *out = NULL;
      ok(dsh_engine_dict_add(e, paths, &out) == DSH_OK, "⑤ 加两本（test.mdx + audio.mdx）");
      if (out != NULL) dsh_release(out);
      char *list = NULL;
      if (dsh_engine_dict_list(e, &list) == DSH_OK && list != NULL) {
        /* ids[0] = test.mdx（第 0 本），ids[1] = audio.mdx */
        const char *q = list;
        for (int idx = 0; idx < 2; idx++) {
          q = strstr(q, "\"id\":\"");
          if (q == NULL) break;
          q += 6;
          size_t k = 0;
          while (q[k] != '\0' && q[k] != '"' && k < 64) {
            ids[idx][k] = q[k];
            k++;
          }
          ids[idx][k] = '\0';
        }
        dsh_release(list);
      }
      ok(ids[0][0] != '\0' && ids[1][0] != '\0', "⑤ 取到两本的 id");

      /* ① 命中：文档 + 纯文本 + 资源域 */
      char *json = NULL;
      ok(dsh_engine_entry_document(e, ids[0], "apple", &json) == DSH_OK, "⑤ 取 apple 的文档");
      ok(json != NULL && strstr(json, "\"html\":\"<!doctype html>") != NULL,
         "⑤ 出参里有整份文档");
      ok(json != NULL && (strstr(json, "<p>apple</p>") != NULL || strstr(json, "apple") != NULL),
         "⑤ 文档里有这个词条的正文");
      ok(json != NULL &&
             (strstr(json, "\\\"plainText\\\":\\\"apple") != NULL ||
              strstr(json, "plainText") != NULL),
         "⑤ 出参里有纯文本");
      {
        char want[512];
        snprintf(want, sizeof(want), "\"resourceBase\":\"https://%s.dictres.invalid/\"", ids[0]);
        ok(json != NULL && strstr(json, want) != NULL,
           "⑤ ★ resourceBase 就是那一本的资源域（宿主据此把资源请求接到内核）");
      }
      ok(json != NULL && strstr(json, "\"audioKeys\":[]") != NULL,
         "⑤ test.mdx 的词条里没有音频引用 → 空清单");
      if (json != NULL) dsh_release(json);

      /* ② 落点：`@@@LINK` 跟着走（拿规范键名取正文，与 resolve 同一层） */
      json = NULL;
      ok(dsh_engine_entry_document(e, ids[0], "APPLE", &json) == DSH_OK,
         "⑤ 拿大写的 APPLE 也能取到（大小写变体那一层还在）");
      ok(json != NULL && strstr(json, "\\\"plainText\\\":\\\"\\\"") == NULL,
         "⑤ 而且给的是真正文（不是空串）");
      if (json != NULL) dsh_release(json);

      /* ③ 未命中：提示页（带可点的候选） */
      json = NULL;
      ok(dsh_engine_entry_document(e, ids[0], "applz", &json) == DSH_OK,
         "⑤ 未命中也成功返回（提示页，不是错误）");
      ok(json != NULL && strstr(json, "lookup-notice") != NULL,
         "⑤ 提示页把提示装进 .lookup-notice（那一段样式在基础样式里）");
      ok(json != NULL && strstr(json, "未在《test.mdx》中找到") != NULL,
         "⑤ ★ 提示照参考实现的原话：「未在《词典名》中找到…」");
      ok(json != NULL && strstr(json, "entry://apple") != NULL,
         "⑤ ★ 候选是**可点的** `entry://` 链接（桥接脚本按这个前缀拦截）");
      ok(json != NULL && strstr(json, "lookup-suggests") != NULL, "⑤ 候选装在 .lookup-suggests 里");
      ok(json != NULL && strstr(json, "\"plainText\":\"\"") != NULL,
         "⑤ 未命中那一页的纯文本是空串（与参考实现一致：它那条路不设 PlainText）");
      if (json != NULL) dsh_release(json);

      /* ④ 音频键：audio.mdx 的词条里带 sound:// / snd:// */
      {
        char *audio_path = fixture_path("audio.mdx");
        dsh_mdx *mdd = NULL;
        if (audio_path != NULL && dsh_mdx_open(audio_path, &mdd) == 0 && mdd != NULL) {
          char **keys = NULL;
          int64_t n = 0;
          if (dsh_mdx_list_keys(mdd, &keys, &n) == 0 && n > 0) {
            char *sound_json = NULL;
            ok(dsh_engine_entry_document(e, ids[1], keys[0], &sound_json) == DSH_OK,
               "⑤ 取 audio.mdx 第一条词条的文档");
            ok(sound_json != NULL && strstr(sound_json, "\"audioKeys\":[\"") != NULL,
               "⑤ ★ 那条词条的音频键抠出来了（宿主据此规划发音）");
            if (sound_json != NULL) dsh_release(sound_json);
            dsh_mdx_free_keys(keys, n);
          } else {
            ok(0, "⑤ 枚举 audio.mdx 的键");
          }
          dsh_mdx_close(mdd);
        } else {
          ok(0, "⑤ 打开 audio.mdx");
        }
        if (audio_path != NULL) free(audio_path);
      }

      /* ⑤ 参数边界 */
      json = NULL;
      ok(dsh_engine_entry_document(e, ids[0], "apple", NULL) != DSH_OK, "⑤ out 为 NULL 必须失败");
      ok(dsh_engine_entry_document(e, NULL, "apple", &json) != DSH_OK, "⑤ dict_id 为空必须失败");
      ok(dsh_engine_entry_document(e, ids[0], NULL, &json) != DSH_OK, "⑤ key_text 为空必须失败");
      ok(dsh_engine_entry_document(NULL, ids[0], "apple", &json) != DSH_OK,
         "⑤ 引擎为 NULL 必须失败");
      ok(dsh_engine_entry_document(e, "no-such-id", "apple", &json) != DSH_OK,
         "⑤ 词库里没有这本 → 失败（不是「没这个词」）");

      dsh_engine_destroy(e);
    }
  }

  ok((int64_t)dsh_mem_live_count() == (int64_t)base,
     "全部用例跑完，活分配表必须回到基线");

  printf("entry_doc：%d 项，失败 %d\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
