/* ==========================================================================
 * 词库的**加载与缓存** —— `lookup` 组六条的共同地基。
 * ⚠️ 核心纪律（接口定义里 `dsh_engine_probe` 那条）：在**别的**词典里查一次、答案拿回来，
 *    那本词典**不许**留在内存里 —— 所以两种访问分开命名，而不是靠一个 `load` 开关：
 *    `dsh_dicts_ensure_loaded` 加载并**留在**缓存里（要长期用）；
 *    `dsh_dicts_borrow_once` 借一次、用完就关（探路用，**绝不入库**）。
 * 两者解析路径是**同一条**，所以「一次性查询说会落到 A」与「真跳过去显示 B」不可能不一致。
 * ========================================================================== */

#ifndef DSH_ENGINE_DICTS_H
#define DSH_ENGINE_DICTS_H

#include "dict/dsh_mdx.h"

#include <stddef.h>
#include <stdint.h>

struct dsh_engine;
struct dsh_settings;
typedef struct dsh_stored_dict dsh_stored_dict;

/** 一本词典最多认这么多卷 `.mdd`（`base.mdd` + `base.1.mdd` …） */
#define DSH_MDD_MAX_VOLUMES 8

/** 缓存里的一本 */
typedef struct {
  char *id;
  dsh_mdx *mdx;
  /* `.mdd` 资源卷：**懒打开**（第一次真要用资源时才开，见 dsh_dicts_mdd_volumes）*/
  dsh_mdx *mdds[DSH_MDD_MAX_VOLUMES];
  int mdd_count;
  int mdd_tried; /* 试过了（成功或失败都算），别再试第二遍 */
} dsh_loaded_dict;

/** 最多同时加载这么多本（超了如实报错，别悄悄挤掉别人） */
#define DSH_LOADED_MAX 32

/** 引擎里与词库加载有关的那点状态（**布局在这里定义**，dsh_engine.c 只留一块存储）。
 *
 *  ⚠️ 这样放是为了两边谁都不必知道对方的字段，全靠 `dsh_engine_dicts_state()` 对接。 */
typedef struct {
  dsh_loaded_dict items[DSH_LOADED_MAX];
  int64_t count;
} dicts_state;

/** 取引擎里那块加载状态（由 dsh_engine.c 提供） */
dicts_state *dsh_engine_dicts_state(struct dsh_engine *e);

/* ── 加载与缓存 ─────────────────────────────────────────────────────────── */

/**
 * 确保这一本已加载并留在缓存里。
 * @return 0 成功；非零失败（词库里没有这个 id / 文件打不开 / 不是合法 MDict）
 */
int dsh_dicts_ensure_loaded(struct dsh_engine *e, const char *dict_id);

/** 这一本现在在内存里吗（给 `dict_list` 的 `loaded` 字段用） */
int dsh_dicts_is_loaded(struct dsh_engine *e, const char *dict_id);

/** 关掉所有已加载的词典（引擎销毁时自动调；测试也用它核对「不许常驻」） */
void dsh_dicts_unload_all(struct dsh_engine *e);
/** 取已加载的句柄（没加载 → NULL）。**借用**，不许关。 */
dsh_mdx *dsh_dicts_peek(struct dsh_engine *e, const char *dict_id);

/**
 * 借一次：打开 → 交给 `fn` → **无论成败都关掉**（绝不入库）—— 「问一句就走」那条路的落点，
 * 所以 `dsh_engine_probe` 探路不会把一本词典留在内存里。
 * @param fn     回调；返回值透传
 * @param ctx    传给回调的上下文
 * @param out_loaded 出参：这次是不是**真的**打开了一本（已经在缓存里的不算）
 * @return 0 成功；非零失败（词库里没有 / 打不开）。回调自己的失败由回调表达。
 */
int dsh_dicts_borrow_once(struct dsh_engine *e, const char *dict_id,
                          int (*fn)(dsh_mdx *mdx, void *ctx), void *ctx, int *out_loaded);

/* ── 落点解析（`resolve` / `lookup` / `probe` / `entry_document` 共用）───── */

