#!/usr/bin/env pwsh
# 挪一下选项窗，看它背后的外壳层（阴影 / 圆角）跟不跟。
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\probe-manager-shadow.ps1 -Port 9425
#
# 为什么要有它：外壳层是**另一张分层窗口**（ChromeWindow），拖动走的是系统那条
# WM_NCLBUTTONDOWN + HTCAPTION 模态循环、没法用脚本驱动；而从外面用 SetWindowPos 挪一下，
# 窗口收到的是同一类 WM_MOVE。检查标准两条：宿主的 bounds 位移 == 我们挪的位移（按屏幕缩放
# 换算）；外壳的 chrome.bounds 位移**必须与它一模一样**。⚠️ 它会**真的挪那个窗口**（不还原）；
# 两个读数都来自应用自己（debug:window 走桥 → 宿主 → 真实窗口），不是截图。

[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][int]$Port,
  # 挪多少（**逻辑像素**：本脚本是 DPI-unaware 的，看到的坐标会被系统虚拟化）
  [int]$Dx = 200,
  [int]$Dy = 120,
  # 窗口标题**前缀**（壳让窗口标题跟着页签走：`选项 · 常规 / 词库 / 语音 / 翻译`，
  # 所以只能按前缀认 —— 用整串会在换页签之后找不到窗口）
  [string]$Title = '选项',
  # 点开管理窗那一页（默认语音页；空 = 不切）
  [string]$Tab = 'speech'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public class DshShadowWin {
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr p);
  public delegate bool EnumWindowsProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  public struct RECT { public int Left, Top, Right, Bottom; }
  /// 按**标题前缀**找那个窗口（整串匹配会在换页签之后失效：标题是 `选项 · <页签名>`）
  public static IntPtr FindByTitlePrefix(string want) {
    IntPtr found = IntPtr.Zero;
    EnumWindows(delegate(IntPtr h, IntPtr p) {
      if (!IsWindowVisible(h)) return true;
      var sb = new StringBuilder(256);
      GetWindowTextW(h, sb, 256);
      if (sb.ToString().StartsWith(want, StringComparison.Ordinal)) { found = h; return false; }
      return true;
    }, IntPtr.Zero);
    return found;
  }
}
'@

$scriptPath = Join-Path $env:TEMP ('dsh-shadow-' + [guid]::NewGuid().ToString('N') + '.js')
Set-Content -Path $scriptPath -Encoding UTF8 -Value @'
(async () => {
  const w = await window.dshLookup.debug.window('manager');
  return JSON.stringify({ bounds: w && w.bounds, chrome: w && w.chrome && w.chrome.bounds,
                          dpi: w && w.dpi, scale: w && w.scale });
})()
'@

function Read-Rects([int]$port, [string]$file) {
  $out = (& node (Join-Path $root 'tools/probe-page.mjs') --port $port --match manager.html --eval-file $file 2>&1) -join "`n"
  $m = [regex]::Match($out, '\{.*\}', 'Singleline')
  if (-not $m.Success) { throw "读不到 debug:window('manager')：$out" }
  return $m.Value | ConvertFrom-Json
}

$hwnd = [DshShadowWin]::FindByTitlePrefix($Title)
if ($hwnd -eq [IntPtr]::Zero) { throw "没找到标题以「$Title」开头的可见窗口（管理窗开着吗？）" }
$r = New-Object DshShadowWin+RECT
[void][DshShadowWin]::GetWindowRect($hwnd, [ref]$r)
Write-Host ("管理窗（本进程看到的逻辑像素）: {0},{1} {2}x{3}" -f $r.Left, $r.Top, ($r.Right - $r.Left), ($r.Bottom - $r.Top))

$before = Read-Rects $Port $scriptPath
Write-Host ("挪之前（应用自己报的物理像素）: 窗口 {0},{1}   外壳 {2},{3}" -f `
  $before.bounds.x, $before.bounds.y, $before.chrome.x, $before.chrome.y)

[void][DshShadowWin]::SetWindowPos($hwnd, [IntPtr]::Zero, $r.Left + $Dx, $r.Top + $Dy, 0, 0, 0x0001 -bor 0x0004)
Start-Sleep -Milliseconds 900

$after = Read-Rects $Port $scriptPath
Write-Host ("挪之后（应用自己报的物理像素）: 窗口 {0},{1}   外壳 {2},{3}" -f `
  $after.bounds.x, $after.bounds.y, $after.chrome.x, $after.chrome.y)

$scale = if ($before.scale -gt 0) { $before.scale } else { 1.0 }
$wantX = [math]::Round($Dx * $scale)
$wantY = [math]::Round($Dy * $scale)
$winDx = $after.bounds.x - $before.bounds.x
$winDy = $after.bounds.y - $before.bounds.y
$chrDx = $after.chrome.x - $before.chrome.x
$chrDy = $after.chrome.y - $before.chrome.y
Write-Host ("位移：窗口 {0},{1}（期望 {2},{3}）；外壳 {4},{5}（缩放 {6}）" -f `
  $winDx, $winDy, $wantX, $wantY, $chrDx, $chrDy, $scale)

if ($chrDx -eq 0 -and $chrDy -eq 0) {
  throw '外壳层**没动** —— 窗口挪了，阴影留在原地（这就是那个 bug）'
}
if ($chrDx -ne $winDx -or $chrDy -ne $winDy) {
  throw ("外壳层的位移与窗口不一致：窗口 {0},{1} vs 外壳 {2},{3}" -f $winDx, $winDy, $chrDx, $chrDy)
}
Write-Host '✓ 外壳层跟着窗口一起挪了（位移与宿主完全一致）' -ForegroundColor Green
