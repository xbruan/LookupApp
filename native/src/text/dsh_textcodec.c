/* ==========================================================================
 * 文本编解码层 · 实现（约定见 dsh_textcodec.h 顶部那段）
 *
 * 三块结构：
 *   ① 一个「落点」结构 dsh_txt_sink —— 同一个解码循环跑两遍：第一遍 out = NULL
 *      （只数长度），第二遍才真写。省掉 realloc，也不用「先猜个大缓冲再回缩」。
 *   ② 四个解码循环（UTF-8 透传 + 校验 / UTF-16LE / GB18030 / Big5），全都只往 sink 里
 *      吐码位 —— 坏字节一律吐 U+FFFD 并继续，**从不返回失败**。
 *   ③ 一个校验 + 分配的外壳，负责「哪些事才算失败」（参数错、OOM）。
 *
 * GB18030 的四字节区本版不做：首字节 0x81–0xFE、次字节 0x30–0x39 → 吃掉四字节、吐一个
 * U+FFFD、记一条诊断（**不**报成硬失败，否则整本词典读不出来）。这一档**不**等价于
 * 「超出 BMP」：四字节区里也有一小段落在 BMP 上，本版按约定一律替换，不做例外。
 * ========================================================================== */

#include "dsh_textcodec.h"

#include "dsh_internal.h"
#include "dsh_textcodec_tables.h"

#define DSH_TXT_REPLACEMENT 0xFFFDu

/* ── ① 落点 ─────────────────────────────────────────────────────────────── */

typedef struct {
  uint8_t *out;    /* NULL 表示只数长度、不写 */
  size_t pos;      /* 已产出（或将产出）的 UTF-8 字节数 */
  size_t replaced; /* 产出的 U+FFFD 个数（b 约定的诊断用） */
  size_t fourbyte; /* 遇到的 GB18030 四字节区段数 */
} dsh_txt_sink;

