/* 朗读规划层的实现（约定见 dsh_speechplan.h）。
 * 三件事：**挑离线音色**、**挑在线音色**、**切段**。全是纯函数 —— 不碰引擎、不读设置、
 * 不发请求，所以能被 单元测试单独钉住。 */

#include "audio/dsh_speechplan.h"

#include "dsh_internal.h"
#include "json_reader.h"
#include "text/dsh_language.h"

#include <stdio.h>
#include <string.h>

/* ── 小工具 ────────────────────────────────────────────────────────────── */

static char *dup_or_empty(const char *s) { return dsh_mem_strdup(s != NULL ? s : ""); }

/** 大小写不敏感的整串比较（音色 id / 语种码这两处都要求它）*/
static int eq_nocase(const char *a, const char *b) {
  if (a == NULL || b == NULL) return 0;
  while (*a != '\0' && *b != '\0') {
    char ca = *a;
    char cb = *b;
    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
    if (ca != cb) return 0;
    a++;
    b++;
  }
  return *a == '\0' && *b == '\0';
}

static int is_ascii_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/* 是不是「空白」：只判 ASCII 空白 + **全角空格 U+3000** 两类。
 * ⚠️ 与参考实现 **有已知的差异**（那一版按 JS 的 `\s`，收的是 Unicode 空白）：
 *    要念的文本来自词典词条与用户选区，实际会出现的空白就是这两类；
 *    为一个用不上的字符面把整张 Unicode 空白表搬进来不划算。 */
static int is_space_at(const char *s, size_t len, size_t at, size_t *out_bytes) {
  if (at >= len) return 0;
  const unsigned char c = (unsigned char)s[at];
  if (c < 0x80) {
    if (is_ascii_space((char)c)) {
      *out_bytes = 1;
      return 1;
    }
    return 0;
  }
  if (c == 0xE3 && at + 2 < len && (unsigned char)s[at + 1] == 0x80 &&
      (unsigned char)s[at + 2] == 0x80) { /* U+3000 全角空格 */
    *out_bytes = 3;
    return 1;
  }
  return 0;
}

/** 一个 UTF-8 码点的字节数（非法首字节按 1 字节吃掉 —— 绝不越界、绝不切半个字）*/
static size_t cp_bytes(const char *s, size_t len, size_t at) {
  if (at >= len) return 0;
  const unsigned char c = (unsigned char)s[at];
  size_t n = 1;
  if ((c & 0xE0) == 0xC0) n = 2;
  else if ((c & 0xF0) == 0xE0) n = 3;
  else if ((c & 0xF8) == 0xF0) n = 4;
  if (at + n > len) return 1;
  /* 续字节必须是 10xxxxxx，否则这个「首字节」是坏的 —— 只吃它自己 */
  for (size_t i = 1; i < n; i++) {
    if (((unsigned char)s[at + i] & 0xC0) != 0x80) return 1;
  }
  return n;
}

/** 从前到后数第 `cp` 个码点（返回 `s[0..cp)` 的字节数；`cp` 超了就返回 len）*/
static size_t bytes_for_cps(const char *s, size_t len, size_t cp) {
  size_t at = 0;
  for (size_t i = 0; i < cp && at < len; i++) at += cp_bytes(s, len, at);
  return at;
}

/** 这段文本有多少个码点 */
static int64_t count_cps(const char *s, size_t len) {
  int64_t n = 0;
  size_t at = 0;
  while (at < len) {
    at += cp_bytes(s, len, at);
    n++;
  }
  return n;
}

/* 去掉两端的空白（码点安全）。`*out_from` / `*out_to` 是**进参也是出参**：进来的是
 * 「要 trim 的那一段」，出去的是 trim 之后的那一段 —— ⚠️ 不许把进参丢掉（从 0..len 重来），
 * 那会让切段循环每一轮从头开始、一直写到数组外面去。 */
static void trim_range(const char *s, size_t len, size_t *out_from, size_t *out_to) {
  size_t from = (out_from != NULL) ? *out_from : 0;
  size_t to = (out_to != NULL) ? *out_to : len;
  if (to > len) to = len;
  if (from > to) from = to;
  for (;;) {
    size_t n = 0;
    if (from < to && is_space_at(s, len, from, &n)) {
      from += n;
      continue;
    }
    break;
  }
  while (to > from) {
    /* 从 to 往前找一个码点的起点：最多回退 3 字节 */
    size_t back = 1;
    while (back < 4 && to > from + back && ((unsigned char)s[to - back] & 0xC0) == 0x80) back++;
    size_t n = 0;
    size_t start = to - back;
    if (start >= from && is_space_at(s, len, start, &n) && start + n == to) {
      to = start;
      continue;
    }
    break;
  }
  *out_from = from;
  *out_to = to;
}

