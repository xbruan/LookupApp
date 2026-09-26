/* 文件元信息 + 原子写文件 —— 内核里与「落盘」有关的两件事，都属系统能力，只许出现在平台层（见 ADR-001）。
 * 原子写**绝不能直接往目标文件上写**：先写**同目录**临时文件 → fflush + fsync → rename 覆盖 ——
 * 中途断电只会留下旧的那份；跨卷 rename 不是原子的。⚠️ 本层**不建目录**，父目录必须已经存在。 */

#ifndef DSH_PLATFORM_FILE_H
#define DSH_PLATFORM_FILE_H

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

/** 文件元信息：给「词典 id = 内容哈希」当缓存键要用的那几项（路径 + 大小 + 修改时间）。 */
typedef struct {
  int64_t size;   /* 字节数 */
  int64_t mtime;  /* 最后修改时间（Unix 毫秒；取不到时为 0） */
  int is_file;    /* 1 = 是一个普通文件（目录为 0） */
  int is_dir;     /* 1 = 是一个目录 */
  int exists;     /* 1 = 存在（文件或目录） */
} dsh_file_info;

/** 取文件元信息。
 * @return 0 成功（结果写进 out；不存在时 `exists = 0` 且其余字段为 0 —— **「不存在」不是错误**，
 * 调用方要靠它区分「没这个文件」与「取不到元信息」）；非零失败（参数错）。 */
int dsh_file_stat(const char *path, dsh_file_info *out);

/** **打开一个文件读**（`rb`）—— 业务层要读文件时**一律走这一个**，别自己 `fopen`。
 * ⚠️ 它存在的唯一理由是**路径编码**：Win32 的 `fopen` 按**进程的 ANSI 代码页**解路径，只有那台机器
 * 的代码页恰好是 UTF-8（65001）时才碰巧对；本层在 Windows 上转 UTF-16 再 `_wfopen`，POSIX 上就是 `fopen`。
 * @return 打开的流（调用方 `fclose`）；失败回 NULL（原因按需自己报，别猜 errno）。 */
FILE *dsh_file_open_read(const char *path);

/** 原子地把一段内容写进文件（见文件头的解释）。
 * @param path 目标路径（UTF-8）；@param data 内容；@param len 字节数。
 * @return 0 成功；非零失败，原因写进 last_error。 */
int dsh_file_write_atomic(const char *path, const void *data, size_t len);

/** **追加**写一段内容（`ab` 模式 + `fflush`），只写这么多字节；@return 0 成功，非零失败并写 last_error。
 * 分工就是「这份数据会不会长」：settings.json 是整体替换 → 原子写；history.jsonl 一行一行往里长 → 追加。
 * ⚠️ 它只 `fflush`、**不 `fsync`**：进程被杀（含强杀）之后还在，整机断电可能丢最后几行 ——
 * 有意如此，历史是可丢的数据，别为它把每一次查词都挂到磁盘上。 */
int dsh_file_append(const char *path, const void *data, size_t len);

/** 删掉一个文件。**文件本来就不在 = 成功**（调用方要的就是「它不在了」）——
 * 用途是清掉上一版留下的文件，那种场景下「早就没了」与「刚删掉」是同一件事。 */
int dsh_file_remove(const char *path);

/** 清掉上一次原子写留下的临时文件（`<path>.tmp`）—— 会反复重写同一份文件的模块在打开时顺手清一次。
 * 原子写在能跑完的失败路径上都会自己删，但「写到一半被打断」（断电 / 被结束）留下的没人清；
 * 临时文件名是本层的约定，业务层不该知道它叫什么。
 * @return 0 成功（**包括「本来就没有临时文件」**）；非零只有一种情形：参数为空。 */
int dsh_file_cleanup_temp(const char *path);

/* ── 写字节计数（**只给测试用**，产品路径一次都不许读）─────────────────────
 * 为了让「写放大是 O(1)」这条能当场量出来：数**真的交给操作系统的字节数**，
 * 而不是看代码像不像追加 —— 「每记一条就把整份文件重写一遍」是这里最容易退化的性质。
 * ⚠️ 不线程安全（一个 int64 裸加）：只在单线程测试里用。 */
void dsh_file_stats_reset(void);

/** 自上次 `dsh_file_stats_reset` 以来，本层写出去的**内容字节数**（不含临时文件名等）*/
int64_t dsh_file_bytes_written(void);

