/* ⚠️ 这一行**必须在任何 include 之前**：`-std=c11` 下 glibc 默认只暴露 ISO C 的名字，
 * 而 `st_mtim`（纳秒精度）与 `fileno` 都是 POSIX 扩展 —— 少了它就报
 * 「struct stat 没有成员 st_mtim」。放在文件最顶上，别往下挪。 */
#define _POSIX_C_SOURCE 200809L

/* 见 dsh_file.h —— 文件元信息、原子写、路径工具的实现。 */
#include "platform/dsh_file.h"
#include "dsh_internal.h"

#include <stdio.h>
#include <string.h>

/* ── 写字节计数（**只给测试**；约定见 dsh_file.h 那一节）──────────────────── */
static int64_t g_bytes_written = 0;

void dsh_file_stats_reset(void) { g_bytes_written = 0; }

int64_t dsh_file_bytes_written(void) { return g_bytes_written; }

/* 临时文件路径 = `<path>` + `.tmp`。**不要带 pid**：带 pid 的话每次启动都留一个新的残骸、
 * 永远不会被下一次复用 / 覆盖；固定后缀的代价是同一个文件的两次原子写不能并发（本内核单线程）。 */
static char *temp_path_for(const char *path) {
  const size_t n = strlen(path);
  char *tmp = (char *)dsh_mem_alloc(n + 5);
  if (tmp == NULL) return NULL;
  memcpy(tmp, path, n);
  memcpy(tmp + n, ".tmp", 5);
  return tmp;
}

/* ── 路径编码：**UTF-8 → UTF-16，再走宽字符 API**（这一层存在的唯一理由）──────
 * Win32 的**窄字符** API（`fopen` / `GetFileAttributesExA` / `MoveFileExA` / `remove`）按
 * **进程的 ANSI 代码页**解这些字节，只有那台机器的代码页恰好是 UTF-8（65001）时才碰巧对 ——
 * 所以每个路径都先转 UTF-16 再用宽 API。POSIX 不需要这一层（路径本来就是字节串）。 */
#if defined(_WIN32)
#include <windows.h>

static wchar_t *path_to_wide(const char *path) {
  if (path == NULL) return NULL;
  const int need = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
  if (need <= 0) return NULL;
  wchar_t *wide = (wchar_t *)dsh_mem_alloc((size_t)need * sizeof(wchar_t));
  if (wide == NULL) return NULL;
  if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, need) <= 0) {
    dsh_release(wide);
    return NULL;
  }
  return wide;
}

static FILE *file_open_append(const char *path) {
  wchar_t *wide = path_to_wide(path);
  if (wide == NULL) return NULL;
  FILE *fp = _wfopen(wide, L"ab");
  dsh_release(wide);
  return fp;
}

/** UTF-16 → UTF-8（`path_to_wide` 的反方向）—— 目录扫描要把宽 API 拿到的文件名交回 UTF-8。
 * 分配走 `dsh_mem_alloc`，调用方 `dsh_release`。 */
static char *wide_to_path(const wchar_t *wide) {
  if (wide == NULL) return NULL;
  const int need = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
  if (need <= 0) return NULL;
  char *path = (char *)dsh_mem_alloc((size_t)need);
  if (path == NULL) return NULL;
  if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, path, need, NULL, NULL) <= 0) {
    dsh_release(path);
    return NULL;
  }
  return path;
}

static FILE *file_open_read(const char *path) {
  wchar_t *wide = path_to_wide(path);
  if (wide == NULL) return NULL;
  FILE *fp = _wfopen(wide, L"rb");
  dsh_release(wide);
  return fp;
}

static FILE *file_open_write(const char *path) {
  wchar_t *wide = path_to_wide(path);
  if (wide == NULL) return NULL;
  FILE *fp = _wfopen(wide, L"wb");
  dsh_release(wide);
  return fp;
}

