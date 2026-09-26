/* JSON 写入器的实现。约定与理由见 json_writer.h（重点是转义必须完整）。 */

#include "json_writer.h"
#include "dsh_internal.h"

#include <stdio.h>
#include <string.h>

#define DSH_JSON_INIT_CAP 256

struct dsh_json {
  char *buf;
  size_t len;
  size_t cap;
  int need_comma;   /* 下一个值前面要不要补逗号 */
  int depth;
  int failed;       /* 内存不足时置上：后续写入全部变成空操作 */
};

/** 追加字节（按需扩容）。失败时把 failed 置上，不再尝试。 */
static int reserve(dsh_json *j, size_t extra) {
  if (j->failed) return 0;
  if (j->len + extra + 1 <= j->cap) return 1;
  size_t cap = j->cap ? j->cap : DSH_JSON_INIT_CAP;
  while (cap < j->len + extra + 1) {
    if (cap > (size_t)1 << 30) { /* 1GB 上限，防病态输入把内存吃爆 */
      j->failed = 1;
      dsh_set_last_error("JSON 输出超过 1GB，数据可能异常");
      return 0;
    }
    cap *= 2;
  }
  char *next = (char *)dsh_mem_alloc(cap);
  if (next == NULL) {
    j->failed = 1;
    dsh_set_last_error("内存不足：JSON 缓冲需要 %zu 字节", cap);
    return 0;
  }
  if (j->len) memcpy(next, j->buf, j->len);
  if (j->buf) dsh_release(j->buf);
  j->buf = next;
  j->cap = cap;
  return 1;
}

static void put(dsh_json *j, const char *s, size_t n) {
  if (!reserve(j, n)) return;
  memcpy(j->buf + j->len, s, n);
  j->len += n;
  j->buf[j->len] = '\0';
}

static void putc_(dsh_json *j, char c) {
  if (!reserve(j, 1)) return;
  j->buf[j->len++] = c;
  j->buf[j->len] = '\0';
}

/** 补逗号（在写一个值/键之前调） */
static void comma(dsh_json *j) {
  if (j->need_comma) putc_(j, ',');
  j->need_comma = 1;
}

/**
 * 写一个容器的开符号（`{` 或 `[`），并把逗号状态清零 ——
 * **容器里的第一个元素前面不该有逗号**。
 * ⚠️ 逗号状态只允许在三个地方被设置：`comma()`（写值前）、本函数、闭容器时置 1；
 * 别在别处碰 need_comma —— 多一个逗号就是畸形 JSON。
 */
static void open_container(dsh_json *j, char symbol) {
  putc_(j, symbol);
  j->need_comma = 0;
}

dsh_json *dsh_json_new(void) {
  dsh_json *j = (dsh_json *)dsh_mem_alloc(sizeof(dsh_json));
  if (j == NULL) {
    dsh_set_last_error("内存不足：JSON 写入器");
    return NULL;
  }
  memset(j, 0, sizeof(*j));
  /* need_comma 从 0 开始：顶层还没有任何元素，第一个值前面不该有逗号 */
  return j;
}

/* ── 值的「裸」发射器 ──────────────────────────────────────────────────────
 * ⚠️ 必须分成两层，否则会产出 `{,"title":"x"}` 这种**畸形 JSON**：`dsh_json_key()`
 * 补逗号、写键、写冒号之后会把 need_comma 清零，值那一侧若又调 `comma()` 就多一个逗号。
 * 所以 `emit_*` 一律不碰逗号，补逗号只由 `dsh_json_*` 负责。 */
static void emit_string(dsh_json *j, const char *value) {
  if (value == NULL) value = "";
  putc_(j, '"');
  for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
    switch (*p) {
      case '"':  put(j, "\\\"", 2); break;
      case '\\': put(j, "\\\\", 2); break;
      case '\b': put(j, "\\b", 2); break;
      case '\f': put(j, "\\f", 2); break;
      case '\n': put(j, "\\n", 2); break;
      case '\r': put(j, "\\r", 2); break;
      case '\t': put(j, "\\t", 2); break;
      default:
        if (*p < 0x20) {
          char esc[7];
          snprintf(esc, sizeof(esc), "\\u%04x", *p);
          put(j, esc, 6);
        } else {
          putc_(j, (char)*p);
        }
    }
  }
  putc_(j, '"');
}

/** 按长度发射（内嵌 \0 也照写，转义成 \u0000）—— 见 dsh_json_str_len 的注释 */
static void emit_string_len(dsh_json *j, const char *value, int64_t len) {
  if (value == NULL) value = "";
  putc_(j, '"');
  for (int64_t i = 0; i < len; i++) {
    const unsigned char c = (unsigned char)value[i];
    switch (c) {
      case '"':  put(j, "\\\"", 2); break;
      case '\\': put(j, "\\\\", 2); break;
      case '\b': put(j, "\\b", 2); break;
      case '\f': put(j, "\\f", 2); break;
      case '\n': put(j, "\\n", 2); break;
      case '\r': put(j, "\\r", 2); break;
      case '\t': put(j, "\\t", 2); break;
      default:
        if (c < 0x20) {
          char esc[7];
          snprintf(esc, sizeof(esc), "\\u%04x", c);
          put(j, esc, 6);
        } else {
          putc_(j, (char)c);
        }
    }
  }
  putc_(j, '"');
}

