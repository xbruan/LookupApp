/** 主进程与渲染进程共享的类型定义：只放跨 IPC 边界的数据结构，业务实现不放这里。 */

/** 吸附的屏幕边缘；null = 未吸附 */
export type SnapEdge = 'left' | 'right' | 'top' | 'bottom' | null

/** 持久化到磁盘的词典记录 */
export interface StoredDictionary {
  id: string
  /** 导入时的默认显示名（文件名）；没有自定义名时才用得上 */
  title: string
  /** 用户在词库管理窗里改的名字。单独一个字段：清空时要能退回「头部标题 → 文件名」。 */
  customTitle?: string
  mdxPath: string
  /** 配套资源库；.mdd / .1.mdd / .2.mdd 多卷都放这里 */
  mddPaths: string[]
  addedAt: number
}

/** 运行期词典信息（含加载状态） */
export interface DictionaryInfo extends StoredDictionary {
  fileName: string
  fileSize: number
  /** 词条总数，未加载完成时为 0 */
  entryCount: number
  encoding: string
  version: string
  status: 'pending' | 'loading' | 'ready' | 'error'
  /** 是否为当前查询使用的词典 */
  current: boolean
  errorMessage?: string
}

export interface SuggestionItem {
  word: string
  /** 匹配方式，用于 UI 上区分前缀命中 / 拼写纠正 */
  kind: 'exact' | 'prefix' | 'fuzzy' | 'associate'
}

/** 这次查询从哪个入口来 —— 决定跑不跑兜底通道 */
export type LookupOrigin = 'input' | 'selection' | 'link' | 'back' | 'history'

/** 书写系统（只有三种）；判定只许在 C# 的 `LanguageDetector`，TS 侧不许再写一份汉字范围 */
export type ScriptKind = 'han' | 'latin' | 'other'

/** 兜底通道进行到哪一步（事件 `lookup:stage` 的载荷） */
export interface LookupStage {
  stage: 'current' | 'borrow' | 'translate'
  /** 正在查 / 正在翻译的那本词典的名字（翻译那一步为空串） */
  dictTitle: string
  word: string
}

/** 一次查词的结果，交给渲染进程渲染 */
export interface EntryPayload {
  /** 用户查询的原始词 */
  query: string
  /** 词典中命中条目的实际键名 */
  keyText: string
  dictId: string
  dictTitle: string
  /** 词条正文文档地址（dictres://…），直接赋给 iframe.src 即可 */
  entryUrl: string
  /** 纯文本，用于「复制释义」 */
  plainText: string
  /** 是否命中 */
  found: boolean
  /**
   * 这一页是怎么来的：`terminal` = 三个通道都试过、翻译用不上。
   * 同时写进 `#reader[data-via]` —— 自动化只认这个界面标记，不去正文里找那句话。
   */
  via?: 'current' | 'borrow' | 'translate' | 'terminal'
  /** 给界面写的解释行（例：当前词典没有 · 已用《X》借查） */
  reason?: string
  /** 终态页专用：「翻译为什么没用上」——四种分开说 */
  translateWhy?: string
  /** 终态页要不要给一条可点的「翻译这个词」（只在用户自己关了自动翻译时） */
  offerTranslate?: boolean
  /** 要不要给「再问一遍」（有没问完的词典时**必须**给） */
  offerRecheck?: boolean
  /** 没能问完的词典名 —— 非空时绝不许说「别的词典也都没有」 */
  unconfirmed?: string[]
  /** 选区那一路的兜底候选：并进正文框底部提示，不占正文区 */
  suggestions?: string[]
  /** 若经过 @@@LINK= 跳转，这里记录原始目标 */
  linkedTo?: string
  /** 未命中时的原因，便于 UI 提示 */
  message?: string
  /**
   * 朗读时要念的文本；缺省 = 念 {@link keyText}。只有机器翻译的伪词条会带它
   * （渲染进程自己拼的载荷，C# 那侧从不发）：那种词条的标题栏要显示原文、念的是译文。
   */
  speakText?: string
}

export interface HistoryItem {
  word: string
  dictId: string
  dictTitle: string
  at: number
}

/**
 * 「一次性落点查询」结果（主进程的 `dict:probe`）：`ok` + `landed` 非空 = 这本词典里有
 * （落点可能被 `@@@LINK=` 重定向过）、为 null = 确实没有；`timeout` / `error` 都不等于没有；
 * `missing` = 这本词典已不在词库里（界面那份列表过期了）。
 */
export interface DictProbeResult {
  state: 'ok' | 'timeout' | 'error' | 'missing'
  /** 落点：词典里的规范键名。只有 `state === 'ok'` 时才有意义 */
  landed: string | null
  dictId: string
  /** 这本词典显示用的名字（未加载时是文件名，与管理窗里一致） */
  title: string
  /** 失败 / 超时时的人话原因 */
  message: string
}

