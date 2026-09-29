/**
 * 0.2.0 的 C 级诊断脚本：接进真实程序的渲染进程，把现场读出来（只读、**不下判断**）。
 * 壳带 `--debug-port <n>` 启动时 WebView2 才开 DevTools 端口，本脚本才连得上；
 * 它**不负责启动程序**（「程序起来了」有它自己的门）。用法见各子命令那几段。
 * 为什么需要：渲染层的问题读代码看不出来，而 D 级 gate 贵（几十秒、还得先重编壳）——
 * 单点观察能回答的别去跑 gate。词条那一页与宿主**同一渲染进程**（关了站点隔离），
 * 所以 `--entry` 是在同一个页面里的另一个执行上下文里求值，不是连另一个目标。
 * 纪律：检查标准留在 A/B/D 三级 gate 里；常用观察固化成子命令，不许每次现拼 `--eval`。
 */
import { readFileSync, writeFileSync } from 'node:fs'
import path from 'node:path'

function arg(name, fallback = null) {
  const at = process.argv.indexOf('--' + name)
  return at >= 0 && process.argv[at + 1] && !process.argv[at + 1].startsWith('--')
    ? process.argv[at + 1]
    : fallback
}
const has = (name) => process.argv.includes('--' + name)

const port = Number(arg('port', '9333'))
const match = arg('match', 'floating.html')
const evalFile = arg('eval-file', null)
const expression = evalFile ? readFileSync(path.resolve(evalFile), 'utf8') : arg('eval', null)

/* ── CDP 那一小层 ───────────────────────────────────────────────────────── */

async function listTargets() {
  const response = await fetch(`http://127.0.0.1:${port}/json`)
  return response.json()
}

class Cdp {
  constructor(url) {
    this.socket = new WebSocket(url)
    this.nextId = 1
    this.pending = new Map()
  }
  async open() {
    await new Promise((resolve, reject) => {
      this.socket.addEventListener('open', resolve, { once: true })
      this.socket.addEventListener('error', reject, { once: true })
    })
    this.socket.addEventListener('message', (event) => {
      const message = JSON.parse(event.data)
      if (message.id && this.pending.has(message.id)) {
        const entry = this.pending.get(message.id)
        this.pending.delete(message.id)
        if (message.error) entry.reject(new Error(message.error.message))
        else entry.resolve(message.result)
      }
    })
  }
  send(method, params = {}) {
    const id = this.nextId++
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject })
      this.socket.send(JSON.stringify({ id, method, params }))
    })
  }
  async evaluate(expression, contextId) {
    const result = await this.send('Runtime.evaluate', {
      expression,
      returnByValue: true,
      awaitPromise: true,
      ...(contextId ? { contextId } : {}),
    })
    if (result.exceptionDetails) {
      throw new Error(result.exceptionDetails.exception?.description || '求值异常')
    }
    return result.result.value
  }
  close() {
    this.socket.close()
  }
}

/* ── 连上去 ─────────────────────────────────────────────────────────────── */

let targets
try {
  targets = await listTargets()
} catch (err) {
  console.log(`连不上 DevTools（127.0.0.1:${port}）：${err.message}`)
  console.log('提示：壳要带 --debug-port <端口> 启动（见本文件顶上那段）。')
  process.exit(1)
}

const pages = targets.filter((t) => t.type === 'page' || t.type === 'iframe')
const page = pages.find((t) => t.url.includes(match)) || pages[0]
if (!page) {
  console.log('没有找到页面目标。DevTools 列出来的是：')
  for (const t of targets) console.log(`- [${t.type}] ${t.url}`)
  process.exit(1)
}

const cdp = new Cdp(page.webSocketDebuggerUrl)
await cdp.open()

/**
 * 页面里所有的执行上下文（词条 iframe 在同一个进程里，靠这个分辨）。
 *
 * ⚠️ **监听必须在 `Runtime.enable` 之前挂上**：那几个事件紧接着 enable 的应答推来，
 * 等 `await cdp.send('Runtime.enable')` 回来再挂就晚了 —— `--entry` / `--selection`
 * 会一直报「没有找到词条页的执行上下文」，而现场看起来像是正文框里没装东西。
 */
const contexts = []
cdp.socket.addEventListener('message', (event) => {
  const message = JSON.parse(event.data)
  if (message.method === 'Runtime.executionContextCreated') {
    contexts.push(message.params.context)
  }
})

await cdp.send('Runtime.enable')
await cdp.send('Log.enable')
await cdp.send('Page.enable')

// 上下文事件是异步推来的，给它一小会儿
await new Promise((r) => setTimeout(r, 300))

/**
 * 词条那一页的执行上下文。
 *
 * ⚠️ 取**最后一个**（最新建的那个），不是第一个：每换一次词条，那个 iframe 都会
 * **新建一个执行上下文**，而老的（已经没人用的那份文档）还留在列表里；拿第一个去
 * 求值 = 在**上一份文档**上动手 —— `--entry` 读到上一次的词条，选区落在旧文档里，
 * 浮层冒一下又被「空选区」收掉。
 */
function entryContext() {
  const hits = contexts.filter((c) => c.origin && c.origin.includes('dictres.invalid'))
  return hits.length > 0 ? hits[hits.length - 1] : undefined
}

/**
 * 词条那一页的**执行上下文**，而且**不依赖 `executionContextCreated` 的推送**。
 *
 * 为什么要有这一条：`Runtime.enable` 只推**连接建立之后**新建的上下文，而很常见的
 * 现场是「先起程序、再跑诊断脚本」—— 那时正文早就装好了，上下文表里根本没有它，
 * `--entry` / `--seltrace` 会一直报「没有找到词条页的执行上下文」。
 * 办法是**自己造一个**：`Page.getFrameTree` 找 `dictres.invalid` 的 frameId，再用
 * `Page.createIsolatedWorld` 建一个隔离世界去求值。⚠️ 隔离世界有自己的 `window`，
 * 但 `document` 与**选区是同一份文档状态** —— 在那里选中一段文字，主世界看得见。
 */
async function entryContextId() {
  const existing = entryContext()
  if (existing) return existing.id
  try {
    await cdp.send('Page.enable')
    const tree = await cdp.send('Page.getFrameTree')
    const stack = [tree.frameTree]
    while (stack.length) {
      const node = stack.shift()
      const url = (node && node.frame && node.frame.url) || ''
      if (url.includes('dictres.invalid')) {
        const world = await cdp.send('Page.createIsolatedWorld', {
          frameId: node.frame.id,
          worldName: 'dsh-probe',
        })
        return world.executionContextId
      }
      for (const child of (node && node.childFrames) || []) stack.push(child)
    }
  } catch (err) {
    return undefined
  }
  return undefined
}

const consoleSeconds = Number(arg('console', '0'))
if (consoleSeconds > 0) {
  console.log(`=== 盯控制台 ${consoleSeconds} 秒 ===`)
  cdp.socket.addEventListener('message', (event) => {
    const message = JSON.parse(event.data)
    if (message.method === 'Log.entryAdded') {
      console.log(`[${message.params.entry.level}] ${message.params.entry.source}: ${message.params.entry.text}`)
    }
    if (message.method === 'Runtime.consoleAPICalled') {
      const text = (message.params.args || []).map((a) => a.value ?? a.description ?? '').join(' ')
      console.log(`[console.${message.params.type}] ${text}`)
    }
    if (message.method === 'Runtime.exceptionThrown') {
      console.log('[exception] ' + (message.params.exceptionDetails.exception?.description || ''))
    }
  })
  await new Promise((r) => setTimeout(r, consoleSeconds * 1000))
}

/**
 * 把**合成鼠标**挪到某个元素中心（或 `none` = 挪到左上角）。
 *
 * ⚠️ 必须**在同一个连接里**先挪再读：CDP 的合成鼠标状态**不跨连接**保存 —— 分两次调用
 * 读 `:hover` 的 `getComputedStyle`，两次都回静止态的值。所以要看 `:hover`，只能
 * `--hover <选择器>` 配 `--eval <表达式>` 这样**一趟里做完**。
 */
async function hoverTo(selector) {
  let box = { x: 1, y: 1 }
  if (selector && selector !== 'none') {
    box = await cdp.evaluate(`(() => {
      const el = document.querySelector(${JSON.stringify(selector)});
      if (!el) return null;
      const r = el.getBoundingClientRect();
      return { x: r.left + r.width / 2, y: r.top + r.height / 2 };
    })()`)
    if (!box) throw new Error('找不到要悬浮的元素：' + selector)
  }
  await cdp.send('Input.dispatchMouseEvent', { type: 'mouseMoved', x: box.x, y: box.y })
  await new Promise((r) => setTimeout(r, 350)) // 等 :hover 的过渡走完
  return box
}

if (expression) {
  const hover = arg('hover', null)
  if (hover) await hoverTo(hover)
  console.log(await cdp.evaluate(expression))
}

/* ── 固化下来的观察（每次要问同样的问题时用它，别现拼 --eval）────────────── */

/*
 * `--pill-buttons`：胶囊上那两个按钮的现场。
 *
 * 为什么固化它：这两件事只活在渲染进程里，D 级 gate 一条都不碰胶囊上的按钮 ——
 * 「顺序对不对、图标是不是那两条线、线宽生效了没有」只有在这里才读得到。
 *
 * 读的都是**被考察的对象本身**，不是替代指标：
 *   ① `.pill-actions` 子元素的 id 顺序 —— 清空在前、历史在后；
 *   ② `#btnClear` 里 svg 的**结构**：子路径的标签与 `d`、path 上的 `stroke-width` 属性；
 *   ③ **生效的**线宽（`getComputedStyle(path).strokeWidth`）应当是 1.4px —— 它才说明
 *      path 上的 presentation attribute 压过了从 `.icon` 继承的 1.7（历史那条该是 1.7px）；
 *   ④ 悬停胶囊（按钮这时才浮出来）之后，两个按钮各自的 `elementFromPoint` 命中是它自己，
 *      而且**清空的中心在历史的左边** —— 顺序由屏幕坐标说，不由 DOM 顺序说。
 */
if (has('pill-buttons')) {
  const read = `(() => {
    const actions = document.getElementById('actions');
    const clear = document.getElementById('btnClear');
    const history = document.getElementById('btnHistory');
    const svgOf = (el) => (el && el.querySelector('svg')) || null;
    const pathsOf = (el) => Array.from((svgOf(el) || { querySelectorAll: () => [] }).querySelectorAll('path'));
    return {
      order: Array.from(actions.children).map((c) => c.id),
      clearDisabled: !!clear.disabled,
      clearTitle: clear.title,
      historyTitle: history.title,
      clearTags: Array.from((svgOf(clear) || { querySelectorAll: () => [] }).querySelectorAll('*')).map((e) => e.tagName.toLowerCase()),
      clearPaths: pathsOf(clear).map((p) => ({
        d: p.getAttribute('d'),
        attrStrokeWidth: p.getAttribute('stroke-width'),
        computedStrokeWidth: getComputedStyle(p).strokeWidth,
      })),
      historyPaths: pathsOf(history).map((p) => ({
        d: p.getAttribute('d'),
        attrStrokeWidth: p.getAttribute('stroke-width'),
        computedStrokeWidth: getComputedStyle(p).strokeWidth,
      })),
      iconBox: (() => { const b = (svgOf(clear) || {}).getBoundingClientRect ? svgOf(clear).getBoundingClientRect() : null;
        return b ? [Math.round(b.width), Math.round(b.height)] : null; })(),
    };
  })()`
  console.log('── 静止态（没悬停：两个按钮这时还是透明的）──')
  console.log(JSON.stringify(await cdp.evaluate(read), null, 2))

  await hoverTo('.pill')
  const hit = `(() => {
    const center = (el) => {
      const b = el.getBoundingClientRect();
      return [Math.round(b.left + b.width / 2), Math.round(b.top + b.height / 2)];
    };
    const at = (p) => {
      const el = document.elementFromPoint(p[0], p[1]);
      if (!el) return '(null)';
      const btn = el.closest('button');
      return btn ? (btn.id || btn.title || 'button') : (el.id || el.tagName.toLowerCase());
    };
    const clear = document.getElementById('btnClear');
    const history = document.getElementById('btnHistory');
    const c = center(clear);
    const h = center(history);
    return {
      clearCenter: c,
      historyCenter: h,
      clearIsLeftOfHistory: c[0] < h[0],
      hitAtClearCenter: at(c),
      hitAtHistoryCenter: at(h),
      clearPointerEvents: getComputedStyle(clear).pointerEvents,
      clearDisabled: !!clear.disabled,
    };
  })()`
  console.log('── 悬停胶囊之后（按钮浮出来，这里才谈得上"命中"）──')
  console.log(JSON.stringify(await cdp.evaluate(hit), null, 2))
}

/**
 * `--panel`：查词那一屏的全部界面标记。
 *
 * 界面标记表（DOM 上那些 `data-*`）是这一版的「可判决据」：**跨源 iframe 里读不到的东西
 * 都留在宿主页的 DOM 上**。这一条只读现场，不下判断。
 *
 * ⚠️ 2026-09 修：这一节以前读的全是**参考实现（0.1.3）**的元素 —— `#q` / `#dictTitle` /
 * `#reason` / `#btnSpeak` / `#chips` 以及 `#reader` 上一堆本版根本没有的 `data-*`。那些
 * 取不到值的地方会当场抛 `TypeError`（`null.value`），于是整条 `--panel` **跑不起来** ——
 * 而"一个跑不起来的子命令比没有它更坏"（见本文件 `--settings` 那段）。现在全部改成读本版
 * 真实存在的节点，并且**取不到就如实写 null**，不再抛。
 *
 * `chips*` 两格读的是**宿主页**上那排出路按钮（`#entryChips`）。它 2026-09 从词条正文里
 * 搬到了宿主页 —— 那是授权边界的要求（安全审查 F1），不是排版调整：
 * 按钮长在跨源 iframe 里，词典自带的脚本就能伪造一条 `chip:translate` 让宿主外发文本。
 * 回归测试见 `tools/check-entry-auth.mjs`。
 */
