/* ==========================================================================
 * `speex_config_types.h` —— 由 `speex_config_types.h.in` 在 configure 时生成的**那一份**。
 *
 * 上游用 autotools 生成它（`@INCLUDE_STDINT@` / `@SIZE16@` … 由 configure 探测）。
 * 0.2.0 的内核不用 autotools（也没有 configure）—— 那几十个探测项里我们只需要
 * "C99 有 `<stdint.h>`、且这四种宽度就是这四种类型"，所以**手写一份等价物**，
 * 而不是把 autotools 搬进来。`speex_types.h` 只在非 Windows 下 include 它：
 * Windows（MSVC/MinGW）那一支用的是内置的 `__int16` / `short` 分支。
 *
 * ⚠️ 这是**上游文件的一份替代品，不改上游任何一个字节**（`.in` 原样留着，便于对照）。
 *    改上游版本时这一份要一起看：它只依赖 `<stdint.h>`，几十年不会变。
 * ========================================================================== */

#ifndef __SPEEX_TYPES_H__
#define __SPEEX_TYPES_H__

#include <stdint.h>

typedef int16_t spx_int16_t;
typedef uint16_t spx_uint16_t;
typedef int32_t spx_int32_t;
typedef uint32_t spx_uint32_t;

#endif
