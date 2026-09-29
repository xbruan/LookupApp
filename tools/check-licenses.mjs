#!/usr/bin/env node
/**
 * 许可与第三方声明检查 —— **专治两类"发出去以后才发现"的毛病**：
 *
 * ── ① `LICENSE` 里被塞进别的东西 ───────────────────────────────────────────
 * `LICENSE` 是**一份逐字不动的 MIT 全文**，不是"顺便写点别的"的地方。
 * 真发生过：MIT 正文后面接了一段中文，讲内嵌的 libspeex 怎么授权。它**不违反** MIT
 * （那段在正文之外，也没缩小授权），但它是错的写法：
 *   · 许可证文件被改了内容之后，下游的许可识别工具 / 打包者会当成"MIT 的改写版"处理；
 *   · 同一件事在 `THIRD-PARTY.md` 里**已经有了第二份**，两份必然各说各话；
 *   · 里面用的是 Markdown 链接，而这个文件会被**原样搬进便携包当 `.txt`** —— 包里的
 *     `native/vendor/speex/COPYING` 这条路径根本不存在（包里那份叫 `libspeex-COPYING.txt`）。
 * 所以这一条钉死：**`LICENSE` 必须逐字等于 MIT 全文（只许换版权人那一行），且全是 ASCII。**
 *
 * ── ② 随包的许可清单跟 `package.ps1` 对不上 ───────────────────────────────
 * 包里有两处 BSD 3-Clause 的东西（内嵌的 libspeex、随包分发的 WebView2 DLL），
 * 按它的**二进制再分发**条款，随包的文档里必须带上版权声明、条件与免责声明 ——
 * 也就是说"包里有几份许可文件"这件事是**硬要求**，不能靠记性。
 * `tools/package.ps1` 第 ④ 步是唯一搬它们的地方，`THIRD-PARTY.md` 开头那张对照表是唯一
 * 说明它们叫什么的地方。这一条把**两边对起来比**，两个方向都查：
 *   · 搬了但清单里没写 → 红（清单不许漏说一份随包文件）；
 *   · 清单里写了但没搬 → 红（包里那份根本不存在）。
 * 另外这条也顺手钉住"`THIRD-PARTY.md` 里不许出现仓库相对链接"—— 它同样会被原样搬进包。
 *
 * 用法：
 *   node tools/check-licenses.mjs                                    # 有问题 → 退出码 1
 *   DSH_LICENSES_ROOT=<别的仓库根> node tools/check-licenses.mjs     # 负向对照：拿修之前的
 *                                                                    # 那份仓库跑，**必须变红**
 */

import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const HERE = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const ROOT = process.env.DSH_LICENSES_ROOT ? path.resolve(process.env.DSH_LICENSES_ROOT) : HERE

/** MIT 全文（`<版权行>` 那一行允许不同，其余逐字） */
const MIT_LICENSE = `MIT License

Copyright (c) <版权行>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.`

const problems = []
const notes = []

const read = (rel) => {
  const p = path.join(ROOT, rel)
  return fs.existsSync(p) ? fs.readFileSync(p, 'utf8').replace(/^\uFEFF/, '') : null
}

// ── ① `LICENSE` 必须逐字等于 MIT 全文 ────────────────────────────────────────
const license = read('LICENSE')
if (license === null) {
  problems.push('`LICENSE` 不存在 —— 没有它就没有任何授权。')
} else {
  const bad = license.match(/[^\x00-\x7F]/)
  if (bad) {
    const line = license.slice(0, bad.index).split('\n').length
    problems.push(
      `\`LICENSE\` 第 ${line} 行有非 ASCII 字符（\`${bad[0]}\`）—— **MIT 全文是纯 ASCII**。` +
      `别在这里写中文注释、也别在这里解释第三方授权：那些属于 \`THIRD-PARTY.md\`。`
    )
  }
  const got = license.replace(/\s+$/, '').split('\n')
  const want = MIT_LICENSE.replace(/\s+$/, '').split('\n')
  if (got.length !== want.length) {
    problems.push(
      `\`LICENSE\` 行数不对：实测 ${got.length} 行，MIT 全文是 ${want.length} 行 —— ` +
      `多出来的那段多半是"顺便写点别的"：把它**搬**进 \`THIRD-PARTY.md\`，别留在 \`LICENSE\` 里。`
    )
  }
  const n = Math.min(got.length, want.length)
  for (let i = 0; i < n; i++) {
    if (i === 2) {
      if (!/^Copyright \(c\) \d{4} \S.*$/.test(got[i])) {
        problems.push(`\`LICENSE\` 第 3 行不是版权行（实测 \`${got[i]}\`）—— 形如 \`Copyright (c) 2026 <名字>\`。`)
      }
      continue
    }
    if (got[i] !== want[i]) {
      problems.push(
        `\`LICENSE\` 第 ${i + 1} 行与 MIT 全文不一致：\n` +
        `      实测 \`${got[i]}\`\n      应为 \`${want[i]}\``
      )
      break
    }
  }
}