if (has('panel')) {
  const dump = `(() => {
    const pick = (id) => document.getElementById(id);
    const txt = (id) => { const n = pick(id); return n ? n.textContent : null };
    const reader = pick('reader');
    const ds = reader ? reader.dataset : {};
    const chipsBox = pick('entryChips');
    const chips = chipsBox ? Array.from(chipsBox.querySelectorAll('button')) : [];
    const frame = pick('entryFrame');
    const toast = pick('readerToast');
    const speak = pick('entrySpeak');
    return {
      booted: document.body.dataset.ready || '(没跑)',
      bootError: document.body.dataset.bootError || '',
      query: pick('input') ? pick('input').value : null,
      // 标题栏：词条名那一格是**隐藏的检查标准**（看不见，但 textContent 有值）
      entryWord: txt('entryWord'),
      dictTitle: txt('entryDict'),
      // #reader 上那几张表才是这一版真正的可判决据
      via: ds.via || '',
      unconfirmed: ds.unconfirmed || '',
      chips: ds.chips || '',
      relayedFragment: ds.relayedFragment || '',
      relayedScrollY: ds.relayedScrollY || '',
      readerHidden: reader ? !!reader.hidden : null,
      readerSrc: frame ? (frame.getAttribute('src') || '') : null,
      // 出路按钮已搬到宿主页（授权边界），所以这一排宿主自己就数得清
      chipButtons: chips.map((b) => b.dataset.chipAction + ':' + b.textContent),
      chipsHidden: chipsBox ? !!chipsBox.hidden : null,
      reason: txt('readerVia'),
      toastShow: toast ? (toast.dataset.show || '') : '',
      toastTone: toast ? (toast.dataset.tone || '') : '',
      toastActions: toast ? (toast.dataset.actions || '') : '',
      toastText: toast ? toast.textContent : '',
      // 词条自带音频：键名与地址挂在 iframe 的 dataset 上（宿主读不到 iframe 内部）
      soundKey: frame ? (frame.dataset.soundKey || '') : '',
      soundSrc: frame ? (frame.dataset.soundSrc || '') : '',
      speakDisabled: speak ? !!speak.disabled : null,
      speakTitle: speak ? speak.title : null,
      speakSrc: speak ? (speak.dataset.src || '') : '',
      speakMime: speak ? (speak.dataset.mime || '') : '',
      pillFocused: pick('pill') ? pick('pill').dataset.focused : null,
    };
  })()`
  console.log(JSON.stringify(await cdp.evaluate(dump), null, 2))
}

/**
 * `--speak`：**点一下「朗读」，把这一路的现场摆出来**（发音那条链的 C 级观察）。
 *
 * 它观察的是这一页在链上做了什么，不是音频好不好听：
 *   · 按钮**能不能点**、`title` 上那句人话是什么（都来自内核的 `speech.plan`）；
 *   · 点完之后 `speech.speak` 给了哪一层、什么地址、多少字节；
 *   · 那条地址**在页面这一侧取得回来吗**（`fetch` 一次看状态与头 4 个字节 ——
 *     `RIFF` = 真的是一段 WAV，不是一句错误正文）；
 *   · `data-played` 有没有置上（那是 `<audio>` 的 `playing` 事件写的，只有「真的开始播了」
 *     才会有 —— 与「我们调了 play()」是两件事）。
 *
 * ⚠️ 它**不下「能不能听见」的结论**：跑测试的机器常常没有声卡 / 锁着屏，能听见只有人耳能判；
 * 机器能判的是「字节到齐了、媒体元素开始播了」。
 */
if (has('speak')) {
  const state = `(() => {
    const b = document.getElementById('btnSpeak');
    return {
      view: document.body.dataset.view || '(没设)',
      query: document.getElementById('q').value,
      keyText: document.getElementById('reader').dataset.keyText || '',
      dictId: document.getElementById('reader').dataset.dictId || '',
      speakSource: document.getElementById('panel').dataset.speakSource || '',
      speakWord: document.getElementById('panel').dataset.speakWord || '',
      speakEnabled: document.getElementById('panel').dataset.speakEnabled || '',
      state: b.dataset.state || '(没记)',
      disabled: !!b.disabled,
      title: b.title,
    };
  })()`
  console.log('── 点之前 ──')
  console.log(JSON.stringify(await cdp.evaluate(state), null, 2))

  if (has('speak-click')) {
    const clicked = await cdp.evaluate(`(() => {
      const b = document.getElementById('btnSpeak');
      if (!b) return '(页面上没有这个按钮)';
      if (b.disabled) return '(按钮是灰的：内核说这会儿念不了 —— title 里有原因)';
      b.click();
      return 'clicked';
    })()`)
    console.log('点下去：' + clicked)
    /* 等它落定：state 离开 loading 就算完（最多 10 秒 —— 合成一句话要几百毫秒） */
    let waited = 0
    for (; waited < 10000; waited += 200) {
      const s = await cdp.evaluate(`document.getElementById('btnSpeak').dataset.state || ''`)
      if (s !== 'loading') break
      await new Promise((r) => setTimeout(r, 200))
    }
    await new Promise((r) => setTimeout(r, 400))
    const after = `(async () => {
      const b = document.getElementById('btnSpeak');
      const url = b.dataset.url || '';
      const out = {
        waitedMs: ${waited},
        state: b.dataset.state || '(没记)',
        played: b.dataset.played || '',
        url: url,
        mime: b.dataset.mime || '',
        bytes: b.dataset.bytes || '',
        chunks: b.dataset.chunks || '',
        voice: b.dataset.voice || '',
        cached: b.dataset.cached || '',
        /* ⚠️ 这一格是**面板上那条反馈**（查词那条链写的），不是朗读的 —— 朗读失败了
         *    会往同一个地方写（showReason），所以两条链共用这一个位置。
         *    ⚠️ 这段文字住在一个模板串里：注释里**不许**出现反引号。 */
        reasonOnPanel: document.getElementById('reason').textContent,
      };
      if (url) {
        /* 那条地址**在页面这一侧**取得回来吗（跨源 + CORS + 路由都对才行）*/
        try {
          const res = await fetch(url);
          const buf = new Uint8Array(await res.arrayBuffer());
          out.fetch = {
            status: res.status,
            type: res.headers.get('content-type'),
            len: buf.length,
            head: String.fromCharCode(buf[0], buf[1], buf[2], buf[3]),
          };
        } catch (err) {
          out.fetch = { error: String(err) };
        }
      }
      return out;
    })()`
    console.log('── 点之后 ──')
    console.log(JSON.stringify(await cdp.evaluate(after), null, 2))
    console.log('↑ `fetch.head` = RIFF = 那段音频真的到页面了；')
    console.log('  `played` = 1 才是媒体元素**真的开始播了**（`state` 只说明我们调过 play()）。')
    console.log('  ⚠️ 走**词典**那条路时 `bytes`/`chunks`/`mime` 是空的 —— 那段录音的字节由')
    console.log('     `/__sound__/` 那条路由从 .mdd 里取，宿主这会儿不读它（只有 `fetch` 那几格是事实）；')
    console.log('     走**系统合成**那条路才有 `bytes`/`chunks`，两者都该与 `fetch.len` 对上。')
  } else {
    console.log('（只看现场，没动手；要顺手点一下加 `--speak-click`）')
  }
}

/**
 * `--entry`：**词条那一页自己**的现场（在同一进程的另一个执行上下文里求值）。
 *
 * 为什么需要：正文住在跨源 iframe 里，宿主页读不到它的 DOM —— 而「桥接脚本跑没跑、
 * 文档到齐没有、里面有几条 🔊」恰恰只有问它自己才知道。
 */
if (has('entry')) {
  const ctx = { id: await entryContextId() }
  if (!ctx) {
    console.log('没有找到词条页的执行上下文（正文框里可能还没装东西）')
    console.log('现在的上下文：')
    for (const c of contexts) console.log(`- [${c.id}] ${c.origin || '(无 origin)'} ${c.name || ''}`)
  } else {
    const dump = `(() => ({
      href: location.href,
      readyState: document.readyState,
      bridge: document.documentElement.dataset.lookupBridge || '(没跑)',
      /*
       * 这个容器**还在**（正文文档的固定形状，内核按参考实现生成），但 2026-09 起它
       * **永远是空的**：出路按钮改由宿主页自己渲染（授权边界，见 --panel 那段与
       * tools/check-entry-auth.mjs），宿主不再往正文送 chips，所以里面不会有按钮。
       * 想看出路按钮**去宿主页**看 --panel 的 chipButtons。
       */
      chipsContainerEmpty: (() => {
        const box = document.getElementById('lookupChips');
        return box ? box.children.length === 0 : null;
      })(),
      scripts: document.scripts.length,
      sounds: Array.from(document.querySelectorAll('a[href^="sound://"],a[href^="snd://"]'))
        .map((a) => a.getAttribute('href')),
      links: Array.from(document.querySelectorAll('a[href^="entry://"]'))
        .map((a) => a.getAttribute('href')),
      height: document.documentElement.scrollHeight,
      ctorSrc: document.documentElement.dataset.ctorSrc || '(没有这个界面标记)',
      bodyHead: (document.body ? document.body.textContent : '').slice(0, 120),
    }))()`
    console.log(JSON.stringify(await cdp.evaluate(dump, ctx.id), null, 2))
  }
}

/*
 * `--settings` 已经删掉：它读的全是参考实现管理页的元素，而 0.2.0 的管理窗是页签式的
 * （`#tabs` + `.pane[data-pane]`），那些元素一个都不存在，一跑就抛异常 ——
 * **一个跑不起来的子命令比没有它更坏**（下次有人会以为「现场就是这样」）。
 * 它当年的三件事现在各有落点：`--tabs` / `--pane-layout` / `--dict-samples`、`--gains`、`--manager`。
 */

/**
 * `--bridge`：**参考实现那套桥**（搬过来的界面 ↔ 重写后的壳）的现场。
 *
 * 为什么非有不可：界面要东西说的是 `web/bridge.js` 那套方法名（`dict:lookup` /
 * `history:get` / `speech:sound` …），而壳的适配层要一条条接起来 ——「哪些接上了、
 * 哪些还差、差的那条报什么」必须能一句话读出来，不然「点了没反应」只能靠猜。
 *
 * 它一次报三样东西：
 *   ① 页面是不是参考实现那一份（那三张样式表 + 参考实现特有的元素 + 胶囊几何）；
 *   ② 逐个调数据类方法，把 **ok / 还差哪一条** 分开摆出来（检查标准是壳自己那句话里的
 *      「壳还没接这条」，不是「没返回值」这种替代指标）；
 *   ③ 剪贴板的一个哨兵往返（写进去再读回来，看是不是**逐字节相同**）。
 *
 * ⚠️ 它**只读现场 / 只调接口**，不下「通过」的结论 —— 检查标准留在 D 级那道 gate 里。
 */
if (has('bridge')) {
  const dump = `(async () => {
    const f = (window.dshLookup && window.dshLookup.floating) || null;
    const g = (s) => document.querySelector(s);
    const pill = g('#pill');
    const box = pill ? pill.getBoundingClientRect() : null;
    const out = {
      page: {
        title: document.title,
        sheets: Array.from(document.styleSheets).map((s) => s.href),
        markers: ['#stage', '#strip', '#content', '#pill', '#grip', '#input']
          .filter((s) => !!g(s)),
        pill: box ? [Math.round(box.width), Math.round(box.height),
                     getComputedStyle(pill).borderRadius] : null,
        dshLookup: typeof window.dshLookup,
        namespaces: window.dshLookup ? Object.keys(window.dshLookup) : null,
      },
      methods: {},
      clipboard: {},
    };
    if (!f) return out;

    async function call(name, run) {
      try {
        const value = await run();
        out.methods[name] = { state: 'ok', value: value === undefined ? null : value };
      } catch (err) {
        const message = String((err && err.message) || err);
        // 「还没接」与「接上了但出错」是两件事，必须分开 —— 前者是搬家搬到一半
        out.methods[name] = {
          state: message.indexOf('壳还没接这条') >= 0 ? 'todo' : 'error',
          message: message.slice(0, 120),
        };
      }
    }

    await call('dict:list', () => f.listDictionaries().then((list) => list.map((d) => ({
      id: d.id.slice(0, 8), title: d.title, status: d.status, current: d.current,
    }))));
    await call('dict:suggest', () => f.suggest('app', 8).then((r) => r.map((x) => x.word + '/' + x.kind)));
    await call('dict:lookup', () => f.lookup('apple').then((p) => ({
      keyText: p.keyText, found: p.found, via: p.via, surface: p.surface,
      entryHost: (p.entryUrl || '').split('/')[2] || '',
    })));
    await call('dict:resolve', () => f.resolve('apple'));
    // dict:probe 拿**真**的一本去问（空的 dictId 会被内核按参数错拒掉 —— 那是内核对的，
    // 但诊断脚本问不出东西）：先读清单，用当前那本（没有当前就用第一本）的 id
    await call('dict:probe', async () => {
      const list = await f.listDictionaries();
      const pick = list.filter((d) => d.current)[0] || list[0] || {};
      if (!pick.id) return '（词库里一本都没有，问不了）';
      return f.probe('apple', pick.id, 500);
    });
    await call('history:get', () => f.getHistory(0, 5).then((p) => ({
      total: p.total, words: (p.items || []).map((i) => i.word), firstAt: (p.items[0] || {}).at,
    })));
    await call('text:script', () => f.scriptOf('苹果'));
    await call('settings:get', () => window.dshLookup.manager
      ? window.dshLookup.manager.getCloseBehavior() : '（没有 manager 命名空间）');
    await call('speech:setSettings', () => f.speechSettings({}).then((s) => s && s.rate));
    await call('speech:clearCache', () => f.clearSpeechCache());
    await call('speech:status', () => f.speechStatus('apple'));
    await call('speech:sound', () => f.playSound('', 'x'));
    await call('dict:borrow', () => f.borrow('apple'));
    await call('translate:status', () => f.translateStatus().then((s) => ({
      hasApiKey: s.hasApiKey, enabled: s.enabled, targetMode: s.targetMode,
      autoTranslate: s.autoTranslate, cache: s.cache,
    })));
    // 真翻一次（**会联网、会按 token 计费**）：只在需要看这条链通不通时用它
    if (${has('bridge-translate')}) {
      await call('translate:text', () => f.translateText('苹果').then((r) => ({
        ok: r.ok, message: r.message, text: r.text, translation: r.translation,
        sourceLanguage: r.sourceLanguage, targetLanguage: r.targetLanguage,
        sourceLabel: r.sourceLabel, targetLabel: r.targetLabel,
        detected: r.detected, tokens: r.tokens, cachedItems: r.cachedItems,
        fetchedItems: r.fetchedItems,
      })));
    }
    await call('layout:info', () => f.getLayoutInfo());

    /*
     * 窗口形状：**问系统**（窗口矩形 + Region + 采样点命中），不靠截图 ——
     * 屏幕截图会被锁屏 / 别的置顶窗口挡掉，而窗口 Region 是 Windows 自己持有的数据。
     * 这一条是窗口那一整套（Region 裁剪 / 吸边 / 拖动）唯一的现场实测结果，
     * 也是 D 级那批断言将来的检查标准来源。
     */
    const d = window.dshLookup.debug;
    if (d) {
      await call('debug:window', () => d.window(${JSON.stringify(arg('window-role', 'floating'))}).then((w) => w && ({
        present: w.present, visible: w.visible, chrome: w.chrome, bounds: w.bounds,
        regionBox: w.regionBox, probes: w.probes,
        /*
         * dpi（这个窗口的真实 DPI）与 scale（布局缩放）是**两个独立来源**：
         * 120 / 1.25 = 125% 屏上 PerMonitorV2 生效；96 / 1 = 进程被判为 DPI 不感知。
         * ⚠️ 这一段住在**模板串**里 —— 里面一个反引号都不许有（：
         *    反引号会把模板串截断，症状是 SyntaxError: Unexpected identifier）。
         */
        dpi: w.dpi, scale: w.scale,
        placement: w.placement, appliedPillTop: w.applied && w.applied.pillTop,
      })));
      await call('debug:dragStats', () => d.dragStats());
    }

    const sentinel = 'probe-bridge-' + Date.now();
    try {
      await f.writeClipboard(sentinel);
      const back = await f.readClipboard();
      out.clipboard = { sentinel: sentinel, back: back, identical: back === sentinel };
    } catch (err) {
      out.clipboard = { error: String((err && err.message) || err).slice(0, 120) };
    }

    // 发完不管那一种情况（send()）不许挂、也不该回包：调一下证明它不炸
    try {
      f.setLayout({ width: 464, panelHeight: 0, direction: 'down', align: 'left' });
      out.methods['layout:set'] = { state: 'sent', value: 'send() 没抛（按设计不回包）' };
    } catch (err) {
      out.methods['layout:set'] = { state: 'error', message: String(err && err.message) };
    }
    return out;
  })()`
  console.log(JSON.stringify(await cdp.evaluate(dump), null, 2))
}

