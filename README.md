# LookupApp

> 一个 Windows 桌面查词工具：读 **MDict 词典**（`.mdx` / `.mdd`），浮窗取词、词条发音、机器翻译、
> 查词历史、词库管理。本仓库是 **0.2.0** 的**源码发布** —— **C11 内核 + C# 外壳 + WebView2 界面**。

![悬浮窗](release/screenshots/screenshot-floating.png)

## 它做什么

| 功能 | 说明 |
| --- | --- |
| **浮窗查词** | 常驻小窗，输入即查；正文里的链接、选中文字都能接着查；窗口能吸边、能收成胶囊 |
| **词库管理** | 导入 / 移除 `.mdx`、多本词典共存、拖动排序、逐本展开看路径与大小、当前词典切换 |
| **发音** | 三层音源：**词典自带录音**（`.mdd` 里的原录音）→ **系统语音**（离线合成）→ **在线语音**（默认关，需自备凭据） |
| **机器翻译** | 整词或整段译文，作为「伪词条」查到（需自备凭据，默认不联网） |
| **查词历史** | 记录「词条 + 词典」，能回放；词典被移走 / 文件丢了会如实说明并给出恢复办法 |
| **兜底通道** | 当前词典查不到时自动去别的词典「借查」，问不到才说没有 —— **「没问完」与「没有」绝不含糊** |
| **托盘与快捷键** | 托盘菜单、全局热键唤起、查词窗与选项窗的键盘操作 |

<details>
<summary>界面截图（点开看）</summary>

| 词库 | 常规 |
| --- | --- |
| ![词库](release/screenshots/screenshot-options-dicts.png) | ![常规](release/screenshots/screenshot-options-general.png) |

| 语音 | 翻译 |
| --- | --- |
| ![语音](release/screenshots/screenshot-options-speech.png) | ![翻译](release/screenshots/screenshot-options-translate.png) |

</details>

## 怎么装

**免安装（推荐）**：到本仓库的 Releases 下载 `LookupApp-0.2.0-portable.zip`，解压到任意目录，
双击 `LookupApp.exe` 即可。配置与缓存写在 `%APPDATA%\LookupApp`。

**系统要求**

