# D 级验收 gate：**真实窗口 → 真页面 → 通信桥 → 内核**。
#
#   powershell -File tools/test-shell-app.ps1
#
# 与 `tools/test-windows-dll.ps1` 是**两条不同的门**，不能互相替代：那道门把内核与宿主适配层
# 单独喂进去（不开窗口），验不了「WebView2 到底有没有把 https://lookup.local/… 交给宿主」、
# 「跨源 iframe 的字节有没有真的经虚拟主机进来」；四层之间的缝只有这道门是事实。
#
# 断言写在 C# 里（shell/Lookup.App/SelfCheck.cs），脚本只管**打前端、编壳、摆、跑、清**。
# 退出码：任一条不成立就 throw（非零）—— 脚本只管流程，逐条结果要读 SelfCheck 那份报告。
# ⚠️ 跑之前先 `node web/build.mjs`：`web/dist/floating.js` 是构建产物（不进版本库），
#    没有它 `floating.html` 会 404 —— 症状是「页面打不开」，像壳坏了，其实是前端没打。
# ⚠️ 跑之前先杀掉旧进程（编壳产物 `Lookup.App.exe`、打包产物 `LookupApp.exe`）：这一跑要现场
#    Copy-Item 覆盖 DLL，被占用时报「文件被占用」，看着像打包坏了。
# ⚠️ 自检**绝不许**碰用户真正的 `%APPDATA%\LookupApp`（旧名 `%APPDATA%\查词`）：内核把设置写在
#    `<配置目录>/settings.json`，所以这里一律给临时目录（`--config`）。

