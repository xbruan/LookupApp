/* Ogg Speex（`.spx`）→ 16bit PCM WAV —— 容器与打包那一半（约定见 dsh_speex.h）。
 *
 * 四件事：Ogg 页 → 包的重组、Speex 头 80 字节的字段布局、逐帧解码循环、WAV 封装。
 * 解码本体是官方 libspeex（`vendor/speex/`）。
 *
 * ⚠️ 四条**不要「顺手改进」**的约定：
 *   ① **播放速率取头里写的 `rate`**，不是模式速率。LDOCE5 的真文件头里写 22050 而
 *      码流按 `frame_size=320` 看像个 16 kHz 流 —— 这里**栽过两次**，两次都是用户用耳朵
 *      纠回来的（「男声成了男超低音」）。官方 `speexdec` 就是 `*rate = header->rate`，
 *      PCM 不做重采样。
 *   ② **附加头直接跳过**（音频包头一个 = `2 + extra_headers`），不喂给解码器。
 *   ③ 流末尾没有以「短 segment」收尾的残余包**丢掉**；一帧都没有时按「解不了」如实报错，
 *      **不是不报错地产出一个空 WAV**。
 *   ④ **错误文案逐句照抄** —— 它们是直接显示给用户的那句话。
 *
 * 一次遍历、不存包：页里每凑满一个包就就地处理，只留一个可复用的包缓冲（省掉
 * 「点一次 🔊 就分配几百个小数组」）。顺序与检查标准不变。
 */

#include "audio/dsh_speex.h"

#include "dsh_internal.h"

#include "speex/speex.h"
#include "speex/speex_bits.h"
#include "speex/speex_stereo.h"

#include <stdio.h>
#include <string.h>

/* ── 小工具 ────────────────────────────────────────────────────────────── */

/** 在 `[from, min(len, limit))` 里找一段 ASCII（参考实现的 `IndexOfAscii`）*/
static int index_of_ascii(const uint8_t *bytes, size_t len, size_t limit, const char *needle,
                          size_t from) {
  if (bytes == NULL || needle == NULL) return -1;
  const size_t n = strlen(needle);
  if (n == 0) return -1;
  const size_t cap = (len < limit) ? len : limit;
  if (cap < n || from > cap - n) return -1;
  for (size_t i = from; i + n <= cap; i++) {
    if (memcmp(bytes + i, needle, n) == 0) return (int)i;
  }
  return -1;
}

int dsh_speex_looks_like(const uint8_t *bytes, size_t len) {
  /* 检查标准：`OggS` 捕获模式 + 前 256 字节里有 `"Speex   "`（**三个空格** —— 那是
   * Speex 头里 `speex_string` 的定长写法）。不解析完整页结构：够用，而且不会被误伤。
   *
   * ⚠️ 这里**不加长度条件**（别照参考实现那个多要求 `Length >= 32` 的 LooksLikeSpeex 抄 ——
   *    它在参考实现里**一个调用点都没有**，是死代码；照它抄会让「20 字节的假 Ogg Speex」
   *    从「认出来但解不了」变成「根本不认识」）。长度够不够由调用方管。
   */
  if (bytes == NULL || len == 0) return 0;
  if (!(bytes[0] == 0x4F && bytes[1] == 0x67 && bytes[2] == 0x67 && bytes[3] == 0x53)) return 0;
  return index_of_ascii(bytes, len, 256, "Speex   ", 0) >= 0;
}

int dsh_speex_mode_rate(int mode) {
  switch (mode) {
    case 1: return 16000; /* 宽带 */
    case 2: return 32000; /* 超宽带 */
    default: return 8000; /* 窄带 */
  }
}

const char *dsh_speex_mode_name(int mode) {
  switch (mode) {
    case 1: return "宽带";
    case 2: return "超宽带";
    default: return "窄带";
  }
}

/* ── 只会长的字节缓冲（WAV 头那 44 字节先留出来）────────────────────────── */

typedef struct {
  uint8_t *p;
  size_t len;  /* 已用（含开头预留的头）*/
  size_t cap;
} out_buf;

