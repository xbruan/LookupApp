#!/usr/bin/env pwsh
# =============================================================================
# 打包：Release 构建 → `dist/LookupApp-<版本>/` → 便携版 zip → **对着打包产物跑 gate**。
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\package.ps1
#
# 为什么这一版要自己一份 `package.ps1`（参考实现那份**不能照抄**）：两版的产物形状不同 ——
#   参考实现：一个 exe + 几个 dll，**前端是嵌进 exe 的**（`web/dist/*.js` 编进了资源）；
#   这一版：exe 旁边还必须有 **`web/` 这一目录**（`AppPaths.FindWebRoot` 是从 exe 所在目录
#          往上找 `web/floating.html`，而页面按相对路径加载 `styles/*.css` 与 `dist/*.js`）。
#   所以这一版的"少搬一个文件"不能靠 exe 自己兜住 —— 下面那条**资源清单自检**就是为它写的：
#   把打包后的 html 里所有相对 `href/src` 抠出来逐个查存在性，缺一个当场红。
#
# ⚠️ 这一步**不是"编完就完"**：交付前必须跑 `AGENTS.md` §二 里「打包发布前」那一格
#   "打包发布前 = 完整测试 gate **+** 对照测试 gate"，而且**完整测试 gate 要打在打包产物上** ——
#   所以本脚本最后一步就是把 `tools/test-shell-app.ps1` 指到这个目录里的 `LookupApp.exe`
#   （`-Exe` / `-Web` 那两个参数就是为此加的），再跑 `tools/golden-gate.ps1`。
#   要只打包不验：`-SkipVerify`（查现场时用）。
#
# ⚠️ 运行打包产物**不会**把 `dist/` 弄脏：设置与 WebView2 的 user data 都在 `--config`
#   指的那个目录里（`WebView2Setup` 的 `Path.Combine(configDir, "webview2")`）——
#   脚本会在跑完门之后**再核一遍清单**，任何变化都如实报出来。
# =============================================================================

