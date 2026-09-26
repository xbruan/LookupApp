/* 见 dsh_language.h —— 字形分区、标题线索、语种判定。 */
#include "text/dsh_language.h"
#include "text/dsh_textutil.h"

#include <stdio.h>

#include <string.h>

/*
 * UTF-8 解码这一段只许有**一份**（在 text/dsh_textutil.c，`dsh_text_next_cp`）：
 * 两份实现等于同一个约定有两个来源，改一处忘另一处就是「同一个坏字节两种判法」。
 */

/** 是不是空白的 ASCII（判定要跳过空白；其余空白交给 other，反正它们不影响结论） */
static int is_ascii_space(uint32_t cp) {
  return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r';
}

void dsh_script_classify(const char *utf8, size_t len, dsh_script_counts *out) {
  if (out == NULL) return;
  memset(out, 0, sizeof(*out));
  if (utf8 == NULL) return;
  size_t i = 0;
  while (i < len) {
    const uint32_t cp = dsh_text_next_cp(utf8, len, &i);
    if (is_ascii_space(cp)) continue;
    /* 分区表只允许有这一份（见 dsh_language.h）：区间划分与参考实现逐个区间对齐 */
    if (cp <= 0x24Fu || (cp >= 0x1E00u && cp <= 0x1EFFu)) { out->latin++; continue; }
    if (cp >= 0x3040u && cp <= 0x30FFu) { out->kana++; continue; }
    if (cp >= 0x31F0u && cp <= 0x31FFu) { out->kana++; continue; }
    if (cp >= 0xAC00u && cp <= 0xD7AFu) { out->hangul++; continue; }
    if (cp >= 0x1100u && cp <= 0x11FFu) { out->hangul++; continue; }
    if (cp >= 0x4E00u && cp <= 0x9FFFu) { out->han++; continue; }
    if (cp >= 0x3400u && cp <= 0x4DBFu) { out->han++; continue; }
    if (cp >= 0xF900u && cp <= 0xFAFFu) { out->han++; continue; }
    if (cp >= 0x0400u && cp <= 0x04FFu) { out->cyrillic++; continue; }
    if (cp >= 0x0500u && cp <= 0x052Fu) { out->cyrillic++; continue; }
    if (cp >= 0x0370u && cp <= 0x03FFu) { out->greek++; continue; }
    if (cp >= 0x0600u && cp <= 0x06FFu) { out->arabic++; continue; }
    if (cp >= 0x0750u && cp <= 0x077Fu) { out->arabic++; continue; }
    if (cp >= 0x0590u && cp <= 0x05FFu) { out->hebrew++; continue; }
    if (cp >= 0x0E00u && cp <= 0x0E7Fu) { out->thai++; continue; }
    if (cp >= 0x0900u && cp <= 0x097Fu) { out->devanagari++; continue; }
    if (cp >= 0x0980u && cp <= 0x09FFu) { out->bengali++; continue; }
    if (cp >= 0x0B80u && cp <= 0x0BFFu) { out->tamil++; continue; }
    out->other++;
  }
}

const char *dsh_script_dominant(const dsh_script_counts *c) {
  if (c == NULL) return "other";
  /* 假名 / 谚文算 other 而**不算** han：日语、韩语的输入不是「中文词」，
   * 不该因为夹了几个汉字就被当成中文处理（与参考实现同一条约定）。 */
  if (c->kana > 0 || c->hangul > 0) return "other";
  if (c->han > 0) return "han"; /* 汉字（含中英混排：混排也算中文输入） */
  if (c->latin > 0) return "latin";
  return "other"; /* 西里尔 / 阿拉伯 / 纯符号 / 空串 */
}

enum dsh_script dsh_script_of(const char *utf8, size_t len) {
  dsh_script_counts c;
  dsh_script_classify(utf8, len, &c);
  const char *name = dsh_script_dominant(&c);
  if (strcmp(name, "han") == 0) return DSH_SCRIPT_HAN;
  if (strcmp(name, "latin") == 0) return DSH_SCRIPT_LATIN;
  return DSH_SCRIPT_OTHER;
}

/* ── 语种码 ─────────────────────────────────────────────────────────────── */

typedef struct {
  const char *code;
  const char *name;
} lang_name;

