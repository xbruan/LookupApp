# 第三方组件与出处（0.2.0）

本文件回答一件事：**这个仓库里哪些东西不是我们写的，它们的许可是什么、出处在哪。**
（我们自己的代码用 [`LICENSE`](LICENSE) 那份 MIT。）

## 一、内嵌源码：libspeex 1.2.1（BSD 3-Clause）

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
`native/vendor/speex/COPYING` 与源码里的原始头注释来满足的；**也不得**用 Xiph.org Foundation
或贡献者的名义为衍生产品背书。

## 二、构建期依赖（不随仓库分发）

| 组件 | 用途 | 许可 |
| --- | --- | --- |
| `esbuild` | 只用来打前端（`node web/build.mjs`） | MIT，**走 npm 安装**，不进仓库 |
| .NET SDK 8 / gcc（MinGW） | 编外壳与内核 | 各自的许可，本机安装 |

## 三、**没有再分发**的第三方文档（有意删掉）

项目开发期参考过火山引擎（豆包）的**官方 API 文档 PDF**。那些是厂商的版权文档，
**没有**放进本仓库 —— 需要时到[火山引擎官方文档站](https://www.volcengine.com/docs)看。
我们自己的接入总结（`docs/豆包语音合成接入方案.md` 等，**放在仓库之外的项目文档里**）
只写我们实测到的事实与请求形状，不复制厂商原文。

## 四、图标

界面图标（`web/src/floating/icons.ts`）是**手写的 SVG 路径**，不是图标库 ——
没有第三方图标集的许可问题。应用标识（放大镜圆底）也是自己画的。
