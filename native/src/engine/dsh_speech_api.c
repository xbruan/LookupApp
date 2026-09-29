/* 接口定义 `speech` 组的实现：自带录音、音频准备、增益、音色展示名与朗读规划。
 * 红线：认不出格式就如实报错，不许让播放器报错误码、更不许拿 TTS 假装顶上。
 * 在线接口出错时经常照样回 200、正文却是错误页 —— 只信嗅探出来的 MIME，不认 Content-Type。 */

/* `dsh_speech_dict_audio`：词条正文 → 音频引用（与接口定义那条 `audioKeys` 同一份实现）→
 * 逐条对上 `.mdd` 资源卷（懒打开）。例句录音不算词目发音，挑的时候只认词目发音，没有就说「只有例句」；
 * 对不上文件的不能直接丢 —— 要留原因（「本来没有发音」与「音频卷没关联上」该说的话不同）。 */

#include "dsh_lookup.h"
#include "audio/dsh_audio.h"
#include "audio/dsh_doubao.h"
#include "audio/dsh_gainmath.h"
#include "audio/dsh_speechplan.h"
#include "audio/dsh_speex.h"
#include "dsh_internal.h"
#include "dict/dsh_entry_doc.h"
#include "engine/dsh_dicts.h"
#include "engine/dsh_engine_internal.h"
#include "engine/dsh_settings.h"
#include "engine/dsh_speech_overrides.h"
#include "json_reader.h"
#include "json_writer.h"
#include "text/dsh_language.h"

#include <stdio.h>
#include <string.h>

/** 一条已经尝试解析过的候选（键名 + 命中的 mdd 键 + 能不能播）*/
typedef struct {
  char *key;
  char *accent;
  int example;
  char *matched_key; /* NULL = 资源卷里没有 */
  int playable;
} resolved_audio;

static void resolved_free(resolved_audio *items, int64_t count) {
  if (items == NULL) return;
  for (int64_t i = 0; i < count; i++) {
    if (items[i].key != NULL) dsh_release(items[i].key);
    if (items[i].accent != NULL) dsh_release(items[i].accent);
    if (items[i].matched_key != NULL) dsh_release(items[i].matched_key);
  }
  dsh_release(items);
}

/* 把一条引用的键名换成候选写法，在 `.mdd` 卷里找第一个命中的键。
 * ⚠️ 两层候选缺一不可：外层换扩展名（`dsh_audio_candidate_keys`），内层换资源键名的六种写法
 * （`dsh_dicts_resource_key_forms`）—— 少了内层，键名带前导反斜杠的 `.mdd` 会一条都找不到。 */
static char *find_matched_key(dsh_engine *e, const char *dict_id, const char *key) {
  char **candidates = NULL;
  int64_t count = 0;
  if (dsh_audio_candidate_keys(key, &candidates, &count) != 0) return NULL;
  char *matched = NULL;
  for (int64_t i = 0; i < count && matched == NULL; i++) {
    char *forms[8] = {0};
    const int form_count = dsh_dicts_resource_key_forms(candidates[i], forms, 8);
    if (form_count < 0) break;
    for (int f = 0; f < form_count && matched == NULL; f++) {
      /* ⚠️ 用 `contains`（只查键在不在，不读记录字节）—— 规划阶段不许产字节。 */
      if (dsh_dicts_mdd_contains(e, dict_id, forms[f])) {
        matched = dsh_mem_strdup(forms[f]);
      }
    }
    dsh_dicts_key_forms_free(forms, form_count);
  }
  dsh_audio_keys_free(candidates, count);
  return matched;
}

/** 「这一层为什么不能走」的说明 —— 四种情形分开说 */
static char *explain_no_dict_audio(const resolved_audio *items, int64_t count) {
  if (count <= 0) {
    return dsh_mem_strdup("本词条没有自带录音（词典里也没有它的音频卷）");
  }
  int64_t missing = 0;
  int64_t unplayable = 0;
  int64_t examples = 0;
  int64_t first_unplayable = -1;
  for (int64_t i = 0; i < count; i++) {
    if (items[i].matched_key == NULL) {
      missing++;
    } else {
      if (!items[i].playable) {
        unplayable++;
        if (first_unplayable < 0) first_unplayable = i;
      }
      if (items[i].example) examples++;
    }
  }
  char buf[512];
  if (missing == count) {
    snprintf(buf, sizeof(buf),
             "词条里挂着 %lld 段录音，但词典的资源卷里没有这些文件 —— "
             "多半是音频卷（.mdd / .1.mdd …）没跟这本词典关联上",
             (long long)count);
    return dsh_mem_strdup(buf);
  }
  if (examples > 0 && examples + missing + unplayable == count) {
    return dsh_mem_strdup(unplayable > 0 ? "本词条只有例句录音，而且它们放不了"
                                         : "本词条只带例句录音，没有词目发音");
  }
  snprintf(buf, sizeof(buf), "词典的资源卷里没有这段录音（%s）",
           (first_unplayable >= 0) ? "格式放不了" : "文件不在");
  return dsh_mem_strdup(buf);
}

