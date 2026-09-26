/* ── 音量增益（`speech.dictGainDb` / `speech.systemGainDb`）：判断与文案只许在内核这一处 ──
   能不能调 = 有没有当前词典 + 这本有没有资源卷（.mdd）；不能调时两种情形两句话、不许合成一句。
   归一化夹到 [-24, +12] dB 取整到 0.1（只在 `set` 做）；落库要发**整份 speech 对象**当补丁（发局部补丁会抹掉别的键）。 */

#include "dsh_internal.h"
#include "engine/dsh_engine_internal.h"
#include "engine/dsh_settings.h"
#include "json_reader.h"
#include "json_writer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** 增益的上下限与精度：夹到 [-24, +12] dB、取整到 0.1 */
#define DSH_GAIN_MIN (-24.0)
#define DSH_GAIN_MAX (12.0)

static double gain_normalize(double value) {
  double rounded;
  if (!(value == value)) return 0.0; /* NaN */
  if (value < DSH_GAIN_MIN) value = DSH_GAIN_MIN;
  if (value > DSH_GAIN_MAX) value = DSH_GAIN_MAX;
  /* 取整到 0.1（用 round 而不是截断 —— 截断会让 +0.35 这类值总往小的一边偏）*/
  rounded = floor(value * 10.0 + 0.5) / 10.0;
  /* 别让 `-0.0` 露出去：显示成 `-0` 很难看，也不等于「没设过」那一个 0 */
  if (rounded == 0.0) rounded = 0.0;
  return rounded;
}

/* ── 读设置里那一节：把 speech 原文抠出来，再读两个键 ────────────────────── */

/** 把某个键后面那个值（对象 / 数组 / 标量）的原文抠出来；没有就回 NULL */
static char *raw_value(const char *json, const char *key) {
  char pattern[96];
  const char *at;
  const char *start;
  const char *scan;
  int depth = 0;
  int in_string = 0;
  char open, close;

  if (json == NULL || key == NULL) return NULL;
  snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  at = strstr(json, pattern);
  if (at == NULL) return NULL;
  start = at + strlen(pattern);
  while (*start == ' ' || *start == ':') start++;
  if (*start == '\0') return NULL;

  if (*start == '{' || *start == '[') {
    open = *start;
    close = (open == '{') ? '}' : ']';
    for (scan = start; *scan != '\0'; scan++) {
      char c = *scan;
      if (in_string) {
        if (c == '\\') { scan++; continue; }
        if (c == '"') in_string = 0;
        continue;
      }
      if (c == '"') { in_string = 1; continue; }
      if (c == open) depth++;
      else if (c == close) {
        depth--;
        if (depth == 0) {
          size_t len = (size_t)(scan - start) + 1;
          char *out = (char *)dsh_mem_alloc(len + 1);
          if (out == NULL) return NULL;
          memcpy(out, start, len);
          out[len] = '\0';
          return out;
        }
      }
    }
    return NULL;
  }

  if (*start == '"') {
    for (scan = start + 1; *scan != '\0'; scan++) {
      if (*scan == '\\') { scan++; continue; }
      if (*scan == '"') {
        size_t len = (size_t)(scan - start) + 1;
        char *out = (char *)dsh_mem_alloc(len + 1);
        if (out == NULL) return NULL;
        memcpy(out, start, len);
        out[len] = '\0';
        return out;
      }
    }
    return NULL;
  }

  /* 标量（数字 / true / false / null）*/
  {
    const char *stop = start;
    while (*stop != '\0' && *stop != ',' && *stop != '}') stop++;
    while (stop > start && (stop[-1] == ' ' || stop[-1] == '\n' || stop[-1] == '\r')) stop--;
    {
      size_t len = (size_t)(stop - start);
      char *out = (char *)dsh_mem_alloc(len + 1);
      if (out == NULL) return NULL;
      memcpy(out, start, len);
      out[len] = '\0';
      return out;
    }
  }
}

/** 从一份小对象里读某个键的数字值；没有 / 不是数字 → 回 fallback */
static double number_of(const char *json, const char *key, double fallback) {
  char *raw = raw_value(json, key);
  double value = fallback;
  if (raw != NULL) {
    if (strcmp(raw, "null") != 0) value = atof(raw);
    dsh_release(raw);
  }
  return value;
}