/**
 * `--sepdot`：**音节分隔点**那条路的现场（`app·le` / `pro·gress` 这种词头写法）。
 *
 * 为什么单独一条：这件事发生在**内核那条兜底链**上（先原样问 → 没命中才去点问 →
 * 再没有才翻译）。B 级钉的是链本身，这里读的是**真页面用通信桥问出来**的那份回包：
 * 那三格（`found` / `via` / `needsTranslate`）就是「正文到底换没换、有没有去翻译」的依据。
 *
 * 三行要一起读（少一行就判不了）：
 *   · `plain`：不带点的同一个词（对照 —— 它本来就该查得到，这一格是基线）；
 *   · `dotted`：带点的写法（`--sepdot-word`，默认 `app·le`）；
 *   · `control`：去点之后**也**没有的词（`--sepdot-control`，默认 `zzz·nothing`）——
 *     它必须仍然落到 `via=translate`（去点不许把「机器翻译兜底」那一步吃掉）。
 * 换 Origin 各问一次（`input` 输入框 / `selection` 正文选中）。
 *
 * ⚠️ 这一条**只读现场、不下判断**：红/绿留给 B 级 gate。
 * ⚠️ 它要 `--match floating.html`（那一页才有 `dshLookup.floating`）。
 */
if (has('sepdot')) {
  const word = arg('sepdot-word', 'app·le')
  const plain = arg('sepdot-plain', 'apple')
  const control = arg('sepdot-control', 'zzz·nothing')
  const dump = `(async () => {
    const f = (window.dshLookup && window.dshLookup.floating) || null;
    if (!f) return { error: '这一页上没有 dshLookup.floating（要 --match floating.html）' };
    const pick = (p) => p ? {
      found: p.found, via: p.via, keyText: p.keyText, dictId: (p.dictId || '').slice(0, 8),
      surface: p.surface, needsTranslate: p.needsTranslate, stage: p.stage,
      reason: (p.reason || '').slice(0, 80),
    } : null;
    const out = { word: ${JSON.stringify(word)}, plain: ${JSON.stringify(plain)},
                  control: ${JSON.stringify(control)}, rows: [] };
    for (const origin of ['selection', 'input']) {
      out.rows.push({ which: 'plain', origin: origin,
                      payload: pick(await f.lookup(${JSON.stringify(plain)}, null, origin)) });
      out.rows.push({ which: 'dotted', origin: origin,
                      payload: pick(await f.lookup(${JSON.stringify(word)}, null, origin)) });
      out.rows.push({ which: 'control', origin: origin,
                      payload: pick(await f.lookup(${JSON.stringify(control)}, null, origin)) });
    }
    return out;
  })()`
  console.log(JSON.stringify(await cdp.evaluate(dump), null, 2))
}

/**
 * `--tray`：**托盘菜单**那一套的现场（弹 → 读 → 收 → 再读），专门盯"收干净了没有"。
 *
 * 为什么单独一条：用户 2026-09 报"**托盘右键菜单的按钮层消失了，背景阴影层还留在屏幕上**"，
 * 而且**手动重复又复现不出来**。这正是那种"读代码看不出来、只能驱动真实程序去量"的问题：
 * 菜单窗与外壳层（投影/圆角）是**两张窗口**，收的时候要两张都收 —— 而外壳层是
 * `SetWindowPos(SWP_SHOWWINDOW)` 显示出来的（不是 WinForms 的 `Show()`），两者状态可能不同步。
 * 检查标准只能**问系统**：`debug:window('tray')` 报的 `visible` 是宿主菜单窗，
 * `chrome.visible` 是外壳层（`IsWindowVisible`，即 Windows 自己那份 WS_VISIBLE）。
 *
 * 每轮做三件事，并把三次实测结果都打出来：`beforeShow` → `afterShow` → `afterHide`。
 * `--tray-runs N` 连做 N 轮（默认 1）—— "偶发"的问题只有连做才可能撞上。
 *
 * ★ `--tray-deactivate`：**把"菜单开着时被别的窗口抢走前台"这一幕造出来**（收菜单的
 * 三条路里最容易漏的那条）。做法是弹完菜单之后调 `debug:focus()` 让悬浮窗抢前台，
 * 于是托盘菜单收到 `Deactivate` → 它该把**宿主窗与外壳层一起**收掉。
 * 检查标准就看 `afterDeactivate.chromeVisible`：**宿主收了、外壳还亮着**就是那个残留。
 * ⚠️ 这一条**只读现场、不下判断**（红/绿留给 D 级 gate）；它要 `--match floating.html`。
 */
if (has('tray')) {
  const runs = Number(arg('tray-runs', '1')) || 1
  const alsoDeactivate = has('tray-deactivate')
  const dump = `(async () => {
    const d = (window.dshLookup && window.dshLookup.debug) || null;
    if (!d) return { error: '这一页没有 debug 钩子（要带 --debug-port 起壳）' };
    const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
    const pick = (w) => w ? {
      visible: w.visible,
      chromeVisible: (w.chrome && w.chrome.visible) || false,
      bounds: w.bounds,
      chromeBounds: (w.chrome && w.chrome.bounds) || null,
    } : null;
    const out = { runs: [], deactivate: ${alsoDeactivate ? 'true' : 'false'} };
    for (let i = 0; i < ${runs}; i++) {
      const row = { i: i };
      row.beforeShow = pick(await d.window('tray'));
      await d.showTrayMenu();
      await sleep(1400);
      row.afterShow = pick(await d.window('tray'));
      if (${alsoDeactivate ? 'true' : 'false'}) {
        /* 让**别的窗口**抢前台 —— 托盘菜单自己从不抢前台，所以这一幕平时很难碰上 */
        try { await d.focus(); } catch (e) { row.focusError = String((e && e.message) || e); }
        await sleep(700);
        row.afterDeactivate = pick(await d.window('tray'));
        row.deactivateLeaked = !!(row.afterDeactivate &&
          !row.afterDeactivate.visible && row.afterDeactivate.chromeVisible);
      }
      await d.hideTrayMenu();
      await sleep(400);
      row.afterHide = pick(await d.window('tray'));
      row.leaked = !!(row.afterHide && (row.afterHide.visible || row.afterHide.chromeVisible));
      row.chromeLeaked = !!(row.afterHide && !row.afterHide.visible && row.afterHide.chromeVisible);
      out.runs.push(row);
      await sleep(500);
    }
    out.leakCount = out.runs.filter((r) => r.leaked).length;
    out.chromeLeakCount = out.runs.filter((r) => r.chromeLeaked).length;
    if (${alsoDeactivate ? 'true' : 'false'}) {
      out.deactivateLeakCount = out.runs.filter((r) => r.deactivateLeaked).length;
    }
    return out;
  })()`
  console.log(JSON.stringify(await cdp.evaluate(dump), null, 2))
}

/**
 * `--focus-loss`：**窗口焦点那一路**的现场（参考实现有、0.2.0 搬家时漏了的那两条）。
 *
 * 两步连起来量：
 *   ① `debug:focus()` 先把前台给悬浮窗，再**叫选项窗抢走前台** —— 于是悬浮窗收到 `Deactivate`；
 *   ② 页面里自己订阅一次 `onWindowFocus`，把收到的东西记下来（`window.blob` 收不到，
 *      所以这条信号只有外壳能给）。
 * 然后读三样：**事件到没到**（`{"focused":false}`）、**浮层收没收**、**胶囊自己吸回去没有**
 * （读 DOM：`#strip[hidden]` / `#content[hidden]` / `#strip[data-edge]` —— 吸进去之后
 * `strip` 露出来、`content` 藏起来，与 `main.ts` 里 `applied.absorbed` 那两行一一对应）。
 *
 * ⚠️ 自动吸边有个前提：胶囊**离某条边 ≤28 DIP** 才吸（`AbsorbDistance`），所以先把胶囊
 * 摆到 `--focus-pill-x/y`（默认 8/200，贴左边缘）。**只读现场、不下判断**；
 * 要 `--match floating.html`。
 */
if (has('focus-loss')) {
  const pillX = Number(arg('focus-pill-x', '8'))
  const pillY = Number(arg('focus-pill-y', '200'))
  const wait = Number(arg('focus-wait', '1700'))
  const dump = `(async () => {
    const f = window.dshLookup.floating;
    const d = window.dshLookup.debug;
    if (!f || !d) return { error: '这一页缺 floating / debug' };
    const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
    const focusEvents = [];
    f.onWindowFocus((focused) => { focusEvents.push(focused); });
    const dom = () => ({
      stripHidden: document.getElementById('strip').hidden,
      contentHidden: document.getElementById('content').hidden,
      stripEdge: document.getElementById('strip').dataset.edge || '',
      contentEdge: document.getElementById('content').dataset.edge || '',
    });
    const out = { focusEvents: focusEvents, steps: {} };
    /* 先把胶囊摆到左边缘附近，再让悬浮窗拿到前台 */
    await d.placePill(${pillX}, ${pillY});
    await sleep(500);
    await d.focus();
    await sleep(700);
    out.steps.afterFocus = { dom: dom(), events: focusEvents.slice() };
    /* 叫选项窗抢前台 —— 悬浮窗于是失活 */
    await f.openManager('dicts');
    await sleep(${wait});
    out.steps.afterBlur = { dom: dom(), events: focusEvents.slice() };
    out.steps.afterBlur.window = await d.window('floating');
    /* 收尾：把选项窗关掉、让焦点回到悬浮窗，看有没有 focused:true */
    /* 关窗那条 API 挂在 manager 命名空间上（悬浮窗这一页不一定有它，取不到就算了） */
    if (window.dshLookup.manager && window.dshLookup.manager.closeManager) {
      window.dshLookup.manager.closeManager();
    }
    await sleep(300);
    await d.focus();
    await sleep(500);
    out.steps.afterRefocus = { dom: dom(), events: focusEvents.slice() };
    out.blurSeen = focusEvents.indexOf(false) >= 0;
    out.refocusSeen = focusEvents.indexOf(true) >= 0;
    out.absorbedAfterBlur = out.steps.afterBlur.dom.contentHidden === true &&
                            out.steps.afterBlur.dom.stripHidden === false;
    return out;
  })()`
  console.log(JSON.stringify(await cdp.evaluate(dump), null, 2))
}

/**
 * `--tray-items`：**托盘菜单每一项的标签 + 图标**（读 DOM，不靠看图）。
 *
 * 为什么单独一条：托盘菜单的「显示 / 隐藏悬浮窗」两个状态必须**标签与图标一起翻**
 * （写「隐藏悬浮窗」时用划掉的眼睛 `eyeOff`、写「显示悬浮窗」时用睁着的眼睛 `eye`），
 * 而图标是内联 SVG —— 靠"看菜单长什么样"验不了，得把**每项的标签、子路径条数、每条 `d`**
 * 读出来：两个状态各读一次，子路径数应当一个是 3（轮廓 + 实心瞳孔 + 斜线）、一个是 2。
 *
 * ⚠️ 两个坑（都踩过）：
 *   ① 托盘菜单窗是**按需弹出**的 —— 必须先在 floating.html 那一页调
 *      `debug:showTrayMenu()` 把菜单弹出来，再连 `--match tray-menu.html` 读 DOM；
 *   ② `--match tray-menu.html` 这一趟要**自己开一条 CDP 连接**（跨页读就是这么做的）。
 *
 * 用法（两个状态各来一遍）：
 *   node tools/probe-page.mjs --port 9333 --match floating.html --eval "window.dshLookup.debug.showTrayMenu()"
 *   node tools/probe-page.mjs --port 9333 --match tray-menu.html --tray-items
 *   （要翻到另一态：`--eval "window.dshLookup.floating.toggleFloating()"`，再重弹菜单再读）
 * **只读现场、不下判断**。
 */
