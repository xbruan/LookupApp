/* 引擎本体。内存形状：句柄 + 配置目录副本 + 设置文档 + 一份 **id 缓存**
 * （缓存键是 路径 + 大小 + 修改时间，见 platform/dsh_file.h）。 */
#include "dsh_lookup.h"          /* 接口定义：对外那 9 条的签名只在这里 */
#include "engine/dsh_engine_internal.h"
#include "dict/dsh_dict_id.h"
#include "dsh_internal.h"
#include "engine/dsh_dicts.h"
#include "engine/dsh_settings.h"
#include "json_reader.h"
#include "json_writer.h"
#include "platform/dsh_file.h"
#include "store/dsh_history.h"

#include <stdio.h>
#include <string.h>

#define DSH_ENGINE_ID_CACHE_MAX 512
#define DSH_ENGINE_SETTINGS_FILE "settings.json"
/** 查词历史的**文件**（与 settings.json 同一个配置目录）。
 * ⚠️ **换名字等于把用户的历史丢了** —— 改文件名必须配一次迁移（见 `migrate_legacy_history`）。 */
#define DSH_ENGINE_HISTORY_FILE "history.jsonl"
/** 旧版留下的 SQLite 库文件 —— **只删不用**（见 `migrate_legacy_history`） */
#define DSH_ENGINE_LEGACY_DB_FILE "lookup.db"
#define DSH_ENGINE_ADD_MAX 10000

/** id 缓存的一条：路径 + 大小 + 修改时间当键 */
typedef struct {
  char *path;
  int64_t size;
  int64_t mtime;
  char id[DSH_DICT_ID_BUF_LEN];
} id_cache_entry;

struct dsh_engine {
  char *user_data_dir; /* 空串 = 不落盘 */
  dsh_settings *settings;
  id_cache_entry *cache;
  int64_t cache_count;
  /*
   * 查词历史（`<配置目录>/history.jsonl`，一行一条 JSON 的追加文件）。
   *
   * ⚠️ **打不开不是致命的**：`history` 为 NULL 时引擎照旧能用，只是没有历史。
   *    原因留在 `history_why` 里、由查询接口原样说给用户 —— **不许悄悄退回成空历史**：
   *    那会让用户以为自己的历史被清空了。
   */
  dsh_history *history;
  char *history_why;
  /*
   * **上一次交出去的、界面正在显示的那条词条**（键名 + 哪一本）。
   *
   * 为什么由引擎记：「落点就是当前词条时**不跳**」这条产品约定要拿「落点是谁」与
   * 「现在显示的是谁」比，而后者只有内核知道自己上一次交出去了什么 —— 让界面拿两个
   * 字符串去比，就是把业务规则放进视图层；查词接口据它给出 `sameAsShown`。
   *
   * ⚠️ 只在**答复真的会让界面显示一条词条**时才更新（见 `remember_shown`）：查不到时
   *    落点是 NULL、停在候选列表那一档什么都没显示 —— 那两种都不动它。
   */
  char *shown_key;
  char *shown_dict_id;
  /* 已加载的词典（`lookup` 组的地基，见 engine/dsh_dicts.c）。
   * ⚠️ 布局由 dsh_dicts.c 决定，这里只留一块**存储**，两边通过
   *    `dsh_engine_dicts_state()` 对接，免得把那份内部细节摊在这个结构体里。 */
  unsigned char dicts[4096];
};

dicts_state *dsh_engine_dicts_state(struct dsh_engine *e) {
  return (dicts_state *)e->dicts;
}

/** 历史库（借用；没打开就回 NULL）—— 给 `dsh_lookup_api.c` 记录查词用 */
dsh_history *dsh_engine_history(struct dsh_engine *e) {
  return (e != NULL) ? e->history : NULL;
}

/* 「界面现在显示的是哪条词条」的两个取用口：**只借出不接管** ——
 * 调用方拿到的指针活到下一次 `remember_shown` 为止，谁都不许释放。 */
const char *dsh_engine_shown_key(const struct dsh_engine *e) {
  return (e != NULL) ? e->shown_key : NULL;
}

const char *dsh_engine_shown_dict_id(const struct dsh_engine *e) {
  return (e != NULL) ? e->shown_dict_id : NULL;
}

/**
 * 记下「这一次交出去的是哪条词条」（键名为空 / 哪一本为空 = **没显示任何词条**，那就清掉 ——
 * 例如「这本词典里没有这个词」，正文框这会儿摆在提示页上）。
 *
 * @return 0 成功；非零只有内存不足一种（那时**保持原样**，不半更新）
 */
