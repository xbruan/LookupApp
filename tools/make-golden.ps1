# =============================================================================
# 编**基线再生工具**：让 0.1.3 的参考实现（C#）能把每本测试用词典的全部输出吐成规范化 JSON。
#
#   powershell -File tools/make-golden.ps1
#
# 它**只负责编译**（链仓库内那份只读的参考实现副本，见 reference/0.1.3-parser/README.md）。
# 跑它、并把结果写进标准答案文件基线的那一步在 WSL 那一侧：
#
#   wsl.exe -d Ubuntu --cd <仓库的 WSL 路径> bash tools/run-golden.sh <仓库的 WSL 路径> --regen
#
# ⚠️ **为什么拆成两步、而且日常回归一点都不碰它**（用户 2026-09 定的约定）：
#   基线是"标准答案"，必须由**参考实现**产出 —— **绝不许由正在被测的 C 实现产出**，
#   否则就是自己出题自己判卷，这道 gate 只剩形式。所以"再生成基线"是一条**显式调用**的
#   路径（`--regen`）；默认那道 gate（tools/golden-gate.ps1）只编 C 侧、只跟冻结基线比。
#
# 为什么要这个工具（这是整个 0.2.0 最要紧的一件事之一）：
#   内核现在是"自己说自己对"（内核单测 + ASan 全绿），但**没有一条证据说明它的行为
#   与 0.1.3 一致**。重写最典型的死法就是"99% 一样、1% 悄悄不一样"，
#   而这一仓库历史上最贵的几个 bug（@@@LINK 落点、书名 vs 文件名、归一化索引）
#   恰好都属于"读代码看不出来"的那一类。
#   所以拿参考实现的输出冻成基线，让 C 版**逐字节**对齐它。
#
# 参考实现怎么跑：`tools/golden/GoldenDump.csproj` —— 它把**仓库内**的
# reference/0.1.3-parser/src/Dictionary/*.cs 链进来编成 net48 的 exe。
#
# ⚠️ 不能用 `C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe`：
#    那是 **C# 5** 的老编译器，而参考源码用了文件作用域命名空间（C# 10）
#    与插值字符串（C# 6），会报一屏 "应输入 {" / "意外的字符 $"。必须用 dotnet SDK。
# =============================================================================

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
# 这个仓库本身**就是** 0.2.0 的代码（没有再套一层版本目录），所以仓库根 = $PSScriptRoot 的父目录。
$root = Split-Path -Parent $PSScriptRoot
$outDir = Join-Path $root 'tools/golden'
$refSrc = Join-Path $root 'reference/0.1.3-parser/src/Dictionary/MdictCore.cs'

# ⚠️ 这里原来找的是"版本目录的父目录/0.1.3/src/Dictionary/MdictCore.cs" —— 也就是**穿到兄弟目录**
#    里去链源码，于是这道 gate 能不能跑取决于邻居的目录还在不在。参考实现已按
#    docs/code-review-2026-09-29.md 的建议冻结进本仓库，那个"$Repo"概念随之取消：
#    找不到就是**本仓库缺件**，不再有"去别处找一份"这条路。
if (-not (Test-Path $refSrc)) {
  throw "找不到参考实现源码：$refSrc`n（它应当随仓库一起提交，见 reference/0.1.3-parser/README.md）"
}

# dotnet SDK：系统 PATH 上那个 `dotnet` 可能只有运行时（没有 sdk），
# 而本机把 SDK 装在用户级目录 —— 两处都试。
$dotnet = if (Test-Path (Join-Path $env:USERPROFILE '.dotnet/dotnet.exe')) {
  Join-Path $env:USERPROFILE '.dotnet/dotnet.exe'
} else { 'dotnet' }

$ver = & $dotnet --version 2>&1
if ($LASTEXITCODE -ne 0) { throw "dotnet 不可用：$ver" }
Write-Host "dotnet：$ver（$dotnet）" -ForegroundColor DarkGray

Write-Host '── 编译基线再生工具（链仓库内的参考实现，net48）──' -ForegroundColor Cyan
$proj = Join-Path $outDir 'GoldenDump.csproj'
& $dotnet build $proj -c Release -v quiet --nologo
if ($LASTEXITCODE -ne 0) { throw "dotnet build 失败（退出码 $LASTEXITCODE）" }

$exe = Join-Path $outDir 'bin/Release/net48/GoldenDump.exe'
if (-not (Test-Path $exe)) { throw "没编出 $exe" }
Write-Host "  ✅ $exe" -ForegroundColor Green

# ── 下一步：跑它、写基线 ───────────────────────────────────────────────────────
# ⚠️ 跑的方式**不在这个脚本里**：基线要连**来源指纹**（参考源码逐文件 SHA256、清单哈希、
#    日期、命令）一起记下来，那些指纹用 shell 的 sha256sum 算最省事，也保证与 WSL 侧
#    C 侧看到的是同一份字节。所以产物写在 tools/run-golden.sh --regen 一处，避免两处漂移。
Write-Host ''
Write-Host '⚠️ 这只是"编好工具"。要真正**再生成基线**（特权操作，会覆盖标准答案）请接着跑：' -ForegroundColor Yellow
Write-Host '   wsl.exe -d Ubuntu --cd <仓库的 WSL 路径> bash tools/run-golden.sh <仓库的 WSL 路径> --regen' -ForegroundColor Yellow
Write-Host '   日常回归**不需要**这两步：tools/golden-gate.ps1 只编 C 侧、只跟冻结基线比。' -ForegroundColor DarkGray
