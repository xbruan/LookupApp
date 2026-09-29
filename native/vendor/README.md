# `native/vendor/` —— 第三方源码（**只有一处：官方 libspeex**）

## 术语表（本文用到的词）

> 完整词表与"旧词 → 新词"对照见 [`../../docs/design/术语表.md`](../../docs/design/术语表.md)。

| 术语 | 一句话解释 | 在本项目里指什么 |
| --- | --- | --- |
| **内核** | 用 C 语言写的核心程序库，业务逻辑都住在里面 | `native/` |
| **第三方源码（vendor）** | 从别人那里原样拿进来、不是本项目写的代码 | 本目录只有 `speex/` 一处 |
| **硬规则** | 不许破的规则 | "纯 C11、零依赖" |
| **例外** | 有意违反一条硬规则的地方，必须把边界写清楚 | 本目录这一处 |
| **检查标准** | 事先写死的条件，用来判一件事算不算通过 | 构建脚本 的 `check` 里有一条 |
| **实测结果** | 程序或测试真的打印出来的数字 | 例：DLL 从 618 KB 涨到 764 KB |
| **逐字节对照** | 拿新版和老版在同样输入下比结果 | 本文件最后一节那张相关系数表 |
| **解码器** | 把压缩过的音频还原成能播放的声音的代码 | Speex 解码器 |
| **许可（license）** | 别人写的代码允许你怎么用的法律条款 | libspeex 是 BSD 3-Clause |
| **C11 / C#** | C 语言的一个标准版本（2011 年）/ 微软的一种编程语言 | 内核是纯 C11；参考实现是 C# |
| **导出表** | DLL 对外公开的函数清单 | 由接口定义生成的 `.def` 文件列出 |

---

> ⚠️ 目录说明与内核全貌见 [`../README.md`](../README.md)。

## 这里有什么

`speex/`：**官方 libspeex 1.2.1 的解码路径**，给 `.spx`（Ogg Speex）用。

| 项 | 值 |
| --- | --- |
| 来源 | `https://downloads.xiph.org/releases/speex/speex-1.2.1.tar.gz` |
| tarball SHA-256 | `4B44D4F2B38A370A2D98A78329FEFC56A0CF93D1C1BE70029217BAAE6628FEEA` |
| 许可 | **BSD 3-Clause**（`COPYING` 原样留着；含"不得用 Xiph.org 的名义为衍生品背书"那一条） |
| 放了哪些 | `include/speex/` 6 个公共头文件 + `libspeex/` 上游 `libspeex_la_SOURCES` 里那**全部 30 个 `.c`** 与对应的 `.h`（逐条对过上游 `Makefile.in`，没有多、没有少） |
| 没放哪些 | `testenc*.c`（自测程序，它们有自己的 `main`）、`kiss_fft*` / `kiss_fftr*` / `smallft*` / `vorbis_psy*` / `fftwrap.h`（前处理与回声消除才要的 FFT，解码用不到）、`vbr.c` 之外的编码器专用件、`Makefile*` 与 autotools 那一套 |

## ⚠️ 这是一次**有意识的例外**，约定由用户定（2026-09）

内核的硬规则是「**纯 C11、零依赖**」。用户定的原话是：

> **移植一个纯 C 的 Speex 解码器进内核。**

为什么非要有它：**Chromium（WebView2）不解码 Speex**，而 LDOCE5 那类词典的自带录音
全是 Ogg Speex（`speex_string = "Speex   "`、版本 1.2rc1）。不自己解，那些录音就是
"点了没声音"。参考实现的做法是 vendor 一份**纯托管**的 NSpeex（C#）—— 因为那一版的
exe 不能带原生 DLL；**0.2.0 的内核本来就是 C**，这个约束不存在了，所以直接上官方 C 实现。

**这次例外的边界（写清楚，免得后来人以为是随便引进来的）**：

1. **只此一处**。再要引第三方源码，得重新问用户，不许顺手加第二个。
   （同一条规则的上一例是 SQLite —— 那一处用户已经推翻了，见下面"被撤销的那一次"。）
2. **它只被 `native/src/audio/` 那一层用**（`.spx` → WAV 那件事）。内核其余部分
   （解析 / 查找 / 设置 / 历史 / 文本）仍然**零依赖**，不许 `#include "speex/speex.h"` ——
   构建脚本 的 `check` 里有一条检查标准盯着这件事。
3. **编译参数单独给**：`-Werror -pedantic` 那套是本仓库对自己代码的要求，对 30 个上游文件
   不适用。Makefile 与 `tools/build-windows-dll.sh` 里各有一组**只对它生效**的参数。
