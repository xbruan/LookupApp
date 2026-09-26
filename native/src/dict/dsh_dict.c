/* 解析层的对外接口（接口定义里 `parser` 组那 6 个 dsh_dict_*）：把 `dsh_mdx` 的结果
 * 转成接口定义约定的 JSON，并把句柄管好。**不含产品约定** ——「落点」「联想」「兜底链」
 * 在 engine / lookup 层。
 *
 * ⚠️ 句柄里放一个魔数（0x44494354 = ASCII 的「DICT」）：宿主隔着 P/Invoke 传错句柄时，
 *    要在**崩溃之前**如实报错，而不是解引用野指针 —— P/Invoke 那边的崩溃连栈都没有。 */

#include "dict/dsh_mdx.h"
#include "dsh_internal.h"
#include "dsh_lookup.h"
#include "json_writer.h"

#include <stdio.h>
#include <string.h>

#define DSH_DICT_MAGIC 0x44494354u

struct dsh_dict {
  uint32_t magic;
  dsh_mdx *mdx;
};

/** 校验句柄。不合法时写原因并返回 NULL。 */
static dsh_mdx *checked(dsh_dict *dict, const char *who) {
  if (dict == NULL) {
    dsh_set_last_error("%s：句柄是空指针", who);
    return NULL;
  }
  if (dict->magic != DSH_DICT_MAGIC) {
    dsh_set_last_error("%s：句柄已被释放、或不是本内核给的（魔数不符）", who);
    return NULL;
  }
  if (dict->mdx == NULL) {
    dsh_set_last_error("%s：句柄状态不对（没有关联的词典）", who);
    return NULL;
  }
  return dict->mdx;
}

enum dsh_error dsh_dict_open(const char *path, dsh_dict **out_dict) {
  dsh_clear_last_error();
  if (out_dict == NULL) {
    dsh_set_last_error("dsh_dict_open：out_dict 不能为空");
    return DSH_E_INVALID_ARG;
  }
  *out_dict = NULL;
  if (path == NULL || path[0] == '\0') {
    dsh_set_last_error("dsh_dict_open：路径不能为空");
    return DSH_E_INVALID_ARG;
  }

  dsh_mdx *mdx = NULL;
  if (dsh_mdx_open(path, &mdx) != 0) {
    /* dsh_mdx_open 已经把原因写进 last_error（打不开 / 不是 MDict / 损坏） */
    return DSH_E_NOT_FOUND;
  }

  dsh_dict *dict = (dsh_dict *)dsh_mem_alloc(sizeof(dsh_dict));
  if (dict == NULL) {
    dsh_mdx_close(mdx);
    dsh_set_last_error("dsh_dict_open：内存不足");
    return DSH_E_OOM;
  }
  dict->magic = DSH_DICT_MAGIC;
  dict->mdx = mdx;
  *out_dict = dict;
  return DSH_OK;
}

void dsh_dict_close(dsh_dict *dict) {
  if (dict == NULL) return;
  if (dict->magic != DSH_DICT_MAGIC) {
    /* 不是我们的句柄（或已经关过一次）—— 拒绝，别去动它 */
    dsh_set_last_error("dsh_dict_close：句柄已被释放、或不是本内核给的（魔数不符）");
    return;
  }
  dict->magic = 0;
  if (dict->mdx != NULL) dsh_mdx_close(dict->mdx);
  dsh_release(dict);
}

enum dsh_error dsh_dict_info(dsh_dict *dict, char **out_json) {
  dsh_clear_last_error();
  if (out_json == NULL) {
    dsh_set_last_error("dsh_dict_info：out_json 不能为空");
    return DSH_E_INVALID_ARG;
  }
  *out_json = NULL;
  dsh_mdx *mdx = checked(dict, "dsh_dict_info");
  if (mdx == NULL) return DSH_E_STATE;

  dsh_json *j = dsh_json_new();
  if (j == NULL) return DSH_E_OOM;

  const char *path = dsh_mdx_path(mdx);
  const char *slash = strrchr(path, '/');
  const char *bslash = strrchr(path, '\\');
  const char *file = (slash > bslash ? slash : bslash);
  file = (file == NULL) ? path : file + 1;

  dsh_json_object_begin(j);
  dsh_json_kv_str(j, "title", dsh_mdx_title(mdx));
  dsh_json_kv_str(j, "fileName", file);
  dsh_json_kv_str(j, "path", path);
  dsh_json_kv_bool(j, "isMdd", dsh_mdx_is_mdd(mdx));
  dsh_json_kv_i64(j, "entryCount", dsh_mdx_key_count(mdx));
  dsh_json_kv_i64(j, "keyBlockCount", dsh_mdx_key_block_count(mdx));
  /* 版本写成一位小数的字符串（与参考实现一个样子）。⚠️ JSON 里同一个键**只能出现一次**：
   * 别「先写 null 占位、后面再覆盖」—— 重复键宿主取到哪个取决于解析器，最难查。 */
  {
    char ver[32];
    snprintf(ver, sizeof(ver), "%.1f", dsh_mdx_version(mdx));
    dsh_json_kv_str(j, "version", ver);
  }
  dsh_json_kv_bool(j, "encrypted", dsh_mdx_encrypted(mdx) != 0);
  dsh_json_kv_int(j, "encryptedFlag", dsh_mdx_encrypted(mdx));
  dsh_json_kv_str(j, "encoding", dsh_mdx_encoding_name(mdx));
  dsh_json_kv_bool(j, "blockOrderMonotone", dsh_mdx_block_order_monotone(mdx));
  dsh_json_kv_int(j, "warningCount", dsh_mdx_warning_count(mdx));
  dsh_json_object_end(j);

