# 不带 `.mdd` 的词典与机器翻译开发指导

> 状态：**已实现**。两件事都落在本仓库的 C 内核里：
> **A. 不带 `.mdd` 的词典**（`.mdx` 旁边散放 css / js / ttf / png，没有 `.mdd`）；
> **B. 机器翻译**（同一把火山引擎 API Key 调豆包机器翻译大模型）。
> 本文写的是**决策、边界与理由**；接口逐条说明见 `docs/api/abi.md`，
> 内核实现见 `native/src/**`，格式支持的真实范围以 `tools/golden/fixtures.txt` 为准。
>
> 两个任务的触发点各一句话：
> **A** —— 「按界面给的路径去读本地文件」全程序**只此一处**（`native/src/dict/dsh_sibling.c`），
> 它是**一次信任边界变更**；
> **B** —— 在线能力**只绑豆包（火山引擎）**，凭据与语音共用同一把 Key，但**权限各自独立**。

---

## 〇、开工前（硬性前置）

### 0.1 先确认权限，再动手

⚠️ **同一把 API Key，不等于同一个权限。**

TTS 要的是 `seed-tts-2.0`，MT 要的是 **`volc.speech.mt`**，官方文档明写：

> 资源 ID：**需开通 volc.speech.mt 权限**

**没开通的症状是调用返回鉴权 / 权限类错误，很容易被误判成「Key 填错了」。**
两个动作完全不同（一个去控制台开通、一个去改 Key），所以这两档必须分开说。

实测细节（`tools/probe-mt.mjs` 的注释里记着）：`code: 55000000` 其实是服务端
**整个鉴权 / 权限家族的外壳**，而码表里那一条写的是「服务内部错误，稍后重试」——
HTTP 500 的时候撞到过「没开通 `volc.speech.mt`」与「Key 是错的」两种。
所以**鉴权 / 权限类要先按报文定性，不能只查错误码表**；分不出来才退回码表。

控制台：<https://console.volcengine.com/speech/new/setting/apikeys>

### 0.2 先跑诊断脚本（probe），再写 provider

```powershell
# 单次连通性 + 一个短句翻译
node tools/probe-mt.mjs --api-key <key> --text "苹果" --target en
# 不给 --api-key：从 settings.json 读（也可用 LOOKUP_USER_DATA=<临时配置目录> 指定另一份）
```

诊断脚本（probe）必须打印：请求用的 `source/target/resource-id`、HTTP 状态码、耗时、
**译文**、`usage.total_tokens`、失败时的人话解释。

**理由**：没有它，第一次真实请求会发生在「业务代码写完、点翻译」那一刻，
失败时分不清是 Key 错、权限没开、语种代码错、还是解析写错 —— 这四种原因的下一步动作完全不同。

`tools/probe-mt.mjs` 不联网时也有价值：它的请求构造与语种映射与内核里的那一份**逐条对着看**，
是这一层最容易漏的地方（内核里的纯逻辑版本在 `native/tests/test_translate.c`，B 级、不联网、不花钱）。

### 0.3 凭据：只从用户自己的设置里读，**不要写死、不要提交、不要编占位值**

- 程序从**用户自己的设置**里读凭据：`%APPDATA%\LookupApp\settings.json`。
  账号级凭据在 `{"volcengine":{"apiKey":"…"}}`（**语音与翻译共用同一把**）；
  `{"speech":{"doubaoApiKey":"…"}}` 是**同一把 Key 的镜像**（两个字段同值）。
  内核读的时候**先看 `volcengine.apiKey`，没有再退回 `speech.doubaoApiKey`** ——
  升级上来的老配置才不会丢 Key（丢 Key 的症状是「以前能用，现在没声音、也没报错」，
  这是最难查的一类问题）。
- 想验在线那条路的开发者，**自己通过程序自己的设置页（或自己的 settings.json）填自己的 Key**。
- **不许写死 Key、不许把 Key 提交进仓库、不许编一个「看起来能跑」的占位 Key**。
  本仓库**不含任何凭据**。
- **没配 Key 时功能必须置灰 + 说明原因**，不许不报错地失败（见 §B7）。
- 实码现状（照这个说，别按旧文档想象）：
  - `dsh_settings_speech_online_ready()`（`native/src/engine/dsh_settings.c`）的判定**只有一条**：
    Key 非空。**刻意没有**第二个「在线总开关」。
  - 界面上 Key 是**掩码**（`shell/Lookup.Host/SpeechStatus.cs` 的 `Mask()`，只回前 8 后 4，
    形如 `a30dba13…47fc`）—— 掩码是为了**认**，不是为了还原。
  - MT 的 resource id 是**固定常量 `volc.speech.mt`**，**不设字段、不给用户改**
    （与 TTS 那边把 `doubaoResourceId` 设成只读同一个道理）。

---

## 一、任务 A：不带 `.mdd` 的词典（资源与 `.mdx` 同目录散放）

### A1. 现象

《新世纪汉英大词典》在 MDict 里排版正常，在本程序里**排版全无**（只剩裸文字）。

它是一个**文件夹**，不是单个 `.mdx`：

```
xsjhy20oct2.mdx      24.2 MB   词条
xsjhy20oct2.css        17 KB   样式表
xsjhy20oct2_pure.css   18 KB   另一份样式（见 A6）
xsjhy20oct2.js         13 KB   脚本
xsjhylibrary.js         5 KB   脚本依赖的库
Bookerly.ttf          353 KB   自定义字体
xsjhy20oct2.png       110 KB   图片
                       ↑ 没有 .mdd
```

词条正文第一行就是**裸文件名**引用：

```html
<link rel="stylesheet" type="text/css" href="xsjhy20oct2.css">
<div class="xsjhy20oct2_container">…</div>
```

### A2. 根因：参考实现的资源查找第一步就短路了

