/* ==========================================================================
 * 标准答案文件对照测试 · C 侧诊断脚本
 *
 * 它把 C 内核（native/）对一本 .mdx 的输出，按 **与 tools/golden/golden-dump.cs
 * 完全相同的形状** 规范成 JSON 写到 stdout。两边的输出应当**逐字节相同** ——
 * 那就是"与参考实现（0.1.3 的 C# 解析器）对齐"的证据。
 *
 * 为什么 C 侧要输出字符串而不是直接比结构：
 *   · 逐字节比对是最硬的检查标准 —— 它不会因为"我实现了一个宽容的比较器"而放过差异；
 *   · 字符串形状一旦定下，双方都不许"顺手美化"（缩进、键序、转义规则都要一样）。
 *
 * ⚠️ 输出必须**确定**：
 *   · 键按词块序（不排序、不依赖 hash 遍历）；
 *   · 记录文本按原样（不 trim、不去 BOM —— 这一层只做"解出来"，不做产品清洗）；
 *   · 警告按产生顺序。
 *
 * 用法：
 *   golden-dump-c <词典.mdx> [更多.mdx ...]      # 输出到 stdout
 *   golden-dump-c --files-from <清单文件>        # 每行一个路径（Windows 路径用 / 或 \\ 都行）
 * ========================================================================== */

