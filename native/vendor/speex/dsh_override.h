/* ==========================================================================
 * 这个目录里**唯一一份是我们写的**（`libspeex/` 与 `include/` 都是上游原样拷贝）。
 *
 * 它只做一件事：**把 libspeex 的"致命错误"从 `exit(1)` 改成"打印一句、然后返回"**。
 *
 * 为什么非改不可：上游 `libspeex/os_support.h` 里那份 `_speex_fatal` 是
 *
 *     fprintf(stderr, "Fatal (internal) error in %s, line %d: %s\n", …);
 *     exit(1);
 *
 * —— 那是**从库内部把整个进程结束掉**。内核的硬规则是"绝不让任何东西把进程掀翻"，
 * 而 0.2.0 交付的是 GUI：一份畸形的 `.spx` 让用户的整个程序凭空消失，正是这条硬规则的反面。
 *
 * 实测（这一版 vendor 进来的 29 个 `.c` 里）：`speex_fatal(` 的调用点 **0 处** ——
 * 所以这是一道**保护措施**，不是"正在生效的补丁"。留着它是因为代价只有几行，
 * 而万一哪天升到一个真有调用点的版本，症状会是"程序没了"，那是最难查的一类。
 *
 * 用法：编译 `vendor/speex/` 那一份时带 `-include dsh_override.h`（见 构建脚本）。
 * 上游其它三处诊断（`speex_warning` / `speex_warning_int` / `speex_notify`）**不动**：
 * 它们只往 stderr 打一行，既不致命也不冒充内核给用户看的那句话 ——
 * "如实说"这条约定不值得为了安静而牺牲（测试里看得到反而是好事）。
 * ========================================================================== */

#ifndef DSH_SPEEX_OVERRIDE_H
#define DSH_SPEEX_OVERRIDE_H

#include <stdio.h>

/* 定义了它，上游就不再提供 `_speex_fatal`（见 os_support.h 的 `#ifndef`），
 * 由下面这一份顶上 —— 上游那条 `#define speex_fatal(str) _speex_fatal(…)` 照旧可用。 */
#define OVERRIDE_SPEEX_FATAL 1

static inline void _speex_fatal(const char *str, const char *file, int line) {
  fprintf(stderr, "libspeex: internal error in %s, line %d: %s\n", file, line, str);
  /* ⚠️ 上游在这里 `exit(1)`。我们**不退** —— 让调用方拿到一个返回值，
   *    `dsh_speex_decode_wav` 会把它变成"这段 Speex 解不了"那句人话。 */
}

#endif /* DSH_SPEEX_OVERRIDE_H */
