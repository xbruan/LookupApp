/* 内核单元测试 · 查词通道的决策表（engine/dsh_fallback.c）—— 表驱动：决策函数是纯的（同样的 facts 必得
 * 同样的结论），用例与检查标准逐条照参考实现的 `tools/FallbackProbe/Program.cs`，少了它行为只剩「读代码
 * 看不出来」。钉住：汉字跳过联想；命中当前词典即结束、没命中才借查；四条件全满足才翻译；链接/回退/历史不走查词通道。 */

#include "engine/dsh_fallback.h"

#include "mem_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DSH_SRC_DIR
#error "需要 -DDSH_SRC_DIR=<0.2.0/native/src 的路径>（见 Makefile）"
#endif

static int g_checks = 0;
static int g_failed = 0;

static void ok(int cond, const char *what) {
  g_checks++;
  if (!cond) {
    g_failed++;
    fprintf(stderr, "FAIL %s\n", what);
  }
}

static void ok_eq_str(const char *actual, const char *expected, const char *what) {
  g_checks++;
  if (actual == NULL || expected == NULL || strcmp(actual, expected) != 0) {
    g_failed++;
    fprintf(stderr, "FAIL %s\n      实际=%s\n      期望=%s\n", what,
            actual ? actual : "(null)", expected ? expected : "(null)");
  }
}

/** 子串包含（NULL 当空串） */
static int has(const char *haystack, const char *needle) {
  return haystack != NULL && strstr(haystack, needle) != NULL;
}

/** 自己拷一份（不用 `strdup`：`-std=c11 -pedantic` 下它不在标准里） */
static char *local_dup(const char *text) {
  if (text == NULL) return NULL;
  const size_t n = strlen(text);
  char *out = (char *)malloc(n + 1);
  if (out != NULL) memcpy(out, text, n + 1);
  return out;
}

/** 结论的现场实测结果（失败时打出来，别让人去猜） */
static void dump_plan(const char *what, const dsh_fallback_result *r) {
  fprintf(stderr, "      %s → Action=%s / DictId=%s / Stop=%d / Via=%s / Surface=%d\n",
          what, dsh_fallback_action_name(r->action), r->dict_id ? r->dict_id : "(null)",
          r->stop, r->via ? r->via : "(null)", (int)r->surface);
  fprintf(stderr, "        Reason=%s\n", r->reason ? r->reason : "(null)");
  if (r->unconfirmed_note != NULL && r->unconfirmed_note[0] != '\0') {
    fprintf(stderr, "        Unconfirmed=%s / OfferRecheck=%d\n", r->unconfirmed_note,
            r->offer_recheck);
  }
}

/** 摆一份「默认都满足」的事实（与参考实现的 `Facts()` 同一套默认值）。
 *  ⚠️ `DECIDE` 顺带钉一条不变式：**正常用例上不许报 oom** —— 决策函数只在「该有说明句却拼不出来」
 *  时才标它，标了就是真出事（它还会把整个结论打出来，省得照 facts 一行行读）。 */
static void facts_default(dsh_fallback_facts *f, enum dsh_origin origin,
                          enum dsh_script script) {
  memset(f, 0, sizeof(*f));
  f->origin = origin;
  f->stage = DSH_STAGE_START;
  f->script = script;
  f->start_dict_title = "当前词典";
  f->started_from_current = 1;
  f->translate_enabled = 1;
  f->auto_translate = 1;
  f->translate_has_key = 1;
  f->translate_supported = 1;
}

#define DECIDE(call, out)                                                          \
  do {                                                                             \
    dsh_fallback_decide((call), &(out));                                           \
    if ((out).oom) {                                                               \
      g_checks++;                                                                  \
      g_failed++;                                                                  \
      fprintf(stderr, "FAIL 决策函数报了内存不足（这一条用例不该涉及分配失败）\n");  \
      dump_plan("      实际结论", &(out));                                          \
    }                                                                              \
  } while (0)

