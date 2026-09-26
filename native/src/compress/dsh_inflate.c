/* DEFLATE 解压（RFC 1951），接口与调用约定见 dsh_inflate.h。
 * ⚠️ 越界与坏数据一律**报错返回**，不返回半截结果 —— 调用方靠这个把「这个词块坏了」
 * 与「整本词典打不开」分开处理，所以这里必须如实说「坏了」。 */

#include "compress/dsh_inflate.h"
#include "dsh_internal.h"

#include <string.h>

/* DEFLATE 的位流是低位在前（LSB-first），与阅读顺序相反，所以单独封一层。 */
typedef struct {
  const uint8_t *data;
  size_t size;
  size_t pos;   /* 下一个要取的字节 */
  uint32_t bitbuf;
  int bitcnt;   /* bitbuf 里还有多少有效位 */
  int error;    /* 读越界就置上，之后所有读取都返回 0 */
} dsh_bitreader;

static void br_init(dsh_bitreader *br, const uint8_t *data, size_t size) {
  br->data = data;
  br->size = size;
  br->pos = 0;
  br->bitbuf = 0;
  br->bitcnt = 0;
  br->error = 0;
}

/* 取 bits 位（≤ 24 位，够 Huffman 与长度/距离用） */
static uint32_t br_bits(dsh_bitreader *br, int bits) {
  while (br->bitcnt < bits) {
    if (br->pos >= br->size) {
      br->error = 1;
      return 0;
    }
    br->bitbuf |= (uint32_t)br->data[br->pos++] << br->bitcnt;
    br->bitcnt += 8;
  }
  const uint32_t value = br->bitbuf & ((1u << bits) - 1u);
  br->bitbuf >>= bits;
  br->bitcnt -= bits;
  return value;
}

static void br_align_byte(dsh_bitreader *br) {
  const int drop = br->bitcnt & 7;
  br->bitbuf >>= drop;
  br->bitcnt -= drop;
}

/* ── Huffman 表 ───────────────────────────────────────────────────────────
 * 用"计数 + 偏移"的规范 Huffman 解码（与 zlib 的 inflate_table 同一路子），
 * 不建二叉树：码长最多 15 位，按码长分段查表既省内存又不会递归。 */
#define DSH_MAXBITS 15

typedef struct {
  uint16_t counts[DSH_MAXBITS + 1]; /* 每种码长有几个码字 */
  uint16_t symbols[288];            /* 按码长排好的符号表 */
} dsh_huff;

static int huff_build(dsh_huff *h, const uint8_t *lengths, int n) {
  memset(h->counts, 0, sizeof(h->counts));
  for (int i = 0; i < n; i++) h->counts[lengths[i]]++;
  if (h->counts[0] == n) return 1; /* 全零：这张表不存在，合法（动态块里会出现） */

  /* 校验码长集合的完备性：Kraft 不等式必须恰好饱和，否则是坏数据 */
  int left = 1;
  for (int len = 1; len <= DSH_MAXBITS; len++) {
    left <<= 1;
    left -= h->counts[len];
    if (left < 0) return 0;
  }

  uint16_t offs[DSH_MAXBITS + 2];
  offs[1] = 0;
  for (int len = 1; len <= DSH_MAXBITS; len++) offs[len + 1] = (uint16_t)(offs[len] + h->counts[len]);
  for (int i = 0; i < n; i++) {
    if (lengths[i] != 0) h->symbols[offs[lengths[i]]++] = (uint16_t)i;
  }
  return 1;
}

/* 解一个符号 */
static int huff_decode(dsh_bitreader *br, const dsh_huff *h) {
  int code = 0;
  int first = 0;
  int index = 0;
  for (int len = 1; len <= DSH_MAXBITS; len++) {
    code |= (int)br_bits(br, 1);
    if (br->error) return -1;
    const int count = h->counts[len];
    if (code - first < count) return h->symbols[index + (code - first)];
    index += count;
    first = (first + count) << 1;
    code <<= 1;
  }
  return -1;
}