export interface WindowPlacement {
  /** 胶囊的屏幕坐标（DIP），窗口几何由它推导 */
  pillX: number
  pillY: number
  edge: SnapEdge
  /** 是否处于被吸入屏幕边缘的窄条状态 */
  absorbed?: boolean
}

/** 关闭窗口时询问用户的选择；记录后不再重复询问 */
export type CloseBehavior = 'ask' | 'quit' | 'tray'

/**
 * 「选项」窗口的页签，顺序即屏幕上的顺序：词库 → 语音 → 翻译 → 常规
 * （先有什么可查 → 查到了怎么念 → 查不到怎么办 → 程序自身）。
 * ⚠️ 必须与 `manager.html` 的 `<nav>` 一致：按顺序钉着，改一处要一起改。
 */
export type OptionsTab = 'dicts' | 'speech' | 'translate' | 'general'

export interface AppSettings {
  version: 1
  dictionaries: StoredDictionary[]
  currentDictId: string | null
  history: HistoryItem[]
  placement: WindowPlacement | null
  closeBehavior: CloseBehavior
  /** 启动时显不显示悬浮窗（默认 `true`）；下一次启动才生效 —— 它只管启动那一瞬间。 */
  showFloatingOnStartup: boolean
}

/** 渲染进程请求的布局尺寸 */
export interface LayoutRequest {
  /** 内容区宽度（不含胶囊阴影留白） */
  width: number
  /** 展开区高度；0 表示收起为纯胶囊 */
  panelHeight: number
  /**
   * 展开方向与胶囊对齐。必须由渲染进程决定：只有它能先把 DOM 的方向切好、
   * 再让主进程改窗口尺寸，否则中间会有一帧按旧方向排版，胶囊会先闪到新窗口顶端再弹回来。
   */
  direction: 'up' | 'down'
  align: 'left' | 'right'
}

/**
 * 窗口内一块真正会画出东西的矩形（窗口坐标，DIP）。WebView2 窗口没有逐像素透明，
 * 可见范围只能靠窗口 Region 裁，而哪里可见只有渲染进程说得准（胶囊、面板、菜单、对话框
 * 各自的位置），所以由它量好上报，主进程只负责把这些矩形并起来当窗口形状。
 */
export interface ShapeRect {
  x: number
  y: number
  w: number
  h: number
  /** 圆角半径 */
  r: number
  /**
   * 卡片底色与描边色（CSS 颜色字符串）。主进程拿它们在外面单独画一层外壳
   * （承载网页的窗口只能用 Region 裁硬边，抗锯齿与投影只能靠那层分层窗口画）。
   * 描边色必须是已经按透明度合成到底色上的结果，外壳直接填。
   */
  fill: string
  border: string
}

/** 主进程计算后实际生效的布局 */
export interface AppliedLayout {
  width: number
  panelHeight: number
  /** 展开区出现在胶囊下方还是上方 */
  direction: 'down' | 'up'
  /** 胶囊在窗口内是左对齐还是右对齐（吸入右边缘时内容向左生长） */
  align: 'left' | 'right'
  /** 胶囊直径与窗口投影留白，渲染进程据此排布 DOM */
  pillWidth: number
  pillHeight: number
  shadow: number
  /** 是否处于被吸入屏幕边缘的窄条状态 */
  absorbed: boolean
  /** 吸入到的边缘；absorbed 为 false 时也可能保留，表示仍贴在该边上 */
  absorbEdge: AbsorbEdge
  /** 已从窄条弹出但依然贴着该边缘 —— 鼠标移开后会自动重新吸入 */
  docked: boolean
  /**
   * 正在进行的吸入 / 弹出动画（'in' = 缩进边缘，'out' = 从边缘弹出）。
   * 渲染进程据此把胶囊宽度切成 100%、去掉留白，让它跟着窗口一起收放。
   */
  edgeAnim: 'in' | 'out' | null
  /** 胶囊左上角的屏幕坐标（DIP），渲染进程据此自己判断展开方向 */
  pillX: number
  pillY: number
  /**
   * 胶囊在窗口内的上边距（DIP）。窗口上下都预留了面板可能占用的空间（展开面板时窗口完全不动，
   * 否则窗口一搬家，WebView2 还没重画完的那一帧会把胶囊显示在错误的位置上），
   * 所以胶囊必须按这个值绝对定位，不能靠 flex 排在窗口顶/底。
   */
  pillTop: number
}

/** 可吸入的边缘；底边不参与（拖到下方不触发任何动作） */
export type AbsorbEdge = 'left' | 'right' | 'top' | null

export interface LayoutInfo {
  /** 当前显示器工作区，供渲染进程限制内容高度 */
  workArea: { x: number; y: number; width: number; height: number }
  scaleFactor: number
  edge: SnapEdge
  /** 胶囊左上角的屏幕坐标（DIP） */
  pillX: number
  pillY: number
  /** 胶囊在窗口内的上边距（DIP） */
  pillTop: number
}

