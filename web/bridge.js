/**
 * WebView2 版的"preload"：把 chrome.webview 的消息通道包成与 Electron 版一致的
 * window.dshLookup.{floating,manager,tray}（WebView2 没有 preload / contextBridge），界面代码一行都不用改。
 * 协议：渲染 → 宿主 { kind:'req', id, method, args, reply }（reply=false = 发完不管）；
 * 宿主 → 渲染 { kind:'res', id, ok, value|error } 与 { kind:'evt', name, payload }（收发实现在宿主侧的 ShellBridge.cs）。
 */
;(function () {
  'use strict'

  const host = window.chrome && window.chrome.webview
  const pending = new Map()
  const listeners = new Map()
  let sequence = 0

  function post(message) {
    if (!host) {
      console.error('[bridge] 没有检测到 WebView2 宿主，接口调用会被丢弃')
      return
    }
    host.postMessage(message)
  }

  /*
   * 超时兜底：正常路径下宿主侧总会回包（ShellBridge 顶层有 catch），但进程崩溃、回调链中断、
   * 或某个 case 忘了 reply 时，pending 里那条会永远挂着 —— 表现为 spinner 永转。
   * 60 秒取的是「正常调用里最长的一次（在线语音合成整段缓冲）也碰不到」的档位，只兜死、不抢跑。
   */
  const INVOKE_TIMEOUT_MS = 60000

  /** 需要宿主回结果 */
  function invoke(method) {
    const args = Array.prototype.slice.call(arguments, 1)
    return new Promise(function (resolve, reject) {
      if (!host) {
        reject(new Error('没有 WebView2 宿主'))
        return
      }
      const id = ++sequence
      const timer = setTimeout(function () {
        pending.delete(id)
        reject(new Error('主进程 ' + method + ' 超过 ' + INVOKE_TIMEOUT_MS / 1000 +
                         ' 秒没有回包（宿主可能崩了或没回话）'))
      }, INVOKE_TIMEOUT_MS)
      pending.set(id, {
        resolve: resolve,
        reject: reject,
        cancel: function () { clearTimeout(timer) },
      })
      post({ kind: 'req', id: id, method: method, args: args, reply: true })
    })
  }

  /** 发完不管 */
  function send(method) {
    const args = Array.prototype.slice.call(arguments, 1)
    post({ kind: 'req', method: method, args: args, reply: false })
  }

  function subscribe(name, callback) {
    if (!listeners.has(name)) listeners.set(name, new Set())
    listeners.get(name).add(callback)
    return function () {
      listeners.get(name).delete(callback)
    }
  }

  if (host) {
    host.addEventListener('message', function (event) {
      const data = event.data
      if (!data || typeof data !== 'object') return

      if (data.kind === 'res') {
        const entry = pending.get(data.id)
        if (!entry) return
        pending.delete(data.id)
        if (entry.cancel) entry.cancel()
        if (data.ok) entry.resolve(data.value)
        else entry.reject(new Error(data.error || '主进程调用失败'))
        return
      }

      if (data.kind === 'evt') {
        const set = listeners.get(data.name)
        if (!set) return
        set.forEach(function (callback) {
          try {
            callback(data.payload)
          } catch (err) {
            console.error('[bridge] 事件处理异常 ' + data.name, err)
          }
        })
      }
    })
  }

  /* ── 悬浮窗 ─────────────────────────────────────────────────────────────── */

  const floating = {
    /* ---- 布局与窗口 ---- */
    setLayout: function (request) {
      send('layout:set', request)
    },
    getLayoutInfo: function () {
      return invoke('layout:info')
    },
    /**
     * 上报窗口内"真正有内容"的形状。WebView2 版窗口没有逐像素透明，可见区域靠窗口 Region 裁出来，
     * 宿主还要拿这些矩形（连同底色/描边色）在外面画一层抗锯齿的外壳。
     * focused 只用来把投影加深一点，不做彩色描边。
     */
    setShape: function (regions, theme, focused) {
      send('shape:set', regions, theme || 'light', !!focused)
    },
    /**
     * 首帧已经出来了（页面在 bootstrap 末尾等**两次** rAF 才发）。
     * 主进程在那之前把窗口停在**屏幕外**：不然用户会先看到一层空壳 / 阴影，再看到真正的内容。
     * 发完不管（没有回值）。
     */
    bootReady: function () {
      send('boot:ready')
    },
    dragPrepare: function () {
      send('drag:prepare')
    },
    dragStart: function () {
      send('drag:start')
    },
    dragMove: function (sentAt) {
      // sentAt = 这次 pointermove 的 Date.now()：宿主拿它算"从手指动到窗口真的挪过去"的延迟
      send('drag:move', typeof sentAt === 'number' ? sentAt : null)
    },
    dragEnd: function () {
      send('drag:end')
    },
    expandFromEdge: function () {
      send('edge:expand')
    },
    collapseToEdge: function () {
      send('edge:collapse')
    },
    resetPosition: function () {
      send('floating:reset')
    },
    hideFloating: function () {
      send('window:hide')
    },
    requestClose: function () {
      send('window:requestClose')
    },
    resolveClose: function (choice, remember) {
      send('window:resolveClose', choice, !!remember)
    },
    openManager: function (tab) {
      // tab 可选：'general' / 'dicts' / 'speech' / 'translate'，用来"打开并直接翻到某一页"
      return invoke('manager:open', tab == null ? null : tab)
    },
    openExternalDictionaryDialog: function () {
      return invoke('dict:add')
    },

    /* ---- 词典 ---- */
    listDictionaries: function () {
      return invoke('dict:list')
    },
    setCurrentDictionary: function (id) {
      return invoke('dict:setCurrent', id)
    },
    removeDictionary: function (id) {
      return invoke('dict:remove', id)
    },
    addDictionaryFiles: function () {
      return invoke('dict:add')
    },

    /* ---- 查询 ---- */
    suggest: function (query, limit) {
      return invoke('dict:suggest', query, typeof limit === 'number' ? limit : 12)
    },
    /*
     * `origin` = 这次查询从哪个入口来：input / selection / link / back / history。
     * **只有 input 与 selection 会跑兜底通道**（当前词典 → 借查别本 → 机器翻译 → 终态页），
     * 其余三个入口一律不跑（它们是"精确还原"）；不传 = input，与老调用方完全兼容。
     */
    lookup: function (word, dictId, origin) {
      return invoke('dict:lookup', word, dictId === undefined ? null : dictId, origin === undefined ? null : origin)
    },
    /** 这段输入的主要书写系统：han / latin / other（判定只有一处来源，见 C# 的 LanguageDetector） */
    scriptOf: function (text) {
      return invoke('text:script', text == null ? '' : text)
    },
    /** 「再问一遍」：把上一轮没能问完的词典重新问一次（单本预算更大） */
    borrow: function (word, recheck) {
      return invoke('dict:borrow', word == null ? '' : word, recheck === true)
    },
    /**
     * 这个词在**当前词典**里会落到哪条词条（大小写变体算命中、逐层跟随 `@@@LINK=` 重定向），查不到回 `null`；
     * 只查表，**不写查词历史**（点击那一刻会被调，"查不到"和"落点就是当前词条"都不该算一次查词）。
     * 问"落点"而不是"在不在"：只回答在不在的话，"选中的是 `apples`、词典把它重定向到 `apple`"会被混进"在"里，
     * 点下去就是把同一篇正文重载一遍。第 2 个参数可选 = 只在指定的那本里问落点（空状态"该推哪一本"靠它）。
     */
    resolve: function (word, dictId) {
      return invoke('dict:resolve', word, dictId === undefined ? null : dictId)
    },
    /**
     * 「这本词典里有没有这个词」的**轻查询**：宿主打开它、查一条、随即放掉，**不把那本词典留在内存里**
     * （`dict:resolve` 会——它走 LoadAsync）。空状态挑"该推哪一本"用它，于是**没被加载过的词典也能参与**。
     * `state` 为 `ok`（`landed` 非空 = 有）/ `timeout`（没查完，**不等于没有**）/ `error`（打不开，
     * 同样不等于没有）/ `missing`（这本已不在词库里）。
     */
    probe: function (word, dictId, timeoutMs) {
      return invoke(
        'dict:probe',
        word == null ? '' : word,
        dictId == null ? null : dictId,
        typeof timeoutMs === 'number' ? timeoutMs : null
      )
    },
    copyText: function (text) {
      return invoke('clipboard:write', text)
    },
    readClipboard: function () {
      return invoke('clipboard:read')
    },
    writeClipboard: function (text) {
      return invoke('clipboard:write', text)
    },

    /* ---- 历史 ---- */
    getHistory: function (offset, limit) {
      return invoke('history:get', offset, limit)
    },
    clearHistory: function () {
      return invoke('history:clear')
    },

    /* ---- 发音 ---- */
    /**
     * 当前这个词的发音规划（音色清单 + 设置 + 能不能念）。只做判断，不合成也不联网 ——
     * 所以查完词就可以放心调它来刷新按钮状态。dictId + keyText 是"当前词条"，
     * 带上它才算得出这一条有没有词典自带的原录音。
     */
    speechStatus: function (text, dictTitle, dictId, keyText) {
      return invoke(
        'speech:status',
        text == null ? '' : text,
        dictTitle == null ? null : dictTitle,
        dictId == null ? null : dictId,
        keyText == null ? null : keyText
      )
    },
    /**
     * 真的产出一段音频，返回可播放地址（`__speak__` 合成音频 / `__sound__` 词典原录音）。
     * options 里的字段都是"这一次"的临时指定：音源 / 音色 / 语种 / 当前词条。
     */
    speak: function (text, options) {
      const opts = options || {}
      return invoke(
        'speech:speak',
        text == null ? '' : text,
        opts.dictTitle == null ? null : opts.dictTitle,
        opts.source == null ? null : opts.source,
        opts.voiceId == null ? null : opts.voiceId,
        opts.language == null ? null : opts.language,
        opts.dictId == null ? null : opts.dictId,
        opts.keyText == null ? null : opts.keyText,
        // 第 8 个：这一次的响度补偿覆盖（音量滑块试听 / 平衡音量要用它量中性电平）
        opts.loudness == null ? null : opts.loudness,
        // 第 9 个：这一次的增益覆盖（非豆包那两条音源）。只给"量系统语音中性电平"用 —— 传 0
        opts.gainDb == null ? null : opts.gainDb
      )
    },
    /** 播放词条里指定的那一段自带音频（词条正文里点了某个 🔊；可能是例句） */
    playSound: function (dictId, key) {
      return invoke('speech:sound', dictId == null ? null : dictId, key == null ? '' : key)
    },
    /** 改发音设置（传什么改什么），返回落盘后的完整设置 */
    speechSettings: function (patch) {
      return invoke('speech:setSettings', patch || {})
    },
    /** 清空发音缓存（内存 + 磁盘） */
    clearSpeechCache: function () {
      return invoke('speech:clearCache')
    },
    /** 拿样本词真去请求一次，验证豆包语音是否还通（不传音色时后端会英文 + 中文各测一次） */
    testDoubao: function (speaker, language) {
      return invoke(
        'speech:testDoubao',
        speaker == null ? null : speaker,
        language == null ? null : language
      )
    },
    onSpeechSettingsChanged: function (cb) {
      return subscribe('speech:settings-changed', cb)
    },

    /* ---- 机器翻译 ---- */
    /** 翻译的现状：有没有 Key（`hasApiKey`）、开关是不是用户开的、目标语种、缓存计数；**不联网**，可以反复问 */
    translateStatus: function () {
      return invoke('translate:status')
    },
    /**
     * 翻一段文本。成功时除了译文，还回一个 `entryUrl` —— 那是**正文框里的伪词条地址**，
     * 于是译文和真词条一样能朗读、能复制、能进返回栈。
     * `dictTitle` 是语种判定的线索（"牛津高阶英汉双解" → 英语），`language` 用来强制语种。
     */
    translateText: function (text, dictTitle, language) {
      return invoke(
        'translate:text',
        text == null ? '' : text,
        dictTitle == null ? null : dictTitle,
        language == null ? null : language
      )
    },
    /** 「检测凭据」里翻译那一半：**绕过缓存**真发一次请求（走缓存的话它只会汇报缓存很好） */
    translateTest: function () {
      return invoke('translate:test')
    },
    /** 改翻译设置（传什么改什么），返回落盘后的完整设置 */
    translateSettings: function (patch) {
      return invoke('translate:setSettings', patch || {})
    },
    clearTranslateCache: function () {
      return invoke('translate:clearCache')
    },
    onTranslateSettingsChanged: function (cb) {
      return subscribe('translate:settings-changed', cb)
    },

    /* ---- 事件订阅 ---- */
    onFocusInput: function (cb) {
      return subscribe('floating:focus-input', function () {
        cb()
      })
    },
    /**
     * 整个窗口的焦点变化（由外壳推来）。页面里的 `window.blur` **收不到** —— 点别的窗口时只是
     * `document.hasFocus()` 悄悄变成 false，于是"失焦就该收起"的浮层（复制/剪切按钮、右键菜单）全都收不了；
     * 外壳那边的 Deactivate / Activated 可靠（自动吸回屏幕边缘用的就是同一个信号）。
     */
    onWindowFocus: function (cb) {
      return subscribe('floating:window-focus', function (payload) {
        cb(!!(payload && payload.focused))
      })
    },
    onCloseRequested: function (cb) {
      return subscribe('close:requested', function () {
        cb()
      })
    },
    onDictionariesChanged: function (cb) {
      return subscribe('dictionaries:changed', cb)
    },
    onSettingsChanged: function (cb) {
      return subscribe('settings:changed', cb)
    },
    onLayoutApplied: function (cb) {
      return subscribe('layout:applied', cb)
    },
    /* 兜底通道进行到哪一步了（"正在用《X》查…" / "正在翻译…"）；链条长，没有它用户会以为程序卡住 */
    onLookupStage: function (cb) {
      return subscribe('lookup:stage', cb)
    }
  }

  /* ── 词库管理窗 ─────────────────────────────────────────────────────────── */

  const manager = {
    /**
     * 页面开机时拉一次"这次要求打开哪一页"：宿主在窗口刚建好时推事件会丢（页面还在加载），
     * 所以那条路走"先记下来、开机来拉"。
     */
    consumeRequestedTab: function () {
      return invoke('manager:initialTab')
    },
    /** 主进程要求翻到某一页（窗口已经开着时才会走这条） */
    onTabRequested: function (cb) {
      return subscribe('options:tab', function (payload) {
        cb(payload && payload.tab ? payload.tab : '')
      })
    },
    listDictionaries: function () {
      return invoke('dict:list')
    },
    addDictionaryFiles: function () {
      return invoke('dict:add')
    },
    removeDictionary: function (id) {
      return invoke('dict:remove', id)
    },
    setCurrentDictionary: function (id) {
      return invoke('dict:setCurrent', id)
    },
    /**
     * 重命名词典（只改显示名，不动硬盘上的文件）：传空串 = 恢复默认名（.mdx 头部标题 → 文件名）。
     * 返回值是刷新后的词典列表，调用方顺手更新列表即可。
     */
    renameDictionary: function (id, title) {
      return invoke('dict:rename', id, title)
    },
    openInExplorer: function (filePath) {
      invoke('shell:openPath', filePath)
    },
    /**
     * 这一次跑的是哪个配置目录（设置 / 历史 / 缓存都在这儿）；「常规 → 数据」用它显示路径，
     * 并把同一个字符串递给 `openInExplorer` 打开。
     */
    configDir: function () {
      return invoke('shell:configDir')
    },
    /**
     * 清空查词历史（**不可撤销**）。与托盘菜单那条 `tray.clearHistory` 是同一个 `history:clear`
     * case（那一份留着给诊断与自检用）；调用方**必须**自己先做一次二次确认。
     */
    clearHistory: function () {
      return invoke('history:clear')
    },
    /**
     * 词典排序（内核的 `dict:move`）：`delta` 负数往前、正数往后。到边界由**内核**夹住
     * （界面那两颗按钮在头尾置灰，这里不重复判边界）；返回值是移动之后的清单。
     */
    moveDictionary: function (id, delta) {
      return invoke('dict:move', id, delta)
    },
    closeManager: function () {
      invoke('manager:close')
    },
    minimizeManager: function () {
      invoke('manager:minimize')
    },
    /** WebView2 里 -webkit-app-region 不生效，标题栏拖动改由主进程接管 */
    startWindowDrag: function () {
      send('manager:drag')
    },
    /** 上报界面底色与焦点状态，供主进程在外面画抗锯齿圆角、投影和焦点浓淡 */
    reportSurface: function (surface) {
      send('manager:surface', surface.fill, surface.border, surface.theme, !!surface.focused)
    },
    getCloseBehavior: function () {
      return invoke('settings:get').then(function (settings) {
        return settings.closeBehavior
      })
    },
    setCloseBehavior: function (behavior) {
      return invoke('settings:setCloseBehavior', behavior)
    },
    /**
     * 「启动时显示悬浮窗」：与 `closeBehavior` 一样是**窗口级**的顶层设置（内核 `settings.showFloatingOnStartup`），
     * 读走 `settings:get`、写走它自己那一条补丁接口。
     * ⚠️ **下一次启动**才生效 —— 它只管启动那一瞬间。
     */
    getShowFloatingOnStartup: function () {
      return invoke('settings:get').then(function (settings) {
        // 老设置文件里没有这个键时按**显示**（与内核默认值一致）
        return settings.showFloatingOnStartup !== false
      })
    },
    setShowFloatingOnStartup: function (enabled) {
      return invoke('settings:setShowFloatingOnStartup', !!enabled)
    },

    /* ---- 发音（管理窗里的"发音"设置区与试听） ---- */
    /*
     * ⚠️ 后两个参数（dictId + keyText）**必须一起转发**：宿主侧只有拿到它们才会去算
     * "这个词条有没有词典自带的原录音"。漏传就会永远回"本词条没有自带录音"（而 `speech:speak`
     * 那条路传全了、同一个词条念得出来），内置录音试听要靠这个判断能不能点 ——
     * 这是**功能级**的错，不是风格问题。
     */
    speechStatus: function (text, dictTitle, dictId, keyText) {
      return invoke(
        'speech:status',
        text == null ? '' : text,
        dictTitle == null ? null : dictTitle,
        dictId == null ? null : dictId,
        keyText == null ? null : keyText
      )
    },
    speak: function (text, options) {
      const opts = options || {}
      return invoke(
        'speech:speak',
        text == null ? '' : text,
        opts.dictTitle == null ? null : opts.dictTitle,
        opts.source == null ? null : opts.source,
        opts.voiceId == null ? null : opts.voiceId,
        opts.language == null ? null : opts.language,
        opts.dictId == null ? null : opts.dictId,
        opts.keyText == null ? null : opts.keyText,
        // 第 8 个：这一次的响度补偿覆盖（音量滑块试听 / 平衡音量要用它量中性电平）
        opts.loudness == null ? null : opts.loudness,
        // 第 9 个：这一次的增益覆盖（非豆包那两条音源）。只给"量系统语音中性电平"用 —— 传 0
        opts.gainDb == null ? null : opts.gainDb
      )
    },
    speechSettings: function (patch) {
      return invoke('speech:setSettings', patch || {})
    },
    clearSpeechCache: function () {
      return invoke('speech:clearCache')
    },
    testDoubao: function (speaker, language) {
      return invoke(
        'speech:testDoubao',
        speaker == null ? null : speaker,
        language == null ? null : language
      )
    },
    /**
     * 当前词典里均匀挑出来的、带录音的词条样本（最多 6 条）。url 是词典原录音的同源地址
     * （`__sound__/<词典 id>/<mdd 键名>`），页面直接 fetch 就能拿到字节量电平。
     */
    dictSamples: function () {
      return invoke('speech:dictSamples')
    },
    /**
     * 写音量增益，返回更新后的语音状态视图（和 speech:status 同一份）。
     * dictGainDb 只改**当前词典**那一项，systemGainDb 是全局那一个；不传的字段不动。
     */
    setGains: function (patch) {
      return invoke('speech:setGains', patch || {})
    },
    onSpeechSettingsChanged: function (cb) {
      return subscribe('speech:settings-changed', cb)
    },

    /* ---- 机器翻译（「翻译」页） ---- */
    /**
     * 缺 Key 时这一页要**分开说**两件事：「用户自己关掉的」与「缺 Key 客观不可用」混成一种灰，
     * 用户会以为是自己关的，于是反复点那个开关。
     */
    translateStatus: function () {
      return invoke('translate:status')
    },
    translateSettings: function (patch) {
      return invoke('translate:setSettings', patch || {})
    },
    /** 「检测凭据」的翻译那一半；语音那一半走已有的 testDoubao，前端把两行并成一个按钮 */
    translateTest: function () {
      return invoke('translate:test')
    },
    clearTranslateCache: function () {
      return invoke('translate:clearCache')
    },
    onTranslateSettingsChanged: function (cb) {
      return subscribe('translate:settings-changed', cb)
    }
  }

  /* ── 自绘托盘菜单 ───────────────────────────────────────────────────────── */

  const tray = {
    getState: function () {
      return invoke('traymenu:state')
    },
    reportSize: function (size) {
      send('traymenu:measured', size)
    },
    toggleFloating: function () {
      send('traymenu:toggle')
    },
    openManager: function (tab) {
      send('traymenu:manager', tab == null ? null : tab)
    },
    resetPosition: function () {
      send('traymenu:reset')
    },
    clearHistory: function () {
      send('traymenu:clear-history')
    },
    setLoginAtStartup: function (enabled) {
      send('traymenu:login', !!enabled)
    },
    quit: function () {
      send('traymenu:quit')
    },
    close: function () {
      send('traymenu:close')
    },
    onOpen: function (cb) {
      return subscribe('traymenu:open', function () {
        cb()
      })
    },
    onLoginChanged: function (cb) {
      return subscribe('traymenu:login-changed', cb)
    }
  }

  /* ── 调试钩子 ─────────────────────────────────────────────────────────────────
     只在宿主显式打开（LOOKUP_DEBUG_PORT / LOOKUP_DEBUG_HOOKS）时才可用。自动化验证靠它读窗口矩形与
     窗口 Region —— 屏幕截图会被锁屏挡掉，而这两个数据是系统自己算出来的，比数像素可靠。
     ──────────────────────────────────────────────────────────────────────────── */

  const debug = {
    window: function (role) {
      return invoke('debug:window', role || 'floating')
    },
    absorb: function (edge) {
      return invoke('debug:absorb', edge)
    },
    /** 直接把胶囊摆到指定位置（DIP）：真实拖拽要走系统光标，自动化测不了精确落点 */
    placePill: function (x, y) {
      return invoke('debug:placePill', x, y)
    },
    showTrayMenu: function () {
      return invoke('debug:showTrayMenu')
    },
    /** 把焦点明确交给某个窗口（floating / tray / manager） */
    focus: function (role) {
      return invoke('debug:focus', role || 'floating')
    },
    hideTrayMenu: function () {
      return invoke('debug:hideTrayMenu')
    },
    /** 最近一次拖动的跟手统计（帧数 / 延迟中位 / p95 / 最大，毫秒） */
    dragStats: function () {
      return invoke('debug:dragStats')
    },
    /**
     * 让宿主在后台睡 ms 毫秒再回包：与机器翻译 / 在线发音走的是同一条两段式（后台做事 →
     * 回 UI 线程回包）。它不做业务、不写设置、不联网，专门用来量"等待期间界面还响不响应"。
     */
    slowReply: function (ms) {
      return invoke('debug:slowReply', ms)
    },
    /** **这条请求在哪条托管线程上被处理**（自检拿它当"UI 线程"的基准） */
    thread: function () {
      return invoke('debug:thread')
    },
    importDictionary: function (filePath) {
      return invoke('debug:importDictionary', filePath)
    },
    /**
     * 把一本词典从内存里请出去（**只掉内存，词库里还在**）。
     * 为什么自动化需要它：导入会顺手 `WarmUp` 新加的那一本，"导入过"就等于"已经加载过"，
     * 而"词库里有这本、但它没被加载"（刚启动只预热上次用的那一本）这个场面没有它就摆不出来 ——
     * 借查那条路只能在假前提下验，那正是它漏掉 bug 的原因。
     */
    unloadDictionary: function (dictId) {
      return invoke('debug:unloadDictionary', dictId)
    }
  }

  window.dshLookup = { floating: floating, manager: manager, tray: tray, debug: debug }
})()
