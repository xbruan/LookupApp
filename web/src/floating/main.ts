import type {
  AbsorbEdge,
  AppliedLayout,
  DictionaryInfo,
  EntryPayload,
  FloatingApi,
  HistoryItem,
  LayoutInfo,
  LookupOrigin,
  LookupStage,
  ShapeRect,
  SpeakResult,
  SpeechOption,
  SpeechStatus,
  SuggestionItem,
  ScriptKind,
  TranslateResult
} from '../../shared/types'
import { SpeechAudioRoute, speechGainDb } from '../shared/audioGraph'
/* 分隔点清单**从接口定义自动生成的文件里读**（唯一来源；见下面 `SEPARATOR_DOT_CODES` 那段注释） */
import { DSH_SEPARATOR_CODEPOINTS, DshConstants } from '../shared/abi'
import { appMarkSvg, iconSvg, type IconName } from './icons'

const api: FloatingApi = window.dshLookup.floating

/* ==========================================================================
   常量
   ========================================================================== */

/** 胶囊与展开面板之间的间距，需要与 CSS 里的 --gap 保持一致 */
const GAP = 8
/** 面板自身占用的额外高度：外边距 + 上下边框 + 少量余量，避免出现无谓的滚动条 */
const PANEL_CHROME = GAP + 4
const LIST_PAD = 12
const LIST_ROW = 44
const LIST_FOOT = 30
const HISTORY_ROW = 42
const HEAD_H = 42
/** 正文模式下 panel-body 的上下 padding */
const CONTENT_PAD = 16
const EMPTY_H = 156
/* 内核常量一律读 abi.ts，禁止在这里抄字面量（抄了就会与内核漂移） */
const MAX_LIST_ROWS = DshConstants.maxListRows

/* 音节分隔点清单的**唯一来源是接口定义**（生成成 abi.ts 的 DSH_SEPARATOR_CODEPOINTS），
 * 不许在这里手抄一份 —— 漂移的后果是"有时能查到、有时查不到"。
 * 词条里的链接（origin='link'）**不跑内核的兜底通道**，所以这里得自己去掉分隔点，
 * 否则词典把词头写成 `dic·tion·ar·y` 时点链接只会得到"查不到"。
 * ⚠️ **不认 `-‐–`** —— 连字符是合法词条字符（`well-known`）。 */
const SEPARATOR_DOT_CODES = DSH_SEPARATOR_CODEPOINTS

function isSeparatorDot(ch: string): boolean {
  const code = ch.codePointAt(0)
  return code !== undefined && SEPARATOR_DOT_CODES.indexOf(code) >= 0
}

/** 这段文字里有没有音节分隔点（没有就别白跑一趟"去掉再问"） */
function hasSeparatorDots(text: string): boolean {
  for (const ch of text || '') {
    if (isSeparatorDot(ch)) return true
  }
  return false
}

/** 去掉分隔点之后的写法（首尾空白也 trim 掉，与内核那一份同约定） */
function withoutSeparatorDots(text: string): string {
  let out = ''
  for (const ch of text || '') {
    if (!isSeparatorDot(ch)) out += ch
  }
  return out.trim()
}
const MAX_HISTORY_ROWS = 10
const CONTENT_MIN = 180
const CONTENT_FALLBACK = 320
const SUGGEST_DEBOUNCE = 80
/* 一律读 DshConstants，禁止手抄字面量 —— 抄一份就会和接口定义漂移 */
const HISTORY_PAGE = DshConstants.historyPage
/**
 * 关闭询问对话框要的展开区高度：真实高度靠 `measureDialogHeight` 量，这里只在"还没量到"时兜底。
 * ⚠️ 别写小 —— 差几像素就够让对话框自己压出一条右侧滚动条（量之前要留宽裕）。
 */
const DIALOG_MIN_HEIGHT = 340
/** 遮罩（.backdrop）的上下内边距，量对话框高度时要一起算上 */
const DIALOG_MARGIN = 10
/** 右键菜单模式下窗口需要的高度：够放下四项菜单 + 与胶囊的间距 */
const MENU_PANEL_HEIGHT = 200
/** 量出来的菜单高度之外再留一点余量，免得卡片边缘正好贴着菜单最后一项 */
const MENU_PANEL_MARGIN = 12

type Mode = 'idle' | 'list' | 'content' | 'history' | 'dialog' | 'menu'

/* ==========================================================================
   DOM
   ========================================================================== */

function el<T extends HTMLElement>(id: string): T {
  const node = document.getElementById(id)
  if (!node) throw new Error(`缺少 DOM 节点 #${id}`)
  return node as T
}

const dom = {
  stage: el<HTMLDivElement>('stage'),
  strip: el<HTMLDivElement>('strip'),
  content: el<HTMLDivElement>('content'),
  panel: el<HTMLElement>('panel'),
  panelHead: el<HTMLElement>('panelHead'),
  panelBody: el<HTMLDivElement>('panelBody'),
  entryWord: el<HTMLSpanElement>('entryWord'),
  entryDict: el<HTMLSpanElement>('entryDict'),
  entryBack: el<HTMLButtonElement>('entryBack'),
  entryBackLink: el<HTMLButtonElement>('entryBackLink'),
  entryTop: el<HTMLButtonElement>('entryTop'),
  entrySpeak: el<HTMLButtonElement>('entrySpeak'),
  entryCopy: el<HTMLButtonElement>('entryCopy'),
  entryClose: el<HTMLButtonElement>('entryClose'),
  suggestList: el<HTMLUListElement>('suggestList'),
  suggestFoot: el<HTMLDivElement>('suggestFoot'),
  suggestCount: el<HTMLSpanElement>('suggestCount'),
  emptyHint: el<HTMLDivElement>('emptyHint'),  historyList: el<HTMLUListElement>('historyList'),
  reader: el<HTMLDivElement>('reader'),
  entryFrame: el<HTMLIFrameElement>('entryFrame'),
  readerLoading: el<HTMLDivElement>('readerLoading'),
  pill: el<HTMLDivElement>('pill'),
  grip: el<HTMLButtonElement>('grip'),
  input: el<HTMLInputElement>('input'),
  spinner: el<HTMLSpanElement>('spinner'),
  meta: el<HTMLSpanElement>('meta'),
  btnHistory: el<HTMLButtonElement>('btnHistory'),
  btnClear: el<HTMLButtonElement>('btnClear'),
  ctxMenu: el<HTMLDivElement>('ctxMenu'),
  selToolbar: el<HTMLDivElement>('selToolbar'),
  closeBackdrop: el<HTMLDivElement>('closeBackdrop'),
  dialog: el<HTMLDivElement>('closeDialog'),
  choiceTray: el<HTMLButtonElement>('choiceTray'),
  choiceQuit: el<HTMLButtonElement>('choiceQuit'),
  closeRemember: el<HTMLInputElement>('closeRemember'),
  closeCancel: el<HTMLButtonElement>('closeCancel'),
  toast: el<HTMLDivElement>('toast'),
  /** 正文框里的反馈条（`.reader` 的子元素）：选中查词"查不到 / 就是当前词条"时说在这儿 */
  readerToast: el<HTMLDivElement>('readerToast'),
  /* 兜底通道的解释行（这一页怎么来的）—— 宿主自己画，正文在跨域 iframe 里读不到 */
  readerVia: el<HTMLDivElement>('readerVia')
}

/* ==========================================================================
   状态
   ========================================================================== */

interface State {
  mode: Mode
  query: string
  suggestions: SuggestionItem[]
  /**
   * `suggestions` 是照着**哪个输入内容**算出来的。
   * 回车该"直接查"还是"先联想"全看它：只有候选是照输入框里当前这段文字算的，才能直接采纳候选，
   * 否则先联想一遍 —— 不记这一笔就会出现"回车查了一串没打完的半个词"。
   */
  suggestionsFor: string
  activeIndex: number
  dictionaries: DictionaryInfo[]
  currentDictId: string | null
  entry: EntryPayload | null
  /** 正文 iframe 上报的实际文档高度 */
  contentHeight: number
  history: HistoryItem[]
  historyTotal: number
  historyLoading: boolean
  busy: boolean
  layoutInfo: LayoutInfo | null
  pillWidth: number
  pillHeight: number
  /** 胶囊在窗口内的上边距（DIP），由主进程按它的屏幕位置算出来 */
  pillTop: number
  shadow: number
  /** 右键菜单锚点：相对胶囊左上角的偏移，避免窗口重排后坐标失效 */
  menuAnchor: { dx: number; dy: number } | null
  /** 打开右键菜单之前的模式，关掉菜单后恢复 */
  modeBeforeMenu: Mode
  /** 已从屏幕边缘弹出但仍贴着该边 —— 鼠标移开会重新吸入 */
  docked: boolean
  /** 当前贴着的边缘 */
  absorbEdge: AbsorbEdge
  /**
   * 链接跳转的返回栈（每次"从输入框 / 候选 / 历史查词"都会清空；点 entry:// 链接时压栈）。
   * 每条记三样：词条、**离开时读到哪个位置**（宿主够不着跨域 iframe 的 scrollTop）、
   * 以及**它属于哪本词典** —— `null` = 当前词典；不带 dictId 退出会查进当前词典，像退错了词条。
   */
  navStack: { word: string; scrollY: number; dictId: string | null; dictTitle: string }[]
  /**
   * 本次查词是不是从输入框发起的（输入框回车 / 点候选列表）。它描述的是**一整轮查询**：
   * 之后在正文里点几次链接、按几次「返回」都不改变它，「返回候选」要一直可用。
   * 从历史记录里点开的不算（那份候选列表跟它没关系）。
   */
  entryFromInput: boolean
  /**
   * 当前这篇正文是从**哪本词典**来的；`null` = 就是当前词典。
   * 「借查」（在别的词典里查一次、**不切换当前词典**）之后，正文里的 entry:// 链接与
   * 「查词」按钮若只按词名问当前词典，就会跳进另一本词典 —— 所以这一轮用哪本要跟着正文记着。
   */
  entryDictId: string | null
  /** 跨词条跳转带来的锚点（`entry://word#sense3`）：iframe 加载完成后取走转发给正文，然后清空 */
  pendingFragment: string
  /**
   * 点「返回」要还原的滚动位置（与 pendingFragment 一样是一次性的，转发后即清空）。
   * `null` = 这次不需要还原（新查询、或当初离开时就在顶端）。
   */
  pendingScrollY: number | null
  /** 关闭询问对话框量出来的高度（含遮罩内边距）；0 表示还没量过 */
  dialogHeight: number
  /**
   * 右键菜单量出来的高度（和对话框一样"量出来再给窗口"）。
   * 发音菜单有"音源 + 语种 + 设置"三组、能到六七项，继续吃固定的 200px 会把最后一项裁掉
   * （菜单是 fixed 定位浮在窗口里的，窗口不够高就看不见）。
   */
  menuHeight: number
  /** 发音 */
  speech: SpeechUiState
}

/**
 * 一次朗读每段的长度上限（字）。
 * ⚠️ 别改成"一次合成整段"：合成是**先出完整段音频**才开始播的，选一段两千字要等好几秒、
 * 音频也有一两 MB；切成三百字一段之后第一段几百毫秒就能开口，后面的在播放间隙预取好。
 */
const SPEECH_CHUNK_CHARS = DshConstants.speechChunkChars

/** 发音在界面上的状态 */
interface SpeechUiState {
  /** 对当前词条的音源规划；null = 还没算出来（或当前没有词条） */
  status: SpeechStatus | null
  /** 正在请求音频（合成或联网，几百毫秒量级） */
  busy: boolean
  /** 正在出声 */
  playing: boolean
  /** 上一次失败的原因，挂在按钮 title 上便于排查 */
  error: string
  /** 上一次成功播放用的是什么（标题提示与自动化断言都会看它） */
  lastSource: string
  lastVoice: string
  /**
   * 当前 `<audio>` 里这段音频是不是"发音按钮要的那一段"。
   * 播放器是全局唯一的（点词条里的 🔊 会顶掉正在念的发音），而播放状态只认媒体事件，
   * 所以必须记着"这次是谁的"，否则点一下例句音频，发音按钮会一直显示"正在出声"。
   */
  ownSrc: string
  /** 这一次朗读的分段计划（**永远非空**，一个词就是一段）；上一条音频放完后由它决定有没有下一段 */
  plan: SpeechPlan | null
}

/** 一次朗读的分段计划：念哪几段、念到第几段、下一段预取到了没有 */
interface SpeechPlan {
  chunks: string[]
  /** 正在念/刚念完第几段（0 起）。-1 = 还没开始 */
  index: number
  /** 这一趟的令牌：停止、重新发音都会让老结果作废 */
  token: number
  /** 每一段都要用同一套参数（音源、音色、语种、词条上下文） */
  context: SpeechContext
  /** 预取到的下一段（段与段之间就没空档了） */
  next: { index: number; result: SpeakResult } | null
}

/** 一段音频要按什么去要（分段朗读时每一段共用） */
interface SpeechContext {
  options: { source?: 'dict' | 'system' | 'online'; voiceId?: string; language?: string; text?: string }
  useEntryAudio: boolean
  token: number
}

const state: State = {
  mode: 'idle',
  query: '',
  suggestions: [],
  suggestionsFor: '',
  activeIndex: -1,
  dictionaries: [],
  currentDictId: null,
  entry: null,
  contentHeight: CONTENT_FALLBACK,
  history: [],
  historyTotal: 0,
  historyLoading: false,
  busy: false,
  layoutInfo: null,
  pillWidth: 464,
  pillHeight: 52,
  pillTop: 0,
  shadow: 0,
  /** 右键菜单锚点：相对胶囊左上角的偏移，避免窗口重排后坐标失效 */
  menuAnchor: null,
  /** 打开右键菜单之前的模式，关掉菜单后恢复 */
  modeBeforeMenu: 'idle',
  docked: false,
  absorbEdge: null,
  navStack: [],
  entryFromInput: false,
  entryDictId: null,
  pendingFragment: '',
  pendingScrollY: null,
  dialogHeight: 0,
  menuHeight: 0,
  speech: {
    status: null,
    busy: false,
    playing: false,
    error: '',
    lastSource: '',
    lastVoice: '',
    ownSrc: '',
    plan: null
  }
}

/** 请求序号，用于丢弃过期的联想结果 */
let suggestToken = 0
let suggestTimer: number | null = null
/** 最近一次查词 IPC 失败的人话原因（catch-all 提示用它；成功查到词就作废） */
let lastLookupError = ''
let toastTimer: number | null = null
/** 正文里那条反馈条的计时器（与窗口那条各算各的，互不干扰） */
let readerToastTimer: number | null = null

/* ==========================================================================
   布局
   ========================================================================== */

function clamp(value: number, min: number, max: number): number {
  if (max < min) return max
  return Math.min(Math.max(value, min), max)
}

function maxPanelHeight(): number {
  // 窗口上下预留了多少空间，面板最多就只能占多少。
  // 用 window.innerHeight 而不是工作区高度：面板必须落在窗口内，否则会被窗口裁掉。
  return Math.max(0, panelCapacity() - PANEL_CHROME)
}

/**
 * 胶囊上方 / 下方在**窗口内**分别还有多少空间。
 * 窗口上下都预留了面板的位置（见 FloatingWindow.NormalGeometry），所以这个值和"屏幕工作区还剩
 * 多少"不是一回事 —— 面板只能往窗口里长。
 */
function panelSpace(): { up: number; down: number } {
  const inner = window.innerHeight || state.pillHeight
  return {
    up: Math.max(0, state.pillTop - GAP),
    down: Math.max(0, inner - state.pillTop - state.pillHeight - GAP)
  }
}

function panelCapacity(): number {
  const space = panelSpace()
  return Math.max(space.up, space.down)
}

/**
 * 面板往哪边弹：只取决于窗口内上下哪边更宽裕。胶囊在窗口里的位置由主进程给（pillTop），
 * 所以这个判断与窗口几何完全解耦 —— 换方向只动面板，胶囊纹丝不动。
 */
function chooseDirection(panelHeight: number): 'up' | 'down' {
  if (panelHeight <= 0) return 'down'
  const space = panelSpace()
  return space.up >= space.down ? 'up' : 'down'
}

function maxContentHeight(): number {
  return Math.max(CONTENT_MIN, maxPanelHeight() - PANEL_CHROME - HEAD_H - CONTENT_PAD)
}

/** 当前模式"想要"的面板高度（未按可用空间收敛） */
function rawPanelHeight(): number {
  switch (state.mode) {
    case 'idle':
      return 0
    case 'dialog':
      /*
       * 对话框铺满窗口，窗口必须撑到能装下它，否则下半截被窗口边界裁掉、或被对话框自己的
       * max-height 压出一条滚动条。高度是**量出来的**（见 measureDialogHeight）：写死一个数
       * 会在别的机器上差几像素，而差这几像素的表现正好就是"右边多出一条滚动条"。
       */
      return PANEL_CHROME + (state.dialogHeight || DIALOG_MIN_HEIGHT)
    case 'menu':
      // 右键菜单同理：菜单浮在胶囊外侧，窗口不留高度就会被裁掉。
      // 高度按菜单实际量出来的值给（见 state.menuHeight），常量只当"还没量到"时的下限。
      return PANEL_CHROME + Math.max(MENU_PANEL_HEIGHT, state.menuHeight + MENU_PANEL_MARGIN)
    case 'list': {
      if (state.suggestions.length === 0) return PANEL_CHROME + EMPTY_H
      const rows = Math.min(state.suggestions.length, MAX_LIST_ROWS)
      return PANEL_CHROME + LIST_PAD + rows * LIST_ROW + LIST_FOOT
    }
    case 'history': {
      const rows = Math.min(Math.max(state.history.length, 1), MAX_HISTORY_ROWS)
      return PANEL_CHROME + LIST_PAD + rows * HISTORY_ROW + LIST_FOOT
    }
    case 'content': {
      const content = clamp(state.contentHeight, CONTENT_MIN, maxContentHeight())
      return PANEL_CHROME + HEAD_H + CONTENT_PAD + content
    }
  }
  return 0
}

/** 最终请求的面板高度：面板只能长在窗口预留出来的空间里，超了会被窗口裁掉，所以必须收敛一次 */
function desiredPanelHeight(): number {
  const raw = rawPanelHeight()
  if (raw <= 0) return 0
  return Math.min(raw, maxPanelHeight() + PANEL_CHROME)
}

/** 面板宽度始终跟随胶囊宽度，内容框与输入框左右对齐 */
function desiredWidth(): number {
  return state.pillWidth
}

/**
 * 胶囊当前所在的屏幕坐标（DIP）与它在窗口内的上边距。
 * 主进程每次推送生效布局时都会带上这些值，getLayoutInfo() 也会给一份，所以基本总是最新的
 * —— 展开方向的判断全靠它。
 */
let pillX = 0
let pillY = 0

function syncPillPosition(
  source: { pillX?: number; pillY?: number; pillTop?: number } | null | undefined
): void {
  if (!source) return
  if (typeof source.pillX === 'number') pillX = source.pillX
  if (typeof source.pillY === 'number') pillY = source.pillY
  if (typeof source.pillTop === 'number') state.pillTop = source.pillTop
}

/**
 * 这个模式下**面板（内容框 / 候选列表）是不是露着的**。
 * 多处地方要对同一个问题给同一个答案（render() 里藏面板、给提示条定位置），所以约定只写
 * 一遍 —— 免得"面板藏了但提示还以为它开着"。
 */
function panelVisibleIn(mode: Mode): boolean {
  return mode === 'list' || mode === 'content' || mode === 'history'
}

/**
 * 把胶囊和面板摆到窗口里的正确位置（两者都是绝对定位：胶囊钉在 pillTop，面板贴在它上/下方），
 * 顺带把提示条停在"离输入框最远的那一头"（写 `--toast-bottom` + `data-dock`，理由见 floating.css）。
 * ⚠️ 别改回 flex 排版（column / column-reverse + justify-content）：那时胶囊位置跟着窗口尺寸走，
 * 窗口一重排就得跟着挪，很容易闪。
 */
function applyPanelGeometry(mode: Mode, panelHeight: number): { direction: 'up' | 'down'; panelTop: number } {
  const direction = chooseDirection(panelHeight)
  // PANEL_CHROME 里含了胶囊与面板之间的 GAP，这里要减掉才是面板元素自身的高度
  const panelElementHeight = Math.max(0, panelHeight - GAP)
  const panelTop =
    direction === 'up'
      ? state.pillTop - GAP - panelElementHeight
      : state.pillTop + state.pillHeight + GAP

  /*
   * 变量挂在 <html> 上而不是 .stage 上：关闭询问对话框的浮层是 .stage 的兄弟节点，
   * 挂在 .stage 上它继承不到，对话框就会跑到整个窗口的正中间、离胶囊老远。
   */
  const style = document.documentElement.style
  style.setProperty('--pill-top', `${state.pillTop}px`)
  style.setProperty('--panel-top', `${panelTop}px`)
  style.setProperty('--panel-h', `${panelElementHeight}px`)
  dom.content.dataset.direction = direction
  if (mode) dom.content.dataset.mode = mode

  /*
   * 提示条落点：面板露着时量"面板下沿离窗口下沿有多远"再加 8px，让**整条都在面板里面**
   * （面板底下不透明，越界那一段会被裁掉）；`Math.max(8, …)` 兜底，别算到窗口外面去。
   */
  const winHeight = window.innerHeight || 0
  if (panelVisibleIn(mode)) {
    const panelBottom = panelTop + panelElementHeight
    const gapToWindowBottom = winHeight > 0 ? winHeight - panelBottom + 8 : 8
    style.setProperty('--toast-bottom', `${Math.max(8, Math.round(gapToWindowBottom))}px`)
    dom.toast.dataset.dock = 'panel'
  } else {
    // 面板收着：贴到胶囊的另一头去（展开方向向下 = 胶囊在上 → 贴窗口底部）
    if (direction === 'down') {
      style.setProperty('--toast-bottom', '8px')
      dom.toast.dataset.dock = 'bottom'
    } else {
      dom.toast.dataset.dock = 'top'
    }
  }

  return { direction, panelTop }
}