export interface AddResult {
  added: DictionaryInfo[]
  updated: DictionaryInfo[]
  /** 无法归属到任何词典的 .mdd（缺少同名 .mdx），提示用户 */
  orphans: string[]
  canceled: boolean
}

/** 悬浮窗渲染进程可调用的 API（由 preload 通过 contextBridge 暴露） */
export interface FloatingApi {
  /* ---- 布局与窗口 ---- */
  setLayout(req: LayoutRequest): void
  /**
   * 上报窗口内的可见形状，主进程据此裁剪窗口 Region 并绘制外壳。
   * focused 只用来加深投影 —— 焦点提示不做彩色描边。
   */
  setShape(regions: ShapeRect[], theme?: 'light' | 'dark', focused?: boolean): void
  getLayoutInfo(): Promise<LayoutInfo>
  /**
   * 拖拽分三步：prepare 在指针按下时记录起点（保证拖动像素精确、不吃掉阈值位移），
   * start 在越过阈值时真正进入拖拽态，end 松手收尾（没真正拖过就只做清理）。
   * 主进程用 screen.getCursorScreenPoint() 采样光标，渲染进程只需报告这三个时机。
   */
  dragPrepare(): void
  dragStart(): void
  /** 拖动中的一次移动。sentAt = 这次 pointermove 的 Date.now()（毫秒），主进程拿它算拖动延迟。 */
  dragMove(sentAt?: number): void
  dragEnd(): void
  /** 鼠标移到屏幕边缘的窄条上：把界面弹出来 */
  expandFromEdge(): void
  /** 鼠标离开：重新吸入屏幕边缘 */
  collapseToEdge(): void
  /** 把悬浮窗放回默认位置（屏幕正下方居中、任务栏上方） */
  resetPosition(): void
  hideFloating(): void
  requestClose(): void
  /** 关闭询问对话框的选择结果回传 */
  resolveClose(choice: 'quit' | 'tray' | 'cancel', remember: boolean): void
  /** 打开「选项」窗口；tab 可选，用来直接翻到某一页 */
  openManager(tab?: OptionsTab): void
  openExternalDictionaryDialog(): Promise<AddResult>

  /* ---- 词典 ---- */
  listDictionaries(): Promise<DictionaryInfo[]>
  setCurrentDictionary(id: string): Promise<DictionaryInfo[]>
  removeDictionary(id: string): Promise<DictionaryInfo[]>
  addDictionaryFiles(): Promise<AddResult>

  /* ---- 查询 ---- */
  suggest(query: string, limit?: number): Promise<SuggestionItem[]>
  /*
     * 查词。**只有 `origin` 为 `input` / `selection` 才跑兜底通道**（当前词典 → 借查别本
     * → 机器翻译 → 终态页）；其余三个入口是精确还原（回退到当时那一页、点历史里那一条），
     * 跑通道会让退回一个查不到的词当场跳到别的词典或翻译上去。
     * 不传 = `input`；传了 `dictId` 也不跑通道 —— 那是明确的就在这一本里查。
     */
    lookup(word: string, dictId?: string, origin?: LookupOrigin): Promise<EntryPayload | null>
    /**
     * 这段输入的主要书写系统，只有三种。前端要问是因为约定「汉字输入不在当前词典做联想」，
     * 而什么算汉字只许有一处来源：判定在 C# 的 `LanguageDetector`，TS 侧不许再写一份汉字范围。
     */
    scriptOf(text: string): Promise<ScriptKind>
    /**
     * 「再问一遍」：把上一轮没能问完的词典重新问一次。`recheck = true` 时单本预算更大。
     * 返回的 `unconfirmed` 仍非空 = 这一次还是没问完 —— 绝不许说成「都没有」。
     */
    borrow(word: string, recheck?: boolean): Promise<{ hitId: string; hitTitle: string; unconfirmed: string[] }>
  /**
   * 这个词在当前词典里会落到哪条词条（大小写变体逐个试 + 逐层跟随 `@@@LINK=` 重定向，
   * 不做前缀补全、不做拼写纠正）；查不到回 `null`。与 {@link lookup} 同源，所以两者的
   * `keyText` 对同一个词必然一致 —— 正文里那个「查词」按钮就靠这条不变式在点击那一刻判断。
   * 只查表，不写查词历史：「查不到」与「落点就是当前词条」都不该算一次查词。
   */
  resolve(word: string, dictId?: string): Promise<string | null>
  /**
   * 「这本词典里有没有这个词」的轻查询：打开它、查一条、随即放掉（`resolve` 那条路会把
   * 词典留在内存里，所以只适合已经加载过的那本）。
   * ⚠️ 三态：只有 `state === 'ok'` 才代表查过了，`timeout` / `error` 不许当成这本里没有。
   */
  probe(word: string, dictId: string, timeoutMs?: number): Promise<DictProbeResult>
  copyText(text: string): Promise<void>
  readClipboard(): Promise<string>
  writeClipboard(text: string): Promise<void>

