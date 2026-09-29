/* ==========================================================================
 * 内核单元测试 · 朗读的规划层（`dsh_speech_plan` 与它那三个纯函数）
 *
 * 这一组钉四类事：
 *   ① **切段**：一段最多 300 字符；断点优先级 句末 → 逗号顿号冒号 → 空白 → 硬切；
 *      中文不许切半个字；`chars` 是码点数。
 *   ② **挑离线音色**：显式指定优先（id 或名字，大小写不敏感）→ 按语种筛 → 英语看地区
 *      （uk → en-GB，**auto 也先找 en-US**）→ 没有精确地区就用第一个（别把发音搞没了）。
 *   ③ **在线音色**：留空 = 内置默认（**不是关掉在线**）；混排一律用中文音色。
 *   ④ **三层排序**（词典自带 → 在线 → 系统离线）与**「一层都走不通」时那句话**
 *      （三种情形三句话，缺什么要说全）。
 *
 * ⚠️ 与平台层的分界：本机装了哪些音色是**壳探测**后经 `voices_json` 传进来的，
 *    所以这里能像造数据一样造出「只有英音音色」「只有中文音色」「一个都没有」三种机器。
 * ========================================================================== */

#include "dsh_lookup.h"
#include "audio/dsh_audio.h"
#include "audio/dsh_speechplan.h"
#include "dsh_internal.h"
#include "mem_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef DSH_TESTDATA_DIR
#error "需要 -DDSH_TESTDATA_DIR=<0.2.0/testdata 的路径>（见 Makefile）"
#endif

static int g_checks = 0;
static int g_failed = 0;

static void ok(int cond, const char *what) {
  g_checks++;
  if (cond) return;
  g_failed++;
  fprintf(stderr, "FAIL %s\n", what);
}

static void ok_eq_i64(int64_t actual, int64_t expected, const char *what) {
  g_checks++;
  if (actual == expected) return;
  g_failed++;
  fprintf(stderr, "FAIL %s：实际=%lld 期望=%lld\n", what, (long long)actual, (long long)expected);
}

static void ok_eq_str(const char *actual, const char *expected, const char *what) {
  g_checks++;
  if (actual != NULL && expected != NULL && strcmp(actual, expected) == 0) return;
  g_failed++;
  fprintf(stderr, "FAIL %s\n      实际=%s\n      期望=%s\n", what, actual != NULL ? actual : "(null)",
          expected != NULL ? expected : "(null)");
}

static int has(const char *haystack, const char *needle) {
  return haystack != NULL && strstr(haystack, needle) != NULL;
}

static char *fixture(const char *name) {
  const size_t n = strlen(DSH_TESTDATA_DIR) + strlen(name) + 2;
  char *p = (char *)malloc(n);
  if (p != NULL) snprintf(p, n, "%s/%s", DSH_TESTDATA_DIR, name);
  return p;
}

/** 造一段重复文本（长度按**码点**算；分段用的都是 ASCII，正好一字一字节）*/
static char *repeat(const char *unit, int times) {
  const size_t n = strlen(unit) * (size_t)times + 1;
  char *s = (char *)malloc(n);
  if (s == NULL) return NULL;
  s[0] = '\0';
  for (int i = 0; i < times; i++) strcat(s, unit);
  return s;
}

/** 把三段拼起来（`repeat` 造出来的临时串由调用方还掉 —— 测试自己也不许漏）*/
static char *join3(const char *a, const char *b, const char *c) {
  const size_t n = strlen(a) + strlen(b) + strlen(c) + 1;
  char *s = (char *)malloc(n);
  if (s == NULL) return NULL;
  snprintf(s, n, "%s%s%s", a, b, c);
  return s;
}

/* ── 临时目录（与 test_history 同一条纪律：不碰用户的配置目录）────────────── */

#define TMP_DIR DSH_TESTDATA_DIR "/tmp-speech"

/**
 * ⚠️ **必须先删再建**：目录里躺着上一轮的 `settings.json`（也许有 Key），不删的话
 *    「没有 Key 时该落哪一层」那条检查标准会被上一轮残留顶掉（source 变成 online）。
 */
static void tmp_reset(void) {
  char cmd[512];
  snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", TMP_DIR, TMP_DIR);
  if (system(cmd) != 0) { /* 建不出来后面会自己报 */ }
}

static void tmp_cleanup(void) {
  char cmd[512];
  snprintf(cmd, sizeof(cmd), "rm -rf '%s'", TMP_DIR);
  if (system(cmd) != 0) { /* 清不掉也不影响结论 */ }
}