/** 把当前模式对应的尺寸告诉主进程 */
function pushLayout(): void {
  const panelHeight = desiredPanelHeight()
  const placement = applyPanelGeometry(state.mode, panelHeight)

  api.setLayout({
    width: desiredWidth(),
    panelHeight,
    direction: placement.direction,
    align: state.absorbEdge === 'right' ? 'right' : state.layoutInfo?.edge === 'right' ? 'right' : 'left'
  })
}

/* ==========================================================================
   窗口形状上报
   ==========================================================================

   窗口没有逐像素透明：可见范围靠主进程用 SetWindowRgn 裁出来，而"哪些地方该可见"只有
   渲染进程说得准 —— 胶囊 / 面板 / 菜单 / 选区按钮 / 对话框的包围盒在这里量好报过去。
   ⚠️ 吸入、弹出、面板展开都是 CSS 过渡，形状每帧都在变，所以变化后要开一个 rAF 窗口逐帧量
   （只在开始和结束各报一次的话，中间那段会被裁错）。
   ========================================================================== */

/** 每个可见块用哪种圆角（要和 CSS 里写的保持一致），以及取色用的选择器 */
const SHAPE_TARGETS: Array<{ node: () => HTMLElement; radius: number; surface: 'card' | 'sub' }> = [
  { node: () => dom.strip, radius: 6, surface: 'card' },
  { node: () => dom.panel, radius: 18, surface: 'card' },
  { node: () => dom.pill, radius: 26, surface: 'card' },
  { node: () => dom.ctxMenu, radius: 12, surface: 'sub' },
  { node: () => dom.selToolbar, radius: 10, surface: 'sub' },
  { node: () => dom.dialog, radius: 18, surface: 'card' }
]

let shapeBurstUntil = 0
let shapeRaf = 0
let lastShapeKey = ''
/* 灰字（词典名）上次量宽度时的文本 —— 见 syncMetaSpace()：量宽度会强制同步布局，别每次渲染都量 */
let lastMetaSpaceKey = ''

/** 解析 rgb()/rgba() 出来的颜色 */
interface Rgba {
  r: number
  g: number
  b: number
  a: number
}

function parseColor(value: string): Rgba | null {
  const text = (value || '').trim()
  // #rgb / #rrggbb：#3b6ef6 这种来自 CSS 变量的写法要单独认，
  // 否则 --accent 读出来是十六进制、正则匹配不上，光圈会悄悄退回兜底色
  const hex = /^#([0-9a-f]{3}|[0-9a-f]{6})$/i.exec(text)
  if (hex) {
    const body = hex[1].length === 3 ? hex[1].replace(/./g, (c) => c + c) : hex[1]
    return {
      r: Number.parseInt(body.slice(0, 2), 16),
      g: Number.parseInt(body.slice(2, 4), 16),
      b: Number.parseInt(body.slice(4, 6), 16),
      a: 1
    }
  }
  const match = /rgba?\(\s*([\d.]+)\s*,\s*([\d.]+)\s*,\s*([\d.]+)\s*(?:,\s*([\d.]+)\s*)?\)/.exec(text)
  if (!match) return null
  return {
    r: Number(match[1]),
    g: Number(match[2]),
    b: Number(match[3]),
    a: match[4] === undefined ? 1 : Number(match[4])
  }
}

/**
 * 把半透明的前景色合成到不透明底色上。
 * 主进程那层"外壳"是逐像素透明的分层窗口，它只会把描边**填**在卡片边缘，没法让半透明的
 * 描边去和底色混合（那底下是投影和桌面，不是卡片）—— 所以在这里先算好合成结果再报过去。
 */
function compositeOver(foreground: string, background: Rgba): string {
  const fg = parseColor(foreground)
  const base = background ?? { r: 255, g: 255, b: 255, a: 1 }
  if (!fg) return `rgb(${base.r}, ${base.g}, ${base.b})`
  const a = Math.max(0, Math.min(1, fg.a))
  const r = Math.round(fg.r * a + base.r * (1 - a))
  const g = Math.round(fg.g * a + base.g * (1 - a))
  const b = Math.round(fg.b * a + base.b * (1 - a))
  return `rgb(${r}, ${g}, ${b})`
}

function currentTheme(): 'light' | 'dark' {
  return window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches
    ? 'dark'
    : 'light'
}

function measureShape(): ShapeRect[] {
  const rects: ShapeRect[] = []
  for (const target of SHAPE_TARGETS) {
    const node = target.node()
    if (!node || node.hidden) continue
    const rect = node.getBoundingClientRect()
    if (rect.width < 1 || rect.height < 1) continue

    const style = getComputedStyle(node)
    const fill = parseColor(style.backgroundColor)
    // 底色必须是实心的：外壳填的就是这个颜色，半透明的话外面那圈会透出桌面
    const opaque = fill && fill.a >= 0.999 ? fill : { r: 255, g: 255, b: 255, a: 1 }
    const borderWidth = Number.parseFloat(style.borderTopWidth)

    rects.push({
      x: rect.left,
      y: rect.top,
      w: rect.width,
      h: rect.height,
      r: target.radius,
      fill: `rgb(${opaque.r}, ${opaque.g}, ${opaque.b})`,
      border: borderWidth > 0 ? compositeOver(style.borderTopColor, opaque) : `rgb(${opaque.r}, ${opaque.g}, ${opaque.b})`
    })
  }
  return rects
}

/** 胶囊是否处于聚焦态。只影响投影浓度 —— 焦点提示不做彩色描边（那圈彩色光环看着太吵） */
function pillFocused(): boolean {
  return dom.pill.dataset.focused === 'true'
}

function pushShape(): void {
  const rects = measureShape()
  // 键里必须带上取色和聚焦状态：换主题、胶囊获得/失去焦点时形状没变，
  // 但外壳得重画一遍（投影浓度不一样了）
  const focused = pillFocused()
  const key =
    rects
      .map((r) => `${Math.round(r.x)},${Math.round(r.y)},${Math.round(r.w)},${Math.round(r.h)},${r.fill},${r.border}`)
      .join(';') +
    '|' + currentTheme() +
    '|' + focused
  if (key === lastShapeKey) return
  lastShapeKey = key
  api.setShape(rects, currentTheme(), focused)
}

function shapeTick(): void {
  shapeRaf = 0
  pushShape()
  if (performance.now() < shapeBurstUntil) shapeRaf = requestAnimationFrame(shapeTick)
}

/** 窗口内容刚变过：接下来一小段时间逐帧跟踪形状 */
function scheduleShape(duration = 420): void {
  shapeBurstUntil = Math.max(shapeBurstUntil, performance.now() + duration)
  if (!shapeRaf) shapeRaf = requestAnimationFrame(shapeTick)
}

function setupShapeReporting(): void {
  // 布局/尺寸变化
  if (typeof ResizeObserver !== 'undefined') {
    const observer = new ResizeObserver(() => scheduleShape())
    for (const target of SHAPE_TARGETS) observer.observe(target.node())
  }
  // 显示/隐藏（hidden 属性）与列表内容变化
  if (typeof MutationObserver !== 'undefined') {
    const observer = new MutationObserver(() => scheduleShape())
    observer.observe(document.body, {
      subtree: true,
      childList: true,
      attributes: true,
      attributeFilter: ['hidden', 'style', 'class', 'data-mode', 'data-direction', 'data-align', 'data-focused']
    })
  }
  // 兜底：低频复查一次，覆盖上面两种观察都漏掉的位移（比如菜单重新定位）
  window.setInterval(() => scheduleShape(0), 250)
  pushShape()
}

function applyAppliedLayout(applied: AppliedLayout): void {
  state.pillWidth = applied.pillWidth
  state.pillHeight = applied.pillHeight
  state.shadow = applied.shadow
  state.docked = applied.docked
  state.absorbEdge = applied.absorbEdge
  // 胶囊在窗口里的位置由主进程给：窗口上下都预留了面板的空间，
  // 所以这个值随胶囊自身的屏幕位置而变，必须照用
  syncPillPosition(applied)
  if (applied.align) dom.content.dataset.align = applied.align
  dom.stage.style.setProperty('--shadow', `${applied.shadow}px`)
  document.documentElement.style.setProperty('--pill-w', `${applied.pillWidth}px`)
  document.documentElement.style.setProperty('--pill-h', `${applied.pillHeight}px`)
  // 窗口尺寸变了，面板能占的空间也跟着变；重算一次位置和方向
  applyPanelGeometry(state.mode, desiredPanelHeight())

  // 吸入/弹出动画：让胶囊收窄/张开到恰好落在窄条的位置
  applyEdgeAnimation(applied.edgeAnim, applied.absorbEdge)
  if (applied.absorbEdge) dom.content.dataset.edge = applied.absorbEdge

  // 吸入状态：整块界面让位给贴边窄条
  dom.strip.hidden = !applied.absorbed
  dom.content.hidden = applied.absorbed
  if (applied.absorbed && applied.absorbEdge) {
    dom.strip.dataset.edge = applied.absorbEdge
  }
  if (applied.absorbed) {
    /* 整块界面收进屏幕边缘了，"浮在胶囊外侧"的浮层（正文那排按钮挂在 <body> 上）
       没有理由还留在外面 —— 不主动收会被一起吸走、hover 唤回来时挡在界面上。
       ⚠️ 用 dismiss 而不是 hide：吸入/弹出会触发 resize → 重摆浮层，hide 会被立刻重新显示出来。 */
    dismissSelectionToolbar()
    hideContextMenu()
  } else {
    cancelReabsorb()
  }
  // 窗口重排后菜单的绝对坐标会变，用相对胶囊的锚点重新摆一次
  positionContextMenu()
  scheduleShape()
}

/* ==========================================================================
   渲染
   ========================================================================== */

function render(): void {
  const mode = state.mode

  dom.panel.hidden = !panelVisibleIn(mode)
  dom.panelHead.hidden = mode !== 'content'
  dom.panelBody.dataset.mode = mode === 'content' ? 'content' : 'list'
  dom.suggestList.hidden = mode !== 'list'
  dom.emptyHint.hidden = !(mode === 'list' && state.suggestions.length === 0)
  dom.historyList.hidden = mode !== 'history'
  dom.reader.hidden = mode !== 'content'
  dom.suggestFoot.hidden = !(
    (mode === 'list' && state.suggestions.length > 0) ||
    mode === 'history'
  )
  dom.suggestCount.textContent =
    mode === 'list' ? `${state.suggestions.length} 个候选` : `${state.historyTotal} 条记录`

  dom.spinner.hidden = !state.busy
  dom.pill.dataset.focused = String(document.activeElement === dom.input)
  // 「清空输入」的可用状态跟着输入框内容走（兜底：别处改了输入框也能刷到）
  updateClearButton()
  // 发音按钮同理：换词条、加载中、设置变了都可能改变它的可用状态，渲染时兜一次底
  updateSpeakButton()

  renderMeta()
  scheduleShape()
}

function renderMeta(): void {
  /* 只有一处出口：先把文案定下来，最后**统一**交给 syncMetaSpace() 去量宽度、让输入框的文字区
     跟着让位（原来五个分支各自 return，量宽度那一步就没地方放）。 */
  let text: string
  if (state.dictionaries.length === 0) {
    text = '未添加词库'
    dom.meta.dataset.state = 'error'
  } else {
    const current = state.dictionaries.find((d) => d.id === state.currentDictId)
    dom.meta.dataset.state = 'ok'
    if (!current) {
      text = '点击左侧图标选择词库'
    } else if (current.status === 'loading' || current.status === 'pending') {
      text = `${current.title} · 载入中`
    } else if (current.status === 'error') {
      text = `${current.title} · 载入失败`
      dom.meta.dataset.state = 'error'
    } else {
      text = current.title
    }
  }
  dom.meta.textContent = text
  syncMetaSpace()
}

/* 灰字（词典名）能占多宽，输入框的文字区就让出多宽：`#meta` 是**浮在输入框上面的绝对定位层**，
 * 只把它的 max-width 放宽会让长名字钻进用户正在打的字底下，所以按**量到的实际宽度**同步
 * padding-right（输入框的框仍铺满整条胶囊，点 / 右键那片空白照旧命中输入框）。
 * ⚠️ 量宽度会强制同步布局，按"文本没变就不重量"缓存；量出来是 0 说明胶囊还没露出来，别记缓存。 */
function syncMetaSpace(): void {
  const width = dom.meta.getBoundingClientRect().width
  if (width <= 0) return
  const key = dom.meta.textContent || ''
  if (key === lastMetaSpaceKey) return
  lastMetaSpaceKey = key
  const pillStyle = getComputedStyle(dom.pill)
  const trailing = parseFloat(pillStyle.getPropertyValue('--pill-trailing-w')) || 148
  const gap = parseFloat(pillStyle.getPropertyValue('--pill-gap')) || 4
  const base = trailing + gap + 2
  dom.input.style.paddingRight = `${Math.round(Math.max(base, width + 10))}px`
}

function renderSuggestionList(): void {
  const list = dom.suggestList
  list.textContent = ''
  state.suggestions.forEach((item, index) => {
    const li = document.createElement('li')
    li.className = 'suggest-item'
    li.dataset.active = String(index === state.activeIndex)
    li.dataset.kind = item.kind

    const word = document.createElement('span')
    word.className = 'suggest-word'
    word.textContent = item.word
    li.append(word)

    if (item.kind === 'fuzzy' || item.kind === 'associate') {
      const badge = document.createElement('span')
      badge.className = 'suggest-kind'
      badge.textContent = item.kind === 'fuzzy' ? '拼写建议' : '关联词'
      li.append(badge)
    }

    li.addEventListener('mouseenter', () => {
      if (state.activeIndex === index) return
      state.activeIndex = index
      paintActiveSuggestion()
    })
    li.addEventListener('mousedown', (event) => {
      // 阻止输入框失焦，否则先触发 blur 收起
      event.preventDefault()
      void lookup(item.word, { source: 'input' })
    })
    list.append(li)
  })
}

function paintActiveSuggestion(): void {
  const items = dom.suggestList.children
  for (let i = 0; i < items.length; i++) {
    ;(items[i] as HTMLElement).dataset.active = String(i === state.activeIndex)
  }
  const active = items[state.activeIndex] as HTMLElement | undefined
  active?.scrollIntoView({ block: 'nearest' })
}

function renderHistory(): void {
  const list = dom.historyList
  list.textContent = ''
  if (state.history.length === 0 && !state.historyLoading) {
    const hint = document.createElement('li')
    hint.className = 'history-more'
    hint.textContent = '还没有查词记录'
    list.append(hint)
    return
  }

  for (const item of state.history) {
    const li = document.createElement('li')
    li.className = 'history-item'

    const word = document.createElement('span')
    word.className = 'history-word'
    word.textContent = item.word

    const dict = document.createElement('span')
    dict.className = 'history-dict'
    /* 词典名按 dictId 现查一遍：历史条目里存的是查词那一刻的名字快照，用户后来在管理窗
       改了名的话，这里要跟着显示新名字。 */
    const named = state.dictionaries.find((d) => d.id === item.dictId)
    dict.textContent = named ? named.title : item.dictTitle

    const time = document.createElement('span')
    time.className = 'history-time'
    time.textContent = formatTime(item.at)

    li.append(word, dict, time)
    li.addEventListener('mousedown', (event) => {
      event.preventDefault()
      void replayHistoryItem(item)
    })
    list.append(li)
  }

  if (state.history.length < state.historyTotal) {
    const more = document.createElement('li')
    more.className = 'history-more'
    more.textContent = state.historyLoading
      ? '正在加载…'
      : `已显示 ${state.history.length} / ${state.historyTotal} 条，滚动加载更多`
    list.append(more)
  }
}

function renderEmptyHint(): void {
  const box = dom.emptyHint
  box.textContent = ''

  if (state.dictionaries.length === 0) {
    const title = document.createElement('strong')
    title.textContent = '还没有词库'
    const desc = document.createElement('span')
    desc.textContent = '添加 .mdx 词条库（可同时选择配套 .mdd 资源库）即可开始查词'
    const action = document.createElement('button')
    action.className = 'link-chip'
    action.type = 'button'
    action.textContent = '添加词典文件…'
    action.addEventListener('mousedown', (event) => {
      event.preventDefault()
      void addDictionaries()
    })
    box.append(title, desc, action)
    return
  }

  const current = state.dictionaries.find((d) => d.id === state.currentDictId)
  const title = document.createElement('strong')
  title.textContent = state.query ? `没有匹配「${state.query}」的条目` : '输入以开始查词'
  const desc = document.createElement('span')
  desc.textContent = current ? `当前词库：${current.title}` : '请先选择一本词典'
  box.append(title, desc)
}

function showReaderLoading(show: boolean): void {
  dom.readerLoading.hidden = !show
}

function toast(message: string, tone: 'info' | 'error' = 'info'): void {
  dom.toast.textContent = message
  dom.toast.dataset.tone = tone
  dom.toast.dataset.show = 'true'
  if (toastTimer !== null) window.clearTimeout(toastTimer)
  toastTimer = window.setTimeout(() => {
    dom.toast.dataset.show = 'false'
    toastTimer = null
  }, 1800)
}

/**
 * 正文框那条提示上的**可点出路**（候选 / 再问一遍）。
 * 为什么必须可点：终态只给一句「未查到」等于把话说到死路上 —— 用户手上明明还有这两张牌。
 */
interface ReaderToastAction {
  /** 按钮上的字（就是用户看到的那句话，例如 `apple` 或「再问一遍」） */
  label: string
  /** 点下去干什么：`lookup`（按 `word` 查一次）/ `recheck`（把没问完的词典重问一遍） */
  action: 'lookup' | 'recheck'
  /** `lookup` 要查的词 */
  word?: string
  /** `lookup` 从哪本词典起查（终态那一轮问的就是这一本） */
  dictId?: string | null
}

/**
 * 正文框里的反馈条：与 `toast()` 同一套行为（1.8 秒自己消失、颜色按 tone），但位置在内容框内部
 * —— 专给"在正文里选中文字后点「查词」"用（用户眼睛在正文里）。
 * ⚠️ `actions` 非空时**不自动消失**（会自动消失的可点提示等于没提示）；容器仍是
 * `pointer-events: none`（别把正文里的选区吃掉），只有按钮自己打开 pointer-events。
 */
function readerToast(
  message: string,
  tone: 'info' | 'error' = 'info',
  actions: ReaderToastAction[] = []
): void {
  const box = dom.readerToast
  // 用 `textContent` 先把上一轮的内容（含按钮）整片清掉，再按需长出新的 —— 全程不走 innerHTML
  box.textContent = message
  if (actions.length > 0) {
    const row = document.createElement('span')
    row.className = 'reader-toast-actions'
    for (const item of actions) {
      const button = document.createElement('button')
      button.type = 'button'
      button.className = 'reader-toast-action'
      button.textContent = item.label
      button.title = item.label
      button.dataset.toastAction = item.action
      if (item.word) button.dataset.word = item.word
      if (item.dictId) button.dataset.dictId = item.dictId
      row.appendChild(button)
    }
    box.appendChild(row)
  }
  box.dataset.actions = actions.length > 0 ? String(actions.length) : ''
  box.dataset.tone = tone
  box.dataset.show = 'true'
  if (readerToastTimer !== null) {
    window.clearTimeout(readerToastTimer)
    readerToastTimer = null
  }
  if (actions.length === 0) {
    readerToastTimer = window.setTimeout(() => {
      box.dataset.show = 'false'
      readerToastTimer = null
    }, 1800)
  }
}

/**
 * 词条里点了一下、**却什么都没发生**时说的那句话（三种原因由词条正文报上来：词典自己写的
 * `<a>` 没有 href、带锚点的链接找不到锚点、"光标是手型却既不是链接也没人接住"）。
 * 文案只写在这一处，内核只报代码；只在"确实什么都没发生"时才说 —— 多数"点了没反应"其实是正常的。
 */
function deadClickText(reason: string): string {
  if (reason === 'no-href') return '这个链接没有目标'
  if (reason === 'no-anchor') return '这一页里没有这个锚点'
  return '这里没有可点开的内容（词典自带的脚本没接上）'
}

/** 换词条了：正文里那条提示跟着作废（否则它会飘在新词条上，最长 1.8 秒） */
function hideReaderToast(): void {
  if (readerToastTimer !== null) {
    window.clearTimeout(readerToastTimer)
    readerToastTimer = null
  }
  dom.readerToast.dataset.show = 'false'
  dom.readerToast.dataset.actions = ''
}

/** 正文框那条提示上的按钮被按下了（事件委托：按钮是每次现造的，逐个挂监听会漏） */
function onReaderToastClick(event: Event): void {
  const target = event.target as HTMLElement | null
  const button = target?.closest?.('.reader-toast-action') as HTMLElement | null
  if (!button) return
  const action = button.dataset.toastAction
  const word = (button.dataset.word || '').trim()
  // 提示条自己立刻收走：一次点击只该有一次反馈，剩下的交给它引起的那个动作
  hideReaderToast()
  if (action === 'recheck' && word) {
    void onEntryChip('recheck', word, 0)
    return
  }
  if (action === 'lookup' && word) {
    // 与「选中查词」同一条路（`viaSelection`）：它要走到链的尽头，而不是只查当前词典
    void lookup(word, { viaSelection: true, dictId: button.dataset.dictId || undefined })
  }
}