static const lang_name LANGS[] = {
    {"en", "英语"},   {"zh", "中文"},   {"ja", "日语"},   {"ko", "韩语"},
    {"fr", "法语"},   {"de", "德语"},   {"es", "西班牙语"}, {"it", "意大利语"},
    {"pt", "葡萄牙语"}, {"ru", "俄语"},   {"ar", "阿拉伯语"}, {"th", "泰语"},
    {"vi", "越南语"},  {"hi", "印地语"},  {"tr", "土耳其语"}, {"pl", "波兰语"},
    {"nl", "荷兰语"},  {"sv", "瑞典语"},  {"da", "丹麦语"},  {"fi", "芬兰语"},
    {"no", "挪威语"},  {"cs", "捷克语"},  {"hu", "匈牙利语"}, {"ro", "罗马尼亚语"},
    {"el", "希腊语"},  {"he", "希伯来语"}, {"uk", "乌克兰语"}, {"id", "印尼语"},
    {"ms", "马来语"},  {"fa", "波斯语"},  {"ur", "乌尔都语"}, {"ta", "泰米尔语"},
    {"bn", "孟加拉语"}, {"sk", "斯洛伐克语"}, {"sl", "斯洛文尼亚语"}, {"hr", "克罗地亚语"},
    {"bg", "保加利亚语"}, {"sr", "塞尔维亚语"}, {"lt", "立陶宛语"}, {"lv", "拉脱维亚语"},
    {"et", "爱沙尼亚语"}, {"is", "冰岛语"},  {"ca", "加泰罗尼亚语"}, {"af", "南非荷兰语"},
    {"sw", "斯瓦希里语"}, {"la", "拉丁语"},
};

/** 取语种主代码（「en-US」→「en」，全小写）。返回静态字符串；认不出返回空串。 */
const char *dsh_language_primary(const char *tag) {
  if (tag == NULL || tag[0] == '\0') return "";
  size_t n = 0;
  while (tag[n] != '\0' && tag[n] != '-') n++;
  for (size_t i = 0; i < sizeof(LANGS) / sizeof(LANGS[0]); i++) {
    const char *code = LANGS[i].code;
    if (strlen(code) != n) continue;
    size_t k = 0;
    for (; k < n; k++) {
      char c = tag[k];
      if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
      if (c != code[k]) break;
    }
    if (k == n) return code;
  }
  return "";
}

int dsh_language_is_known(const char *language) { return dsh_language_primary(language)[0] != '\0'; }

const char *dsh_language_name(const char *language) {
  const char *code = dsh_language_primary(language);
  if (code[0] == '\0') return "";
  for (size_t i = 0; i < sizeof(LANGS) / sizeof(LANGS[0]); i++) {
    if (strcmp(LANGS[i].code, code) == 0) return LANGS[i].name;
  }
  return "";
}

const char *dsh_language_default_tag(const char *language) {
  const char *code = dsh_language_primary(language);
  if (strcmp(code, "en") == 0) return "en-US";
  if (strcmp(code, "zh") == 0) return "zh-CN";
  if (strcmp(code, "pt") == 0) return "pt-PT";
  return code;
}

int dsh_language_is_mixed(const char *text) {
  if (text == NULL) return 0;
  dsh_script_counts c;
  dsh_script_classify(text, strlen(text), &c);
  return (c.han > 0 && c.latin > 0) ? 1 : 0;
}

/* ── 从词典标题认语种 ───────────────────────────────────────────────────── */

/** 「汉X」/「X汉」这一组组合：X 是一个**中文字**，对应的语种码在 `code` 里 */
typedef struct {
  const char *chinese; /* 英 / 日 / 韩 … */
  const char *code;    /* en / ja / ko … */
} chinese_pair;

static const chinese_pair CHINESE_FIRST_PAIRS[] = {
    {"英", "en"}, {"日", "ja"}, {"韩", "ko"}, {"法", "fr"}, {"德", "de"},
    {"俄", "ru"}, {"西", "es"}, {"意", "it"}, {"葡", "pt"}, {"阿", "ar"},
    {"泰", "th"}, {"越", "vi"}, {"印", "hi"},
};

typedef struct {
  const char *code;
  const char *keywords[24];
} title_keywords;

/* ⚠️ 每一行**必须带哨兵** —— 关键词表是按 NULL 结尾扫的（见 `hint_from_title`）。
 *    手写容易漏：漏了会读到数组外面，而且**不会**让任何一条用例变红（读到野指针只是「没匹配上」），
 *    属于不报错地未定义行为 —— 所以哨兵不靠人记得写，改用宏拼。数组开 24：一行最多 20 个词 + 1 个 NULL。 */
