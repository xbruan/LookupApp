/**
 * 播放侧的增益路由：`<audio>.volume` 上限是 1、提不上去，要提升就得串 `GainNode`；三条硬约束：
 * ① 图只在 `warmUp()` 里建、且等 `state === 'running'` 才接（一旦接上，这个元素的输出就只走图，
 * context 是 suspended 时接上等于没声音）；② 任一步失败就一个节点都不接、退回 volume；③ 增益每次播放都要重设。
 */

/** 这次播放实际用的方式：graph = 走音频图（可提可压）；volume = 退路（只能压低） */
export type SpeechRouteMode = 'graph' | 'volume'

/** 页面里的角色，只用于「问主进程调试钩子开没开」 */
export type SpeechRouteRole = 'floating' | 'manager'

export interface SpeechRouteResult {
  mode: SpeechRouteMode
  /** 这一次**实际**施加的增益（dB）。退路下正增益做不到，这里是 0 */
  gainDb: number
  /** 要说给用户 / 日志的一句话（没用音频图时非空，空串表示没什么要说的） */
  note: string
}

/** 诊断用的句柄（只在调试钩子开着时才挂到 window 上，见 SpeechAudioRoute.expose） */
export interface SpeechGraphHandle {
  role: SpeechRouteRole
  element: HTMLAudioElement
  /** 建起来了才有，走退路时是 null —— 诊断看这三个字段，别从 `gain.value` 反推 */
  context: AudioContext | null
  source: MediaElementAudioSourceNode | null
  gain: GainNode | null
  /** 当前方式 + 为什么没用图（诊断读它，不看 gain.value 反推） */
  status(): { mode: SpeechRouteMode; note: string }
  /** 播放前调一次（页面里就是这么调的）；退路模式下同样能调，量的是它把 volume 设成了什么 */
  apply(gainDb: number): SpeechRouteResult
  /**
   * 把分析节点**串在 gain 与 destination 之间**量输出电平，返回还原用的函数。
   * 必须量到真的送往 destination 的那一路，所以由页面提供而不是诊断自己 disconnect
   * （手拆会把页面的图拆坏）；走退路（没有图）时它会抛。
   */
  tap(analyser: AnalyserNode): () => void
}

/** 主进程那三个调试入口在 bridge 里的样子（页面看不到 LOOKUP_DEBUG_HOOKS 这个环境变量，只能问它） */
interface DebugBridgeLike {
  debug?: { window?: (role: string) => Promise<unknown> }
}

const sleep = (ms: number): Promise<void> => new Promise((resolve) => setTimeout(resolve, ms))

function describe(err: unknown): string {
  return err instanceof Error ? err.message : String(err)
}

export class SpeechAudioRoute {
  private readonly element: HTMLAudioElement
  private readonly role: SpeechRouteRole
  private context: AudioContext | null = null
  private source: MediaElementAudioSourceNode | null = null
  private gain: GainNode | null = null
  /** 正在热身（避免同一次点击重复建 context） */
  private warming: Promise<void> | null = null
  /** 上一次播放实际用的方式 */
  private mode: SpeechRouteMode = 'volume'
  /** 「为什么这次没用音频图」；非空 = 走的是退路那条路 */
  private reason = ''

  constructor(element: HTMLAudioElement, role: SpeechRouteRole) {
    this.element = element
    this.role = role
  }

  /**
   * 用户按下发音那一刻调一次：把音频图热起来。异步、**不等**（这次播放照常往下走）——
   * 图必须在用户手势里建，而拿到音频字节还要等一次 IPC / 合成，那时手势已经过去了。
   * 起不来的话下一次点击还会再试（`warming` 会清掉）。
   */
  warmUp(): void {
    if (this.gain || this.warming) return
    const ctor =
      (window as unknown as { AudioContext?: typeof AudioContext }).AudioContext ||
      (window as unknown as { webkitAudioContext?: typeof AudioContext }).webkitAudioContext
    if (!ctor) {
      this.note('这个内核没有 Web Audio')
      return
    }
    let context: AudioContext
    try {
      context = new ctor()
    } catch (err) {
      this.note('建不了 AudioContext：' + describe(err))
      return
    }
    this.warming = this.connectWhenRunning(context).finally(() => {
      this.warming = null
    })
  }

  /**
   * 等到 context 真的在跑，再把元素接进图：`resume()` 在自动播放策略挡着时可能一直不落地，
   * 所以只等一个上限，超时按「这次起不来」处理。
   */
  private async connectWhenRunning(context: AudioContext): Promise<void> {
    try {
      await Promise.race([context.resume().catch(() => undefined), sleep(600)])
      if (context.state !== 'running') {
        /*
         * 起不来就**一个节点都不接**：接上以后这个元素的输出只走图，而图是 suspended 的，
         * 用户听到的就是「点了没声音」；关掉它，让这次播放走 volume 那条退路。
         */
        void context.close().catch(() => undefined)
        this.note(`音频图这次没起来（AudioContext 是 ${context.state}）`)
        return
      }
      const source = context.createMediaElementSource(this.element)
      const gain = context.createGain()
      source.connect(gain)
      gain.connect(context.destination)
      this.context = context
      this.source = source
      this.gain = gain
      this.reason = ''
      /*
       * 走图这条路时必须把元素音量钉回 1：`<audio>.volume` 串在图的输入之前、还会再乘一次，
       * 留着退路路径写进去的旧值就会两处衰减叠在一起。
       */
      this.element.volume = 1
      // 图接好了：把诊断句柄挂上（「有没有图」这一步就靠它区分，量输出电平也用它）
      this.expose()
    } catch (err) {
      // createMediaElementSource 也可能抛（例如同一元素重复接图）——同样退回 volume。
      // AudioContext 是稀缺资源：留一个没接任何东西的 context，点几次之后新的就开不出来了，顺手关掉。
      void context.close().catch(() => undefined)
      this.note('建音频图失败：' + describe(err))
    }
  }

