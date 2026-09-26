/* 查词历史的落盘层：一行一条 JSON 的追加文件。
 * 这里只管一行的形状 / 内存里那份列表 / 什么时候压缩；去重窗口、上限、顺序这些业务约定
 * 全在 dsh_history.h，改动之前先去读它。 */

#include "store/dsh_history.h"

#include "dsh_internal.h"
#include "json_reader.h"
#include "json_writer.h"
#include "platform/dsh_file.h"

#include <stdio.h>
#include <stdlib.h> /* qsort：读回去重 */
#include <string.h>

/** 打开时最多读回这么多字节，超了**只读尾巴**：最新的记录在文件末尾，
 * 被撑大的历史文件里用户在乎的正是最近查的那些词。 */
#define DSH_HIST_FILE_READ_MAX ((size_t)32 * 1024 * 1024)

/** 内存里的一条记录（字符串都是自己分配的） */
typedef struct {
  char *word;
  char *dict_id;
  char *dict_title;
  int64_t at;
  /** 它在文件里的位置（越大越新）—— **只在读回时用**：去重靠它挑「最后那一次」*/
  int64_t seq;
} hist_rec;

struct dsh_history {
  char *path;
  /** 记录：**最新在前**（新的接在队头） */
  hist_rec *recs;
  int64_t count;
  int64_t cap;
  /** 上限（默认 DSH_HISTORY_MAX；测试会把它压小，见 dsh_hist_set_limit） */
  int64_t limit;
  /** 文件里现在有多少行（**含坏行**）—— 压缩的触发看它，不看 count */
  int64_t file_lines;
  /** 读回时跳过了多少行（解析不出来 / 没有词） */
  int64_t skipped;
};

/* ── 小工具 ──────────────────────────────────── */

/** 复制一个可能为 NULL 的串（统一成空串，省掉调用方到处判空） */
static char *dup_or_empty(const char *s) { return dsh_mem_strdup(s != NULL ? s : ""); }

static void rec_free(hist_rec *r) {
  if (r->word != NULL) dsh_release(r->word);
  if (r->dict_id != NULL) dsh_release(r->dict_id);
  if (r->dict_title != NULL) dsh_release(r->dict_title);
  r->word = r->dict_id = r->dict_title = NULL;
}

static void recs_free(hist_rec *recs, int64_t count) {
  if (recs == NULL) return;
  for (int64_t i = 0; i < count; i++) rec_free(&recs[i]);
  dsh_release(recs);
}

/** 砍到上限（超了从**最旧**那头砍），返回有没有真的砍掉东西。
 * ⚠️ 上限是**产品约定**，push 后 / 打开后 / 显式改上限后都得成立：追加式落盘 + 读回去重后的条数
 * 都可能超过上限，不在这里收口，界面就会显示出超过上限的总数。 */
static int trim_to_limit(dsh_history *h) {
  int trimmed = 0;
  while (h->count > h->limit) {
    h->count--;
    rec_free(&h->recs[h->count]);
    trimmed = 1;
  }
  return trimmed;
}

/** 保证数组至少有 `want` 个位置（不够就翻倍地长） */
static int ensure_cap(dsh_history *h, int64_t want) {
  if (want <= h->cap) return 0;
  int64_t next = (h->cap > 0) ? h->cap : 64;
  while (next < want) next *= 2;
  hist_rec *grown = (hist_rec *)dsh_mem_alloc((size_t)next * sizeof(hist_rec));
  if (grown == NULL) {
    dsh_set_last_error("内存不足：历史记录表（%lld 条）", (long long)next);
    return -1;
  }
  if (h->count > 0) memcpy(grown, h->recs, (size_t)h->count * sizeof(hist_rec));
  if (h->recs != NULL) dsh_release(h->recs);
  h->recs = grown;
  h->cap = next;
  return 0;
}

/** 一个只会长的字节缓冲（拼整份文件内容用；内核没有 realloc，所以自己搬） */
typedef struct {
  char *p;
  size_t len;
  size_t cap;
} byte_buf;

