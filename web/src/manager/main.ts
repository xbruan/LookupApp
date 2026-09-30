import type {
  CloseBehavior,
  DictionaryInfo,
  DoubaoTest,
  FloatingApi,
  ManagerApi,
  OptionsTab,
  SpeakResult,
  SpeechOption,
  SpeechSettings,
  SpeechStatus,
  TranslateSettings,
  TranslateStatus,
  TranslateTargetMode,
  TranslateTest,
  TrayMenuApi
} from '../../shared/types'
import { SpeechAudioRoute, speechGainDb, withRouteNote } from '../shared/audioGraph'
import type { SpeechRouteResult } from '../shared/audioGraph'
import { appMarkSvg, iconSvg } from '../floating/icons'

const api: ManagerApi = window.dshLookup.manager
const floatingApi: FloatingApi = window.dshLookup.floating
/* 托盘那一份 API：常规页的「开机自动启动」与托盘菜单共用同一个设置（traymenu:login） */
const trayApi: TrayMenuApi = window.dshLookup.tray

function el<T extends HTMLElement>(id: string): T {
  const node = document.getElementById(id)
  if (!node) throw new Error(`缺少 DOM 节点 #${id}`)
  return node as T
}

const dom = {
  titlebar: el<HTMLElement>('titlebar'),
  titlebarMark: el<HTMLDivElement>('titlebarMark'),
  /* 加词典只由这个词库页入口承担（标题栏那个加号已删，别加回来） */
  btnAddLarge: el<HTMLButtonElement>('btnAddLarge'),
  btnMinimize: el<HTMLButtonElement>('btnMinimize'),
  btnClose: el<HTMLButtonElement>('btnClose'),
  dictList: el<HTMLUListElement>('dictList'),
  emptyState: el<HTMLDivElement>('emptyState'),
  emptyIcon: el<HTMLDivElement>('emptyIcon'),
  closeBehavior: el<HTMLSelectElement>('closeBehavior'),
  loginAtStartup: el<HTMLInputElement>('loginAtStartup'),
  showOnStartup: el<HTMLInputElement>('showOnStartup'),
  openConfigDir: el<HTMLButtonElement>('openConfigDir'),
  configDirNote: el<HTMLParagraphElement>('configDirNote'),
  /*
   * 「清空查词历史」（从托盘菜单搬来）：平时只露这一颗，点一下换成「确认清空 / 取消」。
   * 为什么必须有二次确认：清空历史**不可撤销**，而这一页满是按钮、离"手滑"更近。
   */
  clearHistory: el<HTMLButtonElement>('clearHistory'),
  clearHistoryConfirm: el<HTMLButtonElement>('clearHistoryConfirm'),
  clearHistoryCancel: el<HTMLButtonElement>('clearHistoryCancel'),
  toast: el<HTMLDivElement>('toast'),
  /* ---- 页签 ---- */
  tabs: el<HTMLElement>('tabs'),
  tabDicts: el<HTMLButtonElement>('tabDicts'),
  tabSpeech: el<HTMLButtonElement>('tabSpeech'),
  tabTranslate: el<HTMLButtonElement>('tabTranslate'),
  tabGeneral: el<HTMLButtonElement>('tabGeneral'),
  paneGeneral: el<HTMLElement>('paneGeneral'),
  paneDicts: el<HTMLElement>('paneDicts'),
  paneSpeech: el<HTMLElement>('paneSpeech'),
  paneTranslate: el<HTMLElement>('paneTranslate'),
  /* ---- 翻译设置页 ---- */
  translateMissing: el<HTMLParagraphElement>('translateMissing'),
  translateJumpToSpeech: el<HTMLButtonElement>('translateJumpToSpeech'),
  translateEnabled: el<HTMLInputElement>('translateEnabled'),
  translateTarget: el<HTMLSelectElement>('translateTarget'),
  translateAuto: el<HTMLInputElement>('translateAuto'),
  translateTest: el<HTMLButtonElement>('translateTest'),
  translateTestSpeech: el<HTMLSpanElement>('translateTestSpeech'),
  translateTestResult: el<HTMLParagraphElement>('translateTestResult'),
  translateCacheNote: el<HTMLParagraphElement>('translateCacheNote'),
  translateClearCache: el<HTMLButtonElement>('translateClearCache'),
  /* ---- 语音设置页 ---- */
  speechSummary: el<HTMLParagraphElement>('speechSummary'),
  speechBody: el<HTMLDivElement>('speechBody'),
  speechVoice: el<HTMLSelectElement>('speechVoice'),
  speechRate: el<HTMLInputElement>('speechRate'),
  speechRateValue: el<HTMLSpanElement>('speechRateValue'),
  /* 语速行尾的「重置」：拨回默认 0。它与四个「试听」共用 .field-action —— 同样两个字、同样宽度，四行按钮才落在同一列上 */
  speechRateReset: el<HTMLButtonElement>('speechRateReset'),
  speechAccent: el<HTMLSelectElement>('speechAccent'),
  /* 「默认语种」那一格已删（语种由程序按词判，不给用户选）；协议里的 speech:status.languages 清单留着别删，见 renderSpeech */
  speechEngineNote: el<HTMLParagraphElement>('speechEngineNote'),
  /* ---- 豆包（在线）表单 ---- */
  doubaoApiKey: el<HTMLInputElement>('doubaoApiKey'),
  doubaoStatus: el<HTMLSpanElement>('doubaoStatus'),
  doubaoResourceId: el<HTMLInputElement>('doubaoResourceId'),
  doubaoSpeakerEn: el<HTMLInputElement>('doubaoSpeakerEn'),
  doubaoLoudnessEn: el<HTMLInputElement>('doubaoLoudnessEn'),
  doubaoLoudnessEnValue: el<HTMLSpanElement>('doubaoLoudnessEnValue'),
  doubaoSpeakerZh: el<HTMLInputElement>('doubaoSpeakerZh'),
  doubaoLoudnessZh: el<HTMLInputElement>('doubaoLoudnessZh'),
  doubaoLoudnessZhValue: el<HTMLSpanElement>('doubaoLoudnessZhValue'),
  doubaoBalance: el<HTMLButtonElement>('doubaoBalance'),
  doubaoBalanceResult: el<HTMLSpanElement>('doubaoBalanceResult'),
  doubaoApply: el<HTMLButtonElement>('doubaoApply'),
  /*
   * 按钮下面那一列状态的第 1 行：保存的结果。与 `doubaoBalanceResult` 是**两个独立的格**、上下叠着 ——
   * 谁先有话说谁就出现在第一行，没话说那个整格 `hidden`（不留空行），所以"没有保存情况时
   * 「已平衡」落到第一行"是布局自己算出来的，不需要两套位置（见 .speech-status-stack 的注释）。
   */
  doubaoApplyResult: el<HTMLSpanElement>('doubaoApplyResult'),
  /* ---- 音量对齐（内置录音 / 系统语音的增益） ---- */
  dictGainLabel: el<HTMLLabelElement>('dictGainLabel'),
  dictGain: el<HTMLInputElement>('dictGain'),
  dictGainValue: el<HTMLSpanElement>('dictGainValue'),
  dictGainNote: el<HTMLSpanElement>('dictGainNote'),
  /*
   * 两行行尾各自的「试听」：按**这一行滑块当前的值**放一句（内置录音放原录音、系统语音现场合成）。
   * 走的是和"拖完滑块松手"完全同一条路（见 changeGain）：增益是后端按**设置里**那个值给的，
   * 滑块上还没落盘的位置后端不知道，所以试听前必须先把它写下去。
   */
  dictGainPreview: el<HTMLButtonElement>('dictGainPreview'),
  systemGain: el<HTMLInputElement>('systemGain'),
  systemGainValue: el<HTMLSpanElement>('systemGainValue'),
  systemGainNote: el<HTMLSpanElement>('systemGainNote'),
  systemGainPreview: el<HTMLButtonElement>('systemGainPreview'),
  /* 两个「试听」：各跟着自己那一格的音色（原来那个单独的「试听」念的是按默认语种挑的词，跟这两格没有对应关系，已删） */
  voicePreviewEn: el<HTMLButtonElement>('voicePreviewEn'),
  voicePreviewZh: el<HTMLButtonElement>('voicePreviewZh'),
  speechClearCache: el<HTMLButtonElement>('speechClearCache'),
  speechCacheNote: el<HTMLParagraphElement>('speechCacheNote')
}

let dictionaries: DictionaryInfo[] = []
/**
 * 当前词典的 id（只用来判断"换没换词典"）。
 * 换词典要连语音页一起重画，而这个判断得有上一个值可比（见 `onDictionariesChanged`）。
 */
let currentDictionaryId = ''
let toastTimer: number | null = null
/** 正在等待二次确认删除的词典 id */
let pendingRemovalId: string | null = null
/**
 * 正在改名的词典 id。改名是"就地编辑"：标题换成输入框，回车提交 / Esc 取消 / 失焦提交 ——
 * 比弹系统对话框更贴近界面风格（删除的二次确认也是就地展开的）。
 */
let renamingId: string | null = null
/**
 * 词库里**哪几本的详情是摊开的**：摊开一本不许把别的收起来（记的是 id 集合，不是下标 ——
 * 所以排序之后展开态跟着那本词典走）。跨重画保留（`render()` 会被一堆事件调用）。
 * 某一本被移除后它的 id 留在集合里没有消费者，不必专门清理。
 */
const expandedIds = new Set<string>()

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

/**
 * 描边色要先按透明度合成到界面底色上再报过去 —— 外壳层没法让半透明描边和底色混合
 * （那底下是投影和桌面）。直接丢掉 alpha 的话，`rgba(15,20,40,.1)` 会变成近乎纯黑，看起来就是一圈深灰硬边。
 */
function compositeBorder(borderColor: string, fill: { r: number; g: number; b: number }, fallback: string): string {
  const c = parseRgba(borderColor)
  if (!c) return fallback
  const a = Math.max(0, Math.min(1, c.a))
  return `rgb(${Math.round(c.r * a + fill.r * (1 - a))}, ${Math.round(c.g * a + fill.g * (1 - a))}, ${Math.round(
    c.b * a + fill.b * (1 - a)
  )})`
}

/** 上一次上报过的焦点状态，用来避免重复上报 */
let lastFocused: boolean | null = null

/** 把整块界面的底色/描边/焦点状态报给主进程，它在窗口外面画抗锯齿圆角和投影 */
function reportSurface(): void {
  const shell = document.querySelector('.shell') as HTMLElement | null
  const style = getComputedStyle(shell ?? document.body)
  const fill = parseRgba(style.backgroundColor) ?? { r: 255, g: 255, b: 255, a: 1 }
  const fillCss = `rgb(${Math.round(fill.r)}, ${Math.round(fill.g)}, ${Math.round(fill.b)})`
  /*
   * 只有元素真的有边框时才报描边色：`.shell` 没有 border 时 `borderTopColor` 会回退成继承来的文字色
   * （近乎纯黑），照着画就是一圈突兀的深灰硬边 —— 管理窗本来是不带边框的设计。
   */
  const hasBorder = Number.parseFloat(style.borderTopWidth) > 0
  const focused = document.hasFocus()
  lastFocused = focused
  api.reportSurface({
    fill: fillCss,
    border: hasBorder ? compositeBorder(style.borderTopColor, fill, fillCss) : fillCss,
    theme:
      window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light',
    // 焦点状态由页面说了算：主进程那套 WinForms 的 Activated/Deactivate 实测不可靠
    focused
  })
}

/**
 * 盯着焦点变化重新上报。光靠 focus/blur 事件不够：模块脚本是延迟执行的，页面很可能在脚本跑起来
 * 之前就已经拿到焦点了，那次事件根本没人听（表现是"窗口明明有焦点，投影却是淡的"）——
 * 所以再加一个低频复查兜底，只在状态真的变了才上报。
 */
function watchFocus(): void {
  window.addEventListener('focus', reportSurface)
  window.addEventListener('blur', () => window.setTimeout(reportSurface, 0))
  window.setInterval(() => {
    if (document.hasFocus() === lastFocused) return
    reportSurface()
  }, 300)
}

function toast(message: string, tone: 'info' | 'error' = 'info'): void {
  dom.toast.textContent = message
  dom.toast.dataset.tone = tone
  dom.toast.dataset.show = 'true'
  if (toastTimer !== null) window.clearTimeout(toastTimer)
  toastTimer = window.setTimeout(() => {
    dom.toast.dataset.show = 'false'
    toastTimer = null
  }, 2200)
}

function formatSize(bytes: number): string {
  if (!bytes) return '—'
  const units = ['B', 'KB', 'MB', 'GB']
  let value = bytes
  let unit = 0
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024
    unit++
  }
  return `${value >= 100 || unit === 0 ? Math.round(value) : value.toFixed(1)} ${units[unit]}`
}

function statusBadge(dict: DictionaryInfo): HTMLElement | null {
  if (dict.status === 'ready') return null
  const badge = document.createElement('span')
  badge.className = 'badge'
  if (dict.status === 'loading' || dict.status === 'pending') {
    badge.dataset.tone = 'warn'
    badge.textContent = '载入中'
  } else {
    badge.dataset.tone = 'danger'
    badge.textContent = '载入失败'
    if (dict.errorMessage) badge.title = dict.errorMessage
  }
  return badge
}

function render(): void {
  dom.dictList.textContent = ''
  const empty = dictionaries.length === 0
  dom.emptyState.hidden = !empty
  dom.dictList.hidden = empty

  dictionaries.forEach((dict) => {
    const li = document.createElement('li')
    li.className = 'dict-item'
    li.dataset.current = String(isCurrent(dict))
    li.dataset.id = dict.id

    /*
     * 这一行是摊开的还是收起的 —— 两条"必须摊开"的情况，别漏：
     *   · 用户点开了它（`expandedIds`，各本互不影响）；
     *   · 它正等着二次确认移除（`pendingRemovalId`）—— 那两个按钮就长在详情里，
     *     收着的话用户点了「移除」会**什么都不发生**（按钮在隐藏区里）。
     */
    const open = expandedIds.has(dict.id) || pendingRemovalId === dict.id
    li.dataset.open = String(open)

    const radio = document.createElement('div')
    radio.className = 'dict-radio'
    radio.title = '设为当前词典'
    radio.addEventListener('click', () => void setCurrent(dict.id))

    /*
     * 收起时那一行：**名字（或改名输入框）+ 状态徽标 + 词条数**。只留「词条」一个数 ——
     * 收起行回答的是"这是哪本"，"占多少磁盘"（与资源库/编码/版本同类）查问题时才看，所以进详情。
     * 「载入中 / 载入失败」仍留在收起行：那是"现在要你看一眼"的事，藏起来等于不说。
     */
    const line = document.createElement('div')
    line.className = 'dict-line'

    if (renamingId === dict.id) {
      line.append(renameEditor(dict))
    } else {
      const name = document.createElement('span')
      name.className = 'dict-name'
      name.textContent = dict.title
      name.title = `${dict.title}（双击可改名）`
      // 双击标题也能进入改名，省得每次都去够那个铅笔按钮
      name.addEventListener('dblclick', () => {
        renamingId = dict.id
        render()
      })
      line.append(name)
      const status = statusBadge(dict)
      if (status) line.append(status)

      const brief = document.createElement('span')
      brief.className = 'dict-brief'
      brief.textContent = metaOf(dict)
        .slice(0, 1)
        .map(([label, value]) => `${label} ${value}`)
        .join(' · ')
      line.append(brief)
    }

    /*
     * ★ 收起态也有「移除」按钮：它以前只长在详情里，于是"想删一本"必须先点开那一行，多一步。
     * **详情里不再重复放一个**：同一个动作在同一屏幕上放两遍，只会让人怀疑这两个按钮是不是不一样。
     */
    const removeBtn = document.createElement('button')
    removeBtn.type = 'button'
    removeBtn.className = 'icon-btn dict-remove'
    removeBtn.dataset.tone = 'danger'
    removeBtn.dataset.size = 'sm'
    removeBtn.title = '从词库中移除（不会删除硬盘上的文件）'
    removeBtn.innerHTML = iconSvg('trash')
    removeBtn.addEventListener('click', () => {
      pendingRemovalId = dict.id
      render()
    })

    const toggle = document.createElement('button')
    toggle.type = 'button'
    toggle.className = 'dict-toggle'
    toggle.title = open ? '收起详情' : '展开详情（路径 / 大小 / 资源库 / 编码 / 版本 / 改名 / 排序）'
    toggle.setAttribute('aria-expanded', String(open))
    toggle.innerHTML = iconSvg('chevronDown', 14)
    toggle.addEventListener('click', () => {
      if (open) expandedIds.delete(dict.id)
      else expandedIds.add(dict.id)
      render()
    })

    const detail = document.createElement('div')
    detail.className = 'dict-detail'
    const path = document.createElement('div')
    path.className = 'dict-path'
    path.textContent = dict.mdxPath
    // 详情里路径也**只占一行**（超出省略），完整路径挂在 title 上，悬停就能看全
    path.title = dict.mdxPath
    const rest = document.createElement('div')
    rest.className = 'dict-rest'
    rest.textContent = metaOf(dict)
      .slice(1)
      .map(([label, value]) => `${label} ${value}`)
      .join(' · ')
    detail.append(path, rest)

    const actions = document.createElement('div')
    actions.className = 'dict-actions'

    if (pendingRemovalId === dict.id) {
      // 二次确认直接就地展开，避免弹系统对话框破坏视觉一致性
      const confirm = document.createElement('button')
      confirm.type = 'button'
      confirm.className = 'ghost-btn danger'
      confirm.textContent = '确认移除'
      confirm.addEventListener('click', () => void removeDictionary(dict))

      const cancel = document.createElement('button')
      cancel.type = 'button'
      cancel.className = 'ghost-btn'
      cancel.textContent = '取消'
      cancel.addEventListener('click', () => {
        pendingRemovalId = null
        render()
      })

      actions.append(confirm, cancel)
    } else if (renamingId === dict.id) {
      // 改名中：动作区留空，免得手滑点到别的（确认/取消在输入框旁边）
    } else {
      const renameBtn = document.createElement('button')
      renameBtn.type = 'button'
      renameBtn.className = 'icon-btn'
      renameBtn.dataset.size = 'sm'
      renameBtn.title = '重命名词典（只改显示名，不动文件）'
      renameBtn.innerHTML = iconSvg('pencil')
      renameBtn.addEventListener('click', () => {
        renamingId = dict.id
        render()
      })

      const openBtn = document.createElement('button')
      openBtn.type = 'button'
      openBtn.className = 'icon-btn'
      openBtn.dataset.size = 'sm'
      openBtn.title = '在资源管理器中显示'
      openBtn.innerHTML = iconSvg('folder')
      openBtn.addEventListener('click', () => api.openInExplorer(dict.mdxPath))

      /*
       * 「上移 / 下移」：把这一本在词库里往前/往后挪一位。放在**详情**里而不是收起行上 ——
       * 收起行要保持干净（已经有单选点、名字、词条数、移除、展开箭头五样），而排序是偶尔做一次的事。
       * 头尾各置灰一颗：那一位再挪也是原地，"看得见它不能点"比"点了没反应"清楚。
       * ⚠️ 移动之后**这一行仍然是摊开的**：展开态记的是 id 不是下标（见 expandedIds）。
       */
      const index = dictionaries.indexOf(dict)
      const upBtn = moveButton('上移', 'arrowUp', index <= 0, () => moveDictionary(dict, -1))
      const downBtn = moveButton('下移', 'arrowDown', index >= dictionaries.length - 1, () =>
        moveDictionary(dict, 1)
      )

      actions.append(upBtn, downBtn, renameBtn, openBtn)
    }

    detail.append(actions)
    li.append(radio, line, removeBtn, toggle, detail)
    dom.dictList.append(li)
  })
}