int dsh_engine_remember_shown(struct dsh_engine *e, const char *key, const char *dict_id) {
  if (e == NULL) return -1;
  if (key == NULL || key[0] == '\0' || dict_id == NULL || dict_id[0] == '\0') {
    if (e->shown_key != NULL) {
      dsh_release(e->shown_key);
      e->shown_key = NULL;
    }
    if (e->shown_dict_id != NULL) {
      dsh_release(e->shown_dict_id);
      e->shown_dict_id = NULL;
    }
    return 0;
  }
  /* 已经是这一条就不动（省一次分配，也让"借出"的指针活得久一点）*/
  if (e->shown_key != NULL && e->shown_dict_id != NULL &&
      strcmp(e->shown_key, key) == 0 && strcmp(e->shown_dict_id, dict_id) == 0) {
    return 0;
  }
  char *next_key = dsh_mem_strdup(key);
  char *next_id = dsh_mem_strdup(dict_id);
  if (next_key == NULL || next_id == NULL) {
    if (next_key != NULL) dsh_release(next_key);
    if (next_id != NULL) dsh_release(next_id);
    return -1;
  }
  if (e->shown_key != NULL) dsh_release(e->shown_key);
  if (e->shown_dict_id != NULL) dsh_release(e->shown_dict_id);
  e->shown_key = next_key;
  e->shown_dict_id = next_id;
  return 0;
}

const char *dsh_engine_history_why(const struct dsh_engine *e) {
  return (e != NULL && e->history_why != NULL) ? e->history_why : NULL;
}

/* ── 配置目录与设置文件 ─────────────────────────────────────────────────── */

static char *settings_path(const dsh_engine *e) {
  if (e == NULL || e->user_data_dir == NULL || e->user_data_dir[0] == '\0') return NULL;
  char *p = NULL;
  if (dsh_path_join(e->user_data_dir, DSH_ENGINE_SETTINGS_FILE, &p) != 0) return NULL;
  return p;
}

static int save_settings(dsh_engine *e) {
  char *path = settings_path(e);
  if (path == NULL) return 0; /* 没有配置目录 = 只在内存里活，这不是错误 */
  char *json = dsh_settings_to_json(e->settings);
  if (json == NULL) {
    dsh_release(path);
    return -1;
  }
  const int rc = dsh_file_write_atomic(path, json, strlen(json));
  dsh_release(json);
  dsh_release(path);
  return rc;
}

/* ── id 缓存 ────────────────────────────────────────────────────────────── */

static const char *cache_lookup(const dsh_engine *e, const char *path, int64_t size,
                               int64_t mtime) {
  for (int64_t i = 0; i < e->cache_count; i++) {
    const id_cache_entry *c = &e->cache[i];
    if (c->size == size && c->mtime == mtime && strcmp(c->path, path) == 0) return c->id;
  }
  return NULL;
}

static void cache_store(dsh_engine *e, const char *path, int64_t size, int64_t mtime,
                        const char *id) {
  if (e->cache_count >= DSH_ENGINE_ID_CACHE_MAX) return; /* 满了就不缓存（不算错） */
  id_cache_entry *c = &e->cache[e->cache_count];
  c->path = dsh_mem_strdup(path);
  if (c->path == NULL) return;
  c->size = size;
  c->mtime = mtime;
  memcpy(c->id, id, DSH_DICT_ID_BUF_LEN);
  e->cache_count++;
}

/**
 * 算一本 .mdx 的 id（带缓存）。
 *
 * @return 0 成功；非零失败（不存在 / 是目录 / 算不出来），原因写进 last_error
 */
static int dict_id_for(dsh_engine *e, const char *mdx_path, char out_id[DSH_DICT_ID_BUF_LEN]) {
  dsh_file_info info;
  if (dsh_file_stat(mdx_path, &info) != 0) {
    dsh_set_last_error("取不到文件信息：%s", mdx_path);
    return -1;
  }
  if (!info.exists) {
    dsh_set_last_error("文件不存在：%s", mdx_path);
    return -1;
  }
  if (!info.is_file) {
    dsh_set_last_error("那不是一个普通文件（目录？）：%s", mdx_path);
    return -1;
  }
  const char *hit = cache_lookup(e, mdx_path, info.size, info.mtime);
  if (hit != NULL) {
    memcpy(out_id, hit, DSH_DICT_ID_BUF_LEN);
    return 0;
  }
  if (dsh_dict_id_of_file(mdx_path, out_id) != 0) return -1;
  cache_store(e, mdx_path, info.size, info.mtime, out_id);
  return 0;
}

/* ── 打开 / 关闭 ────────────────────────────────────────────────────────── */

/** 删掉 `path` 后面接 `suffix` 的那个兄弟文件（`lookup.db` → `lookup.db-wal`）*/
static void remove_sibling(const char *path, const char *suffix) {
  const size_t n = strlen(path);
  const size_t m = strlen(suffix);
  char *sib = (char *)dsh_mem_alloc(n + m + 1);
  if (sib == NULL) return;
  memcpy(sib, path, n);
  memcpy(sib + n, suffix, m);
  sib[n + m] = '\0';
  (void)dsh_file_remove(sib); /* 不在也算成功 —— 我们要的就是"它不在" */
  dsh_release(sib);
}

/**
 * 把「上一版留下的历史」一次性收口。
 *
 * ① `settings.json` 里的 `history` 数组（老版本把历史存在设置里）：它会**一直**跟着设置走
 *    （每次存设置都要整份重写一遍），所以**导入一次、然后把那个键删掉** —— 两份并存必然分叉。
 *    ⚠️ 三条自我约束：**每次都试**（导入按 (word, dictId) 去重，重跑安全）；**导入成功才删键**
 *    （删或写失败就把键留着，下次启动再试 —— 这条迁移是自愈的，任何一步失败都不丢数据）；
 *    历史打不开时**一件都不做**。
 * ② `<配置目录>/lookup.db`（中途那一版 SQLite 的库）：**明确忽略并删除** —— 为了搬一份
 *    **从未发布过**的中间态数据再把 SQLite 请回来是本末倒置；`-wal` / `-shm` 一起清掉
 *    （留一个孤零零的 WAL 比留着库更糟）。
 */