static void bb_free(byte_buf *b) {
  if (b->p != NULL) dsh_release(b->p);
  b->p = NULL;
  b->len = b->cap = 0;
}

static int bb_add(byte_buf *b, const char *s, size_t n) {
  if (n == 0) return 0;
  if (b->len + n + 1 > b->cap) {
    size_t next = (b->cap > 0) ? b->cap : 4096;
    while (next < b->len + n + 1) next *= 2;
    char *grown = (char *)dsh_mem_alloc(next);
    if (grown == NULL) {
      dsh_set_last_error("内存不足：历史文件内容（%zu 字节）", b->len + n);
      return -1;
    }
    if (b->len > 0) memcpy(grown, b->p, b->len);
    if (b->p != NULL) dsh_release(b->p);
    b->p = grown;
    b->cap = next;
  }
  memcpy(b->p + b->len, s, n);
  b->len += n;
  b->p[b->len] = '\0';
  return 0;
}

/** 一条记录 → 一行的字节（**末尾带 `\n`**）。
 * 字段名（word / dictId / dictTitle / at）必须与 settings.json 里旧的 history 数组逐字相同，
 * 否则「导入」就变成「翻译」，而翻译迟早会漏字段。 */
static char *build_line_nl(const char *word, const char *dict_id, const char *dict_title,
                           int64_t at) {
  dsh_json *j = dsh_json_new();
  if (j == NULL) {
    dsh_set_last_error("内存不足：历史一行 JSON");
    return NULL;
  }
  dsh_json_object_begin(j);
  dsh_json_kv_str(j, "word", word);
  dsh_json_kv_str(j, "dictId", dict_id);
  dsh_json_kv_str(j, "dictTitle", dict_title);
  dsh_json_kv_i64(j, "at", at);
  dsh_json_object_end(j);
  char *text = dsh_json_take(j);
  dsh_json_free(j);
  if (text == NULL) {
    dsh_set_last_error("内存不足：历史一行 JSON");
    return NULL;
  }
  const size_t n = strlen(text);
  char *line = (char *)dsh_mem_alloc(n + 2);
  if (line == NULL) {
    dsh_release(text);
    dsh_set_last_error("内存不足：历史一行");
    return NULL;
  }
  memcpy(line, text, n);
  line[n] = '\n';
  line[n + 1] = '\0';
  dsh_release(text);
  return line;
}

/* ── 读回 ────────────────────────────────────── */

/** 这一行是不是「空的」（只有空白）—— 空行不算坏行，只是跳过 */
static int line_is_blank(const char *s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    const char c = s[i];
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return 0;
  }
  return 1;
}

/** 解析一行，把记录放进 `h` 的第 `at_index` 位（位置由调用方预先留好）。
 * @return 0 这行成了一条记录；1 这行不要（解析不出来 / 没有词）；-1 内存不足（真失败）*/
static int parse_line_into(dsh_history *h, int64_t at_index, const char *line, size_t len) {
  dsh_json_doc *doc = NULL;
  if (dsh_json_parse(line, len, &doc) != 0) return 1; /* 坏行：跳过，别把整份历史废掉 */
  int rc = 1;
  const dsh_json_node *root = dsh_json_doc_root(doc);
  if (dsh_json_is_object(root)) {
    const dsh_json_node *w = dsh_json_object_get(root, "word");
    const char *word = dsh_json_str_value(w, NULL);
    if (word != NULL && word[0] != '\0') {
      const char *dict_id = dsh_json_str_value(dsh_json_object_get(root, "dictId"), NULL);
      const char *title = dsh_json_str_value(dsh_json_object_get(root, "dictTitle"), NULL);
      int64_t at = 0;
      (void)dsh_json_i64_value(dsh_json_object_get(root, "at"), &at);
      char *cw = dup_or_empty(word);
      char *cd = dup_or_empty(dict_id);
      char *ct = dup_or_empty(title);
      if (cw == NULL || cd == NULL || ct == NULL) {
        if (cw != NULL) dsh_release(cw);
        if (cd != NULL) dsh_release(cd);
        if (ct != NULL) dsh_release(ct);
        rc = -1;
      } else {
        h->recs[at_index].word = cw;
        h->recs[at_index].dict_id = cd;
        h->recs[at_index].dict_title = ct;
        h->recs[at_index].at = at;
        /* 文件位置当「谁更新」的凭据：有效行按文件顺序收，下标就够用 */
        h->recs[at_index].seq = at_index;
        h->count++;
        rc = 0;
      }
    }
  }
  dsh_json_doc_free(doc);
  return rc;
}