  /* ---- 历史 ---- */
  getHistory(offset: number, limit: number): Promise<{ items: HistoryItem[]; total: number }>
  clearHistory(): Promise<void>

  /* ---- 主进程 → 渲染进程的事件 ---- */
  onFocusInput(cb: () => void): () => void
  /**
   * 整个窗口的焦点变化（由外壳推来）。页面里的 `window.blur` 在 WebView2 里收不到
   * （点别的窗口时只有 `document.hasFocus()` 变 false，blur 一次都不触发），
   * 所以失焦要收起的两件事 —— 输入框那排复制 / 剪切浮层、右键菜单 —— 都靠它。
   */
  onWindowFocus(cb: (focused: boolean) => void): () => void
  onCloseRequested(cb: () => void): () => void
  onDictionariesChanged(cb: (list: DictionaryInfo[]) => void): () => void
  onSettingsChanged(cb: (settings: AppSettings) => void): () => void
  onLayoutApplied(cb: (layout: AppliedLayout) => void): () => void

    /** 兜底通道进行到哪一步（正在用《X》查… / 正在翻译…）—— 链条长，没有它用户以为卡住 */
    onLookupStage(cb: (stage: LookupStage) => void): () => void

  /* ---- 发音 ---- */
  /**
   * 当前这个词能不能念、按什么顺序念、念不了为什么。刻意是只算不产字节的轻操作
   * （不合成、不联网），查完词就能随手调；带上 dictId + keyText 才会算这个词条
   * 有没有词典自带的原录音。
   */
  speechStatus(
    text: string,
    dictTitle?: string | null,
    dictId?: string | null,
    keyText?: string | null
  ): Promise<SpeechStatus>
  /** 真的产出音频并返回可播放地址；播放由渲染进程负责，主进程只管给字节。 */
  speak(text: string, options?: SpeakOptions): Promise<SpeakResult>
  /**
   * 播放词条里某一段自带的音频（正文里点了某个 🔊）；与 {@link speak} 的区别是点的
   * 是指定那一段（可能是例句），不挑口音、也不兜底换音源。
   */
  playSound(dictId: string | null, key: string): Promise<SpeakResult>
  /** 改语音设置（传什么改什么），返回落盘后的完整设置 */
  speechSettings(patch: Partial<SpeechSettings>): Promise<SpeechSettings>
  /** 设置被那边改了（例如管理窗里动了音色），悬浮窗要跟着刷新按钮状态 */
  onSpeechSettingsChanged(cb: (settings: SpeechSettings) => void): () => void

  /* ---- 机器翻译 ---- */
  /**
   * 翻译的现状：有没有 Key、开关是不是用户开的、目标语种、缓存计数。
   * **不联网**（真连一次服务端是 {@link translateTest}），所以界面可以随手问它。
   */
  translateStatus(): Promise<TranslateStatus>
  /**
   * 翻一段文本。成功时连**正文框里的伪词条地址**一起给（`entryUrl`），
   * 于是译文能享受和真词条一样的待遇：朗读、复制、进返回栈、能回退。
   */
  translateText(text: string, dictTitle?: string | null, language?: string | null): Promise<TranslateResult>
  /** 「检测凭据」里翻译那一半：**绕过缓存**真发一次请求（缓存命中时它证明不了 Key 还能用） */
  translateTest(): Promise<TranslateTest>
  translateSettings(patch: Partial<TranslateSettings>): Promise<TranslateSettings>
  clearTranslateCache(): Promise<boolean>
  onTranslateSettingsChanged(cb: (settings: TranslateSettings) => void): () => void
}

/** 自绘托盘菜单的 API */
export interface TrayMenuApi {
  /** 菜单是复用窗口，每次展开都取最新状态 */
  getState(): Promise<{ floatingVisible: boolean; loginAtStartup: boolean }>
  /**
   * 量好内容尺寸回报给主进程，由它决定窗口几何后再显示。
   * 顺便把卡片配色带上：主进程要在外面单独画一层抗锯齿圆角和投影。
   */
  reportSize(size: {
    width: number
    height: number
    fill?: string
    border?: string
    theme?: 'light' | 'dark'
  }): void
  toggleFloating(): void
  /** 打开「选项」窗口；tab 可选，用来直接翻到某一页 */
  openManager(tab?: OptionsTab): void
  resetPosition(): void
  clearHistory(): void
  setLoginAtStartup(enabled: boolean): void
  quit(): void
  close(): void
  /** 主进程要求菜单展开 */
  onOpen(cb: () => void): () => void
  /** 开机自启动的真实状态回报（勾选后窗口不关，就地更新勾选标记） */
  onLoginChanged(cb: (enabled: boolean) => void): () => void
}