static void migrate_legacy_history(dsh_engine *e, int have_history) {
  if (have_history) {
    size_t raw_len = 0;
    const char *raw = dsh_settings_foreign_raw(e->settings, "history", &raw_len);
    if (raw != NULL && raw_len > 0) {
      int64_t imported = 0;
      if (dsh_hist_import_json(e->history, raw, raw_len, &imported) == 0) {
        dsh_settings *next = NULL;
        if (dsh_settings_drop_foreign(e->settings, "history", &next) == 0 && next != NULL) {
          dsh_settings *prev = e->settings;
          e->settings = next;
          if (save_settings(e) != 0) {
            /* 落盘失败：内存回退，那个键在文件里照旧 —— 下次启动再试（去重保证幂等）*/
            e->settings = prev;
            dsh_settings_free(next);
          } else {
            dsh_settings_free(prev);
          }
        }
      }
    }
  }

  if (e->user_data_dir[0] != '\0') {
    char *db = NULL;
    if (dsh_path_join(e->user_data_dir, DSH_ENGINE_LEGACY_DB_FILE, &db) == 0 && db != NULL) {
      dsh_file_info info;
      if (dsh_file_stat(db, &info) == 0 && info.exists) {
        /* 先清兄弟（WAL/SHM）再删库：顺序反过来的话，中途失败会留下
         * "库没了、WAL 还在"那种更难理解的状态。 */
        remove_sibling(db, "-wal");
        remove_sibling(db, "-shm");
        (void)dsh_file_remove(db);
      }
      dsh_release(db);
    }
  }
}

/** 把 settings.json 读成一个字符串（没有文件时返回 NULL 且 *ok=1） */
static char *read_settings_file(dsh_engine *e, int *ok) {
  char *path = settings_path(e);
  char *text = NULL;
  *ok = 1;
  if (path == NULL) return NULL;

  dsh_file_info info;
  const int have = (dsh_file_stat(path, &info) == 0);
  if (!have || !info.exists || !info.is_file) {
    dsh_release(path);
    return NULL; /* 没有设置文件 = 全默认，不是错误 */
  }
  FILE *fp = dsh_file_open_read(path);
  if (fp == NULL) {
    dsh_release(path);
    *ok = 0;
    dsh_set_last_error("配置目录里有 settings.json，但打不开它（%s）", path);
    return NULL;
  }
  if (info.size > 0 && info.size <= (int64_t)8 * 1024 * 1024) {
    text = (char *)dsh_mem_alloc((size_t)info.size + 1);
    if (text != NULL) {
      const size_t got = fread(text, 1, (size_t)info.size, fp);
      text[got] = '\0';
      if (got != (size_t)info.size) {
        dsh_release(text);
        text = NULL;
        *ok = 0;
        dsh_set_last_error("设置文件读到一半就断了（%s）", path);
      }
    }
  } else if (info.size > (int64_t)8 * 1024 * 1024) {
    *ok = 0;
    dsh_set_last_error("设置文件大得不像设置（%lld 字节）：%s", (long long)info.size, path);
  }
  fclose(fp);
  dsh_release(path);
  return text;
}

enum dsh_error dsh_engine_create(const char *user_data_dir, dsh_engine **out_engine) {
  if (out_engine == NULL) {
    dsh_set_last_error("dsh_engine_create：out_engine 不能为空");
    return DSH_E_INVALID_ARG;
  }
  *out_engine = NULL;

  dsh_engine *e = (dsh_engine *)dsh_mem_alloc(sizeof(*e));
  if (e == NULL) {
    dsh_set_last_error("内存不足：引擎句柄");
    return DSH_E_OOM;
  }
  memset(e, 0, sizeof(*e));
  e->user_data_dir = dsh_mem_strdup(user_data_dir != NULL ? user_data_dir : "");
  if (e->user_data_dir == NULL) {
    dsh_engine_destroy(e);
    dsh_set_last_error("内存不足：配置目录副本");
    return DSH_E_OOM;
  }
  e->cache = (id_cache_entry *)dsh_mem_alloc(DSH_ENGINE_ID_CACHE_MAX * sizeof(id_cache_entry));
  if (e->cache == NULL) {
    dsh_engine_destroy(e);
    dsh_set_last_error("内存不足：id 缓存");
    return DSH_E_OOM;
  }
  memset(e->cache, 0, DSH_ENGINE_ID_CACHE_MAX * sizeof(id_cache_entry));

  int ok = 1;
  char *text = read_settings_file(e, &ok);
  if (!ok) {
    if (text != NULL) dsh_release(text);
    const char *saved = dsh_last_error_message();
    dsh_engine_destroy(e);
    dsh_set_last_error("%s", saved ? saved : "设置读不动");
    if (saved != NULL) dsh_release((void *)saved);
    return DSH_E_IO;
  }
  const int rc = dsh_settings_parse(text, (text != NULL) ? strlen(text) : 0, &e->settings);
  if (text != NULL) dsh_release(text);
  if (rc != 0) {
    dsh_engine_destroy(e);
    return DSH_E_FORMAT; /* last_error 已由 dsh_settings_parse 写好 */
  }

