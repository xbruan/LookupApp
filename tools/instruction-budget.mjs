#!/usr/bin/env node
/*
 * 指令预算检查 —— **防止自动加载的 AGENTS.md 悄悄顶穿工作区指令预算**。
 *
 * ── 为什么要有一个机械检查（不是洁癖，是踩过的坑）─────────────────────────
 * DSH 每个会话会把根 `AGENTS.md` 与**当前会话涉及的那些目录的 `AGENTS.md`** 一并注入
 * 工作区指令，总量有上限（本仓库按 **65536 B** 算）。一旦超了，**靠后的内容会被不报错地省掉**
 * —— 不是报错、不是警告，就是**那一段等于没写**。实测到过两次：
 *   · 有一份版本级 `AGENTS.md` 被截过，最后一条坑整条消失；
 *   · 更坏的一次是几个 `AGENTS.md` 一起被加载时，**连根 `AGENTS.md` 都被省掉了**
 *     （提示只有干巴巴一句 `omitted AGENTS.md`）—— 而它是最该被读到的那一份。
 *
 * 人眼估不出来（UTF-8 汉字 3 字节，一屏"看着不多"的说明文字就是好几 KB），
 * 所以这条约定必须能**当场量**：跑一次，超了就非零退出，并指出该往哪儿搬。
 *
 * ── 检查标准 ─────────────────────────────────────────────────────────────────
 * 按**最坏情况**判：把仓库里所有会被自动加载的 `AGENTS.md` **全部加起来** ≤ 预算。
 * 本仓库是单版本仓库，正常只有根那一份 —— 但如果以后在子目录里再放 `AGENTS.md`，
 * 这条检查会当场把总量算给你看。
 *
 * 用法：
 *   node tools/instruction-budget.mjs                 # 体检（超预算 → 退出码 1）
 *   node tools/instruction-budget.mjs --budget 65536   # 换预算
 *   node tools/instruction-budget.mjs --quiet          # 只输出最后一行结论
 *   node tools/instruction-budget.mjs --top 5          # 每个文件里最肥的几个二级节（找该搬哪儿）
 *
 * 本检查只管"量"。**"该往哪儿搬"的约定在根 `AGENTS.md` §〇**：
 * 硬规则与一句话结论留在 `AGENTS.md`，实测结果 / 经过 / 论证搬进
 * `docs/design/` 或当轮的实现说明（见根 `AGENTS.md` §〇 第 6 条）。
 */

import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const args = process.argv.slice(2)
const has = (name) => args.includes('--' + name)
const arg = (name, fallback = null) => {
  const i = args.indexOf('--' + name)
  return i >= 0 && args[i + 1] && !args[i + 1].startsWith('--') ? args[i + 1] : fallback
}

const budget = Number(arg('budget', '65536'))
const topN = Number(arg('top', '0'))
const quiet = has('quiet')

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const SKIP = /(\\|\/)(node_modules|logs|bin|obj|dist|vendor|reference|build[^\\/]*)(\\|\/)/

/** 收集所有会被自动加载的 AGENTS.md（根 + 子目录，最多往下两层） */
function collect() {
  const found = []
  const walk = (dir, depth) => {
    if (depth > 2) return
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
      const full = path.join(dir, entry.name)
      if (entry.isFile() && entry.name === 'AGENTS.md') {
        found.push(full)
      } else if (entry.isDirectory() && !SKIP.test(full + path.sep) && !entry.name.startsWith('.')) {
        walk(full, depth + 1)
      }
    }
  }
  walk(root, 0)
  return found.sort((a, b) => {
    const ra = path.relative(root, a)
    // 根排最前，其余按路径
    return ra.split(path.sep).length - path.relative(root, b).split(path.sep).length || ra.localeCompare(path.relative(root, b))
  })
}

/** 一个文件里最肥的几个二级节（`## ` 切分） */
function topSections(file, n) {
  const lines = fs.readFileSync(file, 'utf8').split(/\r?\n/)
  const heads = []
  for (let i = 0; i < lines.length; i++) if (lines[i].startsWith('## ')) heads.push(i)
  const out = []
  for (let k = 0; k < heads.length; k++) {
    const end = k + 1 < heads.length ? heads[k + 1] - 1 : lines.length - 1
    const text = lines.slice(heads[k], end + 1).join('\n')
    out.push({ title: lines[heads[k]].replace(/^## /, ''), bytes: Buffer.byteLength(text, 'utf8') })
  }
  return out.sort((a, b) => b.bytes - a.bytes).slice(0, n)
}

const files = collect()
let total = 0
const rows = []
for (const f of files) {
  const bytes = fs.statSync(f).size
  const lines = fs.readFileSync(f, 'utf8').split(/\r?\n/).length
  total += bytes
  rows.push({ rel: path.relative(root, f), bytes, lines })
}

const over = total - budget
const pct = ((total / budget) * 100).toFixed(1)

if (!quiet) {
  console.log('── 会被自动加载的 AGENTS.md（最坏情况：全部一起加载）──')
  const width = Math.max(...rows.map((r) => r.rel.length), 4)
  for (const r of rows) {
    console.log('  ' + r.rel.padEnd(width) + '  ' + String(r.bytes).padStart(7) + ' B  ' + String(r.lines).padStart(5) + ' 行')
  }
  console.log('  ' + '合计'.padEnd(width) + '  ' + String(total).padStart(7) + ' B  = 预算的 ' + pct + '%')
  console.log('  预算 ' + budget + ' B → ' + (over > 0 ? '超出 ' + over + ' B' : '余量 ' + -over + ' B'))

  if (topN > 0) {
    for (const f of files) {
      console.log('')
      console.log('── ' + path.relative(root, f) + ' 里最肥的 ' + topN + ' 个二级节 ──')
      for (const s of topSections(f, topN)) {
        console.log('  ' + String(s.bytes).padStart(7) + ' B  ' + s.title)
      }
    }
  }
}

if (over > 0) {
  console.log('')
  console.log('✗ 超出工作区指令预算 ' + over + ' B —— **靠后的内容会被不报错地省掉**（可能整份文件被 skipped）。')
  console.log('  约定（根 AGENTS.md §〇）：硬规则与一句话结论留 AGENTS.md，实测结果 / 经过 / 论证搬进 docs/design/。')
  console.log('  找该搬哪儿：node tools/instruction-budget.mjs --top 5')
  process.exit(1)
}

console.log('✓ 指令预算：' + total + ' / ' + budget + ' B（余量 ' + -over + ' B）')
process.exit(0)