/** 把文件读回内存（文件里**最旧在前**，内存要**最新在前**）。
 * ⚠️ **文件里的行 ≠ 现在的记录**：追加式落盘会让同一个 (词, 词典) 留下两行，读回后必须去重、
 * 只留最后一次那行，否则界面重复且总数对不上；去重 + 排序走两趟 `qsort`，**不许逐条往前插 / 扫重复**（O(n²)）。 */
/** 读回去重第一趟：同一个 (词, 词典) 挨在一起，组内**新的在前** */
static int cmp_group(const void *a, const void *b) {
  const hist_rec *x = (const hist_rec *)a;
  const hist_rec *y = (const hist_rec *)b;
  int c = strcmp(x->word, y->word);
  if (c != 0) return c;
  c = strcmp(x->dict_id, y->dict_id);
  if (c != 0) return c;
  if (x->seq > y->seq) return -1;
  if (x->seq < y->seq) return 1;
  return 0;
}

/** 读回去重第二趟：按文件位置倒序 = **最新在前**（这就是产品要的顺序）*/
static int cmp_seq_desc(const void *a, const void *b) {
  const hist_rec *x = (const hist_rec *)a;
  const hist_rec *y = (const hist_rec *)b;
  if (x->seq > y->seq) return -1;
  if (x->seq < y->seq) return 1;
  return 0;
}


static int load_file(dsh_history *h) {
  dsh_file_info info;
  if (dsh_file_stat(h->path, &info) != 0) return 0; /* 问不出来：当没有（不是错误）*/
  if (!info.exists || !info.is_file || info.size <= 0) return 0;

  size_t want = (size_t)info.size;
  long skip_head = 0;
  if ((uint64_t)info.size > (uint64_t)DSH_HIST_FILE_READ_MAX) {
    /* 太大：只读尾巴（最新的行在末尾）并丢掉第一段不完整的行 —— `fseek` 收 `long`，
     * Windows 上它是 32 位，所以先卡一下：超过 2 GB 就**如实拒绝**，远好过悄悄只显示旧的那几百条。 */
    const uint64_t skip = (uint64_t)info.size - (uint64_t)DSH_HIST_FILE_READ_MAX;
    if (skip > 2000000000ull) {
      dsh_set_last_error("历史文件大得不像历史（%lld 字节）：%s —— 没有动它，请手工处理",
                         (long long)info.size, h->path);
      return -1;
    }
    skip_head = (long)skip;
    want = DSH_HIST_FILE_READ_MAX;
  }

  char *buf = (char *)dsh_mem_alloc(want + 1);
  if (buf == NULL) {
    dsh_set_last_error("内存不足：读历史文件（%zu 字节）", want);
    return -1;
  }
  FILE *fp = dsh_file_open_read(h->path);
  if (fp == NULL) {
    dsh_release(buf);
    dsh_set_last_error("历史文件打不开：%s", h->path);
    return -1;
  }
  if (skip_head > 0 && fseek(fp, skip_head, SEEK_SET) != 0) {
    fclose(fp);
    dsh_release(buf);
    dsh_set_last_error("历史文件太大而且定位不了：%s", h->path);
    return -1;
  }
  const size_t got = fread(buf, 1, want, fp);
  fclose(fp);
  buf[got] = '\0';

  /* 预分配：按行数估（一行至少 20 字节，除一下就不会反复扩容）*/
  int64_t guess = (int64_t)(got / 20) + 8;
  if (guess > DSH_HISTORY_MAX * 4) guess = DSH_HISTORY_MAX * 4;
  if (ensure_cap(h, guess) != 0) {
    dsh_release(buf);
    return -1;
  }

  size_t pos = 0;
  int in_first_partial = (skip_head > 0);
  while (pos < got) {
    size_t end = pos;
    while (end < got && buf[end] != '\n') end++;
    const size_t line_len = end - pos;
    if (in_first_partial) {
      in_first_partial = 0; /* 尾巴那一段的开头多半是半行，丢掉 */
    } else if (!line_is_blank(buf + pos, line_len)) {
      h->file_lines++;
      if (ensure_cap(h, h->count + 1) != 0) {
        dsh_release(buf);
        return -1;
      }
      const int rc = parse_line_into(h, h->count, buf + pos, line_len);
      if (rc < 0) {
        dsh_release(buf);
        return -1;
      }
      if (rc == 1) h->skipped++; /* 坏行：如实记下，别不声不响 */
    }
    pos = end + 1;
  }
  dsh_release(buf);

  /* 去重 + 排序：同一个 (词, 词典) 在文件里可能有多行，只留**最后那一行**（它现在的时间与位置）。 */
  if (h->count > 1) {
    qsort(h->recs, (size_t)h->count, sizeof(hist_rec), cmp_group);
    int64_t kept = 0;
    for (int64_t i = 0; i < h->count; i++) {
      /* ⚠️ 比的是**上一个留下来的那一条**（`recs[kept-1]`），不是 `recs[i-1]`：丢了的那条已被
       * `rec_free` 清成 NULL，`recs[i-1]` 可能正是它 → `strcmp(NULL, …)` 段错误。组内相邻由
       * 上面的排序保证，所以「上一个留下的」就是「同组的第一个」。 */
      if (kept > 0 && strcmp(h->recs[i].word, h->recs[kept - 1].word) == 0 &&
          strcmp(h->recs[i].dict_id, h->recs[kept - 1].dict_id) == 0) {
        rec_free(&h->recs[i]);
        continue;
      }
      h->recs[kept++] = h->recs[i]; /* kept <= i：往前搬不会盖掉还没处理的那一项 */
    }
    h->count = kept;
    qsort(h->recs, (size_t)h->count, sizeof(hist_rec), cmp_seq_desc);
  }
  return 0;
}