/* ── 音色表 ────────────────────────────────────────────────────────────── */

void dsh_voices_free(dsh_voice_list *list) {
  if (list == NULL) return;
  if (list->items != NULL) {
    for (int64_t i = 0; i < list->count; i++) {
      if (list->items[i].id != NULL) dsh_release(list->items[i].id);
      if (list->items[i].name != NULL) dsh_release(list->items[i].name);
      if (list->items[i].language != NULL) dsh_release(list->items[i].language);
      if (list->items[i].culture != NULL) dsh_release(list->items[i].culture);
    }
    dsh_release(list->items);
  }
  list->items = NULL;
  list->count = 0;
}

int dsh_speech_parse_voices(const char *json, size_t len, dsh_voice_list *out) {
  if (out == NULL) {
    dsh_set_last_error("dsh_speech_parse_voices：出参不能为空");
    return -1;
  }
  out->items = NULL;
  out->count = 0;
  if (json == NULL || json[0] == '\0') return 0; /* 壳没探测到：空表，不是错误 */
  if (len == 0) len = strlen(json);

  dsh_json_doc *doc = NULL;
  if (dsh_json_parse(json, len, &doc) != 0) {
    /* 宿主给的表读不动：**如实说**，但**不返回失败** —— 那会让整条朗读路变哑。
     * 这里按「没有音色」处理，由调用方在 `reason` 里说清「本机音色表读不动」。 */
    const char *saved = dsh_last_error_message();
    dsh_set_last_error("本机音色表读不动：%s", saved != NULL ? saved : "（没有说明）");
    if (saved != NULL) dsh_release((void *)saved);
    return 0;
  }
  const dsh_json_node *root = dsh_json_doc_root(doc);
  const int64_t n = dsh_json_array_len(root);
  if (n <= 0) {
    dsh_json_doc_free(doc);
    return 0; /* 不是数组 / 空数组：一样按「没有」处理 */
  }

  dsh_voice *items = (dsh_voice *)dsh_mem_alloc((size_t)n * sizeof(dsh_voice));
  if (items == NULL) {
    dsh_json_doc_free(doc);
    dsh_set_last_error("内存不足：本机音色表（%lld 项）", (long long)n);
    return -1;
  }
  memset(items, 0, (size_t)n * sizeof(dsh_voice));
  int64_t kept = 0;
  for (int64_t i = 0; i < n; i++) {
    const dsh_json_node *it = dsh_json_array_at(root, i);
    if (!dsh_json_is_object(it)) continue;
    const char *id = dsh_json_str_value(dsh_json_object_get(it, "id"), NULL);
    const char *name = dsh_json_str_value(dsh_json_object_get(it, "name"), NULL);
    const char *lang = dsh_json_str_value(dsh_json_object_get(it, "language"), NULL);
    const char *culture = dsh_json_str_value(dsh_json_object_get(it, "culture"), NULL);
    if (id == NULL || id[0] == '\0') continue; /* 没有 id 的音色挑中了也没法用 */
    /* 缺什么补什么：name 借 id；culture 缺就从 language 推 —— 都不成就留空，
     * 绝不为一项写坏把整条路弄哑。 */
    const char *prim = (lang != NULL && lang[0] != '\0') ? lang : culture;
    const char *lang_out = (prim != NULL && prim[0] != '\0') ? prim : "";
    dsh_voice *v = &items[kept];
    v->id = dup_or_empty(id);
    v->name = dup_or_empty((name != NULL && name[0] != '\0') ? name : id);
    v->language = dup_or_empty(lang_out);
    if (culture != NULL && culture[0] != '\0') {
      v->culture = dup_or_empty(culture);
    } else if (v->language[0] != '\0') {
      /* ⚠️ `language` 本身可能就是一个**区域标记**（「en-GB」）—— 那就直接拿它当 culture；
       *    只有「纯语种码」（「en」）才去补默认区域。无脑补默认区域会把 `{"language":"en-GB"}`
       *    变成 `en-US`，而「英式音色被当成美式」这种假事实会一路传下去。 */
      v->culture = (strchr(v->language, '-') != NULL)
                       ? dup_or_empty(v->language)
                       : dsh_mem_strdup(dsh_language_default_tag(v->language));
    } else {
      v->culture = dup_or_empty("");
    }
    if (v->id == NULL || v->name == NULL || v->language == NULL || v->culture == NULL) {
      dsh_json_doc_free(doc);
      out->items = items;
      out->count = kept + 1;
      dsh_voices_free(out);
      dsh_set_last_error("内存不足：本机音色表");
      return -1;
    }
    /* 语种只留主代码（「en-US」 → 「en」），比对时少一层规矩 */
    if (v->language[0] != '\0') {
      const char *primary = dsh_language_primary(v->language);
      if (primary != v->language) {
        dsh_release(v->language);
        v->language = dsh_mem_strdup(primary);
        if (v->language == NULL) {
          dsh_json_doc_free(doc);
          out->items = items;
          out->count = kept + 1;
          dsh_voices_free(out);
          dsh_set_last_error("内存不足：本机音色表");
          return -1;
        }
      }
    }
    kept++;
  }
  dsh_json_doc_free(doc);
  out->items = items;
  out->count = kept;
  return 0;
}

