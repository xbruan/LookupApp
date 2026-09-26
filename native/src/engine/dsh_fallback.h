/* ══════════════════════════════════════════════════════════════════════════
 * 查词通道的**决策函数** —— 移植参考实现的 `src/Fallback/FallbackPlan.cs`。
 *
 * 它只回答一个问题：**下一步做什么**。不查词典、不联网、不读设置 ——
 * 需要知道的事一律由调用方填进 `dsh_fallback_facts`，结论写进 `dsh_fallback_result`。
 * 单独抽出来的理由：这张表十几行分支、每行都是**产品约定**（谁先谁后、什么情况停下、什么情况必须给退路）；
 * 而且纯函数才有 **单元测试的立足点** —— 表驱动跑一遍几毫秒，不打开词典、不联网。
 *
 * ⚠️ 它是**纯**的：这个文件不许 include 任何「会去读文件/查词典」的头（`dsh_dicts.h` / `dsh_mdx.h` 一个都不许）。
 *    连 `script` 都由调用方填，而调用方**只许**用 `dsh_script_dominant` 去填它（「这段输入是什么书写系统」
 *    只许有一处来源）。
 * ══════════════════════════════════════════════════════════════════════════ */

#ifndef DSH_FALLBACK_H
#define DSH_FALLBACK_H

#include "dsh_lookup.h" /* 接口定义：dsh_origin / dsh_stage / dsh_script / dsh_surface */

/** 一次能报「没问完」的词典本数上限（比同时能加载的本数宽一点，够用） */
#define DSH_FALLBACK_MAX_UNCONFIRMED 32

/** 下一步做什么 */
typedef enum {
  /** 在当前词典里查 / 问落点（查词通道里的「第一步」） */
  DSH_FALLBACK_LOOKUP = 0,
  /** 显示这一份正文（当前词典 / 借查命中的那本 / 回放译文） */
  DSH_FALLBACK_SHOW,
  /** 摆候选列表（输入框）或把候选并进正文框底部提示（选区） */
  DSH_FALLBACK_SUGGEST,
  /** 去问别的词典（问一句就走，不留常驻） */
  DSH_FALLBACK_PROBE,
  /** 自动翻译（或回放译文缓存） */
  DSH_FALLBACK_TRANSLATE,
  /** 终态页：查词通道的三步都试过、翻译用不上 */
  DSH_FALLBACK_TERMINAL,
  /** 只说话、不查词（那本词典被移除 / 文件丢了 / 这条入口不走查词通道） */
  DSH_FALLBACK_EXPLAIN_ERROR,
  /**
   * ★ **去掉音节分隔点、用新词再来一遍**。
   * 只在「原样问过、没命中、而且词里确实带分隔点、而且还没试过」时才发；执行方要把查询词换成
   * 去掉点的那一份（整条链后面都用它），然后**回到「起点」再走一遍**。
   */
  DSH_FALLBACK_RELOOKUP
} dsh_fallback_action;

/**
 * 调用方在每一步填进来的**事实**（字段与参考实现的 `FallbackFacts` 一一对应）。
 * 字符串一律是**借用**（调用方保证在调用期间活着）；NULL 与空串在这里等价。
 */