/** 排序那一颗按钮（上移 / 下移）——两处只有方向与禁用条件不同 */
function moveButton(
  label: string,
  icon: 'arrowUp' | 'arrowDown',
  disabled: boolean,
  run: () => void
): HTMLButtonElement {
  const btn = document.createElement('button')
  btn.type = 'button'
  btn.className = 'icon-btn'
  btn.dataset.size = 'sm'
  btn.disabled = disabled
  btn.title = disabled ? `${label}（已经在头/尾了）` : `把这一本${label}一位`
  btn.innerHTML = iconSvg(icon, 15)
  btn.addEventListener('click', run)
  return btn
}

/**
 * 一本词典那五项元信息（标签 + 已经格式化好的值）—— **一处算、两处用**：收起那一行取第 1 项（词条），
 * 详情里取后四项（大小 / 资源库 / 编码 / 版本）。⚠️ 要调整哪几项留在表面，改的是两处 `slice()`，
 * 别去动这个函数的顺序。
 */
function metaOf(dict: DictionaryInfo): Array<[string, string]> {
  return [
    ['词条', dict.entryCount ? dict.entryCount.toLocaleString('zh-CN') : '—'],
    ['大小', formatSize(dict.fileSize)],
    ['资源库', dict.mddPaths.length ? `${dict.mddPaths.length} 个` : '无'],
    ['编码', dict.encoding || '—'],
    ['版本', dict.version || '—'],
  ]
}

function isCurrent(dict: DictionaryInfo): boolean {
  return dict.current
}

/**
 * 就地的改名输入框：回车确认 / Esc 取消 / 失焦确认（点到别处就当确认，和大多数列表的改名习惯一致）。
 * 两个小按钮用 `mousedown` + `preventDefault` 而不是 `click`：不然点"取消"会先让输入框失焦、
 * 触发提交，再轮到 click —— 取消就变成了确认。
 */
function renameEditor(dict: DictionaryInfo): HTMLElement {
  const wrap = document.createElement('span')
  wrap.className = 'rename-row'

  const input = document.createElement('input')
  input.className = 'rename-input'
  input.type = 'text'
  input.value = dict.title
  input.maxLength = 48
  input.spellcheck = false
  input.title = '回车确认，Esc 取消；清空后确认 = 恢复默认名（词典自带标题 / 文件名）'

  const confirm = document.createElement('button')
  confirm.type = 'button'
  confirm.className = 'icon-btn'
  confirm.dataset.size = 'sm'
  confirm.title = '确认改名'
  confirm.innerHTML = iconSvg('check', 15)

  const cancel = document.createElement('button')
  cancel.type = 'button'
  cancel.className = 'icon-btn'
  cancel.dataset.size = 'sm'
  cancel.title = '取消'
  cancel.innerHTML = iconSvg('close', 15)

  let settled = false
  const finish = (commit: boolean): void => {
    if (settled) return
    settled = true
    renamingId = null
    if (commit) void renameDictionary(dict, input.value)
    else render()
  }

  // mousedown 而不是 click：见函数头的说明
  confirm.addEventListener('mousedown', (event) => {
    event.preventDefault()
    finish(true)
  })
  cancel.addEventListener('mousedown', (event) => {
    event.preventDefault()
    finish(false)
  })
  input.addEventListener('keydown', (event) => {
    if (event.key === 'Enter') {
      event.preventDefault()
      finish(true)
    } else if (event.key === 'Escape') {
      event.preventDefault()
      finish(false)
    }
  })
  input.addEventListener('blur', () => finish(true))

  wrap.append(input, confirm, cancel)
  // 渲染完就聚焦并全选，弹出来就能直接敲新名字
  window.setTimeout(() => {
    input.focus()
    input.select()
  }, 0)
  return wrap
}

async function refresh(): Promise<void> {
  dictionaries = await api.listDictionaries()
  render()
}

/**
 * 提交改名。主进程返回刷新后的列表（标题已经变了），顺手还会广播给悬浮窗 ——
 * 悬浮窗胶囊右侧那行提示显示的就是当前词典的名字，改完立刻跟着变。
 */
async function renameDictionary(dict: DictionaryInfo, rawTitle: string): Promise<void> {
  const title = rawTitle.trim()
  if (title === dict.title.trim()) {
    render()
    return
  }

  try {
    dictionaries = await api.renameDictionary(dict.id, title)
  } catch (err) {
    console.error('改名失败', err)
    render()
    toast('改名失败，请再试一次', 'error')
    return
  }

  render()
  const renamed = dictionaries.find((d) => d.id === dict.id)
  const shown = renamed ? renamed.title : title
  toast(
    title.length === 0
      ? `已恢复默认名《${shown}》`
      : `已改名为《${shown}》，相关显示已同步`
  )
}

async function setCurrent(id: string): Promise<void> {
  try {
    dictionaries = await api.setCurrentDictionary(id)
  } catch (err) {
    /* 换不动就如实说：IPC 失败无声地吞掉，用户只会觉得"点了没反应" */
    toast(`切换词典失败：${err instanceof Error ? err.message : String(err)}`, 'error')
    return
  }
  render()
  /*
   * ★ 切换之后**要给一句提示**。为什么需要：换了当前词典之后，页面上唯一的变化就是"那个填实的
   * 小圆点挪了位置"加上名字变粗 —— 而**别的页**（语音页的增益、翻译页）跟着换了一整套设置，
   * 悬浮窗上那行词典名也换了。没有一句话，用户很容易以为"我点的那下是不是没生效"。
   * 名字用**刚从内核拿回来的那一份**（`dictionaries`），那才是权威值。
   */
  const now = dictionaries.find((d) => d.id === id)
  toast(now ? `当前词典：《${now.title}》` : '已切换当前词典')
}

/**
 * 词典排序：把这一本往前/往后挪一位（内核 ABI 的 `dict:move`）。
 * 失败要如实说（与改名/移除同一条：不许不报错地失败）；成功**不弹提示** ——
 * 那一行当场就换位置了，再弹一句反而是噪声。
 */
async function moveDictionary(dict: DictionaryInfo, delta: number): Promise<void> {
  try {
    dictionaries = await api.moveDictionary(dict.id, delta)
  } catch (err) {
    toast(`移动失败：${err instanceof Error ? err.message : String(err)}`, 'error')
    return
  }
  render()
}

async function removeDictionary(dict: DictionaryInfo): Promise<void> {
  pendingRemovalId = null
  try {
    dictionaries = await api.removeDictionary(dict.id)
  } catch (err) {
    toast(`移除失败：${err instanceof Error ? err.message : String(err)}`, 'error')
    render()
    return
  }
  render()
  toast(`已移除《${dict.title}》，硬盘上的文件未受影响`)
}

async function addDictionaries(): Promise<void> {
  let result
  try {
    result = await api.addDictionaryFiles()
  } catch (err) {
    toast(`导入失败：${err instanceof Error ? err.message : String(err)}`, 'error')
    return
  }
  await refresh()
  const parts: string[] = []
  if (result.added.length) parts.push(`已添加 ${result.added.length} 本词典`)
  if (result.updated.length) parts.push(`已更新 ${result.updated.length} 本`)
  if (result.orphans.length) parts.push(`${result.orphans.length} 个文件无法识别：${result.orphans.join('、')}`)
  toast(parts.length ? parts.join('；') : '没有新的词典被添加', result.orphans.length ? 'error' : 'info')
}

/* ==========================================================================
   语音设置
   ==========================================================================
   界面只做"改设置 + 试听 + 检测 + 平衡音量"，真正的发音逻辑全在内核。
   与悬浮窗共用同一个接口（speech:speak），所以不会出现"设置里能响、查词时不出声"这种分叉。
   ========================================================================== */

let speech: SpeechStatus | null = null

/**
 * 上一次问 `speech:status` 用的是不是**空文本**（这一页一直都传空，见 `refreshSpeech`）。
 * 为什么要这个标记：内核把"没文本可念"规划成 `source=none` +「没有要念的文本。」，
 * 而 `renderSpeech()`"挑不出音源就报红字"那条检查标准会把自己问出来的空文本当成故障 ——
 * 有了它，那一种情况就不算问题、而"本机真没嗓子"照样会报。
 */
let statusAskedWithEmptyText = true
/** 试听用的播放器；和悬浮窗一样，一个复用对象就够了 */
const previewAudio = new Audio()
/**
 * 试听侧的增益路由：内置录音那条的增益要能**提升**，而 `<audio>.volume` 的上限是 1（只能压低），
 * 所以必须接进 Web Audio（见 src/shared/audioGraph.ts）。三路（豆包 / 系统语音 / 词典录音）共用
 * 这一个播放器，所以每次播放都要重设增益 —— 否则试听听到的和悬浮窗实际播放的不是一回事。
 */
const previewRoute = new SpeechAudioRoute(previewAudio, 'manager')
/**
 * 上一次写进「平衡音量」结果格的那句"为什么不能点 / 为什么没测通"。
 * 用途只有一个：用户改了表单就把它擦掉（那时它已经过期），而**不能**顺手把"量出来的结果"也擦了
 * （见 refreshDoubaoForm 与 balanceDoubaoLoudness 的 finally）。
 */
let balanceBlockedNote = ''
/** 上一次"检测判定为不可用"时写进 #doubaoStatus 的那句原文；非空 = 「保存」处于置灰态 */
let doubaoCheckFailure = ''
/**
 * 上一次 status 请求的序号，用来丢弃**过期**的回包（只认最后发出的那一条 —— 理由是一条约到过的竞态，
 * 见 refreshSpeech）。
 */
let speechStatusToken = 0

/**
 * 试听用的样本词。要求与后端 SpeechService.SampleWords 一致：都是"各语种都有的常见短词"，
 * 这样试听失败基本只会是音源的问题，而不是"这个词词典里没有"。
 */
const SAMPLE_WORDS: Record<string, string> = {
  en: 'apple',
  zh: '苹果',
  ja: 'りんご',
  ko: '사과',
  fr: 'bonjour',
  de: 'Hallo',
  es: 'manzana',
  it: 'mela',
  pt: 'maçã',
  ru: 'яблоко',
  ar: 'تفاحة',
  th: 'แอปเปิล',
  vi: 'táo',
  hi: 'सेब',
  tr: 'elma',
  pl: 'jabłko',
  nl: 'appel',
  sv: 'äpple',
  el: 'μήλο',
  he: 'תפוח',
  uk: 'яблуко',
  id: 'apel'
}

function sampleWord(language: string): string {
  return SAMPLE_WORDS[language] || 'hello'
}

/*
 * 「平衡音量」和"拖完滑块试听"共用的两句样本。为什么要写死：量音量必须在**同一句话**上比 ——
 * 换个句子，时长、停顿、音素构成全变了，两次量出来的 dB 就不可比。英文音色念英文、中文音色念中文，
 * 这两句与参考实现的参考句一致（内置默认表就是它量出来的）。
 */
const DOUBAO_SAMPLE_EN = 'An apple a day keeps the doctor away.'
const DOUBAO_SAMPLE_ZH = '苹果是一种水果。'

/*
 * 音色留空 = 用**内置默认音色**（与后端那边的规范化一致）。界面上写一份的用处有两个：
 *   ① 空格子也要把"留空会用谁"显示出来；② 检测要测**即将生效的那个音色**。
 * ⚠️ 这两串必须跟后端那份默认值一起改（那边一改，这里跟着改）。
 */
const DOUBAO_DEFAULT_VOICE_EN = 'en_female_dacey_uranus_bigtts' // Dacey（美式英语女声）
const DOUBAO_DEFAULT_VOICE_ZH = 'zh_female_vv_uranus_bigtts' // Vivi

/**
 * 这一页会走哪一条音源。顺序**不再由界面选**（"默认音源"那个下拉框已删）：后端给回来的 options
 * 就是定死的优先级 —— 词典自带音频 → 豆包 → 系统语音 —— 这里取第一个可用的。
 * 为什么要排掉 dict：这一页没有词条上下文，"当前词有没有原录音"永远算不出来（后端只在带
 * dictId + keyText 时才算得出），照实取它只会显示一条假结论。
 */
function preferredPlan(status: SpeechStatus): SpeechOption | null {
  const usable = status.options.filter((option) => option.source !== 'dict' && option.available)
  return usable.length ? usable[0] : null
}

/*
 * 页签：每个功能一块自己的页面（以前"发音设置"塞在词库列表下面，两个不相干的功能挤在一起，
 * 词库列表还被挤掉一半高度）。再加新功能只需多一个页签 + 一个 .pane。
 * ⚠️ 这份取值表**别在这里另写一遍**：`OptionsTab`（`web/shared/types.ts`）是主进程与桥共用的
 * 那一份，写成别名就不会出现"这里加了新页签、那边忘了加"的不一致。
 */
type TabName = OptionsTab

/**
 * 页签的中文名。**只有这一处** —— 窗口标题与后续任何"按页签说话"的地方都从这里取，
 * 免得又出现"同一个页签两处写名字"。
 */
const TAB_TITLES: Record<TabName, string> = {
  general: '常规',
  dicts: '词库',
  speech: '语音',
  translate: '翻译'
}

/**
 * 页签顺序（与 manager.html 里 `.tabs` 的先后一致），同时是"从外面请求翻到某一页"的白名单：
 * 主进程推过来的页签名是字符串，认不出来时**必须什么都不做**，不能把四个 pane 全藏起来变成白板。
 * ★ 顺序是**词库 / 语音 / 翻译 / 常规**：先"有什么可查" → 再"查到了怎么念" → 再"查不到怎么办"
 * → 最后才是"程序自身"。这里与 `OptionsTab`、`manager.html` 的 `<nav>`、的 `EXPECTED_TABS`
 * **四处一致**，改一处要一起改。
 */
const TAB_NAMES: TabName[] = ['dicts', 'speech', 'translate', 'general']

function isTabName(value: string): value is TabName {
  return (TAB_NAMES as string[]).includes(value)
}

/**
 * 页面加载完先落在哪一页 —— **第一个页签（词库）**（这条推翻了早先"默认落在常规页"的做法，
 * 两条菜单入口也跟着请求 `dicts` 了）。
 * 为什么是词库而不是"通用设置"：它是四页里唯一的**内容**页，也是唯一会"空"的页
 * （第一次装完一本都没有），打开选项时最可能要看的就是它。
 */
let activeTab: TabName = 'dicts'

function switchTab(tab: TabName, options: { animate?: boolean } = {}): void {
  activeTab = tab
  const panes: Record<TabName, HTMLElement> = {
    general: dom.paneGeneral,
    dicts: dom.paneDicts,
    speech: dom.paneSpeech,
    translate: dom.paneTranslate
  }
  const buttons: Record<TabName, HTMLButtonElement> = {
    general: dom.tabGeneral,
    dicts: dom.tabDicts,
    speech: dom.tabSpeech,
    translate: dom.tabTranslate
  }
  for (const name of TAB_NAMES) {
    const button = buttons[name]
    const active = name === tab
    button.dataset.active = String(active)
    button.setAttribute('aria-selected', String(active))
    panes[name].hidden = !active
  }
  /*
   * 窗口标题跟着页签走。⚠️ 它**只改页面自己的 `<title>`** —— WebView2 不会把页面标题写给宿主窗口，
   * 所以任务栏 / Alt-Tab 上看到的不是这一行（要那两处也跟着变得让壳接 `DocumentTitleChanged`）。
   * 它有两个实在的用处：DevTools 的目标列表里一眼认出在哪一页（ --tabs` 读它），
   * 以及"页签自述"这件事有个唯一出处。
   */
  document.title = '选项 · ' + TAB_TITLES[tab]
  // 换页时把上报的界面底色重算一次（外壳层要按内容重画）
  reportSurface()
  if (options.animate !== false) {
    // 页签切换是即时的，但滚动位置要回到顶部 —— 否则从长页面切到短页面会"停在半空"
    panes[tab].scrollTop = 0
  }
}

