/* Base64 解码 —— 约定与理由见 dsh_base64.h。 */

#include "text/dsh_base64.h"

#include "dsh_internal.h"

#include <string.h>

/** 字符 → 6 bit 的值；不是 base64 字符就回 -1 */
static int b64_value(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

static int is_space(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

int dsh_base64_decode(const char *text, size_t len, uint8_t **out, size_t *out_len) {
  uint8_t *buffer;
  size_t capacity;
  size_t written = 0;
  uint32_t acc = 0;
  int bits = 0;
  size_t i;
  int padding = 0;

  if (out == NULL || out_len == NULL) {
    dsh_set_last_error("dsh_base64_decode：出参不能为空");
    return -1;
  }
  *out = NULL;
  *out_len = 0;

  if (text == NULL) {
    /* 空 = 解出空（不是错误） */
    buffer = (uint8_t *)dsh_mem_alloc(1);
    if (buffer == NULL) {
      dsh_set_last_error("dsh_base64_decode：内存不足");
      return -1;
    }
    *out = buffer;
    return 0;
  }

  /* 输出最多 3/4 的输入长度（+4 是为了「空输入也有一块非 NULL 的缓冲」） */
  capacity = (len / 4) * 3 + 4;
  buffer = (uint8_t *)dsh_mem_alloc(capacity);
  if (buffer == NULL) {
    dsh_set_last_error("dsh_base64_decode：内存不足");
    return -1;
  }

  for (i = 0; i < len; i++) {
    unsigned char c = (unsigned char)text[i];
    int value;

    if (is_space(c)) continue; /* ⚠️ SSE 每行都带 `\r`，不忽略就会多出坏字节 */
    if (c == '=') {
      padding++;
      /* `=` 只能出现在最后：出现过填充又来数据 → 这段是坏的（如实失败，不猜、不跳过） */
      if (padding > 2) {
        dsh_release(buffer);
        dsh_set_last_error("dsh_base64_decode：填充符太多（第 %llu 字节处）", (unsigned long long)i);
        return -1;
      }
      continue;
    }
    if (padding > 0) {
      dsh_release(buffer);
      dsh_set_last_error("dsh_base64_decode：填充符后面还有数据（第 %llu 字节处）",
                         (unsigned long long)i);
      return -1;
    }

    value = b64_value(c);
    if (value < 0) {
      dsh_release(buffer);
      dsh_set_last_error("dsh_base64_decode：第 %llu 个字节不是 base64 字符（0x%02x）",
                         (unsigned long long)i, (unsigned)c);
      return -1;
    }

    acc = (acc << 6) | (uint32_t)value;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (written + 1 > capacity) {
        /* 不该发生（容量是按 3/4 算的），留一道保险 */
        dsh_release(buffer);
        dsh_set_last_error("dsh_base64_decode：输出缓冲不够");
        return -1;
      }
      buffer[written++] = (uint8_t)((acc >> bits) & 0xFF);
    }
  }

  *out = buffer;
  *out_len = written;
  return 0;
}