/**
 * 「另有 N 本没能确认（《A》《B》）」—— 有词典**没问完**时，这一句**必须**出现在结果页上。
 * ⚠️ 这是兜底通道里**唯一不许让步**的约定：把"没问完"说成"都没有"，就是在替没被问过的
 * 对象下结论。借查命中那一页、自动翻译那一页、终态页，三处都要带。
 */
function unconfirmedNote(payload: EntryPayload): string {
  const names = (payload.unconfirmed ?? []).filter((name) => !!name)
  if (names.length === 0) return ''
  return `另有 ${names.length} 本没能确认（${names.map((name) => `《${name}》`).join('、')}）`
}

/**
 * 这段选中文字算几个"词"—— 用来决定提示里是**把选中的词写出来**，还是只说"所选文本"。
 * 规则：**≤4 个词**就把词写出来，**≥5 个词**只说"所选文本"。拉丁文按空白切词；
 * ⚠️ 中日韩没有空格、整段会被数成 1 个词，所以中日韩字符要**逐字计数**。
 */
function countSelectedWords(text: string): number {
  const tokens = (text || '').trim().split(/\s+/).filter(Boolean)
  let count = 0
  for (const token of tokens) {
    const cjk = token.match(/[\u3400-\u4dbf\u4e00-\u9fff\u3040-\u30ff\uac00-\ud7af\uf900-\ufaff]/g)
    count += cjk ? cjk.length : 1
  }
  return count
}

/**
 * 这一次「选中查词」是在**哪本词典**里问的 —— 提示里的《…》要写它。
 * 正常就是当前词典；但正文若是从「借查」进来的（`state.entryDictId`），这一轮问的就是那一本，
 * 必须与 `resolve(word, dictId)` 传的一致 —— 否则用户照着提示去当前词典找，而那儿本来就没有。
 */
function selectionDictTitle(): string {
  const dictId = entryLookupDictId() ?? state.currentDictId
  const hit = state.dictionaries.find((item) => item.id === dictId)
  if (hit && hit.title) return hit.title
  const shown = (state.entry?.dictTitle || '').trim()
  return shown || '当前词典'
}

function formatTime(timestamp: number): string {
  const date = new Date(timestamp)
  const now = new Date()
  const hh = String(date.getHours()).padStart(2, '0')
  const mm = String(date.getMinutes()).padStart(2, '0')
  const sameDay =
    date.getFullYear() === now.getFullYear() &&
    date.getMonth() === now.getMonth() &&
    date.getDate() === now.getDate()
  if (sameDay) return `${hh}:${mm}`
  const yesterday = new Date(now.getFullYear(), now.getMonth(), now.getDate() - 1)
  if (
    date.getFullYear() === yesterday.getFullYear() &&
    date.getMonth() === yesterday.getMonth() &&
    date.getDate() === yesterday.getDate()
  ) {
    return `昨天 ${hh}:${mm}`
  }
  return `${date.getMonth() + 1}-${String(date.getDate()).padStart(2, '0')} ${hh}:${mm}`
}

/* ==========================================================================
   模式切换
   ========================================================================== */

function setMode(mode: Mode): void {
  if (state.mode === mode) return
  state.mode = mode
  if (mode !== 'content') {
    hideContextMenu()
    hideSelectionToolbar()
  }
  render()
  pushLayout()
}

function collapseToPill(): void {
  hideContextMenu()
  hideSelectionToolbar()
  // 收起正文时顺手停掉正在响的发音：界面都收起来了，声音还在出很奇怪
  stopSpeech()
  setMode('idle')
}

/* ==========================================================================
   输入与联想
   ========================================================================== */

function onInputChanged(): void {
  state.query = dom.input.value
  updateClearButton()
  if (!state.query.trim()) {
    state.suggestions = []
    state.suggestionsFor = ''
    state.activeIndex = -1
    collapseToPill()
    renderEmptyHint()
    return
  }
  scheduleSuggest()
}

function scheduleSuggest(): void {
  if (suggestTimer !== null) window.clearTimeout(suggestTimer)
  suggestTimer = window.setTimeout(() => {
    suggestTimer = null
    void runSuggest(state.query)
  }, SUGGEST_DEBOUNCE)
}

/**
 * 联想一次。**输入的词从这里进**，不从 state 里读 —— 回车那条路（lookupFromInput）要拿
 * "输入框当前这段文字"立刻联想，而那时 state.query 可能还是上一次的值。
 */
async function runSuggest(text: string): Promise<void> {
  const query = (text ?? '').trim()
  if (!query) return

  const token = ++suggestToken
  if (state.mode !== 'list') {
    state.mode = 'list'
    state.entry = null
    // 换了个词重新联想 = 新的一次查询，链接跳转的返回栈作废
    state.navStack = []
    updateNavButtons()
  }  state.busy = true
  render()

  let items: SuggestionItem[] = []
  try {
    items = await api.suggest(query, MAX_LIST_ROWS)
  } catch (err) {
    console.error('联想失败', err)
  }
  if (token !== suggestToken) return

  state.busy = false
  state.suggestions = items
  // 记下这批候选是照着哪段文字算的：回车要不要"先联想"就看它（见 lookupFromInput）
  state.suggestionsFor = query
  state.activeIndex = items.length > 0 ? 0 : -1
  state.mode = 'list'
  renderSuggestionList()
  renderEmptyHint()
  render()
  pushLayout()
}

/**
 * 输入框里那一下"提交"（回车）到底该干什么：**先联想，再查词** —— 用户敲进去的常常只是
 * 没打完的半个词，直接拿去查词典只会得到"查不到"。只有两种情况可以直接查：
 * ① 候选列表已经照着当前这段文字摆在那儿了（回车 = 采纳高亮那条，最常用的路径，不能让它多按一次）；
 * ② 联想回来发现输入的就是个精确命中的词（`kind === 'exact'`）。其余一律"把候选摆出来让用户挑"；
 * 联想一个候选都给不出来时（乱码、词典里真没有）退回直接查一次 —— 至少还有"查不到 xxx"这条明路。
 */
async function lookupFromInput(): Promise<void> {
  const typed = dom.input.value.trim()
  if (!typed) return

  /*
   * ⓪ 汉字输入**跳过当前词典的联想**：英文词典对中文词给出的前缀 / 编辑距离候选**全是噪音**。
   * 判定**问主进程**（api.scriptOf）—— "什么算汉字"只有一处来源，TS 侧再写一份就是第二个来源。
   */
  const script = await api.scriptOf(typed).catch(() => 'latin' as ScriptKind)
  if (script === 'han') {
    void lookup(typed, { source: 'input' })
    return
  }

  /*
   * ① 候选列表是照着当前这段文字算的、而且正显示着 → 采纳高亮的那条。
   * `suggestionsFor === typed` 是关键：输入框被改过之后那批候选就不作数了，不能拿旧的顶上。
   */
  if (state.mode === 'list' && state.suggestionsFor === typed && state.suggestions.length > 0) {
    const picked = state.suggestions[state.activeIndex] ?? state.suggestions[0]
    if (picked) {
      void lookup(picked.word, { source: 'input' })
      return
    }
  }

  /*
   * ② 立刻联想一次（不受 80ms 防抖影响：回车是明确的用户意图，等不了）。
   * ⚠️ **先取消排队中的那次防抖联想**：它会在这趟飞到半路时插进来、把这次顶成"过期请求"，
   * 于是下面那个守卫认为状态没跟上而直接返回 —— **回车这一下就什么都没干**（不报错地失败，难查）。
   */
  if (suggestTimer !== null) {
    window.clearTimeout(suggestTimer)
    suggestTimer = null
  }
  await runSuggest(typed)

  /*
   * 联想回来之后先确认"这批候选还是我刚才那次要的"：等待期间用户又敲了字的话，会有更新的
   * 一次联想把它顶掉（suggestToken 保证旧的不会写回 state），这时不能再按旧候选替用户做决定。
   */
  if (state.suggestionsFor !== typed) return

  const exact = state.suggestions.find((item) => item.kind === 'exact')
  if (exact) {
    void lookup(exact.word, { source: 'input' })
    return
  }

  // ③ 有候选：列表已经摆出来了，交给用户挑（再按一次回车就是挑第一条）
  if (state.suggestions.length > 0) return

  // ④ 一个候选都没有：退回直接查（见函数头注释最后一段）
  void lookup(typed, { source: 'input' })
}

/* ==========================================================================
   查词
   ========================================================================== */

/**
 * 查词并展示词条。来源的区别只在"返回栈"怎么维护：新查询（输入框 / 候选 / 历史）清空返回栈；
 * `viaLink` / `viaSelection`（看着一个词条时又查另一个词）把当前词条压栈，同一个词不压；
 * `viaBack` 弹栈取词（栈由调用方维护）。`source` 只在新查询时看：'input' → 这一轮「返回候选」
 * 一直有得退；链接跳转与「返回」**不传**它 —— 它们是同一次查询的延续，不能把入口信息洗掉。
 * `leavingScrollY`（跳走前读到哪儿，压栈时记下）/ `restoreScrollY`（这次要还原到哪儿）：
 * 少了前者，「返回」就只能回到词条顶端。
 */
async function lookup(
  word: string,
  options: {
    source?: 'input' | 'history'
    viaLink?: boolean
    /** 正文里选中文字后点「查词」跳过来：压栈规则与 viaLink 相同，只是入口不同 */
    viaSelection?: boolean
    viaBack?: boolean
    fragment?: string
    fallbackWord?: string
    leavingScrollY?: number
    restoreScrollY?: number
    /**
     * 在**指定的**那本词典里查（借查）：不切换当前词典、也不写 CurrentDictId。
     * 空状态里的「用《X》查一次」走它；普通查词不传 = 当前词典。
     */
    dictId?: string | null
  } = {}
): Promise<void> {
  const target = word.trim()
  if (!target) return

  /*
   * 这次查询有没有为"跳走"压过一层返回栈：选区那一路有可能**最后并没有跳**，
   * 那时要先把刚压的那一层弹掉，否则用户点一次「返回」会退到一个"还是这个词"的空层上。
   */
  let pushedLayer = false

  /*
   * 这次查询从哪个入口来 —— 它决定**跑不跑兜底通道**：只有 `input`（输入框）与 `selection`
   * （正文里选中文字）跑。`link` / `back` / `history` 一律不跑 —— 那三条是"精确还原当时那一页"，
   * 让它们跑通道的话，"退回一个查不到的词"会当场跳到别的词典或翻译上去，用户退不回去。
   */
  const origin: LookupOrigin = options.viaBack
    ? 'back'
    : options.viaLink
      ? 'link'
      : options.viaSelection
        ? 'selection'
        : options.source === 'history'
          ? 'history'
          : 'input'

  if (options.viaBack) {
    /*
     * 返回的目标（词条 + 当初的滚动位置）已经由 navStack 弹出来了，但"这一篇属于哪本词典"
     * 必须跟着换回来：栈里那条记的就是它当时的词典（借查进来的词条不属于当前词典，
     * 不退回去的话，再点链接就会跳进另一本词典）。
     */
    state.entryDictId = options.dictId ?? null
  } else if (options.viaLink || options.viaSelection) {
    /*
     * 压的是"当前显示的那一条"（按 keyText 回查能精确回到同一个词条）。链接跳转与"选中文字
     * 查词"在这里合流：两者都是**看着一个词条的时候又查了另一个词**，返回栈的规矩必须一样。
     * 唯一例外：压的与要查的是同一个词（自链接 / 选中的就是当前词条）就不压，否则栈里会多出
     * 一层"退回去还是这个词"的空跳。
     */
    const current = state.entry?.keyText || state.entry?.query
    if (current && current !== target) {
      state.navStack.push({
        word: current,
        // 报上来的位置为准；万一是老消息/异常路径没带，就当作在顶部
        scrollY: Math.max(0, Math.round(options.leavingScrollY ?? 0)),
        // 这篇是从哪本词典来的（借查进来的话就不是当前词典），返回时要回到同一本
        dictId: state.entryDictId,
        /*
         * 连**书名**一起记：返回时那本词典可能已经被移除 / 文件丢了，那时要给一句
         * 「《X》已不在词库中」—— 而词库里已经没有它了，名字只能从这儿拿。
         */
        dictTitle: state.entry?.dictTitle || ''
      })
      pushedLayer = true
    } else if (current && current === target) return   // 原地跳转（自链接）不用记
  } else {
    state.navStack = []
    state.entryFromInput = options.source === 'input'
    // 新一轮查询：查的是哪本词典由调用方说了算（借查会传 dictId，普通查词不传 = 当前词典）
    state.entryDictId = options.dictId ?? null
  }
  // 只有这一次查询带来的锚点才作数，旧的（比如上一次跳转留下的）先清掉
  state.pendingFragment = options.fragment ?? ''
  // 同上：这次要还原的滚动位置，词条装好之后由 load 事件转给词条正文
  state.pendingScrollY = options.restoreScrollY ?? null
  // 记在 DOM 上：自动化验证要靠它确认"跨词条跳转的锚点确实被送到词条正文了"
  dom.reader.dataset.relayedFragment = ''
  dom.reader.dataset.relayedScrollY = ''
  // 新词条还没加载，滚动位置未知：先按"在顶部"处理，等它自己上报
  dom.entryTop.disabled = true

  /*
   * 作废排队中 / 在途的联想。输入框里每敲一下都会排一次联想（80ms 防抖），而回车查词是"插队"
   * 进来的：那次联想要是晚一步跑起来，会当自己还是当前状态的主人 —— 把 mode 切回候选列表、
   * 把 state.entry 清掉。现象就是"回车后词条刚出来又被候选列表顶掉"，返回栈也会跟着一起没。
   */
  suggestToken++
  if (suggestTimer !== null) {
    window.clearTimeout(suggestTimer)
    suggestTimer = null
  }

  state.busy = true
  render()

  let payload: EntryPayload | null = null
  try {
    /*
     * 查哪本词典：借查（`options.dictId`）与"当前正文所属的那本"（`state.entryDictId`）都优先
     * 于"当前词典"；不传 = 主进程用当前词典，而当前词典的状态**一点都不会被改**（只做借查、
     * 不做自动切换）。`origin` 一起传下去：跑不跑兜底通道由主进程按它决定；对 `selection` 来说，
     * 这里的 dictId 含义是"**从这一本开始**跑通道"，判定与真跳必须是同一本。
     */
    payload = await api.lookup(
      target,
      options.dictId ?? entryLookupDictId(),
      origin
    )
    lastLookupError = ''
  } catch (err) {
    console.error('查词失败', err)
    lastLookupError = err instanceof Error ? err.message : String(err)
  }

  /*
   * 整条词组查不到时，退一步查「鼠标点到的那个词」：牛津高阶的交叉引用里有这种链接
   * （href 是整条词组 `take care of`，但词组并不是词库里的词条），用户点的是词组里的某一个词，
   * 按那个词查才是他要的 —— 点 "care" 就能落到《care》词条，里面正好有这个义项。
   * 词组本身能查到就一切照旧（community care、day care 这些是真有条目的）。
   */
  const fallbackWord = (options.fallbackWord ?? '').trim()
  if (
    payload &&
    !payload.found &&
    fallbackWord &&
    fallbackWord.toLowerCase() !== target.toLowerCase()
  ) {
    try {
      // 按点中的那个词回退查词：这是链接跳转的一部分，所以 origin 照旧是 'link'（不跑通道）
      const retry = await api.lookup(
        fallbackWord,
        options.dictId ?? entryLookupDictId(),
        origin
      )
      if (retry && retry.found) payload = retry
    } catch (err) {
      console.error('按点中的词回退查词失败', err)
      lastLookupError = err instanceof Error ? err.message : String(err)
    }
  }

  state.busy = false

  if (!payload) {
    render()
    updateNavButtons()
    /*
     * ⚠️ 这里是 catch-all：IPC 异常、引擎报错、内核失败都会落进来，**不一定**是"没有词库"，
     * 所以要把真实原因透出来 —— 别让一句误导的话盖住真问题。
     */
    toast(lastLookupError ? `查词失败：${lastLookupError}` : '还没有可用的词库，请先添加词典文件', 'error')
    lastLookupError = ''
    return
  }

  /*
   * 兜底：**这一页与当前显示的其实是同一条词条**时，不许当成一次跳转。
   * 词典把词显示成 `dic·tion·ar·y`、用户整条选中再点「查词」时：判落点那一步只做大小写与
   * `@@@LINK`，认不出带分隔点的写法，于是压了一层"退回去还是这个词"的假返回层，
   * 还把同一篇正文**重载一遍**（阅读位置回顶部）。
   * ⚠️ 检查标准用**载荷**（`keyText` + `dictId`），不要拿输入串比字符串 —— 想知道"是不是同一条
   * 词条"，就去问**决定词条归属的那份数据**。
   */
  const shownWord = state.entry?.keyText || state.entry?.query || ''
  if (
    options.viaSelection &&
    pushedLayer &&
    payload.found &&
    shownWord &&
    payload.keyText === shownWord &&
    (payload.dictId || null) === (state.entryDictId || null)
  ) {
    state.navStack.pop()
    state.busy = false
    render()
    updateNavButtons()
    readerToast(`“${target}”就是当前词条（${payload.keyText}）`)
    return
  }

  /*
   * 选区那一路的**终态**：三个通道都试过、翻译也用不上。
   * ⚠️ 这一支**不替换正文**：用户是在读一篇文章时点了一下「查词」，把正在读的正文换成一张
   * "没找到"的页面等于惩罚他点那一下 —— 所以改成**正文框底部一条提示**（#readerToast），
   * 候选也并进那句提示里（候选不占正文区）。
   * 压过的那一层返回栈要**弹掉**：这一趟并没有跳走。
   */
  if (origin === 'selection' && payload.via === 'terminal') {
    if (pushedLayer) {
      state.navStack.pop()
      updateNavButtons()
    }
    state.busy = false
    render()
    const candidates = (payload.suggestions ?? []).filter((item) => !!item)
    const why = payload.translateWhy ? `（${payload.translateWhy}）` : ''
    /*
     * 文案沿用"≤4 个词把词写出来、≥5 个词只说'所选文本'"这条约定：选了一整句时把整句抄进
     * 提示里既放不下也没意义。词典名用载荷里那一本（`dictTitle`），退回 `selectionDictTitle()`。
     */
    const title = payload.dictTitle || selectionDictTitle()
    /*
     * ⚠️ 「别的词典里也没有」这半句**只在真的都问完了**时才能说：有词典没问完的时候，这句话是
     * **替没被问过的对象下结论**（超时 / 出错绝不能并进"没有"）。没问完就改说
     * 「另有 N 本没能确认（…）」，并给一个能点的「再问一遍」。
     */
    const unconfirmed = unconfirmedNote(payload)
    const head =
      countSelectedWords(target) <= 4
        ? `${target}未在《${title}》中查到`
        : `所选文本未在《${title}》中查到`
    const none = unconfirmed ? `，${unconfirmed}` : '，别的词典里也没有'
    /*
     * **候选与"再问一遍"都做成可点的**（候选不占正文区）：候选点了就按那个词走选区那条路查一次；
     * 没问完时给「再问一遍」—— 那是借查**还没问完**时唯一的补救入口。
     */
    const actions: ReaderToastAction[] = []
    for (const candidate of candidates.slice(0, 4)) {
      actions.push({
        label: candidate,
        action: 'lookup',
        word: candidate,
        dictId: state.entryDictId
      })
    }
    if (unconfirmed) {
      actions.push({ label: '再问一遍', action: 'recheck', word: target })
    }
    const hint = candidates.length > 0 ? `，你是不是想找下面这些` : ''
    readerToast(`${head}${none}${hint}${why}`, 'error', actions)
    return
  }

  applyPayload(payload)
}

/* ==========================================================================
   空状态里的"出路"：借查 / 机器翻译
   ==========================================================================
   词典查不到一个词时不该只留一句"没找到"：② 在**其它已导入词典**里查一次（只借查，不切换当前
   词典）；③ 交给**机器翻译**。这两条都是"用户点一下才发生"的事，绝不在查词时自动做（费钱、泄隐私）。
   按钮长在**词条正文里**（宿主 postMessage 送过去）：高度才会被一起量进去，点击时也能顺手把
   "这篇读到哪儿了"一起报回来压栈 —— 宿主够不着跨域 iframe 的 scrollTop。
   ========================================================================== */

/** 机器翻译伪词条的"虚拟词典 id"，必须与 C# 的 `Translate.TranslateEntry.DictId` 一致 */
const TRANSLATE_DICT_ID = 'translate'

/**
 * 「正文显示的这一篇属于哪本词典」—— 查词 / 问落点时要用的 `dictId`。
 * ⚠️ `state.entryDictId` 也可能是译文伪词典 `translate`，而那个 id **不能拿去查词典**（词库里
 * 没有这一本，主进程会找不到 → 前端就只剩一句误导的"还没有可用的词库"）；译文与"还没有正文"
 * 这两种情况一律退回"当前词典"（返回 `undefined`）。
 */
function entryLookupDictId(): string | undefined {
  const id = state.entryDictId
  return !id || id === TRANSLATE_DICT_ID ? undefined : id
}

/**
 * 算出"这一页还能怎么办"，并把按钮送进词条正文。它们**只服务终态页**（`via === 'terminal'`：
 * 当前词典、别的词典、机器翻译三种都试过且翻译用不上）：
 *   · 「再问一遍」—— 有词典**没问完**（超预算 / 报错）时给。⚠️ **有它才敢说"别的词典里也没有"**；
 *   · 「翻译这个词」—— 用户**自己关掉了自动翻译**时给（那是"他不让它自动"，不是"他不要"）。
 * ⚠️ 「借词典查」不再有：借查已经是通道里的自动一步，不该再让用户点一下做同一件事。
 * 其余四种（总开关关 / 没 Key / 语种不支持 / 翻译失败）**不给按钮**：点了也没用，该去改设置。
 */
