/* 见 dsh_mapfile.h —— 两个平台各一份实现，业务代码一行 #ifdef 都不写。 */
#include "platform/dsh_mapfile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

#include <windows.h>

/* UTF-8 → UTF-16。⚠️ 这里用 malloc/free 而不是内核那套 dsh_mem_*：本文件是平台原语，
 * 连 dsh_internal.h 都不 include（struct dsh_map 自己也是 malloc 的），
 * 引内核的分配器只会给这一层添一条没必要的依赖。 */
static wchar_t *mf_path_to_wide(const char *path) {
  if (path == NULL) return NULL;
  const int need = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
  if (need <= 0) return NULL;
  wchar_t *wide = (wchar_t *)malloc((size_t)need * sizeof(wchar_t));
  if (wide == NULL) return NULL;
  if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, need) <= 0) {
    free(wide);
    return NULL;
  }
  return wide;
}

static void mf_release_wide(wchar_t *wide) {
  if (wide != NULL) free(wide);
}

struct dsh_map {
  HANDLE file;
  HANDLE mapping;
  const uint8_t *base;
  int64_t len;
};

int dsh_map_open(const char *path, dsh_map **out, int64_t *out_len) {
  if (path == NULL || out == NULL || out_len == NULL) return -1;
  *out = NULL;
  *out_len = 0;

  /* ⚠️ 必须走宽字符：内核收进来的是 UTF-8，而 CreateFileA 按**进程的 ANSI 代码页**解它，
   * 只有那台机器的代码页恰好是 UTF-8（65001）时才碰巧对。.mdx 放在中文目录里而代码页
   * 不是 UTF-8 时这本词典会读不动（表现为「导入成功但打不开」）。 */
  wchar_t *wide = mf_path_to_wide(path);
  if (wide == NULL) return -1;
  HANDLE file = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE) {
    mf_release_wide(wide);
    return -1;
  }
  mf_release_wide(wide);

  LARGE_INTEGER size;
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0) {
    CloseHandle(file);
    return -1;
  }

  HANDLE mapping = CreateFileMappingA(file, NULL, PAGE_READONLY, 0, 0, NULL);
  if (mapping == NULL) {
    CloseHandle(file);
    return -1;
  }
  const void *base = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
  if (base == NULL) {
    CloseHandle(mapping);
    CloseHandle(file);
    return -1;
  }

  struct dsh_map *m = (struct dsh_map *)malloc(sizeof(*m));
  if (m == NULL) {
    UnmapViewOfFile(base);
    CloseHandle(mapping);
    CloseHandle(file);
    return -1;
  }
  m->file = file;
  m->mapping = mapping;
  m->base = (const uint8_t *)base;
  m->len = (int64_t)size.QuadPart;
  *out = m;
  *out_len = m->len;
  return 0;
}

void dsh_map_close(dsh_map *map) {
  if (map == NULL) return;
  if (map->base != NULL) UnmapViewOfFile(map->base);
  if (map->mapping != NULL) CloseHandle(map->mapping);
  if (map->file != NULL && map->file != INVALID_HANDLE_VALUE) CloseHandle(map->file);
  free(map);
}

#elif defined(__unix__) || defined(__APPLE__)

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct dsh_map {
  int fd;
  const uint8_t *base;
  int64_t len;
};

int dsh_map_open(const char *path, dsh_map **out, int64_t *out_len) {
  if (path == NULL || out == NULL || out_len == NULL) return -1;
  *out = NULL;
  *out_len = 0;

  const int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;

  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size <= 0) {
    close(fd);
    return -1;
  }

  void *base = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (base == MAP_FAILED) {
    close(fd);
    return -1;
  }

  struct dsh_map *m = (struct dsh_map *)malloc(sizeof(*m));
  if (m == NULL) {
    munmap(base, (size_t)st.st_size);
    close(fd);
    return -1;
  }
  m->fd = fd;
  m->base = (const uint8_t *)base;
  m->len = (int64_t)st.st_size;
  *out = m;
  *out_len = m->len;
  return 0;
}

void dsh_map_close(dsh_map *map) {
  if (map == NULL) return;
  if (map->base != NULL) munmap((void *)map->base, (size_t)map->len);
  if (map->fd >= 0) close(map->fd);
  free(map);
}

#else

/* 其他平台：如实报「没有映射」（返回 -1），让调用方走逐段读那条路 ——
 * 那条路在所有平台上都要留着。 */
struct dsh_map {
  int unused;
};

int dsh_map_open(const char *path, dsh_map **out, int64_t *out_len) {
  (void)path;
  (void)out;
  (void)out_len;
  return -1;
}

void dsh_map_close(dsh_map *map) { (void)map; }

#endif

const uint8_t *dsh_map_base(const dsh_map *map) {
  return (map == NULL) ? NULL : map->base;
}
