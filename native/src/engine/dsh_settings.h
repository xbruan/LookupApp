/* 设置模型 —— 住内核里；界面**不许**自己拼一份、更不许拿内存里的设置回传给内核。
 * 规范化是业务规则（越界语速夹回、不认识的枚举落回默认、nil 与空串分清），放 UI 层
 * 就等于每个平台壳各实现一遍，而「同一件事有多个来源」是这个仓库最贵的坑。
 *
 * ⚠️ **内核不认识的顶层键必须原样活下来**：解析时把它的原文片段记下来
 * （`dsh_json_node_raw`），序列化时原样搬回去 —— 否则内核按自己的理解重写整份设置，
 * 那些字段会在**第一次存盘时不报错地消失**（用户看到的是「我设的偏好全没了」）。
 *
 * ⚠️ 补丁是**局部**的：「没出现」= 保持原值；出现 `null` = **清空**那个字段
 * （用户「取消设置」的表达）；出现空串 = 字符串字段当清空、数组则清成空数组。
 */

#ifndef DSH_SETTINGS_H
#define DSH_SETTINGS_H

#include <stddef.h>
#include <stdint.h>

/* 两个默认音色 id 住在音频层（`DSH_DOUBAO_DEFAULT_SPEAKER_EN/ZH`）—— 这里只取用，
 * 不再抄一份：音色 id 是那一层的产品数据，而"同一个东西两个来源"是本仓库最贵的坑。 */
#include "audio/dsh_doubao.h"

/** 设置文档（不透明；里面的东西归它所有，`dsh_settings_free` 之后全悬空） */
typedef struct dsh_settings dsh_settings;

/** 一条词库记录（对应参考实现里的词库条目模型） */
typedef struct dsh_stored_dict {
  const char *id;          /* 内容哈希（见 dict/dsh_dict_id.h） */
  const char *title;       /* 导入时的默认显示名（文件名） */
  const char *custom_title;/* 用户改过的名字；NULL = 没改过 */
  const char *mdx_path;
  const char **mdd_paths;  /* 配套资源库（.mdd / .1.mdd / .2.mdd…） */
  int64_t mdd_count;
  int64_t added_at;        /* Unix 毫秒 */
} dsh_stored_dict;

/** 关窗行为（只认这三个值） */
typedef enum {
  DSH_CLOSE_ASK = 0,
  DSH_CLOSE_QUIT = 1,
  DSH_CLOSE_TRAY = 2
} dsh_close_behavior;

/*
 * 窗口级的另一条设置（与 `closeBehavior` 同层：都住在设置文件的**顶层**）：
 * `showFloatingOnStartup` —— **启动时显不显示悬浮窗**（默认 1）。关掉后程序照常启动、
 * 托盘图标也在，只是胶囊不摆出来（要它就从托盘菜单点「显示悬浮窗」）。
 * ⚠️ 它**只管启动那一瞬间**：从托盘叫出来的窗口不受它影响，下次启动照旧按它办 ——
 * 不做「记住上次是不是藏着」那种记忆，那会让「今天为什么没出来」变成要看历史才知道的事。
 * 由内核建模而不是壳自己存：它决定「启动后屏幕上有没有东西」，两个壳各写一份就会分叉。
 */

/** 启动时是否显示悬浮窗（借用设置里的值；不在这个结构里，见 `dsh_settings_close_behavior` 那种取法） */
int dsh_settings_show_floating_on_startup(const dsh_settings *s);

/* ── 发音设置 ─────────────────────────────────────────────────────────────
 * 为什么住内核：三层音源的排序与「这次到底怎么念」是产品约定，而那个判断要读的
 * 正是这几个值。界面只许**照抄**内核算出来的结论，不许自己读设置再判一遍。
 */

