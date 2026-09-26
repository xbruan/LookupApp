import type { TrayMenuApi } from '../../shared/types'
import { iconSvg, type IconName } from '../floating/icons'

const api: TrayMenuApi = window.dshLookup.tray

interface Entry {
  label: string
  icon: IconName
  /** 勾选项：用一个自家的小方块表示，避免系统菜单那种额外留白 */
  checkbox?: boolean
  checked?: boolean
  run: () => void
}

/**
 * 在这些条目之前插入分隔线（按 build 之后的元素下标数）。
 * ⚠️ 这是**按下标**写的，菜单增删一项就要跟着重算；`[1, 3]` 把菜单分成三组：
 * 悬浮窗（一键动作）｜设置类（选项 / 重置位置）｜程序类（开机启动 / 退出）。
 */
const SEPARATORS = [1, 3]

const menuEl = document.getElementById('menu')
if (!menuEl) throw new Error('缺少 #menu')
const menu: HTMLElement = menuEl

function build(entries: Entry[]): void {
  menu.textContent = ''
  entries.forEach((entry) => {
    const item = document.createElement('button')
    item.type = 'button'
    item.className = 'menu-item'
    item.dataset.checked = String(!!entry.checked)

    if (entry.checkbox) {
      const box = document.createElement('span')
      box.className = 'menu-check'
      box.innerHTML = iconSvg('check', 11)
      item.append(box)
    } else {
      item.innerHTML = iconSvg(entry.icon)
    }

    const label = document.createElement('span')
    label.textContent = entry.label
    item.append(label)

    item.addEventListener('mousedown', (event) => {
      event.preventDefault()
      if (entry.checkbox) {
        // 勾选项不关菜单：让用户当场看到勾选状态有没有真的生效
        entry.run()
        return
      }
      /*
       * 先发"关闭"、再发动作：两个 IPC 按顺序到达宿主，菜单会先收起来，动作里再弹窗也不会被压住。
       * ⚠️ 这里**千万不要**用 setTimeout 延后动作 —— 窗口一隐藏，Chromium 会把定时器降频到大约
       * 一秒一次，动作会时快时慢甚至看起来没反应。
       */
      api.close()
      entry.run()
    })
    if (entry.checkbox) item.dataset.role = 'login'
    menu.append(item)
  })
}

/** 从后往前插分隔线，避免下标错位 */
function insertSeparators(indices: number[]): void {
  for (const index of [...indices].sort((a, b) => b - a)) {
    const children = [...menu.children] as HTMLElement[]
    if (index <= 0 || index >= children.length) continue
    const line = document.createElement('div')
    line.className = 'menu-sep'
    menu.insertBefore(line, children[index])
  }
}

let lastReported = {
  width: 0,
  height: 0,
  fill: '',
  border: '',
  theme: ''
}
/**
 * 开机自启动的当前状态。必须用可变变量：菜单项回调若直接捕获 open() 里的局部常量，
 * 第一次切换之后闭包里的值就过期了，再点一次会算成同样的结果 —— 表现就是"怎么点都没反应"。
 */
let loginState = false

/** 外壳层是分层窗口，卡片底色必须是实心的，否则会透出投影和桌面 */
function parseRgba(value: string): { r: number; g: number; b: number; a: number } | null {
  const match = /rgba?\(\s*([\d.]+)\s*,\s*([\d.]+)\s*,\s*([\d.]+)\s*(?:,\s*([\d.]+)\s*)?\)/.exec(value || '')
  if (!match) return null
  return {
    r: Number(match[1]),
    g: Number(match[2]),
    b: Number(match[3]),
    a: match[4] === undefined ? 1 : Number(match[4])
  }
}

function opaqueColor(value: string, fallback: string): string {
  const c = parseRgba(value)
  if (!c) return fallback
  return `rgb(${Math.round(c.r)}, ${Math.round(c.g)}, ${Math.round(c.b)})`
}

/**
 * 描边色要先按透明度合成到卡片底色上再报过去：外壳层是逐像素透明的分层窗口，
 * 它只会把描边**填**在卡片边缘，没法让半透明的描边去和底色混合（那底下是投影和桌面）。
 * 直接丢掉 alpha 的话，`rgba(15,20,40,.1)` 这条淡淡的描边会变成近乎纯黑，看着就是一圈深灰硬边。
 */
function compositeBorder(borderColor: string, fill: { r: number; g: number; b: number }, fallback: string): string {
  const c = parseRgba(borderColor)
  if (!c) return fallback
  const a = Math.max(0, Math.min(1, c.a))
  return `rgb(${Math.round(c.r * a + fill.r * (1 - a))}, ${Math.round(c.g * a + fill.g * (1 - a))}, ${Math.round(
    c.b * a + fill.b * (1 - a)
  )})`
}

