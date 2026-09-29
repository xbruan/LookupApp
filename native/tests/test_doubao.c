/* ==========================================================================
 * 内核单元测试 · 豆包在线语音（纯逻辑层）+ base64
 *
 * 一条网络请求都不发（内核零依赖，没有 socket）：测的是
 * **「该发什么」**（请求体 / 音色与模型版本配不配 / 语速范围）与
 * **「回包是什么意思」**（SSE 的 `data:` 行 → 音频字节 / 各种错误码 → 人话）。
 *
 * 三条**踩过才知道**的硬约定（约定全文见 docs/design/豆包语音合成接入方案.md）：
 *
 *   ① **`explicit_language` 一律不传** —— 它的语义是「只念这个语种」，而词典正文中英
 *      混排；拿它去念混排会得到**空句子**。
 *   ② **`code 0` 不等于成功** —— 上面那一档就是 code 0 + 零字节，必须单独判失败。
 *   ③ **模型版本必须与音色配套**，填错回 `40000001`。
 *
 * 另外 base64 那一组钉的是「**宽容与严格的分界**」：空白（SSE 每行的 `\r`）要忽略，
 * 而坏字符 / 乱放的填充符要**如实失败**（不许跳过继续 —— 那会把坏音频交给播放器）。
 * ========================================================================== */

#include "audio/dsh_doubao.h"
#include "dsh_lookup.h"
#include "text/dsh_base64.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks = 0;
static int g_failed = 0;

static void ok(int cond, const char *what) {
  g_checks++;
  if (!cond) {
    g_failed++;
    fprintf(stderr, "FAIL %s\n", what);
  }
}

static void ok_eq_str(const char *actual, const char *expected, const char *what) {
  g_checks++;
  if (actual == NULL || expected == NULL || strcmp(actual, expected) != 0) {
    g_failed++;
    fprintf(stderr, "FAIL %s\n      实际=%s\n      期望=%s\n", what, actual ? actual : "(null)",
            expected ? expected : "(null)");
  }
}

static void ok_eq_i64(int64_t actual, int64_t expected, const char *what) {
  g_checks++;
  if (actual != expected) {
    g_failed++;
    fprintf(stderr, "FAIL %s：实际=%lld 期望=%lld\n", what, (long long)actual, (long long)expected);
  }
}

static void ok_has(const char *text, const char *needle, const char *what) {
  g_checks++;
  if (text == NULL || strstr(text, needle) == NULL) {
    g_failed++;
    fprintf(stderr, "FAIL %s\n      实际=%s\n      里面没有=%s\n", what, text ? text : "(null)",
            needle);
  }
}

/* ══════════════════════════════════════════════════════════════════════════
   ① base64：宽容的是空白，严格的是坏字符
   ══════════════════════════════════════════════════════════════════════════ */

static void test_base64(void) {
  uint8_t *out = NULL;
  size_t len = 0;

  /* 「apple」→ YXBwbGU= */
  ok(dsh_base64_decode("YXBwbGU=", 8, &out, &len) == 0, "① 解得出");
  ok_eq_i64((int64_t)len, 5, "① 长度对");
  ok(out != NULL && memcmp(out, "apple", 5) == 0, "① 内容对");
  if (out) dsh_release(out);

  /* 无填充（服务端的分段常常不带 `=`）*/
  ok(dsh_base64_decode("YXBwbGU", 7, &out, &len) == 0, "① 没有填充也解得出");
  ok_eq_i64((int64_t)len, 5, "① 没有填充时长度也对");
  if (out) dsh_release(out);

  /* ★ 空白必须忽略：SSE 每行都带 `\r`，不忽略就会多出坏字节 */
  ok(dsh_base64_decode("YXBw\r\nbGU=\r\n", 12, &out, &len) == 0, "①★ 换行与回车要忽略");
  ok_eq_i64((int64_t)len, 5, "①★ 忽略空白之后长度仍然对（不是 7）");
  ok(out != NULL && memcmp(out, "apple", 5) == 0, "①★ 忽略空白之后内容也对");
  if (out) dsh_release(out);

  /* 空输入 = 解出**空**（不是失败、也不是 NULL）—— "空"与"坏了"是两件事 */
  ok(dsh_base64_decode("", 0, &out, &len) == 0, "① 空输入不算失败");
  ok(out != NULL, "① 空输入给一块非 NULL 的缓冲");
  ok_eq_i64((int64_t)len, 0, "① 空输入长度是 0");
  if (out) dsh_release(out);

  /* ★ 坏字符必须**如实失败**（不许跳过 —— 那会把坏音频交给播放器）*/
  ok(dsh_base64_decode("YXBw!GU=", 8, &out, &len) != 0, "①★ 坏字符要失败");
  ok_has(dsh_last_error_message(), "base64", "①★ 失败时说清是 base64 的问题");
  ok(dsh_base64_decode("YXB=bGU=", 8, &out, &len) != 0, "①★ 填充符后面还有数据 → 失败");
  ok(dsh_base64_decode("Y===", 4, &out, &len) != 0, "①★ 填充符太多 → 失败");
}

