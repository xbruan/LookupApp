/* 见 dsh_dicts.h —— 加载与缓存、落点解析。 */
#include "engine/dsh_dicts.h"
#include "dsh_internal.h"
#include "engine/dsh_engine_internal.h"
#include "text/dsh_textutil.h"
#include "engine/dsh_settings.h"
#include "json_reader.h"
#include "platform/dsh_file.h"

#include <stdio.h>
#include <string.h>

/** `@@@LINK=` 最多跟这么多层（防环）*/
#define DSH_MAX_LINK_DEPTH 16

/** 缓存里的一本 = 头文件里的 `dsh_loaded_dict`（布局在头文件里定，`dicts_state` 是一整块存储）*/
#define DSH_LOADED_MAX 32



/* ── 落点解析 ───────────────────────────────────────────────────────────── */

char *dsh_parse_link_redirect(const char *text, int64_t len) {
  static const char PREFIX[] = "@@@LINK=";
  const size_t plen = sizeof(PREFIX) - 1;
  if (text == NULL || len < (int64_t)plen) return NULL;
  if (memcmp(text, PREFIX, plen) != 0) return NULL;
  int64_t end = len;
  /* 记录里常带一个结尾的 \0（长度是"到下一条的偏移"算出来的），要去掉 */
  while (end > (int64_t)plen && text[end - 1] == '\0') end--;
  const int64_t n = end - (int64_t)plen;
  if (n <= 0) return NULL;
  char *out = (char *)dsh_mem_alloc((size_t)n + 1);
  if (out == NULL) {
    dsh_set_last_error("内存不足：重定向目标");
    return NULL;
  }
  memcpy(out, text + plen, (size_t)n);
  out[n] = '\0';
  return out;
}

