/* 内核单元测试 · 在线语音（豆包）三条接口：plan（该发什么）/ test_plan（检测凭据）/ accept（回包什么意思）。
 * 单开一组的理由：纯逻辑那层（base64 / 音色 / 请求体 / SSE 解析）已有 `test_doubao.c` 管着，
 * 而这三条**一条检查标准都没有** —— 签名合法 ≠ 行为对。全组不联网（accept 吃手写 SSE 回包）；
 * 配置走临时目录、先清再建，不碰用户目录。 */

#include "dsh_lookup.h"
#include "engine/dsh_engine_internal.h"
#include "mem_registry.h"

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

static void ok_has(const char *json, const char *needle, const char *what) {
  g_checks++;
  if (json == NULL || strstr(json, needle) == NULL) {
    g_failed++;
    fprintf(stderr, "FAIL %s\n      实际=%s\n      里面没有=%s\n", what, json ? json : "(null)",
            needle);
  }
}

static void ok_not_has(const char *json, const char *needle, const char *what) {
  g_checks++;
  if (json != NULL && strstr(json, needle) != NULL) {
    g_failed++;
    fprintf(stderr, "FAIL %s\n      实际=%s\n      **不该**有=%s\n", what, json, needle);
  }
}

static void ok_eq_i64(int64_t actual, int64_t expected, const char *what) {
  g_checks++;
  if (actual != expected) {
    g_failed++;
    fprintf(stderr, "FAIL %s：实际=%lld 期望=%lld\n", what, (long long)actual, (long long)expected);
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

/** 从平坦 JSON 里取一个字符串字段（够用就行；带反斜杠转义的值解不了，本组用不到）*/
static void field(const char *json, const char *key, char *buf, size_t cap) {
  char pat[96];
  const char *p;
  size_t k = 0;
  buf[0] = '\0';
  if (json == NULL) return;
  snprintf(pat, sizeof(pat), "\"%s\":\"", key);
  p = strstr(json, pat);
  if (p == NULL) return;
  p += strlen(pat);
  while (*p != '\0' && *p != '"' && k + 1 < cap) buf[k++] = *p++;
  buf[k] = '\0';
}

/**
 * 取出 `plans[]` 里第 n 项的原文（够用就行）。
 * 不能「找下一个 `}` 为止」：请求体那一项（`body`）**自己就是一段 JSON**、里面满是 `}` ——
 * 所以这里按**层数**走并**跳过字符串内部**（与壳那边 `ShellBridge.SplitArray` 同一条纪律）。
 */
static void plan_item(const char *json, int index, char *buf, size_t cap) {
  const char *open;
  int item = -1;
  int depth = 0;
  int in_string = 0;
  size_t k = 0;
  const char *p;

  buf[0] = '\0';
  if (json == NULL) return;
  open = strstr(json, "\"plans\":[");
  if (open == NULL) return;
  open += strlen("\"plans\":[");

  for (p = open; *p != '\0'; p++) {
    char c = *p;
    if (in_string) {
      if (c == '\\') {
        p++;
        continue;
      }
      if (c == '"') in_string = 0;
      if (item == index && k + 1 < cap) buf[k++] = c;
      continue;
    }
    if (c == '"') {
      in_string = 1;
      if (item == index && k + 1 < cap) buf[k++] = c;
      continue;
    }
    if (c == '{') {
      depth++;
      if (depth == 1) {
        item++;
        if (item > index) break;
      }
      if (item == index && k + 1 < cap) buf[k++] = c;
      continue;
    }
    if (c == '}') {
      depth--;
      if (item == index && k + 1 < cap) buf[k++] = c;
      if (depth == 0 && item == index) break;
      continue;
    }
    if (item == index && k + 1 < cap) buf[k++] = c;
  }
  buf[k < cap ? k : cap - 1] = '\0';
}

/** 数组里一共有几项（数顶层 `{`，同样跳过字符串）*/
static int plan_count(const char *json) {
  const char *open;
  const char *p;
  int brace = 0;
  int bracket = 0;
  int n = 0;
  int in_string = 0;

  if (json == NULL) return 0;
  open = strstr(json, "\"plans\":[");
  if (open == NULL) return 0;
  for (p = open + strlen("\"plans\":["); *p != '\0'; p++) {
    char c = *p;
    if (in_string) {
      if (c == '\\') {
        p++;
        continue;
      }
      if (c == '"') in_string = 0;
      continue;
    }
    if (c == '"') {
      in_string = 1;
      continue;
    }
    if (c == '{') {
      if (brace == 0) n++;
      brace++;
      continue;
    }
    if (c == '}') {
      brace--;
      continue;
    }
    if (c == '[') {
      bracket++;
      continue;
    }
    if (c == ']') {
      /* 收尾认的是**数组自己那个 `]`**（bracket 回到 0 那一个），不是「第一项结束」：
       * 写成「项数够 1 就 break」会把两项永远数成 1。 */
      if (bracket == 0) break;
      bracket--;
      continue;
    }
  }
  return n;
}

int main(void) {
  const size_t base = dsh_mem_live_count();
  const char *fixtures = DSH_TESTDATA_DIR;
  const size_t need = strlen(fixtures) + 40;
  char *dir = (char *)malloc(need);
  dsh_engine *engine = NULL;
  char *json = NULL;
  char item[2048];

  if (dir == NULL) {
    fprintf(stderr, "分配不出临时目录名\n");
    return 1;
  }
  snprintf(dir, need, "%s/tmp-online/cfg", fixtures);

  /* **先清再建**：这一组会写设置（填 Key、改音色），固定目录会让上一次留下的 Key
   * 混进这一次的前提里 —— 脏目录第二遍必红，红的是测试自己脏。 */
  {
    char mk[1024];
    snprintf(mk, sizeof(mk), "rm -rf '%s' && mkdir -p '%s'", dir, dir);
    if (system(mk) != 0) { /* 清不掉也继续：下面那句建引擎会自己判断 */
    }
  }
  if (dsh_engine_create(dir, &engine) != DSH_OK || engine == NULL) {
    fprintf(stderr, "建不起引擎：%s\n", dsh_last_error_message());
    free(dir);
    return 1;
  }

  /* ══ ① plan：没凭据 / 没音色 / 没文本，三种原因分开说 ══ */
  {
    ok(dsh_speech_online_plan(engine, "apple", NULL, NULL, &json) == DSH_OK && json != NULL,
       "① 没填 Key 时也**成功返回**（原因写在 JSON 里，不是错误码）");
    ok_has(json, "\"ok\":false", "① 没填 Key → ok=false");
    ok_has(json, "\"needsHttp\":false", "①★ 没填 Key → needsHttp=false（壳据此**不发请求**）");
    ok_has(json, "\"why\":\"no-key\"", "① 原因码是 no-key");
    ok_has(json, "凭据", "① 那句人话要点出「缺凭据」");
    dsh_release(json);
    json = NULL;

    ok(dsh_speech_online_plan(engine, "", NULL, NULL, &json) == DSH_OK,
       "① 空文本也成功返回");
    ok_has(json, "\"why\":\"empty\"", "①★ 空文本与「没 Key」是两句不同的话");
    dsh_release(json);
    json = NULL;

    ok(dsh_speech_online_plan(engine, NULL, NULL, NULL, &json) == DSH_OK, "① NULL 文本同样说得清");
    ok_has(json, "\"ok\":false", "① NULL 文本 → ok=false");
    dsh_release(json);
    json = NULL;
  }

  /* ══ ② plan：发得出去时，该发的东西一次给全 ══ */
  {
    char *set = NULL;
    ok(dsh_engine_settings_set(engine, "{\"volcengine\":{\"apiKey\":\"k-test\"}}", &set) == DSH_OK,
       "② 填一把（假）Key");
    if (set != NULL) dsh_release(set);

    ok(dsh_speech_online_plan(engine, "apple", NULL, NULL, &json) == DSH_OK && json != NULL,
       "② 有 Key 之后规划一次");
    ok_has(json, "\"ok\":true", "② ok=true");
    ok_has(json, "\"needsHttp\":true", "②★ needsHttp=true（壳该把这一段原样发出去）");
    ok_has(json, "https://openspeech.bytedance.com/api/v3/tts/unidirectional/sse",
           "② 端点是**带 /sse 的那个**（修正第 2 条）");
    ok_has(json, "\"X-Api-Key\"", "② 头里有 X-Api-Key");
    ok_has(json, "\"k-test\"", "②★ Key 用的是设置里那一把（镜像那一份）");
    ok_has(json, "\"X-Api-Resource-Id\"", "② 头里有 X-Api-Resource-Id");
    ok_has(json, "\"X-Api-Request-Id\"", "② 头里有 X-Api-Request-Id（服务端排错要用）");
    ok_has(json, "\"X-Control-Require-Usage-Tokens-Return\"",
           "② 头里有计费数字那个开关（少一个字符就没有 usage.text_words）");
    ok_has(json, "\"speaker\":\"en_female_dacey_uranus_bigtts\"",
           "② 纯英文 → 内置英文音色（留空 = 默认）");
    ok_has(json, "\"resourceId\":\"seed-tts-2.0\"", "② 模型版本与音色配套");
    ok_has(json, "\"language\":\"en\"", "② 语种判成 en");
    /* 请求体在 plan 里是**一个 JSON 串**（引号都转义了），要按转义后的字节去对 ——
     * 拿没转义的裸写法去比永远比不上（红起来看着像「请求体里没有文本」）。 */
    ok_has(json, "\\\"text\\\":\\\"apple\\\"", "② 请求体里带上文本");
    ok_has(json, "\\\"speaker\\\":\\\"en_female_dacey_uranus_bigtts\\\"",
           "② 请求体里的音色与 plan 报的那个是同一个");
    ok_not_has(json, "explicit_language", "②★ 请求体里**不该有** explicit_language（硬约定 ①）");
    /* ★ 响度补偿（`loudness_rate`）：默认（没设过滑块）走内置表 -50（-6.0 dB，参考实现量出来的）——
     * 漏了它，设置里那两个滑块对在线那一层就毫无作用，而且默认音色会响约 6 dB。 */
    ok_has(json, "\\\"loudness_rate\\\":-50",
           "②★ 默认英文音色带上内置的响度补偿 -50（-6.0 dB，参考实现量出来的）");
    dsh_release(json);
    json = NULL;

    /* ★ 中英混排必须走中文音色：拿英文音色念混排会得到空句子（实测）。 */
    ok(dsh_speech_online_plan(engine, "apple苹果", NULL, NULL, &json) == DSH_OK, "② 混排文本规划一次");
    ok_has(json, "\"speaker\":\"zh_female_vv_uranus_bigtts\"",
           "②★ 中英混排 → 中文音色（不照做就是空句子）");
    ok_has(json, "\"mixed\":true", "② 混排这件事本身也报出来");
    ok_has(json, "\\\"loudness_rate\\\":-50", "②★ 混排走中文音色（Vivi）→ 也是内置 -50");
    dsh_release(json);
    json = NULL;

    /* 设置里那两个滑块**优先**（而且只对对应的音色生效）*/
    ok(dsh_engine_settings_set(engine,
                               "{\"speech\":{\"doubaoLoudnessEn\":20,\"doubaoLoudnessZh\":35}}",
                               &set) == DSH_OK,
       "② 把两个响度滑块设成 20 / 35");
    if (set != NULL) dsh_release(set);
    ok(dsh_speech_online_plan(engine, "apple", NULL, NULL, &json) == DSH_OK, "② 再规划一次（英文）");
    ok_has(json, "\\\"loudness_rate\\\":20", "②★ 英文那一格设了 20 → 用它（盖过内置 -50）");
    ok_not_has(json, "\\\"loudness_rate\\\":-50", "②★ 而且内置那个值不再出现");
    dsh_release(json);
    json = NULL;
    ok(dsh_speech_online_plan(engine, "苹果", NULL, NULL, &json) == DSH_OK, "② 规划一次（中文）");
    ok_has(json, "\\\"loudness_rate\\\":35", "②★ 中文那一格设了 35 → 用它（Vivi）");
    dsh_release(json);
    json = NULL;

    /* ⚠️ 补丁里给 null = 「不改这一格」（本版 `apply_patch` 的语义：只改补丁里出现的键，
     * 键在而值不是整数则保持原值）。参考实现那边是 `UpdateSpeech(Action<…>)`、赋 null 就是清掉，
     * 而这一版的补丁载体是 JSON，null 的含义另定为「不改」；管理窗滑块永远送数字，对用户不可见。 */
    ok(dsh_engine_settings_set(engine, "{\"speech\":{\"doubaoLoudnessEn\":null}}", &set) == DSH_OK,
       "② 往补丁里塞一个 null");
    if (set != NULL) dsh_release(set);
    ok(dsh_speech_online_plan(engine, "apple", NULL, NULL, &json) == DSH_OK, "② 塞完 null 再规划一次");
    ok_has(json, "\\\"loudness_rate\\\":20",
           "②★ 补丁里的 null **不改**那一格（仍是 20）—— 本版的补丁语义，别按「清掉」写检查标准");
    dsh_release(json);
    json = NULL;

    /* 设回两个内置音色该有的值：后面几节都走显式值，不再依赖内置表。 */
    ok(dsh_engine_settings_set(engine,
                               "{\"speech\":{\"doubaoLoudnessEn\":-50,\"doubaoLoudnessZh\":-50}}",
                               &set) == DSH_OK,
       "② 把两个响度设回 -50（= 内置表的值）");
    if (set != NULL) dsh_release(set);

    /* ★ 「这一次的覆盖」（`overrides_json`）：界面的「试听」「平衡音量」靠它「这一次就按我给的念」。
     * ⚠️ 它必须**同时**生效在 plan 报的 `speaker` 与请求体里 —— 两条接口看的是同一份覆盖。 */
    ok(dsh_speech_online_plan(engine, "apple", NULL,
                              "{\"voiceId\":\"my-override-voice\",\"loudness\":0}", &json) == DSH_OK,
       "② 带覆盖规划一次（voiceId + loudness=0）");
    ok_has(json, "\"speaker\":\"my-override-voice\"",
           "②★ 覆盖里的 voiceId 就是这次用的音色（不是设置里的 Dacey）");
    ok_has(json, "\\\"speaker\\\":\\\"my-override-voice\\\"",
           "②★ 请求体里的音色也是它（两条接口看**同一份**覆盖）");
    ok_has(json, "\\\"loudness_rate\\\":0",
           "②★★ 覆盖里的 loudness=0 进了 audio_params —— 「平衡音量」量中性电平靠的就是它");
    ok_not_has(json, "\\\"loudness_rate\\\":-50", "②★ 而设置里那个 -50 不再出现");
    dsh_release(json);
    json = NULL;

    ok(dsh_speech_online_plan(engine, "apple", NULL, "{\"source\":\"system\"}", &json) == DSH_OK,
       "② 覆盖里给一个**这条接口不管**的键（source）也要正常工作");
    ok_has(json, "\\\"loudness_rate\\\":-50", "② 不认识的键忽略，其余照规则走（仍是内置 -50）");
    dsh_release(json);
    json = NULL;

    ok(dsh_speech_online_plan(engine, "apple", NULL, "not-json", &json) == DSH_E_INVALID_ARG,
       "②★ overrides_json 坏掉 → 如实拒（DSH_E_INVALID_ARG）");

    /* 设置里填过 Resource 就用填的那个（拿 1.0 的值配 2.0 的音色，回来的必须还是 1.0）。
     * ⚠️ 按音色后缀推 Resource 那条路今天**走不到**：设置那一步读盘时就把默认 `seed-tts-2.0`
     * 落进去了，「没填」表示不出来；参考实现同样走不到 —— 两边行为一致，所以这里不钉后缀推断。 */
    ok(dsh_engine_settings_set(engine, "{\"speech\":{\"doubaoResourceId\":\"seed-tts-1.0\"}}",
                               &set) == DSH_OK,
       "② 把模型版本填成 1.0");
    if (set != NULL) dsh_release(set);
    ok(dsh_speech_online_plan(engine, "apple", NULL, NULL, &json) == DSH_OK, "② 再规划一次");
    ok_has(json, "\"resourceId\":\"seed-tts-1.0\"", "②★ 设置里填过就用填的那个");
    dsh_release(json);
    json = NULL;

    /* 填回默认值：后面几节按默认配置的前提走。 */
    ok(dsh_engine_settings_set(engine, "{\"speech\":{\"doubaoResourceId\":\"seed-tts-2.0\"}}",
                               &set) == DSH_OK,
       "② 模型版本填回默认的 2.0");
    if (set != NULL) dsh_release(set);
    ok(dsh_speech_online_plan(engine, "apple", NULL, NULL, &json) == DSH_OK, "② 回读一次");
    ok_has(json, "\"resourceId\":\"seed-tts-2.0\"", "② 回读确实是 2.0");
    dsh_release(json);
    json = NULL;
  }

  /* ══ ③ test_plan：没给音色 → 英文 + 中文各一项 ══ */
  {
    ok(dsh_speech_online_test_plan(engine, NULL, NULL, &json) == DSH_OK && json != NULL,
       "③ 检测计划回得来");
    ok_has(json, "\"ok\":true", "③ 这一次调用本身是成功的");
    ok_eq_i64(plan_count(json), 2, "③★ 没说测哪个音色 → **两项**（英文 + 中文）");

    plan_item(json, 0, item, sizeof(item));
    ok_has(item, "\"language\":\"en\"", "③ 第 1 项是英文");
    ok_has(item, "\"text\":\"apple\"", "③ 第 1 项念 apple（样本词来自内核那张表）");
    ok_has(item, "\"speaker\":\"en_female_dacey_uranus_bigtts\"", "③ 第 1 项用英文音色");
    ok_has(item, "\"needsHttp\":true", "③ 第 1 项发得出去");

    plan_item(json, 1, item, sizeof(item));
    ok_has(item, "\"language\":\"zh\"", "③ 第 2 项是中文");
    ok_has(item, "\"text\":\"苹果\"", "③ 第 2 项念 苹果");
    ok_has(item, "\"speaker\":\"zh_female_vv_uranus_bigtts\"", "③ 第 2 项用中文音色");
    ok_has(item, "\"explicitLanguage\":\"\"",
           "③★ explicitLanguage 恒为空串（请求体里就不带它）");
    dsh_release(json);
    json = NULL;
  }

  /* ══ ④ ★ 检测测的是「即将存下去的音色」，不是设置里存着的那个 ══
   * 「保存前先检测」那一步的前提：用户在表单里改了音色、还没写盘，要测的必须是**表单里那个**；
   * 测设置里那个 = 测了个旧配置 = 等于没测（参考实现的 `speaker` 重载就是干这个的）。 */
  {
    ok(dsh_speech_online_test_plan(engine, "my-brand-new-voice", "zh", &json) == DSH_OK,
       "④ 指定一个**设置里没有**的音色去检测");
    ok_eq_i64(plan_count(json), 1, "④ 指定了音色 → 只测一项");
    plan_item(json, 0, item, sizeof(item));
    ok_has(item, "\"speaker\":\"my-brand-new-voice\"",
           "④★ 测的是**递进来的那个音色**（不是设置里的英文/中文音色）");
    ok_has(item, "\"language\":\"zh\"", "④ 语种用递进来的那个");
    ok_has(item, "\"text\":\"苹果\"", "④★ 样本词跟着语种走（zh → 苹果）");
    dsh_release(json);
    json = NULL;

    /* 换一个 `_mars_bigtts` 音色：**型号那一栏仍按设置里填的那个走**（不由后缀推）*/
    ok(dsh_speech_online_test_plan(engine, "zh_female_vv_mars_bigtts", "zh", &json) == DSH_OK,
       "④ 换一个 `_mars_bigtts` 音色");
    plan_item(json, 0, item, sizeof(item));
    ok_has(item, "\"resourceId\":\"seed-tts-2.0\"",
           "④ 型号取的是设置里那个值（`resourceId` 这一格今天表示不出「没填」）");
    dsh_release(json);
    json = NULL;

    /* 语种认不出 → 按英文（样本词也要跟着走，别拿中文的样本配英文的嗓子）*/
    ok(dsh_speech_online_test_plan(engine, "my-voice", "klingon", &json) == DSH_OK,
       "④ 语种认不出时也回得来");
    plan_item(json, 0, item, sizeof(item));
    {
      char value[128];
      /* 这里**钉值**而不是钉子串：`language` 那一格的子串在别的键里也可能撞上 */
      field(item, "language", value, sizeof(value));
      ok_eq_str(value, "en", "④★ 认不出的语种 → en");
      field(item, "text", value, sizeof(value));
      ok_eq_str(value, "apple", "④ 于是样本词是 apple");
    }
    dsh_release(json);
    json = NULL;

    /* 大小写与区域标记都要认（界面递上来的可能是 `ZH` / `zh-CN`）*/
    ok(dsh_speech_online_test_plan(engine, "my-voice", "ZH", &json) == DSH_OK,
       "④ 递上来的语种写成大写也回得来");
    plan_item(json, 0, item, sizeof(item));
    {
      char value[128];
      field(item, "language", value, sizeof(value));
      ok_eq_str(value, "zh", "④★ `ZH` → 规范成 zh");
      field(item, "text", value, sizeof(value));
      ok_eq_str(value, "苹果", "④ 于是样本词是 苹果（不是 hello）");
    }
    dsh_release(json);
    json = NULL;
  }

  /* ══ ⑤ ★ 「这一项没东西可测」必须与「服务端说它不能用」分开说 ══
   * 界面（`classifyDoubaoTest`）照这两句话分类：音色留空那一项**不该**拦着用户保存
   * （它本来就没东西可测），而「没填 Key」那种是**真的不通**、要拦下来 —— 混成一句界面就分不了。 */
  {
    char *set = NULL;
    ok(dsh_engine_settings_set(engine, "{\"speech\":{\"doubaoSpeakerEn\":\"\"}}", &set) == DSH_OK,
       "⑤ 把英文音色留空");
    if (set != NULL) dsh_release(set);
    ok(dsh_speech_online_test_plan(engine, NULL, NULL, &json) == DSH_OK, "⑤ 再要一次计划");
    ok_eq_i64(plan_count(json), 2, "⑤ 仍然是两项（留空的那一项也要报回来，不是跳过）");
    plan_item(json, 0, item, sizeof(item));
    ok_has(item, "\"ok\":false", "⑤ 留空那项 ok=false");
    ok_has(item, "\"needsHttp\":false", "⑤★ 那一项不该发请求");
    ok_has(item, "没填", "⑤★ 那句人话里有「没填」（界面就是照这两个字分的类）");
    plan_item(json, 1, item, sizeof(item));
    ok_has(item, "\"needsHttp\":true", "⑤ 中文那项照旧发得出去（一项不填不牵连另一项）");
    dsh_release(json);
    json = NULL;

    /* 没 Key：**两项都是**那句凭据提示，而不是「没填音色」*/
    /* ⚠️ 清 Key 用的是**空串**、不是 `null`：`doubaoApiKey` 只在**非空**时才写进去
     * （`merge_speech`），而 `null` 那一档是「这个键没提到」—— 补丁里写 null 清不掉它，
     * 「没 Key」那一节就会还带着 Key。界面清空输入框走的就是空串这条路。 */
    ok(dsh_engine_settings_set(engine,
                               "{\"speech\":{\"doubaoSpeakerEn\":\"en_x_bigtts\","
                               "\"doubaoApiKey\":\"\"}}",
                               &set) == DSH_OK,
       "⑤ 音色填回来、Key 清掉");
    if (set != NULL) dsh_release(set);
    ok(dsh_speech_online_test_plan(engine, NULL, NULL, &json) == DSH_OK, "⑤ 没 Key 时再要一次");
    ok_eq_i64(plan_count(json), 2, "⑤ 没 Key 也两项");
    plan_item(json, 0, item, sizeof(item));
    ok_has(item, "\"ok\":false", "⑤ 没 Key → ok=false");
    ok_has(item, "凭据", "⑤★ 说的是「缺凭据」，不是「没填音色」");
    ok_not_has(item, "没填（这一项测不了）", "⑤★ 两句人话不许混成一个");
    plan_item(json, 1, item, sizeof(item));
    ok_has(item, "凭据", "⑤ 第二项同一句话");
    dsh_release(json);
    json = NULL;
  }

  /* ══ ⑥ accept：SSE 回包 → 音频字节 / 人话 ══ */
  {
    uint8_t *bytes = NULL;
    size_t len = 0;
    char *plan = NULL;
    char *meta = NULL;
    char *set = NULL;

    ok(dsh_engine_settings_set(engine, "{\"volcengine\":{\"apiKey\":\"k-test\"}}", &set) == DSH_OK,
       "⑥ 再填回 Key");
    if (set != NULL) dsh_release(set);
    ok(dsh_speech_online_plan(engine, "apple", NULL, NULL, &plan) == DSH_OK && plan != NULL,
       "⑥ 拿一份 plan（accept 要用它回话）");

    /* 正常的一包：两行 data，各自一段 base64（`YXBwbGU=` 是 apple、`YmFuYW5h` 是 banana）*/
    {
      const char *sse =
          "data: {\"code\":0,\"data\":\"YXBwbGU=\",\"usage\":{\"text_words\":1}}\n"
          "data: {\"code\":0,\"data\":\"YmFuYW5h\"}\n";
      ok(dsh_speech_online_accept(engine, plan, 200, sse, 123, &bytes, &len, &meta) == DSH_OK,
         "⑥ 正常的一包解得动");
      ok_has(meta, "\"ok\":true", "⑥ ok=true");
      ok_eq_i64((int64_t)len, 11, "⑥★ 两段拼起来是 11 个字节（apple + banana）");
      ok(bytes != NULL && memcmp(bytes, "applebanana", 11) == 0, "⑥★ 顺序是**按行拼**的");
      ok_has(meta, "\"bytes\":11", "⑥ 回包里的 bytes 与真实长度一致");
      ok_has(meta, "\"textWords\":1", "⑥★ 计费字数读出来了（那个头点亮了才有）");
      ok_has(meta, "\"elapsedMs\":123", "⑥ 耗时是壳量到的那个数");
      ok_has(meta, "audio/mpeg", "⑥ MIME 是 mp3");
      {
        /* 回话里那个 `speaker` 是**从 plan 里捡的**（accept 不该自己再判一遍），所以检查标准是
         * 与这次递进去的 plan 相等 —— 钉这条传递，而不是钉某个音色名（那会跟上一节的设置绑在一起）。 */
        char want[128];
        char got[128];
        field(plan, "speaker", want, sizeof(want));
        field(meta, "speaker", got, sizeof(got));
        ok_eq_str(got, want, "⑥★ 回话里的音色就是这次 plan 报的那个");
      }
      if (bytes != NULL) dsh_release(bytes);
      bytes = NULL;
      len = 0;
      if (meta != NULL) dsh_release(meta);
      meta = NULL;
    }

    /* ★ code 0 但没有音频：**不算成功**（实测英文音色念混排就是这一档）*/
    {
      const char *sse = "data: {\"code\":0,\"data\":\"\"}\n";
      ok(dsh_speech_online_accept(engine, plan, 200, sse, 80, &bytes, &len, &meta) == DSH_OK,
         "⑥ code 0 但没音频时也解得动");
      ok_has(meta, "\"ok\":false", "⑥★ code 0 **不等于**成功（空音频要单独判成失败）");
      ok_has(meta, "\"bytes\":0", "⑥ 失败时字节数是 0");
      ok(bytes == NULL, "⑥★ 失败时**不给**字节（不许把空缓冲当音频交出去）");
      if (meta != NULL) dsh_release(meta);
      meta = NULL;
    }

    /* 服务端的错：**错误码翻成人话**，不是把原始码丢给用户 */
    {
      const char *sse = "data: {\"code\":40000001,\"message\":\"param error\"}\n";
      ok(dsh_speech_online_accept(engine, plan, 200, sse, 50, &bytes, &len, &meta) == DSH_OK,
         "⑥ 错误码那一包也解得动");
      ok_has(meta, "\"ok\":false", "⑥ 错误码 → ok=false");
      ok_has(meta, "\"message\":\"", "⑥ 有人话");
      ok_not_has(meta, "\"message\":\"40000001\"", "⑥★ 人话不是原始码本身");
      if (meta != NULL) dsh_release(meta);
      meta = NULL;
    }

    /* 根本没发出去（断网 / 超时）：状态 0 要**与「服务端的错」分开说** */
    {
      ok(dsh_speech_online_accept(engine, plan, 0, "", 30000, &bytes, &len, &meta) == DSH_OK,
         "⑥ 没发出去那一档也解得动");
      ok_has(meta, "\"ok\":false", "⑥ 没发出去 → ok=false");
      ok_has(meta, "没发出去", "⑥★ 与「服务端说它不能用」分开说（超时绝不并进「没有」）");
      if (meta != NULL) dsh_release(meta);
      meta = NULL;
    }

    if (plan != NULL) dsh_release(plan);
  }

  dsh_engine_destroy(engine);
  engine = NULL;
  free(dir);

  /* ⚠️ 实测结果必须打在最后一条断言之后（`g_failed` 是在断言里累加的）——
   * 先 printf 再断言就会出现「这一组写着失败 0，而进程退出码是 1」。 */
  ok((size_t)dsh_mem_live_count() == base, "⑦ 这一组没漏内存（内核记账数回到起点）");
  printf("在线语音：%d 项，失败 %d\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
