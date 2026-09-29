#!/usr/bin/env pwsh
# 启动那一瞬间屏幕上闪过什么（「先显示、再摆位」那个 bug 的现场）。
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\probe-startup-flash.ps1 -Exe dist\LookupApp-<版本>\LookupApp.exe
# 为什么要有它：这类 bug 的现场只有几十毫秒，截图与事后读 DOM 都抓不到 —— 只能从 Start-Process 起
# 用 Win32 高频枚举这个进程的**每一个顶层窗口**（位置 / 尺寸 / 可见 / 类名）。
# 检查标准（脚本自己下、非零退出）：每个窗口第一次出现的位置**不许**落在屏幕左上角那种默认位置
# （0,0 附近）—— 那是「先显示、再摆位」的指纹，而规矩是**先摆位、再显示**；刻意摆到屏幕外不算。
# ⚠️ 它**只读**窗口信息，不点、不输入；跑完把进程杀掉（--config 给临时目录，不碰用户配置）。

[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][string]$Exe,
  # 采样时长（毫秒）与间隔（毫秒）
  [int]$Ms = 2500,
  [int]$Every = 5,
  # 跑几趟（这个 bug 是**时序竞态**：一趟抓不到不等于没有 —— 默认跑 3 趟看复现率）
  [int]$Runs = 3,
  # 允许的「左上角」判定半径（物理像素）：窗口出现时若落在这个方框里就判失败
  [int]$CornerBox = 80,
  # 可选：自检模式（把窗口摆到屏幕外那一路也顺便测）
  [switch]$NoShow
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Exe)) { throw "找不到 $Exe" }
$exePath = (Resolve-Path $Exe).Path

Add-Type -Namespace DshProbe -Name Win -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);
public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr hWnd, System.Text.StringBuilder name, int max);
public struct RECT { public int Left, Top, Right, Bottom; }
'@

function Get-WindowsOf([int]$processId) {
  $found = New-Object System.Collections.ArrayList
  $callback = [DshProbe.Win+EnumWindowsProc] {
    param($hWnd, $lParam)
    # ⚠️ 变量名不能叫 `$pid`：那是 PowerShell 的只读自动变量（改名 `$ownerPid` 才行）
    $ownerPid = 0
    [void][DshProbe.Win]::GetWindowThreadProcessId($hWnd, [ref]$ownerPid)
    if ($ownerPid -eq $processId) {
      $rect = New-Object DshProbe.Win+RECT
      [void][DshProbe.Win]::GetWindowRect($hWnd, [ref]$rect)
      $name = New-Object System.Text.StringBuilder 256
      [void][DshProbe.Win]::GetClassName($hWnd, $name, 256)
      [void]$found.Add([pscustomobject]@{
        Hwnd    = $hWnd
        Class   = $name.ToString()
        Left    = $rect.Left
        Top     = $rect.Top
        Width   = $rect.Right - $rect.Left
        Height  = $rect.Bottom - $rect.Top
        Visible = [DshProbe.Win]::IsWindowVisible($hWnd)
      })
    }
    return $true
  }
  [void][DshProbe.Win]::EnumWindows($callback, [IntPtr]::Zero)
  return $found
}

$runsResult = New-Object System.Collections.ArrayList
for ($run = 1; $run -le $Runs; $run++) {
  $configDir = Join-Path ([System.IO.Path]::GetTempPath()) ('dsh-flash-' + [guid]::NewGuid().ToString('N'))
  New-Item -ItemType Directory -Force -Path $configDir | Out-Null

  $appArgs = @('--config', "`"$configDir`"")
  if ($NoShow) { $appArgs += '--no-show' }

  $proc = Start-Process -FilePath $exePath -PassThru -ArgumentList $appArgs
  $timeline = New-Object System.Collections.ArrayList
  $seen = @{}
  $flashes = New-Object System.Collections.ArrayList
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  try {
    while ($sw.ElapsedMilliseconds -lt $Ms) {
      foreach ($w in (Get-WindowsOf $proc.Id)) {
        $key = "$($w.Hwnd)"
        if (-not $seen.ContainsKey($key)) {
          $seen[$key] = $true
          [void]$timeline.Add([pscustomobject]@{
            AtMs = $sw.ElapsedMilliseconds
            Class = $w.Class
            Rect = "$($w.Left),$($w.Top) $($w.Width)x$($w.Height)"
            Left = $w.Left; Top = $w.Top; Width = $w.Width; Height = $w.Height
            Visible = $w.Visible
          })
        }
        # ★ 每一次采样都判一遍（而不是只看「每个窗口第一次出现」）：那个 bug 是**同一个窗口**
        # 先以默认尺寸露一下、随后才被摆到正确位置，只记「第一次出现」会漏掉它。
        if ($w.Visible -and $w.Width -le 320 -and $w.Height -le 320 -and
            $w.Left -ge -$CornerBox -and $w.Left -le $CornerBox -and
            $w.Top -ge -$CornerBox -and $w.Top -le $CornerBox) {
          $rect = "$($w.Left),$($w.Top) $($w.Width)x$($w.Height)"
          if ($flashes.Count -eq 0 -or $flashes[$flashes.Count - 1].Rect -ne $rect) {
            [void]$flashes.Add([pscustomobject]@{ AtMs = $sw.ElapsedMilliseconds; Rect = $rect })
          }
        }
      }
      Start-Sleep -Milliseconds $Every
    }
  } finally {
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 400
    Remove-Item $configDir -Recurse -Force -ErrorAction SilentlyContinue
  }

  [void]$runsResult.Add([pscustomobject]@{ Run = $run; Flashes = $flashes; Timeline = $timeline })
  $mark = if ($flashes.Count -gt 0) { "抓到 $($flashes.Count) 次" } else { '干净' }
  Write-Host ("  第 {0} 跑：{1}（首次可见几何：{2}）" -f $run, $mark,
    (($timeline | Where-Object { $_.Visible } | Select-Object -First 1).Rect))
  if ($run -eq 1) {
    Write-Host ''
    Write-Host "  ── 第 1 跑的窗口时间线（每个顶层窗口第一次出现）──"
    foreach ($t in $timeline) {
      Write-Host ("    {0,5} ms  visible={1,-5}  {2,-30} {3}" -f $t.AtMs, $t.Visible, $t.Class, $t.Rect)
    }
  }
}

$caught = @($runsResult | Where-Object { $_.Flashes.Count -gt 0 })
Write-Host ''
if ($caught.Count -gt 0) {
  foreach ($r in $caught) {
    foreach ($f in $r.Flashes) {
      Write-Warning ("第 {0} 跑：{1} ms 屏幕上有一块 {2} 的窗口（没摆位就先显示了）" -f $r.Run, $f.AtMs, $f.Rect)
    }
  }
  throw ("启动瞬间有可见窗口出现在屏幕左上角：{0}/{1} 跑抓到（这就是「闪一下」那个 bug）" -f $caught.Count, $Runs)
}
Write-Host ("✓ {0} 跑全都没有出现「没摆位的默认尺寸」窗口" -f $Runs) -ForegroundColor Green
