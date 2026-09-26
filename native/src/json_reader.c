/* JSON 读取：arena（4KB 块链）+ RFC 8259 递归下降解析器 + 只读访问器，契约见 json_reader.h。
 * 容器必须先要到连续的 slots 个槽位再填成员——绝不能让成员各自 new_node 再拷，
 * 否则 children[i] 是野地址；字符串缓冲可切在槽位之后；深度超 DSH_JSON_MAX_DEPTH 如实报错。 */

#include "json_reader.h"
#include "dsh_internal.h"

#include <errno.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>

#define DSH_JSON_CHUNK 4096u
#define DSH_JSON_MAX_DEPTH 64
/* 单个容器里的成员个数上限（挡恶意输入；正常词典与设置补丁离它很远） */
#define DSH_JSON_MAX_CHILDREN 4096

struct dsh_json_node {
  uint8_t kind;    /* dsh_json_kind */
  uint8_t is_true; /* 布尔值 */
  uint16_t pad;
  const char *text; /* 字符串是**解码后**的 UTF-8；数字是原文片段；容器为 NULL */
  size_t len;
  const char *key; /* 对象成员才有：键那串字节（解码后的 UTF-8） */
  size_t key_len;
  /* 节点在输入文本里的原文片段（见 dsh_json_node_raw）：容器是含括号整段，标量是自己那段。 */
  const char *raw;
  size_t raw_len;
  /* 数组：第一个子节点 + 个数；对象：第一个成员节点 + 成员数 */
  const struct dsh_json_node *children;
  int64_t count;
};

struct dsh_json_arena {
  struct dsh_json_arena *next;
  size_t used;
  size_t cap;
  uint8_t bytes[]; /* 柔性数组：跟着结构体一起分配 */
};

struct dsh_json_doc {
  struct dsh_json_arena *arena;
  const struct dsh_json_node *root;
};

typedef struct {
  const char *text; /* 整份文本的起点（报偏移用） */
  const char *p;
  const char *end;
  struct dsh_json_doc *doc;
  int depth;
  const char *err_at; /* 出错位置 */
} parser;

/* ── arena ───────────────────────────────────────── */

static void *arena_alloc(struct dsh_json_doc *doc, size_t size, size_t align) {
  if (align > 8) align = 8;
  struct dsh_json_arena *a = doc->arena;
  /* 第一次分配时 doc->arena 是 NULL：偏移按 0 算，不能先去读 a->used（会空指针解引用）。 */
  size_t off = (a != NULL) ? ((a->used + (align - 1)) & ~(align - 1)) : 0;
  if (a == NULL || off + size > a->cap) {
    const size_t need = (size + align > DSH_JSON_CHUNK) ? (size + align) : DSH_JSON_CHUNK;
    struct dsh_json_arena *n = (struct dsh_json_arena *)dsh_mem_alloc(sizeof(*n) + need);
    if (n == NULL) return NULL;
    n->next = doc->arena;
    n->used = 0;
    n->cap = need;
    doc->arena = n;
    a = n;
    off = 0;
  }
  void *p = a->bytes + off;
  a->used = off + size;
  return p;
}

/* 对外只暴露一个文档句柄，所以整条链一次还清：一份文档算一次活分配事务，不是每个节点一次。 */
static void arena_release_all(struct dsh_json_doc *doc) {
  struct dsh_json_arena *a = doc->arena;
  while (a != NULL) {
    struct dsh_json_arena *next = a->next;
    dsh_release(a);
    a = next;
  }
  doc->arena = NULL;
}

/* ── 小工具 ──────────────────────────────────────── */

static void skip_ws(parser *ps) {
  while (ps->p < ps->end) {
    const char c = *ps->p;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      ps->p++;
      continue;
    }
    break;
  }
}

