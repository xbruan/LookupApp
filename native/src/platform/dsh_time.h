/* 当前时间（Unix 毫秒）—— 内核里唯一需要问系统「现在几点」的地方。
 * 只回 UTC：落盘的 `addedAt` 一旦存本地时间，用户改时区 / 跨夏令时之后就全错位了。
 * 单调性不保证（用户调系统时钟就会跳），它不是计时器；系统能力只许出现在平台层（见 ADR-001）。 */

#ifndef DSH_PLATFORM_TIME_H
#define DSH_PLATFORM_TIME_H

#include <stdint.h>

/** 当前的 Unix 时间戳（毫秒，UTC）。取不到时返回 0（调用方按「未知时间」处理）。 */
int64_t dsh_now_ms(void);

#endif /* DSH_PLATFORM_TIME_H */
