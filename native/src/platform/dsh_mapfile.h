/* 把整个文件映射成只读视图 —— 内核里**唯一一件与系统有关的读操作**。
 * 返回的指针归映射所有：调用方不许释放、不许写，`dsh_map_close` 之后再取就是悬空指针。
 * 别退回「逐段 fseek + fread + 分配」（映射页按需调入、不占常驻集），也别把映射失败当文件坏了。 */

#ifndef DSH_PLATFORM_MAPFILE_H
#define DSH_PLATFORM_MAPFILE_H

#include <stddef.h>
#include <stdint.h>

/** 映射句柄（不透明）：里面的字段是平台自己的事，调用方只当它是一块只读内存。 */
typedef struct dsh_map dsh_map;

/** 把一个文件整块映射成只读内存。
 * @param path 文件路径（UTF-8）；@param out / @param out_len 出参：映射句柄与文件字节数。
 * @return 0 成功；-1 失败（映射不了、文件为空、平台不支持……）——失败时调用方**必须**退回逐段读。 */
int dsh_map_open(const char *path, dsh_map **out, int64_t *out_len);

/** 解映射。传 NULL 是合法的空操作。 */
void dsh_map_close(dsh_map *map);

/** 映射的基址（只读）。map 为 NULL 时返回 NULL。 */
const uint8_t *dsh_map_base(const dsh_map *map);

#endif /* DSH_PLATFORM_MAPFILE_H */