[CmdletBinding()]
param(
  [string]$Version = '0.2.1',
  # 只打包不验（跳过最后两道 gate）
  [switch]$SkipVerify,
  # 可选：带真 Key 跑那一趟（多验 ⑰ 划词翻译 / ⑱ 在线响度；联网、按量计费）
  [string]$MtKey = ""
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$dotnet = if (Test-Path (Join-Path $env:USERPROFILE '.dotnet/dotnet.exe')) {
  Join-Path $env:USERPROFILE '.dotnet/dotnet.exe'
} else { 'dotnet' }

$distDir = Join-Path $root 'dist'
$appName = "LookupApp-$Version"
$appDir = Join-Path $distDir $appName
$zipPath = Join-Path $distDir "$appName-portable.zip"
$binDir = Join-Path $root 'shell/Lookup.App/bin/Release/net48'
$webRoot = Join-Path $root 'web'

# ── ① 打前端（esbuild）────────────────────────────────────────────────────────
Write-Host '── ① 打前端（node web/build.mjs）──' -ForegroundColor Cyan
Push-Location $webRoot
try {
  & node build.mjs
  if ($LASTEXITCODE -ne 0) { throw "node web/build.mjs 失败（退出码 $LASTEXITCODE）" }
} finally { Pop-Location }
foreach ($js in @('dist/floating.js', 'dist/manager.js', 'dist/tray-menu.js')) {
  if (-not (Test-Path (Join-Path $webRoot $js))) { throw "前端产物缺件：web/$js" }
}

# ── ② 编 Windows 内核 DLL ────────────────────────────────────────────────────
# ⚠️ 必须**先编 DLL 再 stage**：`test-shell-app.ps1` / `test-windows-dll.ps1` 都有
#    "DLL 比内核源码新"的防陈旧检查，而它认的是**这个** DLL 的时间戳。
Write-Host '── ② 编 Windows 内核 DLL ──' -ForegroundColor Cyan
$wslRoot = if ($root -match '^([A-Za-z]):\\?(.*)$') {
  '/mnt/' + $Matches[1].ToLower() + '/' + ($Matches[2] -replace '\\', '/')
} else { throw "不知道怎么把这个路径转成 WSL 路径：$root" }
# ⚠️ 脚本路径也必须是 **WSL 路径**：PowerShell 往 `wsl.exe` 传参时会把反斜杠吃掉
#    （第一版传的是 `C:\…\tools\build-windows-dll.sh`，WSL 侧看到的是
#    `C:Users…toolswindows-dll.sh`，报 `No such file or directory`，退出码 127）。
& wsl.exe -- bash "$wslRoot/tools/build-windows-dll.sh" $wslRoot Release
if ($LASTEXITCODE -ne 0) { throw "编 DLL 失败（退出码 $LASTEXITCODE）" }
$dllSrc = Join-Path $root 'dist/win-x64/dsh_lookup.dll'
if (-not (Test-Path $dllSrc)) { throw "没编出 $dllSrc" }

# ── ③ 编壳（Release）─────────────────────────────────────────────────────────
Write-Host '── ③ 编壳（shell/Lookup.App，Release）──' -ForegroundColor Cyan
& $dotnet build (Join-Path $root 'shell/Lookup.App/Lookup.App.csproj') -c Release -v quiet --nologo
if ($LASTEXITCODE -ne 0) { throw "dotnet build 失败（退出码 $LASTEXITCODE）" }

# ── ④ stage ──────────────────────────────────────────────────────────────────
Write-Host "── ④ 摆 $appName ──" -ForegroundColor Cyan
Remove-Item $appDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $appDir | Out-Null

# 壳那一批：`.exe` / `.config` / `.dll`，剔掉 WPF 版程序集（我们用 WinForms 承载 WebView2）
# 与 `.pdb`（那是给我们自己查栈用的，不该进分发包）。
$skip = @('Microsoft.Web.WebView2.Wpf.dll')
Get-ChildItem $binDir -File |
  Where-Object { $_.Name -notin $skip -and $_.Extension -in '.exe', '.dll', '.config' } |
  ForEach-Object { Copy-Item $_.FullName $appDir }
# ⚠️ 下面这一步**不能省**（2026-09 "包里的 exe 时间怎么还是旧的"时查出来的）：
#    `binDir` 里那份 `dsh_lookup.dll` 是**上一次**"谁把它拷过去"留下的 ——
#    第 ② 步重编的是 `dist/win-x64/dsh_lookup.dll`，而 `dotnet build` **不会**把它搬到
#    `bin/Release/net48/`（那条拷贝平时是 `test-shell-app.ps1` 干的）。所以照抄 `binDir`
#    会把**上一轮的旧内核**打进包里（改内核之后尤其危险：exe 是新的、内核是旧的）。
#    这里用第 ② 步刚编出来的那一份**覆盖**掉，并当场核对哈希 —— 包里的内核必须就是这一趟编的。
Copy-Item $dllSrc (Join-Path $appDir 'dsh_lookup.dll') -Force
$packed = Get-FileHash (Join-Path $appDir 'dsh_lookup.dll') -Algorithm SHA256
$fresh = Get-FileHash $dllSrc -Algorithm SHA256
if ($packed.Hash -ne $fresh.Hash) { throw 'staged 的内核 DLL 与这一趟编出来的不是同一份' }
Write-Host ("  ✓ 内核 DLL 用的是这一趟编的那份（SHA256 {0}…）" -f $fresh.Hash.Substring(0, 16))
# exe 与它那份 config **一起改名**（net48 认的是 `<exe 名>.config` —— 只改一个就会不报错地丢配置）。
Move-Item (Join-Path $appDir 'Lookup.App.exe') (Join-Path $appDir 'LookupApp.exe')
Move-Item (Join-Path $appDir 'Lookup.App.exe.config') (Join-Path $appDir 'LookupApp.exe.config')

# 前端那一批：**只搬运行时真正读的那几个**（`.ts` 源码、`node_modules`、`build.mjs` 不进包）。
# 清单是**写死**的 —— 新增一个运行时文件时必须来这儿加一行（下面那条自检会替你发现漏搬）。
$webStage = Join-Path $appDir 'web'
New-Item -ItemType Directory -Force -Path (Join-Path $webStage 'dist') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $webStage 'styles') | Out-Null
foreach ($f in @('floating.html', 'manager.html', 'tray-menu.html', 'bridge.js')) {
  Copy-Item (Join-Path $webRoot $f) $webStage
}
Copy-Item (Join-Path $webRoot 'dist/*.js') (Join-Path $webStage 'dist')
Copy-Item (Join-Path $webRoot 'styles/*.css') (Join-Path $webStage 'styles')