/** 把某个键**换掉**（没有就补上一条），逐字保留别的成员 ——
 * 这是「只改这两个键、别碰 speech 那一节里其它键」的关键。 */
static char *splice_key(const char *object_json, const char *key, const char *value_json) {
  const char *body;
  size_t body_len;
  char pattern[96];
  const char *at;
  size_t skip_to;
  size_t out_len;
  char *out;
  int in_string = 0;
  const char *scan;
  int depth = 0;
  char open, close;

  if (object_json == NULL || key == NULL || value_json == NULL) return NULL;
  body = object_json;
  while (*body == ' ') body++;
  if (*body != '{') return NULL;
  body_len = strlen(object_json);

  snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  at = strstr(object_json, pattern);

  if (at == NULL) {
    /* 补一条：插在最后一个 `}` 之前 */
    size_t cut = body_len;
    while (cut > 0 && object_json[cut - 1] != '}') cut--;
    if (cut == 0) return NULL;
    cut--; /* 指向 `}` */
    {
      /* 看看 `{` 之后是不是空的（决定要不要补逗号）*/
      const char *first = body + 1;
      int empty = 1;
      while (*first == ' ' || *first == '\n' || *first == '\r' || *first == '\t') first++;
      if (*first != '}') empty = 0;
      out_len = cut + strlen(pattern) + strlen(value_json) + 4;
      out = (char *)dsh_mem_alloc(out_len);
      if (out == NULL) return NULL;
      snprintf(out, out_len, "%.*s%s%s:%s%s}", (int)cut, object_json, empty ? "" : ",",
               pattern, value_json, "");
      return out;
    }
  }

  /* 换掉：从那个键名一直找到它那个值的结尾 */
  skip_to = (size_t)(at - object_json) + strlen(pattern);
  while (skip_to < body_len &&
         (object_json[skip_to] == ' ' || object_json[skip_to] == ':')) skip_to++;
  if (skip_to >= body_len) return NULL;

  open = object_json[skip_to];
  if (open == '{' || open == '[') {
    close = (open == '{') ? '}' : ']';
    for (scan = object_json + skip_to; *scan != '\0'; scan++) {
      char c = *scan;
      if (in_string) {
        if (c == '\\') { scan++; continue; }
        if (c == '"') in_string = 0;
        continue;
      }
      if (c == '"') { in_string = 1; continue; }
      if (c == open) depth++;
      else if (c == close) {
        depth--;
        if (depth == 0) { skip_to = (size_t)(scan - object_json) + 1; break; }
      }
    }
  } else if (open == '"') {
    for (scan = object_json + skip_to + 1; *scan != '\0'; scan++) {
      if (*scan == '\\') { scan++; continue; }
      if (*scan == '"') { skip_to = (size_t)(scan - object_json) + 1; break; }
    }
  } else {
    while (skip_to < body_len && object_json[skip_to] != ',' && object_json[skip_to] != '}') skip_to++;
  }

  out_len = (size_t)(at - object_json) + strlen(pattern) + 1 + strlen(value_json) +
            (body_len - skip_to) + 2;
  out = (char *)dsh_mem_alloc(out_len);
  if (out == NULL) return NULL;
  snprintf(out, out_len, "%.*s%s:%s%s", (int)(at - object_json), object_json, pattern, value_json,
           object_json + skip_to);
  return out;
}

/** 读出 `speech` 那一节的原文（没有就给一个空对象）*/
static char *speech_section(const struct dsh_settings *s) {
  char *all = dsh_settings_to_json(s);
  char *speech = NULL;
  if (all != NULL) {
    speech = raw_value(all, "speech");
    dsh_release(all);
  }
  if (speech == NULL) speech = dsh_mem_strdup("{}");
  return speech;
}

/** 这本词典有没有资源卷（.mdd）—— 「能不能调内置录音」的检查标准 */
static int dict_has_audio(const struct dsh_settings *s, const char *dict_id) {
  const dsh_stored_dict *d;
  if (s == NULL || dict_id == NULL || dict_id[0] == '\0') return 0;
  d = dsh_settings_dict_by_id(s, dict_id);
  if (d == NULL) return 0;
  return d->mdd_count > 0;
}

