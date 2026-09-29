/* MDict 解析器的实现。文件结构、约定与「两条容易搞错的地方」见 dsh_mdx.h。
 *
 * 范围：v2.0 的完整读取路径（头部 / 键区 / 记录区 / 词块 / 记录块）；键信息块与记录块
 * 只做 **zlib**，LZO 与加密两条路留明确的失败点；本文件**不实现码表**，解码全交给 src/text/。
 */

#include "dict/dsh_mdx.h"
#include "compress/dsh_inflate.h"
#include "compress/dsh_lzo1x.h"
#include "crypto/dsh_ripemd128.h"
#include "dsh_internal.h"
#include "platform/dsh_file.h" /* 读文件走它（UTF-8 → 宽字符 API），别自己 fopen */
#include "platform/dsh_mapfile.h"
#include "platform/dsh_sleep.h"
#include "text/dsh_textcodec.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DSH_MDX_MAX_WARNINGS 64
#define DSH_COMPRESS_NONE 0x00000000u
#define DSH_COMPRESS_LZO 0x01000000u
#define DSH_COMPRESS_ZLIB 0x02000000u

/* ── 解析期的规模预算（**整份文件只有这一处定义**：索引 / 单块 / 单词条 / 总量共用）──────
 *
 * 为什么非有不可：文件里的长度字段全是**文件内容说了算的整数**。只做「先比大小再相加」
 * 能防溢出，却挡不住一个几百字节的坏文件按声明值去要几百兆内存。2026-09 的审计两条都出在
 * 这里：① 记录块解压长度累加溢出（一个 16,720 字节的文件，1,025 项各声明 2^53）；
 * ② 346 字节的文件声明 1,000 万个词块，解析器当场按声明值要 640,000,000 字节。
 *
 * 三条通则：
 *   · **先算上限、再分配**：条数上限只能由信息块的**真实长度**推出来（每条至少占几个
 *     字节），不许拿头里的声明值直接乘 sizeof 去分配；
 *   · **先比大小、再相加**：任何累加都先查 `INT64_MAX - 累加器` 与总量预算，绝不让它回绕
 *     —— 回绕成负数之后所有基于偏移的边界检查都会失效，而且是**静默**失效；
 *   · **荒诞值按格式错误处理**，不当警告：警告会放着一个已知坏掉的文件继续往下走。
 *
 * 数值取得很宽，只挡荒诞值、不当性能阈值 —— 实测最大的固定素材
 * （9.2 MB 的 big.mdx）记录区解压总量是文件的 10 倍，10 KB 的 audio.mdd 是 26 倍，
 * 离下面这些上限都差好几个量级。 */
#define DSH_MDX_MAX_ENTRIES 10000000LL                        /* 索引项数硬上限（沿用原值，别放大） */
#define DSH_MDX_MAX_INDEX_BYTES ((int64_t)256 * 1024 * 1024)  /* 索引数组**自身**的字节上限 */
#define DSH_MDX_MAX_BLOCK_UNPACK ((int64_t)512 * 1024 * 1024) /* 单块解压后的字节上限（与 dsh_inflate 的输出上限取齐） */
#define DSH_MDX_UNPACK_RATIO 16384LL                          /* 解压总量 ≤ 文件长度 × 这个倍数 … */
#define DSH_MDX_UNPACK_FLOOR ((int64_t)1 << 20)               /* … 再兜 1 MiB 的底（很小的文件也要放得下正常的头与索引） */

struct dsh_mdx {
  FILE *fp;
  char *path;
  int is_mdd;

  /* 整个文件的只读视图。**读路径全部走它**：打开时映射一次，之后每次读只是指针算术，
   * 读路径上不再有任何分配。映射归句柄所有，随 close 一起放。 */
  dsh_map *map;
  const uint8_t *map_base;
  int64_t map_len;

  /* 元信息 */
  char *raw_header;      /* 头部原文（UTF-16LE 转出来的 UTF-8） */
  double version;        /* 解析不出来 = 0（按 v1.2 走） */
  int encrypted;
  char *encoding_name;   /* 归一之后：UTF-16 / UTF-8 / GB18030 / BIG5 */
  dsh_text_encoding encoding;
  char *title;
  int num_width;         /* 8 或 4 */

  /* 键区 */
  int64_t key_block_count;
  int64_t key_count;
  int64_t key_info_unpack_size;
  int64_t key_info_packed_size;
  int64_t key_block_packed_size;

  /* 记录区 */
  int64_t record_block_count;
  int64_t record_entries_num;
  int64_t record_info_comp_size;
  int64_t record_block_comp_size;

  dsh_mdx_key_block *key_blocks;
  dsh_mdx_record_block *record_blocks;

  /* 文件内偏移。名字与含义见 read_header 末尾那段注释。 */
  int64_t key_header_offset;    /* 键区头部起点 */
  int64_t key_info_offset;      /* 键信息块起点 */
  int64_t key_block_offset;     /* 词块数据起点 */
  int64_t record_header_offset; /* 记录区头部起点 */
  int64_t record_info_offset;   /* 记录信息块起点 */
  int64_t record_block_offset;  /* 记录块数据起点 */

  int block_order_monotone;

  char *warnings[DSH_MDX_MAX_WARNINGS];
  int warning_count;
};

/* ── 小工具 ─────────────────────────────────────────────────────────────── */

/* 内部函数的前置声明：这两个定义在文件后半段，但前半段要用 —— 加声明而不是把函数搬走，
 * 因为那些分节是按「读文件 → 解压 → 读键 → 读记录」排的。 */
static uint8_t *decompress_block(dsh_mdx *m, const uint8_t *packed, size_t packed_len,
                                 size_t unpack_size, int64_t block_index, int is_record,
                                 size_t *out_len);
static uint8_t *mdx_decrypt(const uint8_t *block, size_t len);
static void verify_checksum(dsh_mdx *m, uint32_t expected, const uint8_t *data, size_t len,
                            const char *what);
static int locate_entry(dsh_mdx *m, int64_t global_index, int64_t *out_entry_in_block,
                        int64_t *out_block_index);
static int lookup_full(dsh_mdx *m, const char *key, char **out_key, int64_t *out_block,
                       int64_t *out_global_index);

/** 找 global_index 这条词条属于哪个词块、以及它在块内的序号 */
static int locate_entry(dsh_mdx *m, int64_t global_index, int64_t *out_entry_in_block,
                       int64_t *out_block_index) {
  for (int64_t i = 0; i < m->key_block_count; i++) {
    const int64_t start = m->key_blocks[i].entry_offset;
    const int64_t end = start + m->key_blocks[i].entry_count;
    if (global_index >= start && global_index < end) {
      *out_entry_in_block = global_index - start;
      *out_block_index = i;
      return 0;
    }
  }
  return -1;
}

static void warn(dsh_mdx *m, const char *fmt, ...) {
  if (m->warning_count >= DSH_MDX_MAX_WARNINGS) return;
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  m->warnings[m->warning_count] = dsh_mem_strdup(buf);
  if (m->warnings[m->warning_count] != NULL) m->warning_count++;
}

/** zlib / LZO 自己带校验和（MDict 的块头第 4..8 字节是 adler32） */
static uint32_t mdx_adler32(const uint8_t *data, size_t len) {
  const uint32_t mod = 65521u;
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < len; i++) {
    a = (a + data[i]) % mod;
    b = (b + a) % mod;
  }
  return (b << 16) | a;
}

/* 核对解压结果的 adler32 —— 期望值是 0 就不查，对不上**只记警告不失败**；
 * 警告文案必须与参考实现逐字相同（会进标准答案文件的 warnings 数组）。
 * 例外只有一处：Encrypted=1 且对不上时，调用方直接报错（见 decompress_block）。 */
static void verify_checksum(dsh_mdx *m, uint32_t expected, const uint8_t *data, size_t len,
                            const char *what) {
  if (expected == 0) return;
  const uint32_t actual = mdx_adler32(data, len);
  if (actual != expected) {
    warn(m, "解压数据 adler32 校验不符：期望 %08X，实际 %08X", expected, actual);
  }
  (void)what;
}

/* 取一段文件的**只读视图**：不分配、不拷贝、调用方**不许释放**（归句柄的映射所有）。
 * 越界检查必须先比大小再相加 —— `offset + len` 自己可能溢出成负数、绕过检查。 */
static const uint8_t *view_at(dsh_mdx *m, int64_t offset, size_t len, const char *caller) {
  if (offset < 0 || len == 0) {
    dsh_set_last_error("读取位置非法：offset=%lld len=%zu（%s）", (long long)offset, len, caller);
    return NULL;
  }
  if (m->map_base == NULL) {
    /* 映射没建起来（或这个平台没有映射）—— 如实报，不去猜数据 */
    dsh_set_last_error("读文件失败：%s 取不到文件的只读视图（映射未建立）", caller);
    return NULL;
  }
  if (offset > m->map_len || (int64_t)len > m->map_len - offset) {
    dsh_set_last_error("读文件失败：%s 要 [%lld, %lld) 这一段，而文件只有 %lld 字节（越界了）",
                       caller, (long long)offset, (long long)(offset + (int64_t)len),
                       (long long)m->map_len);
    return NULL;
  }
  return m->map_base + offset;
}

/* 取一段文件的**独占副本**（dsh_mem_alloc 分配，调用方必须 dsh_release）。只有
 * 「要把这一段交给别人、而那个人可能释放它」的地方才用它（如键信息块交给解码器）；
 * 其余一律用 view_at —— 那才是没有分配的那条路。 */
static uint8_t *read_at(dsh_mdx *m, int64_t offset, size_t len, const char *caller) {
  const uint8_t *view = view_at(m, offset, len, caller);
  if (view == NULL) return NULL;
  uint8_t *copy = (uint8_t *)dsh_mem_alloc(len);
  if (copy == NULL) {
    dsh_set_last_error("内存不足：需要 %zu 字节", len);
    return NULL;
  }
  memcpy(copy, view, len);
  return copy;
}

/** 视图便于调用方使用的别名（只读；调用方一律按 `const uint8_t *` 用） */
#define VIEW(m, off, len, caller) ((uint8_t *)(uintptr_t)view_at((m), (off), (len), (caller)))

/** 大端整数读取。width 只可能是 1/2/4/8；越界返回 0（与参考实现一致） */
static int64_t read_be(const uint8_t *b, size_t b_len, size_t offset, int width) {
  if (offset + (size_t)width > b_len) return 0;
  switch (width) {
    case 1: return b[offset];
    case 2: return ((int64_t)b[offset] << 8) | b[offset + 1];
    case 4:
      return ((int64_t)b[offset] << 24) | ((int64_t)b[offset + 1] << 16) |
             ((int64_t)b[offset + 2] << 8) | (int64_t)b[offset + 3];
    case 8: {
      uint64_t v = 0;
      for (int i = 0; i < 8; i++) v = (v << 8) | b[offset + (size_t)i];
      /* 超过 2^53 的值在 JS 参考实现里会精度丢失；这里如实报错，不许悄悄截断 */
      if (v > ((uint64_t)1 << 53)) {
        dsh_set_last_error("MDict 的 8 字节整数超过 2^53，文件可能已损坏");
        return -1;
      }
      return (int64_t)v;
    }
    default: return 0;
  }
}

/* ── 规模检查（分配之前、累加之前都要过这几关）─────────────────────────────
 * 四个助手对应预算的四档：索引项数/字节 → 单块 → 累加 → 总量；都在这里收口，
 * 免得同一条检查在四个地方各写一遍、写着写着就不一样了。 */

/** 索引数组能不能按 count 条分配：条数非负、不超条数上限、字节数不超预算。
 * ⚠️ 必须先查再分配：`count * sizeof(T)` 自己会溢出，而 count 是文件说了算的。 */
static int index_budget_ok(const char *what, int64_t count, size_t elem) {
  if (count < 0 || count > DSH_MDX_MAX_ENTRIES) {
    dsh_set_last_error("%s的条数不合法：%lld（上限 %lld）", what, (long long)count,
                       (long long)DSH_MDX_MAX_ENTRIES);
    return 0;
  }
  if (elem > 0 && (uint64_t)count > (uint64_t)DSH_MDX_MAX_INDEX_BYTES / (uint64_t)elem) {
    dsh_set_last_error("%s需要 %lld 项 × %zu 字节，超过索引预算 %lld 字节（文件已损坏）", what,
                       (long long)count, elem, (long long)DSH_MDX_MAX_INDEX_BYTES);
    return 0;
  }
  return 1;
}

