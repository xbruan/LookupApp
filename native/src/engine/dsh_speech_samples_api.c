/* ── 词典取样（`dsh_speech_dict_samples`）：从当前词典里挑几条**真的有录音**的词条 ──
   内核挑候选、真解词条正文找录音、判语种、算那几句人话、守预算（6 条 / 最多扫 60 个候选 / 400 ms）；
   可播地址是平台形状，内核只给 `audioKey` 由外壳拼。⚠️ 今天没有产品流程调它，**别看到没人调就删掉**。 */

#include "dsh_lookup.h"
#include "audio/dsh_dictsample.h"
#include "dict/dsh_mdx.h"
#include "dsh_internal.h"
#include "engine/dsh_dicts.h"
#include "engine/dsh_engine_internal.h"
#include "engine/dsh_settings.h"
#include "json_reader.h"
#include "json_writer.h"
#include "platform/dsh_time.h"
#include "text/dsh_language.h"

#include <stdio.h>
#include <string.h>

/** 一次量内置录音：最多挑几条样本 / 最多扫几个候选 / 最多花多少毫秒 */
#define DSH_DICTSAMPLE_COUNT 6
#define DSH_DICTSAMPLE_MAX_SCAN 60
#define DSH_DICTSAMPLE_BUDGET_MS 400

/** 候选取样位置的上限（`dsh_dictsample_slots` 的缓冲）*/
#define DSH_DICTSAMPLE_SLOT_CAP 256

/** 已解过的候选名记得住多少条（去重用；超出就只对本条样本表去重）*/
#define DSH_DICTSAMPLE_TRIED_CAP 512

/** 当前词典的 id / 显示名（`dict_id` 为空时用它）。
 * ★ 与 `dsh_speech_gains_api.c` 的 `current_dict` 同一条约定：字段可能是空的，那时**兜底取第一本**。 */
static void current_dict(const struct dsh_settings *s, const char **id, const char **title) {
  const char *cid = dsh_settings_current_dict_id(s);
  const dsh_stored_dict *d = (cid != NULL && cid[0] != '\0') ? dsh_settings_dict_by_id(s, cid) : NULL;
  if (d == NULL) d = dsh_settings_dict_at(s, 0);
  *id = (d != NULL) ? d->id : "";
  *title = (d != NULL) ? dsh_settings_dict_display_name(d, NULL) : "";
}

/** 这本词典有没有资源卷（`.mdd`）—— 「它到底有没有录音可量」的检查标准（与增益那条同源）*/
static int dict_has_audio(const struct dsh_settings *s, const char *dict_id) {
  const dsh_stored_dict *d;
  if (s == NULL || dict_id == NULL || dict_id[0] == '\0') return 0;
  d = dsh_settings_dict_by_id(s, dict_id);
  return (d != NULL && d->mdd_count > 0) ? 1 : 0;
}

/** 已经解过的名字里有没有它（去重；表满就不再记新的 —— 预算本来就不允许扫很多）*/
static int tried_before(char **tried, int64_t *tried_count, const char *word) {
  int64_t i;
  for (i = 0; i < *tried_count; i++) {
    if (strcmp(tried[i], word) == 0) return 1;
  }
  if (*tried_count < DSH_DICTSAMPLE_TRIED_CAP) {
    char *copy = dsh_mem_strdup(word);
    if (copy != NULL) {
      tried[*tried_count] = copy;
      (*tried_count)++;
    }
  }
  return 0;
}