/** 铺一份 `VoiceGains` 视图（字段与那两句文案的约定见文件顶上）*/
static char *build_view(const struct dsh_settings *s, const char *dict_id, const char *dict_title,
                        int voice_count) {
  char *speech = speech_section(s);
  char *dict_gains = (speech != NULL) ? raw_value(speech, "dictGainDb") : NULL;
  double system_gain = (speech != NULL) ? number_of(speech, "systemGainDb", 0.0) : 0.0;
  double dict_gain = 0.0;
  int has_dict = (dict_id != NULL && dict_id[0] != '\0');
  int has_audio = dict_has_audio(s, dict_id);
  const char *title = (dict_title != NULL) ? dict_title : "";
  const char *dict_message;
  dsh_json *j;
  char *out;
  char buf[64];

  if (dict_gains != NULL && has_dict) {
    dict_gain = number_of(dict_gains, dict_id, 0.0);
  }

  /* 这两句话是给**手调**用的：说的是这本词典有没有自带录音可调，不带「已经替你算好了」的暗示；
   * 两种情形**两句话**（原因不同、办法也不同），不许合成一句。 */
  if (!has_dict) {
    dict_message = "还没有添加词典，先导入一本带音频卷（.mdd）的词典，才有自带录音可调。";
  } else if (!has_audio) {
    static char scratch[320];
    snprintf(scratch, sizeof(scratch), "《%s》没有资源卷（.mdd），它没有自带录音可调。", title);
    dict_message = scratch;
  } else {
    dict_message = "";
  }

  j = dsh_json_new();
  if (j == NULL) {
    if (speech != NULL) dsh_release(speech);
    if (dict_gains != NULL) dsh_release(dict_gains);
    return NULL;
  }
  dsh_json_object_begin(j);
  dsh_json_kv_str(j, "dictId", has_dict ? dict_id : "");
  dsh_json_kv_str(j, "dictTitle", has_dict ? title : "");
  snprintf(buf, sizeof(buf), "%.1f", gain_normalize(dict_gain));
  dsh_json_key(j, "dictGainDb");
  dsh_json_value_raw(j, buf, strlen(buf));
  snprintf(buf, sizeof(buf), "%.1f", gain_normalize(system_gain));
  dsh_json_key(j, "systemGainDb");
  dsh_json_value_raw(j, buf, strlen(buf));
  dsh_json_kv_bool(j, "dictAvailable", has_dict && has_audio);
  dsh_json_kv_str(j, "dictMessage", dict_message);
  dsh_json_kv_bool(j, "systemAvailable", voice_count > 0);
  dsh_json_kv_str(j, "systemMessage", voice_count > 0
                                           ? ""
                                           : "本机没探到可用的离线语音（系统语音这条路用不了）。");
  dsh_json_object_end(j);

  out = dsh_json_take(j);
  dsh_json_free(j);
  if (speech != NULL) dsh_release(speech);
  if (dict_gains != NULL) dsh_release(dict_gains);
  return out;
}

/** 当前词典的 id / 显示名（`dict_id` 为空时用它）。
 * ★ **「当前词典」这个字段可能是空的，那时兜底取第一本** —— 检查标准是「一本词典都没有」，
 * 不是「没设当前那本」：刚导入 / 刚删完那会儿字段是空的，直接报没有词典会让用户莫名其妙。 */
static void current_dict(const struct dsh_settings *s, const char **id, const char **title) {
  const char *cid = dsh_settings_current_dict_id(s);
  const dsh_stored_dict *d = (cid != NULL && cid[0] != '\0') ? dsh_settings_dict_by_id(s, cid) : NULL;
  if (d == NULL) d = dsh_settings_dict_at(s, 0); /* 兜底第一本（一本都没有时是 NULL）*/
  *id = (d != NULL) ? d->id : "";
  *title = (d != NULL) ? dsh_settings_dict_display_name(d, NULL) : "";
}

