/* ==========================================================================
 * LZO1X 解压 —— 逐行移植参考实现（约定与调用方式见 dsh_lzo1x.h）。
 *
 * 迁移的原则是「一行对着一行搬」，包括那些看起来多余的中间变量与分支顺序：这份解码器
 * 对**位**敏感（一个字节读错后面全错），而它已经被真实词典验过 —— 重排、化简、合并分支
 * 都只会引入无法解释的差异。因此与参考实现的差异**全部是 C 语言逼出来的**：
 *   ① 4 字节对齐的批量拷贝没有搬（触发条件是源与目标都对齐且余数相同，此时两者不可能
 *      重叠，逐字节拷贝结果相同）；
 *   ② 越界读在 C 里是未定义行为，统一收敛到 buf_at / out_at 的「越界当 0」；
 *   ③ 参考实现在截断输入上不终止（长度逃逸串），这里多一条「越界即报损坏」的出口。
 *
 * 内存：工作缓冲与最终结果都走 dsh_mem_alloc，结果交给调用方 dsh_release，
 * 中间的临时缓冲自己还。
 * ========================================================================== */

#include "compress/dsh_lzo1x.h"

#include "dsh_internal.h"

#include <string.h>

/* 扩容步长与参考实现一致。它同时决定了初始容量的算法：输入长度补齐到 4096 的整数倍，
 * 刚好整除时再多给一块。 */
#define LZO_BLOCK_SIZE 4096

/* 输出上限，与参考实现同一个数（64MB）。超过就按损坏数据报错，而不是继续吃内存。 */
#define LZO_MAX_OUTPUT ((ptrdiff_t)(64 * 1024 * 1024))

/* 参考实现的两个返回码照搬（EOF_FOUND 的 -999 是个哨兵值，没有别的含义） */
#define LZO_OK 0
#define LZO_EOF_FOUND (-999)

/* 新增：数据损坏 / 超限 / 截断。参考实现在这些地方是抛异常，C 里只能返回码。 */
#define LZO_BROKEN (-1)

/* ── 解码状态 ─────────────────────────────────────────────────────────────
 * · ip / op / m_pos 在原版里是 int，这里用 ptrdiff_t —— 同样是「按平台的字长」，
 *   但不至于在 32 位平台上被一个大缓冲挤爆（m_pos 允许为负，所以必须带符号）；
 * · t 必须是**定宽 64 位**：损坏数据里它会被长度逃逸串一路累加（每个 0 字节 +255），
 *   int 会溢出成负数（未定义行为），而 long 在 Windows（LLP64）上也只有 32 位。 */
typedef struct {
  const uint8_t *buf;   /* 压缩流 */
  ptrdiff_t buf_len;    /* 流的长度 */
  uint8_t *out;         /* 输出缓冲（容量 capacity，写过的部分到 op 为止） */
  ptrdiff_t capacity;   /* 输出缓冲的容量 */
  ptrdiff_t ip;         /* 读游标 */
  ptrdiff_t op;         /* 写游标 */
  ptrdiff_t m_pos;      /* 匹配源的位置（可以为负 —— 损坏数据） */
  int64_t t;            /* 当前令牌（⚠️ 定宽 64 位 —— long 在 Windows 上是 32 位，逃逸串会溢出） */
} lzo_state;

/* ── 取值：越界一律当 0 ───────────────────────────────────────────────────
 * 原版在数组尾巴之后/负下标拿到的是 undefined，参与算术会变成 NaN、写进 Uint8Array 又是 0；
 * 两边都不想让「越界」变成崩溃，所以这里把同一条语义显式写成取值函数。
 * 只在损坏数据上会走到；合法数据里每一次取值都在范围内，开销可以忽略。 */
static int buf_at(const lzo_state *s, ptrdiff_t index) {
  return (index >= 0 && index < s->buf_len) ? (int)s->buf[index] : 0;
}

/* 输出缓冲的越界读同理。⚠️ 这一条在参考实现里**没有**对应物（它那边越界会抛异常），
 * 但 C 里读越界是未定义行为、又没法抛异常，所以只有这一个选择。 */
static int out_at(const lzo_state *s, ptrdiff_t index) {
  return (index >= 0 && index < s->capacity) ? (int)s->out[index] : 0;
}