if (has('tray-items')) {
  const dump = `(() => {
    const items = Array.from(document.querySelectorAll('#menu .menu-item'));
    return {
      count: items.length,
      items: items.map((el) => {
        const svg = el.querySelector('svg');
        const paths = svg ? Array.from(svg.querySelectorAll('path, circle, rect')) : [];
        return {
          label: (el.querySelector('span:not(.menu-check)') || {}).textContent || '',
          subPaths: paths.map((p) => p.tagName.toLowerCase() + ' ' + (p.getAttribute('d') || '')),
          fills: paths.map((p) => (p.getAttribute('fill') || '-') + '/' + (p.getAttribute('stroke') || '-')),
        };
      }),
    };
  })()`
  console.log(JSON.stringify(await cdp.evaluate(dump), null, 2))
}

/**
 * `--pane-layout`：**某一页能不能滚、有没有元素互相压住**（选项窗口那几页的布局体检）。
 *
 * 为什么固化：用户 2026-09 报「选项-翻译页要加上滚动条，顶部提醒一出现，
 * 下面的『清空缓存』按钮就跟底部设置项重叠」—— 这一页的高度由内容决定，
 * 靠读 CSS 看不出来（病根就是 `.pane[data-pane=…]` 上少一条 `overflow-y:auto`）。
 * 这里直接**量**：这一页能不能滚（`scrollHeight` 对 `clientHeight`）、
 * **同级元素有没有互相压住**（两两比矩形，只算纵向真压住的）、最底下那个元素在不在窗口里。
 *
 * 用法：`node tools/probe-page.mjs --match manager.html --pane-layout`（默认量翻译页）
 *       `--pane-layout speech` / `dicts` 换一页。**只读现场、不下判断**。
 */
if (has('pane-layout')) {
  const which = arg('pane-layout', 'translate')
  const dump = `(() => {
    const pane = document.querySelector('.pane[data-pane=' + JSON.stringify(${JSON.stringify(which)}) + ']');
    if (!pane) return { error: '没有这一页：${which}' };
    const style = getComputedStyle(pane);
    const box = (el) => { const r = el.getBoundingClientRect();
      return { x: Math.round(r.left), y: Math.round(r.top), w: Math.round(r.width), h: Math.round(r.height), bottom: Math.round(r.bottom) }; };
    /*
     * 要量的"同级"是**内容容器里那一层**（.speech-body 的孩子：横幅 / 各个设置组 / 按钮那几行），
     * 不是页面的直接孩子（那只有一个内容容器，两两比矩形什么也看不出来 —— 第一版就是这么写的，
     * 量出来 childCount=1，白跑一趟）。
     */
    const host = pane.querySelector('.speech-body') || pane;
    const kids = Array.from(host.children).filter((el) => !el.hidden);
    const overlaps = [];
    for (let i = 0; i < kids.length; i++) {
      for (let j = i + 1; j < kids.length; j++) {
        const a = kids[i].getBoundingClientRect(), b = kids[j].getBoundingClientRect();
        const vertical = Math.min(a.bottom, b.bottom) - Math.max(a.top, b.top);
        const horizontal = Math.min(a.right, b.right) - Math.max(a.left, b.left);
        if (vertical > 1 && horizontal > 1) {
          overlaps.push({ a: kids[i].id || kids[i].className, b: kids[j].id || kids[j].className, overlapPx: Math.round(vertical) });
        }
      }
    }
    /* 滚到底再看最后一块：能滚到它说明"底下的东西够得着"（而不是被窗口吃掉） */
    const before = pane.scrollTop;
    pane.scrollTop = pane.scrollHeight;
    const paneRect = pane.getBoundingClientRect();
    const last = kids[kids.length - 1];
    const lastBox = last ? box(last) : null;
    const lastReachable = last ? last.getBoundingClientRect().bottom <= paneRect.bottom + 1 : null;
    const lastVisibleAfterScroll = last ? lastBox.bottom <= Math.round(paneRect.bottom) + 1 : null;
    pane.scrollTop = before;
    /*
     * 标签列（这一条是 2026-09 加"常规"页时补的）：
     * '.field-label' 是**定宽**的（flex: 0 0 var(--speech-label-w)，语音页量出来是 54px，
     * 正好放得下四个汉字），所以比四个字长的标签会被挤成两行 ——
     * 而"挤成两行"既不出滚动条也不与谁重叠，上面那两段都看不见它。
     * 这里把它**量出来**：横向裁没裁（scrollWidth 对 clientWidth）+ **折成了几行**。
     *
     * 行数用 Range.getClientRects() 数（**不是** 高 ÷ 行高）：
     * 这些标签的 computed line-height 是 'normal'，parseFloat 出来是 NaN —— 第一版就是
     * 这么写的，量出来 lineHeight: 0 / lines: null，等于没量。
     * Range 给的矩形是**每个行盒一个**，正是"折成几行"本身。
     */
    const labels = Array.from(pane.querySelectorAll('.field-label')).map((el) => {
      const r = el.getBoundingClientRect();
      const range = document.createRange();
      range.selectNodeContents(el);
      const lineRects = Array.from(range.getClientRects()).filter((x) => x.height > 0);
      return {
        text: (el.textContent || '').trim(),
        clientWidth: el.clientWidth,
        scrollWidth: el.scrollWidth,
        clippedX: el.scrollWidth > el.clientWidth + 1,
        height: Math.round(r.height),
        lines: lineRects.length,
        linePitch: lineRects.length > 1 ? Math.round(lineRects[1].top - lineRects[0].top) : null,
      };
    });
    /*
     * 分组与**类别之间的分割线**（2026-09 按需求「相近的选项归类，类别间添加分割线」）。
     * 分割线不是另画的元素，而是 '.speech-group + .speech-group' 上的一条虚线边框 ——
     * 所以"有没有线"只能读**计算样式**：第一组**不该**有线（它是相邻选择器），
     * 后面每一组都该有。这里把三样一起报出来：组的标题、框、以及有没有那条线。
     */
    const groups = Array.from(host.querySelectorAll('.speech-group')).map((g) => {
      const cs = getComputedStyle(g);
      const title = g.querySelector('.speech-group-title');
      return {
        title: title ? title.textContent.trim() : null,
        box: box(g),
        borderTop: cs.borderTopStyle + ' ' + cs.borderTopWidth,
        separated: cs.borderTopStyle !== 'none' && parseFloat(cs.borderTopWidth) > 0,
        controls: Array.from(g.querySelectorAll('button, select, input')).map((el) => el.id || el.tagName),
      };
    });
    return {
      pane: '${which}',
      overflowY: style.overflowY,
      scrollable: pane.scrollHeight > pane.clientHeight,
      scrollHeight: pane.scrollHeight,
      clientHeight: pane.clientHeight,
      paneBox: box(pane),
      contentChildCount: kids.length,
      overlaps: overlaps,
      groups: groups,
      labels: labels,
      lastChild: last ? { which: last.id || last.className, box: lastBox,
        reachableByScrolling: lastReachable && lastVisibleAfterScroll } : null,
    };
  })()`
  console.log(JSON.stringify(await cdp.evaluate(dump), null, 2))
}

/**
 * `--tabs [--tabs-click <页签 id>]`：**选项窗那几个页签的现场**。
 *
 * 为什么固化：用户 2026-09 要求「常规」页排在**词库前面**，还要把页脚那条「关闭悬浮窗时」
 * 搬进这一页 —— "顺序对不对 / 控件到底在不在那一页里 / 页脚还剩什么"三件事
 * 都是**读 DOM 才知道**的（页签的顺序就是 DOM 顺序，肉眼看着对不算数）。
 * 顺带把"点一下能不能翻过去"也量了（`--tabs-click`）：点的是**页签本身**
 * （`#tabSpeech` 这种 id，不是文字），翻没翻看 `activePane` 与 `title`。
 *
 * 用法：`node tools/probe-page.mjs --match manager.html --tabs`
 *       `node tools/probe-page.mjs --match manager.html --tabs --tabs-click tabSpeech`
 * **只读现场、不下判断**。
 */
if (has('tabs')) {
  const clickId = arg('tabs-click', null)
  if (clickId) {
    await cdp.evaluate(`(() => {
      const b = document.getElementById(${JSON.stringify(clickId)});
      if (!b) return false;
      b.click();
      return true;
    })()`)
    // 翻页是同步的，但滚动到底 / 底色重算都在下一帧，等一下再读
    await new Promise((r) => setTimeout(r, 300))
  }
  const dump = `(() => {
    const tabs = Array.from(document.querySelectorAll('.tabs .tab'));
    const panes = Array.from(document.querySelectorAll('.pane'));
    const controls = (root) => Array.from(root.querySelectorAll('button, select, input, a'))
      .filter((el) => el.id || el.textContent.trim())
      .map((el) => el.id || (el.tagName.toLowerCase() + ':' + el.textContent.trim().slice(0, 12)));
    /* 正显示着的那一页：**应当只有一页**（0 = 白板、2 = 两页叠着，都是错的） */
    const visible = panes.filter((p) => !p.hidden);
    return {
      title: document.title,
      tabCount: tabs.length,
      tabs: tabs.map((t, i) => ({
        index: i,
        id: t.id,
        label: t.textContent.trim(),
        dataTab: t.dataset.tab || null,
        paneExists: !!document.getElementById('pane' + t.id.replace(/^tab/, '')),
        active: t.dataset.active === 'true',
        ariaSelected: t.getAttribute('aria-selected'),
      })),
      panes: panes.map((p) => ({
        name: p.dataset.pane || null,
        id: p.id,
        hidden: p.hidden,
        controls: controls(p),
      })),
      activePane: visible.length ? (visible[0].dataset.pane || visible[0].id) : null,
      visiblePaneCount: visible.length,
      footer: controls(document.querySelector('footer.footer') || document.createElement('div')),
    };
  })()`
  console.log(JSON.stringify(await cdp.evaluate(dump), null, 2))
}

/**
 * `--window`：**窗口形状**的现场（窗口矩形 + Region + 采样点命中 + 已生效的布局）。
 *
 * 为什么单独一条：屏幕截图会被锁屏 / 别的置顶窗口挡掉，而"窗口形状对不对"这件事
 * 只能**问系统**（窗口 Region 是 Windows 自己持有的数据）。参考实现当年就是这么量的，
 * 这一版把参考实现的窗口几何搬过来之后同样这么量。
 *
 * `--window-open`：先点一下胶囊（把面板展开）再读 —— 于是能对比"收起 / 展开"两次的
 * Region：**收起时只有胶囊那一块命中，展开时面板那一块也该命中**，
 * 而窗口矩形**两次都不该变**（参考实现的关键设计：窗口上下预留了面板空间，展开时窗口不动）。
 */
if (has('window')) {
  if (has('window-open')) {
    /*
     * 展开面板的**真入口是输入框里有字**（参考实现的 `onInputChanged` → `scheduleSuggest`
     * → `runSuggest` → `setMode('list')` → `pushLayout`）。所以这里给输入框灌两个字再
     * 派一次 `input` —— 只点一下胶囊是没用的：那条路只是把光标送进输入框
     * （`setupPillFocus`），空查询会当场 `collapseToPill()`。
     */
    await cdp.evaluate(`(() => {
      const input = document.getElementById('input');
      if (!input) return false;
      input.value = 'app';
      input.dispatchEvent(new Event('input', { bubbles: true }));
      return true;
    })()`)
    // 联想有防抖，等它跑完再读（不然读到的是"还没有面板"的那一帧）
    await new Promise((r) => setTimeout(r, 1200))
  }

  const dump = `(async () => {
    const d = window.dshLookup && window.dshLookup.debug;
    const f = window.dshLookup && window.dshLookup.floating;
    const out = { dom: {} };
    const pill = document.getElementById('pill');
    const panel = document.getElementById('panel');
    const box = (el) => { const b = el && el.getBoundingClientRect();
      return b ? [Math.round(b.x), Math.round(b.y), Math.round(b.width), Math.round(b.height)] : null; };
    out.dom.pill = box(pill);
    out.dom.panel = box(panel);
    out.dom.panelHidden = panel ? panel.hidden : null;
    out.dom.readerBox = box(document.getElementById('reader'));
    out.applied = await f.getLayoutInfo();
    if (d) {
      const w = await d.window('floating');
      out.window = w && { present: w.present, visible: w.visible, chrome: w.chrome, bounds: w.bounds,
                          regionBox: w.regionBox, probes: w.probes, dpi: w.dpi, scale: w.scale,
                          applied: w.applied };
      out.drag = await d.dragStats();
    }
    return out;
  })()`
  console.log(JSON.stringify(await cdp.evaluate(dump), null, 2))
}

/**
 * `--perf [--perf-word <词>] [--perf-dict <id>]`：**"慢在哪一段"**的现场实测结果。
 *
 * 为什么单独一条："切词典特别慢 / 选中词条回车后要等很久才看到内容"是**端到端**的感受，
 * 而"到底哪一段慢"决定了改哪儿（改错层等于白改）。这条把它拆成几段，全在页面里用
 * `performance.now()` 量（单位 ms）：
 *   ① `setCurrentDictionary(id)` —— 换当前词典**这一步本身**；
 *   ② `lookup(word, id)` 两次    —— **第一次 = 冷加载那一本 `.mdx`**（可能真要读几十 MB），
 *                                   第二次是热态（索引已在内存里）；
 *   ③ `fetch(entryUrl)`          —— **取不到**：宿主页的 CSP 是 `default-src 'self'`，
 *                                   词条正文住在 `*.dictres.invalid`（那个坑），
 *                                   所以这里改成量**用户那条路**：
 *   ④ **输入框里打这个词 → 回车 → 直到 `#readerLoading` 收掉**（词条真的出现在正文框里了）——
 *      这就是用户说的"等很久"，它把 lookup + 词条正文 + 文档里那些资源**一起**算进去。
 *
 * ⚠️ 它会**换当前词典**（这是①②的前提），所以收尾**必须还原**成跑之前那一本 ——
 *    诊断脚本不许改用户的现场（参考实现 那条纪律）。
 * 它**不下判断**：只报数，检查标准由人（或以后的 gate）定。
 */
