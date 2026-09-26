/* 平台无关的那一小撮原语 —— 只有内核不得不碰系统的地方才进这里。
 * 纪律：本文件不含任何业务规则，只有锁与时钟这类原语；
 * 某个平台缺其中一项就在本目录加分支，不要让业务代码去 #ifdef。 */

#ifndef DSH_PORTABILTY_H
#define DSH_PORTABILTY_H

#if defined(_WIN32)
#  include <windows.h>
typedef CRITICAL_SECTION dsh_mutex;
static inline void dsh_mutex_init(dsh_mutex *m) { InitializeCriticalSection(m); }
static inline void dsh_mutex_lock(dsh_mutex *m) { EnterCriticalSection(m); }
static inline void dsh_mutex_unlock(dsh_mutex *m) { LeaveCriticalSection(m); }
#else
#  include <pthread.h>
typedef pthread_mutex_t dsh_mutex;
static inline void dsh_mutex_init(dsh_mutex *m) { pthread_mutex_init(m, NULL); }
static inline void dsh_mutex_lock(dsh_mutex *m) { pthread_mutex_lock(m); }
static inline void dsh_mutex_unlock(dsh_mutex *m) { pthread_mutex_unlock(m); }
#endif

#endif /* DSH_PORTABILTY_H */
