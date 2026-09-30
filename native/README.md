# native/ · C 内核

> 这一层是 0.2.1 的**全部业务逻辑**。界面（`web/`）与各平台的外壳只做显示与转发 ——
> 检查标准见 [`../docs/adr/ADR-001-kernel-boundary-and-binding-generation.md`](../docs/adr/ADR-001-kernel-boundary-and-binding-generation.md)。
>
> ⚠️ 版本号**只**住在 `native/include/dsh_lookup.h` 的 `DSH_VERSION_STRING`（**自动生成**，
> 源头是 `abi/lookup.abi.json`）。本文里写的那个数只是给人看的 —— **判断当前是哪一版看那份头**，
> 别拿本文抄下来的数当依据（它就是这么漂起来的）。

## 术语表（本文用到的词）

> 完整词表与"旧词 → 新词"对照见 [`../docs/design/术语表.md`](../docs/design/术语表.md)。

| 术语 | 一句话解释 | 在本项目里指什么 |
| --- | --- | --- |
| **内核** | 用 C 语言写的核心程序库，业务逻辑都住在里面 | 本目录 `native/` |
| **接口** | 一组可以被别人调用的函数 | 接口定义里那 49 个 `dsh_*` 函数 |
| **接口定义** | 唯一手写的一份清单，写明内核有哪些函数、每个函数收什么参数、返回什么 | `abi/lookup.abi.json` |
| **自动生成的接口代码** | 由脚本从接口定义算出来的文件，不许手改 | `include/dsh_lookup.h` 等 |
| **绑定（binding）** | 让别的编程语言能调用 C 函数的那层代码 | C# 那边在 `shell/Lookup.Interop/` |
| **单元测试** | 只测某个函数或某个模块、不需要界面的测试 | `tests/test_*.c` |
| **测试分级（A / B / C / D）** | 按"这条测试需要什么才能判"分的四层编号 | 分级表在项目规范  |
| **测试用词典（fixture）** | 专为测试准备的输入文件，一个字节都不许改 | `testdata/` 里那些 `.mdx` / `.mdd` |
| **标准答案文件（golden file）** | 对照测试用来比对的"标准答案" | `tools/golden/baseline/golden-baseline.json`（来源记录见同目录 `provenance.md`） |
| **对照测试** | 把新实现和参考实现在同样输入下比结果，要求逐字节相同 | `tools/golden-gate.ps1` |
| **参考实现** | 上一代那一份实现；如今只在"重新生成产物"时才用它 | 上一代的 C# 实现，**不随本仓库发布** |
| **诊断脚本（probe）** | 只查看程序当前状态、不下结论的小脚本 | `tools/wsl-run-test.sh`、`tools/probe-page.mjs` |
| **真实程序** | 真起一个程序来验，不是模拟 | `tools/test-shell-app.ps1` |
| **宿主程序** | 承载界面并把它显示出来的那个真实程序（`host`） | `shell/Lookup.App` |
| **通信桥** | 页面与宿主程序之间传消息的那条通道 | `web/bridge.js` |
| **虚拟主机** | 把本地文件当成一个网址来提供，页面按网址加载 | `https://lookup.invalid/…` |
| **DLL** | Windows 上的动态链接库，程序运行时才加载进来的代码文件 | `dist/win-x64/dsh_lookup.dll` |
| **交叉编译** | 在一种系统里编出另一种系统能用的程序 | 在 WSL（Linux）里编 Windows 用的 DLL |
| **ASan / UBSan** | 编译器自带的两把"放大镜"：ASan 查内存越界与泄漏，UBSan 查未定义行为 | `tools/wsl-make-test.sh asan` |
| **arena** | 一次申请一大块、自己切成小块用的内存区 | `json_reader` 的分块内存 |
| **哈希 / SHA-256** | 把一份内容算成一串固定长度的"指纹"，内容变一个字节指纹就变 | 词典 id、录音比对都用它 |
| **落点** | 这段文字在指定词典里**会落到哪条词条** | `dsh_engine_resolve` |
| **借查** | 在当前词典没查到，就去别的词典里查一次，**不改"当前词典"** | `dsh_engine_borrow`、`dsh_engine_probe` |
| **探路** | 打开一本词典问一条就走、不把它留在内存里（单本预算 120 毫秒） | 同 `dsh_engine_probe` |
| **查词通道** | 「当前词典 → 借查别的词典 → 机器翻译」这条自动往下走的链 | `engine/dsh_fallback.c` |
| **不带 `.mdd` 的词典** | `.mdx` 旁边散放着 css / 字体 / 图片、没有 `.mdd` 的那类词典 | `dict/dsh_sibling.c` |
| **三层音源** | 发音的三条来源，按顺序试：词典自带录音 → 豆包在线语音 → 系统语音 | `engine/dsh_speech_*.c` |
| **音频预处理** | 把取到的音频字节整成能播放的 WAV（含 `.spx` 解码） | `dsh_audio_prepare` |
| **词条正文** | 内核拼好的、可显示的那份词条内容 | `dsh_engine_entry_document` |
| **词条** | 词典里的一条记录（一个词及其释义） | `.mdx` 里的一条 |
| **词库** | 用户导入的词典清单 | 设置里的 `dicts` |
| **音色** | 系统或在线语音里"用哪个声音念" | 设置里的 `doubaoSpeaker*` |
| **JSON / JSONL** | JSON 是一种用文本表示数据的通用格式；JSONL 是一行一条 JSON 的文件 | 接口出入参都是 JSON；历史用 JSONL |
| **PCM / WAV** | PCM 是没有压缩的声音数据；WAV 是装了 PCM 的音频文件 | `.spx` 解码后产出 WAV |
| **键名** | 词典里用来查的那串字（通常是词本身） | `.mdx` 的索引里存的就是键名 |
| **词块 / 记录块** | MDict 文件内部的存放单位：词块放键名，记录块放释义正文 | `.mdx` 的键区与记录区 |
| **码点** | 字符在 Unicode 表里的编号 | 音节分隔点那 6 个常量各对应一个码点 |
| **转义** | 把会被当成标记的特殊字符换成安全的写法 | HTML → 纯文本时要做 |
| **符号链接 / 目录联接** | 一种"快捷方式"式的文件 / Windows 上的目录快捷方式 | 不带 `.mdd` 的词典读同目录文件时要逐段查它们 |
| **MIME 类型** | 给文件类型起的标准名字（如 `image/png`） | 资源按扩展名映射到 MIME |
| **ETag** | 给一份资源算的版本号，用来判断它有没有变 | 资源接口的一个出参 |
| **Range** | 只取资源的一段（配合 HTTP 的 206 / 416 回应） | 音频、图片可以只取需要的部分 |
| **流式** | 一边读一边算，不必先把整份读进内存 | SHA-256 是流式算的 |
| **决策表** | 把"什么情况做什么"逐条列出来的表 | 查词通道的下一步就靠它 |
| **覆盖率** | 测试跑到的代码占全部代码的百分比 | `native-build.ps1 -Cover` 用 gcov 算 |
| **import library** | Windows 上让程序在编译期找到 DLL 里那些函数的"索引文件" | 与 `dsh_lookup.dll` 一起产出 |
| **工具链 / sysroot** | 编译程序用的那一整套工具 / 交叉编译时用的目标系统的头文件与库 | `.toolchain/mingw/sysroot`（也可以用 `DSH_MINGW_ROOT` 指到别处） |
| **落盘** | 写进硬盘文件（与"只放在内存里"相对） | 设置、查词历史都要落盘 |
| **栈** | 先进后出的一摞记录 | "返回上一篇词条"那摞 |
| **MDict** | 一种词典文件格式：`.mdx` 是词条库，`.mdd` 是配套的资源库 | 解析器读的就是它 |
| **C11 / C#** | C 语言的一个标准版本（2011 年）/ 微软的一种编程语言 | 内核是纯 C11；外壳与它生成的接口代码是 C# |
| **WinForms** | 微软的桌面窗口界面库 | 外壳 `Lookup.App` 用它做窗口 |

