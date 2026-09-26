/* JSON **读取** —— 与 json_writer.c（写那一半）配对的另一半。
 * 为什么内核需要它：接口定义里 `dsh_engine_settings_set` 收的是**局部补丁 JSON**、
 * `dsh_engine_dict_add` 收的是**路径数组 JSON**；而「业务逻辑只存在于 C 内核」意味着
 * 解析这些 JSON 也必须在内核里做 —— 放到视图层就等于把「哪个字段合法、越界怎么夹回」
 * 搬出了内核。所以这一层与 json_writer.c 同规格：纯 C11、零依赖、每个平台一模一样。
 *
 * ── 约定（三条，都别自己发明）────────────────────────────────────────────
 * ① **严格按 RFC 8259 收**，不做顺手宽容：不许尾随逗号、不许注释、不许单引号、
 *    不许 `NaN`/`Infinity`；数值不许前导零（`01` 是错的）；解析完后面不许还有非空白字符。
 *    理由：宽容的解析器会把宿主写错的 JSON 悄悄吃下去，让「设置没生效」变成
 *    「设置生效了一半」—— 那比当场报错难查一个数量级。
 * ② **字符串里出现坏字节不算失败**：JSON 的字符串按 UTF-8 收，遇到非法/残缺序列
 *    每个坏字节产出一个 U+FFFD、**继续往下解**（与 dsh_text_decode 同一约定）——
 *    整份文档因为一个坏字节读不出来，代价远大于丢掉那一个字符。但**控制字符（< 0x20）
 *    原样出现在字符串里是错的**（RFC 要求转义），那属于宿主把二进制塞进了字符串，报错。
 * ③ **字符串可能含内嵌的 U+0000**（`\u0000` 解出来就是它），所以取字符串一律
 *    **连长度一起给**；缓冲虽然保证以 \0 结尾，但拿它当 C 字符串会把 `\u0000`
 *    之后的内容截掉 —— 要真值请用长度（与 dsh_mdx.c 里「长度不能用 strlen 算」同源）。
 *
 * ── 生存期与所有权 ───────────────────────────────────────────────────────
 * 文档把整棵树解在一个**分块 arena** 里：整块只算一个活分配（`dsh_release(doc)` 一次还清）。
 * 树里的所有指针（节点、字符串内容）都**归文档所有**，`dsh_json_doc_free` 之后就悬空；
 * 取出来的字符串指针**不许释放** —— 它只是 arena 里的一块，活不过文档。
 *
 * ── 为什么不复用 json_writer 的内部结构 ──────────────────────────────────
 * 写的那一半是顺序吐字节、没有树，读的这一半要随机访问（按名字找字段）；两者共用的
 * 是**转义规则**，靠测试里「写出去再读回来必须相等」那条往返断言保护。 */

#ifndef DSH_JSON_READER_H
#define DSH_JSON_READER_H

#include <stddef.h>
#include <stdint.h>

/** JSON 值的种类（位标志，便于一次判"是数字类"） */
typedef enum {
  DSH_JSON_NULL = 1,
  DSH_JSON_BOOL = 2,
  DSH_JSON_NUMBER = 4,
  DSH_JSON_STRING = 8,
  DSH_JSON_ARRAY = 16,
  DSH_JSON_OBJECT = 32
} dsh_json_kind;

/** 一份解析好的文档（不透明；节点从它里面取，都归它所有） */
typedef struct dsh_json_doc dsh_json_doc;

/** 树里的一个节点（借用；`dsh_json_free` 之后悬空） */
typedef struct dsh_json_node dsh_json_node;

/**
 * 解析一份 JSON。
 *
 * @param text  UTF-8 文本（可以为 NULL，此时返回 NULL 并报错）
 * @param len   字节数（**必须给**：文本里可能有内嵌的 U+0000，不能靠 strlen 量）
 * @param out   出参：成功时写入文档句柄
 * @return 0 成功；非零失败（原因由 dsh_last_error_message 给出，含字节偏移）。
 *         失败时 *out 保持 NULL。
 */
int dsh_json_parse(const char *text, size_t len, dsh_json_doc **out);

/** 释放一份文档（连带它的整个arena）。传 NULL 是合法的空操作。
 *
 * ⚠️ 名字里带 `doc`：**写入器那边已经有一个 `dsh_json_free`**
 *    （释放的是"正在拼的那个缓冲"）。两个都叫 free 会在链接期直接撞车
 *    （第一次编译就是这么红的）。所以"写"的一半是 `dsh_json_free(dsh_json*)`，
 *    "读"的一半是 `dsh_json_doc_free(dsh_json_doc*)` —— 看类型就分得清。
 */