  char *text = dsh_json_take(j);
  dsh_json_free(j);
  if (text == NULL) {
    dsh_set_last_error("dsh_dict_info：内存不足（JSON 结果）");
    return DSH_E_OOM;
  }
  *out_json = text;
  return DSH_OK;
}

enum dsh_error dsh_dict_contains(dsh_dict *dict, const char *key, char **out_json) {
  dsh_clear_last_error();
  if (out_json == NULL) {
    dsh_set_last_error("dsh_dict_contains：out_json 不能为空");
    return DSH_E_INVALID_ARG;
  }
  *out_json = NULL;
  if (key == NULL || key[0] == '\0') {
    dsh_set_last_error("dsh_dict_contains：键不能为空");
    return DSH_E_INVALID_ARG;
  }
  dsh_mdx *mdx = checked(dict, "dsh_dict_contains");
  if (mdx == NULL) return DSH_E_STATE;

  char *landed = NULL;
  const int hit = dsh_mdx_lookup_key(mdx, key, &landed, NULL);
  if (hit < 0) return DSH_E_FORMAT; /* 原因已由 mdx 层写好 */

  dsh_json *j = dsh_json_new();
  if (j == NULL) {
    if (landed != NULL) dsh_release(landed);
    return DSH_E_OOM;
  }
  dsh_json_object_begin(j);
  dsh_json_kv_bool(j, "found", hit == 1);
  /* keyText 是**词典里的规范键名**，不是调用方传进来的写法 —— 产品层很要紧
   * （回答「落点是哪条」）。 */
  dsh_json_kv_str(j, "keyText", hit == 1 ? landed : "");
  dsh_json_object_end(j);
  if (landed != NULL) dsh_release(landed);

  char *text = dsh_json_take(j);
  dsh_json_free(j);
  if (text == NULL) return DSH_E_OOM;
  *out_json = text;
  return DSH_OK;
}

enum dsh_error dsh_dict_fetch(dsh_dict *dict, const char *key, char **out_json) {
  dsh_clear_last_error();
  if (out_json == NULL) {
    dsh_set_last_error("dsh_dict_fetch：out_json 不能为空");
    return DSH_E_INVALID_ARG;
  }
  *out_json = NULL;
  if (key == NULL || key[0] == '\0') {
    dsh_set_last_error("dsh_dict_fetch：键不能为空");
    return DSH_E_INVALID_ARG;
  }
  dsh_mdx *mdx = checked(dict, "dsh_dict_fetch");
  if (mdx == NULL) return DSH_E_STATE;

  char *landed = NULL;
  char *body = NULL;
  int64_t body_len = 0;
  const int hit = dsh_mdx_fetch(mdx, key, &landed, &body, &body_len);

  dsh_json *j = dsh_json_new();
  if (j == NULL) {
    if (landed != NULL) dsh_release(landed);
    if (body != NULL) dsh_release(body);
    return DSH_E_OOM;
  }
  dsh_json_object_begin(j);
  dsh_json_kv_bool(j, "found", hit == 1);
  /* keyText 是**词典里的规范键名**，不是调用方传进来的写法 —— 产品层很要紧
   * （回答「落点是哪条」）。 */
  dsh_json_kv_str(j, "keyText", hit == 1 ? landed : "");
  /* ⚠️ 用带长度的那条写入：记录里可能有内嵌的 U+0000（见 dsh_mdx.c 里那条约定） */
  dsh_json_key(j, "text");
  dsh_json_str_len(j, (hit == 1 && body != NULL) ? body : "", (hit == 1 ? body_len : 0));
  if (hit == 1) dsh_json_kv_i64(j, "textBytes", body_len);
  if (hit < 0) {
  /* ⚠️ `dsh_last_error_message()` 是**新分配的一份拷贝**，抄进 JSON 之后必须还 */
    const char *why = dsh_last_error_message();
    dsh_json_kv_str(j, "reason", why != NULL ? why : "");
    if (why != NULL) dsh_release((void *)why);
  }
  dsh_json_object_end(j);

  if (landed != NULL) dsh_release(landed);
  if (body != NULL) dsh_release(body);

  char *text = dsh_json_take(j);
  dsh_json_free(j);
  if (text == NULL) return DSH_E_OOM;
  *out_json = text;
  return DSH_OK;
}

enum dsh_error dsh_dict_keys(dsh_dict *dict, char **out_json) {
  dsh_clear_last_error();
  if (out_json == NULL) {
    dsh_set_last_error("dsh_dict_keys：out_json 不能为空");
    return DSH_E_INVALID_ARG;
  }
  *out_json = NULL;
  dsh_mdx *mdx = checked(dict, "dsh_dict_keys");
  if (mdx == NULL) return DSH_E_STATE;

  char **keys = NULL;
  int64_t count = 0;
  if (dsh_mdx_list_keys(mdx, &keys, &count) != 0) return DSH_E_FORMAT;

  dsh_json *j = dsh_json_new();
  if (j == NULL) {
    dsh_mdx_free_keys(keys, count);
    return DSH_E_OOM;
  }
  dsh_json_array_begin(j);
  for (int64_t i = 0; i < count; i++) dsh_json_str(j, keys[i]);
  dsh_json_array_end(j);
  dsh_mdx_free_keys(keys, count);

  char *text = dsh_json_take(j);
  dsh_json_free(j);
  if (text == NULL) return DSH_E_OOM;
  *out_json = text;
  return DSH_OK;
}