---

## 一句话认识这一层

接口定义里的 49 条接口分 8 组，**都在 `native/src/` 里有实现**：

| 组 | 条数 | 干什么 |
| --- | --- | --- |
| `core` | 4 | 版本号、释放内存、最后一条错误信息 |
| `parser`（`dsh_dict_*`） | 6 | 打开 / 关闭 `.mdx`、取词条、枚举键名 |
| `engine` | 10 | 引擎、设置落盘、词库清单（增删改名 / 排序 / 选当前） |
| `text` | 4 | 字形分区、音节分隔点、语种判定 |
| `lookup` | 7 | 落点、联想、借查、探路、完整查词链、词条正文、资源 |
| `speech` | 11 | 音源规划、词典自带录音、音频预处理、音量 |
| `history` | 2 | 查词历史的分页与清空 |
| `translate` | 5 | 机器翻译的状态 / 计划 / 回执 / 清缓存 / 取译文 |

接口定义里 49 条，**每一条都能在 `native/src/` 里找到实现**（少一条，编 DLL 时脚本会**如实列出来**，
不算失败 —— 「如实报未实现」是这个内核的硬规则）。

## 目录

```
abi/lookup.abi.json           唯一手写的接口定义 —— 在仓库根下
include/dsh_lookup.h          【自动生成】C 声明，禁止手改
vendor/speex/                 **第三方**：官方 libspeex 1.2.1 的解码路径（BSD 3-Clause）
                              —— .spx（Ogg Speex）的声音解码本体；**只许 src/audio/ 用**，
                              边界与理由见 vendor/README.md（这是**唯一**一处例外）
src/                          内核实现（纯 C11，不依赖系统）
  core.c                       内存层（dsh_release / dsh_version / dsh_last_error_message）
  mem_registry.{h,c}           内存分配登记表 —— "内核分配、内核释放"那条承诺的落点
  json_writer.{h,c}            JSON 输出的最小写入器（接口出参一律 JSON）
  json_reader.{h,c}            JSON **读取**（严格 RFC 8259 + 分块 arena）
                               —— 接口里 settings_set / dict_add 收的是 JSON，
                               解析必须在核内做（否则"哪些字段合法、越界怎么夹回"
                               就跑到界面层去了）
  dict/dsh_mdx.{h,c}           MDict（.mdx / .mdd）解析器：头部 / 键区 / 记录区 / 词块 / 记录块
                               —— ⚠️ **两条取记录的路**：`dsh_mdx_fetch`（文本，`@@@LINK`
                               那套）与 `dsh_mdx_fetch_raw`（**原始字节**，资源用）。
                               `.mdd` 里的图片/音频是二进制，走文本那条会被 UTF-16LE
                               解码器毁掉
  dict/dsh_dict.c              接口里 parser 组那 6 个 dsh_dict_*
  text/dsh_textcodec.{h,c}     文本 → UTF-8（UTF-8 / UTF-16LE / GB18030 / cp950）
  text/dsh_textcodec_tables.{h,c} 【自动生成】码表（tools/make-textcodec-tables.py）
  compress/dsh_inflate.{h,c}   DEFLATE（RFC 1951）解压，不依赖任何库
  compress/dsh_lzo1x.{h,c}     LZO1X 解压（逐行移植自 js-mdict / NSpeex 那条线）
  crypto/dsh_ripemd128.{h,c}   RIPEMD-128（MDict 加密键块**必须**用它）
  crypto/dsh_sha256.{h,c}      SHA-256（流式；词典的内容哈希 id 用它）
  dict/dsh_dict_id.{h,c}       词典 id = **整份文件内容的 SHA-256**
                               —— id 认内容不认路径：换目录、改名都不丢设置
  dict/dsh_mime.{h,c}          资源扩展名 → MIME（表与参考实现的 `MimeTypes` 逐条对应）
  dict/dsh_sibling.{h,c}         **不带 .mdd 的词典**读同目录散放文件的回落（css/字体/图片）
                               —— 四道检查：扩展名白名单（**不许有 .js**）/ 拒绝对路径 /
                               归一化后仍在词典目录内 / 逐段查符号链接与目录联接
  dict/dsh_entry_doc.{h,c}     词条正文（移植参考实现的 `EntryDocument`）：
                               拼装 / 三条网址 / 音频键抠取
  dict/entry_assets.h          【自动生成】基础样式与通信桥脚本（从参考实现反射读出来，原样）
  audio/dsh_audio.{h,c}        发音的**键名层**：词条里的音频引用（sound:// / snd:// /
                               <audio name=…>）、口音与例句判定、候选扩展名顺序、挑哪一条念
  audio/dsh_speex.{h,c}        **Ogg Speex → 16 位 PCM WAV**（容器与打包那一半：Ogg 页重组 /
                               Speex 头字段 / 逐帧循环 / WAV 封装）
                               —— 播放速率取**文件头里写的那个**
  engine/dsh_settings.{h,c}    设置模型（住在核内；界面不许自己拼一份）
                               —— 含**未识别字段的原样搬运**（否则用户设置会被抹掉）；
                               也含发音那一节（口音偏好 / 音色 / 豆包 Key）
  text/dsh_language.{h,c}      字形分区 / 词典标题线索 / 语种判定（连判断依据一起给）
  text/dsh_textutil.{h,c}      音节分隔点 / 按词计数 / 单字符
  text/dsh_html.{h,c}          HTML → 纯文本 / 实体还原 / 转义
  engine/dsh_fallback.{h,c}    **查词通道的决策表**（移植参考实现的 `FallbackPlan`）
                               —— 纯函数：只回答"下一步做什么"，不查词典、不联网
  engine/dsh_text_api.c        接口里 text 组那 3 条（只管包 JSON 与错误码）
  engine/dsh_dicts.{h,c}       **加载缓存 + 落点解析**（`@@@LINK=` 跟到 16 层、大小写变体）
                               —— resolve / suggest / probe / lookup 共用这**同一条**解析路径，
                               接口里那条不变式（问落点与真去查，结果必然相同）靠它成立
  engine/dsh_lookup_api.c      接口里 lookup 组前四条：resolve / suggest / probe / lookup
                               —— probe 走"借一次用完就关"，绝不把词典留在内存里；
                               完整链的**决策**在 dsh_fallback.c，这里只按结论去干活
  engine/dsh_resource_api.c    接口的 `resource`：按 key 从 `.mdd` 卷或词典旁边散放的文件取一段字节
                               —— MIME / ETag / 按字节范围取段都在核内（宿主只管发 206 / 416）
  engine/dsh_entry_api.c       接口的 `entry_document`：把上面那些零件接成一份可渲染文档
  engine/dsh_speech_api.c      接口的 `speech_dict_audio`：这条词条自带的原录音在哪
                               —— 换扩展名找一遍 × 资源键名的六种写法，**例句不算词目发音**
  engine/dsh_translate_api.c   接口里 translate 组那 5 条（机器翻译）
  engine/dsh_engine.{h,c}      接口里 engine 组那 9 条：引擎、设置落盘、词库清单
                               + dsh_engine_internal.h（只给核内看的接口 ——
                               对外签名**只许**由自动生成的接口头决定）
  platform/dsh_file.{h,c}      文件信息 / **原子写文件** / 路径工具
                               —— 设置文件必须先写临时文件再改名，否则断电会留半份
  platform/dsh_time.{h,c}      当前 UTC 毫秒（设置里的时间戳用）
  platform/dsh_mapfile.{h,c}   文件只读映射（Windows: CreateFileMapping / Linux: mmap）
                               —— 读路径上**唯一**的系统相关代码；有了它，「每次读都要先分配
                               一块内存」这件事就不再出现在每次都走的必经之路上
  platform/dsh_sleep.{h,c}     让出 CPU 一小会儿（系统差异只允许出现在这一层）
tests/                         内核自带的单元测试（不用任何测试框架）
  test_*.c                      每支自己打印 `N 项，失败 M`，非零退出码表示失败
  test_inflate.c                【自动生成】DEFLATE 向量
  html_vectors.h                【自动生成】HTML 标准答案表
  entry_doc_vectors.h           【自动生成】词条正文标准答案表
Makefile                       在 WSL / Linux 下构建

../web/                        界面（虚拟主机 https://lookup.invalid/… 的根目录）
../shell/                      各平台宿主（这一版只有 Windows）
  Lookup.Interop/              【部分自动生成】C# 绑定
  Lookup.Host/                 操作系统相关那一半：VirtualHost + ShellAssets + Dispatch
                               —— **一行 WebView2 都没有**，所以 D 级那条检查能不开窗口直接驱动它
  Lookup.App/                  外壳本体：net48 WinForms + WebView2（唯一允许用 WebView2 的地方）
                               —— 也带 `--selfcheck`（那条"真实程序"检查跑的就是它）
```

