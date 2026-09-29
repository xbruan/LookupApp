#!/usr/bin/env node
/**
 * ★ 出路按钮的**授权边界**回归测试（安全审查 F1，P1 的那条）—— B 级：不启动程序、不联网、秒级。
 *
 *   node tools/check-entry-auth.mjs
 *
 * ── 它在守什么 ────────────────────────────────────────────────────────────────
 *
 * 词条正文住在 `sandbox="allow-scripts"` 的**跨源 iframe** 里，而**词典自带的脚本和桥接脚本
 * 跑在同一个 JS 环境**里 —— 中间没有任何可信边界。所以只要"翻译"这个动作的按钮长在正文里，
 * 词典脚本就能自己发一条：
 *
 *     parent.postMessage({ source:'lookup-entry', type:'chip',
 *                          action:'translate', word:'它挑的文本' }, '*')
 *
 * 让宿主把**它指定的文本**发给收费的翻译接口（用户看到的是一次正常翻译，出去的字和账单
 * 却是词典决定的；换个词还能绕开按文本缓存）。这么做的**决定性证据**是：把 `event.source`
 * 设成**真正的正文框窗口**，只加来源校验**依然挡不住** —— 因为发送方本来就是那个窗口。
 * 本脚本第 ① 条就是照着这个场景写的。
 *
 * 修法不是"再校验一次消息"，而是**把授权还给可信父页面**：按钮由宿主页自己画
 * （`main.ts` 的 `renderEntryChips`），处理器里**不再有** `type === 'chip'` 这一支。
 *
 * ── 它怎么测 ──────────────────────────────────────────────────────────────────
 *
 * 从**真实的** `web/src/floating/main.ts` 里切出消息处理器与 `renderEntryChips` 的源码，
 * 用 esbuild 去掉类型语法，在 vm 里拿桩跑 —— **不是**照抄一份逻辑来测（抄的那份永远不会红，
 * 只会证明"抄对了"）。切不出来（函数被改名 / 搬走）就**非零退出**，不会静默通过。
 *
 * 五条检查项：
 *   ① 正文框窗口自己伪造的 `chip:translate` **到不了**动作（翻转前这里必红）；
 *   ② 别的窗口发来的任何消息一律丢弃（来源校验确实在起作用）；
 *   ③ 正文框发来的合法 `lookup` 照旧送达（别把整条通道一起堵死 —— 反向对照）；
 *   ④ 出路按钮由**宿主页**创建，且只有它的 click 会调到 `onEntryChip`；
 *   ⑤ `renderEntryChips` 不再往正文框 postMessage；
 *   ⑥ 那一排摆着 / 收起的**高度变化**要同步重排面板（正文文档量不到它，见 `ENTRY_CHIPS_H`）。
 */

import fs from 'node:fs'
import path from 'node:path'
import vm from 'node:vm'
import { createRequire } from 'node:module'
import { fileURLToPath } from 'node:url'

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
/*
 * 被测前端源码。默认就是本仓库这一份。
 *
 * `DSH_ENTRY_AUTH_TARGET` 是给**反向对照**用的：把 ①③ 两条拿到**修之前**的那一份
 * （例如 `查词软件/0.2.0/web/src/floating/main.ts`）上跑一遍，必须看到 ① 变红 ——
 * 一条"拿旧代码跑也全绿"的检查等于什么都没守。
 */
const MAIN_TS = process.env.DSH_ENTRY_AUTH_TARGET
  ? path.resolve(process.env.DSH_ENTRY_AUTH_TARGET)
  : path.join(ROOT, 'web', 'src', 'floating', 'main.ts')

/** esbuild 住在 web/ 下（本仓库 node_modules 只有它一个包），所以从那边解析。 */
function loadEsbuild() {
  const requireFromWeb = createRequire(path.join(ROOT, 'web', 'package.json'))
  try {
    return requireFromWeb('esbuild')
  } catch (err) {
    throw new Error(
      `读不到 esbuild（${err.message}）—— 先在 web/ 下装依赖：cd web && npm ci`
    )
  }
}