/** 量出自然尺寸回报给主进程，由它把窗口裁到刚好这么大 */
function reportSize(): void {
  const rect = menu.getBoundingClientRect()
  const style = getComputedStyle(menu)
  const fill = parseRgba(style.backgroundColor) ?? { r: 247, g: 248, b: 252, a: 1 }
  const fillCss = `rgb(${Math.round(fill.r)}, ${Math.round(fill.g)}, ${Math.round(fill.b)})`
  // 没边框就报底色，外壳画出来等于没有描边（别把继承来的文字色当成描边）
  const hasBorder = Number.parseFloat(style.borderTopWidth) > 0
  const size = {
    /*
     * 尺寸用 offsetWidth/offsetHeight（布局尺寸），不用 getBoundingClientRect()：弹出动画带着位移，
     * rect 会跟着动；布局尺寸则不受 transform 影响，量出来的窗口大小在任何时刻都是准的。
     */
    width: menu.offsetWidth,
    height: menu.offsetHeight,
    // 顺带把配色报上去：主进程要用它在外面画抗锯齿的圆角和投影
    fill: fillCss,
    border: hasBorder ? compositeBorder(style.borderTopColor, fill, fillCss) : fillCss,
    theme:
      window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light'
  }
  if (
    size.width === lastReported.width &&
    size.height === lastReported.height &&
    size.fill === lastReported.fill &&
    size.border === lastReported.border &&
    size.theme === lastReported.theme
  ) {
    return
  }
  lastReported = size
  api.reportSize(size)
}

async function open(): Promise<void> {
  // 每次展开都重新量，先清掉上次的记录
  lastReported = { width: 0, height: 0, fill: '', border: '', theme: '' }
  const state = await api.getState()
  loginState = state.loginAtStartup
  build([
    {
      label: state.floatingVisible ? '隐藏悬浮窗' : '显示悬浮窗',
      /*
       * 分工：**图标画的是"点下去会变成什么样"** —— 写「隐藏悬浮窗」时用划掉的眼睛（eyeOff）、
       * 写「显示悬浮窗」时用睁着的眼睛（eye），别反过来。
       */
      icon: state.floatingVisible ? 'eyeOff' : 'eye',
      run: () => api.toggleFloating()
    },
    // 与悬浮窗那个小图标菜单同一套说法：选项 = 齿轮（设置）
    /*
     * ⚠️ 打开时落在**词库页**（第一个页签）：它是唯一的**内容**页，也是唯一会"空"的页。
     */
    { label: '选项', icon: 'gear', run: () => api.openManager('dicts') },
    { label: '重置悬浮窗位置', icon: 'arrowRight', run: () => api.resetPosition() },
    /*
     * ★ 托盘菜单里**没有**「清空查词历史」：它已挪进「选项 → 常规 → 数据」（托盘是"顺手点一下"的地方，
     * 而清空历史**不可撤销**）。⚠️ 桥方法 `tray.clearHistory` **留着没删** —— 自检与诊断还从悬浮窗
     * 那一页调它，验的是"内核那条清空历史的路通不通"，与哪个按钮无关。
     */
    {
      label: '开机自动启动',
      icon: 'check',
      checkbox: true,
      checked: loginState,
      run: () => api.setLoginAtStartup(!loginState)
    },
    { label: '退出', icon: 'power', run: () => api.quit() }
  ])
  insertSeparators(SEPARATORS)

  // 量出自然尺寸回报给主进程，由它把窗口裁到刚好这么大
  reportSize()
  // 窗口尺寸变化 / 字体取整稳定之后再校一次，避免差一两像素把边缘裁掉
  requestAnimationFrame(() => requestAnimationFrame(reportSize))
  /*
   * 中文字体换进来之后每项会宽一两像素（首次测量常常量到换字体之前的窄版本），那时窗口已经按旧尺寸
   * 显示出来了，菜单会被挤到一边、投影也左右不对称。字体就绪后再补报一次，宿主按新尺寸重排。
   */
  void document.fonts.ready.then(() => reportSize())
}

api.onOpen(() => {
  void open()
})

// 勾选后主进程回报真实状态，就地更新勾选标记与本地状态
api.onLoginChanged((enabled) => {
  loginState = enabled
  const item = menu.querySelector<HTMLElement>('.menu-item[data-role="login"]')
  if (item) item.dataset.checked = String(enabled)
})