/** UTF-8 编码一个码点（代理区与超出范围的码点写 U+FFFD） */
static size_t utf8_put(uint8_t *out, uint32_t cp) {
  if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) cp = 0xFFFDu;
  if (cp < 0x80u) {
    out[0] = (uint8_t)cp;
    return 1;
  }
  if (cp < 0x800u) {
    out[0] = (uint8_t)(0xC0u | (cp >> 6));
    out[1] = (uint8_t)(0x80u | (cp & 0x3Fu));
    return 2;
  }
  if (cp < 0x10000u) {
    out[0] = (uint8_t)(0xE0u | (cp >> 12));
    out[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
    out[2] = (uint8_t)(0x80u | (cp & 0x3Fu));
    return 3;
  }
  out[0] = (uint8_t)(0xF0u | (cp >> 18));
  out[1] = (uint8_t)(0x80u | ((cp >> 12) & 0x3Fu));
  out[2] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
  out[3] = (uint8_t)(0x80u | (cp & 0x3Fu));
  return 4;
}

/** 一个码点编成 UTF-8 之后的字节数（与 utf8_put 必须一致） */
static size_t utf8_len(uint32_t cp) {
  if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) cp = 0xFFFDu;
  if (cp < 0x80u) return 1;
  if (cp < 0x800u) return 2;
  if (cp < 0x10000u) return 3;
  return 4;
}

static int hex4(const char *p, uint32_t *out) {
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) {
    const char c = p[i];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
    else return 0;
  }
  *out = v;
  return 1;
}

/** 一个 UTF-8 序列的期望长度（首字节决定）；不合法返回 0 */
static size_t utf8_seq_len(uint8_t b) {
  if (b < 0x80u) return 1;
  if ((b & 0xE0u) == 0xC0u) return 2;
  if ((b & 0xF0u) == 0xE0u) return 3;
  if ((b & 0xF8u) == 0xF0u) return 4;
  return 0;
}

/* ── 字符串 ──────────────────────────────────────── */

/* 读一个 \uXXXX 转义（含代理对），返回码点；*used 给出吃掉的字节数，含 \u 与可选的第二个 \uXXXX。
 * 落单的代理不失败，返回 U+FFFD（与浏览器 TextDecoder 同约定）。 */
static uint32_t read_escape_u(const char *p, const char *end, size_t *used) {
  uint32_t cp = 0;
  (void)hex4(p + 2, &cp);
  *used = 6;
  if (cp >= 0xD800u && cp <= 0xDBFFu && (size_t)(end - p) >= 12 && p[6] == '\\' && p[7] == 'u') {
    uint32_t lo = 0;
    if (hex4(p + 8, &lo) && lo >= 0xDC00u && lo <= 0xDFFFu) {
      *used = 12;
      return 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
    }
    return 0xFFFDu;
  }
  if (cp >= 0xD800u && cp <= 0xDFFFu) return 0xFFFDu;
  return cp;
}

/* 解一个字符串：调用时 ps->p 指向开引号之后的第一个字节；两趟（先量并校验语法，再写），
 * 只要一次 arena 分配。非法/残缺的 UTF-8 每个坏字节产出 U+FFFD 且不算失败，但原样的控制
 * 字符（< 0x20）算语法错误——那是宿主把二进制塞进了字符串，如实报错。 */