/**
 * 把"这一页现在为什么不好使"写在顶上那一行（`#speechSummary`），并把它显出来。
 * 为什么是"写进去 + 显出来"而不是另外塞一段文字：这一行**默认是 `hidden` 的**，而原来挂在
 * 页头框里的那三种错（系统语音不可用 / 没有可用音源 / 读不到语音设置）不能跟着消失 ——
 * 那正是用户"点什么都不出声"时唯一看得到的解释。所以元素留着（删了就再也没有地方说这句话）。
 */
function setSpeechIssue(message: string, hint?: string): void {
  dom.speechSummary.textContent = message
  dom.speechSummary.dataset.state = 'error'
  // 完整原因（含后端的 hint）挂在 title 上：那一行按一行省略排，长句子会被裁
  dom.speechSummary.title = hint ? `${message}。${hint}` : message
  dom.speechSummary.hidden = false
}

/** 一切正常：那一行一个字都不留（也不占高度 —— 没话说就别留空行） */
function clearSpeechIssue(): void {
  dom.speechSummary.textContent = ''
  dom.speechSummary.title = ''
  delete dom.speechSummary.dataset.state
  dom.speechSummary.hidden = true
}

/** 把整页语音设置按当前状态重画一遍 */
function renderSpeech(): void {
  if (!speech) return

  /*
   * 顶上那一行只在**真有问题**时出现。检查标准只有两条：
   *   · 系统语音也不可用、而且没有别的可用音源 → 系统语音不可用；
   *   · 一条可用音源都挑不出来（但引擎本身没问题）→ 后端给的原因 / "没有可用音源"。
   * 挑得出音源时**什么都不写**：音源顺序是后端定死的（见 preferredPlan），上面那行静态说明
   * 已经把顺序写清楚了，再报一遍"现在用豆包"只是多一行要维护的话。
   */
  const plan = preferredPlan(speech)
  if (!speech.engineAvailable && !plan) {
    setSpeechIssue('系统语音不可用', speech.message || speech.hint)
  } else if (!plan && !statusAskedWithEmptyText) {
    setSpeechIssue(speech.message || '没有可用音源', speech.hint)
  } else {
    /*
     * ⚠️ `statusAskedWithEmptyText` 那一种情况**不算问题**：这一页问 status 时传的是空文本
     *    （见 `refreshSpeech`），内核因此给出「没有要念的文本。」—— 那是**我问的话本身没带词**，
     *    不是"语音坏了"。真坏了的情况（本机一个嗓子都没有）由上面第一支兜着，照样会报出来。
     */
    clearSpeechIssue()
  }

  // 音色：自动 + 本机每一个
  const settings = speech.settings
  dom.speechVoice.textContent = ''
  const auto = document.createElement('option')
  auto.value = ''
  /*
   * 「自动」那一项**只写"自动"**：括号里那半句是给开发者看的实现说明，而下拉框里其它项都是
   * "名字（语种）"的格式，这一项写长了反而比真音色还显眼。挑音色的规则本身不变。
   */
  auto.textContent = '自动'
  dom.speechVoice.append(auto)
  for (const voice of speech.voices) {
    const option = document.createElement('option')
    option.value = voice.id
    option.textContent = `${voice.name}（${voice.languageLabel}）`
    dom.speechVoice.append(option)
  }
  dom.speechVoice.value = settings.voiceId || ''
  // 指定的音色要是已经不在本机了（换了机器 / 卸载了语音包），退回"自动"
  if (dom.speechVoice.value !== (settings.voiceId || '')) dom.speechVoice.value = ''

  dom.speechRate.value = String(settings.rate)
  dom.speechRateValue.textContent = settings.rate > 0 ? '+' + settings.rate : String(settings.rate)
  dom.speechAccent.value = settings.accent || 'auto'

  /*
   * 「默认语种」下拉框整格删掉了：语种不给用户选，程序自己按词判（判错了也照样能念，
   * 只是挑到的音色可能不是最贴的），所以这个格子里没有一个选项是用户该做的决定。
   * **协议里那个 `speech:status.languages` 清单留着不动**：它是"本机装了哪些离线音色"的权威清单，
   * 后端判语种、挑兜底音色时自己要用，只是**当前没有界面消费者** —— 以后要做语种相关的界面，
   * 直接拿这份清单用，别去别处重新枚举。
   */

  /*
   * 引擎说明：**只在引擎真的不可用时**才写出来。系统语音是三层音源里的兜底，
   * 它坏了得让用户知道为什么点了不响。（"本机离线音色 N 个"那段说明性文字按按需求删了 ——
   * 下面「音色」那一格列的就是本机全部音色，再数一遍没有新信息。）
   */
  if (!speech.engineAvailable) {
    dom.speechEngineNote.textContent = speech.engineMessage || '系统语音引擎不可用。'
    dom.speechEngineNote.dataset.state = 'error'
    dom.speechEngineNote.hidden = false
  } else {
    dom.speechEngineNote.textContent = ''
    dom.speechEngineNote.title = ''
    delete dom.speechEngineNote.dataset.state
    dom.speechEngineNote.hidden = true
  }

  renderDoubao()
  renderGains()
  // 用上面那个 formatSize（词典列表也在用）：同一个窗口里两种字节格式会显得像两个人写的。
  // 括号里只留"（内存）"：条数与字节数已经说清了是内存里那份，"磁盘缓存另算"没有界面出口。
  dom.speechCacheNote.textContent = `发音缓存：${speech.cacheCount} 条 / ${formatSize(speech.cacheBytes)}（内存）`
}

/**
 * 把豆包表单按当前配置重画一遍。这里只有一件反直觉的事：**Key 输入框永远是空的** ——
 * 后端为了不把凭据再散出去一次只回掩码，掩码挂 placeholder、真值谁也没有；用户不重新输入，
 * 保存时就不带 Key，老 Key 原地不动。反过来若在这里填个假的占位值，用户随手点一下保存就会把真 Key 覆盖掉。
 */
function renderDoubao(): void {
  const view = speech?.doubao
  dom.doubaoApiKey.value = ''
  dom.doubaoApiKey.placeholder = view?.apiKeyMasked || '还没填'
  dom.doubaoApiKey.title = view?.apiKeyMasked ? `当前 Key：${view.apiKeyMasked}（只有后端存着完整值）` : ''
  dom.doubaoResourceId.value = view?.resourceId || 'seed-tts-2.0'
  dom.doubaoSpeakerEn.value = view?.speakerEn || ''
  dom.doubaoSpeakerZh.value = view?.speakerZh || ''
  /*
   * 音色名 / 未收录警告不在这里写死：那个格子要跟着**输入框里的当前值**走，
   * 用户改完 ID 还没保存的那段时间也得是对的（见 refreshDoubaoForm）。
   */
  renderLoudness()
  refreshDoubaoForm()

  // 没配好时后端直接给了原因（没填 API Key / 音色那块有问题），照抄即可
  const reason = view?.reason || ''
  dom.doubaoStatus.textContent = reason || (view?.configured ? '已配置' : '')
  if (reason) {
    dom.doubaoStatus.dataset.state = 'error'
    dom.doubaoStatus.title = reason
  } else {
    dom.doubaoStatus.dataset.state = 'ok'
    dom.doubaoStatus.title = view?.endpoint ? `端点：${view.endpoint}` : ''
  }
}

/**
 * 只摆滑块与它右边那个数字。分开写出来的原因：设置写完时手上只有"这一次的值"（没有整份 status），
 * 但**照样要立刻把用户拖到的位置显示出来**（见 syncLoudnessView）。
 */
function setLoudnessInput(input: HTMLInputElement, value: HTMLSpanElement, current: number | null | undefined): void {
  // 不是数字就一个字都不写：range 拿到非法值会跳回 `value` 属性那个默认位置（HTML 里是 0），
  // 于是"没有数据"会被显示成"音量就是 0"（这正是我们要避免的那种假象）
  if (typeof current !== 'number' || !Number.isFinite(current)) return
  input.value = String(current)
  value.textContent = String(current)
}

/**
 * 两个音量滑块的显示：把**当前生效值**摆上去（滑块 + 右边那个数字）。生效值取自 `speech.doubao`
 * （后端已经算过"设置里是 null 就用内置表里的默认值"），界面自己再算一遍迟早会和那张表对不上。
 */
function renderLoudness(): void {
  const view = speech?.doubao
  /*
   * 没有 `doubao` 这一段就**什么都不写**：缺数据时若退回 0，而 0 在这里是个合法取值（min=-50），
   * 滑块会老老实实跳到 0 并停在那儿 —— 宁可保持现状（用户刚拖到的位置），也不要拿"没数据"冒充"音量是 0"。
   */
  if (!view) return
  setLoudnessInput(dom.doubaoLoudnessEn, dom.doubaoLoudnessEnValue, view.loudnessEn)
  setLoudnessInput(dom.doubaoLoudnessZh, dom.doubaoLoudnessZhValue, view.loudnessZh)
}

/** 设置改了就落盘；返回落盘后的设置（可能被规范化过，所以要重画） */
async function applySpeech(patch: Partial<SpeechSettings>): Promise<void> {
  try {
    const saved = await api.speechSettings(patch)
    if (speech) speech.settings = saved
  } catch (err) {
    toast('保存语音设置失败：' + (err instanceof Error ? err.message : String(err)), 'error')
  }
}

/**
 * 重新问一次"现在是什么状态"，然后把整页按它重画。必须有序号守卫（见 `speechStatusToken`）：
 * status 的耗时不是常数，回包的**到达顺序可以跟发出顺序相反**，晚到的那条会把新的状态盖回旧的。
 */
async function refreshSpeech(): Promise<void> {
  const token = ++speechStatusToken
  try {
    /*
     * ⚠️ 这里**故意传空文本**：这一页问的是"配置好不好使"（本机有没有嗓子、凭据填没填），不是
     *    "某个词怎么念"。而内核把"没文本可念"当成一档正经状态（`source=none` +「没有要念的文本。」，
     *    那是给**悬浮窗**"还没查过词"用的）—— 于是这一页拿到的 plan 永远是"一条都不通"，
     *    `renderSpeech()` 那条"没有可用音源"的检查标准就会被自己问出来的空文本顶红。
     *    下面这个 `statusAskedWithEmptyText` 标记就是让 `renderSpeech()` 分清"我问的话里没带词"
     *    与"语音真的坏了"。
     */
    const askText = ''
    statusAskedWithEmptyText = askText.trim().length === 0
    const next = await api.speechStatus(askText, null)
    /*
     * 过期的回包直接丢掉。为什么非挡不可：后端是在**收到请求那一刻**把 settings 快照下来算 status 的，
     * 而这次算得有多快并不固定（首次枚举系统语音能到几百毫秒，之后命中缓存只要几毫秒）—— 于是完全可能
     * "先发出的那条最慢、最后才回来"，它带的是**写之前**的旧值，滑块会被改回旧值**并一直停在那儿**，
     * 而设置文件里其实已经是新值、发音也真的按新值走。
     * 检查标准是"只认最后**发出**的那一条"（后端按收到顺序算，所以最后发出的就是最新的一份）。
     * 悬浮窗那边早就有同一个守卫（见 floating/main.ts 的 speechStatusToken），管理窗漏了。
     */
    if (token !== speechStatusToken) return
    speech = next
    renderSpeech()
  } catch (err) {
    if (token !== speechStatusToken) return
    // 读不到设置 = 这一页显示的东西一个都不可信，必须说出来（原来它写在页头那行摘要里）
    setSpeechIssue('读不到语音设置')
    console.error('读取语音设置失败', err)
  }
}

/**
 * 把当前表单写进设置（**不检测**），谁需要谁调。
 * ⚠️ 现在**只有「保存」按钮在检测通过之后才会调它**：输入框的 change 不再自动落盘 ——
 * 那会在"点保存 → 先检测"之前就把没验过的音色写进设置文件，于是"检测没过、没有保存"变成一句假话。
 * 这也让检测能测到**用户刚敲进去、还没存过的那个音色**（见 detectDoubao）。
 */
async function writeDoubaoSettings(): Promise<void> {
  /*
   * 只有"用户真的在输入框里敲了新值"才把 doubaoApiKey 放进 patch：输入框平时是空的（后端只回掩码），
   * 空值照发就等于把已存的 Key 抹掉。音色两项相反 —— 空串是**明确的选择**（"这个场景我不要在线念"）。
   */
  const key = dom.doubaoApiKey.value.trim()
  const patch: Partial<SpeechSettings> = {
    doubaoResourceId: dom.doubaoResourceId.value.trim(),
    doubaoSpeakerEn: dom.doubaoSpeakerEn.value.trim(),
    doubaoSpeakerZh: dom.doubaoSpeakerZh.value.trim()
  }
  if (key) patch.doubaoApiKey = key
  await applySpeech(patch)
  // 音色可能被规范化（去空格之类）、状态位要跟着变，所以整块重画
  await refreshSpeech()
}

/**
 * 刚粘贴进来的 Key **必须先落盘**才能测 —— 后端是拿**设置里那把** Key 去发请求的（检测和在线发音都一样）。
 * 不先写下去，检测只会回一句"还没有填 API Key"，等于没测。
 * 只写 Key、**不写音色**：音色恰恰是这次要判的东西，得等检测通过才准进设置。也不清空输入框：
 * 留着它，检测没过时用户还能就地改（下一次整页重画会按规矩清掉并填回掩码）。
 */
async function persistTypedKey(): Promise<void> {
  const typed = dom.doubaoApiKey.value.trim()
  if (!typed) return
  await applySpeech({ doubaoApiKey: typed })
  if (speech) speech.doubao.hasApiKey = true
}

/* ==========================================================================
   音色：界面只认 ID，不显示名字
   ==========================================================================
   不做静态快照就没法可靠确定官网名，那就不显示名字 —— 输入框里只有音色 ID（它就是接口要的东西，
   可靠）；留空 = 用内置默认音色；出错时报出来的也是音色 ID，名字一概不出现。
   ========================================================================== */

/**
 * 把"检测说这个音色不能用"贴到状态位上，并把「保存」置灰。
 * 为什么贴在 `#doubaoStatus`（API Key 旁边那一格）：它是这个表单唯一的状态位，而原因十有八九
 * 就是音色 ID 或 Key 写错了。长错误原文必须用整行铺开的样式（`.field-status-wide`），
 * 否则 `.field-status` 那条 120px 的一行省略正好把关键字裁掉。
 */
function markDoubaoFailure(message: string): void {
  doubaoCheckFailure = message
  dom.doubaoStatus.textContent = message
  /*
   * 长错误换成"另起一行"那一种情况。⚠️ `field-status-inside` 必须**留着**：
   * 它是"画在输入框里最右边"的定位（见 manager.css），一整个 className 覆盖掉它，
   * 那格字就会掉回普通流里、把输入框挤窄（这一轮刚把输入框改成撑满的）。
   */
  dom.doubaoStatus.className = 'field-status field-status-inside field-status-wide'
  dom.doubaoStatus.dataset.state = 'error'
  dom.doubaoStatus.title = message
  dom.doubaoApply.disabled = true
  dom.doubaoApply.title = '检测没通过：按旁边那句原文改一下，改完就能再点保存'
}

/**
 * 清掉上一次的失败结论。
 *
 * 谁调它：用户**一动表单**（input）就清 —— 否则填错一次之后「保存」就永远点不动了，
 * 连"改回原来那个音色"都做不到（用户特意提过这个死锁）。
 * 只擦自己写进去的那句：`#doubaoStatus` 平时显示的是后端给的"已配置 / 没有填 API Key"，
 * 无脑清空会把那句话一起擦掉。
 */
function clearDoubaoFailure(): void {
  if (doubaoCheckFailure) {
    if (dom.doubaoStatus.textContent === doubaoCheckFailure) {
      dom.doubaoStatus.textContent = ''
      dom.doubaoStatus.className = 'field-status field-status-inside'
      delete dom.doubaoStatus.dataset.state
      dom.doubaoStatus.title = ''
    }
    doubaoCheckFailure = ''
  }
  dom.doubaoApply.disabled = false
  dom.doubaoApply.title = '先检测豆包，通过了才写进设置'
}

/**
 * 表单的即时反馈：用户一改就把过期的失败结论清掉，并重算「平衡音量」能不能点。
 *
 * 这里**不再显示音色名**（用户：没法可靠确定官网名，干脆不显示）—— 音色那两格
 * 现在只有 ID，留空就用 placeholder 上那句"留空 = 用内置默认音色"。
 */