async function refreshEntryChips(): Promise<void> {
  const entry = state.entry
  const word = (entry?.query || entry?.keyText || '').trim()
  const items: { action: string; label: string; word: string; hint?: string }[] = []

  if (entry && !entry.found && word && entry.via === 'terminal' && entry.dictId !== TRANSLATE_DICT_ID) {
    if (entry.offerRecheck) {
      const names = (entry.unconfirmed ?? []).filter((name) => !!name)
      items.push({
        action: 'recheck',
        label: names.length > 0 ? `还有 ${names.length} 本没查完` : '再问一遍',
        word,
        hint: names.length > 0
          ? `点一下把这（几）本也找一遍：${names.join('、')}`
          : '再问一次别的词典'
      })
    }
    if (entry.offerTranslate) {
      items.push({ action: 'translate', label: `翻译「${word}」`, word, hint: '把这段文字发给火山引擎翻译' })
    }
  }

  // 换过词条了？这次算出来的东西就不作数了（别把上一轮的出路贴到新词条上）
  if (state.entry !== entry) return

  dom.entryFrame.contentWindow?.postMessage({ source: 'lookup-host', type: 'chips', items }, '*')
  /*
   * 记在 DOM 上：自动化验证要断言"到底给了几个出路、分别是什么"。
   * 钉的是**这次算出来的东西**，不是"界面上有几个按钮"——按钮在跨域 iframe 里，宿主读不到。
   */
  dom.reader.dataset.chips = items.map((item) => item.action).join(',')
}

/**
 * 点历史里那一条：**加载并展示当时那本词典上的词条**，而不是拿当前词典再查一遍。
 * 所以这条入口**不跑兜底通道**（`origin = 'history'`），而且**点之前先查本地清单**：
 * 「已不在词库中」与「文件不在了」是三句不同的话，两种都**不发起查词**。
 * 译文条目（伪词典 `translate`）单独一条出路：走翻译缓存回放 —— 拿 `dict:lookup` 去查它
 * 只会得到"词库里没有这一本"。
 */
async function replayHistoryItem(item: HistoryItem): Promise<void> {
  const word = (item.word || '').trim()
  if (!word) return

  // 译文条目：回放走翻译**缓存**（不重新计费），而且**不压返回栈**（D10：它是一轮新查询）
  if (item.dictId === TRANSLATE_DICT_ID) {
    await renderTranslation(word)
    return
  }

  const named = state.dictionaries.find((dict) => dict.id === item.dictId)
  if (!named) {
    /*
     * 那本已经不在词库里了 —— 说清**是哪一本**、为什么、怎么恢复。
     * 恢复办法能说得这么具体，是因为词典 id 是**全路径的哈希**：把文件放回原路径再导入一次，
     * id 与当时那条记录**一模一样**，旧历史自动复活。
     */
    toast(`《${item.dictTitle || '那本词典'}》已不在词库中 —— 重新导入到原路径即可恢复`, 'error')
    return
  }
  if (named.status === 'error' || named.errorMessage) {
    // 「在词库里、但文件丢了」与「已被移除」**不是同一件事**，话也不能合成一句（D8）
    toast(`《${named.title}》的文件不在了 —— 把它放回原来的位置，或重新导入一次`, 'error')
    return
  }
  await lookup(word, { source: 'history', dictId: item.dictId })
}

/** 词条正文里的「出路」按钮被按下了（消息由 EntryDocument 的桥接脚本发来） */
async function onEntryChip(action: string, word: string, scrollY: number): Promise<void> {
  const target = (word || '').trim()
  if (!target) return

  /*
   * 「再问一遍」：把上一轮**没能问完**的词典重新问一次。借查**不再由前端发起**（它已经是通道里
   * 的自动一步），前端留下的只有这个"没问完"的补救入口；单本预算由主进程给足 —— 用户刚点了
   * 按钮，他要的就是答案，可以等。还是没问完就照实说"仍没能确认（哪几本）"：**没问到 ≠ 没有**。
   */
  if (action === 'recheck') {
    let borrow
    try {
      borrow = await api.borrow(target, true)
    } catch (err) {
      /* 这个入口被两处以 void 调用，reject 会变成无声的 unhandled rejection ——
       * 用户点「再问一遍」什么都不会发生。如实说一句。 */
      readerToast(`重问失败：${err instanceof Error ? err.message : String(err)}`, 'error')
      return
    }
    if (borrow.hitId) {
      void lookup(target, { dictId: borrow.hitId })
      return
    }
    if (borrow.unconfirmed.length === 0) {
      readerToast(`别的词典里也没有「${target}」`)
      return
    }
    readerToast(`还有 ${borrow.unconfirmed.length} 本没能确认（${borrow.unconfirmed.join('、')}）`, 'error')
    return
  }

  if (action === 'translate') {
    await translateWord(target, scrollY)
  }
}

/**
 * 译文进正文框的**核心**：翻一段文本（或走缓存**回放**），把译文当成一个词条放进正文框。
 * 三个入口共用它，区别只有"返回栈怎么记"：`translateWord` 是**跳转**（要把当前那一页压栈）；
 * `goBackLink` 返回上一层而那一层是译文页（回放它，**不压栈**）；`replayHistoryItem` 点历史里
 * 一条译文（那是一轮**新查询**，同样不压栈）。
 * ⚠️ 为什么必须共用：这条路上有一串容易只改对一半的动作（压栈 / 弹栈、提示里报不报 token、
 * 伪词条标题栏显示原文而朗读念译文、还原阅读位置），分开写三份迟早有一份漏掉其中一件。
 *
 * @param restoreScrollY 非 null = 还原到当初在译文页读到的位置（返回那条路用）
 * @returns 真的把译文放进正文框了为 true
 */
async function renderTranslation(word: string, restoreScrollY: number | null = null): Promise<boolean> {
  toast('正在翻译…')
  let result: TranslateResult | null = null
  try {
    // 词典名当语种判定的线索（"牛津高阶英汉双解" → 英语），与发音那条路同一个来源
    result = await api.translateText(word, state.entry?.dictTitle ?? null, null)
  } catch (err) {
    console.error('翻译失败', err)
  }
  if (!result || !result.ok) {
    toast(result?.message || '翻译失败', 'error')
    return false
  }

  /*
   * 伪词条：`keyText` 是**原文**（标题栏显示的、也是返回栈里记的"词"），
   * `speakText` 是**译文**（朗读按钮念的是答案，不是问题）——
   * 正文文档里两样都写着，用户看得到对得上。
   */
  if (restoreScrollY !== null) {
    // 和 lookup() 里那条还原路一样：先把目标位置排上，等词条正文装好由 load 事件转给它
    state.pendingScrollY = Math.max(0, Math.round(restoreScrollY))
    dom.reader.dataset.relayedScrollY = ''
  }
  applyPayload({
    query: word,
    keyText: word,
    dictId: result.dictId,
    dictTitle: result.dictTitle,
    entryUrl: result.entryUrl,
    plainText: result.translation,
    found: true,
    speakText: result.translation,
    // 译文那一页在通道约定里就是 `via=translate`（解释行会照着写）
    via: 'translate'
  })
  /*
   * 提示里**不报 token 数**（用户 2026-09 要求去掉）。
   *
   * 理由不是"少写几个字"：那个数字对用户没有任何可操作性 —— 他既不能凭它做决定，
   * 也没法拿它对账（真正的用量在服务商后台）。它出现在这儿只会让人心里一紧、
   * 或者去猜"这次算多还是算少"。翻译成没成、用的是不是缓存，这两件事才是他要的。
   * ⚠️ 别把它加回来： `ui-static-check` 第 ⑪ 节有一条专门盯着这条文案。
   */
  toast(result.fromCache ? '已翻译（用的是缓存，没花钱）' : '已翻译')
  return true
}

/**
 * 翻译一个词/一句话，并把**译文当成一个词条**显示在正文框里（§B6 ③）。
 *
 * 压栈：从译文点「返回」能回到刚才那一页 —— 译文享受和真词条一样的待遇
 * （朗读 / 复制 / 返回栈），这是指导文档  里那条 标准的全部内容。
 *
 * 失败时**把刚压的那一层弹掉**：否则用户点一下"翻译"、失败了、再点返回，
 * 会退到一个"还是刚才那个词"的空层上（返回栈深度对不上，看着像坏了）。
 */
async function translateWord(word: string, leavingScrollY: number): Promise<void> {
  const current = state.entry?.keyText || state.entry?.query
  const pushed = !!current
  if (pushed) {
    state.navStack.push({
      word: current,
      scrollY: Math.max(0, Math.round(leavingScrollY)),
      dictId: state.entryDictId,
      dictTitle: state.entry?.dictTitle || ''
    })
    updateNavButtons()
  }
  const ok = await renderTranslation(word)
  if (!ok && pushed) {
    state.navStack.pop()
    updateNavButtons()
  }
}
/**
 * 把一份查词结果放到正文框里 —— **所有**显示词条的路径最后都走这里。
 * ⚠️ 为什么必须收敛成一个函数：显示一条词条要做的事有一长串（标题栏、复制按钮的可用状态、
 * 导航按钮、停掉正在响的发音、按新词重新规划发音、加载 iframe、重排布局），少做一件就会出现
 * "看得见但哪里不对"的怪状态；两条路（普通查词、机器翻译的伪词条）共享同一串动作。
 */
function applyPayload(payload: EntryPayload): void {
  state.entry = payload
  /*
   * 「这篇正文属于哪本词典」**以载荷里那一本为准**。
   * ⚠️ 别改回"null = 跟随当前词典"：正文是从《A》借查来的、用户在管理窗把当前词典换成《B》、
   * 再点「返回」—— 返回栈里记的是《A》，"跟随当前词典"会把这一跳落到《B》上。
   * 译文伪词条记 `translate`（它不是真词典，查词时会退回当前词典，见 `entryLookupDictId`）。
   */
  state.entryDictId = payload.dictId || state.entryDictId || state.currentDictId
  state.contentHeight = CONTENT_FALLBACK
  state.mode = 'content'
  state.activeIndex = -1

  /*
   * 词条名**不在标题栏显示**了（顶部栏只留词典名），但这里照旧写进 DOM ——
   * 它是"当前词条"的检查标准，冒烟 / 诊断读的就是它。
   */
  dom.entryWord.textContent = payload.keyText
  dom.entryDict.textContent = payload.dictTitle
  // 词典名有 132px 上限，长了就出省略号，完整名字挂 title 上，鼠标停一下还是能看全
  dom.entryDict.title = payload.dictTitle
  dom.entryDict.hidden = false

  /*
   * 兜底通道的产物：这一页是**怎么来的**。界面标记写在 DOM 上（自动化只认 #reader 上的
   * data-via / data-unconfirmed，不去正文里找那句话 —— 正文跑在跨域 iframe 里、宿主读不到）；
   * 解释行则由宿主自己画在正文框顶部（它讲的是"这一页的来历"，属于宿主的知识）。
   */
  const via = payload.via ?? 'current'
  dom.reader.dataset.via = via
  dom.reader.dataset.unconfirmed = (payload.unconfirmed ?? []).join(',')
  const note = [payload.reason, payload.translateWhy, unconfirmedNote(payload)]
    .filter((part) => !!part)
    .join(' · ')
  dom.readerVia.textContent = note
  dom.readerVia.dataset.via = via
  dom.readerVia.hidden = note.length === 0
  // 入口信息属于"这一轮查询"，跳转/返回都改不了它，所以这里读的是 state 而不是当次 options
  dom.entryBack.hidden = !state.entryFromInput
  dom.entryCopy.disabled = !payload.found || payload.plainText.length === 0
  updateNavButtons()

  /* 换词条了，发音这边也要重置：正在响的音频停下，再问一次新词该用哪条音源。 */
  state.speech.lastSource = ''
  state.speech.lastVoice = ''
  stopSpeech()
  // 换词条了，正文里那排「查词 / 朗读 / 复制」也跟着作废（选区属于上一篇正文）
  hideSelectionToolbar()
  // 正文里那条反馈条也跟着作废：它长在正文框里，飘到新词条上会很怪
  hideReaderToast()
  entrySelection = null
  dismissedEntryText = null
  void refreshSpeech()

  showReaderLoading(true)
  // 词条地址相同时 iframe 不会重新加载（例如重复查同一个词），加一个一次性参数强制刷新
  const currentSrc = dom.entryFrame.src
  dom.entryFrame.src = currentSrc === payload.entryUrl ? `${payload.entryUrl}&t=${Date.now()}` : payload.entryUrl

  render()
  pushLayout()
}

/** 返回按钮的可用状态：返回栈空了就置灰 */
function updateNavButtons(): void {
  const depth = state.navStack.length
  dom.entryBackLink.disabled = depth === 0
  // 深度同时写进 dataset：自动化验证要靠它断言"还能返回几层"，比解析 title 靠谱
  dom.entryBackLink.dataset.depth = String(depth)
  /*
   * 提示文案的规矩：**4~6 个字说清"这是干什么的"**，括号只留给快捷键 ——
   * 所以"还能返回几层"用「·」接着写，不再用括号包起来。
   */
  dom.entryBackLink.title = depth === 0 ? '返回上一词条' : `返回上一词条 · 可退 ${depth} 层`
}

/**
 * 返回上一层：把栈顶那个词条重新查出来，并且连同当初离开时读到的位置一起还原
 * （不是又回到词条顶端）。
 */
function goBackLink(): void {
  const previous = state.navStack.pop()
  if (!previous) return
  updateNavButtons()

  /*
   * ⚠️ 上一层如果是**译文页**，绝不能走 `dict:lookup`：译文页属于**伪词典** `translate`，
   * 词库里没有这一本 —— 主进程 `EnsureLoadedAsync` 找不到就回 null，前端于是只弹一句**误导的**
   * 「还没有可用的词库」（用户"在译文里选个词查、再点返回"就回不到机器翻译那一页）。
   * 译文回放要走**翻译缓存**：同一句话不会再计费，也不需要那本词典存在。
   */
  if (previous.dictId === TRANSLATE_DICT_ID) {
    void renderTranslation(previous.word, previous.scrollY)
    return
  }

  /*
   * 那本**已经被移除**、或者**文件丢了**时，不许拿 `dict:lookup` 去撞 —— 撞上去的后果是一句
   * **假话**：主进程找不到那本会回 null，前端只弹「还没有可用的词库」（而词库明明在，只是这一本
   * 不在了）；文件丢了那一种更糟，页面会写「词库尚未加载完成，请稍候重试。」（重试一万次也一样）。
   * 那本的名字从返回栈里拿（`dictTitle`）：词库里已经没有它了，别处问不到。
   */
  const named = state.dictionaries.find((dict) => dict.id === previous.dictId)
  if (!named) {
    toast(`《${previous.dictTitle || '那本词典'}》已不在词库中 —— 重新导入到原路径即可恢复`, 'error')
    return
  }
  if (named.status === 'error' || named.errorMessage) {
    toast(`《${named.title}》的文件不在了 —— 把它放回原来的位置，或重新导入一次`, 'error')
    return
  }

  void lookup(previous.word, {
    viaBack: true,
    restoreScrollY: previous.scrollY,
    // 当初那条是从哪本词典来的就回哪本（借查进来的词条不属于当前词典）
    dictId: previous.dictId
  })
}

function copyEntryText(): void {
  const text = state.entry?.plainText ?? ''
  if (!text) return
  void api.copyText(text)
  toast('已复制释义')
}

/** 让词条 iframe 滚动指定像素量（父页面访问不到跨域 iframe 的滚动位置） */
function scrollReader(delta: number): void {
  dom.entryFrame.contentWindow?.postMessage({ source: 'lookup-host', type: 'scroll', delta }, '*')
}

/* ==========================================================================
   发音
   ==========================================================================
   供给层负责"这个词该怎么念、字节从哪来"，这里只做三件事：显示规划结果（置灰 + 说明原因，
   而不是点了没反应）、把拿到的地址丢给一个复用的 `<audio>`、右键菜单留两个逃生口
   （换音源、换语种）。
   ========================================================================== */

/**
 * 全局唯一的播放器：就一个复用对象，不新建也不插进 DOM（同一时刻只可能有一处在发音，
 * 换音源重播时直接改 src）。音频字节**不走 JSON 桥** —— 主进程给的是一段同源地址。
 */
const speechAudio = new Audio()

/**
 * 播放侧的增益路由：把上面那个 `<audio>` 接进 Web Audio。
 * ⚠️ 为什么需要它：词典自带录音（.mdd 原录音）的增益**可以提升**，而 `<audio>.volume` 的上限是
 * 1、提不上去 —— 只有 `GainNode` 能提。
 */
const speechRoute = new SpeechAudioRoute(speechAudio, 'floating')

/** 发音请求的序号：连点或换词之后，过期的回包直接丢掉，别把旧音频放出来 */
let speakToken = 0
/** 音源规划请求的序号（和播放分开，两者会各问各的） */
let speechStatusToken = 0

/**
 * 要念的文本，**优先 `speakText`**：普通词条用词典命中的那个词（不是用户输入的原文）；
 * 机器翻译的伪词条用**译文**（译文是中文，走中文音色正好）—— 标题栏上仍然显示原文，
 * 那是东西、要念的却是答案，所以这两个不能共用一个字段。
 */
function speechTarget(): string {
  const entry = state.entry
  if (!entry) return ''
  const text = entry.speakText ?? ''
  if (text.trim()) return text.trim()
  return (entry.keyText || entry.query || '').trim()
}

/**
 * 规划里这次会走哪一条：顺序**由内核定死**（词典自带音频 → 豆包 → 系统语音），界面不给选，
 * 所以这里就是"取第一条走得通的"。
 * ⚠️ 别再引入"默认音源偏好"这类开关：顺序本身就是需求，多一个开关只会让用户以为"我选了在线
 * 却没走在线"。
 */
function speechPlan(status: SpeechStatus | null): SpeechOption | null {
  if (!status) return null
  const usable = status.options.filter((option) => option.available)
  if (!usable.length) return null
  return usable[0]
}

/**
 * 音源名进提示之前，先把 label 自带的括号说明削掉：「系统语音（离线）」→「系统语音」。
 * 为什么：这一排按钮的提示规矩是"括号只留给**真的存在**的快捷键"，而「（离线）」「（在线）」
 * 是随 label 带过来的补充说明 —— 它们在设置页有地方铺开讲，挤进 16px 图标的悬停提示只会拉长句子。
 */
function shortSourceLabel(label: string): string {
  return (label || '').replace(/（[^）]*）/g, '').trim()
}

/**
 * 这次走哪条音源，用于**按钮提示**：只给 label，不带 detail。
 *
 * 为什么不带 detail：detail 是给"选项"窗口那种有地方铺开的地方看的
 * （`.mdd 里的原录音`、`本机已装：Microsoft Huihui…`、豆包音色名与语种），
 * 塞进 16px 图标的悬停提示里会把提示拽成一行长句 + 一串括号，正好是这一轮要清掉的东西。
 * 选项窗口的语音页仍然显示 label + detail（见 manager/main.ts）。
 */
function speechPlanText(status: SpeechStatus | null): string {
  const option = speechPlan(status)
  return option ? shortSourceLabel(option.label) : ''
}

function updateSpeakButton(): void {
  const button = dom.entrySpeak
  const speech = state.speech
  const text = speechTarget()
  const status = speech.status
  const available = !!text && !!status && status.available
  /*
   * 分段朗读的进度挂在 dataset 上：按钮提示里要写"第 2/3 段"，
   * 自动化验证也靠它确认"确实在一段一段往下念"（比读音频时长可靠得多）。
   */
  const segments = speech.plan
  const total = segments ? segments.chunks.length : 0
  const current = segments ? segments.index + 1 : 0

  button.dataset.playing = String(speech.playing)
  button.dataset.busy = String(speech.busy)
  button.dataset.available = String(available)
  button.dataset.source = speech.lastSource
  button.dataset.voice = speech.lastVoice
  button.dataset.language = status ? status.detected.language : ''
  button.dataset.plan = status ? (speechPlan(status)?.source ?? '') : ''
  button.dataset.chunks = String(total)
  button.dataset.chunk = String(current)
  // 播放中永远可点 —— 那一下是"停止"
  button.disabled = !available && !speech.playing

  /*
   * 这一排按钮的提示文案统一走一条规矩（见 floating.html 里的说明）：
   *   · "干什么用"用 4~6 个字说完（朗读词条 / 复制释义 / 收起），不再夹括号解释；
   *   · 括号只留给**真的存在**的快捷键（「收起（Esc）」）；
   *   · 附加状态（音源、语种、第几段、用不了的原因）用「·」接在后面 ——
   *     它们是"现在是什么情况"，不是"这个按钮是干什么的"，所以不算违反上面那条。
   * 另外注意 feature 的叫法统一成「朗读」（和浮层的「朗读选中的文字」一致），
   * 原来这里混用「发音」。
   */
  if (speech.playing) {
    button.title = total > 1 ? `停止朗读 · 第 ${current}/${total} 段` : '停止朗读'
    return
  }
  if (speech.busy) {
    button.title = total > 1 ? `正在准备朗读 · 共 ${total} 段` : '正在准备朗读…'
    return
  }
  if (!text) {
    button.title = '先查一个词'
    return
  }
  if (!status) {
    button.title = speech.error ? '朗读不可用：' + speech.error : '检查音源…'
    return
  }
  if (!available) {
    const hint = status.hint ? ' · ' + status.hint : ''
    button.title = '朗读不可用：' + (status.message || '没有可用音源') + hint
    return
  }
  /*
   * 音源顺序是定死的、界面不给选，所以这里不再需要"固定了某条却没走它"那种补充说明：
   * 提示里写的就是这次真的会走的那条（见 speechPlanText）。
   */
  // 语种只留标签（"英语"），去掉原来括号里的判定依据 —— 那是调试信息，不该占提示
  button.title = `朗读词条 · ${speechPlanText(status)} · ${status.detected.label}`
}