int main(void) {
  const size_t base = dsh_mem_live_count();
  tmp_reset();

  /* ══ ① 切段（`splitForSpeech` 的逐条移植）══════════════════════════════ */
  {
    dsh_speech_chunk *chunks = NULL;
    int64_t n = 0;

    ok(dsh_speech_split("", &chunks, &n) == 0 && n == 0,
       "① 空文本 → 0 段（不是 1 段空串）");
    dsh_speech_chunks_free(chunks, n);
    ok(dsh_speech_split("   \t\n ", &chunks, &n) == 0 && n == 0, "① 全是空白 → 0 段");
    dsh_speech_chunks_free(chunks, n);

    ok(dsh_speech_split("  apple  ", &chunks, &n) == 0 && n == 1, "① 短文本 → 1 段");
    if (n == 1) {
      ok_eq_str(chunks[0].text, "apple", "① ★ 两端空白被裁掉（与参考实现的 `.trim()` 同）");
      ok_eq_i64(chunks[0].chars, 5, "① `chars` 是码点数");
    }
    dsh_speech_chunks_free(chunks, n);

    /* 300 是阈值：正好 300 不切，301 才切 */
    {
      char *exact = repeat("a", 300);
      char *over = repeat("a", 301);
      ok(dsh_speech_split(exact, &chunks, &n) == 0 && n == 1,
         "① 正好 300 个码点 → **不切**（一段）");
      dsh_speech_chunks_free(chunks, n);
      ok(dsh_speech_split(over, &chunks, &n) == 0 && n == 2,
         "① 301 个码点 → 切成两段（第二段是剩下的 1 个）");
      if (n == 2) {
        ok_eq_i64(chunks[0].chars, 300, "① 硬切：第一段正好 300（整段没有任何标点/空白）");
        ok_eq_i64(chunks[1].chars, 1, "① 第二段是剩下那个");
      }
      dsh_speech_chunks_free(chunks, n);
      free(exact);
      free(over);
    }

    /*
     * ⚠️ **断点必须落在那扇窗的末尾**才算：分段看的是**前 301 个字符**那扇窗，
     * 而三条正则都是 `$` 锚定的 —— 所以切的是**窗口边界附近**的标点，不是
     * 「全文最后那个标点」。下面两条素材都把标点摆在**第 301 个码点**上。
     */
    {
      /* 300 个 x +。+ 200 个 y：窗口 = 300x +。（301 个码点），断在句号后 */
      char *head = repeat("x", 300);
      char *tail = repeat("y", 200);
      char *text = join3(head, "。", tail);
      ok(dsh_speech_split(text, &chunks, &n) == 0 && n == 2, "① 超长 + 句末标点 → 两段");
      if (n == 2) {
        ok_eq_i64(chunks[0].chars, 301, "① ★ 切在**句末标点之后**（不是硬切 300）");
        ok_eq_i64(chunks[1].chars, 200, "① 第二段是剩下的 200 个");
      }
      dsh_speech_chunks_free(chunks, n);
      free(head);
      free(tail);
      free(text);
    }

    /* 句末标点在窗口里、但**不在末尾**时，那一档不生效；退到逗号那一档 */
    {
      /* 100x +。+ 199z +，+ 200w：窗口 = 100x。199z，（301 个码点）→ 断在逗号后 */
      char *a = repeat("x", 100);
      char *b = repeat("z", 199);
      char *c = repeat("w", 200);
      char *mid = join3(a, "。", b);
      char *text = join3(mid, "，", c);
      ok(dsh_speech_split(text, &chunks, &n) == 0 && n == 2, "① 超长 + 逗号 → 两段");
      if (n == 2) {
        ok_eq_i64(chunks[0].chars, 301, "① ★ 切在**逗号之后**（句末标点不在这扇窗的末尾时不生效）");
      }
      dsh_speech_chunks_free(chunks, n);
      free(a);
      free(b);
      free(c);
      free(mid);
      free(text);
    }

    /* 空白再次之 */
    {
      char *a = repeat("q", 120);
      char *b = repeat("r", 200);
      char *text = join3(a, " ", b);
      ok(dsh_speech_split(text, &chunks, &n) == 0 && n == 2, "① 超长 + 空格 → 两段");
      if (n == 2) ok_eq_i64(chunks[0].chars, 120, "① ★ 切在**最后一个空格**之前（空格本身被裁掉）");
      dsh_speech_chunks_free(chunks, n);
      free(a);
      free(b);
      free(text);
    }

    /* 中文长文本：每段 ≤ 300 码点，而且**不切半个字** */
    {
      char *text = repeat("汉字测试", 100); /* 400 个码点 */
      ok(dsh_speech_split(text, &chunks, &n) == 0 && n == 2, "① 中文 400 码点 → 两段");
      if (n == 2) {
        ok_eq_i64(chunks[0].chars, 300, "① ★ 第一段 300 个**码点**（不是字节）");
        ok_eq_i64(chunks[1].chars, 100, "① 第二段 100 个码点");
        /* UTF-8 合法性：每段长度都必须是 3 的倍数（每个汉字 3 字节） */
        ok(strlen(chunks[0].text) == 900 && strlen(chunks[1].text) == 300,
           "① ★ 每段都是完整的汉字（**没有切半个字**）—— 按字节切就会露馅");
      }
      dsh_speech_chunks_free(chunks, n);
      free(text);
    }
  }

  /* ══ ② 音色表解析（壳探测来的那张表）═══════════════════════════════════ */
  {
    dsh_voice_list list;
    ok(dsh_speech_parse_voices(NULL, 0, &list) == 0 && list.count == 0,
       "② 没给音色表（壳探不到）→ 空表，而且**不是错误**");
    dsh_voices_free(&list);
    ok(dsh_speech_parse_voices("不是 JSON", 0, &list) == 0 && list.count == 0,
       "② 音色表读不动 → 也按空表处理（**不把整条朗读路弄哑**）");
    dsh_voices_free(&list);
    ok(dsh_speech_parse_voices("[]", 0, &list) == 0 && list.count == 0, "② 空数组 → 空表");
    dsh_voices_free(&list);

    {
      const char *json =
          "[{\"id\":\"Microsoft Zira Desktop\",\"name\":\"Zira\",\"language\":\"en-US\","
          "\"culture\":\"en-US\"},"
          "{\"id\":\"Hazel\",\"language\":\"en-GB\"},"
          "{\"id\":\"Huihui\",\"name\":\"慧慧\",\"language\":\"zh-CN\",\"culture\":\"zh-CN\"},"
          "{\"name\":\"没有 id 的那一项\",\"language\":\"en\"}]";
      ok(dsh_speech_parse_voices(json, 0, &list) == 0 && list.count == 3,
         "② ★ 四项里**没有 id 的那一项被跳过**（挑中了也没法用）");
      if (list.count == 3) {
        ok_eq_str(list.items[0].id, "Microsoft Zira Desktop", "② id 读出来了");
        ok_eq_str(list.items[0].language, "en", "② ★ 语种只留主代码（`en-US` → `en`）");
        ok_eq_str(list.items[0].culture, "en-US", "② 区域标记原样留着（英语那条地区规则要用）");
        ok_eq_str(list.items[1].name, "Hazel", "② ★ 缺 name 就借 id（不因为缺一项就丢掉它）");
        ok_eq_str(list.items[1].culture, "en-GB", "② region 从 language 推出来了");
        ok_eq_str(list.items[2].language, "zh", "② 中文音色也在");
      }
      dsh_voices_free(&list);
    }
  }

  /* ══ ③ 挑离线音色（`VoicePicker.Pick` 的逐条移植）══════════════════════ */
  {
    dsh_voice_list list;
    const char *json =
        "[{\"id\":\"Hazel\",\"name\":\"Hazel\",\"language\":\"en\",\"culture\":\"en-GB\"},"
        "{\"id\":\"Zira\",\"name\":\"Zira\",\"language\":\"en\",\"culture\":\"en-US\"},"
        "{\"id\":\"Huihui\",\"name\":\"慧慧\",\"language\":\"zh\",\"culture\":\"zh-CN\"}]";
    (void)dsh_speech_parse_voices(json, 0, &list);
    ok(list.count == 3, "③ 造出三种嗓子：英式、美式、中文");

    ok(dsh_speech_pick_voice(NULL, "en", "auto", NULL) == NULL, "③ 空表 → 挑不到（不是崩）");
    ok_eq_str(dsh_speech_pick_voice(&list, "en", "auto", NULL)->id, "Zira",
              "③ ★ auto 也**先找美式**（英式排在前面也不选它 —— 参考实现那条坑）");
    ok_eq_str(dsh_speech_pick_voice(&list, "en", "uk", NULL)->id, "Hazel",
              "③ ★ 偏英音 → 选英式（用户显式选的，绝不能被兜底抢走）");
    ok_eq_str(dsh_speech_pick_voice(&list, "en", "us", NULL)->id, "Zira", "③ 偏美音 → 选美式");
    ok_eq_str(dsh_speech_pick_voice(&list, "zh", "auto", NULL)->id, "Huihui",
              "③ 中文文本 → 中文嗓子（地区那条只对英语生效）");
    ok_eq_str(dsh_speech_pick_voice(&list, "en", "auto", "Hazel")->id, "Hazel",
              "③ ★ 显式指定的音色**最优先**（连口音偏好都压过去）");
    ok_eq_str(dsh_speech_pick_voice(&list, "en", "auto", "zira")->id, "Zira",
              "③ 指定音色时**大小写不敏感**");
    ok_eq_str(dsh_speech_pick_voice(&list, "en", "auto", "慧慧")->id, "Huihui",
              "③ 用**名字**指定也认（不只认 id）");
    ok(dsh_speech_pick_voice(&list, "ja", "auto", NULL) == NULL,
       "③ ★ 本机没有日语嗓子 → 挑不到（**不许拿英语嗓子糊弄**：那会让置灰检查标准失效）");

    /* 只装了英式音色的机器：auto 也必须能出声（"只要英语兜底就行"）*/
    {
      dsh_voice_list only_uk;
      (void)dsh_speech_parse_voices(
          "[{\"id\":\"Hazel\",\"language\":\"en\",\"culture\":\"en-GB\"}]", 0, &only_uk);
      ok_eq_str(dsh_speech_pick_voice(&only_uk, "en", "auto", NULL)->id, "Hazel",
                "③ ★ 只有英式音色时 auto **也用它**（别因为口音偏好把发音搞没了）");
      dsh_voices_free(&only_uk);
    }
    dsh_voices_free(&list);
  }

  /* ══ ④ 在线音色（`DoubaoSpeech.PickVoice` 的移植）═══════════════════════ */
  {
    ok_eq_str(dsh_speech_doubao_speaker("en", 0, NULL, NULL), DSH_SPEECH_DEFAULT_SPEAKER_EN,
              "④ ★ 音色留空 = **内置默认**（英文 Dacey）——不是「关掉在线」");
    ok_eq_str(dsh_speech_doubao_speaker("zh", 0, NULL, NULL), DSH_SPEECH_DEFAULT_SPEAKER_ZH,
              "④ 中文 → 内置中文音色（Vivi）");
    ok_eq_str(dsh_speech_doubao_speaker("ja", 0, "", ""), DSH_SPEECH_DEFAULT_SPEAKER_EN,
              "④ ★ 空串与留空同一条路（都回内置默认），别的语种一律英文嗓子");
    ok_eq_str(dsh_speech_doubao_speaker("zh", 1, NULL, NULL), DSH_SPEECH_DEFAULT_SPEAKER_ZH,
              "④ ★ 中英混排 → 中文音色（中文那条能读英文，反过来不行）");
    ok_eq_str(dsh_speech_doubao_speaker("en", 0, "my-en", "my-zh"), "my-en",
              "④ 用户填了音色就用他的");
    ok_eq_str(dsh_speech_doubao_speaker("zh", 0, "my-en", "my-zh"), "my-zh",
              "④ 中文用中文那个");
  }

  /* ══ ④b 音色 id → **界面上的说法** ═════════════════════════════════════
     谁吃它：设置页那两个音色格、「检测凭据」每一项、以及内核拼的那句「为什么走这条」。
     约定三条：**默认那两个回官网名**（Dacey / Vivi）、**大小写不敏感**（id 是用户手打的）、
     **认不出来回 id 本身**（不是空串 —— 界面上总得有个能认的东西）。 */
  {
    char *label = NULL;
    ok(dsh_speech_speaker_label(DSH_SPEECH_DEFAULT_SPEAKER_EN, &label) == DSH_OK,
       "④b 默认英文音色问名字：成功");
    ok_eq_str(label, "Dacey", "④b ★ 英文默认音色 → Dacey（与 参考实现逐字相同）");
    if (label != NULL) dsh_release(label);

    label = NULL;
    ok(dsh_speech_speaker_label(DSH_SPEECH_DEFAULT_SPEAKER_ZH, &label) == DSH_OK,
       "④b 默认中文音色问名字：成功");
    ok_eq_str(label, "Vivi", "④b ★ 中文默认音色 → Vivi（用户挑的那两个）");
    if (label != NULL) dsh_release(label);

    label = NULL;
    ok(dsh_speech_speaker_label("EN_FEMALE_DACEY_URANUS_BIGTTS", &label) == DSH_OK,
       "④b 大写 id 也问一次");
    ok_eq_str(label, "Dacey", "④b ★ 大小写不敏感（那串 id 是用户手打进设置里的）");
    if (label != NULL) dsh_release(label);

    label = NULL;
    ok(dsh_speech_speaker_label("my-voice-x", &label) == DSH_OK, "④b 表外音色问名字：成功");
    ok_eq_str(label, "my-voice-x", "④b ★ 表外音色 → **回 id 本身**（不是空串）");
    if (label != NULL) dsh_release(label);

    label = NULL;
    ok(dsh_speech_speaker_label(NULL, &label) == DSH_OK, "④b 音色为空也问得动（不崩）");
    ok_eq_str(label, "", "④b 空音色 → 空串（「没配音色」与「叫什么」不是一回事）");
    if (label != NULL) dsh_release(label);

    label = NULL;
    ok(dsh_speech_speaker_label("", &label) == DSH_OK, "④b 空串也问得动");
    ok_eq_str(label, "", "④b 空串 → 空串");
    if (label != NULL) dsh_release(label);

    /* 出参都不给 = 调用方的错，如实拒（不是写空指针）*/
    ok(dsh_speech_speaker_label("x", NULL) == DSH_E_INVALID_ARG,
       "④b 不给 out_label → DSH_E_INVALID_ARG（不许写空指针）");
  }

  /* ══ ⑤ 三层排序与"一层都走不通"那句话（走引擎）══════════════════════════ */
  {
    char *audio_mdx = fixture("audio.mdx");
    char *test_mdx = fixture("test.mdx");
    dsh_engine *e = NULL;
    if (dsh_engine_create(TMP_DIR, &e) != DSH_OK) {
      ok(0, "⑤ 建引擎（临时目录）");
    } else {
      {
        char paths[2048];
        snprintf(paths, sizeof(paths), "[\"%s\",\"%s\"]", audio_mdx, test_mdx);
        char *added = NULL;
        ok(dsh_engine_dict_add(e, paths, &added) == DSH_OK && has(added, "\"added\":2"),
           "⑤ 加两本测试用词典（音频那本 + 常用那本）");
        if (added != NULL) dsh_release(added);
      }
      /* 当前词典 = 音频那本（它有真录音），要念 `beep` —— 应走**词典自带**那一层 */
      {
        char *list = NULL;
        (void)dsh_engine_dict_list(e, &list);
        /* 这里不解析 JSON：dict_id 留空会走「当前词典」，而它就是刚加进去的音频那本 */
        if (list != NULL) dsh_release(list);
      }
      {
        /* 用 `dsh_engine_dict_add` 之后当前词典是**第一本**（内核那条顺位规则）*/
        char *plan = NULL;
        ok(dsh_speech_plan(e, "beep", NULL, NULL, NULL, &plan) == DSH_OK && plan != NULL,
           "⑤ 问一次规划（当前词典是音频那本、要念 beep）");
        ok(has(plan, "\"source\":\"dict\""),
           "⑤ ★ 词典自带录音那一条在最前面 —— 有原录音就用它");
        ok(has(plan, "\"enabled\":true"), "⑤ 而且这条路是能走的");
        ok(has(plan, "\"chunks\":[{\"text\":\"beep\",\"chars\":4}]") && has(plan, "\"chunkCount\":1"),
           "⑤ 一个词 → 一段（`chunks` 与 `chunkCount` 都给了）");
        ok(has(plan, "\"language\":\"en\""), "⑤ 语种是内核判的（en）");
        /*
         * ⚠️ 词典那一层还要给**哪一本**：壳靠它拼 `https://<词典 id>.dictres.invalid/
         *    __sound__/<键名>`。少了它壳只能拿外壳站点去猜，界面上表现为「点了朗读没声音」。
         *    钉的是「那本 = 刚加进去的音频那本」（内容哈希），不是「字段非空」。
         */
        {
          char *list = NULL;
          (void)dsh_engine_dict_list(e, &list);
          char want[128];
          want[0] = '\0';
          if (list != NULL) {
            /* 清单里第 0 本（`dictAdd` 之后当前是它）的 id */
            const char *at = strstr(list, "\"id\":\"");
            if (at != NULL) {
              at += 6;
              size_t n = 0;
              while (at[n] != '\0' && at[n] != '"' && n + 1 < sizeof(want)) n++;
              memcpy(want, at, n);
              want[n] = '\0';
            }
            dsh_release(list);
          }
          char needle[192];
          snprintf(needle, sizeof(needle), "\"dictId\":\"%s\"", want);
          ok(want[0] != '\0' && has(plan, needle),
             "⑤ ★★ dict 那一层给出**哪一本**（`chosen.dictId` = 刚加进去那本，壳靠它拼地址）");
        }
        /* 在线那一层：没有 Key → 不可用，理由要说清"去设置里填" */
        ok(has(plan, "还没有填 API Key"), "⑤ ★ 没填 Key 时在线那层如实说「还没填 API Key」");
        /* 系统那一层：一个音色都没给 → 说"问不到"，而不是"本机没有英语语音" */
        ok(has(plan, "问不到本机的离线音色"),
           "⑤ ★ 壳没给音色表时说的是「问不到」——**不是**「本机没有这个语种」（两件事）");
        if (plan != NULL) dsh_release(plan);
      }

      /*
       * 给一张英文音色表 + 没有 Key：
       *   · `beep`（**有**原录音）→ 仍然走 dict（顺序第一档，嗓子再好也不抢先）
       *   · `ghost`（词条里挂着音频、卷里没有那个文件）→ dict 走不通，落到**系统离线**
       */
      {
        const char *voices =
            "[{\"id\":\"Zira\",\"name\":\"Microsoft Zira\",\"language\":\"en\","
            "\"culture\":\"en-US\"}]";
        char *plan = NULL;
        ok(dsh_speech_plan(e, "beep", NULL, voices, NULL, &plan) == DSH_OK && plan != NULL,
           "⑤ 带一张英文音色表再问一次（beep：有原录音）");
        ok(has(plan, "\"source\":\"dict\""),
           "⑤ ★ 有原录音时嗓子再多也不抢先（dict 是第一档）");
        if (plan != NULL) dsh_release(plan);

        char *plan2 = NULL;
        ok(dsh_speech_plan(e, "ghost", NULL, voices, NULL, &plan2) == DSH_OK && plan2 != NULL,
           "⑤ 换 ghost（没有原录音）再问一次");
        ok(has(plan2, "\"source\":\"system\""),
           "⑤ ★ 有嗓子能念、而且没有 Key → 落到**系统离线**那一层");
        ok(has(plan2, "\"voiceId\":\"Zira\""), "⑤ 挑中的嗓子是那一张表里的（voiceId=Zira）");
        if (plan2 != NULL) dsh_release(plan2);
      }

      /* 有 Key + 有音色表：**在线排在系统前面**（这就是那条产品顺序）*/
      {
        const char *voices =
            "[{\"id\":\"Zira\",\"name\":\"Microsoft Zira\",\"language\":\"en\","
            "\"culture\":\"en-US\"}]";
        char *out = NULL;
        ok(dsh_engine_settings_set(e, "{\"speech\":{\"doubaoApiKey\":\"test-key-123\"}}", &out) ==
               DSH_OK,
           "⑤ 给设置填一个（假）Key");
        if (out != NULL) dsh_release(out);
        char *plan = NULL;
        ok(dsh_speech_plan(e, "beep", NULL, voices, NULL, &plan) == DSH_OK && plan != NULL,
           "⑤ 有 Key 之后规划一次");
        /* 注意：这条词在**当前那本**里有原录音，所以仍然走 dict —— 换一个没有录音的词
         * 才能看出「在线 vs 系统」的先后 */
        ok(has(plan, "\"source\":\"dict\""), "⑤ 有原录音的词仍然优先走 dict（顺序第一档）");
        if (plan != NULL) dsh_release(plan);

        /* `ghost`：词条里挂着音频、卷里没有那个文件 → dict 那一层走不通 */
        char *plan2 = NULL;
        ok(dsh_speech_plan(e, "ghost", NULL, voices, NULL, &plan2) == DSH_OK && plan2 != NULL,
           "⑤ 换一个**没有原录音**的词（ghost）再规划");
        ok(has(plan2, "\"source\":\"online\""),
           "⑤ ★★ 这一档是**在线排在系统前面**（有 Key 就走在线，不是先试本机嗓子）");
        ok(has(plan2, "\"speaker\":\"en_female_dacey_uranus_bigtts\""),
           "⑤ 在线用的英文默认音色（留空 = 内置默认）");
        /*
         * ★★ **给界面看的那句话**里写的是展示名 + 语种（`Dacey · 英语`），不是那串 id；
         * `chosen.speaker` 仍然是 id（壳拼请求体那一侧要的是 id）—— 两条各司其职。
         */
        ok(has(plan2, "\"detail\":\"Dacey ·"),
           "⑤ ★★ 在线那一条的 `detail` 是**展示名 + 语种**（界面显示的就是它）");
        ok(!has(plan2, "\"detail\":\"en_female_dacey_uranus_bigtts"),
           "⑤ ★ 而且不许把那串 id 当成「界面上的说法」（这一轮修的就是它）");
        ok(has(plan2, "\"options\""), "⑤ 三层各自能不能走都摆在 options 里");
        if (plan2 != NULL) dsh_release(plan2);
      }

      /* 中文词 + 只有中文嗓子 → 系统那一层挑中文 */
      {
        char *titled = fixture("titled.mdx");
        char *out = NULL;
        (void)dsh_engine_settings_set(e, "{\"speech\":{\"doubaoApiKey\":null}}", &out);
        if (out != NULL) dsh_release(out);
        char *plan = NULL;
        ok(dsh_speech_plan(e, "测试", NULL,
                           "[{\"id\":\"Huihui\",\"language\":\"zh\",\"culture\":\"zh-CN\"}]",
                           NULL, &plan) == DSH_OK &&
               plan != NULL,
           "⑤ 中文词 + 一张中文音色表");
        ok(has(plan, "\"language\":\"zh\""), "⑤ 语种判成 zh");
        ok(has(plan, "\"voiceId\":\"Huihui\""), "⑤ ★ 中文词挑中文嗓子");
        if (plan != NULL) dsh_release(plan);
        if (titled != NULL) free(titled);
      }

      /* 一层都走不通：**那句话要把"缺什么"说全**（参考实现 `ExplainUnavailable` 那一支）*/
      {
        char *out = NULL;
        (void)dsh_engine_settings_set(e, "{\"speech\":{\"doubaoApiKey\":null}}", &out);
        if (out != NULL) dsh_release(out);
        char *plan = NULL;
        /* 一个音色都没有 + 没 Key + 词条没有原录音（ghost）→ 三层全不通 */
        ok(dsh_speech_plan(e, "ghost", NULL, "[]", NULL, &plan) == DSH_OK && plan != NULL,
           "⑤ 三层全不通时也要**成功返回**（原因写在 disabledReason 里，不是错误码）");
        ok(has(plan, "\"source\":\"none\"") && has(plan, "\"enabled\":false"),
           "⑤ ★ 如实说这次念不了（source=none / enabled=false）");
        ok(has(plan, "本机没有") && has(plan, "豆包这条路也还没配好"),
           "⑤ ★★ 「缺什么」一次说全：本机没有这个语种的嗓子**而且**豆包还没配好");
        /* 有嗓子、但有 Key 没填、词条没录音 → 与上一条**不是同一句话** */
        char *plan2 = NULL;
        ok(dsh_speech_plan(e, "ghost", NULL,
                           "[{\"id\":\"Zira\",\"language\":\"en\",\"culture\":\"en-US\"}]",
                           NULL, &plan2) == DSH_OK &&
               plan2 != NULL,
           "⑤ 再来一次：这次本机有英文嗓子");
        ok(!has(plan2, "豆包这条路也还没配好"),
           "⑤ ★ 有嗓子时那句话**不再提豆包**（两句话不是同一句）");
        if (plan != NULL) dsh_release(plan);
        if (plan2 != NULL) dsh_release(plan2);
      }

      /*
       * 语速（`speech.rate`）：**由内核随答案一起给**，壳不许自己去翻设置。
       *
       * 检查标准钉的是"内核给的数 == 设置里那个数"（对象本身），不是"JSON 里有个 rate 键"：
       * 只断键在不在，把 `sp->rate` 写成常数 0 也照样绿。
       */
      {
        const char *voices =
            "[{\"id\":\"Zira\",\"name\":\"Microsoft Zira\",\"language\":\"en\","
            "\"culture\":\"en-US\"}]";
        char *out = NULL;
        ok(dsh_engine_settings_set(e, "{\"speech\":{\"rate\":4,\"doubaoApiKey\":null}}", &out) ==
               DSH_OK,
           "⑤ 把语速调成 4");
        if (out != NULL) dsh_release(out);
        char *plan = NULL;
        ok(dsh_speech_plan(e, "ghost", NULL, voices, NULL, &plan) == DSH_OK && plan != NULL,
           "⑤ 系统离线那一档再规划一次");
        ok(has(plan, "\"source\":\"system\""), "⑤ 走的是系统离线（ghost 没有原录音）");
        ok(has(plan, "\"rate\":4"),
           "⑤ ★ 语速**由内核随答案给出**（`chosen.rate` == 设置里那个 4）—— 壳不必去翻设置");
        if (plan != NULL) dsh_release(plan);

        /* 夹的范围也由内核保证（设置那一节已经夹过一遍，这里钉住它对规划的影响）*/
        char *out2 = NULL;
        (void)dsh_engine_settings_set(e, "{\"speech\":{\"rate\":99}}", &out2);
        if (out2 != NULL) dsh_release(out2);
        char *plan2 = NULL;
        ok(dsh_speech_plan(e, "ghost", NULL, voices, NULL, &plan2) == DSH_OK && plan2 != NULL,
           "⑤ 语速填一个越界的值（99）再规划");
        ok(has(plan2, "\"rate\":10"), "⑤ ★ 落到规划里的还是夹过的 10（越界值不许溜到平台 API）");
        if (plan2 != NULL) dsh_release(plan2);
        char *out3 = NULL;
        (void)dsh_engine_settings_set(e, "{\"speech\":{\"rate\":0}}", &out3);
        if (out3 != NULL) dsh_release(out3);
      }

      /*
       * **空文本那一档**（"还没有要念的东西"）。
       *
       * ⚠️ 这一档必须有：系统离线那一层**只关心"有没有这个语种的嗓子"**，
       *    少了这一档，空文本会被规划成 `enabled:true` —— 界面上「朗读」按钮亮着，
       *    点下去合成一段 0 字节的音频。而界面**不许自己判**"有没有东西可念"。
       */
      {
        const char *voices =
            "[{\"id\":\"Zira\",\"name\":\"Microsoft Zira\",\"language\":\"en\","
            "\"culture\":\"en-US\"}]";
        char *plan = NULL;
        ok(dsh_speech_plan(e, "", NULL, voices, NULL, &plan) == DSH_OK && plan != NULL,
           "⑤ 空文本也要**成功返回**（那是一档正经情形，不是参数错误）");
        ok(has(plan, "\"source\":\"none\"") && has(plan, "\"enabled\":false"),
           "⑤ ★ 空文本 → 三层一律不通（本机明明有英文嗓子，也不能说能念）");
        ok(has(plan, "\"disabledReason\":\"没有要念的文本。\""),
           "⑤ ★★ 点不了那句人话就是这一句（界面把它挂在按钮的 title 上，一个字都不拼）");
        ok(!has(plan, "还没有填 API Key"),
           "⑤ ★ 空文本时**不许**说「还没填 Key」—— 那是另一件事的原因，会把人指错方向");
        ok(!has(plan, "\"voiceId\":\"Zira\""),
           "⑤ 也不许选择音色（挑了就等于说这一档能念）");
        if (plan != NULL) dsh_release(plan);

        /*
         * 全是空白与空串**同一档**：检查标准与切段那边是同一份实现
         * （`dsh_speech_has_text` → `trim_range`）—— 这里连它一起钉住，
         * 免得出现"切段说没东西念、规划说能念"这种两处约定打架。
         */
        ok(dsh_speech_has_text("") == 0 && dsh_speech_has_text("   ") == 0 &&
               dsh_speech_has_text("\t\n") == 0 && dsh_speech_has_text("\xE3\x80\x80") == 0,
           "⑤ ★ `dsh_speech_has_text`：空串 / 半角空白 / 换行制表 / 全角空格都算「没东西念」");
        ok(dsh_speech_has_text("a") == 1 && dsh_speech_has_text(" a ") == 1,
           "⑤ 有一个真字符就算有东西念（两侧的空白不算）");
        char *plan2 = NULL;
        ok(dsh_speech_plan(e, "   ", NULL, voices, NULL, &plan2) == DSH_OK && plan2 != NULL,
           "⑤ 纯空白也问一次");
        ok(has(plan2, "\"source\":\"none\"") && has(plan2, "\"disabledReason\":\"没有要念的文本。\""),
           "⑤ 纯空白走同一档（不切段、不选择音色）");
        if (plan2 != NULL) dsh_release(plan2);
      }
      /*
       * ══ ⑥ 「这一次的覆盖」（`overrides_json`）═══════════════════════════════════
       *
       * 它服务的是界面上两个**测量**动作：「试听」（念用户正在填的那个音色）与
       * 「平衡音量」（拿 `loudness: 0` 量**中性电平** —— 照设置念的话，量到的电平会
       * 随着滑块自己变）。两条规则：**键在 = 强制这个值**（`source` 在就只许走那一层，
       * 不可用就如实报**它自己的**原因、**绝不不报错地回落**），**键不在 = 照设置**。
       */
      {
        const char *voices =
            "[{\"id\":\"Zira\",\"name\":\"Microsoft Zira\",\"language\":\"en\","
            "\"culture\":\"en-US\"}]";
        char *set = NULL;
        char *plan = NULL;

        /* 先把状态摆成"两层都能走"：有（假）Key + 有英文嗓子，词用 ghost（没有原录音）*/
        ok(dsh_engine_settings_set(e, "{\"speech\":{\"doubaoApiKey\":\"k-override\"}}", &set) == DSH_OK,
           "⑥ 摆一个（假）Key，让在线那一层也可用");
        if (set != NULL) dsh_release(set);
        set = NULL;
        ok(dsh_speech_plan(e, "ghost", NULL, voices, NULL, &plan) == DSH_OK && plan != NULL,
           "⑥ 没有覆盖时规划一次");
        ok(has(plan, "\"source\":\"online\""),
           "⑥ （前提）没有覆盖时按产品顺序落到**在线**那一层（它排在系统前面）");
        if (plan != NULL) dsh_release(plan);
        plan = NULL;

        /* ★ 强制走 system：在线明明能用，也不许抢 */
        ok(dsh_speech_plan(e, "ghost", NULL, voices, "{\"source\":\"system\"}", &plan) == DSH_OK &&
               plan != NULL,
           "⑥ 覆盖里指定 source=system");
        ok(has(plan, "\"source\":\"system\"") && has(plan, "\"voiceId\":\"Zira\""),
           "⑥ ★ 指定了就走它 —— **在线那一层还在能用也不许抢**（顺序被覆盖压住）");
        if (plan != NULL) dsh_release(plan);
        plan = NULL;

        /* ★ 强制走 online：没有 Key 时**如实报在线那一层的原因**，不回落到系统 */
        ok(dsh_engine_settings_set(e, "{\"speech\":{\"doubaoApiKey\":\"\"}}", &set) == DSH_OK,
           "⑥ 把 Key 清掉");
        if (set != NULL) dsh_release(set);
        set = NULL;
        ok(dsh_speech_plan(e, "ghost", NULL, voices, "{\"source\":\"online\"}", &plan) == DSH_OK &&
               plan != NULL,
           "⑥ 指定 source=online，但没 Key");
        ok(has(plan, "\"enabled\":false") && has(plan, "\"source\":\"none\""),
           "⑥ ★ 这一层走不通 → 点不了");
        ok(has(plan, "\"disabledReason\":\"还没有填 API Key"),
           "⑥ ★★ 而且说的是**在线那一层自己的**原因（不是「本机没有英语语音」）");
        ok(!has(plan, "\"voiceId\":\"Zira\""),
           "⑥ ★★ 绝不不报错地回落到系统离线那一层（那会让「试听」听起来对了、其实换了嗓子）");
        if (plan != NULL) dsh_release(plan);
        plan = NULL;

        /* ★ voiceId 覆盖：用户正在填/正在听的那个音色，优先于设置里存着的 */
        ok(dsh_engine_settings_set(e, "{\"speech\":{\"doubaoApiKey\":\"k-override\"}}", &set) == DSH_OK,
           "⑥ 把 Key 填回去");
        if (set != NULL) dsh_release(set);
        set = NULL;
        ok(dsh_speech_plan(e, "ghost", NULL, voices,
                           "{\"source\":\"online\",\"voiceId\":\"my-voice-x\"}", &plan) == DSH_OK &&
               plan != NULL,
           "⑥ 覆盖里指定 voiceId");
        ok(has(plan, "\"speaker\":\"my-voice-x\""),
           "⑥ ★ 用覆盖里那个音色（不是设置里的默认 Dacey）");
        ok(has(plan, "\"loudness\":0"),
           "⑥ ★ 表外的音色 → 响度补偿 0（用户自己填的嗓子不替它猜）");
        if (plan != NULL) dsh_release(plan);
        plan = NULL;

        /* ★ loudness 覆盖：**设置里是 30，覆盖说 0** → 这一次必须是 0（量中性电平就靠它）*/
        ok(dsh_engine_settings_set(e, "{\"speech\":{\"doubaoLoudnessEn\":30}}", &set) == DSH_OK,
           "⑥ 把英文那一格的响度设成 30");
        if (set != NULL) dsh_release(set);
        set = NULL;
        ok(dsh_speech_plan(e, "ghost", NULL, voices, NULL, &plan) == DSH_OK && plan != NULL,
           "⑥ 这次不带覆盖");
        ok(has(plan, "\"loudness\":30"), "⑥ （前提）不带覆盖时用的是设置里那个 30");
        if (plan != NULL) dsh_release(plan);
        plan = NULL;
        ok(dsh_speech_plan(e, "ghost", NULL, voices, "{\"loudness\":0}", &plan) == DSH_OK &&
               plan != NULL,
           "⑥ 这次带上 loudness=0 的覆盖");
        ok(has(plan, "\"loudness\":0"),
           "⑥ ★★ 覆盖赢（这一次是 0，不是设置里那个 30）—— 「平衡音量」量中性电平靠的就是它");
        if (plan != NULL) dsh_release(plan);
        plan = NULL;

        /* 覆盖里的 language：影响选音色（这一条只需要"没被当成坏输入"且生效）*/
        ok(dsh_speech_plan(e, "ghost", NULL, voices, "{\"language\":\"zh\"}", &plan) == DSH_OK &&
               plan != NULL,
           "⑥ 覆盖里指定 language=zh");
        ok(has(plan, "\"language\":\"zh\""), "⑥ ★ 语种按覆盖走（在线/系统两层都用它挑）");
        if (plan != NULL) dsh_release(plan);
        plan = NULL;

        /* 坏输入**如实拒**（不猜、不不报错地退回）*/
        ok(dsh_speech_plan(e, "ghost", NULL, voices, "{源不合法", &plan) == DSH_E_INVALID_ARG,
           "⑥ ★ overrides_json 坏掉 → 如实拒（DSH_E_INVALID_ARG）");
        ok(dsh_speech_plan(e, "ghost", NULL, voices, "{\"source\":\"nope\"}", &plan) ==
               DSH_E_INVALID_ARG,
           "⑥ ★ source 认不出来 → 如实拒（别猜用户想走哪一层）");
        /* 认不出的**键**忽略（向前兼容：将来加了键，老壳多传一个不该让发音整个失败）*/
        ok(dsh_speech_plan(e, "ghost", NULL, voices, "{\"futureKey\":123}", &plan) == DSH_OK &&
               plan != NULL,
           "⑥ ★ 不认识的键**忽略**（向前兼容），照样出规划");
        if (plan != NULL) dsh_release(plan);
        plan = NULL;
        /* 空串 / NULL 与"没给"完全等价（老调用方一个字不用改）*/
        ok(dsh_speech_plan(e, "ghost", NULL, voices, "", &plan) == DSH_OK && plan != NULL,
           "⑥ ★ 空串 = 没给（老行为逐字不变）");
        if (plan != NULL) dsh_release(plan);
        plan = NULL;

        ok(dsh_engine_settings_set(e, "{\"speech\":{\"doubaoLoudnessEn\":null}}", &set) == DSH_OK,
           "⑥ 收尾：把响度那一格摆回去");
        if (set != NULL) dsh_release(set);
      }

      if (audio_mdx != NULL) free(audio_mdx);
      if (test_mdx != NULL) free(test_mdx);
      dsh_engine_destroy(e);
    }
  }

  tmp_cleanup();
  ok((int64_t)dsh_mem_live_count() == (int64_t)base,
     "全部用例跑完，活分配表必须回到基线");

  printf("speech：%d 项，失败 %d\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