> ⚠️ **标着【自动生成】的那些文件不许手写**：改内核源码不会动它们，要**重跑对应的生成脚本**。
> **重建 / 恢复一棵树之后，自动生成的文件必须拿这份清单逐条核对** —— 不许凭记忆
> （这条是踩过一次恢复才发现的那种坑）：
>
> | 自动生成的文件 | 生成脚本 |
> | --- | --- |
> | `include/dsh_lookup.h` · `web/src/shared/abi.ts` · `shell/Lookup.Interop/DshLookup.g.cs` · `docs/api/abi.md` | `node tools/gen-bindings.mjs` |
> | `src/text/dsh_textcodec_tables.{c,h}` | `python3 tools/make-textcodec-tables.py` |
> | `src/dict/entry_assets.h` · `tests/entry_doc_vectors.h` | `powershell -File tools/make-entry-assets.ps1` |
> | `tests/html_vectors.h` | `powershell -File tools/make-html-vectors.ps1` |
> | `tests/test_inflate.c` | `node tools/make-inflate-test.mjs` |
> | `tools/golden/baseline/golden-baseline.json` | `powershell -File tools\make-golden.ps1` 再 `sh tools/run-golden.sh <仓库的 WSL 路径> --regen`（**特权**：基线只许由参考实现产出，见 `tools/golden/baseline/README.md`） |
> | `testdata/**` | 已随仓库冻结（`tools/freeze-fixtures.ps1` 只**校验**；`-UpdateManifest` 才按现有文件重写清单） |
>
> **少一份自动生成的文件，`make test` 不会红** —— `Makefile` 用 `wildcard` 收 `tests/test_*.c`，
> 少一个文件就少编一支。所以"全部通过"与"生成的文件齐了"是两件事，得各验一遍。
>
> ⚠️ 上表与目录里的 `docs/api/abi.md` 就是接口定义的可读版；
> 路径按原样保留（改名要动生成脚本，不在文档改动的范围内）。