if (has('perf')) {
  const word = arg('perf-word', 'apple')
  const only = arg('perf-dict', null)
  /*
   * 顺手把**网络**那一层记下来（CDP 的 `Network` 域）：哪几个请求慢、慢在哪儿。
   * 少了它，"一共 700ms"这句话没法归因 —— 页面里那些 `performance.now()` 只能量到
   * "从 A 到 B 多久"，量不到"这段时间在等哪个请求"。
   */
  const netOpen = new Map()
  const netDone = []
  cdp.socket.addEventListener('message', (event) => {
    let m = null
    try { m = JSON.parse(event.data) } catch (err) { return }
    if (m.method === 'Network.requestWillBeSent') {
      netOpen.set(m.params.requestId, { url: m.params.request.url, t0: Date.now() })
    } else if (m.method === 'Network.loadingFinished' || m.method === 'Network.loadingFailed') {
      const req = netOpen.get(m.params.requestId)
      if (!req) return
      netOpen.delete(m.params.requestId)
      netDone.push({
        ms: Date.now() - req.t0,
        bytes: m.params.encodedDataLength || 0,
        failed: m.method === 'Network.loadingFailed' ? (m.params.errorText || 'failed') : null,
        url: req.url,
      })
    }
  })
  await cdp.send('Network.enable')
  const dump = `(async () => {
    const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
    const f = window.dshLookup.floating
    const out = { word: ${JSON.stringify(word)}, dicts: [], e2e: null, restored: null }
    const dicts = await f.listDictionaries()
    const was = (dicts.find((d) => d.current) || {}).id || null
    for (const d of dicts) {
      if (${JSON.stringify(only)} && d.id !== ${JSON.stringify(only)}) continue
      const row = { id: d.id, title: d.title }
      let t0 = performance.now(); await f.setCurrentDictionary(d.id)
      row.setCurrentMs = Math.round(performance.now() - t0)
      t0 = performance.now(); const r = await f.lookup(${JSON.stringify(word)}, d.id)
      row.lookupColdMs = Math.round(performance.now() - t0)
      row.found = !!(r && r.found)
      t0 = performance.now(); await f.lookup(${JSON.stringify(word)}, d.id)
      row.lookupWarmMs = Math.round(performance.now() - t0)
      /*
       * origin 这一格是**页面上真正传的那个值**：输入框回车走 input，
       * 而 input 是要跑**兜底通道**的（借查 / 再问一遍 / 翻译那一链）。
       * 上面那两次不带 origin（= 不走通道），两条路的差就是"通道本身的代价"。
       */
      t0 = performance.now(); await f.lookup(${JSON.stringify(word)}, d.id, 'input')
      row.lookupOriginInputMs = Math.round(performance.now() - t0)
      t0 = performance.now(); await f.lookup(${JSON.stringify(word)}, d.id, 'input')
      row.lookupOriginInputWarmMs = Math.round(performance.now() - t0)
      out.dicts.push(row)
    }
    if (was) { await f.setCurrentDictionary(was); out.restored = was }
    /* ④ 用户那条路：打词 → 回车 → 词条可见 */
    const input = document.getElementById('input')
    const loading = document.getElementById('readerLoading')
    const entryWord = document.getElementById('entryWord')
    const frame = document.getElementById('entryFrame')
    /*
     * 长任务观察者：这一趟里**主线程被占住**多久（>50ms 的块）。
     * 它把"在等（IPC / 窗口调整 / 网络）"与"在算（JS / 重排）"分开 ——
     * 少了它，只能看到"一共 700ms"，看不出该改哪一层。
     */
    const longTasks = []
    let observer = null
    try {
      observer = new PerformanceObserver((list) => {
        for (const e of list.getEntries()) longTasks.push({ start: Math.round(e.startTime), ms: Math.round(e.duration) })
      })
      observer.observe({ entryTypes: ['longtask'] })
    } catch (err) { /* 老的运行时没有 longtask：如实不报，不影响别的实测结果 */ }
    const runE2e = async () => {
      longTasks.length = 0
      input.focus()
      input.value = ${JSON.stringify(word)}
      input.dispatchEvent(new Event('input', { bubbles: true }))
      await sleep(1200)                       /* 联想有防抖，等它落定 */
      const t0 = performance.now()
      input.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true, cancelable: true }))
      let shown = false
      let t = 0
      while (t < 1000) { if (loading && !loading.hidden) { shown = true; break } await sleep(10); t = performance.now() - t0 }
      while (t < 30000) { if (loading && loading.hidden) break; await sleep(10); t = performance.now() - t0 }
      return {
        ms: Math.round(t),
        loadingShown: shown,
        longTasks: longTasks.slice(),
        longMs: longTasks.reduce((s, x) => s + x.ms, 0),
      }
    }
    out.e2e = await runE2e()
    /* 同一条路再走一次：这一种情况能看出"词条正文与它的资源有没有被缓存" */
    await sleep(500)
    out.e2eAgain = await runE2e()
    out.entryWord = (entryWord && entryWord.textContent) || ''
    /*
     * ⑤ 词条正文那一段单独量：把 iframe 的 src 换成同一个地址（加个一次性参数强制重载），
     *    从赋值到 load 事件 —— 它就是③里那段"取不到"的东西，只能这样量。
     */
    const src = (frame && frame.src) || ''
    if (src) {
      const base = src.split('&t=')[0]
      const t0 = performance.now()
      const done = new Promise((resolve) => {
        const onLoad = () => { frame.removeEventListener('load', onLoad); resolve() }
        frame.addEventListener('load', onLoad)
        setTimeout(resolve, 30000)
      })
      frame.src = base + '&t=' + Date.now()
      await done
      out.iframeReloadMs = Math.round(performance.now() - t0)
    }
    return out
  })()`
  const result = await cdp.evaluate(dump)
  result.network = netDone.slice().sort((a, b) => b.ms - a.ms).slice(0, 15)
  result.networkCount = netDone.length
  console.log(JSON.stringify(result, null, 2))
}

/**
 * `--pitch [--pitch-word <词>] [--pitch-dict <id>]`：**这段录音到底以什么速率在播**。
 *
 * 为什么单独一条：用户 2026-09 报"词典内置的语音播放速度有点快"，而两版取回来的字节
 * **完全相同**（25 004 B、头里写 22 050 Hz）—— 那就得回答"22 050 这个标注对不对"。
 * 检查标准只能从**声音本身**来：把 PCM 自己读出来，量**基音周期有多少个采样**，然后
 * 在两种假设下各算一次基频与时长：
 *   · 若码流真是 16 kHz（Speex 宽带模式的速率）→ F0 = 16000 / 周期，时长 = N / 16000
 *   · 若码流真是 22.05 kHz（文件头里写的那样）→ F0 = 22050 / 周期，时长 = N / 22050
 * 人声的基频落在 85–255 Hz（男 85–155 / 女 165–255）。**哪一种假设落进人声区、
 * 而且时长像"念一个词"，那一种就是对的** —— 这也是唯一不靠耳朵能说清的办法。
 *
 * ⚠️ 它**自己解析 WAV 字节**（不走 `decodeAudioData`）：后者会按头里的速率重采样，
 *    那样量出来的 F0 只是"按标注播出来的结果"，证不了标注对不对。
 */
if (has('pitch')) {
  const word = arg('pitch-word', 'apple')
  const dictId = arg('pitch-dict', null)
  const dump = `(async () => {
    const f = window.dshLookup.floating
    const out = { word: ${JSON.stringify(word)}, dictId: null, title: '', url: '', mime: '' }
    const dicts = await f.listDictionaries()
    const d = ${JSON.stringify(dictId)}
      ? dicts.filter((x) => x.id === ${JSON.stringify(dictId)})[0]
      : (dicts.filter((x) => x.current)[0] || dicts[0])
    if (!d) return { error: '没有词典' }
    out.dictId = d.id; out.title = d.title
    /* 词条自带录音的键名：走旧协议的 speech.dictAudio（与 --speech-sound 同一招）*/
    const host = window.parent === window ? window : window.parent
    const replies = []
    const onMessage = (e) => { try { replies.push(e.data) } catch (_) {} }
    host.addEventListener('message', onMessage)
    host.postMessage({ id: 9201, method: 'speech.dictAudio', params: { dictId: d.id, keyText: ${JSON.stringify(word)} } })
    await new Promise((r) => setTimeout(r, 600))
    const answer = replies.filter((m) => m && m.id === 9201)[0]
    /* pitch-key 给了就直接用那个键名：旧协议那个 speech.dictAudio 是**按当前词条**
       回答的，诊断脚本页面里的"当前词条"未必是你要量的那一条（拿它当唯一来源会问不到）。*/
    const key = ${JSON.stringify(arg('pitch-key', null))} || ((answer && answer.result && answer.result.audioKey) || '')
    if (!key) return { ...out, error: '这条词条没有自带录音' }
    const s = await f.playSound(d.id, key)
    if (!s || !s.ok) return { ...out, error: 'playSound 失败：' + JSON.stringify(s) }
    out.url = s.url; out.mime = s.mime; out.key = key
    const resp = await fetch(s.url)
    const ab = await resp.arrayBuffer()
    const u8 = new Uint8Array(ab)
    const ascii = (at) => String.fromCharCode(u8[at], u8[at+1], u8[at+2], u8[at+3])
    if (ascii(0) === 'RIFF' && ascii(8) === 'WAVE') {
      const dv = new DataView(ab)
      const rate = dv.getUint32(24, true), ch = dv.getUint16(22, true)
      const n = Math.floor((ab.byteLength - 44) / 2 / (ch || 1))
      out.wavHeaderRate = rate; out.channels = ch; out.samples = n
      const pcm = new Float32Array(n)
      for (let i = 0; i < n; i++) pcm[i] = dv.getInt16(44 + i * 2 * (ch || 1), true) / 32768
      out.durationIf16k = +(n / 16000).toFixed(3)
      out.durationIf22050 = +(n / 22050).toFixed(3)
      /* 自相关量基音周期（采样数）：只在有声帧上量，取中位数 */
      const win = Math.min(n, Math.round(0.040 * rate))   /* 40 ms 窗 */
      const hop = Math.max(1, Math.round(0.010 * rate))
      const periods = []
      for (let at = 0; at + win <= n; at += hop) {
        let e = 0
        for (let i = 0; i < win; i++) e += pcm[at + i] * pcm[at + i]
        if (Math.sqrt(e / win) < 0.02) continue       /* 静音帧不算 */
        let best = 0, bestLag = 0
        const minLag = 8, maxLag = Math.round(win / 2)
        for (let lag = minLag; lag <= maxLag; lag++) {
          let s = 0
          for (let i = 0; i + lag < win; i++) s += pcm[at + i] * pcm[at + i + lag]
          if (s > best) { best = s; bestLag = lag }
        }
        if (bestLag > 0) periods.push(bestLag)
      }
      periods.sort((a, b) => a - b)
      const period = periods.length ? periods[Math.floor(periods.length / 2)] : 0
      out.voicedFrames = periods.length
      out.periodSamples = period
      out.f0If16k = period ? Math.round(16000 / period) : null
      out.f0If22050 = period ? Math.round(22050 / period) : null
    } else {
      /*
       * mp3 那条路：**我们不碰字节**（原样发出去，浏览器按 mp3 自己的头解），
       * 所以这里把**文件自己声明的**那一套也读出来 —— 它决定播放速率：
       * MPEG 帧头 4 字节里的版本 / 层 / 采样率索引 / 声道模式。
       * 两本词典的录音走不同路径（.spx 我们要解、.mp3 我们不碰），
       * 要谈"归一化"就得先把两条路各自的**声明值**摆在一起看。
       */
      const head = (() => {
        for (let i = 0; i + 4 < u8.length; i++) {
          if (u8[i] === 0xff && (u8[i + 1] & 0xe0) === 0xe0) {
            const verBits = (u8[i + 1] >> 3) & 0x03
            const layerBits = (u8[i + 1] >> 1) & 0x03
            const rateIdx = (u8[i + 2] >> 2) & 0x03
            const brIdx = (u8[i + 2] >> 4) & 0x0f
            const chMode = (u8[i + 3] >> 6) & 0x03
            const ver = verBits === 3 ? 'MPEG1' : verBits === 2 ? 'MPEG2' : verBits === 0 ? 'MPEG2.5' : '?'
            const layer = layerBits === 1 ? 'Layer3' : layerBits === 2 ? 'Layer2' : layerBits === 3 ? 'Layer1' : '?'
            const base = verBits === 3 ? [44100, 48000, 32000] : verBits === 2 ? [22050, 24000, 16000] : [11025, 12000, 8000]
            const brTable = verBits === 3
              ? (layerBits === 1 ? [0,32,40,48,56,64,80,96,112,128,160,192,224,256,320] : [])
              : (layerBits === 1 ? [0,8,16,24,32,40,48,56,64,80,96,112,128,144,160] : [])
            return { at: i, version: ver, layer: layer,
                     declaredRate: base[rateIdx] || 0, bitrateKbps: brTable[brIdx] || 0,
                     channelMode: ['stereo','joint','dual','mono'][chMode] }
          }
        }
        return null
      })()
      const ac = new AudioContext()
      const buf = await ac.decodeAudioData(ab)
      const sr = buf.sampleRate
      const pcm = buf.getChannelData(0)
      out.mp3Header = head
      out.decodedRate = sr
      out.decodedSeconds = +buf.duration.toFixed(3)
      const win = Math.round(0.040 * sr), hop = Math.round(0.010 * sr)
      const periods = []
      for (let at = 0; at + win <= pcm.length; at += hop) {
        let e = 0
        for (let i = 0; i < win; i++) e += pcm[at + i] * pcm[at + i]
        if (Math.sqrt(e / win) < 0.02) continue
        let best = 0, bestLag = 0
        for (let lag = Math.round(sr / 500); lag <= Math.round(sr / 60); lag++) {
          let s = 0
          for (let i = 0; i + lag < win; i++) s += pcm[at + i] * pcm[at + i + lag]
          if (s > best) { best = s; bestLag = lag }
        }
        if (bestLag > 0) periods.push(bestLag)
      }
      periods.sort((a, b) => a - b)
      const period = periods.length ? periods[Math.floor(periods.length / 2)] : 0
      out.voicedFrames = periods.length
      out.f0Hz = period ? Math.round(sr / period) : null
      await ac.close()
    }
    return out
  })()`
  console.log(JSON.stringify(await cdp.evaluate(dump), null, 2))
}