static int file_remove_path(const char *path) {
  wchar_t *wide = path_to_wide(path);
  if (wide == NULL) return -1;
  const int rc = DeleteFileW(wide) ? 0 : -1;
  dsh_release(wide);
  return rc;
}
#else
static FILE *file_open_append(const char *path) { return fopen(path, "ab"); }
static FILE *file_open_read(const char *path) { return fopen(path, "rb"); }
static FILE *file_open_write(const char *path) { return fopen(path, "wb"); }
static int file_remove_path(const char *path) { return remove(path); }
#endif

/* ── 业务层要用「开文件读」时走这一个（路径编码见头文件那段）────────────────── */

FILE *dsh_file_open_read(const char *path) {
  if (path == NULL) {
    dsh_set_last_error("dsh_file_open_read：路径不能为空");
    return NULL;
  }
  return file_open_read(path);
}

/* ── 三件与平台无关的（只用 stdio），放在平台分支**之前**只写一份 ─────────── */

int dsh_file_append(const char *path, const void *data, size_t len) {
  if (path == NULL || (data == NULL && len > 0)) {
    dsh_set_last_error("dsh_file_append：参数不能为空");
    return -1;
  }
  FILE *fp = file_open_append(path);
  if (fp == NULL) {
    dsh_set_last_error("追加不进去：打不开 %s", path);
    return -1;
  }
  const size_t wrote = (len > 0) ? fwrite(data, 1, len, fp) : 0;
  g_bytes_written += (int64_t)wrote;
  fflush(fp); /* 见头文件那段：只 fflush 不 fsync —— 进程被杀之后数据还在 */
  fclose(fp);
  if (wrote != len) {
    dsh_set_last_error("追加不进去：只写了 %zu/%zu 字节（磁盘满？）", wrote, len);
    return -1;
  }
  return 0;
}

int dsh_file_remove(const char *path) {
  if (path == NULL) {
    dsh_set_last_error("dsh_file_remove：路径不能为空");
    return -1;
  }
  if (file_remove_path(path) != 0) {
    /* 区分「本来就不在」与「删不掉」：前者是成功。用 stat 问一句，别靠 errno 猜。 */
    dsh_file_info info;
    if (dsh_file_stat(path, &info) == 0 && !info.exists) return 0;
    dsh_set_last_error("删不掉：%s", path);
    return -1;
  }
  return 0;
}

int dsh_file_cleanup_temp(const char *path) {
  if (path == NULL) {
    dsh_set_last_error("dsh_file_cleanup_temp：路径不能为空");
    return -1;
  }
  char *tmp = temp_path_for(path);
  if (tmp == NULL) {
    dsh_set_last_error("内存不足：临时文件路径");
    return -1;
  }
  (void)file_remove_path(tmp); /* 在不在都算干净 —— 不该因为「没有残骸」而失败 */
  dsh_release(tmp);
  return 0;
}

#if defined(_WIN32)

#include <direct.h>
#include <io.h> /* `_get_osfhandle` / `_fileno`：FlushFileBuffers 要用 */
#include <windows.h>

#define DSH_PATH_SEP '\\'

