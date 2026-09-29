/* ==========================================================================
 * 内核单元测试 · 设置模型
 *
 * 检查标准分五组：① **规范化**（脏配置读进来必须是干净的：没路径的条目丢掉、空白裁掉、
 * 非法枚举退回默认、缺失的时间戳补上）；② **未识别字段必须活下来** —— 内核不认识的顶层键
 * 要**连值的原文**一起搬运，这是防「用户设置莫名消失」的唯一手段，所以钉「解析 → 序列化 →
 * 再解析，那个字段的值一字不差」；③ **补丁语义**（「没给」与「给了 null」是两件事，非法值
 * 要夹回而不是报错）；④ **词库五件事**（加 / 删 / 改名 / 指定当前 / 显示名约定）；
 * ⑤ **序列化是确定的**（同一份设置两次序列化逐字节相同，可 diff、可对照测试）。
 * ========================================================================== */

#include "engine/dsh_settings.h"
#include "dsh_lookup.h"
#include "mem_registry.h"

#include <stdio.h>
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
    fprintf(stderr, "FAIL %s：实际=%lld 期望=%lld\n", what, (long long)actual,
            (long long)expected);
  }
}

/** 解析一份设置（失败即记一笔） */
static dsh_settings *parse(const char *json, const char *what) {
  dsh_settings *s = NULL;
  if (dsh_settings_parse(json, json ? strlen(json) : 0, &s) != 0) {
    g_checks++;
    g_failed++;
    fprintf(stderr, "FAIL %s：本该解析成功，却报：%s\n", what, dsh_last_error_message());
    return NULL;
  }
  return s;
}

/** 解析必定失败的输入 */
static void parse_must_fail(const char *json, const char *what) {
  dsh_settings *s = NULL;
  g_checks++;
  if (dsh_settings_parse(json, json ? strlen(json) : 0, &s) == 0) {
    g_failed++;
    fprintf(stderr, "FAIL %s：本该失败却成功了（%s）\n", what, json ? json : "(null)");
    dsh_settings_free(s);
  }
}