function refreshDoubaoForm(): void {
  clearDoubaoFailure()
  /*
   * 保存结果那一行也要清：那句话说的是"改动之前"那份配置。
   * ⚠️ 它**不跟 clearDoubaoFailure 合并**是有原因的：失败原文那一格（`#doubaoStatus`）
   * 平时显示的是后端给的"已配置 / 没有填 API Key"，无脑清空会连它一起擦掉；
   * 而这一格（`#doubaoApplyResult`）整格就是我们自己在写，清了没有副作用。
   */
  clearApplyNote()

  const blocked = balanceBlockReason()
  dom.doubaoBalance.disabled = !!blocked
  /*
   * 这个按钮**不挂 title**（要去掉悬停提示）：它旁边那格已经把"能不能点、
   * 为什么不能点"写成字了，再挂一个 tooltip 只是多一处要维护的文案。
   */
  /*
   * 按钮灰着的时候把"为什么不能点"写在旁边那个结果格里（只挂 title 用户看不见）。
   * 但**不能无条件写**：那个格子也放"量/检测失败的原因"，无脑覆盖会刚写完就被擦掉。
   * 所以只在"原因还在"时写，原因消失时只擦掉自己写的那句（`showBalanceNote('')` 顺手把
   * 整格收起来 —— 成功路径下这一格必须一个字都不显示，见 balanceDoubaoLoudness）。
   */
  if (blocked) {
    balanceBlockedNote = blocked
    showBalanceNote(blocked, 'error')
  } else if (balanceBlockedNote && dom.doubaoBalanceResult.textContent === balanceBlockedNote) {
    showBalanceNote('', 'progress')
    balanceBlockedNote = ''
  } else {
    balanceBlockedNote = ''
  }
}

/** 有 Key 没有：输入框里刚粘过（还没落盘）也算 */
function doubaoHasKey(): boolean {
  return !!dom.doubaoApiKey.value.trim() || !!speech?.doubao.hasApiKey
}

/**
 * 「平衡音量」还需要什么。
 *
 * 只有一件：**得有一把 Key** —— 没有 Key 就没有在线那条路，也就没有"音量"能量。
 * 音色留空不算缺东西了：**留空 = 用内置默认音色**（英文 Dacey、中文 Vivi），
 * 那两个默认音色本来就能念（这一版把"留空 = 回落到系统语音"那套说法和判断都删了）。
 */
function balanceBlockReason(): string {
  if (!doubaoHasKey()) return '还没有填 API Key'
  return ''
}

/* ==========================================================================
   保存前 / 平衡前：先跑一次真实检测
   ==========================================================================
   两件事在这一版变了：
   ① 把"用本地音色清单拦保存"整个撤掉（用户：那个设计不好）—— 清单是快照，
      拦下来的可能是一个刚上线、其实完全能用的音色。**能不能用由真测一次说了算**；
   ② 判定只剩两档：**通了就存，不通就不存**。不再给"网络错误 / 超时"开后门
      （用户否掉了那个分叉）：不通时给提示就够了 —— 在线这条路本来就只是三层音源里的
      一层，它不通会自动退回系统语音，软件功能不受影响，所以没必要为了"没网"把配置
      放进去。代价是保存真的会发请求（英文 + 中文各一次，样本词只有几个字）。
   ========================================================================== */

/** 检测的判定：只有"通过"和"不通"两种 */
type DoubaoCheck = 'pass' | 'fail'

/**
 * "这一项不是失败，而是**那一项本来就是空音色**"。
 *
 * 用户给的通过条件就是两条："每项 ok===true，或那一项本来就是空音色"。
 * 正常情况下不会走到这里 —— 留空的格子会按**内置默认音色**去测（见 detectDoubao），
 * 测的是一个真音色。留着它是兜底：万一后端对空音色回一项"这个音色没填"，
 * 那属于"这一项没东西可测"，不该当失败拦人。
 * ⚠️ 检查标准刻意窄：只认"没填 / 没有填"。像"还没有填 API Key"那种是**真的不通**
 * （没有 Key 就发不出请求），照旧拦下来，不在这条例外里。
 */
const DOUBAO_NOT_FILLED = /没填|没有填/

/** 一次检测的判定：选项 + 要说给用户的话（**照抄后端 error 原文**，不另编） + 明细 */
interface DoubaoCheckResult {
  verdict: DoubaoCheck
  message: string
  results: DoubaoTest[]
}

/** 这一项是不是"本来就没东西可测"（音色是空的），而不是"服务端说它不能用" */
function isNotFilledItem(result: DoubaoTest): boolean {
  return !(result.speaker || '').trim() || DOUBAO_NOT_FILLED.test(result.error || '')
}

/**
 * 判定：**每一项都要 ok，否则算不通**。
 *
 * 通不过的原因一视同仁 —— 鉴权失败、额度用尽、音色与 Resource 不配套、服务端返回空音频、
 * 网络错误、超时，全都是"不通"：不写盘、把后端那句原文贴出来、把「保存」置灰。
 * 不给"网络类"开绿灯是用户明确要的取舍（原话：网络错误为什么会被锁死呢？我们会给出提示，
 * 而且会自动退回系统语音，不影响软件功能）—— 所以这里连 StatusCode==0 都不用看。
 */
function classifyDoubaoTest(results: DoubaoTest[]): DoubaoCheckResult {
  const failed = results.filter((result) => !result.ok && !isNotFilledItem(result))
  if (failed.length === 0) return { verdict: 'pass', message: '', results }
  return {
    verdict: 'fail',
    // 原样照抄后端的 error（含错误码 / 限流 / 额度提示），界面自己另编一句只会让人没法拿它去搜
    message: failed.map((result) => result.error || '没有错误说明').join('；'),
    results
  }
}

/**
 * 真去请求一次，返回判定结果。保存前、平衡前都走它。
 *
 * 测的是**表单里那两个音色**（显式音色重载），不是"设置里已经存着的"那两个：
 * 用户改了 ID、还没写盘时，"先检测再决定存不存"必须检测**即将存下去的那个**，
 * 否则测的是旧配置，等于没测（而"先写盘再测"会让"没通过就没保存"变成一句假话）。
 * 留空的格子按**内置默认音色**去测 —— 留空 = 用默认那两个，测的就该是它们。
 *
 * ⚠️ 这里原来有一个 `renderRows` 开关（"要不要把明细贴进 `#doubaoTestList`"）：
 * 用户第 3、4 条把那份明细连元素一起拿掉了（保存就是检测的超集，页面上不再摊明细），
 * 所以这个参数没有消费者了，一并删掉 —— 结论只走两处：`#doubaoStatus`（失败原因，挂在 Key 旁边）
 * 和按钮下面那一列状态。
 *
 * 隐式依赖：后端是拿**设置里那把 Key** 去发请求的，所以调用方要先 persistTypedKey()。
 */
async function detectDoubao(): Promise<DoubaoCheckResult> {
  const targets: [string, string][] = [
    [dom.doubaoSpeakerEn.value.trim() || DOUBAO_DEFAULT_VOICE_EN, 'en'],
    [dom.doubaoSpeakerZh.value.trim() || DOUBAO_DEFAULT_VOICE_ZH, 'zh']
  ]
  try {
    const results: DoubaoTest[] = []
    for (const [speaker, language] of targets) {
      // 两次分开问：后端给了"指定音色 + 语种"的重载，正好一边一个
      results.push(...(await api.testDoubao(speaker, language)))
    }
    return classifyDoubaoTest(results)
  } catch (err) {
    /*
     * 连检测请求都发不出去（IPC 断了之类）：**没拿到任何结论**。
     * 按"不通"处理（不给网络错误开后门，也就没有第二种选项可选），
     * 但话要说清是这一步就失败了，而不是服务端说音色不对。
     */
    const message = '检测没能进行：' + (err instanceof Error ? err.message : String(err))
    return { verdict: 'fail', message, results: [] }
  }
}

/**
 * 「保存」按钮：**先检测，再决定存不存**。
 *
 * 通了才写盘；不通就一个字都不写、把原文贴出来、按钮置灰。
 * 拦住那档**不能**顺手刷新页面：一刷新，用户刚填的值就被后端那份旧配置冲掉，
 * 那句失败原文也一起没了（用户改一下表单就能重新点保存，见 refreshDoubaoForm）。
 *
 * 这个按钮**同时取代了原来的「检测豆包」**（用户第 3 条的原话：保存按钮在功能上是检测豆包的超集）：
 * 它本来就会真发一次合成请求、验不过就不落盘，所以"单独走一遍检测"是重复入口，已删除。
 * 顺带把它俩的结果做成了**同一处显示**（按钮下面那一列状态的第 1 行）：
 *   通过 → 绿字 `检测通过，已保存`（用户第 3 条点名要的字）
 *   不通 → 红字一行，形如 `保存失败，HTTP 403`（状态码拿不到时才退回后端那句原文的首行）
 * 完整原文一个字不丢地挂在 title 上（那一行是一行省略，见 .speech-status-stack 的注释）。
 */
async function saveDoubao(): Promise<void> {
  const original = dom.doubaoApply.textContent
  dom.doubaoApply.disabled = true
  dom.doubaoApply.textContent = '正在检测…'
  try {
    // 刚粘贴的 Key 必须先落盘：检测是后端拿设置里那把 Key 去发请求的（见 persistTypedKey）
    await persistTypedKey()
    const check = await detectDoubao()
    if (check.verdict === 'fail') {
      markDoubaoFailure(check.message)
      showApplyNote(saveFailureNote(check), 'error', check.message)
      toast('检测没通过，没有保存：' + check.message, 'error')
      return
    }
    await writeDoubaoSettings()
    // 走到这儿说明上一次的失败结论已经作废：把置灰和那句提示清掉
    clearDoubaoFailure()
    /*
     * 成功那句必须写在 writeDoubaoSettings() **之后**：那一步末尾会 refreshSpeech()，
     * 整页重画（renderDoubao → refreshDoubaoForm）顺手把这一列状态清干净 ——
     * 写在前面的绿字会被自己这次刷新擦掉。
     */
    showApplyNote('检测通过，已保存', 'success')
    toast('豆包配置已保存')
  } finally {
    dom.doubaoApply.textContent = original
    // 灰不灰由"有没有失败结论"说了算：不通那档要保持灰，通了才恢复可点
    if (!doubaoCheckFailure) dom.doubaoApply.disabled = false
  }
}

/**
 * 保存失败时**那一行**的字（用户第 3 条：红字、一行、把遇到的问题写出来）。
 *
 * 优先写状态码：`保存失败，HTTP 403` 是能拿去搜的形式，也一眼看得出"是鉴权/额度那类"。
 * ⚠️ 只认 **≥400** 的状态码，这一条是刻意收窄的：
 * 豆包那种"HTTP 200 + SSE 里带一个业务错误码"的失败（例如 40000001 音色/参数不对），
 * 后端把 `statusCode` 填的是 **200** —— 直接照抄就成了「保存失败，HTTP 200」，
 * 那句话不只是没用，是**在骗人**（用户会去查 200 是什么意思）。
 * 这种情形走下面那条：把后端那句人话（含它自己的业务错误码）压成一行。
 * 只有连状态码都没有（`statusCode === 0`：请求根本没发出去，比如 IPC 断了 / 网线拔了）时
 * 同样退回那句话的首行 —— 那种情况话本身才是信息。
 * 两种情况下**完整原文都进 title**（`showApplyNote` 的最后一个参数），所以这里可以放心压。
 */
function saveFailureNote(check: DoubaoCheckResult): string {
  const code = check.results.find((result) => !result.ok && result.statusCode >= 400)?.statusCode
  if (code) return `保存失败，HTTP ${code}`
  const line = (check.message || '检测没通过').split('\n')[0].trim()
  return `保存失败，${line}`
}

/**
 * 按钮下面那一列状态的**第 1 行**：保存的结果（用户第 3、5 条）。
 *
 * 与「平衡音量」那一行（`showBalanceNote`）结构完全对称，各写各的格、共用一个纵向列 ——
 * 谁没话说就整格 `hidden`（不留空行），于是"没有保存情况时「已平衡」自然落到第一行"
 * 是布局算出来的，不需要两套位置。
 *
 * `tone` 决定颜色（都走 CSS：`.field-status[data-state='success']` 是绿的、`'error'` 是红的），
 * `full` 是挂在 title 上的完整原文 —— 那一行按"一行省略"排，长句子会被裁，原文因此不丢。
 * 空串 = 收起来（不占高度）。
 */
function showApplyNote(text: string, tone: 'success' | 'error', full = text): void {
  dom.doubaoApplyResult.textContent = text
  dom.doubaoApplyResult.title = full
  if (text) dom.doubaoApplyResult.dataset.state = tone
  else delete dom.doubaoApplyResult.dataset.state
  dom.doubaoApplyResult.hidden = text.length === 0
}

/**
 * 收起保存结果那一行。
 *
 * 谁调它：用户**一动表单** —— 上一次那句"检测通过，已保存"说的是**改动之前**那份配置，
 * 用户改了音色还留着那句话，等于在骗人（和 `clearDoubaoFailure` 是同一条约定）。
 */
function clearApplyNote(): void {
  showApplyNote('', 'success')
}

/* ==========================================================================
   音量：两个滑块 + 「平衡音量」
   ========================================================================== */

/**
 * loudness_rate 能表达的范围：官方给的是 100 = 2.0 倍音量、-50 = 0.5 倍，
 * 也就是振幅倍数 1 + rate/100 ∈ [0.5, 2.0]，换算成 dB 只有 ±6.02。
 * 差距比这更大时就够不着了，只能"抬轻的 + 压响的"，残余多少如实报出来。
 */
const LOUDNESS_CLAMP_DB = 20 * Math.log10(2)

/**
 * 落盘之后把"生效值"刷回滑块。
 *
 * 为什么不能只等下一次 status：用户松手的那一刻就该看到自己拖到的位置，
 * 而 status 要再跑一趟 IPC（还可能被后端算得比这一次写更早）。
 * 为什么不能直接信滑块上的数：C# 会 Clamp（-50..100），而设置里存 null 时
 * 生效的是内置表里的默认值 —— 两种情况下滑块该显示的数都不是用户刚才拖到的那个。
 * 手上有整份 status 时以 `speech.doubao` 为准（那是后端的约定）；
 * 万一这次没带回 `doubao` 那一段，就直接用刚写进去的数，绝不退回 0。
 */
function syncLoudnessView(): void {
  const view = speech?.doubao
  if (!speech) return
  const en = speech.settings.doubaoLoudnessEn
  const zh = speech.settings.doubaoLoudnessZh
  if (!view) {
    setLoudnessInput(dom.doubaoLoudnessEn, dom.doubaoLoudnessEnValue, en)
    setLoudnessInput(dom.doubaoLoudnessZh, dom.doubaoLoudnessZhValue, zh)
    return
  }
  view.loudnessEn = en ?? view.loudnessDefaultEn
  view.loudnessZh = zh ?? view.loudnessDefaultZh
  renderLoudness()
}

/** 拖动时只动数字；松手才落盘并试听 */
async function changeDoubaoLoudness(which: 'en' | 'zh'): Promise<void> {
  const input = which === 'en' ? dom.doubaoLoudnessEn : dom.doubaoLoudnessZh
  await applySpeech(
    which === 'en' ? { doubaoLoudnessEn: Number(input.value) } : { doubaoLoudnessZh: Number(input.value) }
  )
  syncLoudnessView()
  // 试听用**落盘后的生效值**：C# 会 Clamp，滑块上那个数可能已经被改过了
  await previewDoubaoSample(which, Number(input.value))
}

/**
 * 按指定音色试听**固定样本句**。两条路都走它：
 *   · 音色格后面那个「试听」按钮（见 previewVoice，不传 loudness）；
 *   · 拖完音量滑块松手时（见 changeDoubaoLoudness，传落盘后的生效值）。
 * 两条路共用一份实现，所以"点试听"和"拖完滑块听到的"一定是同一个嗓子、同一句话。
 *
 * `loudness` 不传就用滑块上当前的值：试听的目的就是"听听这个音量合不合适"，
 * 所以必须带上设置里那个值，而不是让后端用默认值念一遍。
 */
async function previewDoubaoSample(which: 'en' | 'zh', loudness?: number): Promise<void> {
  const en = which === 'en'
  // 格子留空 = 用内置默认音色，所以这里也要拿默认那个 ID 去试听，不能报"还没填"
  const voiceId = (en ? dom.doubaoSpeakerEn.value : dom.doubaoSpeakerZh.value).trim() ||
    (en ? DOUBAO_DEFAULT_VOICE_EN : DOUBAO_DEFAULT_VOICE_ZH)
  const who = en ? '英文音色' : '中文音色'
  const text = en ? DOUBAO_SAMPLE_EN : DOUBAO_SAMPLE_ZH
  const level = loudness ?? Number(en ? dom.doubaoLoudnessEn.value : dom.doubaoLoudnessZh.value)
  // 音频图要在**用户手势里**建（这一次点击），合成那一趟回来时手势已经过去了
  previewRoute.warmUp()
  try {
    const result: SpeakResult = await api.speak(text, { source: 'online', voiceId, loudness: level })
    if (!result.ok) {
      // 失败原因原样显示（后端的 message 是人话：没填 Key / 音色不对 / 服务端报错）
      toast(result.message || '试听失败', 'error')
      return
    }
    const applied = playPreview(result)
    toast(
      withRouteNote(
        `试听 ${who} · 音量 ${level}：「${text}」${result.cached ? '（命中缓存）' : ''}`,
        applied
      )
    )
  } catch (err) {
    toast('试听失败：' + (err instanceof Error ? err.message : String(err)), 'error')
  }
}

/** 把一段音频地址交给那个复用同一个 <audio> 播（和悬浮窗一样，一个实例就够） */
function playPreview(result: SpeakResult): SpeechRouteResult {
  /*
   * 增益**每次播放都重设**，而且按这一次的音源算（见 speechGainDb）：
   * 同一个 <audio> 会先后播豆包 / 系统语音 / 词典录音，只有录音那条的增益是播放侧施加的。
   * 返回的 applied 里带着"这次到底用了音频图没有"——调用方要如实缀在提示里
   * （音频图没起来时正增益做不到，不说出来用户只会觉得"调了没用"）。
   */
  const applied = previewRoute.apply(speechGainDb(result))
  try {
    previewAudio.pause()
    previewAudio.src = result.url
    previewAudio.currentTime = 0
  } catch {
    /* 忽略：接着播 */
  }
  const played = previewAudio.play()
  if (played && typeof played.catch === 'function') {
    played.catch((err: unknown) => {
      toast('播放失败：' + (err instanceof Error ? err.message : String(err)), 'error')
    })
  }
  return applied
}

