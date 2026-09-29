# LookupApp 代码与工程审查（2026-09-29）

## 范围与结论

审查对象：Git 仓库 `LookupApp/`，当前 HEAD `2ca9da8`；比对来源：兄弟目录 `查词软件/0.2.0/`，以及其根目录的开发规范和工具。产品源码未修改；未合并目录、删除旧版本、提交或推送。

这是安全相关路径的定向审查与工程完整性核对，不是全量渗透测试。没有调用在线翻译/语音服务，也没有使用用户真实配置。完整窗口验收运行 **0 次**：本轮没有产品改动，公开仓库也没有对应验收脚本。以下复现不表示整个项目已通过 ASan/UBSan。

实测：当前 Git 跟踪 243 个文件，其中 241 个与原 0.2.0 的同路径文件逐字节一致；另有 `.gitignore` 的差异，`.gitattributes` 在原 0.2.0 中无同路径文件。保留的业务源码没有发现拷贝差异。因此“过度精简”的主要问题是失去了测试、构建环境说明、验收、打包和设计依据，不是业务源码被删成了另一种实现。

原 `tools/make-public-repo.ps1` 明确记录了“最小量源码发布”的排除决定，包括测试、测试数据、验收/打包工具与内部文档。这种发布展示目录可以很小，但如果以后它成为唯一开发仓库，就应恢复工程维护能力。已删除会话无法核实；本报告依据现存文件，不声称恢复了那个会话的原话。

## 安全与可靠性发现

### F1 · P1：不可信词典脚本能够触发翻译动作

- 位置：`web/src/floating/main.ts:3817` 的消息处理器，`:3866` 的 `chip` 分支；`onEntryChip` 在 `:1582`，`renderTranslation` 在 `:1629`。
- 外部词条允许内联脚本及词典域脚本运行，见 `native/src/dict/dsh_entry_doc.c:143` 的 CSP，以及 `web/floating.html:83` 的 `sandbox="allow-scripts"`。这是已有功能设计，本身不等于漏洞。
- 消息处理只检查 `data.source === 'lookup-entry'`。这个字符串是发送方自行填写的，没有认证作用。词典脚本能够调用：

```js
parent.postMessage({
  source: 'lookup-entry', type: 'chip', action: 'translate',
  word: '由词典脚本指定的文本'
}, '*');
```

- 消息会调用 `onEntryChip('translate', ...)`，继而进入 `translateWord → renderTranslation → api.translateText`。翻译开关关闭、没有凭据时，内核会拒绝；开启翻译并配置有效凭据时，恶意词典可以诱导发送文本及消耗配额。换不同文本还可以避开按文本缓存。没有声称它能直接窃取 API Key。
- 验证：从现有 TS 源码抽取实际消息处理器，用 esbuild 去掉类型语法，再向它输入伪造消息，结果为 `forgedMessageReachedChipAction: true`。复现只验证消息到动作的分发；未启动 WebView2，也未发送真实 HTTP 请求。
- 修复方向：先校验 `event.source === reader.contentWindow` 并验证消息结构/长度/数值；但**只加 source 校验不能解决当前词典自身的恶意脚本**。涉及计费或外发内容的动作应由可信父页面的真实用户操作授权，建议将翻译按钮放在父页面，iframe 只提供展示及低权限事件。令牌如果交给词典脚本所在的同一 JS 环境，也不能当作可靠授权。`Event.isTrusted` 也不能直接代表用户点击授权。
- 旧版范围：只读检查确认 `0.1.3/web/src/floating/main.ts:4053,4083` 也有同类消息到翻译动作路径。未修旧版；0.1.0–0.1.2 未逐版核实，不推断其状态。

### F2 · P1：记录块累计长度可发生有符号整数溢出

- 位置：`native/src/dict/dsh_mdx.c:700–701`，`pack_acc += rb[i].pack_size` 与 `unpack_acc += rb[i].unpack_size`；同文件 `read_key_infos` 也需要检查条目数、压缩/解压长度的累计。
- `read_be` 对单个 8 字节整数允许到 `2^53`，但累计没有检查 `INT64_MAX - accumulator`。单项合法不表示总和合法。
- 合成文件共 **16,720 字节**，含 1,025 项记录块索引，每项解压长度 `2^53`。调用 `dsh_mdx_open` 即触发 UBSan：

```text
native/src/dict/dsh_mdx.c:701:18: runtime error: signed integer overflow:
9214364837600034816 + 9007199254740992 cannot be represented in type 'long int'
```