  /*
   * 查词历史（`<配置目录>/history.jsonl`，一行一条 JSON 的追加文件）。
   *
   * ⚠️ **打不开不算建引擎失败**：历史是附加功能，没有它照样能查词。但也不会不声不响 ——
   *    原因留在 `e->history_why` 里，查询接口原样说给用户。**绝不能**「打不开就当空历史」，
   *    那会让用户以为历史被清空了。没有配置目录 = 只在内存里活，那就不开文件（不是错误）。
   */
  int have_history = 0;
  if (e->user_data_dir[0] != '\0') {
    char *hist_path = NULL;
    if (dsh_path_join(e->user_data_dir, DSH_ENGINE_HISTORY_FILE, &hist_path) == 0 &&
        hist_path != NULL) {
      dsh_history *h = NULL;
      if (dsh_hist_open(hist_path, &h) == 0) {
        e->history = h;
        have_history = 1;
      } else {
        const char *saved = dsh_last_error_message();
        e->history_why = dsh_mem_strdup(saved != NULL ? saved : "历史打不开");
        if (saved != NULL) dsh_release((void *)saved);
      }
      dsh_release(hist_path);
    }
  }

  /* 上一版留下的历史（settings.json 里那个数组 / SQLite 那个库）：一次性收口 */
  migrate_legacy_history(e, have_history);

  *out_engine = e;
  return DSH_OK;
}

void dsh_engine_destroy(dsh_engine *e) {
  if (e == NULL) return;
  /* ⚠️ 先卸掉已加载的词典：每本都握着映射视图、键块索引表等一堆活分配 ——
   *    漏了这一步，整本词典的内存就留在活分配表里。 */
  dsh_dicts_unload_all(e);
  if (e->history != NULL) dsh_hist_close(e->history);
  if (e->history_why != NULL) dsh_release(e->history_why);
  if (e->shown_key != NULL) dsh_release(e->shown_key);
  if (e->shown_dict_id != NULL) dsh_release(e->shown_dict_id);
  if (e->cache != NULL) {
    for (int64_t i = 0; i < e->cache_count; i++) {
      if (e->cache[i].path != NULL) dsh_release(e->cache[i].path);
    }
    dsh_release(e->cache);
  }
  if (e->settings != NULL) dsh_settings_free(e->settings);
  if (e->user_data_dir != NULL) dsh_release(e->user_data_dir);
  dsh_release(e);
}

const char *dsh_engine_user_data_dir(const dsh_engine *e) {
  return (e == NULL || e->user_data_dir == NULL) ? "" : e->user_data_dir;
}

const struct dsh_settings *dsh_engine_settings(const dsh_engine *e) {
  return (e == NULL) ? NULL : e->settings;
}

int dsh_engine_replace_settings(dsh_engine *e, struct dsh_settings *next) {
  if (e == NULL || next == NULL) {
    dsh_set_last_error("dsh_engine_replace_settings：参数不能为空");
    return -1;
  }
  if (e->settings != NULL) dsh_settings_free(e->settings);
  e->settings = next;
  return 0;
}

/* ── 设置 ───────────────────────────────────────────────────────────────── */

enum dsh_error dsh_engine_settings_get(dsh_engine *e, char **out_json) {
  if (out_json == NULL) {
    dsh_set_last_error("dsh_engine_settings_get：out_json 不能为空");
    return DSH_E_INVALID_ARG;
  }
  *out_json = NULL;
  if (e == NULL || e->settings == NULL) {
    dsh_set_last_error("dsh_engine_settings_get：引擎无效");
    return DSH_E_INVALID_ARG;
  }
  char *json = dsh_settings_to_json(e->settings);
  if (json == NULL) return DSH_E_OOM;
  *out_json = json;
  return 0;
}

enum dsh_error dsh_engine_settings_set(dsh_engine *e, const char *patch_json, char **out_json) {
  if (out_json == NULL) {
    dsh_set_last_error("dsh_engine_settings_set：out_json 不能为空");
    return DSH_E_INVALID_ARG;
  }
  *out_json = NULL;
  if (e == NULL || e->settings == NULL) {
    dsh_set_last_error("dsh_engine_settings_set：引擎无效");
    return DSH_E_INVALID_ARG;
  }
  dsh_settings *next = NULL;
  if (dsh_settings_apply_patch(e->settings, patch_json,
                               patch_json != NULL ? strlen(patch_json) : 0, &next) != 0) {
    return DSH_E_FORMAT; /* last_error 已由 dsh_settings_apply_patch 写好 */
  }
  /* ⚠️ 顺序：**先落盘、再换内存里那份**。落盘失败时内存里的设置保持不动 ——
   * 否则会出现"界面显示改了、重启就变回去"，那比直接报错难查得多。 */
  dsh_settings *prev = e->settings;
  e->settings = next;
  if (save_settings(e) != 0) {
    e->settings = prev;
    dsh_settings_free(next);
    return DSH_E_IO;
  }
  dsh_settings_free(prev);
  return dsh_engine_settings_get(e, out_json);
}