/* ══════════════════════════════════════════════════════════════════════════
   ② 音色与模型版本
   ══════════════════════════════════════════════════════════════════════════ */

static void test_resource(void) {
  ok_eq_str(dsh_doubao_resource_for("en_female_dacey_uranus_bigtts"), "seed-tts-2.0",
            "② 2.0 音色 → seed-tts-2.0");
  ok_eq_str(dsh_doubao_resource_for("zh_female_liuchangnv_uranus_bigtts"), "seed-tts-2.0",
            "② 中文 2.0 音色 → seed-tts-2.0");
  ok_eq_str(dsh_doubao_resource_for("zh_female_vv_mars_bigtts"), "seed-tts-1.0",
            "②★ `_mars_bigtts` → seed-tts-1.0（填错就是 40000001）");
  ok_eq_str(dsh_doubao_resource_for("some_moon_bigtts"), "seed-tts-1.0", "②★ `_moon_bigtts` → 1.0");
}

/* ══════════════════════════════════════════════════════════════════════════
   ③ 该用哪个音色—— 这条不是偏好，是"不照做就出不了声"
   ══════════════════════════════════════════════════════════════════════════ */

static void test_speaker(void) {
  const char *en = "en_female_dacey_uranus_bigtts";
  const char *zh = "zh_female_liuchangnv_uranus_bigtts";

  ok_eq_str(dsh_doubao_speaker_for(1, "en", en, zh), zh, "③★ 中英混排 → 中文音色");
  ok_eq_str(dsh_doubao_speaker_for(0, "zh", en, zh), zh, "③ 中文文本 → 中文音色");
  ok_eq_str(dsh_doubao_speaker_for(0, "en", en, zh), en, "③ 纯英文 → 英文音色");
  ok_eq_str(dsh_doubao_speaker_for(0, "ja", en, zh), en, "③ 日语（没配日语音色）→ 英文音色那一档");

  /* 中文音色空着 → 回落到英文音色（用户只配一个也能出声，功能不缺）*/
  ok_eq_str(dsh_doubao_speaker_for(1, "en", en, NULL), en, "③ 混排但没配中文音色 → 回落英文音色");
  ok_eq_str(dsh_doubao_speaker_for(1, "en", en, ""), en, "③ 混排但中文音色是空串 → 回落");
  /* 英文音色空着、文本也不是中文 → 用中文音色顶上 */
  ok_eq_str(dsh_doubao_speaker_for(0, "en", NULL, zh), zh, "③ 没配英文音色 → 用中文音色顶上");
  /* 两个都没配 → NULL（调用方如实报"没配音色"）*/
  ok(dsh_doubao_speaker_for(0, "en", NULL, NULL) == NULL, "③ 两个音色都没配 → NULL");
}

/* ══════════════════════════════════════════════════════════════════════════
   ③b 检测用的样本词（`dsh_speech_online_test_plan` 念的就是它）
   ══════════════════════════════════════════════════════════════════════════
   检查标准三条：**英中各一个固定的词**、**大小写 / 区域标记都认**、**表外回 "hello"**。
   为什么非要有这一组：这张表是**产品数据**（"检测凭据"那次请求念什么词），
   它只许有一个来源（内核）—— 被改成空串、或者英文那格悄悄变样，都得当场红。 */

static void test_sample_word(void) {
  ok_eq_str(dsh_doubao_sample_word("en"), "apple", "③b 英文的样本词是 apple");
  ok_eq_str(dsh_doubao_sample_word("zh"), "苹果", "③b 中文的样本词是 苹果");
  ok_eq_str(dsh_doubao_sample_word("ja"), "りんご", "③b 日文也有（表照抄参考实现）");
  ok_eq_str(dsh_doubao_sample_word("EN"), "apple", "③b 大小写不敏感");
  ok_eq_str(dsh_doubao_sample_word("zh-CN"), "苹果", "③b 区域标记也认（取主代码）");
  ok_eq_str(dsh_doubao_sample_word("xx"), "hello", "③b 表外的语种回 hello（参考实现的兜底）");
  ok_eq_str(dsh_doubao_sample_word(""), "hello", "③b 空语种也回 hello（**不是空串**）");
  ok_eq_str(dsh_doubao_sample_word(NULL), "hello", "③b NULL 也回 hello（不是 NULL）");
}

