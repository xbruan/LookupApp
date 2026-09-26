/* ==========================================================================
 * 发音的**键名层**与**词条音频引用层**：词条里写了哪些音频（`sound://` / `snd://` /
 * `<audio name=…>`）是词典内容的解析结果，跟着 `.mdx` 走；一个键该换成哪些扩展名再找一遍
 * 则是**我们自己的**产品约定（链接写 `.spx`、音频卷里其实是 `.mp3`），漏掉一种能播的扩展名，
 * 症状只是「某本词典的录音找不到」，既不好复现也不好看出来。
 * ⚠️ **一份实现**：`dsh_entry_doc_audio_keys` 也走这里的 `dsh_audio_extract`，不许另写扫描器
 * —— 同一个判断两个来源迟早分叉。
 * ========================================================================== */

#ifndef DSH_AUDIO_H
#define DSH_AUDIO_H

#include <stddef.h>
#include <stdint.h>

/** 词条正文里的一条音频引用 */
typedef struct {
  char *key;      /* 归一化之后的键名（URL 解码 + 去前导斜杠 + trim） */
  char *accent;   /* 「uk」/「us」/ NULL（认不出） */
  int example;    /* 例句音频（只响应点击，**不参与**统一发音按钮） */
} dsh_audio_ref;

/**
 * 抠出词条正文里的全部音频引用：`sound://` / `snd://` 与 `<audio name=…>`，
 * **按出现顺序**、**去重（大小写不敏感）**，每条都过一遍归一化（先两端 trim → URL 解码 →
 * 去前导斜杠 → 再 trim）。顺序有意义：认不出英/美时按「先出现的那个」挑。
 *
 * @return 0 成功（一条都没有也算成功：`*out_count = 0`、`*out = NULL`）；非零失败
 */
int dsh_audio_extract(const char *html, size_t len, dsh_audio_ref **out, int64_t *out_count);

/** 释放 `dsh_audio_extract` 给出来的清单 */
void dsh_audio_refs_free(dsh_audio_ref *refs, int64_t count);

/** 词条里写的键名 → 归一化之后的键名（新分配，调用方 `dsh_release`） */
char *dsh_audio_normalize(const char *raw, size_t len);

/** 猜口音：按分隔符切 token 再比对（`apple__gb_1` → uk；`apple__us_1` → us）*/
const char *dsh_audio_classify_accent(const char *key);

/** 是不是例句音频的键名（LDOCE5 的 `p123__` 与 gbs/uss/brs/ams/eps/exa 那几种）*/
int dsh_audio_is_example_key(const char *key);

/** 口音的中文说法（「英音」/「美音」/「发音」）—— 界面提示直接用这一句 */
const char *dsh_audio_accent_label(const char *accent);

/** 播放器（Chromium 系）自己就能解码的扩展名，**越常见越靠前** */
int dsh_audio_playable_extension_count(void);

/** 第 i 个能播的扩展名（越界返回 NULL）*/
const char *dsh_audio_playable_extension_at(int index);

/** 键名的扩展名（含那个点；没有就回空串）—— `dsh_audio_extension_of` */
const char *dsh_audio_extension_of(const char *key);

/**
 * 这个键**看起来是不是音频**（能播的那几种扩展名 ∪ `.spx`）。
 *
 * 用处只有一个：把「音频键的扩展名变体」那一轮限定在音频上 —— 否则请求 `theme.css`
 * 而卷里只有 `theme.wav` 时会取到错的东西（取到**错的**比取不到更坏）。
 */
int dsh_audio_is_audio_name(const char *key);

/**
 * 一个键名的全部候选写法：**能播的扩展名优先（按上表顺序），原始键垫底**。
 *
 * 垫底那一条不是多余的：词典里只有 `.spx` 时，要能如实告诉用户「这段录音是 Speex」，
 * 而不是含糊地说「找不到音频」。
 *
 * @return 0 成功；非零失败（last_error 已写）
 */
int dsh_audio_candidate_keys(const char *key, char ***out, int64_t *out_count);

/** 释放 `dsh_audio_candidate_keys` 给出来的清单 */
void dsh_audio_keys_free(char **keys, int64_t count);

/** 扩展名看上去就是 Speex（`.spx`）*/
int dsh_audio_is_speex_name(const char *key);

/**
 * 这个扩展名在**规划阶段**能不能算「这条路可用」。
 *
 * Speex 也算可用（字节真到手时会解成 WAV 再发，见 `dsh_audio_prepare`）；认不出的扩展名
 * 也**不急着否定** —— mdd 里什么怪名字都有，等真拿到字节再判。
 */
int dsh_audio_looks_playable(const char *key);

/** 键名的扩展名（含点，小写；没有则空串）。返回静态串（借用） */
const char *dsh_audio_extension_of(const char *key);

/** 一条"已经对上资源卷"的候选（给统一发音按钮挑用） */
typedef struct {
  const char *key;          /* 词条里写的键名（借用）*/
  const char *accent;       /* 「uk」/「us」/ NULL（借用）*/
  int example;
  const char *matched_key;  /* 实际命中的 mdd 键名（借用）*/
} dsh_audio_candidate;

/**
 * 统一发音按钮该念哪一条：**只认词目发音，例句不算**。
 *
 * 口音偏好说了算；偏好是 auto（NULL / 认不出）或这个口音没有时，用**先出现的那条**
 * （英音在前的词典里就是英音）。
 *
 * @return 选中那条在数组里的下标；一条都没有 → -1
 */
int64_t dsh_audio_pick(const dsh_audio_candidate *candidates, int64_t count,
                       const char *accent_preference);

#endif /* DSH_AUDIO_H */
