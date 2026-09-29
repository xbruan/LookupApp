# =============================================================================
# 生成词条正文的"资产"与标准答案文件 —— 全部来自 **0.1.3 的参考实现**（仓库内的只读副本
# `reference/0.1.3-parser/src/Dictionary/EntryDocument.cs`）。
#
#   powershell -File tools/make-entry-assets.ps1
#
# 两个产物：
#   native/src/dict/entry_assets.h      参考实现的 BaseStyle / BridgeScript（原样）
#   native/tests/entry_doc_vectors.h    若干份完整文档（EntryDocument.Build 的输出）
#
# 为什么必须生成、不许手抄：那两段字符串七百多行（含中文注释、CSS 与桥接 JS），
# 而"手抄"这件事的失败模式特别坏 —— 抄错一个转义符，症状是词条页桥接不生效、
# 样式丢一半，从代码上根本看不出来。让参考实现自己吐出来，这一步就不存在了。
#
# ⚠️ 依赖 dotnet SDK（与 tools/make-golden.ps1 同一个找法）。参考实现是**仓库内**的一份
#    只读副本（reference/0.1.3-parser/，逐字节哈希见那边的 SHA256.txt）——
#    本脚本只编译它、不改它，也不再去任何兄弟目录里取源码。
# =============================================================================

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
# 这个仓库本身**就是** 0.2.0 的代码（没有再套一层版本目录），所以仓库根 = $PSScriptRoot 的父目录。
$root = Split-Path -Parent $PSScriptRoot
$outDir = Join-Path $root 'tools/golden'
$assetsHeader = Join-Path $root 'native/src/dict/entry_assets.h'
$vectorHeader = Join-Path $root 'native/tests/entry_doc_vectors.h'
$refSrc = Join-Path $root 'reference/0.1.3-parser/src/Dictionary/EntryDocument.cs'

# ⚠️ 这里原来找的是"版本目录的父目录/0.1.3/src/Dictionary/EntryDocument.cs"（兄弟目录）——
#    参考实现已冻结进本仓库，那个概念随之取消：找不到就是本仓库缺件。
if (-not (Test-Path $refSrc)) {
  throw "找不到参考实现源码：$refSrc`n（它应当随仓库一起提交，见 reference/0.1.3-parser/README.md）"
}

$dotnet = if (Test-Path (Join-Path $env:USERPROFILE '.dotnet/dotnet.exe')) {
  Join-Path $env:USERPROFILE '.dotnet/dotnet.exe'
} else { 'dotnet' }

$ver = & $dotnet --version 2>&1
if ($LASTEXITCODE -ne 0) { throw "dotnet 不可用：$ver" }
Write-Host "dotnet：$ver（$dotnet）" -ForegroundColor DarkGray

Write-Host '── 编译资产生成器（链仓库内的参考实现 EntryDocument.cs）──' -ForegroundColor Cyan
$proj = Join-Path $outDir 'EntryAssets.csproj'
& $dotnet build $proj -c Release -v quiet --nologo
if ($LASTEXITCODE -ne 0) { throw "dotnet build 失败（退出码 $LASTEXITCODE）" }

$exe = Join-Path $outDir 'bin/Release/net48/EntryAssets.exe'
if (-not (Test-Path $exe)) { throw "没编出 $exe" }

Write-Host '── 让参考实现吐出资产与标准答案文件 ──' -ForegroundColor Cyan
# ⚠️ 位置参数传路径：本仓库路径里有空格与中文，拼字符串再展开会被拆开
& $exe $assetsHeader $vectorHeader
if ($LASTEXITCODE -ne 0) { throw "EntryAssets 失败（退出码 $LASTEXITCODE）" }

foreach ($f in @($assetsHeader, $vectorHeader)) {
  Write-Host ("  ✅ $f（" + (Get-Item $f).Length + " 字节，UTF-8 无 BOM、LF）") -ForegroundColor Green
}
