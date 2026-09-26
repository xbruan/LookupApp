/* 词典取样 · 纯策略层 —— 约定与为什么见 `dsh_dictsample.h` 顶上那段。 */

#include "audio/dsh_dictsample.h"

#include "dsh_internal.h"
#include "text/dsh_language.h"

#include <string.h>

/* ── ① 均匀撒点：按**整本书的词条序号**分 max_scan 段 ────────────────────── */

/** 序号落在哪一块：每块首条的序号是升序的，直接二分 */
static int64_t locate_block(const int64_t *offsets, int64_t block_count, int64_t ordinal) {
  int64_t low = 0;
  int64_t high = block_count - 1;
  int64_t found = 0;
  while (low <= high) {
    int64_t mid = low + (high - low) / 2;
    if (offsets[mid] <= ordinal) {
      found = mid;
      low = mid + 1;
    } else {
      high = mid - 1;
    }
  }
  return found;
}

int64_t dsh_dictsample_slots(const int64_t *block_entry_counts, int64_t block_count,
                             int64_t max_scan, dsh_dictsample_slot *out) {
  int64_t *offsets;
  int64_t total = 0;
  int64_t steps;
  int64_t i;

  if (max_scan <= 0 || block_entry_counts == NULL || block_count <= 0 || out == NULL) return 0;

  /* 累计偏移：第 i 块的第一条词条在全书里的序号 */
  offsets = (int64_t *)dsh_mem_alloc((size_t)block_count * sizeof(int64_t));
  if (offsets == NULL) return 0;
  for (i = 0; i < block_count; i++) {
    offsets[i] = total;
    if (block_entry_counts[i] > 0) total += block_entry_counts[i];
  }
  if (total <= 0) {
    dsh_release(offsets);
    return 0;
  }

  /* 全书没那么多词条时就一条一个，别空转 */
  steps = (max_scan < total) ? max_scan : total;
  for (i = 0; i < steps; i++) {
    int64_t ordinal = i * total / steps;
    int64_t block = locate_block(offsets, block_count, ordinal);
    int64_t entry = ordinal - offsets[block];
    if (entry >= block_entry_counts[block]) entry = block_entry_counts[block] - 1;
    if (entry < 0) entry = 0;
    out[i].block_index = block;
    out[i].entry_index = entry;
  }
  dsh_release(offsets);
  return steps;
}

/* ── ② 键名清洗与粗筛 ────────────────────────────────────────────────────── */

/** 一个 UTF-8 码点的字节数（坏字节按 1 走 —— 这里只用来切分，不做校验）*/
static size_t utf8_step(const char *s) {
  unsigned char c = (unsigned char)s[0];
  if (c < 0x80) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;
}

/** ASCII 空白 */
static int is_ascii_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

/**
 * 这个码点算不算**空白**：ASCII 那六个 + 两个常见的 Unicode 空格
 * （U+00A0 不换行空格 = `C2 A0`、U+3000 全角空格 = `E3 80 80`）。
 *
 * 为什么只认这几个（而不是查一张完整表）：检查标准只服务一件事 —— 「这个名字里有没有
 * 空格」（带空格的键多半是词组、不是词目发音那一条）。漏掉几个冷门空格只是「多收一条
 * 候选」，代价很小。
 */
static int is_space_at(const char *s, size_t step) {
  if (step == 1) return is_ascii_space(s[0]);
  if (step == 2) return (unsigned char)s[0] == 0xC2 && (unsigned char)s[1] == 0xA0;
  if (step == 3) {
    return (unsigned char)s[0] == 0xE3 && (unsigned char)s[1] == 0x80 && (unsigned char)s[2] == 0x80;
  }
  return 0;
}

