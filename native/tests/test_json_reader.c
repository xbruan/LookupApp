/* 内核单元测试 · JSON 读取（写那一半在 test_json.c）。
 * 钉三类：该通过的逐字节核对取出来的值、畸形输入必须失败并报出位置、边界（内嵌 U+0000 连长度给、
 * 落单代理写 U+FFFD、坏 UTF-8 局部替换、嵌套上限、超范围整数）。往返是转义表两边唯一的保护，不能省。
 */

#include "dsh_lookup.h"
#include "json_reader.h"
#include "json_writer.h"
#include "mem_registry.h"

#include <stdio.h>
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
    fprintf(stderr, "FAIL %s：实际=%lld 期望=%lld\n", what, (long long)actual,
            (long long)expected);
  }
}

static void ok_eq_str(const char *actual, const char *expected, const char *what) {
  g_checks++;
  if (actual == NULL || expected == NULL || strcmp(actual, expected) != 0) {
    g_failed++;
    fprintf(stderr, "FAIL %s：实际=%s 期望=%s\n", what, actual ? actual : "(null)",
            expected ? expected : "(null)");
  }
}

/** 解析一段字面量（长度按 strlen 给 —— 测试文本里没有内嵌 NUL） */
static dsh_json_doc *parse_ok(const char *text, const char *what) {
  dsh_json_doc *doc = NULL;
  if (dsh_json_parse(text, strlen(text), &doc) != 0) {
    g_checks++;
    g_failed++;
    fprintf(stderr, "FAIL %s：本该解析成功，却报：%s\n", what, dsh_last_error_message());
    return NULL;
  }
  return doc;
}

/** 解析一段**应当失败**的字面量，并核对报错里含某个关键词 */
static void must_fail(const char *text, const char *keyword, const char *what) {
  dsh_json_doc *doc = NULL;
  const int rc = dsh_json_parse(text, strlen(text), &doc);
  g_checks++;
  if (rc == 0) {
    g_failed++;
    fprintf(stderr, "FAIL %s：本该失败，却成功了（%s）\n", what, text);
    dsh_json_doc_free(doc);
    return;
  }
  if (doc != NULL) {
    g_failed++;
    fprintf(stderr, "FAIL %s：失败时 out 必须保持 NULL\n", what);
    dsh_json_doc_free(doc);
  }
  if (keyword != NULL) {
    const char *msg = dsh_last_error_message();
    g_checks++;
    if (msg == NULL || strstr(msg, keyword) == NULL) {
      g_failed++;
      fprintf(stderr, "FAIL %s：报错里应当含「%s」，实际=%s\n", what, keyword,
              msg ? msg : "(null)");
    }
    if (msg != NULL) dsh_release((void *)msg);
  }
}

static const char *str_of(const dsh_json_node *n) {
  return dsh_json_str_value(n, NULL);
}