/** 收一条候选：真有**词目**录音才算（例句不算 —— 量的是「点发音按钮会听到的那一段」）*/
static int take_candidate(dsh_engine *engine, const char *dict_id, const char *word,
                          const char *dict_title, dsh_json *samples, int64_t *scanned) {
  char *audio_json = NULL;
  dsh_json_doc *doc = NULL;
  const dsh_json_node *root;
  const char *key = NULL;
  const char *kind = NULL;
  const char *language = "en";
  const char *basis = NULL;
  const char *basis_text = NULL;
  const char *label;
  int taken = 0;

  (*scanned)++;
  if (dsh_speech_dict_audio(engine, dict_id, word, &audio_json) != DSH_OK) {
    if (audio_json != NULL) dsh_release(audio_json);
    return 0;
  }
  if (audio_json == NULL || dsh_json_parse(audio_json, strlen(audio_json), &doc) != 0) {
    if (audio_json != NULL) dsh_release(audio_json);
    return 0;
  }
  root = dsh_json_doc_root(doc);
  key = dsh_json_str_value(dsh_json_object_get(root, "audioKey"), NULL);
  kind = dsh_json_str_value(dsh_json_object_get(root, "kind"), NULL);
  /* ⚠️ 只认 `kind` 为 `entry`：例句录音也会被那一条解出来，而量音量要的是
   * 「点发音按钮会听到的那一段」—— 收进例句就量错了对象。 */
  if (dsh_json_bool_value(dsh_json_object_get(root, "found")) && key != NULL && key[0] != '\0' &&
      kind != NULL && strcmp(kind, "entry") == 0) {
    dsh_language_decide(word, dict_title, NULL, NULL, &language, &basis, &basis_text);
    label = dsh_language_name(language);
    dsh_json_object_begin(samples);
    dsh_json_kv_str(samples, "word", word);
    /* `audioKey` = `.mdd` 里那条文件的实际键名（外壳拿它拼地址，界面上那份 `file` 就是它）*/
    dsh_json_kv_str(samples, "audioKey", key);
    dsh_json_kv_str(samples, "language", language);
    dsh_json_kv_str(samples, "languageLabel", (label != NULL) ? label : "");
    dsh_json_object_end(samples);
    taken = 1;
  }
  dsh_json_doc_free(doc);
  dsh_release(audio_json);
  return taken;
}

enum dsh_error dsh_speech_dict_samples(dsh_engine *engine, const char *dict_id, char **out_json) {
  const struct dsh_settings *s;
  const char *id;
  const char *title;
  const dsh_stored_dict *stored;
  dsh_mdx *mdx;
  int64_t *counts = NULL;
  dsh_dictsample_slot *slots = NULL;
  char **tried = NULL;
  int64_t tried_count = 0;
  int64_t scanned = 0;
  int64_t slot_count = 0;
  int64_t block_count;
  int64_t i;
  int64_t started;
  int64_t elapsed;
  int budget_hit;
  int found = 0;
  dsh_json *j;

  dsh_clear_last_error();
  if (out_json == NULL) {
    dsh_set_last_error("dsh_speech_dict_samples：out_json 不能为空");
    return DSH_E_INVALID_ARG;
  }
  if (engine == NULL) {
    dsh_set_last_error("dsh_speech_dict_samples：引擎无效");
    return DSH_E_INVALID_ARG;
  }

  s = dsh_engine_settings(engine);
  if (dict_id != NULL && dict_id[0] != '\0') {
    id = dict_id;
    stored = dsh_settings_dict_by_id(s, id);
    if (stored == NULL) {
      dsh_set_last_error("dsh_speech_dict_samples：词库里没有这本词典（id=%s）", id);
      return DSH_E_NOT_FOUND;
    }
    title = dsh_settings_dict_display_name(stored, NULL);
  } else {
    current_dict(s, &id, &title);
    stored = (id[0] != '\0') ? dsh_settings_dict_by_id(s, id) : NULL;
  }

  j = dsh_json_new();
  if (j == NULL) {
    dsh_set_last_error("dsh_speech_dict_samples：内存不足");
    return DSH_E_OOM;
  }
  dsh_json_object_begin(j);
  dsh_json_key(j, "samples");
  dsh_json_array_begin(j);

  if (stored == NULL || id[0] == '\0') {
    /* 一本词典都没有：这一档与「这本词典没有录音」要做的事完全不同 */
    dsh_json_array_end(j);
    dsh_json_kv_bool(j, "ok", 0);
    dsh_json_kv_str(j, "message",
                    "还没有添加词典。先导入一本带音频卷（.mdd）的词典，再来量它的原录音音量。");
    dsh_json_kv_str(j, "dictId", "");
    dsh_json_kv_str(j, "dictTitle", "");
    dsh_json_kv_i64(j, "count", 0);
    dsh_json_kv_i64(j, "scanned", 0);
    dsh_json_kv_i64(j, "elapsedMs", 0);
    dsh_json_kv_bool(j, "budgetHit", 0);
    dsh_json_object_end(j);
    goto finish;
  }