/** 把两个路径拼起来（`dir` + 分隔符 + `name`）—— 分隔符由本层决定，
 * 业务代码里别出现 `a/b` 这种在本平台不成立的字面量。
 * @param out 出参：新分配的字符串（调用方 `dsh_release`）；@return 0 成功，非零（参数错 / 内存不足）。 */
int dsh_path_join(const char *dir, const char *name, char **out);

/** 从路径里取文件名（不含目录）。返回指向 `path` **内部**的指针（不分配、不许释放）。 */
const char *dsh_path_basename(const char *path);

/** 从路径里取扩展名（含点；没有扩展名则返回空串）。返回指向内部的指针。 */
const char *dsh_path_extension(const char *path);

/** 判断扩展名是不是 `.mdx`（大小写不敏感）。
 * 为什么要在导入**之前**判：接口定义里 `dsh_engine_dict_add` 收的是 `.mdx` 路径数组，
 * 用户拖进来一个 `.txt` 时要当场说清「这不是 .mdx」，别记进词库、等打开时才失败。 */
int dsh_path_is_mdx(const char *path);

/** 判断扩展名是不是 `.mdd`（大小写不敏感） */
int dsh_path_is_mdd(const char *path);

/** 按词典名找配套的资源卷（`.mdd`）：同目录下的 `<基名>.mdd`、`<基名>.1.mdd`、`<基名>.2.mdd`…
 * （**多卷从 1 开始**，`base.mdd` 是「第 0 卷」）。
 * @param out_paths 出参：新分配的**指针数组**（数组与每一项都由调用方 `dsh_release`）；@param out_count 找到几卷。
 * @return 0 成功（**找不到任何卷也算成功**：`*out_count = 0` 且 `*out_paths = NULL`）。 */
int dsh_path_find_mdd_volumes(const char *mdx_path, char ***out_paths, int64_t *out_count);

/* ── 同目录散放的文件的安全定位要用的这几件（见 dict/dsh_sibling.c）───────────────────
 * 它们都是**路径语义**，而路径语义各平台不一样（大小写敏不敏感、什么算分隔符、
 * 「符号链接 / 目录联接」怎么表示）—— 所以放平台层，业务层只拿它们做判断、自己不出现 `#ifdef`。 */

/** 把路径归一成绝对路径（解开 `.` 与 `..`、统一分隔符、去掉重复分隔符）。
 * ⚠️ 这是同目录散放的文件那道检查**必须**的前置：字符串前缀判断会把 `词典目录/../x.css`
 * 误判成「在目录内」，而归一之后它其实跑去别处了。
 * @param out 出参：新分配的字符串（调用方 `dsh_release`）；@return 0 成功，非零失败（写 last_error）。 */
int dsh_path_normalize(const char *path, char **out);

/** `path` 是不是**真的**落在 `dir` 里面（分隔符边界要对齐；大小写按本平台的规矩）。
 * ⚠️ 别在 POSIX 上写成大小写不敏感：那会让 `/home/x/TEST/a.css` 被当成 `/home/x/test` 里的文件，
 * 那是**一次真实的越权**，不是风格问题。 */
int dsh_path_is_inside(const char *dir, const char *path);

/** 这个路径上有没有「重解析点」（Windows 的符号链接 / 目录联接，POSIX 的符号链接）。
 * ⚠️ 为什么非要有它：`词典目录\link\win.ini` 里的 `link` 若指向别处，**字符串比对完全合法**、
 * 文件也真的存在 —— 但读出来的是系统文件。所以「在目录内」必须是**真实路径**意义上的。
 * 查不动（权限 / 竞态）时返回 **1（当作不安全）**：这条路上宁可少一个资源，也不能多读一个文件。 */
int dsh_file_has_reparse_point(const char *path);

/** 扫一遍目录里的条目名（不递归、不含 `.` 与 `..`）；`visit` 返回非零就**提前停下**
 * （调用方多半只想知道「有没有那一个」）。
 * 目录读不动（不存在 / 没权限）**不算错误**：返回 0 且一个都不回调（与参考实现同约定）。 */
int dsh_dir_scan(const char *dir, int (*visit)(const char *name, void *ctx), void *ctx);

#endif /* DSH_PLATFORM_FILE_H */