## 构建与测试

```powershell
# 从 Windows（推荐入口：会先按接口定义重新生成，再在 WSL 里编译并跑测试）
powershell -File tools/native-build.ps1            # 一次完整验收
powershell -File tools/native-build.ps1 -SkipGen   # 只改了 C 代码时省一趟
powershell -File tools/native-build.ps1 -Asan      # ASan + UBSan 重编再跑
powershell -File tools/native-build.ps1 -Cover     # gcov 覆盖率
```
```bash
# 在 WSL / Linux 里直接（等价，少一层包装）
cd native && make            # 编译期自检 + 单元测试
make check                   # 只做编译期自检
make test                    # 只跑单元测试
make asan                    # ASan + UBSan
```

⚠️ **从 PowerShell 里调 WSL，宁可调现成的小脚本，也别把整条命令内联在 `bash -lc "…"` 里**：
那样引号要穿过 PowerShell → `wsl.exe` → bash 两层，路径还得手工转成 `/mnt/<盘>/…`，
两件事都容易出错，而且出错时症状离原因很远。下面这些脚本**在 WSL 里跑**、路径自己从脚本位置推：

```bash
sh tools/wsl-make-test.sh        # = make test，逐文件实测结果存在 /tmp/dsh-b-test.txt
sh tools/wsl-make-test.sh asan   # = make asan，实测结果存在 /tmp/dsh-b-asan.txt
sh tools/wsl-make-test.sh check  # = make check（编译期自检 + 边界检查）
sh tools/wsl-build-win.sh        # = 交叉编 Windows DLL（Release）
sh tools/wsl-run-test.sh history # 只跑一个测试二进制（不重编），把它的原话打出来
sh tools/wsl-debug-test.sh history  # 取崩溃栈：有 gdb 用 gdb，没有就让 ASan 单独编这一份
```