/**
 * `--speech [--speech-word <词>]`：**发音那两条路的现场**。
 *
 * 为什么单独一条：发音是"跨层"的活（内核规划音源 → 壳探测本机嗓子 / 合成 / 解码 →
 * 页面把 `url` 塞给 `<audio>`），任何一层对不上都只会表现为"点了没声音"，
 * 而这条把**每一层给的东西**摆在一起：
 *   · `speak(词)`   —— 内核算的音源与嗓子、壳合成出来的地址与字节数、语种中文名；
 *   · `playSound(词典 id, 键名)` —— 词条里那个 🔊 走的那条路。
 *     键名用**旧协议**的 `speech.dictAudio` 现问一次（它就是"这条词条自带的原录音在哪"），
 *     于是不必手工去 `.mdd` 里找路径。
 *
 * 它**不下判断**（检查标准在 D 级 gate 里）：只报现场。
 */
if (has('speech')) {
  const word = arg('speech-word', 'apple')
  const dump = `(async () => {
    const f = window.dshLookup && window.dshLookup.floating;
    const host = window.chrome && window.chrome.webview;
    const out = { word: ${JSON.stringify(word)} };
    if (!f) return out;

    try {
      const r = await f.speak(${JSON.stringify(word)});
      out.speak = {
        ok: r.ok, source: r.source, url: r.url, mime: r.mime,
        language: r.language, languageLabel: r.languageLabel,
        voiceId: r.voiceId, voiceName: r.voiceName,
        bytes: r.bytes, cached: r.cached, gainDb: r.gainDb,
        templateIndex: r.templateIndex, message: r.message, why: r.why,
      };
    } catch (err) { out.speakError = String((err && err.message) || err); }

    /*
     * 再**把那条地址取一次**：页面拿到 url 之后就是这个动作（塞给 audio 元素）。
     * 这一步把"宿主那条路由真的发得出字节"也验掉 —— 不然"有 url"可能是句空话。
     * （不碰真的声卡：自动化环境常常没有音频设备。）
     */
    if (out.speak && out.speak.ok && out.speak.url) {
      try {
        const res = await fetch(out.speak.url);
        const buf = new Uint8Array(await res.arrayBuffer());
        out.speakFetch = {
          status: res.status,
          contentType: res.headers.get('content-type'),
          bytes: buf.length,
        };
        /*
         * ★ 是 WAV 的话把**头里那几个数**也报出来：采样率、声道、数据长度、PCM 指纹。
         * 这是"这段录音播起来快了 / 慢了"唯一说得清的实测结果 —— 两版对同一段
         * 词典录音（.spx 要现解）若是同一份 PCM、头里速率也一样，声音就不可能不一样。
         */
        const ascii = (at) => String.fromCharCode(buf[at], buf[at + 1], buf[at + 2], buf[at + 3]);
        if (buf.length > 44 && ascii(0) === 'RIFF' && ascii(8) === 'WAVE') {
          const u32 = (at) => buf[at] | (buf[at + 1] << 8) | (buf[at + 2] << 16) | (buf[at + 3] << 24);
          const u16 = (at) => buf[at] | (buf[at + 1] << 8);
          let sum = 0;
          for (let i = 44; i < buf.length; i++) sum = (sum * 31 + buf[i]) >>> 0;
          out.speakFetch.wav = {
            channels: u16(22), rate: u32(24), byteRate: u32(28), dataBytes: u32(40),
            samples: Math.round(u32(40) / (u16(22) || 1) / 2),
            seconds: +(u32(40) / (u32(28) || 1)).toFixed(3),
            pcmHash: sum,
          };
        }
      } catch (err) { out.speakFetchError = String((err && err.message) || err); }
    }

    if (${has('speech-sound')} && host) {
      const list = await f.listDictionaries();
      const pick = list.filter((d) => d.current)[0] || list[0] || {};
      out.dictId = pick.id || '';
      out.dictTitle = pick.title || '';
      const replies = [];
      const onMessage = (e) => { try { replies.push(e.data); } catch (_) {} };
      host.addEventListener('message', onMessage);
      host.postMessage({
        id: 9101, method: 'speech.dictAudio',
        params: { dictId: out.dictId, keyText: ${JSON.stringify(word)} },
      });
      await new Promise((r) => setTimeout(r, 600));
      const answer = replies.filter((m) => m && m.id === 9101)[0];
      out.dictAudio = answer ? answer.result : null;
      const key = (answer && answer.result && answer.result.audioKey) || '';
      if (key) {
        try {
          const s = await f.playSound(out.dictId, key);
          out.sound = {
            ok: s.ok, source: s.source, url: s.url, mime: s.mime, file: s.file,
            bytes: s.bytes, gainDb: s.gainDb, message: s.message,
          };
          /*
           * ★ 顺手把**取回来的字节**量一眼（WAV 头 + 数据长度）：
           * "这段录音播起来快了 / 慢了"这种事只能靠头里的采样率与样本数说清 ——
           * 同一段 .spx 在两版里解出来的 PCM 若一致、而头里标的速率不同，
           * 症状就是"音高与语速差一个比例"。地址是同源的（同一个包），可比。
           */
          if (s && s.ok && s.url) {
            const resp = await fetch(s.url);
            const buf = new Uint8Array(await resp.arrayBuffer());
            const u32 = (at) => buf[at] | (buf[at + 1] << 8) | (buf[at + 2] << 16) | (buf[at + 3] << 24);
            const u16 = (at) => buf[at] | (buf[at + 1] << 8);
            let sum = 0;
            for (let i = 44; i < buf.length; i++) sum = (sum * 31 + buf[i]) >>> 0;
            out.soundBytes = {
              total: buf.length,
              riff: String.fromCharCode(buf[0], buf[1], buf[2], buf[3]),
              wave: String.fromCharCode(buf[8], buf[9], buf[10], buf[11]),
              channels: u16(22),
              rate: u32(24),
              byteRate: u32(28),
              dataBytes: u32(40),
              /* 头里那句速率 × 声道 × 2 与实际字节数对不对得上（对不上说明头在撒谎）*/
              pcmHash: sum,
            };
          }
        } catch (err) { out.soundError = String((err && err.message) || err); }
      } else {
        out.sound = '(这条词条没有自带录音，没法验 🔊 那条路)';
      }
    }
    return out;
  })()`
  console.log(JSON.stringify(await cdp.evaluate(dump), null, 2))
}

/**
 * `--gains [--gains-word <词>] [--gains-value <dB>]`：**设置页那两条音量滑块的现场**。
 *
 * 它把"内核那份视图"与"设置里真正躺着的数"摆在一起：
 *   · `before` —— `speech:status` 里的 `voiceGains`（值、能不能调、不能调时那句话）
 *     与设置里那两个键（`speech.dictGainDb` / `speech.systemGainDb`）；
 *   · `afterWrite` —— 写一个诊断脚本值**之后**同一份视图（内核 `set` 回的就是它）；
 *   · `afterRestore` —— 把**原来那个值**写回去之后的视图。
 *
 * ⚠️ 它**会写设置**（这就是它存在的意义：光读看不出"写进去的数字有没有被归一化、
 *    有没有只改那一本"）。所以运行前把 `LOOKUP_USER_DATA` 指到一个临时目录；
 *    它自己也**尽力把原值写回去**（原本没设过就写 `null` = 清掉那一项），
 *    并把"写回去了什么"报在 `restored` 里 —— 但**别拿它当"绝对不碰用户设置"的保证**。
 *
 * 它**不下判断**（检查标准在 D 级 gate 里）：只报现场。
 */
if (has('gains')) {
  const word = arg('gains-word', 'apple')
  const probeValue = Number(arg('gains-value', '4.5'))
  const dump = `(async () => {
    const b = (window.dshLookup && window.dshLookup.manager) || null;
    const out = {
      page: location.pathname,
      word: ${JSON.stringify(word)},
      probeValue: ${JSON.stringify(probeValue)},
    };
    /*
     * ⚠️ setGains **只在管理窗那一页的桥上**（bridge.js 的 manager 命名空间）——
     *    悬浮窗那一页没有这个方法（写在那儿 = 设置页没打开时也能改音量，而调它的就是设置页）。
     *    所以这条子命令要配 --match manager.html；跑错页面时**明说**，别让它像个别的错。
     *    ⚠️ 这段注释在**注入的模板串**里：这里不许出现反引号（那会把模板串截断）。
     */
    if (!b || typeof b.setGains !== 'function') {
      return 'no-gains-bridge：这一页的桥上没有 setGains（它只在管理窗那页；用 --match manager.html）';
    }
    const view = (s) => (s && s.voiceGains) || null;
    try {
      const before = await b.speechStatus(${JSON.stringify(word)});
      const v0 = view(before) || {};
      out.dictId = v0.dictId;
      out.dictTitle = v0.dictTitle;
      out.before = v0;
      out.settingsBefore = before.settings
        ? { dictGainDb: before.settings.dictGainDb || null, systemGainDb: before.settings.systemGainDb }
        : null;
      const original = (typeof v0.dictGainDb === 'number' && v0.dictGainDb !== 0) ? v0.dictGainDb : null;
      const written = await b.setGains({ dictGainDb: ${JSON.stringify(probeValue)} });
      out.afterWrite = view(written);
      out.settingsAfterWrite = written && written.settings ? written.settings.dictGainDb : null;
      const restored = await b.setGains({ dictGainDb: original });
      out.afterRestore = view(restored);
      out.restored = original;
    } catch (err) { out.error = String((err && err.message) || err); }
    return out;
  })()`
  console.log(JSON.stringify(await cdp.evaluate(dump), null, 2))
}

/**
 * `--dict-samples`：**词典取样那条路的现场**（`dictSamples()`，参考实现的 `speech:dictSamples`）。
 *
 * 它报四件事：挑出来的样本清单（词 / 可播地址 / 语种）、条数、扫描实测结果（`scanned` /
 * `elapsedMs` / `budgetHit`）与那句人话 —— 再加一件**只有真页面才做得到**的：
 * 拿第一条样本的地址**真取一次字节数**（"有 url"骗不过这一步）。
 *
 * ⚠️ 要配 `--match manager.html`：`dictSamples` **只在管理窗那一页的桥上**
 *    （它就是那条「平衡音量」调的接口）。跑错页面时**明说**，别让它像个别的错。
 * ⚠️ 它**只读、不写设置**（与 `--gains` 相反）。
 * ⚠️ 注入的模板串里**不许出现反引号**（会把模板串截断）。
 */
if (has('dict-samples')) {
  const dump = `(async () => {
    const b = (window.dshLookup && window.dshLookup.manager) || null;
    if (!b || typeof b.dictSamples !== 'function') {
      return 'no-dict-samples-bridge：这一页的桥上没有 dictSamples' +
             '（它只在管理窗那页；用 --match manager.html）';
    }
    const out = { page: location.pathname };
    try {
      const r = await b.dictSamples();
      out.ok = r && r.ok;
      out.dictId = r && r.dictId;
      out.dictTitle = r && r.dictTitle;
      out.message = r && r.message;
      out.count = (r && r.samples && r.samples.length) || 0;
      out.samples = ((r && r.samples) || []).map(function (s) {
        return { word: s.word, file: s.file, language: s.language, languageLabel: s.languageLabel, url: s.url };
      });
      /*
       * 「有 url」不算数：真取一次，报字节数。页面 CSP 的 connect-src 'self' 够用
       * （那条地址就挂在外壳站点上）。
       */
      const first = out.samples[0];
      if (first && first.url) {
        try {
          const resp = await fetch(first.url);
          const buf = await resp.arrayBuffer();
          out.firstFetch = { status: resp.status, bytes: buf.byteLength,
                             contentType: resp.headers.get('content-type') };
        } catch (err) { out.firstFetch = '取不到：' + String((err && err.message) || err); }
      } else {
        out.firstFetch = '(没有样本可取)';
      }
    } catch (err) { out.error = String((err && err.message) || err); }
    return out;
  })()`
  console.log(JSON.stringify(await cdp.evaluate(dump), null, 2))
}

/**
 * `--manager [--manager-open]`：**词库管理窗**的现场。
 *
 * 它要回答的是"第二扇窗到底开没开、页面接上没有、桥通不通"：
 *   · `--manager-open` 先**从悬浮窗那一页**叫一次 `openManager(页签，默认 dicts)`
 *     （那就是用户点「管理」按钮走的那条路）；
 *   · 然后**另开一条 CDP 连接**接上 `manager.html`（诊断脚本的规矩：新加的块自己开连接），
 *     报窗口里的 DOM 界面标记 + 两条桥方法的结果（列词典 / 读关闭行为）。
 *
 * ⚠️ 管理窗是**懒建**的：第一次 `manager:open` 才建句柄、才起 WebView2，
 *    所以这里必须等一会儿再找目标（等不到就是"没开成"，那是要报出来的事实）。
 */