static int parse_string(parser *ps, const char **out_text, size_t *out_len) {
  const char *scan = ps->p;
  size_t need = 0;
  size_t bad = 0;
  while (scan < ps->end) {
    const uint8_t c = (uint8_t)*scan;
    if (c == '"') break;
    if (c == '\\') {
      if (scan + 1 >= ps->end) { ps->err_at = scan; return -1; }
      const char e = scan[1];
      switch (e) {
        case '"': case '\\': case '/': case 'b': case 'f':
        case 'n': case 'r': case 't':
          need += 1;
          scan += 2;
          break;
        case 'u': {
          if (ps->end - scan < 6 || !hex4(scan + 2, &(uint32_t){0})) {
            ps->err_at = scan;
            return -1;
          }
          size_t used = 0;
          const uint32_t cp = read_escape_u(scan, ps->end, &used);
          need += utf8_len(cp);
          scan += used;
          break;
        }
        default:
          ps->err_at = scan + 1;
          return -1;
      }
      continue;
    }
    if (c < 0x20u) {
      ps->err_at = scan;
      return -1;
    }
    if (c < 0x80u) { need += 1; scan++; continue; }
    const size_t seq = utf8_seq_len(c);
    int good = (seq != 0 && (size_t)(ps->end - scan) >= seq);
    if (good) {
      for (size_t i = 1; i < seq; i++) {
        if (((uint8_t)scan[i] & 0xC0u) != 0x80u) { good = 0; break; }
      }
    }
    if (good) { need += seq; scan += seq; }
    else { need += 3; bad++; scan++; }
  }
  if (scan >= ps->end) { ps->err_at = scan; return -1; } /* 没闭合 */

  char *buf = (char *)arena_alloc(ps->doc, need + 1, 1);
  if (buf == NULL) {
    dsh_set_last_error("内存不足：JSON 字符串需要 %zu 字节", need + 1);
    return -1;
  }
  char *w = buf;
  const char *r = ps->p;
  while (r < scan) {
    const uint8_t c = (uint8_t)*r;
    if (c == '\\') {
      const char e = r[1];
      r += 2;
      switch (e) {
        case '"': *w++ = '"'; break;
        case '\\': *w++ = '\\'; break;
        case '/': *w++ = '/'; break;
        case 'b': *w++ = '\b'; break;
        case 'f': *w++ = '\f'; break;
        case 'n': *w++ = '\n'; break;
        case 'r': *w++ = '\r'; break;
        case 't': *w++ = '\t'; break;
        case 'u': {
          size_t used = 0;
          const uint32_t cp = read_escape_u(r - 2, ps->end, &used);
          w += utf8_put((uint8_t *)w, cp);
          r = (r - 2) + used;
          break;
        }
        default: break; /* 不会走到：第一趟已经挡下 */
      }
      continue;
    }
    if (c < 0x80u) { *w++ = (char)c; r++; continue; }
    const size_t seq = utf8_seq_len(c);
    int good = (seq != 0 && (size_t)(scan - r) >= seq);
    if (good) {
      for (size_t i = 1; i < seq; i++) {
        if (((uint8_t)r[i] & 0xC0u) != 0x80u) { good = 0; break; }
      }
    }
    if (good) {
      memcpy(w, r, seq);
      w += seq;
      r += seq;
    } else {
      w += utf8_put((uint8_t *)w, 0xFFFDu);
      r++;
    }
  }
  *w = '\0';
  ps->p = scan + 1; /* 跨过闭引号 */
  *out_text = buf;
  *out_len = (size_t)(w - buf);
  if (bad > 0) {
    /* 与 dsh_text_decode 同一约定：解出过替换字符只记诊断，**不算失败** */
    dsh_set_last_error("JSON 字符串里有 %zu 处非法 UTF-8，已按 U+FFFD 替换（不算失败）", bad);
  }
  return 0;
}

/* ── 值与容器 ────────────────────────────────────── */

static const struct dsh_json_node *parse_value(parser *ps, struct dsh_json_node *slot,
                                               const char *key, size_t key_len);

static struct dsh_json_node *new_node(parser *ps) {
  struct dsh_json_node *n = (struct dsh_json_node *)arena_alloc(ps->doc, sizeof(*n), 8);
  if (n == NULL) {
    dsh_set_last_error("内存不足：JSON 节点");
    return NULL;
  }
  memset(n, 0, sizeof(*n));
  return n;
}

/* 为容器预留连续的（不是各自分配的）子节点槽位，见文件头的排布规则。 */
static struct dsh_json_node *reserve_nodes(parser *ps, size_t count) {
  return (struct dsh_json_node *)arena_alloc(ps->doc,
                                             count * sizeof(struct dsh_json_node), 8);
}

static int literal(parser *ps, const char *word) {
  const size_t n = strlen(word);
  if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, word, n) != 0) return 0;
  ps->p += n;
  return 1;
}

/** 数值：只切出原文，**不在这里转成 double**（转换在取值的接口上做） */
static int parse_number_span(parser *ps, struct dsh_json_node *n) {
  const char *p = ps->p;
  const char *start = p;
  if (p < ps->end && *p == '-') p++;
  if (p >= ps->end) { ps->err_at = p; return -1; }
  if (*p == '0') {
    p++;
  } else if (*p >= '1' && *p <= '9') {
    while (p < ps->end && *p >= '0' && *p <= '9') p++;
  } else {
    ps->err_at = p;
    return -1;
  }
  if (p < ps->end && *p == '.') {
    p++;
    if (p >= ps->end || *p < '0' || *p > '9') { ps->err_at = p; return -1; }
    while (p < ps->end && *p >= '0' && *p <= '9') p++;
  }
  if (p < ps->end && (*p == 'e' || *p == 'E')) {
    p++;
    if (p < ps->end && (*p == '+' || *p == '-')) p++;
    if (p >= ps->end || *p < '0' || *p > '9') { ps->err_at = p; return -1; }
    while (p < ps->end && *p >= '0' && *p <= '9') p++;
  }
  n->text = start;
  n->len = (size_t)(p - start);
  ps->p = p;
  return 0;
}