从 PowerShell 那一侧叫它们：

```powershell
wsl.exe -- bash <仓库根的 WSL 路径>/tools/wsl-make-test.sh test
```

**为什么经 WSL**：本机没有任何 Windows C 编译器（`cl` / `gcc` / `clang` / `cmake` 全缺，
也没装 Visual Studio），而 WSL 的 Ubuntu 24.04 里 `gcc 13.3 + make + ASan/UBSan/gcov` 齐全。
内核是纯 C11、不依赖系统，所以"在 Linux 里编"不影响将来发给 Windows —— **只有平台那一层分叉**。

⚠️ 自动生成接口代码用的是 **Windows 的 node**（WSL 里没装 node，也没有免密 sudo 装不上，
且不需要为它装 —— 生成脚本只读 `abi/lookup.abi.json`、只写文本文件，与系统无关）。

## 四级测试各自的入口

| 级 | 入口 | 管什么 |
| --- | --- | --- |
| **A** 静态 / 决定类 | `make check` | 自动生成的声明合法可调用、返回值类型正确、常量与分隔点码点与接口定义一致 |
| **B** 纯逻辑 | `make test` | `tests/test_*.c` 全部单元测试（逐文件打 `N 项，失败 M`） |
| **B′** 内存检查 | `make asan` | ASan + UBSan，**必须同时 grep `runtime error`** |
| **C** 单点 / 时序 | `node tools/probe-page.mjs …` | 接到真实程序的渲染进程里**只看当前状态、不下判断** |
| **D** 真实宿主 | `tools\test-windows-dll.ps1` | Windows 上的真 DLL + C# 绑定 + 操作系统那一半（**不开窗口**） |
| **D** 真实程序 | `tools\test-shell-app.ps1` | 真实窗口 → 真实页面 → 通信桥 → 内核 |