int main(void) {
  /* ── ① 规范化 ─────────────────────────────────────────────────────────── */
  {
    dsh_settings *s = parse(NULL, "没有设置文件 = 全默认");
    if (s != NULL) {
      ok_eq_i64(dsh_settings_dict_count(s), 0, "① 默认设置里没有词库");
      ok_eq_str(dsh_settings_current_dict_id(s), "", "① 默认没有当前词典");
      ok(dsh_settings_close_behavior(s) == DSH_CLOSE_ASK, "① 默认关窗行为是 ask");
      dsh_settings_free(s);
    }
  }
  {
    /* 脏配置：没路径的条目、空白、非法枚举、缺失/为 0 的时间戳、非字符串项 */
    const char *dirty =
        "{\"version\":1,\"closeBehavior\":\"explode\",\"currentDictId\":\"  abc  \","
        "\"dictionaries\":["
        "{\"id\":\"  id1  \",\"title\":\"  My Dict  \",\"mdxPath\":\"  /a/b.mdx  \","
        "\"mddPaths\":[\"  /a/b.mdd  \",\"\",null,42,\"  /a/b.1.mdd  \"],\"addedAt\":0},"
        "{\"id\":\"id2\"},"
        "{\"mdxPath\":\"\"},"
        "null,42,\"nope\","
        "{\"id\":\"id3\",\"mdxPath\":\"/c/d.mdx\",\"addedAt\":1700000000000,\"customTitle\":\"  \"}"
        "]}";
    dsh_settings *s = parse(dirty, "① 脏配置");
    if (s != NULL) {
      ok_eq_i64(dsh_settings_dict_count(s), 2, "① 没路径 / 非对象的条目必须丢掉（只剩 2 条）");
      const dsh_stored_dict *d0 = dsh_settings_dict_at(s, 0);
      ok(d0 != NULL, "① 第一条还在");
      if (d0 != NULL) {
        ok_eq_str(d0->id, "id1", "① id 要裁掉首尾空白");
        ok_eq_str(d0->title, "My Dict", "① 默认名要裁掉首尾空白");
        ok_eq_str(d0->mdx_path, "/a/b.mdx", "① 路径要裁掉首尾空白");
        ok_eq_i64(d0->mdd_count, 2, "① 资源卷里的空串 / null / 数字都要丢掉（只剩 2 条）");
        ok_eq_str(d0->mdd_paths[0], "/a/b.mdd", "① 资源卷路径也要裁空白");
        ok_eq_str(d0->mdd_paths[1], "/a/b.1.mdd", "① 第二条资源卷");
        ok(d0->added_at > 1600000000000LL, "① addedAt 是 0 → 必须补上当前时间");
      }
      const dsh_stored_dict *d1 = dsh_settings_dict_at(s, 1);
      ok(d1 != NULL, "① 第二条还在");
      if (d1 != NULL) {
        ok_eq_str(d1->mdx_path, "/c/d.mdx", "① 第二条的路径");
        ok(d1->custom_title == NULL, "① customTitle 全是空白 → 当作没改过（NULL）");
        ok_eq_i64(d1->added_at, 1700000000000LL, "① 给了时间戳就照原样留着");
      }
      ok(dsh_settings_close_behavior(s) == DSH_CLOSE_ASK,
         "① 非法 closeBehavior 必须退回 ask（不是报错、也不是留着乱值）");
      ok_eq_str(dsh_settings_current_dict_id(s), "abc", "① currentDictId 要裁首尾空白");
      dsh_settings_free(s);
    }
  }
  {
    /* 三个合法枚举值都要认 */
    const char *cases[][2] = {{"{\"closeBehavior\":\"quit\"}", "quit"},
                              {"{\"closeBehavior\":\"tray\"}", "tray"},
                              {"{\"closeBehavior\":\"ask\"}", "ask"}};
    for (int i = 0; i < 3; i++) {
      dsh_settings *s = parse(cases[i][0], "① closeBehavior");
      if (s == NULL) continue;
      char label[80];
      snprintf(label, sizeof(label), "① closeBehavior=%s 要认", cases[i][1]);
      const dsh_close_behavior want = (i == 0)   ? DSH_CLOSE_QUIT
                                      : (i == 1) ? DSH_CLOSE_TRAY
                                                 : DSH_CLOSE_ASK;
      ok(dsh_settings_close_behavior(s) == want, label);
      dsh_settings_free(s);
    }
  }
  parse_must_fail("{", "① 坏 JSON 必须失败（不能拿半份设置去覆盖用户的文件）");
  parse_must_fail("[1,2]", "① 最外层不是对象必须失败");
  parse_must_fail("{\"dictionaries\":{}}", "① dictionaries 不是数组必须失败");

  /* ── ② 未识别字段必须原样活下来 ───────────────────────────────────────── */
  {
    const char *with_unknown =
        "{\"version\":1,\"placement\":{\"pillX\":12,\"pillY\":-3,\"edge\":\"left\"},"
        "\"history\":[{\"word\":\"apple\",\"at\":1700000000001}],"
        /* ⚠️ `speech` 是**内核认识的键**，所以它不走「原样搬运」，而是被解析 + 归一化之后重写。
         *    这里刻意带上两个**没建模**的子键，验的是「接管一个对象之后，不认识的子键也必须
         *    原样活下来」—— 少一个就是「用户设置莫名丢了」。 */
        "\"speech\":{\"rate\":3,\"accent\":\"uk\",\"systemGainDb\":-1.5,"
        "\"futureVoice\":{\"x\":[1,2]}},"
        "\"translate\":{\"enabled\":true,\"targetMode\":\"auto\"},"
        "\"someFutureKey\":[1,2,{\"nested\":true}],"
        "\"dictionaries\":[{\"id\":\"x\",\"mdxPath\":\"/x.mdx\"}]}";
    dsh_settings *s1 = parse(with_unknown, "② 带未识别字段的设置");
    if (s1 != NULL) {
      char *json1 = dsh_settings_to_json(s1);
      ok(json1 != NULL, "② 序列化成功");
      if (json1 != NULL) {
        /* 关键检查标准：解析回去之后，那些字段的值必须**一字不差** */
        dsh_settings *s2 = parse(json1, "② 再解析");
        if (s2 != NULL) {
          char *json2 = dsh_settings_to_json(s2);
          ok(json2 != NULL, "② 二次序列化成功");
          if (json2 != NULL) {
            ok(strcmp(json1, json2) == 0,
               "② 序列化必须是**幂等**的（两次存盘逐字节相同，才能 diff / 才能判「变没变」）");
            dsh_release(json2);
          }
          /* 逐个字段核对：这些值必须真的在，而不是「虽然没报错但被丢了」 */
          const char *needles[] = {
              "\"placement\":{\"pillX\":12,\"pillY\":-3,\"edge\":\"left\"}",
              "\"history\":[{\"word\":\"apple\",\"at\":1700000000001}]",
              /* ⚠️ `translate` 也是内核认识的键：它不走「原样搬运」，而是被解析 + 归一化之后
               *    重写 —— 补丁里没提 `autoTranslate` 就落默认值 `true` 并**一起写出来**。
               *    这是「接管一个对象」的可观察后果，所以这一条按**归一化之后**的形状钉。 */
              "\"translate\":{\"enabled\":true,\"targetMode\":\"auto\",\"autoTranslate\":true}",
              "\"someFutureKey\":[1,2,{\"nested\":true}]",
              /* 发音那一节：认识的键被归一化之后写回，不认识的子键原样活下来 */
              "\"accent\":\"uk\"",
              "\"rate\":3",
              "\"systemGainDb\":-1.5",
              "\"futureVoice\":{\"x\":[1,2]}",
          };
          for (size_t i = 0; i < sizeof(needles) / sizeof(needles[0]); i++) {
            char label[160];
            snprintf(label, sizeof(label), "② 必须活下来：%s", needles[i]);
            ok(strstr(json1, needles[i]) != NULL, label);
          }
          /* `defaultLanguage` 要**被清掉**（不给用户自己选择语种的权利）*/
          ok(strstr(json1, "defaultLanguage") == NULL,
             "② ★ 归一化会把 defaultLanguage 清掉（读过配置之后它永远是「没设过」）");
          dsh_settings_free(s2);
        }
        dsh_release(json1);
      }
      dsh_settings_free(s1);
    }
  }
  ok_eq_i64((int64_t)dsh_mem_live_count(), 0, "② 全部释放之后活分配表回到基线");

  /* ── ③ 补丁语义 ───────────────────────────────────────────────────────── */
  {
    dsh_settings *base = parse("{\"closeBehavior\":\"tray\",\"currentDictId\":\"k1\","
                               "\"placement\":{\"pillX\":5}}",
                               "③ 补丁底座");
    if (base != NULL) {
      /* 只改一个键：其余（包括未识别的 placement）必须原样保留 */
      const char *patch = "{\"closeBehavior\":\"quit\"}";
      dsh_settings *s = NULL;
      ok(dsh_settings_apply_patch(base, patch, strlen(patch), &s) == 0, "③ 打补丁成功");
      if (s != NULL) {
        ok(dsh_settings_close_behavior(s) == DSH_CLOSE_QUIT, "③ 补丁改了 closeBehavior");
        ok_eq_str(dsh_settings_current_dict_id(s), "k1", "③ 补丁没提的键必须保持原值");
        char *json = dsh_settings_to_json(s);
        ok(json != NULL && strstr(json, "\"placement\":{\"pillX\":5}") != NULL,
           "③ 补丁没提的**未识别字段**也必须保留");
        if (json != NULL) dsh_release(json);
        /* 底座不许被改动 —— 补丁是「返回新的一份」 */
        ok(dsh_settings_close_behavior(base) == DSH_CLOSE_TRAY,
           "③ 打补丁不许改动原来那一份（原子性）");
        dsh_settings_free(s);
      }

      /* 显式 null = 清空；没出现 = 保持 */
      const char *clear = "{\"currentDictId\":null}";
      s = NULL;
      ok(dsh_settings_apply_patch(base, clear, strlen(clear), &s) == 0, "③ null 补丁成功");
      if (s != NULL) {
        ok_eq_str(dsh_settings_current_dict_id(s), "", "③ 给了 null 必须清空");
        dsh_settings_free(s);
      }

      /* 非法枚举要**夹回**，不是报错（接口定义里写着"内核负责规范化"） */
      const char *bad = "{\"closeBehavior\":\"nonsense\"}";
      s = NULL;
      ok(dsh_settings_apply_patch(base, bad, strlen(bad), &s) == 0, "③ 非法枚举不该让补丁失败");
      if (s != NULL) {
        ok(dsh_settings_close_behavior(s) == DSH_CLOSE_ASK, "③ 非法枚举夹回 ask");
        dsh_settings_free(s);
      }

      /* 补丁不是对象 / 不是合法 JSON → 失败 */
      s = NULL;
      ok(dsh_settings_apply_patch(base, "[1]", 3, &s) != 0, "③ 补丁不是对象必须失败");
      ok(s == NULL, "③ 失败时 out 保持 NULL");
      ok(dsh_settings_apply_patch(base, "{", 1, &s) != 0, "③ 补丁是坏 JSON 必须失败");
      ok(dsh_settings_apply_patch(NULL, "{}", 2, &s) != 0, "③ base 为 NULL 必须失败");

      dsh_settings_free(base);
    }
  }

  /* ── ④ 词库五件事 ─────────────────────────────────────────────────────── */
  {
    dsh_settings *s0 = NULL;
    ok(dsh_settings_default(&s0) == 0, "④ 建默认设置");
    if (s0 != NULL) {
      /* 加：没有路径 / 没有 id 都必须失败（参考实现那个坑的形态） */
      dsh_settings *s1 = NULL;
      ok(dsh_settings_dict_add(s0, "idA", "", NULL, NULL, 0, 0, &s1) != 0,
         "④ 路径是空串 → 必须失败（绝不许记成一本词库）");
      ok(dsh_settings_dict_add(s0, "", "/a.mdx", NULL, NULL, 0, 0, &s1) != 0,
         "④ id 是空串 → 必须失败");
      ok(s1 == NULL, "④ 失败时 out 保持 NULL");

      const char *mdd[2] = {"/a.mdd", ""};
      ok(dsh_settings_dict_add(s0, "idA", "/a.mdx", "a.mdx", mdd, 2, 0, &s1) == 0, "④ 加一本");
      if (s1 != NULL) {
        ok_eq_i64(dsh_settings_dict_count(s1), 1, "④ 加完之后有 1 本");
        const dsh_stored_dict *d = dsh_settings_dict_at(s1, 0);
        ok(d != NULL && strcmp(d->id, "idA") == 0, "④ 新那本的 id");
        ok(d != NULL && d->added_at > 0, "④ addedAt 为 0 时要自动填当前时间");
        ok(d != NULL && d->mdd_count == 1, "④ 空串的资源卷要丢掉");
        /* 同一本再加一次：不许出现两条（按 id 认同一本） */
        dsh_settings *s2 = NULL;
        ok(dsh_settings_dict_add(s1, "idA", "/moved/a.mdx", "a.mdx", NULL, 0, 0, &s2) == 0,
           "④ 重复导入同一本不报错");
        if (s2 != NULL) {
          ok_eq_i64(dsh_settings_dict_count(s2), 1, "④ 同一 id 不许重复出现（id 认内容不认路径）");
          const dsh_stored_dict *d2 = dsh_settings_dict_at(s2, 0);
          ok(d2 != NULL && strcmp(d2->mdx_path, "/moved/a.mdx") == 0,
             "④ 换目录之后再导入 = 更新路径");

          /* 指定当前：不存在的 id 必须失败 */
          dsh_settings *s3 = NULL;
          ok(dsh_settings_dict_set_current(s2, "nope", &s3) != 0,
             "④ 指定一本不存在的词典必须失败（不许悄悄设成不存在的 id）");
          ok(s3 == NULL, "④ 失败时 out 保持 NULL");
          ok(dsh_settings_dict_set_current(s2, "idA", &s3) == 0, "④ 指定存在的 id 成功");
          if (s3 != NULL) {
            ok_eq_str(dsh_settings_current_dict_id(s3), "idA", "④ 当前词典 id");

            /* 改名：空串 = 恢复默认名 */
            dsh_settings *s4 = NULL;
            ok(dsh_settings_dict_rename(s3, "idA", "  我的词典  ", &s4) == 0, "④ 改名");
            if (s4 != NULL) {
              const dsh_stored_dict *d4 = dsh_settings_dict_at(s4, 0);
              ok(d4 != NULL && d4->custom_title != NULL &&
                     strcmp(d4->custom_title, "我的词典") == 0,
                 "④ 改过的名字要裁掉首尾空白");
              ok_eq_str(dsh_settings_dict_display_name(d4, "头里的书名"), "我的词典",
                        "④ 显示名约定：改过的名优先于头里的书名");
              dsh_settings *s5 = NULL;
              ok(dsh_settings_dict_rename(s4, "idA", "", &s5) == 0, "④ 空串 = 恢复默认名");
              if (s5 != NULL) {
                const dsh_stored_dict *d5 = dsh_settings_dict_at(s5, 0);
                ok(d5 != NULL && d5->custom_title == NULL, "④ 恢复默认名之后 customTitle 为空");
                ok_eq_str(dsh_settings_dict_display_name(d5, "头里的书名"), "头里的书名",
                          "④ 显示名约定：没有改过的名 → 用头里的书名");
                ok_eq_str(dsh_settings_dict_display_name(d5, ""), "a.mdx",
                          "④ 显示名约定：头里也没书名 → 用文件名");
                ok_eq_str(dsh_settings_dict_display_name(d5, NULL), "a.mdx",
                          "④ 显示名约定：头标题为 NULL 也不能崩");
                /* 改名一个不存在的 id 必须失败 */
                dsh_settings *s6 = NULL;
                ok(dsh_settings_dict_rename(s5, "nope", "x", &s6) != 0, "④ 改名不存在的 id 失败");
                /* 删 */
                dsh_settings *s7 = NULL;
                ok(dsh_settings_dict_remove(s5, "idA", &s7) == 0, "④ 删除成功");
                if (s7 != NULL) {
                  ok_eq_i64(dsh_settings_dict_count(s7), 0, "④ 删完之后没有词库");
                  ok_eq_str(dsh_settings_current_dict_id(s7), "",
                            "④ 删掉当前词典时要把「当前」一并清空");
                  /* 幂等：再删一次不报错、也不改动 */
                  dsh_settings *s8 = NULL;
                  ok(dsh_settings_dict_remove(s7, "idA", &s8) == 0, "④ 重复删同一本不报错");
                  if (s8 != NULL) {
                    ok_eq_i64(dsh_settings_dict_count(s8), 0, "④ 重复删之后还是 0 本");
                    dsh_settings_free(s8);
                  }
                  dsh_settings_free(s7);
                }
                dsh_settings_free(s6);
                dsh_settings_free(s5);
              }
              dsh_settings_free(s4);
            }
            dsh_settings_free(s3);
          }
          dsh_settings_free(s2);
        }
        dsh_settings_free(s1);
      }
      dsh_settings_free(s0);
    }
  }
  ok_eq_i64((int64_t)dsh_mem_live_count(), 0, "④ 全部释放之后活分配表回到基线");

  /* ── ⑤ 序列化是确定的，而且形状对得上 ─────────────────────────────────── */
  {
    dsh_settings *s = NULL;
    if (dsh_settings_default(&s) == 0 && s != NULL) {
      char *a = dsh_settings_to_json(s);
      char *b = dsh_settings_to_json(s);
      ok(a != NULL && b != NULL && strcmp(a, b) == 0, "⑤ 同一份设置两次序列化逐字节相同");
      if (a != NULL) {
        /* ⚠️ 从第十五轮起设置文件里多了 `speech` 与 `volcengine` 两节
         *    （发音设置与账号级凭据）—— 它们**总是**被写出来，且值已经归一化过：
         *    `accent` 是 auto、`doubaoResourceId`/`doubaoFormat` 与两个音色都填了内置默认值。
         *    这几条默认值是产品约定（用户定的英文 Dacey / 中文 Vivi），改动前先看
         *    docs/design/豆包语音合成接入方案.md。
         * ⚠️ `showFloatingOnStartup` 是 2026-09 加的（窗口级设置，与 `closeBehavior` 同一档），
         *    同样**总是**写出来；默认是 `true`（显示）。
         * ⚠️ 第二十五轮起曾经还有过 `hotkey` 一节 —— 2026-09 已定**整条删掉**
         *    （「本APP不设置热键」），所以现在这里**不许**再出现它。 */
        ok_eq_str(a,
                  "{\"version\":1,\"dictionaries\":[],\"currentDictId\":null,"
                  "\"closeBehavior\":\"ask\","
                  "\"showFloatingOnStartup\":true,"
                  "\"speech\":{\"rate\":0,\"voiceId\":null,\"accent\":\"auto\","
                  "\"doubaoApiKey\":null,\"doubaoResourceId\":\"seed-tts-2.0\","
                  "\"doubaoFormat\":\"mp3\","
                  "\"doubaoSpeakerEn\":\"en_female_dacey_uranus_bigtts\","
                  "\"doubaoSpeakerZh\":\"zh_female_vv_uranus_bigtts\"},"
                  "\"volcengine\":{\"apiKey\":null},"
                  "\"translate\":{\"enabled\":false,\"targetMode\":\"auto\","
                  "\"autoTranslate\":true}}",
                  "⑤ 空设置的 JSON 形状（逐字节）");
        dsh_release(a);
      }
      if (b != NULL) dsh_release(b);
      dsh_settings_free(s);
    }
  }
  {
    /* 一条完整记录的形状（含 null 的 customTitle 与空数组的 mddPaths） */
    dsh_settings *s1 = NULL;
    if (dsh_settings_default(&s1) == 0 && s1 != NULL) {
      dsh_settings *s2 = NULL;
      if (dsh_settings_dict_add(s1, "abc", "/x/y.mdx", "y.mdx", NULL, 0, 1700000000000LL, &s2) ==
          0 && s2 != NULL) {
        char *json = dsh_settings_to_json(s2);
        if (json != NULL) {
          ok_eq_str(json,
                    "{\"version\":1,\"dictionaries\":[{\"id\":\"abc\",\"title\":\"y.mdx\","
                    "\"customTitle\":null,\"mdxPath\":\"/x/y.mdx\",\"mddPaths\":[],"
                    "\"addedAt\":1700000000000}],\"currentDictId\":\"abc\","
                    "\"closeBehavior\":\"ask\","
                    "\"showFloatingOnStartup\":true,"
                    "\"speech\":{\"rate\":0,\"voiceId\":null,\"accent\":\"auto\","
                    "\"doubaoApiKey\":null,\"doubaoResourceId\":\"seed-tts-2.0\","
                    "\"doubaoFormat\":\"mp3\","
                    "\"doubaoSpeakerEn\":\"en_female_dacey_uranus_bigtts\","
                    "\"doubaoSpeakerZh\":\"zh_female_vv_uranus_bigtts\"},"
                    "\"volcengine\":{\"apiKey\":null},"
                    "\"translate\":{\"enabled\":false,\"targetMode\":\"auto\","
                    "\"autoTranslate\":true}}",
                    "⑤ 单条词库记录的 JSON 形状（逐字节）");
          dsh_release(json);
        }
        dsh_settings_free(s2);
      }
      dsh_settings_free(s1);
    }
  }

  /* ── NULL 容错（宿主可能传任何东西）─────────────────────────────────── */
  {
    dsh_settings *s = NULL;
    ok(dsh_settings_parse("{}", 2, NULL) != 0, "out 为 NULL 必须报错");
    ok(dsh_settings_parse(NULL, 0, &s) == 0, "json 为 NULL + len 0 = 全默认（合法）");
    dsh_settings_free(s);
    dsh_settings_free(NULL); /* 合法空操作 */
    ok(dsh_settings_dict_count(NULL) == 0, "count(NULL) → 0");
    ok(dsh_settings_dict_at(NULL, 0) == NULL, "at(NULL) → NULL");
    ok(dsh_settings_dict_by_id(NULL, "x") == NULL, "by_id(NULL) → NULL");
    ok(dsh_settings_close_behavior(NULL) == DSH_CLOSE_ASK, "close_behavior(NULL) → ask");
    ok_eq_str(dsh_settings_current_dict_id(NULL), "", "current_dict_id(NULL) → 空串");
    ok_eq_str(dsh_settings_dict_display_name(NULL, NULL), "", "display_name(NULL) → 空串");
    ok(dsh_settings_to_json(NULL) == NULL, "to_json(NULL) → NULL");
    ok(1, "对 NULL 调用不许崩");
  }

  /* ── 失败路径的报错信息必须是**自己读得出来**的（这是 use-after-free 的保护措施）──
   * ⚠️ 这一条为什么必须有：内核的报错文案是"调用方唯一的线索"，所以失败路径里
   *    `dsh_set_last_error` 常常要带上刚刚那个 id / 路径。而"先 `free` 再拼报错"
   *    会**读已经释放的内存** —— dev 构建下它通常什么都不报，只是一个偶尔乱码的
   *    错误信息（ASan 才照得出来，见：`READ of size 5` 落在一个已 free 的
   *    4120 字节块里）。所以这里**在失败之后立刻把报错读出来核对内容**，
   *    并把它交给内核外的分配（`dsh_last_error_message` 返回的是副本）——
   *    这样"报错信息本身是坏内存"这件事就会在**每个构建**里暴露出来，而不只在 ASan 下。 */
  {
    struct { const char *what; } cases[] = {{"set_current"}, {"rename"}, {"parse"}};
    (void)cases;

    dsh_settings *s = NULL;
    if (dsh_settings_default(&s) == 0 && s != NULL) {
      dsh_settings *out = NULL;
      /* ① 指定一本不存在的词典：报错里应当**回显那个 id**（而不是乱码/空） */
      ok(dsh_settings_dict_set_current(s, "no-such-id-0123456789", &out) != 0,
         "① 指定不存在的词典必须失败");
      {
        const char *msg = dsh_last_error_message();
        ok(msg != NULL && strstr(msg, "no-such-id-0123456789") != NULL,
           "① 失败报错里必须**原样回显**那个 id（先释放再拼报错的话这里就是乱码）");
        if (msg != NULL) dsh_release((void *)msg);
      }
      /* ② 给不存在的词典改名：同样要回显 id */
      out = NULL;
      ok(dsh_settings_dict_rename(s, "no-such-id-abcdef", "x", &out) != 0,
         "② 给不存在的词典改名必须失败");
      {
        const char *msg = dsh_last_error_message();
        ok(msg != NULL && strstr(msg, "no-such-id-abcdef") != NULL,
           "② 改名失败报错里必须原样回显 id");
        if (msg != NULL) dsh_release((void *)msg);
      }
      /* ③ 坏设置 JSON：报错要能说清是"读不动"，而且带得上底层原因 */
      {
        dsh_settings *bad = NULL;
        ok(dsh_settings_parse("{\"a\":}", 7, &bad) != 0, "③ 坏设置必须失败");
        const char *msg = dsh_last_error_message();
        ok(msg != NULL && strstr(msg, "设置文件读不动") != NULL,
           "③ 设置读不动的报错要指明是设置文件的问题");
        if (msg != NULL) dsh_release((void *)msg);
      }
      dsh_settings_free(s);
    }
  }

  /* ── ⑥ 发音设置那一节（`SpeechSettings` + `NormalizeSpeech` 的约定）────────
   * 检查标准逐条照参考实现的 `Settings.NormalizeSpeech`（它自己有一份 C# 侧的实测结果）：
   * 夹范围、非法值退回、trim、`null` 与空串的区别、Key 的镜像。
   * 这些值决定"这次朗读走哪条路"，所以每一条都要钉住。 */
  {
    /* ① 空设置 → 全是内置默认值 */
    dsh_settings *base = NULL;
    if (dsh_settings_default(&base) == 0 && base != NULL) {
      const dsh_speech_settings *sp = dsh_settings_speech(base);
      ok(sp != NULL, "⑥ 拿得到发音设置");
      if (sp != NULL) {
        ok(sp->rate == 0, "⑥ 默认语速 0");
        ok(strcmp(sp->accent, "auto") == 0, "⑥ 默认口音偏好 auto");
        ok(sp->voice_id == NULL, "⑥ 默认不指定离线音色");
        ok(sp->doubao_api_key == NULL, "⑥ 默认没有 Key");
        ok(strcmp(sp->doubao_resource_id, "seed-tts-2.0") == 0, "⑥ 默认 ResourceId");
        ok(strcmp(sp->doubao_format, "mp3") == 0, "⑥ 默认格式 mp3");
        ok(strcmp(sp->doubao_speaker_en, "en_female_dacey_uranus_bigtts") == 0,
           "⑥ 默认英文音色（用户定的 Dacey）");
        ok(strcmp(sp->doubao_speaker_zh, "zh_female_vv_uranus_bigtts") == 0,
           "⑥ 默认中文音色（用户定的 Vivi）");
        ok(!sp->has_loudness_en && !sp->has_loudness_zh, "⑥ 默认没有响度补偿（用内置表）");
      }
      ok(dsh_settings_speech_online_ready(base) == 0,
         "⑥ ★ 没填 Key → 在线那条**不可用**（检查标准只有这一条，没有总开关）");
      ok(dsh_settings_volcengine_api_key(base) == NULL, "⑥ 账号级 Key 也是空");
      dsh_settings_free(base);
    }

    /* ② 归一化：夹范围、非法值退回、trim */
    {
      dsh_settings *s = NULL;
      const char *json =
          "{\"speech\":{\"rate\":99,\"accent\":\"AU\",\"voiceId\":\"  zh-CN-Xiaoxiao  \","
          "\"defaultLanguage\":\"ja\",\"systemGainDb\":-1.5},"
          "\"volcengine\":{\"apiKey\":\"  key-123  \"}}";
      if (dsh_settings_parse(json, strlen(json), &s) == 0 && s != NULL) {
        const dsh_speech_settings *sp = dsh_settings_speech(s);
        ok(sp != NULL && sp->rate == 10, "⑥ ★ 语速越界夹到 10");
        ok(sp != NULL && strcmp(sp->accent, "auto") == 0,
           "⑥ ★ 口音只认小写 uk / us，其余（含大写 AU）一律退回 auto");
        ok(sp != NULL && sp->voice_id != NULL && strcmp(sp->voice_id, "zh-CN-Xiaoxiao") == 0,
           "⑥ 音色 id 两端空白被 trim");
        ok(dsh_settings_speech_online_ready(s) == 1, "⑥ 填了 Key → 在线可用");
        ok(dsh_settings_volcengine_api_key(s) != NULL &&
               strcmp(dsh_settings_volcengine_api_key(s), "key-123") == 0,
           "⑥ ★ Key 两端空白被 trim（复制粘贴最常见的坑）");
        ok(sp != NULL && sp->doubao_api_key != NULL &&
               strcmp(sp->doubao_api_key, "key-123") == 0,
           "⑥ ★ 镜像：账号级那把 Key 也出现在 speech.doubaoApiKey（两处永远同值）");
        {
          char *text = dsh_settings_to_json(s);
          ok(text != NULL && strstr(text, "defaultLanguage") == NULL,
             "⑥ ★ 归一化把 defaultLanguage 清掉（不再让旧选择绑着用户）");
          ok(text != NULL && strstr(text, "\"systemGainDb\":-1.5") != NULL,
             "⑥ 没建模的子键（systemGainDb）原样活下来");
          ok(text != NULL && strstr(text, "\"rate\":10") != NULL, "⑥ 写回去的是夹好的值");
          if (text != NULL) dsh_release(text);
        }
        dsh_settings_free(s);
      } else {
        ok(0, "⑥ 解析带发音设置的 JSON");
      }
    }

    /* ③ 反向镜像：只有旧的 `speech.doubaoApiKey`（升级上来的用户）*/
    {
      dsh_settings *s = NULL;
      const char *json = "{\"speech\":{\"doubaoApiKey\":\"old-key\"}}";
      if (dsh_settings_parse(json, strlen(json), &s) == 0 && s != NULL) {
        ok(dsh_settings_volcengine_api_key(s) != NULL &&
               strcmp(dsh_settings_volcengine_api_key(s), "old-key") == 0,
           "⑥ ★ 升级路径：旧字段里的 Key 会被搬进账号级字段（否则用户「以前能用，现在没声音」）");
        dsh_settings_free(s);
      }
    }

    /* ④ 两个音色：**NULL 填默认值，空串留着**（清空保存再打开不该被顶回来）*/
    {
      dsh_settings *s = NULL;
      const char *json = "{\"speech\":{\"doubaoSpeakerEn\":\"\",\"doubaoSpeakerZh\":\"my-voice\"}}";
      if (dsh_settings_parse(json, strlen(json), &s) == 0 && s != NULL) {
        const dsh_speech_settings *sp = dsh_settings_speech(s);
        ok(sp != NULL && sp->doubao_speaker_en != NULL && sp->doubao_speaker_en[0] == '\0',
           "⑥ ★ 音色写成空串 → **留着**（那是用户的明确选择，不是「没设过」）");
        ok(sp != NULL && sp->doubao_speaker_zh != NULL &&
               strcmp(sp->doubao_speaker_zh, "my-voice") == 0,
           "⑥ 设过的音色照用");
        dsh_settings_free(s);
      }
      /* 字段不出现（NULL）→ 用默认值 */
      dsh_settings *t = NULL;
      const char *json2 = "{\"speech\":{\"rate\":1}}";
      if (dsh_settings_parse(json2, strlen(json2), &t) == 0 && t != NULL) {
        const dsh_speech_settings *sp = dsh_settings_speech(t);
        ok(sp != NULL && strcmp(sp->doubao_speaker_en, "en_female_dacey_uranus_bigtts") == 0,
           "⑥ ★ 字段**从没设过** → 填内置默认音色（与空串是两回事）");
        dsh_settings_free(t);
      }
    }

    /* ⑤ 响度补偿夹到官方范围 -50..100 */
    {
      dsh_settings *s = NULL;
      const char *json =
          "{\"speech\":{\"doubaoLoudnessEn\":999,\"doubaoLoudnessZh\":-999}}";
      if (dsh_settings_parse(json, strlen(json), &s) == 0 && s != NULL) {
        const dsh_speech_settings *sp = dsh_settings_speech(s);
        ok(sp != NULL && sp->has_loudness_en && sp->loudness_en == 100, "⑥ 响度上限夹到 100");
        ok(sp != NULL && sp->has_loudness_zh && sp->loudness_zh == -50, "⑥ 响度下限夹到 -50");
        dsh_settings_free(s);
      }
    }

    /* ⑥ 改词库 / 改当前词典之后，发音设置**不许丢**（那条路走的是副本）*/
    {
      dsh_settings *s1 = NULL;
      const char *json = "{\"speech\":{\"accent\":\"us\",\"systemGainDb\":2.5}}";
      if (dsh_settings_parse(json, strlen(json), &s1) == 0 && s1 != NULL) {
        dsh_settings *s2 = NULL;
        if (dsh_settings_dict_add(s1, "abc", "/x/y.mdx", "y.mdx", NULL, 0, 1, &s2) == 0 &&
            s2 != NULL) {
          const dsh_speech_settings *sp = dsh_settings_speech(s2);
          ok(sp != NULL && strcmp(sp->accent, "us") == 0,
             "⑥ ★ 加一本词典之后，口音偏好还在（副本要带上发音那一节）");
          {
            char *text = dsh_settings_to_json(s2);
            ok(text != NULL && strstr(text, "\"systemGainDb\":2.5") != NULL,
               "⑥ ★ 没建模的子键也要跟着副本走（漏了就是「设置莫名丢了」）");
            if (text != NULL) dsh_release(text);
          }
          dsh_settings_free(s2);
        } else {
          ok(0, "⑥ 加一本词典");
        }
        dsh_settings_free(s1);
      }
    }

    /* ⑦ 类型不对时不许崩（`speech` 不是对象就整段当默认）*/
    {
      dsh_settings *s = NULL;
      const char *json = "{\"speech\":[1,2,3],\"volcengine\":\"nope\"}";
      if (dsh_settings_parse(json, strlen(json), &s) == 0 && s != NULL) {
        const dsh_speech_settings *sp = dsh_settings_speech(s);
        ok(sp != NULL && strcmp(sp->accent, "auto") == 0,
           "⑥ `speech` 不是对象 → 当默认值（不崩）");
        dsh_settings_free(s);
      } else {
        ok(0, "⑥ speech 类型不对时也应当能解析（整段当默认）");
      }
    }
  }

  /* ══ ⑦ 补丁**能改到 speech / volcengine**（2026-09 修掉的一个真 bug）════════
   *
   * 这一节钉的是一个曾经**不报错地失效**的路：`dsh_settings_apply_patch` 把
   * `speech` / `volcengine` 当成"内核认识的顶层键"直接跳过了，于是
   * **界面上改口音 / 改音色 / 填 Key 一条都存不进去**，而且不报错 ——
   * 打进去 `{"speech":{"accent":"us"}}`，读回来还是 `auto`。
   *
   * 两条检查标准缺一不可：
   *   · **改了要生效**（否则就是上面那个 bug）；
   *   · **没提的键不许动**（否则"改语速"会把 Key / 音色一起打回默认值，那是"改一处丢一片"）。
   */
  {
    dsh_settings *base = parse("{\"speech\":{\"rate\":3,\"accent\":\"uk\",\"voiceId\":\"v1\","
                               "\"doubaoApiKey\":\"k-keep\",\"doubaoSpeakerEn\":\"sp-en\"},"
                               "\"volcengine\":{\"apiKey\":\"k-keep\"}}",
                               "⑦ 带发音那节的底座");
    if (base != NULL) {
      const dsh_speech_settings *b = dsh_settings_speech(base);
      ok(b != NULL && strcmp(b->accent, "uk") == 0, "⑦ 底座的口音是 uk");

      /* ① 只改口音：它要变，**其余一样都不许动** */
      const char *accent = "{\"speech\":{\"accent\":\"us\"}}";
      dsh_settings *s = NULL;
      ok(dsh_settings_apply_patch(base, accent, strlen(accent), &s) == 0, "⑦ 打口音补丁成功");
      if (s != NULL) {
        const dsh_speech_settings *sp = dsh_settings_speech(s);
        ok(sp != NULL && strcmp(sp->accent, "us") == 0,
           "⑦ ★ 补丁真的改到了 speech.accent（曾经被不报错地丢弃）");
        ok(sp != NULL && sp->rate == 3, "⑦ ★ 补丁没提的 rate 必须保持原值（merge 语义）");
        ok(sp != NULL && sp->voice_id != NULL && strcmp(sp->voice_id, "v1") == 0,
           "⑦ ★ 补丁没提的 voiceId 必须保持原值");
        ok(sp != NULL && sp->doubao_api_key != NULL && strcmp(sp->doubao_api_key, "k-keep") == 0,
           "⑦ ★ 补丁没提的 Key 必须保持原值");
        ok_eq_str(dsh_settings_volcengine_api_key(s), "k-keep",
                  "⑦ ★ 镜像出来的账号级 Key 也不许被抹掉");
        dsh_settings_free(s);
      }

      /* ② 非法口音 → 夹回 auto（与读文件那条路同一条约定） */
      const char *bad = "{\"speech\":{\"accent\":\"nonsense\"}}";
      s = NULL;
      ok(dsh_settings_apply_patch(base, bad, strlen(bad), &s) == 0, "⑦ 非法口音不该让补丁失败");
      if (s != NULL) {
        const dsh_speech_settings *sp = dsh_settings_speech(s);
        ok(sp != NULL && strcmp(sp->accent, "auto") == 0, "⑦ 非法口音夹回 auto");
        dsh_settings_free(s);
      }

      /* ③ 空串音色是**用户的明确选择**，必须留着（NULL 才是"没设过"） */
      const char *empty = "{\"speech\":{\"doubaoSpeakerEn\":\"\"}}";
      s = NULL;
      ok(dsh_settings_apply_patch(base, empty, strlen(empty), &s) == 0, "⑦ 空音色补丁成功");
      if (s != NULL) {
        const dsh_speech_settings *sp = dsh_settings_speech(s);
        ok(sp != NULL && sp->doubao_speaker_en != NULL && sp->doubao_speaker_en[0] == '\0',
           "⑦ ★ 空串音色要留着（不能被当成「没设过」再填回默认音色）");
        dsh_settings_free(s);
      }

      /* ④ 账号级 Key：改了要**镜像**回 speech.doubaoApiKey */
      const char *key = "{\"volcengine\":{\"apiKey\":\"k-new\"}}";
      s = NULL;
      ok(dsh_settings_apply_patch(base, key, strlen(key), &s) == 0, "⑦ 改 Key 成功");
      if (s != NULL) {
        ok_eq_str(dsh_settings_volcengine_api_key(s), "k-new", "⑦ 账号级 Key 改了");
        const dsh_speech_settings *sp = dsh_settings_speech(s);
        ok(sp != NULL && sp->doubao_api_key != NULL && strcmp(sp->doubao_api_key, "k-new") == 0,
           "⑦ ★ 两个出口是同一个值（镜像）");
        dsh_settings_free(s);
      }

      /* ⑤ 未建模的子键：打两次同名补丁，存盘里**只能出现一次** */
      const char *extra1 = "{\"speech\":{\"someExtra\":1}}";
      s = NULL;
      ok(dsh_settings_apply_patch(base, extra1, strlen(extra1), &s) == 0, "⑦ 未建模子键补丁成功");
      if (s != NULL) {
        const char *extra2 = "{\"speech\":{\"someExtra\":2}}";
        dsh_settings *s2 = NULL;
        ok(dsh_settings_apply_patch(s, extra2, strlen(extra2), &s2) == 0, "⑦ 再打一次同名补丁");
        if (s2 != NULL) {
          char *json = dsh_settings_to_json(s2);
          ok(json != NULL && strstr(json, "\"someExtra\":2") != NULL,
             "⑦ ★ 未建模子键被**替换**（值是新的那个）");
          if (json != NULL) {
            int hits = 0;
            for (const char *p = json; (p = strstr(p, "\"someExtra\"")) != NULL; p++) hits++;
            ok(hits == 1, "⑦ ★ 同一个未建模子键只许出现一次（否则存盘里有两个同名键）");
            dsh_release(json);
          }
          dsh_settings_free(s2);
        }
        dsh_settings_free(s);
      }

      /* ⑥ 底座不许被改动（补丁的原子性也适用于这两节） */
      ok(b != NULL && strcmp(b->accent, "uk") == 0, "⑦ ★ 打了一串补丁之后底座还是 uk");
      dsh_settings_free(base);
    }
  }

  /* ══ ⑧ 窗口级设置：启动时显不显示悬浮窗（`showFloatingOnStartup`）════════════
   *
   * 这一节原来钉的是"划词与全局快捷键"（第二十五轮加的 `hotkey.enabled` / `chord`）。
   * **2026-09 已定把全局热键整条去掉**（原话：「本APP不设置热键，如果之前做过就去掉
   * 这个功能」），于是那一节连同内核里的组合键规范化器一起删了、这一节的编号让给了
   * 现在这条新设置 —— 检查标准照旧是内核那几条老规矩：
   *   · 默认值唯一来源在"读设置文件"那条路上；
   *   · 坏值不报错、落回默认、**但要说清原因**；
   *   · 补丁只改它提到的键（"改别的设置"不许顺手把这个开关顶回默认）；
   *   · 走副本的那条路（加/删词库）不许把它弄丢。
   */
  {
    /* ① 默认值：**显示**（关掉才是"启动时不显示"，默认不能变成"看不到窗口"） */
    dsh_settings *s = NULL;
    if (dsh_settings_default(&s) == 0 && s != NULL) {
      ok(dsh_settings_show_floating_on_startup(s) == 1,
         "⑧ 默认是**显示**悬浮窗（关掉才不显示）");
      dsh_settings_free(s);
    } else {
      ok(0, "⑧ 建默认设置");
    }

    /*
     * ①b ★ 设置文件里**没有这个键**时也要按默认（显示）——
     *     老版本写的设置、手写的种子、迁移过来的 `%APPDATA%\查词\settings.json`
     *     都是这种形状，而它们走的是**正常解析那条路**（不是"没有设置文件"那条）。
     *     这一条是 2026-09 补的：当时只在"没有设置文件"那条分支里落了默认值，
     *     于是"没设过"被 memset 成的 0 当成 `false` —— **凭空关掉了启动显示**，
     *     而且第一次跑还会把 `false` 写回设置文件（下一跑窗口真的不显示）。
     *     D 级 gate 抓到的是那个症状（外壳层不可见），根因就在这里。
     */
    {
      const char *no_key = "{\"version\":1,\"closeBehavior\":\"tray\"}";
      dsh_settings *s1 = NULL;
      ok(dsh_settings_parse(no_key, strlen(no_key), &s1) == 0 && s1 != NULL, "⑧ 读一份没有这个键的设置");
      if (s1 != NULL) {
        ok(dsh_settings_show_floating_on_startup(s1) == 1,
           "⑧ ★ 没有这个键 → 按默认**显示**（不是被 memset 的 0 当成 false）");
        ok(dsh_settings_close_behavior(s1) == DSH_CLOSE_TRAY, "⑧ 而同一份设置里别的键照旧生效");
        char *json = dsh_settings_to_json(s1);
        ok(json != NULL && strstr(json, "\"showFloatingOnStartup\":true") != NULL,
           "⑧ ★ 存盘时写回去的也是 true（不许把'没设过'固化成 false）");
        if (json != NULL) dsh_release(json);
        dsh_settings_free(s1);
      }
    }

    /* ② 读设置文件：写进去什么就读回什么 */
    {
      const char *off = "{\"showFloatingOnStartup\":false}";
      dsh_settings *s1 = NULL;
      ok(dsh_settings_parse(off, strlen(off), &s1) == 0 && s1 != NULL, "⑧ 解析「关掉」的设置");
      if (s1 != NULL) {
        ok(dsh_settings_show_floating_on_startup(s1) == 0, "⑧ 关掉读回来还是关的");
        dsh_settings_free(s1);
      }
      const char *on = "{\"showFloatingOnStartup\":true}";
      dsh_settings *s2 = NULL;
      if (dsh_settings_parse(on, strlen(on), &s2) == 0 && s2 != NULL) {
        ok(dsh_settings_show_floating_on_startup(s2) == 1, "⑧ 写 true 读回来是显示");
        dsh_settings_free(s2);
      }
      /* 序列化里**总是**有这一条（壳读不到它就只能猜） */
      dsh_settings *s3 = NULL;
      if (dsh_settings_parse(off, strlen(off), &s3) == 0 && s3 != NULL) {
        char *json = dsh_settings_to_json(s3);
        ok(json != NULL && strstr(json, "\"showFloatingOnStartup\":false") != NULL,
           "⑧ 存盘时**总是**写出这一条（关掉的那个值也写得出来）");
        if (json != NULL) dsh_release(json);
        dsh_settings_free(s3);
      }
    }

    /* ③ 类型不对：**不失败**（设置仍然可用）、落回默认，并且**说清原因** */
    {
      const char *bad = "{\"showFloatingOnStartup\":\"no\"}";
      dsh_settings *s = NULL;
      ok(dsh_settings_parse(bad, strlen(bad), &s) == 0 && s != NULL,
         "⑧ 坏类型时**不失败**（一份能用的设置不该为一个开关读不进来）");
      if (s != NULL) {
        ok(dsh_settings_show_floating_on_startup(s) == 1, "⑧ 坏类型 → 落回默认（显示）");
        const char *why = dsh_last_error_message();
        ok(why != NULL && strstr(why, "showFloatingOnStartup") != NULL,
           "⑧ ★ 落回默认时**要说清原因**（不许不声不响）");
        if (why != NULL) dsh_release((void *)why);
        dsh_settings_free(s);
      }
    }

    /* ④ ★ 补丁的 merge 语义：只改它提到的键（这正是 `settingsSet` 那个 bug 的同一类） */
    {
      dsh_settings *base = NULL;
      if (dsh_settings_default(&base) == 0 && base != NULL) {
        dsh_settings *s1 = NULL;
        const char *off = "{\"showFloatingOnStartup\":false}";
        ok(dsh_settings_apply_patch(base, off, strlen(off), &s1) == 0 && s1 != NULL,
           "⑧ 打一条「关掉」的补丁");
        if (s1 != NULL) {
          ok(dsh_settings_show_floating_on_startup(s1) == 0, "⑧ ★ 关掉了");
          /* 再打一条**跟它无关**的补丁：不许把它顶回默认 */
          dsh_settings *s2 = NULL;
          const char *other = "{\"closeBehavior\":\"tray\"}";
          ok(dsh_settings_apply_patch(s1, other, strlen(other), &s2) == 0 && s2 != NULL,
             "⑧ 再打一条只改关闭行为的补丁");
          if (s2 != NULL) {
            ok(dsh_settings_show_floating_on_startup(s2) == 0,
               "⑧ ★ 补丁没提它 → **不许**顶回默认（否则用户关掉的显示会自己回来）");
            ok(dsh_settings_close_behavior(s2) == DSH_CLOSE_TRAY, "⑧ 而那条补丁确实生效了");
            /* 显式 null = 回到默认（与 `closeBehavior` 那条一致） */
            dsh_settings *s3 = NULL;
            const char *reset = "{\"showFloatingOnStartup\":null}";
            if (dsh_settings_apply_patch(s2, reset, strlen(reset), &s3) == 0 && s3 != NULL) {
              ok(dsh_settings_show_floating_on_startup(s3) == 1,
                 "⑧ 显式 null = 回到默认（显示）");
              dsh_settings_free(s3);
            } else {
              ok(0, "⑧ 打一条 null 的补丁");
            }
            dsh_settings_free(s2);
          }
          dsh_settings_free(s1);
        }
      }
      dsh_settings_free(base);
    }

    /* ⑤ ★ 走副本的那条路（加词库）不许把它弄丢 */
    {
      dsh_settings *base = NULL;
      const char *json = "{\"showFloatingOnStartup\":false}";
      if (dsh_settings_parse(json, strlen(json), &base) == 0 && base != NULL) {
        dsh_settings *after = NULL;
        ok(dsh_settings_dict_add(base, "abc", "/x/y.mdx", "y.mdx", NULL, 0, 1, &after) == 0 &&
           after != NULL,
           "⑧ 往词库里加一本");
        if (after != NULL) {
          ok(dsh_settings_show_floating_on_startup(after) == 0,
             "⑧ ★ 加词库之后「启动时不显示」还在（clone 漏了这一步就会自己变回显示）");
          dsh_settings_free(after);
        }
        dsh_settings_free(base);
      }
    }
  }

  /* ══ ⑨ "当前词典"的两条顺位规则（第二十九轮按参考实现补齐）══════════════════
   *
   * 这两条是**业务规则**，出处是参考实现的 `Settings.cs`：
   *   · `AddDictionary`    —— `if (string.IsNullOrEmpty(CurrentDictId)) CurrentDictId = dict.Id;`
   *   · `RemoveDictionary` —— `CurrentDictId = Dictionaries.Count > 0 ? Dictionaries[0].Id : null;`
   *
   * 第一版两条都没有：加完第一本词典之后**当前仍然是空的**，于是用户"加进来"之后
   * 第一次查词得到的是「还没指定当前词典」；删掉当前那本则一律清空。
   * ⚠️ 这两条是**C 级诊断脚本**第一次上手就暴露来的——
   *    那一轮之前，"加词典 → 查词"这条路在真实程序里根本没人走过。
   */
  {
    /* ① 往空词库里加第一本：它**就是**当前 */
    dsh_settings *s = NULL;
    if (dsh_settings_default(&s) == 0 && s != NULL) {
      dsh_settings *a = NULL;
      ok(dsh_settings_dict_add(s, "id-one", "/x/one.mdx", "one.mdx", NULL, 0, 1, &a) == 0 &&
         a != NULL,
         "⑨ 往空词库里加一本");
      if (a != NULL) {
        ok_eq_str(dsh_settings_current_dict_id(a), "id-one",
                  "⑨ ★ 加完第一本之后**它就是当前**（参考实现 `AddDictionary` 同一条）");
        /* ② 再加一本：当前**不动**（只有空的时候才顺手指派） */
        dsh_settings *b = NULL;
        ok(dsh_settings_dict_add(a, "id-two", "/x/two.mdx", "two.mdx", NULL, 0, 2, &b) == 0 &&
           b != NULL,
           "⑨ 再加一本");
        if (b != NULL) {
          ok_eq_str(dsh_settings_current_dict_id(b), "id-one",
                    "⑨ ★ 第二本不会把当前抢走（当前只在**空**的时候指派）");
          /* ③ 删掉当前那本 → 顺位给剩下的第一本 */
          dsh_settings *c = NULL;
          ok(dsh_settings_dict_remove(b, "id-one", &c) == 0 && c != NULL, "⑨ 删掉当前那本");
          if (c != NULL) {
            ok_eq_str(dsh_settings_current_dict_id(c), "id-two",
                      "⑨ ★ 删掉当前之后**顺位给剩下的第一本**（不是清空）");
            /* ④ 删掉非当前那本 → 当前不动 */
            dsh_settings *d1 = NULL;
            ok(dsh_settings_dict_add(c, "id-three", "/x/three.mdx", "three.mdx", NULL, 0, 3,
                                     &d1) == 0 &&
               d1 != NULL,
               "⑨ 再加第三本（当前仍是 id-two）");
            if (d1 != NULL) {
              dsh_settings *d2 = NULL;
              ok(dsh_settings_dict_remove(d1, "id-three", &d2) == 0 && d2 != NULL,
                 "⑨ 删掉一本**不是当前**的");
              if (d2 != NULL) {
                ok_eq_str(dsh_settings_current_dict_id(d2), "id-two",
                          "⑨ 删掉别的词典不会动当前");
                /* ⑤ 把剩下的都删光 → 当前才清空 */
                dsh_settings *e1 = NULL;
                dsh_settings *e2 = NULL;
                if (dsh_settings_dict_remove(d2, "id-two", &e1) == 0 && e1 != NULL) {
                  /* ⚠️ 读接口把"没指定"表达成**空串**（不是 NULL）——见 `dsh_settings.h`
                   *    那句"当前词典 id（可能是空串 = 没指定）"。第一版拿 NULL 比，于是
                   *    这条断言恒红，而内核其实是对的。 */
                  ok_eq_str(dsh_settings_current_dict_id(e1), "",
                            "⑨ ★ 一本都不剩了，当前才清空（不是留一个不在清单里的 id）");
                } else {
                  ok(0, "⑨ 删掉最后一本");
                }
                if (e1 != NULL) dsh_settings_free(e1);
                dsh_settings_free(e2);
                dsh_settings_free(d2);
              }
              dsh_settings_free(d1);
            }
            dsh_settings_free(c);
          }
          dsh_settings_free(b);
        }
        dsh_settings_free(a);
      }
      dsh_settings_free(s);
    } else {
      ok(0, "⑨ 建默认设置");
    }
  }

  /* ══ ⑩ 机器翻译那一节（第五十六轮建模）════════════════════════════════════
   *
   * 约定逐字照参考实现的 `TranslateSettings`（`Models.cs`）+ `Normalize`：
   *   · 总开关**默认关**（要联网、要把文本发给第三方）；「查不到时自动翻译」**默认开**；
   *   · `targetMode` 只认 `auto` / `zh` / `en` —— 认不出**落回 auto**（不报错，
   *     但把原因写进 `last_error`），因为它是从设置文件里读出来的，
   *     脏数据绝不许变成"发一个乱码目标语种给服务端"；
   *   · 补丁语义与别的节**逐字相同**：没提到的子键不改（"只关掉开关"不许把目标语种打回 auto）；
   *   · 没建模的子键（老配置残留的 `dictionaryFirst`）原样活下来。
   */
  {
    /* ① 默认值（读接口） */
    dsh_settings *base = NULL;
    if (dsh_settings_default(&base) == 0 && base != NULL) {
      const dsh_translate_settings *tr = dsh_settings_translate(base);
      ok(tr != NULL, "⑩ 拿得到翻译设置");
      if (tr != NULL) {
        ok(tr->enabled == 0, "⑩ ★ 总开关**默认关**（要联网 + 把文本发给第三方，得用户主动打开）");
        ok(tr->auto_translate == 1, "⑩ ★ 「查不到时自动翻译」**默认开**（D12）");
        ok_eq_str(tr->target_mode, "auto", "⑩ 目标语种默认 auto（中文译英、其余译中）");
      }
      dsh_settings_free(base);
    } else {
      ok(0, "⑩ 建默认设置");
    }

    /* ② 归一化：只认 auto / zh / en，大小写不敏感，认不出落回 auto 并说明原因 */
    {
      dsh_settings *s = NULL;
      const char *json = "{\"translate\":{\"enabled\":true,\"targetMode\":\"ZH\","
                         "\"autoTranslate\":false,\"dictionaryFirst\":true}}";
      if (dsh_settings_parse(json, strlen(json), &s) == 0 && s != NULL) {
        const dsh_translate_settings *tr = dsh_settings_translate(s);
        ok(tr != NULL && tr->enabled == 1, "⑩ 开关读得进来");
        ok(tr != NULL && tr->auto_translate == 0, "⑩ ★ 自动翻译关得掉（默认开，但用户能关）");
        ok_eq_str(tr != NULL ? tr->target_mode : NULL, "zh",
                  "⑩ ★ 目标语种归一化：大写 `ZH` → `zh`");
        {
          char *text = dsh_settings_to_json(s);
          ok(text != NULL && strstr(text, "\"targetMode\":\"zh\"") != NULL,
             "⑩ 写回去的是归一化之后的值");
          /* `dictionaryFirst` 是参考实现 D13 删掉的那个开关：内核不建模它，
           * 但**不许**因为它不认识就把它抹掉（与 speech 那一节的未建模子键同一条纪律）*/
          ok(text != NULL && strstr(text, "\"dictionaryFirst\":true") != NULL,
             "⑩ ★ 没建模的子键（老配置残留的 dictionaryFirst）原样活下来");
          if (text != NULL) dsh_release(text);
        }
        /* 认不出的值：落回 auto，而且**必须说出原因**（别不声不响）*/
        {
          dsh_settings *bad = NULL;
          const char *bad_json = "{\"translate\":{\"targetMode\":\"klingon\"}}";
          ok(dsh_settings_parse(bad_json, strlen(bad_json), &bad) == 0 && bad != NULL,
             "⑩ 认不出的 targetMode 不该让整份设置读不动");
          if (bad != NULL) {
            const dsh_translate_settings *bt = dsh_settings_translate(bad);
            ok_eq_str(bt != NULL ? bt->target_mode : NULL, "auto",
                      "⑩ ★ 认不出的目标语种落回 auto（不是报错、也不是照发）");
            {
              const char *msg = dsh_last_error_message();
              ok(msg != NULL && strstr(msg, "targetMode") != NULL,
                 "⑩ ★ 落回默认时**说得出原因**（`last_error` 里点名是哪个键）");
              if (msg != NULL) dsh_release((void *)msg);
            }
            dsh_settings_free(bad);
          }
        }
        /* 补丁语义：只发 enabled 时，**目标语种与自动翻译都不许动** */
        {
          const char *patch = "{\"translate\":{\"enabled\":false}}";
          dsh_settings *after = NULL;
          ok(dsh_settings_apply_patch(s, patch, strlen(patch), &after) == 0 && after != NULL,
             "⑩ 打一个只提 enabled 的补丁");
          if (after != NULL) {
            const dsh_translate_settings *at = dsh_settings_translate(after);
            ok(at != NULL && at->enabled == 0, "⑩ 补丁改到了开关");
            ok_eq_str(at != NULL ? at->target_mode : NULL, "zh",
                      "⑩ ★ 没提到的子键**保持原值**（`zh` 不许被打回 auto）");
            ok(at != NULL && at->auto_translate == 0,
               "⑩ ★ 「自动翻译」关着这件事也不许被补丁顶回默认");
            /* 改词库那条路走的是副本 —— 翻译设置同样不许在那条路上丢 */
            dsh_settings *grown = NULL;
            ok(dsh_settings_dict_add(after, "abc", "/x/y.mdx", "y.mdx", NULL, 0, 1, &grown) == 0 &&
               grown != NULL,
               "⑩ 往词库里加一本");
            if (grown != NULL) {
              const dsh_translate_settings *gt = dsh_settings_translate(grown);
              ok(gt != NULL && gt->enabled == 0 && gt->auto_translate == 0 &&
                 gt->target_mode != NULL && strcmp(gt->target_mode, "zh") == 0,
                 "⑩ ★ 加词库之后翻译三项都还在（clone 漏了这一步就会打回默认）");
              dsh_settings_free(grown);
            }
            dsh_settings_free(after);
          }
        }
        dsh_settings_free(s);
      } else {
        ok(0, "⑩ 解析带翻译设置的 JSON");
      }
    }

    /* ③ 类型不对：`translate` 不是对象要**报错**（不许当没看见，那会让用户设置不报错地消失）*/
    {
      dsh_settings *bad = NULL;
      ok(dsh_settings_parse("{\"translate\":[]}", 16, &bad) != 0,
         "⑩ `translate` 是数组 → 拒绝解析");
      if (bad != NULL) dsh_settings_free(bad);
    }
  }

  /* ══ ⑪ 词典排序（移动）—— 2026-09 新加，0.2.0 专有 ═══════════════════════════
   *
   * ★ 这一条**没有参考实现可比**：参考实现的 `Settings` 里没有"移动/排序"这个功能，
   *   所以语义是这一轮定的（写在 `abi/lookup.abi.json` 与 `dsh_settings.h` 里）：
   *     · delta < 0 往前、> 0 往后、0 不动；
   *     · **到边界夹住**（清单原样返回，仍算成功）—— 与界面上两颗按钮在头尾置灰是一套；
   *     · id 不在清单里 → 报错；**当前词典不受影响**（它记的是 id 不是下标）。
   *   ⚠️ 顺序不只是显示顺序：「当前词典」那格为空时若干接口**兜底取第一本**，
   *     所以这条测试也顺手钉住"移动不会把当前词典搞丢/换掉"。
   */
  {
    dsh_settings *base = NULL;
    if (dsh_settings_default(&base) == 0 && base != NULL) {
      dsh_settings *s1 = NULL, *s2 = NULL, *s3 = NULL;
      if (dsh_settings_dict_add(base, "id-a", "/x/a.mdx", "a.mdx", NULL, 0, 1, &s1) == 0 &&
          s1 != NULL && dsh_settings_dict_add(s1, "id-b", "/x/b.mdx", "b.mdx", NULL, 0, 2, &s2) == 0 &&
          s2 != NULL && dsh_settings_dict_add(s2, "id-c", "/x/c.mdx", "c.mdx", NULL, 0, 3, &s3) == 0 &&
          s3 != NULL) {
        /* 当前是加第一本时指派的 id-a，下面每一次移动之后都不许变 */
        ok_eq_str(dsh_settings_current_dict_id(s3), "id-a", "⑪ 三本就位，当前是 id-a");

        /* ① 第三本往前挪一位：a b c → a c b */
        {
          dsh_settings *m = NULL;
          ok(dsh_settings_dict_move(s3, "id-c", -1, &m) == 0 && m != NULL, "⑪ 第三本往前挪一位");
          if (m != NULL) {
            ok_eq_str(dsh_settings_dict_at(m, 0)->id, "id-a", "⑪ 挪完第 1 位还是 a");
            ok_eq_str(dsh_settings_dict_at(m, 1)->id, "id-c", "⑪ ★ 挪完第 2 位是 c（与 b 换了）");
            ok_eq_str(dsh_settings_dict_at(m, 2)->id, "id-b", "⑪ 挪完第 3 位是 b");
            ok_eq_str(dsh_settings_current_dict_id(m), "id-a", "⑪ ★ 移动不许动当前词典");

            /* ② 再把它往后挪 5 位：**夹到末尾**（不是报错、也不是绕回去） */
            dsh_settings *n = NULL;
            ok(dsh_settings_dict_move(m, "id-c", 5, &n) == 0 && n != NULL,
               "⑪ 往后挪 5 位（超出范围）仍然成功");
            if (n != NULL) {
              ok_eq_str(dsh_settings_dict_at(n, 0)->id, "id-a", "⑪ 夹住之后第 1 位 a");
              ok_eq_str(dsh_settings_dict_at(n, 1)->id, "id-b", "⑪ 夹住之后第 2 位 b");
              ok_eq_str(dsh_settings_dict_at(n, 2)->id, "id-c", "⑪ ★ 超出范围夹到**末尾**，没有绕回去");

              /* ③ 第一本再往前：原样（夹住） */
              dsh_settings *p = NULL;
              ok(dsh_settings_dict_move(n, "id-a", -1, &p) == 0 && p != NULL,
                 "⑪ 第一本再往前挪一位（边界）");
              if (p != NULL) {
                ok_eq_str(dsh_settings_dict_at(p, 0)->id, "id-a", "⑪ ★ 第一本往前挪 = 清单原样");
                /* ④ 最后一本再往后：原样 */
                dsh_settings *q = NULL;
                ok(dsh_settings_dict_move(p, "id-c", 1, &q) == 0 && q != NULL,
                   "⑪ 最后一本再往后挪一位（边界）");
                if (q != NULL) {
                  ok_eq_str(dsh_settings_dict_at(q, 2)->id, "id-c", "⑪ ★ 最后一本往后挪 = 清单原样");
                  /* ⑤ delta 0：不动 */
                  dsh_settings *r = NULL;
                  ok(dsh_settings_dict_move(q, "id-b", 0, &r) == 0 && r != NULL, "⑪ delta 0");
                  if (r != NULL) {
                    ok_eq_str(dsh_settings_dict_at(r, 1)->id, "id-b", "⑪ ★ delta 0 = 原地不动");
                    /* ⑥ 不存在的 id：报错，且 out 保持 NULL */
                    dsh_settings *bad = NULL;
                    ok(dsh_settings_dict_move(r, "nope", 1, &bad) != 0,
                       "⑪ ★ 挪一本不在清单里的词典必须失败");
                    ok(bad == NULL, "⑪ 失败时 out 保持 NULL");

                    /* ⑦ **真的存进去了**：序列化之后再解析回来，顺序还是新的那个 */
                    {
                      char *json = dsh_settings_to_json(r);
                      dsh_settings *back = NULL;
                      ok(json != NULL, "⑪ 序列化");
                      if (json != NULL) {
                        ok(dsh_settings_parse(json, strlen(json), &back) == 0 && back != NULL,
                           "⑪ 解析回来");
                        if (back != NULL) {
                          ok_eq_i64(dsh_settings_dict_count(back), 3, "⑪ 读回来还是 3 本");
                          ok_eq_str(dsh_settings_dict_at(back, 0)->id, "id-a",
                                    "⑪ ★ 存盘读回之后第 1 位仍是 a");
                          ok_eq_str(dsh_settings_dict_at(back, 1)->id, "id-b",
                                    "⑪ ★ 存盘读回之后第 2 位仍是 b");
                          ok_eq_str(dsh_settings_dict_at(back, 2)->id, "id-c",
                                    "⑪ ★ 存盘读回之后第 3 位仍是 c（顺序真的落盘了）");
                          dsh_settings_free(back);
                        }
                        dsh_release(json);
                      }
                    }
                    dsh_settings_free(r);                  }
                  dsh_settings_free(q);
                }
                dsh_settings_free(p);
              }
              dsh_settings_free(n);
            }
            dsh_settings_free(m);
          }
        }
      } else {
        ok(0, "⑪ 建三本词典");
      }
      if (s3 != NULL) dsh_settings_free(s3);
      if (s2 != NULL) dsh_settings_free(s2);
      if (s1 != NULL) dsh_settings_free(s1);
      dsh_settings_free(base);
    } else {
      ok(0, "⑪ 建默认设置");
    }
  }

  ok_eq_i64((int64_t)dsh_mem_live_count(), 0, "全部用例跑完，活分配表必须回到基线");

  printf("settings：%d 项，失败 %d\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