const dsh_voice *dsh_speech_pick_voice(const dsh_voice_list *list, const char *language,
                                       const char *accent, const char *preferred_id) {
  if (list == NULL || list->items == NULL || list->count <= 0) return NULL;

  /* 1) 显式指定的音色优先（id 或名字都认 —— 与参考实现同约定）*/
  if (preferred_id != NULL && preferred_id[0] != '\0') {
    for (int64_t i = 0; i < list->count; i++) {
      if (eq_nocase(list->items[i].id, preferred_id) ||
          eq_nocase(list->items[i].name, preferred_id)) {
        return &list->items[i];
      }
    }
  }

  /* 2) 按语种匹配（语种码为空 = 认不出，那就一个都不算候选）*/
  const char *want = (language != NULL) ? language : "";
  if (want[0] == '\0') return NULL;
  int64_t first = -1;
  for (int64_t i = 0; i < list->count; i++) {
    if (eq_nocase(list->items[i].language, want)) {
      first = i;
      break;
    }
  }
  if (first < 0) return NULL;

  /* 3) 英语看地区：accent = uk → 先找 en-GB，其余（含 auto）→ 先找 en-US。
   *    ⚠️ auto **也先找美式**：候选按音色名排序，直接取第一个会按字母序先撞上英式，于是
   *    「判不出来 → 英语兜底」这条路上报的区域（en-US）与实际听到的嗓子对不上。 */
  if (eq_nocase(want, "en")) {
    const char *wanted_culture = (accent != NULL && eq_nocase(accent, "uk")) ? "en-GB" : "en-US";
    for (int64_t i = 0; i < list->count; i++) {
      if (eq_nocase(list->items[i].language, "en") &&
          eq_nocase(list->items[i].culture, wanted_culture)) {
        return &list->items[i];
      }
    }
  }
  /* 找不到精确地区就按顺序用第一个：**别因为口音偏好把发音搞没了**。 */
  return &list->items[first];
}

const char *dsh_speech_doubao_speaker(const char *language, int mixed, const char *speaker_en,
                                     const char *speaker_zh) {
  const char *en = (speaker_en != NULL && speaker_en[0] != '\0') ? speaker_en
                                                                 : DSH_SPEECH_DEFAULT_SPEAKER_EN;
  const char *zh = (speaker_zh != NULL && speaker_zh[0] != '\0') ? speaker_zh
                                                                 : DSH_SPEECH_DEFAULT_SPEAKER_ZH;
  /* 混排一律用中文音色：中文那条能读英文，反过来不行（参考实现同约定）*/
  if (mixed) return zh;
  const char *lang = (language != NULL && language[0] != '\0') ? language : "en";
  if (eq_nocase(lang, "zh")) return zh;
  return en; /* 其余语种一律英文音色（`PickVoice` 的兜底分支） */
}

/* ── 切段 ──────────────────────────────────────────────────────────────── */

/** 这个码点是不是句末标点（第一优先级断点）*/
static int is_sentence_end(const char *s, size_t at) {
  const unsigned char c = (unsigned char)s[at];
  if (c == '!' || c == '?' || c == ';') return 1; /* 半角 */
  /* 全角：。！？； —— U+3002 / U+FF01 / U+FF1F / U+FF1B */
  if (c == 0xE3 && (unsigned char)s[at + 1] == 0x80 && (unsigned char)s[at + 2] == 0x82) return 1;
  if (c == 0xEF && (unsigned char)s[at + 1] == 0xBC &&
      ((unsigned char)s[at + 2] == 0x81 || (unsigned char)s[at + 2] == 0x9F ||
       (unsigned char)s[at + 2] == 0x9B)) {
    return 1;
  }
  return 0;
}