int main(void) {
  const size_t base = dsh_mem_live_count();
  dsh_fallback_result r;

  /* ══ ① 输入框那条路 ══ */
  {
    dsh_fallback_facts a;
    facts_default(&a, DSH_ORIGIN_INPUT, DSH_SCRIPT_LATIN);
    DECIDE(&a, r);
    ok(r.action == DSH_FALLBACK_SUGGEST && !r.stop,
       "① 英文输入：先去算当前词典的联想候选（不结束）");
    ok(r.via != NULL && strcmp(r.via, "current") == 0 && r.surface == DSH_SURFACE_LIST,
       "① 这一步落在输入框下方的候选列表上");
    dsh_fallback_result_dispose(&r);

    dsh_fallback_facts h;
    facts_default(&h, DSH_ORIGIN_INPUT, DSH_SCRIPT_HAN);
    DECIDE(&h, r);
    ok(r.action == DSH_FALLBACK_LOOKUP && !r.stop,
       "① ★ 汉字输入：**跳过当前词典的联想**，直接进查询");
    dsh_fallback_result_dispose(&r);

    /* ★★ 音节分隔点：把「先原样问、问不到才去点」这条顺序钉死 —— 带点的词**跳过联想、直接问词典**
     * （拿 `pro·gress` 去拼前缀只有噪音，还会把链停在候选列表上）；没命中且没试过 → 去掉点再问一遍，
     * 已经试过 → **不许再来一次**（否则来回重问）；链接 / 回退 / 历史那条路同样先去点、之后才到此为止。 */
    dsh_fallback_facts dot;
    facts_default(&dot, DSH_ORIGIN_INPUT, DSH_SCRIPT_LATIN);
    dot.has_separator_dots = 1;
    DECIDE(&dot, r);
    ok(r.action == DSH_FALLBACK_LOOKUP && !r.stop,
       "① ★★ 带音节分隔点的输入：**先问词典**（不做前缀联想）");
    dsh_fallback_result_dispose(&r);

    dsh_fallback_facts after;
    facts_default(&after, DSH_ORIGIN_SELECTION, DSH_SCRIPT_LATIN);
    after.stage = DSH_STAGE_AFTER_LOOKUP;
    after.has_separator_dots = 1;
    DECIDE(&after, r);
    ok(r.action == DSH_FALLBACK_RELOOKUP && !r.stop,
       "① ★★ 选区那条路：没命中 + 带点 + 没试过 → **去掉点再问一遍**（排在借查前面）");
    dsh_fallback_result_dispose(&r);

    after.separator_retried = 1;
    DECIDE(&after, r);
    ok(r.action == DSH_FALLBACK_PROBE,
       "① ★★ 已经去点问过一次了：**不许再来一次**，照旧往下走借查");
    dsh_fallback_result_dispose(&r);

    dsh_fallback_facts link;
    facts_default(&link, DSH_ORIGIN_LINK, DSH_SCRIPT_LATIN);
    link.stage = DSH_STAGE_AFTER_LOOKUP;
    link.has_separator_dots = 1;
    DECIDE(&link, r);
    ok(r.action == DSH_FALLBACK_RELOOKUP && !r.stop,
       "① ★★ 链接那条不走查词通道的路：也先去点问一次，然后才「到此为止」");
    dsh_fallback_result_dispose(&r);
    link.separator_retried = 1;
    DECIDE(&link, r);
    ok(r.action == DSH_FALLBACK_TERMINAL,
       "① ★ 去点之后还是没有 → 链接那条路照旧收在「这本词典里没有这一条」");
    dsh_fallback_result_dispose(&r);

    dsh_fallback_facts s;
    facts_default(&s, DSH_ORIGIN_INPUT, DSH_SCRIPT_LATIN);
    s.stage = DSH_STAGE_AFTER_SUGGEST;
    s.suggestion_count = 3;
    DECIDE(&s, r);
    ok(r.action == DSH_FALLBACK_SUGGEST && r.stop && r.surface == DSH_SURFACE_LIST,
       "① 有候选（且没有精确命中）→ 摆候选列表并**停下等用户选**（ B 第 1 条）");
    dsh_fallback_result_dispose(&r);

    /* ★ 精确命中那一格（`suggestion_has_exact`）：依据是参考实现的**实际行为** —— 联想回来后先找
     * `kind === 'exact'` 那一条、有就直接查它。少了这一格，输入框里打一个确实存在的词按回车，
     * 屏幕上永远只有候选、正文永远空着。 */
    dsh_fallback_facts ex;
    facts_default(&ex, DSH_ORIGIN_INPUT, DSH_SCRIPT_LATIN);
    ex.stage = DSH_STAGE_AFTER_SUGGEST;
    ex.suggestion_count = 3;
    ex.suggestion_has_exact = 1;
    DECIDE(&ex, r);
    dump_plan("afterSuggest/有精确命中", &r);
    ok(r.action == DSH_FALLBACK_LOOKUP && !r.stop,
       "① ★ 候选里有一条精确命中 → **接着往下查**，不许停在列表上（参考实现的第 ③ 步）");
    dsh_fallback_result_dispose(&r);

    dsh_fallback_facts hc;
    facts_default(&hc, DSH_ORIGIN_INPUT, DSH_SCRIPT_HAN);
    hc.stage = DSH_STAGE_AFTER_SUGGEST;
    hc.suggestion_count = 3;
    DECIDE(&hc, r);
    ok(r.action == DSH_FALLBACK_LOOKUP,
       "① ★ 汉字输入即使当前词典给了候选也不摆（那一档根本不该发生）");
    dsh_fallback_result_dispose(&r);

    dsh_fallback_facts nc;
    facts_default(&nc, DSH_ORIGIN_INPUT, DSH_SCRIPT_LATIN);
    nc.stage = DSH_STAGE_AFTER_SUGGEST;
    nc.suggestion_count = 0;
    DECIDE(&nc, r);
    ok(r.action == DSH_FALLBACK_LOOKUP && !r.stop, "① 没有候选 → 继续查当前词典");
    dsh_fallback_result_dispose(&r);

    dsh_fallback_facts found;
    facts_default(&found, DSH_ORIGIN_INPUT, DSH_SCRIPT_LATIN);
    found.stage = DSH_STAGE_AFTER_LOOKUP;
    found.entry_found = 1;
    DECIDE(&found, r);
    ok(r.action == DSH_FALLBACK_SHOW && r.stop && strcmp(r.via, "current") == 0,
       "① 当前词典命中 → 显示并结束（via=current）");
    dsh_fallback_result_dispose(&r);

    dsh_fallback_facts miss;
    facts_default(&miss, DSH_ORIGIN_INPUT, DSH_SCRIPT_LATIN);
    miss.stage = DSH_STAGE_AFTER_LOOKUP;
    miss.entry_found = 0;
    DECIDE(&miss, r);
    ok(r.action == DSH_FALLBACK_PROBE && !r.stop && strcmp(r.via, "borrow") == 0,
       "① 当前词典没有 → 去问别的词典（不结束）");
    ok(!r.offer_recheck, "① 去问别本这一步先不给「再问一遍」");
    dsh_fallback_result_dispose(&r);

    dsh_fallback_facts borrowed;
    facts_default(&borrowed, DSH_ORIGIN_INPUT, DSH_SCRIPT_LATIN);
    borrowed.stage = DSH_STAGE_AFTER_PROBE;
    borrowed.hit_dict_id = "dict-oxford";
    borrowed.hit_dict_title = "牛津高阶";
    DECIDE(&borrowed, r);
    ok(r.action == DSH_FALLBACK_SHOW && r.dict_id != NULL &&
           strcmp(r.dict_id, "dict-oxford") == 0 && strcmp(r.via, "borrow") == 0 && r.stop,
       "① ★ 别本命中 → 用**那一本**显示，而且 DictId 就是它（不切当前词典）");
    ok(has(r.reason, "牛津高阶"), "① 借查那一页的解释行里写着是哪一本");
    dsh_fallback_result_dispose(&r);

    /* 四个条件全满足 → 自动翻译 */
    {
      const char *const names[] = {"总开关开着", "自动开关开着", "有 Key", "语种支持"};
      for (int i = 0; i < 4; i++) {
        dsh_fallback_facts t;
        facts_default(&t, DSH_ORIGIN_INPUT, DSH_SCRIPT_LATIN);
        t.stage = DSH_STAGE_AFTER_PROBE;
        DECIDE(&t, r);
        char label[128];
        snprintf(label, sizeof(label), "① 四个条件全满足（%s）→ 自动翻译", names[i]);
        ok(r.action == DSH_FALLBACK_TRANSLATE && strcmp(r.via, "translate") == 0, label);
        dsh_fallback_result_dispose(&r);
      }
    }
    /* 缺任意一个 → 不许翻译，落到终态页 */
    {
      for (int i = 0; i < 4; i++) {
        dsh_fallback_facts t;
        facts_default(&t, DSH_ORIGIN_INPUT, DSH_SCRIPT_LATIN);
        t.stage = DSH_STAGE_AFTER_PROBE;
        const char *name = "";
        switch (i) {
          case 0: t.translate_enabled = 0; name = "总开关关"; break;
          case 1: t.auto_translate = 0; name = "自动开关关"; break;
          case 2: t.translate_has_key = 0; name = "没 Key"; break;
          default: t.translate_supported = 0; name = "语种不支持"; break;
        }
        DECIDE(&t, r);
        char label[160];
        snprintf(label, sizeof(label), "① 缺一个条件（%s）→ **不许**自动翻译，落到终态页", name);
        ok(r.action == DSH_FALLBACK_TERMINAL && strcmp(r.via, "terminal") == 0, label);
        dsh_fallback_result_dispose(&r);
      }
    }
  }

  /* ══ ② 选区那条路 ══ */
  {
    dsh_fallback_facts a;
    facts_default(&a, DSH_ORIGIN_SELECTION, DSH_SCRIPT_LATIN);
    DECIDE(&a, r);
    ok(r.action == DSH_FALLBACK_LOOKUP, "② 选区起点：先问当前词典落点");
    dsh_fallback_result_dispose(&r);

    dsh_fallback_facts miss;
    facts_default(&miss, DSH_ORIGIN_SELECTION, DSH_SCRIPT_LATIN);
    miss.stage = DSH_STAGE_AFTER_LOOKUP;
    DECIDE(&miss, r);
    ok(r.action == DSH_FALLBACK_PROBE, "② 落点为空 → 问别的词典");
    dsh_fallback_result_dispose(&r);

    dsh_fallback_facts borrowed;
    facts_default(&borrowed, DSH_ORIGIN_SELECTION, DSH_SCRIPT_LATIN);
    borrowed.stage = DSH_STAGE_AFTER_PROBE;
    borrowed.hit_dict_id = "dict-2";
    borrowed.hit_dict_title = "第二本";
    DECIDE(&borrowed, r);
    ok(r.action == DSH_FALLBACK_SHOW && r.stop,
       "② 别本命中 → 跳过去显示（这是「在看词条时又查一个词」，要压栈）");
    dsh_fallback_result_dispose(&r);

    dsh_fallback_facts ws;
    facts_default(&ws, DSH_ORIGIN_SELECTION, DSH_SCRIPT_LATIN);
    ws.stage = DSH_STAGE_AFTER_PROBE;
    ws.suggestion_count = 2;
    DECIDE(&ws, r);
    ok(r.action == DSH_FALLBACK_SUGGEST && r.surface == DSH_SURFACE_TOAST && r.stop,
       "② ★ 都没命中但有候选 → 候选**并进正文框底部提示**（不摆列表顶掉正文）");
    ok(strcmp(r.via, "terminal") == 0,
       "② 这一档的结局仍然是 terminal（界面据 `surface` **不替换正文**）");
    dsh_fallback_result_dispose(&r);
    /* ★ 那句提示的前半句**由内核拼**（参考实现里在界面上）：≤4 个词就把词写出来、≥5 个词只说
     * 「所选文本」；没问完时不说「别的词典里也没有」。 */
    ws.query = "applz";
    DECIDE(&ws, r);
    ok_eq_str(r.reason, "applz未在《当前词典》中查到，别的词典里也没有",
              "② ★ 选区没查到那句提示：≤4 个词 → 把词写出来（连书名一起）");
    dsh_fallback_result_dispose(&r);
    ws.query = "take care of";
    DECIDE(&ws, r);
    ok_eq_str(r.reason, "take care of未在《当前词典》中查到，别的词典里也没有",
              "② 三个词（按空白切）照样写出来");
    dsh_fallback_result_dispose(&r);
    ws.query = "take care of them all";
    DECIDE(&ws, r);
    ok_eq_str(r.reason, "所选文本未在《当前词典》中查到，别的词典里也没有",
              "② ★ 五个词 → 只说「所选文本」（别把一整句抄进提示里）");
    dsh_fallback_result_dispose(&r);
    ws.query = "今天天气真好";
    DECIDE(&ws, r);
    ok_eq_str(r.reason, "所选文本未在《当前词典》中查到，别的词典里也没有",
              "② ★ 中日韩没有空格 → **逐字计数**（六个字 = 六个词 → 说「所选文本」）");
    dsh_fallback_result_dispose(&r);
    ws.query = "苹果";
    DECIDE(&ws, r);
    ok_eq_str(r.reason, "苹果未在《当前词典》中查到，别的词典里也没有",
              "② 而两个字 = 两个词 → 写出来（与参考实现同一档）");
    dsh_fallback_result_dispose(&r);
    /* ⚠️ **有词典没问完时不许说「别的词典里也没有」**：那句话是替没被问过的对象下结论，
     * 没问完那半句由 `unconfirmed_note` 另给。 */
    ws.unconfirmed[0] = "第二本";
    ws.unconfirmed_count = 1;
    ws.query = "applz";
    DECIDE(&ws, r);
    ok_eq_str(r.reason, "applz未在《当前词典》中查到",
              "② ★★ 有词典没问完 → **不说**「别的词典里也没有」（那半句由 unconfirmed_note 给）");
    dsh_fallback_result_dispose(&r);

    dsh_fallback_facts ns;
    facts_default(&ns, DSH_ORIGIN_SELECTION, DSH_SCRIPT_LATIN);
    ns.stage = DSH_STAGE_AFTER_PROBE;
    ns.suggestion_count = 0;
    DECIDE(&ns, r);
    ok(r.action == DSH_FALLBACK_TRANSLATE,
       "② 没候选 + 翻译可用 → 自动翻译（译文进正文框）");
    dsh_fallback_result_dispose(&r);

    dsh_fallback_facts blocked;
    facts_default(&blocked, DSH_ORIGIN_SELECTION, DSH_SCRIPT_LATIN);
    blocked.stage = DSH_STAGE_AFTER_PROBE;
    blocked.auto_translate = 0;
    blocked.query = "applz";
    DECIDE(&blocked, r);
    ok(r.action == DSH_FALLBACK_TERMINAL, "② 翻译不可用 → 终态页");
    /* ★★ 选区那条路的**终态也是正文框底部一句提示**（不替换正文），所以 `surface` 必须是 `toast`：
     * 界面**只看 `surface`** 决定换不换正文，它手里没有 `origin`（那是产品约定，不该由它判）。 */
    ok(r.surface == DSH_SURFACE_TOAST,
       "② ★★ 选区那条路的终态 → `surface = toast`（**不替换正文**，只在底部说一句）");
    ok_eq_str(r.reason, "applz未在《当前词典》中查到，别的词典里也没有",
              "② ★ 那一句同样是内核拼的");
    dsh_fallback_result_dispose(&r);

    /* 输入框那条路的终态**照旧是整页**（`surface = none`）：它是「用户主动查一个词」 */
    {
      dsh_fallback_facts in_term;
      facts_default(&in_term, DSH_ORIGIN_INPUT, DSH_SCRIPT_LATIN);
      in_term.stage = DSH_STAGE_AFTER_PROBE;
      in_term.auto_translate = 0;
      DECIDE(&in_term, r);
      ok(r.action == DSH_FALLBACK_TERMINAL && r.surface == DSH_SURFACE_NONE,
         "② ★ 输入框那条路的终态仍是整页（`surface = none`）—— 与选区那条路**不是一档**");
      dsh_fallback_result_dispose(&r);
    }

    /* 选区那条路也**必须**在起点被认成「先问落点」，而不是直接联想 —— 参考实现那一版它整条漏过。 */
    dsh_fallback_facts sel_han;
    facts_default(&sel_han, DSH_ORIGIN_SELECTION, DSH_SCRIPT_HAN);
    DECIDE(&sel_han, r);
    ok(r.action == DSH_FALLBACK_LOOKUP,
       "② 选区那条路无论什么字形都先问落点（参考实现那个坑：它整整漏过一轮）");
    dsh_fallback_result_dispose(&r);
  }

  /* ══ ②b 连续借查：命中当前词典时**不是**借查 ══ */
  {
    dsh_fallback_facts miss;
    facts_default(&miss, DSH_ORIGIN_SELECTION, DSH_SCRIPT_LATIN);
    miss.start_dict_title = "link";
    miss.started_from_current = 0;
    miss.stage = DSH_STAGE_AFTER_LOOKUP;
    DECIDE(&miss, r);
    ok(has(r.reason, "《link》") && !has(r.reason, "当前词典"),
       "②b ★ 起点是借来的那一本时，「谁没有」不许说成「当前词典没有」（那是假话）");
    dsh_fallback_result_dispose(&r);

    dsh_fallback_facts home;
    facts_default(&home, DSH_ORIGIN_SELECTION, DSH_SCRIPT_LATIN);
    home.start_dict_title = "link";
    home.started_from_current = 0;
    home.stage = DSH_STAGE_AFTER_PROBE;
    home.hit_dict_id = "d-test";
    home.hit_dict_title = "test";
    home.hit_is_current = 1;
    DECIDE(&home, r);
    ok(r.action == DSH_FALLBACK_SHOW && strcmp(r.via, "current") == 0 &&
           (r.reason == NULL || r.reason[0] == '\0') && r.dict_id != NULL &&
           strcmp(r.dict_id, "d-test") == 0,
       "②b ★ 借查绕回**当前词典** → via=current（不是 borrow），而且没有「借查」那行解释");
    dsh_fallback_result_dispose(&r);

    dsh_fallback_facts third;
    facts_default(&third, DSH_ORIGIN_SELECTION, DSH_SCRIPT_LATIN);
    third.start_dict_title = "link";
    third.started_from_current = 0;
    third.stage = DSH_STAGE_AFTER_PROBE;
    third.hit_dict_id = "d-oxford";
    third.hit_dict_title = "牛津高阶";
    DECIDE(&third, r);
    ok(strcmp(r.via, "borrow") == 0 && has(r.reason, "《link》里没有") &&
           has(r.reason, "牛津高阶") && !has(r.reason, "当前词典"),
       "②b ★ 命中第三本时点名的是**起点那本**（《link》里没有 · 已用《牛津高阶》借查）");
    dsh_fallback_result_dispose(&r);

    /* 「没问完」照旧要贴上去 —— 唯一不许让步的一条，与「这一页从哪来」无关 */
    dsh_fallback_facts unconfirmed;
    facts_default(&unconfirmed, DSH_ORIGIN_SELECTION, DSH_SCRIPT_LATIN);
    unconfirmed.start_dict_title = "link";
    unconfirmed.started_from_current = 0;
    unconfirmed.stage = DSH_STAGE_AFTER_PROBE;
    unconfirmed.hit_dict_id = "d-test";
    unconfirmed.hit_dict_title = "test";
    unconfirmed.hit_is_current = 1;
    unconfirmed.unconfirmed[0] = "大词典";
    unconfirmed.unconfirmed_count = 1;
    DECIDE(&unconfirmed, r);
    ok(has(r.unconfirmed_note, "大词典") && r.offer_recheck,
       "②b ★ 收成「当前词典命中」之后，「另有 N 本没能确认」照样要贴（那个坑不许让步）");
    dsh_fallback_result_dispose(&r);

    /* 回归：输入框那条路一个字都不许变 */
    dsh_fallback_facts inputPath;
    facts_default(&inputPath, DSH_ORIGIN_INPUT, DSH_SCRIPT_LATIN);
    inputPath.stage = DSH_STAGE_AFTER_PROBE;
    inputPath.hit_dict_id = "d-oxford";
    inputPath.hit_dict_title = "牛津高阶";
    DECIDE(&inputPath, r);
    ok_eq_str(r.reason, "当前词典没有 · 已用《牛津高阶》借查",
              "②b 回归：输入框那条路照旧说「当前词典没有 · 已用《牛津高阶》借查」");
    ok(strcmp(r.via, "borrow") == 0, "②b 回归：输入框那条路照旧 via=borrow");
    dsh_fallback_result_dispose(&r);
  }

  /* ══ ③ 链接 / 回退 / 历史：**在起点那一本里精确查一次**，不走查词通道 ══
   * ⚠️ 参考实现在不进查词通道时走的是 `Engine.Lookup(stored, word)` —— **查一次**，只是不跑借查 /
   * 联想兜底 / 翻译那三步；写成「什么都不查、直接显示」会让 `entry://` 链接点出一张空白页。 */
  {
    const struct {
      enum dsh_origin origin;
      const char *name;
    } ORIGINS[] = {{DSH_ORIGIN_LINK, "link"}, {DSH_ORIGIN_BACK, "back"},
                   {DSH_ORIGIN_HISTORY, "history"}};
    for (size_t i = 0; i < sizeof(ORIGINS) / sizeof(ORIGINS[0]); i++) {
      dsh_fallback_facts t;
      facts_default(&t, ORIGINS[i].origin, DSH_SCRIPT_LATIN);
      DECIDE(&t, r);
      char label[200];
      snprintf(label, sizeof(label),
               "③ `%s` 起点那一步先**查一次**（Lookup 且 stop=0）", ORIGINS[i].name);
      ok(r.action == DSH_FALLBACK_LOOKUP && !r.stop, label);
      dsh_fallback_result_dispose(&r);

      /* 查到了 → 显示；**不借查**（via 记 current） */
      facts_default(&t, ORIGINS[i].origin, DSH_SCRIPT_LATIN);
      t.stage = DSH_STAGE_AFTER_LOOKUP;
      t.entry_found = 1;
      DECIDE(&t, r);
      snprintf(label, sizeof(label), "③ `%s` 命中 → 显示且 via=current", ORIGINS[i].name);
      ok(r.action == DSH_FALLBACK_SHOW && r.stop && r.via != NULL &&
             strcmp(r.via, "current") == 0,
         label);
      dsh_fallback_result_dispose(&r);

      /* 没查到 → **到此为止**：不许借查、不许翻译、也不给「再问一遍」 */
      facts_default(&t, ORIGINS[i].origin, DSH_SCRIPT_LATIN);
      t.stage = DSH_STAGE_AFTER_LOOKUP;
      t.entry_found = 0;
      DECIDE(&t, r);
      snprintf(label, sizeof(label),
               "③ ★ `%s` 没查到也不去借查/翻译（终态页，不是 Probe/Translate）",
               ORIGINS[i].name);
      ok(r.action == DSH_FALLBACK_TERMINAL && r.stop, label);
      ok(!r.offer_recheck && !r.offer_translate,
         "③ 这条路上一次探路都没发生 → 不给「再问一遍」「翻译」");
      ok(r.reason != NULL && r.reason[0] != '\0', "③ 没查到要有一句人话");
      dsh_fallback_result_dispose(&r);
    }

    dsh_fallback_facts missing;
    facts_default(&missing, DSH_ORIGIN_HISTORY, DSH_SCRIPT_LATIN);
    missing.dictionary_missing = 1;
    missing.target_dict_title = "新世纪汉英大词典";
    DECIDE(&missing, r);
    ok(r.action == DSH_FALLBACK_EXPLAIN_ERROR && has(r.reason, "新世纪汉英大词典"),
       "③ ★ 那本已被移除 → 只说话、不查词，而且说清是哪一本");
    ok(has(r.reason, "原路径"), "③ 移除那一句要给出恢复办法（重新导入到原路径）");
    char *missing_reason = local_dup(r.reason);
    dsh_fallback_result_dispose(&r);

    dsh_fallback_facts fileGone;
    facts_default(&fileGone, DSH_ORIGIN_HISTORY, DSH_SCRIPT_LATIN);
    fileGone.dictionary_file_gone = 1;
    fileGone.target_dict_title = "新世纪汉英大词典";
    DECIDE(&fileGone, r);
    ok(r.action == DSH_FALLBACK_EXPLAIN_ERROR && r.reason != NULL &&
           (missing_reason == NULL || strcmp(r.reason, missing_reason) != 0),
       "③ 「文件丢了」与「已被移除」**不是同一句话**（两件事分开说）");
    dsh_fallback_result_dispose(&r);
    if (missing_reason != NULL) free(missing_reason);

    dsh_fallback_facts replay;
    facts_default(&replay, DSH_ORIGIN_HISTORY, DSH_SCRIPT_LATIN);
    replay.entry_is_translation = 1;
    DECIDE(&replay, r);
    ok(r.action == DSH_FALLBACK_TRANSLATE && strcmp(r.via, "translate") == 0 &&
           r.stop,
       "③ 译文条目回放 → 走翻译（缓存），不重新查词典");
    dsh_fallback_result_dispose(&r);

    /* 不走查词通道的入口**不带**「没问完」那行 —— 它连别的词典都不问 */
    dsh_fallback_facts linkUnconfirmed;
    facts_default(&linkUnconfirmed, DSH_ORIGIN_LINK, DSH_SCRIPT_LATIN);
    linkUnconfirmed.unconfirmed[0] = "牛津";
    linkUnconfirmed.unconfirmed_count = 1;
    linkUnconfirmed.stage = DSH_STAGE_AFTER_LOOKUP;
    linkUnconfirmed.entry_found = 0;
    DECIDE(&linkUnconfirmed, r);
    ok(r.action == DSH_FALLBACK_TERMINAL,
       "③ 链接那条路即使带着「没问完」的事实也停在终态页（它不问别的词典）");
    ok(!r.offer_recheck && (r.unconfirmed_note == NULL || r.unconfirmed_note[0] == '\0'),
       "③ 链接那条路不问别的词典，所以也没有「再问一遍」");
    dsh_fallback_result_dispose(&r);
  }

  /* ══ ④ 终态页：「翻译为什么没用上」四档**分开说** ══ */
  {
    char *why[4] = {NULL, NULL, NULL, NULL};
    const char *names[4] = {"总开关关", "自动关", "没 Key", "语种不支持"};
    for (int i = 0; i < 4; i++) {
      dsh_fallback_facts t;
      facts_default(&t, DSH_ORIGIN_INPUT, DSH_SCRIPT_LATIN);
      t.stage = DSH_STAGE_AFTER_PROBE;
      switch (i) {
        case 0: t.translate_enabled = 0; break;
        case 1: t.auto_translate = 0; break;
        case 2: t.translate_has_key = 0; break;
        default:
          t.translate_supported = 0;
          t.translate_unsupported_message = "这个语种不在支持表里（机器翻译目前支持 32 种）";
          break;
      }
      DECIDE(&t, r);
      char label[128];
      snprintf(label, sizeof(label), "④ 终态页给「翻译为什么没用上」的说明（%s）", names[i]);
      ok(r.translate_why != NULL && r.translate_why[0] != '\0', label);
      why[i] = r.translate_why;
      r.translate_why = NULL; /* 留在手上继续比，别被 dispose 收走 */
      dsh_fallback_result_dispose(&r);
    }
    int distinct = 1;
    for (int i = 0; i < 4 && distinct; i++) {
      for (int k = i + 1; k < 4; k++) {
        if (why[i] != NULL && why[k] != NULL && strcmp(why[i], why[k]) == 0) distinct = 0;
      }
    }
    ok(distinct, "④ ★ 四句话互不相同（合并成一句就等于把用户指去改错的地方）");
    ok(has(why[0], "选项"), "④ ① 总开关关 → 指去「选项 → 翻译」");
    ok(has(why[1], "翻译这个词"), "④ ② 自动开关关 → 给一句**可点**的出路");
    ok(has(why[2], "语音") && has(why[2], "API Key"),
       "④ ③ 没 Key → 说清它与语音共用一把、去哪儿填");
    ok(has(why[3], "32 种"), "④ ④ 语种不支持 → 照抄参考实现那句（不自己编）");
    for (int i = 0; i < 4; i++) {
      if (why[i] != NULL) dsh_release(why[i]);
    }

    dsh_fallback_facts autoOff;
    facts_default(&autoOff, DSH_ORIGIN_INPUT, DSH_SCRIPT_LATIN);
    autoOff.stage = DSH_STAGE_AFTER_PROBE;
    autoOff.auto_translate = 0;
    DECIDE(&autoOff, r);
    ok(r.offer_translate,
       "④ ★ 自动开关关着时，终态页必须给一条**可点的**「翻译这个词」");
    dsh_fallback_result_dispose(&r);

    dsh_fallback_facts masterOff;
    facts_default(&masterOff, DSH_ORIGIN_INPUT, DSH_SCRIPT_LATIN);
    masterOff.stage = DSH_STAGE_AFTER_PROBE;
    masterOff.translate_enabled = 0;
    DECIDE(&masterOff, r);
    ok(!r.offer_translate, "④ 总开关关着时不给那条可点出路（那时点了也没用，该指去选项）");
    dsh_fallback_result_dispose(&r);

    dsh_fallback_facts both;
    facts_default(&both, DSH_ORIGIN_INPUT, DSH_SCRIPT_LATIN);
    both.stage = DSH_STAGE_AFTER_PROBE;
    both.translate_enabled = 0;
    both.auto_translate = 0;
    DECIDE(&both, r);
    ok(has(r.translate_why, "总开关"),
       "④ 总开关关 + 自动关：说的是**总开关**那一档（先解决不可用，再说自动）");
    dsh_fallback_result_dispose(&r);
  }

  /* ══ ⑤ 「没问完」永远不等于「都没有」 ══ */
  {
    const char *pending[2] = {"牛津高阶", "朗文当代"};

    dsh_fallback_facts terminal;
    facts_default(&terminal, DSH_ORIGIN_INPUT, DSH_SCRIPT_LATIN);
    terminal.stage = DSH_STAGE_AFTER_PROBE;
    terminal.auto_translate = 0;
    terminal.unconfirmed[0] = pending[0];
    terminal.unconfirmed[1] = pending[1];
    terminal.unconfirmed_count = 2;
    DECIDE(&terminal, r);
    ok(has(r.unconfirmed_note, "另有 2 本没能确认"), "⑤ 终态页带「另有 N 本没能确认」");
    ok(r.offer_recheck, "⑤ ★ 终态页带「再问一遍」—— 本方案唯一不许让步的一条");
    ok(has(r.unconfirmed_note, pending[0]) && has(r.unconfirmed_note, pending[1]),
       "⑤ 那句话里点名是哪几本（用户要知道漏了谁）");
    ok(r.unconfirmed_count == 2 && r.unconfirmed_names[0] != NULL &&
           strcmp(r.unconfirmed_names[0], pending[0]) == 0,
       "⑤ 名字也**原样**带上（界面那个 data-unconfirmed 标记要的是名字清单）");
    dsh_fallback_result_dispose(&r);

    dsh_fallback_facts translating;
    facts_default(&translating, DSH_ORIGIN_INPUT, DSH_SCRIPT_LATIN);
    translating.stage = DSH_STAGE_AFTER_PROBE;
    translating.unconfirmed[0] = pending[0];
    translating.unconfirmed[1] = pending[1];
    translating.unconfirmed_count = 2;
    DECIDE(&translating, r);
    ok(r.action == DSH_FALLBACK_TRANSLATE, "⑤ ★ 没问完也照样继续翻译（翻译不许被挡住）");
    ok(has(r.unconfirmed_note, "没能确认") && r.offer_recheck,
       "⑤ ★ 但翻译那一页同样要带「没能确认 + 再问一遍」");
    dsh_fallback_result_dispose(&r);

    dsh_fallback_facts borrowed;
    facts_default(&borrowed, DSH_ORIGIN_INPUT, DSH_SCRIPT_LATIN);
    borrowed.stage = DSH_STAGE_AFTER_PROBE;
    borrowed.hit_dict_id = "d1";
    borrowed.hit_dict_title = "柯林斯";
    borrowed.unconfirmed[0] = pending[0];
    borrowed.unconfirmed[1] = pending[1];
    borrowed.unconfirmed_count = 2;
    DECIDE(&borrowed, r);
    ok(has(r.unconfirmed_note, "没能确认") && r.offer_recheck,
       "⑤ 借查命中那一页也要带（「最终那一页」包含它）");
    dsh_fallback_result_dispose(&r);

    dsh_fallback_facts allDone;
    facts_default(&allDone, DSH_ORIGIN_INPUT, DSH_SCRIPT_LATIN);
    allDone.stage = DSH_STAGE_AFTER_PROBE;
    allDone.auto_translate = 0;
    DECIDE(&allDone, r);
    ok(r.unconfirmed_note != NULL && r.unconfirmed_note[0] == '\0' && !r.offer_recheck,
       "⑤ 全部问完时那句话是**空串**（这时才可以说「别的词典里也没有」）");
    dsh_fallback_result_dispose(&r);
  }

  /* ══ ⑥ 边界与不变式 ══ */
  {
    dsh_fallback_decide(NULL, &r);
    /* 参考实现是 `f = f ?? new FallbackFacts()`：那个默认对象是 `origin=input / stage=start /
     * script=latin`，所以 NULL 进这一档得到的是 **Suggest**，不是「什么都不做」。 */
    ok(r.action == DSH_FALLBACK_SUGGEST && !r.stop && strcmp(r.via, "current") == 0,
       "⑥ facts 为 NULL 不崩，且按参考实现那个默认对象走（origin=input/start/latin）");
    dsh_fallback_result_dispose(&r);

    ok(dsh_fallback_runs_channel(DSH_ORIGIN_INPUT) &&
           dsh_fallback_runs_channel(DSH_ORIGIN_SELECTION) &&
           !dsh_fallback_runs_channel(DSH_ORIGIN_LINK) &&
           !dsh_fallback_runs_channel(DSH_ORIGIN_BACK) &&
           !dsh_fallback_runs_channel(DSH_ORIGIN_HISTORY),
       "⑥ 只有 input / selection 走查词通道（参考实现 D11）");

    /* 未知阶段：停住并说明，绝不乱跳 */
    {
      dsh_fallback_facts t;
      facts_default(&t, DSH_ORIGIN_INPUT, DSH_SCRIPT_LATIN);
      t.stage = DSH_STAGE_DONE;
      DECIDE(&t, r);
      ok(r.action == DSH_FALLBACK_SHOW && r.stop && has(r.reason, "未知阶段"),
         "⑥ 未知阶段：只显示、停下，并说明是未知阶段");
      dsh_fallback_result_dispose(&r);
    }

    /* 阶段名与动作名要认得全（写日志与现场实测结果靠它） */
    ok(strcmp(dsh_fallback_stage_name(DSH_STAGE_AFTER_LOOKUP), "afterLookup") == 0 &&
           strcmp(dsh_fallback_stage_name(DSH_STAGE_AFTER_PROBE), "afterProbe") == 0 &&
           strcmp(dsh_fallback_stage_name(DSH_STAGE_AFTER_SUGGEST), "afterSuggest") == 0 &&
           strcmp(dsh_fallback_stage_name(DSH_STAGE_START), "start") == 0,
       "⑥ 四个阶段名与参考实现逐字相同（它们出现在界面与日志里）");
    ok(strcmp(dsh_fallback_action_name(DSH_FALLBACK_PROBE), "Probe") == 0 &&
           strcmp(dsh_fallback_action_name(DSH_FALLBACK_EXPLAIN_ERROR), "ExplainError") == 0,
       "⑥ 动作名认得出来");

    /* 都没拼出说明句的正常档位，不许把 oom 标起来 */
    {
      dsh_fallback_facts t;
      facts_default(&t, DSH_ORIGIN_INPUT, DSH_SCRIPT_LATIN);
      t.stage = DSH_STAGE_AFTER_LOOKUP;
      t.entry_found = 1;
      DECIDE(&t, r);
      ok(!r.oom, "⑥ 命中那一档没有说明句，但**不是**内存不足");
      dsh_fallback_result_dispose(&r);
    }
  }

  /* ══ ⑦ 查词通道自己**不许**判汉字：字形只由调用方填，内核侧只许有一处来源 ══
   * 参考实现那一侧的同一道检查也是扫源码找汉字区间的字面量；检查标准是**死事实**（找那两个字面量），
   * 不是「谁调用了谁」的正则猜测。 */
  {
    static const char *const FILES[] = {"engine/dsh_fallback.c", "engine/dsh_fallback.h"};
    static const char *const HAN_LITERALS[] = {"0x4E00", "0x9FFF", "4e00-9fff", "\\u4e00"};
    for (size_t i = 0; i < sizeof(FILES) / sizeof(FILES[0]); i++) {
      char path[1024];
      snprintf(path, sizeof(path), "%s/%s", DSH_SRC_DIR, FILES[i]);
      FILE *fp = fopen(path, "rb");
      char label[256];
      snprintf(label, sizeof(label), "⑦ 读得到 %s（否则这条检查标准是空转的）", FILES[i]);
      ok(fp != NULL, label);
      if (fp == NULL) continue;
      char buf[65536];
      const size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
      buf[n] = '\0';
      fclose(fp);
      for (size_t k = 0; k < sizeof(HAN_LITERALS) / sizeof(HAN_LITERALS[0]); k++) {
        snprintf(label, sizeof(label),
                 "⑦ ★ %s 里不许有汉字区间的字面量（%s）—— 字形只许由调用方填",
                 FILES[i], HAN_LITERALS[k]);
        ok(strstr(buf, HAN_LITERALS[k]) == NULL, label);
      }
    }
    /* 调用方那一侧确实是用内核唯一那处判定填的 */
    {
      char path[1024];
      snprintf(path, sizeof(path), "%s/engine/dsh_lookup_api.c", DSH_SRC_DIR);
      FILE *fp = fopen(path, "rb");
      ok(fp != NULL, "⑦ 读得到 engine/dsh_lookup_api.c");
      if (fp != NULL) {
        char buf[262144];
        const size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
        buf[n] = '\0';
        fclose(fp);
        ok(strstr(buf, "dsh_script_of") != NULL,
           "⑦ ★ 填 facts.script 的那一处用的是 dsh_script_of（唯一来源，那个坑）");
        /* 调用方自己也不许写一份汉字判定 */
        ok(strstr(buf, "0x4E00") == NULL && strstr(buf, "0x9FFF") == NULL,
           "⑦ ★ 调用方自己也不许再写一份汉字区间（那是第二处来源）");
      }
    }
    /* 反过来：那份判定**确实**还在（否则上面两条是空转的）*/
    {
      char path[1024];
      snprintf(path, sizeof(path), "%s/text/dsh_language.c", DSH_SRC_DIR);
      FILE *fp = fopen(path, "rb");
      ok(fp != NULL, "⑦ 读得到 text/dsh_language.c");
      if (fp != NULL) {
        char buf[262144];
        const size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
        buf[n] = '\0';
        fclose(fp);
        ok(strstr(buf, "0x4E00") != NULL,
           "⑦ ★ 汉字区间就在 text/dsh_language.c 里（唯一那一处）");
        ok(strstr(buf, "dsh_script_of") != NULL,
           "⑦ dsh_script_of 与那份判定在同一个文件里（谁也别想绕开它）");
      }
    }
  }

  /* ══ ⑧ 终态页那排出路按钮：**连按钮上那行字都由内核给** ══
   * 这些文案在参考实现里写在前端（`refreshEntryChips`），搬进内核的是**字面**，所以逐字钉住 ——
   * 一个字改了这条就红。⚠️ 只断言「chips 非空」的话，界面照样可以自己拼一句上去而全绿。 */
  {
    dsh_fallback_facts f;
    dsh_chip chips[DSH_FALLBACK_MAX_CHIPS];
    memset(chips, 0, sizeof(chips));

    /* ① 两条都不给 → 一条都不许有（点了也没用的按钮不许出现） */
    facts_default(&f, DSH_ORIGIN_INPUT, DSH_SCRIPT_LATIN);
    ok(dsh_fallback_chips(&f, 0, 0, "apple", chips, DSH_FALLBACK_MAX_CHIPS) == 0,
       "⑧ 两条出路都不给时，一条按钮都不许有");

    /* ② 都问完了（没有「没问完」的）→ 「再问一遍」+ 那句提示 */
    ok(dsh_fallback_chips(&f, 1, 0, "apple", chips, DSH_FALLBACK_MAX_CHIPS) == 1,
       "⑧ 只给「再问一遍」时正好一条");
    ok_eq_str(chips[0].action, "recheck", "⑧ 动作名是接口定义里的 recheck");
    ok_eq_str(chips[0].label, "再问一遍", "⑧ 都问完了 → 按钮上写「再问一遍」");
    ok_eq_str(chips[0].hint, "再问一次别的词典", "⑧ 提示照参考实现的原话");
    dsh_fallback_chips_free(chips, 1);

    /* ③ 有没问完的 → 数目 + 把那几本点名（不许让步的那一条，入口在这里） */
    facts_default(&f, DSH_ORIGIN_INPUT, DSH_SCRIPT_LATIN);
    f.unconfirmed[0] = "丢了的词典";
    f.unconfirmed[1] = "超时的词典";
    f.unconfirmed_count = 2;
    ok(dsh_fallback_chips(&f, 1, 0, "apple", chips, DSH_FALLBACK_MAX_CHIPS) == 1,
       "⑧ 有没问完的时也是「再问一遍」那一条");
    ok_eq_str(chips[0].label, "还有 2 本没查完", "⑧ ★ 有没问完的就把数目写在按钮上");
    ok_eq_str(chips[0].hint, "点一下把这（几）本也找一遍：丢了的词典、超时的词典",
              "⑧ ★ 提示里把那几本点名（顿号分隔，照参考实现的原话）");
    dsh_fallback_chips_free(chips, 1);

    /* ④ 空名字要被过滤掉（参考实现也是先 filter 再数） */
    facts_default(&f, DSH_ORIGIN_INPUT, DSH_SCRIPT_LATIN);
    f.unconfirmed[0] = "";
    f.unconfirmed[1] = "有名字的";
    f.unconfirmed_count = 2;
    ok(dsh_fallback_chips(&f, 1, 0, "apple", chips, DSH_FALLBACK_MAX_CHIPS) == 1,
       "⑧ 空名字那一条也算进数组里");
    ok_eq_str(chips[0].label, "还有 1 本没查完", "⑧ ★ 数的是**有名字的**那几本（不是数组长度）");
    dsh_fallback_chips_free(chips, 1);

    /* ⑤ 翻译那条：字面里带这次查的词 */
    facts_default(&f, DSH_ORIGIN_INPUT, DSH_SCRIPT_LATIN);
    ok(dsh_fallback_chips(&f, 0, 1, "apple", chips, DSH_FALLBACK_MAX_CHIPS) == 1,
       "⑧ 只给翻译时正好一条");
    ok_eq_str(chips[0].action, "translate", "⑧ 动作名是接口定义里的 translate");
    ok_eq_str(chips[0].label, "翻译「apple」", "⑧ 按钮上写「翻译「<这次查的词>」」");
    ok_eq_str(chips[0].hint, "把这段文字发给火山引擎翻译", "⑧ 提示照参考实现的原话");
    dsh_fallback_chips_free(chips, 1);

    /* ⑥ 两条同时给 → 先「再问一遍」后「翻译」（顺序也照参考实现） */
    ok(dsh_fallback_chips(&f, 1, 1, "apple", chips, DSH_FALLBACK_MAX_CHIPS) == 2,
       "⑧ 两条都给时正好两条");
    ok_eq_str(chips[0].action, "recheck", "⑧ ★ 第一条是「再问一遍」（顺序照参考实现）");
    ok_eq_str(chips[1].action, "translate", "⑧ ★ 第二条才是「翻译」");
    dsh_fallback_chips_free(chips, 2);

    /* ⑦ ★ `borrow` 这一档**永不出现**（借查已经是自动的一步，再给这个按钮就是重复入口） */
    {
      int saw_borrow = 0;
      for (int recheck = 0; recheck <= 1; recheck++) {
        for (int translate = 0; translate <= 1; translate++) {
          const int n = dsh_fallback_chips(&f, recheck, translate, "apple", chips,
                                           DSH_FALLBACK_MAX_CHIPS);
          for (int i = 0; i < n; i++) {
            if (chips[i].action != NULL && strcmp(chips[i].action, "borrow") == 0) saw_borrow = 1;
          }
          dsh_fallback_chips_free(chips, n);
        }
      }
      ok(!saw_borrow, "⑧ ★ 四种组合下都不给「借词典查」那个按钮（借查已经自动了）");
    }

    /* ⑧ 还给内核之后活分配表要回到基线（这一节自己的泄漏检查标准） */
    ok((int64_t)dsh_mem_live_count() == (int64_t)base,
       "⑧ 还给内核按钮那几条字符串之后，活分配表回到基线");
  }

  ok((int64_t)dsh_mem_live_count() == (int64_t)base,
     "全部用例跑完，活分配表必须回到基线");
  printf("fallback：%d 项，失败 %d\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