**参考实现**（上一代的 C# 实现）里，资源只有一条来路 —— 从 `.mdd` 卷里取，
而查找函数的开头就是一句 `if (dict == null || dict.Mdds.Count == 0) return null;`。
不带 `.mdd` 的词典**一个 `.mdd` 都没有** → `Mdds.Count == 0` → 直接 `return null`
→ 虚拟主机回 404 → `xsjhy20oct2.css` 取不到 → **CSS 全丢 → 排版全无**。

已实测确认：词条文档用 `<base href="https://<dictId>.dictres.invalid/…">` 做基准，
所以相对路径正好落到虚拟主机的资源分支，走的就是那个查找函数
（基准的拼法见 `native/src/dict/dsh_entry_doc.c` 的 `dsh_entry_doc_base`）。

本仓库的对应实现是三处，**缺一不可**：

| 文件 | 职责 |
| --- | --- |
| `native/src/engine/dsh_resource_api.c` | **去哪找**：先 `.mdd` 卷（候选键名在外、卷在内，与参考实现同一条顺序），再**同目录**（`dsh_sibling_locate`）；顺带定 **MIME**（`dict/dsh_mime.c`）与 **ETag**（`make_etag`） |
| `native/src/dict/dsh_sibling.c` | 同目录那条路的**四道检查**（见 §A7） |
| `native/src/engine/dsh_entry_api.c` 的 `has_resources()` | 这本词典到底有没有资源可给：`.mdd` 卷**或**同目录散放的文件 |

### A3. 好消息：周边该有的都是现成的

周边环节**全都是现成的**，这个任务的主体是**查找回落**，不是"补一整套技术栈"：

| 环节 | 状态 |
| --- | --- |
| 词条正文的 CSP | ✅ **已放行** `style-src … https://*.dictres.invalid`、`font-src`、`img-src`、`script-src`（`native/src/dict/dsh_entry_doc.c` 的 `CSP[]`） |
| MIME | ✅ `.css` / `.js` / `.ttf` / `.woff` / `.woff2` / `.otf` / `.eot` / `.png` **全在** `native/src/dict/dsh_mime.c`（还多出音频与 `.svg` / `.avif` 等） |
| 字体的 CORS | ✅ 资源响应那行 `Access-Control-Allow-Origin: *` 的注释就写着"词条正文在 opaque origin 里，字体这些资源需要 CORS"（`shell/Lookup.Host/VirtualHost.cs`） |
| `<base>` 基准 | ✅ 已有，相对路径能正确解析到资源域 |
| 虚拟主机 | ✅ `shell/Lookup.Host/VirtualHost.cs` 的资源那条路：ETag → 304；Range → 206 / 416；其余 200 / 404 |

### A4. 改法（已落地）

- **资源查找**：`.mdd` 全部未命中时，**回落到 `.mdx` 所在目录**按 `resourcePath` 找文件。
  **查找顺序：`.mdd` 优先 → 同目录兜底。**
  （同一本词典同时有 `.mdd` 和同目录散放的文件时，`.mdd` 是"官方打包版"，优先 ——
  这条顺序在 `dsh_resource_api.c` 里与参考实现逐条相同。）
- **"有没有资源"的判定**：`has_resources()` 现在两条都看（`.mdd` 卷**或**同目录有散放文件），
  它被用来填词条文档的 `data-has-resources`；两条判定**必须一致**，否则会互相矛盾。
- **词条正文那条路**要把那本词典先加载起来（`.mdd` 是懒开的）。

### A5. 放行范围：**决定放行 `.js`**

这条最容易被忽略：**CSP 本来就允许 `script-src https://*.dictres.invalid`**，
今天 JS 没跑，只是因为它 404 了。**一旦同目录散放的文件能读了，`.js` 会立刻开始执行。**

而词条是**外部内容**，里面有 `xsjhy20oct2.js` + `xsjhylibrary.js`（共 18 KB，来路不明）。

**决定：放行 `.js`** —— 因为词典的交互就是靠它（`floatball` 浮球、折叠、页签），
不放行时那些控件点了没反应，看起来像程序坏了。

```
放行（被动）：.css .ttf .otf .woff .woff2 .eot .png .jpg .jpeg .gif .svg .webp .bmp .ico .avif
放行（脚本）：.js                    ← 唯一一个会执行的扩展名
仍不放行：    .mjs .html .htm
```

白名单就写在 `native/src/dict/dsh_sibling.c` 的 `ALLOWED[]` 里。
音频也**不从这条路走** —— 它归发音那条路。

`.mjs` 仍不放行：它是 ES module，与 `<script type="module">` 配套，而这份词典用的是普通脚本；
真要放行它是**另一次**独立决定（谁需要谁提，别顺手加）。

**为什么现在可以放**（风险与边界，逐条对着实现说）：

- 脚本跑在**词条 iframe** 里，那个 iframe 是 `sandbox="allow-scripts"` 且**没有**
  `allow-same-origin`（opaque origin）：拿不到宿主的 DOM / 存储 / Cookie
  （`web/floating.html` 的 `<iframe id="entryFrame">`）；
- 文档的 CSP 是 `default-src 'none'`，并显式写了 **`connect-src 'none'`**、
  `form-action 'none'`、`frame-src 'none'`、`object-src 'none'` ——
  于是脚本**跑得起来，但发不出去**（不能把词条内容外传，也不能自己开窗口 / 表单提交）；
- **只允许 `https://*.dictres.invalid` 上的脚本**（`script-src` 那条），外域脚本进不来。

**改动只有一处（内核）**：`native/src/dict/dsh_sibling.c` 的 `ALLOWED[]` 加 `.js`。
- 壳**不用改**：虚拟主机里没有第二份放行表，它只发内核认可的字节；
- CSP **不用改**：`dsh_entry_doc.c` 的 `script-src` 本来就允许 dictres 域的外部脚本
  （⚠️ 但**也不许顺手放宽**它 —— 那一次扩权不在本次决定里）；
