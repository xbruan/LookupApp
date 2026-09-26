/* ==========================================================================
 * GENERATED — DO NOT EDIT.
 * 由 tools/gen-bindings.mjs 从 abi/lookup.abi.json 生成（C 头文件）。
 * 改接口定义请改 abi/lookup.abi.json，然后跑：node tools/gen-bindings.mjs
 * ========================================================================== */

#ifndef DSH_LOOKUP_H
#define DSH_LOOKUP_H

/* dsh_lookup ABI v2 · 内核版本 0.2.0 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DSH_ABI_VERSION 2
#define DSH_VERSION_STRING "0.2.0"

/* ── 内存与字符串约定 ─────────────────────────────────────────────────────
 * 内核返回的每一块内存都由内核分配；宿主一律用 dsh_release()
 * 还给内核，禁止 free()/FreeHGlobal。
 * 所有可能失败的接口返回
 * dsh_error；失败时输出参数保持不变（宿主初始化过的初值原样留着）。
 * 字符串一律 utf-8 且以 \0 结尾；调用约定 cdecl。
 * ---------------------------------------------------------------------- */

/* ── 不透明句柄（宿主不许解释它的位）────────────────────────────────────── */
typedef struct dsh_engine dsh_engine;
typedef struct dsh_dict dsh_dict;

/* ── 枚举 ──────────────────────────────────────────────────────────────── */
/*
 * 错误码。注意 DSH_E_NOT_FOUND 是『文件不存在 / 不是
 * MDict』，与『词典里没有这个词条』是两件事 —— 后者是成功返回、结果里
 * found=false。
 */
typedef enum dsh_error {
  DSH_OK = 0,                         /* 成功 */
  DSH_E_INVALID_ARG = -1,             /* 参数不合法（空指针 / 空词 / 越界） */
  DSH_E_NOT_FOUND = -2,               /* 文件不存在或不是 .mdx */
  DSH_E_FORMAT = -3,                  /* 能打开但不是合法 MDict */
  DSH_E_IO = -4,                      /* 读写失败 */
  DSH_E_OOM = -5,                     /* 内存不足 */
  DSH_E_STATE = -6,                   /* 句柄状态不对（未加载就查、已释放还再用） */
  DSH_E_BUSY = -7,                    /* 同一句柄上有并发调用（内核多数对象不是线程安全的） */
  DSH_E_NOT_IMPLEMENTED = -8          /* 这一版还没接（如实报，不许装作成功） */
} dsh_error;

/*
 * 一次查询从哪条入口来。只有 input / selection 允许跑兜底通道 —— link / back
 * / history
 * 一律不跑，否则『退回一个查不到的词』会当场跳走（参考实现那个坑D11）。
 */
typedef enum dsh_origin {
  DSH_ORIGIN_INPUT = 0,               /* 输入框回车 / 查找按钮 */
  DSH_ORIGIN_SELECTION = 1,           /* 正文里选中文字 */
  DSH_ORIGIN_LINK = 2,                /* 词条里的 entry:// 链接（不跑通道） */
  DSH_ORIGIN_BACK = 3,                /* 返回上一词条（不跑通道） */
  DSH_ORIGIN_HISTORY = 4              /* 从查词历史回放（不跑通道） */
} dsh_origin;

/* 通道走到哪一步了。界面据此显示进度；主进程每推进一步就回一个 stage。 */
typedef enum dsh_stage {
  DSH_STAGE_START = 0,                /* 还没开始 */
  DSH_STAGE_AFTER_LOOKUP = 1,         /* 在当前词典查过了 */
  DSH_STAGE_AFTER_PROBE = 2,          /* 问过别的词典了 */
  DSH_STAGE_AFTER_SUGGEST = 3,        /* 联想候选摆出来了 */
  DSH_STAGE_DONE = 4                  /* 走到链的尽头了 */
} dsh_stage;

/* 候选摆在哪儿。 */
typedef enum dsh_surface {
  DSH_SURFACE_NONE = 0,
  DSH_SURFACE_LIST = 1,               /* 输入框下方列表 */
  DSH_SURFACE_TOAST = 2               /* 正文框底部提示（选区那一路） */
} dsh_surface;

/*
 * 词条页底部的出路按钮。**按钮上那行字也由内核给**（见 EntryPayload 的
 * chips[]）—— 文案是产品约定，不是视图。
 */
typedef enum dsh_chip_action {
  DSH_CHIP_BORROW = 0,                /* 用《X》查（借查，不切当前词典）。⚠️ 2026-09 起**这一档永不出现**：借查已经是通道里的自动一步，不该再让用户点一下去做同一件事（参考实现的 D1）。值留着不动枚举编号。 */
  DSH_CHIP_RECHECK = 1,               /* 再问一遍（有没问完的词典时必须给） */
  DSH_CHIP_TRANSLATE = 2              /* 翻译这个词 */
} dsh_chip_action;