/* ── 发音：跨进程合同。主进程那边是供给层，渲染进程只管拿 url 丢给 <audio> ── */

/** 一个可用的发音音色 */
export interface SpeechVoice {
  id: string
  name: string
  /** 完整区域标记：en-US / zh-CN */
  culture: string
  /** 主语言代码：en / zh */
  language: string
  /** 语言中文名 + 区域：英语（美国） */
  languageLabel: string
  gender: string
  source: 'system' | 'online'
}

/** 音源规划里的一项：这条路能不能走、走起来什么样 */
export interface SpeechOption {
  /** dict = 词典自带原录音（.mdd 里的，词条里的 🔊 就是它）；system = 系统语音（离线）；online = 豆包语音（在线） */
  source: 'dict' | 'system' | 'online'
  label: string
  detail: string
  available: boolean
  reason: string
  voiceId?: string
}

/** 对当前这个词的语种判定 */
export interface SpeechDetected {
  text: string
  language: string
  label: string
  tag: string
  /** 判断依据（例：按字形判断（假名）），直接显示给用户 */
  reason: string
  /** 是不是中英混排 —— 豆包据此决定传不传 explicit_language、用哪个音色（混排走中文音色） */
  mixed: boolean
}

/* 音源顺序是产品定死的：词典自带音频 → 豆包语音 → 系统语音，前一条不可用就往下退；
   不设「默认音源」开关（多一个开关会让用户以为选了在线却没走在线）。 */

export interface SpeechSettings {
  rate: number
  voiceId?: string | null
  accent: 'auto' | 'uk' | 'us'
  defaultLanguage?: string | null
  /** API Key。只在用户真的重新输入时才出现在 patch 里 —— 后端从不回完整 Key，只给掩码。 */
  doubaoApiKey?: string | null
  /** 模型版本。本项目只用 2.0 音色，界面上是只读的 */
  doubaoResourceId?: string | null
  /** 英文音色（纯英文的词条走它）。留空 = 用内置默认音色（Dacey） */
  doubaoSpeakerEn?: string | null
  /** 中文音色：纯中文、中英混排、其它语种都走它。留空 = 用内置默认音色（Vivi），不是回落系统语音 */
  doubaoSpeakerZh?: string | null
  /** 音频格式（mp3） */
  doubaoFormat?: string | null
  /**
   * 两个音色各自的音量补偿（官方 `loudness_rate`，-50..100，越大约响）；null = 用内置表里
   * 量出来的默认值。一个音色一个值：两者天生响度差到 10 dB 量级，共用一个值调不平。
   */
  doubaoLoudnessEn?: number | null
  doubaoLoudnessZh?: number | null
}

/**
 * 豆包语音现在的配置状态，给管理窗渲染表单用。**没有完整 API Key**：界面只需要知道
 * 填没填、填的是哪一把，掩码就够了 —— 完整 Key 没有理由再从 C# 跑一趟 JSON。
 */
export interface DoubaoView {
  /** 请求地址（自动化验证会把端点指到本地桩服务上，界面照实显示） */
  endpoint: string
  /** 模型版本：恒为 seed-tts-2.0（界面只读） */
  resourceId: string
  format: string
  hasApiKey: boolean
  /** Key 的掩码（3a32a399…50c0），用来确认填的是哪一把 */
  apiKeyMasked: string
  /**
   * 英文音色 ID。界面只显示这个 ID，不显示 `speakerEnName` 那个官网名 —— 官网名要静态
   * 快照才敢确定、快照随时会过期。`speakerEnName` 字段留着是因为后端还在发。
   */
  speakerEn: string
  speakerEnName: string
  /** 中文音色 ID（界面同样只显示 ID，理由见上） */
  speakerZh: string
  speakerZhName: string
  /** 两个音色这次实际生效的音量补偿；设置里为 null 时就是下面那两个内置默认值，滑块显示它 */
  loudnessEn: number
  loudnessZh: number
  /** 内置表里量出来的默认值（音色不在表里时是 0 = 不补偿） */
  loudnessDefaultEn: number
  loudnessDefaultZh: number
  /** 配好了没有（有 Key + 至少有一个音色） */
  configured: boolean
  /** 没配好时缺什么，直接说给用户听 */
  reason: string
}

/** 豆包实测的结果（管理窗里的「检测豆包」） */
export interface DoubaoTest {
  /** 这次用的音色 ID（界面显示的就是它，`speakerName` 那个官网名不再显示） */
  speaker: string
  speakerName: string
  /** 这次测的语种（en / zh / ja …）与样本词 */
  language: string
  text: string
  /** 实际传的 explicit_language（空串 = 不传 = 中英混读） */
  explicitLanguage: string
  /** 实际请求的地址 —— 出错时看着它排查最快 */
  url: string
  ok: boolean
  statusCode: number
  bytes: number
  mime: string
  elapsedMs: number
  /** 这次计费的文本字数（官方 usage.text_words） */
  billedWords: number
  /** 后端给的人话错误，界面上必须原文照抄 */
  error: string
}