/* ── 词库清单 ───────────────────────────────────────────────────────────── */

/** 把一本的信息写成一个 JSON 对象 */
/** 拼一句「前半句 + 书名 + 后半句」（分配失败回 NULL）*/
static char *note_sentence(const char *head, const char *title, const char *tail) {
  const size_t need = strlen(head) + strlen(title) + strlen(tail) + 1;
  char *out = (char *)dsh_mem_alloc(need);
  if (out == NULL) {
    dsh_set_last_error("内存不足：词典状态那句说明");
    return NULL;
  }
  snprintf(out, need, "%s%s%s", head, title, tail);
  return out;
}

/**
 * **这一本现在什么状态**：空串（好好的）/ `removed`（不在词库里）/ `missing`（文件没了），
 * 并把那句要**原样显示**的人话写进 `*out_note`（可用时是 NULL）。
 *
 * 为什么归内核：「哪一本、为什么、怎么恢复」是产品约定，而检查标准（还在不在词库里、文件
 * 还在不在磁盘上）本来就只有内核手里有 —— 历史每一行与词库清单每一本都用**这一处**实现。
 *
 * ⚠️ 两处约定：①「不在词库里」时书名只能用**调用方手里那份快照**（那本已经没得查了），
 *    「文件不在了」用**当前显示名**（改过名就该看到新名字）；② **两种情形是两句话**，
 *    不许合成一句（原因不同、恢复办法也不同）。
 */
const char *dsh_dict_status(const struct dsh_engine *e, const char *dict_id,
                            const char *snapshot_title, char **out_note) {
  if (out_note != NULL) *out_note = NULL;
  const struct dsh_settings *s = (e != NULL) ? e->settings : NULL;
  const dsh_stored_dict *d = (s != NULL && dict_id != NULL && dict_id[0] != '\0')
                                 ? dsh_settings_dict_by_id(s, dict_id)
                                 : NULL;
  if (d == NULL) {
    if (out_note != NULL) {
      *out_note = note_sentence("《",
                                (snapshot_title != NULL && snapshot_title[0] != '\0')
                                    ? snapshot_title
                                    : "那本词典",
                                "》已不在词库中 —— 重新导入到原路径即可恢复");
    }
    return "removed";
  }
  dsh_file_info info;
  const int have = (dsh_file_stat(d->mdx_path, &info) == 0);
  if (!(have && info.exists && info.is_file)) {
    if (out_note != NULL) {
      *out_note = note_sentence("《", dsh_settings_dict_display_name(d, NULL),
                                "》的文件不在了 —— 把它放回原来的位置，或重新导入一次");
    }
    return "missing";
  }
  return "";
}