/* 主导字形。语种判定的第一步只看这个 ——『什么算汉字』只有一处来源。 */
typedef enum dsh_script {
  DSH_SCRIPT_HAN = 0,
  DSH_SCRIPT_LATIN = 1,
  DSH_SCRIPT_OTHER = 2
} dsh_script;

/* 三层音源。排序由内核定（原录音 → 系统语音 → 在线），界面不许自己排。 */
typedef enum dsh_audio_source {
  DSH_AUDIO_DICT = 0,                 /* 词典自带原录音（.mdd） */
  DSH_AUDIO_SYSTEM = 1,               /* 系统语音（离线合成） */
  DSH_AUDIO_ONLINE = 2                /* 在线发音（默认关） */
} dsh_audio_source;

/* ── 常量（凡是别的文件里也抄了一份的数，都在这里）───────────────────────── */
/*
 * 凡是『另一个文件里也抄了一份、不一致就制造假故障』的数，都在这里。UI
 * 与工具一律从自动生成的文件读，禁止再抄。
 */
/* 候选列表最多画几行。参考实现里这个数抄在三个文件里（那个坑），现在只有一处。 */
#define DSH_MAX_LIST_ROWS 8
/* 选中文本按词计数的阈值：≤4 个词就写词名，≥5 个词说『所选文本』。 */
#define DSH_SELECTION_WORD_MAX 4
/* 查词历史上限（最多保留多少条）。 */
#define DSH_HISTORY_LIMIT 5000
/* 历史下拉一次取多少条（滚到底再取下一页）。 */
#define DSH_HISTORY_PAGE 60
/* 借查探路的单本预算（毫秒）。超预算算『没问完』，绝不许并进『没有』。 */
#define DSH_PROBE_BUDGET_MS 120
/* 长文本朗读每段的目标字数（优先在句末断开）。 */
#define DSH_SPEECH_CHUNK_CHARS 300
/* 发音缓存的内存上限（12 MB）。 */
#define DSH_AUDIOCACHE_MEM_BYTES 12582912u
/* 发音缓存的磁盘上限（64 MB），超了按最久没用过的先删。 */
#define DSH_AUDIOCACHE_DISK_BYTES 67108864u

/* ── 音节分隔点 ──────────────────────────────────────────────────────────
 * 音节分隔点：词典把词头写成 dic·tion·ar·y 时要能忽略掉。故意不含连字符 ——
 * 连字符是合法词条字符（well-known）。参考实现里这份清单在 C# 与 main.ts
 * 各有一份（那个坑），现在只有一处。⚠️ 2026-09
 * 已定往里加了两个**重音符**（U+02C8 /
 * U+02CC，见各自的注释）：这两个码点参考实现里没有，是本版有意超出参考实现的一处（参考实现那边只认前四个）
 * —— 检查标准与沿革 /。
 * ---------------------------------------------------------------------- */
#define DSH_SEP_MIDDLE_DOT 0x00B7u          /* U+00B7 MIDDLE DOT（LDOCE 那种写法） */
#define DSH_SEP_HYPHENATION_POINT 0x2027u   /* U+2027 HYPHENATION POINT */
#define DSH_SEP_KATAKANA_MIDDLE_DOT 0x30FBu /* U+30FB KATAKANA MIDDLE DOT */
#define DSH_SEP_SOFT_HYPHEN 0x00ADu         /* U+00AD SOFT HYPHEN（复制粘贴会带出来） */
#define DSH_SEP_PRIMARY_STRESS 0x02C8u      /* U+02C8 MODIFIER LETTER VERTICAL LINE（主重音，用户 2026-09 点名要的那个符号）：词典把词头写成 ˈæpl 时，它**永远不是词条名的一部分** —— 只在『原样问不到』的第二遍里被去掉，所以加了它不会误伤任何真实词条 */
#define DSH_SEP_SECONDARY_STRESS 0x02CCu    /* U+02CC MODIFIER LETTER LOW VERTICAL LINE（次重音）：同 U+02C8 一条约定，两个一起加（只加一个等于留一半的坑） */

/* 清单本身（顺序即优先级）：供"逐个码点核对"的检查标准用，别在别处再抄一份。 */
static const uint32_t DSH_SEPARATOR_CODEPOINTS[] = {
  DSH_SEP_MIDDLE_DOT, DSH_SEP_HYPHENATION_POINT, DSH_SEP_KATAKANA_MIDDLE_DOT, DSH_SEP_SOFT_HYPHEN, DSH_SEP_PRIMARY_STRESS, DSH_SEP_SECONDARY_STRESS
};
#define DSH_SEPARATOR_COUNT 6u

