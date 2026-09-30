# LookupApp

> 一个 Windows 桌面查词工具：读 **MDict 词典**（`.mdx` / `.mdd`），浮窗取词、词条发音、机器翻译、
> 查词历史、词库管理。
>
> 本仓库是**唯一的开发仓库**

## 它做什么

| 功能 | 说明 |
| --- | --- |
| **浮窗查词** | 常驻悬浮输入窗；支持正文链接跳转、选词续查 |
| **词库管理** | 导入 / 移除 `.mdx`、排序、当前词典切换 |
| **发音** | 词典自带录音（`.mdd` 里的原录音）→ 在线语音（仅支持豆包语音）→ 系统语音 |
| **机器翻译** | 整词或整段译文（需自备凭据） |
| **查词历史** | 记录「词条 + 词典」|
| **兜底通道** | 当前词典查不到时自动去别的词典「借查」→ 机器翻译 |
| **托盘** | 托盘菜单（唤出胶囊 / 选项 / 退出 / 开机自启） |

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

**免安装**：到本仓库的 Releases 下载 `LookupApp-0.2.1-portable.zip`，解压到任意目录，
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
  设计依据见 [`docs/design/不带mdd的词典与机器翻译开发指导.md`](docs/design/不带mdd的词典与机器翻译开发指导.md)。
- **加密与压缩的词典**：`Encrypted=1`（记录块）/ `Encrypted=2`（键信息块）、以及 zlib / LZO / 不压缩
  三种词块都能读
  （21 本合成测试用词典覆盖「版本 × 压缩 × 编码 × 加密 × 索引形态 × 结构」，
  见 [`tools/golden/fixtures.txt`](tools/golden/fixtures.txt)

## 从源码构建

**工具链**：Node.js（前端与接口代码生成）、.NET SDK 或 MSBuild（编壳，目标框架 `net48` / x64）、
WSL + MinGW-w64（交叉编译出 Windows 内核 DLL）。版本要求、安装命令与预检见
[`native/README.md`](native/README.md)。

```powershell
node web/build.mjs                     # 打前端（改 web/ 后必做）
node tools/gen-bindings.mjs             # 从接口定义重新生成 4 份接口代码（改 abi/ 后必做）

# 内核：编 + 接口自检 + 单元测试（默认走 WSL 里的 gcc）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\native-build.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools\native-build.ps1 -Asan   # ASan + UBSan

# 交叉编出 Windows 内核 DLL（改内核后必做）
wsl.exe -- bash tools/build-windows-dll.sh /mnt/c/<本仓库的 WSL 路径> Release

& "$env:USERPROFILE\.dotnet\dotnet.exe" build shell\Lookup.App\Lookup.App.csproj -c Release
                                       # 编壳（产物：shell\Lookup.App\bin\Release\net48\Lookup.App.exe）
```

把编出来的 `Lookup.App.exe`、`dsh_lookup.dll` 与 `web\` 放在同一目录下，
要一个装好的便携包就跑 `powershell -File tools\package.ps1`。

## 如何测试（本项目有五道分级检查 + 一道打包验收）

| 级别 | 判什么 | 入口 |
| --- | --- | --- |
| **A/A′** | 离线静态检查 | `node tools/gen-bindings.mjs --check` + `node tools/ui-static-check.mjs` + `node tools/check-entry-auth.mjs` |
| **B** | 内核单测 + 内存检查| `wsl.exe -- bash tools/wsl-make-test.sh`；`make asan`（ASan + UBSan） |
| **C** | 词典格式对照 | `powershell -File tools\golden-gate.ps1`；`node tools/probe-page.mjs --help` |
| **D** | 真 DLL + 绑定 + 宿主适配层 | `powershell -File tools\test-shell-app.ps1`；`powershell -File tools\test-windows-dll.ps1` |
| **打包** | 便携包重建并验证 | `powershell -File tools\package.ps1` |

⚠️ **非必要不做完整测试**。改文案只重打前端；纯逻辑只跑内核单测；跨模块 / 窗口几何 / 启动路径才跑 D 级。
每道检查的原始输出一律写进 `logs/`（已 gitignore）。

## 仓库结构

```
abi/lookup.abi.json   ← 唯一的接口定义（函数 / 枚举 / 常量；注释里写着业务约定）
        │
        ├─ node tools/gen-bindings.mjs ─→ native/include/dsh_lookup.h        （C 头文件）
        │                                 shell/Lookup.Interop/DshLookup.g.cs（C# 绑定）
        │                                 web/src/shared/abi.ts              （TypeScript 类型）
        │                                 docs/api/abi.md
        │
native/src/**（C11 内核：解析 / 查词通道 / 设置 / 历史 / 发音 / 翻译）
        │
        └─ dsh_lookup.dll ─→ shell/**（操作系统相关 + 真实程序）
                             web/**（界面）
```

| 路径 | 是什么 |
| --- | --- |
| `abi/` | **唯一的接口定义**（改它必须跑 `node tools/gen-bindings.mjs`） |
| `native/` | C11 内核：`src/**`、`include/dsh_lookup.h`（自动生成）、`tests/**`（单元测试）、`vendor/speex/`（第三方，BSD 3-Clause） |
| `shell/` | `Lookup.Interop`（自动生成的绑定）、`Lookup.Host`（通信桥 / 虚拟主机 / 平台能力）、`Lookup.App`（窗口） |
| `web/` | 界面（HTML / CSS / TypeScript）+ `build.mjs` |
| `testdata/` | **冻结的合成测试用词典**（`.mdx` / `.mdd` + `variants/` 格式矩阵），按 `SHA256.txt` 校验 |
| `tools/` | 构建 / 生成 / 诊断 / 验收 / 打包脚本（命令见 `AGENTS.md`） |
| `docs/` | `design/`（设计依据）、`adr/`（架构决策记录）、`api/abi.md`（自动生成）、安全审查报告 |
| `release/screenshots/` | 上面那几张截图 |

**为什么这么分**：业务逻辑在 C 内核里，界面是视图层，外壳做「操作系统能力」
（探本机音色、发 HTTP、摆窗口）。纯 C11内核不依赖系统，
既能在 Linux 里编、也能交叉编译成 Windows DLL。


## 已知限制

1. **只支持 Windows**。内核（`native/`）是纯 C11、可移植；但外壳（C# / WinForms）与界面目前只有
   Windows 那一份实现。
2. **在线发音与机器翻译需要自备凭据**（火山引擎 / 豆包语音 API Key 与音色），按量计费。凭据只存在用户自己的设置里。
3. **不附带任何词典**。词典版权属于各自的作者。
4. 界面用 WebView2 渲染，所以外观与行为跟着系统上装的 Edge 内核走。
5. **格式支持范围以对照测试为准**。

## 关于本仓库

- 本仓库含测试、测试用词典与验收 / 打包脚本 —— 开发与验收都在这一个目录里完成。
- 代码注释里偶尔会看到「参考实现」这个说法：指的是上一代的 C# 实现（未发布）。
- 本仓库不含任何凭据；在线发音与机器翻译一律要你自己申请、自己填。

## 许可

本项目以 **MIT 许可**发布，见 [`LICENSE`](LICENSE) —— 那个文件里**只有 MIT 全文**，
第三方组件与它们的许可见 [`THIRD-PARTY.md`](THIRD-PARTY.md)：仓库里那处是 libspeex 1.2.1
（BSD 3-Clause，只用了它的解码路径），便携包里还随包分发微软的 WebView2 组件（同样 BSD 3-Clause）。
