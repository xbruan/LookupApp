/* ==========================================================================
 * 内核单元测试 · 发音的键名层（audio/dsh_audio.c）与自带录音
 * （接口定义 `dsh_engine_speech_dict_audio`）
 *
 * 用例的素材是 `testdata/audio.mdx` + `testdata/audio.mdd`，各词目各钉一档：
 *   `beep`         词目发音英/美两条（`sound://` 与 `snd://` 两种写法）
 *   `example`      词目发音 + 一条 LDOCE5 风格的例句音频（`p001__`）
 *   `exampleonly`  **只有**例句录音，没有词目发音 ← 统一发音按钮必须落空
 *   `ghost`        词条里挂着录音，但资源卷里**没有**那个文件
 *   `nativeaudio<audio>` 标签里带 `src` 指向 `sound://…`（词典自己播的那种写法）
 *   `speex*`       Speex 那几条（键名层只看名字，解码在 `dsh_audio_prepare`）
 * ========================================================================== */

#include "audio/dsh_audio.h"
#include "audio/dsh_speex.h"
#include "dsh_lookup.h"
#include "crypto/dsh_sha256.h"
#include "dict/dsh_mdx.h"
#include "engine/dsh_engine_internal.h"
#include "engine/dsh_settings.h"
#include "mem_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DSH_TESTDATA_DIR
#error "需要 -DDSH_TESTDATA_DIR=<0.2.0/testdata 的路径>（见 Makefile）"
#endif

static int g_checks = 0;
static int g_failed = 0;

static void ok(int cond, const char *what) {
  g_checks++;
  if (!cond) {
    g_failed++;
    fprintf(stderr, "FAIL %s\n", what);
  }
}

static int has(const char *haystack, const char *needle) {
  return haystack != NULL && strstr(haystack, needle) != NULL;
}

/** 断言 + 失败时把那份 meta 打出来（这一组的出参是一小段 JSON，直接看最省事）*/
static void ok_meta(int cond, const char *meta, const char *what) {
  g_checks++;
  if (cond) return;
  g_failed++;
  fprintf(stderr, "FAIL %s\n      meta=%s\n", what, meta != NULL ? meta : "(null)");
}

/** 数值断言：失败时**把实际值与期望值都打出来**（不然红了一条还得自己再去量一遍）*/
static void ok_eq_i64(int64_t actual, int64_t expected, const char *what) {
  g_checks++;
  if (actual == expected) return;
  g_failed++;
  fprintf(stderr, "FAIL %s：实际=%lld 期望=%lld\n", what, (long long)actual,
          (long long)expected);
}

static void ok_eq_str(const char *actual, const char *expected, const char *what) {
  g_checks++;
  if (actual != NULL && expected != NULL && strcmp(actual, expected) == 0) return;
  g_failed++;
  fprintf(stderr, "FAIL %s\n      实际=%s\n      期望=%s\n", what, actual != NULL ? actual : "(null)",
          expected != NULL ? expected : "(null)");
}

/* WAV 头那几个字段（小端）：写成函数免得**下标写错** —— 写错下标得到的是一个
 * 看着合理的数，不是一次崩溃（`data` 长度在 40、采样率在 24）。 */
static uint32_t wav_u32(const uint8_t *b, size_t at) {
  return (uint32_t)b[at] | ((uint32_t)b[at + 1] << 8) | ((uint32_t)b[at + 2] << 16) |
         ((uint32_t)b[at + 3] << 24);
}

static uint16_t wav_u16(const uint8_t *b, size_t at) {
  return (uint16_t)((uint32_t)b[at] | ((uint32_t)b[at + 1] << 8));
}

static char *fixture_path(const char *name) {
  const size_t n = strlen(DSH_TESTDATA_DIR) + strlen(name) + 2;
  char *p = (char *)malloc(n);
  if (p != NULL) snprintf(p, n, "%s/%s", DSH_TESTDATA_DIR, name);
  return p;
}

static void free_keys(char **keys, int64_t count) { dsh_audio_keys_free(keys, count); }

