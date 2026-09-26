/* 见 engine/dsh_fallback.h —— 决策函数本体。 */
#include "engine/dsh_fallback.h"

#include "dsh_internal.h"

/* 数词那件事走 `dsh_text_count_words_cjk_aware`（「什么算中日韩」由它那边判 ——
 * 这个文件里**不许**出现汉字区间）*/
#include "text/dsh_textutil.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ── 拼字符串（精确长度：先用 vsnprintf 量，再分配 —— 不许悄悄截断）──────── */

static char *fmt(const char *pattern, ...) {
  va_list ap;
  va_start(ap, pattern);
  va_list counter;
  va_copy(counter, ap);
  const int need = vsnprintf(NULL, 0, pattern, counter);
  va_end(counter);
  if (need < 0) {
    va_end(ap);
    dsh_set_last_error("拼字符长度算不出来：%s", pattern);
    return NULL;
  }
  char *out = (char *)dsh_mem_alloc((size_t)need + 1);
  if (out == NULL) {
    va_end(ap);
    dsh_set_last_error("内存不足：拼通道的说明句");
    return NULL;
  }
  vsnprintf(out, (size_t)need + 1, pattern, ap);
  va_end(ap);
  return out;
}

/** 空 / NULL 都当成「没有」 */
static int has_text(const char *s) { return s != NULL && s[0] != '\0'; }

/** 书名 / 词典名可能是空的（还没加载的词典、老记录）：占位成一句不留空的话 */
static const char *token(const char *title) { return has_text(title) ? title : "未知词典"; }

/**
 * 解释行里「**谁没有**」那半句：起点是当前词典 → 「当前词典没有」（原话一个字不改）；
 * 起点是借查来的那一本 → 「《那一本》里没有」。
 * ⚠️ 以前一律写「当前词典没有」，而正文里选中文字那条路的起点**可能不是**当前词典 ——
 * 于是从借查页里再选词、借查绕回当前词典时，界面上那句话是**假话**。
 */
static char *missed_phrase(const dsh_fallback_facts *f) {
  if (f->started_from_current) return dsh_mem_strdup("当前词典没有");
  return fmt("《%s》里没有", token(f->start_dict_title));
}

/**
 * 「≤4 个词就把词写出来，≥5 个词只说『所选文本』」。
 * ⚠️ 数词那件事**不在这里做**：走 `dsh_text_count_words_cjk_aware`（属于文本工具，不属于决策表），
 * 而且**汉字区间只许有一处来源**（`text/dsh_language.c`）—— 这个文件里不许出现汉字区间。
 */
static int count_words(const char *text) {
  return (int)dsh_text_count_words_cjk_aware(text);
}

/**
 * 选区那条路「没查到」那句提示的前半句：那句文案是产品约定，所以归内核拼，界面只显示。
 * ⚠️ **「别的词典里也没有」这半句只在真的都问完了时才说**：`unconfirmed` 非空说明有词典没问完
 * （超时 / 出错），那时加上这半句就是**替没问过的对象下结论**。没问完时那半句由
 * `dsh_fallback_unconfirmed_note` 另给（调用方会拼在后面）。
 */
static char *selection_miss_phrase(const dsh_fallback_facts *f) {
  const int words = count_words(f->query);
  char *head = (words > 0 && words <= 4)
                   ? fmt("%s未在《%s》中查到", f->query, token(f->start_dict_title))
                   : fmt("所选文本未在《%s》中查到", token(f->start_dict_title));
  if (head == NULL) return NULL;
  if (f->unconfirmed_count > 0) return head;
  char *full = fmt("%s，别的词典里也没有", head);
  dsh_release(head);
  return full;
}

/* ── 结果组装 ───────────────────────────────────────────────────────────── */

static void set_action(dsh_fallback_result *r, dsh_fallback_action action, const char *dict_id,
                       const char *via, int stop, enum dsh_surface surface, char *reason) {
  r->action = action;
  r->dict_id = has_text(dict_id) ? dict_id : NULL; /* 空串与 NULL 等价（参考实现同） */
  r->via = via;
  r->stop = stop;
  r->surface = surface;
  r->reason = reason;
  if (reason == NULL && !r->oom) {
    /* 只有在「本该有这句话却没拼出来」时才算 OOM——由调用方看 oom 字段 */
  }
}

