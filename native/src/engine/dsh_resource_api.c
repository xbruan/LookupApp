/* ==========================================================================
 * 接口定义 `lookup` 组的 `dsh_engine_resource` —— 按 key 取一段资源字节。
 *
 * 一条资源请求要回答三件事（接口定义里的 `{total,mime,etag,found,reason}`）：
 *   ① **去哪找**：先 `.mdd` 卷、再同目录（`dict/dsh_sibling.c`）；② **是什么类型**：`dict/dsh_mime.c`；
 *   ③ **缓存身份证**：来源 : 名字 : 长度-修改时间；`offset` / `length` 就是 Range（宿主据此实现 206 / 416）。
 *
 * ── 为什么 Range 由内核切、HTTP 由宿主发 ─────────────────────────────────
 * 「206 / 416 怎么发」是**平台层**的事（各 WebView 各写各的），而「这段字节从哪来、总共多长」是**产品约定** ——
 * 切错了（少一个字节、越界读）播放器与字体都会莫名坏掉，所以切这件事必须在核内、只有一份。
 *
 * ⚠️ ETag 里那个时间戳与参考实现**单位不同**（这里是 Unix 毫秒，它是 .NET 的 `Ticks`）：
 *    ETag 是个**不透明**字符串（只在同一个实现内部比较），单位不同不影响任何行为 ——
 *    但「逐字节对齐参考实现」这句话在 ETag 上不成立，别拿去逐字节对照。
 * ========================================================================== */

#include "dsh_lookup.h"
#include "dsh_internal.h"
#include "audio/dsh_audio.h"
#include "dict/dsh_sibling.h"
#include "dict/dsh_mime.h"
#include "engine/dsh_dicts.h"
#include "engine/dsh_engine_internal.h"
#include "engine/dsh_settings.h"
#include "json_writer.h"
#include "platform/dsh_file.h"

#include <stdio.h>
#include <string.h>



/** ETag = 来源 : 名字 : 长度-修改时间；元数据读不到就回 NULL（宁可不缓存，也别给错身份证）*/
static char *make_etag(const char *source, const char *name, const char *file_path) {
  dsh_file_info info;
  if (dsh_file_stat(file_path, &info) != 0 || !info.exists) return NULL;
  const size_t need = strlen(source) + strlen(name) + 96;
  char *out = (char *)dsh_mem_alloc(need);
  if (out == NULL) return NULL;
  snprintf(out, need, "\"%s:%s:%lld-%lld\"", source, name, (long long)info.size,
           (long long)info.mtime);
  return out;
}

static char *make_etag(const char *source, const char *name, const char *file_path);

/**
 * 在 `.mdd` 卷里按**一个**键名取一次（命中就写出字节 / 总长 / 身份证）。
 * 抽出来是因为「取资源」要试两轮键名（前缀变体一轮、音频扩展名变体一轮），
 * 而「命中之后怎么记身份证」两轮必须一模一样 —— 复制一份迟早分叉。
 */
static void try_mdd_key(dsh_engine *engine, const char *dict_id, const char *key, char **bytes,
                        size_t *total, char **etag) {
  if (*bytes != NULL) return;
  char *hit = NULL;
  size_t hit_len = 0;
  const int rc = dsh_dicts_mdd_fetch(engine, dict_id, key, &hit, &hit_len);
  if (rc != 1) return;
  *bytes = hit;
  *total = hit_len;
  /* 身份证取**卷文件**的长度与修改时间（与参考实现一致）*/
  dsh_mdx *const *volumes = NULL;
  int volume_count = 0;
  if (dsh_dicts_mdd_volumes(engine, dict_id, &volumes, &volume_count) == 0) {
    for (int v = 0; v < volume_count && *etag == NULL; v++) {
      if (dsh_mdx_fetch(volumes[v], key, NULL, NULL, NULL) != 1) continue;
      *etag = make_etag("mdd", key, dsh_mdx_path(volumes[v]));
    }
  }
}