int main(void) {
  const size_t base = dsh_mem_live_count();

  /* ── ① 基本形状：每种值都取一遍 ── */
  {
    const char *text =
        "{\"s\":\"hi\",\"n\":-42,\"f\":1.5,\"e\":2e3,\"t\":true,\"z\":false,\"nil\":null,"
        "\"arr\":[1,\"two\",[3]],\"obj\":{\"k\":\"v\"}}";
    dsh_json_doc *doc = parse_ok(text, "基本形状");
    if (doc != NULL) {
      const dsh_json_node *root = dsh_json_doc_root(doc);
      ok(dsh_json_is_object(root), "根是对象");
      ok_eq_i64(dsh_json_object_len(root), 9, "对象成员个数");

      ok_eq_str(str_of(dsh_json_object_get(root, "s")), "hi", "取字符串");
      ok(dsh_json_is_string(dsh_json_object_get(root, "s")), "s 是字符串");

      int64_t iv = 0;
      ok(dsh_json_i64_value(dsh_json_object_get(root, "n"), &iv) == 0 && iv == -42,
         "取负整数 -42");

      double dv = 0;
      ok(dsh_json_double_value(dsh_json_object_get(root, "f"), &dv) == 0 && dv == 1.5,
         "取小数 1.5");
      ok(dsh_json_double_value(dsh_json_object_get(root, "e"), &dv) == 0 && dv == 2000.0,
         "取指数 2e3");
      /* 整数也能取成 double（JSON 里没有整数/浮点之分） */
      ok(dsh_json_double_value(dsh_json_object_get(root, "n"), &dv) == 0 && dv == -42.0,
         "整数取成 double");
      /* 而小数**取不成** int64 —— 不许悄悄截断 */
      ok(dsh_json_i64_value(dsh_json_object_get(root, "f"), &iv) != 0,
         "1.5 取 int64 必须失败（不许截断成 1）");

      ok(dsh_json_bool_value(dsh_json_object_get(root, "t")) == 1, "true");
      ok(dsh_json_bool_value(dsh_json_object_get(root, "z")) == 0, "false");
      ok(dsh_json_is_null(dsh_json_object_get(root, "nil")), "null");

      const dsh_json_node *arr = dsh_json_object_get(root, "arr");
      ok_eq_i64(dsh_json_array_len(arr), 3, "数组长度");
      ok_eq_i64(dsh_json_array_len(NULL), -1, "NULL 不是数组（长度 -1）");
      ok_eq_i64(dsh_json_array_len(root), -1, "对象不是数组");
      ok_eq_i64(dsh_json_i64_value(dsh_json_array_at(arr, 0), &iv) == 0 ? iv : -999, 1,
                "数组[0]");
      ok_eq_str(str_of(dsh_json_array_at(arr, 1)), "two", "数组[1]");
      ok_eq_i64(dsh_json_array_len(dsh_json_array_at(arr, 2)), 1, "数组[2] 是数组");
      ok(dsh_json_array_at(arr, 3) == NULL, "数组越界 → NULL");
      ok(dsh_json_array_at(arr, -1) == NULL, "数组负下标 → NULL");

      ok_eq_str(str_of(dsh_json_object_get(dsh_json_object_get(root, "obj"), "k")), "v",
                "对象里的对象");
      ok(dsh_json_object_get(root, "没有这个键") == NULL, "不存在的键 → NULL");
      /* 名字比对区分大小写 */
      ok(dsh_json_object_get(root, "S") == NULL, "键名比对区分大小写");

      /* 遍历接口：键与值都要能取到 */
      const char *k0 = NULL;
      size_t k0len = 0;
      const dsh_json_node *v0 = dsh_json_object_at(root, 0, &k0, &k0len);
      ok(v0 != NULL && k0 != NULL && k0len == 1 && k0[0] == 's', "按序号取成员并给出键");
      ok(dsh_json_object_at(root, 9, NULL, NULL) == NULL, "对象下标越界 → NULL");
      dsh_json_doc_free(doc);
    }
  }
  ok_eq_i64((int64_t)dsh_mem_live_count(), (int64_t)base, "解析并释放之后活分配表回到基线");

  /* ── ② 字符串：转义、内嵌 U+0000、代理对、坏 UTF-8 ── */
  {
    dsh_json_doc *doc = parse_ok("\"a\\\"b\\\\c\\/d\\be\\ff\\ng\\rh\\ti\"", "各种转义");
    if (doc != NULL) {
      size_t len = 0;
      const char *s = dsh_json_str_value(dsh_json_doc_root(doc), &len);
      const char expected[] = "a\"b\\c/d\be\ff\ng\rh\ti";
      ok(len == sizeof(expected) - 1 && memcmp(s, expected, len) == 0, "转义序列逐个解对");
      dsh_json_doc_free(doc);
    }
  }
  {
    /* \u0000 解出来是**真 NUL** —— 所以必须连长度给（头文件里那条约定） */
    dsh_json_doc *doc = parse_ok("\"ab\\u0000cd\"", "内嵌 U+0000");
    if (doc != NULL) {
      size_t len = 0;
      const char *s = dsh_json_str_value(dsh_json_doc_root(doc), &len);
      ok_eq_i64((int64_t)len, 5, "内嵌 U+0000 的字节数（不是 strlen 的 2）");
      ok(len == 5 && s[0] == 'a' && s[1] == 'b' && s[2] == '\0' && s[3] == 'c' && s[4] == 'd',
         "内嵌 U+0000 的内容");
      ok(s[5] == '\0', "缓冲仍以 \\0 结尾（可以当 C 字符串用，只是会截断）");
      dsh_json_doc_free(doc);
    }
  }
  {
    /* 基本多文种平面 + 代理对 */
    dsh_json_doc *doc = parse_ok("\"\\u4e2d\\uD83D\\uDE00\"", "代理对");
    if (doc != NULL) {
      size_t len = 0;
      const char *s = dsh_json_str_value(dsh_json_doc_root(doc), &len);
      ok_eq_i64((int64_t)len, 7, "「中」3 字节 + U+1F600 4 字节");
      ok((unsigned char)s[0] == 0xE4 && (unsigned char)s[1] == 0xB8 && (unsigned char)s[2] == 0xAD,
         "「中」的 UTF-8 编码");
      ok((unsigned char)s[3] == 0xF0 && (unsigned char)s[4] == 0x9F, "U+1F600 的 UTF-8 编码");
      dsh_json_doc_free(doc);
    }
  }
  {
    /* 落单的代理：**不失败**，写成 U+FFFD（与浏览器 TextDecoder 同约定） */
    dsh_json_doc *doc = parse_ok("\"\\uD83Dx\"", "落单的高代理");
    if (doc != NULL) {
      size_t len = 0;
      const char *s = dsh_json_str_value(dsh_json_doc_root(doc), &len);
      ok_eq_i64((int64_t)len, 4, "U+FFFD(3) + 'x'(1)");
      ok((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBF && (unsigned char)s[2] == 0xBD,
         "落单代理 → U+FFFD");
      ok(s[3] == 'x', "坏序列之后的内容照常解出来");
      dsh_json_doc_free(doc);
    }
  }
  {
    /* 原样的 UTF-8 中文（不是 \u 转义）要照原样保留 */
    dsh_json_doc *doc = parse_ok("\"苹果\"", "原样 UTF-8");
    if (doc != NULL) {
      size_t len = 0;
      const char *s = dsh_json_str_value(dsh_json_doc_root(doc), &len);
      ok(len == 6 && memcmp(s, "苹果", 6) == 0, "原样 UTF-8 中文");
      dsh_json_doc_free(doc);
    }
  }
  {
    /* 坏 UTF-8 字节：**不算失败**，每个坏字节一个 U+FFFD，继续往下解 */
    const char bad[] = {'"', 'a', (char)0xC3, (char)0x28, 'b', '"'};
    dsh_json_doc *doc = NULL;
    const int rc = dsh_json_parse(bad, sizeof(bad), &doc);
    ok(rc == 0, "字符串里的坏 UTF-8 不算失败");
    if (rc == 0) {
      size_t len = 0;
      const char *s = dsh_json_str_value(dsh_json_doc_root(doc), &len);
      /* 0xC3 是残缺序列 → 一个 U+FFFD；'(' 是合法 ASCII → 原样 */
      ok_eq_i64((int64_t)len, 1 + 3 + 1 + 1, "坏字节按 U+FFFD 替换后的长度");
      ok(s[0] == 'a' && (unsigned char)s[1] == 0xEF && s[4] == '(' && s[5] == 'b',
         "坏字节局部替换、其余照常");
      dsh_json_doc_free(doc);
    }
  }
  ok_eq_i64((int64_t)dsh_mem_live_count(), (int64_t)base, "字符串各例之后活分配表回到基线");

  /* ── ③ 数值边界 ── */
  {
    struct { const char *text; int64_t want; } cases[] = {
        {"0", 0}, {"-0", 0}, {"9223372036854775807", INT64_MAX},
        {"-9223372036854775808", INT64_MIN}, {"1e2", -1 /* 有指数 → 不是整数 */},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      dsh_json_doc *doc = parse_ok(cases[i].text, cases[i].text);
      if (doc == NULL) continue;
      int64_t v = 0;
      const int rc = dsh_json_i64_value(dsh_json_doc_root(doc), &v);
      if (cases[i].want == -1) {
        ok(rc != 0, "带指数的数值取 int64 必须失败");
      } else {
        ok(rc == 0 && v == cases[i].want, cases[i].text);
      }
      dsh_json_doc_free(doc);
    }
  }
  {
    /* 超范围：**如实失败**，不许返回截断值 */
    dsh_json_doc *doc = parse_ok("9223372036854775808", "int64 上界+1");
    if (doc != NULL) {
      int64_t v = 12345;
      ok(dsh_json_i64_value(dsh_json_doc_root(doc), &v) != 0 && v == 0,
         "超范围的整数必须失败（且不写回截断值）");
      double d = 0;
      ok(dsh_json_double_value(dsh_json_doc_root(doc), &d) == 0 && d > 9.2e18,
         "同一个数取 double 应当成功");
      dsh_json_doc_free(doc);
    }
    doc = parse_ok("-9223372036854775809", "int64 下界-1");
    if (doc != NULL) {
      int64_t v = 0;
      ok(dsh_json_i64_value(dsh_json_doc_root(doc), &v) != 0, "越过 int64 下界必须失败");
      dsh_json_doc_free(doc);
    }
  }

  /* ── ④ 该失败的必须失败（每条一种畸形，别合并）── */
  must_fail("", "提前结束", "空文本");
  must_fail("   ", "提前结束", "只有空白");
  must_fail("{", "缺少", "只有左花括号");
  must_fail("}", "不认识", "只有右花括号");
  must_fail("[1,2", "缺少", "数组没闭合");
  must_fail("{\"a\":1,}", "不是", "尾随逗号（对象）");
  must_fail("[1,2,]", "不是", "尾随逗号（数组）");
  must_fail("{,\"a\":1}", "键必须是字符串", "开头多一个逗号");
  must_fail("{\"a\"1}", "':'", "键之后没有冒号");
  must_fail("{a:1}", "键必须是字符串", "键没加引号");
  must_fail("{'a':1}", "键必须是字符串", "单引号");
  must_fail("nul", "字面量", "写坏的 null");
  must_fail("truefalse", NULL, "根值之后多余内容");
  must_fail("1 2", "多余", "两个根值");
  must_fail("01", NULL, "前导零");
  must_fail("-", NULL, "只有一个负号");
  must_fail("1.", "数值写法不合法", "小数点后没有数字");
  must_fail("1e", NULL, "指数没有数字");
  must_fail(".5", "不认识", "没有整数部分");
  must_fail("NaN", NULL, "NaN 不是 JSON");
  must_fail("Infinity", NULL, "Infinity 不是 JSON");
  must_fail("\"abc", "不认识", "字符串没闭合");
  must_fail("\"a\\q\"", NULL, "不认识的转义");
  must_fail("\"a\\u12\"", NULL, "\\u 只有两位");
  must_fail("\"a\\uZZZZ\"", NULL, "\\u 后面不是十六进制");
  must_fail("\"a\tb\"", NULL, "字符串里原样放制表符（控制字符必须转义）");
  must_fail("// c\n1", "不认识", "注释不是 JSON");
  must_fail("/* c */1", "不认识", "块注释不是 JSON");
  {
    /* 报错里必须有**字节偏移** —— 那是「哪一行错了」的唯一线索 */
    dsh_json_doc *doc = NULL;
    const char *text = "{\"a\":1,\"b\":}";
    ok(dsh_json_parse(text, strlen(text), &doc) != 0, "缺值的成员必须失败");
    const char *msg = dsh_last_error_message();
    ok(msg != NULL && strstr(msg, "字节") != NULL, "报错里要有字节偏移");
    if (msg != NULL) dsh_release((void *)msg);
  }
  ok_eq_i64((int64_t)dsh_mem_live_count(), (int64_t)base,
            "所有失败例之后活分配表回到基线（失败路径也要还清）");

  /* ── ⑤ 嵌套深度上限：递归下降必须挡住「把栈打穿」── */
  {
    char deep[4096];
    size_t n = 0;
    for (int i = 0; i < 200 && n + 2 < sizeof(deep); i++) deep[n++] = '[';
    deep[n++] = '1';
    for (int i = 0; i < 200 && n + 2 < sizeof(deep); i++) deep[n++] = ']';
    dsh_json_doc *doc = NULL;
    ok(dsh_json_parse(deep, n, &doc) != 0, "嵌套 200 层必须被拒绝（上限 64）");
    const char *msg = dsh_last_error_message();
    ok(msg != NULL && strstr(msg, "嵌套太深") != NULL, "报错要说明是嵌套太深");
    if (msg != NULL) dsh_release((void *)msg);
  }

  /* ── ⑥ 往返：写出去再读回来必须相等（转义表两边各一份，这是唯一保护措施）── */
  {
    const char *samples[] = {
        "普通中文",
        "带\"双引号\"和\\反斜杠",
        "换行\n制表\t回车\r",
        "控制字符\x01\x02",
        "斜杠/不转义也行",
        "emoji 😀 混排",
        "",          /* 空串 */
        "尾随空格 ",
    };
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
      dsh_json *j = dsh_json_new();
      dsh_json_str(j, samples[i]);
      char *written = dsh_json_take(j);
      dsh_json_free(j);
      dsh_json_doc *doc = NULL;
      const int rc = dsh_json_parse(written, strlen(written), &doc);
      ok(rc == 0, "往返：写出去的字符串必须能被自己读回来");
      if (rc == 0) {
        const char *back = str_of(dsh_json_doc_root(doc));
        ok_eq_str(back, samples[i], "往返之后内容相等");
        dsh_json_doc_free(doc);
      }
      dsh_release(written);
    }
  }
  /* 往返：一个真实的「设置补丁」形状 */
  {
    dsh_json *j = dsh_json_new();
    dsh_json_object_begin(j);
    dsh_json_kv_str(j, "speechRate", "1.25");
    dsh_json_key(j, "text");
    dsh_json_str_len(j, "a\0b", 3);
    dsh_json_object_end(j);
    char *written = dsh_json_take(j);
    dsh_json_free(j);

    dsh_json_doc *doc = NULL;
    ok(dsh_json_parse(written, strlen(written), &doc) == 0, "往返：带内嵌 NUL 的对象");
    if (doc != NULL) {
      const dsh_json_node *root = dsh_json_doc_root(doc);
      ok_eq_str(str_of(dsh_json_object_get(root, "speechRate")), "1.25", "往返：字符串型数值");
      size_t len = 0;
      const char *t = dsh_json_str_value(dsh_json_object_get(root, "text"), &len);
      ok(len == 3 && t[0] == 'a' && t[1] == '\0' && t[2] == 'b', "往返：内嵌 NUL 原样保住");
      dsh_json_doc_free(doc);
    }
    dsh_release(written);
  }

  /* ── ⑦ 参数与 NULL 的边界（宿主可能传任何东西）── */
  {
    dsh_json_doc *doc = NULL;
    ok(dsh_json_parse(NULL, 0, &doc) != 0, "文本为 NULL 必须报错");
    ok(dsh_json_parse("1", 1, NULL) != 0, "out 为 NULL 必须报错");
    ok(dsh_json_doc_root(NULL) == NULL, "root(NULL) → NULL");
    ok(dsh_json_node_kind(NULL) == 0, "kind(NULL) → 0");
    ok(dsh_json_is_object(NULL) == 0 && dsh_json_is_string(NULL) == 0, "is_*(NULL) → 0");
    ok(dsh_json_object_get(NULL, "a") == NULL, "object_get(NULL) → NULL");
    ok(dsh_json_str_value(NULL, NULL) == NULL, "str_value(NULL) → NULL");
    dsh_json_doc_free(NULL); /* 必须是合法的空操作 */
    ok(1, "dsh_json_doc_free(NULL) 不该崩");
  }

  ok_eq_i64((int64_t)dsh_mem_live_count(), (int64_t)base,
            "全部用例跑完，活分配表必须回到基线");

  printf("json_reader：%d 项，失败 %d\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