- MIME 早就有：`dsh_mime.c` 已把 `.js` 映成 `text/javascript; charset=utf-8`；
- **零新增依赖**（脚本由 WebView2 执行，内核只负责发字节）。

**怎么验**（每一级都在本仓库里有落点）：

1. **B 级** —— `native/tests/test_sibling.c`：`dsh_sibling_is_allowed_extension(".js") == 1`，
   而 `.html` / `.htm` / `.mjs` 仍为 0；
2. **C 级（标准答案文件对照）** —— `powershell -File tools\golden-gate.ps1` 盯住
   `dsh_entry_doc.c` 的 CSP **逐字未变**（词条文档的拼装逐段照抄参考实现，连换行都不能少）；
3. **D 级** —— `shell/Lookup.App/SelfCheck.cs` 第 ⑮ 节：用
   `tools/make-sibling-fixture.mjs` 造一份带 `.js` 的、同目录散放资源的测试用词典，
   脚本在被点击时改一个 DOM 界面标记，真实程序里读那个界面标记；
4. **反向自检**：把 `.js` 从白名单里撤掉，第 3 条必须**红**（证明它钉的是对象本身）。

**回退**：把 `.js` 从 `ALLOWED[]` 里去掉即可，其余一行都不用动（这也是"只动一处"的好处）。

### A6. 词典侧的资源情况

1. **思源字体已补齐**（用户补进词典目录的）：

   ```
   SourceHanSerifCN-Regular.ttf   14,495,132 B  (13.8 MB)
   ```

   已核验三件事：头 4 字节是 `00 01 00 00`（真 TrueType，不是改名的别的东西）；
   字体名解析为 **"Source Han Serif CN Regular"**；与两份 CSS 里
   `@font-face { font-family: "Source Han Serif CN"; src: url("SourceHanSerifCN-Regular.ttf"); }`
   完全对得上。

   → **A 做完之后"排版与 MDict 一致"是可达的**，词典侧不再有已知缺件。

   ⚠️ 但它 **13.8 MB**，把一个新的性能问题带了进来 —— 见 §A8，**别漏**。

2. **别指望 `_pure.css` 是"无 JS 版"**：两份 CSS 逐行比对只差 27 行、
   `@font-face` / `.floatball` 完全一样 —— 它不是为不支持 JS 的阅读器准备的。
   **别按这个猜测去偷懒**（原本以为是，查了才发现不是）。

### A7. 验收

| 级别 | 验什么 | 入口 |
| --- | --- | --- |
| **B 纯逻辑** | 同目录查找的回落：`.mdd` 命中优先、`.mdd` 未命中回落同目录、**路径穿越被拒**、非白名单扩展名被拒。**不启动程序** | `native/tests/test_sibling.c`（`wsl.exe -- bash tools/wsl-make-test.sh`） |
| **C 定向观察** | 打开真词典，读词条 iframe 里的 `document.styleSheets.length`、有没有 404、`getComputedStyle` 的关键类是否生效 | `node tools/probe-page.mjs --entry`（在**词条页的执行上下文**里求值；壳要带 `--debug-port <n>` 启动） |
| **C 性能现场** | "首次查词 / 第二次查词"的耗时、换词典那一步的耗时（见 §A8） | `node tools/probe-page.mjs --perf --perf-dict <词典 id> --perf-word <词>` |
| **人工** | 与 MDict 并排看排版 | —— |

**安全断言必须是 B 级、必须独立跑**（不能靠"完整测试顺带覆盖"）。
同目录那条路的**四道检查**（`native/src/dict/dsh_sibling.h` 写明了这是全程序**唯一**一处
「按界面给的路径去读本地文件」的地方）：

- 扩展名**白名单**（`ALLOWED[]`；非白名单 → 返回 404）；
- **拒绝绝对路径 / UNC 路径**；
- 归一化之后**仍须在词典目录内**（`..` 走不出去）；
- **逐段拒重解析点**（符号链接 / junction 不给绕）。

`..\..\..\Windows\win.ini` 这类路径必须拒绝；判定的是**解析之后的真实路径**，
不是字符串比对。

### A8. ⚠️ 大资源带来的性能问题（**补齐字体后才出现，别漏**）

思源字体补上之后，这部词典的资源体积是：**ttf 13.8 MB** / png 110 KB / css ×2 共 35 KB。
而**不带 `.mdd` 的词典第一次把"十几 MB 的资源"带了进来** ——
`.mdd` 里的资源通常几 KB~几百 KB，所以这条链路一直没暴露性能问题。

当时实测的现状（**参考实现**，这就是问题被发现时的样子）：

| 环节 | 当时的样子 |
| --- | --- |
| `.mdd` 读取 | 每次返回**整个 `byte[]`** → **整份进内存** |
| 资源响应头 | **5 处**都写死 `Cache-Control: no-cache`，且**没有 ETag / Last-Modified** |
| 协商缓存 | 没有 ETag → 浏览器的"重新验证"必然退化成**整块重传** |
| 词条 iframe | 每次查词都换 `src`（带 `&t=` 防缓存）→ **新文档 = 重新请求全部资源** |

叠起来就是：**每次查词都可能重新读盘 + 重传 13.8 MB 的字体。**

**本仓库现在的状态（逐条对照上面那张表）**：

