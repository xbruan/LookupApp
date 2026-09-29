/* ==========================================================================
 * 内核单元测试 · PCM 增益（`audio/dsh_gainmath.c` + 接口定义的 `dsh_audio_apply_gain`）
 *
 * 这一组钉五类事：① 两个纯换算（dB → 倍率、削顶保护）；② **能量真的上去了** ——
 * 检查标准钉**字节的能量**，不是函数返回了什么数；③ 削顶不能悄悄过去（被夹时
 * `appliedDb` 是夹之后的值、`note` 说清从多少到多少）；④ 0 dB 一个字节都不动
 * （默认路径必须与「没有这个功能」逐字节相同）；⑤ 动不了的格式一律原样返回并说明
 * （太短 / 不是 RIFF-WAVE / 不是 16 位 PCM / 没有 data 块）—— 猜着改就是把音频改坏。
 *
 * ⚠️ 全组**不合成、不发声、不联网**：WAV 是现场拼出来的（头部 + 一段已知的 PCM）。
 * ========================================================================== */

#include "dsh_lookup.h"
#include "audio/dsh_gainmath.h"
#include "mem_registry.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks = 0;
static int g_failed = 0;

static void ok(int cond, const char *what) {
  g_checks++;
  if (!cond) {
    g_failed++;
    fprintf(stderr, "FAIL %s\n", what);
  }
}

static void ok_has(const char *text, const char *needle, const char *what) {
  g_checks++;
  if (text == NULL || strstr(text, needle) == NULL) {
    g_failed++;
    fprintf(stderr, "FAIL %s\n      实际=%s\n      里面没有=%s\n", what, text ? text : "(null)",
            needle);
  }
}

static void ok_near(double actual, double expected, double tol, const char *what) {
  g_checks++;
  if (!(fabs(actual - expected) <= tol)) {
    g_failed++;
    fprintf(stderr, "FAIL %s\n      实际=%.4f 期望=%.4f（容差 %.4f）\n", what, actual, expected, tol);
  }
}

/* ── 现场：拼一段 16 位 PCM 的 WAV（头部按标准 44 字节，样本已知）────────── */