/* ── 压缩（整份重写一次） ────────────────────── */

/** 把内存里这份列表整份写成文件（`dsh_file_write_atomic`：临时文件 + rename） */
static int write_all(dsh_history *h) {
  byte_buf b;
  memset(&b, 0, sizeof(b));
  /* 文件顺序 = 最旧在前 = 内存列表**倒着**走 */
  for (int64_t i = h->count - 1; i >= 0; i--) {
    char *line = build_line_nl(h->recs[i].word, h->recs[i].dict_id, h->recs[i].dict_title,
                               h->recs[i].at);
    if (line == NULL) {
      bb_free(&b);
      return -1;
    }
    const int rc = bb_add(&b, line, strlen(line));
    dsh_release(line);
    if (rc != 0) {
      bb_free(&b);
      return -1;
    }
  }
  const int rc = dsh_file_write_atomic(h->path, (b.p != NULL) ? b.p : "", b.len);
  bb_free(&b);
  if (rc != 0) return -1;
  h->file_lines = h->count;
  return 0;
}

/** 该不该压缩：文件行数涨到上限的 2 倍。
 * 为什么是 2 倍：一次压缩写 `count` 行、摊到多出来的 `limit` 行上，写放大才是 O(1)；触发太勤就又成了经常整份重写。 */
static int should_compact(const dsh_history *h) {
  return h->limit > 0 && h->file_lines >= h->limit * 2;
}

/* ── 打开 / 关闭 ─────────────────────────────── */