#define KW_ROW(code, ...) {code, {__VA_ARGS__, NULL}}

static const title_keywords TITLE_KEYWORDS[] = {
    KW_ROW("en", "英汉", "英英", "英语", "英文", "牛津", "朗文", "柯林斯", "韦氏", "韦伯斯特",
           "麦克米伦", "剑桥", "高阶", "双解", "oxford", "longman", "collins", "merriam",
           "webster", "cambridge", "macmillan"),
    KW_ROW("en", "ldoce", "oald", "ahd", "americanheritage", "4合1", "四合一"),
    KW_ROW("ja", "日语", "日文", "日汉", "新明解", "大辞林", "广辞苑", "広辞苑", "日本語", "jmdict",
           "小学馆"),
    KW_ROW("zh", "汉语", "中文", "新华", "现代汉语", "成语", "康熙", "中华", "國語", "国语", "漢語",
           "汉字", "漢典"),
    KW_ROW("ko", "韩语", "韩汉", "朝鲜语", "韓國語", "韩文"),
    KW_ROW("fr", "法语", "法汉", "拉鲁斯", "larousse", "法文"),
    KW_ROW("de", "德语", "德汉", "朗氏", "杜登", "duden", "德文"),
    KW_ROW("es", "西班牙语", "西汉", "西班牙文"),
    KW_ROW("it", "意大利语", "意汉", "意大利文"),
    KW_ROW("pt", "葡萄牙语", "葡汉", "葡萄牙文"),
    KW_ROW("ru", "俄语", "俄汉", "俄文"),
    KW_ROW("ar", "阿拉伯语", "阿汉", "阿拉伯文"),
    KW_ROW("th", "泰语", "泰汉", "泰文"),
    KW_ROW("vi", "越南语", "越汉", "越南文"),
    KW_ROW("hi", "印地语", "印汉", "印地文"),
    KW_ROW("tr", "土耳其语"),
    KW_ROW("nl", "荷兰语"),
    KW_ROW("pl", "波兰语"),
    KW_ROW("el", "希腊语"),
    KW_ROW("la", "拉丁语"),
    KW_ROW("sv", "瑞典语"),
    KW_ROW("da", "丹麦语"),
    KW_ROW("no", "挪威语"),
    KW_ROW("fi", "芬兰语"),
    KW_ROW("cs", "捷克语"),
    KW_ROW("hu", "匈牙利语"),
    KW_ROW("ro", "罗马尼亚语"),
    KW_ROW("uk", "乌克兰语"),
    KW_ROW("fa", "波斯语"),
    KW_ROW("ur", "乌尔都语"),
};

#undef KW_ROW

/** 子串查找（按字节，大小写不敏感 —— 与参考实现同一条约定） */
static int contains_ci(const char *haystack, const char *needle) {
  const size_t n = strlen(needle);
  if (n == 0) return 0;
  for (const char *p = haystack; *p != '\0'; p++) {
    size_t k = 0;
    for (; k < n; k++) {
      char a = p[k];
      if (a == '\0') break;
      char b = needle[k];
      if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
      if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
      if (a != b) break;
    }
    if (k == n) return 1;
  }
  return 0;
}

/**
 * 从词典标题里认语种（认不出返回 NULL）。
 * ⚠️ **「汉X」必须先于「X汉」判**：两者含义相反（英汉 = 用英语词查中文 → 词条是英语；
 *    汉英 = 用中文词查英语 → 词条是中文）；顺序反了，「汉英大词典」会被当成英语词典。
 */
