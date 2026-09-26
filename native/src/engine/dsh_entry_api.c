/* ==========================================================================
 * `lookup` 组的 `dsh_engine_entry_document` —— 取一条词条的可渲染文档：把落点解析
 * （`dsh_resolve_entry`，与 `resolve` / `lookup` **同一条**）、文档拼装与资源域
 * （`dict/dsh_entry_doc.c`）、资源卷判定、纯文本与音频键接起来，出参
 * `{html, plainText, resourceBase, audioKeys[]}`。
 * ⚠️ `plainText` 只在**命中**时给（剥掉标签的正文），未命中那一页给空串 —— 与参考实现一致。
 * ========================================================================== */

#include "dsh_lookup.h"
#include "dsh_internal.h"
#include "dict/dsh_entry_doc.h"
#include "dict/dsh_sibling.h"
#include "engine/dsh_dicts.h"
#include "engine/dsh_engine_internal.h"
#include "engine/dsh_settings.h"
#include "json_writer.h"
#include "text/dsh_html.h"

#include <stdio.h>
#include <string.h>

/** 这本词典到底有没有资源可给（`.mdd` 卷，或同目录散放的文件）*/
static int has_resources(dsh_engine *e, const dsh_stored_dict *d, const char *dict_id) {
  dsh_mdx *const *volumes = NULL;
  int count = 0;
  if (dsh_dicts_mdd_volumes(e, dict_id, &volumes, &count) == 0 && count > 0) return 1;
  if (d->mdx_path != NULL && d->mdx_path[0] != '\0' && dsh_sibling_has_any(d->mdx_path)) return 1;
  return 0;
}

/**
 * 未命中时的提示（可信 HTML，由内核拼）：跟过 `@@@LINK` 而目标不存在就说「词典内部
 * 跳转目标不存在」，否则说「未在《词典名》中找到「查询词」。」，有候选时再补一排
 * **可点的** `entry://` 链接（最多 6 条，排除查询词自己）。
 * ⚠️ 每处插值都要 `dsh_html_escape`：词典名与查询词都是**外部内容**，不转义就是把
 *    HTML 注入让给了它们。
 */
static char *build_notice(dsh_engine *e, const dsh_stored_dict *d, const char *dict_id,
                          const char *query, const char *linked_to) {
  char *title_escaped = dsh_html_escape(dsh_settings_dict_display_name(d, NULL),
                                        strlen(dsh_settings_dict_display_name(d, NULL)), NULL);
  char *query_escaped = dsh_html_escape(query, strlen(query), NULL);
  if (title_escaped == NULL || query_escaped == NULL) {
    if (title_escaped != NULL) dsh_release(title_escaped);
    if (query_escaped != NULL) dsh_release(query_escaped);
    return NULL;
  }

  /* 前半句 */
  char *head = NULL;
  if (linked_to != NULL && linked_to[0] != '\0') {
    char *link_escaped = dsh_html_escape(linked_to, strlen(linked_to), NULL);
    if (link_escaped != NULL) {
      const size_t need = strlen(query_escaped) + strlen(link_escaped) + 96;
      head = (char *)dsh_mem_alloc(need);
      if (head != NULL) {
        snprintf(head, need,
                 "未找到词条“%s”。词典内部跳转目标 <code>%s</code> 不存在。", query_escaped,
                 link_escaped);
      }
      dsh_release(link_escaped);
    }
  } else {
    const size_t need = strlen(title_escaped) + strlen(query_escaped) + 64;
    head = (char *)dsh_mem_alloc(need);
    if (head != NULL) {
      snprintf(head, need, "未在《%s》中找到“%s”。", title_escaped, query_escaped);
    }
  }
  dsh_release(title_escaped);
  dsh_release(query_escaped);
  if (head == NULL) {
    dsh_set_last_error("内存不足：未命中的提示");
    return NULL;
  }

  /* 后半句：可点的候选（**排除查询词自己**）*/
  char **words = NULL;
  int64_t word_count = 0;
  char *out = NULL;
  if (dsh_suggest_words(e, dict_id, query, 6, &words, &word_count) == 0 && word_count > 0) {
    size_t need = strlen(head) + strlen("<div class=\"lookup-suggests\">你是不是想找：</div>") + 1;
    for (int64_t i = 0; i < word_count; i++) {
      /* 每条链接 = `<a href="entry://<转义>">` + `<转义>` + `</a>` */
      need += strlen(words[i]) * 6 + 64;
    }
    char *full = (char *)dsh_mem_alloc(need);
    if (full != NULL) {
      size_t at = (size_t)snprintf(full, need, "%s", head);
      int emitted = 0;
      for (int64_t i = 0; i < word_count && at + 64 < need; i++) {
        if (strcmp(words[i], query) == 0) continue; /* 候选里不许出现查询词自己 */
        char *escaped = dsh_html_escape(words[i], strlen(words[i]), NULL);
        if (escaped == NULL) continue;
        char *href = dsh_entry_doc_uri_escape(words[i]);
        if (href == NULL) {
          dsh_release(escaped);
          continue;
        }
        if (emitted == 0) {
          const char *open = "<div class=\"lookup-suggests\">你是不是想找：";
          memcpy(full + at, open, strlen(open));
          at += strlen(open);
        }
        const int wrote =
            snprintf(full + at, need - at, "<a href=\"entry://%s\">%s</a>", href, escaped);
        if (wrote > 0) at += (size_t)wrote;
        dsh_release(escaped);
        dsh_release(href);
        emitted++;
      }
      if (emitted > 0) {
        const char *close = "</div>";
        if (at + strlen(close) < need) {
          memcpy(full + at, close, strlen(close));
          at += strlen(close);
        }
        full[at] = '\0';
        out = full;
      } else {
        dsh_release(full);
      }
    }
    dsh_suggest_words_free(words, word_count);
  }
  /* 没有候选（或候选全被排除）时，就只给前半句 —— 把 `head` 的所有权直接交出去 */
  if (out == NULL) return head;
  dsh_release(head);
  return out;
}