  if (dsh_dicts_ensure_loaded(engine, id) != 0) {
    char buf[512];
    snprintf(buf, sizeof(buf), "《%s》还没加载好，稍等一下再点。", title);
    dsh_json_array_end(j);
    dsh_json_kv_bool(j, "ok", 0);
    dsh_json_kv_str(j, "message", buf);
    dsh_json_kv_str(j, "dictId", id);
    dsh_json_kv_str(j, "dictTitle", title);
    dsh_json_kv_i64(j, "count", 0);
    dsh_json_kv_i64(j, "scanned", 0);
    dsh_json_kv_i64(j, "elapsedMs", 0);
    dsh_json_kv_bool(j, "budgetHit", 0);
    dsh_json_object_end(j);
    goto finish;
  }

  mdx = dsh_dicts_peek(engine, id);
  block_count = (mdx != NULL) ? dsh_mdx_key_block_count(mdx) : 0;

  /* 每块有几条词条：均匀撒点要按**整本书的词条序号**分（不是按块，见 .h 里那个坑）*/
  if (block_count > 0) {
    counts = (int64_t *)dsh_mem_alloc((size_t)block_count * sizeof(int64_t));
    slots = (dsh_dictsample_slot *)dsh_mem_alloc((size_t)DSH_DICTSAMPLE_SLOT_CAP *
                                                 sizeof(dsh_dictsample_slot));
    tried = (char **)dsh_mem_alloc((size_t)DSH_DICTSAMPLE_TRIED_CAP * sizeof(char *));
    if (counts == NULL || slots == NULL || tried == NULL) {
      if (counts != NULL) dsh_release(counts);
      if (slots != NULL) dsh_release(slots);
      if (tried != NULL) dsh_release(tried);
      dsh_json_array_end(j);
      dsh_json_object_end(j);
      dsh_json_free(j);
      dsh_set_last_error("dsh_speech_dict_samples：内存不足");
      return DSH_E_OOM;
    }
    memset(tried, 0, (size_t)DSH_DICTSAMPLE_TRIED_CAP * sizeof(char *));
    for (i = 0; i < block_count; i++) {
      dsh_mdx_key_block blk;
      memset(&blk, 0, sizeof(blk));
      counts[i] = (dsh_mdx_key_block_at(mdx, i, &blk) == 0) ? blk.entry_count : 0;
    }
  }

  started = dsh_now_ms();

  /* ① **均匀撒网**：在全书范围内跳着取（一轮最多 DSH_DICTSAMPLE_MAX_SCAN 个位置）。
   * 预算检查放在**每一条之前**：解一条正文 + 到 .mdd 里找文件要十几毫秒，攒够 60 条就可能越过
   * 400 ms —— 与其让调用方干等，不如在这里停下、如实说「只找到 N 条」。 */
  if (block_count > 0) {
    slot_count = dsh_dictsample_slots(counts, block_count, DSH_DICTSAMPLE_MAX_SCAN, slots);
  }
  for (i = 0; i < slot_count && found < DSH_DICTSAMPLE_COUNT; i++) {
    char **keys = NULL;
    int64_t key_count = 0;
    int64_t start;
    int64_t offset;
    int64_t k;
    char word[256];
    int picked = 0;

    if (dsh_now_ms() - started > DSH_DICTSAMPLE_BUDGET_MS) break;
    if (dsh_mdx_block_keys(mdx, slots[i].block_index, &keys, &key_count) != 0) continue;
    if (key_count <= 0) {
      if (keys != NULL) dsh_mdx_free_keys(keys, key_count);
      continue;
    }
    /* 撒点给出的位置可能正好落在符号、缩写、`@@@LINK=` 上，所以**从那个位置往后找第一条
     * 像词条的**，找不到就绕回块首 —— 采样位置只决定扫到书的哪一段，不决定能不能取到名字。
     * ⚠️ **一个采样位置只给一条候选**：多给会让「均匀」失真（候选全挤在某一段的块里）。 */
    start = (slots[i].entry_index < key_count) ? slots[i].entry_index : key_count - 1;
    for (offset = 0; offset < key_count; offset++) {
      k = (start + offset) % key_count;
      if (dsh_dictsample_clean_key(keys[k], word, sizeof(word)) != 0) continue;
      if (dsh_dictsample_looks_like_headword(word)) {
        picked = 1;
        break;
      }
    }
    /* 已经解过的名字：这一格不算一条（参考实现同一句），继续下一格 */
    if (picked && !tried_before(tried, &tried_count, word) &&
        dsh_now_ms() - started <= DSH_DICTSAMPLE_BUDGET_MS) {
      if (take_candidate(engine, id, word, title, j, &scanned)) found++;
    }
    dsh_mdx_free_keys(keys, key_count);
  }