enum dsh_error dsh_speech_dict_audio(dsh_engine *engine, const char *dict_id,
                                            const char *key_text, char **out_json) {
  if (out_json == NULL) {
    dsh_set_last_error("dsh_speech_dict_audio：out_json 不能为空");
    return DSH_E_INVALID_ARG;
  }
  *out_json = NULL;
  if (engine == NULL) {
    dsh_set_last_error("dsh_speech_dict_audio：引擎无效");
    return DSH_E_STATE;
  }
  if (dict_id == NULL || dict_id[0] == '\0' || key_text == NULL || key_text[0] == '\0') {
    dsh_set_last_error("dsh_speech_dict_audio：dict_id 与 key_text 都不能为空");
    return DSH_E_INVALID_ARG;
  }

  const struct dsh_settings *s = dsh_engine_settings(engine);
  const dsh_stored_dict *d = (s != NULL) ? dsh_settings_dict_by_id(s, dict_id) : NULL;
  if (d == NULL) {
    dsh_set_last_error("dsh_speech_dict_audio：词库里没有这本词典（id=%s）", dict_id);
    return DSH_E_NOT_FOUND;
  }
  if (dsh_dicts_ensure_loaded(engine, dict_id) != 0) {
    const char *why = dsh_last_error_message();
    dsh_set_last_error("dsh_speech_dict_audio：《%s》加载不了：%s",
                       dsh_settings_dict_display_name(d, NULL), why != NULL ? why : "");
    if (why != NULL) dsh_release((void *)why);
    return DSH_E_NOT_FOUND;
  }

  /* 词条正文 → 音频引用 → 逐条对上资源卷 */
  dsh_mdx *mdx = dsh_dicts_peek(engine, dict_id);
  dsh_resolved r;
  dsh_resolve_entry(mdx, key_text, &r);

  resolved_audio *items = NULL;
  int64_t count = 0;
  char *reason = NULL;
  if (!r.found || r.definition == NULL) {
    /* 词条本身不存在：与「词条在但没录音」是两件事，话不能合成一句 */
    reason = dsh_mem_strdup("这本词典里没有这条词条");
  } else {
    dsh_audio_ref *refs = NULL;
    int64_t ref_count = 0;
    if (dsh_audio_extract(r.definition, (size_t)r.definition_len, &refs, &ref_count) != 0) {
      dsh_resolved_free(&r);
      return DSH_E_OOM;
    }
    if (ref_count > 0) {
      items = (resolved_audio *)dsh_mem_alloc((size_t)ref_count * sizeof(resolved_audio));
      if (items == NULL) {
        dsh_audio_refs_free(refs, ref_count);
        dsh_resolved_free(&r);
        return DSH_E_OOM;
      }
      memset(items, 0, (size_t)ref_count * sizeof(resolved_audio));
      for (int64_t i = 0; i < ref_count; i++) {
        items[count].key = dsh_mem_strdup(refs[i].key);
        items[count].accent = (refs[i].accent != NULL) ? dsh_mem_strdup(refs[i].accent) : NULL;
        items[count].example = refs[i].example;
        items[count].matched_key = find_matched_key(engine, dict_id, refs[i].key);
        items[count].playable =
            (items[count].matched_key != NULL) ? dsh_audio_looks_playable(items[count].matched_key)
                                               : 0;
        if (items[count].key == NULL) {
          resolved_free(items, ref_count);
          dsh_audio_refs_free(refs, ref_count);
          dsh_resolved_free(&r);
          return DSH_E_OOM;
        }
        count++;
      }
    }
    dsh_audio_refs_free(refs, ref_count);
  }

  /* 挑哪一条念：只认词目发音（例句不算），口音偏好交给 `dsh_audio_pick` 判 */
  int64_t chosen = -1;
  if (count > 0) {
    dsh_audio_candidate *view =
        (dsh_audio_candidate *)dsh_mem_alloc((size_t)count * sizeof(dsh_audio_candidate));
    if (view == NULL) {
      resolved_free(items, count);
      if (reason != NULL) dsh_release(reason);
      dsh_resolved_free(&r);
      return DSH_E_OOM;
    }
    for (int64_t i = 0; i < count; i++) {
      view[i].key = items[i].key;
      view[i].accent = items[i].accent;
      view[i].example = items[i].example;
      view[i].matched_key = (items[i].playable) ? items[i].matched_key : NULL;
    }
    /* 口音偏好（`speech.accent`：uk / us / auto）原样传给 `dsh_audio_pick` ——
     * 这里不做第二次归一：`auto` 与认不出的值由它统一退回「用先出现的那条」。
     * 别改回传 NULL，否则界面上改了偏英音/偏美音不会有任何反应。 */
    const dsh_speech_settings *speech = (s != NULL) ? dsh_settings_speech(s) : NULL;
    chosen = dsh_audio_pick(view, count, (speech != NULL) ? speech->accent : NULL);
    dsh_release(view);
  }

  const char *kind = "none";
  if (chosen >= 0) {
    kind = items[chosen].example ? "sentence" : "entry";
  } else if (count > 0) {
    /* 没挑到：要么只有例句，要么都不可播 —— 分清楚再说话 */
    int64_t playable_examples = 0;
    for (int64_t i = 0; i < count; i++) {
      if (items[i].example && items[i].playable) playable_examples++;
    }
    kind = (playable_examples > 0) ? "sentence" : "none";
  }
  if (chosen < 0 && reason == NULL) {
    reason = explain_no_dict_audio(items, count);
  }

  dsh_json *j = dsh_json_new();
  if (j == NULL) {
    resolved_free(items, count);
    if (reason != NULL) dsh_release(reason);
    dsh_resolved_free(&r);
    return DSH_E_OOM;
  }
  dsh_json_object_begin(j);
  dsh_json_kv_bool(j, "found", chosen >= 0);
  dsh_json_kv_str(j, "audioKey",
                  (chosen >= 0 && items[chosen].matched_key != NULL) ? items[chosen].matched_key
                                                                     : "");
  dsh_json_kv_str(j, "kind", kind);
  dsh_json_kv_str(j, "accent",
                  (chosen >= 0 && items[chosen].accent != NULL) ? items[chosen].accent : "");
  dsh_json_kv_str(j, "reason", reason != NULL ? reason : "");
  dsh_json_object_end(j);

  char *out = dsh_json_take(j);
  dsh_json_free(j);
  resolved_free(items, count);
  if (reason != NULL) dsh_release(reason);
  dsh_resolved_free(&r);
  if (out == NULL) {
    dsh_set_last_error("内存不足：自带录音的 JSON");
    return DSH_E_OOM;
  }
  *out_json = out;
  return DSH_OK;
}

