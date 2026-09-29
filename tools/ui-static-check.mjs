/*
 * 0.2.0 的 **A 级「界面静态检查」**：不启动程序、不跑业务逻辑，只读源码就能判的那些决定类事实。
 *   node tools/ui-static-check.mjs            # 检查（红则非零退出）
 *   node tools/ui-static-check.mjs --list     # 只列出检查项名字，不读文件（秒回）
 * 为什么要有它：`gen-bindings.mjs --check` 只管接口定义与生成物对不对得上，而「页签顺序 / 控件归属
 * 哪一页 / 删掉的东西有没有长回来」这些不启动程序就能判，以前只能靠 C 级诊断脚本在真程序里看一遍。
 * 每条钉的都是那个决定类事实本身而不是替代指标；任何一条不成立都非零退出。
 */

import { readFileSync, readdirSync, statSync } from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const failures = []
const oks = []
const notes = []

function read(rel) {
  return readFileSync(path.join(root, rel), 'utf8')
}

/*
 * 把**注释**整段换成空格（保留换行）后的代码 —— 行号仍然对得上。
 * ⚠️ 必须这么做：仓库的硬约定是「删掉功能就留下说明」，注释里到处写着 `hotkey`、`RegisterHotKey`、
 *    `<footer …>` 这些字面量，按裸文本搜会把**说明**当成**复活**（第一版就是这么误报的）。
 */
