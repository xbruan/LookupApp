/* ==========================================================================
 * 接口定义里 `text` 组那 3 条 —— 只做「把内核算好的东西包成 JSON / 字符串」：
 * 逻辑全在 `dsh_language.c` / `dsh_textutil.c`，这里只管参数校验与错误码、出参的所有权
 * （**内核分配、内核释放**，都交给 `dsh_release`）、以及界面要的那句话由**内核**拼
 * （理由与全仓同一条：同一个东西不要有两个来源）。
 * ========================================================================== */

#include "dsh_lookup.h"
#include "dsh_internal.h"
#include "engine/dsh_engine_internal.h"
#include "engine/dsh_settings.h"
#include "json_writer.h"
#include "text/dsh_language.h"
#include "text/dsh_textutil.h"

#include <stdio.h>
#include <string.h>

/* ── dsh_text_analyze ───────────────────────────────────────────────────── */

enum dsh_error dsh_text_analyze(const char *text, char **out_json) {
  if (out_json == NULL) {
    dsh_set_last_error("dsh_text_analyze：out_json 不能为空");
    return DSH_E_INVALID_ARG;
  }
  *out_json = NULL;
  if (text == NULL) {
    dsh_set_last_error("dsh_text_analyze：text 不能为空");
    return DSH_E_INVALID_ARG;
  }

  dsh_text_analysis a;
  dsh_text_analyze_plain(text, &a);

  /* 去掉点之后的写法：**分析里顺带给出来**，省得调用方为了拿它再调一次；
   * 整段只有点 → 空串（不是 null）。 */
  char *stripped = dsh_text_strip_separator_dots(text);
  if (stripped == NULL) return DSH_E_OOM;

  dsh_json *j = dsh_json_new();
  if (j == NULL) {
    dsh_release(stripped);
    return DSH_E_OOM;
  }
  dsh_json_object_begin(j);
  dsh_json_kv_str(j, "script", a.script);
  dsh_json_kv_bool(j, "hasSeparatorDots", a.has_separator_dots);
  dsh_json_kv_str(j, "withoutSeparatorDots", stripped);
  dsh_json_kv_i64(j, "wordCount", a.word_count);
  dsh_json_kv_bool(j, "isSingleChar", a.is_single_char);
  dsh_json_object_end(j);
  dsh_release(stripped);

  char *text_out = dsh_json_take(j);
  dsh_json_free(j);
  if (text_out == NULL) {
    dsh_set_last_error("内存不足：文本分析 JSON");
    return DSH_E_OOM;
  }
  *out_json = text_out;
  return DSH_OK;
}

/* ── dsh_text_strip_separators ──────────────────────────────────────────── */

enum dsh_error dsh_text_strip_separators(const char *text, char **out_text) {
  if (out_text == NULL) {
    dsh_set_last_error("dsh_text_strip_separators：out_text 不能为空");
    return DSH_E_INVALID_ARG;
  }
  *out_text = NULL;
  if (text == NULL) {
    dsh_set_last_error("dsh_text_strip_separators：text 不能为空");
    return DSH_E_INVALID_ARG;
  }
  char *stripped = dsh_text_strip_separator_dots(text);
  if (stripped == NULL) return DSH_E_OOM;
  *out_text = stripped;
  return DSH_OK;
}

/* ── dsh_language_detect ────────────────────────────────────────────────── */

/**
 * 取「用哪本的标题做线索」：`dict_id` 为空 → 用**当前词典**的标题，给了 id → 用那一本的。
 * ⚠️ 这里**只看设置里记着的标题**，**不打开词典** —— 判语种是界面上的热路径，为它去解
 *    .mdx 头部不值得；而且 .mdx 头部**根本没有语言字段**（见 text/dsh_language.h）。
 */
static const char *title_for(const dsh_engine *e, const char *dict_id) {
  if (e == NULL) return NULL;
  const struct dsh_settings *s = dsh_engine_settings(e);
  if (s == NULL) return NULL;
  if (dict_id != NULL && dict_id[0] != '\0') {
    const dsh_stored_dict *d = dsh_settings_dict_by_id(s, dict_id);
    return (d != NULL) ? dsh_settings_dict_display_name(d, NULL) : NULL;
  }
  const char *cur = dsh_settings_current_dict_id(s);
  if (cur == NULL || cur[0] == '\0') return NULL;
  const dsh_stored_dict *d = dsh_settings_dict_by_id(s, cur);
  return (d != NULL) ? dsh_settings_dict_display_name(d, NULL) : NULL;
}