- 正常对照合成文件打开成功，累计长度为 2。异常文件只需要进入解析器，不需要真实词条查询。
- 影响：外部文件驱动 C 的未定义行为，后续记录范围和偏移会失真。已证明整数溢出；未证明可利用的任意读写或代码执行。
- 修复方向：加法前进行溢出检查，验证压缩范围在文件内、索引项数与索引字节长度匹配，并对不合理总量返回格式错误。不能只靠校验和警告兜底。
- 原 0.2.0 的同路径源码与此处一致；0.1.x 是另一套 C# 解析器，本 C 缺陷结论不直接套用到旧版。

### F3 · P2：很小的异常文件可诱导超大索引分配

- 位置：`native/src/dict/dsh_mdx.c:412–415` 允许最多 10,000,000 个词块，`:555–561` 在验证信息块能够容纳多少索引之前，就按声明数分配并清零。
- 346 字节的合成文件，把词块数改成 10,000,000，解析器尝试申请 **640,000,000 字节**。这不是压缩炸弹，而是对文件声明数量的过早信任。
- 验证通过链接器包装 `malloc`：记录并拒绝超过 16 MB 的申请，避免真正占用 640 MB。输出：

```text
audit refused allocation: 640000000 bytes
open_rc=-1
```

- 影响：真实程序若分配成功，后续 `memset` 会实际触碰整块内存，造成明显内存和界面响应压力；若失败则返回错误。没有为了复现使机器发生 OOM。
- 修复方向：先按信息块实际长度推导最大可能条数，再分配；对所有数组乘法进行大小检查；统一索引、单块、单词条和整体内存预算。`dsh_mdx_list_keys:1729` 按未经充分校验的总词条数分配，也应纳入同一次检查。

### F4 · P2（工程缺陷）：公开仓库不能按现有说明完整构建 DLL

- 位置：`tools/build-windows-dll.sh:14–23`。
- 脚本固定查找项目内 `.toolchain/mingw/sysroot/usr/bin/...`，不使用 PATH 中已安装的 MinGW。当前 LookupApp 没有这套工具链。
- 实测脚本退出码 2，报“找不到交叉 gcc，先跑 tools/build-mingw-sysroot.sh”；但这个安装脚本在当前仓库不存在，原工作目录排除日志/会话备份后的文件清单中也未找到。
- 修复方向：补可复现的工具链安装说明及预检，构建脚本支持明确的编译器路径或常规 PATH；工具链安装在受机器管理的位置，仓库保留版本/安装说明而非工具链二进制。不能把 `.toolchain/` 整目录补进 Git 当成解决方案。

### F5 · P2（工程缺陷）：生成绑定与接口定义检查失败

- 命令：`node tools/gen-bindings.mjs --check`，退出码 1。
- 差异已逐行核对：
  - `native/include/dsh_lookup.h:437` 注释残留 `$1`，生成器应输出另一段注释；`:591` 组标题用词不同。
  - `web/src/shared/abi.ts:102` 注释中的“诊断”与“诊断脚本”不同。
- 全部是注释差异，没有发现函数签名、枚举或常量漂移。本问题会让检查持续失败，也说明生成文件被脱离生成流程编辑过；不应误报成 ABI 内存破坏漏洞。
- 修复方向：从 `abi/lookup.abi.json` 重新生成四份文件，以 `--check` 进入 CI。生成注释需修改规格/生成器，不再手改产物。

## 加固项与文档漂移

- `shell/Lookup.App/WebViewBridge.cs:194` 的 `OnWebMessage` 没有在进入 `Dispatch.Handle` 前验证消息来源 URI；顶层导航也没有发现主动取消未知地址的处理器。应按窗口角色限制可访问的页面、消息来源及桥方法。当前已有 CSP/iframe sandbox/外部 HTTP 拦截，所以本轮不把“存在任意外域到宿主的完整利用链”当作已证明事实。
- Microsoft 的 [WebView2 安全指南](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/security) 要求验证文档来源和 Web 消息，并建议限制可导航来源。来源校验是桥接加固，不能替代 F1 中的用户授权边界。
- `README.md:18` 宣称全局热键，`shell/Lookup.Host/Lookup.Host.csproj` 却明确说明已经删除热键。应删除旧功能宣传。
- README 的 Encrypted=2、LZO“尚未接入”描述与现有 `decode_key_info_block`、`decompress_block` 的解密/LZO调用不一致。应通过恢复后的格式矩阵测试确认真实支持范围，再重写已知限制。

