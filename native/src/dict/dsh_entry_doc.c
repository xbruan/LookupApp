/* 见 dict/dsh_entry_doc.h —— 文档拼装与音频键抠取。 */
#include "dict/dsh_entry_doc.h"
#include "text/dsh_textutil.h"

#include "audio/dsh_audio.h"
#include "dict/entry_assets.h"
/* 有意追加的那一段（与上面那份自动生成的文件分开放，见该文件顶上那段） */
#include "dict/entry_assets_extra.h"
#include "dsh_internal.h"

#include <stdio.h>
#include <string.h>

/** 资源域后缀：`.invalid` 是 RFC 2606 保留的、**永不解析**的顶级域（参考实现同一条） */
#define DSH_ENTRY_RESOURCE_DOMAIN "dictres.invalid"

/** 词条正文文档的路径（放在 host 上，所以资源路径不会与它冲突） */
#define DSH_ENTRY_PATH "/__entry__"

/* ── 小工具 ─────────────────────────────────────────────────────────────── */

typedef struct {
  char *p;
  size_t len;
  size_t cap;
} sbuf;

static int sbuf_add(sbuf *b, const char *bytes, size_t n) {
  if (n == 0) return 1;
  const size_t need = b->len + n + 1;
  if (need > b->cap) {
    size_t cap = (b->cap == 0) ? 256 : b->cap;
    while (cap < need) cap *= 2;
    char *next = (char *)dsh_mem_alloc(cap);
    if (next == NULL) {
      dsh_set_last_error("内存不足：词条正文缓冲");
      return 0;
    }
    if (b->len > 0) memcpy(next, b->p, b->len);
    if (b->p != NULL) dsh_release(b->p);
    b->p = next;
    b->cap = cap;
  }
  memcpy(b->p + b->len, bytes, n);
  b->len += n;
  b->p[b->len] = '\0';
  return 1;
}

static int sbuf_str(sbuf *b, const char *text) {
  return sbuf_add(b, text, text != NULL ? strlen(text) : 0);
}

static char *sbuf_take(sbuf *b, size_t *out_len) {
  if (b->p == NULL && !sbuf_add(b, "", 0)) return NULL;
  if (out_len != NULL) *out_len = b->len;
  char *p = b->p;
  b->p = NULL;
  b->len = 0;
  b->cap = 0;
  return p;
}

/* ⚠️ 转义只有一份实现（`text/dsh_textutil.c` 的 `dsh_text_uri_escape`）；
 * 内存不足的人话仍由**本文件**设 —— 合并只收编算法，不收编各处的说法。 */

/* ── 三条 URL ───────────────────────────────────────────────────────────── */

char *dsh_entry_doc_origin(const char *dict_id) {
  if (dict_id == NULL) dict_id = "";
  /* ⚠️ `+ 2`：`id` 与域之间那个 `.`，以及收尾的 `\0`。少写 1 会让 `snprintf`
   *    **不报错地截掉最后一个字符**（`...dictres.invali`），每次查词少一个字节。 */
  const size_t need =
      strlen("https://") + strlen(dict_id) + 1 + strlen(DSH_ENTRY_RESOURCE_DOMAIN) + 1;
  char *out = (char *)dsh_mem_alloc(need);
  if (out == NULL) {
    dsh_set_last_error("内存不足：词条域");
    return NULL;
  }
  snprintf(out, need, "https://%s.%s", dict_id, DSH_ENTRY_RESOURCE_DOMAIN);
  return out;
}

char *dsh_entry_doc_base(const char *dict_id) {
  char *origin = dsh_entry_doc_origin(dict_id);
  if (origin == NULL) return NULL;
  const size_t need = strlen(origin) + 2;
  char *out = (char *)dsh_mem_alloc(need);
  if (out == NULL) {
    dsh_release(origin);
    dsh_set_last_error("内存不足：词条基准地址");
    return NULL;
  }
  snprintf(out, need, "%s/", origin);
  dsh_release(origin);
  return out;
}