enum dsh_error dsh_language_detect(dsh_engine *engine, const char *text, const char *dict_id,
                                   char **out_json) {
  if (out_json == NULL) {
    dsh_set_last_error("dsh_language_detect：out_json 不能为空");
    return DSH_E_INVALID_ARG;
  }
  *out_json = NULL;
  if (text == NULL) {
    dsh_set_last_error("dsh_language_detect：text 不能为空");
    return DSH_E_INVALID_ARG;
  }
  if (engine == NULL) {
    dsh_set_last_error("dsh_language_detect：引擎无效");
    return DSH_E_STATE;
  }

  /* ⚠️ 用户显式指定的语种（设置里的 `speech` 段）这一版**先不读**：设置模型还不认识它，
   *    显式指定那条路只走诊断 —— 别假装读到了。 */
  const char *override_code = NULL;
  const char *default_language = NULL;

  const char *language = NULL;
  const char *basis = NULL;
  const char *basis_text = NULL;
  dsh_language_decide(text, title_for(engine, dict_id), override_code, default_language,
                      &language, &basis, &basis_text);

  const char *label = dsh_language_name(language);
  if (label[0] == '\0') label = language; /* 认不出的码：至少把码本身给出来 */

  /* 界面要的那句话在这里拼好（接口定义点名：界面不拼）。形状：语种 +（全角括号里的
   * 检查标准说明），例如 `英语（按字形判断（拉丁字母））`。 */
  char explanation[256];
  snprintf(explanation, sizeof(explanation), "%s（%s）", label, basis_text);

  dsh_json *j = dsh_json_new();
  if (j == NULL) return DSH_E_OOM;
  dsh_json_object_begin(j);
  dsh_json_kv_str(j, "language", language);
  dsh_json_kv_str(j, "languageLabel", label);
  dsh_json_kv_str(j, "basis", basis);
  dsh_json_kv_str(j, "basisText", basis_text);
  dsh_json_kv_str(j, "explanation", explanation);
  /* 中英混排：汉字与拉丁字母同时出现（界面据此问「要不要换个词典」）；
   * 用户显式指定语种时不判混排 —— 那是「我就要这一段按这个语种念」的意思。 */
  dsh_json_kv_bool(j, "mixed", override_code == NULL ? dsh_language_is_mixed(text) : 0);
  dsh_json_object_end(j);

  char *text_out = dsh_json_take(j);
  dsh_json_free(j);
  if (text_out == NULL) {
    dsh_set_last_error("内存不足：语种判定 JSON");
    return DSH_E_OOM;
  }
  *out_json = text_out;
  return DSH_OK;
}

/* ── dsh_language_label ─────────────────────────────────────────────────── */

/**
 * 语种码 → 中文名。
 * ★ 与「判定语种」共用同一张表（`text/dsh_language.c`）—— 界面要显示语种名时
 *   **只能问内核**，壳与界面都不许再存一份（两份迟早会对不上）。
 */
enum dsh_error dsh_language_label(const char *code, char **out_json) {
  dsh_json *j;
  char *out;
  const char *primary;
  const char *label;

  dsh_clear_last_error();
  if (out_json == NULL) {
    dsh_set_last_error("dsh_language_label：out_json 不能为空");
    return DSH_E_INVALID_ARG;
  }

  /* 区域标记先取主代码（"en-US" → "en"）—— 与判定那条路同一约定 */
  primary = dsh_language_primary(code == NULL ? "" : code);
  label = dsh_language_name(primary);

  j = dsh_json_new();
  if (j == NULL) {
    dsh_set_last_error("dsh_language_label：内存不足");
    return DSH_E_OOM;
  }
  dsh_json_object_begin(j);
  dsh_json_kv_str(j, "code", primary);
  dsh_json_kv_str(j, "label", label);
  /* 认不出就**如实说认不出**（回空串，不拿码当名字）*/
  dsh_json_kv_bool(j, "known", label[0] != '\0' ? 1 : 0);
  dsh_json_object_end(j);

  out = dsh_json_take(j);
  dsh_json_free(j);
  if (out == NULL) return DSH_E_OOM;
  *out_json = out;
  return DSH_OK;
}
