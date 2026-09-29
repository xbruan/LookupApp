# tools/golden/baseline —— 标准答案文件基线（**冻结的对手**）

对照测试的判决标准是"**当前 C 实现**对每本测试用词典的全部输出"与"**冻结基线**"逐字节相同
（外加一张已确认差异的白名单）。这个目录装的就是那道判决的对手，以及它的来源记录。

```text
golden-baseline.json   标准答案本体：一份 JSON 装全部测试用词典（按 tools/golden/fixtures.txt 的**顺序**）
provenance.md          这一份基线是**怎么来的**（参考实现逐文件 SHA256 / 清单哈希 / 命令 / 日期 / dotnet 版本）
README.md              本文件：基线的规矩（人工维护，**不被 --regen 覆盖**）
```

## 一、谁产出基线（这条是硬约束）

**基线只能由参考实现（C#）产出，绝不能由 C 侧产出。**

让**正在被测的 C 实现**自己产出标准答案，就是自己出题自己判卷：它改错了，基线跟着错，
而这道 gate 会**继续通过** —— 于是"99% 一样、1% 悄悄不一样"这个最典型的死法，
恰好被这道 gate 放过。所以 `tools/run-golden.sh --regen` 里**一个字节都不读** C 侧的输出。

## 二、日常怎么用（默认路径，**不需要 .NET**）

```sh
sh tools/check-golden.sh <仓库的 WSL 路径>          # 从 WSL 里跑
```

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\golden-gate.ps1   # 从 Windows 跑
```

它做的事：编 `tools/golden/golden-dump-c.c`（链 `native/` 源码）→ 跑它 →
`tools/golden/out/golden-c.json` → 用 `tools/golden/compare-golden.py` 跟本目录的
`golden-baseline.json` 比。**不编 C#、不跑参考实现、不需要 dotnet**。

## 三、要再生成基线时（特权操作，显式两步）

```powershell
powershell -File tools\make-golden.ps1        # ① 编参考实现探测器（要 dotnet SDK；不写基线）
```

```sh
sh tools/run-golden.sh <仓库的 WSL 路径> --regen   # ② 跑参考实现 → 覆盖基线 + 重写 provenance.md
```

`--regen` 会**覆盖标准答案**，所以它只在"参考实现确实变了"（换了/改了
`reference/0.1.3-parser/`）或"清单变了"时才该跑；它会把"与上一份基线是否相同"记进
`provenance.md` —— 逐字节相同说明这趟是空跑。**改 `fixtures.txt` 必须重新生成基线**，
否则比的是旧对手（顺序与条目都变了）。

## 四、比不过的时候怎么办

**先看是真差异还是基线过时**，两件事都不许"顺手放宽"：

1. **真差异**（C 侧改了行为、参考实现对）→ 修 C 侧。**不许**把差异加进白名单；
2. **基线过时**（`fixtures.txt` 变过、或参考实现换过）→ 跑 `--regen` 重生成基线，
   并把 `provenance.md` 里"与上一份基线不同"那一条连原因一起写进当轮开发记录；
3. **白名单**（`compare-golden.py` 顶部的 `WHITELIST`）是**已知、已判定责任方**的差异，
   只有一条：`v2-multiblock.mdx` 的 `records[2]` / `records[4]` —— 参考实现
   `GetRecordBytes` 只从"记录起点所在那块"取字节，把跨界的两条记录截断了，C 侧按偏移拼接两块。
   目前实测 **21/21 本逐字节相同 + 2 处已确认差异**；白名单条目一旦**不再出现差异**，
   比对脚本会报"白名单该删掉" —— 那是提示你它已经被修好了，不是噪声。

## 五、这一份基线的沿革

- 2026-09-29：本仓库从兄弟目录 `查词软件/0.2.0/` 恢复测试与工具时，带回来一份
  `tools/golden/golden.json`（399,159 字节，SHA256 `73ddef58cdc9ed5c538a8b3c4025ee02340d75097e4b00e903fa6037d2441731`）。
  本次改造先核对它的出处：**用仓库内那份参考实现重新跑一遍 21 本测试用词典，产出与它逐字节相同**
  （同一 SHA256），确认它是**有效的参考侧输出**、而不是 C 侧产物；
- 随后把它收养为冻结基线（同一份字节），并**删掉旧路径 `tools/golden/golden.json`** ——
  "标准答案"同时存在两份就是下一次漂移的入口；
- 同一次改造里，`fixtures.txt` 的 12 个变体样本从兄弟目录迁进 `testdata/variants/`
  （它们的名字不变，所以基线内容与收养前逐字节相同）。