## 问题 1：是否保留之前的版本

不需要继续并排维护四份完整旧工作目录。以后使用一个主开发目录，版本用 Git 提交、标签和 Release 区分，不再每升版本复制整个工程。

但是**现在不要先删 0.1.3**：

- `tools/golden/GoldenDump.csproj` 直接编译 `../../../0.1.3/src/Dictionary/*.cs`。
- `EntryAssets.csproj` 和 `HtmlVectors.csproj` 直接引用旧版 `EntryDocument.cs` 与 `HtmlUtils.cs`。
- `tools/golden/fixtures.txt` 还引用 `../0.1.3/tools/MdxProbe/variants/` 的多种格式样本。
- `make-golden.ps1` 显式检查旧解析器源码；`package.ps1` 会调用对照验收。

推荐保留一份可恢复、含哈希清单的 0.1.x 归档；0.1.0/0.1.1/0.1.2 可以先退出日常开发目录。0.1.3 等依赖迁移后再退出。由于当前 Git 只有公开版首次提交，旧目录不会自动出现在 Git 历史里；应先做实际归档或导入历史，再谈删除原件。

将对照测试改为读取独立的冻结基线：保留测试词典、预期 JSON、生成来源/版本、SHA256 与已确认差异说明。默认回归应只运行 C 侧并比对基线，不每次构建旧 GUI。若仍需再生基线，单独保留最小旧解析器参考工程；不能由正在被测的 C 实现重生成自己的“标准答案”。

## 问题 2：具体补回清单

下列来源路径均相对 `查词软件/0.2.0/`，目标为 LookupApp 的同名路径；涉及旧版相对路径的工具必须改造后接入。

### 必须恢复的现有文件

| 来源 | 用途与处理 |
| --- | --- |
| `native/Makefile`、`native/README.md` | Linux 内核构建、检查、27 组单测、ASan/UBSan入口；README改成单版本工程说明 |
| `native/tests/test_*.c`（27个）、`abi_compile_check.c`、`html_vectors.h`、`entry_doc_vectors.h` | 恢复实际断言及测试向量；诊断用 `diag_*.c` 可随后按需恢复 |
| `testdata/test.mdx`、`test.mdd`、`big.mdx`、`tall.mdx`、`link.mdx`、`titled.mdx`、`kana.mdx`、`audio.mdx`、`audio.mdd`、`v2-multiblock.mdx`、`SHA256.txt` | 冻结测试数据；先确认样本生成来源和公开许可 |
| `tools/wsl-make-test.sh`、`native-build.ps1` | 标准单测/内存检查入口；适配可配置的WSL发行版和新目录 |
| `tools/ui-static-check.mjs` | UI结构、桥方法存在性等静态检查 |
| `tools/test-windows-dll.ps1`、`tools/WindowsDllTest/{WindowsDllTest.csproj,Program.cs,Runner.cs}` | DLL、绑定、宿主的真实集成验收 |
| `tools/test-shell-app.ps1`、`tools/make-sibling-fixture.mjs` | 真窗口/页面/内核验收；后者是验收脚本实际调用的样本生成器，不能漏 |
| `tools/package.ps1` | 重建并验证便携包；移除/改造旧版对照测试依赖后再用于公开仓库 |
| `tools/golden/{golden-dump-c.c,compare-golden.py,fixtures.txt,golden.json}` | C侧对照工具与基线候选；先核对 golden.json 的出处、内容与路径，冻结后改成独立回归 |
| `0.1.3/tools/MdxProbe/variants/` 中 fixtures.txt 引用的12个样本 | 移到新仓库测试数据目录，更新 fixtures.txt 与基线中的路径；覆盖版本/压缩/编码/加密矩阵 |

`run-golden.sh`、`check-golden.sh`、`golden-gate.ps1` 要同步改为“运行当前 C → 对比冻结基线”，不要原样复制后继续偷偷依赖兄弟 0.1.3。`make-golden.ps1`、`make-html-vectors.ps1`、`make-entry-assets.ps1` 及三个旧 C# 工程属于**基线再生工具**，可放独立参考目录/归档；它们不是日常运行应用的必要依赖。

### 建议恢复或重新整理