char *dsh_entry_doc_entry_url(const char *dict_id, const char *word) {
  char *origin = dsh_entry_doc_origin(dict_id);
  if (origin == NULL) return NULL;
  char *escaped = dsh_text_uri_escape(word);
  if (escaped == NULL) {
    dsh_set_last_error("内存不足：转义词条名");
    dsh_release(origin);
    return NULL;
  }
  const size_t need = strlen(origin) + strlen(DSH_ENTRY_PATH) + strlen("?word=") +
                      strlen(escaped) + 1;
  char *out = (char *)dsh_mem_alloc(need);
  if (out == NULL) {
    dsh_release(origin);
    dsh_release(escaped);
    dsh_set_last_error("内存不足：词条地址");
    return NULL;
  }
  snprintf(out, need, "%s%s?word=%s", origin, DSH_ENTRY_PATH, escaped);
  dsh_release(origin);
  dsh_release(escaped);
  return out;
}

void dsh_entry_doc_audio_keys_free(char **keys, int64_t count) {
  if (keys == NULL) return;
  for (int64_t i = 0; i < count; i++) {
    if (keys[i] != NULL) dsh_release(keys[i]);
  }
  dsh_release(keys);
}

char *dsh_entry_doc_uri_escape(const char *text) {
  char *escaped = dsh_text_uri_escape(text);
  if (escaped == NULL) dsh_set_last_error("内存不足：转义词条名");
  return escaped;
}

/* ── 文档拼装 ───────────────────────────────────────────────────────────── */

/* CSP：只放行词典资源域与内联，杜绝词条把请求发到公网。
 * ⚠️ iframe 的 `sandbox` 只给 `allow-scripts`、**不给** `allow-same-origin`，拿到的是
 *    opaque origin，所以必须**显式列出资源域**（不能指望 `'self'`）。改了就是放宽一次
 *    信任边界 —— 那必须是有意识的决定，不是顺手。 */
static const char CSP[] =
    "default-src 'none'; "
    "img-src https://*." DSH_ENTRY_RESOURCE_DOMAIN " data: blob:; "
    "media-src https://*." DSH_ENTRY_RESOURCE_DOMAIN " data: blob:; "
    "style-src 'unsafe-inline' https://*." DSH_ENTRY_RESOURCE_DOMAIN "; "
    "font-src https://*." DSH_ENTRY_RESOURCE_DOMAIN " data:; "
    "script-src 'unsafe-inline' https://*." DSH_ENTRY_RESOURCE_DOMAIN "; "
    "connect-src 'none'; frame-src 'none'; object-src 'none'; form-action 'none'; "
    "base-uri https://*." DSH_ENTRY_RESOURCE_DOMAIN ";";