#include "dict/dsh_mdx.h"
#include "dsh_internal.h"
#include "dsh_lookup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── JSON 转义：与 golden-dump.cs 的 JsonEscape **逐字节等价** ────────────
 * 规则（两边必须一致）：
 *   · `"` `\` \b \f \n \r \t 用短写；
 *   · 其余 < 0x20 用 \u00xx（小写十六进制）；
 *   · 非 ASCII **原样输出**（UTF-8）；
 *   · NULL → 字面量 null。
 */
static void put_escaped(const char *s) {
  if (s == NULL) {
    fputs("null", stdout);
    return;
  }
  fputc('"', stdout);
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    switch (*p) {
      case '"': fputs("\\\"", stdout); break;
      case '\\': fputs("\\\\", stdout); break;
      case '\b': fputs("\\b", stdout); break;
      case '\f': fputs("\\f", stdout); break;
      case '\n': fputs("\\n", stdout); break;
      case '\r': fputs("\\r", stdout); break;
      case '\t': fputs("\\t", stdout); break;
      default:
        if (*p < 0x20) fprintf(stdout, "\\u%04x", (unsigned)*p);
        else fputc((char)*p, stdout);
    }
  }
  fputc('"', stdout);
}

/** 取文件的基名（两边都用"基名"，因为完整路径随机器变，不该进标准答案文件） */
static const char *base_name(const char *path) {
  const char *a = strrchr(path, '/');
  const char *b = strrchr(path, '\\');
  const char *last = (a > b) ? a : b;
  return (last == NULL) ? path : last + 1;
}

/**
 * 按**长度**写一个 JSON 字符串 —— 记录里可能有内嵌的 U+0000。
 *
 * ⚠️ 为什么不能用 `put_escaped`（它靠 `\0` 结尾判断终点）：
 * MDict 的记录每条末尾带一个 \0，而"结束位置"是下一条的偏移，
 * 所以那个 \0 落在记录**里面**。C# 把它当普通字符保留、JSON 里转义成 `\u0000`；
 * C 侧若按 `\0` 截断就会少一个字符 —— 标准答案文件对照测试第一次就是死在这里。
 *
 * 与 golden-dump.cs 的 JsonEscape 规则仍然**逐字节等价**：0x00 < 0x20，
 * 所以它走 `\u00xx` 那条分支（小写十六进制）。
 */
static void put_escaped_len(const char *s, long len) {
  if (s == NULL) {
    fputs("null", stdout);
    return;
  }
  fputc('"', stdout);
  for (long i = 0; i < len; i++) {
    const unsigned char c = (unsigned char)s[i];
    switch (c) {
      case '"': fputs("\\\"", stdout); break;
      case '\\': fputs("\\\\", stdout); break;
      case '\b': fputs("\\b", stdout); break;
      case '\f': fputs("\\f", stdout); break;
      case '\n': fputs("\\n", stdout); break;
      case '\r': fputs("\\r", stdout); break;
      case '\t': fputs("\\t", stdout); break;
      default:
        if (c < 0x20) fprintf(stdout, "\\u%04x", (unsigned)c);
        else fputc((char)c, stdout);
    }
  }
  fputc('"', stdout);
}

/** 版本号：与 C# 的 `Version.ToString("0.0")` 一致；NaN 写成 "NaN" */
static void put_version(double v) {
  if (!(v == v)) { /* NaN */
    put_escaped("NaN");
    return;
  }
  char buf[32];
  snprintf(buf, sizeof(buf), "%.1f", v);
  put_escaped(buf);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "用法: golden-dump-c <词典.mdx> [更多.mdx ...]\n");
    return 2;
  }

  fputs("{\"files\":[", stdout);
  int first_file = 1;

  for (int argi = 1; argi < argc; argi++) {
    const char *path = argv[argi];
    if (!first_file) fputc(',', stdout);
    first_file = 0;

    dsh_mdx *m = NULL;
    if (dsh_mdx_open(path, &m) != 0) {
      /* 打不开也要如实记下来 —— 对照测试时"两边都打不开"也是一种一致 */
      fputs("{\"name\":", stdout);
      put_escaped(base_name(path));
      fputs(",\"open\":false,\"reason\":", stdout);
      put_escaped(dsh_last_error_message());
      fputc('}', stdout);
      continue;
    }

    fputs("{\"name\":", stdout);
    put_escaped(base_name(path));
    fputs(",\"open\":true", stdout);

    /* ── 头信息（只放与解析有关的那几项）────────────────────────────── */
    fputs(",\"info\":{\"isMdd\":", stdout);
    fputs(dsh_mdx_is_mdd(m) ? "true" : "false", stdout);
    fputs(",\"version\":", stdout);
    put_version(dsh_mdx_version(m));
    fprintf(stdout, ",\"encrypted\":%d", dsh_mdx_encrypted(m));
    fputs(",\"encoding\":", stdout);
    put_escaped(dsh_mdx_encoding_name(m));
    /* numWidth 由版本决定：v2.0 = 8，v1.2 = 4（规则与 C# 的 NumWidth 一致） */
    fprintf(stdout, ",\"numWidth\":%d", dsh_mdx_version(m) >= 2.0 ? 8 : 4);
    fputs(",\"title\":", stdout);
    put_escaped(dsh_mdx_title(m));
    fprintf(stdout, ",\"keyCount\":%lld", (long long)dsh_mdx_key_count(m));
    fprintf(stdout, ",\"keyBlockCount\":%lld", (long long)dsh_mdx_key_block_count(m));
    fputs(",\"blockOrderMonotone\":", stdout);
    fputs(dsh_mdx_block_order_monotone(m) ? "true" : "false", stdout);
    fputc('}', stdout);

    /* ── 词块索引 ────────────────────────────────────────────────────── */
    const int64_t block_count = dsh_mdx_key_block_count(m);
    fputs(",\"keyBlocks\":[", stdout);
    for (int64_t i = 0; i < block_count; i++) {
      dsh_mdx_key_block kb;
      if (dsh_mdx_key_block_at(m, i, &kb) != 0) break;
      if (i > 0) fputc(',', stdout);
      fprintf(stdout, "{\"entryCount\":%lld", (long long)kb.entry_count);
      fputs(",\"first\":", stdout);
      put_escaped(kb.first_key);
      fputs(",\"last\":", stdout);
      put_escaped(kb.last_key);
      fprintf(stdout, ",\"packSize\":%lld", (long long)kb.pack_size);
      fprintf(stdout, ",\"unpackSize\":%lld", (long long)kb.unpack_size);
      fputc('}', stdout);
    }
    fputc(']', stdout);

    /* ── 全部键 + 记录（按词块序，不排序）────────────────────────────── */
    char **keys = NULL;
    int64_t key_count = 0;
    const int list_rc = dsh_mdx_list_keys(m, &keys, &key_count);

    fputs(",\"keys\":[", stdout);
    if (list_rc == 0) {
      for (int64_t i = 0; i < key_count; i++) {
        if (i > 0) fputc(',', stdout);
        put_escaped(keys[i]);
      }
    }
    fputc(']', stdout);
    fprintf(stdout, ",\"emittedKeys\":%lld", (long long)(list_rc == 0 ? key_count : 0));

    fputs(",\"records\":[", stdout);
    if (list_rc == 0) {
      for (int64_t i = 0; i < key_count; i++) {
        char *text = NULL;
        int64_t text_len = 0;
        if (i > 0) fputc(',', stdout);
        if (dsh_mdx_read_record(m, i, &text, &text_len) == 0) {
          put_escaped_len(text, (long)text_len);
          dsh_release(text);
        } else {
          /* 取不到就写 null —— 与 C# 那边抛异常的行为不同，所以一旦出现
           * 会立刻在比对里暴露出来（这正是我们要的：差异不许被悄悄抹平）。 */
          fputs("null", stdout);
        }
      }
    }
    fputc(']', stdout);

    /* ── 警告 ────────────────────────────────────────────────────────── */
    fputs(",\"warnings\":[", stdout);
    const int warn_count = dsh_mdx_warning_count(m);
    for (int i = 0; i < warn_count; i++) {
      if (i > 0) fputc(',', stdout);
      put_escaped(dsh_mdx_warning_at(m, i));
    }
    fputc(']', stdout);

    fputc('}', stdout);

    if (keys != NULL) dsh_mdx_free_keys(keys, key_count);
    dsh_mdx_close(m);
  }

  fputs("]}", stdout);
  fflush(stdout);
  return 0;
}
