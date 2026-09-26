/* 见 dict/dsh_sibling.h —— 白名单与四道检查。 */
#include "dict/dsh_sibling.h"

#include "dsh_internal.h"
#include "platform/dsh_file.h"

#include <stdio.h>
#include <string.h>

/* 允许从 `.mdx` 同目录散放读取的扩展名。**只放行 `.js`**：词典的交互（浮球 / 折叠 / 页签）
 * 就在脚本里，不放行时那些控件点了没反应。⚠️ `.mjs` / `.html` / `.htm` 与外域脚本仍不放行
 * （CSP 与 iframe 的 `sandbox` 一个字没动）；音频也不从这条路走，它归发音那条路。 */
static const char *const ALLOWED[] = {
    ".css", ".ttf",  ".otf", ".woff", ".woff2", ".eot", ".png",
    ".jpg", ".jpeg", ".gif", ".svg",  ".webp",  ".bmp", ".ico",
    ".avif", ".js",
};

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

static int eq_ci(const char *a, const char *b) {
  while (*a != '\0' && *b != '\0') {
    if (lower(*a) != lower(*b)) return 0;
    a++;
    b++;
  }
  return (*a == '\0' && *b == '\0') ? 1 : 0;
}

int dsh_sibling_is_allowed_extension(const char *path) {
  if (path == NULL || path[0] == '\0') return 0;
  const char *dot = NULL;
  for (const char *p = path; *p != '\0'; p++) {
    if (*p == '.') dot = p;
    if (*p == '/' || *p == '\\') dot = NULL;
  }
  if (dot == NULL || dot[1] == '\0') return 0;
  for (size_t i = 0; i < sizeof(ALLOWED) / sizeof(ALLOWED[0]); i++) {
    if (eq_ci(dot, ALLOWED[i])) return 1;
  }
  return 0;
}

char *dsh_sibling_directory_of(const char *mdx_path) {
  if (mdx_path == NULL || mdx_path[0] == '\0') return NULL;
  char *full = NULL;
  if (dsh_path_normalize(mdx_path, &full) != 0) return NULL;
  /* 掐掉最后一段（文件名） */
  size_t cut = strlen(full);
  while (cut > 0 && full[cut - 1] != '/' && full[cut - 1] != '\\') cut--;
  while (cut > 1 && (full[cut - 1] == '/' || full[cut - 1] == '\\')) cut--;
  if (cut == 0) {
    dsh_release(full);
    return NULL;
  }
  full[cut] = '\0';
  return full;
}

/** 白名单那一半的目录扫描回调：只要找到一个就停下 */
typedef struct {
  int found;
} any_ctx;

static int visit_any(const char *name, void *ctx) {
  any_ctx *c = (any_ctx *)ctx;
  if (dsh_sibling_is_allowed_extension(name)) {
    c->found = 1;
    return 1; /* 提前停 */
  }
  return 0;
}

int dsh_sibling_has_any(const char *mdx_path) {
  char *dir = dsh_sibling_directory_of(mdx_path);
  if (dir == NULL) return 0;
  any_ctx c;
  c.found = 0;
  dsh_dir_scan(dir, visit_any, &c);
  dsh_release(dir);
  return c.found;
}

/** 逐段查重解析点（词典目录自己以及它的祖先不参与 —— 用户完全可能把词典放在链接目录里）*/
static int touches_reparse_point(const char *dir, const char *path) {
  const size_t root = strlen(dir);
  if (strlen(path) <= root) return 1; /* 说不清就当不安全 */
  const size_t n = strlen(path);
  size_t i = root;
  while (i < n && (path[i] == '/' || path[i] == '\\')) i++;
  for (;;) {
    /* 取到下一段的末尾 */
    size_t end = i;
    while (end < n && path[end] != '/' && path[end] != '\\') end++;
    if (end == i) break;
    const size_t need = end + 1;
    char *segment = (char *)dsh_mem_alloc(need);
    if (segment == NULL) return 1; /* 内存不够 → 当作不安全（这条路宁少不多）*/
    memcpy(segment, path, end);
    segment[end] = '\0';
    const int bad = dsh_file_has_reparse_point(segment);
    dsh_release(segment);
    if (bad) return 1;
    if (end >= n) break;
    i = end;
    while (i < n && (path[i] == '/' || path[i] == '\\')) i++;
    if (i >= n) break;
  }
  return 0;
}

