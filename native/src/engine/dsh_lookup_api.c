/* ==========================================================================
 * 接口定义里 `lookup` 组中**只查表、不落盘、不写历史**的那三条：`resolve` / `suggest` / `probe`。
 * 它们共用一份 `dsh_dicts` 的加载缓存与落点解析（`dsh_resolve_entry` / `dsh_dicts_borrow_once`）。
 *
 * ── 两条纪律 ───────────────────────────────────────────────────────────────
 * ① `resolve` 与 `lookup` 对同一个词**必须给同一个落点**（`DictionaryEngine.ResolveKey` 那条不变式）——
 *    两条都走 `dsh_resolve_entry` 这**同一条**解析路径，不许各写一份「差不多」的。
 * ② `probe` **不许把词典留在内存里**：打开 → 问一次 → 关掉，**绝不入库**。
 * ========================================================================== */

#include "dsh_lookup.h"
#include "dsh_internal.h"
#include "engine/dsh_dicts.h"
#include "engine/dsh_fallback.h"
#include "engine/dsh_engine_internal.h"
#include "engine/dsh_settings.h"
#include "json_reader.h"
#include "json_writer.h"
#include "platform/dsh_time.h"
#include "store/dsh_history.h"
#include "text/dsh_html.h"
#include "text/dsh_language.h"
#include "text/dsh_textutil.h"
#include "translate/dsh_translate.h"

#include <stdio.h>
#include <string.h>

/* ── 小工具 ── */

/** 去掉首尾空白（返回新分配的串；全空白 → 空串，不是 NULL） */
static char *trim_copy(const char *text) {
  if (text == NULL) return dsh_mem_strdup("");
  const char *b = text;
  while (*b == ' ' || *b == '\t' || *b == '\n' || *b == '\r') b++;
  const char *e = b + strlen(b);
  while (e > b) {
    const char c = e[-1];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
    e--;
  }
  const size_t n = (size_t)(e - b);
  char *out = (char *)dsh_mem_alloc(n + 1);
  if (out == NULL) return NULL;
  memcpy(out, b, n);
  out[n] = '\0';
  return out;
}

/** 取「用哪一本」：给空就用当前词典。返回**借用**指针（错的时候写 last_error） */
static const dsh_stored_dict *pick_dict(dsh_engine *e, const char *dict_id,
                                        const char **out_id) {
  const struct dsh_settings *s = dsh_engine_settings(e);
  if (s == NULL) {
    dsh_set_last_error("引擎里没有设置");
    return NULL;
  }
  const char *id = (dict_id != NULL && dict_id[0] != '\0') ? dict_id
                                                           : dsh_settings_current_dict_id(s);
  if (id == NULL || id[0] == '\0') {
    dsh_set_last_error("还没指定当前词典");
    return NULL;
  }
  const dsh_stored_dict *d = dsh_settings_dict_by_id(s, id);
  if (d == NULL) {
    dsh_set_last_error("词库里没有这本词典（id=%s）", id);
    return NULL;
  }
  if (out_id != NULL) *out_id = id;
  return d;
}

/** 新建一个写入器并把出参取出来（失败时返回 NULL 并释放写入器） */
static char *take_json(dsh_json *j, const char *what) {
  if (j == NULL) return NULL;
  char *text = dsh_json_take(j);
  dsh_json_free(j);
  if (text == NULL) dsh_set_last_error("内存不足：%s 的 JSON", what);
  return text;
}

/* ── dsh_engine_resolve ── */

enum dsh_error dsh_engine_resolve(dsh_engine *engine, const char *dict_id, const char *text,
                                  char **out_json) {
  if (out_json == NULL) {
    dsh_set_last_error("dsh_engine_resolve：out_json 不能为空");
    return DSH_E_INVALID_ARG;
  }
  *out_json = NULL;
  if (engine == NULL) {
    dsh_set_last_error("dsh_engine_resolve：引擎无效");
    return DSH_E_STATE;
  }
  if (text == NULL) {
    dsh_set_last_error("dsh_engine_resolve：text 不能为空");
    return DSH_E_INVALID_ARG;
  }

  dsh_json *j = dsh_json_new();
  if (j == NULL) return DSH_E_OOM;

  char *query = trim_copy(text);
  if (query == NULL) {
    dsh_json_free(j);
    return DSH_E_OOM;
  }

  const char *id = NULL;
  const dsh_stored_dict *d = NULL;
  int enabled = 0;
  char reason[256] = "";
  char *landed = NULL;

  if (query[0] == '\0') {
    snprintf(reason, sizeof(reason), "没有要查的文字");
  } else if ((d = pick_dict(engine, dict_id, &id)) == NULL) {
    /* pick_dict 已经写好 last_error；把它抄进 reason（界面要显示这一句）。
     * ⚠️ `dsh_last_error_message()` 给的是**新分配的一份拷贝**（内核分配、内核释放），
     *    抄完必须还 —— 漏了就是一次悄悄泄漏。 */
    const char *why = dsh_last_error_message();
    snprintf(reason, sizeof(reason), "%s", why != NULL ? why : "");
    if (why != NULL) dsh_release((void *)why);
  } else if (dsh_dicts_ensure_loaded(engine, id) != 0) {
    const char *why = dsh_last_error_message();
    snprintf(reason, sizeof(reason), "%s", why != NULL ? why : "这本词典加载不了");
    if (why != NULL) dsh_release((void *)why);
  } else {
    enabled = 1;
    dsh_mdx *mdx = dsh_dicts_peek(engine, id);
    dsh_resolved r;
    dsh_resolve_entry(mdx, query, &r);
    if (r.found && r.key_text != NULL) {
      landed = dsh_mem_strdup(r.key_text);
  /* 跟过重定向就说明白（界面要能解释「为什么落点与输入不一样」） */
      if (r.linked_to != NULL) {
        snprintf(reason, sizeof(reason), "词典把它重定向到了「%s」", r.key_text);
      }
    } else if (r.linked_to != NULL) {
      snprintf(reason, sizeof(reason), "词典把它重定向到了「%s」，但那条词条不存在",
               r.linked_to);
    } else {
      snprintf(reason, sizeof(reason), "这本词典里没有这一条");
    }
    dsh_resolved_free(&r);
  }

  dsh_json_object_begin(j);
  dsh_json_key(j, "landed");
  if (landed != NULL) dsh_json_str(j, landed);
  else dsh_json_null(j);
  dsh_json_kv_str(j, "dictId", (d != NULL && id != NULL) ? id : "");
  dsh_json_kv_str(j, "dictTitle",
                  (d != NULL) ? dsh_settings_dict_display_name(d, NULL) : "");
  /* ⚠️ `enabled` 是「这本词典现在能不能查」（加载成功），**不是**「这个词查得到」（后者看 `landed`）。
   * 两件事混起来，界面就没法区分「词典坏了」与「这本里没这个词」。 */
  dsh_json_kv_bool(j, "enabled", enabled);
  dsh_json_kv_str(j, "reason", reason);
  dsh_json_object_end(j);

  if (landed != NULL) dsh_release(landed);
  dsh_release(query);
  char *out = take_json(j, "落点");
  if (out == NULL) return DSH_E_OOM;
  *out_json = out;
  return DSH_OK;
}

/* ── dsh_engine_suggest ── */

/** 编辑距离（只需要跟 cap 比大小，用标准 DP，规模很小；超过 64 字符直接放弃） */
static int edit_distance(const char *a, const char *b, int cap) {
  const size_t la = strlen(a);
  const size_t lb = strlen(b);
  if (la > 64 || lb > 64) return cap + 1; /* 太长的直接放弃（不是候选） */
  int prev[65];
  int cur[65];
  for (size_t k = 0; k <= lb; k++) prev[k] = (int)k;
  for (size_t i = 1; i <= la; i++) {
    cur[0] = (int)i;
    int row_min = cur[0];
    for (size_t k = 1; k <= lb; k++) {
      const int cost = (a[i - 1] == b[k - 1]) ? 0 : 1;
      int v = prev[k - 1] + cost;
      if (prev[k] + 1 < v) v = prev[k] + 1;
      if (cur[k - 1] + 1 < v) v = cur[k - 1] + 1;
      cur[k] = v;
      if (v < row_min) row_min = v;
    }
    if (row_min > cap) return cap + 1; /* 剪枝：这一行已经全超了 */
    for (size_t k = 0; k <= lb; k++) prev[k] = cur[k];
  }
  return prev[lb];
}

/** 大小写不敏感相等 */
static int eq_ci(const char *a, const char *b) {
  while (*a != '\0' && *b != '\0') {
    char x = *a, y = *b;
    if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
    if (x != y) return 0;
    a++;
    b++;
  }
  return (*a == '\0' && *b == '\0') ? 1 : 0;
}

