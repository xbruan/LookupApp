/* 内核单元测试 · HTML 文本工具（text/dsh_html.c）
 * 检查标准由标准答案文件驱动（tests/html_vectors.h 由 make-html-vectors.ps1 跑参考实现的 HtmlUtils.cs 产出）：
 * 只写「必须与参考实现逐字节一致」，期望值从不手抄 —— C 版把 8 条 .NET 正则手写成扫描器，最怕 99% 一样。
 */

#include "text/dsh_html.h"
#include "dsh_lookup.h"
#include "mem_registry.h"

#include <stdio.h>
#include <string.h>

#include "html_vectors.h"

static int g_checks = 0;
static int g_failed = 0;

static void ok(int cond, const char *what) {
  g_checks++;
  if (!cond) {
    g_failed++;
    fprintf(stderr, "FAIL %s\n", what);
  }
}

/** 把一段字节原样打进 stderr（超出 cap 就截断），失败的现场靠它看 */
static void dump(const char *tag, const unsigned char *bytes, size_t len) {
  fprintf(stderr, "      %s(%zu) = \"", tag, len);
  for (size_t i = 0; i < len && i < 120; i++) {
    const unsigned char c = bytes[i];
    if (c == '\n') {
      fputs("\\n", stderr);
    } else if (c == '\r') {
      fputs("\\r", stderr);
    } else if (c == '\t') {
      fputs("\\t", stderr);
    } else if (c < 0x20 || c == 0x7F) {
      fprintf(stderr, "\\x%02X", c);
    } else {
      fputc(c, stderr);
    }
  }
  if (len > 120) fputs("…", stderr);
  fputs("\"\n", stderr);
}

/** 跑一张表：逐字节比对（长度 + 内容），并顺手钉住「内核分配、内核释放」 */
static void run_table(const char *title, const dsh_html_vector *table, int count,
                      char *(*fn)(const char *, size_t, size_t *)) {
  for (int i = 0; i < count; i++) {
    const dsh_html_vector *v = &table[i];
    size_t got_len = (size_t)-1;
    char *got = fn((const char *)v->in, v->in_len, &got_len);
    char what[160];
    snprintf(what, sizeof(what), "%s：%s", title, v->label);
    if (got == NULL) {
      g_checks++;
      g_failed++;
      fprintf(stderr, "FAIL %s（函数返回 NULL）\n", what);
      dump("in ", v->in, v->in_len);
      continue;
    }
    if (got_len != v->out_len || memcmp(got, v->out, v->out_len) != 0) {
      g_checks++;
      g_failed++;
      fprintf(stderr, "FAIL %s（与参考实现不一致）\n", what);
      dump("in     ", v->in, v->in_len);
      dump("实际   ", (const unsigned char *)got, got_len);
      dump("参考   ", v->out, v->out_len);
    } else {
      g_checks++;
    }
    /* 末尾那个 \0 也要在（长度是照实给的，`out[out_len]` 必须可读且是 0） */
    ok(got[got_len] == '\0', "返回的缓冲区必须以 \\0 收尾");
    dsh_release(got);
  }
}