/* ══ dsh_audio_prepare —— 把一段音频字节整成能播的东西 ══
 * 先看魔数再看扩展名（顺序不能反：mdd 键名是词典作者写的，写错扩展名是常事）；认不出就如实报错。
 * 能直接播的原样发出 + 嗅探出的 MIME；`.spx` 就地解成 WAV（`audio/dsh_speex.c` + `vendor/speex/`）。 */

/** 在前 `limit` 个字节里找一段 ASCII */
static int contains_ascii(const uint8_t *bytes, size_t len, size_t limit, const char *needle) {
  if (bytes == NULL || needle == NULL) return 0;
  const size_t n = strlen(needle);
  if (n == 0 || len < n) return 0;
  const size_t end = ((len < limit) ? len : limit) - n;
  for (size_t i = 0; i <= end; i++) {
    if (memcmp(bytes + i, needle, n) == 0) return 1;
  }
  return 0;
}

/** 认编码：只看魔数，不看扩展名（这一层拿不到键名）*/
static const char *detect_codec(const uint8_t *b, size_t len) {
  if (b != NULL && len >= 12) {
    if (b[0] == 0x4F && b[1] == 0x67 && b[2] == 0x67 && b[3] == 0x53) { /* OggS */
      /* ⚠️ 「这是不是 Ogg Speex」只有一处实现（`dsh_speex_looks_like`），
       *    解码那边也用同一个；两处各写一遍迟早会分叉。 */
      if (dsh_speex_looks_like(b, len)) return "spx";
      if (contains_ascii(b, len, 256, "OpusHead")) return "ogg-opus";
      return "ogg-vorbis";
    }
    if (b[0] == 0x52 && b[1] == 0x49 && b[2] == 0x46 && b[3] == 0x46) return "wav"; /* RIFF */
    if (b[0] == 0x49 && b[1] == 0x44 && b[2] == 0x33) return "mp3";                 /* ID3 */
    if (b[0] == 0xFF && (b[1] & 0xE0) == 0xE0) return "mp3";                        /* 帧同步 */
    if (b[4] == 0x66 && b[5] == 0x74 && b[6] == 0x79 && b[7] == 0x70) return "mp4"; /* ftyp */
    if (b[0] == 0x66 && b[1] == 0x4C && b[2] == 0x61 && b[3] == 0x43) return "flac"; /* fLaC */
  }
  return "unknown";
}

/** 按魔数认 MIME（认不出来返回 NULL —— 调用方再决定怎么说话）*/
static const char *sniff_mime(const uint8_t *b, size_t len) {
  if (b != NULL && len >= 12) {
    if (b[0] == 0x49 && b[1] == 0x44 && b[2] == 0x33) return "audio/mpeg";
    if (b[0] == 0xFF && (b[1] & 0xE0) == 0xE0) return "audio/mpeg";
    if (b[0] == 0x52 && b[1] == 0x49 && b[2] == 0x46 && b[3] == 0x46 && b[8] == 0x57 &&
        b[9] == 0x41 && b[10] == 0x56 && b[11] == 0x45) {
      return "audio/wav";
    }
    if (b[0] == 0x4F && b[1] == 0x67 && b[2] == 0x67 && b[3] == 0x53) return "audio/ogg";
    if (b[4] == 0x66 && b[5] == 0x74 && b[6] == 0x79 && b[7] == 0x70) return "audio/mp4";
    if (b[0] == 0x66 && b[1] == 0x4C && b[2] == 0x61 && b[3] == 0x43) return "audio/flac";
  }
  return NULL;
}

/** 接口定义里那个 `kind` 字段只认这三档；其余一律空串（靠 `reason` 说话）*/
static const char *kind_of_codec(const char *codec) {
  if (codec == NULL) return "";
  if (strcmp(codec, "wav") == 0) return "wav";
  if (strcmp(codec, "mp3") == 0) return "mp3";
  if (strcmp(codec, "spx") == 0) return "spx";
  return "";
}