| 建议 | 现状 |
| --- | --- |
| ① **先量再优化，不要凭猜去改** | 工具已经现成：`node tools/probe-page.mjs --perf --perf-dict <词典 id> --perf-word <词>` —— 它把耗时拆成几段（换当前词典 / `lookup(word, id)` 两次：**第一次 = 冷加载那一本 `.mdx`**、第二次是热态 / 输入框打字回车直到 `#readerLoading` 收掉，把词条正文与文档里那些资源**一起**算进去），单位 ms，**只报数、不下判断**。⚠️ 它会**换当前词典**，所以收尾必须还原 —— 诊断脚本不许改用户的现场 |
| ② **给资源响应加 `ETag`** | ✅ **已落地**：`shell/Lookup.Host/VirtualHost.cs` 的 `ServeResource` —— 先探一遍「有没有、多长、身份证是什么」（**这一次不要字节**），`If-None-Match` 命中就回 **304、一个字节都不读**；ETag 由 `dsh_resource_api.c` 的 `make_etag()` 造（`.mdd` 那一路带卷路径、同目录那一路带真实文件路径） |
| ③ **大文件别整块进内存** | ◐ **部分落地**：内核侧 `dsh_engine_resource` 支持 `offset` / `want`，虚拟主机把 Range 翻成 **206 / 416**，所以浏览器要一段给一段（字体这种大文件正是靠这条）；⚠️ 但外壳仍把这一片拷进 `byte[]` 才交给 WebView2（不是 `Stream`），**未加 Range 的整份请求仍然整份进内存**。同目录那条路另有一条守卫：单文件超过 **64 MB** 拒绝读进内存并如实说明 |
| ④ **注意字体的按需特性** | 保留：浏览器只在真的用到该 `font-family` 时才下载字体，所以"中文释义"会触发、纯英文词条不一定 —— 量的时候要分清这两种情况，否则会得出"第一次慢、后面快"的错误结论（那只是文档变了） |

两条硬的（改动时别破坏）：

- **词条 HTML 仍然 `no-cache`**（它是每次动态生成的）；
- **不要用长 `max-age` 了事**：`dictId` 是内容哈希，但同一个 id 的资源仍可能在卷之间移动，
  长缓存会拿到旧资源。`no-cache` 的语义是**「每次都要来问一句」，不是「别缓存」** ——
  配上 ETag 才是协商缓存。

**验收要单独加一条**：拿这部不带 `.mdd` 的词典**连查 10 个词**，确认耗时没有随查词次数线性变慢。
这条不属于 §A7 的表，因为它验的是"重复操作下不退化的性质"，不是单次结果对不对。

---

## 二、任务 B：机器翻译接入

### B1. 已定决策（不要重新讨论）

| 问题 | 决定 |
| --- | --- |
| 接入平台 | **只绑豆包（火山引擎）**，界面上不提供平台选择 |
| 与语音的关系 | **同一把 API Key**；但 `X-Api-Resource-Id` **各自独立** |
| 翻译的角色 | **"词典答不了时的下一步"**，不是模式、不是常驻按钮 |
| 引擎类型 | **专用翻译接口**（见 B3） |
| 默认状态 | **关闭** + 明确隐私提示 |

**结构上仍然要解耦**：在线能力做成可替换的 provider（豆包是第一个实现）。
将来换 / 加不用重构 —— **绑定是产品决定，解耦是工程决定**。
本仓库里这个解耦的落点就是「内核说该发什么 / 内核说回包是什么意思，外壳只负责把字节发出去」：
`dsh_translate_plan` → 外壳 POST → `dsh_translate_accept`（`native/src/translate/dsh_translate.c`
与 `native/src/engine/dsh_translate_api.c`；外壳那一半在 `shell/Lookup.Host/TranslateHttp.cs`）。

### B2. 接口规格（照官方文档抄，别再猜）

```
POST https://openspeech.bytedance.com/api/v3/machine_translation/matx_translate

请求头（新版控制台 —— 本程序用的就是这套）：
  X-Api-Key:         <控制台创建的 API Key>        ← 与 TTS 同一把
  X-Api-Resource-Id: volc.speech.mt                ← 固定值（TTS 是 seed-tts-2.0，不同！）
  X-Api-Request-Id:  <uuid>
  Content-Type:      application/json

（旧版控制台用 X-Api-App-Key + X-Api-Access-Key —— 不用，别照抄网上老教程）

请求体：
{
  "source_language": "zh",        // 可选；不传或空串 = 自动检测
  "target_language": "en",        // 必填
  "text_list": ["字节跳动致力于激发创造、丰富生活"],   // 必填
  "corpus": {                     // 可选：术语定制
    "glossary_list": { "Volcengine": "火山引擎" }
  }
}

返回体：
{
  "code": 20000000,               // 成功（注意：这个码与 TTS 那边含义不同）
  "message": "ok",
  "data": {
    "translation_list": [
      { "translation": "…",
        "detected_source_language": "en",     // 仅当请求未指定 source_language 时返回
        "usage": { "prompt_tokens": 31, "completion_tokens": 20, "total_tokens": 51 } }
    ]
  }
}
```

**官方文档（稳定公开 URL）**：
豆包语音 · 机器翻译 产品介绍 <https://docs.volcengine.com/docs/MachineTranslation/Productintroduction?lang=zh>；
豆包语音 · HTTP Chunked/SSE 单向流式合成（V3，语音那一边的接口）
<https://docs.volcengine.com/docs/DoubaoVoice/HTTPChunkedSSEUnidirectionalStreaming-V3?lang=zh>。

**使用限制（硬约束，必须自己先切分）**：

| 限制 | 值 |
| --- | --- |
| `text_list` 条数 | **≤ 16** |
| 单条文本 | **≤ 1024 tokens** |
| 超出时 | `45000130` 请求载荷过大 |

### B3. 它本身就是大模型 —— 之前"MT 还是 LLM"的争论到此为止

官方描述：*"基于**大语言模型**推出的新一代在线文本翻译服务，支持 32 种语言互译，
提供高质量、**上下文感知**的翻译能力，并支持术语定制"*。

所以这不是"老式 MT vs 新 LLM"，而是：

> **专用翻译接口**（大模型驱动，但只做翻译、输出结构化）vs **通用对话模型**（要自己写 prompt、输出形态不可控）

**这正好落在之前建议的那一边**：既要大模型的翻译质量，又要"只给译文"的确定性输出。
**不用再纠结了。**