char *dsh_entry_doc_build(const char *dict_id, const char *definition, size_t definition_len,
                          const char *notice, int has_resources, size_t *out_len) {
  if (out_len != NULL) *out_len = 0;
  if (dict_id == NULL) dict_id = "";

  char *base = dsh_entry_doc_base(dict_id);
  if (base == NULL) return NULL;

  sbuf b;
  memset(&b, 0, sizeof(b));
  int ok = 1;
  /* ⚠️ 这一段拼接逐段照抄参考实现（连换行都不能少）：它是标准答案文件逐字节对照的对象。
   *    任何「顺手改一下排版」都会让词条页与参考实现不再一致。 */
  ok = ok && sbuf_str(&b, "<!doctype html>\n<html lang=\"zh-CN\">\n<head>\n<meta charset=\"utf-8\">\n");
  ok = ok && sbuf_str(&b, "<meta http-equiv=\"Content-Security-Policy\" content=\"");
  ok = ok && sbuf_str(&b, CSP);
  ok = ok && sbuf_str(&b, "\">\n<base href=\"");
  ok = ok && sbuf_str(&b, base);
  ok = ok && sbuf_str(&b, "\">\n<style>");
  ok = ok && sbuf_str(&b, DSH_ENTRY_BASE_STYLE);
  ok = ok && sbuf_str(&b, "</style>\n</head>\n<body class=\"lookup-entry\" data-has-resources=\"");
  ok = ok && sbuf_str(&b, has_resources ? "1" : "0");
  ok = ok && sbuf_str(&b, "\">\n");
  /* 提示非空时**顶替**正文（与参考实现同一条：那是「没查到」的提示页） */
  if (notice != NULL && notice[0] != '\0') {
    ok = ok && sbuf_str(&b, "<div class=\"lookup-notice\">");
    ok = ok && sbuf_str(&b, notice);
    ok = ok && sbuf_str(&b, "</div>");
  } else {
    ok = ok && sbuf_add(&b, definition != NULL ? definition : "", definition_len);
  }
  ok = ok && sbuf_str(&b, "\n<div class=\"lookup-chips\" id=\"lookupChips\" hidden></div>\n");
  ok = ok && sbuf_str(&b, "<script>");
  ok = ok && sbuf_str(&b, DSH_ENTRY_BRIDGE_SCRIPT);
  /*
   * ⚠️ 参考实现那份脚本到此为止，**一个字都不改**（标准答案文件按它核 SHA-256）。
   *    0.2.0 的增量另起一个 `<script>`：它管「点了却没反应」那三条路要说一句话
   *    （见 `entry_assets_extra.h` 顶上那段）。
   */
  ok = ok && sbuf_str(&b, DSH_ENTRY_EXTRA_OPEN);
  ok = ok && sbuf_str(&b, DSH_ENTRY_BRIDGE_EXTRA);
  ok = ok && sbuf_str(&b, "</script>\n</body>\n</html>");
  dsh_release(base);

  if (!ok) {
    if (b.p != NULL) dsh_release(b.p);
    return NULL;
  }
  return sbuf_take(&b, out_len);
}

/* ── 音频键抠取 ─────────────────────────────────────────────────────────── */

typedef struct {
  char **keys;
  int64_t count;
  int64_t cap;
} key_list;

int dsh_entry_doc_audio_keys(const char *html, size_t len, char ***out_keys, int64_t *out_count) {
  if (out_keys != NULL) *out_keys = NULL;
  if (out_count != NULL) *out_count = 0;
  if (out_keys == NULL || out_count == NULL) {
    dsh_set_last_error("dsh_entry_doc_audio_keys：出参不能为空");
    return -1;
  }
  /*
   * ⚠️ **抠取本身只有一份实现**（`audio/dsh_audio.c` 的 `dsh_audio_extract`）——
   * 这里只把"键名"摘出来交给文档那一层。原来这段扫描器就写在本文件里，
   * 而 `dsh_speech_dict_audio` 也要同一份东西，于是搬过去、这里改成薄包装
   * （坑 13：同一个判断两个来源迟早分叉）。
   */
  dsh_audio_ref *refs = NULL;
  int64_t count = 0;
  if (dsh_audio_extract(html, len, &refs, &count) != 0) return -1;
  if (count == 0) return 0;

  char **keys = (char **)dsh_mem_alloc((size_t)count * sizeof(char *));
  if (keys == NULL) {
    dsh_audio_refs_free(refs, count);
    dsh_set_last_error("内存不足：音频键清单");
    return -1;
  }
  for (int64_t i = 0; i < count; i++) {
    keys[i] = dsh_mem_strdup(refs[i].key != NULL ? refs[i].key : "");
    if (keys[i] == NULL) {
      dsh_entry_doc_audio_keys_free(keys, i);
      dsh_audio_refs_free(refs, count);
      dsh_set_last_error("内存不足：音频键");
      return -1;
    }
  }
  dsh_audio_refs_free(refs, count);
  *out_keys = keys;
  *out_count = count;
  return 0;
}