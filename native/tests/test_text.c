/* 内核单元测试 · 文本（字形 / 分隔点 / 语种）
 * 四件易错事：①「汉X」必须先于「X汉」判（否则「汉英大词典」查中文词会用英文嗓子念）；②假名 / 谚文算 other 而非 han；
 * ③分隔点只认 separators 表里那 6 个码位、且不许碰连字符；④整段只有点要得到空串而非 NULL。
 */

#include "dsh_lookup.h"
#include "engine/dsh_engine_internal.h"
#include "mem_registry.h"
#include "text/dsh_language.h"
#include "text/dsh_textutil.h"

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
    fprintf(stderr, "FAIL %s：实际=%lld 期望=%lld\n", what, (long long)actual,
            (long long)expected);
  }
}

/** 判一次语种，核对语种码与检查标准种类 */
static void expect_lang(const char *text, const char *title, const char *want_lang,
                        const char *want_basis, const char *what) {
  const char *lang = NULL;
  const char *basis = NULL;
  const char *bt = NULL;
  dsh_language_decide(text, title, NULL, NULL, &lang, &basis, &bt);
  char label[256];
  snprintf(label, sizeof(label), "%s：语种应当是 %s", what, want_lang);
  ok_eq_str(lang, want_lang, label);
  if (want_basis != NULL) {
    snprintf(label, sizeof(label), "%s：检查标准应当是 %s", what, want_basis);
    ok_eq_str(basis, want_basis, label);
  }
  ok(bt != NULL && bt[0] != '\0', "检查标准说明必须非空（界面要直接用它）");
}