/** 第二优先级：逗号 / 顿号 / 冒号（半角与全角）*/
static int is_clause_end(const char *s, size_t at) {
  const unsigned char c = (unsigned char)s[at];
  if (c == ',' || c == ':') return 1;
  /* U+3001 ／， U+FF0C ／： U+FF1A */
  if (c == 0xE3 && (unsigned char)s[at + 1] == 0x80 && (unsigned char)s[at + 2] == 0x81) return 1;
  if (c == 0xEF && (unsigned char)s[at + 1] == 0xBC &&
      ((unsigned char)s[at + 2] == 0x8C || (unsigned char)s[at + 2] == 0x9A)) {
    return 1;
  }
  return 0;
}

/** 收下一段：拷一份、两端 trim、数码点（空段**不收** —— 参考实现最后有一句 `.filter`）*/
static int emit_chunk(dsh_speech_chunk *slot, const char *s, size_t len) {
  size_t from = 0;
  size_t to = len;
  trim_range(s, len, &from, &to);
  if (to <= from) return 0; /* 整段都是空白：丢掉，不是错误 */
  slot->text = (char *)dsh_mem_alloc((to - from) + 1);
  if (slot->text == NULL) return -1;
  memcpy(slot->text, s + from, to - from);
  slot->text[to - from] = '\0';
  slot->chars = count_cps(slot->text, to - from);
  return 1; /* 1 = 收下了 */
}

int dsh_speech_has_text(const char *text) {
  if (text == NULL) return 0;
  const size_t len = strlen(text);
  size_t from = 0;
  size_t to = len;
  trim_range(text, len, &from, &to);
  return (to > from) ? 1 : 0;
}

int dsh_speech_split(const char *text, dsh_speech_chunk **out, int64_t *out_count) {
  if (out == NULL || out_count == NULL) {
    dsh_set_last_error("dsh_speech_split：出参不能为空");
    return -1;
  }
  *out = NULL;
  *out_count = 0;
  if (text == NULL) return 0;

  const size_t len = strlen(text);
  size_t cur_from = 0;
  size_t cur_to = len;
  trim_range(text, len, &cur_from, &cur_to);
  if (cur_to <= cur_from) return 0; /* 全是空白 = 没有可念的 */

  /* 段数上限：每段至少 1 个码点，所以码点数就是上限。⚠️ 它**不只是省内存** ——
   * 它是「切段循环真的在前进」的机械检查标准：越过了它说明某一轮没有前进。 */
  const int64_t cps_total = count_cps(text + cur_from, cur_to - cur_from);
  const int64_t max_chunks = cps_total + 1;
  dsh_speech_chunk *chunks =
      (dsh_speech_chunk *)dsh_mem_alloc((size_t)max_chunks * sizeof(dsh_speech_chunk));
  if (chunks == NULL) {
    dsh_set_last_error("内存不足：朗读切段（最多 %lld 段）", (long long)max_chunks);
    return -1;
  }
  memset(chunks, 0, (size_t)max_chunks * sizeof(dsh_speech_chunk));
  int64_t count = 0;

  while (count_cps(text + cur_from, cur_to - cur_from) > DSH_SPEECH_CHUNK_CHARS) {
    /* 开窗：前 **301** 个码点（多取一个 —— 切点正好落在第 300 个码点上时也看得见）。
     * 断点优先级 = 句末标点 → 逗号/顿号/冒号 → 空白 → 硬切；换行当空白看
     * （断在换行上等于把段落边界当空格用）。 */
    const size_t wlen = bytes_for_cps(text + cur_from, cur_to - cur_from,
                                     (size_t)DSH_SPEECH_CHUNK_CHARS + 1);
    const int64_t wcps = count_cps(text + cur_from, wlen);

    size_t cut_cp = 0;
    /* ① 从窗口末尾往前跳过空白，看**最后一个非空白码点**是不是句末/分句标点
     *    （等价于「从这里到末尾只有空白」的那个标点）。 */
    int64_t i = wcps;
    while (i > 0) {
      const size_t st = bytes_for_cps(text + cur_from, wlen, (size_t)i - 1);
      size_t sp = 0;
      const size_t after = bytes_for_cps(text + cur_from, wlen, (size_t)i);
      if (is_space_at(text + cur_from, wlen, st, &sp) && st + sp == after) {
        i--;
        continue;
      }
      break;
    }
    if (i > 0) { /* `match.index > 0`：标点在第 0 个码点上不算断点 */
      const size_t st = bytes_for_cps(text + cur_from, wlen, (size_t)i - 1);
      if (is_sentence_end(text + cur_from, st) || is_clause_end(text + cur_from, st)) {
        cut_cp = (size_t)i; /* 切在那个标点**后面**（后面那些空白由 trim 收掉）*/
      }
    }
    /* ② 第三档：窗口里**最后一个空白** */
    if (cut_cp == 0) {
      size_t at = 0;
      size_t best = 0;
      while (at < wlen) {
        size_t sp = 0;
        if (is_space_at(text + cur_from, wlen, at, &sp)) {
          best = at + sp;
          at += sp;
        } else {
          at += cp_bytes(text + cur_from, wlen, at);
        }
      }
      if (best > 0) cut_cp = (size_t)count_cps(text + cur_from, best);
    }
    /* ③ 硬切（整段一个标点、一个空格都没有）*/
    if (cut_cp == 0) cut_cp = DSH_SPEECH_CHUNK_CHARS;

    size_t cut = bytes_for_cps(text + cur_from, cur_to - cur_from, cut_cp);
    if (cut == 0) cut = cp_bytes(text + cur_from, cur_to - cur_from, 0); /* 防御：不吃零 */
    if (cut == 0) break;

    {
      const int added = emit_chunk(&chunks[count], text + cur_from, cut);
      if (added < 0) {
        dsh_speech_chunks_free(chunks, count);
        dsh_set_last_error("内存不足：朗读切段");
        return -1;
      }
      if (added == 1) count++;
      /* 走到这里段数已经不可能超过码点数；真超了说明某一轮没前进，
       * 宁可**少切一段**也不能写到数组外面去。 */
      if (count >= max_chunks) break;
    }

    /* 下一轮的起点：切点之后**整体 trim**（参考实现是 `rest.slice(cut).trim()`）*/
    size_t next_from = cur_from + cut;
    size_t next_to = cur_to;
    trim_range(text, next_to, &next_from, &next_to);
    cur_from = next_from;
    cur_to = next_to;
    if (cur_from >= cur_to) break;
  }

  if (cur_to > cur_from) {
    const int added = emit_chunk(&chunks[count], text + cur_from, cur_to - cur_from);
    if (added < 0) {
      dsh_speech_chunks_free(chunks, count);
      dsh_set_last_error("内存不足：朗读切段");
      return -1;
    }
    if (added == 1) count++;
  }

  *out = chunks;
  *out_count = count;
  return 0;
}