/**
 * 音量对齐：内置录音与系统语音各自的增益（dB，−24…+12）。单独一段而不是塞进 `SpeechSettings`：
 * 内置录音那份按词典存、系统语音那份全局一个，而写它们走 `setGains`（顺带要报这本词典有没有
 * 录音可校准）。⚠️ 后端还没落地这段时这里是 undefined —— 界面置灰并说明原因，不能拿 0 冒充 0。
 */
export interface VoiceGains {
  /** 当前词典（没有词典时是空串） */
  dictId: string
  dictTitle: string
  /** 内置录音的增益（dB）。按词典存，读的就是当前词典那一项 */
  dictGainDb: number
  /** 系统语音的增益（dB）。全局一个 */
  systemGainDb: number
  /** 内置录音这条路能不能校准（没词典 / 这本词典没有带录音的词条 → false） */
  dictAvailable: boolean
  /** 不能校准时缺什么，直接说给用户听 */
  dictMessage: string
  systemAvailable: boolean
  systemMessage: string
}

/** 一条可用于音量校准的内置录音样本（后端从当前词典里均匀挑的） */
export interface DictSample {
  word: string
  /** .mdd 里那条音频的实际路径（说明放的是哪一段） */
  file: string
  language: string
  languageLabel: string
  /**
   * 词典原录音的同源地址（`https://lookup.local/__sound__/<词典 id>/<mdd 键名>`），
   * 直接 `fetch` 就能拿到字节（CSP 的 connect-src 'self' 够用）也带 Range；
   * 与 `api.speak(text, { source: 'dict' })` 回的那个 url 是同一种形式。
   */
  url: string
}

/**
 * `dictSamples()` 的返回：当前词典里最多 6 条带录音的词条。⚠️ 现在没有界面调用它
 * （「平衡音量」那一段已删：跨素材比较不可靠），但桥接方法保留、诊断 `--dict-samples`
 * 还在验 —— 别看到没人调就把它删掉。
 */
export interface DictSamplesResult {
  ok: boolean
  message: string
  dictId: string
  dictTitle: string
  samples: DictSample[]
}

/** 语种清单的一项（设置界面里的下拉） */
export interface SpeechLanguageView {
  /** 语种代码：en / zh / ja … */
  code: string
  /** 中文名：英语 / 中文 / 日语 … */
  label: string
  /** 本机有没有这个语种的离线音色 —— 界面上标一下，用户一眼看出哪几种能离线念 */
  offline: boolean
}

/** speechStatus 的整体返回：音色清单 + 设置 + 对当前这个词的规划 */
export interface SpeechStatus {
  engineAvailable: boolean
  engineMessage: string
  voices: SpeechVoice[]
  settings: SpeechSettings
  detected: SpeechDetected
  options: SpeechOption[]
  /** 豆包音源在界面上的样子（管理窗照着渲染表单） */
  doubao: DoubaoView
  /** 内置录音 / 系统语音的增益（音量对齐那组滑块的值）；后端还没落地时为 undefined */
  voiceGains?: VoiceGains
  /**
   * 可选的语种清单。由 C# 给而不是界面自己写一份：那张「代码 → 中文名」的表在那边就是
   * 判定语种用的同一张表，两边各存一份迟早会对不上。
   */
  languages: SpeechLanguageView[]
  available: boolean
  message: string
  hint: string
  cacheCount: number
  cacheBytes: number
}

/** 一次发音的结果 */
export interface SpeakResult {
  ok: boolean
  /**
   * 可播放地址。两种前缀，播放侧一视同仁（都是本进程内的应答）：
   *   https://lookup.local/__speak__/…  合成/下载来的音频（按内容缓存）
   *   https://lookup.local/__sound__/…  词典自带的原录音（现取）
   */
  url: string
  mime: string
  source: 'dict' | 'system' | 'online'
  voiceId: string | null
  voiceName: string | null
  language: string
  languageLabel: string
  bytes: number
  cached: boolean
  /**
   * 遗留字段：以前在线是一串 URL 模板，这里填回命中的那一条；豆包只有一个端点，
   * C# 现在恒给 -1 —— 留着是为了不动线上格式，界面文案不要再依据它。
   */
  templateIndex: number
  /** 走词典原录音时：实际命中的文件名（说明放的是哪一段） */
  file?: string
  /**
   * 这一次播放该在客户端施加的增益（dB）。只有词典自带音频那条非 0：它的字节是 .mdd 里的
   * 原录音，没人替它调音量，所以由播放侧接进 Web Audio 的 `GainNode`（可提可压）；
   * 豆包与系统语音的增益是后端在合成时就调好的，这里恒为 0。老回包没有这个字段时按 0 处理。
   */
  gainDb?: number
  message: string
}

