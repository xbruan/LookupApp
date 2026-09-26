/* 见 dsh_time.h —— 当前 UTC 毫秒。 */
#include "platform/dsh_time.h"

#if defined(_WIN32)

#include <windows.h>

int64_t dsh_now_ms(void) {
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  /* FILETIME 从 1601 年起算、单位 100 纳秒，与 Unix 纪元差 11644473600 秒 */
  const uint64_t ticks = ((uint64_t)ft.dwHighDateTime << 32) | (uint64_t)ft.dwLowDateTime;
  const uint64_t unix_100ns = ticks - 116444736000000000ull;
  return (int64_t)(unix_100ns / 10000ull);
}

#elif defined(__unix__) || defined(__APPLE__)

#include <time.h>

int64_t dsh_now_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
  return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

#else

int64_t dsh_now_ms(void) { return 0; }

#endif