typedef struct {
  enum dsh_origin origin;
  enum dsh_stage stage;
  enum dsh_script script;
  /** 当前词典查到了吗（`after_lookup` 那一步的事实） */
  int entry_found;
  /** 当前词典给出的联想候选条数 */
  int suggestion_count;
  /**
   * 那批候选里**有没有一条精确命中**（`suggest` 给的第一条，其 `kind` 是 `exact`）。
   *
   * ⚠️ 这一格是「逐字节对齐参考实现行为」这条验收约定本身要求补上的 —— 规格里「有候选 → 摆列表、停」
   *    那句话的完整形状是五步，而参考实现那份表漏了「**先找精确命中，有就直接查它**」这一步。
   *    漏掉的后果不是「少一条候选」，而是**输入框那条路永远出不来词条**：打一个确实存在的词按回车，
   *    屏幕上永远只有一串候选、正文永远是空的。
   */
  int suggestion_has_exact;
  /** 别的词典里命中的那一本（NULL = 都没命中） */
  const char *hit_dict_id;
  /** 命中那一本的书名（写解释行用；NULL 时退回 id） */
  const char *hit_dict_title;
  /**
   * 这次查询**从哪一本开始**的书名。
   * ⚠️ 它**不一定**等于设置里的当前词典：正文里选中文字那条路的起点是**正文显示的那一本**，
   *    而那本可能是借查来的 —— 把它当成「当前词典」就会说出「当前词典没有」这种假话。
   */
  const char *start_dict_title;
  /** 起点那本**就是**设置里的当前词典吗 */
  int started_from_current;
  /**
   * 这次查询的词（借用；可能为空）。
   * ⚠️ 它是给**选区那条路那句提示**用的：那句「≤4 个词就把词写出来、否则只说『所选文本』」的规矩
   *    要拿词本身来数字数。别拿它干别的 —— 这里要的是「用户选中了什么」，不是「落点是谁」。
   */
  const char *query;
  /**
   * `after_probe`：借查命中的那本**就是设置里的当前词典**。
   * 这不是「借查」：用户那一页本来就是他自己那本词典里的词条 ——
   * 此时必须按**当前词典命中**收尾（`via = current`、不带「借查」那行解释）。
   */
  int hit_is_current;
  /**
   * 没问完的词典名（超预算 / 报错）。
   * ⚠️ 与「没有」**分开**：它非空时绝对不许说「别的词典也都没有」。
   */
  const char *unconfirmed[DSH_FALLBACK_MAX_UNCONFIRMED];
  int unconfirmed_count;
  /** 翻译总开关 */
  int translate_enabled;
  /** 「查不到时自动翻译」 */
  int auto_translate;
  /** 填了火山引擎的 Key 吗（与语音共用同一把） */
  int translate_has_key;
  /** 这个语种支持翻译吗 */
  int translate_supported;
  /** 不支持时那句如实的话，照抄，别自己编 */
  const char *translate_unsupported_message;
  /** 当前这一页是不是译文伪词条（历史回放时用） */
  int entry_is_translation;
  /** `link`/`back`/`history` 那条路：记录里那本词典**已不在词库里** */
  int dictionary_missing;
  /** `link`/`back`/`history` 那条路：那本还在词库里，但**文件丢了** */
  int dictionary_file_gone;
  /** 上面两条要指认的那本词典的名字 */
  const char *target_dict_title;
  /**
   * ★ **这次查询的词里有没有音节分隔点**（`·‧・` 软连字符），0 = 没有。
   * 有些词典把词显示成 `pro·gress`，整条选中来查时那串点跟着进了查询而词典里没有这个键 ——
   * 于是白白落到借查 / 机器翻译，明明这本里有 `progress`。
   * ⚠️ 约定是**先原样问、问不到才去掉点问一遍**（顺序不能反：`弗拉基米尔·普京` 这种词条名本来就带中点）。
   */
  int has_separator_dots;
  /** 已经「去掉点重问」过一次了吗（防同一件事问两遍 —— 链上的每一步都要照着这个检查走）*/
  int separator_retried;
} dsh_fallback_facts;

/** 结论。界面上要用的每一样都在这里，调用方不需要再猜一遍。 */
typedef struct {
  dsh_fallback_action action;
  /** 这一步用哪一本（`PROBE` / `SHOW` 的借查那一路）；借用 */
  const char *dict_id;
  /** 摆完 / 说完就停，等用户（0 = 还有下一步） */
  int stop;
  /** 这一页是从哪一步来的，写进 `#reader[data-via]`；借用 */
  const char *via;
  /** `SUGGEST` 落在哪儿 */
  enum dsh_surface surface;
  /** 给界面写的解释行；**本结果所有**，用 `dsh_fallback_result_dispose` 释放 */
  char *reason;
  /** 终态页专用：「翻译为什么没用上」——**四档分开说** */
  char *translate_why;
  /** 终态页专用：「另有 N 本没能确认（…）」；空串 = 没有没问完的 */
  char *unconfirmed_note;
  /** 没能问完的词典**名字**（原样带给界面，不是写给人看的那句话）；借用 */
  const char *unconfirmed_names[DSH_FALLBACK_MAX_UNCONFIRMED];
  int unconfirmed_count;
  /** 终态页要不要给一条**可点的**「翻译这个词」（只在「用户自己关了自动翻译」那一档） */
  int offer_translate;
  /** 要不要给「再问一遍」（有没问完的词典时**必须**给） */
  int offer_recheck;
  /** 拼字符串时内存不足（真出过的话调用方必须**如实报错**，不许当成「没有那句话」） */
  int oom;
} dsh_fallback_result;

