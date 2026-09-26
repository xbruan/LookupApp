/* ==========================================================================
 * 「这一次的覆盖」（`overrides_json`）：两条语音接口共用同一份，接口定义见
 * `abi/lookup.abi.json` 的 `dsh_speech_plan` / `dsh_speech_online_plan` 同名参数。
 * ⚠️ 只有一份，是因为规划与请求体必须一致（各解析一次迟早出现「规划说 A、请求体里是 B」）；
 * 规则：键在 = 强制这个值，键不在 = 照设置；NULL / 空串 = 全套照设置。
 * ========================================================================== */

#ifndef DSH_SPEECH_OVERRIDES_H
#define DSH_SPEECH_OVERRIDES_H

#include <stddef.h>

#define DSH_OVERRIDE_SOURCE_MAX 16
#define DSH_OVERRIDE_VOICE_MAX 256
#define DSH_OVERRIDE_LANGUAGE_MAX 16

typedef struct {
  int has_source;                            /**< 在 = **只许走这一层** */
  char source[DSH_OVERRIDE_SOURCE_MAX];      /**< 「dict」/「online」/「system」*/
  int has_voice;                             /**< 在 = 覆盖音色（online: 豆包音色；system: 本机音色 id/名）*/
  char voice[DSH_OVERRIDE_VOICE_MAX];
  int has_language;                          /**< 在 = 覆盖语种（影响 online 选音色与 system 选择音色）*/
  char language[DSH_OVERRIDE_LANGUAGE_MAX];
  int has_loudness;                          /**< 在 = 覆盖响度补偿（online）*/
  int loudness;                              /**< 已夹到 [-50, 100] */
  int has_gain;                              /**< 在 = 覆盖增益（system，dB×10；**由壳施加**）*/
  int gain_tenths;
} dsh_speech_overrides;

/**
 * 解析覆盖。
 * @param overrides_json NULL / 空串 = 全套照设置（**不是错误**）
 * @param out            出参（`has_*` 全 0 表示没有覆盖）
 * @param what           出错时拼进 last_error 的那个接口名
 * @return 0 = 成功；-1 = JSON 坏掉（**如实拒**，不猜；last_error 已写）*/
int dsh_speech_overrides_parse(const char *overrides_json, dsh_speech_overrides *out,
                               const char *what);

/** `source` 覆盖值是不是合法的层名（dict / online / system）*/
int dsh_speech_overrides_source_known(const char *source);

#endif /* DSH_SPEECH_OVERRIDES_H */
