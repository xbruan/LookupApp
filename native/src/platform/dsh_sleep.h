/* 让出 CPU 大约 ms 毫秒：不保证精确，调用方只当「让出 CPU」用、不许拿它当时钟。
 * 平台差异只许出现在平台层（见 native/README.md），故它单独一份、调用方不写 #ifdef。 */

#ifndef DSH_PLATFORM_SLEEP_H
#define DSH_PLATFORM_SLEEP_H

/**
 * 让出 CPU 大约 ms 毫秒。
 *
 * @param ms 毫秒数；0 表示只让出调度权（不保证一定有别的线程被调度）
 */
void dsh_sleep_ms(int ms);

#endif /* DSH_PLATFORM_SLEEP_H */