/** 这条入口走不走查词通道（链接 / 回退 / 历史一律不跑） */
int dsh_fallback_runs_channel(enum dsh_origin origin);

/** 决策函数本体（纯函数：同样的 facts 一定得到同样的结论） */
void dsh_fallback_decide(const dsh_fallback_facts *facts, dsh_fallback_result *out);

/** 释放结论里那几条本结果自己拼的字符串（结论本身可以留在栈上） */
void dsh_fallback_result_dispose(dsh_fallback_result *r);

/** `TranslateUsable`：四个条件全满足才自动翻译 */
int dsh_fallback_translate_usable(const dsh_fallback_facts *facts);

/** 终态页那句「翻译为什么没用上」（四档分开说，不许合并）—— 新分配的串 */
char *dsh_fallback_translate_why(const dsh_fallback_facts *facts);

/** 「另有 N 本没能确认（《A》《B》）」；全部问完时是空串 —— 新分配的串 */
char *dsh_fallback_unconfirmed_note(const dsh_fallback_facts *facts);

/**
 * 一圈出路按钮最多这么多条（现在实际只有「再问一遍」与「翻译这个词」两种；
 * `borrow` 那一档**永不出现**，见 `dsh_chip_action` 的注释）。
 */
#define DSH_FALLBACK_MAX_CHIPS 2

/**
 * 一条出路按钮。**按钮上那行字也在这里拼** —— 文案是产品约定、不是视图，所以归内核。
 */
typedef struct {
  /** 接口定义里 `dsh_chip_action` 的名字（`recheck` / `translate`）—— **借用**静态串 */
  const char *action;
  /** 按钮上那行字；**本结构所有**，用 `dsh_fallback_chips_free` 释放 */
  char *label;
  /** 悬停提示；**本结构所有**，同上释放（可能是空串，不会是 NULL） */
  char *hint;
} dsh_chip;

/* ══════════════════════════════════════════════════════════════════════════
 * 终态页那排出路按钮（移植参考实现的 `refreshEntryChips`）。约定（一条都别自己发挥）：
 * ⚠️ **只有终态页给**（调用方按 `via` 是 `terminal` 判）；「借词典查」那个按钮已取消，所以 `borrow` **永不出现**。
 * · 「再问一遍」只在 `offer_recheck` 时给 —— 有词典**没问完**时**必须**给：有它才敢说「别的词典里也没有」；
 * · 「翻译这个词」只在 `offer_translate` 时给（= 用户**自己**关掉了自动翻译，不是「他不要」）；其余四档
 *   （总开关关 / 没 Key / 语种不支持 / 翻译失败）**不给按钮** —— 点了没用，理由由 `translate_why` 写清楚。
 *
 * @param word  这次查的词（按钮文字里要用）；NULL 当空串
 * @param out   调用方给的数组（至少 `DSH_FALLBACK_MAX_CHIPS` 个）
 * @param max   `out` 的容量
 * @return 写进 `out` 的条数；内存不足时返回 0 并**置 last_error**（调用方要如实报错）
 */
int dsh_fallback_chips(const dsh_fallback_facts *facts, int offer_recheck, int offer_translate,
                       const char *word, dsh_chip *out, int max);

/** 释放 `dsh_fallback_chips` 拼出来的那几条字符串 */
void dsh_fallback_chips_free(dsh_chip *chips, int count);

/** 动作名 / 阶段名 / via 名 —— 写日志与测试结果用（借用静态串） */
const char *dsh_fallback_action_name(dsh_fallback_action action);
const char *dsh_fallback_stage_name(enum dsh_stage stage);

#endif /* DSH_FALLBACK_H */