- `tools/probe-page.mjs`：可重复的定向UI诊断。
- `tools/probe-startup-flash.ps1`：若以后继续修改启动/几何，恢复相应定向验收。
- 单版本 `AGENTS.md` 或 `CONTRIBUTING.md`：保留层次边界、接口唯一来源、测试入口、词典沙箱、安全与凭据约束。不要照搬整份多版本规范，也不要保留已经失效的路径或要求每轮索取真实Key的流程。
- `CHANGELOG.md`：从公开0.2.0开始记录；旧0.1.x历史放归档，不伪造已有Git历史。
- 从 `查词软件/docs/` 中提炼设计文档：不带mdd与机器翻译、查词兜底与历史、发音设计与豆包接入、原生编辑命令。整理到新的 `docs/`，去掉个人路径/会话要求，保留决策、边界、API来源与理由。
- 从 `开发记录-0.2.x.md` 附录 A 提炼仍有效的陷阱到维护文档；完整开发记录可留私人归档，不要求把约900KB日志公开。
- 新增CI：生成绑定检查、UI静态检查、27组内核测试与sanitizers、冻结基线比对；Windows构建与无联网集成检查单独配置。真实在线服务测试显式开启并安全提供凭据。
- 补工具链安装/预检、Node/.NET/编译器版本说明，锁定依赖并提供从干净环境开始的构建和打包命令。当前 `web/package-lock.json` 已在仓库，应继续保留。

### 不需要补回

`logs/`、`native/build*`、`bin/`、`obj/`、`dist/`、`.toolchain/`、`node_modules/`，测试运行产生的 `testdata/tmp-*`、`tools/golden/out/`，会话逐字记录、旧交接待办、临时截图/注释迁移工具，都不属于源码库必需内容。尤其不能把带设置、路径或凭据的临时配置一起拷回 Git。

## 问题 3：是否合并目录

推荐收敛成**一个开发仓库 LookupApp**，但按内容迁移，不把 `查词软件/` 整棵塞进仓库。

建议形状：

```text
LookupApp/             # 唯一日常开发目录，不再套0.2.0子目录
  abi/
  native/              # 源码、Makefile、tests
  shell/
  web/
  testdata/            # 自有/可公开的合成样本和冻结基线
  tools/               # 构建、诊断、验收、打包
  docs/                # 维护说明、设计依据、安全审查
  .github/workflows/   # 使用GitHub时的CI
  AGENTS.md
  CHANGELOG.md
  README.md

独立归档位置/          # 日常仓库之外
  0.1.x版本快照
  原始开发记录和会话记录
  需要时再生基线的旧参考工程
```

迁移顺序：

1. 归档现有两个目录、记录Git提交与关键文件哈希；保护未入Git的旧历史。
2. 将上述测试、数据、构建/验收工具迁入 LookupApp；全局找出0.1.3、0.2.0及兄弟目录依赖，逐项改造。
3. 冻结独立对照基线与全部格式样本，默认回归脱离旧目录。
4. 修复本报告的安全问题、构建入口与文档漂移。
5. 从干净克隆验证：依赖安装→绑定检查→静态检查→内核测试/ASan/UBSan→基线→DLL/壳构建→Windows验收→便携包正常启动。不能只在原作者带缓存的目录验证。
6. 通过后，`查词软件/` 退为归档并停止写入；以后只改 LookupApp。不要继续用 `make-public-repo.ps1` 覆盖它，该脚本会递归清空目标目录，默认目标正是 LookupApp。

若原始历史/文档不适合公开，优先脱敏后选择性迁移；不必为了目录统一公开全部私人记录。运行环境和编译器二进制继续由机器管理，Git保留源码、配置、锁文件、测试和复现说明。

## 本轮验证与复现入口

复现材料保存在已被 `.gitignore` 忽略的 `logs/audit-2026-09-29/`。正常/异常词典均由 `repro_mdx.py` 合成，没有商业词典正文。

```powershell
node tools/gen-bindings.mjs --check
node logs/audit-2026-09-29/binding-diff.mjs
node logs/audit-2026-09-29/repro_message.mjs
wsl.exe -d Ubuntu --cd /mnt/c/<本仓库的 WSL 路径> bash logs/audit-2026-09-29/run-repro.sh
wsl.exe -d Ubuntu --cd /mnt/c/<本仓库的 WSL 路径> bash logs/audit-2026-09-29/run-alloc.sh
```

绑定检查和两个异常词典复现返回非零是预期证据，不是通过结果。JS复现依赖当前已安装的 `web/node_modules/esbuild`。没有修改产品源码，没有执行真实在线服务测试，没有运行完整窗口验收。
