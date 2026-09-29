#!/usr/bin/env node
/*
 * 注释瘦身的**机械证据**：证明一次改动"只动了注释"，代码一个字没变。
 *
 *   node tools/comment-diff.mjs <改之前> <改之后>
 *   node tools/comment-diff.mjs a.c b.c --syntax c        # 少数情况要手动指定
 *
 * 做法：按扩展名认出这种语言的注释写法 → 把注释换成空格 → 两道检查都要过：
 *   ① **折空白后逐字符比**：代码分词序列一字未变。
 *   ② **保留换行逐行比**（`␊`）：非空代码行不许挪位、不许被并进注释行。
 * 两道都要过才退出 0；红了按**第一处不同**打印上下文，直接定位。
 *
 * ⚠️ 为什么值得单独有它：注释瘦身看起来"没有风险"，但它碰的是**真源码** ——
 *    少一个分号、把 `else` 挪进注释里、顺手"整理"一下字符串，编译期未必立刻报错。
 *    有了这两道比对，"只动注释"就从"我检查过了"变成**可逐字符复核的结论**。
 *    （第 ② 道不是多余的：用编辑器替换整行注释时很容易把**行尾换行一起吃掉**，
 *    下一行代码被并到注释行上 —— 折空白的那道检查对此**完全无感**，真发生过。）
 *
 * 扫描方式：从左到右逐字符走一趟，**字符串与正则字面量优先于注释**（引号里的 `//`
 *   与 `/*` 是内容，不是注释）。⚠️ 老版本先找块注释再看行注释，于是 C# 文档注释里的
 *   `web/**` 会被当成块注释开头、把整份文件剩下的部分吞掉，两侧吞法不同就报假红。
 *
 * 已知的粗糙处（**故意**不追求精确）：正则字面量只在"表达式位置"才认（前一个非空白字符
 *   是 `( , = : [ ! & | ? { } ; + - * % < > ~ ^`）；HTML 不认 `<script>` 里的 `//`。
 *   因为两边用的是同一把尺子，这类误判不影响"代码有没有变"的结论；但若改动本身动了
 *   字符串内容，本工具看不出来（那是人工 review 的事）。
 *
 * 退出码：两道都相同 0 / 任一不同 1 / 用法错 2。
 */
import { readFileSync } from 'node:fs'
import path from 'node:path'

/* ── 各种语言的词法优先级 ───────────────────────────────────────────────────
 * 顺序即优先级：字符串 / 正则在前，注释在后。
 * ⚠️ `#` 只给 shell / PowerShell / Python 用：C 里 `#` 是预处理指令，不能当注释。
 * ⚠️ Python 的三引号排在最前，否则 `"""` 会被当成一个 `"` 开头的空字符串。
 */
const S = (q) => ({ kind: 'str', open: q, close: q })
const B = (o, c) => ({ kind: 'block', open: o, close: c })
const L = (p) => ({ kind: 'line', open: p })
const R = () => ({ kind: 'regex', open: '/', close: '/' })

const CLIKE = [S('"'), S("'"), B('/*', '*/'), L('//')]
const SYNTAX = {
  c: { toks: CLIKE },
  h: { toks: CLIKE },
  cs: { toks: CLIKE },
  ts: { toks: [S('"'), S("'"), S('`'), B('/*', '*/'), L('//'), R()] },
  js: { toks: [S('"'), S("'"), S('`'), B('/*', '*/'), L('//'), R()] },
  mjs: { toks: [S('"'), S("'"), S('`'), B('/*', '*/'), L('//'), R()] },
  css: { toks: [S('"'), S("'"), B('/*', '*/')] },
  html: { toks: [B('<!--', '-->')] },
  ps1: { toks: [S('"'), S("'"), B('<#', '#>'), L('#')] },
  sh: { toks: [S('"'), S("'"), L('#')] },
  py: { toks: [B('"""', '"""'), B("'''", "'''"), S('"'), S("'"), L('#')] },
}

