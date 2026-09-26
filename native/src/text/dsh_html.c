/* ==========================================================================
 * HTML 文本工具（对应参考实现的 HtmlUtils）—— 参考实现用 8 条 .NET 正则，内核不许引依赖，
 * 这里每条**手写成等价的小扫描器**。见 dsh_html.h。
 * ⚠️ 两处刻意的近似：`\b` 处把「字节 ≥ 0x80」当词字符（与 .NET 在 CJK 上一致）；行首尾 trim
 * 用 `uni_ws_len` 而**不是** C 的 `isspace`（后者只认 ASCII，会把 U+3000 留在行首）。
 * ========================================================================== */

#include "text/dsh_html.h"

#include "dsh_internal.h"

#include <string.h>

/* ── 可增长的字节缓冲（内核分配，出错时如实报内存不足）───────────────────── */

typedef struct {
  char *p;
  size_t len;
  size_t cap;
} hbuf;

static int hbuf_reserve(hbuf *b, size_t extra) {
  const size_t need = b->len + extra + 1; /* +1：末尾那个 \0 */
  if (need <= b->cap) return 1;
  size_t cap = (b->cap == 0) ? 64 : b->cap;
  while (cap < need) {
    if (cap > (size_t)1 << 30) {
      dsh_set_last_error("内存不足：HTML 处理缓冲");
      return 0;
    }
    cap *= 2;
  }
  char *next = (char *)dsh_mem_alloc(cap);
  if (next == NULL) {
    dsh_set_last_error("内存不足：HTML 处理缓冲");
    return 0;
  }
  if (b->len > 0) memcpy(next, b->p, b->len);
  next[b->len] = '\0';
  if (b->p != NULL) dsh_release(b->p);
  b->p = next;
  b->cap = cap;
  return 1;
}

static int hbuf_put(hbuf *b, const char *bytes, size_t n) {
  if (n == 0) return 1;
  if (!hbuf_reserve(b, n)) return 0;
  memcpy(b->p + b->len, bytes, n);
  b->len += n;
  b->p[b->len] = '\0';
  return 1;
}

static int hbuf_putc(hbuf *b, char c) {
  if (!hbuf_reserve(b, 1)) return 0;
  b->p[b->len++] = c;
  b->p[b->len] = '\0';
  return 1;
}

static void hbuf_dispose(hbuf *b) {
  if (b->p != NULL) dsh_release(b->p);
  b->p = NULL;
  b->len = 0;
  b->cap = 0;
}

/** 收尾：把缓冲交出去当返回值（长度照实给；末尾那个 \0 不算长度） */
static char *hbuf_take(hbuf *b, size_t *out_len) {
  if (b->p == NULL) {
    if (!hbuf_reserve(b, 0)) return NULL;
  }
  if (out_len != NULL) *out_len = b->len;
  char *p = b->p;
  b->p = NULL;
  b->len = 0;
  b->cap = 0;
  return p;
}

/** 出参为 NULL 时的失败收尾（统一写 last_error 的写法） */
static char *fail(hbuf *b, const char *what) {
  hbuf_dispose(b);
  dsh_set_last_error("内存不足：%s", what);
  return NULL;
}

/* ── 小工具 ─────────────────────────────────────────────────────────────── */

static int is_alpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }

static int is_hex(char c) {
  return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int hex_value(char c) {
  if (is_digit(c)) return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return c - 'A' + 10;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

/** 前 n 个字节是否与 ASCII 字面量 `lit` 相等（大小写不敏感） */
static int starts_with_ci(const char *p, size_t avail, const char *lit, size_t n) {
  if (avail < n) return 0;
  for (size_t i = 0; i < n; i++) {
    if (lower(p[i]) != lit[i]) return 0;
  }
  return 1;
}

/**
 * 这一个字节算不算「词字符」（.NET 的 `\w` 那一侧）。
 *
 * ⚠️ 近似：UTF-8 里 ≥ 0x80 的字节一律当成词字符 —— 于是 `<script中文>` 不认为标签名后有边界，
 *    与 .NET 在 CJK 上的行为一致；真正的 Unicode 字母表不值得为这一处引进一整个分类表。
 */
static int is_word_byte(unsigned char c) {
  return (c < 0x80) ? (is_alpha((char)c) || is_digit((char)c) || c == '_') : 1;
}

/** 这一处是不是 Unicode 空白（.NET 的 `char.IsWhiteSpace`），返回它占几个字节（0 = 不是空白）。
 *  最后那两次 `Trim()` 用的是它，**不是** C 的 `isspace`。 */
static size_t uni_ws_len(const char *p, size_t avail) {
  if (avail == 0) return 0;
  const unsigned char c = (unsigned char)p[0];
  if (c == 0x20 || (c >= 0x09 && c <= 0x0D)) return 1; /* 空格 / \t \n \v \f \r */
  if (avail >= 2 && c == 0xC2 && (unsigned char)p[1] == 0x85) return 2; /* U+0085 */
  if (avail >= 2 && c == 0xC2 && (unsigned char)p[1] == 0xA0) return 2; /* U+00A0 */
  if (avail >= 3 && c == 0xE1 && (unsigned char)p[1] == 0x9A &&
      (unsigned char)p[2] == 0x80) return 3; /* U+1680 */
  if (avail >= 3 && c == 0xE2 && (unsigned char)p[1] == 0x80) {
    const unsigned char d = (unsigned char)p[2];
    if (d >= 0x80 && d <= 0x8A) return 3; /* U+2000 – U+200A */
    if (d == 0xA8 || d == 0xA9 || d == 0xAF) return 3; /* U+2028 / U+2029 / U+202F */
  }
  if (avail >= 3 && c == 0xE2 && (unsigned char)p[1] == 0x81 &&
      (unsigned char)p[2] == 0x9F) return 3; /* U+205F */
  if (avail >= 3 && c == 0xE3 && (unsigned char)p[1] == 0x80 &&
      (unsigned char)p[2] == 0x80) return 3; /* U+3000 表意空格 */
  return 0;
}

/** 把一个码位写成 UTF-8；返回字节数（调用方保证 4 字节空间） */
static size_t utf8_encode(unsigned int cp, char *out) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

/* ── 实体还原 ───────────────────────────────────────────────────────────── */

/** 具名实体表（与参考实现的 15 条逐条对应，查表**大小写不敏感**） */
typedef struct {
  const char *name;
  const char *value; /* UTF-8 */
} named_entity;

static const named_entity NAMED_ENTITIES[] = {
    {"amp", "&"},   {"lt", "<"},      {"gt", ">"},      {"quot", "\""},
    {"apos", "'"},  {"nbsp", "\xC2\xA0"}, {"copy", "\xC2\xA9"}, {"reg", "\xC2\xAE"},
    {"hellip", "\xE2\x80\xA6"}, {"mdash", "\xE2\x80\x94"}, {"ndash", "\xE2\x80\x93"},
    {"ldquo", "\xE2\x80\x9C"},  {"rdquo", "\xE2\x80\x9D"},
    {"lsquo", "\xE2\x80\x98"},  {"rsquo", "\xE2\x80\x99"},
};

/** `&…;` 在 `p`（avail 字节）处能不能还原；能就写出结果并返回**吃掉的字节数**，否则 0 */
static size_t try_entity(const char *p, size_t avail, hbuf *out, int *ok) {
  *ok = 1;
  if (avail < 3 || p[0] != '&') return 0;
  size_t i = 1;
  if (p[i] == '#') {
    /* `#` 后面那个 `x` 是**可选的**，而且**只认小写**：参考实现那条正则没有 IgnoreCase，
     * 所以 `&#X41;` **不还原**、原样留着。 */
    size_t digits = i + 1;
    int hex_mode = 0;
    if (digits < avail && p[digits] == 'x' && digits + 1 < avail && is_hex(p[digits + 1])) {
      hex_mode = 1;
      digits++;
    }
    if (digits >= avail || !is_hex(p[digits])) return 0;
    size_t k = digits;
    while (k < avail && is_hex(p[k])) k++;
    if (k >= avail || p[k] != ';') return 0;
    /* 参考实现走的是 `int.TryParse`：装不进 int32 就**原样留着** */
    unsigned long long value = 0;
    int overflow = 0;
    for (size_t t = digits; t < k; t++) {
      if (!hex_mode && !is_digit(p[t])) return 0; /* `&#4f;` 的十进制那一档解析不了 */
      value = value * (hex_mode ? 16u : 10u) + (unsigned long long)(hex_mode ? hex_value(p[t])
                                                                             : p[t] - '0');
      if (value > 0x7FFFFFFFull) {
        overflow = 1;
        break;
      }
    }
    if (overflow || value > 0x10FFFFull) return 0;
    /* ⚠️ 孤立代理项：参考实现在这里**抛异常**；C 版如实保留原文（内核绝不抛） */
    if (value >= 0xD800ull && value <= 0xDFFFull) return 0;
    char buf[4];
    const size_t n = utf8_encode((unsigned int)value, buf);
    if (!hbuf_put(out, buf, n)) {
      *ok = 0;
      return 0;
    }
    return k + 1;
  }
  if (!is_alpha(p[i])) return 0;
  size_t k = i;
  while (k < avail && is_alpha(p[k])) k++;
  if (k >= avail || p[k] != ';') return 0;
  const size_t name_len = k - i;
  for (size_t t = 0; t < sizeof(NAMED_ENTITIES) / sizeof(NAMED_ENTITIES[0]); t++) {
    const named_entity *e = &NAMED_ENTITIES[t];
    if (strlen(e->name) != name_len) continue;
    if (!starts_with_ci(p + i, name_len, e->name, name_len)) continue;
    if (!hbuf_put(out, e->value, strlen(e->value))) {
      *ok = 0;
      return 0;
    }
    return k + 1;
  }
  return 0; /* 认不出来的具名实体：原样留着（与参考实现一致） */
}

char *dsh_html_decode_entities(const char *text, size_t len, size_t *out_len) {
  if (out_len != NULL) *out_len = 0;
  if (text == NULL) {
    if (out_len != NULL) *out_len = 0;
    char *empty = (char *)dsh_mem_alloc(1);
    if (empty == NULL) return fail(&(hbuf){0}, "实体还原");
    empty[0] = '\0';
    return empty;
  }
  hbuf out = {0};
  size_t i = 0;
  while (i < len) {
    if (text[i] == '&') {
      int ok = 1;
      const size_t taken = try_entity(text + i, len - i, &out, &ok);
      if (!ok) return fail(&out, "实体还原");
      if (taken > 0) {
        i += taken;
        continue;
      }
    }
    if (!hbuf_putc(&out, text[i])) return fail(&out, "实体还原");
    i++;
  }
  return hbuf_take(&out, out_len);
}

/* ── 转义 ───────────────────────────────────────────────────────────────── */

char *dsh_html_escape(const char *text, size_t len, size_t *out_len) {
  if (out_len != NULL) *out_len = 0;
  if (text == NULL) len = 0;
  hbuf out = {0};
  for (size_t i = 0; i < len; i++) {
    const char *rep = NULL;
    switch (text[i]) {
      case '&': rep = "&amp;"; break;
      case '<': rep = "&lt;"; break;
      case '>': rep = "&gt;"; break;
      case '"': rep = "&quot;"; break;
      case '\'': rep = "&#39;"; break;
      default: break;
    }
    if (rep != NULL) {
      if (!hbuf_put(&out, rep, strlen(rep))) return fail(&out, "HTML 转义");
    } else if (!hbuf_putc(&out, text[i])) {
      return fail(&out, "HTML 转义");
    }
  }
  return hbuf_take(&out, out_len);
}

char *dsh_html_escape_for_inline_script(const char *text, size_t len, size_t *out_len) {
  if (out_len != NULL) *out_len = 0;
  if (text == NULL) len = 0;
  hbuf out = {0};
  size_t i = 0;
  while (i < len) {
    /* `</(script)` 大小写不敏感、**不要求**后面跟着 `>`；替换成 `<\/$1`，而 `$1` 是**原文里的大小写** */
    if (text[i] == '<' && i + 1 < len && text[i + 1] == '/' &&
        starts_with_ci(text + i + 2, len - i - 2, "script", 6)) {
      if (!hbuf_put(&out, "<\\/", 3)) return fail(&out, "内联脚本转义");
      if (!hbuf_put(&out, text + i + 2, 6)) return fail(&out, "内联脚本转义");
      i += 8;
      continue;
    }
    if (!hbuf_putc(&out, text[i])) return fail(&out, "内联脚本转义");
    i++;
  }
  return hbuf_take(&out, out_len);
}

/* ── 去标签 ─────────────────────────────────────────────────────────────── */

/** 块级结束标签（`</p>` / `</div>` / …）—— 到 `p` 处是不是其中之一，是就返回整段长度 */
static size_t block_end_len(const char *p, size_t avail) {
  if (avail < 4 || p[0] != '<' || p[1] != '/') return 0;
  static const char *const NAMES[] = {"p", "div", "li", "tr", "h1", "h2", "h3",
                                      "h4", "h5", "h6", "section", "article"};
  for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
    const size_t n = strlen(NAMES[i]);
    if (!starts_with_ci(p + 2, avail - 2, NAMES[i], n)) continue;
    if (avail < n + 3) continue;
    if (p[2 + n] == '>') return n + 3;
  }
  return 0;
}

/** `<br>` / `<br/>` / `<br />` —— 是就返回整段长度 */
static size_t br_len(const char *p, size_t avail) {
  if (avail < 4 || p[0] != '<') return 0;
  if (!starts_with_ci(p + 1, avail - 1, "br", 2)) return 0;
  size_t k = 3;
  while (k < avail && (p[k] == ' ' || p[k] == '\t' || p[k] == '\n' || p[k] == '\r' ||
                       p[k] == '\f' || p[k] == '\v')) {
    k++;
  }
  if (k < avail && p[k] == '/') k++;
  if (k < avail && p[k] == '>') return k + 1;
  return 0;
}

/** 第一处 `</name>`（大小写不敏感）的位置；找不到返回 (size_t)-1 */
static size_t find_close_tag(const char *p, size_t avail, size_t from, const char *name,
                             size_t name_len) {
  for (size_t i = from; i + name_len + 3 <= avail; i++) {
    if (p[i] != '<' || p[i + 1] != '/') continue;
    if (!starts_with_ci(p + i + 2, name_len, name, name_len)) continue;
    if (p[i + 2 + name_len] == '>') return i;
  }
  return (size_t)-1;
}

/**
 * 删掉 `<script …>…</script>` / `<style …>…</style>` 整段。
 *
 * ⚠️ 与那条正则对齐：标签名后面要有 `\b`（词边界）、`[^>]*`（不含 `>`）；
 *    结尾找**第一处** `</同名>`，**找不到结尾标签就不替换**。
 */
static char *strip_script_style(const char *src, size_t len, size_t *out_len) {
  hbuf out = {0};
  size_t i = 0;
  while (i < len) {
    if (src[i] == '<') {
      static const char *const NAMES[] = {"script", "style"};
      int matched = 0;
      for (size_t t = 0; t < 2 && !matched; t++) {
        const size_t n = strlen(NAMES[t]);
        if (!starts_with_ci(src + i + 1, len - i - 1, NAMES[t], n)) continue;
        /* `\b`：标签名后面那个字节不许是词字符 */
        if (i + 1 + n < len && is_word_byte((unsigned char)src[i + 1 + n])) continue;
        size_t k = i + 1 + n;
        while (k < len && src[k] != '>') k++;
        if (k >= len) continue; /* 没有 `>`：[^>]*> 匹配不上 */
        const size_t close = find_close_tag(src, len, k + 1, NAMES[t], n);
        if (close == (size_t)-1) continue;
        i = close + n + 3; /* 跳过 `</name>` */
        matched = 1;
      }
      if (matched) continue;
    }
    if (!hbuf_putc(&out, src[i])) return fail(&out, "去掉脚本与样式");
    i++;
  }
  return hbuf_take(&out, out_len);
}

/** 按「空白单元」收敛（`[ \t\u00a0]+` → 一个空格；U+00A0 是两个字节） */
static char *collapse_spaces(const char *src, size_t len, size_t *out_len) {
  hbuf out = {0};
  size_t i = 0;
  while (i < len) {
    const unsigned char c = (unsigned char)src[i];
    const size_t nb = (c == 0xC2 && i + 1 < len && (unsigned char)src[i + 1] == 0xA0) ? 2 : 0;
    if (c == ' ' || c == '\t' || nb == 2) {
      i += (nb == 2) ? 2 : 1;
      while (i < len) {
        const unsigned char d = (unsigned char)src[i];
        if (d == ' ' || d == '\t') {
          i++;
        } else if (d == 0xC2 && i + 1 < len && (unsigned char)src[i + 1] == 0xA0) {
          i += 2;
        } else {
          break;
        }
      }
      if (!hbuf_putc(&out, ' ')) return fail(&out, "收敛空白");
      continue;
    }
    if (!hbuf_putc(&out, src[i])) return fail(&out, "收敛空白");
    i++;
  }
  return hbuf_take(&out, out_len);
}

/** `\n{3,}` → `\n\n` */
static char *collapse_newlines(const char *src, size_t len, size_t *out_len) {
  hbuf out = {0};
  size_t i = 0;
  while (i < len) {
    if (src[i] == '\n') {
      size_t k = i;
      while (k < len && src[k] == '\n') k++;
      /* ⚠️ 一次一个 `\n` 地写 —— 别拿 `hbuf_put` 去写一个只含 `\n` 的字面量再加长度 2：
       *    那个字面量只有 2 字节，第 2 个字节是**字符串的结尾符**，输出里会多出一个 NUL。 */
      const size_t want = (k - i >= 3) ? 2 : (k - i);
      for (size_t t = 0; t < want; t++) {
        if (!hbuf_putc(&out, '\n')) return fail(&out, "收敛空行");
      }
      i = k;
      continue;
    }
    if (!hbuf_putc(&out, src[i])) return fail(&out, "收敛空行");
    i++;
  }
  return hbuf_take(&out, out_len);
}

/**
 * 把 `[b, e)` 的头尾空白（Unicode 空白）收掉。
 *
 * ⚠️ 尾部不能「逐字节往前退着试」：那会把多字节字符的**续字节**当成尾空白切掉、切出半个
 *    UTF-8 序列。所以改成**正着扫**：记住「最后一个非空白字符的结束位置」当收尾后的 `e`。
 */
static void trim_range(const char *p, size_t *b, size_t *e) {
  while (*b < *e) {
    const size_t n = uni_ws_len(p + *b, *e - *b);
    if (n == 0) break;
    *b += n;
  }
  size_t last_end = *b;
  size_t t = *b;
  while (t < *e) {
    const size_t n = uni_ws_len(p + t, *e - t);
    if (n == 0) {
      t++;
      last_end = t;
    } else {
      t += n;
    }
  }
  *e = last_end;
}

/** 逐行 trim（Unicode 空白）后用 `\n` 拼回去，最后整体 trim 一次 */
static char *trim_lines(const char *src, size_t len, size_t *out_len) {
  hbuf out = {0};
  size_t i = 0;
  int first_line = 1;
  for (;;) {
    size_t end = i;
    while (end < len && src[end] != '\n') end++;
    size_t b = i;
    size_t e = end;
    trim_range(src, &b, &e);
    if (!first_line) {
      if (!hbuf_putc(&out, '\n')) return fail(&out, "逐行 trim");
    }
    first_line = 0;
    if (!hbuf_put(&out, src + b, e - b)) return fail(&out, "逐行 trim");
    if (end >= len) break;
    i = end + 1;
  }
  /* 整体 trim（.NET 的 `builder.ToString().Trim()`） */
  if (out.p == NULL) return hbuf_take(&out, out_len);
  size_t b = 0;
  size_t e = out.len;
  trim_range(out.p, &b, &e);
  hbuf result = {0};
  if (!hbuf_put(&result, out.p + b, e - b)) {
    hbuf_dispose(&out);
    return fail(&result, "逐行 trim");
  }
  hbuf_dispose(&out);
  return hbuf_take(&result, out_len);
}

/* ── 主流程 ─────────────────────────────────────────────────────────────── */

char *dsh_html_strip(const char *html, size_t len, size_t *out_len) {
  if (out_len != NULL) *out_len = 0;
  if (html == NULL) len = 0;
  if (len == 0) {
    char *empty = (char *)dsh_mem_alloc(1);
    if (empty == NULL) return fail(&(hbuf){0}, "去 HTML");
    empty[0] = '\0';
    return empty;
  }

  size_t n1 = 0;
  char *step = strip_script_style(html, len, &n1);
  if (step == NULL) return NULL;

  /* `<br…>` → 换行、块级结束标签 → 换行、其余标签删掉 —— 一趟走完 */
  hbuf flat = {0};
  size_t i = 0;
  while (i < n1) {
    if (step[i] == '<') {
      const size_t br = br_len(step + i, n1 - i);
      if (br > 0) {
        if (!hbuf_putc(&flat, '\n')) {
          dsh_release(step);
          return fail(&flat, "去 HTML");
        }
        i += br;
        continue;
      }
      const size_t blk = block_end_len(step + i, n1 - i);
      if (blk > 0) {
        if (!hbuf_putc(&flat, '\n')) {
          dsh_release(step);
          return fail(&flat, "去 HTML");
        }
        i += blk;
        continue;
      }
      size_t k = i + 1;
      while (k < n1 && step[k] != '>') k++;
      if (k < n1) {
        i = k + 1; /* `<[^>]+>`：连同那个 `>` 一起删掉 */
        continue;
      }
      /* 没有闭合的 `>`：`<[^>]+>` 匹配不上，这个 `<` 原样留下 */
    }
    if (!hbuf_putc(&flat, step[i])) {
      dsh_release(step);
      return fail(&flat, "去 HTML");
    }
    i++;
  }
  dsh_release(step);

  /* 实体还原 —— 在去标签**之后**（所以 `&lt;b&gt;` 变回文字而不再被当标签） */
  size_t n2 = 0;
  char *decoded = dsh_html_decode_entities(flat.p != NULL ? flat.p : "", flat.len, &n2);
  hbuf_dispose(&flat);
  if (decoded == NULL) return NULL;

  /* `\r\n` / `\r` → `\n` */
  hbuf lf = {0};
  for (size_t k = 0; k < n2; k++) {
    if (decoded[k] == '\r') {
      if (k + 1 < n2 && decoded[k + 1] == '\n') continue; /* 让后面的 \n 自己进 */
      if (!hbuf_putc(&lf, '\n')) {
        dsh_release(decoded);
        return fail(&lf, "去 HTML");
      }
      continue;
    }
    if (!hbuf_putc(&lf, decoded[k])) {
      dsh_release(decoded);
      return fail(&lf, "去 HTML");
    }
  }
  dsh_release(decoded);

  size_t n3 = 0;
  char *spaces = collapse_spaces(lf.p != NULL ? lf.p : "", lf.len, &n3);
  hbuf_dispose(&lf);
  if (spaces == NULL) return NULL;

  size_t n4 = 0;
  char *blanks = collapse_newlines(spaces, n3, &n4);
  dsh_release(spaces);
  if (blanks == NULL) return NULL;

  char *result = trim_lines(blanks, n4, out_len);
  dsh_release(blanks);
  return result;
}