void dsh_speech_describe_voices(const dsh_voice_list *list, char *out, size_t out_cap) {
  if (out == NULL || out_cap == 0) return;
  out[0] = '\0';
  if (list == NULL || list->items == NULL || list->count <= 0) {
    /* 与参考实现同约定：一个音色都没有时说「无」，不是空串（空串会让那句话读不通）*/
    snprintf(out, out_cap, "无");
    return;
  }
  size_t used = 0;
  for (int64_t i = 0; i < list->count; i++) {
    const char *label = dsh_language_name(list->items[i].language);
    if (label == NULL || label[0] == '\0') label = list->items[i].language;
    if (label == NULL || label[0] == '\0') continue;
    /* 去重（参考实现按「语种中文名」去重）*/
    int seen = 0;
    {
      /* 在已经写出去的串里找这一个（表很小，直接找）*/
      const size_t n = strlen(label);
      for (size_t at = 0; at + n <= used; at++) {
        if (memcmp(out + at, label, n) == 0) {
          /* 只认「整个名字」的边界：前后是开头/「、」 */
          const int left_ok = (at == 0) || (out[at - 1] == '\xE3' && out[at] == '\x80');
          if (at == 0 || left_ok) {
            seen = 1;
            break;
          }
        }
      }
    }
    if (seen) continue;
    const size_t n = strlen(label);
    const size_t need = used + (used > 0 ? 3 : 0) + n; /* 「、」是 3 字节 */
    if (need + 1 > out_cap) break;
    if (used > 0) {
      memcpy(out + used, "\xE3\x80\x81", 3); /* */
      used += 3;
    }
    memcpy(out + used, label, n);
    used += n;
    out[used] = '\0';
  }
  if (used == 0) snprintf(out, out_cap, "无");
}

void dsh_speech_chunks_free(dsh_speech_chunk *chunks, int64_t count) {
  if (chunks == NULL) return;
  for (int64_t i = 0; i < count; i++) {
    if (chunks[i].text != NULL) dsh_release(chunks[i].text);
  }
  dsh_release(chunks);
}