static void emit_dict_item(dsh_json *j, const struct dsh_engine *e, const dsh_stored_dict *d,
                           int loaded) {
  dsh_file_info info;
  const int have = (dsh_file_stat(d->mdx_path, &info) == 0);
  const int exists = have && info.exists && info.is_file;

  /*
   * **按需开一次头**：词库页要显示每本词典的「书名 / 词条数 / 编码 / 版本」，而这四个
   * 东西只有文件头里有 —— 不读头就只能显示占位符。
   *
   * 只读**头 + 键信息**（`dsh_mdx_open` 那一层），不碰记录块、不建索引 —— 也就是说不算
   * 「把词典加载起来」。读不到（文件不在 / 头坏了）时**如实给空值**，界面照样显示占位符。
   *
   * ⚠️ 别把它挪到「引擎启动时」去做：启动时用户可能挂了十几本词典，而清单这一次调用是
   *    **用户点开词库页才发生**的（一次开头的代价 = 几毫秒）。
   */
  char *header_title = NULL;
  int64_t entry_count = 0;
  char encoding[32];
  char version[32];
  encoding[0] = '\0';
  version[0] = '\0';
  if (exists) {
    dsh_mdx *peek = NULL;
    if (dsh_mdx_open(d->mdx_path, &peek) == 0 && peek != NULL) {
      const char *t = dsh_mdx_title(peek);
      if (t != NULL && t[0] != '\0') header_title = dsh_mem_strdup(t);
      entry_count = dsh_mdx_key_count(peek);
      const char *enc = dsh_mdx_encoding_name(peek);
      if (enc != NULL) {
        snprintf(encoding, sizeof(encoding), "%s", enc);
      }
      snprintf(version, sizeof(version), "%.1f", dsh_mdx_version(peek));
      dsh_mdx_close(peek);
    }
  }

  dsh_json_object_begin(j);
  dsh_json_kv_str(j, "id", d->id);
  /* 显示名：内核那一处的约定（改过的名 → 头里的书名 → 文件名）。
   * 头里的书名现在**真的去读了**（见上面那段）—— 这就是"新加的词典显示文件名"那一条的修法：
   * 加进来的时候就把书名存下来了（`dsh_engine_dict_add`），这里是给**存量记录**兜底
   * （老记录里 `title` 就是文件名，`customTitle` 为空时用头里的书名顶上去）。 */
  dsh_json_kv_str(j, "title", dsh_settings_dict_display_name(d, header_title));
  dsh_json_kv_str(j, "headerTitle", (header_title != NULL) ? header_title : "");
  dsh_json_kv_i64(j, "entryCount", entry_count);
  dsh_json_kv_str(j, "encoding", encoding);
  dsh_json_kv_str(j, "version", version);
  if (header_title != NULL) dsh_release(header_title);
  dsh_json_key(j, "customTitle");
  if (d->custom_title != NULL) dsh_json_str(j, d->custom_title);
  else dsh_json_null(j);
  dsh_json_kv_str(j, "mdxPath", d->mdx_path);
  dsh_json_kv_str(j, "fileName", dsh_path_basename(d->mdx_path));
  dsh_json_kv_i64(j, "fileSize", exists ? info.size : 0);
  dsh_json_kv_i64(j, "addedAt", d->added_at);
  dsh_json_kv_i64(j, "mddCount", d->mdd_count);
  dsh_json_key(j, "mddPaths");
  dsh_json_array_begin(j);
  for (int64_t k = 0; k < d->mdd_count; k++) dsh_json_str(j, d->mdd_paths[k]);
  dsh_json_array_end(j);
  /* ⚠️ `exists` 与 `loaded` 是**两个**字段，别混：前者是"磁盘上还在不在"
   * （用户可能把词典挪走了 —— 那时这是唯一线索），后者是"内存里加载了没有"。
   * `loaded` 读的是**真的加载状态**（`dsh_dicts_is_loaded`）——
   * 在此之前它曾经恒为 false（那时 `lookup` 还没做），那是"如实报"；
   * 现在加载层有了，它就必须接上，否则这个字段变成一句谎话。 */
  dsh_json_kv_bool(j, "exists", exists);
  dsh_json_kv_bool(j, "loaded", loaded ? 1 : 0);
  /*
   * `unavailable` / `note`：**这一本现在什么状态**（第三十八轮加的）。
   *
   * 与历史每一行那两个字段**同一套词、同一处实现**（`dsh_dict_status`）——
   * 凡是要显示"这一本还在不在、为什么、怎么恢复"的地方都用一份，
   * 免得界面上出现第二份说法（返回那条路要用它：返回栈里那一本可能已经被移除 / 文件丢了）。
   */
  {
    char *note = NULL;
    const char *status = dsh_dict_status(e, d->id, NULL, &note);
    dsh_json_kv_str(j, "unavailable", status);
    dsh_json_kv_str(j, "note", (note != NULL) ? note : "");
    if (note != NULL) dsh_release(note);
  }
  dsh_json_object_end(j);
}

enum dsh_error dsh_engine_dict_list(dsh_engine *e, char **out_json) {
  if (out_json == NULL) {
    dsh_set_last_error("dsh_engine_dict_list：out_json 不能为空");
    return DSH_E_INVALID_ARG;
  }
  *out_json = NULL;
  if (e == NULL || e->settings == NULL) {
    dsh_set_last_error("dsh_engine_dict_list：引擎无效");
    return DSH_E_INVALID_ARG;
  }
  dsh_json *j = dsh_json_new();
  if (j == NULL) return DSH_E_OOM;
  dsh_json_array_begin(j);
  const int64_t n = dsh_settings_dict_count(e->settings);
  for (int64_t i = 0; i < n; i++) {
    const dsh_stored_dict *d = dsh_settings_dict_at(e->settings, i);
    const char *id = (d != NULL) ? d->id : NULL;
    emit_dict_item(j, e, d, dsh_dicts_is_loaded((struct dsh_engine *)e, id));
  }
  dsh_json_array_end(j);
  char *text = dsh_json_take(j);
  dsh_json_free(j);
  if (text == NULL) {
    dsh_set_last_error("内存不足：词库清单 JSON");
    return DSH_E_OOM;
  }
  *out_json = text;
  return 0;
}

/* ── 导入 ───────────────────────────────────────────────────────────────── */

typedef struct {
  const char *path;
  char reason[256];
} add_failure;

static void note_failure(add_failure *f, int64_t *count, const char *path, const char *fmt,
                         const char *arg) {
  f[*count].path = path;
  if (arg != NULL) snprintf(f[*count].reason, sizeof(f[*count].reason), fmt, arg);
  else snprintf(f[*count].reason, sizeof(f[*count].reason), "%s", fmt);
  (*count)++;
}

enum dsh_error dsh_engine_dict_add(dsh_engine *e, const char *paths_json, char **out_json) {
  if (out_json == NULL) {
    dsh_set_last_error("dsh_engine_dict_add：out_json 不能为空");
    return DSH_E_INVALID_ARG;
  }
  *out_json = NULL;
  if (e == NULL || e->settings == NULL) {
    dsh_set_last_error("dsh_engine_dict_add：引擎无效");
    return DSH_E_INVALID_ARG;
  }
  if (paths_json == NULL) {
    dsh_set_last_error("dsh_engine_dict_add：路径数组不能为空");
    return DSH_E_INVALID_ARG;
  }

