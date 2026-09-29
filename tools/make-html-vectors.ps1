# =============================================================================
# 生成 HTML 标准答案文件表：让 **0.1.3 的参考实现（C# `HtmlUtils`，仓库内的只读副本
# 参考实现里的 `HtmlUtils.cs`）** 把一批 HTML 输入的处理结果
# 吐成 C 头文件，给内核的 `dsh_html_*` 逐字节对照测试。
#
#   powershell -File tools/make-html-vectors.ps1
#
# 输出：native/tests/html_vectors.h（【生成】不许手改）
# 消费方：native/tests/test_html.c（`make test` 里那一组）
#
# 为什么值得走一趟 C#：C 版是把 8 条 .NET 正则在 C 里手写成扫描器，
# 而"手写的正则"正是最容易"99% 一样、1% 悄悄不一样"的东西 ——
# 字面量 `<` 与实体 `&lt;` 的先后、空行收敛到两行还是一行、Unicode 空白的
# trim 范围（U+3000 算不算）、孤立代理项怎么处理，全靠这份表针对。
#
# ⚠️ 依赖：dotnet SDK（与 tools/make-golden.ps1 同一个找法 —— 系统 PATH 上那个
#    可能只有运行时）。参考实现**不随本仓库发布**（取回来时才需要它），
#    本脚本只编译它、不改它，也不再去任何兄弟目录里取源码。
# =============================================================================

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
# 这个仓库本身**就是** 0.2.0 的代码（没有再套一层版本目录），所以仓库根 = $PSScriptRoot 的父目录。
$root = Split-Path -Parent $PSScriptRoot
$outDir = Join-Path $root 'tools/golden'
$header = Join-Path $root 'native/tests/html_vectors.h'
$refSrc = Join-Path $root 'reference/0.1.3-parser/src/Dictionary/HtmlUtils.cs'

# ⚠️ 这里原来找的是"版本目录的父目录/0.1.3/src/Dictionary/HtmlUtils.cs"（兄弟目录）——
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

Write-Host '── 编译 HTML 向量生成器（链仓库内的参考实现 HtmlUtils.cs）──' -ForegroundColor Cyan
$proj = Join-Path $outDir 'HtmlVectors.csproj'
& $dotnet build $proj -c Release -v quiet --nologo
if ($LASTEXITCODE -ne 0) { throw "dotnet build 失败（退出码 $LASTEXITCODE）" }

$exe = Join-Path $outDir 'bin/Release/net48/HtmlVectors.exe'
if (-not (Test-Path $exe)) { throw "没编出 $exe" }

Write-Host '── 跑参考实现，写出标准答案文件表 ──' -ForegroundColor Cyan
# ⚠️ 用**位置参数**把路径交进去：本仓库的路径里有空格与中文，
#    拼字符串再展开会被按空格拆开（这个仓库为这条踩过坑）。
& $exe $header
if ($LASTEXITCODE -ne 0) { throw "HtmlVectors 失败（退出码 $LASTEXITCODE）" }

$size = (Get-Item $header).Length
Write-Host "  ✅ $header（$size 字节，UTF-8 无 BOM、LF）" -ForegroundColor Green
