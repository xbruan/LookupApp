/* 见 dsh_sleep.h —— 让出 CPU 一小会儿，跨平台的两行实现。 */
#include "platform/dsh_sleep.h"

#if defined(_WIN32)
#include <windows.h>
void dsh_sleep_ms(int ms) {
  if (ms <= 0) {
    Sleep(0);
    return;
  }
  Sleep((DWORD)ms);
}
#else
#include <time.h>
void dsh_sleep_ms(int ms) {
  if (ms <= 0) {
    /* ms<=0 只让出调度权：nanosleep(0) 的语义就是让出 */
    struct timespec zero = {0, 0};
    nanosleep(&zero, NULL);
    return;
  }
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  nanosleep(&ts, NULL);
}
#endif