/* ── 核心：版本、内存、错误 ───────────────────────────────────────────────────────── */
/*
 * 内核版本字符串（如
 * 0.2.0）。宿主启动时打日志、并在握手时与接口定义版本对账。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
const char * dsh_version(void);

/* ABI 整数版本。宿主启动时对不上就应当拒绝启动并给出可读原因，别硬跑。 */
int32_t dsh_abi_version(void);

/*
 * 释放内核返回的任意内存块。传 NULL 是合法的空操作。宿主禁止用 free()
 * 释放内核给的东西；传进来的若不是内核分配的，内核会安全拒绝并记下原因（不崩）。
 */
void dsh_release(void *ptr);

/*
 * 本线程上最近一次失败的人话说明（UTF-8）。没有失败时返回空串。用于日志与『检测凭据』那种要如实说原因的场合。⚠️
 * 例外（约定，见
 * dsh_textcodec.h）：解码类操作遇到坏字节/长度不符等『不算失败』的问题时**返回
 * 0 但把诊断记在这里**（GB18030 四字节区、JSON 字符串坏 UTF-8、zlib
 * 长度不符各一处）—— 所以宿主**不许**在返回值非零之外把 last_error
 * 当『必然出过错』的检查标准，想知道有没有失败看返回值。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
const char * dsh_last_error_message(void);

/* ── 引擎：设置、词库、句柄 ───────────────────────────────────────────────────────── */
/*
 * 建一个引擎。userDataDir 是配置 / 缓存 / 索引的根目录（Windows 上通常是
 * %APPDATA%\LookupApp）。传 NULL 用内核的默认值。
 * 调用方用 dsh_engine_destroy() 关掉它。
 */
enum dsh_error dsh_engine_create(const char *user_data_dir, dsh_engine **out_engine);

/* 销毁引擎，释放它持有的一切（含所有已加载词典）。传 NULL 是合法的空操作。 */
void dsh_engine_destroy(dsh_engine *engine);

