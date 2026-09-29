/* ==========================================================================
 * 豆包在线语音（单向流式 HTTP / SSE）· **纯逻辑层**
 *
 * 内核零依赖、没有 socket，所以「把字节发出去」是平台层的活；而**该发什么、回包是什么
 * 意思**全在这里 —— 外壳只搬运，一个判断都不做。
 *
 * 协议：POST .../api/v3/tts/unidirectional/sse，头带 X-Api-Key 与 X-Api-Resource-Id
 *   （**必须与音色配套**）；体是 {user, req_params:{text, speaker, audio_params, additions}}；
 *   回包是 SSE 的 `data:` 行 `{"code":0,"data":"<base64>"}`，把每行 base64 顺序拼成 mp3。
 *   `code` 为 0 / 20000000 = 成功。完整字段见 `docs/design/豆包语音合成接入方案.md`。
 *
 * 三条**实测得来的硬约定**（不照做就踩坑）：
 *   ① **不传 `explicit_language`** —— 它的语义是「**只念**这个语种」而不是「提示语种」，
 *      而词典正文是中英混排的常态；拿英文音色念混排会让服务端**不报错地**回空句子。
 *      所以混排走**中文音色**（见 `dsh_doubao_speaker_for`）。
 *   ② **`code 0` 不等于成功**（上面那一档就是 code 0 + 零字节）：解出来的音频是空的
 *      必须**单独判成失败**，而且要说清原因，否则用户只看到「点了没声音」。
 *   ③ **计费字数要点亮才给**：请求头里的 `X-Control-Require-Usage-Tokens-Return: *`
 *      少一个字符，回包就没有 `usage.text_words`。
 *
 * 错误码 → 人话那张表**与翻译那张不是同一张**；不许把原始码丢给用户，也不许把
 * 「没开通 / Key 错」说成「参数错误」（那会把用户指去改错的地方）。
 * ========================================================================== */

#ifndef DSH_AUDIO_DSH_DOUBAO_H
#define DSH_AUDIO_DSH_DOUBAO_H

#include <stddef.h>
#include <stdint.h>

/** 端点：**带 `/sse` 的那个**（不带它回的不是流式音频）*/
#define DSH_TTS_ENDPOINT "https://openspeech.bytedance.com/api/v3/tts/unidirectional/sse"

/** 两个模型版本：**必须与音色配套**，填错回 `40000001 参数错误` */
#define DSH_TTS_RESOURCE_2_0 "seed-tts-2.0"
#define DSH_TTS_RESOURCE_1_0 "seed-tts-1.0"

/** 成功码（**这张表里 0 也算成功** —— 与翻译那张只有 20000000 不同）*/
#define DSH_TTS_CODE_OK 0
#define DSH_TTS_CODE_OK_ALT 20000000

/** 音频参数（整文件用 mp3；采样率默认 24000）*/
#define DSH_TTS_FORMAT "mp3"

/** 音频 MIME（mp3）—— 壳往 __speech__ 那条路由上发的时候要用 */
#define DSH_TTS_FORMAT_MIME "audio/mpeg"
#define DSH_TTS_SAMPLE_RATE 24000
#define DSH_TTS_BIT_RATE 64000

/* 两个**默认音色**（英文 Dacey、中文 Vivi），与参考实现的 DoubaoSpeech 默认值逐字相同。
 * ⚠️ 它们住在**这一层**（音色 id 是音频层的产品数据），设置那一层 include 本文件来取默认值
 * —— 只此一处，别再抄第二份（「同一个东西两个来源」是本仓库最贵的坑）。 */
#define DSH_DOUBAO_DEFAULT_SPEAKER_EN "en_female_dacey_uranus_bigtts"
#define DSH_DOUBAO_DEFAULT_SPEAKER_ZH "zh_female_vv_uranus_bigtts"

/**
 * 音色 id → **界面上的说法**：两个默认音色回官网名 `Dacey` / `Vivi`，**其余一律回 id 本身**
 * （不是空串 —— 界面上总得有个能认的东西）。空 id / NULL 回 「」。
 *
 * ⚠️ 这张表**只住在这里**：壳那边要名字就走接口定义的 `dsh_speech_speaker_label`
 *    （见 `abi/lookup.abi.json`），别再抄一份。
 *
 * @param speaker 音色 id（可空；大小写不敏感）
 * @return 静态字符串或**入参本身**
 */
const char *dsh_doubao_speaker_label(const char *speaker);

/** 两个默认音色的内置响度补偿（官方 `loudness_rate`，-50 = 0.5 倍 = 官方下限）：它们
 * 是候选里最响的两个，所以压到下限；表外的音色一律 0。 */
#define DSH_DOUBAO_BUILTIN_LOUDNESS (-50)

/**
 * 这条音色该配哪个模型版本：**`_mars_bigtts` / `_moon_bigtts` 结尾的是 1.0，其余是 2.0**。
 * 填错的表现是 `40000001 参数错误`，而那句人话里已经说了「音色与模型版本不配套」。
 */