/**
 * 把「没问完的词典」贴到**最终那一页**上。
 * 这一条是整套方案里**唯一不许让步的**：只要有词典没问完，结果页就必须带
 * 「另有 N 本没能确认 + 再问一遍」—— 否则用户会把「我们没问完」读成「别的词典也都没有」。
 * ⚠️ 借查命中那一页、自动翻译那一页、终态页，**三者都要带**。
 */
static void annotate(dsh_fallback_result *r, const dsh_fallback_facts *f) {
  r->unconfirmed_note = dsh_fallback_unconfirmed_note(f);
  if (r->unconfirmed_note == NULL) r->oom = 1;
  r->offer_recheck = (f->unconfirmed_count > 0) ? 1 : 0;
  const int n = (f->unconfirmed_count > DSH_FALLBACK_MAX_UNCONFIRMED)
                    ? DSH_FALLBACK_MAX_UNCONFIRMED
                    : f->unconfirmed_count;
  r->unconfirmed_count = n;
  for (int i = 0; i < n; i++) r->unconfirmed_names[i] = f->unconfirmed[i];
}

/* ── 三块可以单独用的纯函数 ─────────────────────────────────────────────── */

int dsh_fallback_runs_channel(enum dsh_origin origin) {
  return (origin == DSH_ORIGIN_INPUT || origin == DSH_ORIGIN_SELECTION) ? 1 : 0;
}

int dsh_fallback_translate_usable(const dsh_fallback_facts *f) {
  return (f->translate_enabled && f->auto_translate && f->translate_has_key &&
          f->translate_supported)
             ? 1
             : 0;
}

char *dsh_fallback_translate_why(const dsh_fallback_facts *f) {
  if (f == NULL) return dsh_mem_strdup("");
  /*
   * 终态页那句「翻译为什么没用上」——**四档分开说，不许合并**。
   * 为什么非要分开：这四件事对用户的含义完全不同 —— ① 他从来没开过翻译（去选项里开）
   * ② 他**只是不让它自动**（那就现场给他点一下的机会）③ 他没填 Key（去语音页填）
   * ④ 这个语种根本不在支持表里（换语种，不是换设置）。合并成一句「翻译不可用」，用户会去改错的地方。
   */
  if (!f->translate_enabled) return dsh_mem_strdup("机器翻译的总开关关着（选项 → 翻译）");
  if (!f->auto_translate) {
    return dsh_mem_strdup("「查不到时自动翻译」关着 —— 你可以直接点「翻译这个词」");
  }
  if (!f->translate_has_key) {
    return dsh_mem_strdup("还没填火山引擎的 API Key（它与语音共用同一把，在「语音」页填）");
  }
  if (!f->translate_supported) {
    return has_text(f->translate_unsupported_message)
               ? dsh_mem_strdup(f->translate_unsupported_message)
               : dsh_mem_strdup("这个语种不支持机器翻译");
  }
  return dsh_mem_strdup(""); /* 四个条件都满足就不该走到终态页 */
}

char *dsh_fallback_unconfirmed_note(const dsh_fallback_facts *f) {
  if (f == NULL || f->unconfirmed_count <= 0) return dsh_mem_strdup("");
  /*
   * ⚠️ **空名字要丢掉，不是占位成「未知词典」**：参考实现那句话是 `filter((name) => !!name)` 再数、再列。
   * 用了 `token()`（空 → 「未知词典」）的后果是同一页上「另有 N 本没能确认」与出路按钮上
   * 「还有 M 本没查完」会**数出两个不同的数**，而它们说的是同一件事。
   */
  const char *names[DSH_FALLBACK_MAX_UNCONFIRMED];
  int n = 0;
  const int seen = (f->unconfirmed_count > DSH_FALLBACK_MAX_UNCONFIRMED)
                       ? DSH_FALLBACK_MAX_UNCONFIRMED
                       : f->unconfirmed_count;
  for (int i = 0; i < seen; i++) {
    if (has_text(f->unconfirmed[i])) names[n++] = f->unconfirmed[i];
  }
  if (n == 0) return dsh_mem_strdup("");

  /* 先量长度再拼（这一句里有中文与书名号，按字节拼容易错） */
  size_t len = strlen("另有 ") + 20 + strlen(" 本没能确认（）") + 1;
  for (int i = 0; i < n; i++) len += strlen(names[i]) + 3;
  char *out = (char *)dsh_mem_alloc(len);
  if (out == NULL) {
    dsh_set_last_error("内存不足：没问完的词典那句说明");
    return NULL;
  }
  const int head = snprintf(out, len, "另有 %d 本没能确认（", n);
  size_t at = (head > 0) ? (size_t)head : 0;
  for (int i = 0; i < n; i++) {
    const int wrote = snprintf(out + at, len - at, "%s《%s》", (i > 0) ? "、" : "", names[i]);
    if (wrote > 0) at += (size_t)wrote;
  }
  snprintf(out + at, len - at, "）");
  return out;
}