/** 豆包（在线语音）的默认值 —— 与参考实现同一套常量 */
#define DSH_DOUBAO_DEFAULT_RESOURCE_ID "seed-tts-2.0"
#define DSH_DOUBAO_DEFAULT_FORMAT "mp3"
/* 两个默认音色 id 在 `audio/dsh_doubao.h` 里（本文件顶上 include 了它）—— 不再抄一份 */

/** 发音设置（全部字段都是**归一化之后**的值） */
typedef struct {
  int rate;                    /* 语速，-10..10（越界夹回），0 = 引擎默认 */
  const char *voice_id;        /* 指定的离线音色；NULL = 按语种自动挑 */
  const char *accent;          /* "auto" / "uk" / "us"（非法值一律退回 auto）*/
  /**
   * 豆包 API Key；NULL = 没填。**明文存在设置文件里**（界面上要写明这一点）。
   * 它与 `volcengine.apiKey` 是**同一个值**（见 `dsh_settings_volcengine_api_key`）。
   */
  const char *doubao_api_key;
  const char *doubao_resource_id; /* 空 → 默认 seed-tts-2.0 */
  const char *doubao_format;      /* 空 → 默认 mp3 */
  /**
   * 两个音色。⚠️ **NULL 与空串不是一回事**：
   *   · NULL = 字段**从来没设过** → 用内置默认音色；
   *   · 空串 = 用户的明确选择，**留着**（清空输入框保存后再打开，不该被默认值顶回来）。
   */
  const char *doubao_speaker_en;
  const char *doubao_speaker_zh;
  /** 两个音色的响度补偿（官方 loudness_rate，-50..100）；has_* = 有没有设过 */
  int has_loudness_en;
  int loudness_en;
  int has_loudness_zh;
  int loudness_zh;
} dsh_speech_settings;

/** 发音设置（借用，不许释放） */
const dsh_speech_settings *dsh_settings_speech(const dsh_settings *s);

/**
 * 火山引擎的 API Key —— **与 `speech.doubaoApiKey` 是同一个值的两个出口**。
 *
 * 为什么必须镜像、不能各存各的：语音那条路读一个、翻译那条路读另一个，只写一边就会出现
 * 「语音能响、翻译说没填 Key」；而只读旧字段更危险 —— 磁盘上留着一份过期旧 Key，
 * 用户清空后重启又被搬回来，表现为「清不掉」。
 */
const char *dsh_settings_volcengine_api_key(const dsh_settings *s);

/**
 * 在线语音这一条**能不能用**：检查标准只有一条 —— **填没填 Key**。
 * ⚠️ 不许再引入「在线发音总开关」：多一个开关只会让用户以为「我开了在线却没走在线」。
 */
int dsh_settings_speech_online_ready(const dsh_settings *s);

/* ── 划词查词与全局快捷键：**已整条删掉** ──────────────────────────────────
 * 这一节（`hotkey.enabled` / `hotkey.chord`）连同它的规范化器一起从内核删掉了 ——
 * 不是「界面不显示」，是**真的没有**：内核不再认识 `hotkey` 键，老配置里残留的整节会走
 * **原样搬运**、下次写盘自然消失；平台层的 `RegisterHotKey` 与「抓前台选区」也一并删掉。
 * ⚠️ **别再顺手加回来**：这是用户对产品的判断（不需要全局快捷键），不是实现上的取舍。
 */

/* ── 机器翻译（「翻译」那一页 + 查词通道的第四条出路）────────────────────────
 *   · `enabled` —— 总开关，**默认关**：要联网、要把文本发给第三方，得用户主动打开
 *     （这是隐私约定，不是「保守默认」）；
 *   · `autoTranslate` —— 「查不到时自动翻译」，**默认开**。关掉**不等于**不要翻译 ——
 *     终态页会给一条**可点的**「翻译这个词」；
 *   · `targetMode` —— `auto`（中文译英、其余译中）/ 固定 `zh` / 固定 `en`。
 *     ⚠️ **只认这三个值**：脏数据不许变成「发一个乱码目标语种给服务端」，
 *     认不出的一律落回 `auto`，原因写进 last_error，但**不报错**（其余设置仍然可用）。
 *
 * ⚠️ 凭据不在这一节：它与语音共用同一把 `volcengine.apiKey`。「词典优先」那个开关已删掉，
 * 老配置里残留的键走**原样搬运**、下次写盘自然消失。
 */