static const char *hint_from_title(const char *title) {
  if (title == NULL) return NULL;
  /* 标题里的空格要去掉（「牛津 高阶」也要能认出来） */
  char buf[512];
  size_t k = 0;
  for (const char *p = title; *p != '\0' && k + 1 < sizeof(buf); p++) {
    if (*p == ' ' || *p == '\t') continue;
    buf[k++] = *p;
  }
  buf[k] = '\0';
  if (k == 0) return NULL;

  /* 逐个组合判：**先「汉X」**（词条是中文），**再「X汉」** */
  for (size_t i = 0; i < sizeof(CHINESE_FIRST_PAIRS) / sizeof(CHINESE_FIRST_PAIRS[0]); i++) {
    char han_x[16];
    char x_han[16];
    snprintf(han_x, sizeof(han_x), "汉%s", CHINESE_FIRST_PAIRS[i].chinese);
    if (contains_ci(buf, han_x)) return "zh";
    snprintf(x_han, sizeof(x_han), "%s汉", CHINESE_FIRST_PAIRS[i].chinese);
    if (contains_ci(buf, x_han)) return CHINESE_FIRST_PAIRS[i].code;
  }
  static const struct { const char *needle; const char *code; } PAIRS2[] = {
      {"中日", "zh"}, {"日中", "ja"}, {"中韩", "zh"}, {"韩中", "ko"}, {"中英", "zh"},
  };
  for (size_t i = 0; i < sizeof(PAIRS2) / sizeof(PAIRS2[0]); i++) {
    if (contains_ci(buf, PAIRS2[i].needle)) return PAIRS2[i].code;
  }
  for (size_t i = 0; i < sizeof(TITLE_KEYWORDS) / sizeof(TITLE_KEYWORDS[0]); i++) {
    for (size_t k2 = 0; TITLE_KEYWORDS[i].keywords[k2] != NULL; k2++) {
      if (contains_ci(buf, TITLE_KEYWORDS[i].keywords[k2])) return TITLE_KEYWORDS[i].code;
    }
  }
  return NULL;
}

/* ── 判定 ───────────────────────────────────────────────────────────────── */