/* 解一个值。slot 非空时就地填那一个节点——容器靠它把成员填进预留的连续槽位里，
 * 所以解析成员的过程中不许再 new_node 去拿成员节点，否则槽位的连续性就没了。 */
static const struct dsh_json_node *parse_value(parser *ps, struct dsh_json_node *slot,
                                               const char *key, size_t key_len) {
  struct dsh_json_node *n = slot;
  if (n == NULL) {
    n = new_node(ps);
    if (n == NULL) return NULL;
  }
  n->key = key;
  n->key_len = key_len;
  n->raw = NULL;
  n->raw_len = 0;

  skip_ws(ps);
  if (ps->p >= ps->end) {
    ps->err_at = ps->p;
    dsh_set_last_error("JSON 提前结束（这里应当有一个值）");
    return NULL;
  }
  if (ps->depth >= DSH_JSON_MAX_DEPTH) {
    ps->err_at = ps->p;
    dsh_set_last_error("JSON 嵌套太深（超过 %d 层）—— 拒绝解析，以免打穿栈",
                       DSH_JSON_MAX_DEPTH);
    return NULL;
  }

  /* 原文片段的起点：`skip_ws` 之后、值本身之前（片段**不含前导空白**） */
  const char *raw_start = ps->p;

  const char c = *ps->p;
  if (c == '"') {
    n->kind = DSH_JSON_STRING;
    ps->p++;
    if (parse_string(ps, &n->text, &n->len) != 0) return NULL;
    n->raw = raw_start;
    n->raw_len = (size_t)(ps->p - raw_start);
    return n;
  }
  if (c == 't' || c == 'f' || c == 'n') {
    if (literal(ps, "true")) { n->kind = DSH_JSON_BOOL; n->is_true = 1; }
    else if (literal(ps, "false")) { n->kind = DSH_JSON_BOOL; n->is_true = 0; }
    else if (literal(ps, "null")) { n->kind = DSH_JSON_NULL; }
    else {
      ps->err_at = ps->p;
      dsh_set_last_error("JSON 里出现了无法识别的字面量（只认 true / false / null）");
      return NULL;
    }
    n->raw = raw_start;
    n->raw_len = (size_t)(ps->p - raw_start);
    return n;
  }
  if (c == '-' || (c >= '0' && c <= '9')) {
    n->kind = DSH_JSON_NUMBER;
    if (parse_number_span(ps, n) != 0) {
      dsh_set_last_error("JSON 里的数值写法不合法");
      return NULL;
    }
    n->raw = raw_start;
    n->raw_len = (size_t)(ps->p - raw_start);
    return n;
  }
  if (c != '{' && c != '[') {
    ps->err_at = ps->p;
    dsh_set_last_error("JSON 里出现了不认识的字符（0x%02X）", (unsigned)(uint8_t)c);
    return NULL;
  }

  /* ── 容器 ───────────────────────────────────────────
   * 单趟：先要到连续槽位，再边解值边填槽位。对象的值节点在奇数下标（arr[1], arr[3], …），
   * 键节点在偶数下标；值的 key 指向解键时切出来的那串字节（切在槽位之后）。 */
  const int is_obj = (c == '{');
  const char close = is_obj ? '}' : ']';
  n->kind = is_obj ? DSH_JSON_OBJECT : DSH_JSON_ARRAY;
  ps->p++;
  ps->depth++;

  int64_t count = 0;
  const char *keys[DSH_JSON_MAX_CHILDREN];
  size_t keylens[DSH_JSON_MAX_CHILDREN];
  int done = 0;

  skip_ws(ps);
  if (ps->p < ps->end && *ps->p == close) {
    ps->p++;
    done = 1;
  }
  while (!done) {
    if (count >= DSH_JSON_MAX_CHILDREN) {
      ps->err_at = ps->p;
      dsh_set_last_error("JSON 单个容器里的元素太多（超过 %d 个）", DSH_JSON_MAX_CHILDREN);
      return NULL;
    }
    if (is_obj) {
      skip_ws(ps);
      /* 先判「没了」再判「键不是字符串」：{ 这种输入应当报缺少 }，报键必须是字符串会指错方向。 */
      if (ps->p >= ps->end) {
        ps->err_at = ps->p;
        dsh_set_last_error("JSON 在容器结束前就没了（缺少 '%c'）", close);
        return NULL;
      }
      if (*ps->p != '"') {
        ps->err_at = ps->p;
        dsh_set_last_error("JSON 对象成员的键必须是字符串");
        return NULL;
      }
      ps->p++;
      if (parse_string(ps, &keys[count], &keylens[count]) != 0) return NULL;
      skip_ws(ps);
      if (ps->p >= ps->end || *ps->p != ':') {
        ps->err_at = ps->p;
        dsh_set_last_error("JSON 对象的键之后应当是 ':'");
        return NULL;
      }
      ps->p++;
    }
    /* 现在才要槽位：之后解值、解子容器、切字符串缓冲都在这片区间之后，碰不到槽位。 */
    if (count == 0) {
      const size_t want = is_obj ? 2 : 1;
      struct dsh_json_node *first = reserve_nodes(ps, want);
      if (first == NULL) {
        dsh_set_last_error("内存不足：JSON 容器");
        return NULL;
      }
      n->children = first;
      memset(first, 0, want * sizeof(*first));
      if (is_obj) {
        first[0].kind = DSH_JSON_STRING;
        first[0].text = keys[0];
        first[0].len = keylens[0];
      }
    } else {
      /* 扩容式增长：新块紧跟其后、旧内容拷过去（成员数很小，不做预留两倍那套）。 */
      const size_t want = is_obj ? (size_t)(count * 2 + 2) : (size_t)(count + 1);
      struct dsh_json_node *big = reserve_nodes(ps, want);
      if (big == NULL) {
        dsh_set_last_error("内存不足：JSON 容器");
        return NULL;
      }
      memcpy(big, n->children, (size_t)count * (is_obj ? 2 : 1) * sizeof(*big));
      memset(big + (size_t)count * (is_obj ? 2 : 1), 0,
             ((size_t)(is_obj ? 2 : 1)) * sizeof(*big));
      if (is_obj) {
        big[count * 2].kind = DSH_JSON_STRING;
        big[count * 2].text = keys[count];
        big[count * 2].len = keylens[count];
      }
      n->children = big;
    }
    const struct dsh_json_node *v =
        parse_value(ps, (struct dsh_json_node *)&n->children[is_obj ? count * 2 + 1 : count],
                    is_obj ? keys[count] : NULL, is_obj ? keylens[count] : 0);
    if (v == NULL) return NULL;
    count++;

    skip_ws(ps);
    if (ps->p >= ps->end) {
      ps->err_at = ps->p;
      dsh_set_last_error("JSON 在容器结束前就没了（缺少 '%c'）", close);
      return NULL;
    }
    if (*ps->p == ',') {
      ps->p++;
      skip_ws(ps);
      /* 尾随逗号如实报出来，别让它掉到「不认识的字符」那一支（那条文案指不到真正的问题）。 */
      if (ps->p < ps->end && *ps->p == close) {
        ps->err_at = ps->p;
        dsh_set_last_error("JSON 容器里出现了既不是值也不是 '%c' 的字符（尾随逗号？）",
                           close);
        return NULL;
      }
      continue;
    }
    if (*ps->p == close) {
      ps->p++;
      break;
    }
    ps->err_at = ps->p;
    dsh_set_last_error("JSON 容器里出现了既不是 ',' 也不是 '%c' 的字符", close);
    return NULL;
  }
  ps->depth--;
  n->count = count;
  if (count == 0) n->children = NULL;
  /* 容器的原文片段：含两端的括号（从 `{` 到 `}`），中间的空白原样保留 */
  n->raw = raw_start;
  n->raw_len = (size_t)(ps->p - raw_start);
  return n;
}