const argv = process.argv.slice(2)
const files = argv.filter((a) => !a.startsWith('--'))
if (files.length !== 2) {
  console.log('用法: node tools/comment-diff.mjs <改之前> <改之后> [--syntax c|cs|ts|js|css|html|ps1|sh|py]')
  console.log('认得的扩展名：' + Object.keys(SYNTAX).join(' / '))
  process.exit(2)
}
const at = argv.indexOf('--syntax')
const ext = (at >= 0 && argv[at + 1] ? argv[at + 1] : path.extname(files[0]).slice(1)).toLowerCase()
const syn = SYNTAX[ext]
if (!syn) {
  console.log(`不认得的扩展名 .${ext}（用 --syntax 指定；认得的：${Object.keys(SYNTAX).join(' / ')}）`)
  process.exit(2)
}

const REGEX_OK_BEFORE = '(,=:[!&|?{};+-*%<>~^'
const isRegexStart = (text, i) =>
  text[i] === '/' &&
  !text.startsWith('/*', i) &&
  !text.startsWith('//', i) &&
  (() => {
    let j = i - 1
    while (j >= 0 && /\s/.test(text[j])) j--
    return j < 0 || REGEX_OK_BEFORE.includes(text[j])
  })()

/** 把注释换成空格；字符串 / 正则 / 其余代码原样保留（换行与缩进不动） */
function scan(file) {
  const text = readFileSync(path.resolve(file), 'utf8')
  const out = []
  let i = 0
  while (i < text.length) {
    const quoted = syn.toks.find((t) => t.kind === 'str' && text.startsWith(t.open, i))
    if (quoted) {
      let j = i + quoted.open.length
      out.push(quoted.open)
      while (j < text.length) {
        if (text[j] === '\\') {
          out.push(text.slice(j, j + 2))
          j += 2
          continue
        }
        if (text.startsWith(quoted.close, j)) {
          out.push(quoted.close)
          j += quoted.close.length
          break
        }
        out.push(text[j])
        j++
      }
      i = j
      continue
    }
    if (syn.toks.some((t) => t.kind === 'block' && text.startsWith(t.open, i))) {
      const t = syn.toks.find((x) => x.kind === 'block' && text.startsWith(x.open, i))
      const end = text.indexOf(t.close, i + t.open.length)
      out.push(' ')
      i = end < 0 ? text.length : end + t.close.length
      continue
    }
    if (syn.toks.some((t) => t.kind === 'line' && text.startsWith(t.open, i))) {
      const end = text.indexOf('\n', i)
      out.push(' ')
      i = end < 0 ? text.length : end
      continue
    }
    if (syn.toks.some((t) => t.kind === 'regex') && isRegexStart(text, i)) {
      let j = i + 1
      let inClass = false
      while (j < text.length) {
        const c = text[j]
        if (c === '\\') {
          j += 2
          continue
        }
        if (c === '[') inClass = true
        else if (c === ']') inClass = false
        else if (c === '/' && !inClass) break
        else if (c === '\n') break
        j++
      }
      out.push(text.slice(i, j + 1))
      i = j + 1
      continue
    }
    out.push(text[i])
    i++
  }
  return out.join('')
}

const fold = (t) => t.replace(/\s+/g, ' ').trim()
const codeLines = (t) =>
  t.split('\n').map((l) => l.replace(/[\r \t]+$/, '')).filter((l) => l.trim() !== '')

/** 按第一处不同打印上下文 */
function report(title, a, b) {
  let i = 0
  while (i < a.length && i < b.length && a[i] === b[i]) i++
  const from = Math.max(0, i - 70)
  console.log(`✗ ${title} —— 第一处不同在第 ${i} 个字符：`)
  console.log('  改之前 …' + a.slice(from, i + 140))
  console.log('  改之后 …' + b.slice(from, i + 140))
  console.log(`  （长度：改之前 ${a.length} / 改之后 ${b.length}）`)
}

const left = scan(files[0])
const right = scan(files[1])
const name = `${path.basename(files[0])} → ${path.basename(files[1])}`

const foldedOk = fold(left) === fold(right)
const ll = codeLines(left).join('\n')
const rl = codeLines(right).join('\n')
const linesOk = ll === rl

if (foldedOk && linesOk) {
  console.log(
    `✓ 只动了注释：${name} 的**代码逐字符相同**（折空白 ${fold(left).length} 字符、` +
      `${codeLines(left).length} 行代码，语法按 .${ext} 认）`
  )
  process.exit(0)
}
if (!foldedOk) report('代码本身变了（不只是注释/空白）', fold(left), fold(right))
if (!linesOk) report('代码行被挪位 / 被并进注释行（换行或缩进被改）', ll, rl)
process.exit(1)