/**
 * 联想候选的**唯一实现**（三处调用：`dsh_engine_suggest`、查词通道里的联想那一步、选区落终态页时并进提示的候选）。
 * `dict_id` 给空 = 用当前词典；查词通道那一路给的是**起点那本**（在正文里选中文字时它不一定是当前词典）。
 * 返回 JSON 数组文本（内核分配），失败给 NULL 并写好 last_error。`out_has_exact`（可空）回「有没有精确命中」。
 */
static char *suggest_json(dsh_engine *engine, const char *dict_id, const char *query,
                          int *out_has_exact) {
  if (out_has_exact != NULL) *out_has_exact = 0;
  dsh_json *j = dsh_json_new();
  if (j == NULL) return NULL;

  dsh_json_array_begin(j);
  const int limit = DSH_MAX_LIST_ROWS;

  const char *id = NULL;
  const dsh_stored_dict *d = pick_dict(engine, dict_id, &id);
  if (d != NULL && query[0] != '\0' && dsh_dicts_ensure_loaded(engine, id) == 0) {
    dsh_mdx *mdx = dsh_dicts_peek(engine, id);
    /* 去重按词典里的规范键名，大小写不敏感 */
    char *seen[DSH_MAX_LIST_ROWS];
    int64_t seen_count = 0;
    int64_t emitted = 0;

    /* ① 精确命中（含大小写变体）—— 走与 resolve 同一条解析，所以「联想的第一条」一定与「回车之后的落点」同一个词。 */
    {
      dsh_resolved r;
      dsh_resolve_entry(mdx, query, &r);
      if (r.found && r.key_text != NULL && emitted < limit) {
        seen[seen_count++] = dsh_mem_strdup(r.key_text);
        dsh_json_object_begin(j);
        dsh_json_kv_str(j, "word", r.key_text);
        dsh_json_kv_str(j, "kind", "exact");
        dsh_json_object_end(j);
        emitted++;
        if (out_has_exact != NULL) *out_has_exact = 1;
      }
      dsh_resolved_free(&r);
    }

    /* ② 前缀补全 */
    if (emitted < limit) {
      char **keys = NULL;
      int64_t n = 0;
      if (dsh_mdx_prefix_search(mdx, query, limit * 2, &keys, &n) == 0) {
        for (int64_t i = 0; i < n && emitted < limit; i++) {
          int dup = 0;
          for (int64_t k = 0; k < seen_count; k++) {
            if (eq_ci(seen[k], keys[i])) { dup = 1; break; }
          }
          if (dup) continue;
          seen[seen_count++] = dsh_mem_strdup(keys[i]);
          dsh_json_object_begin(j);
          dsh_json_kv_str(j, "word", keys[i]);
          dsh_json_kv_str(j, "kind", "prefix");
          dsh_json_object_end(j);
          emitted++;
        }
        if (keys != NULL) dsh_mdx_free_keys(keys, n);
      }
    }

    /* ③ 拼写纠正。
     *
     * ⚠️ **单字符查询一律不做这一步**：编辑距离 ≤1 对单字符是**恒真**条件（改一处就等于另一个单字符），
     *    候选又只在一个词块里找，于是退化成「把这个词块里所有单字符词条按块内顺序列出来」。输入一半时该走前缀补全。
     * 候选只能从前缀搜索附近便宜地拿到，所以做法是**保守**的：在同一前缀区间邻近处找距离 ≤ gap 的键。
     * 召回率低于参考实现，但**不会误报**、开销可控。 */
    if (emitted < limit && strlen(query) > 1) {
      const int gap = (strlen(query) <= 4) ? 1 : 2;
      char **keys = NULL;
      int64_t n = 0;
      /* 用首字符当前缀扫一段，再按编辑距离筛 */
      char head[8];
      snprintf(head, sizeof(head), "%c", query[0]);
      if (dsh_mdx_prefix_search(mdx, head, 256, &keys, &n) == 0) {
        for (int64_t i = 0; i < n && emitted < limit; i++) {
          if (edit_distance(keys[i], query, gap) > gap) continue;
          int dup = 0;
          for (int64_t k = 0; k < seen_count; k++) {
            if (eq_ci(seen[k], keys[i])) { dup = 1; break; }
          }
          if (dup) continue;
          seen[seen_count++] = dsh_mem_strdup(keys[i]);
          dsh_json_object_begin(j);
          dsh_json_kv_str(j, "word", keys[i]);
          dsh_json_kv_str(j, "kind", "fuzzy");
          dsh_json_object_end(j);
          emitted++;
        }
        if (keys != NULL) dsh_mdx_free_keys(keys, n);
      }
    }

    for (int64_t k = 0; k < seen_count; k++) {
      if (seen[k] != NULL) dsh_release(seen[k]);
    }
  }
  dsh_json_array_end(j);
  return take_json(j, "联想");
}

enum dsh_error dsh_engine_suggest(dsh_engine *engine, const char *text, char **out_json) {
  if (out_json == NULL) {
    dsh_set_last_error("dsh_engine_suggest：out_json 不能为空");
    return DSH_E_INVALID_ARG;
  }
  *out_json = NULL;
  if (engine == NULL) {
    dsh_set_last_error("dsh_engine_suggest：引擎无效");
    return DSH_E_STATE;
  }
  if (text == NULL) {
    dsh_set_last_error("dsh_engine_suggest：text 不能为空");
    return DSH_E_INVALID_ARG;
  }

  char *query = trim_copy(text);
  if (query == NULL) return DSH_E_OOM;
  /* 接口定义这条只说「用当前词典」，所以 dict_id 传空 */
  char *out = suggest_json(engine, NULL, query, NULL);
  dsh_release(query);
  if (out == NULL) return DSH_E_OOM;
  *out_json = out;
  return DSH_OK;
}

int dsh_suggest_words(struct dsh_engine *e, const char *dict_id, const char *query, int limit,
                      char ***out_words, int64_t *out_count) {
  if (out_words != NULL) *out_words = NULL;
  if (out_count != NULL) *out_count = 0;
  if (out_words == NULL || out_count == NULL || query == NULL) {
    dsh_set_last_error("dsh_suggest_words：参数不能为空");
    return -1;
  }
  if (limit <= 0) return 0;

  char *json = suggest_json(e, dict_id, query, NULL);
  if (json == NULL) return -1;

  char **words = (char **)dsh_mem_alloc((size_t)limit * sizeof(char *));
  if (words == NULL) {
    dsh_release(json);
    dsh_set_last_error("内存不足：候选词清单");
    return -1;
  }
  int64_t count = 0;

  /* 从**同一份** JSON 上读回来：用核内的读取器，**别拿 `strstr` 去猜** —— 键名里有引号或转义就猜错 */
  dsh_json_doc *doc = NULL;
  if (dsh_json_parse(json, strlen(json), &doc) != 0 || doc == NULL) {
    dsh_release(json);
    dsh_release(words);
    dsh_set_last_error("候选 JSON 读不回来（这是内核自己的产物，不该发生）");
    return -1;
  }
  const dsh_json_node *root = dsh_json_doc_root(doc);
  const int64_t n = dsh_json_array_len(root);
  for (int64_t i = 0; i < n && count < limit; i++) {
    const dsh_json_node *item = dsh_json_array_at(root, i);
    const dsh_json_node *word = dsh_json_object_get(item, "word");
    size_t text_len = 0;
    const char *text = dsh_json_str_value(word, &text_len);
    if (text == NULL || text[0] == '\0') continue;
    words[count] = dsh_mem_strdup(text);
    if (words[count] == NULL) {
      dsh_suggest_words_free(words, count);
      dsh_json_doc_free(doc);
      dsh_release(json);
      dsh_set_last_error("内存不足：候选词");
      return -1;
    }
    count++;
  }
  dsh_json_doc_free(doc);
  dsh_release(json);

  if (count == 0) {
    dsh_release(words);
    return 0;
  }
  *out_words = words;
  *out_count = count;
  return 0;
}

void dsh_suggest_words_free(char **words, int64_t count) {
  if (words == NULL) return;
  for (int64_t i = 0; i < count; i++) {
    if (words[i] != NULL) dsh_release(words[i]);
  }
  dsh_release(words);
}

/* ── dsh_engine_probe ── */
typedef struct {
  const char *query;
  dsh_resolved *out;
} probe_ctx;

static int probe_cb(dsh_mdx *mdx, void *ctx) {
  probe_ctx *pc = (probe_ctx *)ctx;
  dsh_resolve_entry(mdx, pc->query, pc->out);
  return 0;
}

