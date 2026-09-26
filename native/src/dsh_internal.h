/* 内核内部共享的声明 —— 不是对外接口定义（对外接口定义只有生成的 dsh_lookup.h）。
 * 只有实现内部的分工才放进来；业务规则（哪条查词通道、哪个落点、语种怎么判）一律不进。 */

#ifndef DSH_INTERNAL_H
#define DSH_INTERNAL_H

#include "dsh_lookup.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/* ── 内存（memory.c 实现，dsh_release 也在那里）──────────────────────────── */

/** 分配一块由 dsh_release 还给内核的内存。失败返回 NULL（调用方负责转成 DSH_E_OOM）。 */
void *dsh_mem_alloc(size_t size);

/** 复制一个 C 字符串（NULL 视作空串）。失败返回 NULL。 */
char *dsh_mem_strdup(const char *text);

/* ── 错误信息（memory.c 实现）───────────────────────────────────────────── */

/** 记下本线程最近一次失败的人话说明。dsh_last_error_message 会把它复制出来。 */
void dsh_set_last_error(const char *fmt, ...);

/** 清掉本线程上的错误状态（每次对外接口入口先调它 —— 免得读到上一次的旧错误）。 */
void dsh_clear_last_error(void);

#endif /* DSH_INTERNAL_H */
