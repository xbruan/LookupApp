# =============================================================================
# 生成词条正文的"资产"与标准答案文件 —— 全部来自 **0.1.3 的参考实现**（仓库内的只读副本
# 参考实现里的 `EntryDocument.cs`）。
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
#    **不随本仓库发布**（取回来时才需要它；逐字节哈希跟着它自己走）——
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
#    参考实现随后被冻结进本仓库，那个概念随之取消。
#    0.2.1 起参考实现**移出本仓库、改为归档**，所以"找不到"是**正常状态** ——
#    这条报错要做的是说清怎么接回来，而不是留一个死胡同。
if (-not (Test-Path $refSrc)) {
  throw @"
找不到参考实现源码：$refSrc

那份 C# 参考实现**不随本仓库发布**（只有"重新生成产物"才需要它）。
把它接回来：

  ① 找到归档包 reference-0.1.3-parser.zip
     SHA256 71eb1e2ed01db48f67c39700578d717a822df68d107845cdedeede943062e079
  ② tar -xf <归档目录>\reference-0.1.3-parser.zip -C <本仓库>\reference
  ③ 再跑本脚本

它是什么：这份源码**不随本仓库发布**，是上一代（0.1.3）C# 实现的一部分 ——
只有"重新生成产物"才需要它，日常开发、测试、打包都不需要。
"@
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
