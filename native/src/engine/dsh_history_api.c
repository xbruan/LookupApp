/* ==========================================================================
 * `history` 组的两个接口（`dsh_history_query` / `dsh_history_clear`）：这一层是**薄的**，
 * 分页 / 去重窗口 / 上限全在 `store/dsh_history.c`，这里只把「库没打开」如实说清楚、
 * 把 JSON 出参交给调用方、把错误码翻成接口定义里那套 `dsh_error`。
 * ⚠️ 库打不开时**不许装成空历史** —— 空历史与「历史库坏了」在界面上长得一模一样，
 * 前者是「你还没查过词」、后者是「你的历史读不出来」，所以这一档**返回错误码**并写明原因。
 * ========================================================================== */

#include "dsh_lookup.h"
#include "dsh_internal.h"
#include "engine/dsh_engine_internal.h"
#include "json_reader.h" /* 把 store 给的那一页读回来，补两个字段再写出去 */
#include "json_writer.h"
#include "store/dsh_history.h"

#include <stdio.h>
#include <string.h>

/**
 * 给历史每一行补上「**那一本还在不在**」+ 一句人话（检查标准与措辞都走 `dsh_dict_status`，
 * 与词库清单同一处；分页 / 去重 / 上限仍只有 `store/dsh_history.c` 一处实现）。
 * ⚠️ 「不在词库里」的书名只能用**历史里那份快照**（那本已经没得查了），
 *    「文件不在了」用**当前显示名**（`customTitle || title`）；两种情形**两句话**，
 *    不许合成一句 —— 原因不同、恢复办法也不同。
 */
static char *history_with_status(dsh_engine *engine, const char *raw) {
  dsh_json_doc *doc = NULL;
  if (raw == NULL || dsh_json_parse(raw, strlen(raw), &doc) != 0 || doc == NULL) {
    dsh_set_last_error("历史那一页读不动");
    return NULL;
  }
  const dsh_json_node *root = dsh_json_doc_root(doc);
  const dsh_json_node *items = dsh_json_object_get(root, "items");
  int64_t total = 0;
  (void)dsh_json_i64_value(dsh_json_object_get(root, "total"), &total);
  const int has_more = dsh_json_bool_value(dsh_json_object_get(root, "hasMore"));
  const int64_t count = dsh_json_is_array(items) ? dsh_json_array_len(items) : 0;

  dsh_json *j = dsh_json_new();
  if (j == NULL) {
    dsh_json_doc_free(doc);
    dsh_set_last_error("内存不足：历史那一页");
    return NULL;
  }
  dsh_json_object_begin(j);
  dsh_json_kv_i64(j, "total", total);
  dsh_json_key(j, "items");
  dsh_json_array_begin(j);
  for (int64_t i = 0; i < count; i++) {
    const dsh_json_node *it = dsh_json_array_at(items, i);
    /* ⚠️ `dsh_json_str_value` 的第二个参数是**长度出参**，不是默认值 ——
     *    字段不在时它回 NULL，所以下面每一处都按 NULL 兜底。 */
    const char *word = dsh_json_str_value(dsh_json_object_get(it, "word"), NULL);
    const char *dict_id = dsh_json_str_value(dsh_json_object_get(it, "dictId"), NULL);
    const char *snapshot = dsh_json_str_value(dsh_json_object_get(it, "dictTitle"), NULL);
    if (word == NULL) word = "";
    if (dict_id == NULL) dict_id = "";
    if (snapshot == NULL) snapshot = "";
    int64_t at = 0;
    (void)dsh_json_i64_value(dsh_json_object_get(it, "at"), &at);

    /* 检查标准与措辞统一走 `dsh_dict_status`（`engine/dsh_engine.c`），与词库清单同一处 */
    char *note = NULL;
    const char *unavailable = dsh_dict_status(engine, dict_id, snapshot, &note);
    if (note == NULL && unavailable[0] != '\0') {
      /* 该给说明句却拼不出来（内存不足）：把已经建了一半的收掉，如实失败 */
      dsh_json_free(j);
      dsh_json_doc_free(doc);
      return NULL;
    }

    dsh_json_object_begin(j);
    dsh_json_kv_str(j, "word", word);
    dsh_json_kv_str(j, "dictId", dict_id);
    dsh_json_kv_str(j, "dictTitle", snapshot);
    dsh_json_kv_i64(j, "at", at);
    dsh_json_kv_str(j, "unavailable", unavailable);
    dsh_json_kv_str(j, "note", (note != NULL) ? note : "");
    dsh_json_object_end(j);
    if (note != NULL) dsh_release(note);
  }
  dsh_json_array_end(j);
  dsh_json_kv_bool(j, "hasMore", has_more);
  dsh_json_object_end(j);
  dsh_json_doc_free(doc);

  char *text = dsh_json_take(j);
  dsh_json_free(j);
  if (text == NULL) dsh_set_last_error("内存不足：历史那一页");
  return text;
}

enum dsh_error dsh_history_query(dsh_engine *engine, int32_t offset, int32_t limit,
                                 char **out_json) {
  if (out_json == NULL) {
    dsh_set_last_error("dsh_history_query：out_json 不能为空");
    return DSH_E_INVALID_ARG;
  }
  *out_json = NULL;
  if (engine == NULL) {
    dsh_set_last_error("dsh_history_query：引擎无效");
    return DSH_E_STATE;
  }
  dsh_history *h = dsh_engine_history(engine);
  if (h == NULL) {
    const char *why = dsh_engine_history_why(engine);
    dsh_set_last_error("查词历史用不了：%s",
                       why != NULL ? why : "引擎没有配置目录（这一份是只在内存里活的）");
    return DSH_E_NOT_FOUND;
  }
  char *raw = NULL;
  if (dsh_hist_query(h, offset, limit, &raw) != 0) return DSH_E_STATE;
  char *out = history_with_status(engine, raw);
  dsh_release(raw);
  if (out == NULL) return DSH_E_OOM;
  *out_json = out;
  return DSH_OK;
}

enum dsh_error dsh_history_clear(dsh_engine *engine, char **out_json) {
  if (out_json == NULL) {
    dsh_set_last_error("dsh_history_clear：out_json 不能为空");
    return DSH_E_INVALID_ARG;
  }
  *out_json = NULL;
  if (engine == NULL) {
    dsh_set_last_error("dsh_history_clear：引擎无效");
    return DSH_E_STATE;
  }
  dsh_history *h = dsh_engine_history(engine);
  if (h == NULL) {
    const char *why = dsh_engine_history_why(engine);
    dsh_set_last_error("查词历史用不了：%s",
                       why != NULL ? why : "引擎没有配置目录（这一份是只在内存里活的）");
    return DSH_E_NOT_FOUND;
  }
  int64_t total = 0;
  if (dsh_hist_clear(h, &total) != 0) return DSH_E_STATE;
  /* 形状与接口定义那份注释一致：`{"total":0}`。清空之后它就是 0（再查一次也是 0）*/
  if (dsh_hist_count(h, &total) != 0) return DSH_E_STATE;
  char *json = dsh_mem_strdup("{\"total\":0}");
  if (json == NULL) {
    dsh_set_last_error("内存不足：历史清空的应答");
    return DSH_E_OOM;
  }
  *out_json = json;
  return DSH_OK;
}