/* ── 扩容 ─────────────────────────────────────────────────────────────────
 * 参考实现是一块（4096）一块地长，长到 64MB 要拷 16000 次 —— 那是 O(n²)，而容量只影响
 * 性能、不影响输出（输出长度是 op）。所以这里先按同样的步长把目标容量算出来，再一次分配
 * 到位：容量序列与参考实现逐项相同，只是少拷了 16000 次。
 *
 * ⚠️ 新缓冲里 ip 之后的部分**必须清零**：原版是新数组（本来就零初始化），而 C 的
 * dsh_mem_alloc 给的是脏内存 —— 不清零的话，损坏数据里那些「读到还没写过的区域」就会
 * 读到不确定的字节，输出没法复现。 */
static int lzo_extend(lzo_state *s, ptrdiff_t needed) {
  ptrdiff_t want = s->capacity;
  while (want < needed) want += LZO_BLOCK_SIZE;

  uint8_t *next = (uint8_t *)dsh_mem_alloc((size_t)want);
  if (next == NULL) {
    dsh_set_last_error("内存不足：LZO 解压缓冲扩容到 %ld 字节失败", (long)want);
    return 0;
  }
  if (s->op > 0) memcpy(next, s->out, (size_t)s->op);
  memset(next + s->op, 0, (size_t)(want - s->op));
  dsh_release(s->out);

  s->out = next;
  s->capacity = want;
  return 1;
}

/* 保证还能再写出 needed 字节。needed 是「写完之后的总长度」。 */
static int lzo_ensure(lzo_state *s, ptrdiff_t needed) {
  if (needed <= s->capacity) return 1;
  if (needed > LZO_MAX_OUTPUT) {
    dsh_set_last_error("LZO 解压输出超过 64MB，输入数据可能已损坏");
    return 0;
  }
  return lzo_extend(s, needed);
}

/* 保证还能再写出 extra 字节（extra 只可能非负）。溢出这道闸是给损坏数据准备的：
 * op + t 在 32 位平台上会绕回来 —— 先挡住它，报出来的原因才是「损坏」而不是「算错了」。 */
static int lzo_ensure_extra(lzo_state *s, int64_t extra) {
  if (extra < 0 || extra > (int64_t)(LZO_MAX_OUTPUT - s->op)) {
    dsh_set_last_error("LZO 解压输出超过 64MB，输入数据可能已损坏");
    return 0;
  }
  return lzo_ensure(s, s->op + (ptrdiff_t)extra);
}

/* ── 长度逃逸串 ───────────────────────────────────────────────────────────
 * 越界之后读到的永远是 0，于是参考实现在这个 while 里死循环（实测：单字节 0x00 跑满
 * 10 秒没返回，被超时杀掉）。C 里没有异常能救出来，所以读越界那一步直接报损坏：
 * 合法数据走不到它 —— 逃逸串后面必定有一个非 0 字节，那才是它的结尾。 */
static int lzo_escape_length(lzo_state *s, int64_t base) {
  for (;;) {
    if (s->ip >= s->buf_len) {
      dsh_set_last_error("LZO 数据损坏：长度逃逸串越过了输入的末尾（输入被截断？）");
      return 0;
    }
    if (buf_at(s, s->ip) != 0) break;
    s->t += 255;
    s->ip++;
  }
  s->t += base + buf_at(s, s->ip);
  s->ip++;
  return 1;
}

/* ── 四个基本动作（copy_from_buf / copy_match / match_next / match_done）──
 * 顺序与命名都照抄参考实现，免得回头对照时错位。 */

/* 从压缩流里整段搬到输出（字面量）。 */
static int lzo_copy_from_buf(lzo_state *s) {
  if (!lzo_ensure_extra(s, s->t)) return 0;
  do {
    s->out[s->op++] = (uint8_t)buf_at(s, s->ip++);
  } while (--s->t > 0);
  return 1;
}

/* 从**已经解出来的输出**里整段搬（这就是 LZO 的「匹配」）。源与目标可以重叠 ——
 * 逐字节向前拷正是重复展开的写法，所以不能换成 memmove/memcpy。 */
static int lzo_copy_match(lzo_state *s) {
  s->t += 2;
  if (!lzo_ensure_extra(s, s->t)) return 0;
  do {
    s->out[s->op++] = (uint8_t)out_at(s, s->m_pos++);
  } while (--s->t > 0);
  return 1;
}

/* 一个匹配后面跟着 1~3 个字节的字面量，然后读下一个令牌。
 * ⚠️ 这里的 Ensure 写的永远是 3 —— 那是参考实现的预留量，与 t 的实际取值无关。 */
static int lzo_match_next(lzo_state *s) {
  if (!lzo_ensure_extra(s, 3)) return 0;
  s->out[s->op++] = (uint8_t)buf_at(s, s->ip++);
  if (s->t > 1) {
    s->out[s->op++] = (uint8_t)buf_at(s, s->ip++);
    if (s->t > 2) s->out[s->op++] = (uint8_t)buf_at(s, s->ip++);
  }
  s->t = buf_at(s, s->ip++);
  return 1;
}

