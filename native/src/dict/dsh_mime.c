/* 见 dict/dsh_mime.h —— 表与参考实现同一条约定（大小写不敏感）。 */
#include "dict/dsh_mime.h"

#include <string.h>

typedef struct {
  const char *ext;
  const char *mime;
} mime_entry;

static const mime_entry MIME_TABLE[] = {
    {".css", "text/css; charset=utf-8"},
    {".js", "text/javascript; charset=utf-8"},
    {".mjs", "text/javascript; charset=utf-8"},
    {".json", "application/json; charset=utf-8"},
    {".html", "text/html; charset=utf-8"},
    {".htm", "text/html; charset=utf-8"},
    {".xml", "application/xml; charset=utf-8"},
    {".txt", "text/plain; charset=utf-8"},
    {".svg", "image/svg+xml"},
    {".png", "image/png"},
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif", "image/gif"},
    {".webp", "image/webp"},
    {".bmp", "image/bmp"},
    {".ico", "image/x-icon"},
    {".avif", "image/avif"},
    {".mp3", "audio/mpeg"},
    {".m4a", "audio/mp4"},
    {".aac", "audio/aac"},
    {".wav", "audio/wav"},
    {".ogg", "audio/ogg"},
    {".oga", "audio/ogg"},
    {".opus", "audio/ogg"},
    {".flac", "audio/flac"},
    {".spx", "audio/x-speex"},
    {".woff", "font/woff"},
    {".woff2", "font/woff2"},
    {".ttf", "font/ttf"},
    {".otf", "font/otf"},
    {".eot", "application/vnd.ms-fontobject"},
};

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

/** 扩展名相等（大小写不敏感） */
static int ext_eq(const char *a, const char *b) {
  while (*a != '\0' && *b != '\0') {
    if (lower(*a) != lower(*b)) return 0;
    a++;
    b++;
  }
  return (*a == '\0' && *b == '\0') ? 1 : 0;
}

const char *dsh_mime_for(const char *path) {
  if (path == NULL) return "application/octet-stream";
  /* 取**最后一个**点之后那一段（与参考实现同一条约定） */
  const char *dot = NULL;
  for (const char *p = path; *p != '\0'; p++) {
    if (*p == '.') dot = p;
    /* 分隔符之后要把「点」忘掉：只在最后一段里找点 —— 参考实现不看分隔符，
     * `dir.d/a` 这种结果不一样，这里按路径语义办。 */
    if (*p == '/' || *p == '\\') dot = NULL;
  }
  if (dot == NULL || dot[1] == '\0') return "application/octet-stream";
  for (size_t i = 0; i < sizeof(MIME_TABLE) / sizeof(MIME_TABLE[0]); i++) {
    if (ext_eq(dot, MIME_TABLE[i].ext)) return MIME_TABLE[i].mime;
  }
  return "application/octet-stream";
}
