#!/usr/bin/env node
/*
 * 注释体检：**量出"注释占多少、哪几块最肥"**，供"要不要瘦身、瘦哪几块"的讨论用。
 *
 *   node tools/comment-audit.mjs                    # 汇总表（按注释行数排序）
 *   node tools/comment-audit.mjs --blocks           # 再列出所有"超长注释块"（默认 ≥ 14 行）
 *   node tools/comment-audit.mjs --blocks --min 20  # 只列 ≥ 20 行的
 *   node tools/comment-audit.mjs --refs             # 只统计"带 §NN 编号引用"的注释（公开仓库里最该收拾的一类）
 *
 * ## 为什么要它
 *
 * 2026-09 用户准备把 0.2.0 发到 GitHub，提出"源码里注释好像有点冗余"。**"好像"不能当依据**：
 * 得先知道哪几份文件密、密在哪儿、以及密的是哪一类内容 ——
 * 这个仓库的注释里混着三种东西：① 为什么这么做（该留）② 曾经怎么做 / 第几轮改的（该进开发记录）
 * ③ `§105.4` 这种只有配着几千行日志才看得懂的编号（公开仓库里等于乱码）。
 * `--refs` 就是专门数第 ③ 类的。
 *
 * ⚠️ 它**只统计、不下结论**，也不改任何文件：判断"这一块该不该留"要人来拍。
 */
import { readdirSync, readFileSync, statSync } from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
const root = path.resolve(here, '..')

const argv = process.argv.slice(2)
const flag = (n) => argv.includes(n)
const num = (n, d) => {
  const at = argv.indexOf(n)
  return at >= 0 && argv[at + 1] ? Number(argv[at + 1]) : d
}

const EXTS = ['.c', '.h', '.cs', '.ts', '.js', '.mjs', '.css', '.html', '.ps1', '.sh', '.py', '.json']
const MIN_BLOCK = num('--min', 14)
/*
 * 排除的目录（按**路径段**比，不写正则）：产物、依赖、第三方、冻结测试数据都不该进体检 ——
 * 它们不是"我们写的注释"，混进来只会把比例冲淡。
 * ⚠️ 段名前缀也要排（`build-asan` 那种变体），否则会像公开仓库那次一样漏掉一大块产物。
 */
const SKIP_SEGMENTS = ['node_modules', 'dist', 'bin', 'obj', 'logs', '.toolchain', 'testdata', 'vendor']
const SKIP_PREFIXES = ['build', '.toolchain']
const shouldSkip = (p) =>
  p
    .replace(/\\/g, '/')
    .split('/')
    .some((seg) => SKIP_SEGMENTS.includes(seg) || SKIP_PREFIXES.some((pre) => seg.startsWith(pre)))

/**
 * 一行属于哪一类：注释 / 空行 / 代码。只做轻量判断，够用即可（不做真正的词法分析）。
 *
 * ⚠️ 块注释有两种收尾符：C / JS / CSS 是「斜杠星号」，HTML 是「小于叹号减减」——
 *    第一版只认前者，于是 .html 文件里**第一个 `<!--` 之后的所有行都被算成注释**
 *    （`manager.html` 因此报出"97% 注释"的假数字）。两种都要认，否则那一列数字只能骗人。
 * ⚠️ 写这段注释时还踩了一次：**注释里直接写出 C 的收尾符，会把块注释当场截断**
 *    （后半段变成代码 → 语法错误报在莫名其妙的位置）。要么绕开不写，要么中间加个空格。
 */
function classify(line, blockEnd) {
  const t = line.trim()
  if (blockEnd) return { kind: 'comment', blockEnd: t.includes(blockEnd) ? null : blockEnd }
  if (t === '') return { kind: 'blank', blockEnd: null }
  if (t.startsWith('/*')) return { kind: 'comment', blockEnd: t.includes('*/') ? null : '*/' }
  if (t.startsWith('<!--')) return { kind: 'comment', blockEnd: t.includes('-->') ? null : '-->' }
  if (t.startsWith('//') || t.startsWith('*') || t.startsWith('#')) return { kind: 'comment', blockEnd: null }
  return { kind: 'code', blockEnd: null }
}