/** 发音：这一次要用哪条路、念成哪种语言 */
export interface SpeakOptions {
  dictTitle?: string | null
  source?: 'dict' | 'system' | 'online' | null
  /**
   * 走在线那条路时强制用这个音色；不传就按语种自动挑（纯英文 → 英文音色，
   * 中文 / 中英混排 / 其它语种 → 中文音色）。管理窗拖滑块试听要按音色单独发音，靠的就是它。
   */
  voiceId?: string | null
  language?: string | null
  /** 当前词条（走「词典自带音频」时必须给：原录音要按词典 + 词条去 .mdd 里找） */
  dictId?: string | null
  keyText?: string | null
  /** 这一次的响度补偿覆盖（在线专用）；不传 = 用设置里那个滑块的值 */
  loudness?: number | null
  /**
   * 这一次的增益覆盖（非豆包的两条音源专用），与上面的 `loudness` 对称。存在的理由只有一个：
   * 量系统语音的中性电平 —— 它的字节里已经烘进了设置里那个增益，不覆盖则量到的是带增益的电平，
   * 减法会把已经加上的那份再减一遍。所以量那一遍传 `gainDb: 0`，播放 / 试听一律不传。
   */
  gainDb?: number | null
}

/** 词库管理窗口可调用的 API */
export interface ManagerApi {
  /**
   * 渲染进程开机时拉一次「这次被要求打开哪一页」。窗口刚建好时推事件会丢（页面还在加载），
   * 所以走「先记下来、开机来拉」这条路。
   */
  consumeRequestedTab(): Promise<OptionsTab | null>
  /** 主进程要求翻到某一页（窗口已经开着时才走这条） */
  onTabRequested(cb: (tab: OptionsTab | '') => void): () => void
  listDictionaries(): Promise<DictionaryInfo[]>
  addDictionaryFiles(): Promise<AddResult>
  removeDictionary(id: string): Promise<DictionaryInfo[]>
  setCurrentDictionary(id: string): Promise<DictionaryInfo[]>
  /**
   * 重命名词典：只改显示名，不动硬盘上的文件。传空串 = 恢复默认名（.mdx 头部标题，
   * 没有就退回文件名）。
   */
  renameDictionary(id: string, title: string): Promise<DictionaryInfo[]>
  /**
   * 词典排序：把一本在清单里挪 `delta` 位（负数往前、正数往后）。到边界由内核夹住
   * （第一本再往前 = 清单原样、仍然成功），这里不必自己判边界。
   * ⚠️ 顺序不只是显示顺序：「当前词典」那格为空时，若干接口会兜底取第一本。
   */
  moveDictionary(id: string, delta: number): Promise<DictionaryInfo[]>
  openInExplorer(filePath: string): void
  /** 这一次跑的是哪个配置目录（设置 / 历史 / 缓存都在那儿）；「常规 → 数据」用它 */
  configDir(): Promise<string>
  /**
   * 清空查词历史（不可撤销）。托盘菜单与「常规 → 数据」两个入口都调它；
   * ⚠️ 调用方必须先自己做一次二次确认。
   */
  clearHistory(): Promise<void>
  closeManager(): void
  minimizeManager(): void
  /**
   * 标题栏拖动。WebView2 里 `-webkit-app-region: drag` 不生效，所以渲染进程在标题栏按下时
   * 喊一声，由主进程把捕获交给系统的窗口移动循环。
   */
  startWindowDrag(): void
  /**
   * 上报界面底色。主进程要在窗口外单独画一层抗锯齿圆角与投影（Region 只能裁硬边），
   * 那一层用这个颜色把卡片重画一遍，所以必须是实心色。
   */
  reportSurface(surface: {
    fill: string
    border: string
    theme: 'light' | 'dark'
    /** 窗口当前有没有焦点；主进程据此决定投影浓淡 */
    focused: boolean
  }): void
  getCloseBehavior(): Promise<CloseBehavior>
  setCloseBehavior(behavior: CloseBehavior): Promise<CloseBehavior>
  /** 启动时显不显示悬浮窗（下一次启动生效）；回的是内核认可的真实值 */
  getShowFloatingOnStartup(): Promise<boolean>
  setShowFloatingOnStartup(enabled: boolean): Promise<boolean>

