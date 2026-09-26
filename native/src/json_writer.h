/* JSON 输出的最小写入器 —— 内核所有出参都是 JSON，所以它是基础设施。
 * 自己写而不引库：内核承诺零依赖，而要写的东西很简单（对象 / 数组 / 字符串 / 数）。
 * ⚠️ **转义必须是完整的**：字符串常常直接来自词典内容，半角双引号、反斜杠、控制字符
 * 都真实存在；少转义一个字符，宿主收到的就是坏 JSON，而症状是整条查询失败、与真正的
 * 坏字符毫无关联 —— 所以宁可多写几行，也不许用 sprintf 拼字符串。
 * 内存：全部走 dsh_mem_alloc（内核规矩），由 dsh_json_free 还给内核。 */

#ifndef DSH_JSON_WRITER_H
#define DSH_JSON_WRITER_H

#include <stddef.h>
#include <stdint.h>

typedef struct dsh_json dsh_json;

/** 新建一个空的 JSON 缓冲 */
dsh_json *dsh_json_new(void);

/** 取结果（UTF-8，以 \0 结尾；内存归调用方，用 dsh_release 还给内核）。
 *  取过之后这个写入器仍可继续用（返回的是副本）。 */
char *dsh_json_take(dsh_json *j);

/** 释放写入器（不释放 dsh_json_take 交出去的副本） */
void dsh_json_free(dsh_json *j);

/* ── 结构 ───────────────────────────────────────────────────────────────── */

void dsh_json_object_begin(dsh_json *j);
void dsh_json_object_end(dsh_json *j);
void dsh_json_array_begin(dsh_json *j);
void dsh_json_array_end(dsh_json *j);
/** 分隔符（逗号）由写入器自己按需补，调用方不用管 */
void dsh_json_key(dsh_json *j, const char *key);

/* ── 值 ─────────────────────────────────────────────────────────────────── */

void dsh_json_str(dsh_json *j, const char *value);        /* NULL 写成空串 */
/**
 * 按**长度**写一个字符串 —— 内容里可能有内嵌的 `\0`。
 *
 * 为什么需要它：MDict 的记录每条末尾带一个 `\0`，而结束位置是下一条记录的偏移，
 * 所以那个 `\0` 落在记录里面；用 `dsh_json_str` 写会在它那儿截断，给宿主的 JSON 就少一个字符。
 * 规则：写满 `len` 个字节，`\0` 按 `\u0000` 转义（与参考实现同一条约定）。
 */
void dsh_json_str_len(dsh_json *j, const char *value, int64_t len);
void dsh_json_str_raw(dsh_json *j, const char *value);    /* NULL 写成 null */

/**
 * 原样吐一段**已经是合法 JSON 的文本**（不转义、不校验）。
 *
 * 为什么需要它：设置文件里有内核**不认识**的字段；内核把它们原样读进来、再原样写回去，
 * 是**避免数据丢失**的唯一办法 —— 若按自己的理解重写一遍，不认识的字段会在第一次
 * 存盘时不报错地消失。配合 `dsh_json_node_raw`（读取器给出原文片段）使用。
 * ⚠️ 这个调用**不做任何校验**：`json_text` 必须是合法 JSON 值的原文，否则会产出坏 JSON。
 */
void dsh_json_value_raw(dsh_json *j, const char *json_text, size_t len);

void dsh_json_i64(dsh_json *j, int64_t value);
void dsh_json_int(dsh_json *j, int value);
void dsh_json_bool(dsh_json *j, int value);
void dsh_json_double(dsh_json *j, double value);
void dsh_json_null(dsh_json *j);

/** 便捷：直接写一个键值对（省掉 dsh_json_key + 值的两次调用） */
void dsh_json_kv_str(dsh_json *j, const char *key, const char *value);
void dsh_json_kv_i64(dsh_json *j, const char *key, int64_t value);
void dsh_json_kv_int(dsh_json *j, const char *key, int value);
void dsh_json_kv_bool(dsh_json *j, const char *key, int value);

#endif /* DSH_JSON_WRITER_H */