/** 一段音频量出来的两个数 */
interface ClipLevel {
  /** 有效语音电平（dBFS，算法见 measureClip） */
  activeDb: number
  /** 峰值振幅（0..1 的线性值）：提升增益时"会不会削顶"靠它，不靠有效电平 */
  peak: number
}

/**
 * 量一段音频的**有效语音电平**（dBFS）与**峰值**。
 *
 * 电平的约定与参考实现完全一致 —— C# 内置那张表就是它量出来的；
 * 这边算法一变，界面上"平衡音量"算出来的数就和内置默认值不是一回事了。
 *   ① 按 20ms 分帧，逐帧算 RMS；
 *   ② 丢掉比"整段 RMS 低 25 dB"的帧（那是句子之间的停顿）；
 *   ③ 剩下的帧求平均能量，再换成 dB。
 * 为什么不能用整段 RMS：念得慢、停顿多的音色会被算得比实际更轻，调平就会过冲。
 *
 * 峰值是**另一件事**：有效电平低不等于录得轻 —— 一句话里只要有一个字录满，
 * 按电平差把增益提上去就会削顶（削顶是失真，听上去比轻更糟）。
 * 所以峰值取整段的 max|x|，**不做帧门限**：削顶恰恰发生在最响那一瞬，那一瞬正是要被算进来的。
 */
async function measureClip(buffer: ArrayBuffer): Promise<ClipLevel> {
  const context = new AudioContext()
  try {
    const audio = await context.decodeAudioData(buffer)
    const data = audio.getChannelData(0)
    let sum = 0
    let peak = 0
    for (let i = 0; i < data.length; i++) {
      const value = data[i]
      sum += value * value
      const magnitude = Math.abs(value)
      if (magnitude > peak) peak = magnitude
    }
    const wholeDb = 20 * Math.log10(Math.sqrt(sum / data.length) || 1e-9)
    const gate = wholeDb - 25

    const frame = Math.round(audio.sampleRate * 0.02)
    let activeEnergy = 0
    let activeFrames = 0
    for (let start = 0; start + frame <= data.length; start += frame) {
      let energy = 0
      for (let i = 0; i < frame; i++) {
        const value = data[start + i]
        energy += value * value
      }
      const rms = Math.sqrt(energy / frame)
      if (rms <= 0 || 20 * Math.log10(rms) <= gate) continue
      activeEnergy += energy / frame
      activeFrames++
    }
    if (activeFrames === 0) throw new Error('整段都是静音，量不出电平')
    return { activeDb: 20 * Math.log10(Math.sqrt(activeEnergy / activeFrames)), peak }
  } finally {
    /*
     * 量完就关：AudioContext 是稀缺资源（一个页面同时只能开几个），
     * 留着不放，用户多点几次「平衡音量」就会开不出来。
     */
    void context.close()
  }
}

/** 只要电平的地方（豆包那两个音色）走它 */
async function measureActiveDb(buffer: ArrayBuffer): Promise<number> {
  return (await measureClip(buffer)).activeDb
}

/*
 * 这里原来还有一个 `median()`（偶数个取中间两个的平均），只被"内置录音"那一段用来取
 * 中位电平。那一段按按需求整个删掉了（见 balanceDoubaoLoudness 里那段"为什么不做"），
 * 于是它也没有调用点了 —— 一并删掉，不留"以后也许还会用"的死代码。
 */

/** 合成一句并量出它在中性响度（loudness = 0）下的电平 */
async function measureSample(voiceId: string, text: string): Promise<number> {
  const result: SpeakResult = await api.speak(text, { source: 'online', voiceId, loudness: 0 })
  if (!result.ok) throw new Error(result.message || '合成失败')
  /*
   * 同源地址（https://lookup.invalid/__speak__/…），页面 CSP 的 connect-src 'self' 就够，
   * 不用给任何外部域开口子。
   */
  const response = await fetch(result.url)
  if (!response.ok) throw new Error(`取音频失败（HTTP ${response.status}）`)
  return measureActiveDb(await response.arrayBuffer())
}

/**
 * 一个音色被夹进它自己的可达范围（±6.02 dB）之后，实际能落到哪个电平。
 * 目标够得着就是目标本身；够不着就停在边界上 —— 「平衡音量」报的残余差就是按它算的。
 */
function reachableAt(level: number, target: number): number {
  return Math.min(level + LOUDNESS_CLAMP_DB, Math.max(level - LOUDNESS_CLAMP_DB, target))
}

/** 给定目标电平 T：各音色被夹进各自可达范围之后，彼此差多少 dB */
function spreadAt(levels: number[], target: number): number {
  let min = Infinity
  let max = -Infinity
  for (const level of levels) {
    const reachable = reachableAt(level, target)
    min = Math.min(min, reachable)
    max = Math.max(max, reachable)
  }
  return max - min
}

/** 两个音色的名字（`levels` 的顺序是 [英文, 中文]，见 balanceDoubaoLoudness） */
const VOICE_LABELS = ['英文音色', '中文音色']

/**
 * 「够不着」那句话：谁比谁轻多少、这个接口总共能补多少、顶到极限之后还剩多少没对齐。
 *
 * 为什么用 toast、不写进页面：用户把这一页的常驻说明性文字删过一轮，约定是
 * **点完只看到"已平衡"**；而这条解释只在"被夹住"时才有意义（默认的 Dacey / Vivi
 * 只差 1.9 dB，永远碰不到边界）。常驻在页面上 = 大多数用户白读一行字，
 * 一闪而过的一句正好：撞上的那一次看得见，没撞上的一次不占地方。
 *
 * 为什么检查标准由调用方给（`clamped`）：见 balanceDoubaoLoudness 里那段说明 ——
 * "被夹"才是这条信息的适用条件，残差大不是。
 * 三个入参都要给全：`spreadBefore` 是量到的原始差距、`residual` 是顶到极限之后还剩多少
 * （两个数都来自调用方算好的那一份，这里不再自己算一遍 —— 算两遍迟早对不上）。
 */
function clampedNote(levels: number[], spreadBefore: number, residual: number): string {
  const lighter = levels[0] <= levels[1] ? 0 : 1
  const heavier = 1 - lighter
  return (
    `${VOICE_LABELS[lighter]}比${VOICE_LABELS[heavier]}轻 ${spreadBefore.toFixed(1)} dB，` +
    `这个接口最多补 ${Math.round(2 * LOUDNESS_CLAMP_DB)} dB：已各自顶到极限，还差 ${residual.toFixed(1)} dB`
  )
}

/** 一个音色要落到目标电平，该给多少 loudness_rate（换算自振幅倍数，再夹进 -50..100） */
function loudnessRateFor(level: number, target: number): number {
  const reachable = reachableAt(level, target)
  const factor = Math.pow(10, (reachable - level) / 20)
  return Math.round(Math.max(-50, Math.min(100, 100 * (factor - 1))))
}

/* ==========================================================================
   音量对齐：内置录音 / 系统语音的增益（两个滑块 + 写盘）
   ==========================================================================
   背景：豆包那两个 `loudness_rate` 只管它自己。内置录音（.mdd 原录音）与系统语音
   各自还有一份增益，两条路**谁来写**是不一样的（这一版按已定定的）：
     · **系统语音**：点「平衡音量」会按实测电平自动写（见 balanceDoubaoLoudness 的 ②）；
     · **内置录音**：**只由用户手拖**，平衡流程一个字都不写（为什么不做：见
       balanceDoubaoLoudness 里那段——无加权 RMS 跨素材比较的方向是错的，
       而换 A 加权又会把豆包内部平衡弄坏，没有两全的约定）。

   两个来源的增益**不能共用一处施加**：
     · 内置录音是 .mdd 里的原字节，谁也没替它调过音量 → 只能在**播放侧**施加，
       而"提升"要求 `gain.value > 1`，所以播放侧得走 Web Audio 的 GainNode
       （`<audio>.volume` 上限是 1，提不上去，见 src/shared/audioGraph.ts）；
     · 系统语音（和豆包）是后端在合成时烘进字节里的 → 播放侧照 0 dB 走，
       所以它们共用同一个 GainNode、值恒为 1。
   ========================================================================== */

/**
 * 两个增益滑块的量程（dB），与后端夹取的范围一致。
 * 是负一段、正一段的对称区间（不是"只能压低"）：录得比基准轻的录音要能被提上来，
 * 只压不升会把基准整体拖到最小那一本词典的水平上。
 */
const GAIN_MIN_DB = -24
const GAIN_MAX_DB = 12
/** 滑块步长；写进设置的值也按它对齐（见 roundToStep） */
const GAIN_STEP_DB = 0.5

/** 夹进量程（界面上拖不出去，但「平衡音量」算出来的数要先夹好再写） */
function clampGainDb(value: number): number {
  return Math.max(GAIN_MIN_DB, Math.min(GAIN_MAX_DB, value))
}

/**
 * 对齐到滑块步长（0.5 dB）。
 *
 * 为什么要对齐：滑块显示的数就是设置里那个数，写一个 −3.27 进去，界面显示 −3.27，
 * 而用户再拖一下就跳到 −3.5 —— 两个数之间没有对应关系，回头核对时对不上号。
 */
function roundToStep(value: number): number {
  const stepped = Math.round(value / GAIN_STEP_DB) * GAIN_STEP_DB
  // −0 在显示上就是 "0"，但别把它留给 toFixed / 比较：统一归零
  return Object.is(stepped, -0) ? 0 : Number(stepped.toFixed(1))
}

/** 增益的显示约定：正数带 +（和语速那一格一样），0 就是 0 */
function formatGainDb(value: number): string {
  const shown = Math.round(value * 10) / 10
  return shown > 0 ? '+' + shown : String(shown)
}

/**
 * 一整行增益：有效值摆上去，不能改就置灰 + 把原因写在行尾那格。
 *
 * "置灰并说明原因"和这一页别的几处一个约定：只挂 title 用户看不见，
 * 所以原因必须写成字（行尾那格），title 只是补充。
 * 反过来，**没有原因时那一格要整个收起来（`hidden`）**：按需求把这一页的灰色说明性文字
 * 删干净，"没话说却留一行灰字"正是他要去掉的东西（空元素还会占掉 8px 行距）。
 * 那格用的是 `.field-status-wide`（自己占一整行），所以它出现时不会把滑块挤窄 ——
 * 滑块是定长的（见 manager.css 里 `.speech-body` 的算式），出现/消失都不改变这一行的几何。
 *
 * 行尾那个「试听」跟着滑块一起置灰：这一行之所以不能拖，是**这条音源量不出来 / 用不了**
 * （这本词典没有可校准的录音、本机没有系统语音），那"试听一下"同样听不到东西 ——
 * 让按钮亮着而滑块灰着，用户点下去只会得到一句失败 toast，他还得回头去读那行原因。
 */
function setGainRow(
  input: HTMLInputElement,
  value: HTMLSpanElement,
  note: HTMLSpanElement,
  preview: HTMLButtonElement,
  current: number | null,
  reason: string
): void {
  /*
   * 不是数字就一个字都不写：range 拿到非法值会跳回 HTML 里那个 `value="0"`，
   * 于是"没有数据"会被显示成"增益就是 0"（这正是 setLoudnessInput 那边避免的假象）。
   */
  if (typeof current === 'number' && Number.isFinite(current)) {
    input.value = String(current)
    value.textContent = formatGainDb(current)
  }
  input.disabled = !!reason
  preview.disabled = input.disabled
  note.textContent = reason
  note.title = reason
  note.hidden = !reason
  if (reason) note.dataset.state = 'error'
  else delete note.dataset.state
}

/** 把两条增益滑块按当前状态重画（值 / 当前词典 / 置灰 / 为什么置灰） */
function renderGains(): void {
  const view = speech?.voiceGains
  /*
   * 后端还没给 voiceGains（老内核 / 接口定义没落地）时**不能什么都不写**：
   * 滑块会停在 HTML 里那个 0，看起来像"增益就是 0"。
   * 置灰 + 说明原因，和这一页别处遇到"读不到数据"时的处理一致。
   */
  if (!view) {
    dom.dictGainLabel.textContent = '音量'
    dom.dictGainLabel.title = ''
    const reason = '读不到增益数据（后端还没给 voiceGains）'
    setGainRow(dom.dictGain, dom.dictGainValue, dom.dictGainNote, dom.dictGainPreview, null, reason)
    setGainRow(dom.systemGain, dom.systemGainValue, dom.systemGainNote, dom.systemGainPreview, null, reason)
    applyDictPreviewState()
    return
  }

  /*
   * 标签**只写「音量」**（用户这一轮点名的：两条滑块的名字都改成「音量」；哪一种情况音源由大项标题说），
   * 词典名搬到 `title` 上。
   *
   * 为什么词典名必须留着、又只能挂在 title 上：内置录音那一份增益是按词典存的，
   * 换一本词典看到的数就不一样 —— "这是哪一本"得看得到；但写进正文会让标签宽度随词典名变化，
   * 用户的原话是"具体字典名不用体现，因为鼠标悬浮出现的提示也能看到"。
   */
  dom.dictGainLabel.textContent = '音量'
  dom.dictGainLabel.title = view.dictTitle ? `当前词典：${view.dictTitle}` : ''
  setGainRow(
    dom.dictGain,
    dom.dictGainValue,
    dom.dictGainNote,
    dom.dictGainPreview,
    view.dictAvailable ? view.dictGainDb : null,
    view.dictAvailable ? '' : view.dictMessage || '这本词典没有可校准的录音'
  )
  setGainRow(
    dom.systemGain,
    dom.systemGainValue,
    dom.systemGainNote,
    dom.systemGainPreview,
    view.systemAvailable ? view.systemGainDb : null,
    view.systemAvailable ? '' : view.systemMessage || '系统语音不可用'
  )
  /*
   * 内置录音那一行的「试听」还多一条"能不能点"的检查标准：这本词典里有没有 apple / 苹果 的录音。
   * 先用缓存摆一次（词典没换就不用重新问），再让后台去问一次（词典换了才真发 IPC）。
   */
  applyDictPreviewState()
  void refreshDictPreviewWord()
}

/**
 * 写增益，并采纳后端回的那份语音状态。
 *
 * `setGains` 回的就是最新的 `speech:status` 视图，直接拿它重画（顺手让在飞的旧回包作废）——
 * 再跑一趟 `speechStatus` 有可能拿回"更早算出来的"那份（见 refreshSpeech 里那条竞态）。
 */
async function writeGains(patch: { dictGainDb?: number; systemGainDb?: number }): Promise<void> {
  const next = await api.setGains(patch)
  if (next && next.voiceGains) {
    speechStatusToken++
    speech = next
    renderSpeech()
  } else {
    // 后端回的没有 voiceGains 那一段（老内核）：退回重新问一次状态
    await refreshSpeech()
  }
}

/**
 * 把这一行滑块上的**当前值**落盘，然后按它试听一句。
 *
 * 谁调它：滑块松手（change），以及这一行行尾那个「试听」按钮 —— 两处要的是同一件事
 * （"我眼前这个数听起来怎么样"），所以共用一份实现，不会出现"拖一下听到的和点试听听到的不一样"。
 * 用户拖完就听见效果，不用再去别处点一下；想再听一遍又不必再拖一次。
 *
 * ⚠️ 这里只写增益这两项：豆包表单里那几格（Key / 音色）**不在这里落盘**，
 * 所以拖滑块不会把"刚敲进去、还没验过"的音色偷偷写进 settings.json ——
 * 「保存」那条"先检测豆包，通过了才写盘"的规则不被绕过（见 saveDoubao）。
 */
async function changeGain(which: 'dict' | 'system'): Promise<void> {
  const input = which === 'dict' ? dom.dictGain : dom.systemGain
  const value = clampGainDb(roundToStep(Number(input.value)))
  input.value = String(value)
  ;(which === 'dict' ? dom.dictGainValue : dom.systemGainValue).textContent = formatGainDb(value)
  try {
    await writeGains(which === 'dict' ? { dictGainDb: value } : { systemGainDb: value })
  } catch (err) {
    toast('保存增益失败：' + (err instanceof Error ? err.message : String(err)), 'error')
    // 写失败就把界面拨回设置里那份（别让滑块停在一个没落盘的位置上）
    await refreshSpeech()
    return
  }
  await previewGainSample(which)
}

/**
 * 内置录音那一种情况试听**放哪一个词条**（第 5 条）：
 * 英文优先 `apple`、其次中文 `苹果`；两个都没有 → 这个按钮不可用。
 *
 * 为什么不再拿"取样接口给的第一条"：那一条是**按索引均匀撒点**挑出来的，
 * 在牛津高阶那本词典上落到的是 `a'` 这种条目 —— 用户听到的是一个莫名其妙的词，
 * 而不是"这本词典念 apple 是什么样"。固定成 apple / 苹果之后，试听听到的东西才可预期。
 */
const DICT_PREVIEW_WORDS = ['apple', '苹果']
/**
 * 解析出来的结果（按词典缓存：这本词典有没有那两个词条只跟"当前是哪本词典"有关，
 * 而 `renderGains` 每次 status 刷新都会跑 —— 不缓存就会每次都发两次 IPC）。
 */