static int ob_reserve(out_buf *b, size_t total) {
  if (total <= b->cap) return 0;
  size_t next = (b->cap > 0) ? b->cap : 65536;
  while (next < total) next *= 2;
  uint8_t *grown = (uint8_t *)dsh_mem_alloc(next);
  if (grown == NULL) {
    dsh_set_last_error("内存不足：Speex 解出来的 PCM（%zu 字节）", total);
    return -1;
  }
  if (b->len > 0) memcpy(grown, b->p, b->len);
  if (b->p != NULL) dsh_release(b->p);
  b->p = grown;
  b->cap = next;
  return 0;
}

/** 追加一段。PCM 总量卡在 `DSH_SPEEX_MAX_PCM_BYTES` 上（畸形码流不该吃光内存）*/
static int ob_add(out_buf *b, const void *data, size_t n) {
  if (b->len + n > DSH_SPEEX_MAX_PCM_BYTES) {
    dsh_set_last_error("这段 Speex 太长（解出来的 PCM 超过 %d MB），本版不处理",
                       (int)(DSH_SPEEX_MAX_PCM_BYTES / (1024 * 1024)));
    return -1;
  }
  if (ob_reserve(b, b->len + n) != 0) return -1;
  memcpy(b->p + b->len, data, n);
  b->len += n;
  return 0;
}

static void ob_free(out_buf *b) {
  if (b->p != NULL) dsh_release(b->p);
  b->p = NULL;
  b->len = b->cap = 0;
}

/* ── WAV 头 ────────────────────────────────────────────────────────────── */