int dsh_hist_open(const char *path, dsh_history **out) {
  if (out == NULL) {
    dsh_set_last_error("dsh_hist_open：出参不能为空");
    return -1;
  }
  *out = NULL;
  if (path == NULL || path[0] == '\0') {
    dsh_set_last_error("dsh_hist_open：历史文件路径是空的");
    return -1;
  }

  dsh_history *h = (dsh_history *)dsh_mem_alloc(sizeof(*h));
  if (h == NULL) {
    dsh_set_last_error("内存不足：历史句柄");
    return -1;
  }
  memset(h, 0, sizeof(*h));
  h->limit = (int64_t)DSH_HISTORY_MAX;
  h->path = dsh_mem_strdup(path);
  if (h->path == NULL) {
    dsh_release(h);
    dsh_set_last_error("内存不足：历史文件路径");
    return -1;
  }

  /* 清掉上次压缩写到一半留下的临时文件（`<path>.tmp`）—— 没人清就永远躺在那里；
   * 清不掉**不算打不开历史**：它只是一块垃圾。 */
  (void)dsh_file_cleanup_temp(h->path);

  if (load_file(h) != 0) {
    dsh_hist_close(h);
    return -1;
  }
  /* 打开时收口两件事：去重后的条数可能超过上限 → 砍；文件比该有的长（上次跑到一半被杀 /
   * 上限被调小过）→ 压缩一次。压缩失败不致命，历史本身是好的，只是文件偏大，下次 push 还会再试。 */
  if (trim_to_limit(h) || should_compact(h)) (void)write_all(h);

  *out = h;
  return 0;
}

void dsh_hist_close(dsh_history *h) {
  if (h == NULL) return;
  recs_free(h->recs, h->count);
  if (h->path != NULL) dsh_release(h->path);
  dsh_release(h);
}

const char *dsh_hist_path(const dsh_history *h) {
  return (h != NULL && h->path != NULL) ? h->path : "";
}

void dsh_hist_set_limit(dsh_history *h, int64_t limit) {
  if (h == NULL) return;
  h->limit = (limit > 0) ? limit : (int64_t)DSH_HISTORY_MAX;
  /* 上限**立刻生效**：设完就不许再有超出的条数躺在内存里 —— 否则界面会短暂读到大于上限的
   * 总数，而「哪一刻算数」不该由调用时序决定。 */
  if (trim_to_limit(h)) (void)write_all(h);
}

int dsh_hist_count(dsh_history *h, int64_t *out_total) {
  if (out_total == NULL) {
    dsh_set_last_error("dsh_hist_count：出参不能为空");
    return -1;
  }
  *out_total = 0;
  if (h == NULL) {
    dsh_set_last_error("dsh_hist_count：历史没打开");
    return -1;
  }
  *out_total = h->count;
  return 0;
}

int dsh_hist_skipped(dsh_history *h, int64_t *out_skipped) {
  if (out_skipped == NULL) {
    dsh_set_last_error("dsh_hist_skipped：出参不能为空");
    return -1;
  }
  *out_skipped = 0;
  if (h == NULL) {
    dsh_set_last_error("dsh_hist_skipped：历史没打开");
    return -1;
  }
  *out_skipped = h->skipped;
  return 0;
}

/* ── 记一条 ──────────────────────────────────── */





/** 这一批里有没有 (word, dict_id) —— **导入去重**用 */
static int array_has_key(const hist_rec *recs, int64_t count, const char *word,
                         const char *dict_id) {
  for (int64_t i = 0; i < count; i++) {
    if (strcmp(recs[i].word, word) == 0 && strcmp(recs[i].dict_id, dict_id) == 0) return 1;
  }
  return 0;
}



/** 把同 (word, dict_id) 的旧记录删掉（列表里至多该有一条）*/
static void remove_same_key(dsh_history *h, const char *word, const char *dict_id) {
  for (int64_t i = 0; i < h->count; i++) {
    if (strcmp(h->recs[i].word, word) == 0 && strcmp(h->recs[i].dict_id, dict_id) == 0) {
      rec_free(&h->recs[i]);
      if (i + 1 < h->count) {
        memmove(&h->recs[i], &h->recs[i + 1], (size_t)(h->count - i - 1) * sizeof(hist_rec));
      }
      h->count--;
      return;
    }
  }
}

