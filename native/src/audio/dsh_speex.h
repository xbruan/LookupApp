/* ==========================================================================
 * Ogg Speex（`.spx`）→ 16bit PCM WAV。解码本体用官方 libspeex（`vendor/speex/`）。
 *
 * 为什么必须住在内核：Chromium（WebView2）**不解码 Speex**，而 LDOCE5 那类词典的自带
 * 录音全是 Ogg Speex —— 只能自己解。它是**内容处理**不是界面形状，所以界面只拿到
 * 「能播的字节 + 一个 MIME」。
 *
 * 分工：**本模块**管容器与打包（Ogg 页重组、Speex 头字段、逐帧循环、WAV 封装）；
 * **`vendor/speex/`** 管 CELP 解码本体 —— 换实现只需换那一个目录。
 *
 * ⚠️ 与参考实现的已知差异：参考实现用 NSpeex，本模块用官方 libspeex。两者**采样数、时长、
 *    granule 完全一致**，波形是同一段音频但**不是逐位一致**（差异来自后级感知增强滤波与
 *    取整方式）—— 本模块解出来的 PCM 与官方 libspeex 的相关系数是 1.0000，比参考实现更近。
 * ========================================================================== */

#ifndef DSH_SPEEX_H
#define DSH_SPEEX_H

#include <stddef.h>
#include <stdint.h>

/* ── 返回码 ──────────────────────────────────────────────────────────────── */
#define DSH_SPEEX_OK 0     /* 解好了（probe 那一档是「能解」）*/
#define DSH_SPEEX_ENOPE 1  /* 这段码流解不了 —— `reason` 里已写好人话 */
#define DSH_SPEEX_EARG (-1)   /* 参数不对 */
#define DSH_SPEEX_EOMEM (-2)  /* 内存不足 */

/** PCM 出参的上限：64 MB（16 kHz 单声道约 35 分钟）—— 防一段畸形的码流把内存吃光 */
#define DSH_SPEEX_MAX_PCM_BYTES ((size_t)64 * 1024 * 1024)

/** 一段 Ogg Speex 的流信息（对应参考实现的 `SpeexStreamInfo`；诊断与测试用，界面不显示）*/
typedef struct {
  /* **播放速率** —— 取头里写的 `rate`（只在它明显不合理时才退回模式速率）。
   * ⚠️ **不要用「模式速率」替换它**：LDOCE5 那种 `rate=22050 / mode=1 / frame_size=320`
   * 的文件**已经骗过两个人**，两次都是用户听出来「声音偏低」才纠回来的。 */
  int rate;
  /** 0 窄带 / 1 宽带 / 2 超宽带 —— 决定用哪个解码器、每帧多少采样（**不决定播放速率**）*/
  int mode;
  /** 本模式「天然」的采样率（8000 / 16000 / 32000），只用于对照与兜底 */
  int mode_rate;
  int channels;
  /** 头里写的每帧采样数（与模式应当吻合，诊断用）*/
  int header_frame_size;
  /** 头里写的采样率（与 `rate` 在正常情形下同值，单独留一份便于诊断对比）*/
  int header_rate;
  int extra_headers;
  /** 头里的版本串，例如 `1.2rc1`（定长 20 字节、`\0` 补齐）*/
  char version[24];
  int packet_count;
  int audio_packet_count;
} dsh_speex_info;

/**
 * 看着像 Ogg Speex 吗？检查标准：`OggS` 捕获模式 + 前 256 字节里有 `"Speex   "`（三个空格）
 * 标记。**不解容器、不看长度**，所以便宜到能在嗅探阶段调。
 *
 * ⚠️ 内核里「这是不是 Ogg Speex」**只有这一处实现**（`dsh_speech_api.c` 的 `detect_codec`
 *    也调它）—— 两处各写一遍迟早分叉。
 */
int dsh_speex_looks_like(const uint8_t *bytes, size_t len);

/**
 * 只解析容器与头，**不解码**（诊断与测试断言用）。
 *
 * @param out_info    出参（可为 NULL）；能解时写入流信息
 * @param reason      解不了时写人话；容量 `reason_cap`
 * @return `DSH_SPEEX_OK` / `DSH_SPEEX_ENOPE` / `DSH_SPEEX_EARG`
 */
int dsh_speex_probe(const uint8_t *bytes, size_t len, dsh_speex_info *out_info, char *reason,
                    size_t reason_cap);

/**
 * 把一段 Ogg Speex 解成 **16bit PCM 的 WAV**（44 字节头 + PCM）。
 * 感知增强（`SPEEX_SET_ENH`）**开着**，与 libspeex 系播放器的默认一致。
 *
 * @param out_wav      出参：内核分配（调用方 `dsh_release`）；失败时写 NULL
 * @param out_wav_len  出参：WAV 字节数
 * @param reason       解不了时写人话（**直接给用户看**）；容量 `reason_cap`
 * @param out_info     出参（可为 NULL）：解出来的流信息
 * @return `DSH_SPEEX_OK` / `DSH_SPEEX_ENOPE`（reason 已写）/ `DSH_SPEEX_EARG` /
 *         `DSH_SPEEX_EOMEM`（last_error 已写）
 */
int dsh_speex_decode_wav(const uint8_t *bytes, size_t len, uint8_t **out_wav,
                         size_t *out_wav_len, char *reason, size_t reason_cap,
                         dsh_speex_info *out_info);

/** 模式 → 模式速率（0→8000 / 1→16000 / 2→32000，其余按窄带）*/
int dsh_speex_mode_rate(int mode);

/** 模式 → 中文名（「窄带」 / 「宽带」 / 「超宽带」）；界面与日志直接用它 */
const char *dsh_speex_mode_name(int mode);

#endif /* DSH_SPEEX_H */