### B4. 语种映射（32 种 vs 本机的更多种，**必须建表**）

支持的 32 种（ISO 639-1 / BCP-47），逐字照服务端的写法、顺序也照抄（便于对账）——
表在 `native/src/translate/dsh_translate.c` 的 `k_mt_languages[]`：

```
zh  zh-Hant  en  ja  ko  fr  de  es  pt  ru  ar  it  nl  pl  ro  sv
da  nb  fi  hu  cs  hr  el  he  tr  uk  th  vi  id  ms  tl  hi
```

⚠️ **三处与本机写法对不上，会出错却不报错**：

| 语种 | 本机 | MT | 处理 |
| --- | --- | --- | --- |
| 挪威语 | `no` | **`nb`** | 映射 `no` → `nb`（`nn` 也一并归到 `nb`） |
| 菲律宾语 | `fil` | **`tl`** | 映射到 `tl` |
| 繁体中文 | `zh`（默认标签给 `zh-CN`） | **`zh-Hant`** | 单独映射：`zh-Hant` / `zh-TW` / `zh-HK` / `zh-MO` 都归 `zh-Hant`，其余归 `zh` |

**本机有、MT 没有的语种**：
→ **该语种不提供翻译**，在界面上说明「机器翻译暂不支持〈波斯语〉」，**不要退化成"译成英语"**——
那是最糟的失败方式：用户以为译错了，其实是方向被偷偷改掉了。
实码约定：语种映射函数**回 NULL** 表示不支持，**绝不退化**；外壳**不许在中间做任何判断**
（不许改 target、不许把不支持的语种退化成英语、不许把非 200 当失败、不许自己拼错误提示）——
这几条写在 `native/src/translate/dsh_translate.c` 的文件头注释里，是这一层的准入条件。

### B5. 目标语种与方向判定

**规则（不引入"模式"）**：

```
用语言判定（LanguageDetector 的判定结果）定目标：
  文本是中文（Han 为主）        → target = en
  其它（英/日/法…）             → target = zh
用户在设置里可以固定：auto（上面这套）/ 固定译中 / 固定译英
```

`targetMode` **只认 `auto` / `zh` / `en` 三个值**：脏数据不许变成"发一个乱码目标语种给服务端"，
认不出的一律落回 `auto`，原因写进 `last_error`，但**不报错**（其余设置仍然可用）。

**`source_language` 传我们判定的值**，理由：本机的判定多了一条服务端没有的线索——**词典标题**
（"牛津高阶英汉双解" → 英语）。不要为了"让服务端检测"而丢掉这条线索。

**返回值里的 `detected_source_language` 保留显示**，作为一次交叉验证：
如果它与我们的判定不同，界面上标明"（服务端识别为英语）"，便于排错。

### B6. 与词典、朗读的关系：三层兜底（**与发音链同构**）

```
① 当前词典查得到        → 显示词条
      ↓ 查不到
② 在其它已导入词典里找一次 → 用《新世纪汉英大词典》借查并显示
      ↓ 也没有
③ 机器翻译              → 译文作为「伪词条」进正文框
```

**② 的底层能力早就有**：查词典那条接口的第二个参数就是词典 id，
传了就用那一本、**且不写当前词典** —— 也就是"在指定词典里查一次、不改变当前词典"。

> ⚠️ 区分两件事：**"借查"**（在别的词典里查一次，不改状态）≠ **"切换"**（改当前词典）。
> 这套方案只做前者，**不做自动切换**。

整条链的决策函数在 `native/src/engine/dsh_fallback.c`（纯函数，表驱动单测在
`native/tests/test_fallback.c`），逐条规格见 `docs/design/查词兜底通道与历史记录开发指导.md`。

**③ 白捡的组合**：译文可以用**已有的朗读链路**念出来（译文是中文 → 走中文音色）。

> ⚠️ **一条曾经的约定已被推翻，别再照旧的那条做**：
> 早先的约定是「翻译只在用户点一下时发生，**绝不在查词时自动做**」（理由是费钱、泄隐私、
> 喧宾夺主）。**现在不是这样了**：查词通道的第三步就是**自动翻译**，由设置里的
> `translate.autoTranslate`（**默认开**）控制，关掉时终态页会给一条**可点的**「翻译这个词」。
> 隐私那条靠**总开关默认关**（`translate.enabled` 默认 false）+ 翻译页上的静态隐私提示来守，
> 不靠"永不自动"。

### B7. UI：翻译页的两种状态

**缺 Key 时不要整页灰**——灰会把"为什么"一起藏掉（这正是本仓库一直在避免的模式：
**置灰 + 说明原因，而不是点了没反应**）。

```
翻译
  ⚠ 翻译需要火山引擎凭据，它与语音共用同一把 API Key。
     [ 去「语音」页填写 → ]            ← 这个按钮必须可点
  ── 以下设置在有 Key 之后可用 ──
  启用机器翻译  [置灰，但保留可见，悬停有 title 说明原因]
  目标语种      [置灰]
  查不到时自动翻译 [置灰]
```

**有 Key 时**：

```
翻译                              [开关]（默认关 + 隐私提示「会把文本发给火山引擎」）
  目标语种    自动（中文译英，其余译中）/ 固定译中 / 固定译英
  查不到时自动翻译  ✓（默认开）
  [检测凭据]  → 语音合成 ✓ 1.2s / 18KB
                机器翻译 ✓ 0.3s / 51 tokens
  [清空译文缓存]
```

落点是 `web/manager.html` 的 `paneTranslate` 与 `web/src/manager/main.ts` 的 `renderTranslate()`。

**三条硬要求**（在实码里逐条都有对应）：