const char *dsh_doubao_resource_for(const char *speaker);

/**
 * 这次该用哪个音色。
 *
 * **中英混排的文本走中文音色，其余走英文音色** —— 理由是实测：英文音色念中英混排会得到
 * 空句子（见 .c 顶上那段第 ① 条）。中文音色那个空着时**回落到英文音色**。
 *
 * @param mixed      这段文本是不是中英混排（`dsh_language_is_mixed` 判的）
 * @param language   语种主代码（「en」 / 「zh」 / …）
 * @param speaker_en 设置里的英文音色（可空）
 * @param speaker_zh 设置里的中文音色（可空）
 * @return 静态字符串或入参之一；**两个都空就回 NULL**（调用方如实报「没配音色」）
 */
const char *dsh_doubao_speaker_for(int mixed, const char *language, const char *speaker_en,
                                   const char *speaker_zh);

/**
 * 检测 / 试听用的**样本词**：每个语种都给一个「很常见、很短、词典里多半有」的词 ——
 * 这样检测失败基本只会因为接口本身不通，而不是「样本词服务端念不了」。表外语种回 `"hello"`。
 *
 * ⚠️ 表是**产品数据**，所以它住在内核里（界面与外壳一个字都不拼），用户改不了它。
 *
 * @param language 语种主代码（大小写不敏感）
 * @return 静态字符串，永不 NULL
 */
const char *dsh_doubao_sample_word(const char *language);

/**
 * 这一次合成该带多少**响度补偿**（官方 `loudness_rate`，-50..100，100 = 2.0 倍）。
 *
 *   ① **设置里那两个滑块优先**，但只对**它对应的那个音色**生效（比 id 时大小写不敏感）；
 *   ② 没设过就用**内置表**：两个默认音色都是 -50（官方下限）；
 *   ③ 表里没有的音色（用户自己填的）= **0**（不补偿）—— 让他用设置里那两个滑块。
 *
 * ⚠️ 内置表那 -50 是**量出来的，不是拍的**（同一句参考文本、同一次测量）：那两个默认音色
 *    是候选里最响的两个，所以压到下限。
 *
 * @param speaker      这次实际用的音色（`dsh_doubao_speaker_for` 的返回值）
 * @param speaker_en   设置里的英文音色（可空）、`has_loud_en`/`loud_en` 是它配的响度
 * @param speaker_zh   设置里的中文音色（可空）、`has_loud_zh`/`loud_zh` 同上
 * @return 夹到 [-50, 100] 的值
 */
int dsh_doubao_loudness_for(const char *speaker, const char *speaker_en, int has_loud_en, int loud_en,
                            const char *speaker_zh, int has_loud_zh, int loud_zh);

/**
 * 拼请求体。
 *
 * ⚠️ 里面**不写 `explicit_language`** —— 那是刻意的，不是漏了（见 .c 顶上那段第 ① 条）。
 *
 * @param speech_rate   `speech_rate ∈ [-50, 100]`（100 = 2 倍速）
 * @param loudness_rate 响度补偿；这个值**会被夹到 [-50, 100]**（来源只有「已夹过的设置值」
 *                      与「内置常量」两处，为一个音色美化参数把整次合成拒掉并不划算）
 * @return 新分配的 JSON 串（调用方 `dsh_release`）；参数不合法回 NULL（last_error 已写）
 */
char *dsh_doubao_request_body(const char *text, const char *speaker, int speech_rate,
                              int loudness_rate);

/** 一次合成的结果 */
typedef struct {
  int ok;               /**< 拿到**非空**音频才算成功 */
  int64_t code;         /**< 最后一个非成功码（成功时是 0）*/
  uint8_t *audio;       /**< 音频字节（调用方 `dsh_release`）*/
  size_t audio_len;
  const char *mime;     /**< 静态字符串（「audio/mpeg」）*/
  int64_t text_words;   /**< 计费字数（没点亮那个头时是 0）*/
  char *reason;         /**< 失败时那句人话（调用方 `dsh_release`）*/
} dsh_tts_result;

/**
 * 解析 SSE 回包：把每一行 `data:` 里的 base64 顺序拼成音频。
 *
 * @param status HTTP 状态码（0 = 平台层根本没发出去）
 * @param body   回包正文（**整段**；平台层把整个响应缓冲下来再交进来）
 * @return 0 = 解析过程本身成功（**看 `out->ok` 判合成成没成**）；-1 = 入参不合法
 */
int dsh_doubao_parse(int status, const char *body, size_t len, dsh_tts_result *out);

/** 释放 `dsh_doubao_parse` 填出来的那些东西 */
void dsh_doubao_result_free(dsh_tts_result *result);

/** 错误码 → 人话（认不出来时回 NULL，由调用方拼上「服务端原话 + code」）*/
const char *dsh_doubao_error_sentence(int64_t code);

#endif /* DSH_AUDIO_DSH_DOUBAO_H */