void dsh_language_decide(const char *text, const char *dictionary_title,
                         const char *override_code, const char *default_language,
                         const char **out_language, const char **out_basis,
                         const char **out_basis_text) {
  if (out_language != NULL) *out_language = "en";
  if (out_basis != NULL) *out_basis = "glyph";
  if (out_basis_text != NULL) *out_basis_text = "按字形判断（拉丁字母）";

  /* ① 用户显式指定：最高优先级 */
  if (dsh_language_is_known(override_code)) {
    const char *code = dsh_language_primary(override_code);
    if (out_language != NULL) *out_language = code;
    if (out_basis != NULL) *out_basis = "override";
    if (out_basis_text != NULL) *out_basis_text = "按你选的语种";
    return;
  }

  dsh_script_counts c;
  dsh_script_classify(text, text != NULL ? strlen(text) : 0, &c);
  const char *hint = hint_from_title(dictionary_title);

  /* ② 排他性最强的书写系统：看到就能定 */
  if (c.kana > 0) {
    if (out_language != NULL) *out_language = "ja";
    if (out_basis != NULL) *out_basis = "glyph";
    if (out_basis_text != NULL) *out_basis_text = "按字形判断（假名）";
    return;
  }
  if (c.hangul > 0) {
    if (out_language != NULL) *out_language = "ko";
    if (out_basis_text != NULL) *out_basis_text = "按字形判断（谚文）";
    return;
  }
  if (c.thai > 0) {
    if (out_language != NULL) *out_language = "th";
    if (out_basis_text != NULL) *out_basis_text = "按字形判断（泰文）";
    return;
  }
  if (c.hebrew > 0) {
    if (out_language != NULL) *out_language = "he";
    if (out_basis_text != NULL) *out_basis_text = "按字形判断（希伯来文）";
    return;
  }
  if (c.greek > 0) {
    if (out_language != NULL) *out_language = "el";
    if (out_basis_text != NULL) *out_basis_text = "按字形判断（希腊文）";
    return;
  }
  if (c.devanagari > 0) {
    if (out_language != NULL) *out_language = "hi";
    if (out_basis_text != NULL) *out_basis_text = "按字形判断（天城文）";
    return;
  }
  if (c.bengali > 0) {
    if (out_language != NULL) *out_language = "bn";
    if (out_basis_text != NULL) *out_basis_text = "按字形判断（孟加拉文）";
    return;
  }
  if (c.tamil > 0) {
    if (out_language != NULL) *out_language = "ta";
    if (out_basis_text != NULL) *out_basis_text = "按字形判断（泰米尔文）";
    return;
  }

  /* ③ 阿拉伯字母：阿拉伯语最常见，但标题说是乌尔都语/波斯语就听标题的 */
  if (c.arabic > 0) {
    const char *code = (hint != NULL && (strcmp(hint, "ur") == 0 || strcmp(hint, "fa") == 0))
                           ? hint
                           : "ar";
    if (out_language != NULL) *out_language = code;
    if (out_basis != NULL) *out_basis = (strcmp(code, hint ? hint : "") == 0) ? "title" : "glyph";
    if (out_basis_text != NULL) {
      *out_basis_text = (strcmp(code, hint ? hint : "") == 0) ? "按字形 + 词典名判断"
                                                              : "按字形判断（阿拉伯字母）";
    }
    return;
  }

  /* ④ 西里尔字母：俄语最常见，标题能区分出乌克兰语/保加利亚语/塞尔维亚语 */
  if (c.cyrillic > 0) {
    const char *code = "ru";
    if (hint != NULL &&
        (strcmp(hint, "uk") == 0 || strcmp(hint, "bg") == 0 || strcmp(hint, "sr") == 0)) {
      code = hint;
    }
    if (out_language != NULL) *out_language = code;
    if (out_basis != NULL) *out_basis = (hint != NULL && strcmp(code, hint) == 0) ? "title" : "glyph";
    if (out_basis_text != NULL) {
      *out_basis_text = (hint != NULL && strcmp(code, hint) == 0) ? "按字形 + 词典名判断"
                                                                 : "按字形判断（西里尔字母）";
    }
    return;
  }

  /* ⑤ 汉字：中文还是日语？单看字认不出来（"山""水"两边都有），交给标题消歧 */
  if (c.han > 0) {
    if (hint != NULL && strcmp(hint, "ja") == 0) {
      if (out_language != NULL) *out_language = "ja";
      if (out_basis != NULL) *out_basis = "title";
      if (out_basis_text != NULL) *out_basis_text = "按词典名判断（这本是日语词典）";
      return;
    }
    if (hint != NULL && strcmp(hint, "ko") == 0) {
      if (out_language != NULL) *out_language = "ko";
      if (out_basis != NULL) *out_basis = "title";
      if (out_basis_text != NULL) *out_basis_text = "按词典名判断（这本是韩语词典）";
      return;
    }
    if (hint != NULL && strcmp(hint, "zh") == 0) {
      if (out_language != NULL) *out_language = "zh";
      if (out_basis != NULL) *out_basis = "title";
      if (out_basis_text != NULL) *out_basis_text = "按词典名判断（这本是汉语词典）";
      return;
    }
    /* 汉字但没线索：默认中文（用户装的汉语词典远多于日语词典） */
    if (out_language != NULL) *out_language = "zh";
    if (out_basis != NULL) *out_basis = "glyph";
    if (out_basis_text != NULL) *out_basis_text = "按字形判断（汉字）";
    return;
  }

  /* ⑥ 拉丁字母（含带变音符号的越南语/土耳其语等）：字形给不出答案，全看标题 */
  if (c.latin > 0 || c.other > 0) {
    if (hint != NULL) {
      if (out_language != NULL) *out_language = hint;
      if (out_basis != NULL) *out_basis = "title";
      if (out_basis_text != NULL) *out_basis_text = "按词典名判断";
      return;
    }
    if (dsh_language_is_known(default_language)) {
      /* 产品里今天到不了这里（默认语种恒为 NULL）—— 分支留着给诊断与将来 */
      if (out_language != NULL) *out_language = dsh_language_primary(default_language);
      if (out_basis != NULL) *out_basis = "default";
      if (out_basis_text != NULL) *out_basis_text = "按你设置的默认语种";
      return;
    }
    if (out_language != NULL) *out_language = "en";
    if (out_basis != NULL) *out_basis = "glyph";
    if (out_basis_text != NULL) *out_basis_text = "按字形判断（拉丁字母）";
    return;
  }

  /* ⑦ 什么都没认出来（纯数字、符号）：默认语种 / 英语兜底。
   * ⚠️ 文案只承诺**语种**、不承诺区域：accent=uk 的用户在这条路上听到的是英音，
   * 而那与「英语这个语种的默认区域是 en-US」是两件事。 */
  if (dsh_language_is_known(default_language)) {
    if (out_language != NULL) *out_language = dsh_language_primary(default_language);
    if (out_basis != NULL) *out_basis = "default";
    if (out_basis_text != NULL) *out_basis_text = "认不出字形，按默认语种";
    return;
  }
  if (out_language != NULL) *out_language = "en";
  if (out_basis != NULL) *out_basis = "glyph";
  if (out_basis_text != NULL) *out_basis_text = "认不出字形，按英语兜底";
}