/* ── 长度 / 距离表（RFC 1951 第 3.2.5 节）───────────────────────────────────── */
static const uint16_t LEN_BASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t LEN_EXTRA[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                      2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t DIST_BASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                       193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                       6145, 8193, 12289, 16385, 24577};
static const uint8_t DIST_EXTRA[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                       6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
static const uint8_t CLEN_ORDER[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

/* ── 输出缓冲 ──────────────────────────────────────────────────────────── */
typedef struct {
  uint8_t *data;
  size_t len;
  size_t cap;
} dsh_outbuf;

#define DSH_INFLATE_MAX_OUT ((size_t)512 * 1024 * 1024) /* 单块上限，防坏数据把内存吃爆 */

static int ob_reserve(dsh_outbuf *ob, size_t need) {
  if (need <= ob->cap) return 1;
  if (need > DSH_INFLATE_MAX_OUT) {
    dsh_set_last_error("zlib 解压输出超过 512MB，数据可能已损坏");
    return 0;
  }
  size_t cap = ob->cap ? ob->cap : 1024;
  while (cap < need) {
    /* 倍增，但不超过上限 */
    if (cap > DSH_INFLATE_MAX_OUT / 2) { cap = DSH_INFLATE_MAX_OUT; break; }
    cap *= 2;
  }
  uint8_t *next = (uint8_t *)dsh_mem_alloc(cap);
  if (next == NULL) {
    dsh_set_last_error("内存不足：zlib 解压需要 %zu 字节", cap);
    return 0;
  }
  if (ob->len) memcpy(next, ob->data, ob->len);
  if (ob->data) dsh_release(ob->data);
  ob->data = next;
  ob->cap = cap;
  return 1;
}

static int ob_byte(dsh_outbuf *ob, uint8_t b) {
  if (!ob_reserve(ob, ob->len + 1)) return 0;
  ob->data[ob->len++] = b;
  return 1;
}

/** 从输出缓冲里回拷一段（距离引用）。注意**允许重叠**（src 与 dst 区间相交），
 *  所以必须逐字节向前拷 —— 用 memcpy 会在重叠时给出错的结果，
 *  而"距离 < 长度"的重复串（RLE 式压缩）正是 DEFLATE 最常见的用法。 */
static int ob_copy_back(dsh_outbuf *ob, size_t distance, size_t length) {
  if (distance == 0 || distance > ob->len) {
    dsh_set_last_error("zlib 数据损坏：距离引用超出已解出的范围（距离 %zu，已解 %zu 字节）",
                       distance, ob->len);
    return 0;
  }
  if (!ob_reserve(ob, ob->len + length)) return 0;
  size_t src = ob->len - distance;
  for (size_t i = 0; i < length; i++) ob->data[ob->len++] = ob->data[src++];
  return 1;
}

/* ── 三种块 ─────────────────────────────────────────────────────────────── */

static int inflate_stored(dsh_bitreader *br, dsh_outbuf *ob) {
  br_align_byte(br);
  if (br->error || br->pos + 4 > br->size) {
    dsh_set_last_error("zlib 数据损坏：stored 块的头不完整");
    return 0;
  }
  /* LEN / NLEN 是小端，且 NLEN 必须是 LEN 的按位取反 */
  const uint32_t len = (uint32_t)br->data[br->pos] | ((uint32_t)br->data[br->pos + 1] << 8);
  const uint32_t nlen = (uint32_t)br->data[br->pos + 2] | ((uint32_t)br->data[br->pos + 3] << 8);
  br->pos += 4;
  if ((len ^ 0xFFFFu) != nlen) {
    dsh_set_last_error("zlib 数据损坏：stored 块的 LEN/NLEN 不互补");
    return 0;
  }
  if (br->pos + len > br->size) {
    dsh_set_last_error("zlib 数据损坏：stored 块声明 %u 字节，但数据不足", len);
    return 0;
  }
  if (!ob_reserve(ob, ob->len + len)) return 0;
  memcpy(ob->data + ob->len, br->data + br->pos, len);
  ob->len += len;
  br->pos += len;
  return 1;
}

static void build_fixed_tables(dsh_huff *lit, dsh_huff *dist) {
  uint8_t lengths[288];
  for (int i = 0; i < 144; i++) lengths[i] = 8;
  for (int i = 144; i < 256; i++) lengths[i] = 9;
  for (int i = 256; i < 280; i++) lengths[i] = 7;
  for (int i = 280; i < 288; i++) lengths[i] = 8;
  huff_build(lit, lengths, 288);
  for (int i = 0; i < 30; i++) lengths[i] = 5;
  huff_build(dist, lengths, 30);
}

/* 解一段 Huffman 编码的数据（固定块与动态块共用） */
static int inflate_huffman(dsh_bitreader *br, dsh_outbuf *ob, const dsh_huff *lit, const dsh_huff *dist) {
  for (;;) {
    const int sym = huff_decode(br, lit);
    if (sym < 0) {
      dsh_set_last_error("zlib 数据损坏：Huffman 码字无法解析");
      return 0;
    }
    if (sym < 256) {
      if (!ob_byte(ob, (uint8_t)sym)) return 0;
      continue;
    }
    if (sym == 256) return 1; /* 块结束 */
    const int len_index = sym - 257;
    if (len_index >= 29) {
      dsh_set_last_error("zlib 数据损坏：长度符号 %d 越界", sym);
      return 0;
    }
    const size_t length = LEN_BASE[len_index] + br_bits(br, LEN_EXTRA[len_index]);
    const int dsym = huff_decode(br, dist);
    if (dsym < 0 || dsym >= 30) {
      dsh_set_last_error("zlib 数据损坏：距离符号无法解析");
      return 0;
    }
    const size_t distance = DIST_BASE[dsym] + br_bits(br, DIST_EXTRA[dsym]);
    if (br->error) {
      dsh_set_last_error("zlib 数据损坏：长度/距离的附加位读取越界");
      return 0;
    }
    if (!ob_copy_back(ob, distance, length)) return 0;
  }
}

static int inflate_dynamic(dsh_bitreader *br, dsh_outbuf *ob) {
  const int hlit = (int)br_bits(br, 5) + 257;
  const int hdist = (int)br_bits(br, 5) + 1;
  const int hclen = (int)br_bits(br, 4) + 4;
  if (br->error) {
    dsh_set_last_error("zlib 数据损坏：动态块的头不完整");
    return 0;
  }
  if (hlit > 286 || hdist > 30) {
    dsh_set_last_error("zlib 数据损坏：动态块声明了过多的码字（lit=%d dist=%d）", hlit, hdist);
    return 0;
  }

  uint8_t clen_lengths[19];
  memset(clen_lengths, 0, sizeof(clen_lengths));
  for (int i = 0; i < hclen; i++) clen_lengths[CLEN_ORDER[i]] = (uint8_t)br_bits(br, 3);
  dsh_huff clen_huff;
  if (!huff_build(&clen_huff, clen_lengths, 19)) {
    dsh_set_last_error("zlib 数据损坏：码长字母表的码长不合法");
    return 0;
  }

  uint8_t lengths[288 + 30];
  memset(lengths, 0, sizeof(lengths));
  int n = 0;
  while (n < hlit + hdist) {
    const int sym = huff_decode(br, &clen_huff);
    if (sym < 0) {
      dsh_set_last_error("zlib 数据损坏：码长序列无法解析");
      return 0;
    }
    if (sym < 16) {
      lengths[n++] = (uint8_t)sym;
    } else if (sym == 16) {
      if (n == 0) {
        dsh_set_last_error("zlib 数据损坏：码长序列以「重复上一个」开头");
        return 0;
      }
      const int repeat = 3 + (int)br_bits(br, 2);
      const uint8_t prev = lengths[n - 1];
      if (n + repeat > hlit + hdist) {
        dsh_set_last_error("zlib 数据损坏：码长重复次数越界");
        return 0;
      }
      for (int i = 0; i < repeat; i++) lengths[n++] = prev;
    } else if (sym == 17) {
      const int repeat = 3 + (int)br_bits(br, 3);
      if (n + repeat > hlit + hdist) {
        dsh_set_last_error("zlib 数据损坏：码长重复次数越界");
        return 0;
      }
      for (int i = 0; i < repeat; i++) lengths[n++] = 0;
    } else {
      const int repeat = 11 + (int)br_bits(br, 7);
      if (n + repeat > hlit + hdist) {
        dsh_set_last_error("zlib 数据损坏：码长重复次数越界");
        return 0;
      }
      for (int i = 0; i < repeat; i++) lengths[n++] = 0;
    }
    if (br->error) {
      dsh_set_last_error("zlib 数据损坏：码长序列读取越界");
      return 0;
    }
  }

  /* 字面/长度表里必须有一个结束符（码 256），否则这块永远解不完 */
  if (lengths[256] == 0) {
    dsh_set_last_error("zlib 数据损坏：字面/长度表里没有块结束符（码 256）");
    return 0;
  }

  dsh_huff lit, dist;
  if (!huff_build(&lit, lengths, hlit)) {
    dsh_set_last_error("zlib 数据损坏：字面/长度表的码长不合法");
    return 0;
  }
  if (!huff_build(&dist, lengths + hlit, hdist)) {
    dsh_set_last_error("zlib 数据损坏：距离表的码长不合法");
    return 0;
  }
  return inflate_huffman(br, ob, &lit, &dist);
}

/* ── 入口 ───────────────────────────────────────────────────────────────── */

int dsh_zlib_inflate(const uint8_t *input, size_t input_len, size_t expected_size,
                     uint8_t **out_bytes, size_t *out_len) {
  if (input == NULL || out_bytes == NULL || out_len == NULL) {
    dsh_set_last_error("参数不合法：dsh_zlib_inflate 收到了空指针");
    return -1;
  }
  *out_bytes = NULL;
  *out_len = 0;

  if (input_len < 2) {
    dsh_set_last_error("zlib 数据长度不足 2 字节（缺少 zlib 头）");
    return -1;
  }
  /* zlib 头：CMF / FLG。校验一下压缩方法，免得把别的格式当 zlib 解。 */
  const uint8_t cmf = input[0];
  const uint8_t flg = input[1];
  if ((cmf & 0x0F) != 8) {
    dsh_set_last_error("不是 zlib 流：CM 字段 %u（应为 8 = deflate）", cmf & 0x0F);
    return -1;
  }
  if (((cmf << 8) | flg) % 31 != 0) {
    dsh_set_last_error("zlib 头校验失败（CMF/FLG 之和不是 31 的倍数）");
    return -1;
  }

  dsh_bitreader br;
  br_init(&br, input + 2, input_len - 2);

  dsh_outbuf ob;
  ob.data = NULL;
  ob.len = 0;
  ob.cap = 0;
  if (expected_size > 0) {
    if (!ob_reserve(&ob, expected_size)) return -1;
  }

  dsh_huff fixed_lit, fixed_dist;
  build_fixed_tables(&fixed_lit, &fixed_dist);

  int ok = 0;
  int final = 0;
  while (!final) {
    final = (int)br_bits(&br, 1);
    const int type = (int)br_bits(&br, 2);
    if (br.error) {
      dsh_set_last_error("zlib 数据损坏：块头读取越界");
      break;
    }
    if (type == 0) {
      ok = inflate_stored(&br, &ob);
    } else if (type == 1) {
      ok = inflate_huffman(&br, &ob, &fixed_lit, &fixed_dist);
    } else if (type == 2) {
      ok = inflate_dynamic(&br, &ob);
    } else {
      dsh_set_last_error("zlib 数据损坏：块类型 3 是保留值");
      ok = 0;
    }
    if (!ok) break;
    if (ob.len == 0 && final) break; /* 空流：合法 */
  }

  if (!ok) {
    if (ob.data) dsh_release(ob.data);
    return -1;
  }

  if (expected_size > 0 && ob.len != expected_size) {
    /* 与 C# 版不同：那边 DeflateStream 读完就算完，不比对长度。
     * 这里**只记警告不改结论** —— 长度不符说明文件有问题，但解出来的字节仍然可用，
     * 把它当硬失败会让一本"只是长度字段写错"的词典整个打不开。 */
    dsh_set_last_error("zlib 解出 %zu 字节，与头里记的 %zu 字节不一致（仍按解出的内容使用）",
                       ob.len, expected_size);
  }

  if (ob.data == NULL) {
    /* 解出 0 字节：仍要给调用方一块可释放的内存（接口定义里出参一律可 dsh_release） */
    ob.data = (uint8_t *)dsh_mem_alloc(1);
    if (ob.data == NULL) {
      dsh_set_last_error("内存不足：zlib 空结果");
      return -1;
    }
  }
  *out_bytes = ob.data;
  *out_len = ob.len;
  return 0;
}