enum dsh_error dsh_audio_prepare(const uint8_t *bytes, size_t len, uint8_t **out_bytes,
                                 size_t *out_len, char **out_meta_json) {
  if (out_bytes == NULL || out_len == NULL || out_meta_json == NULL) {
    dsh_set_last_error("dsh_audio_prepare：三个出参都不能为空");
    return DSH_E_INVALID_ARG;
  }
  *out_bytes = NULL;
  *out_len = 0;
  *out_meta_json = NULL;
  if (bytes == NULL || len == 0) {
    /* 空内容也是如实报错，不是「成功返回空」—— 宿主据此才能说一句话 */
    dsh_json *j = dsh_json_new();
    if (j == NULL) return DSH_E_OOM;
    dsh_json_object_begin(j);
    dsh_json_kv_str(j, "mime", "");
    dsh_json_kv_str(j, "kind", "");
    dsh_json_kv_bool(j, "decoded", 0);
    dsh_json_kv_bool(j, "playable", 0);
    dsh_json_kv_str(j, "reason", "音频内容是空的。");
    dsh_json_object_end(j);
    char *meta = dsh_json_take(j);
    dsh_json_free(j);
    if (meta == NULL) return DSH_E_OOM;
    *out_meta_json = meta;
    return DSH_OK;
  }

  const char *codec = detect_codec(bytes, len);
  const char *mime = sniff_mime(bytes, len);
  int decoded = 0;
  char reason[512];
  reason[0] = '\0';
  /* Speex 解出来的 WAV 单独放一份（要还给调用方；其余格式不需要它）*/
  uint8_t *spx_wav = NULL;
  size_t spx_wav_len = 0;
  int64_t spx_samples = 0;
  int spx_rate = 0;

  if (strcmp(codec, "spx") == 0) {
    /* Speex 就地解成 16bit PCM WAV：浏览器内核不解码它，LDOCE5 那类词典的自带录音全是它。
     * 解开了 → mime 报 `audio/wav`、`decoded = true`、字节就是 WAV（报 `audio/x-speex` 会让
     * 人以为塞了个放不了的东西）；解不开 → `audio/x-speex` + `playable = false` + 一句人话。 */
    dsh_speex_info info;
    const int rc = dsh_speex_decode_wav(bytes, len, &spx_wav, &spx_wav_len, reason,
                                       sizeof(reason), &info);
    if (rc == DSH_SPEEX_OK) {
      mime = "audio/wav";
      decoded = 1;
      spx_rate = info.rate;
      spx_samples = (int64_t)((spx_wav_len - 44) / (2 * (size_t)info.channels));
    } else {
      mime = "audio/x-speex";
      if (spx_wav != NULL) { /* 失败时它一定是 NULL，这里只是不留悬空指针 */
        dsh_release(spx_wav);
        spx_wav = NULL;
      }
      spx_wav_len = 0;
      if (rc == DSH_SPEEX_EOMEM) {
        /* 内存不足不是「这段录音坏了」：把内核那条更准确的话原样带出去 */
        const char *saved = dsh_last_error_message();
        snprintf(reason, sizeof(reason), "%s",
                 saved != NULL ? saved : "内存不足：Speex 解码");
        if (saved != NULL) dsh_release((void *)saved);
      }
    }
  } else if (mime == NULL) {
    /* 认不出来就当场如实说，别让播放器去报错误码 */
    snprintf(reason, sizeof(reason), "无法识别的音频格式（%s）。", codec);
  } else {
    decoded = 0; /* 能直接播的格式原样发出去，不走「解码」这一步 */
  }

  if (mime == NULL) mime = "application/octet-stream";
  const int playable = (reason[0] == '\0') ? 1 : 0;

  dsh_json *j = dsh_json_new();
  if (j == NULL) {
    if (spx_wav != NULL) dsh_release(spx_wav);
    return DSH_E_OOM;
  }
  dsh_json_object_begin(j);
  dsh_json_kv_str(j, "mime", mime);
  dsh_json_kv_str(j, "kind", kind_of_codec(codec));
  dsh_json_kv_bool(j, "decoded", decoded);
  dsh_json_kv_str(j, "reason", reason);
  /* 多给一个 `playable`：接口定义里那张形状没列它，多一个键不影响宿主；
   * 少了它宿主就得自己去读 reason 猜 —— 那是典型的「让界面做题」。 */
  dsh_json_kv_bool(j, "playable", playable);
  /* Speex 那一档再加两个只有内核知道的值：**解出来的**采样数与播放速率。
   * 别的格式不需要它们（宿主自己就能从字节里读出来），而 `.spx` 解完只剩 PCM 长度，
   * 采样率是内核按头里那个 `rate` 定的（见 dsh_speex.h 顶上 ①）。 */
  if (decoded) {
    dsh_json_kv_i64(j, "samples", spx_samples);
    dsh_json_kv_i64(j, "sampleRate", (int64_t)spx_rate);
  }
  dsh_json_object_end(j);
  char *meta = dsh_json_take(j);
  dsh_json_free(j);
  if (meta == NULL) {
    if (spx_wav != NULL) dsh_release(spx_wav);
    dsh_set_last_error("内存不足：音频元信息 JSON");
    return DSH_E_OOM;
  }

  /* 出参字节只在可播时才给（不可播给空，让宿主只看 reason）：少一次拷贝，
   * 而且「出参为空 + reason 非空」更容易被宿主正确处理。
   * Speex 那一档给的是解出来的 WAV，不是原字节。 */
  uint8_t *copy = NULL;
  if (playable) {
    const uint8_t *src = (spx_wav != NULL) ? spx_wav : bytes;
    const size_t src_len = (spx_wav != NULL) ? spx_wav_len : len;
    copy = (uint8_t *)dsh_mem_alloc(src_len);
    if (copy == NULL) {
      dsh_release(meta);
      if (spx_wav != NULL) dsh_release(spx_wav);
      dsh_set_last_error("内存不足：音频字节（%zu 字节）", src_len);
      return DSH_E_OOM;
    }
    memcpy(copy, src, src_len);
    *out_len = src_len;
  }
  if (spx_wav != NULL) dsh_release(spx_wav);
  *out_bytes = copy;
  *out_meta_json = meta;
  return DSH_OK;
}

/* ══ dsh_audio_apply_gain —— 系统离线语音那条路的音量 ══
 * 在把字节交给播放器之前缩一遍（施加在产字节那一侧，所以回包里的 `gainDb` 恒 0，
 * 前端再乘一次就是叠两遍）。算术全在 `audio/dsh_gainmath.c`；入参拷一份、改了没改都照给字节。 */
enum dsh_error dsh_audio_apply_gain(const uint8_t *bytes, size_t len, int32_t gain_tenths_db,
                                    uint8_t **out_bytes, size_t *out_len, char **out_meta_json) {
  uint8_t *copy;
  double applied = 0;
  char note[256];
  int changed;
  dsh_json *j;
  char *meta;

  if (out_bytes == NULL || out_len == NULL || out_meta_json == NULL) {
    dsh_set_last_error("dsh_audio_apply_gain：三个出参都不能为空");
    return DSH_E_INVALID_ARG;
  }
  *out_bytes = NULL;
  *out_len = 0;
  *out_meta_json = NULL;
  if (bytes == NULL || len == 0) {
    dsh_set_last_error("dsh_audio_apply_gain：bytes 与 len 都不能为空");
    return DSH_E_INVALID_ARG;
  }

  copy = (uint8_t *)dsh_mem_alloc(len);
  if (copy == NULL) {
    dsh_set_last_error("内存不足：增益那一份字节（%zu 字节）", len);
    return DSH_E_OOM;
  }
  memcpy(copy, bytes, len);
  changed = dsh_gain_apply_to_wav(copy, len, (double)gain_tenths_db / 10.0, &applied, note,
                                  sizeof(note));

  j = dsh_json_new();
  if (j == NULL) {
    dsh_release(copy);
    dsh_set_last_error("内存不足：增益元信息 JSON");
    return DSH_E_OOM;
  }
  dsh_json_object_begin(j);
  dsh_json_kv_bool(j, "changed", changed);
  dsh_json_key(j, "appliedDb");
  dsh_json_double(j, applied);
  dsh_json_kv_str(j, "note", note);
  dsh_json_object_end(j);
  meta = dsh_json_take(j);
  dsh_json_free(j);
  if (meta == NULL) {
    dsh_release(copy);
    dsh_set_last_error("内存不足：增益元信息 JSON");
    return DSH_E_OOM;
  }
  *out_bytes = copy;
  *out_len = len;
  *out_meta_json = meta;
  return DSH_OK;
}