# 许可与第三方声明：**必须随包**（2026-09 补）。`dsh_lookup.dll` 里内嵌了 libspeex 的解码路径，
# 而 BSD 3-Clause 要求再分发时**保留版权声明、条件与免责声明** —— 包里少这几份就不是"没写全"，
# 而是**违反它自己的许可**。三份都摆到包根上（Windows 上双击就能看）。
foreach ($lic in @(
    @{ From = (Join-Path $root 'LICENSE');        To = 'LICENSE.txt' },
    @{ From = (Join-Path $root 'THIRD-PARTY.md'); To = 'THIRD-PARTY.txt' },
    @{ From = (Join-Path $root 'native/vendor/speex/COPYING'); To = 'libspeex-COPYING.txt' }
  )) {
  if (-not (Test-Path $lic.From)) { throw "缺许可文件：$($lic.From)" }
  Copy-Item $lic.From (Join-Path $appDir $lic.To) -Force
}

# 打包标记（用户 2026-09 问"包里的 exe 时间还是 1:56 的，我不知道我耳朵测的是不是最新版"）：
# `Copy-Item` / `Move-Item` **保留 mtime**、而 `dotnet build` 在没改 C# 源码时**不重新链接** ——
# 所以**文件时间戳根本不能用来判断"这份包是不是最新"**。这里把"这一趟打包"的证据写进说明文件：
# 打包时刻 + 两个二进制的 SHA256 前 12 位（与 `dist/win-x64/dsh_lookup.dll`、`bin/Release/net48`
# 里那两份逐字节相同，脚本第 ④ 步已经核过）。
$exeHash = (Get-FileHash (Join-Path $appDir 'LookupApp.exe') -Algorithm SHA256).Hash
Set-Content -Path (Join-Path $appDir '使用说明.txt') -Encoding UTF8 -Value @"
LookupApp · 悬浮 MDict 词典  v$Version（内核 C 重写版）

运行「LookupApp.exe」即可，不需要安装。

依赖说明
  界面由系统的 Microsoft Edge WebView2 运行时渲染。
  Windows 11 以及装了新版 Edge 的 Windows 10 都自带，无需额外安装。
  如果提示缺少运行时，按提示打开下载页面装一次即可。

词库
  在悬浮窗左侧小图标上点右键 -> 「选项」->「词库」，选「添加词典文件」，选择硬盘上的 .mdx 文件。
  同目录同名的 .mdd / .1.mdd / .2.mdd 资源库会自动关联。
  不带 .mdd 的词典（样式表 / 脚本 / 字体 / 图片放在 .mdx 旁边同一个目录）也能用。
  ⚠️ 这一版**会执行词典自带的 .js**（在隔开的沙箱里跑）；.mjs / .html / .htm 仍不执行。

设置与历史
  存放在 %APPDATA%\LookupApp\settings.json（**全 ASCII 的目录名**）。
  旧版本用的是 %APPDATA%\查词 —— 第一次运行本版时，会把那个目录里的设置与历史
  **复制**一份到新目录（旧目录原样保留，参考实现系列还在用它），
  所以词库列表、查词历史、悬浮窗位置都会原样带过来。
  选项窗口的「常规」页里能定：开机自动启动、启动时显不显示悬浮窗、
  关闭悬浮窗时怎么办，以及一键打开上面这个目录。
  （「启动时不显示悬浮窗」意思是：程序照常启动、托盘图标还在，只是不摆出胶囊 ——
    要它的时候在托盘图标上点右键选「显示悬浮窗」，或者双击托盘图标。）