/* ── 终态页那排出路按钮 ─────────────────────────────────────────────────── */

/**
 * 逐条搬参考实现的 `refreshEntryChips`。
 * ⚠️ 那几行文案原先**写在前端**，这里按硬规则 1 搬进内核 —— 界面继续只画不拼。
 * 搬的是**字面**：`「」`、`、` 分隔、`（几）本` 这些一个字符都没改。
 */
int dsh_fallback_chips(const dsh_fallback_facts *f, int offer_recheck, int offer_translate,
                       const char *word, dsh_chip *out, int max) {
  if (f == NULL || out == NULL || max <= 0) {
    dsh_set_last_error("dsh_fallback_chips：参数不合法");
    return 0;
  }
  const char *w = (word != NULL) ? word : "";
  int n = 0;

  /* 没问完的那几本**非空**的名字（先 filter 再数，见 `dsh_fallback_unconfirmed_note` 那段） */
  const char *names[DSH_FALLBACK_MAX_UNCONFIRMED];
  int name_count = 0;
  const int seen = (f->unconfirmed_count > DSH_FALLBACK_MAX_UNCONFIRMED)
                       ? DSH_FALLBACK_MAX_UNCONFIRMED
                       : f->unconfirmed_count;
  for (int i = 0; i < seen; i++) {
    if (has_text(f->unconfirmed[i])) names[name_count++] = f->unconfirmed[i];
  }

  if (offer_recheck && n < max) {
    /* 有没问完的 → 「还有 N 本没查完」+ 把那几本点名；否则就是「再问一遍」 */
    char *label = NULL;
    char *hint = NULL;
    if (name_count > 0) {
      size_t len = strlen("还有 ") + 20 + strlen(" 本没查完") + 1;
      label = (char *)dsh_mem_alloc(len);
      if (label != NULL) snprintf(label, len, "还有 %d 本没查完", name_count);

      /* 「点一下把这（几）本也找一遍：A、B」 */
      len = strlen("点一下把这（几）本也找一遍：") + 1;
      for (int i = 0; i < name_count; i++) len += strlen(names[i]) + 4;
      hint = (char *)dsh_mem_alloc(len);
      if (hint != NULL) {
        size_t at = (size_t)snprintf(hint, len, "点一下把这（几）本也找一遍：");
        for (int i = 0; i < name_count && at < len; i++) {
          const int wrote = snprintf(hint + at, len - at, "%s%s", (i > 0) ? "、" : "", names[i]);
          if (wrote > 0) at += (size_t)wrote;
        }
      }
    } else {
      label = dsh_mem_strdup("再问一遍");
      hint = dsh_mem_strdup("再问一次别的词典");
    }
    if (label == NULL || hint == NULL) {
      if (label != NULL) dsh_release(label);
      if (hint != NULL) dsh_release(hint);
      dsh_fallback_chips_free(out, n);
      dsh_set_last_error("内存不足：出路按钮那行字");
      return 0;
    }
    out[n].action = "recheck";
    out[n].label = label;
    out[n].hint = hint;
    n++;
  }

  if (offer_translate && n < max) {
    const size_t len = strlen("翻译「") + strlen(w) + strlen("」") + 1;
    char *label = (char *)dsh_mem_alloc(len);
    if (label == NULL) {
      dsh_fallback_chips_free(out, n);
      dsh_set_last_error("内存不足：出路按钮那行字");
      return 0;
    }
    snprintf(label, len, "翻译「%s」", w);
    char *hint = dsh_mem_strdup("把这段文字发给火山引擎翻译");
    if (hint == NULL) {
      dsh_release(label);
      dsh_fallback_chips_free(out, n);
      dsh_set_last_error("内存不足：出路按钮那行字");
      return 0;
    }
    out[n].action = "translate";
    out[n].label = label;
    out[n].hint = hint;
    n++;
  }

  return n;
}