/** 单个块声明的「解压后字节数」可不可信（单词条那一档也走它） */
static int block_unpack_ok(int64_t size) {
  return size >= 0 && size <= DSH_MDX_MAX_BLOCK_UNPACK;
}

/** 解压总量的可信上限：文件长度 × 倍数，至少给 DSH_MDX_UNPACK_FLOOR 的底。
 * 为什么不用一个固定常数当总量上限：真词典的大小差好几个量级，固定值要么卡死大词典、
 * 要么对畸形文件形同虚设；「相对文件长度」才是既能放过真词典、又能挡住荒诞值的口径。 */
static int64_t unpack_total_cap(const dsh_mdx *m) {
  const int64_t len = (m->map_len > 0) ? m->map_len : 0;
  if (len > INT64_MAX / DSH_MDX_UNPACK_RATIO) return INT64_MAX; /* × 倍数自己也别溢出 */
  const int64_t scaled = len * DSH_MDX_UNPACK_RATIO;
  return (scaled < DSH_MDX_UNPACK_FLOOR) ? DSH_MDX_UNPACK_FLOOR : scaled;
}

/** 累加会不会溢出（**先查后加**的判据）。
 * ⚠️ 顺序不许反：「先加再查」那一次加法本身就是未定义行为（UBSan：signed integer overflow），
 * 而且回绕出来的负数会让后面所有 `offset + len <= 文件长度` 式的检查**静默**失效。
 * 入参 add 与 acc 都必须已确认非负，否则 INT64_MAX - acc 自己就不安全。 */
static int acc_would_overflow(int64_t acc, int64_t add) {
  return acc < 0 || add < 0 || add > INT64_MAX - acc;
}

/** [base + offset, base + offset + len) 是不是整段落在文件里。
 * ⚠️ 三处相加都要**先比大小再相减**：写成 `base + offset + len <= map_len` 的话，
 * 左边自己会溢出成负数，于是越界的区间反而通过检查。 */
static int range_inside_file(const dsh_mdx *m, int64_t base, int64_t offset, int64_t len) {
  if (base < 0 || offset < 0 || len < 0) return 0;
  if (base > m->map_len) return 0;
  const int64_t budget = m->map_len - base;
  if (offset > budget) return 0;
  return len <= budget - offset;
}

/* ── 文本解码 ───────────────────────────────────────────────────────────────
 * 本文件**不自带** UTF-16LE 转换：解码约定必须只有一处，否则同一份文件在两条路上会
 * 解出不同的字节。全部交给 src/text/ 那一层。 */

/** 按当前编码把一段字节解成 UTF-8 */
static char *decode_text(dsh_mdx *m, const uint8_t *b, size_t len, size_t *out_len) {
  char *text = NULL;
  size_t n = 0;
  if (dsh_text_decode(m->encoding, b, len, &text, &n) != 0) return NULL;
  if (out_len) *out_len = n;
  return text;
}

/* ── 头部属性 ───────────────────────────────────────────────────────────── */

/** 在头部文本里找 name=「value」（\w+=「...」 的真子集；属性名都是 [A-Za-z0-9_]） */
static char *attr(const char *text, const char *name) {
  const size_t name_len = strlen(name);
  for (const char *p = text; *p; p++) {
    if (*p != '"') continue;
    /* 回看：引号前面应当是 name= 且 name 之前不是字母数字 */
    if ((size_t)(p - text) < name_len + 2) continue;
    const char *eq = p - 1;
    if (*eq != '=') continue;
    const char *start = eq - name_len;
    if (start - 1 >= text) {
      const char prev = *(start - 1);
      if ((prev >= 'A' && prev <= 'Z') || (prev >= 'a' && prev <= 'z') ||
          (prev >= '0' && prev <= '9') || prev == '_') {
        continue;
      }
    }
    if (strncmp(start, name, name_len) != 0) continue;
    const char *end = strchr(p + 1, '"');
    if (end == NULL) return NULL;
    const size_t vlen = (size_t)(end - (p + 1));
    char *value = (char *)dsh_mem_alloc(vlen + 1);
    if (value == NULL) return NULL;
    memcpy(value, p + 1, vlen);
    value[vlen] = '\0';
    return value;
  }
  return NULL;
}

/* 把 &lt; &gt; &quot; &amp; 反转义。顺序必须与参考实现一致：&amp; 放最后，
 * 这样 `&amp;lt;` 解成 `&lt;` 而不是 `<`。 */