int main(void) {
  const size_t base = dsh_mem_live_count();

  run_table("去 HTML", HTML_STRIP_VECTORS, HTML_STRIP_VECTOR_COUNT, dsh_html_strip);
  run_table("实体还原", HTML_DECODE_VECTORS, HTML_DECODE_VECTOR_COUNT, dsh_html_decode_entities);
  run_table("HTML 转义", HTML_ESCAPE_VECTORS, HTML_ESCAPE_VECTOR_COUNT, dsh_html_escape);
  run_table("内联脚本转义", HTML_INLINE_VECTORS, HTML_INLINE_VECTOR_COUNT,
            dsh_html_escape_for_inline_script);

  /* ── ① 内嵌 U+0000 必须能被表示出来：`&#0;` 在参考实现里是一个 NUL 字符，用 char * 传递会
   * 在那里不报错地截断 —— 所以这一层进出都带长度，长度说了算而不是 strlen 说了算 */
  {
    const char *in = "a&#0;b";
    size_t len = 0;
    char *out = dsh_html_strip(in, strlen(in), &len);
    ok(out != NULL, "① 内嵌 U+0000：函数成功");
    if (out != NULL) {
      ok(len == 3, "① 内嵌 U+0000：长度必须是 3（a \\0 b）");
      ok(out[0] == 'a' && out[1] == '\0' && out[2] == 'b',
         "① 内嵌 U+0000：中间那个字节就是 NUL（不是被截断的两个字节）");
      dsh_release(out);
    }
  }

  /* ── ② 空输入 / NULL 输入不崩，给出空串 ── */
  {
    static char *(*const FNS[])(const char *, size_t, size_t *) = {
        dsh_html_strip, dsh_html_decode_entities, dsh_html_escape,
        dsh_html_escape_for_inline_script};
    for (size_t i = 0; i < sizeof(FNS) / sizeof(FNS[0]); i++) {
      size_t len = 99;
      char *a = FNS[i]("", 0, &len);
      ok(a != NULL && len == 0, "② 空输入 → 空结果，长度 0");
      if (a != NULL) dsh_release(a);
      len = 99;
      char *b = FNS[i](NULL, 0, &len);
      ok(b != NULL && len == 0, "② NULL 输入 → 空结果（不崩）");
      if (b != NULL) dsh_release(b);
      /* out_len 可以不要（接口定义里它不是必填） */
      char *c = FNS[i]("x", 1, NULL);
      ok(c != NULL, "② out_len 传 NULL 也能用");
      if (c != NULL) dsh_release(c);
    }
  }

  /* ── ③ 孤立代理项：参考实现抛异常，内核绝不抛（已知差别，钉住） */
  {
    static char *(*const FNS[])(const char *, size_t, size_t *) = {
        dsh_html_strip, dsh_html_decode_entities};
    for (size_t i = 0; i < sizeof(FNS) / sizeof(FNS[0]); i++) {
      for (int k = 0; k < HTML_REF_THROWS_COUNT; k++) {
        const dsh_html_input *v = &HTML_REF_THROWS[k];
        size_t len = 0;
        char *out = FNS[i]((const char *)v->in, v->in_len, &len);
        char what[192];
        snprintf(what, sizeof(what),
                 "③ 孤立代理项（%s）：内核不抛、如实保留原文", v->label);
        /* 参考实现在这些输入上抛 ArgumentOutOfRangeException（生成脚本已断言），C 版如实保留原文：
         * 这一条钉的是「不崩、不吞、不改写」。 */
        ok(out != NULL, what);
        if (out != NULL) {
          if (i == 1) {
            /* 只做实体还原那一档：原样保留 */
            ok(len == v->in_len && memcmp(out, v->in, len) == 0,
               "③ 实体还原：孤立代理项原样留着（不换成半个错字）");
          } else {
            ok(len >= 1, "③ 去 HTML：给出了结果而不是崩掉");
          }
          dsh_release(out);
        }
      }
    }
  }

  /* ── ④ 转义与还原是互逆的（除实体那几种写法以外） */
  {
    static const char *const SAMPLES[] = {
        "apple", "a & b", "<p>x</p>", "\"引号\" 与 '撇号'", "中文 & 符号",
        "a<b>c&d\"e'f", ""};
    for (size_t i = 0; i < sizeof(SAMPLES) / sizeof(SAMPLES[0]); i++) {
      size_t esc_len = 0;
      char *esc = dsh_html_escape(SAMPLES[i], strlen(SAMPLES[i]), &esc_len);
      ok(esc != NULL, "④ 转义成功");
      if (esc == NULL) continue;
      size_t back_len = 0;
      char *back = dsh_html_decode_entities(esc, esc_len, &back_len);
      ok(back != NULL, "④ 还原成功");
      if (back != NULL) {
        char label[160];
        snprintf(label, sizeof(label), "④ 转义再还原必须等于原文：「%.40s」", SAMPLES[i]);
        ok(back_len == strlen(SAMPLES[i]) && memcmp(back, SAMPLES[i], back_len) == 0, label);
        dsh_release(back);
      }
      dsh_release(esc);
    }
  }

  /* ── ⑤ 一条真实词条的纯文本（「复制释义」看到的就是这个） */
  {
    static const char *const ENTRY =
        "<link rel=\"stylesheet\" href=\"oale8.css\">"
        "<div class=\"entry\"><span class=\"hw\">apple</span><br>"
        "<span class=\"pr\">ˈæpl</span><br>"
        "<div class=\"def\">n. 苹果 &amp; 苹果树</div>"
        "<script>void(0)</script></div>";
    size_t len = 0;
    char *out = dsh_html_strip(ENTRY, strlen(ENTRY), &len);
    ok(out != NULL, "⑤ 真词条去 HTML 成功");
    if (out != NULL) {
      ok(strstr(out, "apple") != NULL, "⑤ 词目留下了");
      ok(strstr(out, "苹果 & 苹果树") != NULL, "⑤ 实体还原成了真的 &");
      ok(strstr(out, "void(0)") == NULL, "⑤ 脚本内容不许出现在纯文本里");
      ok(strstr(out, "oale8.css") == NULL, "⑤ 样式表链接不许出现");
      ok(strstr(out, "<") == NULL, "⑤ 结果里不许再有标签");
      dsh_release(out);
    }
  }

  ok((int64_t)dsh_mem_live_count() == (int64_t)base,
     "跑完之后活分配表必须回到基线（内核分配、内核释放）");

  printf("html：%d 项，失败 %d\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