char *dsh_sibling_locate(const char *mdx_path, const char *resource_path) {
  char *dir = dsh_sibling_directory_of(mdx_path);
  if (dir == NULL || resource_path == NULL || resource_path[0] == '\0') {
    if (dir != NULL) dsh_release(dir);
    return NULL;
  }

  /* URL 解码（`%20` / `%E6%B5%8B` 这类；解不动就保留原样，与参考实现同约定） */
  char *decoded = NULL;
  {
    const size_t n = strlen(resource_path);
    char *buf = (char *)dsh_mem_alloc(n + 1);
    if (buf == NULL) {
      dsh_release(dir);
      return NULL;
    }
    size_t at = 0;
    for (size_t i = 0; i < n; i++) {
      if (resource_path[i] == '%' && i + 2 < n) {
        const int hi = resource_path[i + 1];
        const int lo = resource_path[i + 2];
        const int h = (hi >= '0' && hi <= '9')   ? hi - '0'
                      : (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10
                      : (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10
                                                 : -1;
        const int l = (lo >= '0' && lo <= '9')   ? lo - '0'
                      : (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10
                      : (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10
                                                 : -1;
        if (h >= 0 && l >= 0) {
          buf[at++] = (char)((h << 4) | l);
          i += 2;
          continue;
        }
      }
      buf[at++] = resource_path[i];
    }
    buf[at] = '\0';
    decoded = buf;
  }

  /* 查询串 / 锚点不属于路径（界面偶尔会带上，例如某些词典写了 `a.png?v=2`）*/
  for (char *p = decoded; *p != '\0'; p++) {
    if (*p == '?' || *p == '#') {
      *p = '\0';
      break;
    }
  }

  /* ① 扩展名白名单。⚠️ 只放行 `.js`，别把 `.mjs` / `.html` / `.htm` 顺手加回来。 */
  if (!dsh_sibling_is_allowed_extension(decoded)) {
    dsh_release(decoded);
    dsh_release(dir);
    return NULL;
  }

  /* 统一分隔符 */
  for (char *p = decoded; *p != '\0'; p++) {
    if (*p == '\\') *p = '/';
  }

  /* ② 拒绝「真的跑到别处去」的绝对路径：UNC（`//server/share/a.css`）与盘符（`C:/Windows/…`）。
   * ⚠️ 但**根相对**（`/images/a.png`）不能拒 —— 那是词典里的常见写法，`<base>` 把它解析到
   * 词典根，语义就是「词典目录下的 images/a.png」；当相对路径处理，安全性由 ③④ 兜住。 */
  if (decoded[0] == '/' && decoded[1] == '/') {
    dsh_release(decoded);
    dsh_release(dir);
    return NULL;
  }
  if (decoded[1] == ':') {
    dsh_release(decoded);
    dsh_release(dir);
    return NULL;
  }

  const char *relative = decoded;
  while (*relative == '/') relative++;
  if (*relative == '\0') {
    dsh_release(decoded);
    dsh_release(dir);
    return NULL;
  }

  char *joined = NULL;
  if (dsh_path_join(dir, relative, &joined) != 0) {
    dsh_release(decoded);
    dsh_release(dir);
    return NULL;
  }
  char *candidate = NULL;
  if (dsh_path_normalize(joined, &candidate) != 0) {
    dsh_release(joined);
    dsh_release(decoded);
    dsh_release(dir);
    return NULL;
  }
  dsh_release(joined);
  dsh_release(decoded);

  /* ③ 归一化后必须仍在词典目录内（`../../Windows/win.ini` 在这里被挡下） */
  if (!dsh_path_is_inside(dir, candidate)) {
    dsh_release(candidate);
    dsh_release(dir);
    dsh_set_last_error("同目录散放的文件跑到词典目录外面去了");
    return NULL;
  }

  /* ④ 真实路径兜底：逐段拒绝重解析点（符号链接 / 目录联接）*/
  if (touches_reparse_point(dir, candidate)) {
    dsh_release(candidate);
    dsh_release(dir);
    dsh_set_last_error("同目录散放的文件路径上有符号链接 / 目录联接");
    return NULL;
  }
  dsh_release(dir);

  dsh_file_info info;
  if (dsh_file_stat(candidate, &info) != 0 || !info.exists || !info.is_file) {
    dsh_release(candidate);
    return NULL;
  }
  return candidate;
}
