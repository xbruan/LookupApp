# =============================================================================
# 生成 HTML 标准答案文件表：让 **0.1.3 的参考实现（C# `HtmlUtils`，仓库内的只读副本
# `reference/0.1.3-parser/src/Dictionary/HtmlUtils.cs`）** 把一批 HTML 输入的处理结果
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
#    可能只有运行时）。参考实现是**仓库内**的一份只读副本（reference/0.1.3-parser/），
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