if (has('manager')) {
  let openResult = '(没叫)'
  if (has('manager-open')) {
    await cdp.evaluate(`(async () => {
      const f = window.dshLookup && window.dshLookup.floating;
      if (!f) return 'no-bridge';
      try { await f.openManager(${JSON.stringify(arg('manager-tab', 'dicts'))}); return 'ok'; }
      catch (err) { return 'error: ' + (err && err.message); }
    })()`)
    // 建窗 + 起 WebView2 + 装页面，给它足够时间
    await new Promise((r) => setTimeout(r, 3500))
  }

  const all = await listTargets()
  const managerTarget = all.filter((t) => t.type === 'page' && t.url.includes('manager.html'))[0]
  if (!managerTarget) {
    console.log(
      JSON.stringify(
        {
          found: false,
          hint: '没有 manager.html 这个目标 —— 要么没调 openManager，要么窗口没建起来',
          openResult: openResult,
          targets: all.map((t) => t.url),
        },
        null,
        2
      )
    )
  } else {
    const managerCdp = new Cdp(managerTarget.webSocketDebuggerUrl)
    await managerCdp.open() // ⚠️ 必须先 await，否则第一句 send 会撞上「Sent before connected」
    const dump = `(async () => {
      const m = window.dshLookup && window.dshLookup.manager;
      const g = (s) => document.getElementById(s);
      const out = {
        url: location.href,
        title: document.title,
        markers: ['titlebar', 'tabs', 'paneDicts', 'dictList', 'btnMinimize', 'btnClose']
          .filter((s) => !!g(s)),
        visibleTabs: Array.from(document.querySelectorAll('#tabs .tab'))
          .map((t) => (t.dataset.tab || t.textContent || '').trim()),
        bridge: typeof window.dshLookup,
        // 哪一页是当前页：options:tab 与 manager:initialTab 有没有真的翻过去，看这里
        tabs: Array.from(document.querySelectorAll('#tabs .tab')).map((t) => ({
          tab: t.dataset.tab, selected: t.getAttribute('aria-selected'), cls: t.className,
        })),
      };
      if (m) {
        try { out.closeBehavior = await m.getCloseBehavior(); }
        catch (err) { out.closeBehaviorError = String((err && err.message) || err); }
        try {
          const list = await m.listDictionaries();
          out.dicts = list.map((d) => ({ title: d.title, status: d.status, current: d.current }));
        } catch (err) { out.dictsError = String((err && err.message) || err); }
      }
      return out;
    })()`
    console.log(JSON.stringify(await managerCdp.evaluate(dump), null, 2))
    managerCdp.close && managerCdp.close()
  }
}

/**
 * `--seltrace [--seltrace-word <词>]`：**"选中文字 → 浮出那排按钮"这条路卡在哪一步**。
 *
 * 它是一个**排查工具**，不是检查标准：把三样东西摆在一起看 ——
 *   ① 词条正文那一侧选没选中（在我们注入的 JS 里当场读 `getSelection()`）；
 *   ② 文档有没有把 `selection` 报上来（在宿主页装一个 `window.__selLog` 收 postMessage）；
 *   ③ 宿主页那排浮层（`#selToolbar`）收着还是露着、上面有几个按钮。
 *
 * ⚠️ 第 ② 步是这个工具的关键：光看"浮层没出来"分不清是**文档没报**、
 *    **报了但载荷不全**（父页面要求 `text` 与 `rect` 都在）、还是**报了但被条件挡掉**
 *    （`dismissedEntryText` 那条）。装了消息流水就一眼看得出来。
 */
if (has('seltrace')) {
  const word = arg('seltrace-word', 'apple')

  /* ① 宿主页：装消息流水（装之前先把它清空，免得读上一次的）*/
  await cdp.evaluate(`(() => {
    window.__selLog = [];
    if (!window.__selInstalled) {
      window.__selInstalled = true;
      window.addEventListener('message', (event) => {
        try { window.__selLog.push(event.data); } catch (err) { /* 记不下来就算了 */ }
      });
    }
    return {
      toolbarHidden: document.getElementById('selToolbar').hidden,
      buttons: document.querySelectorAll('#selToolbar button').length,
    };
  })()`)

  /* ② 词条正文：选中那段文字并派发 mouseup（文档在 mouseup/selectionchange 上节流上报）*/
  /*
   * ⓪ ⚠️ **先让词条重新导航一次**：词条 iframe 的执行上下文只有在**我们这条连接建立之后**
   *    新建的才会被 `Runtime.executionContextCreated` 报过来。如果那份文档在诊断脚本接进来
   *    之前就装好了（很常见：先起程序、再跑诊断脚本），上下文表里根本没有它 ——
   *    症状是"没有找到词条页的执行上下文"，看起来像"正文框里没装东西"。
   *    所以这里先打字 + 回车，让它在**这条连接上**重新装一次。
   */
  await cdp.evaluate(`(() => {
    const input = document.getElementById('input');
    input.value = ${JSON.stringify(word)};
    input.dispatchEvent(new Event('input', { bubbles: true }));
    input.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }));
    return 'asked';
  })()`)
  await new Promise((r) => setTimeout(r, 2500))
  /* 词条那一页的上下文：用**隔离世界**拿（见 entryContextId 的说明，别依赖事件推送）*/
  const ctxId = await entryContextId()
  const entryJs = `(() => {
    const needle = ${JSON.stringify(word)};
    const walker = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT, null, false);
    let node;
    while ((node = walker.nextNode())) {
      const at = (node.nodeValue || '').indexOf(needle);
      if (at < 0) continue;
      const range = document.createRange();
      range.setStart(node, at);
      range.setEnd(node, at + needle.length);
      const sel = window.getSelection();
      sel.removeAllRanges();
      sel.addRange(range);
      document.dispatchEvent(new MouseEvent('mouseup', { bubbles: true }));
      const box = range.getBoundingClientRect();
      return {
        selected: String(sel.toString()),
        collapsed: sel.isCollapsed,
        rect: [box.left, box.top, box.width, box.height],
        frameBox: (() => { const b = window.frameElement && window.frameElement.getBoundingClientRect();
          return b ? [b.left, b.top, b.width, b.height] : null; })(),
      };
    }
    return { selected: '(没找到那段文字)' };
  })()`
  const inEntry = ctxId ? await cdp.evaluate(entryJs, ctxId) : '(没有词条正文的上下文)'
  console.log('=== 词条正文那一侧 ===')
  console.log(JSON.stringify(inEntry, null, 2))

  /* ③ 等文档把选区报上来（60ms 节流 + 父页面摆浮层），再看宿主页 */
  await new Promise((r) => setTimeout(r, 1200))
  const after = await cdp.evaluate(`(() => {
    const tb = document.getElementById('selToolbar');
    const box = tb.getBoundingClientRect();
    return {
      toolbarHidden: tb.hidden,
      buttons: Array.from(tb.querySelectorAll('button')).map((b) => b.title || ''),
      toolbarBox: [Math.round(box.left), Math.round(box.top), Math.round(box.width), Math.round(box.height)],
      panelHidden: document.getElementById('panel').hidden,
      messages: (window.__selLog || []).map((m) => ({
        type: m && m.type,
        text: m && typeof m.text === 'string' ? m.text.slice(0, 20) : undefined,
        hasRect: !!(m && m.rect),
        empty: m && m.empty,
      })),
    };
  })()`)
  console.log('=== 宿主页那一侧 ===')
  console.log(JSON.stringify(after, null, 2))
}

/**
 * `--history`：查词历史那一屏的现场（每一行、它带没带 `data-dict-id`、
 * 点一下之后正文框去的是**哪一本**）。
 *
 * 为什么这条非有不可：历史那条路的**核心约定**是"点一条要回到**当时那本词典**"
 * （`docs/design/查词兜底通道与历史记录开发指导.md`  D6/D11）。而"点下去去了哪一本"
 * 只有一个地方看得出来 —— 词条 iframe 的地址里那个**主机名就是 dictId**
 * （`https://<dictId>.dictres.invalid/…`）。所以这条诊断脚本把两样东西摆在一起：
 * 每一行的 `data-dict-id`，和点完之后 `#reader` 的 src / `data-dict-id`。
 * 两者不一致就是那个 bug 复发。
 */
if (has('history')) {
  /* 先按一遍历史按钮（这一条**允许动手**：它观察的是一个交互，不是静态界面标记）*/
  await cdp.evaluate(`(() => {
    const b = document.getElementById('btnHistory');
    if (b) b.click();
    return !!b;
  })()`)
  await new Promise((r) => setTimeout(r, 300))
  const dump = `(() => {
    const list = document.getElementById('historyList');
    const rows = Array.from(list.querySelectorAll('li.history-item'));
    const reader = document.getElementById('reader');
    const clear = document.getElementById('btnHistoryClear');
    return {
      view: document.body.dataset.view || '(没设)',
      listHidden: !!list.hidden,
      readerBoxHidden: !!document.getElementById('readerBox').hidden,
      total: list.dataset.total || '',
      more: (list.querySelector('li.history-more') || {}).textContent || '',
      hint: (list.querySelector('li.history-hint') || {}).textContent || '',
      clearVisible: !clear.hidden,
      clearDisabled: !!clear.disabled,
      clearConfirm: clear.dataset.confirm || '',
      clearLabel: clear.textContent,
      rows: rows.map((r) => ({
        word: r.dataset.word,
        dictId: r.dataset.dictId,
        dict: r.querySelector('.history-dict').textContent,
        time: r.querySelector('.history-time').textContent,
      })),
      readerSrc: reader.getAttribute('src') || '',
      readerDictId: reader.dataset.dictId || '',
      readerOrigin: reader.dataset.origin || '',
      readerKeyText: reader.dataset.keyText || '',
    };
  })()`
  console.log(JSON.stringify(await cdp.evaluate(dump), null, 2))

  /*
   * `--history-clear`：点两下「清空」（第一下只是"再点一下"，第二下才真清）——
   * 与真实程序 gate 里那两条断言走的是同一条路。
   */
  if (has('history-clear')) {
    const click = `document.getElementById('btnHistoryClear').click();`
    await cdp.evaluate(click)
    await new Promise((r) => setTimeout(r, 200))
    const afterFirst = await cdp.evaluate(
      `(function () { var b = document.getElementById('btnHistoryClear');
        return b.dataset.confirm + '|' + b.textContent + '|' + b.disabled; })()`,
    )
    console.log('点第一下之后（confirm|文字|灰）：' + afterFirst)
    await cdp.evaluate(click)
    await new Promise((r) => setTimeout(r, 600))
    const afterSecond = `(() => {
      const list = document.getElementById('historyList');
      const b = document.getElementById('btnHistoryClear');
      return {
        total: list.dataset.total || '',
        rows: list.querySelectorAll('li.history-item').length,
        firstLine: (list.querySelector('li') || {}).textContent || '',
        confirm: b.dataset.confirm || '(已收回)',
        disabled: !!b.disabled,
      };
    })()`
    console.log('点第二下之后：')
    console.log(JSON.stringify(await cdp.evaluate(afterSecond), null, 2))
  }

  /* 要不要顺手点第一行？`--history-click` 时才点 —— 观察与动手分开，
   * 免得"只是想看看"的那一次把现场改了。 */
  if (has('history-click')) {
    const clicked = await cdp.evaluate(`(() => {
      const li = document.querySelector('#historyList li.history-item');
      if (!li) return '(没有行可点)';
      li.dispatchEvent(new MouseEvent('mousedown', { bubbles: true, cancelable: true }));
      return li.dataset.word + ' → ' + li.dataset.dictId;
    })()`)
    console.log('点下去的是：' + clicked)
    await new Promise((r) => setTimeout(r, 800))
    const after = `(() => {
      const reader = document.getElementById('reader');
      const m = /^https:\\/\\/([^.]+)\\./.exec(reader.getAttribute('src') || '');
      return {
        view: document.body.dataset.view || '(没设)',
        readerSrc: reader.getAttribute('src') || '',
        srcHost: m ? m[1] : '',
        readerDictId: reader.dataset.dictId || '',
        readerOrigin: reader.dataset.origin || '',
        readerKeyText: reader.dataset.keyText || '',
        found: reader.dataset.found || '',
        toast: (document.getElementById('readerToast') || {}).textContent || '',
      };
    })()`
    console.log(JSON.stringify(await cdp.evaluate(after), null, 2))
    console.log('↑ `srcHost` 与上面那一行的 `dictId` 相同 = 回到了**当时那本词典**（D6）')
  }
}

/*
 * ── 为什么 D8 那两条**不在这只诊断脚本里**（如实记，别下次又想加回来）──────────────
 *
 * D8 要造出两种"点不下去"的事实：
 *   · 那本**不在词库中**了 → 需要"先记一条历史、再把它移出词库"；
 *   · 在词库里但**文件不在了** → 需要"文件消失"。
 *
 * 第二种在诊断脚本里做不到：**Windows 上被内核映射过的 `.mdx` 删不掉**
 * （实测 `EBUSY: resource busy or locked` —— 查过一次就等于加载过、映射过）。
 * 第一版诊断脚本就是自己 `copyFileSync` 造一本、查一次、再 `unlinkSync` —— 它**时而成功时而 EBUSY**，
 * 而"时而"的测试比没有测试更坏（项目规范：会跟着时序一起红）。
 *
 * 所以那两条搬到 **D 级真实程序 gate**（`shell/Lookup.App/SelfCheck.cs` §⑯ 后半段）——
 * 那里的状态是**程序起来之前**由门脚本安排好的（`tools/test-shell-app.ps1` 往配置目录里
 * 写一份 `settings.json` 种子 + 一份 `history.jsonl`），既不用删文件、也不依赖时序：
 *   · "不在词库中" —— 自己发一条查词、再 `dictRemove` 掉那一本，然后点它；
 *   · "文件不在了" —— 种子词库里那本 `d-gone`（路径本来就是假的）配一条历史记录。
 */

/**
 * `--selection`：**正文里选中一段文字**，看那排浮层（查这个词 / 朗读 / 复制）。
 *
 * 为什么它必须由诊断脚本在**词条那个执行上下文**里造选区：正文在跨源 iframe 里，
 * 宿主页够不着它的 Selection —— 真实用户是"在正文里拖一段"，这里用
 * `window.getSelection().addRange(...)` 造一个**真的**选区（桥接脚本的
 * `selectionchange` 会照常上报，走的是与真人操作同一条路）。
 *
 * 用它一般是三步：① 先查一个词（`--eval` 那一步）；② `--selection --select apple`
 * 造选区看浮层；③ `--selection-click lookup|speak|copy` 真点一下看结果。
 */
