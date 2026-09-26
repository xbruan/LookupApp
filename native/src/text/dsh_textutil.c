/* ==========================================================================
 * 文本小工具 —— 音节分隔点、字形、词数。
 *
 * **音节分隔点**：有些词典把词头显示成 `dic·tion·ar·y`，于是「用户看到的写法」与「词典里
 * 的键」不是同一个字面 —— 直接拿它去查会**查不到**（比的是字面）。
 *
 * ⚠️ 但**内核不许替调用方改语义**：`ResolveKey` 永远只认原样文本，所以顺序必须是
 *    「**先拿原样问一次，问不到才用去掉点的写法重问**」。这个函数只做「去掉」这一件事。
 *
 * **按词计数**：约定是 0.2.0 自己定的，见 `dsh_textutil.h`：
 *    **词数 = 被空白分隔开的、非空的段数**（空白按 Unicode 空白算，含全角空格 U+3000）。
 *    对中文这种不用空格分词的语言，这个数是「段数」而不是语言学意义上的词数。
 * ========================================================================== */

#include "text/dsh_textutil.h"
#include "dsh_internal.h"

#include <string.h>

/* ── UTF-8 解码（约定见 dsh_textutil.h）──────────────────────────────────── */
uint32_t dsh_text_next_cp(const char *s, size_t len, size_t *i) {
  const uint8_t *p = (const uint8_t *)s;
  const uint8_t b = p[*i];
  size_t need = 1;
  uint32_t cp = 0xFFFDu;
  if (b < 0x80u) {
    (*i)++;
    return b;
  } else if ((b & 0xE0u) == 0xC0u) { cp = b & 0x1Fu; need = 2; }
  else if ((b & 0xF0u) == 0xE0u) { cp = b & 0x0Fu; need = 3; }
  else if ((b & 0xF8u) == 0xF0u) { cp = b & 0x07u; need = 4; }
  else { (*i)++; return 0xFFFDu; }
  if (*i + need > len) { (*i)++; return 0xFFFDu; }
  for (size_t k = 1; k < need; k++) {
    const uint8_t c = p[*i + k];
    if ((c & 0xC0u) != 0x80u) { (*i)++; return 0xFFFDu; }
    cp = (cp << 6) | (uint32_t)(c & 0x3Fu);
  }
  *i += need;
  return cp;
}

int dsh_text_is_separator_dot(uint32_t cp) {
  /* ⚠️ **不许在这里再抄一份码点表**：清单的**唯一来源是接口定义**
   * （`abi/lookup.abi.json` 的 `separators` → `DSH_SEPARATOR_CODEPOINTS[]`）。
   * 手写那四个码点与接口定义表等价，但将来往接口定义里加一个码点，这里会**不报错地不跟**。 */
  for (size_t i = 0; i < DSH_SEPARATOR_COUNT; i++) {
    if (cp == DSH_SEPARATOR_CODEPOINTS[i]) return 1;
  }
  return 0;
}

int dsh_text_has_separator_dots(const char *text) {
  if (text == NULL) return 0;
  const size_t len = strlen(text);
  size_t i = 0;
  while (i < len) {
    if (dsh_text_is_separator_dot(dsh_text_next_cp(text, len, &i))) return 1;
  }
  return 0;
}

char *dsh_text_strip_separator_dots(const char *text) {
  if (text == NULL) {
    dsh_set_last_error("去掉分隔点时收到空指针");
    return NULL;
  }
  const size_t len = strlen(text);
  size_t i = 0;
  size_t pos = 0;
  /* 原地过滤：输出**不会比输入长**，拷进自己的缓冲最省事，而且天然保序。 */
  char *out = (char *)dsh_mem_alloc(len + 1);
  if (out == NULL) {
    dsh_set_last_error("内存不足：去掉分隔点需要 %zu 字节", len + 1);
    return NULL;
  }
  while (i < len) {
    const size_t at = i;
    const uint32_t cp = dsh_text_next_cp(text, len, &i);
    if (dsh_text_is_separator_dot(cp)) continue;
    const size_t width = i - at;
    memcpy(out + pos, text + at, width);
    pos += width;
  }
  out[pos] = '\0';
  return out;
}

int64_t dsh_text_count_words(const char *text) {
  if (text == NULL) return 0;
  const size_t len = strlen(text);
  size_t i = 0;
  int64_t words = 0;
  int in_word = 0;
  while (i < len) {
    const uint32_t cp = dsh_text_next_cp(text, len, &i);
    const int space = (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0x3000u ||
                       cp == 0x00A0u || cp == 0x2000u || cp == 0x2001u || cp == 0x2002u ||
                       cp == 0x2003u || cp == 0x2009u || cp == 0x3000u);
    if (space) {
      in_word = 0;
      continue;
    }
    if (!in_word) {
      words++;
      in_word = 1;
    }
  }
  return words;
}