  /**
   * 播放前调一次：把这一次的增益施加到播放器上。调用方必须**每次都调**，
   * 并把返回的 `note` 如实带进结果 / 日志里 —— 退回 volume 时正增益提不上去，不说出来只会觉得「调了没用」。
   */
  apply(gainDb: number): SpeechRouteResult {
    const wanted = typeof gainDb === 'number' && Number.isFinite(gainDb) ? gainDb : 0
    if (this.gain) {
      // 可提可压：+12 dB → 3.98 倍，−24 dB → 0.063 倍
      this.gain.gain.value = Math.pow(10, wanted / 20)
      this.element.volume = 1
      this.publish('graph', wanted, '')
      return { mode: 'graph', gainDb: wanted, note: '' }
    }

    /*
     * 退路：元素音量上限是 1，**只能压低**。这里如实回报实际做到了多少 ——
     * 报成想要的 +6 dB 就成了假话，而用户正是靠这个数值判断设置生效了没有。
     */
    const factor = Math.min(1, Math.pow(10, wanted / 20))
    this.element.volume = Math.max(0, factor)
    const effective = factor >= 1 ? 0 : 20 * Math.log10(factor)
    const note =
      `${this.reason || '音频图还没起来'}：这次按 <audio>.volume 播，` +
      (wanted > 0.05
        ? `${wanted.toFixed(1)} dB 的提升做不到（元素音量上限就是 1，只按 0 dB 算）`
        : `实际压低 ${effective.toFixed(1)} dB`)
    this.publish('volume', effective, note)
    console.warn('[speech] ' + note)
    return { mode: 'volume', gainDb: effective, note }
  }

  /** 这套数据既给诊断读（dataset），也是「这次到底怎么播的」留痕 */
  private publish(mode: SpeechRouteMode, gainDb: number, note: string): void {
    this.mode = mode
    if (this.reason && !note) note = this.reason
    this.element.dataset.speechGraph = mode
    this.element.dataset.speechGainDb = gainDb.toFixed(2)
    this.element.dataset.speechGraphNote = note
  }

  /**
   * 记下「为什么这次没用音频图」，顺手把诊断句柄也挂上（成功那条路在 connectWhenRunning 里挂）：
   * 失败时句柄里的 context / gain 都是 null，这就是「这次走的是退路」最直接的检查标准。
   */
  private note(reason: string): void {
    this.reason = reason
    this.element.dataset.speechGraphNote = reason
    console.warn('[speech] ' + reason)
    this.expose()
  }

  /**
   * 只在**调试钩子开着**时把句柄挂到 `window.__speechGraph`，供诊断接 AnalyserNode 量实际输出电平
   * （`gain.value` 会跟着错误一起绿，不能拿它当检查标准）。页面看不到主进程的 `LOOKUP_DEBUG_HOOKS`，
   * 所以借现成的调试通道问一句：钩子没开时它会抛，我们就不挂。
   */
  private expose(): void {
    const bridge = (window as unknown as { dshLookup?: DebugBridgeLike }).dshLookup
    const ask = bridge && bridge.debug && bridge.debug.window
    if (!ask) return
    const context = this.context
    const source = this.source
    const gain = this.gain
    void Promise.resolve()
      .then(() => ask.call(bridge.debug, this.role))
      .then(
        () => {
          ;(window as unknown as { __speechGraph?: SpeechGraphHandle }).__speechGraph = {
            role: this.role,
            element: this.element,
            context,
            source,
            gain,
            status: () => ({ mode: this.mode, note: this.reason }),
            apply: (value: number) => this.apply(value),
            tap: (analyser: AnalyserNode) => {
              if (!gain || !context) throw new Error('这次没有音频图（走的是 <audio>.volume 那条路）')
              // 串进输出路径量：量到的就是送往 destination 的那一份信号
              gain.disconnect()
              gain.connect(analyser)
              analyser.connect(context.destination)
              return () => {
                try {
                  gain.disconnect()
                  analyser.disconnect()
                } catch {
                  /* 已经拆过了就算了：还原比报错要紧 */
                }
                gain.connect(context.destination)
              }
            }
          }
        },
        () => {
          /* 钩子没开：不挂句柄 */
        }
      )
  }
}

/**
 * 这一次播放该在**客户端**施加多少 dB。只有内置录音那条是客户端施加的（`<audio>` 播的就是 .mdd 里的原字节）；
 * 豆包与系统语音的增益是**后端烘进字节里**的（回包 `gainDb` 因此是 0），这里再多施加一次就变成双倍。
 * 老回包没有 `gainDb` 时按 0 处理：宁可不调，也别拿 `undefined` 算出 NaN 增益。
 */
export function speechGainDb(result: { source?: string; gainDb?: number }): number {
  if (result.source !== 'dict') return 0
  const value = result.gainDb
  return typeof value === 'number' && Number.isFinite(value) ? value : 0
}

/** 把「这次没用音频图」如实缀在提示后面（空串就不缀）；供试听那几条提示用 */
export function withRouteNote(text: string, applied: SpeechRouteResult): string {
  return applied.note ? `${text}（${applied.note}）` : text
}