void dsh_fallback_chips_free(dsh_chip *chips, int count) {
  if (chips == NULL) return;
  for (int i = 0; i < count; i++) {
    if (chips[i].label != NULL) dsh_release(chips[i].label);
    if (chips[i].hint != NULL) dsh_release(chips[i].hint);
    chips[i].label = NULL;
    chips[i].hint = NULL;
    chips[i].action = NULL;
  }
}

/* ── 名字（写日志与测试结果用）──────────────────────────────────────────── */

const char *dsh_fallback_action_name(dsh_fallback_action action) {
  switch (action) {
    case DSH_FALLBACK_LOOKUP: return "Lookup";
    case DSH_FALLBACK_SHOW: return "Show";
    case DSH_FALLBACK_SUGGEST: return "Suggest";
    case DSH_FALLBACK_PROBE: return "Probe";
    case DSH_FALLBACK_TRANSLATE: return "Translate";
    case DSH_FALLBACK_TERMINAL: return "Terminal";
    case DSH_FALLBACK_EXPLAIN_ERROR: return "ExplainError";
    default: return "(未知动作)";
  }
}

const char *dsh_fallback_stage_name(enum dsh_stage stage) {
  switch (stage) {
    case DSH_STAGE_START: return "start";
    case DSH_STAGE_AFTER_LOOKUP: return "afterLookup";
    case DSH_STAGE_AFTER_PROBE: return "afterProbe";
    case DSH_STAGE_AFTER_SUGGEST: return "afterSuggest";
    case DSH_STAGE_DONE: return "done";
    default: return "(未知阶段)";
  }
}

/* ── 决策函数本体 ───────────────────────────────────────────────────────── */

void dsh_fallback_result_dispose(dsh_fallback_result *r) {
  if (r == NULL) return;
  if (r->reason != NULL) dsh_release(r->reason);
  if (r->translate_why != NULL) dsh_release(r->translate_why);
  if (r->unconfirmed_note != NULL) dsh_release(r->unconfirmed_note);
  memset(r, 0, sizeof(*r));
}