enum dsh_error dsh_engine_entry_document(dsh_engine *engine, const char *dict_id,
                                         const char *key_text, char **out_json) {
  if (out_json == NULL) {
    dsh_set_last_error("dsh_engine_entry_document：out_json 不能为空");
    return DSH_E_INVALID_ARG;
  }
  *out_json = NULL;
  if (engine == NULL) {
    dsh_set_last_error("dsh_engine_entry_document：引擎无效");
    return DSH_E_STATE;
  }
  if (dict_id == NULL || dict_id[0] == '\0' || key_text == NULL || key_text[0] == '\0') {
    dsh_set_last_error("dsh_engine_entry_document：dict_id 与 key_text 都不能为空");
    return DSH_E_INVALID_ARG;
  }

  const struct dsh_settings *s = dsh_engine_settings(engine);
  const dsh_stored_dict *d = (s != NULL) ? dsh_settings_dict_by_id(s, dict_id) : NULL;
  if (d == NULL) {
    dsh_set_last_error("dsh_engine_entry_document：词库里没有这本词典（id=%s）", dict_id);
    return DSH_E_NOT_FOUND;
  }
  if (dsh_dicts_ensure_loaded(engine, dict_id) != 0) {
    const char *why = dsh_last_error_message();
    dsh_set_last_error("dsh_engine_entry_document：《%s》加载不了：%s",
                       dsh_settings_dict_display_name(d, NULL), why != NULL ? why : "");
    if (why != NULL) dsh_release((void *)why);
    return DSH_E_NOT_FOUND;
  }

  const int resources = has_resources(engine, d, dict_id);

  /* 落点：接口定义说 key_text 是**规范键名**，走的仍是与 resolve / lookup 同一条解析
   * （大小写变体 + `@@@LINK` 跟随）—— 界面拿落点来取正文时不会差一层而对不上。 */
  dsh_mdx *mdx = dsh_dicts_peek(engine, dict_id);
  dsh_resolved r;
  dsh_resolve_entry(mdx, key_text, &r);

  char *html = NULL;
  char *plain = NULL;
  size_t html_len = 0;
  if (r.found && r.definition != NULL) {
    html = dsh_entry_doc_build(dict_id, r.definition, (size_t)r.definition_len, NULL, resources,
                               &html_len);
    if (html != NULL) {
      size_t plain_len = 0;
      plain = dsh_html_strip(r.definition, (size_t)r.definition_len, &plain_len);
    }
  } else {
    char *notice = build_notice(engine, d, dict_id, key_text, r.linked_to);
    if (notice != NULL) {
      html = dsh_entry_doc_build(dict_id, "", 0, notice, resources, &html_len);
      dsh_release(notice);
    }
  }
  if (html == NULL) {
    dsh_resolved_free(&r);
    return DSH_E_OOM;
  }
  if (plain == NULL) plain = dsh_mem_strdup("");

  char *base = dsh_entry_doc_base(dict_id);
  if (base == NULL || plain == NULL) {
    dsh_release(html);
    if (base != NULL) dsh_release(base);
    if (plain != NULL) dsh_release(plain);
    dsh_resolved_free(&r);
    return DSH_E_OOM;
  }

  dsh_json *j = dsh_json_new();
  if (j == NULL) {
    dsh_release(html);
    dsh_release(base);
    dsh_release(plain);
    dsh_resolved_free(&r);
    return DSH_E_OOM;
  }
  dsh_json_object_begin(j);
  dsh_json_key(j, "html");
  dsh_json_str_len(j, html, (int64_t)html_len);
  dsh_json_kv_str(j, "plainText", plain);
  dsh_json_kv_str(j, "resourceBase", base);
  dsh_json_key(j, "audioKeys");
  dsh_json_array_begin(j);
  if (r.found && r.definition != NULL) {
    char **keys = NULL;
    int64_t key_count = 0;
    if (dsh_entry_doc_audio_keys(r.definition, (size_t)r.definition_len, &keys, &key_count) == 0) {
      for (int64_t i = 0; i < key_count; i++) dsh_json_str(j, keys[i]);
      dsh_entry_doc_audio_keys_free(keys, key_count);
    }
  }
  dsh_json_array_end(j);
  dsh_json_object_end(j);

  char *out = dsh_json_take(j);
  dsh_json_free(j);
  dsh_release(html);
  dsh_release(base);
  dsh_release(plain);
  dsh_resolved_free(&r);
  if (out == NULL) {
    dsh_set_last_error("内存不足：词条正文的 JSON");
    return DSH_E_OOM;
  }
  *out_json = out;
  return DSH_OK;
}