/* ── 对外 ────────────────────────────────────────── */

int dsh_json_parse(const char *text, size_t len, dsh_json_doc **out) {
  if (out == NULL) {
    dsh_set_last_error("dsh_json_parse：out 不能为空");
    return -1;
  }
  *out = NULL;
  if (text == NULL) {
    dsh_set_last_error("dsh_json_parse：文本指针为空");
    return -1;
  }

  struct dsh_json_doc *doc = (struct dsh_json_doc *)dsh_mem_alloc(sizeof(*doc));
  if (doc == NULL) {
    dsh_set_last_error("内存不足：JSON 文档句柄");
    return -1;
  }
  doc->arena = NULL;
  doc->root = NULL;

  parser ps;
  ps.text = text;
  ps.p = text;
  ps.end = text + len;
  ps.doc = doc;
  ps.depth = 0;
  ps.err_at = NULL;

  const struct dsh_json_node *root = parse_value(&ps, NULL, NULL, 0);
  if (root == NULL) {
    /* 报错文案里补上字节偏移——那是判断哪一行错了的唯一线索。 */
    const char *saved = dsh_last_error_message();
    const long long off = (ps.err_at != NULL) ? (long long)(ps.err_at - text) : -1;
    if (saved != NULL && saved[0] != '\0' && off >= 0) {
      dsh_set_last_error("%s（位置：第 %lld 字节，共 %zu 字节）", saved, off, len);
    } else if (saved != NULL && saved[0] != '\0') {
      dsh_set_last_error("%s（共 %zu 字节）", saved, len);
    }
    if (saved != NULL) dsh_release((void *)saved);
    arena_release_all(doc);
    dsh_release(doc);
    return -1;
  }
  skip_ws(&ps);
  if (ps.p != ps.end) {
    dsh_set_last_error("JSON 解析失败：根值之后还有多余内容（第 %lld 字节处，共 %zu 字节）",
                       (long long)(ps.p - text), len);
    arena_release_all(doc);
    dsh_release(doc);
    return -1;
  }

  doc->root = root;
  *out = doc;
  return 0;
}

