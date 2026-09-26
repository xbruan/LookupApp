/* ==========================================================================
 * 字形与语种 —— 「这段文字该用哪种语言的嗓子念？」
 *
 * 为什么住内核：接口定义里 `dsh_language_detect` 要**连检查标准说明一起**回给界面
 * （「英语（按字形判断（拉丁字母））」那句话由内核给，界面不拼），而这个判定在参考实现里
 * 散在三处 —— 合并成一处正是这一层存在的理由。
 *
 * 判定的两条线索，顺序不能换：
 *   ① **字形**（最可靠）：假名→日语、谚文→韩语、希腊字母→希腊语……这些书写系统是
 *      **排他**的，看到就能定；
 *   ② **词典标题**（消歧）：汉字到底是中文还是日语、拉丁字母到底是英法德西 —— 单看字
 *      认不出来，标题里写着「牛津高阶英汉双解」就基本能确定是英语词典。
 *
 * 三条容易搞错的地方：
 *   · **「汉X」 必须先于 「X汉」 判**：命名习惯是「英汉」「汉英」这种两字组合，而先后**含义
 *     相反**（英汉 = 用英语词查中文 → 词条是英语）。不先判「汉X」的话，「汉英大词典」
 *     会被当成英语词典 —— 查「苹果」时用一个英文嗓子去念。
 *   · **假名 / 谚文算 other 而**不算** han**：日语、韩语的输入不是「中文词」，不该因为
 *     里头夹了几个汉字就被当成中文处理。
 *   · **「认不出字形」的文案只承诺语种、不承诺区域**：`accent=uk` 的用户在那条路上听到的
 *     是英音，这与「英语这个语种的默认区域是 en-US」是两件事。
 * ========================================================================== */

#ifndef DSH_TEXT_LANGUAGE_H
#define DSH_TEXT_LANGUAGE_H

#include <stddef.h>
#include <stdint.h>

#include "dsh_lookup.h" /* 接口定义里的 enum dsh_script（dsh_script_of 直接给这个枚举） */

/** 一段文字里各书写系统的**计数**（判定只看有没有 > 0） */
typedef struct {
  int han;         /* 汉字（含扩展 A / 兼容区） */
  int kana;        /* 平假名 + 片假名 */
  int hangul;      /* 谚文（含字母） */
  int latin;       /* 拉丁字母（含变音、扩展附加） */
  int cyrillic;
  int greek;
  int arabic;
  int hebrew;
  int thai;
  int devanagari;  /* 天城文 */
  int bengali;
  int tamil;
  int other;       /* 上面都不属于的**非空白**字符 */
} dsh_script_counts;

/** 把一个码点归到某一类，并累加计数 */
void dsh_script_classify(const char *utf8, size_t len, dsh_script_counts *out);

/** 只分三档的主要书写系统：`"han"` / `"latin"` / `"other"`（返回静态字符串） */
const char *dsh_script_dominant(const dsh_script_counts *c);

/**
 * 一段文字的「主导字形」，**直接给接口定义里那个枚举**（`DSH_SCRIPT_HAN` / `_LATIN` / `_OTHER`）。
 *
 * 存在的理由只有一个：调用方要的是枚举，而「什么算汉字」只许有**一处来源** —— 让每个调用
 * 方自己去 classify + dominant + 比字符串，早晚会有一处把 `"han"` 写成 `"Han"`。
 */
enum dsh_script dsh_script_of(const char *utf8, size_t len);

/**
 * 判语种。
 *
 * @param text             要念的文本（UTF-8）
 * @param dictionary_title 当前词典标题，可为 NULL（那是「没有线索」，不是错误）
 * @param override_code    用户显式指定的语种（最高优先级）；NULL / 认不出的码 = 没用
 * @param default_language 兜底用的默认语种；**产品里一律传 NULL**（那项用户选择权已取消，
 *                         只留这两条分支给诊断与将来）
 * @param out_language     出参：语种主代码，如 「en」（静态字符串）
 * @param out_basis        出参：检查标准种类 —— 「override」 / 「glyph」 / 「title」 / 「default」
 * @param out_basis_text   出参：检查标准的人话说明，如 「按字形判断（拉丁字母）」（静态字符串）
 */
void dsh_language_decide(const char *text, const char *dictionary_title,
                         const char *override_code, const char *default_language,
                         const char **out_language, const char **out_basis,
                         const char **out_basis_text);

/** 语种码 → 中文名（如 「en」 → 「英语」）。认不出时返回空串（**不猜**）。 */
const char *dsh_language_name(const char *language);

/**
 * 语种码的**默认区域标记**（「en」 → 「en-US」、「zh」 → 「zh-CN」、「pt」 → 「pt-PT」，
 * 其余原样返回）。它管的是缓存键与界面显示，**不承诺**这次会挑到哪个嗓子。
 */
const char *dsh_language_default_tag(const char *language);

/** 这个语种码内核认不认识（认不出的一律当「没填」，不要拿去发请求） */
int dsh_language_is_known(const char *language);

/** 从区域标记取主代码（「en-US」 → 「en」；返回静态字符串） */
const char *dsh_language_primary(const char *tag);

/** 文本里汉字与拉丁字母**同时**出现（「中英混排」） */
int dsh_language_is_mixed(const char *text);

#endif /* DSH_TEXT_LANGUAGE_H */