int main(void) {
  const size_t base = dsh_mem_live_count();

  /* ── ① 字形分区（与参考实现的 Scripts.Classify 逐区间对齐）── */
  {
    struct { const char *text; const char *want; } cases[] = {
        {"apple", "latin"},
        {"café", "latin"},          /* 带变音符号 */
        {"苹果", "han"},
        {"りんご", "other"},          /* 假名 → other，**不算** han */
        {"사과", "other"},           /* 谚文 → other */
        {"яблоко", "other"},         /* 西里尔 → other */
        {"", "other"},              /* 空串 */
        /* ⚠️ ASCII 标点（-+=）算 latin 而不是 other：参考实现的 Classify 第一条就是 code <= 0x24F → latin。
         * 看着反直觉，但它就是对的约定 —— 凭直觉写成 other 会被这一行当场挡下。 */
        {"-+=", "latin"},
        {"漢", "han"},              /* 扩展 A 区也是汉字 */
        {"汉a", "han"},             /* 混排：汉字优先算 han */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      dsh_script_counts c;
      dsh_script_classify(cases[i].text, strlen(cases[i].text), &c);
      char label[128];
      snprintf(label, sizeof(label), "① 「%s」的主要字形应当是 %s", cases[i].text,
               cases[i].want);
      ok_eq_str(dsh_script_dominant(&c), cases[i].want, label);
    }
    /* 假名里夹汉字仍然算 other：日语输入不是中文词，不许因为夹了汉字就当中文 */
    dsh_script_counts c;
    dsh_script_classify("日本語の漢字", strlen("日本語の漢字"), &c);
    ok(c.kana > 0 && c.han > 0, "① 假名与汉字都被数到了");
    ok_eq_str(dsh_script_dominant(&c), "other",
              "② 假名 + 汉字 → other（不许因为夹了汉字就算中文）");
  }

  /* ── ② 语种判定：字形 → 标题 → 兜底 ── */
  {
    expect_lang("apple", NULL, "en", "glyph", "② 拉丁字母无线索");
    expect_lang("苹果", NULL, "zh", "glyph", "② 汉字无线索（默认中文）");
    expect_lang("りんご", NULL, "ja", "glyph", "② 假名是排他的");
    expect_lang("사과", NULL, "ko", "glyph", "② 谚文是排他的");
    expect_lang("яблоко", NULL, "ru", "glyph", "② 西里尔默认俄语");
    expect_lang("12345", NULL, "en", "glyph", "② 认不出字形 → 英语兜底");

    /* 标题消歧：汉字到底是中文还是日语 */
    expect_lang("山", "新明解国語辞典", "ja", "title", "② 汉字 + 日语词典标题 → ja");
    expect_lang("山", "现代汉语词典", "zh", "title", "② 汉字 + 汉语词典标题 → zh");
    expect_lang("apple", "牛津高阶英汉双解", "en", "title", "② 拉丁 + 牛津 → en");

    /* 西里尔 / 阿拉伯：标题能细分 */
    expect_lang("слово", "乌克兰语词典", "uk", "title", "② 西里尔 + 乌克兰语标题 → uk");
    expect_lang("слово", NULL, "ru", "glyph", "② 西里尔 + 没线索 → ru");
  }

  /* ── ③ ⚠️「汉X」必须先于「X汉」：判反了就会拿英文嗓子念中文词 ── */
  {
    /* 「汉英大词典」= 用中文词查英语 → 词条是中文 → zh */
    expect_lang("苹果", "汉英大词典", "zh", "title",
                "③ 「汉英大词典」是中文词条（先判汉X）");
    /* 「英汉大词典」= 用英语词查中文 → 词条是英语 → en */
    expect_lang("apple", "英汉大词典", "en", "title",
                "③ 「英汉大词典」是英语词条（X汉）");
    /* 汉日 / 日汉 同理 */
    expect_lang("水", "汉日词典", "zh", "title", "③ 汉日 → 中文词条");
    expect_lang("水", "日汉词典", "ja", "title", "③ 日汉 → 日语词条");
    /* 中英 / 英中 那一组 */
    expect_lang("水", "中英词典", "zh", "title", "③ 中英 → 中文词条");
  }

  /* ── ④ 语种码的工具函数 ── */
  {
    ok_eq_str(dsh_language_name("en"), "英语", "④ en 的中文名");
    ok_eq_str(dsh_language_name("zh"), "中文", "④ zh 的中文名");
    ok_eq_str(dsh_language_name("ja"), "日语", "④ ja 的中文名");
    ok_eq_str(dsh_language_name("xx"), "", "④ 认不出的码给空串（不猜）");
    ok_eq_str(dsh_language_name(NULL), "", "④ NULL 给空串");
    ok_eq_str(dsh_language_default_tag("en"), "en-US", "④ 英语默认区域 en-US");
    ok_eq_str(dsh_language_default_tag("zh"), "zh-CN", "④ 中文默认区域 zh-CN");
    ok_eq_str(dsh_language_default_tag("pt"), "pt-PT", "④ 葡语默认区域 pt-PT");
    ok_eq_str(dsh_language_default_tag("fr"), "fr", "④ 其余语种原样返回");
    ok_eq_str(dsh_language_primary("en-US"), "en", "④ 从区域标记取主代码");
    ok_eq_str(dsh_language_primary("ZH-cn"), "zh", "④ 主代码大小写不敏感");
    ok_eq_str(dsh_language_primary("xx-YY"), "", "④ 认不出的区域标记给空串");
    ok(dsh_language_is_known("en") == 1, "④ en 认识");
    ok(dsh_language_is_known("EN-US") == 1, "④ EN-US 认识");
    ok(dsh_language_is_known("klingon") == 0, "④ 不认识的语种码要如实说不认识");
    ok(dsh_language_is_mixed("汉a") == 1, "④ 汉字 + 拉丁 = 混排");
    ok(dsh_language_is_mixed("汉字") == 0, "④ 纯汉字不是混排");
    ok(dsh_language_is_mixed("apple") == 0, "④ 纯拉丁不是混排");
  }

  /* ── ⑤ 分隔点：只认接口定义那张表里的码位，不许碰连字符 ── */
  {
    /* dic·tion·ar·y（U+00B7） */
    const char *dotted = "dic" "\xC2\xB7" "tion" "\xC2\xB7" "ar" "\xC2\xB7" "y";
    ok(dsh_text_has_separator_dots(dotted) == 1, "⑤ 认得出 U+00B7 分隔点");
    char *plain = dsh_text_strip_separator_dots(dotted);
    ok_eq_str(plain, "dictionary", "⑤ 去掉 U+00B7 之后是 dictionary");
    if (plain != NULL) dsh_release(plain);

    /* 另外三个码位：U+2027 / U+30FB / U+00AD */
    const char *others = "a" "\xE2\x80\xA7" "b" "\xE3\x83\xBB" "c" "\xC2\xAD" "d";
    ok(dsh_text_has_separator_dots(others) == 1, "⑤ 另外三个码位也要认");
    plain = dsh_text_strip_separator_dots(others);
    ok_eq_str(plain, "abcd", "⑤ 另外三个码位都要去掉");
    if (plain != NULL) dsh_release(plain);

    /* ★ 两个重音符（U+02C8 主 / U+02CC 次）也是分隔符：词典把词头写成 ˈæpl 时它们不属于词条名。
     * ⚠️ 参考实现只认那四个音节点 —— 本版有意超出、对照测试测不到，别当回归删掉这一条。 */
    {
      const char *stress = "\xCB\x88" "apple";            /* ˈapple */
      const char *second = "a\xCB\x8C" "pple";            /* aˌpple */
      ok(dsh_text_is_separator_dot(0x02C8u) == 1, "⑤ U+02C8（主重音）是分隔符");
      ok(dsh_text_is_separator_dot(0x02CCu) == 1, "⑤ U+02CC（次重音）是分隔符");
      ok(dsh_text_has_separator_dots(stress) == 1, "⑤ ˈapple 认得出带符号");
      plain = dsh_text_strip_separator_dots(stress);
      ok_eq_str(plain, "apple", "⑤ ˈapple → apple（重音是**删掉**，不是换字）");
      if (plain != NULL) dsh_release(plain);
      plain = dsh_text_strip_separator_dots(second);
      ok_eq_str(plain, "apple", "⑤ aˌpple → apple");
      if (plain != NULL) dsh_release(plain);
      /* 整段就是重音符 → 与「整段只有点」同一条约定：空串 */
      plain = dsh_text_strip_separator_dots("\xCB\x88\xCB\x8C");
      ok_eq_str(plain, "", "⑤ 整段只有重音符 → 空串");
      if (plain != NULL) dsh_release(plain);
    }

    /* ⚠️ 连字符**不许**碰：well-known 不能被改成 wellknown */
    ok(dsh_text_has_separator_dots("well-known") == 0, "⑤ 连字符不是分隔点");
    plain = dsh_text_strip_separator_dots("well-known");
    ok_eq_str(plain, "well-known", "⑤ 连字符必须原样保留（参考实现那个坑）");
    if (plain != NULL) dsh_release(plain);

    /* 没有点：原样返回（而且不是同一个指针 —— 调用方会释放它） */
    plain = dsh_text_strip_separator_dots("apple");
    ok_eq_str(plain, "apple", "⑤ 没有点就原样返回");
    if (plain != NULL) dsh_release(plain);

    /* 整段只有点 → **空串**（不是 NULL）*/
    plain = dsh_text_strip_separator_dots("\xC2\xB7\xC2\xB7\xC2\xB7");
    ok(plain != NULL, "⑤ 整段只有点也要给一个串（不是 NULL）");
    ok_eq_str(plain, "", "⑤ 整段只有点 → 空串");
    if (plain != NULL) dsh_release(plain);

    /* 空串 / NULL */
    plain = dsh_text_strip_separator_dots("");
    ok_eq_str(plain, "", "⑤ 空串进空串出");
    if (plain != NULL) dsh_release(plain);
    ok(dsh_text_strip_separator_dots(NULL) == NULL, "⑤ NULL 进 NULL 出（并报错）");
    ok(dsh_text_has_separator_dots(NULL) == 0, "⑤ has(NULL) → 0");
  }

  /* ── ⑥ 词数 / 单字符 ── */
  {
    ok_eq_i64(dsh_text_count_words(""), 0, "⑥ 空串 0 个词");
    ok_eq_i64(dsh_text_count_words("   "), 0, "⑥ 全空白 0 个词");
    ok_eq_i64(dsh_text_count_words("apple"), 1, "⑥ 一个词");
    ok_eq_i64(dsh_text_count_words("apple banana"), 2, "⑥ 两个词");
    ok_eq_i64(dsh_text_count_words("  apple \t banana \n cherry "), 3,
              "⑥ 多余空白不额外计词（约定：被空白分隔的非空段数）");
    ok_eq_i64(dsh_text_count_words("苹果 香蕉"), 2, "⑥ 中文两个段 = 2（段数约定）");
    ok_eq_i64(dsh_text_count_words("苹果 banana"), 2, "⑥ 中英混排两段");

    ok(dsh_text_is_single_char("a") == 1, "⑥ 单个 ASCII 字符");
    ok(dsh_text_is_single_char("中") == 1, "⑥ 单个汉字（3 字节也是 1 个字符）");
    ok(dsh_text_is_single_char("ab") == 0, "⑥ 两个字符不是单字符");
    ok(dsh_text_is_single_char("") == 0, "⑥ 空串不是单字符");
    ok(dsh_text_is_single_char(NULL) == 0, "⑥ NULL 不是单字符");
  }

  /* ── ⑦ 接口定义那三条：出参形状与错误码 ── */
  {
    char *json = NULL;
    ok(dsh_text_analyze("dic" "\xC2\xB7" "tion" "\xC2\xB7" "ar" "\xC2\xB7" "y", &json) == DSH_OK,
       "⑦ dsh_text_analyze 成功");
    ok_eq_str(json,
              "{\"script\":\"latin\",\"hasSeparatorDots\":true,"
              "\"withoutSeparatorDots\":\"dictionary\",\"wordCount\":1,"
              "\"isSingleChar\":false}",
              "⑦ 文本分析的 JSON 形状（逐字节）");
    if (json != NULL) dsh_release(json);

    json = NULL;
    ok(dsh_text_analyze("中", &json) == DSH_OK, "⑦ 单个汉字");
    ok(json != NULL && strstr(json, "\"isSingleChar\":true") != NULL, "⑦ 单字符标记为真");
    if (json != NULL) dsh_release(json);

    json = NULL;
    ok(dsh_text_analyze(NULL, &json) != DSH_OK, "⑦ text 为 NULL 必须失败");
    ok(dsh_text_analyze("x", NULL) != DSH_OK, "⑦ out 为 NULL 必须失败");

    char *stripped = NULL;
    ok(dsh_text_strip_separators("well-known", &stripped) == DSH_OK,
       "⑦ dsh_text_strip_separators 成功");
    ok_eq_str(stripped, "well-known", "⑦ 连字符那一条在接口定义层也成立");
    if (stripped != NULL) dsh_release(stripped);
    ok(dsh_text_strip_separators(NULL, &stripped) != DSH_OK, "⑦ text 为 NULL 必须失败");
  }

  /* ── ⑧ dsh_language_detect（要引擎：标题线索来自词库清单）── */
  {
    dsh_engine *e = NULL;
    ok(dsh_engine_create(NULL, &e) == DSH_OK, "⑧ 建一个不落盘的引擎");
    if (e != NULL) {
      char *json = NULL;
      ok(dsh_language_detect(e, "apple", NULL, &json) == DSH_OK, "⑧ 判语种成功");
      ok_eq_str(json,
                "{\"language\":\"en\",\"languageLabel\":\"英语\",\"basis\":\"glyph\","
                "\"basisText\":\"按字形判断（拉丁字母）\","
                "\"explanation\":\"英语（按字形判断（拉丁字母））\",\"mixed\":false}",
                "⑧ 语种判定的 JSON 形状（逐字段都对，含界面要的那句话）");
      if (json != NULL) dsh_release(json);

      /* 没配词库时 dict_id 给什么都不该崩 */
      json = NULL;
      ok(dsh_language_detect(e, "苹果", "no-such-id", &json) == DSH_OK,
         "⑧ 给一个不存在的 dict_id 不报错（只是没有标题线索）");
      ok(json != NULL && strstr(json, "\"language\":\"zh\"") != NULL,
         "⑧ 没有线索时汉字 → zh");
      if (json != NULL) dsh_release(json);

      json = NULL;
      ok(dsh_language_detect(e, NULL, NULL, &json) != DSH_OK, "⑧ text 为 NULL 必须失败");
      ok(dsh_language_detect(NULL, "x", NULL, &json) != DSH_OK, "⑧ engine 为 NULL 必须失败");
      ok(dsh_language_detect(e, "x", NULL, NULL) != DSH_OK, "⑧ out 为 NULL 必须失败");
      dsh_engine_destroy(e);
    }
  }

  ok_eq_i64((int64_t)dsh_mem_live_count(), (int64_t)base,
            "全部用例跑完，活分配表必须回到基线");

  /* ── ⑤ 语种码 → 中文名（那张表只许有一处，所以这条也在这儿钉）── */
  {
    char *json = NULL;
    ok(dsh_language_label("en", &json) == DSH_OK, "⑤ 取英文的中文名");
    ok(json != NULL && strstr(json, "\"英语\"") != NULL, "⑤ en → 英语");
    if (json) dsh_release(json);

    ok(dsh_language_label("en-US", &json) == DSH_OK, "⑤ 区域标记也认（先取主代码）");
    ok(json != NULL && strstr(json, "\"英语\"") != NULL, "⑤ en-US → 英语");
    if (json) dsh_release(json);

    ok(dsh_language_label("zh", &json) == DSH_OK, "⑤ 取中文的中文名");
    ok(json != NULL && strstr(json, "\"中文\"") != NULL, "⑤ zh → 中文");
    if (json) dsh_release(json);

    ok(dsh_language_label("xx", &json) == DSH_OK, "⑤ 认不出的码也要回得动");
    ok(json != NULL && strstr(json, "\"known\":false") != NULL, "⑤★ 认不出时 known=false");
    ok(json != NULL && strstr(json, "\"label\":\"\"") != NULL, "⑤★ 而且 label 是**空串**（不拿码当名字）");
    if (json) dsh_release(json);
  }
  printf("text：%d 项，失败 %d\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