/*
 * 取全部设置（JSON）。设置模型住内核里，界面不许自己拼一份、更不许拿内存里的设置回传给内核。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_engine_settings_get(dsh_engine *engine, char **out_json);

/*
 * 改设置（局部补丁 JSON）。内核负责规范化（越界的语速、不认识的音色
 * ID、非法的枚举值都要被夹回合法范围），并负责落盘。返回规范化之后的完整设置。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_engine_settings_set(dsh_engine *engine, const char *patch_json, char **out_json);

/*
 * 词库清单（JSON 数组）。每一项含 id / 显示名 / 路径 / 是否已加载 /
 * 资源卷情况，以及 ★ `unavailable`（`""` / `"missing"`）与
 * `note`（**界面原样显示**的那句人话，可用时是空串）——
 * 「这一本现在还能不能用、不能时怎么恢复」由内核说（与 `dsh_history_query`
 * 每一行**同一处实现**），界面不许自己拼那句话。「已被移出词库」那一档这里看不到（它压根不在清单里）：那一种由历史每一行的
 * `unavailable=removed` 说。id 是内容哈希，不是路径（0.2.0
 * 的决定）——所以重命名文件、换目录都不会丢设置。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_engine_dict_list(dsh_engine *engine, char **out_json);

/*
 * 导入若干 .mdx。paths_json 是 UTF-8 路径数组。返回 {added,
 * failed:[{path,reason}]}。⚠️ 不存在的路径必须进 failed
 * 并写明原因，绝不许记成 added（参考实现那个坑的形态）。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_engine_dict_add(dsh_engine *engine, const char *paths_json, char **out_json);

/*
 * 从词库移除（不动硬盘文件）。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_engine_dict_remove(dsh_engine *engine, const char *dict_id, char **out_json);

/*
 * 给词典起个别名。空串 = 恢复默认名。词典名的合成约定只在内核一处：改过的名
 * → .mdx 头里的书名 → 文件名。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_engine_dict_rename(dsh_engine *engine, const char *dict_id, const char *name, char **out_json);

/*
 * 把一本词典在清单里挪 delta 位（负数往前、正数往后）。★ 2026-09 新加（ABI
 * v2），**参考实现（参考实现）没有这个功能**，语义由本文件定义：到边界就**夹住**（第一本再往前
 * / 最后一本再往后 = 清单原样返回，仍算成功 ——
 * 与界面上那两颗按钮在头尾置灰是一套）；delta 为 0 也是原样返回；id
 * 不在清单里 → 报错（与 dict_rename 同一条）。当前词典**不受影响**（它记的是
 * id 不是下标）。⚠️ 顺序**不只是显示顺序**：「当前词典」那格为空时，查词 /
 * 发音样本 /
 * 增益等若干接口会**兜底取第一本**，所以把某一本挪到第一位会真的改变兜底行为。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_engine_dict_move(dsh_engine *engine, const char *dict_id, int32_t delta, char **out_json);

/*
 * 指定当前词典。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_engine_dict_set_current(dsh_engine *engine, const char *dict_id, char **out_json);

/* ── 查词：落点、联想、通道、正文、资源 ─────────────────────────────────────────────────── */
/*
 * 问落点：这段文字在指定词典里会落到哪条词条。只查表、不落盘、不写历史、不常驻。用于『动手前先问落点』那种判定（参考实现那个坑：用字符串预测点下去的后果必错）。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_engine_resolve(dsh_engine *engine, const char *dict_id, const char *text, char **out_json);

/*
 * 联想候选。三步由内核定：精确命中 → 前缀补全 →
 * 拼写纠正；单字符查询不做拼写纠正（参考实现那个坑：编辑距离 ≤1
 * 对单字符恒真）。行数上限由内核的 DSH_MAX_LIST_ROWS 定。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_engine_suggest(dsh_engine *engine, const char *text, char **out_json);

/*
 * 完整查询（一条链走到底）：当前词典 → 借查别本 → 机器翻译 → 终态页。origin
 * 决定跑不跑这条链。返回值里既有正文数据，也有界面要照抄的整句话与出路按钮。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_engine_lookup(dsh_engine *engine, const char *text, enum dsh_origin origin, const char *dict_id, char **out_json);

/*
 * 问一句就走：在**别的**词典里查一次，答案拿回来，那本词典**不许**留在内存里（参考实现那个坑）。这条与『加载一本』必须分得开。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_engine_probe(dsh_engine *engine, const char *text, const char *dict_id, int32_t budget_ms, char **out_json);

/*
 * 借查：**去别的词典里问一遍**（终态页那条「再问一遍」）。参考实现是
 * `App.BorrowAsync`，这一份逐条照它的约定：① **跳过起点那一本**（入参为空 =
 * 内核按『当前词典』取，『当前』空着时兜底第一本）；②
 * 一本一本问、**问一句就走**（内部走 `dsh_engine_probe` →
 * `dsh_dicts_borrow_once`，那本词典**不许**留在内存里）；③
 * **总预算**：自动那一次 300
 * ms（用户正等着看结果，不能一本一本串着问），`recheck` 时单本放大到 1500
 * ms、总预算 = 词长 × 1500 + 2000（参考实现同一公式 ——
 * 用户刚点了按钮，他要的就是答案，可以等）；④ ★
 * **「没问完」绝不许并成「没有」**（那个坑）：预算用完 / `timeout` / `error`
 * 的那几本进 `unconfirmed`（用**显示名**，界面直接列给人看），而
 * `missing`（这一本明确答没有）**不算没问完**。⚠️
 * 命中时**不加载**那一本，只回『在哪一本、落在哪条』——
 * 真正显示那一条由调用方带明确 dictId
 * 再查一次（`dsh_engine_lookup`），参考实现也是这么分的。⚠️ 返回的
 * `asked`/`perDictMs`/`totalMs`/`elapsedMs`
 * 是**实测结果**（诊断与排错要看它为什么没问完），界面不读。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_engine_borrow(dsh_engine *engine, const char *text, const char *skip_dict_id, int32_t recheck, char **out_json);

/*
 * 取一条词条的可渲染文档（HTML）与其资源。界面把它喂给沙箱 iframe。词条的
 * HTML 兼容性处理、.mdd 资源引用改写、sound:// 与 snd:// 的改写全在内核。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_engine_entry_document(dsh_engine *engine, const char *dict_id, const char *key_text, char **out_json);

/*
 * 按 key 从 .mdd 资源卷（或词典旁边散放的文件）取一段资源字节。支持
 * Range：offset/length 让宿主实现 206/416。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_engine_resource(dsh_engine *engine, const char *dict_id, const char *key, size_t offset, size_t length, uint8_t **out_bytes, size_t *out_len, char **out_meta_json);

/* ── 文本：字形、分隔点、语种 ──────────────────────────────────────────────────────── */
/*
 * 文本分析：字形（判语种第一步）、音节分隔点、去掉点之后的写法、按词计数。★
 * 这一条就是为了把 main.ts 里那三份重复实现收进内核（ADR-001）。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_text_analyze(const char *text, char **out_json);

/*
 * 只做『去掉音节分隔点』这一件事。⚠️ 调用方必须遵守参考实现那个坑/38
 * 的顺序：先拿原样文本问一次，问不到才用这里的返回值重问 ——
 * 内核不替你改语义（ResolveKey 永远只认原样文本）。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_text_strip_separators(const char *text, char **out_text);

/*
 * 判语种：字形 → 词典标题 →
 * 用户设的默认语种。返回判定结果**与检查标准说明**（界面提示里要写『英语（按字形判断（拉丁字母））』，那句话由内核给，界面不拼）。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_language_detect(dsh_engine *engine, const char *text, const char *dict_id, char **out_json);

/*
 * 语种码 → **中文名**（`"en"` → `"英语"`）。★
 * 这张表是**判定语种用的同一张表**（在 `text/dsh_language.c`
 * 里），所以界面要显示语种名时**必须问内核**，不许在壳或界面里再存一份 ——
 * 两处各存一份迟早会对不上（参考实现的注释专门点过这一条）。⚠️
 * 认不出的码回**空串**（不是拿码当名字），调用方自己决定怎么显示。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_language_label(const char *code, char **out_json);

/* ── 发音：音源规划、词典原录音、音频预处理 ───────────────────────────────────────────────── */
/*
 * ★ 豆包音色 id → **界面上的说法**（参考实现的
 * `DoubaoSpeech.DescribeSpeaker` +
 * `DefaultSpeakerEnName/ZhName`）：两个默认音色回官网名 `Dacey` /
 * `Vivi`，**其余一律回 id 本身**（不是空串 ——
 * 界面上总得有个能认的东西）。为什么这条要进接口定义、而不是壳里再抄一张表：音色
 * id
 * 与它的展示名**是同一份产品数据**，抄到壳里就是「同一个东西两个来源」（那个坑），迟早一处改了另一处没改。谁在用：`speech:status`
 * 的两个 `speakerEnName`/`speakerZhName`、`speech:testDoubao` 每一项的
 * `speakerName`、在线发音回包里的 `voiceName`（`voiceId` 那格才是
 * id），以及内核自己拼的那句「为什么走这条」（`chosen.detail` / `why`
 * 里显示的是名字，不是 `en_female_dacey_uranus_bigtts` 这种长串）。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_speech_speaker_label(const char *speaker, char **out_label);

/*
 * 这次朗读怎么念：三层音源的排序与选择、挑哪个嗓子、要不要切段、切几段。★ 把
 * main.ts 的 speechPlan / splitForSpeech
 * 收进内核（ADR-001）。顺序是**产品约定**（词典自带 → 在线 →
 * 系统离线），界面不给选、也不许自己判。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_speech_plan(dsh_engine *engine, const char *text, const char *dict_id, const char *voices_json, const char *overrides_json, char **out_json);

/*
 * 把一段 WAV 里的 16 位 PCM 按 dB 缩放（参考实现的
 * `GainMath.ApplyToWav`）。⚠️
 * 这是**系统离线语音那条路**的音量：参考实现就是在合成完、把字节交给播放器之前缩的（在『产字节』那一侧施加），所以那条路回包里的
 * `gainDb` **恒为 0** —— 前端再乘一次就是叠两遍。三条刻意的约定：① **顺序扫
 * RIFF 块**找 `fmt ` 与 `data`（不假设 44 字节头、也不假设只有一段 data ——
 * SAPI 会插 `LIST`/`fact` 块，写死 44 会让增益『时灵时不灵』且错得隐蔽）；②
 * **只动 16 位 PCM**，别的格式（8/24/32
 * 位、浮点、压缩）**原样返回并说明**，不猜着改；③ **削顶不能不报错地** ——
 * 想提的比峰值允许的多就夹回去，并把『从多少夹到多少、为什么』通过 `note`
 * 说出去（悄悄削波听起来是破音，而用户没有任何线索去查）。⚙
 * 削顶保护的算法：`applied = min(请求值,
 * 20·log10(1/峰值))`，再**向下**取整到 0.1
 * dB（宁可轻一点，也不要因为取整把峰值顶出满刻度）；**0 dB
 * 一个字节都不动**（默认路径必须与『没有这个功能』逐字节相同）。⚠️ 传进来的
 * dB 是**已经归一化过**的（夹到 [-24,+12]、取整到 0.1 —— 那是设置层的活，见
 * `dsh_speech_gains`/`dsh_speech_gains_set`）。⚠️
 * 为什么这个参数是**整数**（dB×10）而不是浮点：接口定义里没有浮点标量类型，而增益在设置那一层本来就取整到
 * 0.1 dB —— 传 dB×10 是**精确**的，也省掉一次跨 ABI 的浮点往返。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_audio_apply_gain(const uint8_t *bytes, size_t len, int32_t gain_tenths_db, uint8_t **out_bytes, size_t *out_len, char **out_meta_json);

/*
 * 这条词条自带的原录音在哪：按『常见扩展名优先、原始键垫底』在多卷 .mdd
 * 里找，返回键名与可播放格式。例句录音**不算**词目发音（参考实现的硬约定）。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_speech_dict_audio(dsh_engine *engine, const char *dict_id, const char *key_text, char **out_json);

/*
 * 从一本词典里挑几条**真的有录音**的词条（参考实现的
 * `App.DictSamplesAsync`）。⚙ 挑法逐条照参考实现：① **均匀撒网** ——
 * 按**整本书的词条序号**分若干段、每段取开头那一条（⚠️
 * 单位是**词条**不是**词块**：按块取的话，一本只有 1 块的词典只能给出 1
 * 个候选 —— 真词典 正是 1 块 9 条，实测踩过）；② 不够 6
 * 条时**按索引顺序兜底补扫**（均匀撒点是一张网，网眼之间可能正好漏掉『录音集中在某一段』的词典）；③
 * 每条都**真解词条正文**去找录音，而且**只认词目发音**（例句不算 ——
 * 量的是『点发音按钮会听到的那一段』，所以挑法与真正发音时完全一致：同一个
 * `dsh_speech_dict_audio`）；④ 三个上限：6 条 / 最多扫 60 个候选 / **最多
 * 400 ms**（一次『解词条正文 + 到 .mdd
 * 里找文件』实测十几毫秒，无上限地扫就是拿调用方的一次点击去跑后台任务）。⚠️
 * `ok=false` 时 message 是**三档不同的人话**：一本词典都没有 /
 * 这本词典没有资源卷（.mdd）/
 * 有资源卷却扫不到（多半是音频卷没关联上）；扫不满 6 条但有一条以上时
 * `ok=true` 且 message 说清『只找到 N 条』。⚠️ 返回的是
 * **`audioKey`**（`.mdd`
 * 里的键名），**可播地址由外壳拼**（`https://<外壳域>/__sound__/<词典
 * id>/<键名>`）—— 地址是平台形状，与 `dsh_speech_plan` 的 dict
 * 那一层同一条规矩。⚠️
 * 今天**没有产品流程调它**（界面上的「平衡音量」按按需求删掉了，参考实现亦然）：接口、外壳接线与
 * B/标准都留着备用，**别看到『没人调』就删**。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_speech_dict_samples(dsh_engine *engine, const char *dict_id, char **out_json);

/*
 * 在线语音（豆包 · 单向流式
 * HTTP）的**第一步：内核说该发什么**。三层音源里在线那一层的判断全在这儿：有没有配凭据与音色、这次该用哪个音色（**中英混排必须走中文音色**
 * ——
 * 实测拿英文音色念混排会得到空句子）、模型版本与音色配不配套（`seed-tts-2.0`
 * / `seed-tts-1.0`）、请求体长什么样、四个头是什么。⚠️ 请求体里**刻意不写
 * `explicit_language`**：它的语义是"只念这个语种"，而词典正文中英混排是常态$1
 * docs/豆包语音合成接入方案.md 与 的实测）。⚠️ ok=false 时 reason
 * 是人话，三种原因分开说（没填 Key / 没配音色 / 文本是空的）。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_speech_online_plan(dsh_engine *engine, const char *text, const char *dict_id, const char *overrides_json, char **out_json);

/*
 * 在线语音的**检测凭据**那一条：管理窗「检测凭据」要发的那几次请求，该发什么。与
 * dsh_speech_online_plan
 * 发的是**同一种东西**（同一份拼请求的代码），差别只在「念什么词、用哪个音色」由谁定
 * —— 这里由**界面正在填的那两个音色**定（用户改了 ID
 * 还没写盘时，要测的必须是「即将存下去的那个」，测设置里存着的旧配置等于没测）。⚙
 * 逐项约定：① 给了 speaker 就只测它一项，语种用给的那个（认不出按
 * en），念的词从**内核自带的样本词表**取（`apple` / `苹果` …，与参考实现的
 * SampleWord 逐条相同）；② 没给 speaker 就**英文 +
 * 中文各测一次**（两个音色的 Key / Resource 配套关系一样，但音色 ID
 * 写错只有实测才发现）；③ 音色是空的 → 那一项 ok=false 且 reason
 * 是「这个音色没填（这一项测不了）」——**「没东西可测」必须与「服务端说它不能用」分开说**，界面的判定（web/src/manager/main.ts
 * 的 classifyDoubaoTest）就吃这一条；④ 没填 Key → 每一项的 reason
 * 都是那句凭据提示。⚠️
 * 它**不受「在线总开关」限制**（先测通了再打开它，参考实现同一约定），但会**真联网**、按字符计费，所以只由用户点按钮触发。⚠️
 * 外壳在每一项上再补上 HTTP
 * 的结果（statusCode/bytes/mime/elapsedMs/billedWords/error）与 speakerName
 * —— 后者是**界面上的说法**（`Dacey` / `Vivi`；认不出就是 id 本身），由壳问
 * `dsh_speech_speaker_label` 得到（参考实现的
 * `DescribeSpeaker`），**壳里不许再抄一张表**（那个坑）。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_speech_online_test_plan(dsh_engine *engine, const char *speaker, const char *language, char **out_json);

/*
 * 在线语音的**第二步：内核说回包是什么意思**。外壳把 plan
 * 给的东西原样发出去，把 **HTTP 状态、整段回包正文、耗时**交回来 —— 解析 SSE
 * 的 `data:` 行、把每段 base64 顺序拼成音频、错误码翻成人话，全在这里。⚠️
 * 回包是 **SSE（`data:`
 * 行）**，所以外壳要**整段缓冲**再交进来，不许自己边收边解析。⚠️ **`code 0`
 * 不等于成功**：实测"英文音色念中英混排"就是 code 0 +
 * 零字节音频，所以"音频是空的"必须单独判成失败，且那句话要点出原因。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_speech_online_accept(dsh_engine *engine, const char *plan_json, int32_t http_status, const char *response_body, int32_t elapsed_ms, uint8_t **out_bytes, size_t *out_len, char **out_json);

/*
 * 音量增益的现状（界面那两条滑块的值）。★
 * **增益是"按词典"存的**（`speech.dictGainDb`：一本一个数）+
 * **全局一个**（`speech.systemGainDb`）——
 * 不同词典的录音本来就录得不一样响。归一化约定（夹到 [-24, +12] dB、取整到
 * 0.1）在**写**那一条里做，这里只报现状。⚠️ `dictAvailable` / `dictMessage`
 * / `systemAvailable` / `systemMessage` 是**判断 +
 * 两句产品文案**：能不能校准、不能校准时缺什么，只有内核手里有检查标准（界面一个字都不拼）。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_speech_gains(dsh_engine *engine, const char *dict_id, int32_t voice_count, char **out_json);

/*
 * 改音量增益，返回改完之后的那份视图（调用方拿它重排滑块，不必再问一次）。⚠️
 * 两个键都是"**可空数字**"，三种语义必须分清：**传数字** =
 * 设成这个值（会被夹到 [-24, +12] 并取整到 0.1）；**传 null** =
 * 清掉这一项（回到"没设过"= 0 增益）；**整个键不传** =
 * 不改（前端只发它要改的那一项）。`dictGainDb`
 * 只改**那一本词典**的数（增益按词典存）。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_speech_gains_set(dsh_engine *engine, const char *dict_id, const char *patch_json, int32_t voice_count, char **out_json);

/*
 * 把一段音频字节整成播放器能播的格式：格式按魔数嗅探、Speex(.spx) 就地解成
 * 16bit PCM WAV、认不出来就**当场如实报错**（不许让播放器报错误码、更不许拿
 * TTS 假装顶上）。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_audio_prepare(const uint8_t *bytes, size_t len, uint8_t **out_bytes, size_t *out_len, char **out_meta_json);

/* ── 历史：分页查询与清空 ────────────────────────────────────────────────────────── */
/*
 * 查词历史（分页）。上限与每页条数由内核常量定。0.2.0 起历史落在
 * `<配置目录>/history.jsonl`（一行一条 JSON
 * 的追加文件），界面只按页取、不许自己算分页。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_history_query(dsh_engine *engine, int32_t offset, int32_t limit, char **out_json);

/*
 * 清空查词历史。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_history_clear(dsh_engine *engine, char **out_json);

/* ── 机器翻译 ──────────────────────────────────────────────────────────────── */
/*
 * 机器翻译的设置与凭据状态（**只读设置、不联网、不花钱**）。界面靠它决定「能不能显示翻译」以及「缺
 * Key 该怎么说明」。字段与参考实现的 TranslateStatus 逐字相同 —— 连 endpoint
 * / resourceId
 * 两个常量也一起给（排错时一眼看出请求被指到哪儿去了；resourceId 恒为
 * volc.speech.mt，它要在控制台**单独开通**，与语音的 seed-tts-2.0
 * 不是一回事）。⚠️ `enabled`（用户自己的开关）与
 * `hasApiKey`（凭据有没有）**必须分开判**：界面把两者合成一个
 * disabled，就会把「我自己关的」与「客观不可用」混成同一种灰，用户会反复点那个开关。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_translate_status(dsh_engine *engine, char **out_json);

/*
 * 机器翻译的**第一步：内核说该发什么**。内核零依赖、没有
 * socket，所以「把字节发出去」是平台层的活；但**判断一个字都不许留在平台层**
 * —— 译成哪个语种、source
 * 传不传、请求体长什么样、端点与三个头、这门语言支不支持，全在这里定。这一步的产物交给外壳原样发出去，回包再交给
 * dsh_translate_accept。⚠️ **命中缓存时不必发请求**：ok=true 且
 * cached=true，translation 就是译文。⚠️ ok=false 时 `message`
 * 是人话，而且**四种原因分开说**（开关关着 / 没填 Key / 这门语言 MT 不支持 /
 * 文本是空的）—— 合并成一句会让用户去改错的地方。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_translate_plan(dsh_engine *engine, const char *text, const char *dict_title, char **out_json);

/*
 * 机器翻译的**第二步：内核说回包是什么意思**。外壳把 plan
 * 给的东西原样发出去，然后把它拿回来的 **HTTP 状态、回包正文、耗时** 交回来
 * —— 解析译文、识别到的语种、计费
 * token、错误码翻成人话、写缓存全在这里做。⚠️ 错误码表**与 TTS
 * 不是同一张**（这边成功是 20000000；TTS
 * 那边同一个码含义不同），复用会把「成功」当「失败」。⚠️
 * **成功码但拿不到译文 → 不算成功**（宁可报错也不给一条空译文）。⚠️
 * 返回的是界面那份 TranslateResult（含 sourceLabel / targetLabel
 * 两个中文名）—— 语言的中文名是产品文案，只许在内核这一处拼。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_translate_accept(dsh_engine *engine, const char *plan_json, int32_t http_status, const char *response_body, int32_t elapsed_ms, char **out_json);

/*
 * 清空译文缓存（界面上那个「清空翻译缓存」）。返回
 * {"count":N}（清掉几条）。⚠️ 它是**必需项而不是优化**：MT 按 token
 * 计费，而用户来回跳词条会把同一句翻很多次。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_translate_clear_cache(dsh_engine *engine, char **out_json);

/*
 * 把一次翻译包成**正文框里那个词条的载荷**（参考实现的 `TranslatePayload` +
 * `Annotate`）。用于查词通道自动翻译那一档：链说「该翻译」（`via=translate`
 * + `needsTranslate=true`），外壳把请求发出去、把回包交给
 * `dsh_translate_accept`，再拿 accept
 * 的结果与**伪词条地址**回来问这一条，得到一份与真词条**同形状**的载荷 ——
 * 于是朗读、复制、返回栈一行都不用改就都生效（参考实现的原话：译文若另起一块
 * UI，这四件事就得各写第二遍）。⚠️ `entry_url`
 * 由外壳给（浏览器地址里那个令牌是外壳的一次性表，内核不认识 socket 也不认识
 * URL 表），其余字段**一个字都不许在外壳拼**。⚠️
 * 翻译失败时（`ok=false`）这一步**不许**装作翻过了：回的是 `found=false` +
 * `via=terminal` + 那句人话。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_translate_payload(dsh_engine *engine, const char *translate_json, const char *entry_url, char **out_json);

/* ── 解析层：单本 .mdx（工具与逐字节对照用，产品路径走引擎） ─────────────────────────────────────── */
/*
 * 打开一本 .mdx（解析层，不经引擎）。探测 / 逐字节对照 /
 * 工具用得上；产品路径一律走引擎。
 * 调用方用 dsh_dict_close() 关掉它。
 */
enum dsh_error dsh_dict_open(const char *path, dsh_dict **out_dict);

/* 关掉一本词典。 */
void dsh_dict_close(dsh_dict *dict);

/*
 * 词典头信息：书名、条目数、版本、加密标志、编码、资源卷命中的那几个。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_dict_info(dsh_dict *dict, char **out_json);

/*
 * 词典里有没有这个键。⚠️ 只认精确命中（大小写变体算命中），**不联想** ——
 * 这是 product 约定，不是解析层细节。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_dict_contains(dsh_dict *dict, const char *key, char **out_json);

/*
 * 取一条记录的原始内容（未渲染）。@@@LINK 重定向在这里解开，并给出最终落点。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_dict_fetch(dsh_dict *dict, const char *key, char **out_json);

/*
 * 枚举全部键（JSON
 * 数组），按词典的序数序。逐字节对照与诊断的主力接口：拿它跟参考实现 /
 * js-mdict 逐条比。
 * 出参由内核分配：调用方用 dsh_release() 还给内核。
 */
enum dsh_error dsh_dict_keys(dsh_dict *dict, char **out_json);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DSH_LOOKUP_H */