static void unescape_entities(char *text) {
  struct { const char *from; char to; } pairs[] = {
      {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&amp;", '&'}};
  for (size_t k = 0; k < sizeof(pairs) / sizeof(pairs[0]); k++) {
    const size_t flen = strlen(pairs[k].from);
    char *p = text;
    while ((p = strstr(p, pairs[k].from)) != NULL) {
      *p = pairs[k].to;
      memmove(p + 1, p + flen, strlen(p + flen) + 1);
    }
  }
}

static char *attr_unescaped(const char *text, const char *name) {
  char *v = attr(text, name);
  if (v != NULL) unescape_entities(v);
  return v;
}

/** parseFloat 的前导数字前缀语义；解析不出来返回 0（等价于参考实现的 NaN → 走 v1.2） */
static double parse_float_prefix(const char *text) {
  if (text == NULL) return 0.0;
  const char *p = text;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  char *end = NULL;
  const double v = strtod(p, &end);
  if (end == p) return 0.0;
  return v;
}

/** parseInt(x,10) 的语义；「Yes」→1，「No」/空→0 */
static int parse_encrypted(const char *text) {
  if (text == NULL || text[0] == '\0' || strcmp(text, "No") == 0) return 0;
  if (strcmp(text, "Yes") == 0) return 1;
  const char *p = text;
  while (*p == ' ' || *p == '\t') p++;
  char *end = NULL;
  const long v = strtol(p, &end, 10);
  if (end == p) return 0;
  return (int)v;
}

/* ── 头部读取 ───────────────────────────────────────────────────────────── */

static int read_header(dsh_mdx *m) {
  uint8_t size4[4];
  if (fseek(m->fp, 0, SEEK_SET) != 0 || fread(size4, 1, 4, m->fp) != 4) {
    dsh_set_last_error("文件太小或读不到头部长度：不是有效的 MDict 词典");
    return -1;
  }
  const int64_t header_len =
      ((int64_t)size4[0] << 24) | ((int64_t)size4[1] << 16) | ((int64_t)size4[2] << 8) | size4[3];
  if (header_len <= 0 || header_len > (int64_t)64 * 1024 * 1024) {
    dsh_set_last_error("MDict 头部长度非法：%lld", (long long)header_len);
    return -1;
  }
  const uint8_t *raw = view_at(m, 4, (size_t)header_len, "read_header（头部规模）");
  if (raw == NULL) return -1;

  /* 参考实现一律按 UTF-16LE 解；少数第三方工具写 UTF-8。
   * 这里先按 UTF-16LE 解，若一个属性都抓不到再退到 UTF-8。 */
  size_t text_len = 0;
  char *text = NULL;
  size_t n16 = 0;
  if (dsh_text_decode(DSH_TXT_UTF16LE, raw, (size_t)header_len, &text, &n16) != 0) {
    /* ⚠️ `raw` 是映射的**只读视图**，不归我们，这里**不许释放** */
    return -1;
  }
  (void)text_len;

  char *probe = attr(text, "GeneratedByEngineVersion");
  if (probe == NULL) {
    /* UTF-16LE 一个属性都读不出来 → 这份头可能是 UTF-8 的 */
    char *utf8_text = NULL;
    size_t n8 = 0;
    if (dsh_text_decode(DSH_TXT_UTF8, raw, (size_t)header_len, &utf8_text, &n8) == 0) {
      char *p2 = attr(utf8_text, "GeneratedByEngineVersion");
      if (p2 != NULL) {
        dsh_release(text);
        text = utf8_text;
        utf8_text = NULL;
        dsh_release(p2);
      }
      if (utf8_text != NULL) dsh_release(utf8_text);
    }
  } else {
    dsh_release(probe);
  }
  /* `raw` 是映射的只读视图，**不释放**（它归句柄的映射所有） */

  m->raw_header = text;

  char *ver = attr(text, "GeneratedByEngineVersion");
  m->version = parse_float_prefix(ver);
  dsh_release(ver);
  m->num_width = (m->version >= 2.0) ? 8 : 4;

  char *enc = attr(text, "Encrypted");
  m->encrypted = parse_encrypted(enc);
  dsh_release(enc);

  char *title = attr_unescaped(text, "Title");
  m->title = (title != NULL && title[0] != '\0') ? title : NULL;
  if (m->title == NULL) dsh_release(title);

  /* 编码：.mdd 一律 UTF-16LE；.mdx 看 Encoding 属性 */
  char *enc_name = attr(text, "Encoding");
  if (m->is_mdd) {
    m->encoding_name = dsh_mem_strdup("UTF-16");
    m->encoding = DSH_TXT_UTF16LE;
  } else if (enc_name == NULL || enc_name[0] == '\0') {
    m->encoding_name = dsh_mem_strdup("UTF-8");
    m->encoding = DSH_TXT_UTF8;
  } else if (strcmp(enc_name, "GBK") == 0 || strcmp(enc_name, "GB2312") == 0) {
    /* GBK 是 GB18030 的子集，参考实现（TextDecoder('gb18030')）也这么做 */
    m->encoding_name = dsh_mem_strdup("GB18030");
    m->encoding = DSH_TXT_GB18030;
  } else {
    m->encoding = dsh_text_encoding_from_name(enc_name);
    m->encoding_name = dsh_mem_strdup(
        m->encoding == DSH_TXT_UTF16LE ? "UTF-16" :
        m->encoding == DSH_TXT_GB18030 ? "GB18030" :
        m->encoding == DSH_TXT_BIG5 ? "BIG5" : "UTF-8");
  }
  dsh_release(enc_name);

  /* 四个偏移的名字必须与「它指向哪一段的**起点**」严格对应 —— 这里栽过一次：
   * key_info_offset 其实是**键区头部**的起点，当成「键信息块起点」会让后面每一段整体前移一节。
   *   header_len+8 → 键区头部 | key_info_offset → 键信息块 | key_block_offset → 词块数据
   *   record_header_offset → 记录区头部 | record_info_offset → 记录信息块 | record_block_offset → 记录块数据 */
  m->key_header_offset = header_len + 8;
  return 0;
}

/* ── 键区 / 记录区头部与信息块 ─────────────────────────────────────────── */

static int read_key_header(dsh_mdx *m) {
  const int meta_size = (m->version >= 2.0) ? 8 * 5 : 4 * 4;
  const int w = m->num_width;
  const uint8_t *buf = view_at(m, m->key_header_offset, (size_t)meta_size, "read_key_header（键区头部）");
  if (buf == NULL) return -1;

  const int64_t a = read_be(buf, (size_t)meta_size, 0, w);
  const int64_t b = read_be(buf, (size_t)meta_size, (size_t)w, w);
  /* `buf` 是映射的只读视图，**不释放** */
  if (a < 0 || b < 0) return -1;
  m->key_block_count = a;
  m->key_count = b;

  size_t off = (size_t)w * 2;
  if (m->version >= 2.0) {
    const int64_t unpack = read_be(buf, (size_t)meta_size, off, w);
    if (unpack < 0) return -1;
    m->key_info_unpack_size = unpack;
    off += (size_t)w;
  }
  const int64_t packed = read_be(buf, (size_t)meta_size, off, w);
  const int64_t blocks = read_be(buf, (size_t)meta_size, off + (size_t)w, w);
  if (packed < 0 || blocks < 0) return -1;
  m->key_info_packed_size = packed;
  m->key_block_packed_size = blocks;

  if (m->key_block_count < 0 || m->key_block_count > 10000000) {
    dsh_set_last_error("词块数非法：%lld", (long long)m->key_block_count);
    return -1;
  }
  if (m->key_info_packed_size <= 0 || m->key_info_packed_size > (int64_t)1 << 31) {
    dsh_set_last_error("键信息块大小非法：%lld", (long long)m->key_info_packed_size);
    return -1;
  }
  /* 键信息块**解压后**的大小只是个提示（zlib 那条路拿它当初始缓冲的规模），但它同样是文件
   * 说了算的数：不查它，一个声明 2^53 的坏文件会让解压器一上来就按它要缓冲。 */
  if (!block_unpack_ok(m->key_info_unpack_size)) {
    dsh_set_last_error("键信息块解压后大小非法：%lld（上限 %lld 字节）",
                       (long long)m->key_info_unpack_size,
                       (long long)DSH_MDX_MAX_BLOCK_UNPACK);
    return -1;
  }

  /* 每一段都从**上一段的末尾**接下去算（偏移名字见 read_header 末尾）。 */
  m->key_info_offset = m->key_header_offset + meta_size + (m->version >= 2.0 ? 4 : 0);
  m->key_block_offset = m->key_info_offset + m->key_info_packed_size;
  m->record_header_offset = m->key_block_offset + m->key_block_packed_size;

  /* 键区两段都必须整段落在文件里。越界的事在这里说清 —— 留到「读第 N 块」时才报的话，
   * 报出来的是**某一项的偏移**，看不出真正坏掉的是键区头部声明的大小。 */
  if (!range_inside_file(m, 0, m->key_info_offset, m->key_info_packed_size) ||
      !range_inside_file(m, 0, m->key_block_offset, m->key_block_packed_size)) {
    dsh_set_last_error(
        "键区声明的大小把文件读穿了：键信息块 [%lld, +%lld)／词块数据 [%lld, +%lld)，"
        "而文件只有 %lld 字节",
        (long long)m->key_info_offset, (long long)m->key_info_packed_size,
        (long long)m->key_block_offset, (long long)m->key_block_packed_size,
        (long long)m->map_len);
    return -1;
  }
  return 0;
}

/* ── 加密块（Encrypted=1 / =2）────────────────────────────────────────────
 * ① key = RIPEMD128(块[4..8] + 95 36 00 00)   ② 对块[8..] 做 fast_decrypt（异或 + 半字节交换）
 * **前 8 字节原样保留**：压缩类型与 adler32 在密文里也是明文，而密钥恰恰来自那 4 字节
 * adler32 —— 先解前面会把密钥一起改掉。约定与参考实现的 MdictCrypto.cs 一致，不许顺手改良。
 */

/** MDict 的 fast_decrypt：对 data[0..len) 解密，key 循环使用，previous 初值 0x36 */
static void fast_decrypt(const uint8_t *data, size_t len, const uint8_t key[16],
                         uint8_t *out) {
  uint8_t previous = 0x36;
  for (size_t i = 0; i < len; i++) {
    const uint8_t raw = data[i];
    uint8_t t = (uint8_t)((raw >> 4) | (raw << 4));
    t = (uint8_t)(t ^ previous ^ (uint8_t)(i & 0xffu) ^ key[i % 16]);
    previous = raw;
    out[i] = t;
  }
}

/** 解开一个被加密的块。返回新分配的整块副本（前 8 字节与原块相同），失败返回 NULL */
static uint8_t *mdx_decrypt(const uint8_t *block, size_t len) {
  if (len < 8) {
    dsh_set_last_error("加密块只有 %zu 字节，不足 8 字节（派生不出密钥）", len);
    return NULL;
  }
  uint8_t key_in[8];
  key_in[0] = block[4];
  key_in[1] = block[5];
  key_in[2] = block[6];
  key_in[3] = block[7];
  key_in[4] = 0x95;
  key_in[5] = 0x36;
  key_in[6] = 0x00;
  key_in[7] = 0x00;

  uint8_t key[16];
  dsh_ripemd128(key_in, sizeof(key_in), key); /* 这条路没有失败模式，不分配 */

  uint8_t *out = (uint8_t *)dsh_mem_alloc(len);
  if (out == NULL) {
    dsh_set_last_error("内存不足：解密块需要 %zu 字节", len);
    return NULL;
  }
  memcpy(out, block, 8);
  fast_decrypt(block + 8, len - 8, key, out + 8);
  return out;
}

/* 解开键信息块的压缩。顺序与参考实现一致：**先解密、再片段、最后解压**；而且只有
 * Encrypted=2 才解键信息块 —— Encrypted=1 说的是「记录块加密」，键信息块在那种词典里是明文。
 * **本函数消费掉入参 packed**：v2.0 各分支都把它释放掉，v1.2 原样返回它。 */
static uint8_t *decode_key_info_block(dsh_mdx *m, uint8_t *packed, size_t packed_len, size_t *out_len) {
  if (!(m->version >= 2.0)) {
    *out_len = packed_len; /* v1.2 不压缩、也没有 4 字节压缩类型 */
    return packed;
  }
  if (packed_len < 8) {
    dsh_set_last_error("键信息块长度不足 8 字节");
    return NULL;
  }
  const uint32_t ctype = ((uint32_t)packed[0] << 24) | ((uint32_t)packed[1] << 16) |
                         ((uint32_t)packed[2] << 8) | packed[3];
  const uint32_t checksum = ((uint32_t)packed[4] << 24) | ((uint32_t)packed[5] << 16) |
                            ((uint32_t)packed[6] << 8) | packed[7];

  uint8_t *buffer = packed;
  if (m->encrypted == 2) {
    buffer = mdx_decrypt(packed, packed_len);
    if (buffer == NULL) return NULL;
    dsh_release(packed);
  }

  const uint8_t *payload = buffer + 8;
  const size_t payload_len = packed_len - 8;

  if (ctype == DSH_COMPRESS_NONE) {
    /* ⚠️ 返回的是**去掉 8 字节头**的那一段，不是整块。这里不做零拷贝引用（buffer + 8 不能
     * 交给 dsh_release），拷一份小缓冲，换来「谁返回谁释放」在所有分支上都成立。 */
    uint8_t *copy = (uint8_t *)dsh_mem_alloc(payload_len ? payload_len : 1);
    if (copy == NULL) {
      dsh_release(buffer);
      dsh_set_last_error("内存不足：键信息块副本");
      return NULL;
    }
    memcpy(copy, payload, payload_len);
    dsh_release(buffer);
    *out_len = payload_len;
    return copy;
  }
  if (ctype == DSH_COMPRESS_ZLIB) {
    uint8_t *out = NULL;
    size_t n = 0;
    if (dsh_zlib_inflate(payload, payload_len, (size_t)m->key_info_unpack_size, &out, &n) != 0) {
      dsh_release(buffer);
      return NULL; /* last_error 已由 inflate 写好 */
    }
    verify_checksum(m, checksum, out, n, "键信息块");
    dsh_release(buffer);
    *out_len = n;
    return out;
  }
  if (ctype == DSH_COMPRESS_LZO) {
    uint8_t *out = NULL;
    size_t n = 0;
    const int rc = dsh_lzo1x_decompress(payload, payload_len, &out, &n);
    dsh_release(buffer);
    if (rc != DSH_LZO1X_OK) return NULL; /* last_error 已由 LZO1X 写好（人话） */
    verify_checksum(m, checksum, out, n, "键信息块");
    *out_len = n;
    return out;
  }
  dsh_release(buffer);
  dsh_set_last_error("无法识别的键信息块压缩类型：0x%08X", ctype);
  return NULL;
}

static int read_key_infos(dsh_mdx *m) {
  uint8_t *packed = read_at(m, m->key_info_offset, (size_t)m->key_info_packed_size, "read_key_infos（键信息块）");
  if (packed == NULL) return -1;
  size_t buf_len = 0;
  uint8_t *buf = decode_key_info_block(m, packed, (size_t)m->key_info_packed_size, &buf_len);
  if (buf == NULL) return -1;

  const int w = m->num_width;
  const int ws = w / 4; /* 词条名长度字段的宽度：v2.0 = 2 字节，v1.2 = 1 字节 */
  const int64_t count = m->key_block_count;

  /* ★★ 先按信息块的**真实长度**推导「最多能有几个词块」，**再**分配 —— 这是 F3 的正面。
   * 每一项至少占：词条数(w) + 首词长(ws) + 尾词长(ws) + 压缩大小(w) + 解压大小(w)
   * = 2w + 2ws 字节（首尾词本身的字节数只会更多，所以这是个**下界**，只会放不会卡）。
   * 声明条数超过这个下界推出来的容量，说明信息块**根本没有那么多内容** ——
   * 只信声明值的话，346 字节的文件就能让解析器按声明值要 640,000,000 字节。 */
  const int64_t per_entry_min = (int64_t)(2 * w + 2 * ws);
  const int64_t max_by_bytes = (int64_t)(buf_len / (size_t)per_entry_min);
  if (count > max_by_bytes) {
    dsh_release(buf);
    dsh_set_last_error(
        "键信息块声明有 %lld 个词块，而它解出来只有 %zu 字节（每个词块至少 %lld 字节）"
        "—— 声明与内容不符",
        (long long)count, buf_len, (long long)per_entry_min);
    return -1;
  }
  if (!index_budget_ok("词块索引数组", count, sizeof(dsh_mdx_key_block))) {
    dsh_release(buf);
    return -1;
  }

  dsh_mdx_key_block *blocks =
      (dsh_mdx_key_block *)dsh_mem_alloc((size_t)(count > 0 ? count : 1) * sizeof(*blocks));
  if (blocks == NULL) {
    dsh_release(buf);
    dsh_set_last_error("内存不足：词块索引需要 %lld 项", (long long)count);
    return -1;
  }
  memset(blocks, 0, (size_t)(count > 0 ? count : 1) * sizeof(*blocks));

  size_t off = 0;
  int64_t entries_acc = 0, pack_acc = 0, unpack_acc = 0;
  int64_t failed_at = -1;  /* 读不下去的那一块（报错时点名，别让人自己数） */
  const char *fail_reason = NULL; /* 新加的那几关失败时的人话（老路径保持原文案） */
  char fail_detail[192];          /* 原因里带上具体数字：只报「坏了」看不出坏成什么样 */
  fail_detail[0] = '\0';
  int ok = 1;
  for (int64_t i = 0; i < count && ok; i++) {
    dsh_mdx_key_block *kb = &blocks[i];
    failed_at = i;
    kb->entry_count = read_be(buf, buf_len, off, w);
    off += (size_t)w;
    if (kb->entry_count < 0) { ok = 0; break; }
    /* 单个词块声明的词条数也要过条数上限：它是「块内读满就停」的判据，天文数字没有意义 */
    if (kb->entry_count > DSH_MDX_MAX_ENTRIES) {
      snprintf(fail_detail, sizeof(fail_detail), "声明的词条数 %lld 超出上限 %lld",
               (long long)kb->entry_count, (long long)DSH_MDX_MAX_ENTRIES);
      fail_reason = fail_detail;
      ok = 0;
      break;
    }

    int64_t first_size = read_be(buf, buf_len, off, ws);
    off += (size_t)ws;
    /* v2.0 的长度里已经含了结尾的 \0（UTF-16 再 ×2）—— 漏了这一步，首尾词会整体串位，
     * 症状是「索引区间全错、什么词都查不到」。 */
    if (m->version >= 2.0) {
      first_size = (m->encoding == DSH_TXT_UTF16LE) ? (first_size + 1) * 2 : first_size + 1;
    } else {
      first_size = (m->encoding == DSH_TXT_UTF16LE) ? first_size * 2 : first_size;
    }
    if (first_size < 0 || off + (size_t)first_size > buf_len) { ok = 0; break; }
    kb->first_key = decode_text(m, buf + off, (size_t)first_size, NULL);
    off += (size_t)first_size;

    int64_t last_size = read_be(buf, buf_len, off, ws);
    off += (size_t)ws;
    if (m->version >= 2.0) {
      last_size = (m->encoding == DSH_TXT_UTF16LE) ? (last_size + 1) * 2 : last_size + 1;
    } else {
      last_size = (m->encoding == DSH_TXT_UTF16LE) ? last_size * 2 : last_size;
    }
    if (last_size < 0 || off + (size_t)last_size > buf_len) { ok = 0; break; }
    kb->last_key = decode_text(m, buf + off, (size_t)last_size, NULL);
    off += (size_t)last_size;

    kb->pack_size = read_be(buf, buf_len, off, w);
    off += (size_t)w;
    kb->unpack_size = read_be(buf, buf_len, off, w);
    off += (size_t)w;

    /* TrimNul：键里可能带结尾的 \0（长度字段含了它） */
    if (kb->first_key != NULL) {
      const size_t n = strlen(kb->first_key);
      if (n > 0 && kb->first_key[n - 1] == '\0') kb->first_key[n - 1] = '\0';
      /* UTF-16 的 \0 解成 UTF-8 也是 \0，上面的 strlen 已经截断了，这里只是澄清 */
    }
    if (kb->first_key == NULL || kb->last_key == NULL) { ok = 0; break; }
    if (kb->pack_size < 0 || kb->unpack_size < 0) { ok = 0; break; }
    /* 单块解压上限：文件说了算的数，超过就当格式错误（真词典的块是几十 KB 量级） */
    if (!block_unpack_ok(kb->unpack_size)) {
      snprintf(fail_detail, sizeof(fail_detail), "声明的解压后大小 %lld 超出单块上限 %lld 字节",
               (long long)kb->unpack_size, (long long)DSH_MDX_MAX_BLOCK_UNPACK);
      fail_reason = fail_detail;
      ok = 0;
      break;
    }
    /* 这一块的压缩数据必须整段落在文件里 —— 越界不要留到「读第 N 块」时才报 */
    if (!range_inside_file(m, m->key_block_offset, pack_acc, kb->pack_size)) {
      snprintf(fail_detail, sizeof(fail_detail),
               "压缩数据（基址 %lld + 偏移 %lld，长 %lld）落在 %lld 字节的文件之外",
               (long long)m->key_block_offset, (long long)pack_acc, (long long)kb->pack_size,
               (long long)m->map_len);
      fail_reason = fail_detail;
      ok = 0;
      break;
    }

    kb->pack_offset = pack_acc;
    kb->unpack_offset = unpack_acc;
    kb->entry_offset = entries_acc;
    /* ★★ 三处累加都**先查后加**（F2 同样出在词块这一侧：单项合法不等于总和合法） */
    if (acc_would_overflow(entries_acc, kb->entry_count) ||
        acc_would_overflow(pack_acc, kb->pack_size) ||
        acc_would_overflow(unpack_acc, kb->unpack_size)) {
      snprintf(fail_detail, sizeof(fail_detail),
               "累计长度加溢出（词条 %lld+%lld／压缩 %lld+%lld／解压 %lld+%lld）",
               (long long)entries_acc, (long long)kb->entry_count, (long long)pack_acc,
               (long long)kb->pack_size, (long long)unpack_acc, (long long)kb->unpack_size);
      fail_reason = fail_detail;
      ok = 0;
      break;
    }
    entries_acc += kb->entry_count;
    pack_acc += kb->pack_size;
    unpack_acc += kb->unpack_size;
    /* 总量预算：解压总量相对文件长度说不通时也是格式错误，不是警告 */
    if (unpack_acc > unpack_total_cap(m)) {
      snprintf(fail_detail, sizeof(fail_detail),
               "解压总长度 %lld 超出「文件长度 %lld × %lld + 余量」的口径，与文件大小不符",
               (long long)unpack_acc, (long long)m->map_len, (long long)DSH_MDX_UNPACK_RATIO);
      fail_reason = fail_detail;
      ok = 0;
      break;
    }
  }

  /* 无论成败都要把已经建好的索引还给 m（失败时由 close 释放，免得泄漏） */
  m->key_blocks = blocks;
  if (!ok) {
    dsh_release(buf);
    /* 老路径（字段读不下去）保持原话术不动 —— 它被别的检查标准盯着；新加的那几关带上原因 */
    if (fail_reason != NULL) {
      dsh_set_last_error("键信息块内容损坏：第 %lld 个词块%s（已解出 %zu 字节）",
                         (long long)failed_at, fail_reason, buf_len);
    } else {
      dsh_set_last_error("键信息块内容损坏：第 %lld 个词块的条目读不下去（已解出 %zu 字节）",
                         (long long)failed_at, buf_len);
    }
    return -1;
  }

  if (pack_acc != m->key_block_packed_size) {
    warn(m, "词块压缩总大小 %lld 与键区头部记录 %lld 不一致",
         (long long)pack_acc, (long long)m->key_block_packed_size);
  }
  if (entries_acc != m->key_count) {
    warn(m, "键信息块里统计的词条数 %lld 与键区头部记录 %lld 不一致",
         (long long)entries_acc, (long long)m->key_count);
  }

  /* ⚠️ **只释放 `buf` 这一块，绝不再单独释放 `packed`**：解码器已经消费掉 packed
   * （v1.2 时 buf == packed，v2.0 时 packed 已被解码器释放），再放一次是双重释放，
   * 会把**活着的** blocks 从活分配表里注销掉 —— 症状只在 .mdd 上暴露。 */
  dsh_release(buf);
  return 0;
}

static int read_record_infos(dsh_mdx *m) {
  const int len = (m->version >= 2.0) ? 4 * 8 : 4 * 4;
  const int w = m->num_width;
  const uint8_t *buf = view_at(m, m->record_header_offset, (size_t)len, "read_record_infos（记录区头部）");
  if (buf == NULL) return -1;

  int64_t v[4] = {0, 0, 0, 0};
  for (int i = 0; i < 4; i++) {
    v[i] = read_be(buf, (size_t)len, (size_t)i * (size_t)w, w);
    /* `buf` 是映射的只读视图，**不释放** */
    if (v[i] < 0) return -1;
  }
  m->record_block_count = v[0];
  m->record_entries_num = v[1];
  m->record_info_comp_size = v[2];
  m->record_block_comp_size = v[3];

  if (m->record_entries_num != m->key_count) {
    warn(m, "记录区头部记录的词条数 %lld 与键区 %lld 不一致",
         (long long)m->record_entries_num, (long long)m->key_count);
  }
  if (m->record_block_count < 0 || m->record_block_count > 10000000) {
    dsh_set_last_error("记录块数非法：%lld", (long long)m->record_block_count);
    return -1;
  }

  /* 记录块数据紧接着记录信息块之后 */
  m->record_info_offset = m->record_header_offset + len;
  m->record_block_offset = m->record_info_offset + m->record_info_comp_size;

  /* ★★ 索引项数必须与**索引自己的字节长度**对得上：每项固定占 pack_size(w) + unpack_size(w)
   * = 2w 字节，所以「声明块数 > 信息块字节数 / 2w」时，那份信息块根本装不下这么多项 ——
   * 这是**先于分配**就能判定的格式错误（F3 的同一条通则：先按真实长度算上限再分配）。 */
  const int64_t rec_entry_min = (int64_t)(2 * w);
  const int64_t rec_max_by_bytes = (int64_t)(m->record_info_comp_size / rec_entry_min);
  if (m->record_block_count > rec_max_by_bytes) {
    dsh_set_last_error("记录信息块声明有 %lld 个记录块，而它只有 %lld 字节（每块 %lld 字节）"
                       "—— 声明与内容不符",
                       (long long)m->record_block_count,
                       (long long)m->record_info_comp_size, (long long)rec_entry_min);
    return -1;
  }
  if (!index_budget_ok("记录块索引数组", m->record_block_count, sizeof(dsh_mdx_record_block))) {
    return -1;
  }
  /* 记录块数据整段都要落在文件里（压缩区间在后面逐项再查一遍） */
  if (!range_inside_file(m, 0, m->record_block_offset, m->record_block_comp_size)) {
    dsh_set_last_error("记录块数据把文件读穿了：记录块数据区 [%lld, +%lld)，文件只有 %lld 字节",
                       (long long)m->record_block_offset,
                       (long long)m->record_block_comp_size, (long long)m->map_len);
    return -1;
  }

  if (m->record_block_count > 0) {
    const uint8_t *info = view_at(m,
                            m->record_info_offset,
                            (size_t)m->record_info_comp_size,
                            "read_record_header（记录信息块）");
    if (info == NULL) return -1;
    dsh_mdx_record_block *rb = (dsh_mdx_record_block *)dsh_mem_alloc(
        (size_t)m->record_block_count * sizeof(*rb));
    if (rb == NULL) {
      dsh_set_last_error("内存不足：记录块索引需要 %lld 项", (long long)m->record_block_count);
      return -1;
    }
    memset(rb, 0, (size_t)m->record_block_count * sizeof(*rb));
    size_t off = 0;
    int64_t pack_acc = 0, unpack_acc = 0;
    int64_t failed_at = -1;
    const char *fail_reason = NULL;
    char fail_detail[192];
    fail_detail[0] = '\0';
    int ok = 1;
    for (int64_t i = 0; i < m->record_block_count; i++) {
      failed_at = i;
      rb[i].pack_size = read_be(info, (size_t)m->record_info_comp_size, off, w);
      off += (size_t)w;
      rb[i].unpack_size = read_be(info, (size_t)m->record_info_comp_size, off, w);
      off += (size_t)w;
      if (rb[i].pack_size < 0 || rb[i].unpack_size < 0) { ok = 0; break; }
      /* 单块解压上限（真词典的块是几十 KB 量级；512MB 与 dsh_inflate 的输出上限取齐） */
      if (!block_unpack_ok(rb[i].unpack_size)) {
        snprintf(fail_detail, sizeof(fail_detail),
                 "声明的解压后大小 %lld 超出单块上限 %lld 字节",
                 (long long)rb[i].unpack_size, (long long)DSH_MDX_MAX_BLOCK_UNPACK);
        fail_reason = fail_detail;
        ok = 0;
        break;
      }
      /* 这一块的压缩数据必须整段落在文件里 */
      if (!range_inside_file(m, m->record_block_offset, pack_acc, rb[i].pack_size)) {
        snprintf(fail_detail, sizeof(fail_detail),
                 "压缩数据（基址 %lld + 偏移 %lld，长 %lld）落在 %lld 字节的文件之外",
                 (long long)m->record_block_offset, (long long)pack_acc,
                 (long long)rb[i].pack_size, (long long)m->map_len);
        fail_reason = fail_detail;
        ok = 0;
        break;
      }
      rb[i].pack_offset = pack_acc;
      rb[i].unpack_offset = unpack_acc;
      /* ★★ F2 的正面：**先查后加**。每一项单独看都合法（read_be 允许到 2^53），
       * 但「单项合法」不等于「总和合法」—— 直接相加那一步就是 UBSan 报的
       * signed integer overflow，回绕出来的负数还会让后面所有偏移检查静默失效。 */
      if (acc_would_overflow(pack_acc, rb[i].pack_size) ||
          acc_would_overflow(unpack_acc, rb[i].unpack_size)) {
        snprintf(fail_detail, sizeof(fail_detail),
                 "累计长度加溢出（压缩 %lld+%lld／解压 %lld+%lld）", (long long)pack_acc,
                 (long long)rb[i].pack_size, (long long)unpack_acc,
                 (long long)rb[i].unpack_size);
        fail_reason = fail_detail;
        ok = 0;
        break;
      }
      pack_acc += rb[i].pack_size;
      unpack_acc += rb[i].unpack_size;
      /* 解压总量与文件大小差了好几个量级 → 格式错误，不是警告 */
      if (unpack_acc > unpack_total_cap(m)) {
        snprintf(fail_detail, sizeof(fail_detail),
                 "解压总长度 %lld 超出「文件长度 %lld × %lld + 余量」的口径，与文件大小不符",
                 (long long)unpack_acc, (long long)m->map_len, (long long)DSH_MDX_UNPACK_RATIO);
        fail_reason = fail_detail;
        ok = 0;
        break;
      }
    }
    m->record_blocks = rb;
    if (!ok) {
      if (fail_reason != NULL) {
        dsh_set_last_error("记录信息块内容损坏：第 %lld 项读不下去：%s", (long long)failed_at,
                           fail_reason);
      } else {
        dsh_set_last_error("记录信息块内容损坏（第 %lld 项读不下去）", (long long)failed_at);
      }
      return -1;
    }
    if (pack_acc != m->record_block_comp_size) {
      warn(m, "记录块压缩总大小 %lld 与记录区头部记录 %lld 不一致",
           (long long)pack_acc, (long long)m->record_block_comp_size);
    }
  }

  /* 相邻词块的首尾词是否单调；不单调时查询退化成全块扫描 */
  m->block_order_monotone = 1;
  for (int64_t i = 1; i < m->key_block_count; i++) {
    const char *prev = m->key_blocks[i - 1].last_key;
    const char *cur = m->key_blocks[i].first_key;
    if (prev != NULL && cur != NULL && strcmp(prev, cur) > 0) {
      m->block_order_monotone = 0;
      warn(m, "第 %lld 个词块的首词在序数序上早于前一块的尾词，查询会退化为全块扫描",
           (long long)i);
      break;
    }
  }
  return 0;
}

/* ── 打开 / 关闭 ────────────────────────────────────────────────────────── */

int dsh_mdx_open(const char *path, dsh_mdx **out) {
  if (path == NULL || out == NULL) {
    dsh_set_last_error("参数不合法：path/out 不能为空");
    return -1;
  }
  *out = NULL;

  FILE *fp = dsh_file_open_read(path);
  if (fp == NULL) {
    dsh_set_last_error("打不开文件：%s", path);
    return -1;
  }

  dsh_mdx *m = (dsh_mdx *)dsh_mem_alloc(sizeof(dsh_mdx));
  if (m == NULL) {
    fclose(fp);
    dsh_set_last_error("内存不足：解析器句柄");
    return -1;
  }
  memset(m, 0, sizeof(*m));
  m->fp = fp;
  m->path = dsh_mem_strdup(path);
  if (m->path == NULL) {
    dsh_mdx_close(m);
    dsh_set_last_error("内存不足：路径副本");
    return -1;
  }

  /* 扩展名决定 .mdd / .mdx（.mdd 的编码强制 UTF-16LE、键的归一化规则也不同） */
  const size_t n = strlen(path);
  m->is_mdd = (n >= 4) &&
              (path[n - 4] == '.') &&
              (path[n - 3] == 'm' || path[n - 3] == 'M') &&
              (path[n - 2] == 'd' || path[n - 2] == 'D') &&
              (path[n - 1] == 'd' || path[n - 1] == 'D');

  /* 建一次只读映射：之后所有读都是指针算术。⚠️ 映射失败是**致命**的 —— 宁可当场说清
   * 「映射建不起来」，也不要悄悄退回一条已知会给出脏数据的逐段读路径。 */
  if (dsh_map_open(path, &m->map, &m->map_len) != 0) {
    dsh_mdx_close(m);
    dsh_set_last_error("无法把词典映射进内存（文件为空、被占用、或文件系统不支持）：%s", path);
    return -1;
  }
  m->map_base = dsh_map_base(m->map);

  int rc = read_header(m);
  if (rc == 0) rc = read_key_header(m);
  if (rc == 0) rc = read_key_infos(m);
  if (rc == 0) rc = read_record_infos(m);

  if (rc != 0) {
    /* 失败时如实保留 last_error，把已分配的东西全部释放 */
    const char *saved = dsh_last_error_message();
    dsh_mdx_close(m);
    if (saved != NULL) {
      dsh_set_last_error("%s", saved);
      dsh_release((void *)saved);
    }
    return rc;
  }

  *out = m;
  return 0;
}

void dsh_mdx_close(dsh_mdx *m) {
  if (m == NULL) return;
  /* ⚠️ 顺序：**先解映射，再释放别的东西** —— 解掉之后任何视图指针都悬空了。 */
  if (m->map != NULL) {
    dsh_map_close(m->map);
    m->map = NULL;
    m->map_base = NULL;
    m->map_len = 0;
  }
  if (m->key_blocks != NULL) {
    for (int64_t i = 0; i < m->key_block_count; i++) {
      if (m->key_blocks[i].first_key != NULL) dsh_release(m->key_blocks[i].first_key);
      if (m->key_blocks[i].last_key != NULL) dsh_release(m->key_blocks[i].last_key);
    }
    dsh_release(m->key_blocks);
  }
  if (m->record_blocks != NULL) dsh_release(m->record_blocks);
  for (int i = 0; i < m->warning_count; i++) {
    if (m->warnings[i] != NULL) dsh_release(m->warnings[i]);
  }
  if (m->raw_header != NULL) dsh_release(m->raw_header);
  if (m->encoding_name != NULL) dsh_release(m->encoding_name);
  if (m->title != NULL) dsh_release(m->title);
  if (m->path != NULL) dsh_release(m->path);
  if (m->fp != NULL) fclose(m->fp);
  dsh_release(m);
}

/* ── 元信息访问 ─────────────────────────────────────────────────────────── */

const char *dsh_mdx_path(const dsh_mdx *m) { return m->path; }
int dsh_mdx_is_mdd(const dsh_mdx *m) { return m->is_mdd; }
double dsh_mdx_version(const dsh_mdx *m) { return m->version; }
int dsh_mdx_encrypted(const dsh_mdx *m) { return m->encrypted; }
const char *dsh_mdx_encoding_name(const dsh_mdx *m) { return m->encoding_name; }
const char *dsh_mdx_title(const dsh_mdx *m) { return m->title != NULL ? m->title : ""; }
int64_t dsh_mdx_key_count(const dsh_mdx *m) { return m->key_count; }
int64_t dsh_mdx_key_block_count(const dsh_mdx *m) { return m->key_block_count; }
int dsh_mdx_block_order_monotone(const dsh_mdx *m) { return m->block_order_monotone; }
int dsh_mdx_warning_count(const dsh_mdx *m) { return m->warning_count; }
const char *dsh_mdx_warning_at(const dsh_mdx *m, int index) {
  if (index < 0 || index >= m->warning_count) return "";
  return m->warnings[index];
}

int64_t dsh_mdx_key_block_at(const dsh_mdx *m, int64_t index, dsh_mdx_key_block *out) {
  if (index < 0 || index >= m->key_block_count || out == NULL) return -1;
  *out = m->key_blocks[index];
  return 0;
}

/* 取词块里第 idx 条的记录偏移（越过 idx 条之后顺带给出「下一条偏移」，没有则 -1） */
static int record_offsets_in_block(dsh_mdx *m, int64_t block_index, int64_t idx,
                                   int64_t *out_start, int64_t *out_next) {
  uint8_t *buf = NULL;
  size_t len = 0;
  if (dsh_mdx_read_key_block(m, block_index, &buf, &len) != 0) return -1;

  const int w = m->num_width;
  const int key_width = (m->encoding == DSH_TXT_UTF16LE || m->is_mdd) ? 2 : 1;
  size_t off = 0;
  int64_t seen = 0;
  int rc = -1;
  *out_start = -1;
  *out_next = -1;

  while (off + (size_t)w < len) {
    const int64_t rec = read_be(buf, len, off, w);
    const size_t key_start = off + (size_t)w;
    size_t key_end = (size_t)-1;
    for (size_t i = key_start; i + (size_t)key_width <= len; i += (size_t)key_width) {
      if (key_width == 1) {
        if (buf[i] == 0) { key_end = i; break; }
      } else if (buf[i] == 0 && buf[i + 1] == 0) {
        key_end = i;
        break;
      }
    }
    if (key_end == (size_t)-1) break;
    off = key_end + (size_t)key_width;

    if (seen == idx) {
      *out_start = rec;
      /* 下一条的偏移（同块内）；没有下一条就留 -1，由调用方跨块处理 */
      if (off + (size_t)w < len) *out_next = read_be(buf, len, off, w);
      rc = 0;
      break;
    }
    seen++;
  }

  dsh_release(buf);
  return rc;
}

int dsh_mdx_record_range(dsh_mdx *m, int64_t global_index, int64_t *out_start, int64_t *out_end) {
  if (m == NULL || out_start == NULL || out_end == NULL) {
    dsh_set_last_error("参数不合法：dsh_mdx_record_range 收到了空指针");
    return -1;
  }
  *out_start = -1;
  *out_end = -1;
  if (global_index < 0 || global_index >= m->key_count) {
    dsh_set_last_error("词条下标越界：%lld（共 %lld 条）",
                       (long long)global_index, (long long)m->key_count);
    return -1;
  }

  int64_t entry_in_block = 0, bi = -1;
  if (locate_entry(m, global_index, &entry_in_block, &bi) != 0) {
    dsh_set_last_error("找不到第 %lld 条词条所属的词块", (long long)global_index);
    return -1;
  }

  int64_t start = -1, next = -1;
  if (record_offsets_in_block(m, bi, entry_in_block, &start, &next) != 0) {
    dsh_set_last_error("词块 %lld 里读不到第 %lld 条的记录偏移",
                       (long long)bi, (long long)entry_in_block);
    return -1;
  }

  /* 结束位置三档：① 同块有下一条 → 用它的偏移；② 没有但还有下一块 → 用那块**首条**的
   * 偏移；③ 最后一条 → 用记录区解压总大小。与参考实现的 GetDefinition 逐条对应。 */
  int64_t end = -1;
  if (next >= 0) {
    end = next;
  } else if (bi + 1 < m->key_block_count) {
    int64_t first_of_next = -1, dummy = -1;
    if (record_offsets_in_block(m, bi + 1, 0, &first_of_next, &dummy) != 0) return -1;
    end = (first_of_next >= 0) ? first_of_next : dsh_mdx_total_record_unpacked(m);
  } else {
    end = dsh_mdx_total_record_unpacked(m);
  }

  *out_start = start;
  *out_end = end;
  return 0;
}

/* 把记录区 `[start, end)` **原样拼出来**（跨块拼接、解压，**不解码**）。
 *
 * ⚠️ 为什么必须有一条「不解码」的路：`.mdx` 的记录是文本，按文件编码解一遍是对的；
 * 而 `.mdd` 的记录是 **PNG/MP3/SPX 这类二进制**，拿 UTF-16LE 解码器过一遍就彻底毁了
 * （症状：`.mdd` 里的图片与音频全读不出来，而 CSS 这种纯文本看着「没坏」，所以很难发现）。 */
static int read_raw_range(dsh_mdx *m, int64_t start, int64_t end, uint8_t **out_bytes,
                          size_t *out_len) {
  *out_bytes = NULL;
  *out_len = 0;
  if (end <= start) {
    uint8_t *empty = (uint8_t *)dsh_mem_alloc(1);
    if (empty == NULL) {
      dsh_set_last_error("内存不足：空记录");
      return -1;
    }
    empty[0] = '\0';
    *out_bytes = empty;
    *out_len = 0;
    return 0;
  }

  /* ⚠️ 跨块的记录要**先拼字节、再交给需要解码的调用方**，不能分块各自解码后拼字符串 ——
   * 多字节编码（UTF-8 的中文、UTF-16 的代理对）会在块边界上被切断，解出替换字符。 */
  size_t total = (size_t)(end - start);
  uint8_t *joined = (uint8_t *)dsh_mem_alloc(total);
  if (joined == NULL) {
    dsh_set_last_error("内存不足：拼接记录需要 %zu 字节", total);
    return -1;
  }
  size_t filled = 0;
  for (int64_t i = 0; i < m->record_block_count && filled < total; i++) {
    const dsh_mdx_record_block *rb = &m->record_blocks[i];
    const int64_t rb_start = rb->unpack_offset;
    const int64_t rb_end = rb_start + rb->unpack_size;
    if (end <= rb_start || start >= rb_end) continue; /* 不相交 */

    const int64_t take_from = (start > rb_start) ? start : rb_start;
    const int64_t take_to = (end < rb_end) ? end : rb_end;
    const size_t want = (size_t)(take_to - take_from);
    if (want == 0) continue;

    const uint8_t *packed = view_at(m, m->record_block_offset + rb->pack_offset, (size_t)rb->pack_size, "读记录块");
    if (packed == NULL) { dsh_release(joined); return -1; }
    size_t blen = 0;
    uint8_t *block = decompress_block(m, packed, (size_t)rb->pack_size, (size_t)rb->unpack_size,
                                      i, 1, &blen);
    /* `packed` 是映射的只读视图，**不释放** */
    if (block == NULL) { dsh_release(joined); return -1; }

    const size_t off_in_block = (size_t)(take_from - rb_start);
    if (off_in_block + want > blen) {
      dsh_release(block);
      dsh_release(joined);
      dsh_set_last_error("记录块 %lld 解压后只有 %zu 字节，取不到 [%zu, %zu)",
                         (long long)i, blen, off_in_block, off_in_block + want);
      return -1;
    }
    memcpy(joined + filled, block + off_in_block, want);
    filled += want;
    dsh_release(block);
  }

  if (filled != total) {
    dsh_release(joined);
    dsh_set_last_error("记录区字节不足：要 %zu 字节，只凑到 %zu 字节", total, filled);
    return -1;
  }
  *out_bytes = joined;
  *out_len = total;
  return 0;
}

int dsh_mdx_read_record_raw(dsh_mdx *m, int64_t global_index, uint8_t **out_bytes,
                            size_t *out_len) {
  if (out_bytes == NULL || out_len == NULL) {
    dsh_set_last_error("参数不合法：dsh_mdx_read_record_raw 收到了空指针");
    return -1;
  }
  *out_bytes = NULL;
  *out_len = 0;
  int64_t start = -1, end = -1;
  if (dsh_mdx_record_range(m, global_index, &start, &end) != 0) return -1;
  return read_raw_range(m, start, end, out_bytes, out_len);
}

int dsh_mdx_fetch_raw(dsh_mdx *m, const char *key, char **out_key, uint8_t **out_bytes,
                      size_t *out_len) {
  if (out_key != NULL) *out_key = NULL;
  if (out_bytes != NULL) *out_bytes = NULL;
  if (out_len != NULL) *out_len = 0;
  if (m == NULL || key == NULL || key[0] == '\0') {
    dsh_set_last_error("参数不合法：键不能为空");
    return -1;
  }
  char *landed = NULL;
  int64_t global = -1;
  const int hit = lookup_full(m, key, &landed, NULL, &global);
  if (hit < 0) return -1;
  if (hit == 0) return 0;

  uint8_t *bytes = NULL;
  size_t len = 0;
  if (dsh_mdx_read_record_raw(m, global, &bytes, &len) != 0) {
    if (landed != NULL) dsh_release(landed);
    return -1;
  }
  if (out_key != NULL) *out_key = landed; else dsh_release(landed);
  if (out_bytes != NULL) *out_bytes = bytes; else dsh_release(bytes);
  if (out_len != NULL) *out_len = len;
  return 1;
}

int dsh_mdx_read_record_bytes(dsh_mdx *m, int64_t start, int64_t end, char **out_text,
                              int64_t *out_len) {
  if (out_text == NULL || out_len == NULL) {
    dsh_set_last_error("参数不合法：dsh_mdx_read_record_bytes 收到了空指针");
    return -1;
  }
  *out_text = NULL;
  *out_len = 0;
  if (end <= start) {
    *out_text = dsh_mem_strdup("");
    return (*out_text == NULL) ? -1 : 0;
  }

  uint8_t *joined = NULL;
  size_t total = 0;
  if (read_raw_range(m, start, end, &joined, &total) != 0) return -1;

  /* ⚠️ 记录的真值长度**只有一个来源：解码器自己数出来的字节数**，不许用 strlen ——
   * MDict 每条记录末尾带一个 \0，而「结束位置」是**下一条**记录的偏移，所以那个 \0 落在
   * 记录**里面**，strlen 会在它那儿截断（长度少 1、逐字节对照必红）。 */
  size_t decoded_len = 0;
  char *text = decode_text(m, joined, total, &decoded_len);
  dsh_release(joined);
  if (text == NULL) return -1;
  *out_text = text;
  *out_len = (int64_t)decoded_len;
  return 0;
}

/* ── 词块内容 ───────────────────────────────────────────────────────────── */

/** 解开一个词块（先按 pack_size 读进来，再按压缩类型解） */
static uint8_t *decompress_block(dsh_mdx *m, const uint8_t *packed, size_t packed_len,
                                 size_t unpack_size, int64_t block_index, int is_record,
                                 size_t *out_len) {
  if (packed_len < 8) {
    dsh_set_last_error("%s块 %lld 的长度不足 8 字节（%zu）",
                       is_record ? "记录" : "词", (long long)block_index, packed_len);
    return NULL;
  }
  const uint32_t ctype = ((uint32_t)packed[0] << 24) | ((uint32_t)packed[1] << 16) |
                         ((uint32_t)packed[2] << 8) | packed[3];
  const uint32_t checksum = ((uint32_t)packed[4] << 24) | ((uint32_t)packed[5] << 16) |
                            ((uint32_t)packed[6] << 8) | packed[7];
  const uint8_t *payload = packed + 8;
  const size_t payload_len = packed_len - 8;

  /* ① 未压缩的块一律不解密。② Encrypted=1 只加密**记录块**，词块在那种词典里仍是明文，
   *    所以下面那个条件必须带 is_record，否则会把词区也解成垃圾。 */
  if (ctype == DSH_COMPRESS_NONE) {
    uint8_t *copy = (uint8_t *)dsh_mem_alloc(payload_len ? payload_len : 1);
    if (copy == NULL) {
      dsh_set_last_error("内存不足：词块副本");
      return NULL;
    }
    memcpy(copy, payload, payload_len);
    *out_len = payload_len;
    return copy;
  }

  uint8_t *buffer = NULL;
  if (m->encrypted == 1 && is_record) {
    buffer = mdx_decrypt(packed, packed_len);
    if (buffer == NULL) return NULL;
  } else {
    buffer = (uint8_t *)dsh_mem_alloc(packed_len);
    if (buffer == NULL) {
      dsh_set_last_error("内存不足：块副本需要 %zu 字节", packed_len);
      return NULL;
    }
    memcpy(buffer, packed, packed_len);
  }
  const uint8_t *body = buffer + 8;
  const size_t body_len = packed_len - 8;

  uint8_t *out = NULL;
  size_t n = 0;
  int rc = 0;
  if (ctype == DSH_COMPRESS_ZLIB) {
    rc = dsh_zlib_inflate(body, body_len, unpack_size, &out, &n);
  } else if (ctype == DSH_COMPRESS_LZO) {
    rc = dsh_lzo1x_decompress(body, body_len, &out, &n);
  } else {
    dsh_release(buffer);
    dsh_set_last_error("%s块 %lld 的压缩类型无法识别：0x%08X",
                       is_record ? "记录" : "词", (long long)block_index, ctype);
    return NULL;
  }
  dsh_release(buffer);
  if (rc != 0) return NULL; /* last_error 已由解压器写好（人话） */

  if (n != unpack_size) {
    /* 只记警告 —— 但这条**很可能就是「格式理解错了」的第一个信号**，值得优先看一眼。 */
    warn(m, "第 %lld 个%s块解压后 %zu 字节，索引里写的是 %zu 字节",
         (long long)block_index, is_record ? "记录" : "词", n, unpack_size);
  }

  if (m->encrypted == 1 && is_record && checksum != 0 && mdx_adler32(out, n) != checksum) {
    dsh_release(out);
    dsh_set_last_error(
        "记录块 %lld 的 adler32 校验失败：该词典标记为 Encrypted=\"1\"，"
        "而它的密钥需要用户注册信息，无法用 MDict 的固定派生密钥解开",
        (long long)block_index);
    return NULL;
  }
  verify_checksum(m, checksum, out, n, is_record ? "记录块" : "词块");
  *out_len = n;
  return out;
}

int dsh_mdx_read_key_block(dsh_mdx *m, int64_t block_index, uint8_t **out_bytes, size_t *out_len) {
  if (out_bytes == NULL || out_len == NULL) {
    dsh_set_last_error("参数不合法：dsh_mdx_read_key_block 收到了空指针");
    return -1;
  }
  *out_bytes = NULL;
  *out_len = 0;
  if (block_index < 0 || block_index >= m->key_block_count) {
    dsh_set_last_error("词块下标越界：%lld（共 %lld 块）",
                       (long long)block_index, (long long)m->key_block_count);
    return -1;
  }
  const dsh_mdx_key_block *kb = &m->key_blocks[block_index];
  const uint8_t *packed = view_at(m, m->key_block_offset + kb->pack_offset, (size_t)kb->pack_size, "读词块");
  if (packed == NULL) {
    /* ⚠️ 把这一项索引的**全部字段**追加进错误信息：Windows 上实测过 pack_size 偶发变成
     * 脏值，只报一个越界区间看不出是哪一项坏了、坏成了什么。 */
    const char *saved = dsh_last_error_message();
    dsh_set_last_error(
        "%s｜词块索引项 [%lld/%lld]：entry_count=%lld pack_size=%lld unpack_size=%lld "
        "pack_offset=%lld unpack_offset=%lld entry_offset=%lld key_block_offset=%lld first=%s",
        saved ? saved : "", (long long)block_index, (long long)(m->key_block_count - 1),
        (long long)kb->entry_count, (long long)kb->pack_size, (long long)kb->unpack_size,
        (long long)kb->pack_offset, (long long)kb->unpack_offset, (long long)kb->entry_offset,
        (long long)m->key_block_offset, kb->first_key ? kb->first_key : "(null)");
    if (saved != NULL) dsh_release((void *)saved);
    return -1;
  }
  size_t n = 0;
  uint8_t *out = decompress_block(m, packed, (size_t)kb->pack_size, (size_t)kb->unpack_size,
                                  block_index, 0, &n);
  /* `packed` 是映射的只读视图，**不释放** */
  if (out == NULL) return -1;
  *out_bytes = out;
  *out_len = n;
  return 0;
}

/* ── 记录块索引与记录范围（逐字节对照与诊断用）──────────────────────────────── */

int64_t dsh_mdx_record_block_count(const dsh_mdx *m) { return m->record_block_count; }


int64_t dsh_mdx_total_record_unpacked(const dsh_mdx *m) {
  if (m->record_blocks == NULL || m->record_block_count <= 0) return 0;
  const dsh_mdx_record_block *last = &m->record_blocks[m->record_block_count - 1];
  return last->unpack_offset + last->unpack_size;
}


/** 找 global_index 这条词条所属的词块与块内偏移（内部工具） */


/* ── 键查找 ─────────────────────────────────────────────────────────────── */

/* 索引键的归一化形式：只留字母数字（ASCII 转小写），其余（空格、下划线、点、连字符…）丢掉。
 * 为什么需要它：有些词典（如 LDOCE5）的词块索引里存的是归一化过的键，索引区间与真实键
 * **不在同一个字符空间里**，只按序数比较的话二分永远落不进去，症状是「词条在词典里却查不到」。
 * ⚠️ 归一化**只用来挑词块**，块内命中仍按**原词逐字符**比对 —— 所以它不会把大小写敏感
 * 改成不敏感。非 ASCII 一律保留（保守：不误丢 CJK，代价是少数非 ASCII 标点不会被丢掉）。 */
static char *normalize_index_key(const char *text) {
  if (text == NULL || text[0] == '\0') return NULL;
  const size_t n = strlen(text);
  char *out = (char *)dsh_mem_alloc(n + 1);
  if (out == NULL) return NULL;
  size_t o = 0;
  for (size_t i = 0; i < n; i++) {
    const unsigned char c = (unsigned char)text[i];
    if (c < 0x80) {
      if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')) {
        out[o++] = (char)c;
      } else if (c >= 'A' && c <= 'Z') {
        out[o++] = (char)(c - 'A' + 'a');
      }
      /* 其余 ASCII（空格、下划线、点、连字符、撇号…）丢掉 */
    } else {
      out[o++] = (char)c; /* 非 ASCII 一律保留（多字节序列逐字节照抄） */
    }
  }
  out[o] = '\0';
  return out;
}

/* 在一个词块里找一个键。
 *
 * ⚠️ **词块里的记录格式与键信息块不一样，别照着那边写**：
 *   词块（每条）：记录偏移(numWidth 字节) + 键名 + \0   ← **没有长度字段**
 *   键信息块（每块）：词条数 + 首词长 + 首词 + 尾词长 + 尾词 + 压缩大小 + 解压大小
 * 键名以 \0 结尾（UTF-16LE 是两字节 00 00），长度靠扫到 \0 得到；误把「长度」当第一个字段
 * 会让键名全都取不到，而且**不报错**，只是不报错地查不到。
 *
 * @param want           要查的键（UTF-8，已 trim）
 * @param out_key        命中时写回词典里的规范键名（新分配）
 * @param out_record     命中时写回记录偏移
 * @param out_next_offset 命中时写回「这条之后的字节偏移」（用来界定记录范围）
 * @return 1 命中 / 0 没有 / -1 出错
 */
static int find_in_key_block(dsh_mdx *m, int64_t block_index, const char *want,
                             char **out_key, int64_t *out_record, size_t *out_next_offset,
                             int64_t *out_index_in_block) {
  uint8_t *buf = NULL;
  size_t len = 0;
  if (dsh_mdx_read_key_block(m, block_index, &buf, &len) != 0) return -1;

  const int w = m->num_width;
  /* 键名里的 \0 是宽字符还是单字节：UTF-16LE（含全部 .mdd）是 2，其余是 1 */
  const int key_width = (m->encoding == DSH_TXT_UTF16LE || m->is_mdd) ? 2 : 1;
  const dsh_mdx_key_block *kb = &m->key_blocks[block_index];

  size_t off = 0;
  int found = 0;
  char *found_key = NULL;
  int64_t found_record = 0;
  size_t found_next = 0;
  int64_t seen = 0;

  while (off + (size_t)w < len) {
    const int64_t record_offset = read_be(buf, len, off, w);
    const size_t key_start = off + (size_t)w;

    /* 扫到第一个 \0（UTF-16LE 是 00 00） */
    size_t key_end = (size_t)-1;
    for (size_t i = key_start; i + (size_t)key_width <= len; i += (size_t)key_width) {
      if (key_width == 1) {
        if (buf[i] == 0) { key_end = i; break; }
      } else if (buf[i] == 0 && buf[i + 1] == 0) {
        key_end = i;
        break;
      }
    }
    if (key_end == (size_t)-1) break; /* 没有终止符：块坏了，停止 */

    char *key_text = decode_text(m, buf + key_start, key_end - key_start, NULL);
    if (key_text == NULL) { dsh_release(buf); return -1; }

    off = key_end + (size_t)key_width;

    if (!found && strcmp(key_text, want) == 0) {
      found = 1;
      found_key = key_text; /* 交给调用方 */
      found_record = record_offset;
      found_next = off;
      if (out_index_in_block != NULL) *out_index_in_block = seen;
    } else {
      dsh_release(key_text);
    }
    seen++;

    /* 键信息块里记了这一块有几条；读满就停（免得块尾的填充字节被当成条目） */
    if (kb->entry_count > 0 && seen >= kb->entry_count) break;
  }

  dsh_release(buf);
  if (found) {
    if (out_key != NULL) *out_key = found_key; else dsh_release(found_key);
    if (out_record != NULL) *out_record = found_record;
    if (out_next_offset != NULL) *out_next_offset = found_next;
    return 1;
  }
  return 0;
}

/* 在候选词块里用**归一化形式**碰一遍：索引区间对不上时（LDOCE5 那类词典）靠它把词块挑
 * 出来，块内命中仍按原词逐字符。遍历所有块 —— 那类词典的归一化区间同样不可靠。 */
static int find_by_normalized(dsh_mdx *m, const char *want, char **out_key,
                              int64_t *out_block, int64_t *out_index_in_block) {
  char *normalized = normalize_index_key(want);
  if (normalized == NULL) return 0;
  if (strcmp(normalized, want) == 0) { /* 归一化之后没变化 → 这一趟没有意义 */
    dsh_release(normalized);
    return 0;
  }
  int result = 0;
  for (int64_t i = 0; i < m->key_block_count && result == 0; i++) {
    char *first = normalize_index_key(m->key_blocks[i].first_key);
    char *last = normalize_index_key(m->key_blocks[i].last_key);
    if (first == NULL || last == NULL) {
      if (first != NULL) dsh_release(first);
      if (last != NULL) dsh_release(last);
      continue;
    }
    const int inside = strcmp(first, normalized) <= 0 && strcmp(last, normalized) >= 0;
    dsh_release(first);
    dsh_release(last);
    if (!inside) continue;
    /* 区间对上了：块内按**原词**逐字符比对（约定不许放宽） */
    char *found = NULL;
    int64_t index_in_block = -1;
    const int r = find_in_key_block(m, i, want, &found, NULL, NULL, &index_in_block);
    if (r < 0) { result = -1; break; }
    if (r == 1) {
      if (out_key != NULL) *out_key = found; else dsh_release(found);
      if (out_block != NULL) *out_block = i;
      if (out_index_in_block != NULL) *out_index_in_block = index_in_block;
      result = 1;
    }
  }
  dsh_release(normalized);
  return result;
}

/* 索引区间里**有没有**这个词 —— 只比索引里那两个首尾词，**一块都不解压**。
 * ⚠️ 首尾词缺失时**回 1（不跳过）**：宁可多解压一块，也绝不许因为索引不全而漏查。 */
static int index_range_contains(const dsh_mdx_key_block *kb, const char *want) {
  if (kb == NULL || want == NULL) return 1;
  if (kb->first_key == NULL || kb->last_key == NULL) return 1;
  return (strcmp(kb->first_key, want) <= 0 && strcmp(kb->last_key, want) >= 0) ? 1 : 0;
}

/* 查键的**完整**结果：规范键名 + 全局序 + 记录偏移。收成一个函数是为了**查找只有一处** ——
 * 两个对外入口各走一遍迟早会被改得不一样。
 *
 * @param out_key       规范键名（新分配，调用方用 dsh_release 还给内核）；可传 NULL
 * @param out_block     命中块的序号；可传 NULL
 * @param out_global    词条的全局序；可传 NULL
 * @return 1 命中 / 0 没有 / -1 出错
 */
static int lookup_full(dsh_mdx *m, const char *key, char **out_key, int64_t *out_block,
                       int64_t *out_global) {
  if (out_key != NULL) *out_key = NULL;
  if (out_block != NULL) *out_block = -1;
  if (out_global != NULL) *out_global = -1;

  /* 与参考实现一致的两步：先按原样，再按去掉首尾空白。
   * ⚠️ 「去空白」那一步大小写仍然敏感 —— 不要顺手加 ToLower，那会改变「查不到」与「查到」的边界。 */
  char *trimmed = NULL;
  size_t b = 0, e = strlen(key);
  while (b < e && (key[b] == ' ' || key[b] == '\t' || key[b] == '\n' || key[b] == '\r')) b++;
  while (e > b && (key[e - 1] == ' ' || key[e - 1] == '\t' || key[e - 1] == '\n' || key[e - 1] == '\r')) e--;
  if (b != 0 || e != strlen(key)) {
    trimmed = (char *)dsh_mem_alloc(e - b + 1);
    if (trimmed == NULL) {
      dsh_set_last_error("内存不足：键的副本");
      return -1;
    }
    memcpy(trimmed, key + b, e - b);
    trimmed[e - b] = '\0';
  }

  const char *candidates[2];
  candidates[0] = key;
  candidates[1] = trimmed;
  int result = 0;

  for (int ci = 0; ci < 2 && result == 0; ci++) {
    if (candidates[ci] == NULL) continue;
    if (ci == 1 && strcmp(candidates[0], candidates[1]) == 0) continue;

    /* ① 二分定位候选词块（索引单调时） */
    int64_t candidate = -1;
    if (m->block_order_monotone && m->key_block_count > 0) {
      int64_t lo = 0, hi = m->key_block_count - 1;
      while (lo <= hi) {
        const int64_t mid = lo + (hi - lo) / 2;
        if (strcmp(candidates[ci], m->key_blocks[mid].last_key) > 0) { lo = mid + 1; continue; }
        if (strcmp(candidates[ci], m->key_blocks[mid].first_key) < 0) { hi = mid - 1; continue; }
        candidate = mid;
        break;
      }
      if (candidate >= 0) {
        char *found = NULL;
        int64_t index_in_block = -1;
        const int r = find_in_key_block(m, candidate, candidates[ci], &found, NULL, NULL,
                                       &index_in_block);
        if (r < 0) { result = -1; break; }
        if (r == 1) {
          if (out_key != NULL) *out_key = found; else dsh_release(found);
          if (out_block != NULL) *out_block = candidate;
          if (out_global != NULL) *out_global = m->key_blocks[candidate].entry_offset + index_in_block;
          result = 1;
          break;
        }
      }
    }

    /* ★ ② 索引区间扫描：**只比索引里的首尾词，一块都不解压**。这一圈是「查词慢」的正面 ——
     *    在它之前这里是「对每一块都解压、逐键解码一遍」，于是一个注定查不到的键（发音规划
     *    一次要试几十个候选）要把整本词典的词区解压几十遍；区间不包含 ⇒ 这一块里一定没有。 */
    for (int64_t i = 0; i < m->key_block_count; i++) {
      if (i == candidate) continue; /* ① 已经在这一块里逐键找过了 */
      if (!index_range_contains(&m->key_blocks[i], candidates[ci])) continue;
      char *found = NULL;
      int64_t index_in_block = -1;
      const int r = find_in_key_block(m, i, candidates[ci], &found, NULL, NULL, &index_in_block);
      if (r < 0) { result = -1; break; }
      if (r == 1) {
        if (out_key != NULL) *out_key = found; else dsh_release(found);
        if (out_block != NULL) *out_block = i;
        if (out_global != NULL) *out_global = m->key_blocks[i].entry_offset + index_in_block;
        result = 1;
        break;
      }
    }
    if (result != 0) break;

    /* ③ 归一化兜底：索引里存的是「去标点 + 小写」的形式时，上面两步都落不进去。放最后。 */
    {
      char *found = NULL;
      int64_t block = -1, index_in_block = -1;
      const int r = find_by_normalized(m, candidates[ci], &found, &block, &index_in_block);
      if (r < 0) { result = -1; break; }
      if (r == 1) {
        if (out_key != NULL) *out_key = found; else dsh_release(found);
        if (out_block != NULL) *out_block = block;
        if (out_global != NULL) *out_global = m->key_blocks[block].entry_offset + index_in_block;
        result = 1;
        break;
      }
    }

    /* ④ 文件内键序**不是**序数序时，二分与区间扫描都可能落空，只能老实全扫一遍。
     *    ⚠️ 这一圈是**逐块解压**的，所以它**只在真的不单调时才跑**。 */
    if (!m->block_order_monotone) {
      for (int64_t i = 0; i < m->key_block_count; i++) {
        if (i == candidate) continue;
        char *found = NULL;
        int64_t index_in_block = -1;
        const int r = find_in_key_block(m, i, candidates[ci], &found, NULL, NULL, &index_in_block);
        if (r < 0) { result = -1; break; }
        if (r == 1) {
          if (out_key != NULL) *out_key = found; else dsh_release(found);
          if (out_block != NULL) *out_block = i;
          if (out_global != NULL) *out_global = m->key_blocks[i].entry_offset + index_in_block;
          result = 1;
          break;
        }
      }
    }
  }

  dsh_release(trimmed);
  return result;
}

int dsh_mdx_lookup_key(dsh_mdx *m, const char *key, char **out_key, int64_t *out_block_index) {
  if (out_key != NULL) *out_key = NULL;
  if (out_block_index != NULL) *out_block_index = -1;
  if (m == NULL || key == NULL || key[0] == '\0') {
    dsh_set_last_error("参数不合法：键不能为空");
    return -1;
  }
  return lookup_full(m, key, out_key, out_block_index, NULL);
}

/* 前缀查询（见 dsh_mdx.h）。先用二分找到**起点词块**（拿 prefix 与每块的 last_key 比），
 * 再从那一块的头开始逐键筛前缀；一旦解出来的键在序数序上越过 prefix，后面不可能再有，整体收工。 */
int dsh_mdx_prefix_search(dsh_mdx *m, const char *prefix, int64_t max_count, char ***out_keys,
                          int64_t *out_count) {
  if (out_keys == NULL || out_count == NULL) {
    dsh_set_last_error("参数不合法：dsh_mdx_prefix_search 收到了空指针");
    return -1;
  }
  *out_keys = NULL;
  *out_count = 0;
  if (m == NULL || prefix == NULL || prefix[0] == '\0') return 0;
  if (max_count <= 0) return 0;

  /* 条数虽然由调用方给，但 `max_count * sizeof(char *)` 这个乘法必须在这里过一遍预算：
   * 乘法自己会溢出（溢出成一个很小的数就成了**堆溢出**），而预算那一关是同一条规矩。 */
  if (!index_budget_ok("前缀结果数组", max_count, sizeof(char *))) return -1;

  char **keys = (char **)dsh_mem_alloc((size_t)max_count * sizeof(char *));
  if (keys == NULL) {
    dsh_set_last_error("内存不足：前缀结果需要 %lld 项", (long long)max_count);
    return -1;
  }
  memset(keys, 0, (size_t)max_count * sizeof(char *));
  const size_t plen = strlen(prefix);
  int64_t n = 0;
  int done = 0;

  /* 先二分：第一个 last_key >= prefix 的词块就是起点所在（首尾词是**索引词**，可能与真实键
   * 不同，所以这里只用它**缩小范围**，真正的筛选用真实键做）。 */
  int64_t start_block = 0;
  {
    int64_t lo = 0, hi = m->key_block_count - 1;
    while (lo <= hi) {
      const int64_t mid = lo + ((hi - lo) >> 1);
      const char *last = m->key_blocks[mid].last_key;
      if (last != NULL && strcmp(last, prefix) < 0) lo = mid + 1;
      else hi = mid - 1;
    }
    start_block = lo; /* 第一个「尾词 >= prefix」的块；可能等于块数（那就是没有） */
    if (start_block >= m->key_block_count) {
      dsh_release(keys);
      return 0;
    }
  }

  for (int64_t bi = start_block; bi < m->key_block_count && !done; bi++) {
    const int64_t want = m->key_blocks[bi].entry_count;
    uint8_t *buf = NULL;
    size_t len = 0;
    if (dsh_mdx_read_key_block(m, bi, &buf, &len) != 0) { done = 1; break; }
    const int w = m->num_width;
    const int key_width = (m->encoding == DSH_TXT_UTF16LE || m->is_mdd) ? 2 : 1;
    size_t off = 0;
    int64_t read_in_block = 0;
    while (off + (size_t)w < len && !done) {
      const size_t key_start = off + (size_t)w;
      size_t key_end = (size_t)-1;
      for (size_t i = key_start; i + (size_t)key_width <= len; i += (size_t)key_width) {
        if (key_width == 1) {
          if (buf[i] == 0) { key_end = i; break; }
        } else if (buf[i] == 0 && buf[i + 1] == 0) {
          key_end = i;
          break;
        }
      }
      if (key_end == (size_t)-1) break;
      char *text = decode_text(m, buf + key_start, key_end - key_start, NULL);
      if (text == NULL) { done = 1; break; }
      off = key_end + (size_t)key_width;
      read_in_block++;

      if (strncmp(text, prefix, plen) == 0) {
        if (n < max_count) keys[n++] = text;
        else dsh_release(text);
        if (n >= max_count) done = 1;
      } else if (strcmp(text, prefix) > 0) {
        /* 序数序上已经越过 prefix 了：后面不可能再有它的前缀（块内是有序的）。 */
        dsh_release(text);
        done = 1;
      } else {
        /* 还排在 prefix 前面：继续往后走 */
        dsh_release(text);
      }
      if (want > 0 && read_in_block >= want) break;
    }
    dsh_release(buf);
  }

  if (n == 0) {
    dsh_release(keys);
    return 0;
  }
  *out_keys = keys;
  *out_count = n;
  return 0;
}

int dsh_mdx_fetch(dsh_mdx *m, const char *key, char **out_key, char **out_text, int64_t *out_len) {
  if (out_key != NULL) *out_key = NULL;
  if (out_text != NULL) *out_text = NULL;
  if (out_len != NULL) *out_len = 0;
  if (m == NULL || key == NULL || key[0] == '\0') {
    dsh_set_last_error("参数不合法：键不能为空");
    return -1;
  }

  char *landed = NULL;
  int64_t global = -1;
  const int hit = lookup_full(m, key, &landed, NULL, &global);
  if (hit < 0) return -1;
  if (hit == 0) return 0;

  char *text = NULL;
  int64_t text_len = 0;
  if (dsh_mdx_read_record(m, global, &text, &text_len) != 0) {
    if (landed != NULL) dsh_release(landed);
    return -1;
  }
  if (out_key != NULL) *out_key = landed; else dsh_release(landed);
  if (out_text != NULL) *out_text = text; else dsh_release(text);
  if (out_len != NULL) *out_len = text_len;
  return 1;
}

/* ── 枚举键 ─────────────────────────────────────────────────────────────── */

/* 解析**第 block_index 个词块**里的键名。
 * ⚠️ 「扫到 \0」这条解析循环在本文件里有四份（record_offsets_in_block / find_in_key_block /
 * prefix_search 内圈 / 本函数）—— 任何一份改了（比如 UTF-16 判定），其余三份要同步检查。
 * 记录格式：每词条 = {记录偏移(numWidth 字节) + 键名 + \0}，**没有长度字段**；宽度按编码取
 * （UTF-16LE 与 .mdd 是 2 字节，其余 1 字节）。抽出来是因为 `dsh_mdx_list_keys` 与
 * `dsh_mdx_block_keys` 必须读**同一份格式**，两处各解析一遍迟早会在某个编码分支上分叉。
 *
 * @return 0 成功（**一个键都没有也算成功**）；-1 失败（last_error 已写）
 */
static int parse_block_keys(dsh_mdx *m, int64_t block_index, char ***out_keys,
                           int64_t *out_count) {
  const int w = m->num_width;
  const int key_width = (m->encoding == DSH_TXT_UTF16LE || m->is_mdd) ? 2 : 1;
  /* 声明条数：正常文件 > 0；**0 表示这个数不可信**，那就读到底（与旧行为一致）*/
  const int64_t want = m->key_blocks[block_index].entry_count;
  uint8_t *buf = NULL;
  size_t len = 0;
  char **keys = NULL;
  int64_t n = 0;
  int64_t cap = 0;

  *out_keys = NULL;
  *out_count = 0;

  if (dsh_mdx_read_key_block(m, block_index, &buf, &len) != 0) return -1;

  cap = (want > 0 && want < 4096) ? want : 64;
  keys = (char **)dsh_mem_alloc((size_t)cap * sizeof(char *));
  if (keys == NULL) {
    dsh_release(buf);
    dsh_set_last_error("内存不足：词块键名数组需要 %lld 项", (long long)cap);
    return -1;
  }
  memset(keys, 0, (size_t)cap * sizeof(char *));

  size_t off = 0;
  while (off + (size_t)w < len) {
    const size_t key_start = off + (size_t)w;
    size_t key_end = (size_t)-1;
    char *text;
    for (size_t i = key_start; i + (size_t)key_width <= len; i += (size_t)key_width) {
      if (key_width == 1) {
        if (buf[i] == 0) {
          key_end = i;
          break;
        }
      } else if (buf[i] == 0 && buf[i + 1] == 0) {
        key_end = i;
        break;
      }
    }
    if (key_end == (size_t)-1) break;
    if (want > 0 && n >= want) break; /* 声明数已读满：后面的不读 */
    text = decode_text(m, buf + key_start, key_end - key_start, NULL);
    if (text == NULL) break;
    if (n == cap) {
      /* 增长也要过索引预算：这一条的增长由**真读出来的键数**驱动（不是声明值），
       * 但真读出来的键数最终也由文件内容决定，所以同样要有上限兜底。 */
      const int64_t bigger = cap * 2;
      if (!index_budget_ok("词块键名数组", bigger, sizeof(char *))) break;
      char **grown = (char **)dsh_mem_alloc((size_t)bigger * sizeof(char *));
      if (grown == NULL) {
        dsh_release(text);
        break;
      }
      memcpy(grown, keys, (size_t)n * sizeof(char *));
      memset(grown + n, 0, (size_t)(bigger - n) * sizeof(char *));
      dsh_release(keys);
      keys = grown;
      cap = bigger;
    }
    keys[n++] = text;
    off = key_end + (size_t)key_width;
  }
  dsh_release(buf);
  *out_keys = keys;
  *out_count = n;
  return 0;
}

int dsh_mdx_block_keys(dsh_mdx *m, int64_t block_index, char ***out_keys, int64_t *out_count) {
  if (out_keys == NULL || out_count == NULL) {
    dsh_set_last_error("参数不合法：dsh_mdx_block_keys 收到了空指针");
    return -1;
  }
  *out_keys = NULL;
  *out_count = 0;
  if (m == NULL) {
    dsh_set_last_error("参数不合法：dsh_mdx_block_keys 的 mdx 为空");
    return -1;
  }
  if (block_index < 0 || block_index >= m->key_block_count) {
    dsh_set_last_error("词块下标越界：要第 %lld 块，这本只有 %lld 块", (long long)block_index,
                       (long long)m->key_block_count);
    return -1;
  }
  return parse_block_keys(m, block_index, out_keys, out_count);
}

int dsh_mdx_list_keys(dsh_mdx *m, char ***out_keys, int64_t *out_count) {
  if (out_keys == NULL || out_count == NULL) {
    dsh_set_last_error("参数不合法：dsh_mdx_list_keys 收到了空指针");
    return -1;
  }
  *out_keys = NULL;
  *out_count = 0;
  if (m->key_count <= 0) return 0;

  /* ⚠️ 这里**不许**按头里声明的 `key_count` 预先分配：那是个文件说了算的数，
   * 而「这本词典到底有多少键」只有**真从词块里读出来**才算数（同一个坏文件把声明放大
   * 一万倍，按声明值分配就会去要几十兆、几百兆）。所以按需翻倍增长，每次增长都过同一套
   * 索引预算；`key_count` 只当**上界**用（声明之外的多余键仍然丢掉，与原来同一条约定）。 */
  char **keys = NULL;
  int64_t cap = 0;
  int64_t n = 0;
  int ok = 1;

  for (int64_t bi = 0; bi < m->key_block_count && ok; bi++) {
    char **block_keys = NULL;
    int64_t block_count = 0;
    if (parse_block_keys(m, bi, &block_keys, &block_count) != 0) {
      ok = 0;
      break;
    }
    for (int64_t i = 0; i < block_count; i++) {
      if (n >= m->key_count) {
        /* 声明数之外的多余键：丢掉（与解析那一侧的容错同一条约定）*/
        dsh_release(block_keys[i]);
        continue;
      }
      if (n == cap) {
        /* 起始 64 条；之后翻倍，但**封顶在声明数**上（不多要，声明数本来就是个上界） */
        int64_t bigger = (cap == 0) ? 64 : cap * 2;
        if (bigger > m->key_count) bigger = m->key_count;
        if (!index_budget_ok("键名数组", bigger, sizeof(char *))) {
          ok = 0;
          break;
        }
        char **grown = (char **)dsh_mem_alloc((size_t)bigger * sizeof(char *));
        if (grown == NULL) {
          dsh_set_last_error("内存不足：键名数组需要 %lld 项", (long long)bigger);
          ok = 0;
          break;
        }
        if (n > 0) memcpy(grown, keys, (size_t)n * sizeof(char *));
        if (keys != NULL) dsh_release(keys);
        keys = grown;
        cap = bigger;
      }
      keys[n++] = block_keys[i];
    }
    if (block_keys != NULL) dsh_release(block_keys);
  }

  if (!ok) {
    /* ⚠️ 失败时必须把**已分配的那些**全部还掉，否则会悄悄泄漏（活分配表会暴露）。 */
    const char *saved = dsh_last_error_message();
    for (int64_t i = 0; i < n; i++) {
      if (keys[i] != NULL) dsh_release(keys[i]);
    }
    dsh_release(keys);
    if (saved != NULL) {
      dsh_set_last_error("%s", saved);
      dsh_release((void *)saved);
    }
    return -1;
  }
  *out_keys = keys;
  *out_count = n;
  return 0;
}

void dsh_mdx_free_keys(char **keys, int64_t count) {
  if (keys == NULL) return;
  for (int64_t i = 0; i < count; i++) {
    if (keys[i] != NULL) dsh_release(keys[i]);
  }
  dsh_release(keys);
}

/* ── 读记录 ─────────────────────────────────────────────────────────────── */

/* 读一条记录的**文本**（按文件编码解码）。
 * ⚠️ 这一条**必须**走「先按记录偏移把字节跨块拼出来、再解码」那条路。曾经它是自己实现的：
 * 只从记录起点所在的那一块取字节、end 夹到本块末尾，遇上**横跨记录块边界的记录**会把内容
 * 截断（停在半个标签里）。参考实现也是这样截断的，所以那样写逐字节对照是绿的 —— 但那是坏数据。 */
int dsh_mdx_read_record(dsh_mdx *m, int64_t global_index, char **out_text, int64_t *out_len) {
  if (out_text == NULL || out_len == NULL) {
    dsh_set_last_error("参数不合法：dsh_mdx_read_record 收到了空指针");
    return -1;
  }
  *out_text = NULL;
  *out_len = 0;

  int64_t start = -1, end = -1;
  if (dsh_mdx_record_range(m, global_index, &start, &end) != 0) return -1;
  return dsh_mdx_read_record_bytes(m, start, end, out_text, out_len);
}