static void emit_i64(dsh_json *j, int64_t value) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%lld", (long long)value);
  put(j, buf, strlen(buf));
}

char *dsh_json_take(dsh_json *j) {
  if (j == NULL) return NULL;
  if (j->failed) return NULL;
  return dsh_mem_strdup(j->buf != NULL ? j->buf : "");
}

void dsh_json_free(dsh_json *j) {
  if (j == NULL) return;
  if (j->buf != NULL) dsh_release(j->buf);
  dsh_release(j);
}

void dsh_json_object_begin(dsh_json *j) {
  comma(j);
  open_container(j, '{');
  j->depth++;
}

void dsh_json_object_end(dsh_json *j) {
  putc_(j, '}');
  j->need_comma = 1;
  if (j->depth > 0) j->depth--;
}

void dsh_json_array_begin(dsh_json *j) {
  comma(j);
  open_container(j, '[');
  j->depth++;
}

void dsh_json_array_end(dsh_json *j) {
  putc_(j, ']');
  j->need_comma = 1;
  if (j->depth > 0) j->depth--;
}

void dsh_json_key(dsh_json *j, const char *key) {
  comma(j);
  /* ⚠️ 用 emit_string 而不是 dsh_json_str —— 后者会**再补一次逗号**
   * （它是「写一个独立的值」的语义），产出 `{,"k":v}`。 */
  emit_string(j, key);
  putc_(j, ':');
  j->need_comma = 0; /* 键后面的值不该再补逗号 */
}

/**
 * 写一个 JSON 字符串：反斜杠与半角双引号必须转义，U+0000–U+001F 必须转义
 * （`\b\f\n\r\t` 用短写），**其余一律原样输出** —— 包括中文与其它非 ASCII，
 * JSON 允许直接写 UTF-8，转成 `\uXXXX` 反而让中文词条名在日志里不可读。
 * ⚠️ 不要顺手把 U+2028 / U+2029 也转义 —— 那是给 JS 源码字面量准备的规则，
 * 这里是纯数据、没必要，而且会让产物与参考实现不一致。
 */
void dsh_json_str(dsh_json *j, const char *value) {
  comma(j);
  emit_string(j, value);
}

void dsh_json_str_len(dsh_json *j, const char *value, int64_t len) {
  comma(j);
  emit_string_len(j, value, len);
  j->need_comma = 1;
}

void dsh_json_str_raw(dsh_json *j, const char *value) {
  if (value == NULL) {
    dsh_json_null(j);
    return;
  }
  dsh_json_str(j, value);
}

void dsh_json_value_raw(dsh_json *j, const char *json_text, size_t len) {
  if (json_text == NULL) {
    dsh_json_null(j);
    return;
  }
  if (!reserve(j, len + 1)) return;
  comma(j);
  memcpy(j->buf + j->len, json_text, len);
  j->len += len;
  j->buf[j->len] = '\0';
}

void dsh_json_i64(dsh_json *j, int64_t value) {
  comma(j);
  emit_i64(j, value);
}

void dsh_json_int(dsh_json *j, int value) { dsh_json_i64(j, (int64_t)value); }

void dsh_json_bool(dsh_json *j, int value) {
  comma(j);
  if (value) put(j, "true", 4); else put(j, "false", 5);
}

void dsh_json_double(dsh_json *j, double value) {
  comma(j);
  char buf[40];
  /* %.17g 保证往返精度；整数形态不带小数点，所以 6.0 会写成 6 */
  snprintf(buf, sizeof(buf), "%.17g", value);
  put(j, buf, strlen(buf));
}

void dsh_json_null(dsh_json *j) {
  comma(j);
  put(j, "null", 4);
}

/* 便捷写法：值那一侧**不能**走 dsh_json_*，要走 emit_* —— 见上面那段说明。
 * 键写完之后 need_comma 已经被清零，此时再补逗号就是那个畸形逗号的来源。 */
void dsh_json_kv_str(dsh_json *j, const char *key, const char *value) {
  dsh_json_key(j, key);
  emit_string(j, value);
  j->need_comma = 1;
}
void dsh_json_kv_i64(dsh_json *j, const char *key, int64_t value) {
  dsh_json_key(j, key);
  emit_i64(j, value);
  j->need_comma = 1;
}
void dsh_json_kv_int(dsh_json *j, const char *key, int value) {
  dsh_json_key(j, key);
  emit_i64(j, (int64_t)value);
  j->need_comma = 1;
}
void dsh_json_kv_bool(dsh_json *j, const char *key, int value) {
  dsh_json_key(j, key);
  if (value) put(j, "true", 4); else put(j, "false", 5);
  j->need_comma = 1;
}
