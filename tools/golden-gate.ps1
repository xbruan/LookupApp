#!/usr/bin/env pwsh
# =============================================================================
# 标准答案文件对照测试 gate（从 Windows 调 WSL）—— **第一道验收 gate**。
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\golden-gate.ps1
#
# 它把 WSL 里的两步包起来，好让交付流程有一条**单一入口**、退出码可判：
#   ① sh tools/run-golden.sh <root>   编 C 诊断脚本、跑**当前 C 实现** → out/golden-c.json
#   ② python3 tools/golden/compare-golden.py <C 侧> <冻结基线>   逐字节比对
# 两步都能单独跑（查现场时用得上）；这条脚本的退出码 = 比对那一步的退出码。
#
# ⚠️ **默认这道 gate 只依赖 C 侧**（用户/评审 2026-09 定的约定）：
#   · 不需要 .NET，不编 C#，不跑参考实现 —— 那台机器上没装 dotnet 也能跑；
#   · 比的对手是**冻结基线** tools/golden/baseline/golden-baseline.json，
#     它的来源记录在 tools/golden/baseline/provenance.md（参考实现逐文件 SHA256 + 命令 + 日期）。
#
# 再生成基线是**另一条显式路径**（特权操作，会覆盖标准答案）：
#   powershell -File tools\make-golden.ps1                    # 编参考实现探测器（要 dotnet SDK）
#   wsl.exe -d Ubuntu --cd <仓库 WSL 路径> bash tools/run-golden.sh <仓库 WSL 路径> --regen
# 基线**绝不由 C 侧产出** —— 让被测实现自己出标准答案是"自己出题自己判卷"。
#
# 为什么它是第一道 gate（用户 2026-09 定的约定）：
#   内核自己的单测只证明"它自己前后一致"，**不证明它与 0.1.3 一致**。
#   重写最典型的死法就是"99% 一样、1% 悄悄不一样"。这道 gate 第一次跑就发现了
#   三处真差异（UTF-16 记录长度越界一字节、BIG5 少一个字符、键信息块/记录块的
#   LZO 与 Encrypted=1/2 还没接）—— 全是内核自己的测试照不出来的。
#
# ⚠️ 先决条件（缺了会在 WSL 那一侧直接报出来，不会不报错地通过）：
#   · WSL 里有 gcc 与 python3（比对脚本用 python3 读 JSON）；
#   · 冻结基线 `tools/golden/baseline/golden-baseline.json` **存在**（随仓库提交）。
# =============================================================================

[CmdletBinding()]
param(
  [string]$Distro = ""
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot            # 仓库根
$rootWin = (Resolve-Path $root).Path

# 基线得先在（它是这道 gate 的对手；C 侧自己产不出标准答案，见上面的说明）
$baseline = Join-Path $root 'tools/golden/baseline/golden-baseline.json'
if (-not (Test-Path $baseline)) {
  throw "缺少冻结基线：$baseline`n（再生成它：sh tools/run-golden.sh <root> --regen —— 特权操作，要 dotnet SDK）"
}

if ($rootWin -notmatch '^([A-Za-z]):\\(.*)$') { throw "看不懂的路径：$rootWin" }
$rootWsl = "/mnt/$($Matches[1].ToLower())/$($Matches[2] -replace '\\', '/')"

$wslArgs = @()
if ($Distro) { $wslArgs += @('-d', $Distro) }

Write-Host '── 标准答案文件对照测试（当前 C 实现 vs 冻结基线）──' -ForegroundColor Cyan
& wsl.exe @wslArgs -- bash -lc "cd '$rootWsl' && sh tools/check-golden.sh '$rootWsl'"
$code = $LASTEXITCODE

if ($code -ne 0) {
  throw "标准答案文件对照测试**不一致**（退出码 $code）。上面给出了第一处差异与两边的上下文。"
}
Write-Host '── 标准答案文件对照测试通过 ──' -ForegroundColor Green
