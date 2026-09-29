#!/usr/bin/env node
/**
 * 脚本编码检查 —— **专治"本地照不出来"的那一类**：Windows PowerShell 5.1 读一个
 * **没有 BOM 的 UTF-8 文件**时，会按**机器的 ANSI 代码页**去解码。
 *
 * ── 为什么必须有一条机械检查（这是真踩过的，不是洁癖）────────────────────────
 * 本仓库所有 `tools/*.ps1` 都写着中文注释与中文报错文案。在**本机**（ANSI 代码页正好是
 * UTF-8 / 65001，见 `chcp`）怎么跑都对 —— 于是这个 bug 潜伏了很久。
 * 一旦到了代码页不是 UTF-8 的机器（英文 Windows 是 1252），那些中文字节会被按 cp1252 解码成
 * 乱码；而乱码里会出现 `’`（cp1252 的 0x92）这类字符 —— PowerShell 把它当成**单引号**，
 * 于是**整个脚本语法错误**、一行都跑不了。
 *
 * 实测（GitHub runner 上真的红了）：`tools/test-windows-dll.ps1` 报 6 处语法错误，
 * 全部 13 个 `tools/*.ps1` 在 cp1252 下都有 3–24 处错误 —— 也就是说
 * **整套 Windows 侧工具链在非 UTF-8 代码页的机器上是坏的**。
 *
 * 修法：给 `.ps1` 加 **UTF-8 BOM**（PowerShell 见到 BOM 就按 UTF-8 读，与机器代码页无关）。
 * 这条检查就是钉住"加了就别去掉"。
 *
 * ── 各扩展名要什么（别一刀切）──────────────────────────────────────────────
 *   · `.ps1` —— **必须有 BOM**（上面的原因）；
 *   · `.sh`  —— **必须没有 BOM**：BOM 会跑到 `#!/bin/sh` 前面，shebang 立刻失效；
 *   · `.json`—— **必须没有 BOM**：`JSON.parse` / `require()` 见到 BOM 会直接报错；
 *   · 其余文本 —— 只要求**是合法 UTF-8**（内容里不带坏字节）。
 *
 * 用法：node tools/check-encoding.mjs        # 有问题 → 退出码 1
 */

import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const BOM = Buffer.from([0xef, 0xbb, 0xbf])

/** 不看的目录：产物 / 依赖 / 归档 */
const SKIP_DIR = /^(node_modules|\.git|logs|dist|build.*|\.toolchain|bin|obj|out|reference|testdata)$/

/** 要检查的文本类扩展名 */
const TEXT_EXT = new Set([
  '.ps1', '.sh', '.mjs', '.cjs', '.js', '.ts', '.py', '.cs', '.c', '.h',
  '.md', '.json', '.html', '.css', '.yml', '.yaml', '.txt', '.gitignore', '.gitattributes',
])

/**
 * **有意不是合法 UTF-8** 的文件 —— 豁免"必须是合法 UTF-8"那一条（BOM 那两条照旧管）。
 *
 * 为什么会有这种文件：标准答案文件对照测试的那份基线是拿 C# 参考实现跑出来的，
 * 而参考实现会把 **GBK / BIG5 词典里的原文按原字节吐进 JSON** —— 于是那份 JSON 里
 * 本来就带着非法 UTF-8 字节。这正是 `tools/golden/compare-golden.py` 的 `load()`
 * 要按 `errors='replace'` 解的原因（它那一层的目的是比结构，不是比编码）。
 * 所以这不是"文件坏了"，是**它的内容性质如此**。
 */
const ALLOW_INVALID_UTF8 = new Set([
  'tools/golden/baseline/golden-baseline.json',
])

function walk(dir, out = []) {
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    if (e.name.startsWith('.') && e.name !== '.gitignore' && e.name !== '.gitattributes' && e.name !== '.github') continue
    const full = path.join(dir, e.name)
    if (e.isDirectory()) {
      if (SKIP_DIR.test(e.name)) continue
      walk(full, out)
    } else {
      out.push(full)
    }
  }
  return out
}

const problems = []
const counts = { ps1: 0, sh: 0, json: 0, other: 0 }

for (const file of walk(ROOT)) {
  const rel = path.relative(ROOT, file).replace(/\\/g, '/')
  const ext = path.extname(file).toLowerCase()
  if (!TEXT_EXT.has(ext) && !/\.(gitignore|gitattributes)$/.test(file) && !rel.startsWith('.github/')) continue

  const buf = fs.readFileSync(file)
  const hasBom = buf.length >= 3 && buf[0] === 0xef && buf[1] === 0xbb && buf[2] === 0xbf
  const body = hasBom ? buf.subarray(3) : buf

  // 必须是合法 UTF-8（坏字节 → 解码出来会带 U+FFFD）
  if (!ALLOW_INVALID_UTF8.has(rel) && body.toString('utf8').includes('\uFFFD')) {
    problems.push(`${rel}：不是合法的 UTF-8（有坏字节）`)
  }

  if (ext === '.ps1') {
    counts.ps1++
    if (!hasBom) {
      problems.push(
        `${rel}：**.ps1 必须有 UTF-8 BOM** —— 没有的话，在 ANSI 代码页不是 UTF-8 的机器上` +
        `（英文 Windows 是 1252）中文注释会变乱码、整个脚本语法错误。加 BOM 的办法见本文件顶部。`
      )
    }
  } else if (ext === '.sh') {
    counts.sh++
    if (hasBom) {
      problems.push(`${rel}：**.sh 不许有 BOM** —— BOM 会跑到 \`#!/bin/sh\` 前面，shebang 失效。`)
    }
  } else if (ext === '.json') {
    counts.json++
    if (hasBom) {
      problems.push(`${rel}：**.json 不许有 BOM** —— \`JSON.parse\` / \`require()\` 见到 BOM 会直接报错。`)
    }
  } else {
    counts.other++
  }
}

console.log('脚本编码检查（专治"本地照不出来"的那一类）：')
console.log(`  · .ps1  ${counts.ps1} 个（必须带 BOM）`)
console.log(`  · .sh   ${counts.sh} 个（不许带 BOM）`)
console.log(`  · .json ${counts.json} 个（不许带 BOM）`)
console.log(`  · 其余文本 ${counts.other} 个（只要求合法 UTF-8）`)

if (problems.length > 0) {
  console.log('')
  for (const p of problems) console.log(`  ✗ ${p}`)
  console.log('')
  console.log('怎么修：')
  console.log('  · `.ps1` 缺 BOM（正文一个字节都不用改，只在文件最前面加 3 个字节 EF BB BF）：')
  console.log('      $p = "<文件>"')
  console.log('      $b = [IO.File]::ReadAllBytes($p)')
  console.log('      $out = New-Object byte[] ($b.Length + 3)')
  console.log('      $out[0]=0xEF; $out[1]=0xBB; $out[2]=0xBF')
  console.log('      [Array]::Copy($b, 0, $out, 3, $b.Length); [IO.File]::WriteAllBytes($p, $out)')
  console.log('    ⚠️ 常见来源：**有些编辑器 / 改写工具会顺手把 BOM 吃掉** —— 改完 .ps1 记得跑一次本检查。')
  console.log('  · `.sh` / `.json` 多了 BOM：把那 3 个字节删掉即可。')
  console.log('')
  console.log(`✗ 脚本编码检查：${problems.length} 处不合格`)
  process.exit(1)
}
console.log('')
console.log('✓ 脚本编码检查：全部合格（.ps1 都带 BOM；.sh / .json 都不带 BOM；都是合法 UTF-8）')