1. **区分两种"关"**：「用户主动关的」（开关可点）vs「缺 Key 客观不可用」（置灰 + 原因 + 跳转）。
   混成一种灰，用户会以为"是我自己关的"，反复点开关。
   → 实码把「缺 Key 时要置灰的三样」写成一条**显式清单**
   （`translateEnabled` / `translateTarget` / `translateAuto`），
   检查标准是**置灰、但保留可见（不是 `hidden`）、原因写在 `title` 上**，而且清单里
   **不包括**那个解锁用的跳转按钮。
2. **API Key 输入框永远可填**，不能被任何开关连带禁用 —— 它是解锁条件，
   一起禁用会变成**死锁：没 Key 就填不了 Key**。
   这一页**没有** Key 输入框：凭据只在「语音」页填（**凭据不单独成页**），
   所以这里能做且该做的就是"说清缺什么 + 把人送过去"，那个跳转按钮**任何时候都不置灰**。
3. **检测合成一个按钮报多行**：同一把 Key、同一个平台，分开检测等于让用户点两次、
   还要自己拼"Key 是好是坏"的结论。
   ⚠️ 「检测凭据」**会真联网、按字符计费**，所以只由用户点按钮触发。

### B8. 设置 schema（**凭据共享是关键**）

凭据**只有一处**（账号级），语音与翻译共用；如果新加一个 `translateApiKey`，
就会出现**两处存同一把 Key、改一处另一处失效**。

```
volcengine.apiKey            ← 账号级，语音与翻译共用
speech.doubaoApiKey          ← 同一把 Key 的**镜像**（同值）；兼容读取见下
speech.doubaoResourceId      = seed-tts-2.0（已有）
speech.doubaoSpeakerEn/Zh    （已有）
translate.enabled            ← 默认 false（总开关：要联网、要把文本发给第三方）
translate.autoTranslate      ← 默认 true（「查不到时自动翻译」）
translate.targetMode         ← auto / zh / en
```

- **MT 的 resource id 是固定值 `volc.speech.mt`，不设字段、不给用户改**
  （和 TTS 那边把 `doubaoResourceId` 设成 readonly 同一个道理）。
- ⚠️ **必须兼容读旧路径**：升级上来的用户 Key 在 `speech.doubaoApiKey` 里，
  直接改名会让他们**丢掉已填好的 Key**，症状是"以前能用，现在没声音、也没报错"——
  这是最难查的一类问题。读的时候**先看新字段、没有再看旧的**。
  实码：`native/src/engine/dsh_settings.c` 的 `merge_speech` —— 两个字段都没在补丁里出现时
  **镜像不许动**（第一版无条件走"清空"，"只改语速"的补丁会把已填好的 Key 抹掉）。
- ⚠️ **「词典优先」那个开关已经取消**，`translate.dictionaryFirst` 字段**一并删掉**了：
  它的字面承诺是「词典命中时不再显示翻译」，而那本来就是这个链的固有行为
  （只在没查到时才给翻译），所以它是个**勾了没作用的开关** ——
  它当时的"作用"只是让人以为自己关掉了某件事，属于**误导性控件**。
  真正需要用户控制的那个开关是 **`translate.autoTranslate`**（「查不到时自动翻译」），
  它**不是**同一件事：关掉它**不等于**不要翻译 —— 终态页会给一条**可点的**「翻译这个词」。
  老配置里残留的 `dictionaryFirst` 键**不用迁移**：内核不建模它，它走「没建模的子键原样搬运」
  那条路（`dsh_settings.c` 的 `foreign_pair` / `keep_extra`），下次写盘自然消失 —— 实测见
  `native/tests/test_settings.c`。
- ⚠️ **`translate` 是一节设置，不是一个平铺的键**：改设置的补丁包在 `{"translate":…}` 里递下去，
  **归一化与默认值由内核定**，外壳不许自己拼。

### B9. 错误码（与 TTS **不是同一张表**，别复用）

| code | 含义 | 给用户的话 |
| --- | --- | --- |
| `20000000` | 成功 | — |
| `45000001` | 请求参数错误 | 「目标语种没指定」等 —— 通常是代码 bug，不是用户问题 |
| `45000130` | 载荷过大 | 「这段太长」——**正常不该出现**（要自己先按 §B2 的限制切分） |
| `55000001` | 服务内部错误 | 「翻译服务出错，稍后重试」 |
| `55000000` | **整个鉴权 / 权限家族的外壳** | 要**按报文**分开说：没开通 `volc.speech.mt` → 「账号未开通机器翻译权限，去控制台开通（和语音是两个独立权限）」；Key 错 → 去改 Key。分不出来才退回码表 |
| 权限 / 鉴权类 | 未开通 `volc.speech.mt` | 「账号未开通机器翻译权限，去控制台开通（和语音是两个独立权限）」 |

**同一条：HTTP 状态与业务 code 要分开判**，非 200 也算"答上来了"（403 要翻成"没开通权限"）。

### B10. 缓存与计费

- **按 token 计费**（返回体里就有 `usage.total_tokens`）→ 比 TTS 的按字符更容易失控。
  **缓存是必需项，不是优化。**
- **缓存键**：`引擎版本 | source | target | 文本 hash`；译文落盘。
  同一个句子反复翻译（来回跳词条很常见）不该重复计费。
  实码：`native/src/translate/dsh_translate.c` 的缓存键 + 外壳侧那份落盘缓存
  （`shell/Lookup.Host/Speech.cs` 的 `CacheKey(...)` 是同一条思路的语音版）；
  界面上有「清空译文缓存」，`dsh_translate_clear_cache` 回清掉几条。
- **善用 `text_list` 批量**：长文本按 ≤16 条、每条 ≤1024 tokens 打包成**一次**请求，
  而不是 N 次往返。（与朗读那边"分段 + 预取"是同一套思路，但翻译是"一次全给"更合适——
  翻译要整段一起看，不需要边译边显示。）
  切分与批量在 `dsh_translate_plan` 那一侧统一做，**外壳不做任何判断**。