  /* ② **兜底补扫**：均匀那批带录音的不够 6 条时，按索引顺序把剩下的补上 ——
   * 均匀撒点是一张网，网眼之间可能正好漏掉「录音集中在某一段」的词典；同样受预算约束、
   * 扫过的词不重复解。⚠️ 这一趟**按块走**，不先物化全书键表（大词典几十万条，那步就是几兆内存）。 */
  if (found < DSH_DICTSAMPLE_COUNT && block_count > 0) {
    for (i = 0; i < block_count && found < DSH_DICTSAMPLE_COUNT; i++) {
      char **keys = NULL;
      int64_t key_count = 0;
      int64_t k;
      if (dsh_now_ms() - started > DSH_DICTSAMPLE_BUDGET_MS) break;
      if (dsh_mdx_block_keys(mdx, i, &keys, &key_count) != 0) continue;
      for (k = 0; k < key_count && found < DSH_DICTSAMPLE_COUNT; k++) {
        char word[256];
        if (dsh_dictsample_clean_key(keys[k], word, sizeof(word)) != 0) continue;
        if (!dsh_dictsample_looks_like_headword(word)) continue;
        if (tried_before(tried, &tried_count, word)) continue;
        if (dsh_now_ms() - started > DSH_DICTSAMPLE_BUDGET_MS) break;
        if (take_candidate(engine, id, word, title, j, &scanned)) found++;
      }
      dsh_mdx_free_keys(keys, key_count);
    }
  }

  elapsed = dsh_now_ms() - started;
  budget_hit = (elapsed > DSH_DICTSAMPLE_BUDGET_MS) ? 1 : 0;

  dsh_json_array_end(j);
  dsh_json_kv_str(j, "dictId", id);
  dsh_json_kv_str(j, "dictTitle", title);
  dsh_json_kv_i64(j, "count", found);
  dsh_json_kv_i64(j, "scanned", scanned);
  dsh_json_kv_i64(j, "elapsedMs", elapsed);
  dsh_json_kv_bool(j, "budgetHit", budget_hit);

  if (found == 0) {
    /* 一条都没找到时**必须说人话**，而且两种情形要做的事完全不同：没有资源卷 = 这本压根
     * 没有录音（无事可做）；有资源卷却扫不到 = 多半是 `.mdd` 没关联上（加上音频卷就有声了）。 */
    char buf[768];
    dsh_json_kv_bool(j, "ok", 0);
    if (dict_has_audio(s, id)) {
      snprintf(buf, sizeof(buf),
               "这本词典里没找到带录音的词条（在 %lld 个候选词上扫了 %lld ms%s）。"
               "多半是音频卷（.mdd）没有关联上，或这本词典本来就没有词目录音。",
               (long long)scanned, (long long)elapsed, budget_hit ? "，到时间上限就停了" : "");
    } else {
      snprintf(buf, sizeof(buf), "《%s》没有资源卷（.mdd），它没有自带录音可量。", title);
    }
    dsh_json_kv_str(j, "message", buf);
  } else {
    char buf[512];
    dsh_json_kv_bool(j, "ok", 1);
    /* 扫不满就说清是「没扫够」而不是「只有这么多」—— 调用方会把这句话显示出来 */
    if (found < DSH_DICTSAMPLE_COUNT) {
      snprintf(buf, sizeof(buf),
               "只找到 %lld 条带录音的词条（扫了 %lld 个候选，用了 %lld ms%s）。",
               (long long)found, (long long)scanned, (long long)elapsed,
               budget_hit ? "，到时间上限就停了" : "");
      dsh_json_kv_str(j, "message", buf);
    } else {
      dsh_json_kv_str(j, "message", "");
    }
  }
  dsh_json_object_end(j);

finish:
  {
    char *out = dsh_json_take(j);
    dsh_json_free(j);
    if (counts != NULL) dsh_release(counts);
    if (slots != NULL) dsh_release(slots);
    if (tried != NULL) {
      for (i = 0; i < tried_count; i++) {
        if (tried[i] != NULL) dsh_release(tried[i]);
      }
      dsh_release(tried);
    }
    if (out == NULL) {
      dsh_set_last_error("dsh_speech_dict_samples：内存不足");
      return DSH_E_OOM;
    }
    *out_json = out;
  }
  return DSH_OK;
}