/**
 * 按词计数（**中日韩逐字算一个词**）。
 *
 * ⚠️ **「什么算中日韩」在这里一个字都不判**：走 `dsh_script_classify`（汉字区间只有
 *    `text/dsh_language.c` 那一处来源）。
 *
 * 与 `dsh_text_count_words` 的差别只有一处：一个**段**里只要出现中日韩字符，这个段就按
 * **中日韩字符的个数**算（`apple苹果` = 2，不是 1 + 2）。
 */
int64_t dsh_text_count_words_cjk_aware(const char *text) {
  if (text == NULL) return 0;
  const size_t len = strlen(text);
  int64_t words = 0;
  size_t at = 0; /* 当前这一段的起点 */
  while (at <= len) {
    /* 先切出一段（到下一个空白或结尾为止）*/
    size_t i = at;
    while (i < len) {
      size_t probe = i;
      const uint32_t cp = dsh_text_next_cp(text, len, &probe);
      const int space = (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0x3000u ||
                         cp == 0x00A0u || cp == 0x2000u || cp == 0x2001u || cp == 0x2002u ||
                         cp == 0x2003u || cp == 0x2009u || cp == 0x3000u);
      if (space) break;
      i = probe;
    }
    if (i > at) {
      dsh_script_counts counts;
      memset(&counts, 0, sizeof(counts));
      dsh_script_classify(text + at, i - at, &counts);
      const int64_t cjk = (int64_t)counts.han + counts.kana + counts.hangul;
      words += (cjk > 0) ? cjk : 1;
    }
    if (i >= len) break;
    at = i;
    /* 跳过这一段之后的所有空白 */
    while (at < len) {
      size_t probe = at;
      const uint32_t cp = dsh_text_next_cp(text, len, &probe);
      const int space = (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0x3000u ||
                         cp == 0x00A0u || cp == 0x2000u || cp == 0x2001u || cp == 0x2002u ||
                         cp == 0x2003u || cp == 0x2009u || cp == 0x3000u);
      if (!space) break;
      at = probe;
    }
  }
  return words;
}

int dsh_text_is_single_char(const char *text) {  if (text == NULL) return 0;
  const size_t len = strlen(text);
  size_t i = 0;
  if (len == 0) return 0;
  (void)dsh_text_next_cp(text, len, &i); /* 第一个字符 */
  return (i == len) ? 1 : 0;
}

void dsh_text_analyze_plain(const char *text, dsh_text_analysis *out) {
  if (out == NULL) return;
  memset(out, 0, sizeof(*out));
  const char *s = (text != NULL) ? text : "";
  dsh_script_classify(s, strlen(s), &out->scripts);
  out->script = dsh_script_dominant(&out->scripts);
  out->has_separator_dots = dsh_text_has_separator_dots(s);
  out->word_count = dsh_text_count_words(s);
  out->is_single_char = dsh_text_is_single_char(s);
}

/* ── 两个小工具：原来各有两份**逐字相同**的副本，收到这里，谁都不许再抄 ────────
 * ⚠️ 内存不足**只回 NULL、不设 last_error**：三处调用方各有自己的人话
 * （「转义词条名」 / 「资源键名」 / 「音频键」），改由它们自己设。 */

char *dsh_text_uri_escape(const char *text) {
  static const char HEX[] = "0123456789ABCDEF";
  if (text == NULL) text = "";
  const size_t n = strlen(text);
  char *out = (char *)dsh_mem_alloc(n * 3 + 1);
  if (out == NULL) return NULL;
  size_t at = 0;
  for (size_t i = 0; i < n; i++) {
    const unsigned char c = (unsigned char)text[i];
    const int unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                           c == '~';
    if (unreserved) {
      out[at++] = (char)c;
    } else {
      out[at++] = '%';
      out[at++] = HEX[c >> 4];
      out[at++] = HEX[c & 0x0F];
    }
  }
  out[at] = '\0';
  return out;
}

char *dsh_text_percent_decode(const char *begin, size_t len) {
  if (begin == NULL) return NULL;
  char *out = (char *)dsh_mem_alloc(len + 1);
  if (out == NULL) return NULL;
  size_t at = 0;
  for (size_t i = 0; i < len; i++) {
    if (begin[i] == '%' && i + 2 < len) {
      const int hi = begin[i + 1];
      const int lo = begin[i + 2];
      const int h = (hi >= '0' && hi <= '9')   ? hi - '0'
                    : (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10
                    : (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10
                                               : -1;
      const int l = (lo >= '0' && lo <= '9')   ? lo - '0'
                    : (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10
                    : (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10
                                               : -1;
      if (h >= 0 && l >= 0) {
        out[at++] = (char)((h << 4) | l);
        i += 2;
        continue;
      }
    }
    out[at++] = begin[i];
  }
  out[at] = '\0';
  return out;
}