typedef struct {
  int enabled;             /* 总开关；默认 0（关）*/
  int auto_translate;      /* 查不到时自动翻译；默认 1（开）*/
  const char *target_mode; /* `auto` / `zh` / `en` —— **永远是这三个之一** */
} dsh_translate_settings;

/** 机器翻译的设置（借用，不许释放）*/
const dsh_translate_settings *dsh_settings_translate(const dsh_settings *s);

/**
 * 从设置 JSON 建一份设置。
 *
 * @param json  设置文本；**NULL 或空**表示「没有设置文件」，按全默认建一份
 * @param len   json 的字节数
 * @param out   出参
 * @return 0 成功；非零失败（JSON 不合法等），原因写进 last_error。
 *         ⚠️ 失败时**不返回半份设置**：宁可让调用方看到「配置读不动」，
 *         也不要拿一份残缺的设置去覆盖用户原来的文件。
 */
int dsh_settings_parse(const char *json, size_t len, dsh_settings **out);

/** 建一份全默认设置（等价于 `dsh_settings_parse(NULL, 0, …)`） */
int dsh_settings_default(dsh_settings **out);

/** 释放。传 NULL 是合法的空操作。 */
void dsh_settings_free(dsh_settings *s);

/* ── 读（都给"借用"的指针；不许释放）────────────────────────────────────── */

int64_t dsh_settings_dict_count(const dsh_settings *s);
const dsh_stored_dict *dsh_settings_dict_at(const dsh_settings *s, int64_t index);

/** 按 id 找一本（找不到 → NULL） */
const dsh_stored_dict *dsh_settings_dict_by_id(const dsh_settings *s, const char *id);

/** 当前词典 id（可能是空串 = 没指定） */
const char *dsh_settings_current_dict_id(const dsh_settings *s);

/** 关窗行为 */
dsh_close_behavior dsh_settings_close_behavior(const dsh_settings *s);

/**
 * 词典的**显示名**（约定只在内核一处）：改过的名 → .mdx 头里的书名 → 文件名。
 *
 * @param header_title .mdx 头里的书名（可以是 NULL 或空串 = 没有）
 * @return 借用指针（不是新分配）
 */
const char *dsh_settings_dict_display_name(const dsh_stored_dict *d, const char *header_title);

/* ── 写 ─────────────────────────────────────────────────────────────────── */

/**
 * 应用一个**局部补丁**（JSON 对象）。只改补丁里出现的键，其余保持原值。
 *
 * 返回一份**新的**设置（调用方 `dsh_settings_free`），原来那份不动 ——
 * 这样"打补丁"是原子的：补丁中途发现不合法时可以整份丢掉，不会留下改了一半的设置。
 *
 * @return 0 成功；非零失败（补丁不是对象、JSON 不合法等）
 */
int dsh_settings_apply_patch(const dsh_settings *base, const char *patch_json, size_t len,
                             dsh_settings **out);

/**
 * 往词库清单里加一本（返回新的一份设置）。
 *
 * @param id         内容哈希 id（调用方负责算，见 dsh_dict_id_of_file）
 * @param mdx_path   .mdx 路径
 * @param title      默认显示名（一般是文件名；NULL 当空串）
 * @param mdd_paths  配套资源路径数组（可以为 NULL / count 0）
 * @return 0 成功；`added_at` 为 0 时自动填当前时间。
 *         ⚠️ **id 或路径为空 → 失败**：绝不许把「没有路径的东西」记成一本书。
 */