/* ══ dsh_speech_speaker_label —— 音色 id → 界面上的说法 ══
 * 表只有一张（`dsh_doubao_speaker_label`，在音频层与默认音色 id 住在一起），出参 utf8 由
 * 调用方 `dsh_release`。壳不许自己抄一张表，否则两张表迟早分叉。 */

enum dsh_error dsh_speech_speaker_label(const char *speaker, char **out_label) {
  const char *label;
  if (out_label == NULL) {
    dsh_set_last_error("dsh_speech_speaker_label：out_label 不能为空");
    return DSH_E_INVALID_ARG;
  }
  *out_label = NULL;
  label = dsh_doubao_speaker_label(speaker);
  *out_label = dsh_mem_strdup(label != NULL ? label : "");
  if (*out_label == NULL) {
    dsh_set_last_error("内存不足：音色展示名");
    return DSH_E_OOM;
  }
  return DSH_OK;
}

/* ══ dsh_speech_plan —— 这次朗读怎么念 ══
 * 层序是产品约定、用户不给选：词典自带 → 在线（豆包）→ 系统离线；都走不通时那句话要把
 * 「缺什么」说全。⚠️ 壳只探测（`voices_json`）、内核只判断；切段在 `audio/dsh_speechplan.c`。 */

/** 一层的规划结果（内部用；字符串要么借用、要么本函数自己分配）*/
typedef struct {
  const char *source;
  const char *label;
  char *detail; /* 自有（可为 NULL）*/
  char *reason; /* 自有（可为 NULL）*/
  int available;
  /* `chosen` 那几项按层不同：dict 用前两个、online 用 speaker、system 用 voice */
  char *audio_key;
  char *accent;
  const char *speaker;
  const dsh_voice *voice;
} speech_layer;

static void layer_free(speech_layer *l) {
  if (l->detail != NULL) dsh_release(l->detail);
  if (l->reason != NULL) dsh_release(l->reason);
  if (l->audio_key != NULL) dsh_release(l->audio_key);
  if (l->accent != NULL) dsh_release(l->accent);
  memset(l, 0, sizeof(*l));
}

/** 把三个入参拼成一句人话放进新分配的内存（NULL 的段直接跳过）*/
static char *join_parts(const char *a, const char *sep, const char *b) {
  if (a == NULL || a[0] == '\0') return (b != NULL && b[0] != '\0') ? dsh_mem_strdup(b) : NULL;
  if (b == NULL || b[0] == '\0') return dsh_mem_strdup(a);
  const size_t n = strlen(a) + strlen(sep) + strlen(b) + 1;
  char *out = (char *)dsh_mem_alloc(n);
  if (out == NULL) return NULL;
  snprintf(out, n, "%s%s%s", a, sep, b);
  return out;
}

/* ══ 「这一次的覆盖」（`overrides_json`）══
 * 「试听」要念用户正在填的音色、「平衡音量」要拿 `loudness: 0` / `gainDb: 0` 量中性电平
 * （照设置念，量到的电平会随滑块自己变）。解析在 `engine/dsh_speech_overrides.c`，两条语音接口共用。 */