let dictPreviewCache: { key: string; word: string } | null = null
/** 正在解析：防止连续几次 status 刷新叠出好几趟 IPC */
let dictPreviewPending = false

/**
 * 问后端：当前词典里 apple / 苹果 这两个词条**有没有原录音**。
 *
 * 走的还是那条"只算不产字节"的轻接口（`speech:status` 带上 dictId + keyText 才会算这一项），
 * 检查标准是返回里 `source === 'dict'` 那一项的 `available` —— 它说的正是"这个词条有没有词典自带的原录音"。
 * ⚠️ 这个返回值**不能**赋给 `speech`：它是"针对某一个词"的视图，会把整页状态换掉。
 */
async function refreshDictPreviewWord(force?: boolean): Promise<void> {
  const view = speech?.voiceGains
  const key = `${view?.dictId || ''}|${view?.dictTitle || ''}`
  if (dictPreviewPending || (!force && dictPreviewCache && dictPreviewCache.key === key)) return
  dictPreviewPending = true
  try {
    const dictId = view?.dictId || null
    let found = ''
    for (const word of DICT_PREVIEW_WORDS) {
      const status = await api.speechStatus(word, view?.dictTitle || null, dictId, word)
      const option = status.options.find((item) => item.source === 'dict')
      if (option?.available) {
        found = word
        break
      }
    }
    dictPreviewCache = { key, word: found }
  } catch {
    // 问不到就当"没有"：按钮灰着，鼠标悬浮能看到为什么（见 applyDictPreviewState）
    dictPreviewCache = { key, word: '' }
  } finally {
    dictPreviewPending = false
    applyDictPreviewState()
  }
}

/**
 * 把内置录音那一行的「试听」按钮按解析结果摆好（能不能点、悬浮提示说什么）。
 *
 * 两件事一起管：**这一行被置灰时**（读不到增益 / 这本词典根本没有可校准的录音）
 * 按钮跟着灰；**这本词典里没有 apple / 苹果 时**也灰 —— 后者必须写出原因
 * （项目规范：置灰要说明原因），所以原因挂在 title 上。
 */
function applyDictPreviewState(): void {
  const word = dictPreviewCache?.word || ''
  const rowDisabled = dom.dictGain.disabled
  dom.dictGainPreview.disabled = rowDisabled || !word
  dom.dictGainPreview.title = rowDisabled
    ? '这一行现在是灰的，先看行尾那句原因'
    : word
      ? `按这一行的音量试听这本词典里「${word}」的录音`
      : '这本词典里没有 apple / 苹果 这两个词条的录音，试听不了'
}

/**
 * 按这一行当前的增益试听一句 —— 让用户直接听见这一格改了什么。
 *
 * 内置录音放的是**这本词典里 apple / 苹果 那一条原录音**（见 refreshDictPreviewWord；
 * 两个都没有时按钮是灰的，走不到这里），系统语音沿用这一页本来那个样本词，
 * 并且**显式指定 source='system'**：不指的话很可能被词典原录音/豆包抢先，
 * 那就试听不出系统语音这一格的效果了。
 */
async function previewGainSample(which: 'dict' | 'system'): Promise<void> {
  previewRoute.warmUp()
  try {
    if (which === 'dict') {
      /*
       * 念哪一个词条：apple / 苹果（见 refreshDictPreviewWord）。解析结果就在缓存里，
       * 没解析出来（或这本词典两个都没有）时按钮本来就是灰的 —— 这里再兜一次，
       * 免得"点了没反应"（`force` 那一下是"缓存还没落"时现问一次）。
       */
      if (!dictPreviewCache?.word) await refreshDictPreviewWord(true)
      const word = dictPreviewCache?.word || ''
      const dictId = speech?.voiceGains?.dictId || null
      if (!word) {
        toast('这本词典里没有 apple / 苹果 这两个词条的录音，听不了', 'error')
        return
      }
      // 走和悬浮窗**同一条路**（source='dict'）：增益是播放侧按回包里的 gainDb 施加的，
      // 拿样本字节直接播虽然也能听，但那就不是"实际会听到的那一条"了
      const result: SpeakResult = await api.speak(word, {
        source: 'dict',
        dictId,
        keyText: word
      })
      if (!result.ok) {
        toast(result.message || '试听失败', 'error')
        return
      }
      const applied = playPreview(result)
      toast(withRouteNote(`试听「${word}」的内置录音 · 音量 ${formatGainDb(Number(dom.dictGain.value))} dB`, applied))
      return
    }

    const language = speech?.detected.language || 'en'
    const word = sampleWord(language)
    const result: SpeakResult = await api.speak(word, { source: 'system', language })
    if (!result.ok) {
      toast(result.message || '试听失败', 'error')
      return
    }
    const applied = playPreview(result)
    toast(withRouteNote(`试听「${word}」${result.languageLabel} · 系统语音增益 ${formatGainDb(Number(dom.systemGain.value))} dB`, applied))
  } catch (err) {
    toast('试听失败：' + (err instanceof Error ? err.message : String(err)), 'error')
  }
}

/**
 * 平衡音量：把三条音源对齐到同一个基准。
 *
 * 动手量之前**先跑一次真实检测**（用的是保存前那一套判定，见 classifyDoubaoTest）：
 * 音色 ID 都不通就去量，量回来的是"错音色"的电平，还会把好音色的补偿值覆盖掉。
 * 检测通过之后分两段：
 *   ① **豆包**：两个音色各念**同一句**样本、响度传 0（量的是中性电平）→ 在 ±6.02 dB 的可达范围里
 *      扫一个目标，让两者"被夹到可达范围后"彼此差最小 → 换算成 loudness_rate 写进设置。
 *      基准 T 取**平衡后**两个音色各自的实测电平的平均 —— 不是原始电平，也不是扫出来的目标：
 *      某个音色被夹在 ±6 dB 边界上时，它实际停在边界上、够不着目标。
 *   ② **系统语音**：中英各念一句样本、量电平。它和豆包一样是**后端烘进字节**里的，
 *      所以量回来的是"已经带上当前增益的电平"，要先反推中性电平（见 measureSystemNeutralLevel）。
 *
 * ⚠️ **内置录音（词典原录音）那一段已经删掉了 —— 不是漏了，是不做**（已定）。
 * 这一条很容易被"顺手加回来"，所以把实测依据留在这儿：
 *   · 这套约定是**无加权宽带 RMS**，它在**跨素材**比较上方向就是错的：那条词典原录音实测
 *     active −14.35 dBFS、豆包念同一个词 −19.56 dBFS ⇒ 按测量"录音该比豆包响 5.2 dB"，
 *     **而耳朵听到的是豆包略响**（偏差 5~6 dB）。根因是能量分布：那条录音约 1/3 能量在 200 Hz
 *     以下，豆包 TTS 在 200 Hz 以下几乎没能量（低 25 dB），而人耳对那一段的响度敏感度低十几 dB ——
 *     无加权 RMS 把录音**算响了**。
 *   · 换成贴耳朵的 **A 加权**能把上面那个偏差从 5~6 dB 收到 1.34 dB，**但它会把已经好用的
 *     豆包内部平衡从 1.6 dB 拉坏到 4.4 dB**（Vivi 的低频能量远多于 Dacey）——
 *     也就是说**没有任何一个约定能同时管住"跨素材"和"同素材"**，这不是调参能解决的问题。
 *   · 而且那条录音峰值 −1.25 dBFS，不削顶余量只有 +1.16 dB —— 就算算对了也提不动。
 * 所以这一格改成**只由用户手拖**（滑块 / 范围 / 「试听」都留着，见 renderGains），
 * 「平衡音量」**一个字都不往 `dictGainDb` 里写**：绝不覆盖用户手调的那个值。
 * 系统语音那一路照旧自动写（实测 −0.88 与现在写的 −0.5 差不多，而且 SAPI 与豆包同属合成音、
 * 频谱相近，跨素材那个坑不存在）。桥接方法 `dictSamples` **保留**（它本身是好的），
 * 只是不再被这条流程调用。
 *
 * 结果：**页面上只留一句结论**（按需求）—— 跑的过程中是 `平衡中…`（按钮上）
 * 与"正在量…"（结果格），跑完是 **`已平衡`**，有段落没量成时是 `已平衡（N 段没量成）`。
 * 更早那两版还把每一段量到的数（中性电平 / loudness_rate / 中位峰值 / 不削顶上限）
 * 逐行写进一列分步结果里 —— 用户连着两轮要求删：先删数字，再删整列
 * （第 4 条："跑完不用显示那两行蓝字，弹出提示已经足够"）。
 * 所以现在**页面上除了那一句结论什么都没有**，够不着 / 哪一段没量成全部只在 toast 里说
 * （一闪而过，正是"弹出提示"）。
 * 失败/置灰时照旧把原因写出来（那是"必须说明原因"的另一半约定），但**一行封顶**。
 * `#doubaoBalanceResult` 这一格**留在 DOM 里**：删掉就没有地方说失败了。
 */
async function balanceDoubaoLoudness(): Promise<void> {
  const blocked = balanceBlockReason()
  if (blocked) {
    showBalanceNote(blocked, 'error')
    toast('平衡音量：' + blocked, 'error')
    return
  }
  /*
   * 已经在跑了就别再开一趟。
   *
   * 为什么要有这条守卫：这一趟要十几秒，而按钮文字/禁用状态是**这一趟自己在改**的
   * （`平衡中…` → 跑完还回「平衡音量」）。两趟叠在一起时，**后开始的那一趟会把自己开始时
   * 看到的文字存成 "original"** —— 那时候它看到的是"平衡中…"，于是跑完把按钮**写成"平衡中…"**
   * 再也回不来（谁最后结束谁说了算）。真人点不出第二趟（按钮在跑的时候是 disabled 的，
   * 合成事件才点得动），但一条"点了两次就卡在平衡中…"的路不该留着。
   */
  if (dom.doubaoBalance.disabled) return

  const original = dom.doubaoBalance.textContent
  dom.doubaoBalance.disabled = true
  /*
   * 按钮上的进行中文案。这是**唯一**能跟用户说"还在跑"的地方：整段要十几秒
   * （两个音色各合成一句 + 系统语音两句 + 逐段取音频解码），
   * 一句话不说用户只会以为按钮坏了。
   */
  dom.doubaoBalance.textContent = '平衡中…'
  showBalanceNote('正在量两个音色…', 'progress')
  try {
    // 刚粘贴的 Key 必须先落盘：检测和在线合成都是后端拿设置里那把 Key 去发请求的
    await persistTypedKey()

    /*
     * 1) 先真测一次再量音量。不通就不量了 —— 音色 ID 都不通还去量，
     *    量回来的是"错音色"的电平（甚至只是两次报错），写进设置还会把好音色的补偿值覆盖掉。
     *    判定与保存那条路完全同一份（见 classifyDoubaoTest）。
     *    这一趟的结论只进下面那一行（失败原因给 `showBalanceNote`）——
     *    检测明细连元素一起删掉了（用户第 3、4 条，见 detectDoubao 的注释）。
     */
    const check = await detectDoubao()
    if (check.verdict === 'fail') {
      /*
       * 不通 → 提示 + 把「保存」也置灰（用户改一下表单就恢复，见 refreshDoubaoForm）。
       * 网络错误 / 超时同样拦：跟保存那边一条规则，不再分"能不能连上"。
       */
      balanceBlockedNote = check.message
      showBalanceNote(check.message, 'error')
      markDoubaoFailure(check.message)
      toast('平衡音量：检测没通过，先按提示改好再量 — ' + check.message, 'error')
      return
    }

    /* ---- ① 豆包：量中性电平、扫目标、写 loudness_rate、定基准 T ---- */
    const en = dom.doubaoSpeakerEn.value.trim() || DOUBAO_DEFAULT_VOICE_EN
    const zh = dom.doubaoSpeakerZh.value.trim() || DOUBAO_DEFAULT_VOICE_ZH

    const levels = [await measureSample(en, DOUBAO_SAMPLE_EN), await measureSample(zh, DOUBAO_SAMPLE_ZH)]

    // 目标电平：能靠拢就扫出最靠拢的那个；够不着就取可达区间的中点（两头都尽力）
    const lo = Math.min(...levels) + LOUDNESS_CLAMP_DB
    const hi = Math.max(...levels) - LOUDNESS_CLAMP_DB
    let target = levels[0]
    let best = Infinity
    if (lo <= hi) {
      for (let t = lo; t <= hi; t += 0.05) {
        const spread = spreadAt(levels, t)
        if (spread < best) {
          best = spread
          target = t
        }
      }
    } else {
      target = (lo + hi) / 2
    }

    const rates = levels.map((level) => loudnessRateFor(level, target))
    await applySpeech({ doubaoLoudnessEn: rates[0], doubaoLoudnessZh: rates[1] })
    syncLoudnessView()

    const before = Math.max(...levels) - Math.min(...levels)
    const after = spreadAt(levels, target)
    /** 平衡后每个音色实际停在哪个电平（够不着目标的就是边界值） */
    const settled = levels.map((level) => reachableAt(level, target))
    /** 基准 T：平衡后两个音色实测电平的平均 */
    const baseDb = (settled[0] + settled[1]) / 2
    /*
     * **够不着**这一种情况：目标修正量超出可达范围（±6.02 dB）时，某个音色是被**夹**在边界上的。
     *
     * 检查标准为什么钉在"真的被夹了"上，而不是"残差大"：用户实际遇到的那一对音色差 11.6 dB，
     * 两边各自被顶到 ±6 dB 的极限，**算出来的残差只有 ~0.4 dB** —— 残差小得看着像"已经对齐了"，
     * 而他真正卡住的是"两个滑块都顶死了，为什么还不是一个音量"。夹住 = 该出现解释的时刻；
     * 残差大只是它的一个（而且是不充分的）表象。
     * 容差 1e-9 只用来吸收浮点误差：`reachableAt` 恰好等于边界时不算被夹。
     */
    const clamped = levels.some((level) => Math.abs(target - level) > LOUDNESS_CLAMP_DB + 1e-9)
    /*
     * 这一段量到的数（中性电平 / loudness_rate / 平衡后差多少 / 基准 T）**一个都不写进页面** ——
     * 按需求点完「平衡音量」看到的只有"已平衡"，不需要实测结果字。它们没有消失：
     * 量出来的补偿值就在两个滑块的显示值上（也是 settings.json 里那两个数）。
     */
    showBalanceNote('正在量系统语音…', 'progress')

    /*
     * 这一次"哪一段没量成"：只攒在这里给收尾那句 toast 用。
     * 为什么是**这一次**的局部数组、不是模块级状态：它是这一次运行的产物，
     * 放外面就得记着在每一条路径上清干净（漏一次，上一次的失败会跟到这一次的结论里）。
     */
    const balanceProblems: string[] = []
    const noteProblem = (text: string): void => {
      balanceProblems.push(text)
    }
    /*
     * ⚠️ **这里原来还有一段"内置录音"：量当前词典最多 6 条带录音的词条、取中位数、
     * 按 `min(不削顶上限, T − 中位电平)` 算出 `dictGainDb` 并写下去。整段已删，而且不许加回来。**
     *
     * 为什么不做（实测依据，见函数头那段）：无加权宽带 RMS 在**跨素材**比较上方向就是错的 ——
     * 词典原录音与豆包念同一个词时，测量说"录音该比豆包响 5.2 dB"，而耳朵听到的是豆包略响；
     * 换成 A 加权能修好跨素材（偏差 5~6 dB → 1.34 dB），却会把已经好用的豆包内部平衡
     * 从 1.6 dB 拉坏到 4.4 dB。**没有任何一个约定能同时管住这两件事**，所以这一格交给手拖。
     *
     * ⚠️ 连带的一条：这一段删掉之后，`dictGainDb` 这个设置**一个字都不能写** ——
     * 不是"写 0"、也不是"写 null"，而是**根本不碰**：它是用户手拖出来的值（按词典存），
     * 点一下「平衡音量」把它清零，用户会以为自己的调整被吃掉了。所以下面整个流程里
     * 不许出现 `dictGainDb` / `writeGains({ dictGainDb })` ——
     * ⚠️ **这一条没有自动检查**：`tools/ui-static-check.mjs` 只有 ①–⑧ 项，其中 ⑧ 是"同一 id
     * 不许跨页签重复"，**没有**钉这件事。改这里之前先读上面这段（别指望检查器会拦住你）。
     * `api.dictSamples()` 也**不再调用**（桥接方法本身保留：它还是好的、还有诊断在验）。
     */

    /* ---- ② 系统语音：中英各一句，量出来的是"带当前增益"的，要先反推中性电平 ---- */
    let systemGainDb: number | null = null
    try {
      const neutralDb = await measureSystemNeutralLevel()
      systemGainDb = clampGainDb(roundToStep(baseDb - neutralDb))
      await writeGains({ systemGainDb })
    } catch (err) {
      noteProblem('系统语音：跳过 — ' + (err instanceof Error ? err.message : String(err)))
    }

    /*
     * 收尾：**页面那一格只留一句结论**（用户要的就是"点一下看到已平衡"）。
     * 有段落没量成时结论后面跟一句"几段没量成"，明细只在 toast 里说
     * （逐段那份列表按按需求删掉了：页面上不留那两行蓝字）。
     */
    const problems = balanceProblems.length
    showBalanceNote(problems ? `已平衡（${problems} 段没量成）` : '已平衡', problems ? 'error' : 'ok')
    /*
     * 详细的话全部走 toast：它是**一闪而过的**，不是"页面上留下的灰字蓝字"。
     *
     * 为什么拼成一句再发：`toast()` 只有一个位置（后一句会盖掉前一句），
     * 而"够不着"正是最需要被看见的那一句 —— 它跟「已平衡」二选一，不能两个都发。
     * 够不着时**不写「已平衡」**：那句话本身已经把话说全了（"已各自顶到极限"就是结论），
     * 而结论在页面那一格上照旧写着。
     */
    const notes: string[] = []
    if (clamped) notes.push(clampedNote(levels, before, after))
    if (problems) notes.push(`${problems} 段没量成：${balanceProblems.join('；')}`)
    toast(notes.length === 0 ? '已平衡' : clamped ? notes.join('；') : `已平衡（${notes.join('；')}）`)
  } catch (err) {
    // 失败原因原样显示：没填 Key / 音色不对 / 网络 / 服务端报错，界面不另编说法
    const message = err instanceof Error ? err.message : String(err)
    /*
     * 失败那句话**一行封顶**（按需求）：后端原文经常带音色 ID 与错误码，60+ 字按自然折行
     * 就是三行，把整页推长。所以这一格做了一行省略（见 manager.css），
     * 完整的原文一个字都不丢 —— 它就在同一格的 title 上（见 showBalanceNote），悬停能看全。
     */
    showBalanceNote('失败：' + message, 'error')
    toast('平衡音量失败：' + message, 'error')
  } finally {
    dom.doubaoBalance.textContent = original
    /*
     * 只重算"能不能点"，**不要**在这里调 refreshDoubaoForm：
     * 那个函数里有"用户一改就擦掉过期提示"的逻辑，在这儿跑会把刚写进去的失败原因擦掉。
     */
    dom.doubaoBalance.disabled = !!balanceBlockReason()
  }
}