/**
 * 问一次"当前这个词怎么念"。
 *
 * 刻意和"真的出声"分开：这一步不合成、不联网，几十微秒就回来，
 * 所以每次查完词都可以顺手调，让按钮的灰/亮始终跟着当前词条走。
 */
async function refreshSpeech(): Promise<void> {
  const text = speechTarget()
  const token = ++speechStatusToken
  if (!text) {
    state.speech.status = null
    state.speech.error = ''
    updateSpeakButton()
    return
  }

  try {
    // 带上词典 id 与词条名：主进程要据此去 .mdd 里看这个词有没有自带录音
    const status = await api.speechStatus(text, state.entry?.dictTitle ?? null, state.entry?.dictId ?? null, text)
    if (token !== speechStatusToken) return
    state.speech.status = status
    state.speech.error = status.available ? '' : status.message
  } catch (err) {
    if (token !== speechStatusToken) return
    state.speech.status = null
    state.speech.error = err instanceof Error ? err.message : String(err)
  }
  updateSpeakButton()
}

function stopSpeech(): void {
  try {
    speechAudio.pause()
    speechAudio.currentTime = 0
  } catch {
    /* 还没加载过任何音频 */
  }
  state.speech.ownSrc = ''
  state.speech.playing = false
  /*
   * 停止 = **整段朗读都停**，不只是当前这一段：清掉分段计划，
   * 并让令牌作废 —— 否则"下一段"如果已经在路上，停下来之后它还会自己冒出来接着念。
   */
  state.speech.plan = null
  /*
   * ⚠️ busy 也必须清：startSpeech 先置 busy 再 await，若朗读进行中走到这里，
   * 在途的 fetchClip 会因令牌作废而直接返回，startSpeech 尾部那句
   * 「token === speakToken 才清 busy」就轮不到了 —— 不清这里，
   * 发音按钮永远卡在「正在准备朗读…」（快速双击朗读按钮即可复现）。
   */
  state.speech.busy = false
  speakToken++
  updateSpeakButton()
}

/**
 * 把要念的文本切成几段。
 *
 * 断点优先级：句末标点 → 逗号/顿号 → 空白 → 硬切。
 * 尽量在句末断开是因为 TTS 每段独立合成，断在句子中间会让语调听着像"被掐了一下"；
 * 兜底的硬切只为"整段没有一个标点"这种极端输入（比如一整串没有空格的字母）。
 */
function splitForSpeech(text: string): string[] {
  const trimmed = (text || '').trim()
  if (trimmed.length <= SPEECH_CHUNK_CHARS) return trimmed ? [trimmed] : []

  const chunks: string[] = []
  let rest = trimmed
  while (rest.length > SPEECH_CHUNK_CHARS) {
    const window = rest.slice(0, SPEECH_CHUNK_CHARS + 1)
    let cut = -1
    // 依次放宽：句末 → 逗号顿号冒号 → 空白
    for (const pattern of [/[。！？!?；;]\s*$/, /[，,、:：]\s*$/, /\s\S*$/]) {
      const match = pattern.exec(window.replace(/\n/g, ' '))
      if (match && match.index > 0) {
        cut = match.index + match[0].length
        break
      }
    }
    if (cut <= 0) cut = SPEECH_CHUNK_CHARS
    chunks.push(rest.slice(0, cut).trim())
    rest = rest.slice(cut).trim()
  }
  if (rest) chunks.push(rest)
  return chunks.filter((chunk) => chunk.length > 0)
}

/**
 * 这段音频是不是"发音按钮要的"。
 * `speechAudio.src` 会被浏览器补成绝对地址，而 result.url 本来就是绝对地址，直接比即可。
 */
function isOwnAudio(): boolean {
  return !!state.speech.ownSrc && speechAudio.src === state.speech.ownSrc
}

function playSpeech(result: SpeakResult): void {
  dom.entrySpeak.dataset.src = result.url
  dom.entrySpeak.dataset.mime = result.mime
  dom.entrySpeak.dataset.bytes = String(result.bytes)
  state.speech.ownSrc = result.url
  /*
   * 增益**每次播放都重设**，而且按这一次的音源算（见 speechGainDb）：
   * 这个 `<audio>` 是全局唯一的，会先后播豆包 / 系统语音 / 词典录音 / 词条里的例句，
   * 而三条路的增益各不相同（只有录音那条非 0）—— 只在启动时设一次必然张冠李戴。
   * 没起来音频图时会退到 `<audio>.volume`（只能压低），那时候"提升做不到"这件事
   * 由 speechRoute 留在 dataset 与 console 里（用户不会为了一条内部提示被打断）。
   */
  const applied = speechRoute.apply(speechGainDb(result))
  dom.entrySpeak.dataset.gainDb = String(applied.gainDb)
  /*
   * 只有"用户设了提升、这次却提不上去"才开口。
   * 其余情况（走了退路但增益本来就是 0 dB）记在 console 与 dataset 里就够了 ——
   * 每次发音都弹一条内部提示，只会盖住"正在念哪个词"这类真正要说的话。
   */
  if (applied.mode === 'volume' && speechGainDb(result) > 0.05) {
    toast(applied.note, 'error')
  }
  try {
    speechAudio.pause()
    speechAudio.src = result.url
    speechAudio.currentTime = 0
  } catch {
    /* 忽略：接着往下播就是了 */
  }

  const started = speechAudio.play()
  if (started && typeof started.catch === 'function') {
    started.catch((err: unknown) => {
      const message = err instanceof Error ? err.message : String(err)
      state.speech.error = message
      state.speech.playing = false
      // 到这一步说明数据通路是通的、是浏览器不肯播（例如整机没有可用音频设备）
      toast('播放失败：' + message, 'error')
      updateSpeakButton()
    })
  }
  /*
   * 状态由 playing / ended / pause 事件驱动，这里**不乐观置位**：
   * 播放真被拦掉时（没有音频设备、自动播放策略），乐观置位会让按钮一直显示"正在响"，
   * 而用户听不到任何声音 —— 那是最难查的一种假象。
   */
}

/**
 * 发音 / 停止。左键走这里：按规划里第一条能走的路。
 */
async function toggleSpeech(): Promise<void> {
  // 分段朗读进行中（哪怕正卡在两段之间那一下）也算"在播"：再点一下是停止整段
  if (state.speech.playing || state.speech.plan) {
    stopSpeech()
    return
  }
  await startSpeech()
}

/**
 * 真的去要一段音频。
 * options 里都是"这一次"的临时指定（右键菜单、正文里的「朗读」用），不写进设置。
 *
 * `text` 是"念别的文字"（正文里选中的那段）：只有它**正好就是当前词条那个词**时，
 * 才把词条上下文交给发音层 —— 那样能放词典自带的原录音；
 * 选中的是一句话/一个词组时不给词条上下文，让发音层走 TTS/在线（词典原录音和它没关系，
 * 硬按词条去取会念出错的东西）。
 *
 * 长文本（选了一整段）在这里被切成几段，一段一段接着念 —— 见 splitForSpeech 与 playSegment。
 */
async function startSpeech(
  options: { source?: 'dict' | 'system' | 'online'; voiceId?: string; language?: string; text?: string } = {}
): Promise<void> {
  const text = (options.text ?? speechTarget()).trim()
  if (!text || state.speech.busy) return

  const entry = state.entry
  const sameAsEntry = !!entry && text.toLowerCase() === (entry.keyText || '').trim().toLowerCase()
  /*
   * 要不要把"当前词条"交给发音层（它能据此放词典自带的原录音）。
   *
   * 机器翻译的伪词条**永远不要**：它不属于任何词典，词条名与要念的文本也不是一回事
   * （`speakText` 是译文）。硬把伪词条塞进去，发音层会拿着一本不存在的词典去 .mdd 里找录音。
   */
  const pseudoEntry = !!(entry && entry.speakText)
  const useEntryAudio = !pseudoEntry && (options.text === undefined || sameAsEntry)

  const token = ++speakToken
  const chunks = splitForSpeech(text)
  if (chunks.length === 0) return

  state.speech.error = ''
  state.speech.busy = true
  state.speech.plan = {
    chunks,
    index: -1,
    token,
    context: { options, useEntryAudio, token },
    next: null
  }
  updateSpeakButton()

  await playSegment(0)
  if (token === speakToken) {
    state.speech.busy = false
    updateSpeakButton()
  }
}

/** 取一段音频。quiet = 预取：失败只记日志、不弹提示（用户还没听到那一段） */
async function fetchClip(text: string, context: SpeechContext, quiet = false): Promise<SpeakResult | null> {
  const entry = state.entry
  const options = context.options
  const fail = (message: string): null => {
    if (context.token !== speakToken) return null
    state.speech.error = message
    if (!quiet) toast(message, 'error')
    else console.error('预取下一段失败：' + message)
    return null
  }

  try {
    const result = await api.speak(text, {
      dictTitle: entry?.dictTitle ?? null,
      source: options.source ?? null,
      voiceId: options.voiceId ?? null,
      language: options.language ?? null,
      // 词典自带音频要按"词典 + 词条"去 .mdd 里找，所以这两个必须带上
      dictId: context.useEntryAudio ? entry?.dictId ?? null : null,
      keyText: context.useEntryAudio ? entry?.keyText ?? text : null
    })
    if (context.token !== speakToken) return null
    if (!result || !result.ok) return fail((result && result.message) || '发音失败')
    return result
  } catch (err) {
    return fail('发音失败：' + (err instanceof Error ? err.message : String(err)))
  }
}

/**
 * 念第 index 段：预取到的直接用，没有就现取，取回来就播，
 * 并且在播的同时**预取下一段** —— 段与段之间就不会出现"正在准备发音"的空档。
 */
async function playSegment(index: number): Promise<void> {
  const plan = state.speech.plan
  if (!plan) return
  const context = plan.context

  let result: SpeakResult | null = null
  if (plan.next && plan.next.index === index) {
    result = plan.next.result
    plan.next = null
  } else {
    result = await fetchClip(plan.chunks[index], context)
  }
  if (!result || context.token !== speakToken || state.speech.plan !== plan) return

  plan.index = index
  state.speech.lastSource = result.source
  state.speech.lastVoice = result.voiceName || ''
  playSpeech(result)
  updateSpeakButton()

  // 下一段先要回来放着：合成/下载的那点时间就藏在当前这段的播放里了
  const nextIndex = index + 1
  if (nextIndex < plan.chunks.length && (!plan.next || plan.next.index !== nextIndex)) {
    void fetchClip(plan.chunks[nextIndex], context, true).then((prefetched) => {
      if (!prefetched) return
      const current = state.speech.plan
      if (!current || current.token !== context.token) return
      current.next = { index: nextIndex, result: prefetched }
    })
  }
}

/**
 * 词条正文里点了某个 🔊：播放那一**段**词典自带的录音（可能是例句）。
 *
 * 与发音按钮的区别：这里点的是指定的那一段，所以**不挑口音、也不兜底换音源** ——
 * 用户点的是"这一条例句的读音"，拿 TTS 念当前词条顶上就是骗人。
 * 放不了就把原因说出来（例如"这段是 Speex，这一版还不能解码"）。
 *
 * 播放仍旧交给全局唯一的那个 `<audio>`：同一时刻只该有一个声音在响
 * （点例句会顶掉正在念的词目发音，这是对的），所以先把"归属"从按钮那边摘掉。
 */
async function playEntrySound(key: string): Promise<void> {
  const entry = state.entry
  if (!entry || !key) return
  try {
    const result = await api.playSound(entry.dictId, key)
    if (!result || !result.ok) {
      toast((result && result.message) || '这段音频放不了', 'error')
      return
    }
    state.speech.ownSrc = ''
    state.speech.playing = false
    // 词条里点 🔊 同样算"把播放器抢走了"：分段朗读到此结束，别留着进度骗人
    state.speech.plan = null
    updateSpeakButton()
    /*
     * 这一段是词典自带的原录音，所以同样要按录音那条的增益播 —— 同一个 `<audio>`，
     * 不重设的话"从发音按钮播"和"从词条里的 🔊 播"会是两个响度。
     */
    speechRoute.apply(speechGainDb(result))
    try {
      speechAudio.pause()
      speechAudio.src = result.url
      speechAudio.currentTime = 0
    } catch {
      /* 忽略：接着往下播就是了 */
    }
    // 把这一次放的到底是什么记在 iframe 元素上：自动化验证要断言"真的换成那一段了"
    dom.entryFrame.dataset.soundKey = key
    dom.entryFrame.dataset.soundSrc = result.url
    const started = speechAudio.play()
    if (started && typeof started.catch === 'function') {
      started.catch((err: unknown) => {
        toast('播放失败：' + (err instanceof Error ? err.message : String(err)), 'error')
      })
    }
  } catch (err) {
    toast('播放失败：' + (err instanceof Error ? err.message : String(err)), 'error')
  }
}

/**
 * 音频播不了时，把"为什么"问出来再告诉用户。
 *
 * 我们自己的两条音频路由（`/__sound__/` 词典原录音、`/__speak__/` 发音）在失败时
 * 都会**带着一句人话**（比如"这段 Speex 里没有可解码的音频帧"），
 * 而 `<audio>` 只会给一个错误码（4 = MEDIA_ERR_SRC_NOT_SUPPORTED）。
 * 与其让用户对着"错误码 4"发呆，不如把响应体读回来直接显示。
 */
async function explainAudioFailure(src: string, code: number): Promise<void> {
  let detail = ''
  if (/\/__(sound|speak)__\//.test(src)) {
    try {
      const response = await fetch(src)
      if (!response.ok) detail = (await response.text()).trim()
    } catch {
      /* 取不到就退回错误码，别把原始失败也吞了 */
    }
  }
  const message = detail.length > 0 ? detail : '音频无法播放（错误码 ' + code + '）'
  state.speech.error = message
  updateSpeakButton()
  toast(message, 'error')
}

/** 挂上发音相关的事件（按钮、播放器、设置变更） */function setupSpeech(): void {
  dom.entrySpeak.innerHTML = iconSvg('speaker')

  /*
   * 把播放器挂进 DOM（hidden，不占位置也不显示任何控件）。
   *
   * 本来不挂也能播，挂上去有两个实打实的好处：
   *   1. 自动化验证能直接读它的 paused / readyState / duration，
   *      "到底有没有在响"是可断言的事实，而不是靠听；
   *   2. DevTools 里一眼能看到它现在的 src 和时长，排查"点了没声音"时最快。
   */
  speechAudio.id = 'speechAudio'
  speechAudio.hidden = true
  speechAudio.preload = 'auto'
  document.body.append(speechAudio)

  /*
   * 发音按钮**只有左键**：念当前词条 / 停止。
   *
   * 原来它还有个右键菜单（选默认音源、临时换语种、进语音设置）。去掉了，原因是那套东西
   * 两头不讨好：菜单一弹出来就按"菜单模式"把内容框收掉（用户："这个逻辑并不符合习惯，
   * 应该保持内容框"），而换语种这种事本来就是设置，值得去「选项 → 语音」里改一次、
   * 而不是每次查词都在右键里挑。少一个入口，"发音"就只剩一个动作：按一下念。
   */
  dom.entrySpeak.addEventListener('click', () => {
    /*
     * 音频图要在**这一个用户手势里**建（懒创建，理由见 SpeechAudioRoute）：
     * 图一旦接上，这个元素的输出就只走图了，而图在自动播放策略挡着时会是 suspended
     * —— 那时候接上等于彻底没声音。所以既不能开机就建，也不能等音频回来才建。
     */
    speechRoute.warmUp()
    void toggleSpeech()
  })

  /*
   * 播放状态只认媒体事件，不认我们自己的猜测（见 playSpeech 的说明）。
   * pause 会在 ended 之前也触发一次，所以 ended 单独再收一遍尾。
   *
   * 另外：这个 <audio> 是全局唯一的，词条里点 🔊 放的音频也走它 ——
   * 所以每种事件都要先问一句"这一趟是发音按钮要的那段吗"，不是就别去动按钮状态。
   */
  speechAudio.addEventListener('playing', () => {
    if (!isOwnAudio()) return
    state.speech.playing = true
    updateSpeakButton()
  })
  speechAudio.addEventListener('pause', () => {
    if (!isOwnAudio()) return
    /*
     * 分段朗读时，每一段放完都会先来一个 pause（紧接着才是 ended）。
     * 那一下如果也照办，按钮就会在段与段之间闪一下"没在播" —— 所以有计划时先不动。
     * 用户主动停止走的是 stopSpeech：它先把 plan 清掉，于是这条判断不成立，
     * 状态照常收尾。
     */
    if (state.speech.plan) return
    state.speech.playing = false
    updateSpeakButton()
  })
  speechAudio.addEventListener('ended', () => {
    if (!isOwnAudio()) return
    /*
     * 还有下一段就接着念。
     * 注意这里**不把 playing 置回 false**：按钮的含义是"整段朗读还在进行"，
     * 段与段之间那一瞬间不该闪。
     */
    const plan = state.speech.plan
    if (plan && plan.index + 1 < plan.chunks.length) {
      void playSegment(plan.index + 1)
      return
    }
    state.speech.plan = null
    state.speech.playing = false
    updateSpeakButton()
  })
  speechAudio.addEventListener('error', () => {
    if (!speechAudio.src) return
    const code = speechAudio.error ? speechAudio.error.code : 0
    if (isOwnAudio()) {
      state.speech.playing = false
      updateSpeakButton()
    }
    void explainAudioFailure(speechAudio.src, code)
  })

  // 管理窗里改了音色/语速/在线开关 → 悬浮窗这边按钮的可用状态要跟着变
  api.onSpeechSettingsChanged(() => {
    void refreshSpeech()
  })
}

/* ==========================================================================
   历史
   ========================================================================== */

async function openHistory(): Promise<void> {
  if (state.mode === 'history') {
    collapseToPill()
    return
  }
  state.history = []
  state.historyTotal = 0
  state.historyLoading = true
  state.mode = 'history'
  render()
  pushLayout()
  await loadHistoryPage()
}

async function loadHistoryPage(): Promise<void> {
  if (state.historyLoading && state.history.length > 0) return
  state.historyLoading = true
  renderHistory()
  try {
    const page = await api.getHistory(state.history.length, HISTORY_PAGE)
    state.history = [...state.history, ...page.items]
    state.historyTotal = page.total
  } catch (err) {
    console.error('读取历史失败', err)
  }
  state.historyLoading = false
  renderHistory()
  render()
  pushLayout()
}

/* ==========================================================================
   词库
   ========================================================================== */

/**
 * 同步"当前词典"。
 *
 * 主进程返回的列表里每项都带 `current` 标记（它在管理窗口切换后会重新广播），
 * 必须以这个标记为准。之前这里是"id 不在列表里就取第一本"，结果在管理窗口
 * 换过当前词典之后，胶囊上那个灰色的词典名一直不跟着变。
 */
function syncCurrentDictionary(list: DictionaryInfo[]): void {
  const flagged = list.find((d) => d.current)
  if (flagged) {
    state.currentDictId = flagged.id
    return
  }
  if (!state.currentDictId || !list.some((d) => d.id === state.currentDictId)) {
    state.currentDictId = list[0]?.id ?? null
  }
}

async function refreshDictionaries(): Promise<DictionaryInfo[]> {
  try {
    const list = await api.listDictionaries()
    state.dictionaries = list
    syncCurrentDictionary(list)
    renderEmptyHint()
    render()
    return list
  } catch (err) {
    console.error('读取词库失败', err)
    return []
  }
}

async function addDictionaries(): Promise<void> {
  const result = await api.addDictionaryFiles()
  if (result.canceled) return
  await refreshDictionaries()
  const parts: string[] = []
  if (result.added.length) parts.push(`已添加 ${result.added.length} 本`)
  if (result.updated.length) parts.push(`已更新 ${result.updated.length} 本`)
  if (result.orphans.length) parts.push(`${result.orphans.length} 个文件未能识别`)
  toast(parts.length ? parts.join('，') : '未添加任何词典', result.orphans.length ? 'error' : 'info')
  if (state.query.trim() && state.mode === 'list') scheduleSuggest()
}

/* ==========================================================================
   右键菜单 + 选中文字浮现按钮
   ========================================================================== */

type MenuEntry =
  | {
      label: string
      icon: IconName
      shortcut?: string
      /**
       * 右侧的补充说明（灰色小字）。
       * 和 shortcut 共用同一块位置和样式：键盘快捷键与"这一项具体是什么"
       * 在视觉上都是"不出声的旁注"，没必要给两套排版。
       */
      detail?: string
      disabled?: boolean
      run: () => void
    }
  | { separator: true }

function selectedRange(): { start: number; end: number } {
  const start = dom.input.selectionStart ?? 0
  const end = dom.input.selectionEnd ?? 0
  return { start, end }
}

function hasSelection(): boolean {
  const { start, end } = selectedRange()
  return end > start
}

