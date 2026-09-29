#!/usr/bin/env pwsh
# 内核构建入口（从 Windows 调 WSL 里的 gcc）：生成接口代码 → 编译 → 接口自检 + 单元测试。
#
#   pwsh -File tools/native-build.ps1 [-Asan | -Cover] [-SkipGen]
#     -Asan = ASan + UBSan 重编再跑一遍；-Cover = gcov 覆盖率；-SkipGen = 只改 C 代码时省一趟。
#
# 为什么经 WSL：这台机器上没有任何 Windows C 编译器，而 WSL 里 gcc + make + ASan/UBSan/gcov
# 齐全；内核是纯 C11、零系统依赖，所以只有平台层（platform/）才分平台。
# ⚠️ 生成接口代码用 **Windows 的 node**（WSL 里没装 node），所以顺序是这边生成、那边编译，
# 两边都对同一份 abi/lookup.abi.json。

[CmdletBinding()]
param(
  [switch]$Asan,
  [switch]$Cover,
  [switch]$SkipGen
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot   # 0.2.0/
Push-Location $root
try {
  # ── 1) 从唯一的接口定义文件生成接口代码（C 头 / TS / 文档）──
  if (-not $SkipGen) {
    Write-Host '── 自动生成接口代码（abi/lookup.abi.json → include / ts / docs）──' -ForegroundColor Cyan
    node tools/gen-bindings.mjs
    if ($LASTEXITCODE -ne 0) { throw "gen-bindings.mjs 失败（退出码 $LASTEXITCODE）" }
  } else {
    Write-Host '── 跳过生成（-SkipGen）──' -ForegroundColor DarkGray
  }

  # ── 2) 把 native/ 的 Windows 路径翻成 WSL 路径 ────────────────────────────
  $nativeWin = (Resolve-Path 'native').Path
  if ($nativeWin -notmatch '^([A-Za-z]):\\(.*)$') { throw "看不懂的路径：$nativeWin" }
  $drive = $Matches[1].ToLower()
  $rest = $Matches[2] -replace '\\', '/'
  $nativeWsl = "/mnt/$drive/$rest"

  # ── 3) 在 WSL 里编译 + 跑测试 ────────────────────────────────────────────
  $makeTarget = if ($Cover) { 'cover' } elseif ($Asan) { 'asan' } else { 'all' }
  Write-Host "── WSL: make $makeTarget（$nativeWsl）──" -ForegroundColor Cyan

  # 用 -lc 而不是 -c：让 make 的环境与登录 shell 一致（PATH 等）
  wsl.exe -- bash -lc "cd '$nativeWsl' && make $makeTarget"
  $code = $LASTEXITCODE

  if ($code -ne 0) {
    throw "内核构建/测试失败（退出码 $code）。上面 WSL 的输出就是现场。"
  }
  Write-Host '── 内核构建与测试全部通过 ──' -ForegroundColor Green
} finally {
  Pop-Location
}