/** 把一段文本按"大小写变体"依次试；命中返回 1 并写出规范键名与正文 */
static int try_variants(dsh_mdx *mdx, const char *query, char **out_key, char **out_def,
                        int64_t *out_def_len) {
  /* 变体顺序：原样 → 全小写 → 首字母大写 → 全大写。
   * ⚠️ 原样必须排第一：词典里真有 `Apple` 这种键时，先试小写会把它抢走 —— 而用户打的就是那个写法。 */
  char *variants[4] = {NULL, NULL, NULL, NULL};
  variants[0] = dsh_mem_strdup(query);
  {
    const size_t n = strlen(query);
    variants[1] = (char *)dsh_mem_alloc(n + 1);
    variants[2] = (char *)dsh_mem_alloc(n + 1);
    variants[3] = (char *)dsh_mem_alloc(n + 1);
    if (variants[0] == NULL || variants[1] == NULL || variants[2] == NULL ||
        variants[3] == NULL) {
      for (int i = 0; i < 4; i++) {
        if (variants[i] != NULL) dsh_release(variants[i]);
      }
      dsh_set_last_error("内存不足：大小写变体");
      return -1;
    }
    for (size_t i = 0; i < n; i++) {
      char c = query[i];
      variants[1][i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
      variants[3][i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    variants[1][n] = '\0';
    variants[3][n] = '\0';
    for (size_t i = 0; i < n; i++) variants[2][i] = variants[1][i];
    if (n > 0 && variants[2][0] >= 'a' && variants[2][0] <= 'z') {
      variants[2][0] = (char)(variants[2][0] - 'a' + 'A');
    }
    variants[2][n] = '\0';
  }

  int found = 0;
  for (int i = 0; i < 4 && !found; i++) {
    char *landed = NULL;
    char *def = NULL;
    int64_t def_len = 0;
    const int hit = dsh_mdx_fetch(mdx, variants[i], &landed, &def, &def_len);
    if (hit == 1) {
      found = 1;
      *out_key = landed;
      *out_def = def;
      *out_def_len = def_len;
      break;
    }
    /* hit == 0（没有）继续试下一个变体；hit < 0（出错）也继续 ——
     * 某个变体解析出错不该让整个查询失败。 */
    if (landed != NULL) dsh_release(landed);
    if (def != NULL) dsh_release(def);
  }
  for (int i = 0; i < 4; i++) {
    if (variants[i] != NULL) dsh_release(variants[i]);
  }
  return found;
}

void dsh_resolve_entry(dsh_mdx *mdx, const char *query, dsh_resolved *out) {
  if (out == NULL) return;
  memset(out, 0, sizeof(*out));
  if (mdx == NULL || query == NULL || query[0] == '\0') return;

  char *cursor = dsh_mem_strdup(query);
  if (cursor == NULL) return;

  for (int depth = 0; depth < DSH_MAX_LINK_DEPTH; depth++) {
    char *key = NULL;
    char *def = NULL;
    int64_t def_len = 0;
    const int hit = try_variants(mdx, cursor, &key, &def, &def_len);
    if (hit == 0) {
      dsh_release(cursor);
      return; /* 没找到：found 保持 0 */
    }

    char *redirect = dsh_parse_link_redirect(def, def_len);
    if (redirect != NULL && strcmp(redirect, cursor) != 0) {
      /* 跟下一层。⚠️ 检查标准是「重定向目标与当前游标不同」—— 自指（`x` → `@@@LINK=x`）
       * 要停，否则会原地打转。 */
      if (out->linked_to != NULL) dsh_release(out->linked_to);
      out->linked_to = redirect;
      dsh_release(cursor);
      cursor = dsh_mem_strdup(redirect);
      if (key != NULL) dsh_release(key);
      if (def != NULL) dsh_release(def);
      if (cursor == NULL) return;
      continue;
    }
    if (redirect != NULL) dsh_release(redirect);

    out->found = 1;
    out->key_text = key;
    out->definition = def;
    out->definition_len = def_len;
    dsh_release(cursor);
    return;
  }

  /* 跟了 16 层还在跟：当成环，如实报"没找到"（linked_to 里留着最后那一步，便于诊断） */
  dsh_release(cursor);
}

void dsh_resolved_free(dsh_resolved *r) {
  if (r == NULL) return;
  if (r->key_text != NULL) dsh_release(r->key_text);
  if (r->definition != NULL) dsh_release(r->definition);
  if (r->linked_to != NULL) dsh_release(r->linked_to);
  memset(r, 0, sizeof(*r));
}

/* ── 加载与缓存 ─────────────────────────────────────────────────────────── */

static dsh_loaded_dict *find_loaded(dicts_state *st, const char *id) {
  for (int64_t i = 0; i < st->count; i++) {
    if (st->items[i].id != NULL && strcmp(st->items[i].id, id) == 0) return &st->items[i];
  }
  return NULL;
}

int dsh_dicts_is_loaded(struct dsh_engine *e, const char *dict_id) {
  if (e == NULL || dict_id == NULL) return 0;
  dicts_state *st = dsh_engine_dicts_state(e);
  return find_loaded(st, dict_id) != NULL;
}


dsh_mdx *dsh_dicts_peek(struct dsh_engine *e, const char *dict_id) {
  if (e == NULL || dict_id == NULL) return NULL;
  dsh_loaded_dict *ld = find_loaded(dsh_engine_dicts_state(e), dict_id);
  return (ld != NULL) ? ld->mdx : NULL;
}

void dsh_dicts_unload_all(struct dsh_engine *e) {
  if (e == NULL) return;
  dicts_state *st = dsh_engine_dicts_state(e);
  for (int64_t i = 0; i < st->count; i++) {
    if (st->items[i].mdx != NULL) dsh_mdx_close(st->items[i].mdx);
    for (int v = 0; v < st->items[i].mdd_count; v++) {
      if (st->items[i].mdds[v] != NULL) dsh_mdx_close(st->items[i].mdds[v]);
      st->items[i].mdds[v] = NULL;
    }
    st->items[i].mdd_count = 0;
    st->items[i].mdd_tried = 0;
    if (st->items[i].id != NULL) dsh_release(st->items[i].id);
    st->items[i].mdx = NULL;
    st->items[i].id = NULL;
  }
  st->count = 0;
}

/* ── `.mdd` 资源卷 ─────────────────────────────────────────────────────── */

/**
 * 把这一本的 `.mdd` 卷打开（懒、只试一次）。
 * ⚠️ 打不开**不算错误**（不带 .mdd 的词典一个卷都没有，那是合法的）——
 *    失败只在 last_error 里留一句原因，返回值仍然是 0。
 */
static int ensure_mdd_open(struct dsh_engine *e, const char *dict_id) {
  dicts_state *st = dsh_engine_dicts_state(e);
  dsh_loaded_dict *ld = find_loaded(st, dict_id);
  if (ld == NULL) {
    dsh_set_last_error("取资源卷失败：这一本还没加载（id=%s）", dict_id);
    return -1;
  }
  if (ld->mdd_tried) return 0;
  ld->mdd_tried = 1;

  const struct dsh_settings *s = dsh_engine_settings(e);
  const dsh_stored_dict *d = (s != NULL) ? dsh_settings_dict_by_id(s, dict_id) : NULL;
  if (d == NULL || d->mdx_path == NULL || d->mdx_path[0] == '\0') return 0;

  char **paths = NULL;
  int64_t count = 0;
  if (dsh_path_find_mdd_volumes(d->mdx_path, &paths, &count) != 0) return 0;

  const char *first_error = NULL;
  char *first_error_copy = NULL;
  for (int64_t i = 0; i < count && ld->mdd_count < DSH_MDD_MAX_VOLUMES; i++) {
    dsh_mdx *mdd = NULL;
    if (dsh_mdx_open(paths[i], &mdd) == 0) {
      ld->mdds[ld->mdd_count++] = mdd;
    } else if (first_error_copy == NULL) {
      first_error = dsh_last_error_message();
      first_error_copy = (char *)first_error;
    }
    dsh_release(paths[i]);
  }
  if (paths != NULL) dsh_release(paths);

  if (first_error_copy != NULL) {
    /* 留一句原因（不覆盖调用方真正关心的错误由调用方决定，这里只记现场）*/
    dsh_set_last_error("资源卷打不开：%s", first_error_copy);
    dsh_release(first_error_copy);
    return 0;
  }
  return 0;
}

int dsh_dicts_mdd_volumes(struct dsh_engine *e, const char *dict_id, dsh_mdx *const **out,
                          int *out_count) {
  if (out != NULL) *out = NULL;
  if (out_count != NULL) *out_count = 0;
  if (e == NULL || dict_id == NULL) return -1;
  if (ensure_mdd_open(e, dict_id) != 0) return -1;
  dsh_loaded_dict *ld = find_loaded(dsh_engine_dicts_state(e), dict_id);
  if (ld == NULL) return -1;
  if (out != NULL) *out = (dsh_mdx *const *)ld->mdds;
  if (out_count != NULL) *out_count = ld->mdd_count;
  return 0;
}

int dsh_dicts_mdd_fetch(struct dsh_engine *e, const char *dict_id, const char *candidate,
                        char **out_bytes, size_t *out_len) {
  if (out_bytes != NULL) *out_bytes = NULL;
  if (out_len != NULL) *out_len = 0;
  if (e == NULL || dict_id == NULL || candidate == NULL || out_bytes == NULL) return -1;
  if (ensure_mdd_open(e, dict_id) != 0) return -1;
  dsh_loaded_dict *ld = find_loaded(dsh_engine_dicts_state(e), dict_id);
  if (ld == NULL) return -1;
  for (int v = 0; v < ld->mdd_count; v++) {
    char *landed = NULL;
    uint8_t *bytes = NULL;
    size_t len = 0;
    /* ⚠️ **原始字节**（`dsh_mdx_fetch_raw`），不是文本那条：`.mdd` 的文件编码是
     *    UTF-16LE，而资源是二进制（PNG / MP3 / SPX）—— 按文本解一遍就毁了。 */
    const int hit = dsh_mdx_fetch_raw(ld->mdds[v], candidate, &landed, &bytes, &len);
    if (landed != NULL) dsh_release(landed);
    if (hit == 1 && bytes != NULL) {
      if (out_len != NULL) *out_len = len;
      *out_bytes = (char *)bytes;
      return 1;
    }
    if (bytes != NULL) dsh_release(bytes);
  }
  return 0;
}

int dsh_dicts_mdd_contains(struct dsh_engine *e, const char *dict_id, const char *candidate) {
  if (e == NULL || dict_id == NULL || candidate == NULL) return 0;
  if (ensure_mdd_open(e, dict_id) != 0) return 0;
  dsh_loaded_dict *ld = find_loaded(dsh_engine_dicts_state(e), dict_id);
  if (ld == NULL) return 0;
  for (int v = 0; v < ld->mdd_count; v++) {
    char *landed = NULL;
    int64_t block = 0;
    const int hit = dsh_mdx_lookup_key(ld->mdds[v], candidate, &landed, &block);
    if (landed != NULL) dsh_release(landed);
    if (hit == 1) return 1;
  }
  return 0;
}

/** 从词库清单里找一条（找不到 → NULL） */
static const dsh_stored_dict *find_stored(struct dsh_engine *e, const char *dict_id) {
  const struct dsh_settings *s = dsh_engine_settings(e);
  if (s == NULL) return NULL;
  return dsh_settings_dict_by_id(s, dict_id);
}

int dsh_dicts_ensure_loaded(struct dsh_engine *e, const char *dict_id) {
  if (e == NULL || dict_id == NULL || dict_id[0] == '\0') {
    dsh_set_last_error("加载词典：id 为空");
    return -1;
  }
  dicts_state *st = dsh_engine_dicts_state(e);
  if (find_loaded(st, dict_id) != NULL) return 0; /* 已经在了 */

  const dsh_stored_dict *d = find_stored(e, dict_id);
  if (d == NULL) {
    dsh_set_last_error("加载词典失败：词库里没有这本（id=%s）", dict_id);
    return -1;
  }
  if (st->count >= DSH_LOADED_MAX) {
    dsh_set_last_error("同时加载的词典太多（上限 %d 本）", DSH_LOADED_MAX);
    return -1;
  }
  if (d->mdx_path == NULL || d->mdx_path[0] == '\0') {
    dsh_set_last_error("加载词典失败：这本没有 .mdx 路径");
    return -1;
  }

  dsh_mdx *mdx = NULL;
  if (dsh_mdx_open(d->mdx_path, &mdx) != 0) {
    const char *why = dsh_last_error_message();
    dsh_set_last_error("加载《%s》失败：%s", dsh_settings_dict_display_name(d, NULL),
                       why ? why : "");
    if (why != NULL) dsh_release((void *)why);
    return -1;
  }
  st->items[st->count].id = dsh_mem_strdup(dict_id);
  st->items[st->count].mdx = mdx;
  if (st->items[st->count].id == NULL) {
    dsh_mdx_close(mdx);
    dsh_set_last_error("内存不足：已加载词典的 id");
    return -1;
  }
  st->count++;
  return 0;
}

int dsh_dicts_borrow_once(struct dsh_engine *e, const char *dict_id,
                          int (*fn)(dsh_mdx *mdx, void *ctx), void *ctx, int *out_loaded) {
  if (out_loaded != NULL) *out_loaded = 0;
  if (e == NULL || dict_id == NULL || fn == NULL) {
    dsh_set_last_error("借查词典：参数不能为空");
    return -1;
  }
  /* ⚠️ 纪律：**借一次用完就关，绝不入库**。已经在缓存里的那本复用（那本来就该在），
   * 但**这一次的打开**不入库 —— 「问一句就走」不会让一本词典被加载进内存。 */
  dsh_mdx *cached = dsh_dicts_peek(e, dict_id);
  dsh_mdx *mdx = cached;
  if (mdx == NULL) {
    const dsh_stored_dict *d = find_stored(e, dict_id);
    if (d == NULL) {
      dsh_set_last_error("借查失败：词库里没有这本（id=%s）", dict_id);
      return -1;
    }
    if (d->mdx_path == NULL || d->mdx_path[0] == '\0') {
      dsh_set_last_error("借查失败：这本没有 .mdx 路径");
      return -1;
    }
    if (dsh_mdx_open(d->mdx_path, &mdx) != 0) return -1;
    if (out_loaded != NULL) *out_loaded = 1;
  }
  const int rc = fn(mdx, ctx);
  if (cached == NULL && mdx != NULL) dsh_mdx_close(mdx); /* 借的必须还 */
  return rc;
}

/* ── 资源键名的候选写法 ─────────────────────────────────────────────────── */
/**
 * 六种候选键名，因为 MDict 资源键名在**不同词典里写法不一样**：有的带前导反斜杠
 * （`\style.css`）、有的用正斜杠（`/style.css`）、有的干脆没有前缀（`style.css`）——
 * 这是「同一个资源在四本词典里四种写法」的真实情况，不是过度设计。
 */
/* 百分号解码的**算法在 `text/dsh_textutil.c` 的 `dsh_text_percent_decode`**，这里不自带一份 */
static char *url_decode(const char *text) {
  if (text == NULL) text = "";
  char *out = dsh_text_percent_decode(text, strlen(text));
  if (out == NULL) dsh_set_last_error("内存不足：资源键名");
  return out;
}

int dsh_dicts_resource_key_forms(const char *key, char **out, int max) {
  char *decoded = url_decode(key);
  if (decoded == NULL) return -1;
  /* 去掉前导反斜杠与正斜杠，再按四种组合拼回去 */
  const char *clean = decoded;
  while (*clean == '\\' || *clean == '/') clean++;

  char *back = (char *)dsh_mem_alloc(strlen(clean) + 1);
  char *fwd = (char *)dsh_mem_alloc(strlen(clean) + 1);
  if (back == NULL || fwd == NULL) {
    if (back != NULL) dsh_release(back);
    if (fwd != NULL) dsh_release(fwd);
    dsh_release(decoded);
    dsh_set_last_error("内存不足：资源候选键名");
    return -1;
  }
  size_t i = 0;
  for (; clean[i] != '\0'; i++) {
    back[i] = (clean[i] == '/') ? '\\' : clean[i];
    fwd[i] = (clean[i] == '\\') ? '/' : clean[i];
  }
  back[i] = '\0';
  fwd[i] = '\0';

  /* 顺序与参考实现一致：`\反斜杠` → `\正斜杠` → `反斜杠` → `正斜杠` → `/\反斜杠` → 原样 */
  const char *raw[6];
  char *owned[5] = {NULL, NULL, NULL, NULL, NULL};
  const size_t nb = strlen(back);
  const size_t nf = strlen(fwd);
  char *b0 = (char *)dsh_mem_alloc(nb + 2);
  char *b1 = (char *)dsh_mem_alloc(nf + 2);
  char *b2 = (char *)dsh_mem_alloc(nb + 3);
  if (b0 == NULL || b1 == NULL || b2 == NULL) {
    if (b0 != NULL) dsh_release(b0);
    if (b1 != NULL) dsh_release(b1);
    if (b2 != NULL) dsh_release(b2);
    dsh_release(back);
    dsh_release(fwd);
    dsh_release(decoded);
    dsh_set_last_error("内存不足：资源候选键名");
    return -1;
  }
  snprintf(b0, nb + 2, "\\%s", back);
  snprintf(b1, nf + 2, "\\%s", fwd);
  snprintf(b2, nb + 3, "/\\%s", back);
  owned[0] = b0;
  owned[1] = b1;
  owned[2] = back;
  owned[3] = fwd;
  owned[4] = b2;
  raw[0] = b0;
  raw[1] = b1;
  raw[2] = back;
  raw[3] = fwd;
  raw[4] = b2;
  raw[5] = decoded;

  int count = 0;
  for (size_t k = 0; k < 6 && count < max; k++) {
    if (raw[k] == NULL || raw[k][0] == '\0') continue;
    int dup = 0;
    for (int t = 0; t < count; t++) {
      if (strcmp(out[t], raw[k]) == 0) {
        dup = 1;
        break;
      }
    }
    if (dup) continue;
    out[count] = dsh_mem_strdup(raw[k]);
    if (out[count] == NULL) {
      for (int t = 0; t < count; t++) dsh_release(out[t]);
      for (size_t t = 0; t < 5; t++) {
        if (owned[t] != NULL) dsh_release(owned[t]);
      }
      dsh_release(decoded);
      dsh_set_last_error("内存不足：资源候选键名");
      return -1;
    }
    count++;
  }
  for (size_t t = 0; t < 5; t++) {
    if (owned[t] != NULL) dsh_release(owned[t]);
  }
  dsh_release(decoded);
  return count;
}
void dsh_dicts_key_forms_free(char **keys, int count) {
  if (keys == NULL) return;
  for (int i = 0; i < count; i++) {
    if (keys[i] != NULL) dsh_release(keys[i]);
  }
  dsh_release(keys);
}