/* ══════════════════════════════════════════════════════════════════════════
   ④ 请求体
   ══════════════════════════════════════════════════════════════════════════ */

static void test_request_body(void) {
  char *body = dsh_doubao_request_body("apple", "en_female_dacey_uranus_bigtts", 0, -50);
  char *plain;

  ok(body != NULL, "④ 拼得出请求体");
  ok_has(body, "\"user\":{\"uid\":\"lookup\"}", "④ 带上 user.uid");
  ok_has(body, "\"req_params\":{", "④ 参数在 req_params 里");
  ok_has(body, "\"text\":\"apple\"", "④ 带上文本");
  ok_has(body, "\"speaker\":\"en_female_dacey_uranus_bigtts\"", "④ 带上音色");
  ok_has(body, "\"audio_params\":{", "④ 音频参数在 audio_params 里（的修正第 1 条）");
  ok_has(body, "\"format\":\"mp3\"", "④ 整文件用 mp3");
  ok_has(body, "\"sample_rate\":24000", "④ 采样率");
  ok_has(body, "\"speech_rate\":0", "④ 语速");
  /* ★ 这条是**刻意不写**的（见 .h 顶上那段第 ① 条）*/
  ok(strstr(body, "explicit_language") == NULL, "④★ 请求体里**不该有** explicit_language");
  ok(strstr(body, "\"additions\":\"{") != NULL, "④ additions 是一个 JSON **串**（不是对象）");
  /* 响度补偿（参考实现一直有，0.2.0 之前漏了 ——）*/
  ok_has(body, "\"loudness_rate\":-50", "④ 带上响度补偿（loudness_rate 在 audio_params 里）");
  dsh_release(body);

  /* 0 也**照样发**（与参考实现同约定）：少一个字段就少一条可核对的证据 */
  plain = dsh_doubao_request_body("apple", "x", 0, 0);
  ok(plain != NULL && strstr(plain, "\"loudness_rate\":0") != NULL,
     "④ 响度 0 也照发（不是省掉那个字段）");
  if (plain != NULL) dsh_release(plain);

  /* 响度**夹**到官方范围（与参考实现的 ClampLoudness 同约定；语速那条是"如实拒"，两者不同）*/
  plain = dsh_doubao_request_body("apple", "x", 0, 999);
  ok(plain != NULL && strstr(plain, "\"loudness_rate\":100") != NULL,
     "④ 响度 999 → 夹到 100（不是拒）");
  if (plain != NULL) dsh_release(plain);
  plain = dsh_doubao_request_body("apple", "x", 0, -999);
  ok(plain != NULL && strstr(plain, "\"loudness_rate\":-50") != NULL,
     "④ 响度 -999 → 夹到 -50（不是拒）");
  if (plain != NULL) dsh_release(plain);

  /* 语速越界**如实拒**，不悄悄夹 */
  ok(dsh_doubao_request_body("apple", "x", 200, 0) == NULL, "④ 语速 200 → 拒");
  ok(dsh_doubao_request_body("apple", "x", -60, 0) == NULL, "④ 语速 -60 → 拒");
  ok(dsh_doubao_request_body("apple", "x", 100, 0) != NULL, "④ 语速 100（上限）→ 可以");
  ok(dsh_doubao_request_body("apple", "x", -50, 0) != NULL, "④ 语速 -50（下限）→ 可以");
  ok(dsh_doubao_request_body("", "x", 0, 0) == NULL, "④ 空文本 → 拒");
  ok(dsh_doubao_request_body("apple", "", 0, 0) == NULL, "④ 空音色 → 拒");
}

/* ══════════════════════════════════════════════════════════════════════════
   ④b 响度补偿：设置优先 / 内置表 / 表外 0（约定逐条照参考实现的 LoudnessFor）
   ══════════════════════════════════════════════════════════════════════════ */