/** 解析结果：落在哪条词条、正文是什么、跟过哪些重定向 */
typedef struct {
  int found;
  char *key_text;    /* 词典的**规范键名**（`@@@LINK` 跟到底之后的那个）；失败时为 NULL */
  char *definition;  /* 正文原文（可能含内嵌 U+0000，长度见 definition_len） */
  int64_t definition_len;
  char *linked_to;   /* 跟过的最后一个重定向目标；没跟过为 NULL */
} dsh_resolved;

/**
 * 跟随 `@@@LINK=` 解析出最终词条：每个写法都试一遍**大小写变体**，命中之后正文是
 * `@@@LINK=<键>` 就把键当新查询**继续跟**（最多 16 层）。
 * ⚠️ **不做**「没命中就跳到最接近的词」—— 那会把拼错的查询悄悄换成另一个词条，让人以为查到了；
 *    全程**只做字典查表**：不碰历史、不动缓存、不产 HTML。
 * @param mdx   已经打开的词典
 * @param query 查询文字（调用方负责 trim；这里不再动它）
 */
void dsh_resolve_entry(dsh_mdx *mdx, const char *query, dsh_resolved *out);

/** 释放 `dsh_resolved` 里的东西（幂等；传 NULL 安全） */
void dsh_resolved_free(dsh_resolved *r);

/** 词条正文的原始字节里，`@@@LINK=` 指向的那个键（不是重定向则返回 NULL） */
char *dsh_parse_link_redirect(const char *text, int64_t len);

/* ── `.mdd` 资源卷（`entry_document` / `resource` / 发音共用）────────────── */

/** 一本词典最多认这么多卷 `.mdd`（`base.mdd` + `base.1.mdd` …） */
#define DSH_MDD_MAX_VOLUMES 8

/**
 * 在那本词典的 `.mdd` 卷里找一个键，命中就把**原始字节**交出来（内核分配）。
 * ⚠️ 找法是**候选键名在外、卷在内** —— 同一个键名先在所有卷里找，找不到才试下一个候选键名。
 * @param candidate 已经算好的**一个**候选键名（候选由调用方生成）
 * @return 1 命中（`*out_bytes` 归调用方 `dsh_release`）；0 没有；-1 失败
 */
int dsh_dicts_mdd_fetch(struct dsh_engine *e, const char *dict_id, const char *candidate,
                        char **out_bytes, size_t *out_len);

/** 只要"有没有"（**不读记录字节**）—— 协商缓存的身份证要用它 */
int dsh_dicts_mdd_contains(struct dsh_engine *e, const char *dict_id, const char *candidate);

/**
 * 那本词典的 `.mdd` 卷（**借用**，不许关；`*out_count = 0` 表示这一本没有资源卷）。
 * ⚠️ 卷是**懒打开**的：第一次真的要用资源时才开，所以「导入一本词典」不会顺手把它
 *    几百 MB 的资源卷也映射进来。打开失败**不算错误**（当没有卷），但会在 last_error
 *    里留下原因 —— 发音与资源那条路会把它显示出来。
 */
int dsh_dicts_mdd_volumes(struct dsh_engine *e, const char *dict_id, dsh_mdx *const **out,
                          int *out_count);

/**
 * 一个资源键名的**六种候选写法**：`\反斜杠` → `\正斜杠` → `反斜杠` → `正斜杠`
 * → `/\反斜杠` → 原样（去重后按序）；顺序与参考实现同一条约定。
 * ⚠️ 为什么要六种：MDict 资源键名在**不同词典里写法不一样**（有无前导反斜杠、斜杠方向），
 *    是「同一个资源在四本词典里四种写法」的真实情况，不是过度设计。
 * ⚠️ **只有这一份实现**：`dsh_engine_resource` 与 `dsh_engine_speech_dict_audio` 都要它。
 * @param out  出参：新分配的指针数组（长度最多 max；调用方 `dsh_audio_keys_free` 那种释放法）
 * @return 候选个数（≥ 0）；-1 失败
 */
int dsh_dicts_resource_key_forms(const char *key, char **out, int max);

/** 释放 `dsh_dicts_resource_key_forms` 给出来的清单 */
void dsh_dicts_key_forms_free(char **keys, int count);

#endif /* DSH_ENGINE_DICTS_H */