/* 匹配长度用完没有，看的是**上一个令牌字节的低 2 位**。
 * ip - 2 可能是负数（流一开头就走这条路）—— buf_at 把它当 0，与参考实现同义。 */
static int64_t lzo_match_done(lzo_state *s) {
  s->t = buf_at(s, s->ip - 2) & 3;
  return s->t;
}

/* ── 匹配循环 ─────────────────────────────────────────────────────────────
 * 返回 LZO_OK / LZO_EOF_FOUND / LZO_BROKEN。EOF_FOUND **不是错误**：LZO 流的结尾就是
 * 这个标记（m_pos == op 的那个分支），调用方拿到它就该收工，把已写出的长度当作结果。 */
static int lzo_match(lzo_state *s) {
  for (;;) {
    if (s->t >= 64) {
      /* 短匹配：长度和距离都塞在这一个字节里 */
      s->m_pos = s->op - 1;
      s->m_pos -= (s->t >> 2) & 7;
      s->m_pos -= (ptrdiff_t)buf_at(s, s->ip++) << 3;
      s->t = (s->t >> 5) - 1;
      if (!lzo_copy_match(s)) return LZO_BROKEN;
      if (lzo_match_done(s) == 0) break;
      if (!lzo_match_next(s)) return LZO_BROKEN;
      continue;
    }

    if (s->t >= 32) {
      /* 中匹配：距离占 2 字节（低位在前） */
      s->t &= 31;
      if (s->t == 0) {
        if (!lzo_escape_length(s, 31)) return LZO_BROKEN;
      }
      s->m_pos = s->op - 1;
      s->m_pos -= (ptrdiff_t)((buf_at(s, s->ip) >> 2) + (buf_at(s, s->ip + 1) << 6));
      s->ip += 2;
    } else if (s->t >= 16) {
      /* 长匹配 / 流结束标记 */
      s->m_pos = s->op;
      s->m_pos -= (ptrdiff_t)((s->t & 8) << 11);
      s->t &= 7;
      if (s->t == 0) {
        if (!lzo_escape_length(s, 7)) return LZO_BROKEN;
      }
      s->m_pos -= (ptrdiff_t)((buf_at(s, s->ip) >> 2) + (buf_at(s, s->ip + 1) << 6));
      s->ip += 2;
      if (s->m_pos == s->op) return LZO_EOF_FOUND; /* 流结束：距离为 0 的那一档 */
      s->m_pos -= 0x4000; /* 长匹配的距离基准比中匹配远 0x4000 */
    } else {
      /* 最短的匹配：只写 2 个字节 */
      s->m_pos = s->op - 1;
      s->m_pos -= s->t >> 2;
      s->m_pos -= (ptrdiff_t)buf_at(s, s->ip++) << 2;
      if (!lzo_ensure_extra(s, 2)) return LZO_BROKEN;
      s->out[s->op++] = (uint8_t)out_at(s, s->m_pos++);
      s->out[s->op++] = (uint8_t)out_at(s, s->m_pos);
      if (lzo_match_done(s) == 0) break;
      if (!lzo_match_next(s)) return LZO_BROKEN;
      continue;
    }

    if (!lzo_copy_match(s)) return LZO_BROKEN;
    if (lzo_match_done(s) == 0) break;
    if (!lzo_match_next(s)) return LZO_BROKEN;
  }
  return LZO_OK;
}

/* ── 主循环 ───────────────────────────────────────────────────────────────
 * 逐行对应参考实现，连 skip_to_first_literal 这个「跳过第一段字面量」的开关都照搬：
 * 它是为了少写一次循环体而设的状态，去掉它就得重排整个循环。 */
