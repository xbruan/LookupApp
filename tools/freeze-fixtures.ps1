#!/usr/bin/env pwsh
# =============================================================================
# 冻结件校验：把标准答案文件赖以成立的**两份冻结件**逐字节核一遍。
#
#   pwsh -File tools/freeze-fixtures.ps1                  校验（默认动作，不写任何文件）
#   pwsh -File tools/freeze-fixtures.ps1 -Verify          同上（保留旧写法）
#   pwsh -File tools/freeze-fixtures.ps1 -UpdateManifest  只按 testdata/ 里**现有**文件重写清单
#
# 核的两份清单：
#   ① testdata/SHA256.txt                —— 测试用词典与格式矩阵样本（21 个条目 / 23 个文件）
#   ② reference/0.1.3-parser/SHA256.txt  —— 参考实现的 16 个 .cs（**基线是它们产出的**）
#      ⚠️ 参考实现**不随本仓库发布** —— 这份清单**在就核、不在就明着说一句跳过**，不是失败。
#
# ⚠️ 它**不再**"从兄弟目录复制进来"。这批东西以前是从
#    ../0.1.3/testdata 与 ../0.1.3/tools/MdxProbe/variants 拷过来的，于是
#    "这道 gate 能不能跑"取决于邻居的目录还在不在。现在它们都是**本仓库的普通文件**
#    （见 docs/code-review-2026-09-29.md 的问题 1）：复制这件事已经发生过一次、到此为止。
#
# 为什么"冻结"这件事本身要紧：
#   1. 标准答案文件对照测试的基线必须**逐字节可复现** —— 测试用词典一变，基线就悄悄失效
#      （0.1.3 坑 6：`robocopy /XD` 把一棵目录整棵剔掉、当时不暴露出来，就是这类学费）；
#   2. 参考实现漂移比样本漂移更坏：它一变，基线会**继续通过**、却已经不反映任何东西。
# =============================================================================

[CmdletBinding()]
param(
  # 兼容旧的调用写法 —— 校验本来就是默认动作
  [switch]$Verify,
  # 只按 testdata/ 里现有文件重写 testdata/SHA256.txt（**不读仓库外的任何目录**）
  [switch]$UpdateManifest
)

$ErrorActionPreference = 'Stop'
# 这个仓库本身**就是**当前版本的代码（没有再套一层版本目录），所以仓库根 = $PSScriptRoot 的父目录。
$root = Split-Path -Parent $PSScriptRoot
$dst = Join-Path $root 'testdata'
$manifest = Join-Path $dst 'SHA256.txt'
$refBase = Join-Path $root 'reference/0.1.3-parser'
$refManifest = Join-Path $refBase 'SHA256.txt'

function Get-Sha256([string]$path) {
  (Get-FileHash -Algorithm SHA256 -Path $path).Hash.ToLowerInvariant()
}

# 两份清单格式相同（`<哈希>  <相对清单所在目录的路径>`，# 与空行忽略），所以核法共用一份。
function Test-Manifest([string]$manifestPath, [string]$baseDir) {
  if (-not (Test-Path $manifestPath)) { throw "清单不存在：$manifestPath" }
  $lines = Get-Content -LiteralPath $manifestPath -Encoding UTF8 |
    Where-Object { $_ -match '\S' -and $_ -notmatch '^\s*#' }
  $bad = 0
  foreach ($line in $lines) {
    $parts = $line -split '\s+', 2
    if ($parts.Count -ne 2) { continue }
    $want = $parts[0]; $name = $parts[1].Trim()
    $p = Join-Path $baseDir $name
    if (-not (Test-Path -LiteralPath $p)) { Write-Host "缺文件: $name" -ForegroundColor Red; $bad++; continue }
    $got = Get-Sha256 $p
    if ($got -ne $want) {
      Write-Host "哈希不符: $name`n  期望 $want`n  实际 $got" -ForegroundColor Red
      $bad++
    }
  }
  if ($bad -gt 0) { throw "$bad 个文件与清单不符：$manifestPath" }
  Write-Host ("  ✅ {0}（{1} 个文件）" -f $manifestPath.Substring($root.Length + 1), $lines.Count) -ForegroundColor Green
}