  dsh_json_doc *doc = NULL;
  if (dsh_json_parse(paths_json, strlen(paths_json), &doc) != 0) {
    const char *saved = dsh_last_error_message();
    dsh_set_last_error("导入参数读不动（应当是一个路径数组）：%s", saved ? saved : "");
    if (saved != NULL) dsh_release((void *)saved);
    return DSH_E_FORMAT;
  }
  const dsh_json_node *root = dsh_json_doc_root(doc);
  if (!dsh_json_is_array(root)) {
    dsh_json_doc_free(doc);
    dsh_set_last_error("导入参数必须是一个路径数组，例如 [\"C:\\\\词典\\\\a.mdx\"]");
    return DSH_E_FORMAT;
  }
  const int64_t n = dsh_json_array_len(root);
  if (n > DSH_ENGINE_ADD_MAX) {
    dsh_json_doc_free(doc);
    dsh_set_last_error("一次导入的路径太多（%lld 个，上限 %d）", (long long)n,
                       DSH_ENGINE_ADD_MAX);
    return DSH_E_INVALID_ARG;
  }

  add_failure *failures = NULL;
  int64_t failure_count = 0;
  int64_t added = 0;
  if (n > 0) {
    failures = (add_failure *)dsh_mem_alloc((size_t)n * sizeof(*failures));
    if (failures == NULL) {
      dsh_json_doc_free(doc);
      dsh_set_last_error("内存不足：导入结果");
      return DSH_E_OOM;
    }
    memset(failures, 0, (size_t)n * sizeof(*failures));
  }

  dsh_settings *cur = e->settings;

  for (int64_t i = 0; i < n; i++) {
    const dsh_json_node *item = dsh_json_array_at(root, i);
    if (!dsh_json_is_string(item)) {
      note_failure(failures, &failure_count, "(不是字符串)",
                   "路径数组里出现了不是字符串的项", NULL);
      continue;
    }
    const char *path = dsh_json_str_value(item, NULL);
    if (path == NULL || path[0] == '\0') {
      note_failure(failures, &failure_count, "(空路径)", "路径是空的", NULL);
      continue;
    }

    /* ⚠️ 三步都必须**先确认再做**（接口定义点名的那条，参考实现那个坑的形态）：
     *    ① 是 .mdx？② 真的存在、是文件？③ 才算 id。
     *    顺序上先查扩展名 —— 用户拖进来的多半是别的东西，先给一句说得清的原因。 */
    if (!dsh_path_is_mdx(path)) {
      note_failure(failures, &failure_count, path, "不是 .mdx 文件（扩展名不认）", NULL);
      continue;
    }
    char id[DSH_DICT_ID_BUF_LEN] = "";
    if (dict_id_for(e, path, id) != 0) {
      const char *why = dsh_last_error_message();
      note_failure(failures, &failure_count, path, "%s", why != NULL ? why : "算不出内容哈希");
      if (why != NULL) dsh_release((void *)why);
      continue;
    }

    /* 配套资源卷：同目录下的 .mdd / .1.mdd / .2.mdd…（找不到也可以：不带 .mdd 的词典） */
    char **mdd = NULL;
    int64_t mdd_count = 0;
    (void)dsh_path_find_mdd_volumes(path, &mdd, &mdd_count);

    /*
     * ★ 书名**从 .mdx 头里读**（用户 2026-09 报的第 2 条：新加的词典在词库页显示的是
     *   "文件名（带 `.mdx`）"）。原来存的是 `dsh_path_basename(path)`，于是用户第一眼
     *   看到的是磁盘上的文件名，而不是这本书自己的名字（参考实现存的就是头里的书名）。
     * 读不到（头坏了 / 不是标准 .mdx）就**回落文件名** —— 至少有个能认的东西。
     * 这一次开头的代价只发生在"用户选了文件、点导入"那一刻，一次性。
     */
    char *title_from_header = NULL;
    {
      dsh_mdx *peek = NULL;
      if (dsh_mdx_open(path, &peek) == 0 && peek != NULL) {
        const char *t = dsh_mdx_title(peek);
        if (t != NULL && t[0] != '\0') title_from_header = dsh_mem_strdup(t);
        dsh_mdx_close(peek);
      }
    }
    const char *store_title =
        (title_from_header != NULL) ? title_from_header : dsh_path_basename(path);

    dsh_settings *next = NULL;
    const int rc = dsh_settings_dict_add(cur, id, path, store_title,
                                         (const char *const *)mdd, mdd_count, 0, &next);
    if (title_from_header != NULL) dsh_release(title_from_header);
    if (mdd != NULL) {
      for (int64_t k = 0; k < mdd_count; k++) dsh_release(mdd[k]);
      dsh_release(mdd);
    }
    if (rc != 0) {
      const char *why = dsh_last_error_message();
      note_failure(failures, &failure_count, path, "%s", why != NULL ? why : "加进词库失败");
      if (why != NULL) dsh_release((void *)why);
      continue;
    }
    dsh_settings *prev = cur;
    cur = next;
    if (prev != e->settings) dsh_settings_free(prev);
    added++;
  }

  /* 落盘并换掉引擎里那份（失败就整份回退 —— 与 settings_set 同一个理由） */
  if (cur != e->settings) {
    dsh_settings *prev = e->settings;
    e->settings = cur;
    if (save_settings(e) != 0) {
      e->settings = prev;
      dsh_settings_free(cur);
      if (failures != NULL) dsh_release(failures);
      dsh_json_doc_free(doc);
      return DSH_E_IO;
    }
    dsh_settings_free(prev);
  }