int dsh_settings_dict_add(const dsh_settings *base, const char *id, const char *mdx_path,
                          const char *title, const char *const *mdd_paths, int64_t mdd_count,
                          int64_t added_at, dsh_settings **out);

/** 移除一本（返回新的一份设置）。找不到那个 id 时**不改动**，并返回 0。 */
int dsh_settings_dict_remove(const dsh_settings *base, const char *id, dsh_settings **out);

/** 给一本改名（返回新的一份设置）。`name` 为 NULL/空串 = 恢复默认名。 */
int dsh_settings_dict_rename(const dsh_settings *base, const char *id, const char *name,
                             dsh_settings **out);

/**
 * 把一本在清单里挪 `delta` 位（返回新的一份设置）。
 *
 *   · `delta < 0` 往前挪、`delta > 0` 往后挪、`0` = 原样返回；
 *   · **到边界就夹住**（第一本再往前、最后一本再往后 = 清单原样返回，仍然算成功）——
 *     界面上那两颗按钮在头尾本来就是置灰的，夹住只是「别让脏输入把顺序搞乱」的兜底；
 *   · 找不到那个 id → 报错（与 `dsh_settings_dict_rename` 同一条）；
 *   · **当前词典不受影响**（它记的是 id 不是下标）。
 *
 * ⚠️ 顺序**不只是显示顺序**：「当前词典」那个字段为空时，查词 / 发音样本 / 增益等若干
 * 接口会**兜底取第一本**（`dsh_settings_dict_at(s, 0)` 那几处）—— 所以「把某一本挪到
 * 第一位」会真的改变兜底行为。这条也写进了接口定义。
 */
int dsh_settings_dict_move(const dsh_settings *base, const char *id, int64_t delta,
                           dsh_settings **out);

/**
 * 指定当前词典（返回新的一份设置）。
 * `id` 为 NULL/空串 = 取消指定；id 不在清单里 → 报错（**不许悄悄设成不存在的词典**）。
 */
int dsh_settings_dict_set_current(const dsh_settings *base, const char *id, dsh_settings **out);

/* ── 序列化 ─────────────────────────────────────────────────────────────── */

/**
 * 序列化成 JSON（UTF-8，以 \0 结尾）。
 *
 * 结构：内核认识的字段按**固定顺序**写；内核不认识的顶层键**原样附在后面**。
 * 顺序固定是刻意的：同一份设置两次存盘**逐字节相同**（可 diff、可逐字节对照）。
 *
 * @return 新分配的字符串（调用方 `dsh_release`）；失败返回 NULL。
 */
char *dsh_settings_to_json(const dsh_settings *s);

/* ── 对「内核不认识的顶层键」的两件操作 ───────────────────────────────────
 *
 * 存在的理由只有一个：老版本的用户历史就躺在 `settings.json` 的顶层键 `history` 里，
 * 升级时必须**搬出去一次、然后把那个键删掉** —— 两份并存必然分叉，而「不认识的键
 * 原样搬运」这条机制会让那份历史**一直**跟着设置走（每次存设置都要整份重写一遍）。
 *
 * ⚠️ 这两件**不改动**传进来的那份设置：`drop` 返回**新的一份**。
 */

/**
 * 取一个"原样搬运"的顶层键的**原文片段**（不认识的键）。
 *
 * @param out_len 出参，可为 NULL
 * @return 片段（归 `s` 所有，`dsh_settings_free(s)` 之后悬空）；没有这个键 → NULL
 */
const char *dsh_settings_foreign_raw(const dsh_settings *s, const char *key, size_t *out_len);

/**
 * 去掉一个"原样搬运"的顶层键（返回新的一份设置）。
 *
 * 其余字段、其余未识别键**照旧**（顺序也保持）。键本来就不在 → 返回一份等价的副本并返回 0
 * （"它不在了"正是调用方要的结果）。
 */
int dsh_settings_drop_foreign(const dsh_settings *s, const char *key, dsh_settings **out);

#endif /* DSH_SETTINGS_H */
