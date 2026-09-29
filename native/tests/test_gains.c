/* ==========================================================================
 * 内核单元测试 · 音量增益（`speech.dictGainDb` / `speech.systemGainDb`）
 *
 * 这一组钉四类事：① **归一化**（夹到 [-24, +12]、取整到 0.1 —— 不归一化就会出现
 * 「设置里是 999 dB」）；② **按词典存**（改 A 本不动 B 本，`systemGainDb` 与它互不影响）；
 * ③ **三种语义分得开**：传数字 = 设成它、传 null = 清掉、**整个键不传 = 不改**
 * （前端只发它要改的那一项 —— 把「没传」当成「清零」会让用户拖 A 滑块把 B 清零）；
 * ④ **那两句文案**（没有词典 / 这本没有资源卷）要说对，`dictAvailable` 跟着一起变。
 *
 * ⚠️ 全程在测试用词典目录旁边的临时配置目录里跑，**先清再建、不碰用户的配置目录**。
 * ========================================================================== */

#include "dsh_lookup.h"
#include "engine/dsh_engine_internal.h"
#include "engine/dsh_settings.h"
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

static void ok_eq_str(const char *actual, const char *expected, const char *what) {
  g_checks++;
  if (actual == NULL || expected == NULL || strcmp(actual, expected) != 0) {
    g_failed++;
    fprintf(stderr, "FAIL %s\n      实际=%s\n      期望=%s\n", what, actual ? actual : "(null)",
            expected ? expected : "(null)");
  }
}

/** 从平坦 JSON 里取某个字符串键的值（够用就行，不做通用解析） */
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

static char *tmp_config(const char *name) {
  const char *fixtures = DSH_TESTDATA_DIR;
  const size_t need = strlen(fixtures) + strlen(name) + 32;
  char *path = (char *)malloc(need);
  if (path != NULL) snprintf(path, need, "%s/tmp-gains/%s", fixtures, name);
  return path;
}