static void test_loudness(void) {
  const char *en = DSH_DOUBAO_DEFAULT_SPEAKER_EN;
  const char *zh = DSH_DOUBAO_DEFAULT_SPEAKER_ZH;

  /* ② 没设过 → 内置表：两个默认音色都是 -50 */
  ok(dsh_doubao_loudness_for(en, en, 0, 0, zh, 0, 0) == DSH_DOUBAO_BUILTIN_LOUDNESS,
     "④b ★ 默认英文音色（Dacey）没设过 → 内置 -50");
  ok(dsh_doubao_loudness_for(zh, en, 0, 0, zh, 0, 0) == DSH_DOUBAO_BUILTIN_LOUDNESS,
     "④b ★ 默认中文音色（Vivi）没设过 → 内置 -50");

  /* ① 设置里的滑块优先 —— 而且只对**它对应的那个音色**生效 */
  ok(dsh_doubao_loudness_for(en, en, 1, 20, zh, 0, 0) == 20,
     "④b ★ 英文那一格设了 20 → 用它（盖过内置的 -50）");
  ok(dsh_doubao_loudness_for(zh, en, 1, 20, zh, 0, 0) == DSH_DOUBAO_BUILTIN_LOUDNESS,
     "④b ★ 英文那一格的值**不会**串到中文音色上（Vivi 仍是内置 -50）");
  ok(dsh_doubao_loudness_for(zh, en, 0, 0, zh, 1, 35) == 35,
     "④b 中文那一格设了 35 → 用它");
  ok(dsh_doubao_loudness_for(en, en, 0, 0, zh, 1, 35) == DSH_DOUBAO_BUILTIN_LOUDNESS,
     "④b 中文那一格的值也不会串到英文音色上");
  /* 音色 id 是用户手打的 → 大小写不敏感 */
  ok(dsh_doubao_loudness_for("EN_FEMALE_DACEY_URANUS_BIGTTS", en, 1, -10, zh, 0, 0) == -10,
     "④b 音色 id 比较**大小写不敏感**（用户手打的那个）");
  /* 设置值越界 → 夹（不拒）*/
  ok(dsh_doubao_loudness_for(en, en, 1, 500, zh, 0, 0) == 100, "④b 设置 500 → 夹到 100");
  ok(dsh_doubao_loudness_for(en, en, 1, -500, zh, 0, 0) == -50, "④b 设置 -500 → 夹到 -50");

  /* ③ 表外的音色 = 0（不补偿）—— 让用户用滑块，而滑块由「平衡音量」实测写下来 */
  ok(dsh_doubao_loudness_for("zh_female_liuchangnv_uranus_bigtts", en, 0, 0, zh, 0, 0) == 0,
     "④b ★ 表外的音色（例如流畅女声）= 0，不补偿");
  ok(dsh_doubao_loudness_for("my-voice", "my-voice", 0, 0, zh, 0, 0) == 0,
     "④b 用户自己填的音色没配响度 → 0");
  ok(dsh_doubao_loudness_for("my-voice", "my-voice", 1, -30, zh, 0, 0) == -30,
     "④b 但用户给**自己那个音色**配了响度 → 用它（不是「表外一律 0」）");
  ok(dsh_doubao_loudness_for(NULL, en, 1, 50, zh, 1, 50) == 0, "④b 没有音色 → 0（不崩）");
  ok(dsh_doubao_loudness_for("", en, 1, 50, zh, 1, 50) == 0, "④b 空音色 → 0（不崩）");
}

/* ══════════════════════════════════════════════════════════════════════════
   ⑤ SSE 回包：正常的要能拼出音频，异常的要说人话
   ══════════════════════════════════════════════════════════════════════════ */

static void test_parse_ok(void) {
  /* 两段 base64（"app" 与 "le"）顺序拼成 "apple"，中间夹一行 `event:` */
  const char *sse =
      "event: 351\ndata: {\"code\":0,\"data\":\"YXBw\"}\n\n"
      "event: 352\ndata: {\"code\":0,\"data\":\"bGU=\"}\n\n"
      "event: 152\ndata: {\"code\":0,\"usage\":{\"text_words\":5}}\n\n";
  dsh_tts_result r;

  ok(dsh_doubao_parse(200, sse, strlen(sse), &r) == 0, "⑤ 解析返回 0");
  ok(r.ok == 1, "⑤★ 两段拼起来就是完整音频");
  ok_eq_i64((int64_t)r.audio_len, 5, "⑤ 音频长度对");
  ok(r.audio != NULL && memcmp(r.audio, "apple", 5) == 0, "⑤★ 段与段的**顺序**对（app + le）");
  ok_eq_i64(r.text_words, 5, "⑤ 计费字数（要点亮那个请求头才有）");
  ok_eq_str(r.mime, "audio/mpeg", "⑤ MIME 是 mp3");
  ok(r.reason == NULL, "⑤ 成功时没有人话原因");
  dsh_doubao_result_free(&r);

  /* 单个 data 行、`data:` 后面带空格（SSE 允许）*/
  {
    const char *one = "data: {\"code\":0,\"data\":\"YXBwbGU=\"}\n";
    ok(dsh_doubao_parse(200, one, strlen(one), &r) == 0, "⑤ 单行也解析得动");
    ok(r.ok == 1, "⑤ 单行拿到音频");
    ok_eq_i64((int64_t)r.audio_len, 5, "⑤ 单行长度对");
    dsh_doubao_result_free(&r);
  }
}

