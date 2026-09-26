/* ──── 查词历史的落盘层：一行一条 JSON 的追加文件（`history.jsonl`）────
 * 业务约定：去重键 **(word, dictId)**（同一个词在两本词典里本来就并存）；队头那条同键、且不到
 * `DSH_HISTORY_DEDUPE_MS`（5 分钟）→ **完全不记**；上限 `DSH_HISTORY_MAX`（5000）超了从**最旧**
 * 那头砍；顺序 = 记录顺序（最新在前），**不按 `at` 重排**（时钟回拨不该让历史跳序）；**只有查到了才记**。
 * 落盘只在尾巴追加一行，查询 / 去重 / 裁剪全在内存；行数涨到上限 2 倍时整份重写一次。
 * 红线：文件读不动**不许悄悄重建**（等于悄悄删历史）、**一行坏行不许毒死整份历史**（跳过的条数由 `dsh_hist_skipped` 报出）。 */

#ifndef DSH_HISTORY_H
#define DSH_HISTORY_H

#include <stddef.h>
#include <stdint.h>

/** 历史（不透明；打不开时不会产生对象） */
typedef struct dsh_history dsh_history;

/* ── 两条产品约定常量（改之前先看本文件顶上那段）──────────────── */
/** 上限：超过就从最旧那头砍（= 接口定义常量 `DSH_HISTORY_LIMIT`） */
#ifndef DSH_HISTORY_MAX
#define DSH_HISTORY_MAX 5000
#endif
/** 队头去重窗口：同一 (word, dictId) 在这个毫秒数之内重复落到队头**不记** */
#ifndef DSH_HISTORY_DEDUPE_MS
#define DSH_HISTORY_DEDUPE_MS (5 * 60 * 1000)
#endif

/** 打开（必要时创建）历史文件；`path` 是**文件**路径（调用方拼好，目录要已存在），`out` 是出参。
 * @return 0 成功；非零失败（原因写进 `dsh_last_error_message()`）。
 * ⚠️ **失败了就是失败了**：不许不报错地退回「就当没有历史」，得让调用方把原因说给用户听。 */
int dsh_hist_open(const char *path, dsh_history **out);

/** 关掉（内存里那份跟着消失，**已经写出去的行不受影响**）。传 NULL 是合法空操作。 */
void dsh_hist_close(dsh_history *h);

/** 历史文件路径（**借用指针**，调用方不许释放）。 */
const char *dsh_hist_path(const dsh_history *h);

/** 记一次查词（去重窗口与落盘约定见文件顶上那段）。
 * @param word 落点的规范词（调用方给的 `keyText`，不是用户打进去的写法）
 * @param dict_id 词典 id（内容哈希；译文伪词典也走这里）
 * @param dict_title 入库当时的显示名**快照**（词典改名后旧记录仍显示旧名）
 * @param now_ms 当前时间（Unix 毫秒，由调用方给；测试才好造去重窗口那种现场）
 * @return 0 成功（**跳过去重窗口那一次也算成功**）；非零失败（写不出去，原因写进 last_error） */
int dsh_hist_push(dsh_history *h, const char *word, const char *dict_id, const char *dict_title,
                  int64_t now_ms);

/** 按页取（内存里那份已经是最新在前，offset 直接按它数）。
 * @param offset 从 0 开始（负数按 0 算）；limit = 要几条，0 = 用接口定义常量 `DSH_HISTORY_PAGE`。
 * @param out_json 出参（**内核分配，调用方 `dsh_release`**）：本层的内层形状
 *   {「total」:N,「items」:[{「word」,「dictId」,「dictTitle」,「at」}…],「hasMore」:bool} —— 对外那一版
 *   （接口定义 `dsh_history_query`）由 `engine/dsh_history_api.c` 再补 「unavailable」/「note」两列。 */
int dsh_hist_query(dsh_history *h, int32_t offset, int32_t limit, char **out_json);

/** 清空（回总条数 0；文件被截成空文件，不是删掉）。 */
int dsh_hist_clear(dsh_history *h, int64_t *out_total);

/** 把上限**压小**（`limit <= 0` = 恢复 `DSH_HISTORY_MAX`）。
 * ⚠️ **只给测试用**：默认上限 5000，单测为了验「超了从最旧那头砍」塞 5001 条既慢又没必要。
 * **产品路径一次都不许调它** —— 谁调谁就是在改一条产品约定。 */
void dsh_hist_set_limit(dsh_history *h, int64_t limit);

/** 现在有多少条（诊断与测试用；出参不许为空） */
int dsh_hist_count(dsh_history *h, int64_t *out_total);

/** 读回时**跳过了多少行**（解析不出来的那些）。出参不许为空，没有跳过就是 0。
 * 跳过本身是对的（一行坏行不该毒死整份历史），但**跳过了却不说**就是不报错地丢数据 ——
 * 用户看到的会是「我的历史怎么少了几条」。这个数在清空时归零。 */
int dsh_hist_skipped(dsh_history *h, int64_t *out_skipped);

/** 一次性**导入**一份旧格式的历史：`settings.json` 里那个 `history` 数组（字段名与本层一行
 * JSON 逐字相同：word / dictId / dictTitle / at；空词 = 跳过），**幂等**，可以安全重跑。
 * 顺序：数组原序（最新在前）整体**接在已有记录之后**、**不重排**；去重按 **(word, dictId)**。
 * @param json / len 旧数组原文与字节数；out_imported 出参（可为 NULL）= 真导入了几条。
 * @return 0 成功（**一条都没导入也算**）；非零失败（不是数组 / 内存不足 / 写不出去）。
 * ⚠️ 导入是**一次落盘**（整份重写）：要么整份成功、要么原地不动。 */
int dsh_hist_import_json(dsh_history *h, const char *json, size_t len, int64_t *out_imported);

#endif /* DSH_HISTORY_H */