/*
 * 剪切 / 复制 / 粘贴走**原生命令**（`document.execCommand`），不再自己拼 `.value`。
 *
 * 修的是什么：给 `input.value` **程序性赋值不进浏览器的撤销栈**，于是
 * 「右键粘贴之后按 Ctrl+Z 撤不回来」，而按 Ctrl+V 粘贴**可以**撤 ——
 * 同一个动作、两个入口、行为不一致。`execCommand('cut' / 'insertText')` 是浏览器
 * 自己的编辑命令，与 Ctrl+X / Ctrl+V 走同一套语义：撤销栈、`input` 事件、
 * 选区处理全都自动正确（菜单项右侧本来就写着 `Ctrl+X` 那几行快捷键提示，
 * 改完之后那个提示从"效果大致相同"变成**字面属实**）。
 *
 * 依据与实测（`docs/输入框右键菜单改走原生编辑命令.md`，诊断
 *  --edit-commands`）：
 *   · `insertText` **确实进撤销栈**：值 AAA → insertText 'BBB' → `undo` → 回到 **AAA**；
 *     同一段流程改用 `value =` 赋值 → `undo` 之后**还是 BBB**（这就是那个缺陷本身）；
 *   · 编辑命令**会抛 `input` 事件**（insertText 一次、undo 再一次），
 *     所以 `onInputChanged` 照常跑，联想与「清空」按钮不用手工补刷新；
 *   · `cut` / `copy` 在用户手势里**同步调用成功**（返回 true）；
 *   · 多行文本插进单行 `<input>` 时换行会变成**空格**（`"a\nb"` → `a b`），
 *     而旧写法是把它**删掉**（`ab`）—— 这是本次唯一的可见行为变化，与原生 Ctrl+V 一致。
 *
 * ⚠️ 三条不许动的边界：
 *   1. **`cut` / `copy` 不能 `await`** —— 剪贴板类命令要用户手势，`mousedown` 给的那一次
 *      不能被 `await` 冲掉（`insertText` 不受此限，粘贴仍先 `await` 读剪贴板）。
 *   2. **菜单项必须继续用 `mousedown` + `preventDefault()`** —— 焦点留在输入框、
 *      选区还在，`execCommand` 才作用于它；改成 `click` 或去掉 `preventDefault()` 会
 *      **不报错地失灵**（表现成"点了没反应"）。
 *   3. **`api.writeClipboard` 不能删** —— 正文那排浮层的「复制」还在用它
 *      （跨源 iframe 里的选区宿主拿不到，`execCommand('copy')` 对它没用）。
 */
function cutSelection(): void {
  if (!hasSelection()) return
  document.execCommand('cut')
  toast('已剪切')
}

function copySelection(): void {
  if (!hasSelection()) return
  document.execCommand('copy')
  toast('已复制')
}

async function pasteClipboard(): Promise<void> {
  /*
   * 剪贴板仍由**宿主**读：宿主这边 `PermissionRequested` 一律 Deny，
   * `navigator.clipboard` 那条路拿不到权限；而 `document.execCommand('paste')`
   * 被 Chromium 明令禁止（脚本发起粘贴永远是 false）。WinForms 的 `Clipboard.GetText()`
   * 本来就不需要浏览器权限。
   *
   * 插入则用 `insertText`：它表现得像"用户敲进去的"——替换当前选区、触发 `input`、
   * **进撤销栈**。
   */
  const text = await api.readClipboard()
  if (!text) return
  document.execCommand('insertText', false, text)
}

/**
 * 「全选」—— 也走原生命令，与另外三项统一（用户 2026-09 要求）。
 *
 * ⚠️ 与另外三项的差别必须说清楚：`execCommand` 作用于**当前有焦点的可编辑元素**，
 * 而 `dom.input.select()` 是**明确指向输入框**的。菜单这条路的焦点是稳的
 * （`showTextMenu` 开头先 `dom.input.focus()`，菜单项又走 `mousedown` + `preventDefault()`），
 * 实测：这套 WebView2 设置下 `execCommand('selectAll')` 返回 true、
 * 输入框选区变成 `[0, 值长]`（诊断 `--edit-commands` 第 ⑥ 节）。
 *
 * **退路因此不能只看返回值**：实测在"输入框失焦"那个构造下它**照样返回 true**
 * （那时它落到了整篇文档上；而文档里唯一的文本就是输入框的值，实测结果上分不干净）。
 * 只判返回值等于没判。所以判"跑完**输入框里到底有没有选区**" ——
 * 没有就退回明确指向输入框的那一句。四项里任何一项"点了没反应"都不能接受。
 */
function selectAllInput(): void {
  if (!document.execCommand('selectAll') || !hasSelection()) dom.input.select()
}

/**
 * 「清空输入」只在输入框里真有字的时候才可用。
 *
 * 空输入框上它本来就无事可做，置灰比"按下去没反应"清楚得多。
 * 输入框的值有三条改动路径：敲键盘、剪切/粘贴（都走原生命令 → 抛 `input` 事件）、
 * 清空（clearInput → onInputChanged）—— 三条都会走到 onInputChanged；
 * render() 里再兜一次底，免得别处改了输入框却忘了刷这个按钮。
 */
function updateClearButton(): void {
  dom.btnClear.disabled = dom.input.value.length === 0
}

/** 清空输入框并收起展开区（那个退格按钮的功能） */
function clearInput(): void {
  hideContextMenu()
  hideSelectionToolbar()
  dom.input.value = ''
  state.query = ''
  state.suggestions = []
  state.suggestionsFor = ''
  state.activeIndex = -1
  state.entry = null
  state.navStack = []
  // 词条没了，发音也就无从谈起：停掉声音、清掉规划，按钮回到"先查一个词"
  stopSpeech()
  state.speech.status = null
  updateNavButtons()
  onInputChanged()
  dom.input.focus()
}

/** 输入框的文本右键菜单 */
function showTextMenu(anchor: { dx: number; dy: number }): void {
  const selection = hasSelection()
  /*
   * **这里写 Ctrl+X / Ctrl+C / Ctrl+V / Ctrl+A = 说实话，不是假快捷键。**
   *
   * 这四条是 **WebView2 对输入框的原生编辑命令**，不是本程序自己实现的快捷键：
   * 浏览器加速键虽然关着（`src/Bridge/WebHost.cs` 的 `AreBrowserAcceleratorKeysEnabled = false`），
   * 但那条注释紧接着就写明"Ctrl+C/V/X/A 属于**编辑命令**，仍然由 WebView2 正常处理" ——
   * 光标在输入框里时，这四个键的剪切 / 复制 / 粘贴 / 全选**确实生效**；两版 README 的快捷键表
   * 也是这么记的（「输入框内的剪切、复制、粘贴、全选（**系统原生行为**）」）。
   * 也就是说：行为是**宿主（WebView2）**给的，本程序在 JS 里一个 Ctrl 键盘处理都没有 ——
   * 标出来是如实告知"这四条本来就管用"，删掉反而是漏信息。
   *
   * ⚠️ 别把它跟**「复制释义」那个按钮**的 Ctrl+C 混为一谈：那一条才是**从来没实现过**的
   * 假快捷键（把整条释义复制进剪贴板没有任何加速键），所以 `#entryCopy` 的提示已经从
   * 「复制释义（Ctrl+C）」精简成「复制释义」，**那个按钮的提示里不许再出现 Ctrl**
   * （见 `floating.html` 的注释与  的"删掉的东西不许回来"）。
   * 一处真、一处假：**这里的四条要保持存在**，别再按"假快捷键"的约定删一次
   **/
  openMenu(
    [
      {
        label: '剪切',
        icon: 'cut',
        shortcut: 'Ctrl+X',
        disabled: !selection,
        run: cutSelection
      },
      {
        label: '复制',
        icon: 'copy',
        shortcut: 'Ctrl+C',
        disabled: !selection,
        run: copySelection
      },
      {
        label: '粘贴',
        icon: 'paste',
        shortcut: 'Ctrl+V',
        run: () => void pasteClipboard()
      },
      { label: '全选', icon: 'selectAll', shortcut: 'Ctrl+A', run: selectAllInput }
    ],
    anchor
  )
}

/** 左侧拖动小图标的右键菜单 */
function showGripMenu(anchor: { dx: number; dy: number }): void {
  openMenu(
    [
      /*
       * 名字就叫「选项」、**后面不带那串灰字**：选项窗口从参考实现起就不止「词库 / 发音」
       * 两页了（还有「翻译」页），写死页签名迟早说错话（用户 2026-09 点名去掉那串灰字）。
       * 图标用齿轮：三处菜单（这里 / 托盘菜单 / 发音设置）统一，含义就是"设置"。
       *
       * ⚠️ 打开时落在**词库页**（第一个页签）。这条 2026-09 来回改过两次：
       *    `'dicts'` → 按需求"默认显示常规页"改成 `'general'` →
       *    同一轮里用户又说"不要管我之前怎么决定的"，页签顺序改成 词库/语音/翻译/常规，
       *    落点跟着回到词库页（唯一的**内容**页、也是唯一会"空"的页）。
       */
      { label: '选项', icon: 'gear', run: () => api.openManager('dicts') },
      { label: '重置悬浮窗位置', icon: 'arrowRight', run: () => api.resetPosition() },
      { separator: true },
      { label: '关闭悬浮窗', icon: 'close', run: () => api.requestClose() }
    ],
    anchor
  )
}

function openMenu(entries: MenuEntry[], anchor: { dx: number; dy: number }): void {
  dom.ctxMenu.textContent = ''
  entries.forEach((entry) => {
    if ('separator' in entry) {
      const line = document.createElement('div')
      line.className = 'menu-sep'
      dom.ctxMenu.append(line)
      return
    }

    const item = document.createElement('button')
    item.type = 'button'
    item.className = 'menu-item'
    item.dataset.disabled = String(!!entry.disabled)
    /*
     * 标签单独一个 .menu-label：菜单宽度上限 280px，标签要能被压窄出省略号，
     * 而右侧的旁注（快捷键 / 说明）不能被压 —— 它是"这一项具体是什么"，
     * 挤掉了就只剩一行看不懂的标题。
     */
    item.innerHTML = `${iconSvg(entry.icon)}<span class="menu-label">${entry.label}</span>${
      entry.shortcut ? `<span class="shortcut">${entry.shortcut}</span>` : ''
    }${entry.detail ? `<span class="menu-detail">${entry.detail}</span>` : ''}`
    if (entry.detail) item.title = `${entry.label} · ${entry.detail}`
    if (!entry.disabled) {
      item.addEventListener('mousedown', (event) => {
        event.preventDefault()
        hideContextMenu()
        entry.run()
      })
    }
    dom.ctxMenu.append(item)
  })

  // 记住右键点在胶囊内的相对位置：窗口尺寸/位置会因为切换模式而变化，
  // 只有"相对胶囊"的坐标在重排之后依然有效
  state.menuAnchor = anchor
  state.modeBeforeMenu = state.mode === 'menu' ? state.modeBeforeMenu : state.mode
  state.mode = 'menu'

  dom.ctxMenu.hidden = false
  /*
   * 先把菜单量一遍再让窗口长大。
   * 菜单是 fixed 定位、宽度取 max-content，高度只由内容决定，
   * 所以这一步量到的是它的自然高度，不受当前窗口大小影响（窗口不会把它压扁）。
   * 顺序反过来的话，第一帧窗口还是旧高度，菜单最后两项会先被裁掉一下再弹回来。
   */
  state.menuHeight = Math.ceil(dom.ctxMenu.getBoundingClientRect().height)
  render()
  pushLayout()
  positionContextMenu()
}

/** 把菜单的**左上角**摆到锚点（也就是鼠标位置），只在贴边时做最小限度的夹取 */
function positionContextMenu(): void {
  if (dom.ctxMenu.hidden || !state.menuAnchor) return
  const pillRect = dom.pill.getBoundingClientRect()
  const anchorX = pillRect.left + state.menuAnchor.dx
  const anchorY = pillRect.top + state.menuAnchor.dy

  const rect = dom.ctxMenu.getBoundingClientRect()
  /*
   * 需求：菜单左上角随鼠标位置确定。
   * 下方放不下时（胶囊贴在屏幕下方、面板朝上展开时很常见）翻到锚点上方，
   * 这时左边缘仍然对着鼠标，右/下越界才做最小限度的夹取。
   */
  let top = anchorY
  if (anchorY + rect.height > window.innerHeight - 4) {
    top = anchorY - rect.height - 2
  }
  top = clamp(top, 4, Math.max(4, window.innerHeight - rect.height - 4))
  let left = anchorX
  if (left + rect.width > window.innerWidth - 4) {
    left = Math.max(4, window.innerWidth - rect.width - 4)
  }
  dom.ctxMenu.style.left = `${Math.round(left)}px`
  dom.ctxMenu.style.top = `${Math.round(top)}px`
}

function hideContextMenu(): void {
  if (dom.ctxMenu.hidden) return
  dom.ctxMenu.hidden = true
  state.menuAnchor = null
  if (state.mode === 'menu') {
    // 右键之前是什么模式就回到什么模式（比如正在看词条，关掉菜单继续看）
    state.mode = state.modeBeforeMenu
    render()
    pushLayout()
  }
}

/**
 * 这排浮层现在是谁的：**只有正文（词条）那一处**。
 *
 * 参考实现起**输入框里选中文字不再弹浮层**（用户 2026-09 定的：那排「复制 / 剪切」利用率低，
 * 而右键菜单里有同样的四项、Ctrl+X/C 又是 WebView2 的原生编辑命令）——
 * 于是这里只剩正文那一路。浮层元素本身留着：正文里选中文字弹出的
 * 「查词 / 朗读 / 复制」还要用它（那一路**不能**砍：查词是这一版的首要功能，
 * 而且正文是跨源 iframe、宿主拿不到系统右键菜单那套编辑命令）。
 */
let selectionToolbarOwner: 'entry' | null = null

/** 正文里选中的文字与它在宿主页坐标系里的位置；null = 正文里没选中 */
interface EntrySelection {
  text: string
  /** 截断过（超过 300 字），界面上要说明 */
  clipped: boolean
  /**
   * 选这段文字的时候，正文读到了哪儿（像素）。
   *
   * 「查词」按钮一按就换词条了，得把当前词条和这个位置一起压进返回栈，
   * 「返回上一词条」才不用又从词条顶端重看一遍 —— 与 entry:// 链接跳转同一套待遇。
   * 值由词条正文在选区内报里带过来（宿主够不着跨域 iframe 的 scrollTop）。
   */
  scrollY: number
  left: number
  top: number
  bottom: number
  width: number
}

let entrySelection: EntrySelection | null = null

/**
 * 正文那边**刚被用掉**的那段文字：同一段文字的报告直接忽略，直到选区真的变过。
 *
 * 为什么需要：点浮层上的按钮会收起浮层，但词条正文那边可能还有一个在路上的报告
 * （桥接脚本对 selectionchange 做了 60ms 节流），它一到就会把浮层原地显示回来 ——
 * 表现成"点了复制，浮层不消失"。检查标准具体到**同一段文字**才忽略
 * （选区换了一段、或者被清掉重选，就该照常浮出来）。
 */
let dismissedEntryText: string | null = null

function dismissSelectionToolbar(): void {
  if (entrySelection) dismissedEntryText = entrySelection.text
  hideSelectionToolbar()
}

/** 把浮层摆到某个矩形旁边（优先上方，放不下就放下方，都不出面板可见区） */
function placeToolbarNear(anchor: { left: number; width: number; top: number; bottom: number }): void {
  const toolbar = dom.selToolbar
  const rect = toolbar.getBoundingClientRect()
  const center = anchor.left + anchor.width / 2
  const left = clamp(center - rect.width / 2, 6, Math.max(6, window.innerWidth - rect.width - 6))

  /*
   * 正文里的浮层要待在**阅读区那块可见区域**里：窗口上下都预留了面板空间，
   * 越出去的部分会被窗口 Region 裁掉（看不见还点不着）。
   * 所以先试选区上方，顶不下就放到选区下方，再整体夹在面板范围内。
   */
  const panelRect = dom.panel.getBoundingClientRect()
  const limitTop = panelRect.top + 4
  const limitBottom = Math.max(limitTop, Math.min(panelRect.bottom, window.innerHeight) - rect.height - 4)
  let top = anchor.top - rect.height - 6
  if (top < limitTop) top = anchor.bottom + 6
  top = clamp(top, limitTop, limitBottom)

  toolbar.style.left = `${left}px`
  toolbar.style.top = `${Math.round(top)}px`
}

/**
 * 按下浮层上的按钮：先记"用掉了"，再干活（两个动作都可能顺带触发同步）。
 *
 * 这一排**没有置灰状态**：正文里那三个动作（查词 / 朗读 / 复制）"能不能做"要么
 * 由点击后主进程的回答决定（查词），要么本来对任何选中文字都成立（朗读 / 复制）。
 * 「查词」曾经带过一个 `disabled` 参数用于"选中时就置灰"，那一版已经撤掉了 ——
 * 见 lookupFromSelection 的说明：选中时**猜**不出来，得点了才知道。
 */
function wireToolbarButton(action: { icon: IconName; title: string; run: () => void }): void {
  const button = document.createElement('button')
  button.type = 'button'
  button.className = 'icon-btn'
  button.dataset.size = 'sm'
  button.title = action.title
  button.innerHTML = iconSvg(action.icon, 15)
  button.addEventListener('mousedown', (event) => {
    event.preventDefault()
    action.run()
    hideSelectionToolbar()
  })
  dom.selToolbar.append(button)
}

/* ==========================================================================
   正文（词条）里选中文字：查词 / 朗读 / 复制
   ==========================================================================
   需求：内容框里选中文字要浮出一排快捷按钮 —— 查它、念它、复制它。
   这是这一版浮层**唯一**的用处：输入框那排「复制 / 剪切」已在 参考实现砍掉
   （用户 2026-09 定的：利用率低，而右键菜单有同样四项、Ctrl+X/C 是 WebView2 原生编辑命令）。
   正文这一排**不能**砍：正文是跨源 iframe，系统那套编辑命令与右键菜单都够不着它，
   而「查词」正是这一版的首要功能。

   与输入框那排的差别只有两点：
     · 动作是「查词 / 朗读 / 复制」（正文里没有"剪切"这回事）；
     · 位置跟着**选区**走（输入框那排是放在胶囊外侧的留白里）。
   选区是跨源 iframe 里的，宿主够不着 —— 由词条正文的桥接脚本上报（见 EntryDocument 里的
   reportSelection），这里只负责换算坐标、摆浮层、执行动作。
   ========================================================================== */

/** 正文的桥接脚本上报了选区（空选区也要报，用来收起浮层） */
function handleEntrySelection(raw: {
  text?: string
  rect?: Record<string, number>
  clipped?: boolean
  empty?: boolean
  scrollY?: number
}): void {
  const text = typeof raw.text === 'string' ? raw.text.trim() : ''
  const rect = raw.rect
  if (!text || !rect || raw.empty) {
    entrySelection = null
    // 选区被清掉了：下次（哪怕重新选中的还是同一段）也该能浮出来
    dismissedEntryText = null
    hideSelectionToolbar()
    return
  }
  // 刚被用掉的那一段：忽略。否则节流的报告一到达，浮层就自己冒回来了
  if (dismissedEntryText === text) return

  // iframe 视口坐标 → 宿主页坐标（选区矩形是相对 iframe 的，加上 iframe 自己的位置即可）
  const frame = dom.entryFrame.getBoundingClientRect()
  entrySelection = {
    text,
    clipped: raw.clipped === true,
    /*
     * 正文读到哪儿了 —— 只有词条正文知道（跨域 iframe，宿主读不到 scrollTop），
     * 所以跟着选区内报一起过来。下面「查词」按钮压返回栈时要它。
     * 老消息 / 异常路径没带这个字段时按"在顶部"处理，与链接跳转那条路的兜底一致。
     */
    scrollY: Math.max(0, Math.round(Number(raw.scrollY) || 0)),
    left: frame.left + Number(rect.left || 0),
    top: frame.top + Number(rect.top || 0),
    bottom: frame.top + Number(rect.bottom || (Number(rect.top || 0) + Number(rect.height || 0))),
    width: Number(rect.width || 0)
  }

  /*
   * 先落进 entrySelection 再摆浮层：浮层元素只有一个，输入框那排已经砍掉，
   * 所以这里不需要再"抢"元素了。
   */
  showEntrySelectionToolbar()
}

function showEntrySelectionToolbar(): void {
  const selection = entrySelection
  if (!selection) return
  // 窗口没焦点（点了别的程序）时不浮出来：那时候用户根本不在看这个词条
  if (!document.hasFocus()) return

  const toolbar = dom.selToolbar
  toolbar.hidden = false
  toolbar.textContent = ''
  selectionToolbarOwner = 'entry'

  /*
   * 三个按钮的顺序：查词 / 朗读 / 复制。
   *
   * 「查词」放最左是刻意的 —— 它是这一轮的主动作，位置也最靠近选区起点。
   * 朗读与复制保持原有相对顺序，老用户的肌肉记忆不变。
   *
   * ⚠️ **它不置灰**（这一版把置灰撤了，见 lookupFromSelection 的说明）：
   * 能不能查、会落到哪条词条，是**点击那一刻**才问词典的，选中时不猜。
   */
  wireToolbarButton({
    icon: 'search',
    title: '查这个词',
    run: () => {
      /*
       * 先按"用掉了"记一笔（挡住随后节流到达的那条同段报告），再去判、去跳。
       * 判定是异步的（一次跨进程往返），所以这里 fire-and-forget；
       * 浮层已经被收起来了，不存在"回包回来时要回写界面"的时序问题。
       */
      dismissSelectionToolbar()
      void lookupFromSelection(selection)
    }
  })

  const clipNote = selection.clipped ? '（只念前 20000 字）' : ''
  wireToolbarButton({
    icon: 'speaker',
    title: '朗读选中的文字' + clipNote,
    run: () => {
      // 先按"用掉了"记一笔（这一步会挡住随后到达的那条同段报告），再念
      dismissSelectionToolbar()
      void speakSelection(selection.text)
    }
  })
  wireToolbarButton({
    icon: 'copy',
    title: '复制选中的文字',
    run: () => {
      dismissSelectionToolbar()
      void api.writeClipboard(selection.text)
      toast('已复制')
    }
  })

  placeToolbarNear(selection)
}

