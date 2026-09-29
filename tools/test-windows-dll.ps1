# Windows 侧验收（D 级）：**真 DLL + 生成的 C# 绑定 + 宿主适配层**。
#
#   powershell -File tools/test-windows-dll.ps1
#
# 它编 `tools/WindowsDllTest`（引用 `shell/Lookup.Interop` 与 `shell/Lookup.Host` 两个真工程），
# 把 `dsh_lookup.dll` 放到 exe 旁边（壳也是这么发布的），再用真测试用词典跑一遍**五组 +
# 虚拟资源主机**的断言。⚠️ 断言写在 C# 里（`tools/WindowsDllTest/`），脚本只管**编、摆、跑、清**。
# ⚠️ 下面那段防陈旧 gate：DLL 比内核源码旧就直接报错 —— 否则会报一个早就不存在的失败。

[CmdletBinding()]
param(
  [string]$Dll = "",
  [string]$TestData = ""
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

if (-not $Dll) { $Dll = Join-Path $root 'dist/win-x64/dsh_lookup.dll' }
if (-not $TestData) { $TestData = Join-Path $root 'testdata' }
$project = Join-Path $root 'tools/WindowsDllTest/WindowsDllTest.csproj'
$bindings = Join-Path $root 'shell/Lookup.Interop/DshLookup.g.cs'
$hostAdapter = Join-Path $root 'shell/Lookup.Host/VirtualHost.cs'

foreach ($p in @($Dll, $project, $bindings, $hostAdapter)) {
  if (-not (Test-Path $p)) { throw "找不到 $p（先跑 tools/build-windows-dll.sh 与 node tools/gen-bindings.mjs）" }
}

$dllPath = (Resolve-Path $Dll).Path
$testDataPath = (Resolve-Path $TestData).Path

# 引擎那一段要建一份设置文件，所以给它一个**临时目录** ——
# 绝不去碰用户真正的 %APPDATA%\LookupApp（旧名 %APPDATA%\查词；那会在验收时改掉他自己的设置）。
# ⚠️ 目录**先只算路径、等编译过了再建**：否则编译失败那一次会在 %TEMP% 里留一个空目录
# （验收工具不该在用户机器上留东西）。
$tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("dsh-wintest-" + [guid]::NewGuid().ToString("N"))

# ── 防陈旧：DLL 必须比内核源码新 ─────────────────────────────────────────────
$dllTime = (Get-Item $dllPath).LastWriteTimeUtc
$newestSrc = Get-ChildItem (Join-Path $root 'native/src') -Recurse -File -Include *.c, *.h |
  Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
$newestIncl = Get-Item (Join-Path $root 'native/include/dsh_lookup.h')
$newest = if ($newestSrc.LastWriteTimeUtc -gt $newestIncl.LastWriteTimeUtc) { $newestSrc } else { $newestIncl }
if ($newest.LastWriteTimeUtc -gt $dllTime) {
  throw ("DLL 比内核源码旧（$($dllPath) 建于 $dllTime，而 $($newest.Name) 改于 $($newest.LastWriteTimeUtc)）。" +
         "先跑：wsl.exe -- bash tools/build-windows-dll.sh <0.2.0 路径> Release")
}

$dotnet = if (Test-Path (Join-Path $env:USERPROFILE '.dotnet/dotnet.exe')) {
  Join-Path $env:USERPROFILE '.dotnet/dotnet.exe'
} else { 'dotnet' }

Write-Host "DLL     : $dllPath"
Write-Host "工程    : $project"
Write-Host "测试用词典    : $testDataPath"
Write-Host "临时配置: $tempDir"
Write-Host ""

Write-Host '── 编译 D 级验收（引用 shell/Lookup.Interop 与 shell/Lookup.Host）──' -ForegroundColor Cyan
& $dotnet build $project -c Release -v quiet --nologo
if ($LASTEXITCODE -ne 0) { throw "dotnet build 失败（退出码 $LASTEXITCODE）" }

$exe = Join-Path $root 'tools/WindowsDllTest/bin/Release/net48/WindowsDllTest.exe'
if (-not (Test-Path $exe)) { throw "没编出 $exe" }

# 壳的发布形态：DLL 与 exe **同目录**（`DllImport` 认的就是 dsh_lookup.dll 这个名字）
Copy-Item $dllPath (Join-Path (Split-Path -Parent $exe) 'dsh_lookup.dll') -Force

$cwd = Get-Location
$rc = 1
New-Item -ItemType Directory -Path $tempDir -Force | Out-Null
try {
  Set-Location (Split-Path -Parent $exe)
  & $exe $testDataPath $tempDir
  $rc = $LASTEXITCODE
} finally {
  Set-Location $cwd
  # ⚠️ 清理放在 finally 里：**失败时也要清**（验收工具在用户机器上拉屎是不能接受的副作用）
  Remove-Item -Recurse -Force $tempDir -ErrorAction SilentlyContinue
}
if ($rc -ne 0) { throw "Windows 侧验收失败" }
Write-Host '── Windows 侧验收通过 ──' -ForegroundColor Green
