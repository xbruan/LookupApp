/* 引擎的**内部**接口：对外的那几条住生成头 `native/include/dsh_lookup.h`，这一层
 * **不许再声明一遍**（重复声明会报「conflicting types」，返回类型不同也算）；
 * 别的内核模块要看引擎状态的走这里 —— 加一条内部接口就加在这儿，别加在生成头里。 */

#ifndef DSH_ENGINE_INTERNAL_H
#define DSH_ENGINE_INTERNAL_H

struct dsh_engine;
struct dsh_settings;
struct dsh_history;

/** 引擎用的配置目录（没配就返回空串）—— 内部用；对外的信息走接口定义 */
const char *dsh_engine_user_data_dir(const struct dsh_engine *e);

/** 底层设置文档（借用，不许释放）—— 给内核其他模块与测试用 */
const struct dsh_settings *dsh_engine_settings(const struct dsh_engine *e);

/* ── 查词历史（实现见 `store/dsh_history.c`，引擎在 `create` 里开它）────────── */

/** 历史库（借用；没打开就回 NULL）。`dsh_lookup_api.c` 记一次查词时用它。 */
struct dsh_history *dsh_engine_history(struct dsh_engine *e);

/**
 * 历史库打不开的原因（借用的人话；打开成功就回 NULL）。
 * ⚠️ 要**原样**说给用户听 —— 不许换成「没有历史」：那是两件不同的事。
 */
const char *dsh_engine_history_why(const struct dsh_engine *e);

/* 「界面现在显示的是哪条词条」—— 引擎记着的那一对（键名 + 哪一本）。
 *
 * 为什么要引擎记：「落点就是当前词条时不跳」这条产品约定要拿「落点」与「现在显示的是谁」比，
 * 而后者只有内核知道自己上一次交出去了什么。界面拿两个字符串去比就是把业务规则放进视图层。
 *
 * ⚠️ 两个都是**借用**（活到下一次 `dsh_engine_remember_shown` 为止），谁都不许释放。
 */
const char *dsh_engine_shown_key(const struct dsh_engine *e);
const char *dsh_engine_shown_dict_id(const struct dsh_engine *e);

/**
 * 记下"这一次交出去的是哪条词条"（键名或哪一本为空 = **什么都没显示**，那就清掉）。
 * 只在答复真的会让界面显示一条词条时调（查不到、停在候选列表那两档都不动它）。
 *
 * @return 0 成功；非零 = 内存不足（那时**保持原样**，不半更新）
 */
int dsh_engine_remember_shown(struct dsh_engine *e, const char *key, const char *dict_id);

/**
 * **这一本现在什么状态**：空串（好好的）/ `removed`（不在词库里）/ `missing`（文件没了），
 * 并把要**原样显示**的人话写进 `*out_note`（可用时 NULL；调用方 `dsh_release`）。
 * 只有内核查得动这个状态，所以这句话也归内核：历史每行与词库每本走同一处实现，界面不许有第二份说法。
 * ⚠️ 两种情形两句话不许合成一句：「不在词库里」只能用调用方那份快照的 `snapshot_title` 说
 *    （那本已经没得查了），「文件不在了」用当前显示名。
 */
const char *dsh_dict_status(const struct dsh_engine *e, const char *dict_id,
                            const char *snapshot_title, char **out_note);


/**
 * 换掉引擎里那份设置（旧的由本函数释放）。
 *
 * ⚠️ **不落盘**：只动内存。要"改了同时还存盘"请走接口定义里的 `dsh_engine_settings_set`
 * / `dsh_engine_dict_*`（它们先落盘、成功了才换内存里那份 —— 见 dsh_engine.c 那条注释）。
 * 本函数给"引擎自己的初始化"与"测试要造一个特定状态"用。
 */
int dsh_engine_replace_settings(struct dsh_engine *e, struct dsh_settings *next);

/**
 * 联想候选**只要词**（按顺序，最多 `limit` 条）。
 *
 * 为什么要有它：入口文档的「未命中」提示页要列**可点的候选**，而候选只有一处实现
 * （`dsh_engine_suggest` 那条路，出参是 JSON）—— 这里从同一份 JSON 上读回来，
 * **不许再写一份「差不多的」候选逻辑**。
 *
 * @param dict_id 给空 = 当前词典
 * @return 0 成功（`*out_count = 0` 时 `*out_words = NULL`）；非零失败（last_error 已写）
 */
int dsh_suggest_words(struct dsh_engine *e, const char *dict_id, const char *query, int limit,
                      char ***out_words, int64_t *out_count);

/** 释放 `dsh_suggest_words` 给出来的数组（每一项与数组本身都 `dsh_release`） */
void dsh_suggest_words_free(char **words, int64_t count);

#endif /* DSH_ENGINE_INTERNAL_H */