int dsh_hist_push(dsh_history *h, const char *word, const char *dict_id, const char *dict_title,
                  int64_t now_ms) {
  if (h == NULL) {
    dsh_set_last_error("dsh_hist_push：历史没打开");
    return -1;
  }
  /* 空词不记（先去掉前导空白）*/
  if (word == NULL) return 0;
  while (*word == ' ' || *word == '\t' || *word == '\n' || *word == '\r') word++;
  if (*word == '\0') return 0;
  if (dict_id == NULL) dict_id = "";
  if (dict_title == NULL) dict_title = "";

  /* ① 队头那条：同 (word, dict_id) 且不到去重窗口 → 这一次不记（**不是失败**）*/
  if (h->count > 0 && strcmp(h->recs[0].word, word) == 0 &&
      strcmp(h->recs[0].dict_id, dict_id) == 0 &&
      now_ms - h->recs[0].at < (int64_t)DSH_HISTORY_DEDUPE_MS) {
    return 0;
  }

  /* ② 先把要用的东西全准备好（数组位置 / 一行的字节 / 三个字符串）。
   * ⚠️ **内存里能失败的动作必须赶在写文件之前做完** —— 反过来会出现「文件里有这一条、
   * 内存里没有」，同一个进程里读到的历史与落盘的不一致，而失败又报给了调用方。 */
  if (ensure_cap(h, h->count + 1) != 0) return -1;
  char *line = build_line_nl(word, dict_id, dict_title, now_ms);
  if (line == NULL) return -1;
  char *cw = dup_or_empty(word);
  char *cd = dup_or_empty(dict_id);
  char *ct = dup_or_empty(dict_title);
  if (cw == NULL || cd == NULL || ct == NULL) {
    dsh_release(line);
    if (cw != NULL) dsh_release(cw);
    if (cd != NULL) dsh_release(cd);
    if (ct != NULL) dsh_release(ct);
    dsh_set_last_error("内存不足：历史记录");
    return -1;
  }

  /* ③ 落盘：**只在尾巴上追加这一行**（写放大 O(1)）*/
  if (dsh_file_append(h->path, line, strlen(line)) != 0) {
    dsh_release(line);
    dsh_release(cw);
    dsh_release(cd);
    dsh_release(ct);
    return -1;
  }
  dsh_release(line);
  h->file_lines++;

  /* ④ 内存跟着改：删同键 → 插最前 → 砍到上限 */
  remove_same_key(h, word, dict_id);
  if (h->count > 0) {
    memmove(&h->recs[1], &h->recs[0], (size_t)h->count * sizeof(hist_rec));
  }
  h->recs[0].word = cw;
  h->recs[0].dict_id = cd;
  h->recs[0].dict_title = ct;
  h->recs[0].at = now_ms;
  h->recs[0].seq = 0; /* 只在读回时用，这里只是别留未初始化的值 */
  h->count++;
  (void)trim_to_limit(h);

  /* ⑤ 该压缩就压缩：**失败不影响这一次的结果**（行已写出），只是文件偏大，下次 push 会再试。 */
  if (should_compact(h)) (void)write_all(h);
  return 0;
}

/* ── 取一页 ──────────────────────────────────── */

int dsh_hist_query(dsh_history *h, int32_t offset, int32_t limit, char **out_json) {
  if (out_json == NULL) {
    dsh_set_last_error("dsh_hist_query：出参不能为空");
    return -1;
  }
  *out_json = NULL;
  if (h == NULL) {
    dsh_set_last_error("dsh_hist_query：历史没打开");
    return -1;
  }
  if (offset < 0) offset = 0;
  if (limit <= 0) limit = DSH_HISTORY_PAGE;

  int64_t emitted = 0;
  dsh_json *j = dsh_json_new();
  if (j == NULL) {
    dsh_set_last_error("内存不足：历史 JSON");
    return -1;
  }
  dsh_json_object_begin(j);
  dsh_json_kv_i64(j, "total", h->count);
  dsh_json_key(j, "items");
  dsh_json_array_begin(j);
  for (int64_t i = offset; i < h->count && emitted < (int64_t)limit; i++) {
    dsh_json_object_begin(j);
    dsh_json_kv_str(j, "word", h->recs[i].word);
    dsh_json_kv_str(j, "dictId", h->recs[i].dict_id);
    dsh_json_kv_str(j, "dictTitle", h->recs[i].dict_title);
    dsh_json_kv_i64(j, "at", h->recs[i].at);
    dsh_json_object_end(j);
    emitted++;
  }
  dsh_json_array_end(j);
  dsh_json_kv_bool(j, "hasMore", ((int64_t)offset + emitted) < h->count);
  dsh_json_object_end(j);

  char *text = dsh_json_take(j);
  dsh_json_free(j);
  if (text == NULL) {
    dsh_set_last_error("内存不足：历史 JSON");
    return -1;
  }
  *out_json = text;
  return 0;
}