  {
    dsh_json *j = dsh_json_new();
    if (j == NULL) {
      if (failures != NULL) dsh_release(failures);
      dsh_json_doc_free(doc);
      return DSH_E_OOM;
    }
    dsh_json_object_begin(j);
    dsh_json_kv_i64(j, "added", added);
    dsh_json_key(j, "failed");
    dsh_json_array_begin(j);
    for (int64_t i = 0; i < failure_count; i++) {
      dsh_json_object_begin(j);
      dsh_json_kv_str(j, "path", failures[i].path);
      dsh_json_kv_str(j, "reason", failures[i].reason);
      dsh_json_object_end(j);
    }
    dsh_json_array_end(j);
    dsh_json_object_end(j);
    char *text = dsh_json_take(j);
    dsh_json_free(j);
    if (failures != NULL) dsh_release(failures);
    dsh_json_doc_free(doc);
    if (text == NULL) {
      dsh_set_last_error("内存不足：导入结果 JSON");
      return DSH_E_OOM;
    }
    *out_json = text;
  }
  return 0;
}

/* ── 移除 / 改名 / 指定当前 ─────────────────────────────────────────────── */

/** 三条"改一下清单再出 JSON"的公共尾巴 */
static int commit_and_list(dsh_engine *e, dsh_settings *next, char **out_json) {
  dsh_settings *prev = e->settings;
  e->settings = next;
  if (save_settings(e) != 0) {
    e->settings = prev;
    dsh_settings_free(next);
    return DSH_E_IO;
  }
  dsh_settings_free(prev);
  return dsh_engine_dict_list(e, out_json);
}

enum dsh_error dsh_engine_dict_remove(dsh_engine *e, const char *dict_id, char **out_json) {
  if (out_json == NULL) {
    dsh_set_last_error("dsh_engine_dict_remove：out_json 不能为空");
    return DSH_E_INVALID_ARG;
  }
  *out_json = NULL;
  if (e == NULL || e->settings == NULL || dict_id == NULL) {
    dsh_set_last_error("dsh_engine_dict_remove：参数不能为空");
    return DSH_E_INVALID_ARG;
  }
  dsh_settings *next = NULL;
  if (dsh_settings_dict_remove(e->settings, dict_id, &next) != 0)
    return DSH_E_NOT_FOUND; /* 常见是「词库里没有这本词典」，last_error 已由被调方写好 */
  return commit_and_list(e, next, out_json);
}

enum dsh_error dsh_engine_dict_rename(dsh_engine *e, const char *dict_id, const char *name,
                                  char **out_json) {
  if (out_json == NULL) {
    dsh_set_last_error("dsh_engine_dict_rename：out_json 不能为空");
    return DSH_E_INVALID_ARG;
  }
  *out_json = NULL;
  if (e == NULL || e->settings == NULL || dict_id == NULL) {
    dsh_set_last_error("dsh_engine_dict_rename：参数不能为空");
    return DSH_E_INVALID_ARG;
  }
  dsh_settings *next = NULL;
  if (dsh_settings_dict_rename(e->settings, dict_id, name, &next) != 0)
    return DSH_E_NOT_FOUND; /* 同上 */
  return commit_and_list(e, next, out_json);
}

/*
 * 移动一本词典在清单里的位置（2026-09 新加，0.2.0 专有）。
 * 尾巴与上面三条一模一样（`commit_and_list`：先落盘、失败就把旧设置摆回去、再出清单），
 * 所以这里只做参数校验 + 转调 `dsh_settings_dict_move`。
 */
enum dsh_error dsh_engine_dict_move(dsh_engine *e, const char *dict_id, int32_t delta,
                                    char **out_json) {
  if (out_json == NULL) {
    dsh_set_last_error("dsh_engine_dict_move：out_json 不能为空");
    return DSH_E_INVALID_ARG;
  }
  *out_json = NULL;
  if (e == NULL || e->settings == NULL || dict_id == NULL) {
    dsh_set_last_error("dsh_engine_dict_move：参数不能为空");
    return DSH_E_INVALID_ARG;
  }
  dsh_settings *next = NULL;
  if (dsh_settings_dict_move(e->settings, dict_id, (int64_t)delta, &next) != 0)
    return DSH_E_NOT_FOUND; /* 同上：常见是「词库里没有这本词典」 */
  return commit_and_list(e, next, out_json);
}

enum dsh_error dsh_engine_dict_set_current(dsh_engine *e, const char *dict_id, char **out_json) {
  if (out_json == NULL) {
    dsh_set_last_error("dsh_engine_dict_set_current：out_json 不能为空");
    return DSH_E_INVALID_ARG;
  }
  *out_json = NULL;
  if (e == NULL || e->settings == NULL) {
    dsh_set_last_error("dsh_engine_dict_set_current：引擎无效");
    return DSH_E_INVALID_ARG;
  }
  dsh_settings *next = NULL;
  if (dsh_settings_dict_set_current(e->settings, dict_id, &next) != 0)
    return DSH_E_NOT_FOUND; /* 同上 */
  return commit_and_list(e, next, out_json);
}