int main(void) {
  const size_t base = dsh_mem_live_count();
  char *dir = tmp_config("cfg");
  dsh_engine *engine = NULL;
  char *json = NULL;
  char title[256];

  /*
   * 建引擎（用临时配置目录）。
   *
   * ⚠️ **先清空再建**（`rm -rf` + `mkdir -p`）：这一组会**落盘**（增益写进 settings.json、
   *    ⑨ 还导入两本词典），而目录是固定的 —— 上一轮留下的 settings.json 会让这一轮
   *    从「已经设过 -0.4 dB、词库里已经有两本」开始，于是①那一组当场红。
   */
  {
    char mk[1024];
    snprintf(mk, sizeof(mk), "rm -rf '%s' && mkdir -p '%s'", dir, dir);
    if (system(mk) != 0) { /* 清不掉也要继续：下面那句建引擎会自己判断 */ }
  }
  if (dsh_engine_create(dir, &engine) != DSH_OK || engine == NULL) {
    fprintf(stderr, "建不起引擎：%s\n", dsh_last_error_message());
    return 1;
  }

  /* ① 起初：什么都没设过 → 两个增益都是 0.0，而且「没有词典」那句话说对了 */
  ok(dsh_speech_gains(engine, NULL, 2, &json) == DSH_OK, "① 取增益视图");
  ok_has(json, "\"dictGainDb\":0.0", "① 没设过时内置录音增益是 0.0（不是缺键）");
  ok_has(json, "\"systemGainDb\":0.0", "① 没设过时系统语音增益是 0.0");
  ok_has(json, "\"systemAvailable\":true", "① 本机有两个音色 → 系统语音那一路可用");
  ok_has(json, "\"dictAvailable\":false", "① 还没加词典 → 内置录音那一项不可调");
  ok_has(json, "先导入一本带音频卷", "①★ 不可调时的说法是「还没有添加词典…」");
  dsh_release(json);
  json = NULL;

  /* ② 写进去一个系统增益：**归一化**要生效（999 → +12.0；-0.44 → -0.4）*/
  ok(dsh_speech_gains_set(engine, NULL, "{\"systemGainDb\":999}", 2, &json) == DSH_OK,
     "② 设系统增益");
  ok_has(json, "\"systemGainDb\":12.0", "②★ 999 被夹到 +12.0（归一化）");
  dsh_release(json);
  json = NULL;

  ok(dsh_speech_gains_set(engine, NULL, "{\"systemGainDb\":-0.44}", 2, &json) == DSH_OK,
     "② 设一个要取整的值");
  ok_has(json, "\"systemGainDb\":-0.4", "②★ -0.44 取整到 -0.4（一位小数）");
  dsh_release(json);
  json = NULL;

  /* ③ 「整个键不传 = 不改」：只发 dictGainDb 时，上面那个系统增益**不许被清零** */
  ok(dsh_speech_gains_set(engine, "d-a", "{\"dictGainDb\":3.25}", 2, &json) == DSH_OK,
     "③ 设 A 本的增益");
  ok_has(json, "\"dictGainDb\":3.3", "③★ 3.25 取整到 3.3");
  ok_has(json, "\"systemGainDb\":-0.4", "③★ 只发 dict 那一个键 → 系统增益**原样留着**（没被清零）");
  dsh_release(json);
  json = NULL;

  /* ④ 按词典存：改 B 本不动 A 本 */
  ok(dsh_speech_gains_set(engine, "d-b", "{\"dictGainDb\":-6.5}", 2, &json) == DSH_OK,
     "④ 设 B 本的增益");
  /*
   * ⚠️ 这一句 `dsh_release` **不能省**：`set` 回的那份视图是内核交给宿主的活分配，
   *    直接让下一句的 `&json` 把它覆盖掉就是一条泄漏（活分配表当场从 0 变成 1），
   *    而症状只在最后那条「没漏内存」上暴露出来。
   */
  dsh_release(json);
  json = NULL;
  ok(dsh_speech_gains(engine, "d-a", 2, &json) == DSH_OK, "④ 回读 A 本");
  ok_has(json, "\"dictGainDb\":3.3", "④★ A 本还是 3.3（B 本那一改没波及它）");
  dsh_release(json);
  json = NULL;
  ok(dsh_speech_gains(engine, "d-b", 2, &json) == DSH_OK, "④ 回读 B 本");
  ok_has(json, "\"dictGainDb\":-6.5", "④ B 本是 -6.5");
  dsh_release(json);
  json = NULL;

  /* ⑤ 传 null = 清掉这一项（清完回到 0.0）*/
  ok(dsh_speech_gains_set(engine, "d-b", "{\"dictGainDb\":null}", 2, &json) == DSH_OK,
     "⑤ 清掉 B 本的增益");
  ok_has(json, "\"dictGainDb\":0.0", "⑤★ 清掉之后回到 0.0");
  dsh_release(json);
  json = NULL;
  ok(dsh_speech_gains(engine, "d-a", 2, &json) == DSH_OK, "⑤ 清 B 本之后回读 A 本");
  ok_has(json, "\"dictGainDb\":3.3", "⑤★ A 本**不受影响**（清的是 B 本那一格）");
  dsh_release(json);
  json = NULL;

  /* ⑥ 系统语音那一路不可用时：available=false 且那句话非空（壳报 0 个音色）*/
  ok(dsh_speech_gains(engine, NULL, 0, &json) == DSH_OK, "⑥ 报「本机没有音色」那一档");
  ok_has(json, "\"systemAvailable\":false", "⑥★ 没有音色时系统语音那一路不可用");
  ok_has(json, "没探到可用的离线语音", "⑥★ 而且给了一句人话（界面原样显示）");
  dsh_release(json);
  json = NULL;

  /* ⑦ 标题：词典名由内核算（界面不拼）——`d-a` 不在词库里，就退回 id 本身 */
  ok(dsh_speech_gains(engine, "d-a", 2, &json) == DSH_OK, "⑦ 取一个不在词库里的 id");
  field(json, "dictTitle", title, sizeof(title));
  ok_eq_str(title, "d-a", "⑦★ 不在词库里的 id：标题退回 id 本身（不编一个）");
  dsh_release(json);
  json = NULL;

  /* ⑧ 参数不合法要如实拒（不许悄悄成功）*/
  ok(dsh_speech_gains(engine, NULL, 2, NULL) != DSH_OK, "⑧ out_json 为空 → 拒");
  ok(dsh_speech_gains_set(engine, NULL, NULL, 2, &json) != DSH_OK, "⑧ patch 为空 → 拒");

  /*
   * ⑨ ★ **「当前词典」空着时要兜底第一本**：检查标准是「一本词典都没有」，不是
   *    「没设当前那本」—— 不兜底的话，刚导入 / 刚删完词典那一刻滑块会莫名其妙不能用。
   *
   *    这里刻意**先导入一本没有资源卷的**（`kana.mdx` 没有同名 `.mdd`），再导入 `audio.mdx`
   *    （有 `.mdd`）：于是「兜底到了第一本」与「兜底到了有录音的那本」是两个可分辨的结果。
   */
  ok(dsh_engine_dict_add(engine, "[\"" DSH_TESTDATA_DIR "/kana.mdx\",\"" DSH_TESTDATA_DIR
                                 "/audio.mdx\"]", &json) == DSH_OK, "⑨ 导入两本词典");
  dsh_release(json);
  json = NULL;
  ok(dsh_engine_dict_set_current(engine, "", &json) == DSH_OK, "⑨ 把「当前词典」清空");
  dsh_release(json);
  json = NULL;

  ok(dsh_speech_gains(engine, NULL, 2, &json) == DSH_OK, "⑨ 当前词典空着时取增益视图");
  field(json, "dictTitle", title, sizeof(title));
  ok_eq_str(title, "kana.mdx", "⑨★ 没有当前词典 → 兜底**第一本**（不是那本有录音的）");
  ok_has(json, "\"dictAvailable\":false", "⑨★ 第一本没有资源卷 → 这一项不可调");
  ok_has(json, "没有资源卷", "⑨★ 于是给的是「这本没有自带录音」那句话");
  dsh_release(json);
  json = NULL;

  /* 指定一本不存在的词典要被拒（别悄悄变成「没有当前词典」）*/
  ok(dsh_engine_dict_set_current(engine, "d-不存在", &json) != DSH_OK,
     "⑨ 指定一本不存在的词典 → 拒");

  /*
   * ⑩ ★ **落盘**：写进去的增益必须**重开引擎还在**。
   *
   * 为什么非有不可：只换内存里那份设置**不算落库** —— 界面上拖滑块一切正常，
   * `settings.json` 里根本没有 `speech` 那一节，一重启回到 0；而同一次运行里回读内存
   * 那份视图当然是对的、验不出来。钉住它的办法只有一个：**真的重开一次引擎**再从盘上读。
   */
  ok(dsh_speech_gains_set(engine, NULL, "{\"systemGainDb\":-3}", 2, &json) == DSH_OK,
     "⑩ 写一个系统增益（准备验落盘）");
  dsh_release(json);
  json = NULL;
  dsh_engine_destroy(engine);
  engine = NULL;

  ok(dsh_engine_create(dir, &engine) == DSH_OK && engine != NULL, "⑩ 重开引擎（同一份配置目录）");
  ok(dsh_speech_gains(engine, NULL, 2, &json) == DSH_OK, "⑩ 重开之后回读增益视图");
  ok_has(json, "\"systemGainDb\":-3.0", "⑩★ 重启之后系统增益还是 -3.0（**真落盘**，不是只在内存里）");
  dsh_release(json);
  json = NULL;

  /* ⑩b 词典增益同样要落盘（它是"一本一个数"的那张表）*/
  ok(dsh_speech_gains_set(engine, "d-persist", "{\"dictGainDb\":6.25}", 2, &json) == DSH_OK,
     "⑩b 写一本词典的增益");
  dsh_release(json);
  json = NULL;
  dsh_engine_destroy(engine);
  engine = NULL;
  ok(dsh_engine_create(dir, &engine) == DSH_OK && engine != NULL, "⑩b 再重开一次");
  ok(dsh_speech_gains(engine, "d-persist", 2, &json) == DSH_OK, "⑩b 回读那一本");
  ok_has(json, "\"dictGainDb\":6.3", "⑩b★ 重启之后那一本还是 6.3（归一化 + 落盘都在）");
  dsh_release(json);
  json = NULL;

  dsh_engine_destroy(engine);
  engine = NULL;
  free(dir);

  /*
   * ⚠️ **那一行实测结果必须打在最后一条断言之后**：`g_failed` 是在断言里累加的，
   *    先 `printf` 再断言就会出现"这一组写着「失败 0」而进程退出码是 1"——
   *    上一轮就是这么把一次真失败读成了绿的。
   *    与 `test_engine` 的收尾顺序保持一致：**先断言，后打印**。
   */
  ok((size_t)dsh_mem_live_count() == base, "⑪ 这一组没漏内存（内核记账数回到起点）");
  printf("增益：%d 项，失败 %d\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
