#!/usr/bin/env pwsh
# 把 Big5 码表从 python3 的 big5 切成 cp950（.NET 的 Encoding.GetEncoding(950)），
# 并**同时**重生成固化在 test_textcodec.c 里的对照测试用例。
#
#   pwsh -File tools/switch-big5-to-cp950.ps1
#
# 为什么要切：参考实现（参考实现）用的是 .NET 的 cp950，两者的码表在若干槽位上取值不同，
# 而对照测试要求「逐字节对齐参考实现」，所以以 cp950 为准。
# ⚠️ 两件事必须一起做：码表与用例出自同一个开关，只换一个测试就会红。

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot          # 0.2.0/
$test = Join-Path $root 'native/tests/test_textcodec.c'
if (-not (Test-Path $test)) { throw "找不到 $test" }

$win = (Resolve-Path $root).Path
$wsl = "/mnt/" + $win.Substring(0,1).ToLower() + ($win.Substring(2) -replace '\\','/')

function Run-Wsl([string]$cmd) {
  $out = wsl.exe -- bash -lc $cmd 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "WSL 命令失败：$cmd`n$out" }
  return $out
}

Write-Host '① 重生成码表（cp950）' -ForegroundColor Cyan
Run-Wsl "cd '$wsl' && python3 tools/make-textcodec-tables.py --big5-codec cp950" | Write-Host

Write-Host '② 重生成对照测试用例，替换 test_textcodec.c 里那一整块' -ForegroundColor Cyan
$cases = Run-Wsl "cd '$wsl' && python3 tools/make-textcodec-tables.py --print-cases --big5-codec cp950"

# 用例块的边界：脚本生成的块以「--print-cases 生成后粘进来」那句注释开头，
# 以「共 N 组对照测试用例。」那句注释结尾（这句是被 -match 找的**原文**，不许改）。
$lines = [System.Collections.Generic.List[string]]([IO.File]::ReadAllLines($test))
$startIdx = -1; $endIdx = -1
for ($i = 0; $i -lt $lines.Count; $i++) {
  if ($startIdx -lt 0 -and $lines[$i] -match '--print-cases 生成后粘进来') { $startIdx = $i }
  if ($startIdx -ge 0 -and $lines[$i] -match '组对照测试用例') { $endIdx = $i; break }
}
if ($startIdx -lt 0 -or $endIdx -lt 0) {
  throw "没找到用例块的边界（start=$startIdx end=$endIdx）—— 手工确认 test_textcodec.c 的结构再改这个脚本"
}

$newCases = ($cases -replace "`r`n", "`n").TrimEnd("`n") -split "`n"
$out = @()
$out += $lines[0..($startIdx - 1)]
$out += $newCases
$out += $lines[($endIdx + 1)..($lines.Count - 1)]
[IO.File]::WriteAllLines($test, $out)

Write-Host "   替换了第 $($startIdx+1) ~ $($endIdx+1) 行（旧 $($endIdx-$startIdx+1) 行 → 新 $($newCases.Count) 行）" -ForegroundColor Green

Write-Host '③ 重新构建并跑测试' -ForegroundColor Cyan
Run-Wsl "cd '$wsl/native' && make 2>&1 | grep -E '项，失败|FAIL|error' | head -20" | Write-Host