enum dsh_error dsh_engine_probe(dsh_engine *engine, const char *text, const char *dict_id,
                                int32_t budget_ms, char **out_json) {
  if (out_json == NULL) {
    dsh_set_last_error("dsh_engine_probe：out_json 不能为空");
    return DSH_E_INVALID_ARG;
  }
  *out_json = NULL;
  if (engine == NULL) {
    dsh_set_last_error("dsh_engine_probe：引擎无效");
    return DSH_E_STATE;
  }
  if (text == NULL || dict_id == NULL || dict_id[0] == '\0') {
    dsh_set_last_error("dsh_engine_probe：text 与 dict_id 都不能为空");
    return DSH_E_INVALID_ARG;
  }
  if (budget_ms <= 0) budget_ms = DSH_PROBE_BUDGET_MS;

  dsh_json *j = dsh_json_new();
  if (j == NULL) return DSH_E_OOM;
  char *query = trim_copy(text);
  if (query == NULL) {
    dsh_json_free(j);
    return DSH_E_OOM;
  }

  const char *status = "ok";
  char reason[256] = "";
  char *landed = NULL;
  const dsh_stored_dict *d = NULL;

  if (query[0] == '\0') {
    status = "missing";
    snprintf(reason, sizeof(reason), "没有要查的文字");
  } else {
    const struct dsh_settings *s = dsh_engine_settings(engine);
    d = (s != NULL) ? dsh_settings_dict_by_id(s, dict_id) : NULL;
    if (d == NULL) {
      status = "missing";
      snprintf(reason, sizeof(reason), "词库里没有这本词典");
    } else {
      const int64_t t0 = dsh_now_ms();
      /* ⚠️ 必须清零：只有 borrow 成功回调（`probe_cb`）才写 r；borrow 失败（词典文件丢失等）时下面仍会走
       * `dsh_resolved_free` —— 不清零就是拿垃圾栈内存去 free（覆盖真实失败原因，极端时误释放）。 */
      dsh_resolved r = {0};
      probe_ctx pc;
      pc.query = query;
      pc.out = &r;
      int loaded = 0;
      const int rc = dsh_dicts_borrow_once(engine, dict_id, probe_cb, &pc, &loaded);
      const int64_t elapsed = dsh_now_ms() - t0;
      (void)loaded;

      if (rc != 0) {
        /* ⚠️ **error 绝不许并成 missing**：读不动就是读不动，说成「这本里没有」会让用户以为词条不存在。 */
        status = "error";
        const char *why = dsh_last_error_message();
        snprintf(reason, sizeof(reason), "%s", why != NULL ? why : "借查失败");
        if (why != NULL) dsh_release((void *)why);
      } else if (!r.found) {
        status = "missing";
        if (r.linked_to != NULL) {
          snprintf(reason, sizeof(reason), "重定向目标「%s」不存在", r.linked_to);
        } else {
          snprintf(reason, sizeof(reason), "这本词典里没有这一条");
        }
      } else {
        landed = dsh_mem_strdup(r.key_text != NULL ? r.key_text : query);
      }
      /* 超预算：**只有真的超了才报**（`budget_ms` 是单本预算）。读路径不会中断（文件是映射的、解析是同步的），
       * 所以这是「事后量到超时」而不是「打断查询」，但结论照旧：那次询问确实没在预算内拿到答案。 */
      if (rc == 0 && r.found && elapsed > budget_ms) {
        status = "timeout";
        snprintf(reason, sizeof(reason), "这本用了 %lld 毫秒（预算 %d 毫秒）",
                 (long long)elapsed, (int)budget_ms);
        if (landed != NULL) {
          dsh_release(landed);
          landed = NULL;
        }
      }
      dsh_resolved_free(&r);
    }
  }

  dsh_json_object_begin(j);
  dsh_json_kv_str(j, "status", status);
  dsh_json_key(j, "landed");
  if (landed != NULL) dsh_json_str(j, landed);
  else dsh_json_null(j);
  dsh_json_kv_str(j, "dictTitle",
                  (d != NULL) ? dsh_settings_dict_display_name(d, NULL) : "");
  dsh_json_kv_str(j, "reason", reason);
  dsh_json_object_end(j);

  if (landed != NULL) dsh_release(landed);
  dsh_release(query);
  char *out = take_json(j, "探路");
  if (out == NULL) return DSH_E_OOM;
  *out_json = out;
  return DSH_OK;
}

/* ══════════════════════════════════════════════════════════════════════════
 * dsh_engine_borrow —— 借查：「去别的词典里问一遍」（终态页那条「再问一遍」）
 *
 * 约定四条：① **跳过起点那一本**；② 一本一本问、**问一句就走**（`dsh_dicts_borrow_once`，那本词典
 * **不许**留在内存里）；③ **总预算**（自动那一次 300 ms；「再问一遍」单本 1500 ms、总预算按词长折算）；
 * ④ ★ **「没问完」绝不许并成「没有」**：预算用完 / `timeout` / `error` 的那几本进 `unconfirmed`；
 * `missing`（这本明确答「没有」）**不算没问完**。
 * ⚠️ 它**不加载**命中的那一本：命中只回「在哪一本、落在哪条」，真正显示由调用方再查一次
 *    （`dsh_engine_lookup` 带明确 dictId）—— 拿没加载的词典直接去查当年会把整个进程打掉。
 * ══════════════════════════════════════════════════════════════════════════ */

/** 自动借查的总预算（毫秒）—— 用户正等着看结果，不能一本一本串着问 */
#define DSH_BORROW_TOTAL_BUDGET_MS 300
/** 「再问一遍」的单本预算：用户刚点了按钮，他要的就是答案，可以等（参考实现 1500）*/
#define DSH_BORROW_RECHECK_PER_DICT_MS 1500
/** 「再问一遍」的总预算 = 词长 × 单本预算 + 这一档固定开销（参考实现同一公式）*/
#define DSH_BORROW_RECHECK_BASE_MS 2000

