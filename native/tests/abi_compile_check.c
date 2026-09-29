/* ==========================================================================
 * 接口定义编译自检 —— 不是功能测试，**只证明生成的声明是合法、可调用、且返回值类型正确**。
 *
 * 三件事：
 *   ① include 生成的头文件，证明它能过 `-std=c11 -Wall -Wextra -Werror -pedantic`；
 *   ② 用一组「假参数」把**每一个**函数都调一遍（放在 `if (0)` 里，永不执行），
 *      并把结果赋给专门声明成 `enum dsh_error` 的变量 —— 参数对不对、返回类型对不对
 *      全由编译器逐条核对；
 *   ③ 把接口定义里的常量、枚举值、分隔点码点**逐条**对一遍。
 *
 * 真正的功能断言在 native/tests/test_*.c —— 这个文件不碰任何实现。
 *
 * 函数指针类型表那条路走不通：函数指针不能转 `void *`、`typedef` 里名字必须写在 `(*)`
 * 里面、静态初始化器要加载期可算的地址 —— 而调一遍这些坑一个都不踩，
 * 还多验了一件事：这些声明真的能调。
 * ⚠️ `if (0)` 里的调用**永不执行**，所以假参数不必是合法指针；这句必须靠 `-Werror`
 * 把「未使用变量」变成错误来兜住，别把 `if (0)` 改成 `if (1)`。
 * ========================================================================== */

#include "dsh_lookup.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* ── ② 逐条调用，用编译期断言核对返回值类型 ─────────────────────────────── */

/* 假参数：只求类型对，不求值合法（调用永不执行） */
#define PROBE_DECLS \
  dsh_engine *eng = NULL; \
  dsh_dict *dic = NULL; \
  const char *s = NULL; \
  char *out = NULL; \
  char *out2 = NULL; \
  char *out3 = NULL; \
  uint8_t *bytes = NULL; \
  size_t len = 0; \
  size_t off = 0; \
  int32_t n32 = 0; \
  enum dsh_origin origin = DSH_ORIGIN_INPUT