void dsh_json_doc_free(dsh_json_doc *doc);

/** 根节点。doc 为 NULL 时返回 NULL。 */
const dsh_json_node *dsh_json_doc_root(const dsh_json_doc *doc);

/* ── 节点访问 ───────────────────────────────────────────────────────────── */

/** 节点的种类；node 为 NULL 时返回 0（既不等于任何一种，也不等于"空文档"） */
int dsh_json_node_kind(const dsh_json_node *node);

/** 便捷检查标准（都接受 NULL 节点，返回 0） */
int dsh_json_is_null(const dsh_json_node *node);
int dsh_json_is_bool(const dsh_json_node *node);
int dsh_json_is_number(const dsh_json_node *node);
int dsh_json_is_string(const dsh_json_node *node);
int dsh_json_is_array(const dsh_json_node *node);
int dsh_json_is_object(const dsh_json_node *node);

/* ── 取标量（种类不对时：布尔/数值返回 0 并**不**报错；字符串/数组/对象返回 NULL） */

/** 布尔值（不是布尔 → 0） */
int dsh_json_bool_value(const dsh_json_node *node);

/**
 * 取字符串。**连长度一起给**（字符串里可能有内嵌 U+0000）。
 *
 * @param out_len 出参，可为 NULL；给出的是**不含结尾 \0** 的字节数
 * @return 归**文档**所有的 UTF-8 缓冲（保证以 \0 结尾，所以也能当 C 字符串用 ——
 *         但那样会把 `\u0000` 之后的内容截掉，要真值请读 out_len）；
 *         种类不对（不是字符串）时返回 NULL
 */
const char *dsh_json_str_value(const dsh_json_node *node, size_t *out_len);
/**
 * 取整数。
 *
 * @param out 出参：值
 * @return 0 成功；非零失败（种类不对、或超出了 int64 能表示的范围）。
 *         超范围时**不返回截断值** —— 那会把一个错误变成一个更小的错误。
 */
int dsh_json_i64_value(const dsh_json_node *node, int64_t *out);

/** 取浮点数（种类不对返回非零）。用 strtod 的语义，但**受 locale 影响的那一步已隔离** */
int dsh_json_double_value(const dsh_json_node *node, double *out);

/* ── 取容器 ─────────────────────────────────────────────────────────────── */

/** 数组长度（不是数组 → -1；空数组 → 0） */
int64_t dsh_json_array_len(const dsh_json_node *node);

/** 数组第 index 项（越界或不是数组 → NULL） */
const dsh_json_node *dsh_json_array_at(const dsh_json_node *node, int64_t index);

/** 对象成员个数（不是对象 → -1） */
int64_t dsh_json_object_len(const dsh_json_node *node);

/** 按名字取成员（不是对象、或没有这个成员 → NULL）。名字按**字节**比对，区分大小写 */
const dsh_json_node *dsh_json_object_get(const dsh_json_node *node, const char *name);

/** 按序号取成员（给"遍历所有字段"用；越界 → NULL）。用 key/out_key 取出名字 */
const dsh_json_node *dsh_json_object_at(const dsh_json_node *node, int64_t index,
                                        const char **out_key, size_t *out_key_len);

/**
 * 取一个节点的**原文片段**（零拷贝，指向当初喂进来的那段文本）。
 *
 * 为什么需要它：`dsh_engine_settings_set` 收的是**局部补丁**，而设置里有些字段
 * 内核**不认识**（参考实现那份模型一直在长：窗口位置、历史、翻译页……）。
 * 让内核把它们"读进来、再原样写回去"是**避免数据丢失**的唯一办法 ——
 * 若内核按自己的理解重写一遍，不认识的字段会在第一次存盘时**不报错地消失**。
 * 原文片段让这件事变成一次 `memcpy`，不需要内核对它有任何理解。
 *
 * @param out_len 出参：片段字节数
 * @return 指向输入文本的只读指针（**归调用方当初喂的那段文本所有，不许释放**）；
 *         node 为 NULL 时返回 NULL。
 *
 * ⚠️ 生存期：片段指向的是**输入文本**，所以"喂进来的那段文本被释放之后"它就不安全了。
 *    本层不改写输入文本，能保证的是"文档还活着时它没被本层动过"。
 */
const char *dsh_json_node_raw(const dsh_json_node *node, size_t *out_len);

#endif /* DSH_JSON_READER_H */
