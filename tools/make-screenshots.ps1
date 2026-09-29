#!/usr/bin/env pwsh
# =============================================================================
# 给公开仓库出**产品截图**（README 用）。
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\make-screenshots.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\make-screenshots.ps1 -Out D:\shots
#
# ## 两条硬要求（决定了这个脚本怎么写）
#
# 1. **截图里不许出现开发者本人的东西**：词典名、路径、Key 都不能露。
#    所以它用 `testdata/` 里那几本**冻结测试用词典**启动（名字是中性的），
#    而且**不读** `%APPDATA%` 里那份真实配置（`--config <临时目录>`）。
# 2. **必须可复现**：UI 改了要重拍，所以它不是一段一次性命令，而是这个脚本。
#
# 出图靠 `tools/probe-page.mjs`（CDP）：窗口在屏幕外也能拍（`--no-show` + 挪到 −4000），
# 所以跑这个脚本**不会在你桌面上弹窗**。
# =============================================================================

[CmdletBinding()]
param(
  [string]$Exe = "",
  [string]$Web = "",
  [string]$Out = "",
  [int]$Port = 9334,
  # 查哪个词（用它出"悬浮窗查词结果"那张图）
  [string]$Word = "apple"
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Exe) { $Exe = Join-Path $root 'shell/Lookup.App/bin/Release/net48/Lookup.App.exe' }
if (-not $Web) { $Web = Join-Path $root 'web' }
if (-not $Out) { $Out = Join-Path $root 'release/screenshots' }

if (-not (Test-Path $Exe)) { throw "找不到外壳程序：$Exe（先编一次：dotnet build shell\Lookup.App\Lookup.App.csproj -c Release）" }
if (-not (Test-Path (Join-Path $Web 'floating.html'))) { throw "找不到界面资源：$Web" }
New-Item -ItemType Directory -Force -Path $Out | Out-Null

# 上一趟的实例会占着 exe / DLL（`dotnet build` 会因此失败），先收掉
Get-Process -Name 'Lookup.App' -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800

# 临时配置目录（空目录 = 一本真实词典都没有；词典靠下面的 --dict 现场加）
$cfg = Join-Path ([System.IO.Path]::GetTempPath()) ('dsh-shots-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $cfg | Out-Null

# ★ 用冻结测试用词典（名字中性、内容固定）：test = 6 个常用词、titled = 头里有书名、audio = 带原录音
$dicts = @('test.mdx', 'titled.mdx', 'audio.mdx') | ForEach-Object { Join-Path $root "testdata/$_" }
foreach ($d in $dicts) { if (-not (Test-Path $d)) { throw "缺测试用词典：$d" } }

$probe = Join-Path $PSScriptRoot 'probe-page.mjs'
function Shoot([string]$match, [string]$file, [string[]]$extra) {
  $a = @('tools/probe-page.mjs', '--port', "$Port", '--match', $match) + $extra + @('--shot', (Join-Path $Out $file))
  Push-Location $root
  try { & node @a | Out-Null } finally { Pop-Location }
  Write-Host "  出图 $file"
}

$args = @('--web', "`"$Web`"", '--config', "`"$cfg`"", '--no-show', '--debug-port', "$Port")
foreach ($d in $dicts) { $args += @('--dict', "`"$d`"") }
$proc = Start-Process -FilePath $Exe -PassThru -ArgumentList $args
Write-Host "── 起程序（离屏，pid=$($proc.Id)）──"

try {
  # 等 DevTools 端口起来（它要装 WebView2，冷启动几秒）
  $ready = $false
  for ($i = 0; $i -lt 30; $i++) {
    Start-Sleep -Milliseconds 500
    try { [void](Invoke-WebRequest -UseBasicParsing "http://127.0.0.1:$Port/json" -TimeoutSec 2); $ready = $true; break } catch { }
  }
  if (-not $ready) { throw "等不到 DevTools 端口 $Port（程序没起来？）" }

  # ① 悬浮窗：打字 + 回车 → 把词条正文拍下来。
  #    ⚠️ 用合成的 KeyboardEvent：页面是在 `dom.input` 的 keydown 上接线的，够用（不依赖 isTrusted）。
  $typeJs = Join-Path ([System.IO.Path]::GetTempPath()) ('dsh-type-' + [guid]::NewGuid().ToString('N') + '.js')
  @"
(() => {
  const i = document.getElementById('input')
  i.focus()
  i.value = '$Word'
  i.dispatchEvent(new Event('input', { bubbles: true }))
  i.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true, cancelable: true }))
  return 'typed: $Word'
})()
"@ | Set-Content -Path $typeJs -Encoding UTF8
  Push-Location $root
  try { & node tools/probe-page.mjs --port $Port --match floating.html --eval-file $typeJs } finally { Pop-Location }
  Start-Sleep -Seconds 3   # 等词条 iframe 加载出来
  Shoot 'floating.html' 'screenshot-floating.png' @()

  # ② 选项窗：先叫它开出来（悬浮窗那一页的「选项」走的就是这条路），再逐个页签拍
  Push-Location $root
  try { & node tools/probe-page.mjs --port $Port --match floating.html --manager --manager-open | Out-Null } finally { Pop-Location }
  Start-Sleep -Seconds 3
  $tabs = @(
    @{ id = 'tabDicts'; file = 'screenshot-options-dicts.png' },
    @{ id = 'tabSpeech'; file = 'screenshot-options-speech.png' },
    @{ id = 'tabTranslate'; file = 'screenshot-options-translate.png' },
    @{ id = 'tabGeneral'; file = 'screenshot-options-general.png' }
  )
  foreach ($t in $tabs) { Shoot 'manager.html' $t.file @('--tabs', '--tabs-click', $t.id) }

  Remove-Item $typeJs -Force -ErrorAction SilentlyContinue
  Write-Host "`n=== 出好了 ===" -ForegroundColor Green
  Get-ChildItem $Out -Filter '*.png' | Sort-Object Name |
    ForEach-Object { Write-Host ("  {0,9:N0} B  {1}" -f $_.Length, $_.Name) }
} finally {
  if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
  Start-Sleep -Milliseconds 500
  Remove-Item -Recurse -Force $cfg -ErrorAction SilentlyContinue
}