  /* ---- 发音 ---- */
  /** 只为语音设置界面取音色清单与当前配置（text 传空即可，不做语种规划） */
  speechStatus(text: string, dictTitle?: string | null): Promise<SpeechStatus>
  /** 试听：走的是和悬浮窗完全一样的发音通道 */
  speak(text: string, options?: SpeakOptions): Promise<SpeakResult>
  speechSettings(patch: Partial<SpeechSettings>): Promise<SpeechSettings>
  clearSpeechCache(): Promise<boolean>
  /**
   * 拿样本词真去请求一次，验证豆包语音还通不通（保存前 /「检测豆包」都走它）。
   * 传了 speaker 就只测那一个音色（language 决定样本词），返回一项；留空则英文 + 中文各测一次，
   * 返回可能是两项。界面总是显式传音色，测的是即将存下去的那个，而不是设置里存着的旧配置。
   */
  testDoubao(speaker?: string | null, language?: string | null): Promise<DoubaoTest[]>
  /**
   * 当前词典里均匀挑出来的、带录音的词条样本（最多 6 条）。现在没有界面调用它
   * （「平衡音量」那一段已删），但桥接方法保留、诊断 `--dict-samples` 照旧验它。
   */
  dictSamples(): Promise<DictSamplesResult>
  /**
   * 写音量增益，返回更新后的语音状态视图。`dictGainDb` 只改当前词典那一项（各词典各存各的），
   * `systemGainDb` 是全局那一个；不传的字段不动，所以拖一个滑块不会顺手写另一个。
   */
  setGains(patch: { dictGainDb?: number; systemGainDb?: number }): Promise<SpeechStatus>
  onSpeechSettingsChanged(cb: (settings: SpeechSettings) => void): () => void

  /* ---- 机器翻译（「翻译」页） ---- */
  translateStatus(): Promise<TranslateStatus>
  translateSettings(patch: Partial<TranslateSettings>): Promise<TranslateSettings>
  /** 「检测凭据」的翻译那一半；语音那一半走 testDoubao，界面把两行并成一个按钮 */
  translateTest(): Promise<TranslateTest>
  clearTranslateCache(): Promise<boolean>
  onTranslateSettingsChanged(cb: (settings: TranslateSettings) => void): () => void
}

/* ── 机器翻译：主进程管语种映射 / 请求构造 / 缓存 / 调度，界面只问能不能翻、翻成什么 ── */

/**
 * 「用户自己关掉的」与「缺 Key 客观不可用」是两件不同的事，界面必须分开说 ——
 * 混成一种灰，用户会以为是自己关的，于是反复点那个开关，而真正的原因（没填 Key）一直没被说出来。
 */
export interface TranslateStatus {
  /** 账号级那把 Key 填了没有（语音与翻译共用同一把） */
  hasApiKey: boolean
  /** 用户自己的开关（默认关）—— 与 hasApiKey 分开判，别把两者合成一个 disabled */
  enabled: boolean
  /** auto = 中文译英、其余译中；zh / en = 固定方向 */
  targetMode: TranslateTargetMode
  /** 「查不到时自动翻译」（默认 true）—— 关掉时终态页会给一条可点的「翻译这个词」 */
  autoTranslate: boolean
  /** 请求端点（排错用：被指到本地桩服务时一眼能看出来） */
  endpoint: string
  /** 恒为 volc.speech.mt —— 它要在控制台单独开通，与语音的 seed-tts-2.0 不是一回事 */
  resourceId: string
  cache: { count: number; hits: number; misses: number }
}

export type TranslateTargetMode = 'auto' | 'zh' | 'en'

export interface TranslateSettings {
  enabled: boolean
  targetMode: TranslateTargetMode
  /** 「查不到时自动翻译」。不要再加回「词典命中时不再显示翻译」那类勾了没作用的开关。 */
  autoTranslate: boolean
}

/** 一次翻译的结果 */
export interface TranslateResult {
  ok: boolean
  /** 失败时的人话（成功时是空串） */
  message: string
  /** 这次翻的原文（已去首尾空白 —— 缓存键用的就是它） */
  text: string
  /** 译文；失败时是空串 */
  translation: string
  sourceLanguage: string
  targetLanguage: string
  /** 语种的中文名，直接显示（例：英语 → 中文） */
  sourceLabel: string
  targetLabel: string
  /**
   * 服务端识别出的源语种（只有请求没指定 source_language 时才回）；与我们判定的不一致时
   * 界面上标一句 —— 一次交叉验证，排错时最有用。
   */
  detected: string
  /** 本次真花掉的 tokens（命中缓存的那些不计费，所以不会虚高） */
  tokens: number
  cachedItems: number
  fetchedItems: number
  fromCache: boolean
  /**
   * 正文框里的伪词条地址（`https://translate.dictres.invalid/__entry__?tok=…`）。
   * 直接赋给词条 iframe 的 src 即可 —— 它和真词条的地址是同一种东西。
   */
  entryUrl: string
  /** 伪词条的「词典 id」（恒为 translate，永远不与真词典撞车） */
  dictId: string
  /** 伪词条的「词典名」（机器翻译）—— 标题栏照它显示，用户一眼看出这段不是词典给的 */
  dictTitle: string
}

/** 「检测凭据」里翻译那一行的结果 */
export interface TranslateTest {
  ok: boolean
  message: string
  elapsedMs: number
  tokens: number
  sample: string
  translation: string
}