void dsh_json_doc_free(dsh_json_doc *doc) {
  if (doc == NULL) return;
  arena_release_all(doc);
  dsh_release(doc);
}

const dsh_json_node *dsh_json_doc_root(const dsh_json_doc *doc) {
  return (doc == NULL) ? NULL : doc->root;
}

int dsh_json_node_kind(const dsh_json_node *node) {
  return (node == NULL) ? 0 : (int)node->kind;
}
int dsh_json_is_null(const dsh_json_node *n) { return n != NULL && n->kind == DSH_JSON_NULL; }
int dsh_json_is_bool(const dsh_json_node *n) { return n != NULL && n->kind == DSH_JSON_BOOL; }
int dsh_json_is_number(const dsh_json_node *n) {
  return n != NULL && n->kind == DSH_JSON_NUMBER;
}
int dsh_json_is_string(const dsh_json_node *n) {
  return n != NULL && n->kind == DSH_JSON_STRING;
}
int dsh_json_is_array(const dsh_json_node *n) { return n != NULL && n->kind == DSH_JSON_ARRAY; }
int dsh_json_is_object(const dsh_json_node *n) { return n != NULL && n->kind == DSH_JSON_OBJECT; }

int dsh_json_bool_value(const dsh_json_node *node) {
  return (node != NULL && node->kind == DSH_JSON_BOOL && node->is_true) ? 1 : 0;
}

const char *dsh_json_str_value(const dsh_json_node *node, size_t *out_len) {
  if (out_len != NULL) *out_len = 0;
  if (node == NULL || node->kind != DSH_JSON_STRING) return NULL;
  if (out_len != NULL) *out_len = node->len;
  return node->text;
}