/**
 * 点下「查词」之后：先问词典"这个词会落到哪条词条"，再决定跳不跳。
 *
 * ## 为什么不在选中时就把按钮置灰（这一版改掉的正是它）
 *
 * 上一版的做法是"选中时预测能不能查"，靠的是**字符串比较**：
 * 选中的文字与当前词条名相同就灰、否则去问一次 `dict:exists`，命中也灰不了。
 * 它有一类输入**必然判错**：词典会把名词复数、动词过去式这类变形形式
 * **重定向到原型词条**（`apples` → `@@@LINK=apple`）。于是
 * `"apples" !== "apple"` ⇒ 不算自跳转 ⇒ `exists("apples")` 答"在" ⇒ **按钮亮了**；
 * 点下去 `lookup()` 里那两句同样是字符串比较，于是
 * ① 压进返回栈一层 `apple`（退回去还是这个词，假的），
 * ② `keyText` 解析回 `apple` ⇒ 地址与当前相同 ⇒ 加 `&t=` **把同一篇正文重载一遍**，
 * 阅读位置回到顶部。（用户 2026-09 报的："看起来就是跳转到本词条顶部（不是滚上去）"。）
 *
 * 根子在于**用不一样的信息去预测**：置灰检查标准手里是字符串，真正的结果由词典的解析
 * （大小写变体 → 逐层跟随 `@@@LINK=`）决定。所以修法不是把字符串比得更聪明，
 * 而是**别预测** —— 点击时问词典要"落点"，再拿它跟当前词条比。
 * 这样判定与跳转用的是**同一份解析结果**，两者不可能不一致；
 * 顺带还少了一次跨进程往返（原先每选中一段文字就问一次，现在只有点击才问），
 * 也符合"总要先查一下才知道查不查得到"的直觉。
 *
 * @returns 动作已经发出（真的跳 / 或者交给通道去借查、翻译）为 true；
 *          "落点就是当前词条"为 false（那一条**不进通道**，只弹一句提示）
 *
 * ⚠️ **2026-09 约定变了**：以前"查不到"就到此为止（只弹一句提示），现在它**要走进兜底通道**
 * （`origin = 'selection'`）—— 当前词典没有就问别的词典，别本没有就自动翻译，
 * 都不行才在正文框底部给一条提示。依据 `docs/查词兜底通道与历史记录开发指导.md`  C 那六行，
 * 以及用户那句话「总之每次查询最后一定给出一个结果」。
 */
/*
 * 调试钩子：把"在正文里选中一段文字，再点「查词」"这条路**摊给诊断脚本**。
 *
 * 为什么需要：这条路的真实入口在**跨源 iframe** 里的选区 + 宿主那排浮层上的按钮，
 * 诊断脚本（CDP 连在宿主页上）够不着那个选区；而这条路现在会走进兜底通道
 * （借查别本 / 自动翻译 / 终态提示），**必须能被定向观测**，
 * 否则它就会像 2026-09 那次一样：改完了、却没人验过。
 *
 * 它只在 `LOOKUP_DEBUG_HOOKS=1` 时存在（`window.dshLookup.debug` 本身就是那时候才挂的），
 * 所以产品路径一点都没变。完整测试里那两条断言仍然驱动**真实选区**，
 * 这个钩子只给开发期的诊断用（选区那两例的现场观察）。
 */
if (window.dshLookup && window.dshLookup.debug) {
  const debugHooks = window.dshLookup.debug as Record<string, unknown>
  debugHooks.lookupFromSelection = (text: string, scrollY = 0) =>
    lookupFromSelection({ text, scrollY, clipped: false } as EntrySelection)
}
async function lookupFromSelection(selection: EntrySelection): Promise<boolean> {
  const word = selection.text.trim()
  if (!word) return false

  /*
   * 问哪本词典要落点：**与接下来真跳的那一本必须是同一本**。
   *
   * 正文是从「借查」进来的时候（`state.entryDictId` 有值），`lookup()` 会拿着这个 id 去那一本里查，
   * 所以判定这一步也得问同一本 —— 否则会出现"判定说当前词典里没有（其实那本里有）"，
   * 提示里写的词典名也会是不对的那一本。
   */
  const dictId = entryLookupDictId()
  let landed: string | null = null
  try {
    landed = await api.resolve(word, dictId)
  } catch (err) {
    console.error('解析目标词条失败', err)
    toast('查不到：' + (err instanceof Error ? err.message : String(err)))
    return false
  }

  /*
   * ② 落点就是当前这条词条：**既不用跳，也不用进通道**（ C 第 2 条）。
   *
   * 提示里把落点写出来（「apples 就是当前词条（apple）」）是刻意的：不规则变形
   * （`ran` → `run`）光看选区看不出来，这句话顺手就把"它其实是哪个词"交代了。
   * 这一条的结局是"点了但没跳"，所以**不进通道** —— 进去就等于"这个词在当前词典里
   * 明明有、却因为落点相同就去问别的词典 / 翻译"，那是另一回事。
   */
  const current = (state.entry?.keyText || state.entry?.query || '').trim()
  if (landed && current && landed === current) {
    readerToast(`“${word}”就是当前词条（${landed}）`)
    return false
  }

  /*
   * ③④⑤⑥ 交给兜底通道 —— 这一步现在与输入框那条路走的是**同一套判断**
   * （`Fallback.FallbackPlan` 那张表，`origin = 'selection'`）：
   *   · 别本命中 → 用那一本显示（压栈：这是"在看词条时又查一个词"）；
   *   · 都没命中 + 当前词典有候选 → 候选**并进正文框底部提示**（不摆列表顶掉正文）；
   *   · 都没命中 + 没候选 + 翻译可用 → 自动翻译（译文进正文框，返回能回到原来读到的位置）；
   *   · 都不行 → 正文框底部一条提示，**正文一个字都不动**。
   *
   * `dictId` 传的是**正文显示的那一本**：对选区这条路，它的含义是"从这一本开始找"
   * （正文可能是借查来的），判定与真跳必须同一本（ §D）。
   * `leavingScrollY` 让「返回」时落在当初读到的位置。
   */
  await lookup(word, { viaSelection: true, leavingScrollY: selection.scrollY, dictId })
  return true
}

/**
 * 词条里点了链接（`entry://word` / `entry://word#sense3`）：先问一句"会落到哪条词条"，再决定。
 *
 * ## 为什么不直接 `lookup()` 了事
 *
 * 因为**"是不是跳到别的词条"比字符串比不出来**：词典会把名词复数、动词过去式这类
 * 变形形式**重定向到原型词条**（`apples` → `@@@LINK=apple`）。
 * 正在看 `apple` 时点一个指向 `apples` 的链接，字面上 `"apples" !== "apple"`，
 * 于是 `lookup()` 把它当成"跳到别的词条"：
 *   ① 压进返回栈一层 `apple`（退回去还是这个词，**假的**）；
 *   ② 解析落点回到 `apple` ⇒ 词条地址与当前相同 ⇒ 加 `&t=` **把同一篇正文重载一遍**，
 *      阅读位置回到顶部。
 * 这跟「查词」按钮当初那个 bug 是**同一个根因**，只是入口不同（见 lookupFromSelection 的说明）。
 * 用户 2026-09 报的就是它 —— 只不过当时是从按钮那条路撞上的。
 *
 * ⚠️ 词条正文那边确实有个"自链接就地滚动"的守卫，但它比的是**字面**
 * （`word === CURRENT_WORD`），所以"链到重定向词条、而落点正好是本文档"这种事它拦不住，
 * 照样会送到宿主这边来 —— 判定只能在这儿做。
 *
 * ## 判定约定
 *
 * 继续用 `dict:resolve`（与 `lookup` 内部走**同一条** `Resolve`，所以不可能出现
 * "判定说会跳到 A、真跳却是 B"）：
 *
 *   · **落点就是当前词条** → 不当跳转：链接带锚点就就地滚到那个锚点（那是词典自己的
 *     页面内导航意图），**不带锚点就什么都不做**（正文一个字都不动、返回栈也不压）。
 *     刻意**不**"滚回顶部"：用户报的原文就是"看起来就是跳转到本词条顶部"，
 *     把重载修掉却仍然弹到顶部，对他来说等于没修。
 *   · **其余一切情况（含"这个词压根查不到"）一律原样放行**给 `lookup()`。
 *     这一条**不能省**：词组链接（`take care of`）整体查不到是**正常**的，
 *     它靠 `lookup()` 里的 `fallbackWord` 退回去查「鼠标点到的那个词」（落到《care》）；
 *     要是因为 `resolve` 回了 `null` 就在这里提前返回，「词组链接退回单个词」那条路会被弄坏。
 */
async function followEntryLink(link: {
  word: string
  fragment: string
  fallbackWord: string
  scrollY: number
}): Promise<void> {
  const word = link.word.trim()
  if (!word) return

  /*
   * 与选区那条路**同一约定**：
   * **先拿原样文本问落点，问不到再去掉音节分隔点问一遍**（顺序不能反 ——
   * 有些词条名本来就带中点，「弗拉基米尔·普京」那类）。
   *
   * 为什么链接这条路得自己补这一刀：`origin = 'link'` **不跑兜底通道**（D11），
   * 所以通道里那套"去掉点再问"它一次都到不了；而词典把词头显示成 `dic·tion·ar·y` 时，
   * 它自己给的交叉引用 `href` 往往就是同一个写法 —— 不补这一刀，用户点这种链接
   * 永远只会看到"查不到"那一页（明明这本词典里就有 `dictionary`）。
   */
  let target = word
  try {
    let landed = await api.resolve(word)
    if (!landed && hasSeparatorDots(word)) {
      const stripped = withoutSeparatorDots(word)
      // 整段就是几个点 → 去掉之后是空串，拿去查没有意义（与 C# 那一份同一条接口定义）
      if (stripped && stripped !== word) {
        const retry = await api.resolve(stripped)
        if (retry) {
          landed = retry
          target = stripped
        }
      }
    }
    const current = (state.entry?.keyText || state.entry?.query || '').trim()
    if (landed && current && landed === current) {
      // 带锚点的自链接：就地滚过去（与词条正文对字面自链接的处理一致）
      if (link.fragment) {
        dom.entryFrame.contentWindow?.postMessage(
          { source: 'lookup-host', type: 'scroll-to', fragment: link.fragment },
          '*'
        )
      }
      return
    }
  } catch (err) {
    /*
     * 问不出来（桥出错、词库没加载完…）就**按老路走** ——
     * 绝不能因为"判定失败"让链接变成点不动：那比多压一层返回栈糟得多。
     */
    console.error('解析链接落点失败，按普通跳转处理', err)
  }

  await lookup(target, {
    viaLink: true,
    fragment: link.fragment,
    /*
     * 「鼠标点到的那个词」也按同一约定归一化：词组链接（`take care of`）整体查不到时，
     * 靠它退回单个词（《care》）；而整条词组带分隔点时，那个词通常也带着同一种点。
     */
    fallbackWord: hasSeparatorDots(link.fallbackWord) ? withoutSeparatorDots(link.fallbackWord) : link.fallbackWord,
    leavingScrollY: link.scrollY
  })
}

/** 念一段**选中的文字**（不是当前词条） */
async function speakSelection(text: string): Promise<void> {
  const trimmed = (text || '').trim()
  if (!trimmed) return
  await startSpeech({ text: trimmed })
}

/**
 * 收起正文那排「查词 / 朗读 / 复制」。
 *
 * 参考实现之前它还要处理"输入框那排浮层露着时窗口被撑高（`selmenu` 模式）"这件事，
 * 那排砍掉之后这个模式整个没有了 —— 现在它只是把元素藏起来。
 */
function hideSelectionToolbar(): void {
  selectionToolbarOwner = null
  dom.selToolbar.hidden = true
}

/* ==========================================================================
   贴边窄条的悬停：弹出 / 重新吸入
   ========================================================================== */

/**
 * 播放吸入 / 弹出动画。
 *
 * 'in'：加上 data-edge-anim，胶囊宽度/高度过渡到 8px，看起来被吸进屏幕边缘
 * 'out'：先"无过渡"地把胶囊摆成 8px，再放开，让它长回去 —— 看起来从边缘挤出来
 * null：结束动画，恢复常态
 */
function applyEdgeAnimation(mode: 'in' | 'out' | null, edge: AbsorbEdge): void {
  const content = dom.content
  if (edge) content.dataset.edge = edge

  if (mode === 'in') {
    delete content.dataset.noTransition
    content.dataset.edgeAnim = 'in'
    return
  }
  if (mode === 'out') {
    // 关掉过渡 → 摆到"已吸入"的形态 → 强制重排 → 打开过渡 → 下一帧恢复
    content.dataset.noTransition = 'true'
    content.dataset.edgeAnim = 'out'
    void content.offsetWidth
    delete content.dataset.noTransition
    window.requestAnimationFrame(() => {
      delete content.dataset.edgeAnim
    })
    return
  }
  delete content.dataset.edgeAnim
  delete content.dataset.noTransition
}

/** 重新吸入的延迟，给鼠标一点"路过"的余地 */
const REABSORB_DELAY = 650
let reabsorbTimer: number | null = null

function cancelReabsorb(): void {
  if (reabsorbTimer !== null) {
    window.clearTimeout(reabsorbTimer)
    reabsorbTimer = null
  }
}

function scheduleReabsorb(): void {
  cancelReabsorb()
  reabsorbTimer = window.setTimeout(() => {
    reabsorbTimer = null
    // 真正要收起来之前再确认一次：还在贴边、没聚焦、没展开面板、鼠标也不在胶囊上
    if (!state.docked) return
    if (document.hasFocus()) return
    if (state.mode !== 'idle') return
    if (dom.pill.matches(':hover')) return
    if (!dom.ctxMenu.hidden) return
    api.collapseToEdge()
  }, REABSORB_DELAY)
}

function setupEdgeBehavior(): void {
  // 鼠标碰到窄条：把完整界面弹出来
  dom.strip.addEventListener('mouseenter', () => {
    api.expandFromEdge()
  })

  // 鼠标离开整个窗口：贴着边缘的话就重新吸回去
  document.addEventListener('mouseleave', () => {
    if (!state.docked) return
    scheduleReabsorb()
  })
  document.addEventListener('mouseenter', () => {
    cancelReabsorb()
  })
}

/* ==========================================================================
   拖拽与吸附
   ========================================================================== */

/**
 * 只有左侧的小图标是拖动把手。
 *
 * 曾经试过"整个胶囊都能拖"，虽然更好抓，但会和"在输入框里按住拖动选中文字"冲突，
 * 所以退回专用把手：拖动时指针基本不会停在输入框上，选词手势保持原生。
 * 用 window 上的捕获阶段监听 + setPointerCapture，指针移出窗口也能继续拖。
 */
function setupDrag(): void {
  let pending = false
  let dragging = false
  let pointerId = -1

  const onGripDown = (event: PointerEvent): void => {
    if (event.button !== 0) return
    event.preventDefault()
    pending = true
    dragging = false
    pointerId = event.pointerId
    try {
      dom.grip.setPointerCapture(pointerId)
    } catch {
      /* 合成事件（自动化测试）没有真实指针，捕获失败不影响拖拽 */
    }
    // 按下瞬间就让主进程记录起点，保证拖动像素精确
    api.dragPrepare()
  }

  const onMove = (): void => {
    if (!pending) return
    if (!dragging) {
      dragging = true
      dom.pill.dataset.dragging = 'true'
      collapseToPill()
      api.dragStart()
    }
    /*
     * 带上这一刻的时间戳：主进程用它量"从 pointermove 到窗口真的挪过去"的延迟。
     * 拖动跟不跟手就是这一串数字 —— 光看代码看不出来，只能量。
     */
    api.dragMove(Date.now())
  }

  const finish = (): void => {
    if (!pending) return
    const wasDragging = dragging
    pending = false
    dragging = false
    dom.pill.dataset.dragging = 'false'
    try {
      if (dom.grip.hasPointerCapture(pointerId)) dom.grip.releasePointerCapture(pointerId)
    } catch {
      /* 忽略 */
    }
    // 无论有没有真正拖动都要告诉主进程收尾：没拖过就只是清掉待拖状态
    api.dragEnd()
    if (wasDragging) {
      void api.getLayoutInfo().then((info) => {
        state.layoutInfo = info
        syncPillPosition(info)
      })
    }
  }

  dom.grip.addEventListener('pointerdown', onGripDown)
  window.addEventListener('pointermove', onMove, true)
  window.addEventListener('pointerup', finish, true)
  window.addEventListener('pointercancel', finish, true)
}

/** 点击胶囊空白处（非按钮、非输入框）也把光标送进输入框 */
function setupPillFocus(): void {
  dom.pill.addEventListener('mousedown', (event) => {
    // 同样是"只认左键"：右键在胶囊边框/内边距上不该有任何动作
    if (event.button !== 0) return
    const target = event.target as HTMLElement | null
    if (target?.closest('.pill-actions button')) return
    if (target === dom.input) return
    dom.input.focus()
  })

  /*
   * 整个窗口的焦点变化由外壳推过来（页面里的 window.blur 实测收不到，见
   * FloatingWindow.EmitWindowFocus 的说明）。
   *
   * 失焦时把两个"浮在胶囊外侧"的浮层都收掉，这是本来就写在需求里的行为：
   *   - 输入框那排复制 / 剪切按钮（bug：点别处不消失，跟着窗口一起被吸走，
   *     hover 唤回来时挡在输入框上）；
   *   - 右键菜单（同一个道理，点别的程序时它同样会孤零零挂在桌面上）。
   * 展开区**不动**：吸回边缘那条规则本来就写着"面板开着时不吸"，
   * 失焦顺手把用户正在看的内容收掉是另一件事，没这个需求。
   */
  api.onWindowFocus((focused) => {
    if (focused) return
    dismissSelectionToolbar()
    hideContextMenu()
  })
}

/* ==========================================================================
   关闭询问
   ========================================================================== */

/**
 * 量出对话框的**自然高度**（不受 max-height 约束时它自己要多高）。
 *
 * 为什么必须先解开 max-height：对话框的 `max-height: calc(100% - 8px)` 是相对
 * 展开区高度算的，而展开区高度正是我们要算的东西 —— 不解开就会量到一个被压过的值，
 * 于是窗口永远只给"刚好差一点"的空间，右边那条滚动条就一直在。
 */
function measureDialogHeight(): number {
  const previous = dom.dialog.style.maxHeight
  dom.dialog.style.maxHeight = 'none'
  const natural = dom.dialog.offsetHeight
  dom.dialog.style.maxHeight = previous
  // 多给 2px：浏览器在 DPI 缩放下的高度取整可能比量到的多半像素
  return Math.ceil(natural + DIALOG_MARGIN * 2 + 2)
}

function showCloseDialog(): void {
  dom.closeRemember.checked = false
  // 切到 dialog 模式：把窗口撑到能装下对话框，否则会被胶囊窗口的边界裁掉
  state.mode = 'dialog'
  render()
  // 先露出来再量 —— display:none 的元素量出来的高度是 0
  dom.closeBackdrop.hidden = false
  state.dialogHeight = measureDialogHeight()
  pushLayout()
}

function hideCloseDialog(): void {
  dom.closeBackdrop.hidden = true
  if (state.mode === 'dialog') {
    state.mode = 'idle'
    render()
    pushLayout()
  }
}

/* ==========================================================================
   事件绑定
   ========================================================================== */