/**
 * 切出 `window.addEventListener('message', …)` **整条语句**。
 * 用"下一处 `\n  })\n}`"当终点（处理器是 `setupReaderEvents` 里最后一个语句），
 * 比数大括号稳 —— 处理器里有模板串、中文注释，手写括号匹配会被绕进去。
 */
function extractListener(source) {
  const marker = "window.addEventListener('message', (event) => {"
  const start = source.indexOf(marker)
  if (start < 0) {
    throw new Error(
      '找不到正文消息处理器 —— 它被改名或搬走了。这条检查必须跟着改，别删掉它（删掉就等于没人守 F1 了）'
    )
  }
  const tail = '\n  })\n}'
  const end = source.indexOf(tail, start)
  if (end < 0) throw new Error('找不到消息处理器的结尾（`\\n  })\\n}`）—— 提取失败')
  return source.slice(start, end + '\n  })'.length)
}

/** 切出 `function renderEntryChips(…) { … }`（顶格 `}` 收尾）。 */
function extractFunction(source, name) {
  const start = source.indexOf(`function ${name}(`)
  if (start < 0) {
    throw new Error(`找不到 ${name}() —— 它被改名或搬走了。这条检查必须跟着改，别静默跳过`)
  }
  const end = source.indexOf('\n}\n', start)
  if (end < 0) throw new Error(`找不到 ${name}() 的结尾 —— 提取失败`)
  return source.slice(start, end + '\n}'.length)
}

/* ── 一个够用的假元素（只实现 renderEntryChips 用得上的那几个方法）───────────── */
function makeElement(tag) {
  return {
    tagName: String(tag).toUpperCase(),
    className: '',
    textContent: '',
    title: '',
    type: '',
    hidden: true,
    dataset: {},
    children: [],
    listeners: {},
    appendChild(child) {
      this.children.push(child)
      return child
    },
    replaceChildren(...nodes) {
      this.children = nodes
    },
    addEventListener(type, fn) {
      ;(this.listeners[type] ||= []).push(fn)
    },
    /** 模拟一次真实点击：把挂上的 click 监听都跑一遍 */
    click() {
      for (const fn of this.listeners.click || []) fn({ type: 'click' })
    }
  }
}

const failures = []
let checks = 0
function check(ok, label, detail) {
  checks++
  if (ok) {
    console.log(`  ✓ ${label}`)
  } else {
    console.log(`  ✗ ${label}${detail ? ` —— ${detail}` : ''}`)
    failures.push(label)
  }
}