int dsh_json_i64_value(const dsh_json_node *node, int64_t *out) {
  if (out == NULL) return -1;
  *out = 0;
  if (node == NULL || node->kind != DSH_JSON_NUMBER) return -1;
  /* 自己按十进制解、不经过 strtod（那会把大整数变成浮点、悄悄丢精度）；位数超了如实报错，不截断。 */
  const char *p = node->text;
  const char *end = node->text + node->len;
  int neg = 0;
  if (p < end && *p == '-') { neg = 1; p++; }
  uint64_t acc = 0;
  const uint64_t limit = neg ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
  for (; p < end; p++) {
    if (*p == '.' || *p == 'e' || *p == 'E') return -1; /* 不是整数 */
    if (*p < '0' || *p > '9') return -1;
    const uint64_t d = (uint64_t)(*p - '0');
    if (acc > (limit - d) / 10u) return -1; /* 溢出 */
    acc = acc * 10u + d;
  }
  *out = neg ? (int64_t)(~acc + 1u) : (int64_t)acc;
  return 0;
}

int dsh_json_double_value(const dsh_json_node *node, double *out) {
  if (out == NULL) return -1;
  *out = 0.0;
  if (node == NULL || node->kind != DSH_JSON_NUMBER) return -1;
  /* strtod 认 locale 的小数点，而 JSON 永远是 '.'：中文/欧洲 locale 下 strtod(「1.5」)
   * 会停在 '.' 上只解出 1，所以先拷成 C 字符串、再在显式切到 C locale 的窗口里解一次。
   * 这一段是本层唯一与 locale 打交道的地方，集中在这里、别散开。 */
  char buf[64];
  if (node->len >= sizeof(buf)) return -1; /* 正常数值远短于 64；超了当异常 */
  memcpy(buf, node->text, node->len);
  buf[node->len] = '\0';

  const char *saved_locale = setlocale(LC_NUMERIC, NULL);
  char saved_copy[64];
  if (saved_locale != NULL) {
    strncpy(saved_copy, saved_locale, sizeof(saved_copy) - 1);
    saved_copy[sizeof(saved_copy) - 1] = '\0';
    saved_locale = saved_copy;
  }
  setlocale(LC_NUMERIC, "C");
  char *endp = NULL;
  errno = 0;
  const double v = strtod(buf, &endp);
  if (saved_locale != NULL) setlocale(LC_NUMERIC, saved_locale);
  if (endp == NULL || (size_t)(endp - buf) != node->len) return -1;
  if (errno == ERANGE) return -1;
  *out = v;
  return 0;
}

int64_t dsh_json_array_len(const dsh_json_node *node) {
  if (node == NULL || node->kind != DSH_JSON_ARRAY) return -1;
  return node->count;
}

const dsh_json_node *dsh_json_array_at(const dsh_json_node *node, int64_t index) {
  if (node == NULL || node->kind != DSH_JSON_ARRAY) return NULL;
  if (index < 0 || index >= node->count) return NULL;
  return &node->children[index];
}

int64_t dsh_json_object_len(const dsh_json_node *node) {
  if (node == NULL || node->kind != DSH_JSON_OBJECT) return -1;
  return node->count;
}

/* 对象的子节点是 [键][值][键][值]…，第 i 个成员的值是 children[2i+1]，键在它前面。 */
const dsh_json_node *dsh_json_object_at(const dsh_json_node *node, int64_t index,
                                        const char **out_key, size_t *out_key_len) {
  if (out_key != NULL) *out_key = NULL;
  if (out_key_len != NULL) *out_key_len = 0;
  if (node == NULL || node->kind != DSH_JSON_OBJECT) return NULL;
  if (index < 0 || index >= node->count) return NULL;
  const dsh_json_node *child = &node->children[index * 2 + 1];
  if (out_key != NULL) *out_key = child->key;
  if (out_key_len != NULL) *out_key_len = child->key_len;
  return child;
}

const dsh_json_node *dsh_json_object_get(const dsh_json_node *node, const char *name) {
  if (node == NULL || node->kind != DSH_JSON_OBJECT || name == NULL) return NULL;
  const size_t n = strlen(name);
  for (int64_t i = 0; i < node->count; i++) {
    const dsh_json_node *child = &node->children[i * 2 + 1];
    if (child->key_len == n && child->key != NULL && memcmp(child->key, name, n) == 0) {
      return child;
    }
  }
  return NULL;
}

const char *dsh_json_node_raw(const dsh_json_node *node, size_t *out_len) {
  if (out_len != NULL) *out_len = 0;
  if (node == NULL || node->raw == NULL) return NULL;
  if (out_len != NULL) *out_len = node->raw_len;
  return node->raw;
}

