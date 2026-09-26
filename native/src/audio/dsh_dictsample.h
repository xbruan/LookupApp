/* ==========================================================================
 * 词典取样 · **纯策略层**（从一本词典里挑「像词条」的候选名）。
 *
 * 为什么单独一层：`dsh_mdx` 只管格式，**不认识设置、也不认识「词条该长什么样」**；而
 * 「挑哪几条」是产品判断、**最容易悄悄退化**（写错时表现只是量出来的音量偏一点，肉眼和
 * 人耳都看不出来），所以它必须能**不启动界面、拿真词典文件**直接量（测试）。
 * ⚠️ 取样的单位是**词条**、不是**词块**：块只是存储单位，块数少不等于候选少。
 * ========================================================================== */

#ifndef DSH_AUDIO_DSH_DICTSAMPLE_H
#define DSH_AUDIO_DSH_DICTSAMPLE_H

#include <stddef.h>
#include <stdint.h>

/** 一条候选的位置：第几块 + 块内第几条 */
typedef struct {
  int64_t block_index;
  int64_t entry_index;
} dsh_dictsample_slot;

/**
 * 按**整本书的词条序号**均匀撒点：把全书 N 条词条分成 `max_scan` 段，取每段开头那一条。
 *
 * @param block_entry_counts 每块有几条词条
 * @param block_count        `block_entry_counts` 的长度
 * @param max_scan           最多给几个候选
 * @param out                出参缓冲（至少要 `max_scan` 项）
 * @return 实际写出几个（0 = 没有词条，或参数不合法）
 */
int64_t dsh_dictsample_slots(const int64_t *block_entry_counts, int64_t block_count,
                             int64_t max_scan, dsh_dictsample_slot *out);

/**
 * 索引里的键名去掉结尾的 `\0`（个别 v2.0 词典会带）与首尾空白，写进 `out`。
 *
 * @return 0 成功；-1 = 装不下（`out` 原样留下，调用方按「这个名字不能用」处理）
 */
int dsh_dictsample_clean_key(const char *src, char *out, size_t cap);

/**
 * 「这条键值得拿去量录音吗」的粗筛。检查标准刻意**便宜且宽**：以字母（含中日韩、带音标的
 * 拉丁字母）开头、长度 2..24、不含空白、不含 `@ \ / : < >` 与半角引号（`@@@LINK`、
 * 资源路径、命名空间那些不是词条名）。宁可漏掉一些真词条，也不要把噪声当样本 ——
 * 少一条不影响结论，而拿一条根本不是词条的东西去解正文只会白花时间。
 *
 * @param text 键名（可有结尾 `\0` 与首尾空白 —— 本函数自己会跳过它们）
 * @return 1 = 像词条；0 = 不像
 */
int dsh_dictsample_looks_like_headword(const char *text);

#endif /* DSH_AUDIO_DSH_DICTSAMPLE_H */