⚠️ **两道 D 级检查不能互相替代**：一条把内核单独拿出来喂（不开窗口），
只有另一条验得了"渲染进程里到底发生了什么"。

各支测试的具体条数由它们**自己打印**（跑 `make test` 看输出）—— 那份数**不在这里抄一份**：
抄下来的会漂，而且抄错的时候没人会发现（本文开头那条版本号就是这么漂起来的）。

## 出 Windows DLL（交叉编译，不需要 root、也不需要 Visual Studio）

```bash
# 在 WSL 里、仓库根下（仓库根不给就按脚本自己的位置推，见脚本头部注释）
sh tools/build-windows-dll.sh Release
# 也可以显式给仓库根：sh tools/build-windows-dll.sh <仓库根的 WSL 路径> Release
```

产物在 `dist/win-x64/`：`dsh_lookup.dll` + `dsh_lookup.lib`（import library）+
`dsh_lookup.def`（**本轮生成**的导出列表）；依赖只有 `KERNEL32.dll` 与 `msvcrt.dll`
（`-static-libgcc`，不带 `libgcc_s_*.dll`）。

### 交叉工具链：脚本去哪找它（三段，按优先级）

| 优先级 | 来源 | 什么时候用 |
| --- | --- | --- |
| 1 | 环境变量 **`DSH_MINGW_ROOT`** | 工具链装在别处、或要钉死某一套 |
| 2 | **`PATH`** 里的 `x86_64-w64-mingw32-gcc` | 系统已经用 `apt install gcc-mingw-w64-x86-64` 装过 |
| 3 | 项目内 **`.toolchain/mingw/sysroot`** | 本仓库的便捷默认（`.gitignore` 挡着，**不进版本库**） |