int main(void) {
  const size_t base = dsh_mem_live_count();

  /* ══ ① 键名策略：候选扩展名的顺序与去重 ═══════════════════════════════ */
  {
    char **keys = NULL;
    int64_t n = 0;
    ok(dsh_audio_playable_extension_count() == 8, "① 能播的扩展名有 8 种");
    ok(strcmp(dsh_audio_playable_extension_at(0), ".mp3") == 0, "① 第一个是 .mp3（越常见越靠前）");
    ok(dsh_audio_playable_extension_at(8) == NULL, "① 越界返回 NULL");

    ok(dsh_audio_candidate_keys("beep__gb_1.spx", &keys, &n) == 0, "① 取候选键名");
    ok(n == 9, "① `.spx` 那把得到 8 个扩展名 + 原始键 = 9 条");
    ok(strcmp(keys[0], "beep__gb_1.mp3") == 0, "① 第一条是 .mp3");
    ok(strcmp(keys[n - 1], "beep__gb_1.spx") == 0,
       "① ★ 原始键**垫底**（词典里只有 .spx 时要能如实说「这段是 Speex」，而不是「找不到音频」）");
    free_keys(keys, n);

    keys = NULL;
    n = 0;
    ok(dsh_audio_candidate_keys("beep__gb_1.mp3", &keys, &n) == 0 && n == 8,
       "① 原始键已经能播时去重 → 8 条（不是 9 条）");
    free_keys(keys, n);

    keys = NULL;
    n = 0;
    ok(dsh_audio_candidate_keys("\\style/apple.spx", &keys, &n) == 0,
       "① 带目录与反斜杠的键名也认");
    ok(n > 0 && strcmp(keys[0], "style/apple.mp3") == 0,
       "① 前导斜杠去掉、目录保留、扩展名换掉");
    free_keys(keys, n);

    keys = NULL;
    n = 0;
    ok(dsh_audio_candidate_keys("", &keys, &n) == 0 && n == 0, "① 空键名 → 空清单");
    ok(dsh_audio_candidate_keys(NULL, &keys, &n) == 0 && n == 0, "① NULL → 空清单");
  }

  /* ══ ② 口音 / 例句 / 能播性 ═══════════════════════════════════════════ */
  {
    ok(strcmp(dsh_audio_classify_accent("beep__gb_1.wav"), "uk") == 0, "② gb → uk");
    ok(strcmp(dsh_audio_classify_accent("beep__us_1.wav"), "us") == 0, "② us → us");
    ok(strcmp(dsh_audio_classify_accent("apple__bre_2.mp3"), "uk") == 0, "② bre → uk");
    ok(strcmp(dsh_audio_classify_accent("apple__ams_2.mp3"), "us") == 0, "② ams → us");
    ok(dsh_audio_classify_accent("apple__1.mp3") == NULL, "② 认不出 → NULL");
    ok(dsh_audio_classify_accent("apple__plus_1.mp3") == NULL,
       "② ★ 按 token 比对：`plus` 里的 us **不算**美音（包含式判断会误伤）");
    ok(dsh_audio_classify_accent("busy.mp3") == NULL, "② `busy` 也不算美音");

    ok(strcmp(dsh_audio_accent_label("uk"), "英音") == 0, "② uk 的中文说法");
    ok(strcmp(dsh_audio_accent_label("us"), "美音") == 0, "② us 的中文说法");
    ok(strcmp(dsh_audio_accent_label(NULL), "发音") == 0, "② 认不出时说「发音」");

    ok(dsh_audio_is_example_key("p001__000123.wav") == 1, "② ★ LDOCE5 的例句键名（p001__）");
    ok(dsh_audio_is_example_key("apple__gbs_1.mp3") == 1, "② gbs = 英音例句");
    ok(dsh_audio_is_example_key("apple__uss_1.mp3") == 1, "② uss = 美音例句");
    ok(dsh_audio_is_example_key("apple__gb_1.wav") == 0, "② ★ `gb` 是**词目**发音，不是例句");
    ok(dsh_audio_is_example_key("ptest.wav") == 0, "② `ptest` 不是例句（p 后面得是数字）");

    ok(dsh_audio_looks_playable("a.mp3") == 1, "② .mp3 能播");
    ok(dsh_audio_looks_playable("a.spx") == 1, "② .spx 规划阶段算可用（点下去时才解码）");
    ok(dsh_audio_looks_playable("a.weird") == 1, "② 认不出的扩展名**不急着否定**（等拿到字节再判）");
    ok(dsh_audio_looks_playable("") == 0, "② 空键名 → 不可用");
    ok(dsh_audio_is_speex_name("a.spx") == 1 && dsh_audio_is_speex_name("a.SPX") == 1,
       "② 认得出 Speex（大小写不敏感）");
    ok(strcmp(dsh_audio_extension_of("dir.d/a.mp3"), ".mp3") == 0,
       "② 扩展名只在最后一段里找（`dir.d` 不算）");
  }

  /* ══ ③ 词条里抠音频引用（含口音与例句标记）═══════════════════════════ */
  {
    /*
     * ⚠️ 顺序是两趟：**先扫全部 scheme，再扫全部 `<audio>` 标签**。所以
     *    `<audio>` 里 `src` 指向 `sound://…` 的那条**不是**排在 `<audio name=…>` 后面，
     *    而是跟着 scheme 那一趟一起出来 —— 别按「文本里出现的先后」写期望。
     */
    const char *html =
        "<a href=\"sound://beep__gb_1.wav\">x</a>"
        "<a href=\"snd://beep__us_1.wav\">y</a>"
        "<audio name=\"p001__000123.wav\"></audio>"
        "<audio src=\"sound://native__gb_1.wav\" preload=\"none\"></audio>";
    dsh_audio_ref *refs = NULL;
    int64_t n = 0;
    ok(dsh_audio_extract(html, strlen(html), &refs, &n) == 0 && n == 4,
       "③ 抠出 4 条（3 条 scheme + 1 条 <audio name=>）");
    ok(n >= 1 && strcmp(refs[0].key, "beep__gb_1.wav") == 0 &&
           refs[0].accent != NULL && strcmp(refs[0].accent, "uk") == 0 && refs[0].example == 0,
       "③ 第一条：英音、词目发音");
    ok(n >= 2 && refs[1].accent != NULL && strcmp(refs[1].accent, "us") == 0,
       "③ 第二条：美音（顺序按出现）");
    ok(n >= 3 && strcmp(refs[3 - 1].key, "native__gb_1.wav") == 0,
       "③ 第三条来自 `<audio src=\"sound://…\">`（scheme 那一趟扫到的）");
    ok(n >= 4 && strcmp(refs[3].key, "p001__000123.wav") == 0 && refs[3].example == 1,
       "③ 第四条来自 `<audio name=…>`，而且是**例句**（`p001__`）");
    dsh_audio_refs_free(refs, n);
  }

  /* ══ ④ 挑哪一条念：例句不算词目发音 ═══════════════════════════════════ */
  {
    dsh_audio_candidate list[4];
    memset(list, 0, sizeof(list));
    /* 只有例句 → 挑不出来（这是那条硬约定）*/
    list[0].key = "p001__000123.wav";
    list[0].example = 1;
    list[0].matched_key = "\\p001__000123.wav";
    ok(dsh_audio_pick(list, 1, NULL) == -1, "④ ★ 只有例句录音 → 挑不出来（统一按钮必须落空）");

    /* 英音在前、美音在后；偏好 auto → 取先出现的那条 */
    memset(list, 0, sizeof(list));
    list[0].key = "beep__gb_1.wav";
    list[0].accent = "uk";
    list[0].matched_key = "\\beep__gb_1.wav";
    list[1].key = "beep__us_1.wav";
    list[1].accent = "us";
    list[1].matched_key = "\\beep__us_1.wav";
    ok(dsh_audio_pick(list, 2, NULL) == 0, "④ 偏好 auto → 取词条里先出现的那条");
    ok(dsh_audio_pick(list, 2, "us") == 1, "④ ★ 偏好美音 → 取美音那条");
    ok(dsh_audio_pick(list, 2, "uk") == 0, "④ 偏好英音 → 取英音那条");
    ok(dsh_audio_pick(list, 2, "auto") == 0, "④ 偏好写 auto → 等同于没偏好");

    /* 偏好美音但只有英音 → 回落到第一条（不许因为「没有偏好那种」就落空）*/
    memset(list, 0, sizeof(list));
    list[0].key = "beep__gb_1.wav";
    list[0].accent = "uk";
    list[0].matched_key = "\\beep__gb_1.wav";
    ok(dsh_audio_pick(list, 1, "us") == 0, "④ 偏好那种口音没有 → 回落到第一条");

    /* 对不上文件的（matched_key = NULL）不参与 */
    memset(list, 0, sizeof(list));
    list[0].key = "ghost__gb_1.mp3";
    list[0].accent = "uk";
    list[0].matched_key = NULL;
    list[1].key = "beep__us_1.wav";
    list[1].accent = "us";
    list[1].matched_key = "\\beep__us_1.wav";
    ok(dsh_audio_pick(list, 2, NULL) == 1, "④ ★ 资源卷里没有的那条不参与挑选");
    ok(dsh_audio_pick(NULL, 0, NULL) == -1, "④ 空清单 → -1");
  }

  /* ══ ⑤ 接口定义：这条词条自带的原录音在哪（真测试用词典）══════════════════ */
  {
    const char *names[] = {"audio.mdx", "test.mdx"};
    char *ids[4] = {(char[65]){0}, (char[65]){0}, NULL, NULL};
    dsh_engine *e = NULL;
    ok(dsh_engine_create(NULL, &e) == DSH_OK && e != NULL, "⑤ 建引擎");
    if (e != NULL) {
      char paths[4096];
      char *p1 = fixture_path(names[0]);
      char *p2 = fixture_path(names[1]);
      snprintf(paths, sizeof(paths), "[\"%s\",\"%s\"]", p1 != NULL ? p1 : "",
               p2 != NULL ? p2 : "");
      if (p1 != NULL) free(p1);
      if (p2 != NULL) free(p2);
      char *out = NULL;
      ok(dsh_engine_dict_add(e, paths, &out) == DSH_OK, "⑤ 加 audio.mdx 与 test.mdx");
      if (out != NULL) dsh_release(out);
      char *list = NULL;
      if (dsh_engine_dict_list(e, &list) == DSH_OK && list != NULL) {
        const char *q = list;
        for (int idx = 0; idx < 2; idx++) {
          q = strstr(q, "\"id\":\"");
          if (q == NULL) break;
          q += 6;
          size_t k = 0;
          while (q[k] != '\0' && q[k] != '"' && k < 64) {
            ids[idx][k] = q[k];
            k++;
          }
          ids[idx][k] = '\0';
        }
        dsh_release(list);
      }
      ok(ids[0][0] != '\0' && ids[1][0] != '\0', "⑤ 取到两本的 id");

      char *json = NULL;

      /* ① beep：英/美两条词目发音（外加一条 <audio name=…> 指向同一个文件）*/
      ok(dsh_speech_dict_audio(e, ids[0], "beep", &json) == DSH_OK, "⑤ beep 取自带录音");
      ok(has(json, "\"found\":true"), "⑤ beep 有自带录音");
      ok(has(json, "\"kind\":\"entry\""), "⑤ 而且算**词目**发音");
      ok(has(json, "\"accent\":\"uk\""), "⑤ 挑中的是英音（词条里先出现的词目发音）");
      ok(has(json, "beep__gb_1.wav"), "⑤ 键名就是资源卷里那个（带前导反斜杠的写法）");
      if (json != NULL) dsh_release(json);

      /*
       * ⚠️ ①b 口音偏好（`speech.accent`）**真的被读了**：钉的是**同一条词条在两种偏好下
       *    挑出不同的那一条**，而不是「设置里存住了」—— 后者是个会跟着错误一起绿的
       *    替代指标（偏好没传到那条路上时，界面改了也毫无反应）。
       */
      {
        char *patched = NULL;
        ok(dsh_engine_settings_set(e, "{\"speech\":{\"accent\":\"us\"}}", &patched) == DSH_OK,
           "⑤ 把口音偏好改成美音");
        if (patched != NULL) dsh_release(patched);
        json = NULL;
        ok(dsh_speech_dict_audio(e, ids[0], "beep", &json) == DSH_OK, "⑤ 改偏好后再取一次");
        ok(has(json, "\"accent\":\"us\""),
           "⑤ ★ 偏好改成 us 之后，挑中的就是**美音**那一条（不再是先出现的英音）");
        if (json != NULL) dsh_release(json);

        patched = NULL;
        ok(dsh_engine_settings_set(e, "{\"speech\":{\"accent\":\"auto\"}}", &patched) == DSH_OK,
           "⑤ 再改回 auto");
        if (patched != NULL) dsh_release(patched);
        json = NULL;
        ok(dsh_speech_dict_audio(e, ids[0], "beep", &json) == DSH_OK, "⑤ auto 再取一次");
        ok(has(json, "\"accent\":\"uk\""),
           "⑤ ★ auto = 用先出现的那条（英音）—— 与刚才的 us 显然不同");
        if (json != NULL) dsh_release(json);
      }

      /* ② example：词目发音与例句各一条 —— 必须挑词目那条 */
      json = NULL;
      ok(dsh_speech_dict_audio(e, ids[0], "example", &json) == DSH_OK,
         "⑤ example 取自带录音");
      ok(has(json, "\"kind\":\"entry\"") && has(json, "example__gb_1.wav"),
         "⑤ ★ 词目发音与例句都有时，挑的是**词目**那条（例句不算词目发音）");
      if (json != NULL) dsh_release(json);

      /* ③ exampleonly：只有例句 → 如实说「只有例句」，不是「没有录音」 */
      json = NULL;
      ok(dsh_speech_dict_audio(e, ids[0], "exampleonly", &json) == DSH_OK,
         "⑤ exampleonly 取自带录音");
      ok(has(json, "\"found\":false"), "⑤ ★ 只有例句 → found 为假（统一按钮必须落空）");
      ok(has(json, "例句"), "⑤ 而且如实说这是例句录音");
      if (json != NULL) dsh_release(json);

      /* ④ ghost：词条里挂着、资源卷里没有 → 两件事要分开说 */
      json = NULL;
      ok(dsh_speech_dict_audio(e, ids[0], "ghost", &json) == DSH_OK, "⑤ ghost 取自带录音");
      ok(has(json, "\"found\":false"), "⑤ ghost 没有可播的");
      ok(has(json, "资源卷"), "⑤ ★ 理由是「资源卷里没有这些文件」（不是「本来没有发音」）");
      if (json != NULL) dsh_release(json);

      /* ⑤ test.mdx 的词条（那份没有音频卷）→ 说「没有自带录音」 */
      json = NULL;
      ok(dsh_speech_dict_audio(e, ids[1], "apple", &json) == DSH_OK,
         "⑤ test.mdx 的 apple 取自带录音");
      ok(has(json, "\"found\":false") && has(json, "没有自带录音"),
         "⑤ 没有音频卷的词典如实说「本词条没有自带录音」");
      if (json != NULL) dsh_release(json);

      /* ⑥ 不存在的词条 / 参数边界 */
      json = NULL;
      ok(dsh_speech_dict_audio(e, ids[0], "zzz-no-such", &json) == DSH_OK,
         "⑤ 不存在的词条也成功返回");
      ok(has(json, "没有这条词条"), "⑤ 而且与「有词条但没录音」分开说");
      if (json != NULL) dsh_release(json);

      ok(dsh_speech_dict_audio(e, ids[0], "beep", NULL) != DSH_OK, "⑤ out 为 NULL 必须失败");
      ok(dsh_speech_dict_audio(e, NULL, "beep", &json) != DSH_OK, "⑤ dict_id 为空必须失败");
      ok(dsh_speech_dict_audio(e, ids[0], NULL, &json) != DSH_OK, "⑤ key_text 为空必须失败");
      ok(dsh_speech_dict_audio(NULL, ids[0], "beep", &json) != DSH_OK,
         "⑤ 引擎为 NULL 必须失败");
      ok(dsh_speech_dict_audio(e, "no-such-id", "beep", &json) != DSH_OK,
         "⑤ 词库里没有这本 → 失败（不是「没录音」）");

      dsh_engine_destroy(e);
    }
  }

  /* ══ ⑥ 接口定义：把一段音频字节整成能播的东西（`dsh_audio_prepare`）═══════
   * 检查标准两层：魔数嗅探的几条分支（mp3 / wav / ogg / flac / m4a / 认不出），
   * 以及 `audio.mdd` 里那几条真素材（wav 原样发、Speex 如实说解不了）。 */
  {
    struct {
      const char *label;
      const uint8_t *bytes;
      size_t len;
      const char *want_kind;
      int want_playable;
    } cases[8];
    static const uint8_t MP3_ID3[] = {0x49, 0x44, 0x33, 0x03, 0, 0, 0, 0, 0, 0, 0, 0,
                                      0x41, 0x42, 0x43, 0x44};
    static const uint8_t MP3_SYNC[] = {0xFF, 0xFB, 0x90, 0x64, 0, 0, 0, 0, 0, 0, 0, 0,
                                       0x41, 0x42};
    static const uint8_t WAV[] = {0x52, 0x49, 0x46, 0x46, 0x24, 0x00, 0x00, 0x00,
                                  0x57, 0x41, 0x56, 0x45, 0x66, 0x6D, 0x74, 0x20};
    static const uint8_t OGG_VORBIS[] = {0x4F, 0x67, 0x67, 0x53, 0, 2, 0, 0, 0, 0, 0, 0,
                                         0x01, 0x1E, 0x01, 0x76};
    static const uint8_t OGG_SPEEX[] = {0x4F, 0x67, 0x67, 0x53, 0, 2, 0, 0, 0, 0, 0, 0,
                                        0x53, 0x70, 0x65, 0x65, 0x78, 0x20, 0x20, 0x20};
    static const uint8_t FLAC[] = {0x66, 0x4C, 0x61, 0x43, 0, 0, 0, 0x22, 0, 0, 0, 0};
    static const uint8_t M4A[] = {0, 0, 0, 0x20, 0x66, 0x74, 0x79, 0x70, 0x4D, 0x34, 0x41, 0x20};
    static const uint8_t JUNK[] = {0x7B, 0x22, 0x65, 0x72, 0x72, 0x6F, 0x72, 0x22, 0x3A,
                                   0x31, 0x7D, 0x00, 0x41, 0x42};
    int n = 0;
    cases[n].label = "mp3（ID3 开头）";
    cases[n].bytes = MP3_ID3;
    cases[n].len = sizeof(MP3_ID3);
    cases[n].want_kind = "mp3";
    cases[n].want_playable = 1;
    n++;
    cases[n].label = "mp3（帧同步 FF Ex）";
    cases[n].bytes = MP3_SYNC;
    cases[n].len = sizeof(MP3_SYNC);
    cases[n].want_kind = "mp3";
    cases[n].want_playable = 1;
    n++;
    cases[n].label = "wav（RIFF….WAVE）";
    cases[n].bytes = WAV;
    cases[n].len = sizeof(WAV);
    cases[n].want_kind = "wav";
    cases[n].want_playable = 1;
    n++;
    cases[n].label = "ogg（vorbis）";
    cases[n].bytes = OGG_VORBIS;
    cases[n].len = sizeof(OGG_VORBIS);
    cases[n].want_kind = "";
    cases[n].want_playable = 1;
    n++;
    cases[n].label = "flac";
    cases[n].bytes = FLAC;
    cases[n].len = sizeof(FLAC);
    cases[n].want_kind = "";
    cases[n].want_playable = 1;
    n++;
    cases[n].label = "m4a（ftyp）";
    cases[n].bytes = M4A;
    cases[n].len = sizeof(M4A);
    cases[n].want_kind = "";
    cases[n].want_playable = 1;
    n++;
    cases[n].label = "Speex（OggS + Speex 标记）";
    cases[n].bytes = OGG_SPEEX;
    cases[n].len = sizeof(OGG_SPEEX);
    cases[n].want_kind = "spx";
    cases[n].want_playable = 0;
    n++;
    cases[n].label = "认不出的字节（一段 JSON 错误页）";
    cases[n].bytes = JUNK;
    cases[n].len = sizeof(JUNK);
    cases[n].want_kind = "";
    cases[n].want_playable = 0;
    n++;

    for (int i = 0; i < n; i++) {
      uint8_t *out = NULL;
      size_t out_len = 0;
      char *meta = NULL;
      char label[192];
      const enum dsh_error rc =
          dsh_audio_prepare(cases[i].bytes, cases[i].len, &out, &out_len, &meta);
      snprintf(label, sizeof(label), "⑥ %s：准备成功", cases[i].label);
      ok(rc == DSH_OK && meta != NULL, label);
      {
        char want[64];
        snprintf(want, sizeof(want), "\"kind\":\"%s\"", cases[i].want_kind);
        snprintf(label, sizeof(label), "⑥ %s：kind=%s", cases[i].label,
                 cases[i].want_kind[0] ? cases[i].want_kind : "(空)");
        ok(has(meta, want), label);
      }
      snprintf(label, sizeof(label), "⑥ %s：%s", cases[i].label,
               cases[i].want_playable ? "能直接播（原样发出去）" : "如实说不可播");
      ok(has(meta, cases[i].want_playable ? "\"playable\":true" : "\"playable\":false"), label);
      if (cases[i].want_playable) {
        snprintf(label, sizeof(label), "⑥ %s：字节原样带回来", cases[i].label);
        ok(out != NULL && out_len == cases[i].len &&
               memcmp(out, cases[i].bytes, out_len) == 0,
           label);
        snprintf(label, sizeof(label), "⑥ %s：能播的那一档 reason 是空的", cases[i].label);
        ok(has(meta, "\"reason\":\"\""), label);
      } else {
        snprintf(label, sizeof(label), "⑥ %s：不可播时必须给一句人话", cases[i].label);
        ok(out == NULL && out_len == 0 && !has(meta, "\"reason\":\"\""), label);
      }
      if (meta != NULL) dsh_release(meta);
      if (out != NULL) dsh_release(out);
    }

    /* Speex 那一档的话要说得具体（不是「格式不对」，而是「这一版还没接解码器」）*/
    {
      uint8_t *out = NULL;
      size_t out_len = 0;
      char *meta = NULL;
      ok(dsh_audio_prepare(OGG_SPEEX, sizeof(OGG_SPEEX), &out, &out_len, &meta) == DSH_OK,
         "⑥ Speex 那一档也成功返回（错误写在 reason 里，不是崩）");
      ok(has(meta, "Speex"), "⑥ ★ reason 里点名是 Speex（宿主据此说清为什么没声音）");
      ok(has(meta, "\"mime\":\"audio/x-speex\""), "⑥ Speex 的 MIME 是 audio/x-speex");
      if (meta != NULL) dsh_release(meta);
    }

    /* 空字节：如实说「内容是空的」，不是不报错地成功 */
    {
      uint8_t *out = NULL;
      size_t out_len = 0;
      char *meta = NULL;
      ok(dsh_audio_prepare(NULL, 0, &out, &out_len, &meta) == DSH_OK && meta != NULL,
         "⑥ 空字节也成功返回");
      ok_meta(has(meta, "空的") && has(meta, "\"playable\":false"), meta,
              "⑥ ★ 空内容如实说「音频内容是空的」");
      if (meta != NULL) dsh_release(meta);
    }

    /* 参数边界 */
    {
      uint8_t *out = NULL;
      size_t out_len = 0;
      char *meta = NULL;
      ok(dsh_audio_prepare(MP3_ID3, sizeof(MP3_ID3), NULL, &out_len, &meta) != DSH_OK,
         "⑥ out_bytes 为 NULL 必须失败");
      ok(dsh_audio_prepare(MP3_ID3, sizeof(MP3_ID3), &out, NULL, &meta) != DSH_OK,
         "⑥ out_len 为 NULL 必须失败");
      ok(dsh_audio_prepare(MP3_ID3, sizeof(MP3_ID3), &out, &out_len, NULL) != DSH_OK,
         "⑥ out_meta_json 为 NULL 必须失败");
    }

    /* 真测试用词典：`audio.mdd` 里的 wav 与 Speex */
    {
      char *mdd_path = fixture_path("audio.mdd");
      dsh_mdx *mdd = NULL;
      if (mdd_path != NULL && dsh_mdx_open(mdd_path, &mdd) == 0 && mdd != NULL) {
        const char *keys[] = {"\\beep__gb_1.wav", "\\speexword__gb_1.spx"};
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
          char *landed = NULL;
          uint8_t *body = NULL;
          size_t body_len = 0;
          /* ⚠️ 必须用 **`dsh_mdx_fetch_raw`**（原始字节）：`.mdd` 里的资源是二进制，
           *    走文本那条（`dsh_mdx_fetch`）会被 UTF-16LE 解码器毁掉。 */
          if (dsh_mdx_fetch_raw(mdd, keys[i], &landed, &body, &body_len) == 1 && body != NULL) {
            uint8_t *out = NULL;
            size_t out_len = 0;
            char *meta = NULL;
            ok(dsh_audio_prepare(body, body_len, &out, &out_len, &meta) == DSH_OK,
               "⑥ 真测试用词典里的那条音频能准备");
            if (i == 0) {
              ok_meta(has(meta, "\"kind\":\"wav\"") && has(meta, "\"playable\":true"), meta,
                      "⑥ ★ `.mdd` 里的 wav 认得出、能直接播");
              ok(out != NULL && out_len == body_len, "⑥ 而且字节一字不差地交回去");
              ok(out != NULL && out_len == body_len && memcmp(out, body, body_len) == 0,
                 "⑥ ★ 原样字节与 `.mdd` 里那条记录逐字节相同（没被解码器动过）");
            } else {
              /*
               * `.mdd` 里那条是**真的 Ogg Speex**，必须真的解出来：`decoded = true`、
               * `mime = audio/wav`，交出去的字节是**真正的 WAV**。
               * ⚠️ `kind` 仍是 `spx` —— 那是**源编码**，不是「发出去的东西」（由 `mime` 说）。
               */
              ok_meta(has(meta, "\"kind\":\"spx\"") && has(meta, "\"playable\":true") &&
                          has(meta, "\"decoded\":true") && has(meta, "\"mime\":\"audio/wav\""),
                      meta, "⑥ ★ `.mdd` 里的真 Speex **解出来了**，而且按 WAV 发出去");
              ok_meta(has(meta, "\"reason\":\"\""), meta, "⑥ 解开了就没有理由那一句");
              ok(out != NULL && out_len > 44 && memcmp(out, "RIFF", 4) == 0 &&
                     memcmp(out + 8, "WAVE", 4) == 0,
                 "⑥ ★ 交出去的字节是**真正的 WAV**（RIFF/WAVE 头）");
              ok(out_len != body_len, "⑥ ★ 而且它与 `.mdd` 里的源字节**不是同一份**（是真解出来的）");
            }
            if (meta != NULL) dsh_release(meta);
            if (out != NULL) dsh_release(out);
          } else {
            ok(0, "⑥ 从 audio.mdd 里取那条音频（原始字节）");
          }
          if (landed != NULL) dsh_release(landed);
          if (body != NULL) dsh_release(body);
        }
        dsh_mdx_close(mdd);
      } else {
        ok(0, "⑥ 打开 audio.mdd");
      }
      if (mdd_path != NULL) free(mdd_path);
    }
  }

  /* ══ ⑦ 音频键的**扩展名变体** ══════════════════════════════════════════
   *
   * 病根：词典把音频链接写成 `.spx`、而音频卷里其实是 `.wav` / `.mp3`（牛津高阶就
   * 是这样），所以音频键得跨扩展名去找 —— 否则那种链接只会得到「这本词典里没有
   * 这个资源」，而录音其实就在卷里。
   * ⚠️ **只对看起来是音频的键**做（`dsh_audio_is_audio_name`）：否则请求 `theme.css`
   *    而卷里只有 `theme.wav` 时会取到错的东西。两条检查标准一正一反，缺一不可。
   */
  {
    char *mdx_path = fixture_path("audio.mdx");
    char *mdd_path = fixture_path("audio.mdd");
    if (mdx_path != NULL && mdd_path != NULL) {
      dsh_engine *e = NULL;
      if (dsh_engine_create(NULL, &e) == DSH_OK && e != NULL) {
        char id_buf[65] = {0};
        const char *ids[1] = {id_buf};
        char paths[1200];
        snprintf(paths, sizeof(paths), "[\"%s\"]", mdx_path);
        /* 测试用词典的 id 是内容哈希，得先加进词库再取回来（与其他测试同一条路子）*/
        char *list = NULL;
        if (dsh_engine_dict_add(e, paths, &list) == DSH_OK) {
          if (list != NULL) dsh_release(list);
          char *dicts = NULL;
          if (dsh_engine_dict_list(e, &dicts) == DSH_OK && dicts != NULL) {
            const char *at = strstr(dicts, "\"id\":\"");
            if (at != NULL) {
              at += 6;
              size_t k = 0;
              /* ⚠️ 上界要用**数组自己的大小**：`sizeof(ids[0])` 在一个指针数组里
               *    是**指针的大小**（8）—— id 会被截成 7 个字符，后面每一条都报
               *    `DSH_E_NOT_FOUND`（看起来像内核找不到资源）。 */
              while (at[k] != '\0' && at[k] != '"' && k + 1 < sizeof(id_buf)) {
                id_buf[k] = at[k];
                k++;
              }
              id_buf[k] = '\0';
            }
            dsh_release(dicts);
          }
          if (id_buf[0] != '\0') {
            /* ① 链接写 `.spx`、卷里其实是 `.wav` —— 必须找到，而且拿到的是那段 wav */
            {
              uint8_t *bytes = NULL;
              size_t len = 0;
              char *meta = NULL;
              const enum dsh_error rc = dsh_engine_resource(e, ids[0], "spxlink__gb_1.spx",
                                                            (size_t)0, (size_t)0, &bytes, &len, &meta);
              /* 命中的到底是**卷里那一个**（etag 里带着真键名，一眼看得出）*/
              fprintf(stderr, "  [实测结果] \"spxlink__gb_1.spx\" → %zu 字节，etag=%s\n", len,
                      meta != NULL ? meta : "(null)");
              ok(rc == DSH_OK && bytes != NULL && len > 0,
                 "⑦ ★ 链接写 `.spx`、卷里是 `.wav` 时**也能取到**（扩展名变体那一轮）");
              ok(bytes != NULL && len >= 4 && memcmp(bytes, "RIFF", 4) == 0,
                 "⑦ ★ 而且取到的确实是那段 wav（头四字节是 RIFF）");
              if (meta != NULL && has(meta, "\"found\":true")) {
                /* 元信息也得跟着说「找到了」（界面据此决定放不放）*/
              } else {
                ok(0, "⑦ 元信息里 found 应当是 true");
              }
              if (bytes != NULL) dsh_release(bytes);
              if (meta != NULL) dsh_release(meta);
            }
            /* ② 反例：**不是音频**的键不许跨扩展名去找 —— 否则会取到错的东西 */
            {
              uint8_t *bytes = NULL;
              size_t len = 0;
              char *meta = NULL;
              const enum dsh_error rc = dsh_engine_resource(e, ids[0], "beep__gb_1.css",
                                                            (size_t)0, (size_t)0, &bytes, &len, &meta);
              ok(rc == DSH_OK && bytes == NULL && has(meta, "\"found\":false"),
                 "⑦ ★ 不是音频的键不许跨扩展名命中（`beep__gb_1.css` 不该取到那段 wav）");
              if (bytes != NULL) dsh_release(bytes);
              if (meta != NULL) dsh_release(meta);
            }
            /* ③ 判定表本身（纯函数，单独钉） */
            ok(dsh_audio_is_audio_name("a.spx") && dsh_audio_is_audio_name("a.wav") &&
               dsh_audio_is_audio_name("A.MP3") && dsh_audio_is_audio_name("\\a.ogg"),
               "⑦ `is_audio_name`：`.spx` / `.wav` / 大写 / 带前缀都算音频");
            ok(!dsh_audio_is_audio_name("a.css") && !dsh_audio_is_audio_name("a.png") &&
               !dsh_audio_is_audio_name("noext") && !dsh_audio_is_audio_name("dir.d/file") &&
               !dsh_audio_is_audio_name(NULL),
               "⑦ `is_audio_name`：css / png / 没有扩展名 / 目录里的点 / NULL 都不算");
          } else {
            ok(0, "⑦ 取不到测试用词典的词典 id");
          }
        } else {
          ok(0, "⑦ 往引擎里加测试用词典失败");
        }
        dsh_engine_destroy(e);
      } else {
        ok(0, "⑦ 建引擎");
      }
    }
    if (mdx_path != NULL) free(mdx_path);
    if (mdd_path != NULL) free(mdd_path);
  }

  /* ══ ⑧ Speex 解码：三层**互相独立**的检查标准 ═══════════════════════════
   *
   * 不能只钉一个哈希：哈希只回答「变了没有」，回答不了「对不对」；而解码有外层
   * 容器与内层 CELP 两半，写错任一半都能出声，只是声不对。三层各自独立可判：
   *   ① 结构性事实：采样数 == 包数 × 每帧采样数、采样率与 WAV 头自洽；
   *   ② **播放速率听头里的**：同一段码流换头里的 `rate` 时，PCM 必须**逐样本相同**、
   *      只有 WAV 头里的采样率不同（按模式速率播会让整本 LDOCE5 偏低偏慢）；
   *   ③ 冻结的 PCM 哈希（本仓库 vendor 的官方 libspeex 1.2.1 浮点配置解出的那段
   *      向量）：换解码器版本 / 换开关 / 改帧循环，它都会当场变红。
   */
  {
    char *mdd_path = fixture_path("audio.mdd");
    dsh_mdx *mdd = NULL;
    if (mdd_path != NULL && dsh_mdx_open(mdd_path, &mdd) == 0 && mdd != NULL) {
      static const char *SPX_WORD = "\\speexword__gb_1.spx";
      static const char *SPX_RATE = "\\speexrate__gb_1.spx";
      static const char *SPX_BROKEN = "\\speexbroken__gb_1.spx";
      /* 官方 libspeex（浮点）解出来的那段 PCM 的 SHA-256 */
      static const char *WANT_PCM_SHA =
          "fc7d90b1d818b1567cd71878f400ceda3847e8dfcb5285ab1a2af38b16b76344";
      uint8_t *word_wav = NULL;
      size_t word_len = 0;
      char word_sha[65];

      /* 取三段真测试用词典的字节（`.mdd` 里的资源是二进制的，必须走 `fetch_raw`）*/
      uint8_t *word_src = NULL, *rate_src = NULL, *broken_src = NULL;
      size_t word_src_len = 0, rate_src_len = 0, broken_src_len = 0;
      int got_word = 0, got_rate = 0, got_broken = 0;
      {
        char *landed = NULL;
        got_word = (dsh_mdx_fetch_raw(mdd, SPX_WORD, &landed, &word_src, &word_src_len) == 1);
        if (landed != NULL) dsh_release(landed);
        landed = NULL;
        got_rate = (dsh_mdx_fetch_raw(mdd, SPX_RATE, &landed, &rate_src, &rate_src_len) == 1);
        if (landed != NULL) dsh_release(landed);
        landed = NULL;
        got_broken =
            (dsh_mdx_fetch_raw(mdd, SPX_BROKEN, &landed, &broken_src, &broken_src_len) == 1);
        if (landed != NULL) dsh_release(landed);
      }
      ok(got_word && got_rate && got_broken,
         "⑧ ★ 三段 Speex 测试用词典都取到了（真的那段 / 头里写 22050 的那段 / 坏的那段）");

      /* ── ① 结构性事实 ───────────────────────────────────────────────── */
      if (got_word) {
        uint8_t *out = NULL;
        size_t out_len = 0;
        char *meta = NULL;
        char reason[512];
        dsh_speex_info info;
        ok(dsh_speex_probe(word_src, word_src_len, &info, reason, sizeof(reason)) == DSH_SPEEX_OK,
           "⑧ 诊断脚本：这段 Speex 的容器与头都读得出来");
        ok_eq_i64(info.mode, 1, "⑧ ★ 模式是 1（宽带）—— 与 参考实现的 `SpeexProbe info` 结果一致");
        ok_eq_i64(info.rate, 16000, "⑧ ★ 播放速率 16000 = 宽带模式的速率（也是头里写的那个）");
        ok_eq_i64(info.mode_rate, 16000, "⑧ 模式速率 16000");
        ok_eq_i64(info.header_frame_size, 320,
                  "⑧ ★ 头里的 frame_size = 320（宽带 20 ms）—— 播放速率就是按它这个模式定的");
        ok_eq_i64(dsh_speex_mode_rate(0), 8000, "⑧ 窄带模式 → 8000");
        ok_eq_i64(dsh_speex_mode_rate(2), 32000, "⑧ ★ 超宽带模式 → 32000（别漏了这一档）");
        ok_eq_i64(info.header_rate, 16000, "⑧ 头里的 rate 字段 = 16000");
        ok_eq_i64(info.channels, 1, "⑧ 单声道");
        ok(info.audio_packet_count == 100, "⑧ ★ 音频包 100 个（与 参考实现诊断脚本一致）");
        ok_eq_str(info.version, "1.2rc1", "⑧ ★ 版本串 = 1.2rc1（与参考实现一致）");

        ok(dsh_audio_prepare(word_src, word_src_len, &out, &out_len, &meta) == DSH_OK,
           "⑧ 经 `dsh_audio_prepare` 解出 WAV");
        ok_meta(has(meta, "\"samples\":32000") && has(meta, "\"sampleRate\":16000"), meta,
                "⑧ ★★ 采样数 32000 = 100 包 × 320 采样（**一帧都不多、一帧都不少**）");
        {
          /* WAV 头逐字段核（这些是与解码器实现**无关**的事实）*/
          const int64_t derived_samples = (int64_t)((out_len - 44) / 2);
          ok_eq_i64(derived_samples, 32000, "⑧ ★ PCM 长度也对得上 32000 个 16bit 采样");
          ok(out_len == 44 + 32000 * 2, "⑧ ★ 整个 WAV 正好是 44 + 64000 字节");
          ok(wav_u32(out, 24) == 16000, "⑧ ★ WAV 头里写的采样率是 16000");
          ok(wav_u16(out, 22) == 1, "⑧ WAV 头里写的声道数是 1");
          ok(wav_u16(out, 34) == 16, "⑧ WAV 头里写的位深是 16");
          ok(wav_u32(out, 40) == (uint32_t)(out_len - 44), "⑧ WAV 头里的 data 长度与真实长度一致");
          ok(wav_u32(out, 4) == (uint32_t)(out_len - 8), "⑧ WAV 头里的 RIFF 长度也自洽");
        }
        /* ── ③ 冻结的 PCM 哈希 ─────────────────────────────────────────── */
        dsh_sha256_hex(out + 44, out_len - 44, word_sha);
        {
          char label[256];
          snprintf(label, sizeof(label),
                   "⑧ ★★ PCM 与**官方 libspeex 1.2.1（浮点）**解出来的那段逐字节相同"
                   "（实际 %s）", word_sha);
          ok(strcmp(word_sha, WANT_PCM_SHA) == 0, label);
        }
        printf("      [实测结果] Speex 解码：32000 采样 / 64000 字节 PCM / SHA256 %s\n", word_sha);
        word_wav = out; /* 留给 ② 用（下面比 PCM）*/
        word_len = out_len;
        if (meta != NULL) dsh_release(meta);
      }

      /* ── ② 「播放速率听头里的」：同一段码流、头里 22050 ─────────────────
       *
       * ⚠️ 官方 `speexdec` 的做法是 `*rate = header->rate`（PCM 不重采样），照它 ——
       *    按模式速率播会让整本 LDOCE5 音高偏低、语速偏慢。两段测试用词典是**同一
       *    段码流**，只把头里的 `rate` 改成 22050（LDOCE5 的真文件就长这样）。
       */
      if (got_rate && word_wav != NULL) {
        uint8_t *out = NULL;
        size_t out_len = 0;
        char *meta = NULL;
        char reason2[512];
        dsh_speex_info info2;
        ok(dsh_audio_prepare(rate_src, rate_src_len, &out, &out_len, &meta) == DSH_OK,
           "⑧ 头里写 22050 的那段也解得出来");
        ok_meta(has(meta, "\"sampleRate\":22050"), meta,
                "⑧ ★★ 播放速率**取头里的 22050**（不是模式速率 16000）—— "
                "改成模式速率会让整本 LDOCE5 偏低偏慢（参考实现 / 那一轮）");
        ok(dsh_speex_probe(rate_src, rate_src_len, &info2, reason2, sizeof(reason2)) ==
               DSH_SPEEX_OK &&
               info2.header_rate == 22050 && info2.rate == 22050 && info2.mode_rate == 16000,
           "⑧ ★ 头里 22050 / 播放速率 22050 / 模式速率 16000 —— 三个值各是各的");
        ok(out != NULL && out_len == word_len &&
               memcmp(out + 44, word_wav + 44, out_len - 44) == 0,
           "⑧ ★★ 而且 PCM 与头里写 16000 的那份**逐样本相同** —— 只有 WAV 头里的速率不同"
           "（解码器不做重采样，这条约定的保护措施）");
        ok(out != NULL && wav_u32(out, 24) == 22050,
           "⑧ 而 WAV 头里写的确实是 22050（播放器照它播）");
        if (out != NULL) dsh_release(out);
        if (meta != NULL) dsh_release(meta);
      } else {
        ok(0, "⑧ 22050 那份测试用词典没取到（或第一段没解出来）");
      }

      /* ── 负例：坏码流**必须明确说解不了**，不许假装能播 ───────────────── */
      if (got_broken) {
        uint8_t *out = NULL;
        size_t out_len = 0;
        char *meta = NULL;
        ok(dsh_audio_prepare(broken_src, broken_src_len, &out, &out_len, &meta) == DSH_OK,
           "⑧ 坏码流也走的是「成功返回 + 人话」那条路（不是崩、不是抛）");
        ok_meta(has(meta, "\"playable\":false") && has(meta, "\"decoded\":false") &&
                    has(meta, "\"mime\":\"audio/x-speex\""),
                meta, "⑧ ★ 坏的那段如实报「解不了」（kind 仍是 spx、mime 回到 audio/x-speex）");
        ok(out == NULL && out_len == 0,
           "⑧ ★ 而且**一个字节都不交出去**（不可播时出参为空，宿主只看 reason）");
        ok_meta(!has(meta, "\"reason\":\"\""), meta, "⑧ 并且给了一句人话");
        if (meta != NULL) dsh_release(meta);
      }

      if (word_wav != NULL) dsh_release(word_wav);
      if (word_src != NULL) dsh_release(word_src);
      if (rate_src != NULL) dsh_release(rate_src);
      if (broken_src != NULL) dsh_release(broken_src);
      dsh_mdx_close(mdd);
    } else {
      ok(0, "⑧ 打开 audio.mdd");
    }
    if (mdd_path != NULL) free(mdd_path);
  }

  ok((int64_t)dsh_mem_live_count() == (int64_t)base,
     "全部用例跑完，活分配表必须回到基线");

  printf("audio：%d 项，失败 %d\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