- Windows 10 / 11（x64）
- **Microsoft Edge WebView2 Runtime** —— Windows 11 自带；Windows 10 若没装过，去
  [微软官网](https://developer.microsoft.com/microsoft-edge/webview2/) 装一次（Evergreen 版）
- 不需要安装 .NET Framework（Windows 10/11 自带 4.8）

第一件事通常是「选项」→「词库」里导入你自己的 `.mdx` 词典；程序**不附带任何词典**。

## 能读哪些词典文件

- `.mdx`（词条库）、`.mdd`（资源库：图片 / 音频 / 样式）
- **不带 `.mdd` 的词典**：`.mdx` 旁边散放的 `.css` / 字体 / 图片 / **`.js`** 也能读
  （样式与图片**先从 `.mdd` 里找**，里面没有才去旁边找）。
  ⚠️ 放行的只有 `.js`；`.mjs` / `.html` / `.htm` **不放行**（词典是外部内容，信任边界只有一条）。
- 加密词典里 `Encrypted=2` 的键信息块、以及 LZO / zlib 压缩的词块：见下面的「已知限制」

## 从源码构建

**工具链**：Node.js（前端与接口代码生成）、.NET SDK 或 MSBuild（编壳，目标框架 `net48` / x64）、
WSL + MinGW-w64（交叉编译出 Windows 内核 DLL）。

> ⚠️ **本仓库是「源码发布」**：**不含测试、测试用词典与验收脚本** —— 只带 `native/` 的源码、
> 外壳、界面、文档，以及三个**构建用**的脚本。所以下面只有构建命令。

```powershell
node web/build.mjs                     # 打前端（改 web/ 后必做）
node tools/gen-bindings.mjs             # 从接口定义重新生成 4 份接口代码（改 abi/ 后必做）

sh tools/build-windows-dll.sh <0.2.0 目录的 WSL 路径> Release   # 交叉编出 Windows 内核 DLL

& "$env:USERPROFILE\.dotnet\dotnet.exe" build shell\Lookup.App\Lookup.App.csproj -c Release
                                       # 编壳（产物：shell\Lookup.App\bin\Release\net48\Lookup.App.exe）
```

跑起来看效果：把编出来的 `Lookup.App.exe`、`dsh_lookup.dll` 与 `web\` 摆在一起即可 ——
⚠️ `web\` 与 exe 的相对位置别弄错（外壳是从 exe 所在目录**往上**找 `web/floating.html` 的）。

## 仓库结构

```
abi/lookup.abi.json   ← 唯一的接口定义（函数 / 枚举 / 常量；注释里写着业务约定）
        │
        ├─ node tools/gen-bindings.mjs ─→ native/include/dsh_lookup.h        （C 头文件）
        │                                 shell/Lookup.Interop/DshLookup.g.cs（C# 绑定）
        │                                 web/src/shared/abi.ts              （TypeScript 类型）
        │                                 docs/api/abi.md                    （可读版）
        │
native/src/**（C11 内核：解析 / 查词通道 / 设置 / 历史 / 发音 / 翻译 …，零第三方依赖）
        │
        └─ dsh_lookup.dll ─→ shell/**（操作系统相关的那一半 + 真实程序）
                             web/**（界面）
```

| 路径 | 是什么 |
| --- | --- |
| `abi/` | **唯一的接口定义**（改它必须跑 `node tools/gen-bindings.mjs`） |
| `native/` | C11 内核：`src/**`、`include/dsh_lookup.h`（自动生成）、`vendor/speex/`（第三方，BSD 3-Clause） |
| `shell/` | `Lookup.Interop`（自动生成的绑定）、`Lookup.Host`（通信桥 / 虚拟主机 / 平台能力）、`Lookup.App`（窗口） |
| `web/` | 界面（HTML / CSS / TypeScript）+ `build.mjs` |
| `tools/` | 只有三个**构建用**的脚本：生成接口代码、生成码表、交叉编 DLL |
| `docs/` | 接口文档（`api/abi.md`，自动生成）、架构决策记录（`adr/`） |
| `release/screenshots/` | 上面那几张截图 |

**为什么这么分**：业务逻辑只在 C 内核里，界面只是视图层，外壳只做「操作系统能力」那一半
（探本机音色、发 HTTP、摆窗口）—— **同一件事不许有两个来源**。内核是纯 C11、不依赖系统，
所以它能在 Linux 里编、也能交叉编译成 Windows DLL。

## 已知限制

1. **只支持 Windows**。内核（`native/`）是纯 C11、可移植；但外壳（C# / WinForms）与界面目前只有
   Windows 那一份实现。
2. **解析器的两条路会如实报错而不是硬撑**：`Encrypted=2` 的键信息块解密、LZO 压缩的词块 ——
   解压器都在，差的是接上；遇到这两类词典会明确报「这一版还没接」，绝不无声地给出错结果。
3. **查词通道里的机器翻译那一步**同样如实报「未实现」。
4. **在线发音与机器翻译需要自备凭据**（火山引擎 / 豆包语音合成的 API Key 与音色），
   按量计费。没填就**置灰并说明原因**，绝不不报错地失败。
5. **不附带任何词典**。词典版权属于各自的作者。
6. 界面用 WebView2 渲染，所以外观与行为跟着系统上装的 Edge 内核走。

## 关于本仓库

- 本仓库是 **0.2.0** 的源码，也是本项目的**第一个公开发布版**；**只发源码与文档**，
  不含测试、测试用词典与验收脚本。
- 代码注释里偶尔会看到「参考实现」这个说法：指的是作者本机上的上一代实现（**不随本仓库发布**），
  本版内核是拿它当标准答案逐字节对照移植过来的。读代码不需要它。
- 本仓库不含任何凭据；在线发音与机器翻译一律要你自己申请、自己填。

## 许可

本项目以 **MIT 许可**发布，见 [`LICENSE`](LICENSE)。第三方组件与它们的许可见
[`THIRD-PARTY.md`](THIRD-PARTY.md)（其中 libspeex 1.2.1 是 BSD 3-Clause，只用了它的解码路径）。