`DSH_MINGW_ROOT` 指 **sysroot**、指 **MinGW 前缀**、或**直接指那个 gcc 可执行文件**都认：

```bash
DSH_MINGW_ROOT=/opt/mingw sh tools/build-windows-dll.sh Release
```

⚠️ 显式指定**不回落**：指错了当场报错，并列出它找过的两个位置（`usr/bin/` 与 `bin/` 下三个候选名），
**不会**静默改用别处的编译器 —— 静默回落会编出一个「不是你要的那套」的 DLL，比编不出来难查得多。
三段都没找到时退出码 2，并把「找过哪三处 + 下一步怎么办」一次说清。**不留死胡同**：原来那句
「先跑 `tools/build-mingw-sysroot.sh`」指向一个本仓库里根本不存在的脚本、照它做也走不通，已删掉。

### 装工具链（可复现，不需要 root；实测跑通过）

第一次要先准备交叉工具链（**不需要 root**）：`apt-get download` 只写当前目录、
`dpkg-deb -x` 也只要写权限 —— 所以没有免密 sudo 也能做。实测在 WSL Ubuntu 24.04.4 上跑通：

```bash
cd .toolchain/mingw/deb              # 在仓库根下；这一层不进版本库，自己建
apt-get download gcc-mingw-w64-x86-64-win32 mingw-w64-x86-64-dev \
                 binutils-mingw-w64-x86-64 mingw-w64-common gcc-mingw-w64-base
for d in *.deb; do dpkg-deb -x "$d" ../sysroot; done      # → .toolchain/mingw/sysroot
```

下载约 51 MB、解开约 330 MB。解出来的编译器自报
`x86_64-w64-mingw32-gcc-win32 (GCC) 13-win32`（包版本 `13.2.0-6ubuntu1+26.1`，
目标三元组 `x86_64-w64-mingw32`）——**这个包里没有 `-posix` 变体**，脚本会自己挑 `-win32`。

有 root 的那条路更省事：`sudo apt install gcc-mingw-w64-x86-64`（约 250 MB）。装完
`/usr/bin/x86_64-w64-mingw32-gcc` 就在 `PATH` 里（那是 `update-alternatives` 的入口），
脚本第二条就认它。

⚠️ 换别的版本之前先想清楚：产出的 DLL 只许依赖 `KERNEL32.dll` 与 `msvcrt.dll`
（`objdump -p dist/win-x64/dsh_lookup.dll | grep 'DLL Name'` 一查就知道）。

### 装完先预检，别直接编内核

```bash
sh tools/build-windows-dll.sh --preflight     # 不给仓库根就按脚本自己的位置推
```

它报出「这一轮用的是哪套、什么版本、头文件与库在哪、sysroot 在哪」，并**真编一个最小 DLL**
（撞一遍 C 头文件、Windows 头文件与链接期）。光看「文件在不在」判不出可用性：头文件缺一个、
库路径错一格，都要到编内核时才现形，那时已经白编了几十个 `.c`。顺带还查 `python3` / `nm` /
`objdump`（生成 `.def` 与核对 PE 导出表要用）。

> **工具链二进制不进版本库**（`.toolchain/` 在 `.gitignore` 里），仓库里存的是**版本与装法**。
> 换过机器、或者怀疑「手上这个 DLL 是哪套工具链产的」时，`--preflight` 的输出就是答案。

### Windows 侧验收

```powershell
powershell -File tools/test-windows-dll.ps1     # 真 DLL + C# 绑定 + 操作系统那一半
powershell -File tools/test-shell-app.ps1       # 真实程序端到端
```

`test-windows-dll.ps1` 开头有一道**新鲜度检查**：DLL 比内核源码旧就直接报错 ——
免得拿旧二进制验出一个误报失败。

## 这一版做到哪、哪还没有

