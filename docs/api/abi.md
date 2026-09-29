# 内核接口定义（ABI v2 · dsh_lookup 0.2.1）

> 本文件由 `tools/gen-bindings.mjs` 从 [`abi/lookup.abi.json`](../../abi/lookup.abi.json) 生成。**不要手改。**

唯一的接口定义文件。C 头文件 / C# P/Invoke / TypeScript 类型 / 接口定义文档全部由本文件生成，禁止手改产物。改动本文件后跑 `node tools/gen-bindings.mjs`。

## 约定

- **内存**：内核返回的每一块内存都由内核分配；宿主一律用 dsh_release() 还给内核，禁止 free()/FreeHGlobal。
- **错误**：所有可能失败的接口返回 dsh_error；失败时输出参数保持不变（宿主初始化过的初值原样留着）。
- **字符串**：utf-8，以 `\0` 结尾。
- **调用约定**：cdecl。

## 常量

| 常量 | 值 | 说明 |
| --- | --- | --- |
| `DSH_MAX_LIST_ROWS` | 8 | 候选列表最多画几行。参考实现里这个数抄在三个文件里（那个坑），现在只有一处。 |
| `DSH_SELECTION_WORD_MAX` | 4 | 选中文本按词计数的阈值：≤4 个词就写词名，≥5 个词说『所选文本』。 |
| `DSH_HISTORY_LIMIT` | 5000 | 查词历史上限（最多保留多少条）。 |
| `DSH_HISTORY_PAGE` | 60 | 历史下拉一次取多少条（滚到底再取下一页）。 |
| `DSH_PROBE_BUDGET_MS` | 120 | 借查探路的单本预算（毫秒）。超预算算『没问完』，绝不许并进『没有』。 |
| `DSH_SPEECH_CHUNK_CHARS` | 300 | 长文本朗读每段的目标字数（优先在句末断开）。 |
| `DSH_AUDIOCACHE_MEM_BYTES` | 12582912 | 发音缓存的内存上限（12 MB）。 |
| `DSH_AUDIOCACHE_DISK_BYTES` | 67108864 | 发音缓存的磁盘上限（64 MB），超了按最久没用过的先删。 |

### 音节分隔点

音节分隔点：词典把词头写成 dic·tion·ar·y 时要能忽略掉。故意不含连字符 —— 连字符是合法词条字符（well-known）。参考实现里这份清单在 C# 与 main.ts 各有一份（那个坑），现在只有一处。⚠️ 2026-09 已定往里加了两个**重音符**（U+02C8 / U+02CC，见各自的注释）：这两个码点参考实现里没有，是本版有意超出参考实现的一处（参考实现那边只认前四个） —— 检查标准与沿革 /。

| 常量 | 码点 | 字符 | 说明 |
| --- | --- | --- | --- |
| `DSH_SEP_MIDDLE_DOT` | U+00B7 | `·` | U+00B7 MIDDLE DOT（LDOCE 那种写法） |
| `DSH_SEP_HYPHENATION_POINT` | U+2027 | `‧` | U+2027 HYPHENATION POINT |
| `DSH_SEP_KATAKANA_MIDDLE_DOT` | U+30FB | `・` | U+30FB KATAKANA MIDDLE DOT |
| `DSH_SEP_SOFT_HYPHEN` | U+00AD | `­` | U+00AD SOFT HYPHEN（复制粘贴会带出来） |
| `DSH_SEP_PRIMARY_STRESS` | U+02C8 | `ˈ` | U+02C8 MODIFIER LETTER VERTICAL LINE（主重音，用户 2026-09 点名要的那个符号）：词典把词头写成 ˈæpl 时，它**永远不是词条名的一部分** —— 只在『原样问不到』的第二遍里被去掉，所以加了它不会误伤任何真实词条 |
| `DSH_SEP_SECONDARY_STRESS` | U+02CC | `ˌ` | U+02CC MODIFIER LETTER LOW VERTICAL LINE（次重音）：同 U+02C8 一条约定，两个一起加（只加一个等于留一半的坑） |

## 枚举

### `dsh_error`

错误码。注意 DSH_E_NOT_FOUND 是『文件不存在 / 不是 MDict』，与『词典里没有这个词条』是两件事 —— 后者是成功返回、结果里 found=false。

| 取值 | 值 | 说明 |
| --- | --- | --- |
| `DSH_OK` | 0 | 成功 |
| `DSH_E_INVALID_ARG` | -1 | 参数不合法（空指针 / 空词 / 越界） |
| `DSH_E_NOT_FOUND` | -2 | 文件不存在或不是 .mdx |
| `DSH_E_FORMAT` | -3 | 能打开但不是合法 MDict |
| `DSH_E_IO` | -4 | 读写失败 |
| `DSH_E_OOM` | -5 | 内存不足 |
| `DSH_E_STATE` | -6 | 句柄状态不对（未加载就查、已释放还再用） |
| `DSH_E_BUSY` | -7 | 同一句柄上有并发调用（内核多数对象不是线程安全的） |
| `DSH_E_NOT_IMPLEMENTED` | -8 | 这一版还没接（如实报，不许装作成功） |

### `dsh_origin`

一次查询从哪条入口来。只有 input / selection 允许跑兜底通道 —— link / back / history 一律不跑，否则『退回一个查不到的词』会当场跳走（参考实现那个坑D11）。

| 取值 | 值 | 说明 |
| --- | --- | --- |
| `DSH_ORIGIN_INPUT` | 0 | 输入框回车 / 查找按钮 |
| `DSH_ORIGIN_SELECTION` | 1 | 正文里选中文字 |
| `DSH_ORIGIN_LINK` | 2 | 词条里的 entry:// 链接（不跑通道） |
| `DSH_ORIGIN_BACK` | 3 | 返回上一词条（不跑通道） |
| `DSH_ORIGIN_HISTORY` | 4 | 从查词历史回放（不跑通道） |

### `dsh_stage`

通道走到哪一步了。界面据此显示进度；主进程每推进一步就回一个 stage。

| 取值 | 值 | 说明 |
| --- | --- | --- |
| `DSH_STAGE_START` | 0 | 还没开始 |
| `DSH_STAGE_AFTER_LOOKUP` | 1 | 在当前词典查过了 |
| `DSH_STAGE_AFTER_PROBE` | 2 | 问过别的词典了 |
| `DSH_STAGE_AFTER_SUGGEST` | 3 | 联想候选摆出来了 |
| `DSH_STAGE_DONE` | 4 | 走到链的尽头了 |

### `dsh_surface`

候选摆在哪儿。

| 取值 | 值 | 说明 |
| --- | --- | --- |
| `DSH_SURFACE_NONE` | 0 |  |
| `DSH_SURFACE_LIST` | 1 | 输入框下方列表 |
| `DSH_SURFACE_TOAST` | 2 | 正文框底部提示（选区那一路） |

### `dsh_chip_action`

词条页底部的出路按钮。**按钮上那行字也由内核给**（见 EntryPayload 的 chips[]）—— 文案是产品约定，不是视图。

| 取值 | 值 | 说明 |
| --- | --- | --- |
| `DSH_CHIP_BORROW` | 0 | 用《X》查（借查，不切当前词典）。⚠️ 2026-09 起**这一档永不出现**：借查已经是通道里的自动一步，不该再让用户点一下去做同一件事（参考实现的 D1）。值留着不动枚举编号。 |
| `DSH_CHIP_RECHECK` | 1 | 再问一遍（有没问完的词典时必须给） |
| `DSH_CHIP_TRANSLATE` | 2 | 翻译这个词 |