/** 一个 UTF-8 码点的值（坏字节按单字节走 —— 这里只用来判区间，不做校验）*/
static uint32_t utf8_cp(const char *s, size_t step) {
  unsigned char c = (unsigned char)s[0];
  if (step == 1) return c;
  if (step == 2) return ((uint32_t)(c & 0x1Fu) << 6) | ((uint32_t)((unsigned char)s[1] & 0x3Fu));
  if (step == 3) {
    return ((uint32_t)(c & 0x0Fu) << 12) | ((uint32_t)((unsigned char)s[1] & 0x3Fu) << 6) |
           ((uint32_t)((unsigned char)s[2] & 0x3Fu));
  }
  return ((uint32_t)(c & 0x07u) << 18) | ((uint32_t)((unsigned char)s[1] & 0x3Fu) << 12) |
         ((uint32_t)((unsigned char)s[2] & 0x3Fu) << 6) |
         ((uint32_t)((unsigned char)s[3] & 0x3Fu));
}

/**
 * 这个码点算不算**字母**。
 *
 * ⚠️ **不能直接拿内核那张字形分区表当「是不是字母」用**：那张表把 `≤ U+024F` 一律算
 *    `latin`（它是给「判语种」用的粗分，数字与标点在里面也算 latin），于是 `123` / `-abc`
 *    都会被当成「字母开头」。所以 ASCII 显式按 `A-Z a-z` 判，其余的先用分区表认那些
 *    **排他**的书写系统（汉字 / 假名 / 谚文 / 西里尔 …），再按码点区间收拉丁字母。
 */
static int is_letter_at(const char *s, size_t step) {
  uint32_t cp = utf8_cp(s, step);
  dsh_script_counts counts;
  if (cp < 0x80) {
    return ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) ? 1 : 0;
  }
  memset(&counts, 0, sizeof(counts));
  dsh_script_classify(s, step, &counts);
  if (counts.han > 0 || counts.kana > 0 || counts.hangul > 0 || counts.cyrillic > 0 ||
      counts.greek > 0 || counts.arabic > 0 || counts.hebrew > 0 || counts.thai > 0 ||
      counts.devanagari > 0 || counts.bengali > 0 || counts.tamil > 0) {
    return 1;
  }
  if (cp >= 0x00C0u && cp <= 0x024Fu) return 1; /* 拉丁字母（含变音）*/
  if (cp >= 0x1E00u && cp <= 0x1EFFu) return 1;
  return 0;
}

int dsh_dictsample_looks_like_headword(const char *text) {
  const char *p;
  const char *stop;
  int64_t chars = 0;
  int first = 1;
  int has_letter = 0;

  if (text == NULL) return 0;

  /*
   * 首尾空白去掉。⚠️ 结尾那个 `\0` 不用管：解析器给的键已经不含它
   * （`dsh_mdx_block_keys` 是按长度解码到 `\0` 之前），这里只处理空白。
   */
  p = text;
  while (*p != '\0' && is_ascii_space(*p)) p++;
  stop = p + strlen(p);
  while (stop > p && is_ascii_space(stop[-1])) stop--;
  if (stop <= p) return 0;

  while (p < stop) {
    size_t step = utf8_step(p);
    if ((size_t)(stop - p) < step) step = (size_t)(stop - p);

    /* `@@@LINK=`、资源路径、命名空间那些不是词条名 */
    if (step == 1) {
      char c = *p;
      if (c == '@' || c == '\\' || c == '/' || c == ':' || c == '<' || c == '>' || c == '"') {
        return 0;
      }
    }
    if (is_space_at(p, step)) return 0;
    if (is_letter_at(p, step)) {
      has_letter = 1;
      if (first) first = 0; /* 首字符是字母：允许继续判后面 */
    } else if (first) {
      return 0; /* **首字符不是字母** → 不是词条名 */
    }
    chars++;
    if (chars > 24) return 0;
    p += step;
  }
  return (chars >= 2 && has_letter) ? 1 : 0;
}

int dsh_dictsample_clean_key(const char *src, char *out, size_t cap) {
  const char *p;
  const char *stop;
  size_t n = 0;

  if (src == NULL || out == NULL || cap == 0) return -1;
  out[0] = '\0';

  p = src;
  while (*p != '\0' && is_ascii_space(*p)) p++;
  stop = p + strlen(p);
  while (stop > p && is_ascii_space(stop[-1])) stop--;

  for (; p < stop; p++) {
    if (n + 1 >= cap) return -1;
    out[n++] = *p;
  }
  out[n] = '\0';
  return 0;
}