/**
 * 「平衡音量」那一格显示什么：**跑的过程中是进度，跑完是结论**（`平衡中…` → `已平衡`），
 * 失败/置灰时是"为什么"。
 *
 * 空串 = 整格 `hidden`。之所以是"隐藏"而不是"把元素删掉"：失败了必须有地方说原因
 * 这一格是那条路径唯一的落点（见 balanceDoubaoLoudness 开头那几处）。
 * 一起把 `hidden` 也管上，是为了不留空行 —— 元素空着也占 flex 行距（它是 `flex: 1 1 100%`，自己占一行）。
 *
 * ⚠️ 这一格是**一行封顶**的（`.speech-actions .field-status` 做了一行省略），
 * 所以完整原文必须挂到 `title` 上：后端那句失败原因常常 60+ 字，
 * 让它在页面上折三行正是用户要去掉的东西，但"失败要说清"这条约定不能因此打折 ——
 * 省略号后面的那半句在 title 里一个字都不少。
 */
function showBalanceNote(text: string, tone: 'progress' | 'ok' | 'error'): void {
  dom.doubaoBalanceResult.textContent = text
  dom.doubaoBalanceResult.title = text
  if (tone === 'progress') delete dom.doubaoBalanceResult.dataset.state
  else dom.doubaoBalanceResult.dataset.state = tone
  dom.doubaoBalanceResult.hidden = text.length === 0
}

/*
 * 这里原来还有一个 `addBalanceProblem()`：它往 `#balanceSteps` 那一列一项一行地贴
 * "哪一段没量成"。用户这一轮点名的第 4 条是"平衡跑完不用显示那两行蓝字，弹出提示已经足够"，
 * 于是那份列表连元素、连这个函数一起删掉了 —— 现在这些原因只进收尾那句 toast
 * （收集在 `balanceProblems` 这个本次运行的局部数组里，见 balanceDoubaoLoudness）。
 *
 * ⚠️ 为什么"没量成"这件事**不能**干脆不记：那一段的增益会停在旧值上，
 * 而页面上完全看不出来（滑块照样显示着数）—— 不说出来就是不报错地失败。
 */

/**
 * 量系统语音的**中性电平**（不叠增益那一下的电平）。
 *
 * 量它为什么要传 `gainDb: 0`：系统语音的增益是**后端烘进字节里**的（播放侧对非 dict 恒用
 * 0 dB 的增益节点，见 speechGainDb），所以直接量到的是"已经带上当前增益的电平"。
 * 拿它去算 `T − 实测` 就等于把上一次加的增益又减了一遍 ——
 * 表现是"点一次 +3 dB、再点一次变回 0"来回震荡。`gainDb: 0` 是这一次的覆盖
 * （与豆包量中性电平传的 `loudness: 0` 完全对称）。
 *
 * 为什么还留一道减法：覆盖万一没生效（老内核 / 这个参数还没落地），字节里就带着旧增益，
 * 而回包里的 `gainDb` 说的正是"这一段实际带了多少" —— 减掉它拿到中性电平，
 * 与上面那条约定等价（覆盖生效时它是 0，这一步什么都不减）。
 *
 * 中英各量一句再平均：单独一句会受音素构成影响，两句平均比一句话稳。
 */
async function measureSystemNeutralLevel(): Promise<number> {
  const targets: [string, string][] = [
    [DOUBAO_SAMPLE_EN, 'en'],
    [DOUBAO_SAMPLE_ZH, 'zh']
  ]
  const levels: number[] = []
  for (const [text, language] of targets) {
    // gainDb: 0 = 这次合成不要叠增益，量中性电平（试听/播放那条路不传这个参数）
    const result: SpeakResult = await api.speak(text, { source: 'system', language, gainDb: 0 })
    if (!result.ok) throw new Error(result.message || '系统语音合成失败')
    // 同源地址（https://lookup.invalid/__speak__/…），页面 CSP 的 connect-src 'self' 就够
    const response = await fetch(result.url)
    if (!response.ok) throw new Error(`取音频失败（HTTP ${response.status}）`)
    const measured = await measureActiveDb(await response.arrayBuffer())
    const carried =
      typeof result.gainDb === 'number' && Number.isFinite(result.gainDb) ? result.gainDb : 0
    levels.push(measured - carried)
  }
  return levels.reduce((total, level) => total + level, 0) / levels.length
}

/*
 * 这里原来还有三个东西，随用户第 3、4 条一起删掉了：
 *   · `renderDoubaoTestRows(results)` —— 把一次检测的每一项贴进 `#doubaoTestList`（一项一行）；
 *   · `testDoubao()` —— 那个「检测豆包」按钮的处理器；
 *   · `describeDoubaoTest(result)` —— 拼出"en_female_dacey · en『An apple…』— 200 · 16k B /
 *     820 ms · 计费 4 字"那一行的函数（用户第 4 条点名不要的正是这种行）。
 *
 * 为什么删得掉、且不算丢功能：保存按钮本来就会真发一次合成请求、验不过就不落盘
 * （用户第 3 条的原话：保存是检测的超集），所以"单独走一遍检测"只是同一个动作的第二个入口；
 * 而每一行里那堆数字（状态码 / 字节 / 耗时 / 计费字数）**只服务于"手点一次检测"这个动作** ——
 * 保存那条路走完就落盘了，用户要看的是"通没通"，不是逐项明细。
 *
 * 失败时要说的话一个字没少：后端那句 error 原文照旧贴在 `#doubaoStatus`（Key 旁边，
 * 用 `.field-status-wide` 整行铺开、原文一字不裁），它的**首行**另写进按钮下面那一行红字
 * （见 saveFailureNote），title 上挂完整原文。
 */

/**
 * 「英文音色 / 中文音色」后面那个「试听」：念**这一格里的那个音色**。
 *
 * 为什么按格子分成两个按钮（原来只有一个「试听」）：那个按钮念的是"按默认语种挑的样本词、
 * 音源由优先级自动决定"，跟眼前两个音色格子没有对应关系 —— 用户改完英文音色点它，
 * 听到的可能压根不是这一格（甚至可能是系统语音）。现在每个按钮的行为是确定的：
 *   点的哪一格 → 用**那一格里当前填着的 ID**（留空 = 内置默认音色，与保存后的行为一致）
 *   → 念那一格的固定样本句（英文 An apple… / 中文 苹果是一种水果。）
 *   → 音量用**那一格自己滑块上的当前值**（试听的意义就是听这个音量合不合适）。
 * 这三件事全在 previewDoubaoSample 里（它与"拖完滑块自动试听一句"是同一条路），
 * 所以这里只多做了"点的那颗按钮先置灰"这一件事。
 * 失败**必须出话**：没填 Key / 音色不对时 previewDoubaoSample 会把后端那句原文吐司出来
 * （项目规范：不许不报错地失败失败）。
 */
async function previewVoice(which: 'en' | 'zh'): Promise<void> {
  const button = which === 'en' ? dom.voicePreviewEn : dom.voicePreviewZh
  button.disabled = true
  try {
    await previewDoubaoSample(which)
  } finally {
    button.disabled = false
  }
}

function setupSpeech(): void {
  // 和悬浮窗一样把播放器挂进 DOM（hidden）：自动化验证能直接读它的 paused / readyState
  previewAudio.id = 'previewAudio'
  previewAudio.hidden = true
  document.body.append(previewAudio)

  /*
   * 音源**不给选**：顺序由 C# 定死（词典自带音频 → 豆包 → 系统语音），
   * 这一页顶上那段说明就是原来那个"默认音源"下拉框的替代品 ——
   * 所以这里不再有那个下拉的 change 处理器了，元素本身也已经删掉。
   */

  dom.speechVoice.addEventListener('change', async () => {
    await applySpeech({ voiceId: dom.speechVoice.value || null })
    await refreshSpeech()
  })
  dom.speechAccent.addEventListener('change', async () => {
    await applySpeech({ accent: dom.speechAccent.value as SpeechSettings['accent'] })
    await refreshSpeech()
  })
  /*
   * 这里原来还有「默认语种」下拉的 change 处理器（`applySpeech({defaultLanguage: …})`）。
   * 那一格整格删掉了（用户第 8 条：语种不给用户选，程序自己按词判），
   * 所以这里不再有它的监听 —— 详见 renderSpeech 里那段说明（协议里的 `languages` 留着）。
   */
  /* 语速：拖动时只更新数字，松手（change）才落盘 —— 拖一次写一次盘太浪费 */
  dom.speechRate.addEventListener('input', () => {
    const value = Number(dom.speechRate.value)
    dom.speechRateValue.textContent = value > 0 ? '+' + value : String(value)
  })
  dom.speechRate.addEventListener('change', async () => {
    await applySpeech({ rate: Number(dom.speechRate.value) })
  })
  /*
   * 语速那一行行尾的「重置」（第 6 条）：把语速拨回默认的 0。
   * 为什么先把界面拨回 0 再落盘：落盘是异步的，`applySpeech` 不会重画这一页，
   * 不先把滑块和数字改掉的话，用户点完还看着原来那个数（几百毫秒后才可能被下一次 status 刷回来）。
   */
  dom.speechRateReset.addEventListener('click', async () => {
    dom.speechRate.value = '0'
    dom.speechRateValue.textContent = '0'
    await applySpeech({ rate: 0 })
    toast('语速已重置')
  })

  /*
   * 音色 ID：边打边查清单。这里必须用 input 而不是 change —— 名字要在**敲完的那一刻**
   * 就出现，用户才知道这串乱码认不认得；等到回车/失焦才发现"这个 ID 不能保存"太晚了。
   */
  dom.doubaoSpeakerEn.addEventListener('input', refreshDoubaoForm)
  dom.doubaoSpeakerZh.addEventListener('input', refreshDoubaoForm)
  /* 刚粘进 Key 的那一下，「平衡音量」要立刻从灰变亮，所以这一格也要听 input */
  dom.doubaoApiKey.addEventListener('input', refreshDoubaoForm)

  /*
   * 两个音量滑块：拖动（input）只改数字，**松手（change）才落盘并试听**。
   * 为什么不用 input 落盘：拖一次滑块会经过几十个位置，每个位置发一次合成请求 ——
   * 那是按字数计费的接口，等于每挪一格就买一次单。
   */
  const sliders: [HTMLInputElement, HTMLSpanElement, 'en' | 'zh'][] = [
    [dom.doubaoLoudnessEn, dom.doubaoLoudnessEnValue, 'en'],
    [dom.doubaoLoudnessZh, dom.doubaoLoudnessZhValue, 'zh']
  ]
  for (const [input, value, which] of sliders) {
    input.addEventListener('input', () => {
      value.textContent = input.value
    })
    input.addEventListener('change', () => void changeDoubaoLoudness(which))
  }

  /*
   * 两个**增益**滑块（音量对齐那一组）：交互与上面豆包那两个完全一致 ——
   * 拖动（input）只改数字，松手（change）才写盘 + 试听一句。
   * 写盘走 setGains（只写增益这两项），不是 speechSettings：豆包表单那几格
   * 仍然只能由「保存」在检测通过之后落盘（见 changeGain 里的说明）。
   */
  const gainSliders: [HTMLInputElement, HTMLSpanElement, 'dict' | 'system'][] = [
    [dom.dictGain, dom.dictGainValue, 'dict'],
    [dom.systemGain, dom.systemGainValue, 'system']
  ]
  for (const [input, value, which] of gainSliders) {
    input.addEventListener('input', () => {
      value.textContent = formatGainDb(Number(input.value))
    })
    input.addEventListener('change', () => void changeGain(which))
  }

  /*
   * 两行行尾的「试听」走的是**和拖滑块松手完全同一条路**（changeGain）：
   * 先把滑块当前值写进设置、再按它放一句 —— 内置录音放这本词典的一条原录音，
   * 系统语音现场合成一句离线语音（见 previewGainSample）。
   *
   * 为什么不直接调 previewGainSample：播放时的那份增益是**后端按设置里那个值**给的
   * （回包里的 gainDb），滑块上还没落盘的位置它不知道 —— 那样"按这一行当前值试听"
   * 就成了假话（听到的是上一次的值）。写下去再放，听到的才是眼前这个数。
   * 代价是点一次会落一次盘，但那本来就是这一页的模型：增益滑块没有"保存"这一步，拖完即生效。
   */
  dom.dictGainPreview.addEventListener('click', () => void changeGain('dict'))
  dom.systemGainPreview.addEventListener('click', () => void changeGain('system'))

  /*
   * 豆包表单里**没有"改了就地保存"**：三个输入框都不再挂 change 处理器。
   *
   * 为什么删掉（原来是有的）：这一版「保存」= 先检测、通过了才写盘。
   * 要是输入框失焦时先偷偷写一遍，"检测没通过、没有保存"就成了一句假话 ——
   * 文件里其实已经把那个没验过的音色写进去了。而且那样也会让检测测到旧配置
   * （值已经写下去、覆盖了"刚敲进去但没验过"的那个）。
   * 于是写盘只剩一条路：`saveDoubao`（检测通过之后）。
   */
  dom.doubaoApply.addEventListener('click', () => void saveDoubao())

  /*
   * 这里原来还有「检测豆包」按钮的处理器（`dom.speechTest` → `testDoubao()`）。
   * 按钮与处理器一起删了（用户第 3 条：保存是检测的超集，不需要两个入口）——
   * 想手点一次真请求，点「保存」即可：通了就落盘、绿字「检测通过，已保存」；
   * 不通就一个字不写盘、红字一行 + Key 旁边那句完整原文。
   */
  /* 两个「试听」各自跟着自己那一格（见 previewVoice 的说明） */
  dom.voicePreviewEn.addEventListener('click', () => void previewVoice('en'))
  dom.voicePreviewZh.addEventListener('click', () => void previewVoice('zh'))
  dom.doubaoBalance.addEventListener('click', () => void balanceDoubaoLoudness())
  dom.speechClearCache.addEventListener('click', async () => {
    await api.clearSpeechCache()
    await refreshSpeech()
    toast('已清空发音缓存')
  })

  // 悬浮窗那边改了设置（目前没有，但接口是双向的）也要跟上
  api.onSpeechSettingsChanged(() => void refreshSpeech())
}

/* ==========================================================================
   翻译页（机器翻译）
   ==========================================================================
   两种状态（开发指导文档 §B7），沿用语音页那条硬约定：**置灰 + 说明原因**，
   不许"点了没反应"。但这里的"不能用"有**两种**，必须分开（§B7 硬要求 1）：
     · 缺 Key —— 客观不可用：横幅说清缺什么 + 一个**可点**的跳转，控件置灰并在 title 里写原因；
     · 用户自己关掉的开关 —— 开关本身**永远可点**（那是他的选择）。
   混成一种灰，用户会以为是自己关的，于是反复点那个开关，而真正的原因一直没被说出来。
   ========================================================================== */

let translate: TranslateStatus | null = null
/** 「检测凭据」正在跑（两个请求并行），期间按钮置灰防连点 */
let translateTesting = false

async function refreshTranslate(): Promise<void> {
  try {
    translate = await api.translateStatus()
  } catch (err) {
    console.error('读翻译状态失败', err)
    translate = null
  }
  renderTranslate()
}