if (has('selection')) {
  const ctx = { id: await entryContextId() }
  if (!ctx) {
    console.log('没有找到词条页的执行上下文（正文框里可能还没装东西）')
  } else {
    const want = arg('select', 'apple')
    /*
     * ⚠️ 先补一下**左键按下**：真实拖选就是从 `mousedown` 开始的，而桥接脚本靠它
     *    清掉「这段文字刚被用掉」的记账 —— 少了它，**重新选同一段文字浮层不会再出来**
     *    （那是参考实现定的规矩：点过「复制」之后再选一次要有反应）。
     *    第一版诊断脚本没补这一下，于是"浮层不出来"看起来像功能坏了。
     */
    await cdp.evaluate(
      `document.dispatchEvent(new MouseEvent('mousedown', { bubbles: true, cancelable: true }))`,
      ctx.id,
    )
    const made = await cdp.evaluate(
      `(() => {
        const want = ${JSON.stringify(want)};
        /* ⚠️ **跳过 script / style 里的文字**（这一段住在一个模板串里：注释里**不许**出现反引号）：
         *    词条页里带着一段桥接脚本，它的源码也是 body 里的文本节点 ——
         *    在上面造选区会得到一个**空选区**（脚本元素里的范围选不中），
         *    现场看起来像「选中了：（空）」。真人在正文里拖选也不会拖到脚本源码上。 */
        const walker = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT, {
          acceptNode: (node) => {
            const tag = node.parentNode ? node.parentNode.nodeName : '';
            return (tag === 'SCRIPT' || tag === 'STYLE')
              ? NodeFilter.FILTER_REJECT
              : NodeFilter.FILTER_ACCEPT;
          },
        });
        let node = null;
        while ((node = walker.nextNode())) {
          const at = (node.nodeValue || '').indexOf(want);
          if (at >= 0) {
            const range = document.createRange();
            range.setStart(node, at);
            range.setEnd(node, at + want.length);
            const selection = window.getSelection();
            selection.removeAllRanges();
            selection.addRange(range);
            return '选中了：' + selection.toString();
          }
        }
        return '正文里没找到这段文字：' + want;
      })()`,
      ctx.id,
    )
    console.log(made)
    /* 桥那边对 selectionchange 有 60ms 节流，等它飘过来 */
    await new Promise((r) => setTimeout(r, 400))

    const dump = `(() => {
      const bar = document.getElementById('selToolbar');
      const reader = document.getElementById('reader');
      return {
        hidden: !!bar.hidden,
        belongsTo: bar.dataset.text || '(没记)',
        leavingScrollY: bar.dataset.leavingScrollY || '',
        buttons: Array.from(bar.querySelectorAll('button')).map(
          (b) => b.dataset.action + '|' + b.title
        ),
        rect: (() => { const r = bar.getBoundingClientRect();
                       return { left: Math.round(r.left), top: Math.round(r.top),
                                width: Math.round(r.width), height: Math.round(r.height) }; })(),
        readerBox: (() => { const r = document.getElementById('readerBox').getBoundingClientRect();
                            return { left: Math.round(r.left), top: Math.round(r.top),
                                     right: Math.round(r.right), bottom: Math.round(r.bottom) }; })(),
        readerSrc: reader.getAttribute('src') || '',
        keyText: reader.dataset.keyText || '',
        reason: document.getElementById('reason').textContent,
        toast: (document.getElementById('readerToast') || {}).textContent || '',
      };
    })()`
    console.log(JSON.stringify(await cdp.evaluate(dump), null, 2))

    if (has('selection-click')) {
      const which = arg('selection-click', 'lookup')
      const clicked = await cdp.evaluate(`(() => {
        const b = document.querySelector('#selToolbar button[data-action="' + ${JSON.stringify(which)} + '"]');
        if (!b) return '(没有这个按钮：' + ${JSON.stringify(which)} + ')';
        b.dispatchEvent(new MouseEvent('mousedown', { bubbles: true, cancelable: true }));
        return '点了：' + b.title;
      })()`)
      console.log(clicked)
      await new Promise((r) => setTimeout(r, 1500))
      const after = `(() => {
        const bar = document.getElementById('selToolbar');
        const reader = document.getElementById('reader');
        return {
          barHidden: !!bar.hidden,
          barText: bar.dataset.text || '(没记)',
          readerSrc: reader.getAttribute('src') || '',
          keyText: reader.dataset.keyText || '',
          dictId: reader.dataset.dictId || '',
          origin: reader.dataset.origin || '',
          sameAsShown: reader.dataset.sameAsShown || '',
          speakSource: bar.dataset.speakSource || '',
          speakUrl: bar.dataset.speakUrl || '',
          played: bar.dataset.played || '',
          reason: document.getElementById('reason').textContent,
          toast: (document.getElementById('readerToast') || {}).textContent || '',
        };
      })()`
      console.log(JSON.stringify(await cdp.evaluate(after), null, 2))
    }
  }
}

/**
 * `--dead-clicks`：**「点了却没反应」那三条路**的现场。
 *
 * 造六个靶子塞进词条正文，逐个点一下，读宿主页里 `#readerToast` 的两个界面标记：
 *   · `nohref`    词典自己写的 `<a>` **没有 href**        → 应当弹「这个链接没有目标」
 *   · `badanchorentry://#不存在的锚点`                 → 应当弹「这一页里没有这个锚点」
 *   · `pointercursor:pointer` 的 `<span>`、没人接住  → 应当弹「这里没有可点开的内容…」
 *   · `text`      普通正文                                → **不许弹**（点正文本来就不该有事）
 *   · `handled`   脚本接住了的那个（点一下会改自己的文案）→ **不许弹**（这是词典 .js 的成功路径）
 *   · `liveentry://#真的锚点`（目标也在文档里）    → **不许弹**（锚点找得到）
 *
 * 后三个是**反向对照**：少了它们，"弹出来了"这件事证明不了"只在没反应时才弹"。
 *
 * ⚠️ 要真实程序已经起来（诊断脚本不负责启动程序）；**正文框里装着哪条词条不重要** ——
 *    这一块自己先打字 + 回车装一条（`--dead-word` 换词，默认 `apple`），
 *    理由与 `--selection` 那块同一条：上下文要在**这条连接之后**新建才拿得到。
 * ⚠️ 诊断脚本**只报实测结果、不下判断** —— 检查标准在 D 级 gate（`shell/Lookup.App/SelfCheck.cs` 第 ⑯ 节）。
 */
if (has('dead-clicks')) {
  const deadWord = arg('dead-word', 'apple')
  await cdp.evaluate(`(() => {
    const input = document.getElementById('input');
    input.value = ${JSON.stringify(deadWord)};
    input.dispatchEvent(new Event('input', { bubbles: true }));
    input.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }));
    return 'asked';
  })()`)
  await new Promise((r) => setTimeout(r, 2500))
  const ctx = { id: await entryContextId() }
  if (!ctx.id) {
    console.log('没有找到词条页的执行上下文（正文框里可能还没装东西）')
  } else {
    const inject = `(function () {
      var IDS = ['probe-tray', 'probe-nohref', 'probe-badanchor', 'probe-pointer',
                 'probe-text', 'probe-handled', 'probe-anchor-target', 'probe-live'];
      for (var i = 0; i < IDS.length; i++) {
        var old = document.getElementById(IDS[i]);
        if (old && old.parentNode) old.parentNode.removeChild(old);
      }
      var tray = document.createElement('div');
      tray.id = 'probe-tray';
      function add(tag, id, text) {
        var el = document.createElement(tag);
        el.id = id;
        el.textContent = text;
        tray.appendChild(el);
        return el;
      }
      add('a', 'probe-nohref', '没有 href 的链接');
      add('a', 'probe-badanchor', '锚点不存在').setAttribute('href', 'entry://#probe-no-such-anchor');
      add('span', 'probe-pointer', '看着可点的东西').style.cursor = 'pointer';
      add('span', 'probe-text', '普通正文');
      var handled = add('span', 'probe-handled', '脚本接住了');
      handled.style.cursor = 'pointer';
      var hits = 0;
      handled.addEventListener('click', function () {
        hits++;
        handled.textContent = '脚本接住了（已响应 ' + hits + '）';
      });
      add('div', 'probe-anchor-target', '锚点目标').style.height = '32px';
      add('a', 'probe-live', '真的锚点').setAttribute('href', 'entry://#probe-anchor-target');
      document.body.appendChild(tray);
      return '塞进去 ' + tray.children.length + ' 个靶子';
    })()`
    console.log(await cdp.evaluate(inject, ctx.id))

    const cases = ['nohref', 'badanchor', 'pointer', 'text', 'handled', 'live']
    for (const name of cases) {
      await cdp.evaluate(`(function () {
        var b = document.getElementById('readerToast');
        if (b) { b.dataset.show = 'false'; b.dataset.tone = ''; b.textContent = ''; }
        return 'cleared';
      })()`)
      const clicked = await cdp.evaluate(
        `(function () {
          var el = document.getElementById('probe-${name}');
          if (!el) return 'no-target';
          el.dispatchEvent(new MouseEvent('click', { bubbles: true, cancelable: true }));
          return 'clicked';
        })()`,
        ctx.id,
      )
      await new Promise((r) => setTimeout(r, 400))
      const read = await cdp.evaluate(`(function () {
        var b = document.getElementById('readerToast');
        return { show: (b && b.dataset.show) || '', text: (b && b.textContent) || '' };
      })()`)
      console.log(`  probe-${name.padEnd(10)} ${clicked} → ${JSON.stringify(read)}`)
    }
  }
}

/**
 * `--back`：**返回上一词条**那条路（第三十八轮）。
 *
 * 三步，每一步都能单独跑（`--back-scroll N` / `--back-link <词>` / `--back-click`）：
 *   ① 把正文滚下去一段（宿主够不着跨源 iframe 的 `scrollTop`，只能喊一声让它自己滚）；
 *   ② 在正文里**塞一条 `entry://` 链接再点它**（测试用词典的正文里没有现成的链接，
 *      与真实程序 gate §⑤b 同一个做法）—— 这一步会压一层返回栈；
 *   ③ 点「返回」——看它是不是回到原来那条、**并且落回当初读到的位置**。
 */
if (has('back')) {
  const scrollY = Number(arg('back-scroll', '0'))
  if (scrollY > 0) {
    await cdp.evaluate(`(function () {
      var f = document.getElementById('reader');
      if (f && f.contentWindow) f.contentWindow.postMessage(
        { source: 'lookup-host', type: 'scroll', delta: ${scrollY} }, '*');
      return 'asked';
    })()`)
    await new Promise((r) => setTimeout(r, 500))
  }

  const wantLink = arg('back-link', 'ran')
  const ctx = { id: await entryContextId() }
  if (!ctx) {
    console.log('没有找到词条页的执行上下文（正文框里可能还没装东西）')
  } else if (has('back-link') || has('back')) {
    const linked = await cdp.evaluate(
      `(function () {
        var want = ${JSON.stringify(wantLink)};
        var a = document.getElementById('probe-link');
        if (!a) {
          a = document.createElement('a');
          a.id = 'probe-link';
          a.href = 'entry://' + want;
          a.textContent = want;
          document.body.appendChild(a);
        }
        a.dispatchEvent(new MouseEvent('click', { bubbles: true, cancelable: true }));
        return '点了 entry://' + want + '（塞在正文末尾）';
      })()`,
      ctx.id,
    )
    console.log(linked)
    await new Promise((r) => setTimeout(r, 1500))
    const after = `(() => {
      const b = document.getElementById('btnBack');
      const reader = document.getElementById('reader');
      return {
        backHidden: !!b.hidden,
        backDepth: b.dataset.depth || '',
        readerSrc: reader.getAttribute('src') || '',
        keyText: reader.dataset.keyText || '',
        dictId: reader.dataset.dictId || '',
        origin: reader.dataset.origin || '',
        relayedScrollY: reader.dataset.relayedScrollY || '',
        pendingScrollY: reader.dataset.pendingScrollY || '',
        reason: document.getElementById('reason').textContent,
      };
    })()`
    console.log('── 压过一层之后 ──')
    console.log(JSON.stringify(await cdp.evaluate(after), null, 2))

    if (has('back-click')) {
      await cdp.evaluate("document.getElementById('btnBack').click();")
      await new Promise((r) => setTimeout(r, 2000))
      const back = `(() => {
        const b = document.getElementById('btnBack');
        const reader = document.getElementById('reader');
        return {
          backHidden: !!b.hidden,
          backDepth: b.dataset.depth || '',
          readerSrc: reader.getAttribute('src') || '',
          keyText: reader.dataset.keyText || '',
          dictId: reader.dataset.dictId || '',
          origin: reader.dataset.origin || '',
          relayedScrollY: reader.dataset.relayedScrollY || '',
          reason: document.getElementById('reason').textContent,
        };
      })()`
      console.log('── 点过「返回」之后 ──')
      console.log(JSON.stringify(await cdp.evaluate(back), null, 2))
      const entryNow = entryContext()
      if (entryNow) {
        const at = await cdp.evaluate(
          "String(Math.round(window.scrollY || document.documentElement.scrollTop || 0))",
          entryNow.id,
        )
        console.log('回到那一条之后正文实际的滚动位置：' + at +
                    '（与上面 relayedScrollY 相比 —— 位置被文档高度夹住时会更小）')
      }
    }
  }
}

/*
 * `--shot <文件>`：出一张图给人看（视觉类改动的复核；窗口在屏幕外也能出图）。
 *
 * `--shot-hover <选择器>`：截图**之前**把鼠标移到那个元素中心 —— 为的是能拍到 `:hover` 那一种情况。
 * 为什么需要它：这一版的按钮改动要求"静止态看得出是按钮、**而且不许削弱悬浮态**"，
 * 两种都得有图才能交人确认（`docs/design/图标设计与改进指导.md`：出图交人看，不可省略）。
 * `--shot-scale N`：整页放大 N 倍再拍（按钮只有 30px 高，图上要看得清）。
 *   ⚠️ **隐藏窗口（`--no-show`）下 `--shot-scale` 会挂住**：`Emulation.setDeviceMetricsOverride`
 *   之后 `Page.captureScreenshot` 一直不返回（2026-09 实测，等 25s 也没回来）。
 *   不加 `--shot-scale` 就能正常出图 —— 要在隐藏窗口里放大出图，就改用
 *   `--eval-file` 现量一遍几何 / 或者把窗口显示出来再拍。
 */
const shot = arg('shot', null)
if (shot) {
  const hoverSel = arg('shot-hover', null)
  const scale = Number(arg('shot-scale', '1')) || 1
  if (scale > 1) {
    await cdp.send('Emulation.setDeviceMetricsOverride', {
      width: 900,
      height: 700,
      deviceScaleFactor: scale,
      mobile: false,
    })
  }
  if (hoverSel) await hoverTo(hoverSel)
  const result = await cdp.send('Page.captureScreenshot', { format: 'png' })
  const file = path.resolve(shot)
  writeFileSync(file, Buffer.from(result.data, 'base64'))
  console.log('截图写到 ' + file + (hoverSel ? '（鼠标停在 ' + hoverSel + ' 上）' : ''))
}

cdp.close()