### `dsh_script`

主导字形。语种判定的第一步只看这个 ——『什么算汉字』只有一处来源。

| 取值 | 值 | 说明 |
| --- | --- | --- |
| `DSH_SCRIPT_HAN` | 0 |  |
| `DSH_SCRIPT_LATIN` | 1 |  |
| `DSH_SCRIPT_OTHER` | 2 |  |

### `dsh_audio_source`

三层音源。**这三个值只是音源的标识**：0 / 1 / 2 是取值编号，**取值顺序（词典 → 系统 → 在线）不是优先级顺序**。优先级由内核定（产品约定）：**词典自带原录音 → 在线 → 系统离线**，界面不许自己排、也不许自己判（见 dsh_speech_plan 那条）。

| 取值 | 值 | 说明 |
| --- | --- | --- |
| `DSH_AUDIO_DICT` | 0 | 词典自带原录音（.mdd） |
| `DSH_AUDIO_SYSTEM` | 1 | 系统语音（离线合成） |
| `DSH_AUDIO_ONLINE` | 2 | 在线发音（豆包 · 单向流式；要自备凭据 —— 没填 Key 就用不上，没有单独的开关） |

## 接口

### 核心

#### `dsh_version`

内核版本字符串（如 0.2.0）。宿主启动时打日志、并在握手时与接口定义版本对账。

```c
const char * dsh_version(void);
```

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

#### `dsh_abi_version`

ABI 整数版本。宿主启动时对不上就应当拒绝启动并给出可读原因，别硬跑。

```c
int32_t dsh_abi_version(void);
```

ABI v1 起可用。

#### `dsh_release`

释放内核返回的任意内存块。传 NULL 是合法的空操作。宿主禁止用 free() 释放内核给的东西；传进来的若不是内核分配的，内核会安全拒绝并记下原因（不崩）。

```c
void dsh_release(void *ptr);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `ptr` | `opaque` | 入 | 内核返回过的指针 |

ABI v1 起可用。

#### `dsh_last_error_message`

本线程上最近一次失败的人话说明（UTF-8）。没有失败时返回空串。用于日志与『检测凭据』那种要如实说原因的场合。⚠️ 例外（约定，见 dsh_textcodec.h）：解码类操作遇到坏字节/长度不符等『不算失败』的问题时**返回 0 但把诊断记在这里**（GB18030 四字节区、JSON 字符串坏 UTF-8、zlib 长度不符各一处）—— 所以宿主**不许**在返回值非零之外把 last_error 当『必然出过错』的检查标准，想知道有没有失败看返回值。

```c
const char * dsh_last_error_message(void);
```

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

### 引擎

#### `dsh_engine_create`

建一个引擎。userDataDir 是配置 / 缓存 / 索引的根目录（Windows 上通常是 %APPDATA%\LookupApp）。传 NULL 用内核的默认值。

```c
enum dsh_error dsh_engine_create(const char *user_data_dir, dsh_engine **out_engine);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `user_data_dir` | `utf8` | 入 | 配置目录，UTF-8；NULL = 默认 |
| `out_engine` | `engine` | 出 | 成功时写入句柄 |

调用方用 `dsh_engine_destroy()` 关掉句柄。 ABI v1 起可用。

#### `dsh_engine_destroy`

销毁引擎，释放它持有的一切（含所有已加载词典）。传 NULL 是合法的空操作。

