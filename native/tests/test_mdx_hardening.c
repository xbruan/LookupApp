/* 内核单元测试 · MDict 解析器的加固（2026-09 审计 F2 / F3）
 *
 * 钉的是「**坏文件能不能把解析器推到不该到的地方**」，所以每条用例都是三步：
 *   ① 在临时目录里**当场合成**一本词典（坏文件不进仓库，都是几十到几万字节）；
 *   ② 开它，断言它**如实报错** —— 不崩、不溢出、不按头里的声明值去狂分配；
 *      对好文件则反过来，断言它**照常读得出来**（防「检查加过头、把好文件也拒了」）；
 *   ③ 每条反例都配一份**只差那一处**的对照件（同一个构造函数换个参数），
 *      否则分不清「检查对了」还是「构造得本来就不成形」。
 *
 * 两条缺陷（见 `docs/code-review-2026-09-29.md`）：
 *   F2：记录块索引的累计长度可以加成 64 位溢出 —— 单项 ≤ 2^53 合法，**总和**不一定合法；
 *   F3：346 字节的文件声明 1000 万个词块，解析器按声明值去要 640,000,000 字节。
 *
 * ⚠️ 断言「被哪一关拦下」时用的是 last_error 里的**关键短语**（不是整句）：这是唯一能把
 *    「按字节容量拦下的」与「碰巧因为别的原因失败的」分开的办法 —— 只断言 rc != 0 的话，
 *    一个把好文件也一起拒掉的实现照样全绿。
 */

#define _POSIX_C_SOURCE 200809L

#include "dict/dsh_mdx.h"
#include "dsh_lookup.h"
#include "mem_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef DSH_TESTDATA_DIR
#error "需要 -DDSH_TESTDATA_DIR=<0.2.0/testdata 的路径>（见 Makefile）"
#endif

/* ⚠️ 下面三个数必须与 `native/src/dict/dsh_mdx.c` 顶上那份「解析期规模预算」**一致**。
 * 为什么测试要抄一份：边界那两条（正好卡在上限上 / 刚过上限）**只能**用确定的口径来判，
 * 而预算常量住在 .c 里、不在公开头上（公开头不许动）。抄一份的代价是：谁改了预算而没改这里，
 * 本组当场变红 —— 那正是「不许悄悄放宽」想要的效果。 */
#define TEST_UNPACK_RATIO 16384LL
#define TEST_UNPACK_FLOOR ((int64_t)1 << 20)
#define TEST_MAX_BLOCK_UNPACK ((int64_t)512 * 1024 * 1024)

static int g_checks = 0;
static int g_failed = 0;