目录结构（别把 web 这个目录删掉）
  LookupApp.exe        主程序
  dsh_lookup.dll      词典内核（查词 / 解析 / 发音 / 翻译都在这儿）
  Lookup.*.dll        外壳与接口代码绑定
  web\                界面资源（页面 / 脚本 / 样式）

许可
  本程序以 MIT 许可发布 —— 全文见 LICENSE.txt。
  内嵌的第三方代码（libspeex，用来播词典自带的 .spx 录音）按 BSD 3-Clause 授权，
  全文见 libspeex-COPYING.txt；其余第三方情况见 THIRD-PARTY.txt。
  再分发本包时请把这三份文件一起带上。

这一份是什么时候打的（**判断"是不是最新"请看这里，别看文件时间**）
  打包时间   $((Get-Date).ToString('yyyy-MM-dd HH:mm:ss'))
  LookupApp.exe  SHA256 $($exeHash.Substring(0,12))…
  dsh_lookup.dll SHA256 $($fresh.Hash.Substring(0,12))…
"@

# ── ⑤ 资源清单自检 + 清单存档 ────────────────────────────────────────────────
# 把打包后 html 里所有**相对** href/src 抠出来，逐个查在不在包里 —— 少搬一个 css
# 的症状是"页面样式不对/脚本没跑"，而那很容易被当成"壳坏了"。
Write-Host '── ⑤ 资源清单自检（页面引用的相对文件是否都在包里）──' -ForegroundColor Cyan
$missing = @()
foreach ($html in Get-ChildItem $webStage -File -Filter *.html) {
  foreach ($m in [regex]::Matches((Get-Content -LiteralPath $html.FullName -Raw), '(?:href|src)="([^"]+)"')) {
    $ref = $m.Groups[1].Value
    if ($ref -match '^[a-zA-Z]+:' -or $ref.StartsWith('//') -or $ref.StartsWith('#')) { continue }
    $rel = ($ref -split '\?')[0] -replace '^\./', ''
    if (-not (Test-Path (Join-Path $webStage $rel))) { $missing += "$($html.Name) → $ref" }
  }
}
if ($missing.Count -gt 0) { throw ("打包产物缺件（页面引用了但包里没有）：`n  " + ($missing -join "`n  ")) }
Write-Host '  ✓ 三个页面引用的文件都在包里'