static void put_u16(uint8_t *p, unsigned value) {
  p[0] = (uint8_t)(value & 0xFFu);
  p[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static void put_u32(uint8_t *p, unsigned long value) {
  p[0] = (uint8_t)(value & 0xFFu);
  p[1] = (uint8_t)((value >> 8) & 0xFFu);
  p[2] = (uint8_t)((value >> 16) & 0xFFu);
  p[3] = (uint8_t)((value >> 24) & 0xFFu);
}

/** 拼一段 WAV：`samples` 里的 16 位样本原样进 data 块 */
static size_t make_wav(uint8_t *buf, size_t cap, const int16_t *samples, size_t count) {
  const size_t data_len = count * 2;
  size_t i;
  if (cap < 44 + data_len) return 0;
  memcpy(buf, "RIFF", 4);
  put_u32(buf + 4, (unsigned long)(36 + data_len));
  memcpy(buf + 8, "WAVE", 4);
  memcpy(buf + 12, "fmt ", 4);
  put_u32(buf + 16, 16);
  put_u16(buf + 20, 1);    /* PCM */
  put_u16(buf + 22, 1);    /* 单声道 */
  put_u32(buf + 24, 16000);/* 采样率 */
  put_u32(buf + 28, 32000);
  put_u16(buf + 32, 2);    /* 块对齐 */
  put_u16(buf + 34, 16);   /* 位深 */
  memcpy(buf + 36, "data", 4);
  put_u32(buf + 40, (unsigned long)data_len);
  for (i = 0; i < count; i++) put_u16(buf + 44 + i * 2, (unsigned)(uint16_t)samples[i]);
  return 44 + data_len;
}

/** data 块里的第 i 个样本（16 位有符号小端）*/
static int16_t sample_at(const uint8_t *wav, size_t index) {
  size_t at = 44 + index * 2;
  return (int16_t)((uint16_t)wav[at] | ((uint16_t)wav[at + 1] << 8));
}

/** 从平坦 JSON 里取一个**数字**字段。`dsh_json_double` 用 `%.17g`：6.0 会写成 `6`，
 *  所以检查标准必须**钉那个数**，不许钉子串（数值的字符串写法不固定，拼出来匹配不上）*/
static double num_field(const char *json, const char *key) {
  char pat[96];
  const char *p;
  if (json == NULL) return -1e30;
  snprintf(pat, sizeof(pat), "\"%s\":", key);
  p = strstr(json, pat);
  if (p == NULL) return -1e30;
  return strtod(p + strlen(pat), NULL);
}

/** 一整段 PCM 的能量（平方和）—— ★ 检查标准就钉它 */
static double energy_of(const uint8_t *wav, size_t count) {
  double sum = 0;
  size_t i;
  for (i = 0; i < count; i++) {
    double v = (double)sample_at(wav, i);
    sum += v * v;
  }
  return sum;
}

/* ── ① 两个纯换算 ──────────────────────────────────────────────────────── */

static void test_math(void) {
  ok_near(dsh_gain_db_to_factor(0), 1.0, 1e-9, "① 0 dB → ×1");
  ok_near(dsh_gain_db_to_factor(6), 1.9953, 1e-3, "① +6 dB → ×2（能量 ×4）");
  ok_near(dsh_gain_db_to_factor(-6), 0.5012, 1e-3, "① -6 dB → ×0.5");
  ok_near(dsh_gain_db_to_factor(12), 3.9811, 1e-3, "① +12 dB → ×4");

  /* 削顶保护：峰值顶格（1.0）时天花板是 0 dB */
  ok_near(dsh_gain_clamp_to_peak(12, 1.0), 0.0, 1e-9, "① 峰值顶格 → 天花板 0 dB（提不动）");
  ok_near(dsh_gain_clamp_to_peak(-6, 1.0), -6.0, 1e-9, "① 压音量不受削顶保护影响");
  ok_near(dsh_gain_clamp_to_peak(12, 0.5), 6.0206, 1e-3, "① 峰值 0.5 → 最多提 +6.02 dB");
  ok_near(dsh_gain_clamp_to_peak(12, 0.0), 12.0, 1e-9, "① 整段静音（峰值 0）不夹");
  ok_near(dsh_gain_clamp_to_peak(-3, 0.0), -3.0, 1e-9, "① 静音 + 压音量也不夹");
}

/* ── ② ★ 能量真的上去了 ────────────────────────────────────────────────── */

static void test_energy(void) {
  int16_t samples[64];
  uint8_t wav0[44 + 128];
  uint8_t wav6[44 + 128];
  uint8_t wavm6[44 + 128];
  size_t len;
  double applied = -1;
  char note[256];
  double e0;
  double e6;
  double e6_expected;
  int i;

  /* 幅度 8000 的方波（峰值 8000/32768 ≈ 0.244 → 天花板约 +12.2 dB，所以 +6 提得动）*/
  for (i = 0; i < 64; i++) samples[i] = (i % 2 == 0) ? 8000 : -8000;
  len = make_wav(wav0, sizeof(wav0), samples, 64);
  ok(len > 0, "② 现场拼出一段 WAV");
  memcpy(wav6, wav0, len);
  memcpy(wavm6, wav0, len);
  e0 = energy_of(wav0, 64);

  /* 0 dB：一个字节都不许动 */
  applied = -1;
  note[0] = '\0';
  ok(dsh_gain_apply_to_wav(wav0, len, 0.0, &applied, note, sizeof(note)) == 0,
     "② 0 dB → 不改字节（changed=0）");
  ok_near(applied, 0.0, 1e-9, "② 0 dB → appliedDb 是 0");
  ok(note[0] == '\0', "② 0 dB 不用说任何话");

  /* +6 dB：能量必须上去，而且约 ×4（振幅 ×2）*/
  applied = -1;
  note[0] = '\0';
  ok(dsh_gain_apply_to_wav(wav6, len, 6.0, &applied, note, sizeof(note)) == 1,
     "② +6 dB → 改了字节");
  ok_near(applied, 6.0, 1e-9, "② +6 dB → appliedDb 就是 6.0（没被夹）");
  e6 = energy_of(wav6, 64);
  ok(e6 > e0, "②★ 写 +6 dB 之后**能量高于** 0 dB 那次（这就是这一项的完工检查标准）");
  e6_expected = e0 * 4.0; /* 振幅 ×2 → 能量 ×4 */
  ok(fabs(e6 - e6_expected) / e6_expected < 0.01,
     "②★ 而且约等于 ×4（振幅 ×2 的平方）—— 逐样本算一遍");
  ok(sample_at(wav6, 0) > sample_at(wav0, 0), "② 单个样本确实变大了");
  ok(note[0] == '\0', "② 没被夹就不用说话");

  /* -6 dB：能量约 ÷4 */
  applied = -1;
  ok(dsh_gain_apply_to_wav(wavm6, len, -6.0, &applied, note, sizeof(note)) == 1,
     "② -6 dB → 改了字节");
  ok(energy_of(wavm6, 64) < e0, "② 压音量之后能量更低");
  ok(fabs(energy_of(wavm6, 64) - e0 / 4.0) / (e0 / 4.0) < 0.02, "② 压一半多一点（×0.5 的平方）");
}

/* ── ③ 削顶保护：不能悄悄过去 ──────────────────────────────────────────── */

static void test_clipping(void) {
  int16_t loud[32];
  uint8_t wav[44 + 64];
  size_t len;
  double applied = 0;
  char note[256];
  int i;

  /* 峰值顶格（30000/32768 ≈ 0.9155）→ 天花板约 +0.77 dB，+12 必然被夹 */
  for (i = 0; i < 32; i++) loud[i] = (i % 2 == 0) ? 30000 : -30000;
  len = make_wav(wav, sizeof(wav), loud, 32);

  note[0] = '\0';
  ok(dsh_gain_apply_to_wav(wav, len, 12.0, &applied, note, sizeof(note)) == 1 || applied != 0,
     "③ 峰值顶格 + 请求 +12 dB → 还是动了一点（夹到天花板）");
  ok(applied < 12.0, "③★ appliedDb 被夹到小于请求值");
  ok(applied > 0.0, "③ 而且仍然是个正值（天花板没到 0）");
  ok_has(note, "夹到", "③★ 被夹时**必须说出来**（削顶不能悄悄过去）");
  ok_has(note, "12.0", "③ 那句话里带着请求值");
  ok_has(note, "削顶", "③ 也说清了为什么（不夹就会削顶破音）");
  /*
   * 缩放之后**不许绕回**（防御性夹取挡的就是这件事：32767.5 四舍五入成 32768 会变成负数，
   * 听起来是一声爆音）。检查标准是「同号且幅度不减」——
   * ⚠️ 别写成 `sample <= 32767`：int16 转 int 之后那条比较**永远为真**（`-Werror=type-limits`
   *    会挡下），是典型的「钉了个替代指标」。
   */
  for (i = 0; i < 32; i++) {
    int before = loud[i];
    int after = sample_at(wav, i);
    ok((after >= 0) == (before >= 0) && abs(after) >= abs(before),
       "③ 缩放后的样本**没有绕回**（同号、幅度不减）—— 防御性夹取挡住了溢出");
  }
}

/* ── ④ 动不了的格式：**原样返回 + 说明** ───────────────────────────────── */

static void test_untouched(void) {
  uint8_t tiny[30];
  uint8_t notwave[100];
  uint8_t baddepth[44 + 64];
  uint8_t nodata[60];   /* ⚠️ 要 > 44：否则先撞上「太短」那一档 */
  int16_t samples[32];
  double applied = 123;
  char note[256];
  int i;

  for (i = 0; i < 32; i++) samples[i] = 1000;
  memset(tiny, 0, sizeof(tiny));

  /* 太短 */
  note[0] = '\0';
  ok(dsh_gain_apply_to_wav(tiny, sizeof(tiny), 6.0, &applied, note, sizeof(note)) == 0,
     "④ 太短的一段 → 不改");
  ok_near(applied, 0.0, 1e-9, "④ 太短时 appliedDb 是 0");
  ok_has(note, "太短", "④ 而且说明是「太短」");

  /* 不是 RIFF/WAVE */
  memset(notwave, 0x41, sizeof(notwave));
  note[0] = '\0';
  ok(dsh_gain_apply_to_wav(notwave, sizeof(notwave), 6.0, &applied, note, sizeof(note)) == 0,
     "④ 不是 RIFF/WAVE → 不改");
  ok_has(note, "RIFF", "④ 说明是「不是 RIFF/WAVE」");

  /* 位深不是 16：把 fmt 里的位深改成 8 */
  make_wav(baddepth, sizeof(baddepth), samples, 32);
  put_u16(baddepth + 34, 8);
  note[0] = '\0';
  ok(dsh_gain_apply_to_wav(baddepth, sizeof(baddepth), 6.0, &applied, note, sizeof(note)) == 0,
     "④ 8 位 PCM → 不改（只动 16 位，别猜着改）");
  ok_has(note, "16 位", "④ 说明是「不是 16 位 PCM」");
  ok_has(note, "位深=8", "④ 连实际位深一起说出来");

  /* 没有 data 块：只有头 */
  memset(nodata, 0, sizeof(nodata));
  memset(nodata, 0, sizeof(nodata));
  memcpy(nodata, "RIFF", 4);
  put_u32(nodata + 4, 52);
  memcpy(nodata + 8, "WAVE", 4);
  memcpy(nodata + 12, "fmt ", 4);
  put_u32(nodata + 16, 16);
  put_u16(nodata + 20, 1);
  put_u16(nodata + 34, 16);
  note[0] = '\0';
  ok(dsh_gain_apply_to_wav(nodata, sizeof(nodata), 6.0, &applied, note, sizeof(note)) == 0,
     "④ 没有 data 块 → 不改");
  ok_has(note, "data", "④ 说明是「没有可缩放的 data 块」");

  /* 非有限值 */
  note[0] = '\0';
  ok(dsh_gain_apply_to_wav(nodata, sizeof(nodata), NAN, &applied, note, sizeof(note)) == 0,
     "④ NaN → 不改");
  (void)applied;
}

/* ── ⑤ 接口定义那一条（`dsh_audio_apply_gain`）：拷一份、给元信息、0 dB 逐字节相同 ── */

static void test_abi(void) {
  int16_t samples[32];
  uint8_t wav[44 + 64];
  size_t len;
  uint8_t *out = NULL;
  size_t out_len = 0;
  char *meta = NULL;
  int i;

  for (i = 0; i < 32; i++) samples[i] = (i % 2 == 0) ? 6000 : -6000;
  len = make_wav(wav, sizeof(wav), samples, 32);

  /* +6 dB（传 0.1 dB 单位的整数：60）*/
  ok(dsh_audio_apply_gain(wav, len, 60, &out, &out_len, &meta) == DSH_OK && out != NULL,
     "⑤ 接口定义那一条回得来字节");
  ok(out_len == len, "⑤ 长度不变（只改样本）");
  ok_has(meta, "\"changed\":true", "⑤ 元信息说改了");
  ok_near(num_field(meta, "appliedDb"), 6.0, 1e-6, "⑤ 元信息里的 appliedDb 是 6.0（钉数字，不钉它的字符串写法）");
  ok(energy_of(out, 32) > energy_of(wav, 32), "⑤ 出参的字节能量确实更高（而且**入参没被改**）");
  ok(sample_at(wav, 0) == 6000, "⑤★ 宿主那份缓冲**一个字节都没动**（内核拷一份再改）");
  dsh_release(out);
  out = NULL;
  if (meta != NULL) dsh_release(meta);
  meta = NULL;

  /* 0 dB：逐字节相同 */
  ok(dsh_audio_apply_gain(wav, len, 0, &out, &out_len, &meta) == DSH_OK && out != NULL,
     "⑤ 0 dB 也照给字节");
  ok(out_len == len && memcmp(out, wav, len) == 0,
     "⑤★ 0 dB 时**逐字节相同**（默认路径与「没有这个功能」完全一样）");
  ok_has(meta, "\"changed\":false", "⑤ 元信息说没改");
  dsh_release(out);
  out = NULL;
  if (meta != NULL) dsh_release(meta);
  meta = NULL;

  /* 参数边界 */
  ok(dsh_audio_apply_gain(NULL, len, 60, &out, &out_len, &meta) != DSH_OK, "⑤ bytes 为 NULL → 拒");
  ok(dsh_audio_apply_gain(wav, 0, 60, &out, &out_len, &meta) != DSH_OK, "⑤ len 为 0 → 拒");
  ok(dsh_audio_apply_gain(wav, len, 60, NULL, &out_len, &meta) != DSH_OK, "⑤ out_bytes 为 NULL → 拒");
}

int main(void) {
  const size_t base = dsh_mem_live_count();

  test_math();
  test_energy();
  test_clipping();
  test_untouched();
  test_abi();

  /* ⚠️ **实测结果必须打在最后一条断言之后**（`g_failed` 是在断言里累加的）。 */
  ok((size_t)dsh_mem_live_count() == base, "⑥ 这一组没漏内存（内核记账数回到起点）");
  printf("增益缩放：%d 项，失败 %d\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