- **界面可以显示本次用量**（tokens），帮助用户感知花销。
  ⚠️ 但**自动翻译那条路不许报 token 数**（见 B11 的检查标准）。

### B11. 验收

| 级别 | 验什么 | 入口 |
| --- | --- | --- |
| **B 纯逻辑** | 请求体构造（source/target/text_list/corpus）；**语种映射表**（含 `no→nb`、`zh-Hant`、不支持的语种要明确失败而不是退回）；**超长切分**（>16 条、单条 >1024 tokens）；响应解析（`20000000` 才成功）；错误码 → 人话 | `native/tests/test_translate.c`（`wsl.exe -- bash tools/wsl-make-test.sh`） |
| **C 定向观察** | 缺 Key / 有 Key 两种状态下翻译页的样子：开关是否可点、灰项是否可见、跳转是否可用、检测是否报两行 | `node tools/probe-page.mjs --manager --manager-tab translate`（两种状态各看一次；壳要带 `--debug-port <n>` 启动，见脚本顶上那段） |
| **C 真服务** | 对着**真**服务端验一次（要 Key、花几个 token） | `node tools/probe-mt.mjs` |
| **D 端到端** | 缺 Key 的现场与"清空 Key 之后 `hasApiKey` 变假" | `shell/Lookup.App/SelfCheck.cs` 第 ⑰ 节（`powershell -File tools\test-shell-app.ps1`） |
| **D 完整测试** | 只在"翻译结果进内容框 + 返回栈"这条跨模块路径上才需要（进栈 / 返回 / 滚动） | `powershell -File tools\test-shell-app.ps1` |

> ⚠️ **有一处检查目前是缺的，别以为它被守住了**：设计上要求 A 级盯住三件事 ——
> ①「缺 Key 时置灰」的那份**显式清单**（`translateEnabled` / `translateTarget` / `translateAuto`：
> 置灰、**保留可见**、原因写在 `title` 上，而且**不包括**解锁用的跳转按钮）；
> ②「词典优先」相关符号（`translateDictFirst` / `dictionaryFirst`）一处不许剩；
> ③「查不到时自动翻译」的说明文字必须是**静态的**（不许跟着开关状态变）。
> 本仓库的 `tools/ui-static-check.mjs` 目前是 ①–⑧ 节（页签与页面一一对应、页签名字、
> 页面取的 DOM 节点存在、页脚与全局热键不许回来、**页面调的桥方法名壳认不认**、常规页控件归属、
> 同一个 id 不许两处），**这三条都还没有对应的检查标准** ——
> `web/src/manager/main.ts` 的 `blockedRows` 清单已经写好了，但**没有东西钉住它**。
> 补齐这三条是这份设计留下的未完成项。


**四条纯逻辑层面的硬约定**（`native/tests/test_translate.c` 的文件头逐条列着）：
① `no`→`nb`、繁体→`zh-Hant`、菲律宾→`tl` 三处与本机对不上，少一条只会翻出奇怪的东西；
② MT 不支持的语言必须回 NULL，**绝不退化成英语**；③ 错误码表与 TTS **不是同一张**
（`20000000` 在这儿是成功），混起来会把成功当失败；④ 非 200 也算"答上来了"。

**不要**把"请求体长什么样"这类断言塞进完整测试。

---

## 三、测试分级（开工前先想清楚放哪一级）

| 新增测试 | 级别 | 入口 |
| --- | --- | --- |
| 同目录散放文件的查找 / 路径穿越 / 扩展名白名单 | **B** | `native/tests/test_sibling.c` |
| MT 请求构造 / 语种映射 / 切分 / 解析 | **B** | `native/tests/test_translate.c` |
| 翻译页两种状态的 UI 检查标准 | **A** | `node tools/ui-static-check.mjs` |
| 缺 Key 时"跳转 + 说明 + 灰项可见" | **A** | 同上 |
| 不带 `.mdd` 的词典里 `.js` 真的跑起来了 | **D** | `shell/Lookup.App/SelfCheck.cs` 第 ⑮ 节（`powershell -File tools\test-shell-app.ps1`） |
| 译文进内容框 + 返回栈 | **D** | 完整测试（只有这条真需要真实程序） |

判断标准只有一句：**这条测试需要什么才能判，就放在哪一级**；
**能用一句话说清"这次只影响哪一层"，就不许去跑最贵的那一级。**

**一条机械检查标准（防退化）**：完整测试的项数**不应该随功能增加而线性增长**。
某轮项数涨了，必须说明"新增的这几条为什么降不到 A/B/C"。

---

## 四、不要做的事

1. ❌ **不要为不带 `.mdd` 的词典引入新的第三方解析库或系统库**——
   内核是纯 C11、零第三方依赖（`native/vendor/` 只有 libspeex，且只用了它的解码路径）。
   这次要动的只是**一个查找回落**。
2. ⚠️ **`.js` 的放行是一次"已经做过"的决定，不许再顺带扩大它** —— 见 §A5：
   `.js` 放行，但 **`.mjs` / `.html` / `.htm` 仍不放行**，
   **CSP 与 iframe 的 `sandbox` 属性一个字都不许顺手放宽**。要动这三样，等于再开一次信任边界决定。
3. ❌ **不要自动切换词典** —— 只做"借查"，不改当前词典。
4. ❌ **不要做输入框的"查词 / 翻译"模式切换** —— 破坏"一个输入框一个语义"。
5. ❌ **不要给翻译单独开标签页以外的页面** —— 凭据不单独成页（语音页已经就地填了，那是好做法）。
6. ❌ **不要复用 TTS 的错误码表** —— MT 是另一套（§B9）。
7. ❌ **不要把 `doubaoApiKey` 改名了事** —— 必须兼容读（§B8）。
8. ❌ **不要把「词典优先」那个开关加回来** —— 它是"勾了没作用"的误导性控件，
   字段已删；用户真正要的那个开关是 `translate.autoTranslate`（§B8）。