enum dsh_error dsh_speech_gains(dsh_engine *engine, const char *dict_id, int voice_count,
                                char **out_json) {
  const struct dsh_settings *s;
  const char *id;
  const char *title;
  char *view;
  struct dsh_settings *next = NULL;

  dsh_clear_last_error();
  if (out_json == NULL) {
    dsh_set_last_error("dsh_speech_gains：out_json 不能为空");
    return DSH_E_INVALID_ARG;
  }
  if (engine == NULL) {
    dsh_set_last_error("dsh_speech_gains：引擎无效");
    return DSH_E_INVALID_ARG;
  }

  s = dsh_engine_settings(engine);
  if (dict_id != NULL && dict_id[0] != '\0') {
    const dsh_stored_dict *d = dsh_settings_dict_by_id(s, dict_id);
    id = dict_id;
    title = (d != NULL) ? dsh_settings_dict_display_name(d, NULL) : dict_id;
  } else {
    current_dict(s, &id, &title);
  }

  view = build_view(s, id, title, voice_count);
  (void)next;
  if (view == NULL) {
    dsh_set_last_error("dsh_speech_gains：内存不足");
    return DSH_E_OOM;
  }
  *out_json = view;
  return DSH_OK;
}

enum dsh_error dsh_speech_gains_set(dsh_engine *engine, const char *dict_id, const char *patch_json,
                                    int voice_count, char **out_json) {
  const struct dsh_settings *s;
  char *speech = NULL;
  char *next_speech = NULL;
  char *value = NULL;
  char *patch = NULL;
  char *next_json = NULL;
  const char *id;
  const char *title;
  char buf[64];

  dsh_clear_last_error();
  if (out_json == NULL || patch_json == NULL) {
    dsh_set_last_error("dsh_speech_gains_set：出参不能为空");
    return DSH_E_INVALID_ARG;
  }
  if (engine == NULL) {
    dsh_set_last_error("dsh_speech_gains_set：引擎无效");
    return DSH_E_INVALID_ARG;
  }

  s = dsh_engine_settings(engine);
  if (dict_id != NULL && dict_id[0] != '\0') {
    const dsh_stored_dict *d = dsh_settings_dict_by_id(s, dict_id);
    id = dict_id;
    title = (d != NULL) ? dsh_settings_dict_display_name(d, NULL) : dict_id;
  } else {
    current_dict(s, &id, &title);
  }

  speech = speech_section(s);
  if (speech == NULL) {
    dsh_set_last_error("dsh_speech_gains_set：内存不足");
    return DSH_E_OOM;
  }
  next_speech = dsh_mem_strdup(speech);

  /* ── systemGainDb：传数字 = 设成它（归一化）；传 null = 清掉；没这个键 = 不改 ── */
  value = raw_value(patch_json, "systemGainDb");
  if (value != NULL) {
    char *replaced;
    if (strcmp(value, "null") == 0) {
      replaced = splice_key(next_speech, "systemGainDb", "null");
    } else {
      snprintf(buf, sizeof(buf), "%.1f", gain_normalize(atof(value)));
      replaced = splice_key(next_speech, "systemGainDb", buf);
    }
    if (replaced == NULL) goto oom;
    dsh_release(next_speech);
    next_speech = replaced;
    dsh_release(value);
    value = NULL;
  }

  /* ── dictGainDb：只改**这一本**那一格（增益按词典存）── */
  value = raw_value(patch_json, "dictGainDb");
  if (value != NULL && id[0] != '\0') {
    char *gains = raw_value(next_speech, "dictGainDb");
    char *map = (gains != NULL) ? gains : dsh_mem_strdup("{}");
    char *new_map = NULL;
    char *replaced;

    if (strcmp(value, "null") == 0) {
      /* 清掉这一本（设置页明确点了「重置」才会走到这儿）—— 整张表里去掉那一个键 */
      new_map = dsh_mem_strdup(map);
      if (new_map != NULL) {
        char pattern[256];
        char *at;
        snprintf(pattern, sizeof(pattern), "\"%s\"", id);
        at = strstr(new_map, pattern);
        if (at != NULL) {
          char *from = at;
          char *to = at + strlen(pattern);
          while (*to != '\0' && (*to == ' ' || *to == ':')) to++;
          if (*to == '"') { to++; while (*to != '\0' && *to != '"') to++; if (*to == '"') to++; }
          else { while (*to != '\0' && *to != ',' && *to != '}') to++; }
          /* 顺手把前面那个逗号也吃掉（如果它是最后一项，就吃它前面那个）*/
          if (from > new_map && from[-1] == ',') from--;
          else if (*to == ',') to++;
          memmove(from, to, strlen(to) + 1);
        }
      }
    } else {
      snprintf(buf, sizeof(buf), "%.1f", gain_normalize(atof(value)));
      new_map = splice_key(map, id, buf);
    }
    if (new_map == NULL) {
      if (gains != NULL) dsh_release(gains);
      dsh_release(map);
      goto oom;
    }
    replaced = splice_key(next_speech, "dictGainDb", new_map);
    if (gains != NULL) dsh_release(gains);
    dsh_release(map);
    if (replaced == NULL) { dsh_release(new_map); goto oom; }
    dsh_release(new_map);
    dsh_release(next_speech);
    next_speech = replaced;
    dsh_release(value);
    value = NULL;
  }
  if (value != NULL) { dsh_release(value); value = NULL; }

  /* 把**完整的** speech 对象当补丁发下去（别的键原样带着走）*/
  patch = (char *)dsh_mem_alloc(strlen(next_speech) + 32);
  if (patch == NULL) goto oom;
  sprintf(patch, "{\"speech\":%s}", next_speech);

  /* ★ **落盘必须交给 `dsh_engine_settings_set`，别自己换内存里那份** —— 只换内存就写不进
   * settings.json：界面拖滑块看着正常、一重启回到 0。它里头的顺序是先落盘再换内存（落盘失败
   * 内存不动）；它回的整份 JSON 这一版用不上，拿到就还掉。 */
  {
    char *ignored = NULL;
    if (dsh_engine_settings_set(engine, patch, &ignored) != DSH_OK) {
      const char *why = dsh_last_error_message();
      if (ignored != NULL) dsh_release(ignored);
      dsh_release(patch);
      dsh_release(next_speech);
      dsh_release(speech);
      dsh_set_last_error("dsh_speech_gains_set：增益存不下来（%s）", why != NULL ? why : "");
      if (why != NULL) dsh_release((void *)why);
      return DSH_E_IO;
    }
    if (ignored != NULL) dsh_release(ignored);
  }

  /* 三块中间态都还掉（别把 `speech` / `next_speech` / `patch` 留在活分配表里）*/
  dsh_release(patch);
  dsh_release(next_speech);
  dsh_release(speech);

  /* ★★ **换上新设置之后必须重新解析 id / 书名**：上面那步会释放旧的设置文档并装上新的，
   * 而开头取到的 `id` / `title` 正指向那份已释放的 arena —— `build_view` 会读到已 free 的内存
   * （只在 ASan 下暴露，release 版看着是对的，因为那块内存还没被别人改写，只是随时可能）。 */
  {
    const struct dsh_settings *after = dsh_engine_settings(engine);
    if (dict_id != NULL && dict_id[0] != '\0') {
      const dsh_stored_dict *d = dsh_settings_dict_by_id(after, dict_id);
      /* ⚠️ 找不到就退回**调用方给的 id 本身**（那是它的串，不是设置里的）——
       *    别留着旧设置里那个已经悬空的 title。 */
      title = (d != NULL) ? dsh_settings_dict_display_name(d, NULL) : dict_id;
    } else {
      current_dict(after, &id, &title);
    }
  }

  next_json = build_view(dsh_engine_settings(engine), id, title, voice_count);
  if (next_json == NULL) {
    dsh_set_last_error("dsh_speech_gains_set：内存不足");
    return DSH_E_OOM;
  }
  *out_json = next_json;
  return DSH_OK;

oom:
  if (value != NULL) dsh_release(value);
  if (patch != NULL) dsh_release(patch);
  if (next_speech != NULL) dsh_release(next_speech);
  if (speech != NULL) dsh_release(speech);
  dsh_set_last_error("dsh_speech_gains_set：内存不足");
  return DSH_E_OOM;
}