enum dsh_error dsh_engine_borrow(dsh_engine *engine, const char *text, const char *skip_dict_id,
                                 int32_t recheck, char **out_json) {
  const struct dsh_settings *s;
  const char *skip;
  int64_t per_dict_ms;
  int64_t total_ms;
  int64_t deadline;
  int64_t started;
  int64_t asked = 0;
  int64_t i;
  const char *unconfirmed[DSH_FALLBACK_MAX_UNCONFIRMED];
  int unconfirmed_count = 0;
  char *hit_id = NULL;
  char *hit_title = NULL;
  char *hit_landed = NULL;
  char *query;
  dsh_json *j;

  dsh_clear_last_error();
  if (out_json == NULL) {
    dsh_set_last_error("dsh_engine_borrow：out_json 不能为空");
    return DSH_E_INVALID_ARG;
  }
  *out_json = NULL;
  if (engine == NULL) {
    dsh_set_last_error("dsh_engine_borrow：引擎无效");
    return DSH_E_STATE;
  }
  if (text == NULL || text[0] == '\0') {
    dsh_set_last_error("dsh_engine_borrow：要问的词不能为空");
    return DSH_E_INVALID_ARG;
  }

  query = trim_copy(text);
  if (query == NULL) return DSH_E_OOM;
  if (query[0] == '\0') {
    dsh_release(query);
    dsh_set_last_error("dsh_engine_borrow：要问的词不能只有空白");
    return DSH_E_INVALID_ARG;
  }

  s = dsh_engine_settings(engine);
  /* 跳过哪一本：入参为空 = 「当前词典」；而「当前」那个字段可能是空的 → 兜底取第一本
   * （与 `dsh_speech_gains_api.c` 的 `current_dict` 同一条约定）*/
  skip = skip_dict_id;
  if (skip == NULL || skip[0] == '\0') {
    const char *cid = dsh_settings_current_dict_id(s);
    const dsh_stored_dict *d = (cid != NULL && cid[0] != '\0') ? dsh_settings_dict_by_id(s, cid) : NULL;
    if (d == NULL) d = dsh_settings_dict_at(s, 0);
    skip = (d != NULL) ? d->id : NULL;
  }

  per_dict_ms = (recheck != 0) ? DSH_BORROW_RECHECK_PER_DICT_MS : DSH_PROBE_BUDGET_MS;
  total_ms = (recheck != 0)
                 ? (int64_t)strlen(query) * DSH_BORROW_RECHECK_PER_DICT_MS + DSH_BORROW_RECHECK_BASE_MS
                 : DSH_BORROW_TOTAL_BUDGET_MS;
  started = dsh_now_ms();
  deadline = started + total_ms;

  for (i = 0; i < dsh_settings_dict_count(s); i++) {
    const dsh_stored_dict *d = dsh_settings_dict_at(s, i);
    char *probe_json = NULL;
    char *landed = NULL;
    char status[16];
    const char *title;
    enum dsh_error rc;
    dsh_json_doc *doc = NULL;

    status[0] = '\0';
    if (d == NULL || d->id == NULL) continue;
    if (skip != NULL && strcmp(d->id, skip) == 0) continue; /* 起点那一本不问 */
    title = dsh_settings_dict_display_name(d, NULL);

    /*
     * ★ 预算检查放在**每一本之前**：已经没时间了就别再开一本 —— 剩下的**如实报成「没问完」**，不许当成「没有」。
     */
    if (dsh_now_ms() >= deadline) {
      if (unconfirmed_count < DSH_FALLBACK_MAX_UNCONFIRMED) {
        unconfirmed[unconfirmed_count++] = title;
      }
      continue;
    }

    rc = dsh_engine_probe(engine, query, d->id, (int32_t)per_dict_ms, &probe_json);
    if (rc != DSH_OK || probe_json == NULL || dsh_json_parse(probe_json, strlen(probe_json), &doc) != 0) {
      /* 读不动 / 问不出：**这一本没问完**（不是「它没有」）*/
      if (unconfirmed_count < DSH_FALLBACK_MAX_UNCONFIRMED) {
        unconfirmed[unconfirmed_count++] = title;
      }
      if (probe_json != NULL) dsh_release(probe_json);
      continue;
    }
    asked++;
    {
      const dsh_json_node *root = dsh_json_doc_root(doc);
      const dsh_json_node *landed_node = dsh_json_object_get(root, "landed");
      const char *status_in = dsh_json_str_value(dsh_json_object_get(root, "status"), NULL);
      /*
       * ★ `status` 是从**这份 doc 的 arena 里借来**的指针，必须在 `dsh_json_doc_free` 之前抄进自己的缓冲 ——
       * 第一版没抄，「free 之后再拿它比字符串」当场被 ASan 抓住。（`landed` 不受影响：它本来就另拷了一份。）
       */
      if (status_in != NULL) {
        size_t n = strlen(status_in);
        if (n >= sizeof(status)) n = sizeof(status) - 1;
        memcpy(status, status_in, n);
        status[n] = '\0';
      }
      if (landed_node != NULL) {
        size_t len = 0;
        const char *value = dsh_json_str_value(landed_node, &len);
        if (value != NULL) {
          landed = (char *)dsh_mem_alloc(len + 1);
          if (landed != NULL) {
            memcpy(landed, value, len);
            landed[len] = '\0';
          }
        }
      }
    }
    dsh_json_doc_free(doc);
    dsh_release(probe_json);

    if (status[0] != '\0' && strcmp(status, "ok") == 0 && landed != NULL && landed[0] != '\0') {
      /* 命中：记下「在哪一本、落在哪条」，**不加载**（那是调用方的事）*/
      hit_id = dsh_mem_strdup(d->id);
      hit_title = dsh_mem_strdup(title);
      hit_landed = landed;
      landed = NULL;
      break;
    }
    if (status[0] != '\0' && (strcmp(status, "timeout") == 0 || strcmp(status, "error") == 0)) {
      /* 超预算 / 读出错：这一本没问完 */
      if (unconfirmed_count < DSH_FALLBACK_MAX_UNCONFIRMED) {
        unconfirmed[unconfirmed_count++] = title;
      }
    }
    /* `missing`（这本明确答「没有」、或清单里已经没它了）→ **接着问下一本**（不算没问完） */
    if (landed != NULL) dsh_release(landed);
    landed = NULL;
  }

  j = dsh_json_new();
  if (j == NULL) {
    if (hit_id != NULL) dsh_release(hit_id);
    if (hit_title != NULL) dsh_release(hit_title);
    if (hit_landed != NULL) dsh_release(hit_landed);
    dsh_release(query);
    dsh_set_last_error("内存不足：借查结果");
    return DSH_E_OOM;
  }
  dsh_json_object_begin(j);
  dsh_json_kv_str(j, "hitId", (hit_id != NULL) ? hit_id : "");
  dsh_json_kv_str(j, "hitTitle", (hit_title != NULL) ? hit_title : "");
  dsh_json_kv_str(j, "hitLanded", (hit_landed != NULL) ? hit_landed : "");
  dsh_json_key(j, "unconfirmed");
  dsh_json_array_begin(j);
  for (i = 0; i < unconfirmed_count; i++) {
    dsh_json_str(j, unconfirmed[i] != NULL ? unconfirmed[i] : "");
  }
  dsh_json_array_end(j);
  dsh_json_kv_i64(j, "asked", asked);
  dsh_json_kv_i64(j, "perDictMs", per_dict_ms);
  dsh_json_kv_i64(j, "totalMs", total_ms);
  dsh_json_kv_i64(j, "elapsedMs", dsh_now_ms() - started);
  dsh_json_object_end(j);

  if (hit_id != NULL) dsh_release(hit_id);
  if (hit_title != NULL) dsh_release(hit_title);
  if (hit_landed != NULL) dsh_release(hit_landed);
  dsh_release(query);

  {
    char *out = take_json(j, "借查");
    if (out == NULL) return DSH_E_OOM;
    *out_json = out;
  }
  return DSH_OK;
}

/* ══════════════════════════════════════════════════════════════════════════
 * dsh_engine_lookup —— 完整查询：一条链走到底
 *
 * **驱动器**：决策在 `engine/dsh_fallback.c`（纯表，单独验过），这里只负责「按结论去干活」，
 * 并把结果包成接口定义里那个 EntryPayload 形状。三条纪律：
 *  ① **决策与执行分开** —— 这里不许再出现「什么情况该借查」这种判断，否则表里验过的约定与真跑起来的各说各话；
 *  ② **`script` 只许来自 `dsh_script_dominant`**（「什么算汉字」一处来源）；
 *  ③ **不许抛、不许装作成功** —— 加载不了就如实说加载不了（`via=terminal` + 那句话）。
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * `uri_escape` 那一段已收到 `dsh_text_uri_escape`（`text/dsh_textutil.c`）：这里原来自己带了一份，
 * 与 `dict/dsh_entry_doc.c` 那份逐字相同 —— 两份重复实现改一处漏一处。
 */

/**
 * 词条正文文档的地址（直接赋给 `iframe.src` 就行）：`https://<词典 id>.dictres.invalid/__entry__?word=<转义键名>`。
 * 词典 id 是内容哈希 ⇒ 同一本词典在任何机器上都是同一个源，**内核不需要**知道任何真实路径。
 */

static char *entry_url_for(const char *dict_id, const char *word) {
  char *escaped = dsh_text_uri_escape(word);
  if (escaped == NULL) {
    dsh_set_last_error("内存不足：转义词条名");
    return NULL;
  }
  const size_t need = strlen("https://") + strlen(dict_id ? dict_id : "") +
                      strlen(".dictres.invalid/__entry__?word=") + strlen(escaped) + 1;
  char *out = (char *)dsh_mem_alloc(need);
  if (out == NULL) {
    dsh_release(escaped);
    dsh_set_last_error("内存不足：词条地址");
    return NULL;
  }
  snprintf(out, need, "https://%s.dictres.invalid/__entry__?word=%s",
           dict_id ? dict_id : "", escaped);
  dsh_release(escaped);
  return out;
}

/** 借查问出来的一条（正文自己拿着，借的那本已经关掉了） */
typedef struct {
  const char *query; /* 借用（调用方那个 query）——**不许**塞进 resolved 里：见下 */
  dsh_resolved resolved;
} probe_hit;

/**
 * 借查的回调。
 * ⚠️ 借用指针（查询词）**不许**放进「会被 free 的结构」里：打不开那本词典时这个回调**根本不会被调用**，
 *    `hit->resolved` 里留下的借用指针会被后面的 `dsh_resolved_free` 释放掉 —— 堆释放后再用。
 */
static int probe_chain_cb(dsh_mdx *mdx, void *ctx) {
  probe_hit *hit = (probe_hit *)ctx;
  dsh_resolve_entry(mdx, hit->query, &hit->resolved);
  return 0;
}

/** 数一数联想 JSON 里有几条候选（`kind` 字段出现几次）—— 只用来判「有没有候选」 */
static int count_suggest_rows(const char *json) {
  if (json == NULL) return 0;
  int rows = 0;
  for (const char *p = json; (p = strstr(p, "\"kind\":")) != NULL; p++) rows++;
  return rows;
}