void dsh_fallback_decide(const dsh_fallback_facts *facts, dsh_fallback_result *out) {
  if (out == NULL) return;
  memset(out, 0, sizeof(*out));
  out->action = DSH_FALLBACK_SHOW;
  out->via = "current";
  out->surface = DSH_SURFACE_NONE;

  dsh_fallback_facts empty;
  memset(&empty, 0, sizeof(empty));
  empty.origin = DSH_ORIGIN_INPUT;
  empty.stage = DSH_STAGE_START;
  empty.script = DSH_SCRIPT_LATIN;
  empty.started_from_current = 1;
  const dsh_fallback_facts *f = (facts != NULL) ? facts : &empty;

  /* ── 0) 不走查词通道的三个入口 ────────────────────────────────────────────────
   * 链接 / 回退 / 历史**只做精确还原**：这里连「借查」都不许发生 —— 否则「退回一个查不到的词」
   * 会当场跳走，用户退不回去。
   * ⚠️ **但「不走查词通道」不等于「什么都不做」**：参考实现在这一档里**在那一本里精确查一次**，
   * 只是后面那三步（借查 / 联想兜底 / 翻译）不跑。原来在 START 阶段直接给一张 `SHOW`
   * （= 什么都没查、空页），症状是**点词条里的 `entry://` 链接，正文变成一张什么都没有的页面**。
   * 所以这里改成走一步 `LOOKUP`（`stop = 0`），查完再决定收尾。 */
  if (!dsh_fallback_runs_channel(f->origin)) {
    if (f->dictionary_missing) {
      set_action(out, DSH_FALLBACK_EXPLAIN_ERROR, NULL, NULL, 1, DSH_SURFACE_NONE,
                 fmt("《%s》已不在词库中 —— 重新导入到原路径即可恢复",
                     token(f->target_dict_title)));
      if (out->reason == NULL) out->oom = 1;
      return;
    }
    if (f->dictionary_file_gone) {
      set_action(out, DSH_FALLBACK_EXPLAIN_ERROR, NULL, NULL, 1, DSH_SURFACE_NONE,
                 fmt("《%s》的文件不在了 —— 把它放回原来的位置，或重新导入一次",
                     token(f->target_dict_title)));
      if (out->reason == NULL) out->oom = 1;
      return;
    }
    if (f->entry_is_translation) {
      /* 译文条目回放走**翻译缓存**（不是重新计费），也不压返回栈 */
      set_action(out, DSH_FALLBACK_TRANSLATE, NULL, "translate", 1, DSH_SURFACE_NONE,
                 dsh_mem_strdup("这是机器翻译的译文条目 —— 回放走译文缓存"));
      if (out->reason == NULL) out->oom = 1;
      return;
    }

    if (f->stage == DSH_STAGE_START) {
      set_action(out, DSH_FALLBACK_LOOKUP, NULL, "current", 0, DSH_SURFACE_NONE,
                 dsh_mem_strdup("这条入口不跑兜底通道：在起点那一本里精确查一次"));
      if (out->reason == NULL) out->oom = 1;
      return;
    }

    if (f->stage == DSH_STAGE_AFTER_LOOKUP) {
      if (f->entry_found) {
        /* 命中就显示，**不借查**（`via = current`：正文就是起点那一本给的）*/
        set_action(out, DSH_FALLBACK_SHOW, NULL, "current", 1, DSH_SURFACE_NONE, NULL);
        return;
      }
      /*
       * ★ 带音节分隔点时**先去掉点在本里再问一次**（对所有不走通道的入口都生效）。
       * 这一档放在「到此为止」之前 —— `弗拉基米尔·普京` 那类原样命中的词条走上面那一支，一个字都不受影响。
       */
      if (f->has_separator_dots && !f->separator_retried) {
        set_action(out, DSH_FALLBACK_RELOOKUP, NULL, "current", 0, DSH_SURFACE_NONE,
                   dsh_mem_strdup("带音节分隔点：去掉点之后再问一次这本词典"));
        if (out->reason == NULL) out->oom = 1;
        return;
      }
      /*
       * 没查到：**到此为止**。不许借查、不许翻译、不许给「再问一遍」——
       * 这条路上一次探路都没发生，「再问一遍」点了也只会得到同一张页。
       */
      char *missed = missed_phrase(f);
      set_action(out, DSH_FALLBACK_TERMINAL, NULL, "terminal", 1, DSH_SURFACE_NONE,
                 (missed != NULL) ? missed : dsh_mem_strdup("这本词典里没有这一条"));
      /* TERMINAL 这一档要求带上「翻译为什么没用上」（见函数末尾那条不变式）*/
      out->translate_why =
          dsh_mem_strdup("这条入口不跑兜底通道（链接 / 回退 / 历史只做精确还原）");
      if (out->reason == NULL || out->translate_why == NULL) out->oom = 1;
      return;
    }

    /* 别的阶段（不该发生）：保守地停在「显示空页」，并把阶段写进说明 */
    set_action(out, DSH_FALLBACK_SHOW, NULL, "current", 1, DSH_SURFACE_NONE,
               fmt("未知阶段：%s", dsh_fallback_stage_name(f->stage)));
    if (out->reason == NULL) out->oom = 1;
    return;
  }

  switch (f->stage) {
    /* ── 1) 起点：当前词典那一步之前 ───────────────────────────────────── */
    case DSH_STAGE_START:
      if (f->origin == DSH_ORIGIN_INPUT) {
        if (f->has_separator_dots) {
          /*
           * ★ **带音节分隔点的词先问词典，不先联想**。
           * 为什么：`pro·gress` 这种写法是**词典自己的排版**、不是用户打错的字 —— 拿它去拼前缀 /
           * 编辑距离只会得到一串噪音候选，而且会把链**停在候选列表上**，于是「明明有 `progress` 却查不到」。
           */
          set_action(out, DSH_FALLBACK_LOOKUP, NULL, "current", 0, DSH_SURFACE_NONE,
                     dsh_mem_strdup("这个词带音节分隔点：先原样问词典（问不到再去掉点问）"));
        } else if (f->script == DSH_SCRIPT_HAN) {
        /* 汉字输入**跳过当前词典的联想** —— 英文词典对中文词的前缀 / 编辑距离候选是噪音 */
          set_action(out, DSH_FALLBACK_LOOKUP, NULL, "current", 0, DSH_SURFACE_NONE,
                     dsh_mem_strdup("汉字输入：不在当前词典做联想"));
        } else {
          set_action(out, DSH_FALLBACK_SUGGEST, NULL, "current", 0, DSH_SURFACE_LIST,
                     dsh_mem_strdup("先看当前词典的联想候选"));
        }
      } else {
        /* 选区：先问当前词典「这个词落在哪条词条」 */
        set_action(out, DSH_FALLBACK_LOOKUP, NULL, "current", 0, DSH_SURFACE_NONE,
                   dsh_mem_strdup("先问当前词典落点"));
      }
      break;

    /* ── 2) 联想算完了 ─────────────────────────────────────────────────── */
    case DSH_STAGE_AFTER_SUGGEST:
      if (f->script != DSH_SCRIPT_HAN && f->suggestion_count > 0 &&
          !f->suggestion_has_exact) {
        /* 摆候选列表并停下等用户选 */
        set_action(out, DSH_FALLBACK_SUGGEST, NULL, "current", 1, DSH_SURFACE_LIST,
                   dsh_mem_strdup("当前词典有联想候选"));
      } else {
        /*
         * 两件事都落到同一格（见 facts 里 `suggestion_has_exact` 那段）：
         *   · 一个候选都没有 → 直接查；· **有一条精确命中** → 直接查那一条。
         *     `suggest` 的第一条就是「与 resolve 同一条解析」算出来的规范键名，所以接着走的 `LOOKUP` 用原样 query 就落到同一个词条。
         */
        set_action(out, DSH_FALLBACK_LOOKUP, NULL, "current", 0, DSH_SURFACE_NONE,
                   dsh_mem_strdup("没有候选 / 有精确命中：直接查当前词典"));
      }
      break;

    /* ── 3) 当前词典查完了 ─────────────────────────────────────────────── */
    case DSH_STAGE_AFTER_LOOKUP:
      if (f->entry_found) {
        set_action(out, DSH_FALLBACK_SHOW, NULL, "current", 1, DSH_SURFACE_NONE, NULL);
        break;
      } else {
        /*
         * ★ **先给「去掉音节分隔点再来一遍」这一步**（那一步在决策之前做，所以借查 / 联想 / 翻译
         *    用的都是修剪后的词）。检查标准三条一起看：**没命中** + **词里确实带点** + **还没试过**；
         *    少了第三条就会来回重问（`RELOOKUP` 自己要把 `separator_retried` 置上）。
         * ⚠️ 这一档**排在借查前面**：先在自己这本里把点去掉问一遍再去麻烦别的词典，
         *    否则 `pro·gress` 会拿带点的原样去借查一遍（对方也一样没有），白白多问几本。
         */
        if (f->has_separator_dots && !f->separator_retried) {
          set_action(out, DSH_FALLBACK_RELOOKUP, NULL, "current", 0, DSH_SURFACE_NONE,
                     dsh_mem_strdup("带音节分隔点：去掉点之后再问一次这本词典"));
          out->offer_recheck = 0;
          break;
        }
        /* 去问别的词典（「落点 = 当前词条」由调用方处理 —— 那件事发生在**动手之前**，不属于「查完之后的下一步」）。 */
        char *missed = missed_phrase(f);
        char *reason = NULL;
        if (missed != NULL) {
          reason = f->started_from_current
                       ? fmt("%s：%s", missed, token(f->start_dict_title))
                       : dsh_mem_strdup(missed);
          dsh_release(missed);
        }
        set_action(out, DSH_FALLBACK_PROBE, NULL, "borrow", 0, DSH_SURFACE_NONE, reason);
        out->offer_recheck = 0; /* 参考实现：这一步先不给「再问一遍」 */
        break;
      }

    /* ── 4) 别的词典问完了 ─────────────────────────────────────────────── */
    case DSH_STAGE_AFTER_PROBE: {
      if (has_text(f->hit_dict_id)) {
        if (f->hit_is_current) {
          /*
           * 命中的那本**就是设置里的当前词典**时**不许**按借查收尾：用户那一页本来就是他自己那本
           * 词典里的词条，写成「当前词典没有 · 已用《它》借查」是两处都说反了。
           * 于是按**当前词典命中**收尾：`via = current`、不带解释行。
           * ⚠️ 但「没问完的词典」照样要贴上去（`annotate`）—— 那条与「这一页从哪来」无关。
           */
          set_action(out, DSH_FALLBACK_SHOW, f->hit_dict_id, "current", 1, DSH_SURFACE_NONE,
                     NULL);
          annotate(out, f);
          break;
        }
        /* 用那一本查并显示（**不切当前词典**） */
        char *missed = missed_phrase(f);
        char *reason = NULL;
        if (missed != NULL) {
          reason = fmt("%s · 已用《%s》借查", missed,
                       token(has_text(f->hit_dict_title) ? f->hit_dict_title : f->hit_dict_id));
          dsh_release(missed);
        }
        set_action(out, DSH_FALLBACK_SHOW, f->hit_dict_id, "borrow", 1, DSH_SURFACE_NONE,
                   reason);
        annotate(out, f);
        break;
      }

      /* 选区那一路：候选**不占正文区**，并进正文框底部提示。
       * `via` 仍记 `terminal`（这一档的**结局**就是「没有结果」）；界面据 `surface = toast` 决定**不替换正文**。 */
      if (f->origin == DSH_ORIGIN_SELECTION && f->suggestion_count > 0) {
        set_action(out, DSH_FALLBACK_SUGGEST, NULL, "terminal", 1, DSH_SURFACE_TOAST,
                   selection_miss_phrase(f));
        annotate(out, f);
        break;
      }

      if (dsh_fallback_translate_usable(f)) {
        /* 自动翻译，但「没问完」照样要跟着这一页走。
         * 起点是**借查来的那一本**时就只说「都没有」、不点名 —— 把一本借来的词典写在
         * 「词典里都没有：」后面，读起来像是「它没有、别的有」。 */
        char *reason = f->started_from_current
                           ? fmt("词典里都没有：%s → 机器翻译", token(f->start_dict_title))
                           : dsh_mem_strdup("词典里都没有 → 机器翻译");
        set_action(out, DSH_FALLBACK_TRANSLATE, NULL, "translate", 1, DSH_SURFACE_NONE,
                   reason);
        annotate(out, f);
        break;
      }

      /*
       *  E：终态页 —— 查词通道的三步都试过、翻译用不上。
       * ⚠️ **选区那条路的终态也是「正文框底部一句提示」**（**不替换正文**）：用户是在读文章时点了一下
       *    查词，把正文换成一张「没找到」的页等于惩罚他点那一下。界面**只看 `surface`** 决定换不换正文。
       */
      if (f->origin == DSH_ORIGIN_SELECTION) {
        set_action(out, DSH_FALLBACK_TERMINAL, NULL, "terminal", 1, DSH_SURFACE_TOAST,
                   selection_miss_phrase(f));
      } else {
        set_action(out, DSH_FALLBACK_TERMINAL, NULL, "terminal", 1, DSH_SURFACE_NONE,
                   dsh_mem_strdup("三档都没有结果"));
      }
      out->translate_why = dsh_fallback_translate_why(f);
      /* 只是不让它自动，不是不要它 */
      out->offer_translate = (f->translate_enabled && !f->auto_translate) ? 1 : 0;
      annotate(out, f);
      break;
    }

    default:
      /* 未知阶段：保守地什么都不做（宁可停，也别乱跳） */
      set_action(out, DSH_FALLBACK_SHOW, NULL, "current", 1, DSH_SURFACE_NONE,
                 fmt("未知阶段：%s", dsh_fallback_stage_name(f->stage)));
      break;
  }

  /* 该有说明句的地方没拼出来 → 如实当成内存不足（不许当成「这句话本来就没有」） */
  if (out->reason == NULL && !(f->stage == DSH_STAGE_AFTER_LOOKUP) &&
      !(f->stage == DSH_STAGE_AFTER_PROBE && f->hit_is_current && has_text(f->hit_dict_id))) {
    out->oom = 1;
  }
  if (out->translate_why == NULL && out->action == DSH_FALLBACK_TERMINAL) out->oom = 1;
}