/* ── 清空 / 导入 ─────────────────────────────── */

int dsh_hist_clear(dsh_history *h, int64_t *out_total) {
  if (out_total != NULL) *out_total = 0;
  if (h == NULL) {
    dsh_set_last_error("dsh_hist_clear：历史没打开");
    return -1;
  }
  /* 先落盘（截成空文件）、成功了才动内存 —— 反过来写失败会让「内存里已清、文件里还有」，
   * 下次启动历史又冒出来。 */
  if (dsh_file_write_atomic(h->path, "", 0) != 0) return -1;
  recs_free(h->recs, h->count);
  h->recs = NULL;
  h->count = 0;
  h->cap = 0;
  h->file_lines = 0;
  h->skipped = 0; /* 那些行确实已经不在了，跳过的计数跟着归零 */
  return 0;
}

int dsh_hist_import_json(dsh_history *h, const char *json, size_t len, int64_t *out_imported) {
  if (out_imported != NULL) *out_imported = 0;
  if (h == NULL) {
    dsh_set_last_error("dsh_hist_import_json：历史没打开");
    return -1;
  }
  if (json == NULL) {
    dsh_set_last_error("dsh_hist_import_json：JSON 不能为空");
    return -1;
  }

  dsh_json_doc *doc = NULL;
  if (dsh_json_parse(json, len, &doc) != 0) {
    /* 旧历史读不动：**如实报错**，调用方据此决定「那个键先别删」*/
    const char *saved = dsh_last_error_message();
    dsh_set_last_error("旧历史（settings.json 里的 history）解析不了：%s",
                       saved != NULL ? saved : "（没有说明）");
    if (saved != NULL) dsh_release((void *)saved);
    return -1;
  }
  const dsh_json_node *root = dsh_json_doc_root(doc);
  if (!dsh_json_is_array(root)) {
    dsh_json_doc_free(doc);
    dsh_set_last_error("旧历史（settings.json 里的 history）不是一个数组");
    return -1;
  }

  const int64_t n = dsh_json_array_len(root);
  int64_t skipped_here = 0;
  int64_t imp_n = 0;
  hist_rec *imp = NULL;
  if (n > 0) {
    imp = (hist_rec *)dsh_mem_alloc((size_t)n * sizeof(hist_rec));
    if (imp == NULL) {
      dsh_json_doc_free(doc);
      dsh_set_last_error("内存不足：导入旧历史（%lld 条）", (long long)n);
      return -1;
    }
    memset(imp, 0, (size_t)n * sizeof(hist_rec));
  }

  /* 数组是「最新在前」，就按这个顺序收 */
  int rc = 0;
  for (int64_t i = 0; i < n; i++) {
    const dsh_json_node *item = dsh_json_array_at(root, i);
    if (!dsh_json_is_object(item)) {
      skipped_here++;
      continue;
    }
    const char *word = dsh_json_str_value(dsh_json_object_get(item, "word"), NULL);
    if (word == NULL || word[0] == '\0') {
      skipped_here++;
      continue;
    }
    const char *dict_id = dsh_json_str_value(dsh_json_object_get(item, "dictId"), NULL);
    const char *title = dsh_json_str_value(dsh_json_object_get(item, "dictTitle"), NULL);
    int64_t at = 0;
    (void)dsh_json_i64_value(dsh_json_object_get(item, "at"), &at);
    /* 按 (word, dictId) 去重：已有的、以及这一批里前面已收下的都不再进来 —— 这条迁移必须
     * **幂等**（导入成功但删键那一步存盘失败，下次启动会重跑同一个数组），重跑一次必须安全。 */
    const char *key_dict = (dict_id != NULL) ? dict_id : "";
    if (array_has_key(h->recs, h->count, word, key_dict) ||
        array_has_key(imp, imp_n, word, key_dict)) {
      continue;
    }
    char *cw = dup_or_empty(word);
    char *cd = dup_or_empty(dict_id);
    char *ct = dup_or_empty(title);
    if (cw == NULL || cd == NULL || ct == NULL) {
      if (cw != NULL) dsh_release(cw);
      if (cd != NULL) dsh_release(cd);
      if (ct != NULL) dsh_release(ct);
      rc = -1;
      break;
    }
    imp[imp_n].word = cw;
    imp[imp_n].dict_id = cd;
    imp[imp_n].dict_title = ct;
    imp[imp_n].at = at;
    imp[imp_n].seq = 0;
    imp_n++;
  }
  dsh_json_doc_free(doc);
  if (rc != 0) {
    recs_free(imp, imp_n);
    dsh_set_last_error("内存不足：导入旧历史");
    return -1;
  }
  if (imp_n == 0) {
    if (imp != NULL) dsh_release(imp);
    if (out_imported != NULL) *out_imported = 0;
    h->skipped += skipped_here;
    return 0;
  }

  /* 合并：内存里那份（较新）在前、导入的（较旧）接在后面，两边都已是「最新在前」。
   * **先写整份文件、成功了才换内存** —— 导入要么整份成功、要么原地不动，不许出现
   * 「内存里有一半、文件里有一半」。 */
  const int64_t merged_total = h->count + imp_n;
  int64_t keep = (merged_total < h->limit) ? merged_total : h->limit;
  if (keep < 0) keep = 0;

  if (ensure_cap(h, keep) != 0) {
    recs_free(imp, imp_n);
    return -1;
  }

  /* 把要保留的那些「虚拟合并列表」拼成整份文件内容（最旧在前）*/
  byte_buf b;
  memset(&b, 0, sizeof(b));
  for (int64_t idx = keep - 1; idx >= 0; idx--) {
    /* 虚拟合并列表的第 idx 项：先内存那份，再导入的那份 */
    const hist_rec *r = (idx < h->count) ? &h->recs[idx] : &imp[idx - h->count];
    char *line = build_line_nl(r->word, r->dict_id, r->dict_title, r->at);
    if (line == NULL) {
      bb_free(&b);
      recs_free(imp, imp_n);
      return -1;
    }
    const int add_rc = bb_add(&b, line, strlen(line));
    dsh_release(line);
    if (add_rc != 0) {
      bb_free(&b);
      recs_free(imp, imp_n);
      return -1;
    }
  }
  if (dsh_file_write_atomic(h->path, (b.p != NULL) ? b.p : "", b.len) != 0) {
    bb_free(&b);
    recs_free(imp, imp_n);
    return -1;
  }
  bb_free(&b);

  /* 落盘成功：换内存（先砍到 keep，再接管导入的前若干条）*/
  while (h->count > keep) {
    h->count--;
    rec_free(&h->recs[h->count]);
  }
  const int64_t take = (keep > h->count) ? (keep - h->count) : 0;
  for (int64_t i = 0; i < take; i++) {
    h->recs[h->count] = imp[i];
    h->count++;
  }
  /* 没被接管的那些（含因上限被砍掉的）在这里还掉 —— **不能整份 recs_free(imp, imp_n)**：
   * 前 take 条的字符串已经交给 h->recs 了，那样会把它们连同内存里那份一起放掉。 */
  for (int64_t i = take; i < imp_n; i++) rec_free(&imp[i]);
  dsh_release(imp);
  h->file_lines = h->count;
  h->skipped += skipped_here;
  if (out_imported != NULL) *out_imported = take;
  return 0;
}