int dsh_file_stat(const char *path, dsh_file_info *out) {
  if (path == NULL || out == NULL) {
    dsh_set_last_error("dsh_file_stat：参数不能为空");
    return -1;
  }
  memset(out, 0, sizeof(*out));
  WIN32_FILE_ATTRIBUTE_DATA attr;
  /* 宽字符路径（见文件上半部分那段） */
  wchar_t *wide = path_to_wide(path);
  if (wide == NULL) return 0; /* 路径都转不出来 = 当作不存在（调用方按「没有」处理） */
  const BOOL ok = GetFileAttributesExW(wide, GetFileExInfoStandard, &attr);
  dsh_release(wide);
  if (!ok) return 0; /* 不存在 */
  out->exists = 1;
  out->is_dir = (attr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
  out->is_file = out->is_dir ? 0 : 1;
  out->size = ((int64_t)attr.nFileSizeHigh << 32) | (int64_t)attr.nFileSizeLow;
  {
    /* FILETIME → Unix 毫秒（与 platform/dsh_time.c 同一个换算） */
    const uint64_t ticks =
        ((uint64_t)attr.ftLastWriteTime.dwHighDateTime << 32) | attr.ftLastWriteTime.dwLowDateTime;
    out->mtime = (int64_t)((ticks - 116444736000000000ull) / 10000ull);
  }
  return 0;
}

int dsh_file_write_atomic(const char *path, const void *data, size_t len) {
  if (path == NULL || data == NULL) {
    dsh_set_last_error("dsh_file_write_atomic：参数不能为空");
    return -1;
  }
  char *tmp = temp_path_for(path);
  if (tmp == NULL) {
    dsh_set_last_error("内存不足：临时文件路径");
    return -1;
  }

  FILE *fp = file_open_write(tmp);
  if (fp == NULL) {
    dsh_release(tmp);
    dsh_set_last_error("写不进去：打不开临时文件 %s", path);
    return -1;
  }
  const size_t wrote = (len > 0) ? fwrite(data, 1, len, fp) : 0;
  g_bytes_written += (int64_t)wrote;
  fflush(fp);
  /* `FlushFileBuffers`：rename 之前要让内容**真的**落到盘上，否则断电后可能得到
   * 「文件名是新的、内容是空的」—— 那正是原子写要避免的东西。两个平台各自都要 flush。 */
  {
    const intptr_t os_handle = _get_osfhandle(_fileno(fp));
    if (os_handle != -1) (void)FlushFileBuffers((HANDLE)os_handle);
  }
  fclose(fp);
  if (wrote != len) {
    (void)file_remove_path(tmp);
    dsh_release(tmp);
    dsh_set_last_error("写不进去：只写了 %zu/%zu 字节（磁盘满？）", wrote, len);
    return -1;
  }
  /* 同目录内 rename 是原子的（跨卷不是）—— 所以临时文件必须与目标同目录。
   * ⚠️ 用**宽字符**的 `MoveFileExW`：这条路上两个路径都可能是 UTF-8（见文件上半部分那段）。 */
  {
    wchar_t *wide_tmp = path_to_wide(tmp);
    wchar_t *wide_dst = path_to_wide(path);
    const BOOL moved = (wide_tmp != NULL && wide_dst != NULL)
                           ? MoveFileExW(wide_tmp, wide_dst, MOVEFILE_REPLACE_EXISTING)
                           : FALSE;
    if (wide_tmp != NULL) dsh_release(wide_tmp);
    if (wide_dst != NULL) dsh_release(wide_dst);
    if (!moved) {
      (void)file_remove_path(tmp);
      dsh_release(tmp);
      dsh_set_last_error("写不进去：替换 %s 失败（文件被占用？）", path);
      return -1;
    }
  }
  dsh_release(tmp);
  return 0;
}

#elif defined(__unix__) || defined(__APPLE__)

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define DSH_PATH_SEP '/'

int dsh_file_stat(const char *path, dsh_file_info *out) {
  if (path == NULL || out == NULL) {
    dsh_set_last_error("dsh_file_stat：参数不能为空");
    return -1;
  }
  memset(out, 0, sizeof(*out));
  struct stat st;
  if (stat(path, &st) != 0) return 0; /* 不存在 */
  out->exists = 1;
  out->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
  out->is_file = S_ISREG(st.st_mode) ? 1 : 0;
  out->size = (int64_t)st.st_size;
  out->mtime = (int64_t)st.st_mtime * 1000 + (int64_t)(st.st_mtim.tv_nsec / 1000000);
  return 0;
}

int dsh_file_write_atomic(const char *path, const void *data, size_t len) {
  if (path == NULL || data == NULL) {
    dsh_set_last_error("dsh_file_write_atomic：参数不能为空");
    return -1;
  }
  char *tmp = temp_path_for(path);
  if (tmp == NULL) {
    dsh_set_last_error("内存不足：临时文件路径");
    return -1;
  }

  FILE *fp = file_open_write(tmp);
  if (fp == NULL) {
    dsh_release(tmp);
    dsh_set_last_error("写不进去：打不开临时文件 %s", path);
    return -1;
  }
  const size_t wrote = (len > 0) ? fwrite(data, 1, len, fp) : 0;
  g_bytes_written += (int64_t)wrote;
  fflush(fp);
  /* fsync：rename 之前要让内容**真的**落到盘上，否则断电后可能得到
   * 「文件名是新的、内容是空的」—— 那正是原子写要避免的东西。 */
  {
    const int fd = fileno(fp);
    if (fd >= 0) fsync(fd);
  }
  fclose(fp);
  if (wrote != len) {
    (void)file_remove_path(tmp);
    dsh_release(tmp);
    dsh_set_last_error("写不进去：只写了 %zu/%zu 字节（磁盘满？）", wrote, len);
    return -1;
  }
  if (rename(tmp, path) != 0) {
    (void)file_remove_path(tmp);
    dsh_release(tmp);
    dsh_set_last_error("写不进去：替换 %s 失败", path);
    return -1;
  }
  dsh_release(tmp);
  return 0;
}

#else

#define DSH_PATH_SEP '/'

int dsh_file_stat(const char *path, dsh_file_info *out) {
  (void)path;
  if (out == NULL) return -1;
  memset(out, 0, sizeof(*out));
  return -1; /* 这个平台没有实现 */
}

int dsh_file_write_atomic(const char *path, const void *data, size_t len) {
  (void)path;
  (void)data;
  (void)len;
  dsh_set_last_error("这个平台还没有实现原子写文件");
  return -1;
}

#endif

/* ── 路径工具（与平台无关的那部分）──────────────────────────────────────── */

const char *dsh_path_basename(const char *path) {
  if (path == NULL) return "";
  const char *a = strrchr(path, '/');
  const char *b = strrchr(path, '\\');
  const char *last = (a > b) ? a : b;
  return (last == NULL) ? path : last + 1;
}

const char *dsh_path_extension(const char *path) {
  const char *base = dsh_path_basename(path);
  const char *dot = strrchr(base, '.');
  return (dot == NULL) ? "" : dot;
}

/** 扩展名比对（大小写不敏感） */
static int ext_equals(const char *path, const char *want) {
  const char *ext = dsh_path_extension(path);
  const size_t n = strlen(want);
  if (strlen(ext) != n) return 0;
  for (size_t i = 0; i < n; i++) {
    char c = ext[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (c != want[i]) return 0;
  }
  return 1;
}

int dsh_path_is_mdx(const char *path) { return ext_equals(path, ".mdx"); }
int dsh_path_is_mdd(const char *path) { return ext_equals(path, ".mdd"); }

int dsh_path_join(const char *dir, const char *name, char **out) {
  if (out == NULL) {
    dsh_set_last_error("dsh_path_join：out 不能为空");
    return -1;
  }
  *out = NULL;
  if (name == NULL) {
    dsh_set_last_error("dsh_path_join：name 不能为空");
    return -1;
  }
  if (dir == NULL || dir[0] == '\0') {
    *out = dsh_mem_strdup(name);
    return (*out == NULL) ? -1 : 0;
  }
  const size_t d = strlen(dir);
  const size_t n = strlen(name);
  const int need_sep = (dir[d - 1] != '/' && dir[d - 1] != '\\');
  char *p = (char *)dsh_mem_alloc(d + (need_sep ? 1 : 0) + n + 1);
  if (p == NULL) {
    dsh_set_last_error("内存不足：路径");
    return -1;
  }
  memcpy(p, dir, d);
  size_t k = d;
  if (need_sep) p[k++] = DSH_PATH_SEP;
  memcpy(p + k, name, n);
  p[k + n] = '\0';
  *out = p;
  return 0;
}

/* 按词典名找配套资源卷：`<基名>.mdd` 是第 0 卷，`<基名>.1.mdd` / `<基名>.2.mdd`… 是后续卷。
 * ⚠️ 从 0 卷开始一卷一卷试、遇到第一个不存在的就停（**卷号必须连续**）——
 * 否则「缺第 3 卷」会让第 4 卷被认成第 1 卷。 */
int dsh_path_find_mdd_volumes(const char *mdx_path, char ***out_paths, int64_t *out_count) {
  if (out_paths == NULL || out_count == NULL) {
    dsh_set_last_error("dsh_path_find_mdd_volumes：出参不能为空");
    return -1;
  }
  *out_paths = NULL;
  *out_count = 0;
  if (mdx_path == NULL || mdx_path[0] == '\0') return 0;

  /* 目录 + 基名（去掉 .mdx 扩展名） */
  const size_t len = strlen(mdx_path);
  char *dir = (char *)dsh_mem_alloc(len + 1);
  if (dir == NULL) return -1;
  memcpy(dir, mdx_path, len + 1);
  char *slash = strrchr(dir, '/');
  char *bslash = strrchr(dir, '\\');
  char *sep = (slash > bslash) ? slash : bslash;
  if (sep != NULL) *sep = '\0';
  else dir[0] = '\0';

  const char *base = dsh_path_basename(mdx_path);
  const char *dot = strrchr(base, '.');
  const size_t stem_len = (dot != NULL) ? (size_t)(dot - base) : strlen(base);
  char *stem = (char *)dsh_mem_alloc(stem_len + 1);
  if (stem == NULL) {
    dsh_release(dir);
    return -1;
  }
  memcpy(stem, base, stem_len);
  stem[stem_len] = '\0';

  char **found = (char **)dsh_mem_alloc(64 * sizeof(char *));
  if (found == NULL) {
    dsh_release(stem);
    dsh_release(dir);
    dsh_set_last_error("内存不足：资源卷列表");
    return -1;
  }
  int64_t count = 0;

  for (int volume = 0; volume < 64; volume++) {
    char name[512];
    if (volume == 0) {
      snprintf(name, sizeof(name), "%s.mdd", stem);
    } else {
      snprintf(name, sizeof(name), "%s.%d.mdd", stem, volume);
    }
    char *full = NULL;
    if (dsh_path_join(dir, name, &full) != 0) break;
    dsh_file_info info;
    if (dsh_file_stat(full, &info) == 0 && info.exists && info.is_file) {
      found[count++] = full;
    } else {
      dsh_release(full);
      if (volume == 0) continue; /* 没有第 0 卷不奇怪（不带 .mdd 的词典本来就没有） */
      break;                     /* 卷号必须连续：缺一卷就到此为止 */
    }
  }

  dsh_release(stem);
  dsh_release(dir);
  if (count == 0) {
    dsh_release(found);
    return 0; /* 一卷都没有 = 合法（不带 .mdd 的词典） */
  }
  *out_paths = found;
  *out_count = count;
  return 0;
}

/* ── 同目录散放的文件的安全定位要用的这几件（见 dsh_file.h）───────────────────────── */

int dsh_path_normalize(const char *path, char **out) {
  if (out == NULL) {
    dsh_set_last_error("dsh_path_normalize：out 不能为空");
    return -1;
  }
  *out = NULL;
  if (path == NULL || path[0] == '\0') {
    dsh_set_last_error("dsh_path_normalize：路径为空");
    return -1;
  }

  /* 相对路径先接上当前目录（与参考实现的 Path.GetFullPath 同语义）*/
  char *absolute = NULL;
  const int is_absolute =
#if defined(_WIN32)
      (path[0] == '\\' || path[0] == '/') ||
      (path[0] != '\0' && path[1] == ':');
#else
      (path[0] == '/');
#endif
  if (is_absolute) {
    absolute = dsh_mem_strdup(path);
  } else {
    char cwd[4096];
#if defined(_WIN32)
    if (_getcwd(cwd, (int)sizeof(cwd)) == NULL) cwd[0] = '\0';
#else
    if (getcwd(cwd, sizeof(cwd)) == NULL) cwd[0] = '\0';
#endif
    if (cwd[0] == '\0') {
      dsh_set_last_error("取不到当前目录");
      return -1;
    }
    const size_t need = strlen(cwd) + strlen(path) + 2;
    absolute = (char *)dsh_mem_alloc(need);
    if (absolute == NULL) {
      dsh_set_last_error("内存不足：路径归一");
      return -1;
    }
    snprintf(absolute, need, "%s%c%s", cwd,
#if defined(_WIN32)
             '\\',
#else
             '/',
#endif
             path);
  }
  if (absolute == NULL) {
    dsh_set_last_error("内存不足：路径副本");
    return -1;
  }

  /* 逐段解开 `.` 与 `..`：**纯文本**地解、与 GetFullPath 一样不碰文件系统。
   * `\` 与 `/` 都当分隔符 —— 内核在两个平台上都要跑，两边都认才不至于
   * 同一份设置在两平台上读出的东西不同。 */
  const size_t n = strlen(absolute);
  size_t *starts = (size_t *)dsh_mem_alloc((n + 1) * sizeof(size_t));
  size_t *lens = (size_t *)dsh_mem_alloc((n + 1) * sizeof(size_t));
  char *result = (char *)dsh_mem_alloc(n + 2);
  if (starts == NULL || lens == NULL || result == NULL) {
    if (starts != NULL) dsh_release(starts);
    if (lens != NULL) dsh_release(lens);
    if (result != NULL) dsh_release(result);
    dsh_release(absolute);
    dsh_set_last_error("内存不足：路径归一");
    return -1;
  }

  size_t count = 0;
  int rooted = 0;
#if defined(_WIN32)
  char drive[4] = {0};
  if (n >= 2 && absolute[1] == ':') {
    drive[0] = absolute[0];
    drive[1] = ':';
    rooted = 1;
  }
#endif
  size_t i = 0;
#if defined(_WIN32)
  if (rooted) i = 2;
#endif
  if (i < n && (absolute[i] == '/' || absolute[i] == '\\')) {
    rooted = 1;
    i++;
    while (i < n && (absolute[i] == '/' || absolute[i] == '\\')) i++;
  }
  while (i < n) {
    const size_t begin = i;
    while (i < n && absolute[i] != '/' && absolute[i] != '\\') i++;
    const size_t len = i - begin;
    if (len == 1 && absolute[begin] == '.') {
      /* 当前目录：丢掉 */
    } else if (len == 2 && absolute[begin] == '.' && absolute[begin + 1] == '.') {
      if (count > 0) count--; /* 回到上一段；已经在根上就当没写 */
    } else if (len > 0) {
      starts[count] = begin;
      lens[count] = len;
      count++;
    }
    while (i < n && (absolute[i] == '/' || absolute[i] == '\\')) i++;
  }

  size_t at = 0;
#if defined(_WIN32)
  if (drive[0] != '\0') {
    result[at++] = drive[0];
    result[at++] = drive[1];
    result[at++] = '\\';
  } else
#endif
      if (rooted) {
    result[at++] = '/';
  }
  for (size_t s = 0; s < count; s++) {
    if (s > 0) {
#if defined(_WIN32)
      result[at++] = '\\';
#else
      result[at++] = '/';
#endif
    }
    memcpy(result + at, absolute + starts[s], lens[s]);
    at += lens[s];
  }
  result[at] = '\0';

  dsh_release(starts);
  dsh_release(lens);
  dsh_release(absolute);

  if (at == 0) {
    dsh_release(result);
    dsh_set_last_error("归一之后是个空路径");
    return -1;
  }
  *out = result;
  return 0;
}

/** 这个字符算不算路径分隔符（Windows 认两个，POSIX 认一个） */
static int is_sep(char c) {
#if defined(_WIN32)
  return (c == '/' || c == '\\') ? 1 : 0;
#else
  return (c == '/') ? 1 : 0;
#endif
}

static char fold(char c) {
#if defined(_WIN32)
  /* Windows 路径不区分大小写（与参考实现的 OrdinalIgnoreCase 同约定）*/
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
#else
  return c; /* POSIX 区分大小写 —— 写死不敏感会变成一次真实的越权 */
#endif
}

int dsh_path_is_inside(const char *dir, const char *path) {
  if (dir == NULL || path == NULL) return 0;
  size_t root = strlen(dir);
  while (root > 0 && is_sep(dir[root - 1])) root--; /* 末尾的分隔符不算 */
  if (root == 0) return 0;
  const size_t len = strlen(path);
  if (len <= root) return 0;
  for (size_t i = 0; i < root; i++) {
    if (fold(dir[i]) != fold(path[i])) return 0;
  }
  /* 边界必须对齐：否则 `C:\词典2` 会被当成「在 `C:\词典` 里面」 */
  return is_sep(path[root]) ? 1 : 0;
}

int dsh_file_has_reparse_point(const char *path) {
  if (path == NULL || path[0] == '\0') return 1; /* 查不动就当不安全 */
#if defined(_WIN32)
  /* 宽字符路径（见文件上半部分那段） */
  wchar_t *wide = path_to_wide(path);
  if (wide == NULL) return 1;
  const DWORD attrs = GetFileAttributesW(wide);
  dsh_release(wide);
  if (attrs == INVALID_FILE_ATTRIBUTES) return 1; /* 读不到属性 → 当作不安全 */
  return (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ? 1 : 0;
#else
  struct stat st;
  /* lstat：**不跟随**最后那一段的符号链接，否则永远看不出它是链接 */
  if (lstat(path, &st) != 0) return 1;
  return S_ISLNK(st.st_mode) ? 1 : 0;
#endif
}

int dsh_dir_scan(const char *dir, int (*visit)(const char *name, void *ctx), void *ctx) {
  if (dir == NULL || visit == NULL) {
    dsh_set_last_error("dsh_dir_scan：参数不能为空");
    return -1;
  }
#if defined(_WIN32)
  /* ★ 走**宽字符**那一套：`FindFirstFileA` 按**进程的 ANSI 代码页**解 pattern 里的 UTF-8 字节，
   * 代码页不是 65001 的机器上 `C:\…\词典\*` 解出来是乱码 —— 目录读不动 → 当空 →
   * `dsh_sibling_has_any()` 报「这本词典没有资源」，而资源其实取得到。
   * 本机代码页就是 65001，所以本地永远照不出来，只有 那条源码边界检查盯着它。 */
  wchar_t *wide_dir = path_to_wide(dir);
  if (wide_dir == NULL) {
    dsh_set_last_error("dsh_dir_scan：路径转不成宽字符（%s）", dir);
    return -1;
  }
  const size_t wide_len = wcslen(wide_dir);
  wchar_t *pattern = (wchar_t *)dsh_mem_alloc((wide_len + 3) * sizeof(wchar_t));
  if (pattern == NULL) {
    dsh_release(wide_dir);
    dsh_set_last_error("内存不足：目录扫描");
    return -1;
  }
  memcpy(pattern, wide_dir, wide_len * sizeof(wchar_t));
  /* 末尾是 `\` 就不要再来一个（`C:\dir\` + `\*` 也认得，但少一个更干净）*/
  if (wide_len > 0 && (wide_dir[wide_len - 1] == L'\\' || wide_dir[wide_len - 1] == L'/')) {
    pattern[wide_len] = L'*';
    pattern[wide_len + 1] = L'\0';
  } else {
    pattern[wide_len] = L'\\';
    pattern[wide_len + 1] = L'*';
    pattern[wide_len + 2] = L'\0';
  }
  dsh_release(wide_dir);

  WIN32_FIND_DATAW data;
  HANDLE h = FindFirstFileW(pattern, &data);
  dsh_release(pattern);
  if (h == INVALID_HANDLE_VALUE) return 0; /* 目录读不动 = 当空（与参考实现同约定）*/
  do {
    if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
    /* 名字交回 UTF-8（回调与上层全程只用 UTF-8）；转不出来就跳过这一项 */
    char *name = wide_to_path(data.cFileName);
    if (name == NULL) continue;
    const int stop = visit(name, ctx);
    dsh_release(name);
    if (stop != 0) break;
  } while (FindNextFileW(h, &data));
  FindClose(h);
  return 0;
#else
  DIR *handle = opendir(dir);
  if (handle == NULL) return 0; /* 目录读不动 = 当空 */
  struct dirent *entry;
  while ((entry = readdir(handle)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
    if (visit(entry->d_name, ctx) != 0) break;
  }
  closedir(handle);
  return 0;
#endif
}