// ── ② `THIRD-PARTY.md` 不许有仓库相对链接（它会被原样搬进包）────────────────
const thirdParty = read('THIRD-PARTY.md')
if (thirdParty === null) {
  problems.push('`THIRD-PARTY.md` 不存在 —— 第三方声明没地方放了。')
} else {
  for (const m of thirdParty.matchAll(/\]\(([^)\s]+)\)/g)) {
    const target = m[1]
    if (/^https?:\/\//.test(target) || target.startsWith('#')) continue
    const line = thirdParty.slice(0, m.index).split('\n').length
    problems.push(
      `\`THIRD-PARTY.md\` 第 ${line} 行有仓库相对链接 \`](${target})\` —— ` +
      `这个文件会被**原样搬进便携包当 \`THIRD-PARTY.txt\`**，那种链接在包里是死链。` +
      `改成开头那张"仓库里叫 / 包里叫"的对照表。`
    )
  }
}

// ── ③ 从 `package.ps1` 里抠出"随包搬哪些许可文件" ───────────────────────────
const pkg = read('tools/package.ps1')
const shipped = new Map() // To → From
if (pkg === null) {
  problems.push('`tools/package.ps1` 不存在 —— 打包那一步没了，这份检查也就无从比起。')
} else {
  const re = /From\s*=\s*\(Join-Path\s+\$root\s+'([^']+)'\s*\)\s*;?\s*To\s*=\s*'([^']+)'/g
  for (const m of pkg.matchAll(re)) {
    const [, from, to] = m
    if (shipped.has(to)) problems.push(`\`package.ps1\` 里 \`${to}\` 出现了两次 —— 后搬的那份会悄悄盖掉前一份。`)
    shipped.set(to, from)
  }
  // 扫不到就红：**不许"不报错地少查一件事"**（这份清单是硬要求，解析不到等于没查）
  if (shipped.size === 0) {
    problems.push(
      '在 `tools/package.ps1` 里**一条随包许可文件都没解析到** —— 要么那一段被删了/改了写法，' +
      '要么搬法变了。**本检查宁可变红，也不许"没查到就当没有"。**'
    )
  }
  for (const [to, from] of shipped) {
    if (!fs.existsSync(path.join(ROOT, from))) {
      problems.push(`\`package.ps1\` 要搬 \`${from}\`，但这个文件不存在（包里那份 \`${to}\` 会缺）。`)
    }
  }
}

// ── ④ 对照表第 3 列 vs `package.ps1` 的 To：**两个方向都要对得上** ──────────
if (thirdParty !== null && shipped.size > 0) {
  const documented = new Map() // 包里叫 → 行号
  let rows = 0
  thirdParty.split('\n').forEach((line, i) => {
    const m = line.match(/^\|\s*([^|]+?)\s*\|\s*([^|]+?)\s*\|\s*([^|]+?)\s*\|\s*$/)
    if (!m) return
    const [, a, , c] = m
    if (a === '提到的东西' || /^-+$/.test(a)) return
    rows++
    const name = c.match(/^`([^`]+\.txt)`$/)
    if (name) documented.set(name[1], i + 1)
  })
  if (rows === 0) {
    problems.push(
      '`THIRD-PARTY.md` 开头那张"提到的东西 / 在仓库里叫 / 在便携包里叫"**三列对照表解析不到** —— ' +
      '**本检查宁可变红，也不许"没查到就当没有"。**'
    )
  }
  for (const [to] of shipped) {
    if (!documented.has(to)) {
      problems.push(
        `包里会多出 \`${to}\`（\`package.ps1\` 搬了它），但 \`THIRD-PARTY.md\` 的对照表里**没写**它 —— ` +
        `拿了别人的东西却不声明，正是这一条要挡的事。`
      )
    }
  }
  for (const [name, line] of documented) {
    if (!shipped.has(name)) {
      problems.push(
        `\`THIRD-PARTY.md\` 第 ${line} 行说包里有一份 \`${name}\`，但 \`package.ps1\` **没搬**它 —— ` +
        `包里的说明指向一个不存在的文件。`
      )
    }
  }
  notes.push(`随包许可文件：${shipped.size} 份（清单与 \`package.ps1\` 一致）`)
}

// ── 实测结果 ────────────────────────────────────────────────────────────────
console.log(`许可与第三方声明检查（根：${path.relative(HERE, ROOT) || '.'}）：`)
if (license !== null) console.log('  · LICENSE        逐字等于 MIT 全文（只许换版权那一行），且纯 ASCII')
if (thirdParty !== null) console.log('  · THIRD-PARTY.md 无仓库相对链接；对照表与 package.ps1 双向对齐')
for (const n of notes) console.log(`  · ${n}`)

if (problems.length > 0) {
  console.log('')
  for (const p of problems) console.log(`  ✗ ${p}`)
  console.log('')
  console.log('怎么修：')
  console.log('  · `LICENSE` 被塞了东西：把多出来的那一段**搬**进 `THIRD-PARTY.md`（不是删掉），')
  console.log('    再把 `LICENSE` 恢复成本文件顶部那份全文（只留版权那一行不同）。')
  console.log('  · 包里那份跟清单对不上：`tools/package.ps1` 第 ④ 步与 `THIRD-PARTY.md`')
  console.log('    开头那张对照表，**同一件事的两处写法，改一处就要改另一处**。')
  console.log('')
  console.log(`✗ 许可检查：${problems.length} 处不合格`)
  process.exit(1)
}
console.log('')
console.log('✓ 许可检查：全部合格（LICENSE 是纯 MIT 全文；随包许可文件与清单双向一致）')