enum dsh_error dsh_engine_resource(dsh_engine *engine, const char *dict_id, const char *key,
                                   size_t offset, size_t length, uint8_t **out_bytes,
                                   size_t *out_len, char **out_meta_json) {
  if (out_bytes == NULL || out_len == NULL || out_meta_json == NULL) {
    dsh_set_last_error("dsh_engine_resource：三个出参都不能为空");
    return DSH_E_INVALID_ARG;
  }
  *out_bytes = NULL;
  *out_len = 0;
  *out_meta_json = NULL;
  if (engine == NULL) {
    dsh_set_last_error("dsh_engine_resource：引擎无效");
    return DSH_E_STATE;
  }
  if (dict_id == NULL || dict_id[0] == '\0' || key == NULL || key[0] == '\0') {
    dsh_set_last_error("dsh_engine_resource：dict_id 与 key 都不能为空");
    return DSH_E_INVALID_ARG;
  }

  const struct dsh_settings *s = dsh_engine_settings(engine);
  const dsh_stored_dict *d = (s != NULL) ? dsh_settings_dict_by_id(s, dict_id) : NULL;
  if (d == NULL) {
    dsh_set_last_error("dsh_engine_resource：词库里没有这本词典（id=%s）", dict_id);
    return DSH_E_NOT_FOUND;
  }

  /* 词条正文文档与资源都要先把那本加载起来（`.mdd` 是懒开的，见 dsh_dicts.c）*/
  if (dsh_dicts_ensure_loaded(engine, dict_id) != 0) {
    const char *why = dsh_last_error_message();
    dsh_set_last_error("dsh_engine_resource：《%s》加载不了：%s",
                       dsh_settings_dict_display_name(d, NULL), why != NULL ? why : "");
    if (why != NULL) dsh_release((void *)why);
    return DSH_E_NOT_FOUND;
  }

  char *candidates[8] = {0};
  const int candidate_count = dsh_dicts_resource_key_forms(key, candidates, 8);
  if (candidate_count < 0) return DSH_E_OOM;

  char *bytes = NULL;
  size_t total = 0;
  char *etag = NULL;
  char *reason = NULL;

  /* ① `.mdd` 卷（候选键名在外、卷在内 —— 与参考实现同一条顺序）*/
  for (int i = 0; i < candidate_count && bytes == NULL; i++) {
    try_mdd_key(engine, dict_id, candidates[i], &bytes, &total, &etag);
  }

  /*
   * ①b **音频键的扩展名变体**（只在上面一个都没命中时试）。
   * 为什么需要：「词典里把音频链接写成 `.spx`、而音频卷里其实是 `.wav` / `.mp3`」是真实情况，
   * 而上面那几个候选键只换**前缀**（`\` / `/` / 没有）—— 扩展名一个字都不换。
   * 参考实现是在 `/__sound__/` 那条路由上先把键换成一卷里真有的那一个；这一版把同一条约定放在**内核**：
   * 修的是同一个病根、纯逻辑可单测，宿主那条路由也不用懂「音频扩展名该试哪些」（那就是业务规则）。
   * ⚠️ 只对**看起来是音频**的键做：否则请求 `theme.css` 而卷里只有 `theme.wav` 时会**取到错的东西**，比取不到更坏。
   */
  if (bytes == NULL && dsh_audio_is_audio_name(key)) {
    char **audio_keys = NULL;
    int64_t audio_count = 0;
    if (dsh_audio_candidate_keys(key, &audio_keys, &audio_count) == 0) {
      for (int64_t a = 0; a < audio_count && bytes == NULL; a++) {
        if (audio_keys[a] == NULL) continue;
        char *forms[8] = {0};
        const int fc = dsh_dicts_resource_key_forms(audio_keys[a], forms, 8);
        if (fc < 0) break;
        for (int i = 0; i < fc && bytes == NULL; i++) {
          try_mdd_key(engine, dict_id, forms[i], &bytes, &total, &etag);
        }
        dsh_dicts_key_forms_free(forms, fc);
      }
      dsh_audio_keys_free(audio_keys, audio_count);
    }
  }

  /* 同目录（`.mdx` 同目录散放的 css / 字体 / 图片 —— 四道检查在 dsh_sibling.c）*/
  if (bytes == NULL && d->mdx_path != NULL && d->mdx_path[0] != '\0') {
    char *path = dsh_sibling_locate(d->mdx_path, key);
    if (path != NULL) {
      dsh_file_info info;
      if (dsh_file_stat(path, &info) == 0 && info.exists && info.is_file &&
          info.size >= 0 && info.size <= (int64_t)64 * 1024 * 1024) {
        const size_t n = (size_t)info.size;
        char *buf = (char *)dsh_mem_alloc(n + 1);
        if (buf == NULL) {
          dsh_release(path);
          dsh_dicts_key_forms_free(candidates, candidate_count);
          dsh_set_last_error("内存不足：同目录散放的文件（%lld 字节）", (long long)info.size);
          return DSH_E_OOM;
        }
        FILE *fp = dsh_file_open_read(path);
        if (fp != NULL) {
          const size_t got = fread(buf, 1, n, fp);
          fclose(fp);
          if (got == n) {
            buf[n] = '\0';
            bytes = buf;
            total = n;
          } else {
            dsh_release(buf);
          }
        } else {
          dsh_release(buf);
        }
      } else if (info.exists && info.is_file && info.size > (int64_t)64 * 1024 * 1024) {
        reason = dsh_mem_strdup("同目录散放的文件大得不像样式表或图片（超过 64 MB，拒绝读进内存）");
      }
      if (bytes != NULL) {
        etag = make_etag("sibling", dsh_path_basename(path), path);
      }
      dsh_release(path);
    }
  }

  /* 切 Range。接口定义：`length == 0` 表示「到末尾」。*/
  char *slice = NULL;
  size_t slice_len = 0;
  if (bytes != NULL) {
    if (offset > total) {
      if (reason == NULL) {
        reason = dsh_mem_strdup("Range 起点超过资源总长（宿主据此回 416）");
      }
    } else {
      const size_t available = total - offset;
      if (length == 0 || length > available) length = available;
      slice = (char *)dsh_mem_alloc(length + 1);
      if (slice == NULL) {
        dsh_dicts_key_forms_free(candidates, candidate_count);
        dsh_release(bytes);
        if (etag != NULL) dsh_release(etag);
        if (reason != NULL) dsh_release(reason);
        dsh_set_last_error("内存不足：资源切片（%zu 字节）", length);
        return DSH_E_OOM;
      }
      if (length > 0) memcpy(slice, bytes + offset, length);
      slice[length] = '\0';
      slice_len = length;
    }
  } else if (reason == NULL) {
    reason = dsh_mem_strdup("这本词典里没有这个资源");
  }

  /* 包元信息 */
  dsh_json *j = dsh_json_new();
  if (j == NULL) {
    dsh_dicts_key_forms_free(candidates, candidate_count);
    if (bytes != NULL) dsh_release(bytes);
    if (slice != NULL) dsh_release(slice);
    if (etag != NULL) dsh_release(etag);
    if (reason != NULL) dsh_release(reason);
    return DSH_E_OOM;
  }
  dsh_json_object_begin(j);
  dsh_json_kv_i64(j, "total", (int64_t)total);
  dsh_json_kv_str(j, "mime", dsh_mime_for(key));
  dsh_json_key(j, "etag");
  if (etag != NULL) dsh_json_str(j, etag);
  else dsh_json_null(j);
  dsh_json_kv_bool(j, "found", bytes != NULL);
  dsh_json_kv_str(j, "reason", reason != NULL ? reason : "");
  dsh_json_object_end(j);

  char *meta = dsh_json_take(j);
  dsh_json_free(j);

  dsh_dicts_key_forms_free(candidates, candidate_count);
  if (bytes != NULL) dsh_release(bytes);
  if (etag != NULL) dsh_release(etag);
  if (reason != NULL) dsh_release(reason);

  if (meta == NULL) {
    if (slice != NULL) dsh_release(slice);
    dsh_set_last_error("内存不足：资源的元信息 JSON");
    return DSH_E_OOM;
  }
  *out_meta_json = meta;
  /* 出参是 `u8ptr`（裸字节），所以这里**转成无符号指针**再交出去 */
  *out_bytes = (uint8_t *)slice;
  *out_len = slice_len;
  return DSH_OK;
}