enum dsh_error dsh_speech_plan(dsh_engine *engine, const char *text, const char *dict_id,
                               const char *voices_json, const char *overrides_json,
                               char **out_json) {
  if (out_json == NULL) {
    dsh_set_last_error("dsh_speech_plan：out_json 不能为空");
    return DSH_E_INVALID_ARG;
  }
  *out_json = NULL;
  if (engine == NULL) {
    dsh_set_last_error("dsh_speech_plan：引擎无效");
    return DSH_E_STATE;
  }
  if (text == NULL) text = "";

  /* 「这一次的覆盖」解析失败就如实拒，不猜 */
  dsh_speech_overrides ov;
  if (dsh_speech_overrides_parse(overrides_json, &ov, "dsh_speech_plan") != 0) {
    return DSH_E_INVALID_ARG;
  }

  /* ⚠️ 「还没有要念的东西」（空串 / 全是空白）必须单独一档：系统离线那一层只关心有没有
   * 这个语种的嗓子、不关心念什么，少了它就会把空文本规划成 `system` + `enabled:true`，
   * 界面上按钮亮着却合成 0 字节音频。判空白走 `dsh_speech_has_text`（与切段同一份），界面不许自判。 */
  const int nothing_to_read = !dsh_speech_has_text(text);
  static const char *const NOTHING_TO_READ = "没有要念的文本。";

  const struct dsh_settings *s = dsh_engine_settings(engine);
  const dsh_speech_settings *sp = (s != NULL) ? dsh_settings_speech(s) : NULL;
  const char *accent = (sp != NULL) ? sp->accent : NULL;
  const char *voice_id = (sp != NULL) ? sp->voice_id : NULL;

  /* 起点那本：入参为空 = 当前词典（与 `dsh_speech_dict_audio` 同一条入口规矩）*/
  const char *start_id = dict_id;
  if (start_id == NULL || start_id[0] == '\0') start_id = dsh_settings_current_dict_id(s);
  const dsh_stored_dict *start =
      (s != NULL && start_id != NULL) ? dsh_settings_dict_by_id(s, start_id) : NULL;

  /* ── ① 语种（内核判，界面照抄）──────────────────── */
  const char *language = "en";
  const char *basis = NULL;
  const char *basis_text = NULL;
  dsh_language_decide(text, (start != NULL) ? dsh_settings_dict_display_name(start, NULL) : NULL,
                      NULL, NULL, &language, &basis, &basis_text);
  /* 覆盖里的 `language`：界面为某个音色试听/量电平时显式给语种（它更清楚用户在试听哪个），
   * 同时影响 online 的音色选择与 system 的嗓子挑选 —— 就是「按这个语种念」的意思。 */
  if (ov.has_language) {
    language = ov.language;
    basis = "override";
    basis_text = NULL;
  }
  const int mixed = dsh_language_is_mixed(text);
  const char *language_name = dsh_language_name(language);
  if (language_name == NULL || language_name[0] == '\0') language_name = language;

  speech_layer layers[3];
  memset(layers, 0, sizeof(layers));
  layers[0].source = "dict";
  layers[0].label = "词典自带音频";
  layers[1].source = "online";
  layers[1].label = "豆包语音（在线）";
  layers[2].source = "system";
  layers[2].label = "系统语音（离线）";

  char *dict_audio_json = NULL;
  dsh_json_doc *dict_audio_doc = NULL;

  /* ── ② 词典自带音频 ────────────────────────────── */
  if (start_id != NULL && start_id[0] != '\0' && text[0] != '\0') {
    if (dsh_speech_dict_audio(engine, start_id, text, &dict_audio_json) == DSH_OK &&
        dict_audio_json != NULL && dsh_json_parse(dict_audio_json, strlen(dict_audio_json),
                                                  &dict_audio_doc) == 0) {
      const dsh_json_node *root = dsh_json_doc_root(dict_audio_doc);
      const dsh_json_node *found_node = dsh_json_object_get(root, "found");
      const char *audio_key = dsh_json_str_value(dsh_json_object_get(root, "audioKey"), NULL);
      const char *dict_accent = dsh_json_str_value(dsh_json_object_get(root, "accent"), NULL);
      const char *why = dsh_json_str_value(dsh_json_object_get(root, "reason"), NULL);
      if (dsh_json_bool_value(found_node) && audio_key != NULL && audio_key[0] != '\0') {
        layers[0].available = 1;
        layers[0].audio_key = dsh_mem_strdup(audio_key);
        layers[0].accent = dsh_mem_strdup(dict_accent != NULL ? dict_accent : "");
        {
          /* Detail 是「有地方铺开讲」时才显示的那一行：口音 + 键名 */
          const char *acc = dsh_audio_accent_label(dict_accent);
          layers[0].detail = join_parts(acc, " · ", audio_key);
        }
      } else {
        /* ⚠️ 不可用时直接引用 `explain_no_dict_audio` 给的原因（四种情形它已分清），
         * 不在这里另编一套说法。 */
        layers[0].reason = dsh_mem_strdup((why != NULL && why[0] != '\0')
                                              ? why
                                              : "本词条没有自带录音");
      }
    } else {
      layers[0].reason = dsh_mem_strdup("本词条没有自带录音（词典里也没有它的音频卷）");
    }
  } else {
    layers[0].reason = dsh_mem_strdup(nothing_to_read
                                          ? NOTHING_TO_READ
                                          : "没有起点词典（先加一本 .mdx，或把当前词典设上）");
  }

  /* ── ③ 在线（豆包）：只看填没填 Key（没有总开关）──────────── */
  {
    const char *key = (sp != NULL) ? sp->doubao_api_key : NULL;
    if (nothing_to_read) {
      layers[1].available = 0;
      layers[1].reason = dsh_mem_strdup(NOTHING_TO_READ);
    } else if (key == NULL || key[0] == '\0') {
      layers[1].available = 0;
      layers[1].reason = dsh_mem_strdup("还没有填 API Key（在「设置 → 语音」里填，旁边写着怎么申请）");
    } else {
      /* 覆盖里的 `voiceId` = 用户正在填/正在听的那个音色，优先于设置里存着的那个 */
      const char *speaker = ov.has_voice
                                ? ov.voice
                                : dsh_speech_doubao_speaker(
                                      language, mixed, (sp != NULL) ? sp->doubao_speaker_en : NULL,
                                      (sp != NULL) ? sp->doubao_speaker_zh : NULL);
      layers[1].available = 1;
      layers[1].speaker = speaker;
      /* `detail` 给的是界面上的说法（Dacey / Vivi），不是那个长 id；
       * 它在界面上就是「为什么走这条」那句（`why`，悬停提示里）。 */
      layers[1].detail = join_parts(dsh_doubao_speaker_label(speaker), " · ",
                                    mixed ? "中英混读" : language_name);
    }
  }

  /* ── ④ 系统语音（离线）：壳探测来的那张表 + 内核选择音色 ──
   * ⚠️ 「没给表」与「给了一张空表」是两件事：没给 = 壳探不到（说「问不到」），
   * 给了 `[]` = 探测成功但一个音色都没装（说「本机没有X语音（已装：无）」）。 */
  dsh_voice_list voices;
  voices.items = NULL;
  voices.count = 0;
  const int voices_given = (voices_json != NULL && voices_json[0] != '\0');
  if (dsh_speech_parse_voices(voices_json, 0, &voices) != 0) {
    /* 内存不足：把已分配的收掉再走，不做「半个规划」*/
    if (dict_audio_doc != NULL) dsh_json_doc_free(dict_audio_doc);
    if (dict_audio_json != NULL) dsh_release(dict_audio_json);
    for (int i = 0; i < 3; i++) layer_free(&layers[i]);
    return DSH_E_OOM;
  }
  {
    /* 覆盖里的 `voiceId` 在这一层是「本机那个嗓子的名字/id」（按 id 或 name 匹配）*/
    const char *want_voice = ov.has_voice ? ov.voice : voice_id;
    const dsh_voice *voice = nothing_to_read ? NULL
                                             : dsh_speech_pick_voice(&voices, language, accent, want_voice);
    if (voice == NULL) {
      layers[2].available = 0;
      if (nothing_to_read) {
        /* 空文本那一档的固定那句话 */
        layers[2].reason = dsh_mem_strdup(NOTHING_TO_READ);
      } else if (!voices_given) {
        /* 壳根本没给表：如实说「问不到」，不要说「本机没有这个语种」（那是另一件事）*/
        layers[2].reason = dsh_mem_strdup(
            "问不到本机的离线音色（壳没能列出音色表）—— 无网时这一层就走不了");
      } else {
        char installed[256];
        dsh_speech_describe_voices(&voices, installed, sizeof(installed));
        const size_t n = strlen(language_name) + strlen(installed) + 128;
        char *text_out = (char *)dsh_mem_alloc(n);
        if (text_out != NULL) {
          snprintf(text_out, n,
                   "本机没有%s语音（已装：%s）。可在 Windows 的「时间和语言 → 语音」里安装，"
                   "或改用在线发音。",
                   language_name, installed);
          layers[2].reason = text_out;
        } else {
          layers[2].reason = dsh_mem_strdup("本机没有这个语种的离线语音。");
        }
      }
    } else {
      layers[2].available = 1;
      layers[2].voice = voice;
      layers[2].detail = join_parts(voice->name, "（", language_name);
      if (layers[2].detail != NULL) {
        /* `join_parts` 只连两段，这里补上右括号（形如：名字（语种））*/
        const size_t n = strlen(layers[2].detail) + 2;
        char *fixed = (char *)dsh_mem_alloc(n);
        if (fixed != NULL) {
          snprintf(fixed, n, "%s）", layers[2].detail);
          dsh_release(layers[2].detail);
          layers[2].detail = fixed;
        }
      }
    }
  }

  /* ── ⑤ 挑第一条走得通的（顺序就是 layers 的顺序）──
   * ⚠️ 覆盖里的 `source` 在时只许走那一层：不可用就报它自己的原因，绝不悄悄回落到别的层
   * （「点了试听却用别的嗓子念」是最坏的失败形态）；认不出的 source 如实拒、不猜。 */
  int chosen = -1;
  int forced = -1;
  if (ov.has_source) {
    for (int i = 0; i < 3; i++) {
      if (strcmp(layers[i].source, ov.source) == 0) {
        forced = i;
        break;
      }
    }
    if (forced < 0) {
      if (dict_audio_doc != NULL) dsh_json_doc_free(dict_audio_doc);
      if (dict_audio_json != NULL) dsh_release(dict_audio_json);
      dsh_voices_free(&voices);
      for (int i = 0; i < 3; i++) layer_free(&layers[i]);
      dsh_set_last_error(
          "dsh_speech_plan：overrides_json 的 source 只能是 dict / online / system（给的是「%s」）",
          ov.source);
      return DSH_E_INVALID_ARG;
    }
    if (layers[forced].available) chosen = forced;
  } else {
    for (int i = 0; i < 3; i++) {
      if (layers[i].available) {
        chosen = i;
        break;
      }
    }
  }

  /* ── ⑥ 切段 ───────────────────────────────────── */
  dsh_speech_chunk *chunks = NULL;
  int64_t chunk_count = 0;
  if (dsh_speech_split(text, &chunks, &chunk_count) != 0) {
    if (dict_audio_doc != NULL) dsh_json_doc_free(dict_audio_doc);
    if (dict_audio_json != NULL) dsh_release(dict_audio_json);
    dsh_voices_free(&voices);
    for (int i = 0; i < 3; i++) layer_free(&layers[i]);
    return DSH_E_OOM;
  }

  /* ── ⑦ 出 JSON ─────────────────────────────────── */
  dsh_json *j = dsh_json_new();
  if (j == NULL) {
    dsh_speech_chunks_free(chunks, chunk_count);
    if (dict_audio_doc != NULL) dsh_json_doc_free(dict_audio_doc);
    if (dict_audio_json != NULL) dsh_release(dict_audio_json);
    dsh_voices_free(&voices);
    for (int i = 0; i < 3; i++) layer_free(&layers[i]);
    return DSH_E_OOM;
  }

  dsh_json_object_begin(j);
  dsh_json_kv_str(j, "source", (chosen >= 0) ? layers[chosen].source : "none");
  dsh_json_kv_bool(j, "enabled", chosen >= 0);
  dsh_json_key(j, "chosen");
  dsh_json_object_begin(j);
  if (chosen >= 0) {
    const speech_layer *l = &layers[chosen];
    dsh_json_kv_str(j, "source", l->source);
    dsh_json_kv_str(j, "label", l->label);
    dsh_json_kv_str(j, "detail", (l->detail != NULL) ? l->detail : "");
    if (strcmp(l->source, "dict") == 0) {
      dsh_json_kv_str(j, "audioKey", (l->audio_key != NULL) ? l->audio_key : "");
      dsh_json_kv_str(j, "accent", (l->accent != NULL) ? l->accent : "");
      /* `dictId`：这段录音住在哪一本的音频卷里，壳要用它拼 `/__sound__/` 地址。
       * ⚠️ 必须由内核给：入参 `dict_id` 为空时内核走的是「当前词典」，
       * 壳按自己那一侧推不出来（它不知道当前是哪本）。 */
      dsh_json_kv_str(j, "dictId", (start_id != NULL) ? start_id : "");
    } else if (strcmp(l->source, "online") == 0) {
      dsh_json_kv_str(j, "speaker", (l->speaker != NULL) ? l->speaker : "");
      dsh_json_kv_str(j, "language", language);
      /* `loudness`：这一次用的响度补偿（覆盖优先；否则按设置/内置表算，见
       * `dsh_doubao_loudness_for`）。壳把同一串覆盖交给 `dsh_speech_online_plan`，
       * 不必读这个键；给出来只是让「这次带了多少响度」有据可查。 */
      dsh_json_kv_int(j, "loudness",
                      ov.has_loudness
                          ? ov.loudness
                          : dsh_doubao_loudness_for(
                                l->speaker, (sp != NULL) ? sp->doubao_speaker_en : NULL,
                                (sp != NULL) ? sp->has_loudness_en : 0,
                                (sp != NULL) ? sp->loudness_en : 0,
                                (sp != NULL) ? sp->doubao_speaker_zh : NULL,
                                (sp != NULL) ? sp->has_loudness_zh : 0,
                                (sp != NULL) ? sp->loudness_zh : 0));
    } else {
      dsh_json_kv_str(j, "voiceId", (l->voice != NULL) ? l->voice->id : "");
      dsh_json_kv_str(j, "voiceName", (l->voice != NULL) ? l->voice->name : "");
      dsh_json_kv_str(j, "language", language);
      /* `rate`：语速（`speech.rate`，内核已夹在 -10..10 里）。
       * ⚠️ 必须由内核给、壳不许自己读设置：哪一层用哪个值、要不要夹是产品约定，
       * 壳去翻 `settings.json` 就是业务规则跑到了视图层。 */
      dsh_json_kv_int(j, "rate", (sp != NULL) ? sp->rate : 0);
    }
  }
  dsh_json_object_end(j);
  dsh_json_key(j, "chunks");
  dsh_json_array_begin(j);
  for (int64_t i = 0; i < chunk_count; i++) {
    dsh_json_object_begin(j);
    dsh_json_kv_str(j, "text", (chunks[i].text != NULL) ? chunks[i].text : "");
    dsh_json_kv_i64(j, "chars", chunks[i].chars);
    dsh_json_object_end(j);
  }
  dsh_json_array_end(j);
  dsh_json_kv_i64(j, "chunkCount", chunk_count);
  dsh_json_kv_str(j, "language", language);
  /* `why`：这次走这条的理由（供悬停提示用，界面上只显示这一句）*/
  if (chosen >= 0) {
    char *why = join_parts(layers[chosen].label, " · ", layers[chosen].detail);
    dsh_json_kv_str(j, "why", (why != NULL) ? why : layers[chosen].label);
    if (why != NULL) dsh_release(why);
  } else {
    dsh_json_kv_str(j, "why", "");
  }
  /* `disabledReason`：一层都走不通时那句话（三个分支）*/
  {
    char *reason = NULL;
    if (chosen >= 0) {
      reason = NULL;
    } else if (forced >= 0) {
      /* ★ 覆盖指定了某一层而它不可用：报那一层自己的原因，不拼「本机没有X语音…」那一套
       * （那套是「一层都走不通」时用的）—— 用户要知道的就是为什么这层不行。 */
      reason = dsh_mem_strdup(layers[forced].reason != NULL ? layers[forced].reason
                                                            : "这一层现在走不了。");
    } else if (nothing_to_read) {
      /* 空文本：三层的原因都是同一句，直接给那一句 */
      reason = dsh_mem_strdup(NOTHING_TO_READ);
    } else if (!voices_given) {
      /* 探测没成功 → 报探测那句话 */
      reason = dsh_mem_strdup(layers[2].reason != NULL
                                  ? layers[2].reason
                                  : "问不到本机的离线音色");
    } else if (layers[1].reason != NULL) {
      /* 本机没有这个语种的嗓子、豆包也没配好 —— 「缺什么」要说全 */
      const size_t n = strlen(language_name) + strlen(layers[1].reason) + 96;
      reason = (char *)dsh_mem_alloc(n);
      if (reason != NULL) {
        snprintf(reason, n, "本机没有%s语音，豆包这条路也还没配好（%s）。", language_name,
                 layers[1].reason);
      }
    } else if (layers[2].reason != NULL) {
      reason = dsh_mem_strdup(layers[2].reason);
    } else {
      reason = dsh_mem_strdup("这次没有可用的音源。");
    }
    dsh_json_kv_str(j, "disabledReason", (reason != NULL) ? reason : "");
    if (reason != NULL) dsh_release(reason);
  }
  /* 三层各自为什么通 / 不通（多出来的一个键，见接口定义里那段说明）*/
  dsh_json_key(j, "options");
  dsh_json_array_begin(j);
  for (int i = 0; i < 3; i++) {
    dsh_json_object_begin(j);
    dsh_json_kv_str(j, "source", layers[i].source);
    dsh_json_kv_bool(j, "available", layers[i].available);
    dsh_json_kv_str(j, "label", layers[i].label);
    dsh_json_kv_str(j, "detail", (layers[i].detail != NULL) ? layers[i].detail : "");
    dsh_json_kv_str(j, "reason", (layers[i].reason != NULL) ? layers[i].reason : "");
    dsh_json_object_end(j);
  }
  dsh_json_array_end(j);
  dsh_json_object_end(j);

  char *out = dsh_json_take(j);
  dsh_json_free(j);
  dsh_speech_chunks_free(chunks, chunk_count);
  if (dict_audio_doc != NULL) dsh_json_doc_free(dict_audio_doc);
  if (dict_audio_json != NULL) dsh_release(dict_audio_json);
  dsh_voices_free(&voices);
  for (int i = 0; i < 3; i++) layer_free(&layers[i]);
  if (out == NULL) {
    dsh_set_last_error("内存不足：朗读规划的 JSON");
    return DSH_E_OOM;
  }
  *out_json = out;
  return DSH_OK;
}