static void put_u32le(uint8_t *b, size_t at, uint32_t v) {
  b[at] = (uint8_t)(v & 0xFFu);
  b[at + 1] = (uint8_t)((v >> 8) & 0xFFu);
  b[at + 2] = (uint8_t)((v >> 16) & 0xFFu);
  b[at + 3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void put_u16le(uint8_t *b, size_t at, uint16_t v) {
  b[at] = (uint8_t)(v & 0xFFu);
  b[at + 1] = (uint8_t)((v >> 8) & 0xFFu);
}

/** 44 字节的极简 WAV 头（16bit PCM，格式写死 —— 播放器只认这个）*/
static void wav_header(uint8_t *h, int rate, int channels, uint32_t data_bytes) {
  const uint32_t byte_rate = (uint32_t)rate * (uint32_t)channels * 2u;
  memcpy(h, "RIFF", 4);
  put_u32le(h, 4, 36u + data_bytes);
  memcpy(h + 8, "WAVE", 4);
  memcpy(h + 12, "fmt ", 4);
  put_u32le(h, 16, 16u);                      /* fmt 块长度 */
  put_u16le(h, 20, 1u);                       /* PCM */
  put_u16le(h, 22, (uint16_t)channels);
  put_u32le(h, 24, (uint32_t)rate);
  put_u32le(h, 28, byte_rate);
  put_u16le(h, 32, (uint16_t)(channels * 2)); /* 块对齐 */
  put_u16le(h, 34, 16u);                      /* 位深 */
  memcpy(h + 36, "data", 4);
  put_u32le(h, 40, data_bytes);
}

/* ── Speex 头 ──────────────────────────────────────────────────────────── */

static uint32_t rd_u32le(const uint8_t *b, size_t at) {
  return (uint32_t)b[at] | ((uint32_t)b[at + 1] << 8) | ((uint32_t)b[at + 2] << 16) |
         ((uint32_t)b[at + 3] << 24);
}

/**
 * 第一个包是 Speex 头（**80 字节，字段全是小端 32 位**），第二个包是注释头，
 * 之后可能还有 `extra_headers` 个附加头 —— 音频从它们后面才开始。
 * 用到的偏移：36 采样率 ｜ 40 模式 ｜ 48 声道数 ｜ 56 帧长 ｜ 68 附加头数。
 */
static void parse_header(const uint8_t *head, size_t len, dsh_speex_info *info, char *reason,
                         size_t reason_cap, int *out_rc) {
  memset(info, 0, sizeof(*info));
  if (len < 80) {
    snprintf(reason, reason_cap, "Speex 头不完整（不足 80 字节）。");
    *out_rc = DSH_SPEEX_ENOPE;
    return;
  }
  const int mode = (int)rd_u32le(head, 40);
  const int header_rate = (int)rd_u32le(head, 36);
  const int mode_rate = dsh_speex_mode_rate(mode);
  const int header_frame_size = (int)rd_u32le(head, 56);
  info->mode = mode;
  info->mode_rate = mode_rate;
  info->header_rate = header_rate;
  info->header_frame_size = header_frame_size;
  info->channels = (int)rd_u32le(head, 48);
  info->extra_headers = (int)rd_u32le(head, 68);
  /* ★ **播放速率取头里写的 `rate`**（只在它明显不合理时才退回模式速率）。
   * ⚠️ 这一条被推翻过两次，两次都是用户用耳朵纠回来的（「男声成了男超低音」）——
   *    「头里那个 22050 与 `frame_size=320` 对不上，所以该取模式速率」这条推理的
   *    **前提就是错的**：官方 `speexdec` 就是 `*rate = header->rate`，PCM 不做重采样。 */
  info->rate = (header_rate >= 4000 && header_rate <= 192000) ? header_rate : mode_rate;
  {
    size_t k = 0;
    for (size_t i = 8; i < 28 && head[i] != 0 && k + 1 < sizeof(info->version); i++) {
      info->version[k++] = (char)head[i];
    }
    info->version[k] = '\0';
  }
  if (info->channels != 1 && info->channels != 2) {
    snprintf(reason, reason_cap, "这段 Speex 报了 %d 个声道，本版只支持 1~2 声道。",
             info->channels);
    *out_rc = DSH_SPEEX_ENOPE;
    return;
  }
  if (mode < 0 || mode > 2) {
    snprintf(reason, reason_cap, "这段 Speex 的模式是 %d（0 窄带 / 1 宽带 / 2 超宽带），本版不认识。",
             mode);
    *out_rc = DSH_SPEEX_ENOPE;
    return;
  }
  *out_rc = DSH_SPEEX_OK;
}

/* ── 一次遍历：页 → 包 → 帧 ────────────────────────────────────────────── */

typedef struct {
  /* 攒包 */
  uint8_t *pkt;
  size_t pkt_len;
  size_t pkt_cap;
  /** 正在攒的是第几个包（0 起）—— 完成那一刻用它判「这是头 / 注释头 / 音频」 */
  int64_t pkt_index;
  int64_t packets;
  int64_t first_audio;
  /* 头 */
  dsh_speex_info info;
  int have_info;
  int header_rc;
  char header_reason[512];
  /* 解码器（只有真要 PCM 时才建）*/
  void *dec;
  SpeexBits bits;
  int bits_ok;
  SpeexStereoState *stereo;
  int frame_size;
  spx_int16_t *frame;
  /* 出参 */
  out_buf out;
  int64_t frames;
  int oom; /* 内存不足：与「这段码流解不了」是**两件事**（返回码不同）*/
} spx_state;

static void spx_free(spx_state *st) {
  if (st->dec != NULL) speex_decoder_destroy(st->dec);
  if (st->bits_ok) speex_bits_destroy(&st->bits);
  if (st->stereo != NULL) speex_stereo_state_destroy(st->stereo);
  if (st->frame != NULL) dsh_release(st->frame);
  if (st->pkt != NULL) dsh_release(st->pkt);
  ob_free(&st->out);
}

/** 按头里的模式建解码器、开感知增强、问出每帧采样数 */
static int setup_decoder(spx_state *st, char *reason, size_t reason_cap) {
  const SpeexMode *mode = speex_lib_get_mode(st->info.mode);
  if (mode == NULL) {
    snprintf(reason, reason_cap, "这段 Speex 的模式是 %d，本版不认识。", st->info.mode);
    return DSH_SPEEX_ENOPE;
  }
  st->dec = speex_decoder_init(mode);
  if (st->dec == NULL) {
    snprintf(reason, reason_cap, "这段 Speex 解不了：解码器起不来（模式 %d）。", st->info.mode);
    return DSH_SPEEX_ENOPE;
  }
  /* 感知增强（梳状后置滤波）**开着** —— 与 libspeex 系播放器的默认一致；
   * 关掉它波形会明显变「闷」。 */
  {
    int enh = 1;
    (void)speex_decoder_ctl(st->dec, SPEEX_SET_ENH, &enh);
  }
  {
    int fs = 0;
    if (speex_decoder_ctl(st->dec, SPEEX_GET_FRAME_SIZE, &fs) != 0 || fs <= 0) {
      snprintf(reason, reason_cap, "这段 Speex 解不了：问不出每帧采样数（模式 %d）。",
               st->info.mode);
      return DSH_SPEEX_ENOPE;
    }
    st->frame_size = fs;
  }
  /* 立体声时 `speex_decode_stereo_int` 会把一帧就地展成 2×frame_size 个交错样本 */
  st->frame = (spx_int16_t *)dsh_mem_alloc((size_t)st->frame_size * (size_t)st->info.channels *
                                           sizeof(spx_int16_t));
  if (st->frame == NULL) {
    dsh_set_last_error("内存不足：Speex 一帧的缓冲（%d 采样）", st->frame_size);
    st->oom = 1;
    return DSH_SPEEX_EOMEM;
  }
  speex_bits_init(&st->bits);
  st->bits_ok = 1;
  if (st->info.channels == 2) {
    st->stereo = speex_stereo_state_init();
    if (st->stereo == NULL) {
      dsh_set_last_error("内存不足：Speex 立体声状态");
      st->oom = 1;
      return DSH_SPEEX_EOMEM;
    }
  }
  /* WAV 头先留出来（后面往它后面追加 PCM，跑完再回填）*/
  if (ob_reserve(&st->out, 44) != 0) {
    st->oom = 1;
    return DSH_SPEEX_EOMEM;
  }
  st->out.len = 44;
  return DSH_SPEEX_OK;
}

/** 一个包凑齐了：头包解析字段，音频包把**包里所有帧**解出来 */
static int on_packet(spx_state *st, int want_pcm, char *reason, size_t reason_cap) {
  if (st->pkt_index == 0) {
    parse_header(st->pkt, st->pkt_len, &st->info, st->header_reason,
                 sizeof(st->header_reason), &st->header_rc);
    st->have_info = (st->header_rc == DSH_SPEEX_OK);
    if (st->have_info) {
      st->first_audio = 2 + ((st->info.extra_headers > 0) ? st->info.extra_headers : 0);
      if (want_pcm) {
        const int rc = setup_decoder(st, reason, reason_cap);
        if (rc != DSH_SPEEX_OK) return rc;
      }
    }
  } else if (want_pcm && st->dec != NULL && st->pkt_index >= st->first_audio &&
             st->pkt_len > 0) {
    speex_bits_read_from(&st->bits, (const char *)st->pkt, (int)st->pkt_len);
    for (;;) {
      /* 0 = 这一帧解好了；1 = 包里没有更多帧了；-1 = 坏帧 —— **只在 0 上继续**。 */
      const int r = speex_decode_int(st->dec, &st->bits, st->frame);
      if (r != 0) break;
      if (st->info.channels == 2) {
        speex_decode_stereo_int(st->frame, st->frame_size, st->stereo);
      }
      const size_t bytes = (size_t)st->frame_size * (size_t)st->info.channels *
                           sizeof(spx_int16_t);
      if (ob_add(&st->out, st->frame, bytes) != 0) {
        st->oom = 1;
        return DSH_SPEEX_EOMEM;
      }
      st->frames++;
    }
  }
  st->pkt_index++;
  return DSH_SPEEX_OK;
}

/**
 * 按 Ogg 规范把页重组成**包**，边重组边处理。
 *
 * 页头固定 27 字节，后面跟 segment 表（表长由页头最后一个字节给），然后是各 segment 的
 * 数据；**长度正好 255 的 segment 表示包还没结束**，要接着下一个 segment（甚至下一页）
 * 拼 —— 音频帧常常跨页。
 */
static int walk_pages(const uint8_t *bytes, size_t len, spx_state *st, int want_pcm, char *reason,
                      size_t reason_cap) {
  size_t pos = 0;
  while (pos + 27 <= len) {
    if (!(bytes[pos] == 0x4F && bytes[pos + 1] == 0x67 && bytes[pos + 2] == 0x67 &&
          bytes[pos + 3] == 0x53)) {
      /* 容忍流前面有杂质：往后找下一个页头 */
      const int next = index_of_ascii(bytes, len, len, "OggS", pos + 1);
      if (next < 0) break;
      pos = (size_t)next;
      continue;
    }
    const size_t seg_count = bytes[pos + 26];
    const size_t table = pos + 27;
    const size_t payload = table + seg_count;
    if (payload > len) break;

    size_t cursor = payload;
    for (size_t i = 0; i < seg_count; i++) {
      const size_t seg_len = bytes[table + i];
      if (cursor + seg_len > len) {
        cursor = len;
        break;
      }
      if (seg_len > 0) {
        if (st->pkt_len + seg_len > st->pkt_cap) {
          size_t next = (st->pkt_cap > 0) ? st->pkt_cap : 4096;
          while (next < st->pkt_len + seg_len) next *= 2;
          uint8_t *grown = (uint8_t *)dsh_mem_alloc(next);
          if (grown == NULL) {
            dsh_set_last_error("内存不足：Speex 包缓冲（%zu 字节）", next);
            st->oom = 1;
            return DSH_SPEEX_EOMEM;
          }
          if (st->pkt_len > 0) memcpy(grown, st->pkt, st->pkt_len);
          if (st->pkt != NULL) dsh_release(st->pkt);
          st->pkt = grown;
          st->pkt_cap = next;
        }
        memcpy(st->pkt + st->pkt_len, bytes + cursor, seg_len);
        st->pkt_len += seg_len;
      }
      cursor += seg_len;
      if (seg_len < 255) {
        /* 一个包凑齐了 */
        const int rc = on_packet(st, want_pcm, reason, reason_cap);
        if (rc != DSH_SPEEX_OK) return rc;
        st->packets++;
        st->pkt_len = 0;
      }
    }
    pos = cursor;
  }
  /* 流末尾没有以「短 segment」收尾的残余包：**丢掉**（截断的文件只能解出完整帧）*/
  return DSH_SPEEX_OK;
}

/**
 * 干活的唯一一处实现（`dsh_speex_probe` 与 `dsh_speex_decode_wav` 都走它）。
 *
 * `want_pcm = 0`：只解析容器与头（诊断）；`1`：连 PCM 一起解出来。
 * 两侧的**检查标准与文案完全一样** —— 不然「诊断说能解、解码说解不了」会变成常事。
 * 报错顺序：先数包够不够，再说头的事，再说附加头，最后才说「一帧都没解出来」。
 */
static int run(const uint8_t *bytes, size_t len, int want_pcm, spx_state *st, char *reason,
               size_t reason_cap) {
  if (index_of_ascii(bytes, len, 256, "Speex   ", 0) < 0) {
    snprintf(reason, reason_cap, "这不是 Ogg Speex 流（没有找到 `Speex` 标记）。");
    return DSH_SPEEX_ENOPE;
  }
  const int rc = walk_pages(bytes, len, st, want_pcm, reason, reason_cap);
  if (rc != DSH_SPEEX_OK) return rc;

  /* ── 报错顺序（照参考实现）── */
  if (st->packets < 2) {
    snprintf(reason, reason_cap, "Ogg Speex 流里只有头部、没有音频数据（文件可能不完整）。");
    return DSH_SPEEX_ENOPE;
  }
  if (!st->have_info) {
    /* 头那一档的具体原因在解析时就写好了（不足 80 字节 / 声道数 / 模式）*/
    snprintf(reason, reason_cap, "%s", st->header_reason);
    return DSH_SPEEX_ENOPE;
  }
  if (st->first_audio >= st->packets) {
    snprintf(reason, reason_cap, "Speex 头里写着还有 %d 个附加头，但流里已经没有数据了。",
             st->info.extra_headers);
    return DSH_SPEEX_ENOPE;
  }
  if (!want_pcm) return DSH_SPEEX_OK;
  if (st->frames == 0) {
    snprintf(reason, reason_cap,
             "这段 Speex 里没有可解码的音频帧（文件可能被截断或不是标准的 Ogg Speex）。");
    return DSH_SPEEX_ENOPE;
  }
  return DSH_SPEEX_OK;
}

/* ── 对外两个入口 ──────────────────────────────────────────────────────── */

int dsh_speex_probe(const uint8_t *bytes, size_t len, dsh_speex_info *out_info, char *reason,
                    size_t reason_cap) {
  if (reason == NULL || reason_cap == 0) {
    dsh_set_last_error("dsh_speex_probe：reason 缓冲不能为空");
    return DSH_SPEEX_EARG;
  }
  reason[0] = '\0';
  if (bytes == NULL || len == 0) {
    /* 空内容也是**如实报错**，不是「成功返回空」 */
    snprintf(reason, reason_cap, "音频内容是空的。");
    return DSH_SPEEX_ENOPE;
  }
  spx_state st;
  memset(&st, 0, sizeof(st));
  st.header_rc = DSH_SPEEX_ENOPE;
  const int rc = run(bytes, len, 0, &st, reason, reason_cap);
  if (rc == DSH_SPEEX_OK && out_info != NULL) {
    memcpy(out_info, &st.info, sizeof(*out_info));
    out_info->packet_count = (int)st.packets;
    out_info->audio_packet_count = (int)(st.packets - st.first_audio);
  }
  spx_free(&st);
  return rc;
}

int dsh_speex_decode_wav(const uint8_t *bytes, size_t len, uint8_t **out_wav,
                         size_t *out_wav_len, char *reason, size_t reason_cap,
                         dsh_speex_info *out_info) {
  if (out_wav == NULL || out_wav_len == NULL || reason == NULL || reason_cap == 0) {
    dsh_set_last_error("dsh_speex_decode_wav：出参与 reason 都不能为空");
    return DSH_SPEEX_EARG;
  }
  *out_wav = NULL;
  *out_wav_len = 0;
  reason[0] = '\0';
  if (bytes == NULL || len == 0) {
    snprintf(reason, reason_cap, "音频内容是空的。");
    return DSH_SPEEX_ENOPE;
  }

  spx_state st;
  memset(&st, 0, sizeof(st));
  st.header_rc = DSH_SPEEX_ENOPE;
  const int rc = run(bytes, len, 1, &st, reason, reason_cap);
  if (rc != DSH_SPEEX_OK) {
    if (rc == DSH_SPEEX_EOMEM) {
      /* 内存不足：`dsh_set_last_error` 已经在出问题的那个点上写好了 */
    } else {
      /* 解不了也要有一条 last_error（与内核其它模块同约定：调用方能查）
       * ⚠️ 顺序：先写 last_error，再动 st（它不能被 last_error 影响）*/
      dsh_set_last_error("Speex 解不了：%s", reason);
    }
    spx_free(&st);
    return rc;
  }

  /* 回填 WAV 头（那 44 字节建解码器时就先留出来了）*/
  wav_header(st.out.p, st.info.rate, st.info.channels, (uint32_t)(st.out.len - 44));
  if (out_info != NULL) {
    memcpy(out_info, &st.info, sizeof(*out_info));
    out_info->packet_count = (int)st.packets;
    out_info->audio_packet_count = (int)(st.packets - st.first_audio);
  }
  *out_wav = st.out.p; /* 所有权交出去 */
  *out_wav_len = st.out.len;
  st.out.p = NULL;
  st.out.len = st.out.cap = 0;
  spx_free(&st);
  return DSH_SPEEX_OK;
}