4. **导出表里不许出现它的符号**：DLL 的导出是接口定义生成的 `.def` 显式列出来的
   （不是 `--export-all-symbols`）。加它那一刻实测"导出 32 条 / 未实现 2 条"与加之前
   **一模一样**；现在的实测结果是 **33 / 1**（第三十四轮把 `dsh_speech_plan` 实现了 ——
   那一条与 Speex 无关，只是接口表在长）。
5. **上游文件一个字节都不改**（要改就改在自己的那两三个文件里，见下）；
   升级要连带改这里的哈希，并重跑 `make test`（`test_audio` ⑧ 盯着解出来的 PCM 哈希）。

## 我们自己写的只有三个文件（其余都是原样拷贝）

| 文件 | 干什么 | 为什么必须由我们写 |
| --- | --- | --- |
| `speex/libspeex/config.h` | autotools 的 `config.h` 的手写等价物 | 上游每个 `.c` 都是 `#ifdef HAVE_CONFIG_H #include "config.h"`，而 `EXPORT` 就住在里面 —— 少了它上游**一行都编不过**（62 处 `expected ';' before 'void'`）。里面也定下了**浮点/定点**那个开关（选了浮点，理由与实测结果在那个文件里） |
| `speex/include/speex/speex_config_types.h` | `speex_config_types.h.in` 的手写等价物 | 非 Windows 下 `speex_types.h` 要 include 它；上游由 `configure` 生成（`@SIZE16@` 那一套），我们不用 autotools，所以手写一份"`<stdint.h>` + 四个 typedef" |
| `speex/dsh_override.h` | 把上游 `_speex_fatal` 里的 **`exit(1)`** 换成"打印一句、返回" | 内核的硬规则是"绝不让任何东西把进程掀翻"，而那是**从库内部 `exit` 整个进程**。实测这一版 `speex_fatal(` 的调用点是 **0 处**，所以它是保护措施而不是补丁 —— 但代价只有几行 |

容器与打包（Ogg 页重组 / Speex 头 / 逐帧循环 / WAV 封装）**不在这个目录**：
它在 `native/src/audio/dsh_speex.c`，是从参考实现的 `src/Audio/OggSpeex.cs` 逐条移植过来的
（连错误文案都是逐句照抄 —— 那些话是直接显示给用户的）。

## 怎么编（两条路都要，各有一条检查标准）

```
构建脚本             → VENDOR_SRCS / VENDOR_OBJS / VENDOR_CFLAGS + $(OBJ)/vendor/speex/%.o
tools/build-windows-dll.sh  → 同一组参数（Release 也编一次）
```

两条都要带 `-DHAVE_CONFIG_H`（否则当场红，见上表）与 `-include dsh_override.h`。
Linux 侧链接还要 `-lm`（`stereo.c` / `vbr.c` 用 `log`/`pow`）—— 那不是第三方依赖，
数学函数是 C 标准库的一部分，只是 POSIX 上单独一个 `-lm`。

实测产物：**DLL 从 618 KB 涨到 764 KB**（+146 KB），**导出表照旧 32 条**。

## 逐字节对照：拿官方那份 DLL 当裁判

参考实现就有一套成熟的比对工具（参考实现/tools/SpeexProbe` 与 `SpeexCrossCheck`），
**在仓库外**构建它们（`dotnet build … -o <临时目录> -p:BaseIntermediateOutputPath=<临时目录>\obj\`，
这样参考实现/tools/*/bin|obj` 一个都不产生），实测结果：

| 两份 PCM 相比 | 相关系数 | 平均样本差 | 最大差 |
| --- | --- | --- | --- |
| **内核（浮点）** vs 官方 DLL | **1.0000** | **0.361** | **2** |
| 内核（定点）vs 官方 DLL | 0.9999 | 63 | 588 |
| 官方 DLL vs 参考实现的 NSpeex | 0.978 | 324 | — |

采样数、时长、granule **完全一致**（32000 = 100 包 × 320）。逐条见  `。

## 被撤销的那一次（SQLite，同一天）

0.2.0 的第二十九轮曾把 **SQLite 的 amalgamation**（`sqlite3.c` 9,089,564 B +
`sqlite3.h` 644,069 B，公有领域）vendor 进来当查词历史的落盘层，边界也写在本文件里。
用户随后推翻了它（原话：「0.2.0 的历史量根本够不着 SQLite 的地板，而 vendor 一个 9 MB 的
第三方源码 + Makefile 特例 + 一条 grep 判据，是为了一个用不上的能力付的长期成本」），
历史改回**一行一条 JSON 的追加文件** `history.jsonl`，那 9 MB 与它的编译参数、
链接、检查标准一起删掉。全过程见  `。
**留这段的原因**：当时这份 README 是"这次例外的边界"的唯一书面记录 ——
删文件不写清楚，后来的人会看到"用户选了 A"那句话，然后把 SQLite 再加一遍。