const isRefLine = (line) => /§\s?\d|附录\s?[A-Z]|第\s?\d+\s?轮/.test(line)

const files = []
const walk = (dir) => {
  for (const entry of readdirSync(dir)) {
    const full = path.join(dir, entry)
    if (shouldSkip(path.relative(root, full))) continue
    const st = statSync(full)
    if (st.isDirectory()) walk(full)
    else if (EXTS.includes(path.extname(entry))) files.push(full)
  }
}
walk(root)

const rows = []
const blocks = []
for (const file of files) {
  const lines = readFileSync(file, 'utf8').split(/\r?\n/)
  let blockEnd = null
  let comment = 0
  let blank = 0
  let refs = 0
  let blockStart = -1
  let refsInBlock = 0
  const own = []
  lines.forEach((line, i) => {
    const c = classify(line, blockEnd)
    blockEnd = c.blockEnd
    if (c.kind === 'comment') {
      comment++
      if (isRefLine(line)) refs++
      if (blockStart < 0) {
        blockStart = i
        refsInBlock = 0
      }
      if (isRefLine(line)) refsInBlock++
    } else if (blockStart >= 0) {
      own.push({ start: blockStart, size: i - blockStart, refs: refsInBlock, head: lines[blockStart].trim() })
      blockStart = -1
    }
    if (c.kind === 'blank') blank++
  })
  if (blockStart >= 0) {
    own.push({ start: blockStart, size: lines.length - blockStart, refs: refsInBlock, head: lines[blockStart].trim() })
  }
  for (const b of own) {
    if (b.size >= MIN_BLOCK) blocks.push({ file: path.relative(root, file), ...b })
  }
  const total = lines.length
  rows.push({ file: path.relative(root, file), total, comment, blank, code: total - comment - blank, refs })
}

const pct = (n, d) => (d === 0 ? '  0%' : `${Math.round((n / d) * 100)}%`.padStart(4))

if (flag('--refs')) {
  const withRefs = rows.filter((r) => r.refs > 0).sort((a, b) => b.refs - a.refs)
  console.log(`带 §NN / 第 N 轮 / 附录 X 这类编号引用的注释行（共 ${withRefs.reduce((s, r) => s + r.refs, 0)} 行）：`)
  for (const r of withRefs) console.log(`  ${String(r.refs).padStart(4)} 行  ${r.file}`)
  process.exit(0)
}

console.log(`注释体检：${rows.length} 个文件（跳过 node_modules / dist / build / logs / .toolchain / testdata / vendor）\n`)
console.log('  注释占比   注释   代码   总行   文件')
for (const r of rows.slice().sort((a, b) => b.comment - a.comment)) {
  console.log(`  ${pct(r.comment, r.total)}  ${String(r.comment).padStart(6)} ${String(r.code).padStart(6)} ${String(r.total).padStart(6)}   ${r.file}`)
}
const sum = rows.reduce(
  (a, r) => ({ c: a.c + r.comment, code: a.code + r.code, t: a.t + r.total, refs: a.refs + r.refs }),
  { c: 0, code: 0, t: 0, refs: 0 }
)
console.log(`\n  合计：注释 ${sum.c} 行（${Math.round((sum.c / sum.t) * 100)}%）/ 代码 ${sum.code} 行 / 共 ${sum.t} 行`)
console.log(`  其中带 §NN / 第 N 轮 / 附录 这类编号引用的注释：**${sum.refs} 行**（公开仓库里最该收拾的一类）`)

if (flag('--blocks')) {
  console.log(`\n超长注释块（≥ ${MIN_BLOCK} 行，共 ${blocks.length} 块，按块大小排序）：`)
  for (const b of blocks.sort((a, b2) => b2.size - a.size)) {
    console.log(`  ${String(b.size).padStart(3)} 行  ${b.file}:${b.start + 1}${b.refs ? `（含 ${b.refs} 行编号引用）` : ''}`)
    console.log(`          ${b.head.slice(0, 96)}`)
  }
}