function stripComments(text, kind) {
  const blank = (m) => m.replace(/[^\n]/g, ' ')
  let out = text
  if (kind === 'html' || kind === 'xml') return out.replace(/<!--[\s\S]*?-->/g, blank)
  out = out.replace(/\/\*[\s\S]*?\*\//g, blank)
  // 行注释：避开 `https://` 这种（前一个字符是冒号的不算注释）
  out = out.replace(/(^|[^:])\/\/[^\n]*/g, (m, lead) => lead + ' '.repeat(m.length - lead.length))
  return out
}

function kindOf(rel) {
  if (rel.endsWith('.html')) return 'html'
  if (rel.endsWith('.csproj')) return 'xml'
  return 'code'
}

/* 读「去掉注释的代码」（检查一律用它） */
function readCode(rel) {
  return stripComments(read(rel), kindOf(rel))
}

/* 第几行（1 起）—— 报错要能直接跳到那一行去 */
function lineOf(text, index) {
  return text.slice(0, index).split('\n').length
}

function ok(what) {
  oks.push(what)
}

function fail(what, where) {
  failures.push(where ? what + ' —— ' + where : what)
}

/* 递归列出某几个后缀的文件（跳过 dist / bin / obj / node_modules / logs） */
function walk(rel, exts, out = []) {
  const abs = path.join(root, rel)
  let entries
  try {
    entries = readdirSync(abs)
  } catch {
    return out
  }
  for (const name of entries) {
    if (name === 'dist' || name === 'bin' || name === 'obj' || name === 'node_modules' || name === 'logs') continue
    const childRel = rel + '/' + name
    const st = statSync(path.join(root, childRel))
    if (st.isDirectory()) walk(childRel, exts, out)
    else if (exts.some((e) => name.endsWith(e))) out.push(childRel)
  }
  return out
}

const CHECKS = [
  '页签与页面一一对应、顺序固定',
  '页签名字在三处一致（HTML / OptionsTab / TAB_TITLES）',
  '页面 JS 取的 DOM 节点都必须存在',
  '删掉的东西不许回来：页脚',
  '删掉的东西不许回来：全局热键',
  '页面调的桥方法名必须都在壳的路由表里',
  '常规页里必须有那几个控件（归属）',
  '同一个 id 不许在两个页签里各有一份',
]

if (process.argv.includes('--list')) {
  for (const name of CHECKS) console.log('  · ' + name)
  process.exit(0)
}

/* ── ① 页签与页面一一对应、顺序固定 ── */

/* ⚠️ 这两行与 `web/manager.html` 的 `<nav>`、`web/shared/types.ts` 的 `OptionsTab`、
 * `web/src/manager/main.ts` 的 `TAB_TITLES` **四处必须一致**，改名或调序要一起改。 */
const EXPECTED_TABS = ['dicts', 'speech', 'translate', 'general']
const EXPECTED_LABELS = ['词库', '语音', '翻译', '常规']

const managerHtml = readCode('web/manager.html')
const tabBlock = managerHtml.match(/<nav class="tabs"[\s\S]*?<\/nav>/)
if (!tabBlock) {
  fail('manager.html 里找不到 <nav class="tabs">（页签那一排）')
} else {
  const tabs = [...tabBlock[0].matchAll(/id="tab(\w+)"[^>]*data-tab="(\w+)"[^>]*>([^<]*)</g)].map((m) => ({
    id: 'tab' + m[1],
    name: m[2],
    label: m[3].trim(),
    at: tabBlock.index + m.index,
  }))
  const names = tabs.map((t) => t.name)
  if (names.join(',') !== EXPECTED_TABS.join(',')) {
    fail(
      '页签（顺序或集合）不对：期望 ' + EXPECTED_TABS.join(' / ') + '，实际 ' + (names.join(' / ') || '(没读到)'),
      'web/manager.html:' + lineOf(managerHtml, tabBlock.index)
    )
  } else {
    ok('页签顺序 ' + names.join(' / '))
  }
  const labels = tabs.map((t) => t.label)
  if (labels.join(',') !== EXPECTED_LABELS.join(',')) {
    fail('页签文字不对：期望 ' + EXPECTED_LABELS.join(' / ') + '，实际 ' + (labels.join(' / ') || '(没读到)'))
  } else {
    ok('页签文字 ' + labels.join(' / '))
  }
  for (const tab of tabs) {
    const paneId = tab.id.replace(/^tab/, 'pane')
    const paneAt = managerHtml.indexOf('id="' + paneId + '"')
    if (paneAt < 0) {
      fail('页签「' + tab.label + '」没有对应的 #' + paneId + '（点下去会是白板）', 'web/manager.html:' + lineOf(managerHtml, tab.at))
      continue
    }
    const paneName = (managerHtml.slice(paneAt, paneAt + 200).match(/data-pane="(\w+)"/) || [])[1]
    if (paneName !== tab.name) {
      fail('#' + paneId + ' 的 data-pane 是「' + paneName + '」，与页签的「' + tab.name + '」对不上')
    }
  }
  if (failures.length === 0) ok('每个页签都有同名页面（' + tabs.length + ' 个）')
}

/* ── ② 页签名字与 types.ts 的 OptionsTab、main.ts 的 TAB_TITLES 一致 ── */

const typesTs = readCode('web/shared/types.ts')
const optionsTabAt = typesTs.indexOf('export type OptionsTab')
if (optionsTabAt < 0) {
  fail('web/shared/types.ts 里找不到 OptionsTab（页签的取值表）')
} else {
  const union = (typesTs.slice(optionsTabAt, optionsTabAt + 400).match(/export type OptionsTab =([^\n]*)/) || [])[1] || ''
  const names = [...union.matchAll(/'(\w+)'/g)].map((m) => m[1])
  if (names.join(',') !== EXPECTED_TABS.join(',')) {
    fail('OptionsTab 的取值与页签对不上：' + (names.join(' / ') || '(没读到)'), 'web/shared/types.ts:' + lineOf(typesTs, optionsTabAt))
  } else {
    ok('OptionsTab = ' + names.join(' / '))
  }
}

const managerTs = readCode('web/src/manager/main.ts')
const titlesAt = managerTs.indexOf('const TAB_TITLES')
if (titlesAt < 0) {
  fail('web/src/manager/main.ts 里找不到 TAB_TITLES（页签中文名的唯一出处）')
} else {
  const block = managerTs.slice(titlesAt, managerTs.indexOf('}', titlesAt))
  const keys = [...block.matchAll(/(\w+):\s*'/g)].map((m) => m[1])
  if (keys.slice().sort().join(',') !== EXPECTED_TABS.slice().sort().join(',')) {
    fail('TAB_TITLES 的键与页签对不上：' + (keys.join(' / ') || '(没读到)'), 'web/src/manager/main.ts:' + lineOf(managerTs, titlesAt))
  } else {
    ok('TAB_TITLES 覆盖全部 ' + keys.length + ' 个页签')
  }
}

/* ── ③ 页面 JS 取的 DOM 节点都必须存在 ───────────────────────────────────── */

{
  const wanted = []
  for (const m of managerTs.matchAll(/el<[^>]*>\('([\w-]+)'\)/g)) wanted.push({ id: m[1], at: m.index })
  for (const m of managerTs.matchAll(/getElementById\('([\w-]+)'\)/g)) wanted.push({ id: m[1], at: m.index })
  const missing = wanted.filter((w) => !managerHtml.includes('id="' + w.id + '"'))
  if (missing.length > 0) {
    for (const w of missing) {
      fail('main.ts 要的 #' + w.id + ' 在 manager.html 里不存在（那一页一加载就抛）', 'web/src/manager/main.ts:' + lineOf(managerTs, w.at))
    }
  } else {
    ok('main.ts 取的都是 manager.html 里真实存在的节点（' + wanted.length + ' 个）')
  }
}

/* ── ④ 页脚不许回来 ─────────────────────────────────────────────────────── */

{
  const footerAt = managerHtml.indexOf('<footer')
  const css = readCode('web/styles/manager.css')
  const cssAt = css.search(/^\.footer[\s.{:]/m)
  if (footerAt >= 0) {
    fail('manager.html 里又出现了 <footer>（2026-09 已整块删掉：那是一块从来没被填过的死版面）', 'web/manager.html:' + lineOf(managerHtml, footerAt))
  }
  if (cssAt >= 0) {
    fail('manager.css 里又出现了 .footer 规则（页脚删了，样式也该删）', 'web/styles/manager.css:' + lineOf(css, cssAt))
  }
  if (footerAt < 0 && cssAt < 0) ok('页脚（元素 + 样式）都不在')
}

/* ── ⑤ 全局热键不许回来 ─────────────────────────────────────────────────── */

{
  const webFiles = ['web/bridge.js', ...walk('web/src', ['.ts']), ...walk('web', ['.html', '.css'])]
  const shellFiles = walk('shell', ['.cs', '.csproj'])
  const nativeFiles = walk('native/src', ['.c', '.h'])
  const hits = []
  for (const rel of [...webFiles, ...shellFiles]) {
    const text = readCode(rel)
    for (const m of text.matchAll(/hotkey|Hotkey|RegisterHotKey|全局热键/g)) {
      const lineStart = text.lastIndexOf('\n', m.index) + 1
      const line = text.slice(lineStart, text.indexOf('\n', m.index))
      hits.push(rel + ':' + lineOf(text, m.index) + '  ' + line.trim().slice(0, 90))
    }
  }
  for (const rel of nativeFiles) {
    const text = readCode(rel)
    for (const m of text.matchAll(/dsh_hotkey|DSH_HOTKEY/g)) {
      hits.push(rel + ':' + lineOf(text, m.index))
    }
  }
  if (hits.length > 0) {
    fail('全局热键（2026-09 按需求整条删掉）又有 ' + hits.length + ' 处真代码/声明：\n      ' + hits.join('\n      '))
  } else {
    ok('热键在页面 / 壳 / 内核里都没有真代码（只剩注释里的说明）')
  }
}

/* ── ⑥ 页面调的桥方法名必须都在壳的路由表里 ─────────────────────────────── */

{
  const bridgeJs = readCode('web/bridge.js')
  const shellBridge = readCode('shell/Lookup.Host/ShellBridge.cs')
  const wanted = new Map()
  for (const m of bridgeJs.matchAll(/\b(?:invoke|send)\('([a-zA-Z0-9:_-]+)'/g)) {
    if (!wanted.has(m[1])) wanted.set(m[1], lineOf(bridgeJs, m.index))
  }
  const cases = new Set([...shellBridge.matchAll(/case "([a-zA-Z0-9:_-]+)"/g)].map((m) => m[1]))
  const missing = [...wanted.entries()].filter(([name]) => !cases.has(name))
  if (missing.length > 0) {
    for (const [name, line] of missing) {
      fail('页面会调「' + name + '」，但壳的路由表里没有这个名字（调下去就是「未知的接口」）', 'web/bridge.js:' + line + ' → shell/Lookup.Host/ShellBridge.cs')
    }
  } else {
    ok('页面调的 ' + wanted.size + ' 个桥方法名壳全都认（ShellBridge 共 ' + cases.size + ' 个 case）')
  }
  const unused = [...cases].filter((name) => !wanted.has(name)).sort()
  if (unused.length > 0) notes.push('壳里另有 ' + unused.length + ' 个 case 页面不调（诊断协议 / C 级诊断脚本用）：' + unused.join(' / '))
}

/* ── ⑦ 常规页里必须有那几个控件（归属） ───────────────────────── */

{
  const paneAt = managerHtml.indexOf('id="paneGeneral"')
  if (paneAt < 0) {
    fail('manager.html 里找不到 #paneGeneral（常规页）')
  } else {
    const end = managerHtml.indexOf('</section>', paneAt)
    const pane = managerHtml.slice(paneAt, end < 0 ? undefined : end)
    /*
     * ⚠️ `clearHistory`（「清空查词历史」）2026-09 从**托盘菜单**搬进这一页（用户："这种设置应该
     *    放深一点的地方"），所以它从此必须住在这里 —— 搬回去 / 搬丢了这个检查都会红。
     */
    const mustHave = ['closeBehavior', 'showOnStartup', 'loginAtStartup', 'openConfigDir', 'clearHistory']
    const missing = mustHave.filter((id) => !pane.includes('id="' + id + '"'))
    if (missing.length > 0) {
      /*
       * ⚠️ 报错里**只给能照着做的东西**。这里原来还挂着一串「见 §98 / §99 / §106」的开发记录节号，
       *    而那些记录不随本仓库发布 —— 清扫时把节号删了，却把「见  / / 」这个空壳留下了，
       *    于是报错长成一句残句。既然指向的东西不在仓库里，就**整句删掉**，别留一个指不到任何地方的"见"。
       */
      fail('常规页里缺少：' + missing.join(' / ') + '（这五个控件必须住在 #paneGeneral 里）',
           'web/manager.html:' + lineOf(managerHtml, paneAt))
    } else {
      ok('常规页里有：' + mustHave.join(' / '))
    }
  }
}

/* ── ⑧ 同一个 id 不许在两个页签里各有一份 ───────────────────────────────── */

{
  const panes = [...managerHtml.matchAll(/<section class="pane"[^>]*id="(pane\w+)"[^>]*>/g)]
  const seen = new Map()
  const dup = []
  for (let i = 0; i < panes.length; i++) {
    const start = panes[i].index
    const end = i + 1 < panes.length ? panes[i + 1].index : managerHtml.length
    const body = managerHtml.slice(start, end)
    for (const m of body.matchAll(/id="([\w-]+)"/g)) {
      const id = m[1]
      if (seen.has(id)) dup.push(id + '（' + seen.get(id) + ' 与 ' + panes[i][1] + '）')
      else seen.set(id, panes[i][1])
    }
  }
  if (dup.length > 0) fail('同一个 id 出现在两个页签里：' + dup.join('、'))
  else ok('页签之间没有重复的 id（共 ' + panes.length + ' 页、' + seen.size + ' 个 id）')
}

/* ── 结果 ───────────────────────────────────────────────────────────────── */

console.log('界面静态检查（不启动程序）：')
for (const line of oks) console.log('  ✓ ' + line)
for (const line of notes) console.log('  · ' + line)
if (failures.length > 0) {
  console.log('')
  for (const line of failures) console.log('  ✗ ' + line)
  console.log('')
  console.log('界面静态检查不通过：' + failures.length + ' 项（' + oks.length + ' 项通过）')
  process.exit(1)
}
console.log('')
console.log('✓ 界面静态检查：' + oks.length + ' 项全部通过')