/** 把一个码位编成 UTF-8 存进 sink（sink->out 为 NULL 时只累加长度）。 */
static void dsh_txt_put(dsh_txt_sink *sink, uint32_t cp) {
  uint8_t *out = sink->out;
  const size_t pos = sink->pos;

  if (cp < 0x80u) {
    if (out != NULL) out[pos] = (uint8_t)cp;
    sink->pos = pos + 1;
  } else if (cp < 0x800u) {
    if (out != NULL) {
      out[pos] = (uint8_t)(0xC0u | (cp >> 6));
      out[pos + 1] = (uint8_t)(0x80u | (cp & 0x3Fu));
    }
    sink->pos = pos + 2;
  } else if (cp < 0x10000u) {
    if (out != NULL) {
      out[pos] = (uint8_t)(0xE0u | (cp >> 12));
      out[pos + 1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
      out[pos + 2] = (uint8_t)(0x80u | (cp & 0x3Fu));
    }
    sink->pos = pos + 3;
  } else {
    if (out != NULL) {
      out[pos] = (uint8_t)(0xF0u | (cp >> 18));
      out[pos + 1] = (uint8_t)(0x80u | ((cp >> 12) & 0x3Fu));
      out[pos + 2] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
      out[pos + 3] = (uint8_t)(0x80u | (cp & 0x3Fu));
    }
    sink->pos = pos + 4;
  }
}

/** 吐一个 U+FFFD（这是「坏字节」的统一出口，调用方一律继续往下解）。 */
static void dsh_txt_put_replacement(dsh_txt_sink *sink) {
  sink->replaced++;
  dsh_txt_put(sink, DSH_TXT_REPLACEMENT);
}

/* ── ② 四个解码循环 ─────────────────────────────────────────────────────── */

/* UTF-8：合法就透传，非法 / 残缺按「每个坏字节一个 U+FFFD」处理（**不是** WHATWG 的
 * 「最长大子串」约定：那种约定下 F0 9F 98 只给一个，本层给三个 —— 与本任务约定一致）。 */
static void dsh_txt_decode_utf8(const uint8_t *bytes, size_t len, dsh_txt_sink *sink) {
  size_t i = 0;
  while (i < len) {
    const uint8_t lead = bytes[i];
    size_t need = 0;
    uint32_t cp = 0;
    uint8_t first_lo = 0x80u;
    uint8_t first_hi = 0xBFu;

    if (lead < 0x80u) {
      dsh_txt_put(sink, lead);
      i++;
      continue;
    } else if (lead >= 0xC2u && lead <= 0xDFu) {
      need = 2;
      cp = (uint32_t)(lead & 0x1Fu);
    } else if (lead >= 0xE0u && lead <= 0xEFu) {
      need = 3;
      cp = (uint32_t)(lead & 0x0Fu);
      if (lead == 0xE0u) first_lo = 0xA0u; /* 过长的三字节序列要挡掉 */
      if (lead == 0xEDu) first_hi = 0x9Fu; /* 代理区不许出现在 UTF-8 里 */
    } else if (lead >= 0xF0u && lead <= 0xF4u) {
      need = 4;
      cp = (uint32_t)(lead & 0x07u);
      if (lead == 0xF0u) first_lo = 0x90u; /* 过长的四字节序列要挡掉 */
      if (lead == 0xF4u) first_hi = 0x8Fu; /* 上限 U+10FFFF */
    } else {
      /* 0x80–0xC1（孤立的后续字节 / 过长编码的首字节）与 0xF5–0xFF */
      dsh_txt_put_replacement(sink);
      i++;
      continue;
    }

    int bad = 0;
    size_t k = 1;
    for (; k < need; k++) {
      if (i + k >= len) { /* 残缺：后面没字节了 */
        bad = 1;
        break;
      }
      const uint8_t next = bytes[i + k];
      const uint8_t lo = (k == 1) ? first_lo : 0x80u;
      const uint8_t hi = (k == 1) ? first_hi : 0xBFu;
      if (next < lo || next > hi) {
        bad = 1;
        break;
      }
      cp = (cp << 6) | (uint32_t)(next & 0x3Fu);
    }

    if (bad) {
      dsh_txt_put_replacement(sink);
      i++; /* 只吃一个字节：剩下的字节各自再判一次（每个坏字节一个 U+FFFD） */
      continue;
    }
    dsh_txt_put(sink, cp);
    i += need;
  }
}

/* UTF-16LE：代理对合成补充平面；落单的代理与末尾半个码元各给一个 U+FFFD。 */
static void dsh_txt_decode_utf16le(const uint8_t *bytes, size_t len, dsh_txt_sink *sink) {
  size_t i = 0;
  while (i + 1 < len) {
    const uint32_t unit = (uint32_t)bytes[i] | ((uint32_t)bytes[i + 1] << 8);
    i += 2;

    if (unit >= 0xD800u && unit <= 0xDBFFu) { /* 高代理 */
      if (i + 1 < len) {
        const uint32_t low = (uint32_t)bytes[i] | ((uint32_t)bytes[i + 1] << 8);
        if (low >= 0xDC00u && low <= 0xDFFFu) {
          i += 2;
          dsh_txt_put(sink, 0x10000u + ((unit - 0xD800u) << 10) + (low - 0xDC00u));
          continue;
        }
      }
      dsh_txt_put_replacement(sink); /* 落单的高代理：只吃它自己 */
      continue;
    }
    if (unit >= 0xDC00u && unit <= 0xDFFFu) { /* 落单的低代理 */
      dsh_txt_put_replacement(sink);
      continue;
    }
    dsh_txt_put(sink, unit);
  }
  if (i < len) dsh_txt_put_replacement(sink); /* 末尾剩 1 个字节 → 一个 U+FFFD */
}

/* GB18030（含 GBK / GB2312）：单字节 ASCII + 双字节区查表 + 四字节区按约定替换。 */
static void dsh_txt_decode_gb18030(const uint8_t *bytes, size_t len, dsh_txt_sink *sink) {
  size_t i = 0;
  while (i < len) {
    const uint8_t lead = bytes[i];

    if (lead < 0x80u) {
      dsh_txt_put(sink, lead);
      i++;
      continue;
    }
    if (lead == 0x80u || lead == 0xFFu) { /* 这两个首字节在 GB18030 里没用 */
      dsh_txt_put_replacement(sink);
      i++;
      continue;
    }

    /* lead 现在是 0x81–0xFE */
    if (i + 1 < len && bytes[i + 1] >= 0x30u && bytes[i + 1] <= 0x39u) {
      /* 四字节区（0x81–0xFE 0x30–0x39 0x81–0xFE 0x30–0x39）：本版不支持 →
       * **整段**换成一个 U+FFFD（既不悄悄丢掉，也不报成硬失败，否则整本词典读不出来）。
       * 残缺时吃满 4 字节或吃到末尾为止。 */
      const size_t take = (len - i >= 4) ? 4 : (len - i);
      sink->fourbyte++;
      dsh_txt_put_replacement(sink);
      i += take;
      continue;
    }

    if (i + 1 < len) {
      const uint8_t trail = bytes[i + 1];
      if (trail >= DSH_GBK_TRAIL_FIRST && trail <= DSH_GBK_TRAIL_LAST) {
        const uint16_t cp = dsh_gbk_bmp[DSH_GBK_INDEX(lead, trail)];
        if (cp != 0) { /* 槽位 0 表示 python3 的 gb18030 也解不出来（含 0x7F 那一列） */
          dsh_txt_put(sink, cp);
          i += 2;
          continue;
        }
      }
    }
    dsh_txt_put_replacement(sink);
    i++;
  }
}

/* Big5：首字节 0xA1–0xF9，次字节 0x40–0x7E 或 0xA1–0xFE，其余一律坏字节。 */
static void dsh_txt_decode_big5(const uint8_t *bytes, size_t len, dsh_txt_sink *sink) {
  size_t i = 0;
  while (i < len) {
    const uint8_t lead = bytes[i];

    if (lead < 0x80u) {
      dsh_txt_put(sink, lead);
      i++;
      continue;
    }
    if (lead >= DSH_BIG5_LEAD_FIRST && lead <= DSH_BIG5_LEAD_LAST && i + 1 < len) {
      const uint8_t trail = bytes[i + 1];
      const int trail_ok = (trail >= 0x40u && trail <= 0x7Eu) || (trail >= 0xA1u && trail <= 0xFEu);
      if (trail_ok) {
        const uint16_t cp = dsh_big5_bmp[DSH_BIG5_INDEX(lead, trail)];
        if (cp != 0) {
          dsh_txt_put(sink, cp);
          i += 2;
          continue;
        }
      }
    }
    dsh_txt_put_replacement(sink);
    i++;
  }
}

/** 按编码选一个解码循环（enc 已在调用方校验过）。 */
static void dsh_txt_decode_bytes(dsh_text_encoding enc, const uint8_t *bytes, size_t len,
                                 dsh_txt_sink *sink) {
  switch (enc) {
    case DSH_TXT_UTF16LE:
      dsh_txt_decode_utf16le(bytes, len, sink);
      break;
    case DSH_TXT_GB18030:
      dsh_txt_decode_gb18030(bytes, len, sink);
      break;
    case DSH_TXT_BIG5:
      dsh_txt_decode_big5(bytes, len, sink);
      break;
    case DSH_TXT_UTF8:
    default:
      dsh_txt_decode_utf8(bytes, len, sink);
      break;
  }
}

/* ── ③ 外壳：编码名、BOM、参数校验 ──────────────────────────────────────── */

static char dsh_txt_ascii_lower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/** name 与 want 是否同一个名字（忽略大小写、连字符、下划线、空格）。 */
static int dsh_txt_name_is(const char *name, const char *want) {
  size_t i = 0;
  while (name[i] != '\0') {
    const char c = name[i];
    if (c == '-' || c == '_' || c == ' ') {
      i++;
      continue;
    }
    if (*want == '\0') return 0;
    if (dsh_txt_ascii_lower(c) != *want) return 0;
    i++;
    want++;
  }
  return *want == '\0';
}

dsh_text_encoding dsh_text_encoding_from_name(const char *name) {
  if (name == NULL) return DSH_TXT_UTF8; /* 头里没写 Encoding：按 UTF-8 */

  if (dsh_txt_name_is(name, "gbk") || dsh_txt_name_is(name, "gb2312") ||
      dsh_txt_name_is(name, "gb18030")) {
    return DSH_TXT_GB18030; /* GBK 是 GB18030 的子集，按大的解 */
  }
  if (dsh_txt_name_is(name, "big5")) return DSH_TXT_BIG5;
  if (dsh_txt_name_is(name, "utf16") || dsh_txt_name_is(name, "utf16le")) {
    return DSH_TXT_UTF16LE;
  }
  /* utf8、空串、Shift_JIS、UTF-16BE 之类一律按 UTF-8（默认分支） */
  return DSH_TXT_UTF8;
}

static int dsh_txt_encoding_is_known(dsh_text_encoding enc) {
  return enc == DSH_TXT_UTF8 || enc == DSH_TXT_UTF16LE || enc == DSH_TXT_GB18030 ||
         enc == DSH_TXT_BIG5;
}

/** 开头的 BOM 要吃掉（只认 UTF-8 与 UTF-16LE 两种）。 */
static size_t dsh_txt_bom_size(dsh_text_encoding enc, const uint8_t *bytes, size_t len) {
  if (enc == DSH_TXT_UTF8 && len >= 3 && bytes[0] == 0xEFu && bytes[1] == 0xBBu && bytes[2] == 0xBFu) {
    return 3;
  }
  if (enc == DSH_TXT_UTF16LE && len >= 2 && bytes[0] == 0xFFu && bytes[1] == 0xFEu) {
    return 2;
  }
  return 0;
}

int dsh_text_decode(dsh_text_encoding enc, const uint8_t *bytes, size_t len, char **out_text,
                    size_t *out_len) {
  dsh_txt_sink count;
  dsh_txt_sink fill;
  char *text = NULL;
  size_t skip = 0;
  const uint8_t *body = NULL;
  size_t body_len = 0;

  dsh_clear_last_error();
  if (out_text != NULL) *out_text = NULL;
  if (out_len != NULL) *out_len = 0;

  if (out_text == NULL || out_len == NULL) {
    dsh_set_last_error("dsh_text_decode 的出参指针不能为空（out_text / out_len）");
    return 1;
  }
  if (bytes == NULL && len != 0) {
    dsh_set_last_error("dsh_text_decode 收到的字节指针为空，但长度是 %zu", len);
    return 1;
  }
  if (!dsh_txt_encoding_is_known(enc)) {
    dsh_set_last_error("dsh_text_decode 收到未知的编码枚举值 %d（只认 0–3）", (int)enc);
    return 1;
  }

  skip = dsh_txt_bom_size(enc, bytes, len);
  body = (bytes != NULL) ? (bytes + skip) : NULL;
  body_len = len - skip;

  /* 第一遍只数长度（同一段解码逻辑跑两遍，省掉猜缓冲与回缩） */
  count.out = NULL;
  count.pos = 0;
  count.replaced = 0;
  count.fourbyte = 0;
  dsh_txt_decode_bytes(enc, body, body_len, &count);

  text = (char *)dsh_mem_alloc(count.pos + 1);
  if (text == NULL) {
    dsh_set_last_error("内存不足：解码结果需要 %zu 字节", count.pos + 1);
    return 1;
  }

  fill.out = (uint8_t *)text;
  fill.pos = 0;
  fill.replaced = 0;
  fill.fourbyte = 0;
  dsh_txt_decode_bytes(enc, body, body_len, &fill);
  text[fill.pos] = '\0'; /* 缓冲保证以 \0 结尾（但真值请看 out_len） */

  *out_text = text;
  *out_len = fill.pos;

  /* 解出过替换字符 —— 记诊断，但**仍然返回 0**（见头文件那条约定）。 */
  if (fill.fourbyte > 0) {
    dsh_set_last_error(
        "遇到 GB18030 四字节区（%zu 处），本版不支持，已整段替换成 U+FFFD（共 %zu 个，不算失败）",
        fill.fourbyte, fill.replaced);
  } else if (fill.replaced > 0) {
    dsh_set_last_error("解码时遇到 %zu 处非法或残缺的序列，已按 U+FFFD 替换（不算失败）",
                       fill.replaced);
  }
  return 0;
}