[CmdletBinding()]
param(
  [string]$Dll = "",
  [string]$TestData = "",
  [string]$ConfigDir = "",
  # 真 Key（可选）：给了就多跑 ⑰ 划词翻译 / ⑱ 在线响度两节，不给就明着跳过、gate 照旧**离线**。
  # ⚠️ 只从命令行来，**绝不写进仓库**（与 `tools/probe-mt.mjs` 同一条纪律）。
  [string]$MtKey = "",
  # 验**打包产物**时给这两个（`tools/package.ps1` 就是这么调的）：`-Exe` 指
  # `dist\LookupApp-<版本>\LookupApp.exe`、`-Web` 指它旁边的 `web\`。给了它**不再打前端、不再编壳、
  # 也不往包里拷 DLL**（验的就是包里那几份东西）；不给就是编壳 → 拷 DLL → 跑编译产物。
  [string]$Exe = "",
  [string]$Web = ""
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$packaged = -not [string]::IsNullOrEmpty($Exe)

if ($Web) {
  if (-not (Test-Path $Web)) { throw "找不到 -Web 指的目录：$Web" }
  $webRoot = (Resolve-Path $Web).Path
} else {
  $webRoot = Join-Path $root 'web'
}
if (-not $TestData) { $TestData = Join-Path $root 'testdata' }
if (-not $Dll) {
  # 验打包产物时，防陈旧检查要认**包里那一份** DLL（那才是要交付的东西）
  $Dll = if ($packaged) {
    Join-Path (Split-Path -Parent (Resolve-Path $Exe).Path) 'dsh_lookup.dll'
  } else {
    Join-Path $root 'dist/win-x64/dsh_lookup.dll'
  }
}
$appProject = Join-Path $root 'shell/Lookup.App/Lookup.App.csproj'
$webBuild = Join-Path $webRoot 'build.mjs'

$need = @($Dll, (Join-Path $webRoot 'floating.html'))
if (-not $packaged) { $need += @($appProject, $webBuild) }
foreach ($p in $need) {
  if (-not (Test-Path $p)) { throw "找不到 $p（先跑 tools/build-windows-dll.sh 与 node web/build.mjs）" }
}

$dllPath = (Resolve-Path $Dll).Path
$testDataPath = (Resolve-Path $TestData).Path
$webPath = (Resolve-Path $webRoot).Path
$testMdx = Join-Path $testDataPath 'test.mdx'
$audioMdx = Join-Path $testDataPath 'audio.mdx'
# `link.mdx`：`apple` 词条刻意撑到 ~3000 像素高，专给「返回上一词条时阅读位置要还原」
# 那条检查标准用 —— 词条不够高的话「回到顶部」与「回到原处」根本分不出来。
$linkMdx = Join-Path $testDataPath 'link.mdx'
# `audio.mdd`：**资源卷**。种子里必须写进 `mddPaths`，否则内核不知道这本有资源卷。
# ⚠️ 种子是**手写 JSON**，不走 `engine.dictAdd`（它会自己去找同目录的 .mdd）——
#    漏了这一步不会报错，只会让「有资源卷」那几条检查标准悄悄退化。
$audioMdd = Join-Path $testDataPath 'audio.mdd'
if (-not (Test-Path $testMdx)) { throw "没找到测试用词典 $testMdx" }
if (-not (Test-Path $audioMdx)) { throw "没找到测试用词典 $audioMdx（资源那条路由的 Range 检查标准要用它）" }
if (-not (Test-Path $audioMdd)) { throw "没找到测试用词典 $audioMdd（词典自带录音那条路要用它）" }

# ── 防陈旧：DLL 必须比内核源码新（与 test-windows-dll.ps1 同一条约定）──
$dllTime = (Get-Item $dllPath).LastWriteTimeUtc
$newestSrc = Get-ChildItem (Join-Path $root 'native/src') -Recurse -File -Include *.c, *.h |
  Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
$newestIncl = Get-Item (Join-Path $root 'native/include/dsh_lookup.h')
$newest = if ($newestSrc.LastWriteTimeUtc -gt $newestIncl.LastWriteTimeUtc) { $newestSrc } else { $newestIncl }
if ($newest.LastWriteTimeUtc -gt $dllTime) {
  throw ("DLL 比内核源码旧（$($dllPath) 建于 $dllTime，而 $($newest.Name) 改于 $($newest.LastWriteTimeUtc)）。" +
         "先跑：wsl.exe -- bash tools/build-windows-dll.sh <0.2.0 路径> Release")
}

# 临时配置目录**先只算路径、等编译过了再建**（编译失败那一次不该在 %TEMP% 里留一个空目录）。
if (-not $ConfigDir) {
  $ConfigDir = Join-Path ([System.IO.Path]::GetTempPath()) ("dsh-shellapp-" + [guid]::NewGuid().ToString("N"))
}
$madeConfig = $false

$dotnet = if (Test-Path (Join-Path $env:USERPROFILE '.dotnet/dotnet.exe')) {
  Join-Path $env:USERPROFILE '.dotnet/dotnet.exe'
} else { 'dotnet' }

Write-Host "DLL     : $dllPath"
Write-Host "外壳资源: $webPath"
Write-Host "测试用词典    : $testMdx"
Write-Host "临时配置: $ConfigDir"
if ($packaged) { Write-Host "模式    : **验打包产物**（不打前端、不编壳、不拷 DLL）" -ForegroundColor Yellow }
Write-Host ""

if ($packaged) {
  Write-Host '── ① / ② 跳过：这一跑验的是 dist 里那个包 ──' -ForegroundColor Yellow
} else {
  Write-Host '── ① 打前端（esbuild）──' -ForegroundColor Cyan
  Push-Location $webPath
  try {
    & node build.mjs
    if ($LASTEXITCODE -ne 0) { throw "node web/build.mjs 失败（退出码 $LASTEXITCODE）" }
  } finally { Pop-Location }
  if (-not (Test-Path (Join-Path $webPath 'dist/floating.js'))) { throw "前端没打出 dist/floating.js" }

  Write-Host '── ② 编壳（shell/Lookup.App，引用 Lookup.Host 与 Lookup.Interop）──' -ForegroundColor Cyan
  & $dotnet build $appProject -c Release -v quiet --nologo
  if ($LASTEXITCODE -ne 0) { throw "dotnet build 失败（退出码 $LASTEXITCODE）" }
}

$exe = if ($packaged) {
  (Resolve-Path $Exe).Path
} else {
  Join-Path $root 'shell/Lookup.App/bin/Release/net48/Lookup.App.exe'
}
if (-not (Test-Path $exe)) { throw "找不到要跑的那个 exe：$exe" }

if (-not $packaged) {
  # 壳的发布形态：DLL 与 exe **同目录**（`DllImport` 才找得到 dsh_lookup.dll）。
  Copy-Item $dllPath (Join-Path (Split-Path -Parent $exe) 'dsh_lookup.dll') -Force
}

# ── ②b 现场生成「同目录散放资源的词典」（词条里外链 .js 那条检查标准要用它）──
# 为什么要生成：现有 `.mdx` 没有一本带 `<script src=`，而这条只能由词条自己的 HTML 发起
# （内核不往词条里塞外链脚本）—— 不造就无从验起；造到 `testdata/tmp-sibling/`，因为
# `testdata/` 根目录按 SHA256 清单冻结，多一份现场生成的文件会把清单语义搅浑。
# 词条名 / 界面标记名与 `SelfCheck.cs` 里读它的那一节成对，改名要两处一起改。
# 词条名 / 界面标记名与 `SelfCheck.cs` 里读它的那一节成对，改名要两处一起改。
Write-Host '── ②b 造同目录散放资源的测试用词典（词条里的外链 .js 那条检查标准要用它）──' -ForegroundColor Cyan
$siblingDir = Join-Path $root 'testdata/tmp-sibling'
Push-Location $root
try {
  & node tools/make-sibling-fixture.mjs $siblingDir
  if ($LASTEXITCODE -ne 0) { throw "生成同目录散放资源的测试用词典失败（退出码 $LASTEXITCODE）" }
} finally { Pop-Location }
$siblingMdx = Join-Path $siblingDir 'sibling.mdx'
$siblingScript = Join-Path $siblingDir 'sibling.js'
$siblingControl = Join-Path $siblingDir 'sibling-control.mjs'
foreach ($p in @($siblingMdx, $siblingScript, $siblingControl)) {
  if (-not (Test-Path $p)) { throw "同目录散放资源的测试用词典缺件：$p" }
}

Write-Host '── ③ 跑自检（真实窗口 → 真实页面 → 通信桥 → 内核）──' -ForegroundColor Cyan
$rc = 1
New-Item -ItemType Directory -Path $ConfigDir -Force | Out-Null
$madeConfig = $true
$report = Join-Path $ConfigDir 'selfcheck.txt'
try {
  # ── 设置种子：造出「有一本词典的路径是假的」这个事实 ──
  # ⚠️ 为什么必须种：有一条检查标准要**终态页 + 「再问一遍」**，它要求「有词典没问完」
  #    （借查问到一半失败），而 `engine.dictAdd` 会**正确地**挡下不存在的路径 ——
  #    只能建引擎**之前**先落一份 settings.json。
  #    五本词典各供一条检查标准用（查词 / 资源卷 / 路径是假的 / 词条很高 / 同目录脚本）；
  #    ⚠️ `d-sibling` 的 `title` 是 SelfCheck 认它的唯一凭据，改标题要连读它的那一节一起改。
  $seed = Join-Path $ConfigDir 'settings-seed.json'
  $json = @"
{"version":1,"currentDictId":"d-test","dictionaries":[
{"id":"d-test","title":"好词典","mdxPath":"$($testMdx -replace '\\','\\')"},
{"id":"d-audio","title":"音频测试用词典","mdxPath":"$($audioMdx -replace '\\','\\')","mddPaths":["$($audioMdd -replace '\\','\\')"]},
{"id":"d-gone","title":"丢了的词典","mdxPath":"/no/such/place.mdx"},
{"id":"d-link","title":"链接测试用词典","mdxPath":"$($linkMdx -replace '\\','\\')"},
{"id":"d-sibling","title":"同目录脚本词典","mdxPath":"$($siblingMdx -replace '\\','\\')"}]}
"@
  [System.IO.File]::WriteAllText($seed, $json, (New-Object System.Text.UTF8Encoding($false)))

  # ── 历史种子：造出「有一条历史记录，而那一本的文件早就不在了」这个事实 ──
  # ⚠️ 为什么也要种：那条检查标准要求点这样一条历史时说**另一句话**（文件不在了、
  #    把它放回原处或重新导入）且**不发起查词**；而程序跑起来之后造不出来 ——
  #    Windows 上被内核映射过的 `.mdx` 删不掉，靠诊断脚本自己布景时而成功时而失败，比没测更坏。
  #    做法：让记录指向种子里**路径本来就是假的**那本 `d-gone`（内核只在启动那一刻读历史）。
  $histSeed = Join-Path $ConfigDir 'history.jsonl'
  $when = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() - 60000
  $line = '{"word":"ghostword","dictId":"d-gone","dictTitle":"丢了的词典","at":' + $when + "}`n"
  [System.IO.File]::WriteAllText($histSeed, $line, (New-Object System.Text.UTF8Encoding($false)))

  # ⚠️ 别改回 `& $exe …`：壳是 **WinExe**，PowerShell **不会等**它退出，`$LASTEXITCODE`
  #    拿到的是上一条命令的残留 —— 于是「自检一条断言都没跑」却报了通过。
  #    所以用 `Start-Process -Wait -PassThru` 拿退出码，`--report` 落文件读实测结果。
  $appArgs = @(
    '--selfcheck',
    '--web', "`"$webPath`"",
    '--config', "`"$ConfigDir`"",
    '--settings', "`"$seed`"",
    '--report', "`"$report`""
  )
  if ($MtKey) {
    $appArgs += @('--mt-key', "`"$MtKey`"")
    Write-Host '── 这一跑**带真 Key**：会多跑第 ⑰ / ⑱ 节（联网、按量计费）──' -ForegroundColor Yellow
  }
  $proc = Start-Process -FilePath $exe -Wait -PassThru -NoNewWindow -ArgumentList $appArgs
  $rc = $proc.ExitCode

  if (-not (Test-Path $report)) {
    throw "自检连报告都没写出来（$report）—— 说明它根本没跑到那一步"
  }
  Get-Content -LiteralPath $report -Encoding UTF8 | ForEach-Object { Write-Host $_ }
} finally {
  # ⚠️ 清理放在 finally 里：**失败时也要清**。
  # ⚠️ 次数不能少：WebView2 的浏览器进程（msedgewebview2.exe）**不会**在宿主退出那一刻收摊，
  #    它占着 `<配置目录>/webview2/` 里的 leveldb —— 次数少了**成功的那次也留一堆文件**。
  #    现在是 60 次 × 500 ms（30 s）；还清不掉就 `Write-Warning` 把路径打出来，绝不不报错地放过。
  for ($i = 0; $i -lt 60 -and $madeConfig; $i++) {
    try { Remove-Item -Recurse -Force $ConfigDir -ErrorAction Stop; break }
    catch { Start-Sleep -Milliseconds 500 }
  }
  if ($madeConfig -and (Test-Path $ConfigDir)) {
    Write-Warning "临时配置目录没清干净（WebView2 的子进程还占着）：$ConfigDir"
  }
}
if ($rc -ne 0) { throw "真实程序自检失败（退出码 $rc）" }

# ── ④ 「启动时显示悬浮窗」的**效果**：重启一次程序才验得到 ──
# 降不到 A/B/C：那个值内核读得对（B 级已钉），但「设成 false 之后下一次启动屏幕上真的没有胶囊」
# 必须**再启动一次**、且在窗口显示出来之前观测 —— C 级诊断脚本连上去的时候已经晚了。
# 两个方向都验，否则设置被忽略时这些断言照样绿：
#   ① `showFloatingOnStartup:false` → 进程活着，但**没有任何可见窗口**；
#   ② 反向对照：同一份配置改成 `true` → 进程活着，而且**有可见窗口**。
Write-Host '── ④ 第二趟启动：设置里关掉「启动时显示悬浮窗」→ 起来之后不该有可见窗口 ──' -ForegroundColor Cyan

Add-Type -Namespace DshGate -Name Win -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc f, IntPtr p);
public delegate bool EnumWindowsProc(IntPtr h, IntPtr p);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
'@

# 这个进程有几个**可见**的顶层窗口（WebView2 那些辅助窗口都是不可见的，不计）
function Get-VisibleWindows([int]$ownerPid) {
  $out = New-Object System.Collections.ArrayList
  $cb = [DshGate.Win+EnumWindowsProc] {
    param($h, $p)
    # ⚠️ 变量名不能叫 `$pid`：那是 PowerShell 的只读自动变量。
    $op = 0
    [void][DshGate.Win]::GetWindowThreadProcessId($h, [ref]$op)
    if ($op -eq $ownerPid -and [DshGate.Win]::IsWindowVisible($h)) { [void]$out.Add($h) }
    return $true
  }
  [void][DshGate.Win]::EnumWindows($cb, [IntPtr]::Zero)
  return $out
}

$startCfg = Join-Path ([System.IO.Path]::GetTempPath()) ("dsh-startshow-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $startCfg -Force | Out-Null
$startReport = @()
$startFailures = 0
$startCases = @(
  @{ label = 'showFloatingOnStartup=false'; setting = $false; extra = @(); wantVisible = $false },
  @{ label = 'showFloatingOnStartup=true（反向对照）'; setting = $true; extra = @(); wantVisible = $true },
  # ★ 第三档：**开机拉起那条命令行**（注册表 Run 里写的就是 `<exe> --autostart`）。
  #   它曾经不被 `AppPaths.Parse` 认 → 抛异常 → **悄悄退出（退出码 2、无窗口）**，症状是
  #   「勾了开机自启动，重启之后它根本没起来」；这一档钉的就是带它也得照常起来、窗口照常显示。
  @{ label = '--autostart（开机拉起那一条，H1）'; setting = $true; extra = @('--autostart');
     wantVisible = $true }
)
try {
  foreach ($case in $startCases) {
    # 每一档都**重写一份干净的设置**（内核会把它规范化之后再写回来）
    $seedJson = '{"version":1,"dictionaries":[],"currentDictId":null,"closeBehavior":"ask",' +
                '"showFloatingOnStartup":' + ($(if ($case.setting) { 'true' } else { 'false' })) + '}'
    [System.IO.File]::WriteAllText((Join-Path $startCfg 'settings.json'), $seedJson,
                                   (New-Object System.Text.UTF8Encoding($false)))

    $argList = @('--web', "`"$webPath`"", '--config', "`"$startCfg`"", '--dict', "`"$testMdx`"")
    if ($case.extra.Count -gt 0) { $argList += $case.extra }
    $p = Start-Process -FilePath $exe -PassThru -ArgumentList $argList
    # 采 2 秒（每 50 ms 一次）：窗口只在起来那一瞬间出现，采晚一点就错过了
    $seen = 0
    for ($i = 0; $i -lt 40; $i++) {
      Start-Sleep -Milliseconds 50
      $seen = [Math]::Max($seen, @(Get-VisibleWindows $p.Id).Count)
    }
    $alive = -not $p.HasExited
    if ($alive) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 400

    $okAlive = $alive
    $okSeen = if ($case.wantVisible) { $seen -ge 1 } else { $seen -eq 0 }
    if (-not ($okAlive -and $okSeen)) { $startFailures++ }
    $startReport += ("    {0} {1} → 进程活着={2} / 可见窗口最多 {3} 个（{4}）" -f `
      $(if ($okAlive -and $okSeen) { '✓' } else { '✗' }), $case.label, $alive, $seen,
      $(if ($case.wantVisible) { '≥1 才算对' } else { '必须 0' }))
  }
} finally {
  for ($i = 0; $i -lt 40; $i++) {
    try { Remove-Item -Recurse -Force $startCfg -ErrorAction Stop; break }
    catch { Start-Sleep -Milliseconds 500 }
  }
  if (Test-Path $startCfg) { Write-Warning "第二趟启动的临时配置目录没清干净：$startCfg" }
}
$startReport | ForEach-Object { Write-Host $_ }
if ($startFailures -ne 0) {
  throw "「启动时显示悬浮窗」/「--autostart」那几条不成立：$startFailures / $($startCases.Count) 档没对上（见上面实测结果）"
}
Write-Host '  ✓ 三档都对：关掉之后真的没有可见窗口、开着的时候真的有、带 --autostart 也照常起来' -ForegroundColor Green

Write-Host '── 真实程序那条门通过 ──' -ForegroundColor Green