static void test_parse_failures(void) {
  dsh_tts_result r;

  /* ★ 实测那一档：code 0 + **零字节音频**（英文音色念中英混排）—— 必须判失败 */
  {
    const char *sse = "event: 351\nevent: 152\ndata: {\"code\":0}\n";
    ok(dsh_doubao_parse(200, sse, strlen(sse), &r) == 0, "⑤ 空音频也解析得动");
    ok(r.ok == 0, "⑤★ code 0 但**没有音频** → 不算成功");
    ok_has(r.reason, "中英混排", "⑤★ 那句话要点出最常见的原因（音色不匹配 / 混排）");
    dsh_doubao_result_free(&r);
  }

  /* 40000001 参数错误：人话要指向"音色与模型版本配套" */
  {
    const char *sse = "data: {\"code\":40000001,\"message\":\"bad speaker\"}\n";
    ok(dsh_doubao_parse(200, sse, strlen(sse), &r) == 0, "⑤ 参数错解析得动");
    ok(r.ok == 0, "⑤ 参数错不算成功");
    ok_eq_i64(r.code, 40000001, "⑤ 码原样带着");
    ok_has(r.reason, "配套", "⑤★ 参数错要指向音色与模型版本");
    dsh_doubao_result_free(&r);
  }

  /* 40300001 鉴权：**不许**说成"参数错误"（那会把用户指去改错的地方）*/
  {
    const char *sse = "data: {\"code\":40300001}\n";
    ok(dsh_doubao_parse(200, sse, strlen(sse), &r) == 0, "⑤ 鉴权错解析得动");
    ok_has(r.reason, "API Key", "⑤★ 鉴权错说的是 Key / 开通 / 实名");
    ok(strstr(r.reason, "音色") == NULL, "⑤★ 鉴权错**不许**说成音色参数问题");
    dsh_doubao_result_free(&r);
  }

  /* 陌生码：把码与服务端原话一起给（排错用）*/
  {
    const char *sse = "data: {\"code\":12345678,\"message\":\"who knows\"}\n";
    ok(dsh_doubao_parse(200, sse, strlen(sse), &r) == 0, "⑤ 陌生码解析得动");
    ok_has(r.reason, "12345678", "⑤ 陌生码带上码");
    ok_has(r.reason, "who knows", "⑤ 陌生码带上服务端原话");
    dsh_doubao_result_free(&r);
  }

  /* ★ 非 200 也算"答上来了"：403 要有专门那句话，而不是一句 HTTP 码 */
  {
    ok(dsh_doubao_parse(403, "", 0, &r) == 0, "⑤ 403 解析得动");
    ok(r.ok == 0, "⑤ 403 不算成功");
    ok_has(r.reason, "403", "⑤ 403 的说明里带上状态码");
    dsh_doubao_result_free(&r);
  }
  {
    ok(dsh_doubao_parse(0, "", 0, &r) == 0, "⑤ 没发出去那一档解析得动");
    ok_has(r.reason, "没发出去", "⑤★ 状态 0（断网/超时）与「服务端的错」分开说");
    dsh_doubao_result_free(&r);
  }

  /* 坏包：一行都不是认识的结构 → 不算成功 */
  {
    const char *junk = "this is not an sse stream at all\n";
    ok(dsh_doubao_parse(200, junk, strlen(junk), &r) == 0, "⑤ 坏包解析得动");
    ok(r.ok == 0, "⑤★ 坏包不算成功（不许把空音频当成功）");
    ok(r.reason != NULL, "⑤ 坏包有人话原因");
    dsh_doubao_result_free(&r);
  }

  /* base64 坏了：如实报，且**不返回半截音频** */
  {
    const char *sse = "data: {\"code\":0,\"data\":\"YXBw!GU=\"}\n";
    ok(dsh_doubao_parse(200, sse, strlen(sse), &r) == 0, "⑤ base64 坏了也解析得动");
    ok(r.ok == 0, "⑤★ base64 坏 → 不算成功");
    ok(r.audio == NULL, "⑤★ 坏包**不许**返回半截音频");
    dsh_doubao_result_free(&r);
  }
}

int main(void) {
  test_base64();
  test_resource();
  test_sample_word();
  test_speaker();
  test_request_body();
  test_loudness();
  test_parse_ok();
  test_parse_failures();

  printf("豆包语音：%d 项，失败 %d\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