⚠️ **这一节原来列着两条「还没做」，两条都已经不是事实** —— 那是从旧工作目录带过来的结论，
搬过来之后没人再核对。下面把它改成「**去哪自己查**」，别再照抄旧结论：

| 原来写着「还没做」的 | 现在的实情 | 怎么就地核对 |
| --- | --- | --- |
| `Encrypted=2` 的键信息块解密 | **已经接上**：`decode_key_info_block()` 在 `encrypted == 2` 时先 `mdx_decrypt()`，再按压缩类型分支（顺序与参考实现一致：先解密、再片段、最后解压） | 读 `native/src/dict/dsh_mdx.c` 的 `decode_key_info_block()` 与它的调用点 `read_key_infos()`；对照测试覆盖 `testdata/variants/v2-enc2.mdx`（`v2-enc1.mdx` 是另一种：记录块加密） |
| LZO 压缩的词块 | **已经接上**：键信息块与记录块**两条路**都调 `dsh_lzo1x_decompress()` | 同一个文件里的两处调用；对照测试覆盖 `testdata/variants/v2-lzo.mdx` / `v12-lzo.mdx` / `v2-lzo-record-zlib-key.mdx` |
| 翻译那一步「用不上时报 `DSH_E_NOT_IMPLEMENTED`」 | **没有这回事**：`grep -rn DSH_E_NOT_IMPLEMENTED native/src/` **一条都没有**（没有任何一条路返回它）。实情是两句话：① 四个条件（总开关 / 自动翻译 / 填了 Key / 语种支持）**全满足**时，内核**不查词、也不联网**（零依赖、没有 socket），只如实报出「该翻译了」（`via = translate` + `needsTranslate`），外壳走 `translate:text`（plan → HTTP → accept）再拼回同形状载荷；② 用不上时由 `translate_why` **四档分开说清原因**（总开关关着 / 只是不让它自动 / 没填 Key / 语种不在支持表里），**不许合并成一句、也不拿终态页装作翻过了** | 读 `native/src/engine/dsh_lookup_api.c` 里 `DSH_FALLBACK_TRANSLATE` 那一档与 `dsh_fallback_translate_why()`；再跑一遍那个 `grep` |

**已知差异只有两处，而且责任在参考实现那边**：`tools/golden/fixtures.txt` 是「哪些测试用词典
进对照测试」的唯一来源（21 项：v2.0 / v1.2 × zlib / lzo / none × UTF-8 / UTF-16LE / GBK / BIG5 ×
无加密 / `Encrypted=1` / `Encrypted=2` × 普通索引 / 归一化索引 × 单词块 / 多词块 / 记录跨块 /
`@@@LINK` / 两卷 `.mdd`）。其中 `testdata/v2-multiblock.mdx` **稳定地报两处差异**，
`tools/golden/compare-golden.py` 顶部的 `WHITELIST` 逐条记着「哪两处、各多少字节、为什么」——
根因是参考实现只从「记录起点所在的那一块」取字节、把跨块记录截断了，**C 侧才是完整的那一方**
（三条证据写在白名单上面那段注释里）。

> 这一节只负责**把你指到会说话的地方**（代码、`fixtures.txt`、`compare-golden.py` 的白名单；
> 要读数就跑 `tools/golden-gate.ps1`）。**覆盖范围与已知差异以那些文件为准，不看这一节。**

---

## 更细的东西放哪

| 想知道的东西 | 去哪看 |
| --- | --- |
| 每个模块的实测条数 | 跑一次 `make test`，各支测试自己打印 |
| 交叉工具链要哪一版、装在哪、怎么预检 | 本文上面「出 Windows DLL」那一节（`--preflight` 的输出就是「这一轮用的哪套」的答案） |
| 在 `-Wall -Wextra -Werror -pedantic` 下踩过的 C 的坑 | 各模块顶部的注释 |
| 内核的边界规则（什么算业务逻辑、什么算界面） | `../docs/adr/ADR-001-kernel-boundary-and-binding-generation.md` |
| 接口逐条说明（参数、返回、错误码） | `../docs/api/abi.md`（自动生成） |
