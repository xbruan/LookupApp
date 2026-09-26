/* PCM 增益 · 纯计算 —— 约定与出处见 `dsh_gainmath.h` 顶上那段。 */

#include "audio/dsh_gainmath.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

double dsh_gain_db_to_factor(double db) { return pow(10.0, db / 20.0); }

double dsh_gain_clamp_to_peak(double requested_db, double peak) {
  double ceiling;
  if (!(peak > 0)) return requested_db; /* 峰值 ≤ 0（整段静音）没有可保护的东西 */
  ceiling = 20.0 * log10(1.0 / peak);
  return (requested_db < ceiling) ? requested_db : ceiling;
}

/** 写一句人话（装不下就截断 —— 它只是给人看的）*/
static void say(char *out, size_t cap, const char *text) {
  if (out == NULL || cap == 0) return;
  if (text == NULL) text = "";
  /* ⚠️ 用 snprintf 而不是 strncpy：**它保证结尾有 0**（截断也保证） */
  snprintf(out, cap, "%s", text);
}

/** 无符号小端 32 位（流式写出来的 WAV 会把 data 长度写成 0xFFFFFFFF，读成有符号就全错）*/
static uint32_t read_u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t read_u16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }

/** dB 的文本形式：固定 1 位小数（与参考实现的 `DbToken` 同约定 —— 这句话是给人看的）*/
static void db_token(double db, char *out, size_t cap) {
  double value = (isnan(db) || isinf(db)) ? 0.0 : floor(db * 10.0 + 0.5) / 10.0;
  if (value == 0) value = 0; /* 把 -0.0 归一成 0 */
  snprintf(out, cap, "%.1f", value);
}

/* 最多记 8 段 `data` 块（真正的 WAV 只有一段，留余量给写坏了但还能读的文件）——
 * 超出的**不再改**：宁可不改，也不越界。 */
#define DSH_GAIN_MAX_DATA_CHUNKS 8

int dsh_gain_apply_to_wav(uint8_t *wav, size_t len, double requested_db, double *out_applied_db,
                          char *out_note, size_t note_cap) {
  int format_tag = -1;
  int bits_per_sample = -1;
  size_t chunk_off[DSH_GAIN_MAX_DATA_CHUNKS];
  size_t chunk_len[DSH_GAIN_MAX_DATA_CHUNKS];
  int chunk_count = 0;
  int more_chunks = 0;
  size_t pos;
  double peak = 0;
  double applied;
  double factor;
  int i;

  if (out_applied_db != NULL) *out_applied_db = 0;
  say(out_note, note_cap, "");

  if (isnan(requested_db) || isinf(requested_db)) {
    say(out_note, note_cap, "增益不是有限值，没施加增益。");
    return 0;
  }
  if (wav == NULL || len <= 44) {
    say(out_note, note_cap, "这段数据太短，不像一段完整 WAV，没施加增益。");
    return 0;
  }
  if (!(wav[0] == 'R' && wav[1] == 'I' && wav[2] == 'F' && wav[3] == 'F' && wav[8] == 'W' &&
        wav[9] == 'A' && wav[10] == 'V' && wav[11] == 'E')) {
    say(out_note, note_cap, "拿到的不是 RIFF/WAVE 数据，没施加增益。");
    return 0;
  }

  /* 扫块：长度按无符号读；声明的长度超出剩余字节时按实际剩余算（写坏了也不越界）*/
  pos = 12;
  while (pos + 8 <= len) {
    uint32_t size = read_u32(wav + pos + 4);
    size_t payload = pos + 8;
    if ((size_t)size > len - payload) size = (uint32_t)(len - payload);

    if (memcmp(wav + pos, "fmt ", 4) == 0 && size >= 16) {
      format_tag = (int)read_u16(wav + payload);
      bits_per_sample = (int)read_u16(wav + payload + 14);
    } else if (memcmp(wav + pos, "data", 4) == 0 && size > 0) {
      if (chunk_count < DSH_GAIN_MAX_DATA_CHUNKS) {
        chunk_off[chunk_count] = payload;
        chunk_len[chunk_count] = (size_t)size;
        chunk_count++;
      } else {
        more_chunks = 1;
      }
    }
    /* 块按偶数字节对齐：奇数长度后面有一个填充字节 */
    pos = payload + (size_t)size + ((size_t)size % 2u);
  }

  if (format_tag != 1 || bits_per_sample != 16) {
    char buf[160];
    snprintf(buf, sizeof(buf), "音频不是 16 位 PCM（format=%d，位深=%d），没施加增益。", format_tag,
             bits_per_sample);
    say(out_note, note_cap, buf);
    return 0;
  }
  if (chunk_count == 0) {
    say(out_note, note_cap, "WAV 里没有可缩放的 data 块，没施加增益。");
    return 0;
  }
  if (more_chunks) {
    /* 段数超了：**宁可整段不改**，也不要「改了一半」（那听起来是忽大忽小）*/
    say(out_note, note_cap, "这段 WAV 的 data 块太多，没施加增益。");
    return 0;
  }

  /* 峰值按满刻度 32768 归一（-32768 的绝对值是 32768，正好 1.0）*/
  for (i = 0; i < chunk_count; i++) {
    size_t k;
    for (k = 0; k + 1 < chunk_len[i]; k += 2) {
      int16_t sample = (int16_t)read_u16(wav + chunk_off[i] + k);
      double amplitude = fabs((double)sample / 32768.0);
      if (amplitude > peak) peak = amplitude;
    }
  }

  applied = dsh_gain_clamp_to_peak(requested_db, peak);
  /* 夹完再**向下**取整到 0.1 dB：宁可轻一点，也不要因为取整把峰值顶出满刻度 */
  applied = floor(applied * 10.0) / 10.0;
  if (out_applied_db != NULL) *out_applied_db = applied;

  if (fabs(applied - requested_db) >= DSH_GAIN_CLAMP_NOTE_TOLERANCE) {
    char req[32];
    char got[32];
    char buf[192];
    db_token(requested_db, req, sizeof(req));
    db_token(applied, got, sizeof(got));
    snprintf(buf, sizeof(buf),
             "峰值已经顶到满刻度，增益从 %s dB 夹到 %s dB（不夹就会削顶破音）。", req, got);
    say(out_note, note_cap, buf);
  }

  /* ★ 0 dB 一个字节都不动：默认路径（没调过增益）必须和加这个功能之前逐字节相同 */
  if (applied == 0) return 0;

  factor = dsh_gain_db_to_factor(applied);
  for (i = 0; i < chunk_count; i++) {
    size_t k;
    for (k = 0; k + 1 < chunk_len[i]; k += 2) {
      int16_t sample = (int16_t)read_u16(wav + chunk_off[i] + k);
      /* 四舍五入**远离零**（与参考实现的 MidpointRounding.AwayFromZero 同一条）*/
      double scaled_d = (double)sample * factor;
      long scaled = (long)(scaled_d >= 0 ? floor(scaled_d + 0.5) : ceil(scaled_d - 0.5));
      /* 防御性夹取：四舍五入本身能把 32767.5 推成 32768，绕回负数就是一声爆音。 */
      if (scaled > 32767) scaled = 32767;
      else if (scaled < -32768) scaled = -32768;
      wav[chunk_off[i] + k] = (uint8_t)((unsigned long)scaled & 0xFFu);
      wav[chunk_off[i] + k + 1] = (uint8_t)(((unsigned long)scaled >> 8) & 0xFFu);
    }
  }
  return 1;
}