static void ok(int condition, const char *what) {
  g_checks++;
  if (!condition) {
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
  if (actual == NULL || strcmp(actual, expected) != 0) {
    g_failed++;
    fprintf(stderr, "FAIL %s：实际=%s 期望=%s\n", what, actual ? actual : "(null)", expected);
  }
}

/** 活分配表有没有回到基线（只在「所有对象都已关闭」之后调用） */
static void ok_no_leak(const char *stage) {
  g_checks++;
  const size_t live = dsh_mem_live_count();
  if (live != 0) {
    g_failed++;
    fprintf(stderr, "FAIL 泄漏于「%s」之后：活分配表还剩 %zu 条（期望 0）\n", stage, live);
  }
}

/** 上一条 last_error 里有没有这段短语（复制出来判，判完就还回去 —— 它也算一次内核分配） */
static int last_error_has(const char *needle) {
  const char *msg = dsh_last_error_message();
  const int hit = (msg != NULL && strstr(msg, needle) != NULL);
  if (msg != NULL) dsh_release((void *)msg);
  return hit;
}

/** 打开一个文件，返回 rc；失败时把 last_error 打到 stderr（红的时候能自己说出现场） */
static int open_rc(const char *path, const char *label) {
  dsh_mdx *mdx = NULL;
  const int rc = dsh_mdx_open(path, &mdx);
  if (rc != 0) {
    const char *msg = dsh_last_error_message();
    fprintf(stderr, "      %s 打开失败：%s\n", label, msg ? msg : "(null)");
    if (msg != NULL) dsh_release((void *)msg);
    ok(mdx == NULL, "失败时句柄必须保持 NULL");
  }
  if (mdx != NULL) dsh_mdx_close(mdx);
  return rc;
}

/* ── 临时目录 ──────────────────────────────────────────────────────────── */

static char g_root[64]; /* mkdtemp 就地改写的模板 */

static const char *tmp_dir(void) {
  snprintf(g_root, sizeof(g_root), "/tmp/dsh-mdx-hardening-XXXXXX");
  if (mkdtemp(g_root) == NULL) return NULL;
  return g_root;
}

static char *tmp_path(const char *name) {
  const size_t n = strlen(g_root) + strlen(name) + 2;
  char *p = (char *)malloc(n);
  if (p == NULL) return NULL;
  snprintf(p, n, "%s/%s", g_root, name);
  return p;
}

static int write_file(const char *path, const uint8_t *data, size_t len) {
  FILE *fp = fopen(path, "wb");
  if (fp == NULL) return -1;
  const size_t written = (len > 0) ? fwrite(data, 1, len, fp) : 0;
  fclose(fp);
  return (written == len) ? 0 : -1;
}

/* ── 合成词典用的小工具 ───────────────────────────────────────────────────
 * 一律按 v2.0 / 8 字节宽度 / UTF-8 / **不压缩**（ctype = 0）来造：不压缩就不必真的编 zlib
 * 数据，坏值改在哪一处、好坏两份只差那几个字节，一眼能对上。 */

typedef struct {
  uint8_t *data;
  size_t len;
  size_t cap;
} bytes;

static int bytes_put(bytes *b, const void *p, size_t n) {
  if (b->len + n > b->cap) {
    size_t cap = (b->cap > 0) ? b->cap : 256;
    while (cap < b->len + n) cap *= 2;
    uint8_t *grown = (uint8_t *)realloc(b->data, cap);
    if (grown == NULL) return -1;
    b->data = grown;
    b->cap = cap;
  }
  if (n > 0) memcpy(b->data + b->len, p, n);
  b->len += n;
  return 0;
}

/** 大端整数（宽度 2 / 8；v2.0 的 numWidth 是 8，词条名长度字段是 2） */
static int bytes_be(bytes *b, int width, int64_t v) {
  uint8_t t[8];
  const uint64_t u = (uint64_t)v;
  for (int i = 0; i < width; i++) t[i] = (uint8_t)(u >> (8 * (width - 1 - i)));
  return bytes_put(b, t, (size_t)width);
}

static int bytes_zeros(bytes *b, size_t n) {
  static const uint8_t zero[64] = {0};
  size_t left = n;
  while (left > 0) {
    const size_t step = (left < sizeof(zero)) ? left : sizeof(zero);
    if (bytes_put(b, zero, step) != 0) return -1;
    left -= step;
  }
  return 0;
}

/** 逐字符展成 UTF-16LE —— 只支持 ASCII（头部文本本来就只放 ASCII） */
static int bytes_utf16le_ascii(bytes *b, const char *text) {
  for (const char *p = text; *p != '\0'; p++) {
    const uint8_t pair[2] = {(uint8_t)*p, 0};
    if (bytes_put(b, pair, 2) != 0) return -1;
  }
  return 0;
}

/** 把缓冲区补 0 撑到至少 n 字节（键信息块的负载长度要**可控**才能钉边界） */
static int bytes_pad_to(bytes *b, size_t base, size_t n) {
  return (n > base) ? bytes_zeros(b, n - base) : 0;
}

/* ── 合成词典：真值与「改成坏值的那几处」 ───────────────────────────────── */

static const char *const FIX_KEYS[4] = {"apple", "apply", "banana", "cherry"};
static const char *const FIX_TEXTS[4] = {"apple", "apply", "<b>banana</b>", "<b>cherry</b>"};
/* 2 个词块、每块 2 条；记录区 4 块、每块 1 条 */
static const char *const FIX_FIRST[2] = {"apple", "banana"};
static const char *const FIX_LAST[2] = {"apply", "cherry"};

static const char FIX_HEADER[] =
    "<Dictionary GeneratedByEngineVersion=\"2.0\" Encoding=\"UTF-8\" Encrypted=\"0\" "
    "Title=\"hardening\"/>";

/* 全部字段的语义：**-1 = 用真值**。每条反例都从「全是真值」出发只改一处。 */
typedef struct {
  int64_t rec_blocks_declared;    /* 记录区头部声明的记录块数 */
  int64_t rec_info_entries;       /* 记录信息块里**实际**写几项 */
  int64_t rec_pack_declared;      /* 每项声明的压缩大小 */
  int64_t rec_unpack_declared;    /* 每项声明的解压大小 */
  int64_t rec_comp_size_declared; /* 记录区头部声明的压缩总量 */
  int64_t rec_data_bytes;         /* 记录块数据**实际**写几个字节 */
  int64_t key_blocks_declared;    /* 键区头部声明的词块数 */
  int64_t key_info_entries;       /* 键信息块里**实际**写几条真实条目（0/1/2） */
  int64_t key_info_bytes;         /* 键信息块负载**实际**的字节数（不足补 0） */
  int64_t key_count_declared;     /* 键区头部声明的词条总数 */
  int64_t key_pack_declared;      /* 每个词块声明的压缩大小 */
  int64_t key_unpack_declared;    /* 每个词块声明的解压大小 */
} fixture_spec;

/** 「全是真值」的起点：只改一处，才分得清是那一处把解析器推歪的 */
static fixture_spec truth(void) {
  fixture_spec s;
  s.rec_blocks_declared = -1;
  s.rec_info_entries = -1;
  s.rec_pack_declared = -1;
  s.rec_unpack_declared = -1;
  s.rec_comp_size_declared = -1;
  s.rec_data_bytes = -1;
  s.key_blocks_declared = -1;
  s.key_info_entries = -1;
  s.key_info_bytes = -1;
  s.key_count_declared = -1;
  s.key_pack_declared = -1;
  s.key_unpack_declared = -1;
  return s;
}

static int64_t pick(int64_t override, int64_t actual) {
  return (override >= 0) ? override : actual;
}

/**
 * 造一本结构完整的 v2.0 词典，再按 spec 把点名的几处改成声明值。
 * 返回新分配的缓冲区（调用方 free），失败返回 NULL；`*out_len` 是**磁盘上的字节数**。
 */
static uint8_t *build_fixture(const fixture_spec *spec, size_t *out_len) {
  int64_t rec_real = 4; /* 真值：4 条记录、每条一块 */
  int64_t pack_real[4];
  size_t pack_stream_len = 0;
  for (int i = 0; i < 4; i++) {
    pack_real[i] = 8 + (int64_t)strlen(FIX_TEXTS[i]) + 1; /* 8 = 不压缩块的类型/adler 头 */
    pack_stream_len += (size_t)pack_real[i];
  }
  int64_t key_data_len[2];
  /* 每个词块的数据 = 8 字节块头（压缩类型 + adler32）+ 该块各条 {记录偏移(8) + 键名 + \0} */
  key_data_len[0] = 8 + 2 * (8 + (int64_t)strlen(FIX_KEYS[0]) + 1);
  key_data_len[1] = 8 + 2 * (8 + (int64_t)strlen(FIX_KEYS[2]) + 1);
  const int64_t key_data_total = key_data_len[0] + key_data_len[1];

  bytes b;
  b.data = NULL;
  b.len = 0;
  b.cap = 0;

  /* ① 头部：u32 长度 + UTF-16LE 正文 + 4 字节 adler32（不校验，写 0） */
  if (bytes_be(&b, 4, (int64_t)strlen(FIX_HEADER) * 2) != 0 ||
      bytes_utf16le_ascii(&b, FIX_HEADER) != 0 || bytes_zeros(&b, 4) != 0) {
    free(b.data);
    return NULL;
  }

  /* ② 键信息块的负载（先算出来，头部里要写它的长度） */
  bytes info;
  info.data = NULL;
  info.len = 0;
  info.cap = 0;
  const int64_t info_entries_written = pick(spec->key_info_entries, 2);
  for (int64_t i = 0; i < info_entries_written && i < 2; i++) {
    const int64_t pack = pick(spec->key_pack_declared, key_data_len[i]);
    const int64_t unpack = pick(spec->key_unpack_declared, key_data_len[i]);
    const size_t first_len = strlen(FIX_FIRST[i]);
    const size_t last_len = strlen(FIX_LAST[i]);
    if (bytes_be(&info, 8, 2) != 0 ||                     /* entry_count */
        bytes_be(&info, 2, (int64_t)first_len) != 0 ||    /* 长度字段：v2.0 不含 \0，解析时会 +1 */
        bytes_put(&info, FIX_FIRST[i], first_len + 1) != 0 ||
        bytes_be(&info, 2, (int64_t)last_len) != 0 ||
        bytes_put(&info, FIX_LAST[i], last_len + 1) != 0 ||
        bytes_be(&info, 8, pack) != 0 || bytes_be(&info, 8, unpack) != 0) {
      free(info.data);
      free(b.data);
      return NULL;
    }
  }
  const size_t info_real_len = info.len;
  const int64_t info_bytes = pick(spec->key_info_bytes, (int64_t)info_real_len);

  /* ③ 键区头部：5 × 8 字节 + 4 字节 adler32 */
  if (bytes_be(&b, 8, pick(spec->key_blocks_declared, 2)) != 0 ||
      bytes_be(&b, 8, pick(spec->key_count_declared, 4)) != 0 ||
      bytes_be(&b, 8, info_bytes) != 0 ||                     /* 解压后大小（不压缩时 = 负载长度） */
      bytes_be(&b, 8, info_bytes + 8) != 0 ||                 /* 压缩后大小（含 8 字节块头） */
      bytes_be(&b, 8, key_data_total) != 0 || bytes_zeros(&b, 4) != 0) {
    free(info.data);
    free(b.data);
    return NULL;
  }

  /* ④ 键信息块：8 字节块头（ctype = 0 不压缩、adler = 0）+ 负载 */
  if (bytes_zeros(&b, 8) != 0) {
    free(info.data);
    free(b.data);
    return NULL;
  }
  {
    size_t base = b.len;
    if (bytes_put(&b, info.data, info_real_len) != 0 ||
        bytes_pad_to(&b, base + info_real_len, (size_t)info_bytes) != 0) {
      free(info.data);
      free(b.data);
      return NULL;
    }
  }
  free(info.data);

  /* ⑤ 词块数据：每块 = 8 字节块头（ctype = 0 不压缩、adler = 0）+ 各条 {记录偏移(8) + 键名 + \0} */
  {
    static const int which[2][2] = {{0, 1}, {2, 3}};
    const int64_t rec_offset[4] = {0,
                                   6,
                                   6 + (int64_t)strlen(FIX_TEXTS[1]) + 1,
                                   6 + (int64_t)strlen(FIX_TEXTS[1]) + 1 +
                                       (int64_t)strlen(FIX_TEXTS[2]) + 1};
    for (int bi = 0; bi < 2; bi++) {
      if (bytes_zeros(&b, 8) != 0) {
        free(b.data);
        return NULL;
      }
      for (int k = 0; k < 2; k++) {
        const int idx = which[bi][k];
        const size_t klen = strlen(FIX_KEYS[idx]);
        if (bytes_be(&b, 8, rec_offset[idx]) != 0 ||
            bytes_put(&b, FIX_KEYS[idx], klen + 1) != 0) {
          free(b.data);
          return NULL;
        }
      }
    }
  }

  /* ⑥ 记录区头部：4 × 8 字节 */
  const int64_t rec_blocks = pick(spec->rec_blocks_declared, rec_real);
  const int64_t rec_info_entries = pick(spec->rec_info_entries, rec_blocks);
  int64_t rec_pack_sum = 0;
  for (int64_t i = 0; i < rec_info_entries; i++) {
    rec_pack_sum += pick(spec->rec_pack_declared, (i < rec_real) ? pack_real[i] : 0);
  }
  if (bytes_be(&b, 8, rec_blocks) != 0 ||
      bytes_be(&b, 8, pick(spec->key_count_declared, 4)) != 0 || /* 词条数：与键区对得上 */
      bytes_be(&b, 8, rec_info_entries * 16) != 0 ||
      bytes_be(&b, 8, pick(spec->rec_comp_size_declared, rec_pack_sum)) != 0) {
    free(b.data);
    return NULL;
  }

  /* ⑦ 记录信息块：每项 {压缩大小, 解压大小} */
  for (int64_t i = 0; i < rec_info_entries; i++) {
    const int64_t real_pack = (i < rec_real) ? pack_real[i] : 0;
    const int64_t real_unpack = (i < rec_real) ? (int64_t)strlen(FIX_TEXTS[i]) + 1 : 0;
    if (bytes_be(&b, 8, pick(spec->rec_pack_declared, real_pack)) != 0 ||
        bytes_be(&b, 8, pick(spec->rec_unpack_declared, real_unpack)) != 0) {
      free(b.data);
      return NULL;
    }
  }

  /* ⑧ 记录块数据：每块 = 8 字节块头（ctype = 0、adler = 0）+ 记录正文 + \0 */
  {
    bytes stream;
    stream.data = NULL;
    stream.len = 0;
    stream.cap = 0;
    for (int i = 0; i < 4; i++) {
      if (bytes_zeros(&stream, 8) != 0 ||
          bytes_put(&stream, FIX_TEXTS[i], strlen(FIX_TEXTS[i]) + 1) != 0) {
        free(stream.data);
        free(b.data);
        return NULL;
      }
    }
    const size_t want = (size_t)pick(spec->rec_data_bytes, (int64_t)stream.len);
    const size_t take = (want < stream.len) ? want : stream.len;
    if (bytes_put(&b, stream.data, take) != 0 ||
        bytes_pad_to(&b, b.len, b.len + (want - take)) != 0) {
      free(stream.data);
      free(b.data);
      return NULL;
    }
    free(stream.data);
  }

  *out_len = b.len;
  return b.data;
}

/** 合成一份、写盘、返回路径（调用方 free）；失败返回 NULL */
static char *make_fixture(const char *name, const fixture_spec *spec, size_t *out_len) {
  size_t len = 0;
  uint8_t *data = build_fixture(spec, &len);
  if (data == NULL) return NULL;
  char *path = tmp_path(name);
  if (path == NULL || write_file(path, data, len) != 0) {
    free(data);
    free(path);
    return NULL;
  }
  free(data);
  if (out_len != NULL) *out_len = len;
  return path;
}

int main(void) {
  const size_t base = dsh_mem_live_count();

  if (tmp_dir() == NULL) {
    fprintf(stderr, "建不了临时目录，这一组没法跑\n");
    return 2;
  }

  /* ── ① 好文件照常：合成的这本必须能开、能查、能逐块取键（防「检查加过头」）── */
  {
    size_t len = 0;
    char *path = make_fixture("sane.mdx", &(fixture_spec){-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1}, &len);
    ok(path != NULL && len > 0, "① 合成一本结构完整的词典");
    if (path != NULL) {
      fprintf(stderr, "      ① 合成词典 %zu 字节\n", len);
      dsh_mdx *m = NULL;
      ok_eq_i64(dsh_mdx_open(path, &m), 0, "① 结构完整的合成词典必须打得开");
      if (m != NULL) {
        ok_eq_i64(dsh_mdx_key_count(m), 4, "① 词条数");
        ok_eq_i64(dsh_mdx_key_block_count(m), 2, "① 词块数");
        ok_eq_i64(dsh_mdx_record_block_count(m), 4, "① 记录块数");
        ok_eq_i64(dsh_mdx_total_record_unpacked(m), 40, "① 记录区解压总量");
        ok_eq_i64(dsh_mdx_warning_count(m), 0, "① 好文件不该产生任何警告");
        ok(dsh_mdx_block_order_monotone(m) == 1, "① 词块索引应当单调");

        char **keys = NULL;
        int64_t count = 0;
        ok_eq_i64(dsh_mdx_list_keys(m, &keys, &count), 0, "① 枚举全部键");
        ok_eq_i64(count, 4, "① 枚举出来的键条数");
        for (int64_t i = 0; i < count && i < 4; i++) {
          ok_eq_str(keys[i], FIX_KEYS[i], "① 逐条比对键名");
        }
        dsh_mdx_free_keys(keys, count);

        char **block0 = NULL;
        int64_t block0_count = 0;
        ok_eq_i64(dsh_mdx_block_keys(m, 0, &block0, &block0_count), 0, "① 取第 0 个词块的键");
        ok_eq_i64(block0_count, 2, "① 第 0 个词块的键条数");
        if (block0_count == 2) {
          ok_eq_str(block0[0], "apple", "① 第 0 块第 0 个键");
          ok_eq_str(block0[1], "apply", "① 第 0 块第 1 个键");
        }
        dsh_mdx_free_keys(block0, block0_count);

        char *landed = NULL;
        ok_eq_i64(dsh_mdx_lookup_key(m, "banana", &landed, NULL), 1, "① 查 banana 要命中");
        ok_eq_str(landed, "banana", "① 命中时的规范键名");
        if (landed != NULL) dsh_release(landed);
        ok_eq_i64(dsh_mdx_lookup_key(m, "nope", &landed, NULL), 0, "① 不存在的键如实返回没有");
        if (landed != NULL) dsh_release(landed);

        char *text = NULL;
        int64_t text_len = 0;
        ok_eq_i64(dsh_mdx_fetch(m, "cherry", &landed, &text, &text_len), 1, "① 按内容取 cherry");
        ok(text != NULL && strstr(text, "<b>cherry</b>") != NULL, "① cherry 的正文要对得上");
        if (landed != NULL) dsh_release(landed);
        if (text != NULL) dsh_release(text);
        dsh_mdx_close(m);
      }
      remove(path);
      free(path);
      ok_no_leak("① 打开、枚举、查询并关闭合成的词典");
    }
  }

  /* ── ② 真测试用词典照常（合成的那本以外，真文件也不能被新检查误伤）── */
  {
    char path[1024];
    snprintf(path, sizeof(path), "%s/test.mdx", DSH_TESTDATA_DIR);
    dsh_mdx *m = NULL;
    ok_eq_i64(dsh_mdx_open(path, &m), 0, "② 真测试用词典 test.mdx 必须照常打开");
    if (m != NULL) {
      char *landed = NULL;
      char *text = NULL;
      int64_t text_len = 0;
      ok_eq_i64(dsh_mdx_fetch(m, "apple", &landed, &text, &text_len), 1, "② 真词典查 apple");
      ok(text != NULL, "② 真词典的正文取回来了");
      if (landed != NULL) dsh_release(landed);
      if (text != NULL) dsh_release(text);
      dsh_mdx_close(m);
    }
    ok_no_leak("② 真测试用词典开关一遍");
  }

  /* ── ③ F2 的复现件：1,025 个记录块，每项声明解压 2^53 ──
   * 单项合法（read_be 允许到 2^53），**总和**不合法：不加检查就是 UBSan 的
   * signed integer overflow（审计原文：9214364837600034816 + 9007199254740992）。 */
  {
    const int64_t huge = (int64_t)1 << 53;
    fixture_spec s = truth();
    s.rec_blocks_declared = 1025;
    s.rec_info_entries = 1025;
    s.rec_pack_declared = 0;
    s.rec_unpack_declared = huge;
    s.rec_comp_size_declared = 0;
    s.rec_data_bytes = 0;
    size_t len = 0;
    char *path = make_fixture("overflow.mdx", &s, &len);
    ok(path != NULL, "③ 合成 F2 复现件");
    if (path != NULL) {
      fprintf(stderr, "      ③ F2 复现件 %zu 字节（审计那份是 16,720 字节）\n", len);
      ok(open_rc(path, "③ F2 复现件") != 0, "③ 每项声明 2^53 的复现件必须**报错**（不是溢出后继续）");
      ok(last_error_has("超出单块上限"), "③ 报的应当是「解压后大小超出单块上限」这一关");
      ok_no_leak("③ F2 复现件被拒之后");

      /* 对照件：**同一个文件**，只把那个荒诞的解压长度改成 0 → 就能打开。
       * 这一条把「拒绝是因为那个数」钉死，而不是「构造得本来就不成形」。 */
      fixture_spec t = s;
      t.rec_unpack_declared = 0;
      char *ok_path = make_fixture("overflow-control.mdx", &t, NULL);
      ok(ok_path != NULL, "③ 合成 F2 对照件");
      if (ok_path != NULL) {
        ok_eq_i64(open_rc(ok_path, "③ F2 对照件"), 0,
                  "③ 只把解压长度改成 0 → 同一个文件必须能打开（证明拦的是那个数）");
        remove(ok_path);
        free(ok_path);
      }
      ok_no_leak("③ F2 对照件开关一遍");
      remove(path);
      free(path);
    }
  }

  /* ── ④ 累计总量说不通：64 块 × 1 MiB = 64 MiB，远超「文件长度 × 倍数」的口径 ──
   * 每一块单看都在单块上限之内，是**累加**出来的总量才说不通 —— F2 的另一半。 */
  {
    fixture_spec s = truth();
    s.rec_blocks_declared = 64;
    s.rec_info_entries = 64;
    s.rec_pack_declared = 0;
    s.rec_unpack_declared = (int64_t)1 << 20; /* 1 MiB/块：单块合法 */
    s.rec_comp_size_declared = 0;
    s.rec_data_bytes = 0;
    char *path = make_fixture("total-budget.mdx", &s, NULL);
    ok(path != NULL, "④ 合成累计总量超预算的件");
    if (path != NULL) {
      ok(open_rc(path, "④ 累计总量") != 0, "④ 累计解压总量与文件大小不符 → 必须报错");
      ok(last_error_has("与文件大小不符"), "④ 报的应当是「解压总长度与文件大小不符」这一关");
      remove(path);
      free(path);
    }
    ok_no_leak("④ 累计总量超预算的件");
  }

  /* ── ⑤ 单块上限（记录区与键区各一条）── */
  {
    fixture_spec s = truth();
    s.rec_unpack_declared = TEST_MAX_BLOCK_UNPACK + 1;
    char *path = make_fixture("rec-block-cap.mdx", &s, NULL);
    ok(path != NULL, "⑤ 合成记录区超单块上限的件");
    if (path != NULL) {
      ok(open_rc(path, "⑤ 记录区单块上限") != 0, "⑤ 记录块解压大小超上限 → 必须报错");
      ok(last_error_has("超出单块上限"), "⑤ 报的应当是「超出单块上限」这一关");
      remove(path);
      free(path);
    }

    fixture_spec u = truth();
    u.key_unpack_declared = TEST_MAX_BLOCK_UNPACK + 1;
    char *kpath = make_fixture("key-block-cap.mdx", &u, NULL);
    ok(kpath != NULL, "⑤ 合成键区超单块上限的件");
    if (kpath != NULL) {
      ok(open_rc(kpath, "⑤ 键区单块上限") != 0, "⑤ 词块解压大小超上限 → 必须报错");
      ok(last_error_has("超出单块上限"), "⑤ 报的应当是「超出单块上限」这一关");
      remove(kpath);
      free(kpath);
    }

    /* 键区的累计总量：两块各 512 MiB（都在单块上限之内），加起来 1 GiB 与文件不符 */
    fixture_spec v = truth();
    v.key_unpack_declared = TEST_MAX_BLOCK_UNPACK;
    char *vpath = make_fixture("key-total.mdx", &v, NULL);
    ok(vpath != NULL, "⑤ 合成键区累计总量超预算的件");
    if (vpath != NULL) {
      ok(open_rc(vpath, "⑤ 键区累计总量") != 0, "⑤ 词块解压总长度与文件大小不符 → 必须报错");
      ok(last_error_has("与文件大小不符"), "⑤ 报的应当是「解压总长度与文件大小不符」这一关");
      remove(vpath);
      free(vpath);
    }
    ok_no_leak("⑤ 单块上限与键区总量三条");
  }

  /* ── ⑥ 解压总量的**边界**：正好卡在预算上 → 照常打开；刚过一点 → 报错 ──
   * 预算 = max(文件长度 × 倍数, 1 MiB)，所以要先造一份拿到文件长度，再按它算上限。 */
  {
    fixture_spec s = truth();
    s.rec_blocks_declared = 1;
    s.rec_info_entries = 1;
    s.rec_pack_declared = 8;
    s.rec_unpack_declared = 0; /* 先占位：文件长度与这个数无关（都是 8 字节字段） */
    s.rec_comp_size_declared = 8;
    s.rec_data_bytes = 8;
    size_t len = 0;
    uint8_t *probe = build_fixture(&s, &len);
    ok(probe != NULL && len > 0, "⑥ 先合成一份量文件长度");
    if (probe != NULL) {
      free(probe);
      int64_t cap = (int64_t)len * TEST_UNPACK_RATIO;
      if (cap < TEST_UNPACK_FLOOR) cap = TEST_UNPACK_FLOOR;
      fprintf(stderr, "      ⑥ 文件 %zu 字节 → 解压总量上限 %lld 字节\n", len, (long long)cap);
      ok(cap <= TEST_MAX_BLOCK_UNPACK, "⑥ 这份件的上限必须落在**单块**上限之内（否则验的就不是总量那一关）");

      fixture_spec at = s;
      at.rec_unpack_declared = cap; /* 正好在上限上 */
      char *at_path = make_fixture("total-at-limit.mdx", &at, NULL);
      ok(at_path != NULL, "⑥ 合成「正好在上限上」的件");
      if (at_path != NULL) {
        dsh_mdx *m = NULL;
        ok_eq_i64(dsh_mdx_open(at_path, &m), 0, "⑥ 总量**正好**在预算上 → 必须照常打开");
        if (m != NULL) {
          ok_eq_i64(dsh_mdx_total_record_unpacked(m), cap, "⑥ 正好在预算上时解压总量照实报出来");
          dsh_mdx_close(m);
        }
        remove(at_path);
        free(at_path);
      }

      fixture_spec past = s;
      past.rec_unpack_declared = cap + 1; /* 刚过一点 */
      char *past_path = make_fixture("total-past-limit.mdx", &past, NULL);
      ok(past_path != NULL, "⑥ 合成「刚过上限」的件");
      if (past_path != NULL) {
        ok(open_rc(past_path, "⑥ 总量刚过预算") != 0, "⑥ 总量刚过预算 → 必须干净地报错");
        ok(last_error_has("与文件大小不符"), "⑥ 报的应当是总量那一关");
        remove(past_path);
        free(past_path);
      }
      ok_no_leak("⑥ 总量边界两条");
    }
  }

  /* ── ⑦ F3：词块数声明得远超信息块**实际**能装的条数 ──
   * 341/346 字节那种件声明 10,000,000 个词块 —— 修之前解析器按声明值要 640,000,000 字节。 */
  {
    fixture_spec s = truth();
    s.key_blocks_declared = 10000000;
    size_t len = 0;
    char *path = make_fixture("huge-count.mdx", &s, &len);
    ok(path != NULL, "⑦ 合成 F3 声明件");
    if (path != NULL) {
      fprintf(stderr, "      ⑦ F3 声明件 %zu 字节（审计那份是 346 字节）\n", len);
      ok(len < 4096, "⑦ 这份件必须是**很小**的文件（F3 的要点就是小文件诱导大分配）");
      ok(open_rc(path, "⑦ F3 声明件") != 0, "⑦ 声明 1000 万个词块 → 必须报错");
      ok(last_error_has("键信息块声明有"), "⑦ 报的应当是「按信息块字节长度推出来的条数上限」这一关");
      ok(last_error_has("声明与内容不符"), "⑦ 报错要说清是「声明与内容不符」，不是别的失败");
      remove(path);
      free(path);
    }

    /* 对照件：结构一模一样，只把词块数改成真值 2 → 必须能打开 */
    fixture_spec c = truth();
    char *cpath = make_fixture("huge-count-control.mdx", &c, NULL);
    ok(cpath != NULL, "⑦ 合成 F3 对照件");
    if (cpath != NULL) {
      ok_eq_i64(open_rc(cpath, "⑦ F3 对照件"), 0, "⑦ 只把词块数改回真值 → 必须能打开");
      remove(cpath);
      free(cpath);
    }
    ok_no_leak("⑦ F3 声明件与对照件");
  }

  /* ── ⑧ F3 的**边界**：条数正好等于「信息块字节数 ÷ 每项最小字节数」时不许被容量那一关拦下，
   * 再多一条就必须拦下。上限是**按实际字节长度推的**，不是拍脑袋的常数 —— 这两条钉的就是它。 */
  {
    const int64_t per_entry_min = 20; /* v2.0：词条数(8) + 首词长(2) + 尾词长(2) + 压缩(8) + 解压(8) */
    const int64_t payload = 200;      /* 200 / 20 = 10 条 */
    const int64_t cap_n = payload / per_entry_min;
    ok_eq_i64(cap_n, 10, "⑧ 这份件的容量上限应当是 10 条");

    fixture_spec s = truth();
    s.key_info_bytes = payload;
    s.key_blocks_declared = cap_n; /* 正好在上限上 */
    s.key_count_declared = 4;
    char *path = make_fixture("key-info-at-limit.mdx", &s, NULL);
    ok(path != NULL, "⑧ 合成「正好在上限上」的件");
    if (path != NULL) {
      /* 这个件的条目内容本来就不够 10 条（补 0 的部分读不出东西），所以它会失败 ——
       * 但**不许**是容量那一关拦下的：容量算紧了就会把这种件和合规件一起误伤。 */
      ok(open_rc(path, "⑧ 条数正好在上限上") != 0, "⑧ 内容不够时仍然要如实报错");
      ok(!last_error_has("声明与内容不符"), "⑧ 条数正好等于容量上限 → 不许说「声明与内容不符」");
      remove(path);
      free(path);
    }

    fixture_spec s2 = s;
    s2.key_blocks_declared = cap_n + 1; /* 刚过一条 */
    char *path2 = make_fixture("key-info-past-limit.mdx", &s2, NULL);
    ok(path2 != NULL, "⑧ 合成「刚过上限」的件");
    if (path2 != NULL) {
      ok(open_rc(path2, "⑧ 条数刚过上限") != 0, "⑧ 声明条数刚过容量上限 → 必须报错");
      ok(last_error_has("声明与内容不符"), "⑧ 报的应当是容量那一关（多一条就拦）");
      remove(path2);
      free(path2);
    }
    ok_no_leak("⑧ 容量边界两条");
  }

  /* ── ⑨ 记录块索引的项数与索引自己的字节长度对不上 ── */
  {
    fixture_spec s = truth();
    s.rec_blocks_declared = 100; /* 信息块里只写了 1 项（16 字节）→ 装不下 100 项 */
    s.rec_info_entries = 1;
    char *path = make_fixture("rec-index-mismatch.mdx", &s, NULL);
    ok(path != NULL, "⑨ 合成记录块索引项数不符的件");
    if (path != NULL) {
      ok(open_rc(path, "⑨ 记录块索引项数不符") != 0, "⑨ 声明 100 块而索引只有 16 字节 → 必须报错");
      ok(last_error_has("记录信息块声明有"), "⑨ 报的应当是记录信息块那一关");
      ok(last_error_has("声明与内容不符"), "⑨ 报错要说清是「声明与内容不符」");
      remove(path);
      free(path);
    }
    ok_no_leak("⑨ 记录块索引项数不符的件");
  }

  /* ── ⑩ 声明的压缩区间落在文件之外（记录区、键区各一条）──
   * 头里记的压缩总量是对的（区间检查在总量那关过得了），越界的是**单项**自己。 */
  {
    fixture_spec s = truth();
    s.rec_blocks_declared = 2;
    s.rec_info_entries = 2;
    s.rec_pack_declared = (int64_t)1 << 20; /* 每项声明 1 MiB */
    s.rec_unpack_declared = 8;
    s.rec_comp_size_declared = 16; /* 头里的总量写小一点，让总量那一关过得了 */
    s.rec_data_bytes = 32;         /* 真数据要够 16 字节，好让**段级**的区间检查也过得了 */
    char *path = make_fixture("rec-range-outside.mdx", &s, NULL);
    ok(path != NULL, "⑩ 合成记录区压缩区间越界的件");
    if (path != NULL) {
      ok(open_rc(path, "⑩ 记录区压缩区间越界") != 0, "⑩ 压缩区间落到文件之外 → 必须报错");
      ok(last_error_has("之外"), "⑩ 报的应当是「落到文件之外」这一关");
      remove(path);
      free(path);
    }

    fixture_spec k = truth();
    k.key_pack_declared = (int64_t)1 << 20; /* 第 0 块的压缩大小声明成 1 MiB */
    char *kpath = make_fixture("key-range-outside.mdx", &k, NULL);
    ok(kpath != NULL, "⑩ 合成键区压缩区间越界的件");
    if (kpath != NULL) {
      ok(open_rc(kpath, "⑩ 键区压缩区间越界") != 0, "⑩ 词块的压缩区间落到文件之外 → 必须报错");
      ok(last_error_has("之外"), "⑩ 报的应当是「落到文件之外」这一关");
      remove(kpath);
      free(kpath);
    }
    ok_no_leak("⑩ 两处区间越界的件");
  }

  /* ── ⑪ 声明一个天文数字的「词条总数」——`list_keys` 不许按它去分配 ──
   * 修之前它按 `key_count` 一次要 8 × key_count 字节（2^40 条就是 8 TiB，必然失败）；
   * 修之后按**真读出来的键**增长，所以这本词典照常枚举出那 4 条。 */
  {
    fixture_spec s = truth();
    s.key_count_declared = (int64_t)1 << 40;
    char *path = make_fixture("huge-key-count.mdx", &s, NULL);
    ok(path != NULL, "⑪ 合成词条总数被放大的件");
    if (path != NULL) {
      dsh_mdx *m = NULL;
      ok_eq_i64(dsh_mdx_open(path, &m), 0, "⑪ 声明总数被放大不影响开得起来");
      if (m != NULL) {
        ok_eq_i64(dsh_mdx_key_count(m), (int64_t)1 << 40,
                  "⑪ 访问器照实报文件里写的总数（钳制它是另一件事，这里不动契约）");
        char **keys = NULL;
        int64_t count = 0;
        ok_eq_i64(dsh_mdx_list_keys(m, &keys, &count), 0,
                  "⑪ 枚举必须成功（按声明值分配的话这里会是内存不足）");
        ok_eq_i64(count, 4, "⑪ 枚举出来的必须是**真实存在**的 4 条，不是声明的那 2^40 条");
        for (int64_t i = 0; i < count && i < 4; i++) {
          ok_eq_str(keys[i], FIX_KEYS[i], "⑪ 逐条比对键名");
        }
        dsh_mdx_free_keys(keys, count);
        dsh_mdx_close(m);
      }
      remove(path);
      free(path);
    }
    ok_no_leak("⑪ 词条总数被放大的件");
  }

  /* 全部还清：活分配表必须回到基线（能暴露解析器在新路上泄漏） */
  ok_eq_i64((int64_t)dsh_mem_live_count(), (int64_t)base,
            "解析器全部关闭之后活分配表必须回到基线");

  rmdir(g_root);

  printf("mdx_hardening：%d 项，失败 %d\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