static int lzo_run(lzo_state *s) {
  s->t = 0;
  s->ip = 0;
  s->op = 0;
  s->m_pos = 0;

  int skip_to_first_literal = 0;
  if (buf_at(s, s->ip) > 17) {
    s->t = buf_at(s, s->ip++) - 17;
    if (s->t < 4) {
      if (!lzo_match_next(s)) return LZO_BROKEN;
      int r = lzo_match(s);
      if (r != LZO_OK) return r;
    } else {
      if (!lzo_copy_from_buf(s)) return LZO_BROKEN;
      skip_to_first_literal = 1;
    }
  }

  for (;;) {
    if (!skip_to_first_literal) {
      s->t = buf_at(s, s->ip++);
      if (s->t >= 16) {
        int r = lzo_match(s);
        if (r != LZO_OK) return r; /* 包含 EOF_FOUND：正常收工 */
        continue;
      }
      if (s->t == 0) {
        if (!lzo_escape_length(s, 15)) return LZO_BROKEN;
      }
      s->t += 3;
      if (!lzo_copy_from_buf(s)) return LZO_BROKEN;
    } else {
      skip_to_first_literal = 0;
    }

    s->t = buf_at(s, s->ip++);
    if (s->t < 16) {
      /* 这一档是「距离 1 + 0x0800 再减」的老式短匹配，固定写 3 个字节 */
      s->m_pos = s->op - (1 + 0x0800);
      s->m_pos -= s->t >> 2;
      s->m_pos -= (ptrdiff_t)buf_at(s, s->ip++) << 2;
      if (!lzo_ensure_extra(s, 3)) return LZO_BROKEN;
      s->out[s->op++] = (uint8_t)out_at(s, s->m_pos++);
      s->out[s->op++] = (uint8_t)out_at(s, s->m_pos++);
      s->out[s->op++] = (uint8_t)out_at(s, s->m_pos);
      if (lzo_match_done(s) == 0) continue;
      if (!lzo_match_next(s)) return LZO_BROKEN;
    }

    int r = lzo_match(s);
    if (r != LZO_OK) return r;
  }
}

/* ── 对外接口 ───────────────────────────────────────────────────────────── */
int dsh_lzo1x_decompress(const uint8_t *input, size_t input_len,
                         uint8_t **out_bytes, size_t *out_len) {
  if (out_bytes == NULL || out_len == NULL) {
    dsh_set_last_error("dsh_lzo1x_decompress 的出参指针不能为空");
    return DSH_LZO1X_E_INVALID_ARG;
  }
  *out_bytes = NULL;
  *out_len = 0;

  if (input == NULL && input_len != 0) {
    dsh_set_last_error("dsh_lzo1x_decompress 收到空指针，但长度不为 0");
    return DSH_LZO1X_E_INVALID_ARG;
  }
  /* 压缩流本身比输出上限还大 —— 那只能是损坏数据。参考实现不会主动拦（它会先照输入
   * 长度分配、直到第一次扩容才报错）；这里提前拦，省一次大分配。 */
  if (input_len > (size_t)LZO_MAX_OUTPUT) {
    dsh_set_last_error("LZO 输入长度超过 64MB，输入数据可能已损坏");
    return DSH_LZO1X_E_CORRUPT;
  }

  /* 初始容量与参考实现同一个算法：补齐到 4096 的整数倍（刚好整除时再多给一块）。
   * 输入为 0 时得到 4096 —— 于是空输入这条路只会走到「要扩容时才发现超限」。 */
  ptrdiff_t capacity = (ptrdiff_t)input_len + (LZO_BLOCK_SIZE - (ptrdiff_t)(input_len % (size_t)LZO_BLOCK_SIZE));

  lzo_state st;
  st.buf = input;
  st.buf_len = (ptrdiff_t)input_len;
  st.capacity = capacity;
  st.ip = 0;
  st.op = 0;
  st.m_pos = 0;
  st.t = 0;

  /* 工作缓冲：dsh_mem_alloc 给的内存是脏的，必须先清零（理由见 lzo_extend 上面那段） */
  st.out = (uint8_t *)dsh_mem_alloc((size_t)capacity);
  if (st.out == NULL) {
    dsh_set_last_error("内存不足：LZO 解压缓冲分配 %ld 字节失败", (long)capacity);
    return DSH_LZO1X_E_OOM;
  }
  memset(st.out, 0, (size_t)capacity);

  const int rc = lzo_run(&st);
  if (rc == LZO_BROKEN) {
    /* 具体原因已经由出错的那一步写进 last_error */
    dsh_release(st.out);
    return DSH_LZO1X_E_CORRUPT;
  }

  /* 结果按实际长度另给一块**恰好 out_len 字节**的内存（与 C# 版的 Finish 一致）：
   * 调用方不必知道容量这回事，也不必担心 out_len 之外还露着别的东西。 */
  uint8_t *result = (uint8_t *)dsh_mem_alloc((size_t)st.op);
  if (result == NULL) {
    dsh_release(st.out);
    dsh_set_last_error("内存不足：LZO 解压结果（%ld 字节）分配失败", (long)st.op);
    return DSH_LZO1X_E_OOM;
  }
  if (st.op > 0) memcpy(result, st.out, (size_t)st.op);
  dsh_release(st.out);

  *out_bytes = result;
  *out_len = (size_t)st.op;
  return DSH_LZO1X_OK;
}