function renderTranslate(): void {
  const status = translate
  const hasKey = !!status?.hasApiKey

  /*
   * 缺 Key 的那条横幅与跳转。
   *
   * 这一页**没有** Key 输入框：凭据只在「语音」页填（：凭据不单独成页），
   * 所以这里能做且该做的就是"说清缺什么 + 把人送过去"。
   * 跳转按钮**任何时候都不置灰** —— 它正是解锁条件（§B7 硬要求 2）：
   * 把解锁入口一起禁掉就会死锁（没 Key 就填不了 Key）。
   */
  dom.translateMissing.hidden = hasKey
  dom.translateJumpToSpeech.hidden = hasKey
  dom.translateMissing.textContent = hasKey
    ? ''
    : '机器翻译需要火山引擎凭据，它与语音共用同一把 API Key —— 在「语音」页填一次，两边都能用。'

  const blocked = '需要先在「语音」页填好火山引擎的 API Key（它与语音共用同一把）'
  /*
   * 缺 Key 时要置灰的三样，**写成一个显式清单**。
   *
   * 为什么要有这么个清单而不是三行散在代码里： 第 ⑪ 节
   * 钉的就是"这三样置灰、而且**保留可见**（不是 hidden）、原因写在 title 上"，
   * 以及"**不包括**那个解锁用的跳转按钮"。清单在这儿，那条静态检查标准才有钉得住的对象。
   */
  const blockedRows: [HTMLElement, HTMLElement | null][] = [
    [dom.translateEnabled, dom.translateEnabled.closest('.switch-row')],
    [dom.translateTarget, dom.translateTarget.closest('.field')],
    [dom.translateAuto, dom.translateAuto.closest('.switch-row')]
  ]
  for (const [node, row] of blockedRows) {
    // 置灰而**不是**藏起来：藏起来等于把"为什么不能点"一起藏掉
    node.toggleAttribute('disabled', !hasKey)
    if (row) {
      row.dataset.disabled = String(!hasKey)
      row.title = hasKey ? '' : blocked
    }
  }

  if (status) {
    dom.translateEnabled.checked = status.enabled
    dom.translateTarget.value = status.targetMode
    dom.translateAuto.checked = status.autoTranslate
    /*
     * 缓存计数给用户看"省了多少次计费"。这里报的是**累计**命中/未命中，
     * 不是"缓存里有几条" —— 前者才对应"少花了多少钱"。
     */
    dom.translateCacheNote.textContent =
      `缓存里 ${status.cache.count} 条译文 · 命中 ${status.cache.hits} 次 / 未命中 ${status.cache.misses} 次。` +
      '相同的一句话反复翻译不会再计费。'
  } else {
    dom.translateCacheNote.textContent = '读不到翻译设置。'
  }
}

/** 一行状态的统一写法（与语音页那几行同一套：`data-state` 决定颜色） */
function setStatusLine(node: HTMLElement, text: string, state: 'ok' | 'error' | ''): void {
  node.textContent = text
  node.title = text
  if (state) node.dataset.state = state
  else delete node.dataset.state
}

/**
 * 「检测凭据」：**一个按钮两行字**（§B7 硬要求 3）。
 *
 * 语音合成与机器翻译是同一把 Key、同一个平台，分两个按钮等于让用户点两次，
 * 还要他自己把两个结论拼起来。所以这里并行测两件事，结果分两行写出来。
 *
 * ⚠️ 翻译那一行是**绕过缓存**真发一次请求的（见 C# 的 translate:test）：
 * 走缓存的话它只证明"缓存里有这句话"，Key 被撤销了照样报 ✓。
 */
async function testCredentials(): Promise<void> {
  if (translateTesting) return
  translateTesting = true
  dom.translateTest.disabled = true
  setStatusLine(dom.translateTestSpeech, '语音合成：正在检测…', '')
  setStatusLine(dom.translateTestResult, '机器翻译：正在检测…', '')

  try {
    const [speechResults, translation] = await Promise.all([api.testDoubao(), api.translateTest()])
    renderSpeechTest(speechResults)
    renderTranslateTest(translation)
  } catch (err) {
    const message = err instanceof Error ? err.message : String(err)
    setStatusLine(dom.translateTestSpeech, `语音合成：✗ ${message}`, 'error')
    setStatusLine(dom.translateTestResult, `机器翻译：✗ ${message}`, 'error')
  } finally {
    translateTesting = false
    dom.translateTest.disabled = false
  }
}

function renderSpeechTest(results: DoubaoTest[]): void {
  const list = Array.isArray(results) ? results : []
  const failed = list.find((item) => !item.ok)
  if (failed) {
    setStatusLine(dom.translateTestSpeech, `语音合成：✗ ${failed.error || '请求失败'}`, 'error')
    return
  }
  const first = list[0]
  if (!first) {
    setStatusLine(dom.translateTestSpeech, '语音合成：✗ 没有拿到结果', 'error')
    return
  }
  // 项数一起报：留空音色时后端英文 + 中文各测一次，用户看得见"这两条都通了"
  const bytes = first.bytes >= 1024 ? `${Math.round(first.bytes / 1024)} KB` : `${first.bytes} 字节`
  const suffix = list.length > 1 ? `（${list.length} 项）` : ''
  setStatusLine(dom.translateTestSpeech, `语音合成：✓ ${first.elapsedMs} ms / ${bytes}${suffix}`, 'ok')
}

function renderTranslateTest(result: TranslateTest | null): void {
  if (!result) {
    setStatusLine(dom.translateTestResult, '机器翻译：✗ 没有拿到结果', 'error')
    return
  }
  if (!result.ok) {
    setStatusLine(dom.translateTestResult, `机器翻译：✗ ${result.message || '请求失败'}`, 'error')
    return
  }
  setStatusLine(
    dom.translateTestResult,
    `机器翻译：✓ ${result.elapsedMs} ms / ${result.tokens} tokens`,
    'ok'
  )
}

/** 存一项翻译设置：只发改动的那一个键（与语音页同一条规矩） */
async function saveTranslate(patch: Partial<TranslateSettings>): Promise<void> {
  try {
    const applied = await api.translateSettings(patch)
    translate = translate ? { ...translate, ...applied } : translate
    renderTranslate()
  } catch (err) {
    toast('保存翻译设置失败：' + (err instanceof Error ? err.message : String(err)), 'error')
    await refreshTranslate()
  }
}

function setupTranslate(): void {
  /*
   * 「去『语音』页填写 →」：翻过去，并且**把光标放到 Key 输入框里**。
   * 只翻页不聚焦的话，用户还得自己在那一页找那个框 —— 而这一下点击的意图非常明确。
   */
  dom.translateJumpToSpeech.addEventListener('click', () => {
    switchTab('speech')
    dom.doubaoApiKey.focus()
    dom.doubaoApiKey.scrollIntoView({ block: 'center' })
  })
  dom.translateEnabled.addEventListener('change', () =>
    void saveTranslate({ enabled: dom.translateEnabled.checked })
  )
  dom.translateTarget.addEventListener('change', () =>
    void saveTranslate({ targetMode: dom.translateTarget.value as TranslateTargetMode })
  )
  dom.translateAuto.addEventListener('change', () =>
    void saveTranslate({ autoTranslate: dom.translateAuto.checked })
  )
  dom.translateTest.addEventListener('click', () => void testCredentials())
  dom.translateClearCache.addEventListener('click', async () => {
    await api.clearTranslateCache()
    toast('已清空译文缓存')
    await refreshTranslate()
  })
  // 那边改了（例如语音页里把 Key 清空了），这一页的可用状态要跟着变
  api.onTranslateSettingsChanged(() => void refreshTranslate())
  /*
   * 语音设置一改也要刷这一页：**凭据是共用的**（§B8），用户在语音页填/清 Key，
   * 这一页的"缺 Key"横幅与三个控件的灰/亮立刻就变了 —— 不刷的话，
   * 用户刚填完 Key、切回这一页，看到的还是"缺 Key"那一条，会以为没生效。
   */
  api.onSpeechSettingsChanged(() => void refreshTranslate())
}

async function bootstrap(): Promise<void> {
  dom.titlebarMark.innerHTML = appMarkSvg(26)
  dom.btnMinimize.innerHTML = iconSvg('minimize')
  dom.btnClose.innerHTML = iconSvg('close')
  dom.emptyIcon.innerHTML = iconSvg('library', 26)
  dom.btnAddLarge.innerHTML = `${iconSvg('plus', 15)}<span>添加词典文件</span>`

  /* 加词典只剩这一个入口了（标题栏那个加号已删，见 dom 里那段说明） */
  dom.btnAddLarge.addEventListener('click', () => void addDictionaries())
  dom.btnClose.addEventListener('click', () => api.closeManager())
  dom.btnMinimize.addEventListener('click', () => api.minimizeManager())

  /* 页签：点一下就换页。所有同步的挂载都放在第一个 await 之前（见下面 onTabRequested 的说明）。 */
  dom.tabGeneral.addEventListener('click', () => switchTab('general'))
  dom.tabDicts.addEventListener('click', () => switchTab('dicts'))
  dom.tabSpeech.addEventListener('click', () => switchTab('speech'))
  dom.tabTranslate.addEventListener('click', () => switchTab('translate'))

  /*
   * "打开选项窗并直接翻到某一页"（悬浮窗发音菜单里的「语音设置…」就是这么进来的）。
   *
   * 事件订阅必须在**第一个 await 之前**注册好：主进程是在页面导航完成时把这条消息推出来的，
   * 而模块脚本是 deferred 执行的 —— 只要订阅发生在 await 之前，消息就不会漏。
   * 另外主进程还留了一份"被请求的页签"给开机时主动拉一次（见下面的 initialTab），
   * 两条路都留着，免得某一种打开顺序下翻不过去。
   */
  api.onTabRequested((tab) => {
    if (isTabName(tab)) switchTab(tab)
  })

  /*
   * 上报界面底色。
   * 窗口 Region 只能裁硬边圆角，抗锯齿和投影得靠主进程在外面单独画一层，
   * 那一层要用这个颜色把整块卡片重画一遍，所以必须是实心色。
   */
  reportSurface()
  watchFocus()

  /*
   * 标题栏拖动。
   * Electron 版靠 CSS 的 `-webkit-app-region: drag`，WebView2 不支持这个属性，
   * 所以在标题栏按下鼠标时（按钮除外）让主进程把捕获交给系统的移动循环。
   */
  dom.titlebar.addEventListener('mousedown', (event) => {
    if (event.button !== 0) return
    const target = event.target as HTMLElement | null
    if (target?.closest('button, select, input, a')) return
    event.preventDefault()
    api.startWindowDrag()
  })

  setupSpeech()
  setupTranslate()
  /*
   * 默认落在「词库」页（它排在第一个）—— 理由与 `activeTab` 那段同一处。
   * ⚠️ 这段注释 2026-09 改过一次：它以前写着"从菜单进来的那两条路都**明确请求了 `dicts`**"，
   *    而当时两条菜单传的其实是 `general`（注释与代码不符，是搬运时留下的）。现在两边一致了：
   *    菜单请求 `dicts`，这里的默认也是 `dicts`。
   */
  switchTab('dicts')

  const requested = await api.consumeRequestedTab()
  if (requested !== null && isTabName(requested)) switchTab(requested)

  const behavior = await api.getCloseBehavior()
  dom.closeBehavior.value = behavior
  dom.closeBehavior.addEventListener('change', async () => {
    const next = dom.closeBehavior.value as CloseBehavior
    await api.setCloseBehavior(next)
    toast('已保存关闭行为设置')
  })

  /*
   * 「启动时显示悬浮窗」（2026-09 加，与 `closeBehavior` 同一档的窗口级设置）。
   * ⚠️ 两条与用户直接相关的语义，必须在这里说清（界面上的那行小字也写着）：
   *   · **下一次启动才生效** —— 这一条只管"启动那一瞬间"，不改变眼前这个窗口；
   *   · 关掉之后程序照常启动、托盘图标也在，胶囊只是不摆出来（托盘菜单里点「显示悬浮窗」
   *     或双击托盘图标都能叫出来）。
   * 回包给的是**内核认可的真实值**，写不进去时把勾选退回去（不许"勾上了其实没生效"）。
   */
  const showOnStartup = await api.getShowFloatingOnStartup()
  dom.showOnStartup.checked = showOnStartup
  dom.showOnStartup.addEventListener('change', async () => {
    const wanted = dom.showOnStartup.checked
    dom.showOnStartup.disabled = true
    try {
      const actual = await api.setShowFloatingOnStartup(wanted)
      dom.showOnStartup.checked = actual
      toast(actual ? '启动时会显示悬浮窗' : '启动时不显示悬浮窗（从托盘图标叫出来）')
    } catch (err) {
      // 写不进去就把勾选退回原样：不许"勾上了其实没生效"
      dom.showOnStartup.checked = !wanted
      toast('保存失败：' + (err instanceof Error ? err.message : String(err)), 'error')
    } finally {
      dom.showOnStartup.disabled = false
    }
  })

  /*
   * 「打开配置目录」：路径**问壳要**（只有壳知道这一次跑的是哪个目录），
   * 再把同一个字符串交给 `openInExplorer`（`shell:openPath`，与词库列表里
   * 「打开所在文件夹」是同一条路）。那行小字就是这句路径 ——
   * 「设置 / 历史 / 缓存都在这里」这件事以前只写在打包里的说明文件上。
   */
  void (async () => {
    try {
      const dir = await api.configDir()
      dom.configDirNote.textContent = dir ? `设置、历史、缓存都在 ${dir}` : '读不到配置目录'
    } catch (err) {
      dom.configDirNote.textContent =
        '读不到配置目录：' + (err instanceof Error ? err.message : String(err))
    }
  })()
  dom.openConfigDir.addEventListener('click', async () => {
    try {
      api.openInExplorer(await api.configDir())
    } catch (err) {
      toast('打不开配置目录：' + (err instanceof Error ? err.message : String(err)), 'error')
    }
  })

  /*
   * 「清空查词历史」（2026-09 从**托盘右键菜单**搬来 —— 原话："这种设置应该放深一点的地方"）。
   *
   * 三格两个状态：默认只露「清空查词历史」；点一下换成「确认清空 / 取消」（就地展开，
   * 不弹系统对话框，与词库页「移除」同一个做法）。
   * 为什么必须加这一步：清空历史**不可撤销**，托盘那一版是点一下就清 —— 那是"顺手点一下"的
   * 场景，搬进一个满是按钮的设置页之后，那个脾气就不合适了。
   *
   * 清完弹一句提示条（用户要能看到"真的做了"），并且**不**顺手去动任何别的状态：
   * 历史只影响悬浮窗那一页的「历史」列表，与词库/设置无关。
   */
  const showClearHistoryConfirm = (on: boolean): void => {
    dom.clearHistory.hidden = on
    dom.clearHistoryConfirm.hidden = !on
    dom.clearHistoryCancel.hidden = !on
  }
  dom.clearHistory.addEventListener('click', () => showClearHistoryConfirm(true))
  dom.clearHistoryCancel.addEventListener('click', () => showClearHistoryConfirm(false))
  dom.clearHistoryConfirm.addEventListener('click', async () => {
    try {
      await api.clearHistory()
      showClearHistoryConfirm(false)
      toast('已清空查词历史')
    } catch (err) {
      // 失败**不收起**确认态：用户可以再点一次，也不会以为"清掉了"
      toast('清空失败：' + (err instanceof Error ? err.message : String(err)), 'error')
    }
  })

  /*
   * 开机自动启动。
   * 它与**托盘菜单里那一项是同一个设置**（注册表 Run 项），所以两边都改得动、
   * 也都得跟着另一边变：
   *   · 初值问一次真实状态（`traymenu:state`），**不读本地记忆** ——
   *     注册表可能被组策略挡住，或者程序被挪到别处、Run 项还指着旧路径；
   *   · 勾选后写注册表，回包给的是**系统里的真实状态**，
   *     写不进去时要把勾选退回原样（不许"勾上了其实没生效"）；
   *   · 另一边改了就靠 `onLoginChanged` 就地同步（壳是广播的，见 Dispatch.cs 那段说明）。
   */
  const loginState = await trayApi.getState()
  dom.loginAtStartup.checked = loginState.loginAtStartup
  dom.loginAtStartup.addEventListener('change', () => {
    trayApi.setLoginAtStartup(dom.loginAtStartup.checked)
  })
  trayApi.onLoginChanged((enabled) => {
    dom.loginAtStartup.checked = enabled
  })

  /*
   * 词库清单变了（导入 / 移除 / 改名 / **换当前词典**）→ 词库页重画，而且**当前词典一换就要
   * 把语音页也刷一遍**：那一页里「内置录音」整段都是**按词典**的 ——
   *   · 滑块上是这本词典的增益、标签 title 上是这本词典的名字；
   *   · 「试听」能不能点还要看**这本词典里有没有 apple / 苹果 的录音**（见 refreshDictPreviewWord）。
   * 不刷的话，用户在悬浮窗里换了词典、再回到这一页，看到的是上一本词典的数和判断
   * （实测就是这么发现"试听念的还是上一本的词"的）。
   * 只在**当前词典真的变了**时才刷 —— 别的清单变化（改名之类）没必要重画这一页，
   * 更重要的是别把用户正在输入的音色 ID 冲掉。
   */
  floatingApi.onDictionariesChanged((list) => {
    dictionaries = list
    render()
    const next = (list.find((item) => item.current) || {}).id || ''
    if (next !== currentDictionaryId) {
      currentDictionaryId = next
      void refreshSpeech()
    }
  })

  await refresh()
  await refreshSpeech()
  await refreshTranslate()
}

/*
 * bootstrap 的**顶层兑底**（2026-09-24 补）：里面任何一个 await 拒绝，
 * 以前就是一条 unhandled rejection —— 页面停在半初始化状态、零反馈。
 * 悬浮窗那边早有 bootError 兜底，两个入口的标准应该一样。
 */
void bootstrap().catch((err: unknown) => {
  const message = err instanceof Error ? err.message : String(err)
  console.error('管理窗初始化失败', err)
  try {
    const body = document.body
    if (body) {
      body.dataset.bootError = message
      const note = document.createElement('div')
      note.style.cssText = 'position:fixed;left:12px;bottom:12px;right:12px;padding:10px 14px;' +
        'background:#b3261e;color:#fff;border-radius:8px;font-size:13px;z-index:9999'
      note.textContent = `管理窗初始化失败：${message}`
      body.appendChild(note)
    }
  } catch {
    /* 连 DOM 都没有时只剩控制台那条 */
  }
})