```c
void dsh_engine_destroy(dsh_engine *engine);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `engine` | `engine` | 入 | 要销毁的句柄 |

ABI v1 起可用。

#### `dsh_engine_settings_get`

取全部设置（JSON）。设置模型住内核里，界面不许自己拼一份、更不许拿内存里的设置回传给内核。

```c
enum dsh_error dsh_engine_settings_get(dsh_engine *engine, char **out_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `engine` | `engine` | 入 |  |
| `out_json` | `json` | 出 | 设置 JSON |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

#### `dsh_engine_settings_set`

改设置（局部补丁 JSON）。内核负责规范化（越界的语速、不认识的音色 ID、非法的枚举值都要被夹回合法范围），并负责落盘。返回规范化之后的完整设置。

```c
enum dsh_error dsh_engine_settings_set(dsh_engine *engine, const char *patch_json, char **out_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `engine` | `engine` | 入 |  |
| `patch_json` | `utf8` | 入 | 只写要改的字段 |
| `out_json` | `json` | 出 | 规范化之后的完整设置 |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

#### `dsh_engine_dict_list`

词库清单（JSON 数组）。每一项含 id / 显示名 / 路径 / 是否已加载 / 资源卷情况，以及 ★ `unavailable`（`""` / `"missing"`）与 `note`（**界面原样显示**的那句人话，可用时是空串）—— 「这一本现在还能不能用、不能时怎么恢复」由内核说（与 `dsh_history_query` 每一行**同一处实现**），界面不许自己拼那句话。「已被移出词库」那一档这里看不到（它压根不在清单里）：那一种由历史每一行的 `unavailable=removed` 说。id 是内容哈希，不是路径（0.2.0 的决定）——所以重命名文件、换目录都不会丢设置。

```c
enum dsh_error dsh_engine_dict_list(dsh_engine *engine, char **out_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `engine` | `engine` | 入 |  |
| `out_json` | `json` | 出 |  |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

#### `dsh_engine_dict_add`

导入若干 .mdx。paths_json 是 UTF-8 路径数组。返回 {added, failed:[{path,reason}]}。⚠️ 不存在的路径必须进 failed 并写明原因，绝不许记成 added（参考实现那个坑的形态）。

```c
enum dsh_error dsh_engine_dict_add(dsh_engine *engine, const char *paths_json, char **out_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `engine` | `engine` | 入 |  |
| `paths_json` | `utf8` | 入 | ["C:\\词典\\a.mdx", …] |
| `out_json` | `json` | 出 | {added, failed} |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

#### `dsh_engine_dict_remove`

从词库移除（不动硬盘文件）。

```c
enum dsh_error dsh_engine_dict_remove(dsh_engine *engine, const char *dict_id, char **out_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `engine` | `engine` | 入 |  |
| `dict_id` | `utf8` | 入 |  |
| `out_json` | `json` | 出 | 移除之后的清单 |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

#### `dsh_engine_dict_rename`

给词典起个别名。空串 = 恢复默认名。词典名的合成约定只在内核一处：改过的名 → .mdx 头里的书名 → 文件名。

```c
enum dsh_error dsh_engine_dict_rename(dsh_engine *engine, const char *dict_id, const char *name, char **out_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `engine` | `engine` | 入 |  |
| `dict_id` | `utf8` | 入 |  |
| `name` | `utf8` | 入 | 新名；空串 = 恢复默认 |
| `out_json` | `json` | 出 | 改名之后的词库清单 |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

#### `dsh_engine_dict_move`

把一本词典在清单里挪 delta 位（负数往前、正数往后）。★ 2026-09 新加（ABI v2），**参考实现（参考实现）没有这个功能**，语义由本文件定义：到边界就**夹住**（第一本再往前 / 最后一本再往后 = 清单原样返回，仍算成功 —— 与界面上那两颗按钮在头尾置灰是一套）；delta 为 0 也是原样返回；id 不在清单里 → 报错（与 dict_rename 同一条）。当前词典**不受影响**（它记的是 id 不是下标）。⚠️ 顺序**不只是显示顺序**：「当前词典」那格为空时，查词 / 发音样本 / 增益等若干接口会**兜底取第一本**，所以把某一本挪到第一位会真的改变兜底行为。

```c
enum dsh_error dsh_engine_dict_move(dsh_engine *engine, const char *dict_id, int32_t delta, char **out_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `engine` | `engine` | 入 |  |
| `dict_id` | `utf8` | 入 |  |
| `delta` | `i32` | 入 | 负数往前挪、正数往后挪；0 = 不动 |
| `out_json` | `json` | 出 | 移动之后的词库清单 |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v2 起可用。

#### `dsh_engine_dict_set_current`

指定当前词典。

```c
enum dsh_error dsh_engine_dict_set_current(dsh_engine *engine, const char *dict_id, char **out_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `engine` | `engine` | 入 |  |
| `dict_id` | `utf8` | 入 |  |
| `out_json` | `json` | 出 | 词库清单 |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

### 查词

#### `dsh_engine_resolve`

问落点：这段文字在指定词典里会落到哪条词条。只查表、不落盘、不写历史、不常驻。用于『动手前先问落点』那种判定（参考实现那个坑：用字符串预测点下去的后果必错）。

```c
enum dsh_error dsh_engine_resolve(dsh_engine *engine, const char *dict_id, const char *text, char **out_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `engine` | `engine` | 入 |  |
| `dict_id` | `utf8` | 入 | 哪一本；NULL/空 = 当前词典 |
| `text` | `utf8` | 入 | 要问的文字（内核先 trim，再按词典规范键名给落点） |
| `out_json` | `json` | 出 | {"landed":"apple"|null,"dictId":"…","dictTitle":"…","enabled":bool,"reason":"…"} |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

#### `dsh_engine_suggest`

联想候选。三步由内核定：精确命中 → 前缀补全 → 拼写纠正；单字符查询不做拼写纠正（参考实现那个坑：编辑距离 ≤1 对单字符恒真）。行数上限由内核的 DSH_MAX_LIST_ROWS 定。

```c
enum dsh_error dsh_engine_suggest(dsh_engine *engine, const char *text, char **out_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `engine` | `engine` | 入 |  |
| `text` | `utf8` | 入 | 查询文字 |
| `out_json` | `json` | 出 | [{"word":"apple","kind":"exact"|"prefix"|"fuzzy"|"associate"}] |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

#### `dsh_engine_lookup`

完整查询（一条链走到底）：当前词典 → 借查别本 → 机器翻译 → 终态页。origin 决定跑不跑这条链。返回值里既有正文数据，也有界面要照抄的整句话与出路按钮。

```c
enum dsh_error dsh_engine_lookup(dsh_engine *engine, const char *text, enum dsh_origin origin, const char *dict_id, char **out_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `engine` | `engine` | 入 |  |
| `text` | `utf8` | 入 | 要查的文字；NULL/空则改看 selection 的现值 |
| `origin` | `dsh_origin` | 入 | 从哪条入口来 |
| `dict_id` | `utf8` | 入 | 起点那本；NULL/空 = 当前词典 |
| `out_json` | `json` | 出 | EntryPayload 形状：{query,keyText,dictId,dictTitle,entryUrl,plainText,found,sameAsShown,linkedTo,via,unconfirmed[],reason,translateWhy,offerTranslate,offerRecheck,chips[],suggestions[],speakText,stage,surface}。`sameAsShown` 是**「落点就是界面正在显示的那条词条」**（只对 `origin = selection` / `link` 有意义，见 `docs/design/查词兜底通道与历史记录开发指导.md` C 第 2 条）：为真时 `entryUrl` **是空的**（没有要跳的地方）而 `reason` 是内核那句「「X」就是当前词条（Y）」—— 界面见它**一个字都不动正文**。检查标准是**解析之后的落点**与引擎记着的「上次交出去的那条」比（不是拿输入的文字比：`apples` 会重定向到 `apple`，字面上并不相等却是同一条）。chips[] 是**终态页那排出路按钮**（{action,label,hint,word}）—— 连按钮上那行字一起给，界面一个字都不拼。⚠️ **没命中（found:false）时 keyText 是查询词本身、entryUrl 指向那一本的提示页**（参考实现同约定）：提示页上那句「未在《…》中找到「…」」后面跟着的候选是可点的 entry:// 链接，那是全程序里 entry:// 链接唯一的来源。⚠️ `speakText` 是**这次朗读该念什么**（词典命中的那个词 / 查不到时是查询词 / 译文伪词条是译文 / **「没打完的半个词」那一档是 null**）—— 界面把它原样交给 `dsh_speech_plan`，**不许自己拼「keyText 否则 query」**（那是业务规则，参考实现里那几行已按硬规则 1 搬进内核）。 |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

#### `dsh_engine_probe`

问一句就走：在**别的**词典里查一次，答案拿回来，那本词典**不许**留在内存里（参考实现那个坑）。这条与『加载一本』必须分得开。

```c
enum dsh_error dsh_engine_probe(dsh_engine *engine, const char *text, const char *dict_id, int32_t budget_ms, char **out_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `engine` | `engine` | 入 |  |
| `text` | `utf8` | 入 |  |
| `dict_id` | `utf8` | 入 | 问哪一本 |
| `budget_ms` | `i32` | 入 | 单本预算；0 = 用内核默认（DSH_PROBE_BUDGET_MS） |
| `out_json` | `json` | 出 | {"status":"ok"|"missing"|"timeout"|"error","landed":"…"|null,"dictTitle":"…","reason":"…"}。⚠️ timeout/error 绝不许并成 missing。 |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

#### `dsh_engine_borrow`

借查：**去别的词典里问一遍**（终态页那条「再问一遍」）。参考实现是 `App.BorrowAsync`，这一份逐条照它的约定：① **跳过起点那一本**（入参为空 = 内核按『当前词典』取，『当前』空着时兜底第一本）；② 一本一本问、**问一句就走**（内部走 `dsh_engine_probe` → `dsh_dicts_borrow_once`，那本词典**不许**留在内存里）；③ **总预算**：自动那一次 300 ms（用户正等着看结果，不能一本一本串着问），`recheck` 时单本放大到 1500 ms、总预算 = 词长 × 1500 + 2000（参考实现同一公式 —— 用户刚点了按钮，他要的就是答案，可以等）；④ ★ **「没问完」绝不许并成「没有」**（那个坑）：预算用完 / `timeout` / `error` 的那几本进 `unconfirmed`（用**显示名**，界面直接列给人看），而 `missing`（这一本明确答没有）**不算没问完**。⚠️ 命中时**不加载**那一本，只回『在哪一本、落在哪条』—— 真正显示那一条由调用方带明确 dictId 再查一次（`dsh_engine_lookup`），参考实现也是这么分的。⚠️ 返回的 `asked`/`perDictMs`/`totalMs`/`elapsedMs` 是**实测结果**（诊断与排错要看它为什么没问完），界面不读。

```c
enum dsh_error dsh_engine_borrow(dsh_engine *engine, const char *text, const char *skip_dict_id, int32_t recheck, char **out_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `engine` | `engine` | 入 |  |
| `text` | `utf8` | 入 | 要问的词（不能为空/纯空白） |
| `skip_dict_id` | `utf8` | 入 | 跳过哪一本（可空 = 当前词典）；起点那一本已经答过『没有』，再问一遍它没有意义 |
| `recheck` | `i32` | 入 | 1 = 「再问一遍」（单本预算 1500 ms、总预算按词长折算）；0 = 自动那一次（单本 DSH_PROBE_BUDGET_MS、总预算 300 ms） |
| `out_json` | `json` | 出 | {hitId,hitTitle,hitLanded,unconfirmed[],asked,perDictMs,totalMs,elapsedMs}。hitId 空 + unconfirmed 空 = **确定别的词典里也没有**；unconfirmed 非空 = **没问完**（这两档绝不许混）。 |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

#### `dsh_engine_entry_document`

取一条词条的可渲染文档（HTML）与其资源。界面把它喂给沙箱 iframe。词条的 HTML 兼容性处理、.mdd 资源引用改写、sound:// 与 snd:// 的改写全在内核。

```c
enum dsh_error dsh_engine_entry_document(dsh_engine *engine, const char *dict_id, const char *key_text, char **out_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `engine` | `engine` | 入 |  |
| `dict_id` | `utf8` | 入 |  |
| `key_text` | `utf8` | 入 | 词典的规范键名（用 resolve 拿到的落点，不要传用户输入的写法） |
| `out_json` | `json` | 出 | {"html":"…","plainText":"…","resourceBase":"…","audioKeys":[…]} |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

#### `dsh_engine_resource`

按 key 从 .mdd 资源卷（或词典旁边散放的文件）取一段资源字节。支持 Range：offset/length 让宿主实现 206/416。

```c
enum dsh_error dsh_engine_resource(dsh_engine *engine, const char *dict_id, const char *key, size_t offset, size_t length, uint8_t **out_bytes, size_t *out_len, char **out_meta_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `engine` | `engine` | 入 |  |
| `dict_id` | `utf8` | 入 |  |
| `key` | `utf8` | 入 | 资源键名（如 \\style.css 或 sound 键） |
| `offset` | `usize` | 入 | 起始偏移（Range） |
| `length` | `usize` | 入 | 要多少字节；0 = 到末尾 |
| `out_bytes` | `u8ptr` | 出 | 内核分配，用 dsh_release 还给内核 |
| `out_len` | `usize` | 出 |  |
| `out_meta_json` | `json` | 出 | {"total":N,"mime":"…","etag":"…","found":bool,"reason":"…"} |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

### 文本

#### `dsh_text_analyze`

文本分析：字形（判语种第一步）、音节分隔点、去掉点之后的写法、按词计数。★ 这一条就是为了把 main.ts 里那三份重复实现收进内核（ADR-001）。

```c
enum dsh_error dsh_text_analyze(const char *text, char **out_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `text` | `utf8` | 入 |  |
| `out_json` | `json` | 出 | {"script":"han"|"latin"|"other","hasSeparatorDots":bool,"withoutSeparatorDots":"…","wordCount":N,"isSingleChar":bool} |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

#### `dsh_text_strip_separators`

只做『去掉音节分隔点』这一件事。⚠️ 调用方必须遵守参考实现那个坑/38 的顺序：先拿原样文本问一次，问不到才用这里的返回值重问 —— 内核不替你改语义（ResolveKey 永远只认原样文本）。

```c
enum dsh_error dsh_text_strip_separators(const char *text, char **out_text);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `text` | `utf8` | 入 |  |
| `out_text` | `utf8` | 出 | 去掉分隔点之后的写法；整段只有点 → 空串 |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

#### `dsh_language_detect`

判语种：字形 → 词典标题 → 用户设的默认语种。返回判定结果**与检查标准说明**（界面提示里要写『英语（按字形判断（拉丁字母））』，那句话由内核给，界面不拼）。

```c
enum dsh_error dsh_language_detect(dsh_engine *engine, const char *text, const char *dict_id, char **out_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `engine` | `engine` | 入 |  |
| `text` | `utf8` | 入 |  |
| `dict_id` | `utf8` | 入 | 用哪本的标题做线索；NULL = 当前词典 |
| `out_json` | `json` | 出 | {"language":"en","languageLabel":"英语","basis":"glyph"|"title"|"default","basisText":"按字形判断（拉丁字母）","explanation":"英语（按字形判断（拉丁字母））"} |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

#### `dsh_language_label`

语种码 → **中文名**（`"en"` → `"英语"`）。★ 这张表是**判定语种用的同一张表**（在 `text/dsh_language.c` 里），所以界面要显示语种名时**必须问内核**，不许在壳或界面里再存一份 —— 两处各存一份迟早会对不上（参考实现的注释专门点过这一条）。⚠️ 认不出的码回**空串**（不是拿码当名字），调用方自己决定怎么显示。

```c
enum dsh_error dsh_language_label(const char *code, char **out_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `code` | `utf8` | 入 | 语种码或区域标记（`en` / `en-US` / `zh` …） |
| `out_json` | `json` | 出 | {"code":"en","label":"英语","known":bool}；认不出时 label 是空串、known 为 false |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

### 发音

#### `dsh_speech_speaker_label`

★ 豆包音色 id → **界面上的说法**（参考实现的 `DoubaoSpeech.DescribeSpeaker` + `DefaultSpeakerEnName/ZhName`）：两个默认音色回官网名 `Dacey` / `Vivi`，**其余一律回 id 本身**（不是空串 —— 界面上总得有个能认的东西）。为什么这条要进接口定义、而不是壳里再抄一张表：音色 id 与它的展示名**是同一份产品数据**，抄到壳里就是「同一个东西两个来源」（那个坑），迟早一处改了另一处没改。谁在用：`speech:status` 的两个 `speakerEnName`/`speakerZhName`、`speech:testDoubao` 每一项的 `speakerName`、在线发音回包里的 `voiceName`（`voiceId` 那格才是 id），以及内核自己拼的那句「为什么走这条」（`chosen.detail` / `why` 里显示的是名字，不是 `en_female_dacey_uranus_bigtts` 这种长串）。

```c
enum dsh_error dsh_speech_speaker_label(const char *speaker, char **out_label);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `speaker` | `utf8` | 入 | 音色 id（可空；**大小写不敏感** —— 它是用户手打进去的） |
| `out_label` | `utf8` | 出 | 界面上的说法：默认那两个是 `Dacey` / `Vivi`，其余是 id 本身，空 id 是空串 |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

#### `dsh_speech_plan`

这次朗读怎么念：三层音源的排序与选择、挑哪个嗓子、要不要切段、切几段。★ 把 main.ts 的 speechPlan / splitForSpeech 收进内核（ADR-001）。顺序是**产品约定**（词典自带 → 在线 → 系统离线），界面不给选、也不许自己判。

```c
enum dsh_error dsh_speech_plan(dsh_engine *engine, const char *text, const char *dict_id, const char *voices_json, const char *overrides_json, char **out_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `engine` | `engine` | 入 |  |
| `text` | `utf8` | 入 | 要念的文本（词或整段）。⚠️ **空串是一档正经的情形**（界面上「还没查过词」）：三层一律不通、`disabledReason` 是「没有要念的文本。」—— 少了这一档，只关心「有没有这个语种的嗓子」的系统离线层会把空文本规划成可以点，按钮亮着却念不出东西（界面不许自己判这件事）。 |
| `dict_id` | `utf8` | 入 | 起点那本（挑原录音用）；NULL = 当前词典 |
| `voices_json` | `utf8` | 入 | ★ **本机装了哪些离线音色**，形如 `[{"id":"…","name":"…","language":"en","culture":"en-US"}]`。⚠️ 这一项**由宿主探测**（那是平台能力：Windows 上要问 System.Speech，跟读一个文件同级），**内核只负责判**：走哪一层、哪个语种配哪个嗓子、能不能点、不能点时那句人话。NULL / 空 = 探不到（系统离线那一层就如实报「问不到」）。 |
| `overrides_json` | `utf8` | 入 | ★ **这一次的覆盖** —— 参考实现的 `speak(text, options)` 那 9 个参数里真正影响「怎么念」的那几个，形如 `{"source":"online","voiceId":"…","language":"en"}`。规则只有两条：**键在 = 强制这个值**（`source` 在就**只许走这一层**，那一层不可用就如实报**它自己的**原因，**绝不不报错地回落到别的层**），**键不在 = 全套照设置**。NULL / 空 = 全套照设置（老调用方一个字都不用改）。谁在用：管理窗的「试听」与「平衡音量」—— 后者要拿 `loudness:0` / `gainDb:0` 量**中性电平**（不这么做的话，量到的电平会随着滑块自己变，算出来的补偿值来回震荡，见 `web/src/manager/main.ts` 的 `balanceDoubaoLoudness`）。⚠️ 响度覆盖是**整数**（接口定义没有浮点标量）；增益覆盖见 `dsh_speech_online_plan` 那一侧的同名参数与壳里的施加（增益不在内核里施加，见 `dsh_audio_apply_gain`）。⚠️ JSON 坏掉时**如实拒**（`DSH_E_INVALID_ARG` + last_error），不猜。 |
| `out_json` | `json` | 出 | {"source":"dict"|"system"|"online"|"none","enabled":bool,"chosen":{...},"chunks":[{"text":"…","chars":N}],"chunkCount":N,"language":"en","why":"…","disabledReason":"…","options":[{"source":"…","available":bool,"label":"…","detail":"…","reason":"…"}]}。`chosen` 按层给不同的键：dict → `audioKey`/`accent`/**`dictId`**（这段录音住在哪一本 —— 壳要靠它拼 `https://<词典 id>.dictres.invalid/__sound__/<键名>`，而入参为空时内核走的是「当前词典」，壳自己推不出来）/`label`；online → `speaker`/`language`/**`loudness`**（这一次用的响度补偿，覆盖优先）；system → `voiceId`/`voiceName`/`language`/**`rate`**（念多快，-10..10，**已由内核夹好** —— 壳只把它交给本机语音引擎，不许自己去翻设置）。`options` 是**多出来的一个键**（接口定义那张形状没列它，与 `audio_prepare` 的 `playable` 同一个先例）：三层各自为什么通/不通要说清楚，设置页与悬停提示都要用。 |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

#### `dsh_audio_apply_gain`

把一段 WAV 里的 16 位 PCM 按 dB 缩放（参考实现的 `GainMath.ApplyToWav`）。⚠️ 这是**系统离线语音那条路**的音量：参考实现就是在合成完、把字节交给播放器之前缩的（在『产字节』那一侧施加），所以那条路回包里的 `gainDb` **恒为 0** —— 前端再乘一次就是叠两遍。三条刻意的约定：① **顺序扫 RIFF 块**找 `fmt ` 与 `data`（不假设 44 字节头、也不假设只有一段 data —— SAPI 会插 `LIST`/`fact` 块，写死 44 会让增益『时灵时不灵』且错得隐蔽）；② **只动 16 位 PCM**，别的格式（8/24/32 位、浮点、压缩）**原样返回并说明**，不猜着改；③ **削顶不能不报错地** —— 想提的比峰值允许的多就夹回去，并把『从多少夹到多少、为什么』通过 `note` 说出去（悄悄削波听起来是破音，而用户没有任何线索去查）。⚙ 削顶保护的算法：`applied = min(请求值, 20·log10(1/峰值))`，再**向下**取整到 0.1 dB（宁可轻一点，也不要因为取整把峰值顶出满刻度）；**0 dB 一个字节都不动**（默认路径必须与『没有这个功能』逐字节相同）。⚠️ 传进来的 dB 是**已经归一化过**的（夹到 [-24,+12]、取整到 0.1 —— 那是设置层的活，见 `dsh_speech_gains`/`dsh_speech_gains_set`）。⚠️ 为什么这个参数是**整数**（dB×10）而不是浮点：接口定义里没有浮点标量类型，而增益在设置那一层本来就取整到 0.1 dB —— 传 dB×10 是**精确**的，也省掉一次跨 ABI 的浮点往返。

```c
enum dsh_error dsh_audio_apply_gain(const uint8_t *bytes, size_t len, int32_t gain_tenths_db, uint8_t **out_bytes, size_t *out_len, char **out_meta_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `bytes` | `u8ptr` | 入 | 整段 WAV 字节（宿主拥有；内核不改它） |
| `len` | `usize` | 入 |  |
| `gain_tenths_db` | `i32` | 入 | 要施加的增益，单位 **0.1 dB**（4.5 dB 就传 45） |
| `out_bytes` | `u8ptr` | 出 | 缩放之后的整段字节（内核分配，宿主用 dsh_release 还给内核）。**改了没改都照给**：没改时是与入参逐字节相同的一份 |
| `out_len` | `usize` | 出 |  |
| `out_meta_json` | `json` | 出 | {"changed":bool,"appliedDb":number,"note":"…"}。changed=false 时 appliedDb 是 0、note 说明为什么没动（太短/不是 RIFF-WAVE/不是 16 位 PCM/没有 data 块/增益不是有限值/被峰值夹到 0） |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

#### `dsh_speech_dict_audio`

这条词条自带的原录音在哪：按『常见扩展名优先、原始键垫底』在多卷 .mdd 里找，返回键名与可播放格式。例句录音**不算**词目发音（参考实现的硬约定）。

```c
enum dsh_error dsh_speech_dict_audio(dsh_engine *engine, const char *dict_id, const char *key_text, char **out_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `engine` | `engine` | 入 |  |
| `dict_id` | `utf8` | 入 |  |
| `key_text` | `utf8` | 入 | 词典规范键名 |
| `out_json` | `json` | 出 | {"found":bool,"audioKey":"…","kind":"entry"|"sentence"|"none","accent":"uk"|"us"|"","reason":"…"}。⚠️ accent 的约定跟**设置里那个口音偏好**同一套词（uk / us）—— 参考实现的 ClassifyAccent 就是这么给的，界面拿它跟设置里的值直接比。 |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

#### `dsh_speech_dict_samples`

从一本词典里挑几条**真的有录音**的词条（参考实现的 `App.DictSamplesAsync`）。⚙ 挑法逐条照参考实现：① **均匀撒网** —— 按**整本书的词条序号**分若干段、每段取开头那一条（⚠️ 单位是**词条**不是**词块**：按块取的话，一本只有 1 块的词典只能给出 1 个候选 —— 真词典 正是 1 块 9 条，实测踩过）；② 不够 6 条时**按索引顺序兜底补扫**（均匀撒点是一张网，网眼之间可能正好漏掉『录音集中在某一段』的词典）；③ 每条都**真解词条正文**去找录音，而且**只认词目发音**（例句不算 —— 量的是『点发音按钮会听到的那一段』，所以挑法与真正发音时完全一致：同一个 `dsh_speech_dict_audio`）；④ 三个上限：6 条 / 最多扫 60 个候选 / **最多 400 ms**（一次『解词条正文 + 到 .mdd 里找文件』实测十几毫秒，无上限地扫就是拿调用方的一次点击去跑后台任务）。⚠️ `ok=false` 时 message 是**三档不同的人话**：一本词典都没有 / 这本词典没有资源卷（.mdd）/ 有资源卷却扫不到（多半是音频卷没关联上）；扫不满 6 条但有一条以上时 `ok=true` 且 message 说清『只找到 N 条』。⚠️ 返回的是 **`audioKey`**（`.mdd` 里的键名），**可播地址由外壳拼**（`https://<外壳域>/__sound__/<词典 id>/<键名>`）—— 地址是平台形状，与 `dsh_speech_plan` 的 dict 那一层同一条规矩。⚠️ 今天**没有产品流程调它**（界面上的「平衡音量」按按需求删掉了，参考实现亦然）：接口、外壳接线与 B/标准都留着备用，**别看到『没人调』就删**。

```c
enum dsh_error dsh_speech_dict_samples(dsh_engine *engine, const char *dict_id, char **out_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `engine` | `engine` | 入 |  |
| `dict_id` | `utf8` | 入 | 问哪一本（可空 = 当前词典；『当前』这个字段空着时**兜底第一本**，与 dsh_speech_gains 同一条约定） |
| `out_json` | `json` | 出 | {ok,message,dictId,dictTitle,samples:[{word,audioKey,language,languageLabel}],count,scanned,elapsedMs,budgetHit} |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

#### `dsh_speech_online_plan`

在线语音（豆包 · 单向流式 HTTP）的**第一步：内核说该发什么**。三层音源里在线那一层的判断全在这儿：有没有配凭据与音色、这次该用哪个音色（**中英混排必须走中文音色** —— 实测拿英文音色念混排会得到空句子）、模型版本与音色配不配套（`seed-tts-2.0` / `seed-tts-1.0`）、请求体长什么样、四个头是什么。⚠️ 请求体里**刻意不写 `explicit_language`**：它的语义是"只念这个语种"，而词典正文中英混排是常态（见 docs/design/豆包语音合成接入方案.md §6.1 的实测）。⚠️ ok=false 时 reason 是人话，三种原因分开说（没填 Key / 没配音色 / 文本是空的）。

```c
enum dsh_error dsh_speech_online_plan(dsh_engine *engine, const char *text, const char *dict_id, const char *overrides_json, char **out_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `engine` | `engine` | 入 |  |
| `text` | `utf8` | 入 | 要念的文本 |
| `dict_id` | `utf8` | 入 | 当前词典（可空）。语种判定用得上它的标题那条线索 |
| `overrides_json` | `utf8` | 入 | ★ **这一次的覆盖**，与 `dsh_speech_plan` 那个同名参数**同一份东西**（壳把同一串原样传给两条接口 —— 两条必须看到同一份覆盖，否则"规划说走在线、真发请求时却换了音色"）。这一条只吃其中两个键：`voiceId`（覆盖音色 —— 界面上正在试听/正在量的那个）与 `loudness`（覆盖响度补偿）。NULL / 空 = 照设置。⚠️ 未知键一律忽略（向前兼容：将来加了键，老的壳不会因为多传一个键就失败）；JSON 坏掉才是错误。 |
| `out_json` | `json` | 出 | {ok,needsHttp,url,headers:[[名字,值],…],body,speaker,resourceId,language,mixed,reason}。needsHttp 为真时外壳把 url/headers/body 原样发出去、整段回包交给 _accept；为假时把这一份原样交回界面（reason 是人话） |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

#### `dsh_speech_online_test_plan`

在线语音的**检测凭据**那一条：管理窗「检测凭据」要发的那几次请求，该发什么。与 dsh_speech_online_plan 发的是**同一种东西**（同一份拼请求的代码），差别只在「念什么词、用哪个音色」由谁定 —— 这里由**界面正在填的那两个音色**定（用户改了 ID 还没写盘时，要测的必须是「即将存下去的那个」，测设置里存着的旧配置等于没测）。⚙ 逐项约定：① 给了 speaker 就只测它一项，语种用给的那个（认不出按 en），念的词从**内核自带的样本词表**取（`apple` / `苹果` …，与参考实现的 SampleWord 逐条相同）；② 没给 speaker 就**英文 + 中文各测一次**（两个音色的 Key / Resource 配套关系一样，但音色 ID 写错只有实测才发现）；③ 音色是空的 → 那一项 ok=false 且 reason 是「这个音色没填（这一项测不了）」——**「没东西可测」必须与「服务端说它不能用」分开说**，界面的判定（web/src/manager/main.ts 的 classifyDoubaoTest）就吃这一条；④ 没填 Key → 每一项的 reason 都是那句凭据提示。⚠️ 它**不受「在线总开关」限制**（先测通了再打开它，参考实现同一约定），但会**真联网**、按字符计费，所以只由用户点按钮触发。⚠️ 外壳在每一项上再补上 HTTP 的结果（statusCode/bytes/mime/elapsedMs/billedWords/error）与 speakerName —— 后者是**界面上的说法**（`Dacey` / `Vivi`；认不出就是 id 本身），由壳问 `dsh_speech_speaker_label` 得到（参考实现的 `DescribeSpeaker`），**壳里不许再抄一张表**（那个坑）。

```c
enum dsh_error dsh_speech_online_test_plan(dsh_engine *engine, const char *speaker, const char *language, char **out_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `engine` | `engine` | 入 |  |
| `speaker` | `utf8` | 入 | 要测的音色（**界面正在填的那个**，可空）。为空 = 按设置里的英文 + 中文两个各测一次 |
| `language` | `utf8` | 入 | 给 speaker 时用它的语种（决定念哪个样本词；认不出按 en）。speaker 为空时**不看它**（两项各自是 en / zh） |
| `out_json` | `json` | 出 | {ok,plans:[{ok,needsHttp,speaker,language,text,explicitLanguage,reason,url?,headers?,body?,resourceId?}]}。plans 是**逐项**的：needsHttp 为真时外壳把 url/headers/body 原样发出去、回包交给 dsh_speech_online_accept；为假时 reason 是人话（那一项测不了的原因） |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

#### `dsh_speech_online_accept`

在线语音的**第二步：内核说回包是什么意思**。外壳把 plan 给的东西原样发出去，把 **HTTP 状态、整段回包正文、耗时**交回来 —— 解析 SSE 的 `data:` 行、把每段 base64 顺序拼成音频、错误码翻成人话，全在这里。⚠️ 回包是 **SSE（`data:` 行）**，所以外壳要**整段缓冲**再交进来，不许自己边收边解析。⚠️ **`code 0` 不等于成功**：实测"英文音色念中英混排"就是 code 0 + 零字节音频，所以"音频是空的"必须单独判成失败，且那句话要点出原因。

```c
enum dsh_error dsh_speech_online_accept(dsh_engine *engine, const char *plan_json, int32_t http_status, const char *response_body, int32_t elapsed_ms, uint8_t **out_bytes, size_t *out_len, char **out_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `engine` | `engine` | 入 |  |
| `plan_json` | `utf8` | 入 | dsh_speech_online_plan 给的那份，原样交回来 |
| `http_status` | `i32` | 入 | HTTP 状态码；0 = 根本没发出去（断网 / 超时） |
| `response_body` | `utf8` | 入 | 整段回包正文（SSE 文本） |
| `elapsed_ms` | `i32` | 入 | 外壳量到的耗时（毫秒） |
| `out_bytes` | `u8ptr` | 出 | 合成出来的音频字节（mp3）；内核分配，宿主用 dsh_release 还给内核 |
| `out_len` | `usize` | 出 |  |
| `out_json` | `json` | 出 | {ok,mime,bytes,textWords,elapsedMs,message,speaker,language}；ok=false 时 out_bytes 为 NULL、message 是人话 |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

#### `dsh_speech_gains`

音量增益的现状（界面那两条滑块的值）。★ **增益是"按词典"存的**（`speech.dictGainDb`：一本一个数）+ **全局一个**（`speech.systemGainDb`）—— 不同词典的录音本来就录得不一样响。归一化约定（夹到 [-24, +12] dB、取整到 0.1）在**写**那一条里做，这里只报现状。⚠️ `dictAvailable` / `dictMessage` / `systemAvailable` / `systemMessage` 是**判断 + 两句产品文案**：能不能校准、不能校准时缺什么，只有内核手里有检查标准（界面一个字都不拼）。

```c
enum dsh_error dsh_speech_gains(dsh_engine *engine, const char *dict_id, int32_t voice_count, char **out_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `engine` | `engine` | 入 |  |
| `dict_id` | `utf8` | 入 | 问哪一本（可空 = 当前词典）；增益是按词典存的，所以这个参数决定读哪一格 |
| `voice_count` | `i32` | 入 | 本机装了几个离线音色（**平台事实**，由壳探好传进来 —— 与 `dsh_speech_plan` 的 voicesJson 同一条路：壳只报"有什么"，"这意味着什么"是内核判的） |
| `out_json` | `json` | 出 | VoiceGains：{dictId,dictTitle,dictGainDb,systemGainDb,dictAvailable,dictMessage,systemAvailable,systemMessage} |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

#### `dsh_speech_gains_set`

改音量增益，返回改完之后的那份视图（调用方拿它重排滑块，不必再问一次）。⚠️ 两个键都是"**可空数字**"，三种语义必须分清：**传数字** = 设成这个值（会被夹到 [-24, +12] 并取整到 0.1）；**传 null** = 清掉这一项（回到"没设过"= 0 增益）；**整个键不传** = 不改（前端只发它要改的那一项）。`dictGainDb` 只改**那一本词典**的数（增益按词典存）。

```c
enum dsh_error dsh_speech_gains_set(dsh_engine *engine, const char *dict_id, const char *patch_json, int32_t voice_count, char **out_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `engine` | `engine` | 入 |  |
| `dict_id` | `utf8` | 入 | 改哪一本的增益（可空 = 当前词典） |
| `patch_json` | `utf8` | 入 | {"systemGainDb":number|null,"dictGainDb":number|null}（缺哪个键就不改哪个） |
| `voice_count` | `i32` | 入 | 同 dsh_speech_gains：本机有几个离线音色（**平台事实**，壳探好传进来） |
| `out_json` | `json` | 出 | 改完之后的那份 VoiceGains |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

#### `dsh_audio_prepare`

把一段音频字节整成播放器能播的格式：格式按魔数嗅探、Speex(.spx) 就地解成 16bit PCM WAV、认不出来就**当场如实报错**（不许让播放器报错误码、更不许拿 TTS 假装顶上）。

```c
enum dsh_error dsh_audio_prepare(const uint8_t *bytes, size_t len, uint8_t **out_bytes, size_t *out_len, char **out_meta_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `bytes` | `u8ptr` | 入 | 原始字节（宿主拥有） |
| `len` | `usize` | 入 |  |
| `out_bytes` | `u8ptr` | 出 | 可直接播的字节；内核分配 |
| `out_len` | `usize` | 出 |  |
| `out_meta_json` | `json` | 出 | {"mime":"audio/wav","kind":"wav"|"mp3"|"spx","decoded":bool,"reason":"…","playable":bool}；`.spx` 解开时**多两个键** `samples` / `sampleRate`（解出来的采样数与播放速率，别的格式宿主自己能读） |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

### 历史

#### `dsh_history_query`

查词历史（分页）。上限与每页条数由内核常量定。0.2.0 起历史落在 `<配置目录>/history.jsonl`（一行一条 JSON 的追加文件），界面只按页取、不许自己算分页。

```c
enum dsh_error dsh_history_query(dsh_engine *engine, int32_t offset, int32_t limit, char **out_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `engine` | `engine` | 入 |  |
| `offset` | `i32` | 入 |  |
| `limit` | `i32` | 入 | 0 = 用内核默认（DSH_HISTORY_PAGE） |
| `out_json` | `json` | 出 | {"total":N,"items":[{"word":"…","dictId":"…","dictTitle":"…","at":1670000000,"unavailable":"","note":""}],"hasMore":bool}。★ 每一行多两个字段（第三十七轮按硬规则 1 从界面搬进来的）：`unavailable` = `""` / `"removed"`（那一本已经不在词库里）/ `"missing"`（在词库里、但文件不在了）；`note` 是**界面原样显示**的那句人话（能用时是空串）——「哪一本、为什么、怎么恢复」本来只有内核手里有检查标准，界面拼那句话等于把同一条规则实现第二遍。两种情形的两句话**不许合成一句**（原因不同、恢复办法也不同）。 |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

#### `dsh_history_clear`

清空查词历史。

```c
enum dsh_error dsh_history_clear(dsh_engine *engine, char **out_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `engine` | `engine` | 入 |  |
| `out_json` | `json` | 出 | {"total":0} |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

### 机器翻译

#### `dsh_translate_status`

机器翻译的设置与凭据状态（**只读设置、不联网、不花钱**）。界面靠它决定「能不能显示翻译」以及「缺 Key 该怎么说明」。字段与参考实现的 TranslateStatus 逐字相同 —— 连 endpoint / resourceId 两个常量也一起给（排错时一眼看出请求被指到哪儿去了；resourceId 恒为 volc.speech.mt，它要在控制台**单独开通**，与语音的 seed-tts-2.0 不是一回事）。⚠️ `enabled`（用户自己的开关）与 `hasApiKey`（凭据有没有）**必须分开判**：界面把两者合成一个 disabled，就会把「我自己关的」与「客观不可用」混成同一种灰，用户会反复点那个开关。

```c
enum dsh_error dsh_translate_status(dsh_engine *engine, char **out_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `engine` | `engine` | 入 |  |
| `out_json` | `json` | 出 | {"hasApiKey":bool,"enabled":bool,"targetMode":"auto"|"zh"|"en","autoTranslate":bool,"endpoint":"…","resourceId":"volc.speech.mt","cache":{"count":N,"hits":N,"misses":N}} |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

#### `dsh_translate_plan`

机器翻译的**第一步：内核说该发什么**。内核零依赖、没有 socket，所以「把字节发出去」是平台层的活；但**判断一个字都不许留在平台层** —— 译成哪个语种、source 传不传、请求体长什么样、端点与三个头、这门语言支不支持，全在这里定。这一步的产物交给外壳原样发出去，回包再交给 dsh_translate_accept。⚠️ **命中缓存时不必发请求**：ok=true 且 cached=true，translation 就是译文。⚠️ ok=false 时 `message` 是人话，而且**四种原因分开说**（开关关着 / 没填 Key / 这门语言 MT 不支持 / 文本是空的）—— 合并成一句会让用户去改错的地方。

```c
enum dsh_error dsh_translate_plan(dsh_engine *engine, const char *text, const char *dict_title, char **out_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `engine` | `engine` | 入 |  |
| `text` | `utf8` | 入 | 要翻的文本 |
| `dict_title` | `utf8` | 入 | 当前词典的显示名（可空）。语种判定用得上它 —— 「牛津高阶英汉双解」这条线索服务端没有，别丢 |
| `out_json` | `json` | 出 | {ok,needsHttp,cached,url,headers:[[名字,值],…],body,text,sourceLanguage,targetLanguage,sourceLabel,targetLabel,translation,detected,message,tokens,cachedItems,fetchedItems,elapsedMs,why}。★ **外壳那一条路只有一句判断**：`needsHttp` 为真就把它给的 url/headers/body 原样发出去、回包交给 dsh_translate_accept；为假就**把这一份原样交回界面** —— 失败那一档与"命中缓存"那一档回的**都是界面那份 TranslateResult 的形状**（`message` 是人话/空串），所以外壳既不需要会拼载荷、也不需要认识失败的原因。 |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

#### `dsh_translate_accept`

机器翻译的**第二步：内核说回包是什么意思**。外壳把 plan 给的东西原样发出去，然后把它拿回来的 **HTTP 状态、回包正文、耗时** 交回来 —— 解析译文、识别到的语种、计费 token、错误码翻成人话、写缓存全在这里做。⚠️ 错误码表**与 TTS 不是同一张**（这边成功是 20000000；TTS 那边同一个码含义不同），复用会把「成功」当「失败」。⚠️ **成功码但拿不到译文 → 不算成功**（宁可报错也不给一条空译文）。⚠️ 返回的是界面那份 TranslateResult（含 sourceLabel / targetLabel 两个中文名）—— 语言的中文名是产品文案，只许在内核这一处拼。

```c
enum dsh_error dsh_translate_accept(dsh_engine *engine, const char *plan_json, int32_t http_status, const char *response_body, int32_t elapsed_ms, char **out_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `engine` | `engine` | 入 |  |
| `plan_json` | `utf8` | 入 | dsh_translate_plan 给的那份原样交回来（缓存键与语种都在里面） |
| `http_status` | `i32` | 入 | 外壳拿到的 HTTP 状态码；0 = 根本没发出去（断网 / 超时） |
| `response_body` | `utf8` | 入 | 回包正文（可以为空串）；外壳**不许**自己解析它 |
| `elapsed_ms` | `i32` | 入 | 外壳量到的耗时（毫秒）—— 只用于显示与「检测凭据」 |
| `out_json` | `json` | 出 | TranslateResult：{ok,message,text,translation,sourceLanguage,targetLanguage,sourceLabel,targetLabel,detected,tokens,cachedItems,fetchedItems,elapsedMs} |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

#### `dsh_translate_clear_cache`

清空译文缓存（界面上那个「清空翻译缓存」）。返回 {"count":N}（清掉几条）。⚠️ 它是**必需项而不是优化**：MT 按 token 计费，而用户来回跳词条会把同一句翻很多次。

```c
enum dsh_error dsh_translate_clear_cache(dsh_engine *engine, char **out_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `engine` | `engine` | 入 |  |
| `out_json` | `json` | 出 | {"count":N} |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

#### `dsh_translate_payload`

把一次翻译包成**正文框里那个词条的载荷**（参考实现的 `TranslatePayload` + `Annotate`）。用于查词通道自动翻译那一档：链说「该翻译」（`via=translate` + `needsTranslate=true`），外壳把请求发出去、把回包交给 `dsh_translate_accept`，再拿 accept 的结果与**伪词条地址**回来问这一条，得到一份与真词条**同形状**的载荷 —— 于是朗读、复制、返回栈一行都不用改就都生效（参考实现的原话：译文若另起一块 UI，这四件事就得各写第二遍）。⚠️ `entry_url` 由外壳给（浏览器地址里那个令牌是外壳的一次性表，内核不认识 socket 也不认识 URL 表），其余字段**一个字都不许在外壳拼**。⚠️ 翻译失败时（`ok=false`）这一步**不许**装作翻过了：回的是 `found=false` + `via=terminal` + 那句人话。

```c
enum dsh_error dsh_translate_payload(dsh_engine *engine, const char *translate_json, const char *entry_url, char **out_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `engine` | `engine` | 入 |  |
| `translate_json` | `utf8` | 入 | `dsh_translate_accept`（或命中缓存时 `dsh_translate_plan`）给的那份原样交回来 |
| `entry_url` | `utf8` | 入 | 伪词条的文档地址（外壳按 `entryToken` 拼好的那条）；空串 = 没有地址，这时不给正文 |
| `out_json` | `json` | 出 | 界面那份 `EntryPayload`（与 `dsh_engine_lookup` **逐字段同名**） |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

### 解析层

#### `dsh_dict_open`

打开一本 .mdx（解析层，不经引擎）。探测 / 逐字节对照 / 工具用得上；产品路径一律走引擎。

```c
enum dsh_error dsh_dict_open(const char *path, dsh_dict **out_dict);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `path` | `utf8` | 入 |  |
| `out_dict` | `dict` | 出 |  |

调用方用 `dsh_dict_close()` 关掉句柄。 ABI v1 起可用。

#### `dsh_dict_close`

关掉一本词典。

```c
void dsh_dict_close(dsh_dict *dict);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `dict` | `dict` | 入 |  |

ABI v1 起可用。

#### `dsh_dict_info`

词典头信息：书名、条目数、版本、加密标志、编码、资源卷命中的那几个。

```c
enum dsh_error dsh_dict_info(dsh_dict *dict, char **out_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `dict` | `dict` | 入 |  |
| `out_json` | `json` | 出 | {"title":"…","fileName":"…","entryCount":N,"version":"2.0","encrypted":bool,"encoding":"UTF-8","sizeBytes":N,"mddVolumes":["…"]} |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

#### `dsh_dict_contains`

词典里有没有这个键。⚠️ 只认精确命中（大小写变体算命中），**不联想** —— 这是 product 约定，不是解析层细节。

```c
enum dsh_error dsh_dict_contains(dsh_dict *dict, const char *key, char **out_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `dict` | `dict` | 入 |  |
| `key` | `utf8` | 入 |  |
| `out_json` | `json` | 出 | {"found":bool,"keyText":"规范键名"|""} |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

#### `dsh_dict_fetch`

取一条记录的原始内容（未渲染）。@@@LINK 重定向在这里解开，并给出最终落点。

```c
enum dsh_error dsh_dict_fetch(dsh_dict *dict, const char *key, char **out_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `dict` | `dict` | 入 |  |
| `key` | `utf8` | 入 |  |
| `out_json` | `json` | 出 | {"found":bool,"keyText":"…","text":"…","redirectedFrom":"…"} |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。

#### `dsh_dict_keys`

枚举全部键（JSON 数组），按词典的序数序。逐字节对照与诊断的主力接口：拿它跟参考实现 / js-mdict 逐条比。

```c
enum dsh_error dsh_dict_keys(dsh_dict *dict, char **out_json);
```

| 参数 | 类型 | 方向 | 说明 |
| --- | --- | --- | --- |
| `dict` | `dict` | 入 |  |
| `out_json` | `json` | 出 | ["apple","application",…] |

出参由内核分配，调用方用 `dsh_release()` 还给内核。 ABI v1 起可用。