function Get-Manifest([string]$dir) {
  Get-ChildItem $dir -Recurse -File | Sort-Object FullName | ForEach-Object {
    [pscustomobject]@{
      Rel  = $_.FullName.Substring($dir.Length + 1)
      Size = $_.Length
      Hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
    }
  }
}
$manifestBefore = Get-Manifest $appDir
Write-Host ("  清单：{0} 个文件，合计 {1:N0} 字节（{2:N2} MB）" -f `
  $manifestBefore.Count, ($manifestBefore | Measure-Object Size -Sum).Sum,
  (($manifestBefore | Measure-Object Size -Sum).Sum / 1MB))

# ── ⑥ 便携版 zip ────────────────────────────────────────────────────────────
# 形状与参考实现那份**逐条一致**：`Compress-Archive -Path "$appDir\*"` —— 解压出来就是
# 一堆文件（没有多包一层同名目录）。
Write-Host '── ⑥ 便携版 zip ──' -ForegroundColor Cyan
Remove-Item $zipPath -Force -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $appDir '*') -DestinationPath $zipPath -CompressionLevel Optimal
$zip = Get-Item $zipPath

# ── ⑦ 对着**打包产物**跑 gate ──────────────────────────────────────────────────
$appExe = Join-Path $appDir 'LookupApp.exe'
if (-not $SkipVerify) {
  Write-Host '── ⑦ 对着打包产物跑 D 级门（真实窗口 → 真实页面 → 通信桥 → 内核）──' -ForegroundColor Cyan
  $verifyArgs = @{ Exe = $appExe; Web = $webStage }
  if ($MtKey) { $verifyArgs.MtKey = $MtKey }
  & (Join-Path $root 'tools/test-shell-app.ps1') @verifyArgs

  Write-Host '── ⑧ 对照测试门（与 参考实现逐字节比）──' -ForegroundColor Cyan
  & (Join-Path $root 'tools/golden-gate.ps1')

  # ── ⑨ 正常启动那一趟（**双击那条路**）─────────────────────────────────────
  #
  # ⚠️ 为什么非有不可：D 级 gate 一直是**带 `--web`** 跑的，所以"exe 自己能不能找到界面资源"
  #    这件事在那一趟里**根本没被验过**；而打包产物恰恰只能走这条路（用户不会加参数）。
  #    检查标准一条正向、一条反向 —— 只有正向说明不了"它读的是包里那份 web\"（可能悄悄
  #    读了别处一份），所以反向那份必须做：把包**复制**一份、删掉它的 `web/`，
  #    同样起法**必须挂不出那一页**。复制件用完就删，产物本身一个字节都不动。
  Write-Host '── ⑨ 正常启动那一趟（不带 --web：让 exe 自己找 web\）──' -ForegroundColor Cyan
  Get-Process -Name '查词', 'Lookup.App' -ErrorAction SilentlyContinue | Stop-Process -Force
  Start-Sleep -Seconds 1

  $probe = Join-Path $root 'tools/probe-page.mjs'
  $probeExpr = @'
(function () { return 'PKG|' + decodeURIComponent(location.href) + '|' + (typeof (window.dshLookup || {}).floating); })()
'@.Trim()

  function Start-PkgLaunch([string]$dir, [int]$port) {
    $cfg = Join-Path ([System.IO.Path]::GetTempPath()) ('dsh-pkglaunch-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $cfg | Out-Null
    $proc = Start-Process -FilePath (Join-Path $dir 'LookupApp.exe') -PassThru `
      -ArgumentList @('--config', "`"$cfg`"", '--debug-port', "$port")
    return @{ Proc = $proc; Config = $cfg }
  }
  function Wait-Floating([int]$port, [int]$seconds) {
    for ($i = 0; $i -lt $seconds; $i++) {
      Start-Sleep -Seconds 1
      $out = (& node $probe --port $port --eval $probeExpr 2>&1) -join "`n"
      if ($LASTEXITCODE -eq 0 -and $out -match 'floating\.html\|object') { return $out }
    }
    return $null
  }
  function Stop-PkgLaunch($job) {
    if ($job -and $job.Proc) { Stop-Process -Id $job.Proc.Id -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Seconds 2
    if ($job -and $job.Config) { Remove-Item $job.Config -Recurse -Force -ErrorAction SilentlyContinue }
  }

  $pos = Start-PkgLaunch $appDir 9411
  try {
    $hit = Wait-Floating 9411 20
    if (-not $hit) { throw "不带 --web 起不来，或者调试端口上没有 floating.html（正常启动那条路坏了）" }
    Write-Host "  ✓ 正常启动：$hit"
  } finally { Stop-PkgLaunch $pos }

  # 反向对照：把 web\ 删掉之后，**自检**必须明着报「没找到外壳资源目录」。
  #
  # ⚠️ 这一趟原来走的是"正常启动 + 看调试端口上有没有那一页"—— 而正常启动在找不到 `web\` 时
  #    会**弹一个模态错误框**（`Program.cs` 的 `win.Ready` 失败那一支，文案里带
  #    `web/floating.html`）。跑 gate 的机器就是用户的桌面，那个框会真的弹到用户脸上
  #    （用户 2026-09 报："你的测试中弹出错误提示：外壳资源打不开……web/floating.html"）。
  #    自检那条路上同一件事只是**打一行 `[!!]` + 退 2**，既不弹框、检查标准还更直接。
  # 复制一份包、把它的 `web\` 删掉（**不碰真产物**，`finally` 里删干净）。
  $copyDir = Join-Path ([System.IO.Path]::GetTempPath()) ('dsh-pkgcopy-' + [guid]::NewGuid().ToString('N'))
  Copy-Item $appDir $copyDir -Recurse
  Remove-Item (Join-Path $copyDir 'web') -Recurse -Force
  $negConfig = Join-Path ([System.IO.Path]::GetTempPath()) ('dsh-negcfg-' + [guid]::NewGuid().ToString('N'))
  $negReport = Join-Path $negConfig 'neg-selfcheck.txt'
  New-Item -ItemType Directory -Force -Path $negConfig | Out-Null
  $negProc = Start-Process -FilePath (Join-Path $copyDir 'LookupApp.exe') -Wait -PassThru -NoNewWindow `
    -ArgumentList @('--selfcheck', '--config', "`"$negConfig`"", '--report', "`"$negReport`"",
                    '--dict', "`"$(Join-Path $root 'testdata/test.mdx')`"")
  $negText = if (Test-Path $negReport) { Get-Content -LiteralPath $negReport -Raw -Encoding UTF8 } else { '' }
  if ($negProc.ExitCode -ne 2 -or $negText -notmatch '没找到外壳资源目录') {
    throw ("反向对照失败：删掉包里的 web\ 之后自检**没有**明着报「没找到外壳资源目录」" +
           "（退出码 $($negProc.ExitCode)）—— 说明它读的不是包里那个 web\")
  }
  Write-Host '  ✓ 反向对照：删掉包里的 web\ 之后自检明着报「没找到外壳资源目录」（退出码 2）'
  Remove-Item $negConfig -Recurse -Force -ErrorAction SilentlyContinue
  for ($i = 0; $i -lt 10; $i++) {
    try { Remove-Item $copyDir -Recurse -Force -ErrorAction Stop; break }
    catch { Start-Sleep -Milliseconds 500 }
  }

  # ── ⑩ 复核：跑过之后产物有没有变样 ────────────────────────────────────────
  #
  # 设置与 WebView2 的 user data 都在 `--config` 那个临时目录里，所以**跑一次不该改产物**。
  # 多出 / 少掉 / 变样都要如实说 —— 那说明产物会自我修改，用户机器上就是"跑一次就变脏"。
  Write-Host '── ⑩ 复核：跑过之后产物有没有变样 ──' -ForegroundColor Cyan
  $manifestAfter = Get-Manifest $appDir
  $diff = Compare-Object $manifestBefore $manifestAfter -Property Rel, Size, Hash
  if ($diff) {
    $diff | ForEach-Object { Write-Warning ("{0} {1}" -f $_.SideIndicator, $_.Rel) }
    throw "打包产物在跑过之后变了样（上面这些）—— 别把会自我修改的目录发给用户"
  }
  Write-Host '  ✓ 逐文件（路径 + 大小 + SHA256）与打包时完全一致'
}

# ── 实测结果 ────────────────────────────────────────────────────────────────────
Write-Host ''
Write-Host '=== 目录产物 ==='
$manifestBefore | Sort-Object Size -Descending |
  ForEach-Object { '{0,10:N0}  {1}' -f $_.Size, $_.Rel }
$dirSum = ($manifestBefore | Measure-Object Size -Sum).Sum
Write-Host ('{0,10:N0}  合计（{1:N2} MB）' -f $dirSum, ($dirSum / 1MB))

Write-Host ''
Write-Host '=== 分发产物 ==='
'{0,10:N0}  {1}' -f $zip.Length, $zip.Name
'{0,10:N0}  合计（{1:N2} MB）' -f $zip.Length, ($zip.Length / 1MB)
'SHA256     ' + (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash
Write-Host ''
Write-Host '── 打包完成 ──' -ForegroundColor Green
