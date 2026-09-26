/* ==========================================================================
 * 朗读的**规划层**（纯策略）：这次到底怎么念。
 *
 * 「这次怎么念」是一条**产品约定**：顺序是**词典自带 → 在线 → 系统离线**，用户不给选
 * （多一个开关只会让用户以为「我选了在线却没走在线」）。所以界面只许照抄内核算出来的结论。
 *
 * 这一层与「平台能力」的分界：**壳负责探测**（本机装了哪些离线音色，把那张表原样交给内核），
 * **内核负责判断**（哪个语种配哪个嗓子、哪一层能走、不能走时那句人话）。
 *
 * 四条改动前必须先读的约定：
 *   ① **挑离线音色**：显式指定的音色优先 → 按语种筛 → 英语再看地区（uk → en-GB、其余
 *      en-US）→ 都没有就用候选里的第一个。⚠️ 「找精确地区失败就用第一个」是**刻意的**
 *      （只装了英音音色的机器上 auto 用户也得听得到英语声）。
 *   ② **在线音色留空 = 用内置默认**（英文 Dacey / 中文 Vivi），**不是「关掉在线」**。
 *   ③ **切段**：一段最多 300 字符，断点优先级 句末标点 → 逗号/顿号/冒号 → 空白 → 硬切
 *      （TTS 每段独立合成，断在句子中间听着像「被掐了一下」）。
 *   ④ **在线那一层的可用性只看「填没填 Key」**：没有总开关 —— 真到无网时它会失败，
 *      然后落到系统语音。
 * ========================================================================== */

#ifndef DSH_SPEECHPLAN_H
#define DSH_SPEECHPLAN_H

#include <stddef.h>
#include <stdint.h>

/** 一段最多多少字符 —— 与参考实现前端的 `SPEECH_CHUNK_CHARS` **同值** */
#define DSH_SPEECH_CHUNK_CHARS 300

/** 音源层（顺序就是产品约定，别改）*/
typedef enum {
  DSH_SPEECH_DICT = 0,
  DSH_SPEECH_ONLINE = 1,
  DSH_SPEECH_SYSTEM = 2
} dsh_speech_source;

/** 一个离线音色（壳探测来的那张表里的一项） */
typedef struct {
  char *id;        /* 音色 id（挑中之后回给宿主的就是它）*/
  char *name;      /* 显示名 */
  char *language;  /* 主代码：「en」 / 「zh」 / … */
  char *culture;   /* 区域标记：「en-US」 / 「en-GB」 / …（英语那条地区规则要用）*/
} dsh_voice;

/** 一张音色表（内核分配；用 `dsh_voices_free` 还）*/
typedef struct {
  dsh_voice *items;
  int64_t count;
} dsh_voice_list;

/** 一段朗读文本 */
typedef struct {
  char *text;
  int64_t chars;
} dsh_speech_chunk;

/**
 * 解析壳给的那张音色表。
 *
 * 形状：`[{"id":"…","name":"…","language":"en","culture":"en-US"}, …]`
 * @param json  UTF-8（NULL / 空串 = 壳没探测到 → 返回 0 且 `out->count = 0`）
 * @param len   字节数（0 = 用 strlen）
 * @return 0 成功（空表也算成功）；非零失败（JSON 不是数组 / 内存不足）
 * ⚠️ 每一项缺字段**不算失败**：能读多少读多少（`name` 缺就借 `id`，`culture` 缺就从
 *    `language` 推默认区域）—— 为了一项写错就把整个朗读弄哑是不划算的。
 */
int dsh_speech_parse_voices(const char *json, size_t len, dsh_voice_list *out);

/** 释放音色表（传 NULL 是空操作）*/
void dsh_voices_free(dsh_voice_list *list);

/**
 * 按语种 + 口音偏好挑一个离线音色。
 *
 * @param preferred_id 用户显式指定的音色（NULL / 空 = 没指定）
 * @return 选中的那一项（借用，归 `list` 所有）；一个都没有 → NULL
 */
const dsh_voice *dsh_speech_pick_voice(const dsh_voice_list *list, const char *language,
                                       const char *accent, const char *preferred_id);

/**
 * 在线（豆包）这次用哪个音色。
 *
 * @param language 语种主代码（NULL / 认不出 → 当 「en」）
 * @param mixed    中英混排（混排一律用中文音色 —— 中文那条能读英文，反过来不行）
 * @param speaker_en / speaker_zh 设置里的两个音色（NULL / 空 → 用内置默认）
 * @return 音色 id（**借用**：要么是入参的指针，要么是内核自带的默认常量）
 */
const char *dsh_speech_doubao_speaker(const char *language, int mixed, const char *speaker_en,
                                     const char *speaker_zh);

/** 内置默认音色（与参考实现的常量逐字相同）*/
#define DSH_SPEECH_DEFAULT_SPEAKER_EN "en_female_dacey_uranus_bigtts"
#define DSH_SPEECH_DEFAULT_SPEAKER_ZH "zh_female_vv_uranus_bigtts"

/**
 * 把要念的文本切成几段。
 *
 * @param out       出参：内核分配的数组（调用方 `dsh_speech_chunks_free`）
 * @param out_count 出参：几段（**空文本 → 0 段**）
 * @return 0 成功；非零失败（内存不足）
 */
int dsh_speech_split(const char *text, dsh_speech_chunk **out, int64_t *out_count);

/** 释放切段结果 */
void dsh_speech_chunks_free(dsh_speech_chunk *chunks, int64_t count);

/**
 * 这段文字**有没有可念的东西**（空串 / 全是空白 → 0）。
 *
 * ⚠️ 它是「空白」这件事**唯一的那份定义**（`dsh_speech_split` 用的是同一段逻辑）。
 *    `dsh_speech_plan` 必须**先**知道「根本没东西念」才能不去选择音色 —— 少了这一条，
 *    空文本会被规划成「可以念」。
 */
int dsh_speech_has_text(const char *text);

/**
 * 「本机装了哪些语种」：取**不重复的语种中文名**、用「、」连起来；一个音色都没有时回「无」。
 *
 * @param out      出参缓冲（调用方给）
 * @param out_cap  容量（字节，建议 ≥ 256）
 */
void dsh_speech_describe_voices(const dsh_voice_list *list, char *out, size_t out_cap);

#endif /* DSH_SPEECHPLAN_H */