enum dsh_error dsh_engine_lookup(dsh_engine *engine, const char *text, enum dsh_origin origin,
                                 const char *dict_id, char **out_json) {
  if (out_json == NULL) {
    dsh_set_last_error("dsh_engine_lookup：out_json 不能为空");
    return DSH_E_INVALID_ARG;
  }
  *out_json = NULL;
  if (engine == NULL) {
    dsh_set_last_error("dsh_engine_lookup：引擎无效");
    return DSH_E_STATE;
  }
  /* ⚠️ 接口定义写的是「text 为空则改看 selection 的现值」—— 但**选中内容住在宿主那边**（内核没有「当前选区」这种状态）。
   *    所以宿主必须把选中文字显式传进来；这里对 NULL 与 resolve/suggest 一视同仁地报错。 */
  if (text == NULL) {
    dsh_set_last_error("dsh_engine_lookup：text 不能为空（选中内容由宿主传进来）");
    return DSH_E_INVALID_ARG;
  }

  const struct dsh_settings *s = dsh_engine_settings(engine);
  if (s == NULL) {
    dsh_set_last_error("dsh_engine_lookup：引擎里没有设置");
    return DSH_E_STATE;
  }

  char *query = trim_copy(text);
  if (query == NULL) return DSH_E_OOM;

  /* ── 起点那本：`dict_id` 给空就用当前词典 ── */
  const char *start_id = (dict_id != NULL && dict_id[0] != '\0') ? dict_id
                                                                 : dsh_settings_current_dict_id(s);
  const dsh_stored_dict *start = (start_id != NULL) ? dsh_settings_dict_by_id(s, start_id) : NULL;
  const char *current_id = dsh_settings_current_dict_id(s);

  char reason_fallback[256] = "";
  char *landed = NULL;       /* 落点的规范键名（本函数所有） */
  char *definition = NULL;   /* 落点的正文（本函数所有） */
  size_t definition_len = 0;
  char *linked_to = NULL;    /* 跟过 @@@LINK 的话，记录重定向目标 */
  char *suggestions = NULL;  /* 联想那一步的 JSON 数组文本（内核分配） */
  int suggestion_rows = 0;
  int reached_stage = DSH_STAGE_START;
  const char *shown_dict_id = NULL; /* 正文是哪一本给的（借用 settings 里的串） */
  const dsh_stored_dict *shown = NULL;
  /*
   * 这一次的落点**就是界面正在显示的那条**（只对选区 / 链接那两条路有意义）。判法与出处见写 JSON 那一段。
   */
  int same_as_shown = 0;
  enum dsh_error result_error = DSH_OK;
  /* 不支持翻译时那句人话（本函数分配 → 收尾时还掉；`facts` 只借用它）*/
  char *unsupported_sentence = NULL;
  /* 这一条链走到了「该翻译」那一档（外壳据此把翻译做完，见 DSH_FALLBACK_TRANSLATE）*/
  int needs_translate = 0;

  /* 查词通道跑到的那个阶段：`stage` 出参说的就是「这一份答复是哪一步出来的」 */
  dsh_fallback_facts facts;
  memset(&facts, 0, sizeof(facts));
  facts.origin = origin;
  facts.stage = DSH_STAGE_START;
  /* ⚠️ 字形判定的**唯一来源**是内核的 `dsh_script_of`（它在 text/dsh_language.c 里，
   *    汉字区间的字面量只有那一处）—— 决策表自己一个字都不判汉字
   *    （测试 ⑦ 那几条检查标准在盯着这件事）。 */
  facts.script = dsh_script_of(query, strlen(query));
  facts.started_from_current =
      (start_id != NULL && current_id != NULL && strcmp(start_id, current_id) == 0) ? 1 : 0;
  facts.start_dict_title =
      (start != NULL) ? dsh_settings_dict_display_name(start, NULL) : start_id;
  /* 用户选中/敲进去的那个词（选区那句提示要拿它数字数，见 `dsh_fallback.h` 那一格）*/
  facts.query = query;
  /*
   * ★ **这个词里有没有音节分隔点**：有些词典把词显示成 `pro·gress`，整条选中来查时那串点跟着进了查询，
   * 而词典里没有这个键，于是白白落到借查 / 机器翻译。决策表拿这一格决定「要不要去掉点再问一遍」（`RELOOKUP`）。
   */
  facts.has_separator_dots = dsh_text_has_separator_dots(query);
  /*
   * ── 翻译那四个事实（参考实现 `App.FillTranslateFacts`）──
   * 四个条件**全满足**才自动翻译（终态页那句「为什么没翻译」也照四条分开说）：① `translate.enabled`（默认关，
   * 要联网、要把文本发给第三方）② `translate.autoTranslate`（默认开）③ 填了 Key（与语音共用）④ 语种支持。
   * 检查标准只有一处：`dsh_translate_language()` 回不回 NULL；不支持时那句话也由内核给（界面一个字都不拼）。
   */
  {
    const dsh_translate_settings *tr = dsh_settings_translate(s);
    const char *key = dsh_settings_volcengine_api_key(s);
    const char *tag = NULL;
    const char *basis = NULL;
    const char *basis_text = NULL;
    facts.translate_enabled = (tr != NULL) ? tr->enabled : 0;
    facts.auto_translate = (tr != NULL) ? tr->auto_translate : 1;
    facts.translate_has_key = (key != NULL && key[0] != '\0') ? 1 : 0;
    /* 源语种用**同一个判定**（发音与翻译共用的那一个），再问支持表 */
    dsh_language_decide(query, facts.start_dict_title, NULL, NULL, &tag, &basis, &basis_text);
    if (tag != NULL && tag[0] != '\0') {
      facts.translate_supported = (dsh_translate_language(tag) != NULL) ? 1 : 0;
      if (!facts.translate_supported) {
        /* 那句人话（`机器翻译暂不支持〈波斯语〉`）—— 名字同样问内核的语种表 */
        const char *name = dsh_language_name(tag);
        const size_t need = strlen(name) + 48;
        char *sentence = (char *)dsh_mem_alloc(need);
        if (sentence != NULL) {
          snprintf(sentence, need, "机器翻译暂不支持〈%s〉", (name[0] != '\0') ? name : "这种语言");
          unsupported_sentence = sentence; /* 本函数所有，收尾时还掉 */
          facts.translate_unsupported_message = sentence;
        }
      }
    } else {
      /* 语种都认不出：**不挡路**（让服务端去认，比在这儿猜强）*/
      facts.translate_supported = 1;
    }
    (void)basis;
    (void)basis_text;
  }

  dsh_fallback_result plan;
  memset(&plan, 0, sizeof(plan));
  const char *final_via = "current";
  char *final_reason = NULL; /* 本函数所有 */
  int final_offer_translate = 0;
  int final_offer_recheck = 0;
  char *final_translate_why = NULL;
  char *final_unconfirmed_note = NULL;
  const char *final_surface = "none";
  const char *unconfirmed_names[DSH_FALLBACK_MAX_UNCONFIRMED];
  int unconfirmed_count = 0;
  int terminal_reached = 0;

  /* 起点那本加载不上：**如实说这一本读不动**，不许当成「这本里没有这个词」（后者会让用户以为词条不存在）。 */
  if (query[0] == '\0') {
    snprintf(reason_fallback, sizeof(reason_fallback), "没有要查的文字");
    final_reason = dsh_mem_strdup(reason_fallback);
    final_via = "terminal";
    terminal_reached = 1;
  } else if (start == NULL) {
    /* 指了一本词库里没有的 / 根本没指定当前词典 —— 两件事分开说 */
    char buf[320];
    if (dict_id != NULL && dict_id[0] != '\0') {
      /* ⚠️ 这句**给用户看**（`reason` 原样上屏），所以**不带 id** —— 一串 64 位哈希只会更迷惑。
       *    「哪一本、为什么、怎么恢复」那套在 `engine.dictList` 的 `unavailable` / `note` 里，返回那条路先问它；
       *    这一句是兜底：那一本已被移出词库时，只有它是准的。 */
      snprintf(buf, sizeof(buf), "词库里没有这本词典");
    } else {
      snprintf(buf, sizeof(buf), "还没指定当前词典");
    }
    final_reason = dsh_mem_strdup(buf);
    final_via = "terminal";
    terminal_reached = 1;
  } else if (dsh_dicts_ensure_loaded(engine, start_id) != 0) {
    const char *why = dsh_last_error_message();
    char buf[512];
    snprintf(buf, sizeof(buf), "《%s》加载不了：%s",
             dsh_settings_dict_display_name(start, NULL), why != NULL ? why : "");
    if (why != NULL) dsh_release((void *)why);
    final_reason = dsh_mem_strdup(buf);
    final_via = "terminal";
    terminal_reached = 1;
  }

  /* ── 跑链（每一轮：问一次决策表，做一件事，再问一次）；
   *    上限只是防呆：正常的链最多四步（联想 / 查 / 借查 / 收尾）。 */
  for (int guard = 0; !terminal_reached && guard < DSH_MAX_LIST_ROWS + 8; guard++) {
    /* ⚠️ 每次决策**先还上一轮那份结论**：`dsh_fallback_decide` 开头会清空出参，直接再问一次就把上一轮
     *    拼出来的说明句丢在原地（那是内核分配的）—— 表现为活分配表只涨不落。 */
    dsh_fallback_result_dispose(&plan);
    dsh_fallback_decide(&facts, &plan);
    if (plan.oom) {
      result_error = DSH_E_OOM;
      break;
    }
    reached_stage = facts.stage;

    switch (plan.action) {
      case DSH_FALLBACK_SUGGEST: {
        if (suggestions == NULL) {
          suggestions = suggest_json(engine, start_id, query, &facts.suggestion_has_exact);
          if (suggestions == NULL) {
            result_error = DSH_E_OOM;
            break;
          }
          suggestion_rows = count_suggest_rows(suggestions);
        }
        facts.suggestion_count = suggestion_rows;
        if (!plan.stop) {
          facts.stage = DSH_STAGE_AFTER_SUGGEST;
          continue;
        }
        /* 停下等用户选（输入框那条路）/ 把候选并进正文框底部提示（选区那条路）*/
        final_via = plan.via;
        final_surface = (plan.surface == DSH_SURFACE_LIST) ? "list" : "toast";
        final_reason = dsh_mem_strdup(plan.reason != NULL ? plan.reason : "");
        final_offer_recheck = plan.offer_recheck;
        for (int i = 0; i < plan.unconfirmed_count; i++) {
          unconfirmed_names[i] = plan.unconfirmed_names[i];
        }
        unconfirmed_count = plan.unconfirmed_count;
        if (plan.unconfirmed_note != NULL) final_unconfirmed_note = dsh_mem_strdup(plan.unconfirmed_note);
        terminal_reached = 1;
        break;
      }

      case DSH_FALLBACK_LOOKUP: {
        /* 在当前词典（= 起点那本）里问落点。用的是与 resolve 完全同一条解析。 */
        dsh_resolved r;
        dsh_mdx *mdx = dsh_dicts_peek(engine, start_id);
        dsh_resolve_entry(mdx, query, &r);
        facts.entry_found = r.found;
        if (r.found) {
          landed = dsh_mem_strdup(r.key_text != NULL ? r.key_text : query);
          if (r.definition != NULL) {
            definition = (char *)dsh_mem_alloc(r.definition_len + 1);
            if (definition != NULL) {
              memcpy(definition, r.definition, r.definition_len);
              definition[definition_len = r.definition_len] = '\0';
            }
          }
          /* ⚠️ 先还旧值再接新值：链允许 LOOKUP 走第二次（RELOOKUP → START），不还就泄漏上一轮那份 strdup。 */
          if (linked_to != NULL) dsh_release(linked_to);
          linked_to = (r.linked_to != NULL) ? dsh_mem_strdup(r.linked_to) : NULL;
          shown_dict_id = start_id;
          shown = start;
        } else if (r.linked_to != NULL) {
          if (linked_to != NULL) dsh_release(linked_to);
          linked_to = dsh_mem_strdup(r.linked_to);
        }
        dsh_resolved_free(&r);
        facts.stage = DSH_STAGE_AFTER_LOOKUP;
        reached_stage = facts.stage;
        continue;
      }

      case DSH_FALLBACK_RELOOKUP: {
        /*
         * ★ **去掉音节分隔点、用新词再来一遍**：`pro·gress` 整条选中来查时那串点会跟着进查询，于是白落借查/翻译。
         * `query` 是整条链用的那个词，换掉它并重算语种；然后 `stage = START` 回起点再走一遍（先原样问、问不到才去点）。
         * ⚠️ 整段都是点时不动 query；两档都置 `separator_retried`，否则下一轮还会给同一条动作（来回重问）。
         * ⚠️ 翻译那几格**故意不重算** —— 约定照参考实现：它拿的是**原文本**（在去点之前）。
         */
        char *stripped = dsh_text_strip_separator_dots(query);
        if (stripped == NULL) {
          result_error = DSH_E_OOM;
          break;
        }
        if (stripped[0] != '\0') {
          dsh_release(query); /* 旧的 `query` 是本函数分配的（`trim_copy`）*/
          query = stripped;
          stripped = NULL;
          facts.query = query;
          facts.script = dsh_script_of(query, strlen(query));
        }
        if (stripped != NULL) dsh_release(stripped);
        facts.has_separator_dots = 0;
        facts.separator_retried = 1;
        facts.stage = DSH_STAGE_START;
        reached_stage = facts.stage;
        continue;
      }

      case DSH_FALLBACK_PROBE: {
        /* 去问**别的**词典：一本一本借（问完就关），谁先有就停。 */
        const int64_t budget_ms = DSH_PROBE_BUDGET_MS;
        for (int64_t i = 0; i < dsh_settings_dict_count(s) && facts.hit_dict_id == NULL; i++) {
          const dsh_stored_dict *d = dsh_settings_dict_at(s, i);
          if (d == NULL || d->id == NULL || strcmp(d->id, start_id) == 0) continue;
          probe_hit hit;
          memset(&hit, 0, sizeof(hit));
          hit.query = query;
          const int64_t t0 = dsh_now_ms();
          int loaded = 0;
          const int rc = dsh_dicts_borrow_once(engine, d->id, probe_chain_cb, &hit, &loaded);
          const int64_t elapsed = dsh_now_ms() - t0;
          const int timed_out = (rc == 0 && elapsed > budget_ms) ? 1 : 0;
          if (rc != 0 || timed_out) {
        /* ⚠️ 没问完绝不许并成「没有」（那个坑）。名字原样记下来。 */
            if (facts.unconfirmed_count < DSH_FALLBACK_MAX_UNCONFIRMED) {
              facts.unconfirmed[facts.unconfirmed_count++] =
                  dsh_settings_dict_display_name(d, NULL);
            }
          } else if (hit.resolved.found) {
            landed = dsh_mem_strdup(hit.resolved.key_text != NULL ? hit.resolved.key_text : query);
            if (hit.resolved.definition != NULL) {
              definition = (char *)dsh_mem_alloc(hit.resolved.definition_len + 1);
              if (definition != NULL) {
                memcpy(definition, hit.resolved.definition, hit.resolved.definition_len);
                definition[definition_len = hit.resolved.definition_len] = '\0';
              }
            }
            /* 同上：LOOKUP 那格可能已记了一份 dangling 重定向，先还再接。 */
            if (linked_to != NULL) dsh_release(linked_to);
            linked_to = (hit.resolved.linked_to != NULL) ? dsh_mem_strdup(hit.resolved.linked_to)
                                                         : NULL;
            facts.hit_dict_id = d->id;
            facts.hit_dict_title = dsh_settings_dict_display_name(d, NULL);
            facts.hit_is_current =
                (current_id != NULL && strcmp(d->id, current_id) == 0) ? 1 : 0;
            shown_dict_id = d->id;
            shown = d;
          }
          dsh_resolved_free(&hit.resolved);
        }
        /*
         * ⚠️ 选区那一路都没命中时，要把候选**并进正文框底部提示**（`surface = toast`），
         * 那需要候选条数，所以这里顺手算一次；输入框那一路在 afterSuggest 已经算过了。
         */
        if (facts.hit_dict_id == NULL && origin == DSH_ORIGIN_SELECTION &&
            suggestions == NULL) {
          /* 选区那一路只要**条数**（把候选并进正文框底部提示）；精确命中那格用不上。 */
          suggestions = suggest_json(engine, start_id, query, NULL);
          if (suggestions == NULL) {
            result_error = DSH_E_OOM;
            break;
          }
          suggestion_rows = count_suggest_rows(suggestions);
        }
        facts.suggestion_count = suggestion_rows;
        facts.stage = DSH_STAGE_AFTER_PROBE;
        reached_stage = facts.stage;
        continue;
      }

      case DSH_FALLBACK_SHOW:
        final_via = plan.via;
        final_reason = dsh_mem_strdup(plan.reason != NULL ? plan.reason : "");
        final_offer_recheck = plan.offer_recheck;
        if (plan.unconfirmed_note != NULL) {
          final_unconfirmed_note = dsh_mem_strdup(plan.unconfirmed_note);
        }
        for (int i = 0; i < plan.unconfirmed_count; i++) {
          unconfirmed_names[i] = plan.unconfirmed_names[i];
        }
        unconfirmed_count = plan.unconfirmed_count;
        if (plan.dict_id != NULL) {
          const dsh_stored_dict *d = dsh_settings_dict_by_id(s, plan.dict_id);
          if (d != NULL) {
            shown = d;
            shown_dict_id = d->id;
          }
        }
        /*
         * ⚠️ `stage` 说的是**哪一步给出了这份答复**：当前词典命中 = `afterLookup`、借查命中 = `afterProbe`；
         * `done` 专门留给「链根本没跑」（链接/回退/历史的精确还原、以及出错说明）。
         */
        reached_stage = facts.stage;
        terminal_reached = 1;
        break;

      case DSH_FALLBACK_EXPLAIN_ERROR:
        final_via = plan.via != NULL ? plan.via : "terminal";
        final_reason = dsh_mem_strdup(plan.reason != NULL ? plan.reason : "");
        reached_stage = DSH_STAGE_DONE;
        terminal_reached = 1;
        break;

      case DSH_FALLBACK_TERMINAL:
        final_via = "terminal";
        /*
         * ⚠️ `surface` **三档都要映射对**：`list` = 摆候选列表（没打完的半个词）；
         * `toast` = 正文框底部一条提示、**不替换正文**（选区那条路的终态）；`none` = 整页（输入框那条路）。
         * ⚠️ 这一格漏写，选区那条路的终态会报 `none`，界面就把正在读的正文换成一张「没找到」的页。
         */
        final_surface = (plan.surface == DSH_SURFACE_TOAST)
                            ? "toast"
                            : ((plan.surface == DSH_SURFACE_LIST) ? "list" : "none");
        final_reason = dsh_mem_strdup(plan.reason != NULL ? plan.reason : "");
        final_translate_why =
            dsh_mem_strdup(plan.translate_why != NULL ? plan.translate_why : "");
        final_offer_translate = plan.offer_translate;
        final_offer_recheck = plan.offer_recheck;
        if (plan.unconfirmed_note != NULL) {
          final_unconfirmed_note = dsh_mem_strdup(plan.unconfirmed_note);
        }
        for (int i = 0; i < plan.unconfirmed_count; i++) {
          unconfirmed_names[i] = plan.unconfirmed_names[i];
        }
        unconfirmed_count = plan.unconfirmed_count;
        reached_stage = facts.stage; /* 终态页是「借查问完之后」那一步给出来的 */
        terminal_reached = 1;
        break;

      case DSH_FALLBACK_TRANSLATE:
        /*
         * ★ 走到这一档 = **四个条件全满足**（总开关 + 自动翻译 + 填了 Key + 语种支持），所以该翻译这个词。
         * 这一条**不查词、也不翻**（内核零依赖、没有 socket）：它只把「该翻译了」如实报出去（`via = translate`
         * + `needsTranslate`），外壳走 `translate:text` 那条路（plan → HTTP → accept）再拼回同形状载荷。
         * ⚠️ `reason` 留空（「不该走到终态页」那条约定）；`found` 为假是暂时的 —— 外壳补上译文之后就是真。
         */
        final_via = "translate";
        needs_translate = 1;
        reached_stage = facts.stage;
        terminal_reached = 1;
        break;

      default:
        dsh_set_last_error("dsh_engine_lookup：决策表给了一个不认识的动作");
        result_error = DSH_E_STATE;
        break;
    }
    if (result_error != DSH_OK) break;
  }

  /* ── 包 EntryPayload ── */
  char *out = NULL;
  /* 终态页那排出路按钮（文字也由内核拼）—— 见下面 `dsh_fallback_chips` 那一段 */
  dsh_chip chips[DSH_FALLBACK_MAX_CHIPS];
  int chip_count = 0;
  memset(chips, 0, sizeof(chips));
  if (result_error == DSH_OK) {
    /*
     * ⚠️ **整句解释行由内核拼好**：「另有 N 本没能确认（《A》《B》）」是产品约定、不是视图，
     * 所以并进 `reason`，界面直接照抄 —— 一个字的判断都不用做。
     * `unconfirmed[]` 仍照给（`#reader[data-unconfirmed]` 要的是名字清单，不是写给人看的那句话）。
     */
    char *reason_full = NULL;
    if (final_unconfirmed_note != NULL && final_unconfirmed_note[0] != '\0') {
      const char *base = (final_reason != NULL) ? final_reason : "";
      const size_t need = strlen(base) + strlen(final_unconfirmed_note) + 4;
      reason_full = (char *)dsh_mem_alloc(need);
      if (reason_full == NULL) {
        result_error = DSH_E_OOM;
      } else if (base[0] == '\0') {
        snprintf(reason_full, need, "%s", final_unconfirmed_note);
      } else {
        snprintf(reason_full, need, "%s · %s", base, final_unconfirmed_note);
      }
    } else {
      reason_full = dsh_mem_strdup(final_reason != NULL ? final_reason : "");
      if (reason_full == NULL) result_error = DSH_E_OOM;
    }
    if (result_error != DSH_OK) {
      if (reason_full != NULL) dsh_release(reason_full);
    } else {
      /*
       * 终态页那排出路按钮：**连按钮上那行字一起由内核给**（`dsh_fallback_chips` 顶上那段）。
       * 条件照参考实现：**只有终态页**（调用方按 `via` 是 `terminal` 判）、**没查到**、而且**有词**。
       */
      const int is_terminal = (final_via != NULL && strcmp(final_via, "terminal") == 0);
      if (is_terminal && landed == NULL && query[0] != '\0') {
        chip_count = dsh_fallback_chips(&facts, final_offer_recheck, final_offer_translate,
                                        query, chips, DSH_FALLBACK_MAX_CHIPS);
        /* 该给按钮却一条都没拼出来 = 内存不足（`dsh_fallback_chips` 已经写好 last_error） */
        if (chip_count == 0 && (final_offer_recheck || final_offer_translate)) {
          result_error = DSH_E_OOM;
        }
      }
      if (result_error != DSH_OK) {
        dsh_fallback_chips_free(chips, chip_count);
        chip_count = 0;
        if (reason_full != NULL) dsh_release(reason_full);
      } else {
        dsh_json *j = dsh_json_new();
        if (j == NULL) {
          result_error = DSH_E_OOM;
          dsh_fallback_chips_free(chips, chip_count);
          chip_count = 0;
          if (reason_full != NULL) dsh_release(reason_full);
        } else {
        char *plain = NULL;
        if (definition != NULL) {
          size_t plain_len = 0;
          plain = dsh_html_strip(definition, definition_len, &plain_len);
          if (plain == NULL) {
            dsh_json_free(j);
            result_error = DSH_E_OOM;
          }
        }
        /*
         * ⚠️ **「没打完的半个词」那一档例外**（`afterSuggest` + `list`）：那时**根本没查过这个词**，摆候选列表就是终点。
         * 这一档若也给出提示页地址，正文框会被换成一张「未在《…》中找到 appl」—— 所以 `keyText` / `entryUrl` 照旧是空。
         */
        const int stopping_at_suggest = (reached_stage == DSH_STAGE_AFTER_SUGGEST) ? 1 : 0;
        const dsh_stored_dict *owner = NULL;
        const char *owner_id = NULL;
        if (landed != NULL) {
          owner = shown;
          owner_id = shown_dict_id;
        } else if (!stopping_at_suggest && start != NULL && query[0] != '\0') {
          owner = start;
          owner_id = start->id;
        }
        const char *title = (owner != NULL) ? dsh_settings_dict_display_name(owner, NULL)
                                            : (owner_id != NULL ? owner_id : "");
        /*
         * ★ 落点**就是界面正在显示的那条词条**（只对选区 / 链接那两条路）：比的是**解析之后的落点**与引擎记着的
         * `shown_key` / `shown_dict_id`，**不许**拿输入文字去比（`apples` → `@@@LINK=apple` 字面不等而同一条）。
         * ⚠️ 属产品约定，由内核判（硬规则 1）；判错当年会压一层假返回栈、并把正文重载回顶部。
         * 结果：`sameAsShown: true` + `entryUrl` 为空 + `reason` 是那句人话；界面一个字都不动正文，`found` 仍为真。
         */
        {
          const int walking = (origin == DSH_ORIGIN_SELECTION || origin == DSH_ORIGIN_LINK);
          const char *was_key = dsh_engine_shown_key(engine);
          const char *was_id = dsh_engine_shown_dict_id(engine);
          same_as_shown = (walking && landed != NULL && owner_id != NULL && was_key != NULL &&
                           was_id != NULL && strcmp(was_key, landed) == 0 &&
                           strcmp(was_id, owner_id) == 0)
                              ? 1
                              : 0;
        }
        /*
         * `sameAsShown` 那句人话（「X」就是当前词条（Y））在这里先拼好：写 JSON 那一步再分配失败就得收掉建了一半的 `j`，
         * 这里是能体面退出的位置。引号用「」与内核别处一致。
         */
        char *shown_note = NULL;
        if (result_error == DSH_OK && same_as_shown) {
          const size_t note_need = strlen(query) + strlen(landed) + 64;
          shown_note = (char *)dsh_mem_alloc(note_need);
          if (shown_note == NULL) {
            dsh_json_free(j);
            result_error = DSH_E_OOM;
          } else {
            snprintf(shown_note, note_need, "「%s」就是当前词条（%s）", query, landed);
          }
        }
        if (result_error == DSH_OK) {
          /*
           * ⚠️ **没命中也给全 `dictId` / `dictTitle` / `keyText` / `entryUrl` 这四个**（载荷形状与命中时同）——
           * 只是要 `owner_id` 非空：查不到时正文框要导航到 `?word=…` 那张提示页，页上的 `entry://` 候选可点。
           * `owner` 为空时退回起点那一本。`stopping_at_suggest` 那一档（没打完的半个词）例外，见上。
           */
          dsh_json_object_begin(j);
          dsh_json_kv_str(j, "query", query);
          dsh_json_kv_str(j, "keyText",
                          landed != NULL ? landed : (owner_id != NULL ? query : ""));
          dsh_json_kv_str(j, "dictId", owner_id != NULL ? owner_id : "");
          dsh_json_kv_str(j, "dictTitle", title);
          {
            /*
             * ⚠️ `sameAsShown` 时**不给地址**（空地址 = 没有要跳的地方）。
             * 给了地址，界面那条「地址与现在相同就加 `&t=` 强制重载」的逻辑会把同一篇正文重载一遍。
             */
            const char *url_word = (landed != NULL) ? landed : query;
            char *url = (!same_as_shown && owner_id != NULL && url_word[0] != '\0')
                            ? entry_url_for(owner_id, url_word)
                            : NULL;
            dsh_json_kv_str(j, "entryUrl", url != NULL ? url : "");
            if (url != NULL) dsh_release(url);
          }
          dsh_json_kv_str(j, "plainText", plain != NULL ? plain : "");
          dsh_json_kv_bool(j, "found", landed != NULL);
          dsh_json_kv_bool(j, "sameAsShown", same_as_shown);
          dsh_json_key(j, "linkedTo");
          if (linked_to != NULL) dsh_json_str(j, linked_to);
          else dsh_json_null(j);
          dsh_json_kv_str(j, "via", final_via != NULL ? final_via : "");
          dsh_json_key(j, "unconfirmed");
          dsh_json_array_begin(j);
          for (int i = 0; i < unconfirmed_count; i++) {
            dsh_json_str(j, unconfirmed_names[i] != NULL ? unconfirmed_names[i] : "");
          }
          dsh_json_array_end(j);
          dsh_json_kv_str(j, "reason",
                          (same_as_shown && shown_note != NULL) ? shown_note : reason_full);
          dsh_json_kv_str(j, "translateWhy",
                          final_translate_why != NULL ? final_translate_why : "");
          dsh_json_kv_bool(j, "offerTranslate", final_offer_translate);
          dsh_json_kv_bool(j, "offerRecheck", final_offer_recheck);
          /*
           * `chips[]`：终态页那排出路按钮，**按钮上那行字也在内核**（`label`）—— 界面一个字都不拼。
           * `word` = 点了之后拿什么再去查一遍。
           */
          dsh_json_key(j, "chips");
          dsh_json_array_begin(j);
          for (int i = 0; i < chip_count; i++) {
            dsh_json_object_begin(j);
            dsh_json_kv_str(j, "action", chips[i].action != NULL ? chips[i].action : "");
            dsh_json_kv_str(j, "label", chips[i].label != NULL ? chips[i].label : "");
            dsh_json_kv_str(j, "hint", chips[i].hint != NULL ? chips[i].hint : "");
            dsh_json_kv_str(j, "word", query);
            dsh_json_object_end(j);
          }
          dsh_json_array_end(j);
          dsh_json_key(j, "suggestions");
          if (suggestions != NULL) {
            dsh_json_value_raw(j, suggestions, strlen(suggestions));
          } else {
            dsh_json_array_begin(j);
            dsh_json_array_end(j);
          }
          /*
           * `speakText`：这次朗读该念什么，不是用户打进去的原文。
           * 优先序（留在内核，别让界面去挑）：译文伪词条 → 译文；普通词条 → 词典命中的那个词；再退 → 查询词本身。
           */
          {
            const char *speak_word = (landed != NULL)
                                         ? landed
                                         : ((!stopping_at_suggest && query[0] != '\0') ? query : NULL);
            dsh_json_key(j, "speakText");
            if (speak_word != NULL) dsh_json_str(j, speak_word);
            else dsh_json_null(j);
          }
          dsh_json_kv_str(j, "stage", dsh_fallback_stage_name((enum dsh_stage)reached_stage));
          dsh_json_kv_str(j, "surface", final_surface);
          /*
           * `needsTranslate`：**这一条查询该由外壳接着翻**（见 `DSH_FALLBACK_TRANSLATE`）。
           * 界面不读它（界面读的是外壳补完之后的载荷）—— 它是给外壳的**两段式交接**用的，
           * 与发音/翻译那两条的 `needsHttp` 同一种写法。
           */
          dsh_json_kv_bool(j, "needsTranslate", needs_translate);
          dsh_json_object_end(j);
          out = take_json(j, "完整查询");
          if (out == NULL) result_error = DSH_E_OOM;
        }
        if (plain != NULL) dsh_release(plain);
        if (shown_note != NULL) dsh_release(shown_note);
        }   /* else：`dsh_json_new` 那一步 */
      if (reason_full != NULL) dsh_release(reason_full);
      }   /* else：按钮那一步（没拼出按钮就不写 JSON） */
    }     /* else：解释行那一步 */
  }       /* if (result_error == DSH_OK) */

  /*
   * 记一次查词历史：**只有 `landed != NULL` 才记**，记的是落点的规范键名（查 `apples` 落到 `apple`，历史写 `apple`）。
   * 词典名记当时那份快照（改名后旧记录仍显旧名）；去重窗口与上限在 `dsh_hist_push` 里。
   * ⚠️ 历史写不进去**不许**把查词应答搞坏：不设 result_error，失败只写 last_error。
   */
  if (result_error == DSH_OK && landed != NULL) {
    dsh_history *hist = dsh_engine_history(engine);
    if (hist != NULL) {
      const char *title = (shown != NULL) ? dsh_settings_dict_display_name(shown, NULL)
                                          : (shown_dict_id != NULL ? shown_dict_id : "");
      (void)dsh_hist_push(hist, landed, shown_dict_id != NULL ? shown_dict_id : "", title,
                             dsh_now_ms());
    }
  }

  /*
   * 记下「现在显示的是哪条词条」，供下一次选区 / 链接查词判「落点是不是就在眼前」（`sameAsShown`）。
   * ⚠️ 查不到 = 什么都没显示，要清掉 —— 否则会误判成「不用跳」。
   * ⚠️ 与历史那条不同：失败**不设** `result_error`（视图状态记不上，不该毁掉一次成功的查词）。
   */
  if (result_error == DSH_OK) {
    if (dsh_engine_remember_shown(engine, landed, (landed != NULL) ? shown_dict_id : NULL) != 0) {
      dsh_set_last_error("记不住「当前显示的是哪条词条」（内存不足）—— 下一次选区的落点判断会退化成「照常跳」");
    }
  }

  /* ── 收拾（本函数自己拿的每一块都要还）── */
  dsh_fallback_chips_free(chips, chip_count);
  dsh_fallback_result_dispose(&plan);
  if (suggestions != NULL) dsh_release(suggestions);
  if (landed != NULL) dsh_release(landed);
  if (definition != NULL) dsh_release(definition);
  if (linked_to != NULL) dsh_release(linked_to);
  if (final_reason != NULL) dsh_release(final_reason);
  if (final_translate_why != NULL) dsh_release(final_translate_why);
  if (final_unconfirmed_note != NULL) dsh_release(final_unconfirmed_note);
  if (unsupported_sentence != NULL) dsh_release(unsupported_sentence);
  dsh_release(query);
  if (result_error != DSH_OK) return result_error;
  *out_json = out;
  return DSH_OK;
}
