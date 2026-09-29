/* 内核单元测试 · JSON 输出
 * 畸形 JSON 是一类「看起来没问题」的错：曾写出每个键前都多一个逗号的输出，而当时全是子串断言、
 * 照样全绿。所以这里一律**逐字节等于期望值**，不靠「含某个子串」。
 */

#include "dsh_lookup.h"
#include "json_writer.h"
#include "mem_registry.h"

#include <stdio.h>
#include <string.h>

static int g_checks = 0;
static int g_failed = 0;

static void ok_eq_str(const char *actual, const char *expected, const char *what) {
  g_checks++;
  if (actual == NULL || strcmp(actual, expected) != 0) {
    g_failed++;
    fprintf(stderr, "FAIL %s\n      实际=%s\n      期望=%s\n", what,
            actual ? actual : "(null)", expected);
  }
}

static void ok(int cond, const char *what) {
  g_checks++;
  if (!cond) { g_failed++; fprintf(stderr, "FAIL %s\n", what); }
}

/** 取一次结果（副本），调用方用 dsh_release 还给内核 */
static char *take(dsh_json *j) { return dsh_json_take(j); }

int main(void) {
  const size_t base = dsh_mem_live_count();

  /* ── 对象：每个键前不许有多余逗号（那个 bug 的回归）── */
  {
    dsh_json *j = dsh_json_new();
    dsh_json_object_begin(j);
    dsh_json_kv_str(j, "title", "x");
    dsh_json_kv_i64(j, "n", 6);
    dsh_json_kv_bool(j, "ok", 1);
    dsh_json_object_end(j);
    char *s = take(j);
    dsh_json_free(j);
    ok_eq_str(s, "{\"title\":\"x\",\"n\":6,\"ok\":true}", "对象：键值对的逗号位置必须正确");
    dsh_release(s);
  }

  /* ── 嵌套：对象里套数组、数组里套对象 ── */
  {
    dsh_json *j = dsh_json_new();
    dsh_json_object_begin(j);
    dsh_json_key(j, "items");
    dsh_json_array_begin(j);
    dsh_json_object_begin(j);
    dsh_json_kv_str(j, "word", "apple");
    dsh_json_kv_i64(j, "len", 5);
    dsh_json_object_end(j);
    dsh_json_object_begin(j);
    dsh_json_kv_str(j, "word", "测试");
    dsh_json_kv_i64(j, "len", 2);
    dsh_json_object_end(j);
    dsh_json_array_end(j);
    dsh_json_kv_bool(j, "hasMore", 0);
    dsh_json_object_end(j);
    char *s = take(j);
    dsh_json_free(j);
    ok_eq_str(s,
              "{\"items\":[{\"word\":\"apple\",\"len\":5},{\"word\":\"测试\",\"len\":2}],\"hasMore\":false}",
              "嵌套：对象数组混排的标点必须完全正确");
    dsh_release(s);
  }

  /* ── 空对象 / 空数组 ── */
  {
    dsh_json *j = dsh_json_new();
    dsh_json_object_begin(j);
    dsh_json_object_end(j);
    char *s = take(j);
    dsh_json_free(j);
    ok_eq_str(s, "{}", "空对象");
    dsh_release(s);

    j = dsh_json_new();
    dsh_json_array_begin(j);
    dsh_json_array_end(j);
    s = take(j);
    dsh_json_free(j);
    ok_eq_str(s, "[]", "空数组");
    dsh_release(s);
  }

  /* ── 转义：词条名里真的有半角双引号与反斜杠，漏一个就是坏 JSON ── */
  {
    dsh_json *j = dsh_json_new();
    dsh_json_object_begin(j);
    dsh_json_kv_str(j, "q", "say \"hi\"");
    dsh_json_kv_str(j, "path", "C:\\词典\\a.mdx");
    dsh_json_kv_str(j, "nl", "a\nb\tc");
    dsh_json_kv_str(j, "ctrl", "\x01\x1f");
    dsh_json_object_end(j);
    char *s = take(j);
    dsh_json_free(j);
    ok_eq_str(s,
              "{\"q\":\"say \\\"hi\\\"\",\"path\":\"C:\\\\词典\\\\a.mdx\","
              "\"nl\":\"a\\nb\\tc\",\"ctrl\":\"\\u0001\\u001f\"}",
              "转义：引号 / 反斜杠 / 控制字符都要按 RFC 8259 转");
    dsh_release(s);
  }

  /* ── 中文与 emoji 不转义（与参考实现的 Newtonsoft 一致：原样输出 UTF-8）── */
  {
    dsh_json *j = dsh_json_new();
    dsh_json_object_begin(j);
    dsh_json_kv_str(j, "zh", "牛津高阶英汉双解词典");
    dsh_json_kv_str(j, "emoji", "\xF0\x9F\x98\x80"); /* U+1F600 */
    dsh_json_object_end(j);
    char *s = take(j);
    dsh_json_free(j);
    ok_eq_str(s, "{\"zh\":\"牛津高阶英汉双解词典\",\"emoji\":\"\xF0\x9F\x98\x80\"}",
              "中文与 emoji 原样输出（不转成 \\uXXXX）");
    dsh_release(s);
  }

  /* ── NULL 字符串写成空串而不是 null：接口定义里「没有这个值」与「空串」是两件事，
   * 绝大多数键的语义是后者，要写 null 请用 dsh_json_null */
  {
    dsh_json *j = dsh_json_new();
    dsh_json_object_begin(j);
    dsh_json_kv_str(j, "a", NULL);
    dsh_json_key(j, "b");
    dsh_json_null(j);
    dsh_json_object_end(j);
    char *s = take(j);
    dsh_json_free(j);
    ok_eq_str(s, "{\"a\":\"\",\"b\":null}", "NULL 字符串写空串；要写 null 用 dsh_json_null");
    dsh_release(s);
  }

  /* ── 转义之后仍是合法 JSON：这里只用括号/引号计数兜一层，防「括号不成对」这类结构性错误
   * （真正的合法性检查在 Windows 侧由真宿主给） */
  {
    dsh_json *j = dsh_json_new();
    dsh_json_object_begin(j);
    dsh_json_kv_str(j, "k\"1", "v\"1");
    dsh_json_key(j, "arr");
    dsh_json_array_begin(j);
    dsh_json_str(j, "x\\y");
    dsh_json_array_end(j);
    dsh_json_object_end(j);
    char *s = take(j);
    dsh_json_free(j);

    int depth = 0, in_str = 0, escaped = 0, brackets_ok = 1;
    for (const char *p = s; *p; p++) {
      if (escaped) { escaped = 0; continue; }
      if (in_str && *p == '\\') { escaped = 1; continue; }
      if (*p == '"') { in_str = !in_str; continue; }
      if (in_str) continue;
      if (*p == '{' || *p == '[') depth++;
      else if (*p == '}' || *p == ']') { depth--; if (depth < 0) brackets_ok = 0; }
    }
    ok(brackets_ok && depth == 0 && !in_str,
       "转义之后括号仍成对、字符串仍闭合（防结构性错误）");
    dsh_release(s);
  }

  /* ── 内存：取回的每一份都要能还清 ── */
  {
    dsh_json *j = dsh_json_new();
    dsh_json_object_begin(j);
    dsh_json_kv_str(j, "a", "b");
    dsh_json_object_end(j);
    char *s1 = dsh_json_take(j);
    char *s2 = dsh_json_take(j);
    ok(s1 != NULL && s2 != NULL && s1 != s2, "take 两次要给两份独立内存");
    dsh_release(s1);
    dsh_release(s2);
    dsh_json_free(j);
  }

  {
    const size_t live = dsh_mem_live_count();
    g_checks++;
    if (live != base) {
      g_failed++;
      fprintf(stderr, "FAIL 泄漏：活分配表 %zu，期望 %zu\n", live, base);
    }
  }

  printf("json：%d 项，失败 %d\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