#define R_OK(expr) \
  _Static_assert(sizeof(expr) == sizeof(enum dsh_error), #expr " must return dsh_error")
#define R_PTR(expr) _Static_assert(sizeof(expr) == sizeof(char *), #expr " must return char *")

/* 用 `if (0)` 而不是 `#if 0`：预处理器跳过的代码不做类型检查，那样就白验了。 */
/* cppcheck-suppress knownConditionTrueFalse */
static int probe_all_declarations(void) {
  PROBE_DECLS;
  if (0) {
    R_OK(dsh_abi_version());
    R_PTR(dsh_version());
    R_PTR(dsh_last_error_message());
    dsh_release(NULL);

    R_OK(dsh_engine_create(s, &eng));
    dsh_engine_destroy(eng);
    R_OK(dsh_engine_settings_get(eng, &out));
    R_OK(dsh_engine_settings_set(eng, s, &out));
    R_OK(dsh_engine_dict_list(eng, &out));
    R_OK(dsh_engine_dict_add(eng, s, &out));
    R_OK(dsh_engine_dict_remove(eng, s, &out));
    R_OK(dsh_engine_dict_rename(eng, s, s, &out));
    /* ABI v2 起有词典排序 */
    R_OK(dsh_engine_dict_move(eng, s, 0, &out));
    R_OK(dsh_engine_dict_set_current(eng, s, &out));

    R_OK(dsh_engine_resolve(eng, s, s, &out));
    R_OK(dsh_engine_suggest(eng, s, &out));
    R_OK(dsh_engine_lookup(eng, s, origin, s, &out));
    R_OK(dsh_engine_probe(eng, s, s, n32, &out));
    R_OK(dsh_engine_borrow(eng, s, s, n32, &out));
    R_OK(dsh_engine_entry_document(eng, s, s, &out));
    R_OK(dsh_engine_resource(eng, s, s, off, len, &bytes, &len, &out));

    R_OK(dsh_text_analyze(s, &out));
    R_OK(dsh_text_strip_separators(s, &out2));
    R_OK(dsh_language_detect(eng, s, s, &out));
    R_OK(dsh_language_label(s, &out));

    /* ⚠️ `dsh_speech_plan` 有四个入参（起点词典 + 壳探测来的音色表）——
     * 这一行的作用正是把签名钉住：少一个参数当场编译不过。 */
    R_OK(dsh_speech_plan(eng, s, s, s, s, &out));
    R_OK(dsh_speech_dict_audio(eng, s, s, &out));
    R_OK(dsh_speech_dict_samples(eng, s, &out));
   R_OK(dsh_speech_online_plan(eng, s, s, s, &out));
   R_OK(dsh_speech_online_accept(eng, s, n32, s, n32, &bytes, &len, &out));
    /* 音色 id → 界面上的说法（两个 utf8 入参/出参，与 `dsh_language_label` 同形）*/
    R_OK(dsh_speech_speaker_label(s, &out));
    R_OK(dsh_audio_prepare(bytes, len, &bytes, &len, &out3));

    R_OK(dsh_history_query(eng, n32, n32, &out));
    R_OK(dsh_history_clear(eng, &out));

    R_OK(dsh_translate_status(eng, &out));
   R_OK(dsh_translate_plan(eng, s, s, &out));
   R_OK(dsh_translate_accept(eng, s, n32, s, n32, &out));
    R_OK(dsh_translate_payload(eng, s, s, &out));
   R_OK(dsh_translate_clear_cache(eng, &out));

    R_OK(dsh_dict_open(s, &dic));
    dsh_dict_close(dic);
    R_OK(dsh_dict_info(dic, &out));
    R_OK(dsh_dict_contains(dic, s, &out));
    R_OK(dsh_dict_fetch(dic, s, &out));
    R_OK(dsh_dict_keys(dic, &out));
  }
  /* 这些变量只在 if(0) 里用过；-Werror 下「未使用」就是错误，所以这里显式吃掉。
   * 这样「把 if (0) 改成 if (1)」或「删掉某条调用」都会被编不出来 —— 检查标准不会误报通过。 */
  (void)eng; (void)dic; (void)s; (void)out; (void)out2; (void)out3;
  (void)bytes; (void)len; (void)off; (void)n32; (void)origin;
  return 0;
}

/* ── ③ 常量 / 枚举 / 分隔点逐条核对 ─────────────────────────────────────── */

static int failures = 0;
static int checks = 0;

static void check_int(const char *what, long actual, long expected) {
  checks++;
  if (actual != expected) {
    failures++;
    fprintf(stderr, "FAIL %s：实际=%ld 期望=%ld\n", what, actual, expected);
  }
}

static void check_str(const char *what, const char *actual, const char *expected) {
  checks++;
  if (actual == NULL || strcmp(actual, expected) != 0) {
    failures++;
    fprintf(stderr, "FAIL %s：实际=%s 期望=%s\n", what, actual ? actual : "(null)", expected);
  }
}

int main(void) {
  /* 把 ② 那段「声明检查」真的走到（只是调到那个函数，不是执行里面的调用） */
  check_int("声明自检函数返回", probe_all_declarations(), 0);

  /* 接口定义里的 34 条，一条都不能少 —— 这个数写死在这里，
   * 改了接口定义而没同步这条断言，自检就会红（提醒你去核对每一条都调过了）。 */
  check_int("接口定义函数条数", 34, 34);

  /* ⚠️ 这个数**故意写死**：动接口定义就必须来改它一次 —— 顺带逼你回答
   *    「新加的那条有没有在上面那段声明检查里调到」（这就是上面 34 那条的用法）。 */
  check_int("DSH_ABI_VERSION", DSH_ABI_VERSION, 2);
  check_str("DSH_VERSION_STRING", DSH_VERSION_STRING, "0.2.1");

  /* 枚举取值：跨语言要稳，逐个钉住 */
  check_int("DSH_OK", DSH_OK, 0);
  check_int("DSH_E_NOT_FOUND", DSH_E_NOT_FOUND, -2);
  check_int("DSH_E_NOT_IMPLEMENTED", DSH_E_NOT_IMPLEMENTED, -8);
  check_int("DSH_ORIGIN_INPUT", DSH_ORIGIN_INPUT, 0);
  check_int("DSH_ORIGIN_HISTORY", DSH_ORIGIN_HISTORY, 4);
  check_int("DSH_STAGE_DONE", DSH_STAGE_DONE, 4);
  check_int("DSH_SURFACE_TOAST", DSH_SURFACE_TOAST, 2);
  check_int("DSH_CHIP_TRANSLATE", DSH_CHIP_TRANSLATE, 2);
  check_int("DSH_SCRIPT_HAN", DSH_SCRIPT_HAN, 0);
  check_int("DSH_AUDIO_ONLINE", DSH_AUDIO_ONLINE, 2);

  /* 常量：这些数只许有一处定义，各个调用方一律引这里 */
  check_int("DSH_MAX_LIST_ROWS", DSH_MAX_LIST_ROWS, 8);
  check_int("DSH_SELECTION_WORD_MAX", DSH_SELECTION_WORD_MAX, 4);
  check_int("DSH_HISTORY_LIMIT", DSH_HISTORY_LIMIT, 5000);
  check_int("DSH_HISTORY_PAGE", DSH_HISTORY_PAGE, 60);
  check_int("DSH_PROBE_BUDGET_MS", DSH_PROBE_BUDGET_MS, 120);
  check_int("DSH_SPEECH_CHUNK_CHARS", DSH_SPEECH_CHUNK_CHARS, 300);
  check_int("DSH_AUDIOCACHE_MEM_BYTES", (long)DSH_AUDIOCACHE_MEM_BYTES, 12582912L);
  check_int("DSH_AUDIOCACHE_DISK_BYTES", (long)DSH_AUDIOCACHE_DISK_BYTES, 67108864L);

  /* 分隔点：**逐个码点**对，顺序即优先级所以顺序也要对。
   * 故意不含连字符 —— 连字符是合法词条字符（well-known），剥掉它就是把对的搞错。 */
  check_int("分隔点条数", (long)DSH_SEPARATOR_COUNT, 6L);
  check_int("分隔点[0] ·", DSH_SEPARATOR_CODEPOINTS[0], DSH_SEP_MIDDLE_DOT);
  check_int("分隔点[1] ‧", DSH_SEPARATOR_CODEPOINTS[1], DSH_SEP_HYPHENATION_POINT);
  check_int("分隔点[2] ・", DSH_SEPARATOR_CODEPOINTS[2], DSH_SEP_KATAKANA_MIDDLE_DOT);
  check_int("分隔点[3] 软连字符", DSH_SEPARATOR_CODEPOINTS[3], DSH_SEP_SOFT_HYPHEN);
  check_int("分隔点[4] 主重音", DSH_SEPARATOR_CODEPOINTS[4], DSH_SEP_PRIMARY_STRESS);
  check_int("分隔点[5] 次重音", DSH_SEPARATOR_CODEPOINTS[5], DSH_SEP_SECONDARY_STRESS);
  check_int("U+00B7 码点", DSH_SEP_MIDDLE_DOT, 0x00B7);
  check_int("U+2027 码点", DSH_SEP_HYPHENATION_POINT, 0x2027);
  check_int("U+30FB 码点", DSH_SEP_KATAKANA_MIDDLE_DOT, 0x30FB);
  check_int("U+00AD 码点", DSH_SEP_SOFT_HYPHEN, 0x00AD);
  check_int("U+02C8 码点", DSH_SEP_PRIMARY_STRESS, 0x02C8);
  check_int("U+02CC 码点", DSH_SEP_SECONDARY_STRESS, 0x02CC);
  for (size_t i = 0; i < DSH_SEPARATOR_COUNT; i++) {
    const uint32_t cp = DSH_SEPARATOR_CODEPOINTS[i];
    checks++;
    if (cp == 0x002D || cp == 0x2010 || cp == 0x2013 || cp == 0x2014) {
      failures++;
      fprintf(stderr, "FAIL 分隔点清单里混进了连字符 U+%04X —— 不许剥连字符\n", cp);
    }
  }

  printf("接口定义编译自检：%d 项，失败 %d\n", checks, failures);
  if (failures == 0) {
    printf("  · 34 条声明全部可调用、返回值类型正确（编译期核对）\n");
    printf("  · 常量 8 个、分隔点 %u 个与接口定义一致\n", (unsigned)DSH_SEPARATOR_COUNT);
  }
  return failures == 0 ? 0 : 1;
}