9. ❌ **不许写死 / 提交 / 编造占位凭据** —— 凭据只从用户自己的设置里读（§0.3）。

---

## 五、来源与取证方式（"不要只说'我检查过了'"）

| 结论 | 怎么得来的 |
| --- | --- |
| 词典目录里的文件清单、没有 `.mdd` | 列目录实测 |
| 词条引用裸文件名 `xsjhy20oct2.css` | 用 `js-mdict` 直接 dump 词条正文 |
| 参考实现的资源查找第一步就短路 | 读参考实现的 `DictionaryEngine.cs`（`reference/0.1.3-parser/src/Dictionary/` 里有它的冻结副本） |
| CSP / MIME / CORS / `<base>` **已就绪** | 读 `native/src/dict/dsh_entry_doc.c` 的 `CSP[]` 与 `dsh_entry_doc_base`、`native/src/dict/dsh_mime.c`、`shell/Lookup.Host/VirtualHost.cs` |
| `_pure.css` **不是**无 JS 版 | 两份 CSS 逐行比对（差 27 行） |
| **思源字体已补齐且可用** | 列目录 + 读头 4 字节（`00 01 00 00` = 真 TrueType）+ 用 PyMuPDF 解析出字体名 `Source Han Serif CN Regular`，与 CSS 的 `font-family` 对得上 |
| **ETag / 304 / Range 的现状** | 读 `shell/Lookup.Host/VirtualHost.cs` 的 `ServeResource` 与 `native/src/engine/dsh_resource_api.c` 的 `make_etag` |
| 同目录那条路的四道检查 | 读 `native/src/dict/dsh_sibling.c` 与它的头文件 `dsh_sibling.h` |
| MT 接口规格、错误码、32 种语言 | 厂商公开文档（URL 见 §B2）与 `tools/probe-mt.mjs`、`native/src/translate/dsh_translate.c` |
| 语种映射三处对不上 | 比对 MT 的 `k_mt_languages[]` 与本机语种表 |
| 设置字段与凭据共用 | 读 `native/src/engine/dsh_settings.c` / `dsh_settings.h` 与 `native/tests/test_settings.c` |

**验证时的一条约定**：断言钉"被考察的对象本身"，
不要钉看起来相关的替代指标（例如"界面上有个翻译框"不能代表"翻译真的发生了"）。

---

## 来源与整理说明

- 本文整理自迁移前的项目文档 `docs/不带mdd的词典与机器翻译开发指导.md`（原属按版本复制目录的
  私人工作目录，原名 `散装词典与机器翻译开发指导.md`，后来按用词规范改名 ——
  「散装词典」这个说法对用户含义不明，已统一成**不带 `.mdd` 的词典**），
  现置于本仓库 `docs/design/`。
- 已清理：操作者的个人绝对路径与版本目录引用；指向历史会话、开发记录与轮次编号的过程说明；
  以及"每一轮向用户索要真实 API Key"的旧流程 —— 凭据一律由程序从**用户自己的设置**里读，
  仓库不含任何 Key，缺凭据时功能置灰并说明原因。
- 原始的技术决策、信任边界、API 来源与理由逐条保留，未作删减。
- 两份厂商 API 文档的大 PDF **没有**随本文迁入，改为给出厂商公开文档的稳定 URL（见 §B2）。
- 事实更正：
  1. 原文状态写「**方案已定，待实现**」；实际两件事都已落地，状态已改为**已实现**，
     并把每一节的"改法"改写成"已落地的实现 + 落点文件"。
  2. 原文 §B8 写「新增 `translate.dictionaryFirst` ← 默认 true」；实际这个字段**已被取消并删除**
     （它是"勾了没作用"的开关），取而代之的是 `translate.autoTranslate`（默认 true）——
     依据 `native/src/engine/dsh_settings.h` 的 `dsh_translate_settings` 与
     `web/manager.html` 的 `translateAuto`。
  3. 原文 §B6 / §四.5 写「不要自动翻译所有查询（绝不在查词时自动做）」；该约定已被推翻：
     查词通道的第三步是自动翻译，由 `translate.autoTranslate`（默认开）控制。已改并注明新约定。
  4. 原文 §B7 的界面草图里有「词典优先」一行；实际那一行已删除（见事实更正 2）。
  5. 原文 §A8 把「加 ETag」列为**建议**；实际已落地（`VirtualHost.ServeResource` 的
     304 协商缓存），"大文件别整块进内存"只**部分**落地（外壳仍拷进 `byte[]`），已按现状改写。
  6. 原文 §A5 的验证路径写「B 级 `native/tests/test_sibling.c` 与 D 级 `SelfCheck.cs` 第 ⑮ 节」——
     这两条经核对**存在**，保留；原文另处的 `tools/SiblingResProbe` / `tools/MtProbe` /
     `tools/ui-static-check.mjs` 里的 `tools/smoke-test.mjs` 等入口在本仓库不存在，
     已换成真实入口（`native/tests/test_translate.c`、`tools/probe-mt.mjs`、
     `tools/test-shell-app.ps1`）。
  7. 原文 §B9 的错误码表没有 `55000000` 这一档；按 `tools/probe-mt.mjs` 的实测补齐了
     「它是整个鉴权 / 权限家族的外壳，要按报文定性」这条。
  8. 原文 §B11 的验收入口写「C 级 `node tools/probe-page.mjs --translate-page --key off` /
     `--key on --test` 两档」「A 级 `node tools/ui-static-check.mjs` 钉缺 Key 置灰清单」——
     本仓库的 `tools/probe-page.mjs` **没有 `--translate-page` 子命令**（翻译页走
     `--manager --manager-tab translate`），`tools/ui-static-check.mjs` 也**没有**翻译页那一节
     （目前只有 ①–⑧ 节）。已按现存入口改写，并把「那三条 A 级检查还没有标准」
     作为未完成项明写在 §B11。