function setupPillEvents(): void {
  dom.input.addEventListener('input', onInputChanged)
  dom.input.addEventListener('focus', () => {
    dom.pill.dataset.focused = 'true'
  })
  dom.input.addEventListener('blur', () => {
    dom.pill.dataset.focused = 'false'
    /*
     * 输入框一失焦（点了胶囊别处、点了面板…），那排复制/剪切浮层就该收起来。
     *
     * 光靠这个还不够：点的是**别的程序**时输入框不会收到 blur（DOM 焦点还在它身上），
     * 那一路由外壳推来的 `floating:window-focus` 兜住。
     */
    dismissSelectionToolbar()
  })

  dom.input.addEventListener('keydown', (event) => {
    /*
     * 正文展示出来后，上下键和翻页键用来滚动词条内容。
     * 词条在跨域 sandbox iframe 里，父页面够不到它的滚动位置，
     * 所以这里把滚动量 postMessage 给 iframe 里的桥接脚本执行。
     */
    if (state.mode === 'content' && !dom.reader.hidden) {
      const viewport = Math.max(1, dom.reader.getBoundingClientRect().height)
      const line = Math.max(40, Math.round(viewport * 0.15))
      const page = Math.max(120, Math.round(viewport * 0.85))
      const scrollKeys: Record<string, number> = {
        ArrowDown: line,
        ArrowUp: -line,
        PageDown: page,
        PageUp: -page
      }
      const delta = scrollKeys[event.key]
      if (delta !== undefined) {
        event.preventDefault()
        scrollReader(delta)
        return
      }
    }

    if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
      if (state.mode !== 'list' || state.suggestions.length === 0) return
      event.preventDefault()
      const delta = event.key === 'ArrowDown' ? 1 : -1
      const count = state.suggestions.length
      state.activeIndex = (state.activeIndex + delta + count) % count
      paintActiveSuggestion()
      return
    }

    if (event.key === 'Enter') {
      event.preventDefault()
      /*
       * 回车 = 提交输入框里的内容。**规矩在 lookupFromInput 里**（先联想、再查词），
       * 将来的「查找」按钮直接调同一个函数即可，两条入口的行为不会有第二种说法。
       */
      void lookupFromInput()
      return
    }

    if (event.key === 'Escape') {
      event.preventDefault()
      if (state.mode !== 'idle') {
        // 第一下：收起展开区
        collapseToPill()
      } else if (dom.input.value) {
        // 第二下：清空输入
        dom.input.value = ''
        onInputChanged()
      }
      // 再按就不做事了 —— 悬浮窗不会因为按 Esc 而缩到托盘
      return
    }

    if (event.key === 'Tab' && state.mode === 'list' && state.suggestions.length > 0) {
      // Tab 补全当前高亮候选
      event.preventDefault()
      const picked = state.suggestions[state.activeIndex]
      if (picked) {
        dom.input.value = picked.word
        state.query = picked.word
        dom.input.setSelectionRange(picked.word.length, picked.word.length)
        scheduleSuggest()
      }
    }
  })

  // 右键：自定义菜单，替代系统默认菜单
  const anchorOf = (event: MouseEvent) => {
    const pillRect = dom.pill.getBoundingClientRect()
    return { dx: event.clientX - pillRect.left, dy: event.clientY - pillRect.top }
  }

  // 左侧小图标：右键出【词库 / 关闭】
  dom.grip.addEventListener('contextmenu', (event) => {
    event.preventDefault()
    event.stopPropagation()
    showGripMenu(anchorOf(event))
  })

  // 胶囊其余位置：右键出文本菜单
  dom.pill.addEventListener('contextmenu', (event) => {
    // 系统默认菜单一律不要（只认我们自己画的那个）
    event.preventDefault()
    const target = event.target as HTMLElement | null
    /*
     * 只有输入框才算"文本区"。
     *
     * 原来这里不看到底点在哪，只要发生在胶囊上就弹菜单，于是两个按钮上右键
     * 也会弹出剪切/复制/粘贴/全选 —— 那些操作跟按钮毫无关系。现在：
     *   - 按钮区（.pill-actions）右键：不出菜单、不做任何事（但也没收掉系统菜单的默认行为）
     *   - 胶囊自己的 1px 边框和 8px 内边距：同理，那不是输入框
     *   - 按钮左侧那片空白：尾部浮层和 spinner 都是 pointer-events:none，
     *     命中的是下面的输入框，所以照样算文本区
     */
    if (target !== dom.input) return
    dom.input.focus()
    showTextMenu(anchorOf(event))
  })

  document.addEventListener('mousedown', (event) => {
    const target = event.target as Node
    if (!dom.ctxMenu.hidden && !dom.ctxMenu.contains(target)) hideContextMenu()
  })

  /*
   * 窗口尺寸一变（面板开合、切换模式）浮层和浮动按钮的定位都要重算，
   * 否则 position:fixed 的元素会停在旧坐标上、被新窗口裁掉。
   */
  window.addEventListener('resize', () => {
    positionContextMenu()
    // 正文那排要跟着阅读区重新摆：窗口尺寸一变，面板和 iframe 的位置都变了
    if (entrySelection) placeToolbarNear(entrySelection)
    else hideSelectionToolbar()
  })

  /*
   * 光标回到输入框 = 用户在输入框那边动手了，正文那排浮层该让位。
   *
   * （参考实现之前这里还要把"输入框那排浮层被用掉过"的界面标记清掉 —— 那排已经砍了，
   *   现在这三条只做"让位"这一件事。）
   */
  dom.input.addEventListener('mousedown', () => {
    hideSelectionToolbar()
  })
  dom.input.addEventListener('keydown', () => {
    hideSelectionToolbar()
  })
  dom.input.addEventListener('focus', () => {
    hideSelectionToolbar()
  })

  // 失焦即收起展开区，保持悬浮窗不碍事。
  // 注意：iframe 被替换 / 内部元素抢焦点也会触发 window 的 blur，
  // 所以延后一拍用 document.hasFocus() 确认"整个窗口"真的失去焦点了才收起。
  window.addEventListener('blur', () => {
    window.setTimeout(() => {
      if (document.hasFocus()) return
      if (!dom.closeBackdrop.hidden) return
      /*
       * 右键菜单是浮在胶囊外侧的浮层，窗口一失焦就该收回。
       * 以前这里是 `if (!dom.ctxMenu.hidden) return`（菜单开着就什么都不做），
       * 结果点了别处之后菜单会孤零零挂在桌面上，只能再点一次才关得掉。
       */
      hideContextMenu()
      collapseToPill()
    }, 0)
  })
}

/** 把光标交回输入框末尾：退回候选列表之后，用户十有八九是接着改词 */
function focusInputAtEnd(): void {
  dom.input.focus()
  const end = dom.input.value.length
  try {
    dom.input.setSelectionRange(end, end)
  } catch {
    /* 个别输入类型不支持选区，忽略即可 */
  }
}

function setupButtonEvents(): void {
  dom.grip.innerHTML = appMarkSvg(20)
  dom.btnHistory.innerHTML = iconSvg('history')
  /*
   * 「清空输入」用一个**小一号的叉**（不套容器），不再是退格键 ——
   * 完整理由（为什么"外框 + 内小界面标记"在 1.7 的笔宽下装不下、为什么又允许用叉、
   * 为什么正好是 7.2px @ 1.4）写在 `icons.ts` 里 `clearInput` 那一段。
   */
  dom.btnClear.innerHTML = iconSvg('clearInput')
  dom.entryCopy.innerHTML = iconSvg('copy')
  dom.entryBack.innerHTML = iconSvg('list')
  dom.entryBackLink.innerHTML = iconSvg('arrowLeft')
  dom.entryTop.innerHTML = iconSvg('arrowUp')
  dom.entryClose.innerHTML = iconSvg('close')
  // 关闭询问框：「最小化到托盘」是"收起来"（托盘图标），「直接退出」用电源键 ——
  // 和托盘菜单里的「退出」保持一致（退出是结束进程，不是关窗口）。
  // 托盘那一项原来画的是"查词历史"的图标，纯属串了。
  el<HTMLSpanElement>('choiceTrayIcon').innerHTML = iconSvg('toTray', 18)
  el<HTMLSpanElement>('choiceQuitIcon').innerHTML = iconSvg('power', 18)

  /*
   * 两个按钮挂的是 mousedown 而不是 click（为了不把光标从输入框里抢走），
   * 但 mousedown 是**所有**鼠标键都会触发的 —— 右键点按钮会连带把按钮按下去。
   * 所以这里显式只认左键：右键要么出文本菜单（在输入框上），要么什么都不做。
   */
  dom.btnHistory.addEventListener('mousedown', (event) => {
    if (event.button !== 0) return
    event.preventDefault()
    void openHistory()
  })
  dom.btnClear.addEventListener('mousedown', (event) => {
    if (event.button !== 0) return
    event.preventDefault()
    clearInput()
  })
  dom.entryCopy.addEventListener('click', copyEntryText)
  dom.entryBackLink.addEventListener('click', goBackLink)
  /*
   * 正文框那条提示上的出路按钮（候选 / 再问一遍）走**事件委托**：
   * 它们是每次现造的临时元素，逐个挂监听必然漏（提示条会反复重建）。
   */
  dom.readerToast.addEventListener('click', onReaderToastClick)
  dom.entryTop.addEventListener('click', () => {
    dom.entryFrame.contentWindow?.postMessage({ source: 'lookup-host', type: 'scroll-top' }, '*')
  })
  /*
   * 「收起」和「返回候选」是同一类动作：收起展开区之后，用户十有八九还要接着改词
   * （打字、Esc、↑↓、回车全是监听在输入框上的），所以同样要主动把光标交回去 ——
   * 光靠"mousedown 不抢焦点"不够：用户可能是先在正文里点过（那时焦点在 iframe 上）。
   */
  dom.entryClose.addEventListener('click', () => {
    focusInputAtEnd()
    collapseToPill()
  })

  /*
   * 标题栏这排按钮就贴在输入框上方，点它们的时候谁都**不该把输入框的光标抢走**。
   *
   * mousedown 的默认行为就是把焦点交给按钮；焦点一走，键盘打字没处去，
   * Esc / ↑↓ / 回车这些都是监听在输入框上的，等于整个键盘操作全失效 ——
   * 正是"点了「返回候选」之后输入框没光标、没法改词、Esc 也清不掉输入"。
   * 胶囊上那两个按钮（历史 / 清空）一开始就是这么处理的，这里把同一套规矩
   * 补齐到标题栏。动作仍旧挂在 click 上 —— mousedown 只负责别抢焦点，
   * 自动化里的 .click() 照样能触发。
   *
   * **左键右键都要 preventDefault**：以前这里写的是"只认左键、右键的默认行为别碰"，
   * 结果右键按下去照样会把焦点交给按钮（实测 `document.activeElement` 变成 entryCopy），
   * 光标从输入框被抢走，而且按钮带着焦点之后，浏览器默认那圈 outline 会沿着 50% 圆角
   * 画成一个**黑圈**（用户说的"不出现黑色圆圈就好"就是它）。
   *
   * ⚠️ 别把这句话理解成"按下时不该有视觉反馈"：圆底 + 图标微缩那套（`.icon-btn:active`）
   * 是**要保留**的，用户明确说那个"挺好看"、而且左右键按下都该有。preventDefault 只管
   * "别抢焦点"，不影响 `:active` 这个状态本身。
   */
  const keepCaret = (button: HTMLButtonElement) => {
    button.addEventListener('mousedown', (event) => {
      event.preventDefault()
    })
    /*
     * 这一排按钮上**右键什么都不做**：不弹菜单（系统菜单在 WebView2 那侧本来就关了）、
     * 不动光标、不动内容框。这里显式 preventDefault 是为了把话说清楚，
     * 也免得将来谁给某个按钮加上右键行为时顺手把这排都带进去。
     */
    button.addEventListener('contextmenu', (event) => {
      event.preventDefault()
    })
  }
  ;[dom.entryBack, dom.entryBackLink, dom.entryTop, dom.entrySpeak, dom.entryCopy, dom.entryClose].forEach(keepCaret)

  const inHeadButton = (event: Event): boolean => {
    const target = event.target as HTMLElement | null
    return !!target && !!target.closest('.icon-btn')
  }

  /*
   * 上面那套管不到**禁用的按钮**：浏览器对 disabled 的元素根本不派发 mousedown，
   * 而默认行为照样执行一遍"聚焦"—— 禁用按钮接不了焦点，于是光标直接从输入框掉到 BODY。
   * 实测：右键（或左键）点一下置灰的「返回候选」，输入框就没了光标，打字、Esc、回车全哑，
   * 而按钮自己一点反应都没有（它本来就是禁用）。
   *
   * 修法只能用"兜底交回"：pointerdown / contextmenu / pointerup 这些**会**冒到容器上来
   * （禁用元素自己不派发 mousedown，但指针事件不在此限），在这几处看一眼光标，
   * 掉了就交回输入框。
   *
   * 刻意**不**在 pointerdown 上 preventDefault：那会连兼容的 mousedown / click 一起吞掉，
   * 这一排按钮就真的点不动了（禁用按钮看着没反应，其实启用之后也没反应 —— 更难查）。
   */
  const restoreHeadCaret = () => {
    if (!document.hasFocus()) return
    if (document.activeElement === dom.input) return
    dom.input.focus()
  }
  dom.panelHead.addEventListener('contextmenu', (event) => {
    if (inHeadButton(event)) event.preventDefault()
    restoreHeadCaret()
  })
  dom.panelHead.addEventListener('pointerup', restoreHeadCaret)

  dom.entryBack.addEventListener('click', () => {
    /*
     * 退回候选列表。
     *
     * 不能只把 mode 切成 'list'：候选列表有两处可能不对劲 ——
     * 一是那次查词是在联想返回之前就回车的（列表还是空的），
     * 二是输入框里已经换过词但还没联想（列表还停在上一个词的候选上）。
     * 所以这里按输入框里现在的文字重新拉一次，顺便让联想那条路把
     * mode 切过去、清掉这次查询的返回栈。
     *
     * 把光标交回输入框放在切模式**之前**：render() 是按 document.activeElement
     * 来决定胶囊的焦点样式的，顺序反了会先渲染成"没焦点"。
     * 也不只是"别抢焦点"那么简单：如果用户刚在正文里点过（焦点在 iframe 上），
     * 这一下必须主动把焦点要回来，不然退回去还是打不了字。
     */
    focusInputAtEnd()
    // 按**输入框里现在的文字**重新联想（不是 state.query：那个可能落后于输入框）
    if (dom.input.value.trim()) {
      void runSuggest(dom.input.value)
      return
    }
    state.mode = 'list'
    render()
    pushLayout()
  })

  dom.choiceTray.addEventListener('click', () => {
    hideCloseDialog()
    api.resolveClose('tray', dom.closeRemember.checked)
  })
  dom.choiceQuit.addEventListener('click', () => {
    hideCloseDialog()
    api.resolveClose('quit', dom.closeRemember.checked)
  })
  dom.closeCancel.addEventListener('click', () => {
    hideCloseDialog()
    api.resolveClose('cancel', false)
  })

  dom.historyList.addEventListener('scroll', () => {
    const { scrollTop, scrollHeight, clientHeight } = dom.historyList
    if (scrollHeight - scrollTop - clientHeight < 60) void loadHistoryPage()
  })
}

function setupReaderEvents(): void {
  dom.entryFrame.addEventListener('load', () => {
    showReaderLoading(false)
    /*
     * 跨词条跳转带了锚点（entry://word#sense3）：词条装好之后让 iframe 落回去。
     * 词条内部自己的锚点跳转（entry://#noun）不用走这里 —— 那种点击根本没离开当前文档。
     */
    const fragment = state.pendingFragment
    if (fragment) {
      state.pendingFragment = ''
      dom.reader.dataset.relayedFragment = fragment
      dom.entryFrame.contentWindow?.postMessage({ source: 'lookup-host', type: 'scroll-to', fragment }, '*')
    }
    /*
     * 从别的词条"返回"回来：把它还原到当初离开时读到的位置。
     * 和锚点是同一个套路 —— 宿主够不着 iframe 的滚动位置，只能等文档装好再喊一声。
     */
    const scrollY = state.pendingScrollY
    if (scrollY !== null) {
      state.pendingScrollY = null
      dom.reader.dataset.relayedScrollY = String(scrollY)
      dom.entryFrame.contentWindow?.postMessage({ source: 'lookup-host', type: 'scroll-to-y', y: scrollY }, '*')
    }
    /*
     * 最后再算"这个词没查到的话，还能走哪两条路"。
     *
     * 必须等文档装好、而且放在最后：那一排按钮是**送进文档里**渲染的
     * （理由见 EntryDocument 的 renderChips），文档还没起来时 postMessage 会石沉大海。
     * 它顺带也会让文档重新量一次高度 —— 按钮多占的那一行得算进面板高度里。
     */
    void refreshEntryChips()
  })

  window.addEventListener('message', (event) => {
    const data = event.data as
      | {
          source?: string
          type?: string
          word?: string
          fragment?: string
          fallbackWord?: string
          url?: string
          value?: number
          atTop?: boolean
          /** 词条正文现读的滚动位置：链接点击、选区内报、出路按钮都会带（宿主读不到 iframe 的 scrollTop） */
          scrollY?: number
          /** 出路按钮的动作（borrow / translate）与它带的词 */
          action?: string
          /** `dead-click` 的原因码：no-href / no-anchor / nothing-happened（文案在 deadClickText） */
          reason?: string
        }
      | undefined
    if (!data || data.source !== 'lookup-entry') return

    if (data.type === 'lookup' && typeof data.word === 'string') {
      /*
       * 词条里的链接：记进返回栈，返回按钮就能一层层退回来。
       * 链接可能带锚点（`entry://word#sense3` = 跳到那个词条的某个义项），
       * 得等新词条装好之后再让 iframe 滚过去，所以把锚点一起带上；
       * 词组链接还会带一个「鼠标点到的那个词」当兜底。
       * scrollY 是"这一篇读到了哪儿"，跟着一起压栈，返回时还原。
       */
      void followEntryLink({
        word: data.word,
        fragment: typeof data.fragment === 'string' ? data.fragment : '',
        fallbackWord: typeof data.fallbackWord === 'string' ? data.fallbackWord : '',
        scrollY: typeof data.scrollY === 'number' ? data.scrollY : 0
      })
      return
    }
    if (data.type === 'height' && typeof data.value === 'number') {
      const next = Math.round(data.value)
      if (Math.abs(next - state.contentHeight) < 4) return
      state.contentHeight = next
      if (state.mode === 'content') pushLayout()
      return
    }
    /*
     * 词条正文里那排「出路」按钮（用《X》查一次 / 翻译）被按下了。
     * 按钮是宿主送过去的（见 refreshEntryChips），动作在这里执行 ——
     * 于是"要查哪个词、去哪本词典查、要不要翻译"这些策略仍然只写在宿主这一处。
     */
    if (data.type === 'chip') {
      void onEntryChip(
        typeof data.action === 'string' ? data.action : '',
        typeof data.word === 'string' ? data.word : '',
        typeof data.scrollY === 'number' ? data.scrollY : 0
      )
      return
    }
    /*
     * 词条里点了一下、**可什么都没发生**（词典自己写的链接没有 href / 锚点找不到 /
     * 那个「看着可点」的东西没人接住）。参考实现时代这里是完全不报错的，用户为此报过三次
     * 「点了没反应」—— 现在在正文框里如实说一句，走既有的 `#readerToast`，不新增控件。
     */
    if (data.type === 'dead-click') {
      readerToast(deadClickText(typeof data.reason === 'string' ? data.reason : ''))
      return
    }
    /*
     * 词条正文上报"正文在不在顶部"：回顶部按钮据此置灰。
     * 宿主读不到跨域 iframe 的 scrollTop，只能等它自己说。
     */
    if (data.type === 'scroll-state') {
      dom.entryTop.disabled = data.atTop !== false
      return
    }
    /*
     * 词条里点了词典自带的音频（`sound://` / `snd://`，词目发音和例句发音都走这条）。
     * 桥接脚本把键名与词典 id 一起带过来了 —— 键名就是 .mdd 里那个资源名，
     * 按它去取字节；取不到或者格式放不了（Speex），宿主会如实说明。
     *
     * open-external-link 仍然直接忽略：外链一律拦截是既定需求（不联网、不把用户带走）。
     */
    if (data.type === 'play-sound') {
      void playEntrySound(typeof data.key === 'string' ? data.key : '')
      return
    }
    /*
     * 正文里选中了文字（或选区被清掉）：浮出 / 收起「查词 · 朗读 · 复制」那排按钮。
     * 位置在跨源 iframe 里量，由桥接脚本上报（见 EntryDocument 的 reportSelection）；
     * 那一份上报里还带着 scrollY —— 「查词」换词条时要把阅读位置一起压进返回栈。
     */
    if (data.type === 'selection') {
      handleEntrySelection(
        data as {
          text?: string
          rect?: Record<string, number>
          clipped?: boolean
          empty?: boolean
          scrollY?: number
        }
      )
      return
    }
    /*
     * 正文里又按下了鼠标：用户要开始选新的一段了 —— 清掉"这段刚被用掉"的记账，
     * 这样**重新选同一段**照样能浮出按钮（点过「复制」之后它本来会被挡掉）。
     */
    if (data.type === 'selection-start') {
      dismissedEntryText = null
      return
    }
  })
}

/* ==========================================================================
   启动
   ========================================================================== */

async function bootstrap(): Promise<void> {
  setupButtonEvents()
  setupPillEvents()
  setupPillFocus()
  setupDrag()
  setupEdgeBehavior()
  setupReaderEvents()
  setupSpeech()

  state.layoutInfo = await api.getLayoutInfo()
  syncPillPosition(state.layoutInfo)
  // 拿到 pillTop 之后先把胶囊摆正，再开形状上报 ——
  // 否则第一次上报的会是"胶囊贴在窗口顶上"的错误形状，主进程会照着裁错窗口。
  applyPanelGeometry(state.mode, desiredPanelHeight())
  setupShapeReporting()
  await refreshDictionaries()

  api.onLayoutApplied((applied) => {
    applyAppliedLayout(applied)
    // 工作区可能因为拖到另一块显示器而改变，刷新一次用于限制面板高度
    void api.getLayoutInfo().then((info) => {
      state.layoutInfo = info
      syncPillPosition(info)
    })
  })
  api.onDictionariesChanged((list) => {
    state.dictionaries = list
    syncCurrentDictionary(list)
    renderEmptyHint()
    render()
    if (state.query.trim() && state.mode === 'list') scheduleSuggest()
  })
  api.onFocusInput(() => {
    dom.input.focus()
    dom.input.select()
  })
  api.onCloseRequested(() => showCloseDialog())

  /*
   * 兜底通道进行到哪一步了。
   *
   * 为什么必须有：通道一条查询要串"当前词典 → 借查别本（每本最多 80ms）→ 机器翻译（联网）"，
   * 长的时候好几秒。没有这行字，用户看到的就是"按了回车什么都没发生"，然后以为程序卡住
   * （§B 第 5 条）。**只说阶段与词典名** —— 不报 token 数，也不报耗时（用户 2026-09 的要求）。
   */
  api.onLookupStage((stage: LookupStage) => {
    if (!stage) return
    if (stage.stage === 'borrow') {
      // 开始问的时候没有具体书名（还不知道会命中哪一本），只有命中了才带上名字
      toast(stage.dictTitle ? `正在用《${stage.dictTitle}》查…` : '正在查别的词典…')
    } else if (stage.stage === 'translate') {
      toast('正在翻译…')
    }
  })

  renderEmptyHint()
  render()
  pushLayout()
  dom.input.focus()
  document.body.dataset.ready = 'true'
}

bootstrap().catch((err: unknown) => {
  // 初始化失败时把原因留在 DOM 上，便于排查（正常路径不会走到这里）
  const message = err instanceof Error ? err.message : String(err)
  console.error('[floating] 初始化失败', err)
  document.body.dataset.bootError = message
  dom.meta.textContent = '初始化失败'
  dom.meta.dataset.state = 'error'
})