function main() {
  const esbuild = loadEsbuild()
  const source = fs.readFileSync(MAIN_TS, 'utf8')

  console.log('出路按钮的授权边界（安全审查 F1 回归）—— 不启动程序：')

  /* ── 第一节：消息处理器 ─────────────────────────────────────────────────── */
  const listenerCode = esbuild.transformSync(extractListener(source), { loader: 'ts' }).code

  const calls = {
    onEntryChip: [],
    followEntryLink: [],
    pushLayout: [],
    readerToast: [],
    deadClickText: [],
    playEntrySound: [],
    handleEntrySelection: []
  }
  const entryWindow = { __role: 'entryFrame.contentWindow' }
  const foreignWindow = { __role: 'foreignWindow' }
  const sandbox = {
    console,
    Math,
    Number,
    dom: {
      entryFrame: { contentWindow: entryWindow },
      entryTop: { disabled: true }
    },
    state: { entryScrollY: 0, contentHeight: 0 },
    dismissedEntryText: 'x',
    onEntryChip: (...a) => calls.onEntryChip.push(a),
    followEntryLink: (...a) => calls.followEntryLink.push(a),
    pushLayout: (...a) => calls.pushLayout.push(a),
    readerToast: (...a) => calls.readerToast.push(a),
    deadClickText: (reason) => `dead:${reason}`,
    playEntrySound: (...a) => calls.playEntrySound.push(a),
    handleEntrySelection: (...a) => calls.handleEntrySelection.push(a)
  }
  let handler = null
  sandbox.window = { addEventListener: (type, fn) => { if (type === 'message') handler = fn } }
  vm.createContext(sandbox)
  vm.runInContext(listenerCode, sandbox)
  if (typeof handler !== 'function') throw new Error('消息处理器没挂上 —— 提取到的东西不对')

  const deliver = (from, data) => handler({ source: from, origin: 'null', isTrusted: false, data })
  const reset = () => { for (const key of Object.keys(calls)) calls[key].length = 0 }

  /*
   * ① 决定性的一条：`event.source` 就是**真正的正文框窗口**（词典脚本所在的那个环境），
   *    声称自己是 chip:translate。来源校验对此无能为力 —— 挡住它的只能是"处理器里根本没有
   *    能接住它的分支"。
   */
  reset()
  deliver(entryWindow, {
    source: 'lookup-entry',
    type: 'chip',
    action: 'translate',
    word: 'attacker chosen text'
  })
  check(
    calls.onEntryChip.length === 0,
    '正文框自己伪造的 chip:translate 到不了任何动作',
    `onEntryChip 被调了 ${calls.onEntryChip.length} 次：${JSON.stringify(calls.onEntryChip)}`
  )

  // 顺带把另一个动作也钉住：`recheck` 同样是"正文换得出动作"的那一类
  reset()
  deliver(entryWindow, { source: 'lookup-entry', type: 'chip', action: 'recheck', word: 'apple', scrollY: 9 })
  check(
    calls.onEntryChip.length === 0,
    '正文框自己伪造的 chip:recheck 同样到不了动作',
    `onEntryChip 被调了 ${calls.onEntryChip.length} 次`
  )

  /* ② 来源校验：别的窗口发来的东西一律丢，连低权限的也不行 */
  reset()
  sandbox.state.contentHeight = 0
  deliver(foreignWindow, { source: 'lookup-entry', type: 'height', value: 500 })
  deliver(foreignWindow, { source: 'lookup-entry', type: 'lookup', word: 'apple' })
  deliver(foreignWindow, { source: 'lookup-entry', type: 'dead-click', reason: 'no-href' })
  check(
    calls.pushLayout.length === 0 && calls.followEntryLink.length === 0 &&
      calls.readerToast.length === 0 && sandbox.state.contentHeight === 0,
    '别的窗口发来的消息一律丢弃（来源校验在起作用）',
    `pushLayout=${calls.pushLayout.length} followEntryLink=${calls.followEntryLink.length}`
  )

  /* ③ 反向对照：正文框发来的**合法**消息必须照旧送达 —— 别把整条通道一起堵死 */
  reset()
  deliver(entryWindow, { source: 'lookup-entry', type: 'lookup', word: 'apple', scrollY: 7 })
  const linkOk = calls.followEntryLink.length === 1 && calls.followEntryLink[0][0].word === 'apple'
  check(linkOk, '正文框发来的合法 entry:// lookup 照旧送达', JSON.stringify(calls.followEntryLink))

  reset()
  deliver(entryWindow, { source: 'lookup-entry', type: 'dead-click', reason: 'no-anchor' })
  check(
    calls.readerToast.length === 1 && calls.readerToast[0][0] === 'dead:no-anchor',
    '正文框发来的低权限事件（dead-click）照旧送达',
    JSON.stringify(calls.readerToast)
  )

  reset()
  sandbox.state.entryScrollY = 0
  deliver(entryWindow, { source: 'lookup-entry', type: 'selection', text: 'x', scrollY: 42 })
  check(
    sandbox.state.entryScrollY === 42,
    '正文报上来的滚动位置被记下（压返回栈要用）',
    `entryScrollY=${sandbox.state.entryScrollY}`
  )

  /* ── 第二节：按钮归宿主页所有 ───────────────────────────────────────────── */
  const chipsCode = esbuild.transformSync(extractFunction(source, 'renderEntryChips'), { loader: 'ts' }).code

  const posted = []
  const chipsDom = {
    entryChips: makeElement('div'),
    reader: { dataset: {} },
    entryFrame: {
      contentWindow: {
        postMessage: (...a) => posted.push(a)
      }
    }
  }
  const chipCalls = []
  const layoutCalls = []
  const chipState = { mode: 'content', chipsVisible: false }
  const chipSandbox = {
    console,
    document: { createElement: (tag) => makeElement(tag) },
    dom: chipsDom,
    state: chipState,
    pushLayout: () => layoutCalls.push(1),
    onEntryChip: (...a) => chipCalls.push(a)
  }
  vm.createContext(chipSandbox)
  vm.runInContext(chipsCode, chipSandbox)

  const items = [
    { action: 'recheck', label: '再问一遍', word: 'apple', hint: 'h1' },
    { action: 'translate', label: '翻译「apple」', word: 'apple', hint: 'h2' }
  ]
  chipSandbox.renderEntryChips(items)

  const rendered = chipsDom.entryChips.children
  check(
    rendered.length === 2 && rendered.every((b) => b.className === 'entry-chip'),
    '出路按钮由宿主页创建（2 枚 .entry-chip）',
    `拿到 ${rendered.length} 枚：${rendered.map((b) => b.className).join(',')}`
  )
  check(
    chipsDom.entryChips.hidden === false && chipsDom.reader.dataset.chips === 'recheck,translate',
    '界面上留了可判决据（#entryChips 可见 + #reader[data-chips]）',
    `hidden=${chipsDom.entryChips.hidden} data-chips=${chipsDom.reader.dataset.chips}`
  )

  /* 只有**宿主页上这一枚按钮的真实点击**才该换出动作 */
  reset()
  chipCalls.length = 0
  const translateBtn = rendered[1]
  translateBtn.click()
  check(
    chipCalls.length === 1 && chipCalls[0][0] === 'translate' && chipCalls[0][1] === 'apple',
    '点上「翻译」按钮 → onEntryChip("translate", "apple")',
    JSON.stringify(chipCalls)
  )

  check(
    posted.length === 0,
    'renderEntryChips 不再往正文框 postMessage（那条路已经拆了）',
    `postMessage 被调了 ${posted.length} 次`
  )

  /* 算不出出路时整排要收回去（别把上一轮的按钮留在界面上） */
  chipCalls.length = 0
  layoutCalls.length = 0
  chipSandbox.renderEntryChips([])
  check(
    chipsDom.entryChips.children.length === 0 &&
      chipsDom.entryChips.hidden === true &&
      chipsDom.reader.dataset.chips === '',
    '算不出出路时整排收起、检查标准一并清空',
    `children=${chipsDom.entryChips.children.length} hidden=${chipsDom.entryChips.hidden}`
  )
  /*
   * 摆着 / 不摆**变了**就必须重算一次面板高度：那一排的高度住在宿主页上，
   * 正文文档量不到它（见 `ENTRY_CHIPS_H`）—— 不重算，正文底部会被 `overflow: hidden` 裁掉。
   */
  check(
    chipState.chipsVisible === false && layoutCalls.length === 1,
    '按钮收起时同步刷新面板高度（chipsVisible=false 且重排一次）',
    `chipsVisible=${chipState.chipsVisible} pushLayout=${layoutCalls.length}`
  )

  /* 反向对照：摆出按钮时也要重排一次；而"没变化"时不许白跑布局 */
  layoutCalls.length = 0
  chipSandbox.renderEntryChips(items)
  const grewLayout = layoutCalls.length
  chipSandbox.renderEntryChips(items)
  check(
    chipState.chipsVisible === true && grewLayout === 1 && layoutCalls.length === grewLayout,
    '按钮摆出时重排一次；状态没变时不重复重排',
    `第一次=${grewLayout} 第二次后累计=${layoutCalls.length}`
  )

  console.log()
  if (failures.length > 0) {
    console.log(`✗ 出路按钮的授权边界：${checks - failures.length}/${checks} 项通过，${failures.length} 项不通过`)
    process.exitCode = 1
    return
  }
  console.log(`✓ 出路按钮的授权边界：${checks} 项全部通过`)
}

try {
  main()
} catch (err) {
  console.error(`✗ ${err.message}`)
  process.exitCode = 1
}
