/* ==========================================================================
 * PCM 增益 · **纯计算**：只管**把一份已经归一化过的 dB 真的乘到 16 位 PCM 上**，外加
 * 削顶保护（增益归一化本身住在设置层，不是这里的事）。
 *
 * 为什么单独一层：增益是**听出不对才知道错**的东西，靠人耳试错太贵，所以把算术抽干净、
 * 拿固定用例钉死（测试）。
 * ⚠️ 三条刻意的约定（别当「顺手优化」）：① **顺序扫 RIFF 块**找 `fmt ` 与 `data`，不假设
 * 44 字节头、也不假设只有一段 data（SAPI 会插 `LIST` / `fact` 块，写死 44 会让增益「时灵
 * 时不灵」而且错得隐蔽）；② **只动 16 位 PCM**，别的格式原样返回并说明，不猜着改；
 * ③ **削顶不能悄悄过去**：夹了多少要通过 `out_note` 说清。
 * ========================================================================== */

#ifndef DSH_AUDIO_DSH_GAINMATH_H
#define DSH_AUDIO_DSH_GAINMATH_H

#include <stddef.h>
#include <stdint.h>

/** dB → 线性倍率（数字音量只有这一个换算方向需要）*/
double dsh_gain_db_to_factor(double db);

/**
 * 削顶保护：把请求的增益夹到「峰值乘上去正好顶到满刻度」为止。
 *
 * 返回值永远**不大于**请求值 —— 所以压音量（负增益）从来不受影响，只有「想提但会削顶」时
 * 才会被夹。峰值 ≤ 0（整段静音）时不夹。
 *
 * @param peak 峰值（按满刻度 32768 归一到 0..1）
 */
double dsh_gain_clamp_to_peak(double requested_db, double peak);

/**
 * 这一档要不要改字节：**只有「峰值已经顶格、请求提音量」才会被夹到 0**，而那时正是最需要
 * 说明的一次 —— 检查标准是「实际与请求差多少」，不是「实际是不是 0」。
 */
#define DSH_GAIN_CLAMP_NOTE_TOLERANCE 0.05

/**
 * 把一段 WAV 里的 16 位 PCM 样本按 `requested_db` 缩放。
 *
 * ⚠️ **就地改**（`wav` 会被改写）：`applied_db == 0` 时**一个字节都不动**（默认路径必须和
 * 「没有这个功能」逐字节相同）；格式动不了（不是 16 位 PCM / 不是 RIFF-WAVE / 太短 / 没有
 * data 块）时也不动，只在 `out_note` 里说明为什么。
 *
 * @param wav            整段 WAV 字节（RIFF/WAVE）
 * @param len            字节数
 * @param requested_db   请求的增益（**已经归一化过**：调用方负责夹到 [-24,+12]）
 * @param out_applied_db 出参：实际施加的 dB（夹到峰值之后**向下**取整到 0.1）
 * @param out_note       出参：要如实说给用户的那句话（没有就是空串）。装不下就截断
 * @param note_cap       `out_note` 的容量
 * @return 1 = 真的改过字节；0 = 原样返回（`out_note` 说明原因）
 */
int dsh_gain_apply_to_wav(uint8_t *wav, size_t len, double requested_db, double *out_applied_db,
                          char *out_note, size_t note_cap);

#endif /* DSH_AUDIO_DSH_GAINMATH_H */
