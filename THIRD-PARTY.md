# 第三方组件与出处

> ⚠️ 标题里**刻意不写版本号**：这份清单跟着仓库走，写死一个版本号，下一次升版它就成了
> 一句假话（原来那句写的还是 0.2.0）。要看这是哪一版，看 `abi/lookup.abi.json` 的 `meta.version`。

本文件回答一件事：**这个仓库里、以及我们发出去的便携包里，哪些东西不是我们写的，
它们的许可是什么、出处在哪。** 我们自己的代码用 MIT —— 全文见下面那张表的第一行。

★ **本文件会被原样搬进便携包当 `THIRD-PARTY.txt`**（搬法见**仓库里的** `tools/package.ps1`
第 ④ 步），所以下面**一个仓库相对链接都不写** —— 那种链接在包里是死链。
要指东西，一律按这张表对照：

| 提到的东西 | 在仓库里叫 | 在便携包里叫 |
| --- | --- | --- |
| 本项目自己的许可（MIT 全文） | `LICENSE` | `LICENSE.txt` |
| 本清单 | `THIRD-PARTY.md` | `THIRD-PARTY.txt` |
| libspeex 的 BSD 3-Clause 全文 | `native/vendor/speex/COPYING` | `libspeex-COPYING.txt` |
| WebView2 的许可全文 | `licenses/webview2/LICENSE.txt` | `WebView2-LICENSE.txt` |
| WebView2 的上游声明 | `licenses/webview2/NOTICE.txt` | `WebView2-NOTICE.txt` |
| libspeex 的裁剪说明与逐样本对照数据 | `native/vendor/README.md` | 不进包（那是我们自己的说明，不是别人的许可） |

## 一、内嵌源码：libspeex 1.2.1（BSD 3-Clause）—— 在仓库里

| 项 | 值 |
| --- | --- |
| 位置 | `native/vendor/speex/` |
| 是什么 | 官方 **libspeex 1.2.1** 的**解码路径**（编码侧与不用的模块已裁掉） |
| 用途 | 播词典自带音频里的 `.spx`（Ogg Speex）—— 系统解码器不认这一种，所以必须自带 |
| 许可 | **BSD 3-Clause**，全文 `native/vendor/speex/COPYING`（**原样保留，不许改**） |
| 版权 | Xiph.Org Foundation / Jean-Marc Valin / Analog Devices Inc. / CSIRO / David Rowe / EpicGames / Jutta Degener, Carsten Bormann |
| 我们改了什么 | 只做了"收窄"：删掉编码器与不用的模块、按本项目的编译参数整理成一份文件清单。**算法一个字节没改**，每个源文件原始的版权头注释保留 |
| 验证 | 与参考实现做过逐样本对照（相关系数、峰值、RMS 三列），见 `native/vendor/README.md` |

**再分发的前提**（BSD 3-Clause 的要求）：保留版权声明、条件与免责声明 —— 我们就是靠
`native/vendor/speex/COPYING` 与源码里的原始头注释来满足的（便携包里那份叫
`libspeex-COPYING.txt`）；**也不得**用 Xiph.org Foundation 或贡献者的名义为衍生产品背书。

## 二、随包再分发的二进制：Microsoft WebView2 1.0.2903.40（BSD 3-Clause）—— **只在便携包里**

| 项 | 值 |
| --- | --- |
| 是什么 | 界面渲染用的 WebView2（Edge 内核）的 .NET 程序集与加载器，来自 NuGet 包 `Microsoft.Web.WebView2`，引用处是 `shell/Lookup.App/Lookup.App.csproj` 里那行 `PackageReference` |
| 包里的哪几个文件 | `Microsoft.Web.WebView2.Core.dll`、`Microsoft.Web.WebView2.WinForms.dll`、`WebView2Loader.dll` |
| 许可 | **BSD 3-Clause**，版权 `Copyright (C) Microsoft Corporation. All rights reserved.`，全文 `licenses/webview2/LICENSE.txt`（包里 `WebView2-LICENSE.txt`） |
| 我们改了什么 | **一个字节都没改**，是 NuGet 包里的原件（`dotnet build` 直接搬进包里） |
| 为什么这份清单里也有它 | 它不是本项目写的，而且**跟着包发出去**了 —— 按 BSD 3-Clause 的**二进制**再分发条款，随包的文档里必须带上版权声明、条件与免责声明。它不在仓库里（构建时从 NuGet 取），所以只在"包"这一侧出现 |
| 另附 | `licenses/webview2/NOTICE.txt`（包里 `WebView2-NOTICE.txt`）—— 上游那份**声明自己不许翻译**，我们原样附上、不译 |
| 不属于这一节的 | WebView2 的**运行时**（真正渲染的那套 Edge 内核）由系统提供，**我们不分发它**；这里说的只是上面那三个 DLL |

## 三、构建期依赖（不随仓库、也不随包分发）

| 组件 | 用途 | 许可 |
| --- | --- | --- |
| `esbuild` | 只用来打前端（`node web/build.mjs`） | MIT，**走 npm 安装**，不进仓库 |
| .NET SDK 8 / gcc（MinGW） | 编外壳与内核 | 各自的许可，本机安装 |

程序跑起来要的 .NET Framework 4.8 运行时同样由系统提供，本包不带。

## 四、**没有再分发**的第三方文档（有意删掉）

项目开发期参考过火山引擎（豆包）的**官方 API 文档 PDF**。那些是厂商的版权文档，
**没有**放进本仓库 —— 需要时到[火山引擎官方文档站](https://www.volcengine.com/docs)看。
我们自己的接入总结（`docs/design/豆包语音合成接入方案.md` 等）
只写我们实测到的事实与请求形状，不复制厂商原文。

## 五、图标

界面图标（`web/src/floating/icons.ts`）是**手写的 SVG 路径**，不是图标库 ——
没有第三方图标集的许可问题。应用标识（放大镜圆底）也是自己画的。