if ($UpdateManifest) {
  # ⚠️ 只重写 testdata 那一份，**不碰** reference/0.1.3-parser/SHA256.txt：
  #    给参考实现重写清单等于把"它漂移过"这件事洗白 —— 那一份只许手工加文件时同步改，
  #    而且改之前先想清楚这是不是"换掉了参考实现"。
  if (-not (Test-Path $dst)) { throw "目录不存在：$dst" }
  $rows = @()
  foreach ($f in (Get-ChildItem -LiteralPath $dst -Recurse -File | Sort-Object FullName)) {
    $rel = $f.FullName.Substring($dst.Length + 1).Replace('\', '/')
    # ⚠️ 排除"跑测试留下来的垃圾"：清单文件自己，以及任何 `tmp-*`（**目录或文件**）。
    #    只按文件名排是不够的 —— 有一次漏掉了 `tmp-gains/cfg/settings.json` 那种
    #    "文件在 tmp-* 目录里"的形状，于是清单里混进 5 条跑测的临时配置。
    if ($rel -eq 'SHA256.txt') { continue }
    if ($rel -match '(^|/)tmp-') { continue }
    $rows += ('{0}  {1}' -f (Get-Sha256 $f.FullName), $rel)
  }
  $header = @(
    '# 冻结测试用词典清单（由 tools/freeze-fixtures.ps1 -UpdateManifest 生成）',
    '# ⚠️ 表头**刻意不写版本号**：这份清单跟着仓库走，写死一个版本号、下一次升版它就成了一句假话',
    '#    （它原来写的是 0.2.0）。要看这是哪一版，看 abi/lookup.abi.json 的 meta.version。',
    '# 来源：0.1.3 参考实现里的 testdata/ 与 tools/MdxProbe/variants/ —— 已逐字节冻结进本仓库，',
    '#       不再读任何兄弟目录（校验：pwsh -File tools/freeze-fixtures.ps1）',
    ('# 生成时间：{0:yyyy-MM-dd HH:mm:ss}' -f (Get-Date)),
    ''
  )
  # ⚠️ 写成 **带 BOM 的 UTF-8 + LF**。
  #
  # · **LF 不是随便挑的**：本仓库 `.gitattributes` 定的是 `* text=auto eol=lf` ——
  #   工作区里那份不管写成什么，**进版本库时都会被归一成 LF**。原来这里写的是 CRLF
  #   （Windows PowerShell `-Encoding UTF8` 的形状），于是：**克隆下来的是 LF、
  #   本机重新生成出来的是 CRLF** —— 下一次"只是重新生成了一遍"就变成**整份文件的行尾 diff**，
  #   而那份 diff 里没有一行是真正的改动（生成器原来那句注释想避免的正是这件事，
  #   只是它把行尾定成了 CRLF，而版本库那一侧并不接受）。
  #   把生成器与版本库**写成同一种行尾**，这个坑才真的不存在。
  # · **BOM 保留**：BOM 与行尾是两件事，归一化不动它；留着与历史上那份保持一致。
  $text = (($header + $rows) -join "`n") + "`n"
  [System.IO.File]::WriteAllText($manifest, $text, (New-Object System.Text.UTF8Encoding($true)))
  Write-Host "已按 testdata/ 现有文件重写清单（$($rows.Count) 个文件）：testdata/SHA256.txt" -ForegroundColor Green
  $rows | ForEach-Object { Write-Host "  $_" }
  exit 0
}

# ── 默认动作：校验 ────────────────────────────────────────────────────────────
Write-Host '── 冻结件校验 ──' -ForegroundColor Cyan
Test-Manifest $manifest $dst
if (Test-Path $refManifest) {
  Test-Manifest $refManifest $refBase
} else {
  # ⚠️ **不是失败，是正常状态**：参考实现是上一代（0.1.3）C# 实现的一部分，
  #    **不随本仓库发布**（移植已完成，日常回归不需要它）。这里**明着说一句**而不是静默
  #    跳过 —— "少核了一份东西"必须看得见。
  Write-Host '  · 参考实现不随本仓库发布 —— 跳过它的逐文件校验（那份清单跟着它自己一起走）' -ForegroundColor Yellow
}
Write-Host '冻结件全部与清单逐字节相同。' -ForegroundColor Green
exit 0
