// 豆包（火山引擎 seed-tts）诊断脚本：独立可跑、默认只发一次请求 —— 动在线那一层之前先跑它。
//
// 为什么先有它再写业务代码：这是全程序唯一会联网的付费接口。没有它，第一次真实请求会发生在
// "业务代码写完、点发音"那一刻 —— 失败时分不清是 Key 错、音色错、还是 SSE 解析写错。
// 有了它，这三件事被摊在三个独立步骤里逐个排除。规格见
// docs/design/豆包语音合成接入方案.md。
//
// 它是从参考实现那棵工作目录里**原样搬过来的独立 HTTP 诊断脚本**（代码一个字节都没改，
// 不依赖任何一版的实现）：0.2.0 以前没有这个前置件，而已发布的设计文档与当时的接续件
// 都指着"先跑 probe-doubao.mjs 再动在线那一层"。
//
// 本版应用的默认音色与参考实现相同：`en_female_dacey_uranus_bigtts`（Dacey）/
// `zh_female_vv_uranus_bigtts`（Vivi）—— 见 `native/src/engine/dsh_settings.h` 的
// `DSH_DOUBAO_DEFAULT_SPEAKER_EN/ZH`。⚠️ 但**诊断脚本自己的** `--mixed-test` 默认值仍是
// 它原来那两个（Dacey / 流畅女声），要按本版默认音色测就显式传 `--speaker`。
//
// 用法：
//   node tools/probe-doubao.mjs --api-key <key> [--speaker <音色ID>] [--text <文本>] [--out <文件>]
//   node tools/probe-doubao.mjs --api-key <key> --mixed-test     # 中英混排专门测试（发 5 次请求）
//   node tools/probe-doubao.mjs --api-key <key> --tone-matrix    # 语音指令（context_texts）实测矩阵
//   node tools/probe-doubao.mjs --list-voices                    # 打印候选音色表（离线，不发请求）
//
// 语音指令（官方字段表的 `context_texts`，见 docs/豆包语音_单向流式语音合成HTTP_*.pdf 第 8 页）：
//   --instruction <文案>           可重复，每次都进同一个数组（= context_texts 的元素）
//   --instruction-shape <形状>     array（默认）| string | toplevel | none —— 专门用来量"传错了会怎样"
//   --show-body                    打印请求体（确认字段落在 additions 里、而不是 req_params 顶层）
//   --resource-id <id>             覆盖按音色后缀推断的 Resource ID（做"1.0 音色 + 指令"那种实验）
//
// API Key 也可以放在环境变量 DSH_DOUBAO_API_KEY 里 —— **不要写进任何文件**。
import fs from 'node:fs'
import path from 'node:path'
import { spawn } from 'node:child_process'
import { fileURLToPath } from 'node:url'
import { randomUUID, createHash } from 'node:crypto'

const HERE = path.dirname(fileURLToPath(import.meta.url))
const ROOT = path.resolve(HERE, '..')

const ENDPOINT = 'https://openspeech.bytedance.com/api/v3/tts/unidirectional/sse'
const DEFAULT_SPEAKER_EN = 'en_female_dacey_uranus_bigtts'
const DEFAULT_SPEAKER_ZH = 'zh_female_liuchangnv_uranus_bigtts'
const MIXED_SAMPLE = 'apple 苹果，一种水果。An apple a day keeps the doctor away.'

/*
 * 语气指令（`context_texts`）的候选文案。
 *
 * 为什么是"试两个候选"而不是拍一个：这条指令是**提示词**，好不好用只能靠耳朵判
 * （见 docs/ …）。一个中文、一个英文 —— 英文候选是为了量"指令本身用英文写会不会不一样"，
 * 因为英文音色念单词时也可能受指令语言影响。
 */
const TONE_CANDIDATES = [
  ['A', '中文', '请用平常、自然的语气朗读，不要加入夸张的情感。'],
  ['B', '英文', 'Please read this in a plain, neutral tone, without dramatic emotion.']
]
/** 实测矩阵用的默认音色 = 应用里的内置默认（src/Audio/DoubaoSpeech.cs 的 DefaultSpeakerEn/Zh） */
const APP_SPEAKER_EN = 'en_female_dacey_uranus_bigtts'
const APP_SPEAKER_ZH = 'zh_female_vv_uranus_bigtts'

const MAX_BYTES = 12 * 1024 * 1024
/**
 * --dump：把 SSE 的**每一条原始行**打出来。
 *
 * 为什么需要它：流式响应里"没有 data 字段"可能有多种原因（服务端只发了事件号、
 * 发了纯文本的 OK、或者字段名跟文档不一致）。只看"0 字节 + 失败"分不清是哪种，
 * 而这是全程序唯一联网的付费接口，宁可多一个开关把原始流量摆出来。
 */
const DUMP = process.argv.includes('--dump')

function arg(name, fallback = null) {
  const index = process.argv.indexOf('--' + name)
  return index >= 0 && process.argv[index + 1] && !process.argv[index + 1].startsWith('--')
    ? process.argv[index + 1]
    : fallback
}

/**
 * 可重复参数：`--instruction a --instruction b` → ['a','b']。
 *
 * 为什么 `context_texts` 要允许给多条：官方把它定义成 **array**，数组里几条就是几条指令；
 * 实测时常常要"先给一条看看，再叠一条看看"，所以按出现顺序收集，不做逗号切分
 * （指令文案里本来就可能带顿号、逗号 —— 切开就成了另一句话）。
 */
function argAll(name) {
  const values = []
  for (let i = 0; i < process.argv.length; i++) {
    if (process.argv[i] !== '--' + name) continue
    const next = process.argv[i + 1]
    if (!next || next.startsWith('--')) continue
    if (next.trim()) values.push(next.trim())
  }
  return values
}

/*
 * 收尾**不要直接 process.exit()**。
 * 实测：在 fetch 用过的连接上直接 process.exit()，Node 会在 Windows 上崩在 libuv 的
 * async.c 断言里（`Assertion failed: !(handle->flags & UV_HANDLE_CLOSING)`），
 * 退出码变成 0xC0000409 —— 也就是"诊断脚本验通没验通还没读到，先看到诊断脚本自己崩了"。
 * 设 exitCode 让事件循环自己收干净即可（keep-alive 连接会让它多留一两秒，无所谓）。
 */
function finish(code) {
  process.exitCode = code
}

/* ---------------------------------------------------------------- 离线音色表 */

/** docs/design/豆包语音合成接入方案.md 的候选（第三方整理，下单前请到官方音色列表核对） */
const VOICES = {
  en: [
    ['en_female_dacey_uranus_bigtts', '美式英语女声（默认）'],
    ['en_female_stokie_uranus_bigtts', '美式英语女声'],
    ['en_male_tim_uranus_bigtts', '美式英语男声']
  ],
  zh: [
    ['zh_female_liuchangnv_uranus_bigtts', '流畅女声 2.0（默认，适合长文朗读）'],
    ['zh_female_xiaohe_uranus_bigtts', '小何 2.0，通用女声'],
    ['zh_female_yingyujiaoxue_uranus_bigtts', 'Tina老师 2.0，中文 / 英式英语'],
    ['zh_female_vv_uranus_bigtts', 'Vivi 2.0（中/日/印尼/西，不含英语）'],
    ['zh_male_m191_uranus_bigtts', '云舟 2.0，通用男声']
  ]
}

if (process.argv.includes('--list-voices')) {
  console.log('英文音色（词典主力）：')
  for (const [id, desc] of VOICES.en) console.log(`  ${id.padEnd(38)} ${desc}`)
  console.log('\n中文音色（汉英词典的词目 / 中文朗读，也负责中英混排）：')
  for (const [id, desc] of VOICES.zh) console.log(`  ${id.padEnd(38)} ${desc}`)
  console.log('\n全部配 seed-tts-2.0。以上摘自第三方整理，下单前请到官方音色列表核对一次：')
  console.log('  https://www.volcengine.com/docs/6561/1257544')
  finish(0)
}

/* ------------------------------------------------------------ 请求 / 解析 */

/** Resource ID 必须与音色配套，填错返回 40000001 */
function resourceIdFor(speaker) {
  if (/^S_/.test(speaker)) return 'seed-icl-1.0'
  if (/_mars_bigtts$/.test(speaker) || /_moon_bigtts$/.test(speaker)) return 'seed-tts-1.0'
  return 'seed-tts-2.0'
}

/** 错误码翻成人话，不把原始码丢给用户（业务代码照抄这张表） */
function explainCode(code, message = '') {
  const c = String(code)
  if (c === '0' || c === '20000000') return ''
  if (c === '40000001') return '参数错误：音色 ID 与模型版本不配套，或文本 / 参数有误'
  if (c === '40300001') return '鉴权失败：API Key 不对，或服务未开通 / 实名认证未完成'
  if (c === '40402003') return '文本超长：这段太长，需要分段'
  // 实测遇到过：API Key 本身有效（不是 403），但这个音色 / 模型没在这个账号下开通，
  // 服务端回 45000030 + "requested resource not granted"。这不是代码问题，只有去控制台能解决。
  if (c === '45000030' || /not granted/i.test(message)) {
    return '这个音色 / 服务没在当前账号下开通：去控制台把「语音合成大模型」开通、并开启要用的试用音色'
  }
  if (/^(4500|45000)/.test(c)) return '客户端错误：' + c + '（参数或调用方式有问题）' + (message ? '：' + message : '')
  if (/^55000/.test(c)) return '火山引擎服务端出错，稍后重试'
  if (/quota|Quota|QUOTA|exceed|Exceed/.test(c)) return '试用额度已用完，需在控制台开通正式版'
  return '未知错误码 ' + c + (message ? '：' + message : '')
}

function sniffMime(bytes) {
  if (bytes.length >= 3 && bytes[0] === 0x49 && bytes[1] === 0x44 && bytes[2] === 0x33) return 'audio/mpeg' // ID3
  if (bytes.length >= 2 && bytes[0] === 0xff && (bytes[1] & 0xe0) === 0xe0) return 'audio/mpeg'
  if (bytes.length >= 4 && bytes[0] === 0x52 && bytes[1] === 0x49 && bytes[2] === 0x46 && bytes[3] === 0x46) return 'audio/wav'
  if (bytes.length >= 4 && bytes[0] === 0x4f && bytes[1] === 0x67 && bytes[2] === 0x67 && bytes[3] === 0x53) return 'audio/ogg'
  return '(认不出，原始头 ' + [...bytes.slice(0, 4)].map((b) => b.toString(16).padStart(2, '0')).join(' ') + ')'
}

/**
 * 语音指令往哪儿放：**`additions` 里**（`additions` 本身是 `req_params` 下的一个
 * JSON 序列化**字符串**，官方字段表里 context_texts 与 disable_markdown_filter /
 * explicit_language / post_process 同级）。
 *
 * `shape` 是用来**量错**的，不是给人用的：
 *   array    —— 文档写法（数组）；
 *   string   —— 传成字符串而不是数组（看服务端报不报错、报什么码）；
 *   toplevel —— 放 req_params 顶层（看是不是"不报错地不生效"）；
 *   none     —— 明确不带（对照组 = 现状）。
 * 把这几种摆在一起量，才能回答"生效了没有"而不是"没报错所以大概生效了"。
 */
function applyInstruction(additions, reqParams, instructions, shape) {
  const list = (instructions || []).filter((text) => text && text.length > 0)
  if (list.length === 0 || shape === 'none') return
  switch (shape) {
    case 'array': additions.context_texts = list; return
    case 'string': additions.context_texts = list.join(' '); return
    case 'toplevel': reqParams.context_texts = list; return
    default: throw new Error('不认识的 --instruction-shape：' + shape + '（可用 array / string / toplevel / none）')
  }
}

/**
 * 发一次请求，返回 { bytes, status, ms, code, lastCode, raw, error, body, billedWords }。
 * 与将来 C# 侧 DoubaoSpeech.Fetch 保持同一套语义（两处实现、同一份字段说明）。
 */
async function fetchSpeech({
  apiKey, speaker, text, explicitLanguage = '', enableLanguageDetector = false,
  speechRate = 0, loudnessRate = 0, instructions = [], instructionShape = 'array', resourceId = null
}) {
  const additions = {
    post_process: { pitch: 0 },
    disable_markdown_filter: true
  }
  // 混排时**不传** explicit_language（官方"正常中英混读"）；传了就是"只念这个语种"的限制
  if (explicitLanguage) additions.explicit_language = explicitLanguage
  if (enableLanguageDetector) additions.enable_language_detector = true

  /*
   * sample_rate 放**audio_params 里** —— 官方《单向流式语音合成HTTP》第 5 页把
   * format / sample_rate / bit_rate / speech_rate / loudness_rate 都列在 audio_params 下。
   * 早先照方案文档放在 req_params 顶层也能出声，但那是服务端容忍，不是文档写法。
   */
  const reqParams = {
    text,
    speaker,
    audio_params: { format: 'mp3', sample_rate: 24000, speech_rate: speechRate, loudness_rate: loudnessRate, bit_rate: 64000 }
  }
  applyInstruction(additions, reqParams, instructions, instructionShape)
  reqParams.additions = JSON.stringify(additions)

  const body = { user: { uid: 'lookup' }, req_params: reqParams }
  const effectiveResourceId = resourceId || resourceIdFor(speaker)

  const started = Date.now()
  const controller = new AbortController()
  const timer = setTimeout(() => controller.abort(), 30000)
  let response
  try {
    response = await fetch(ENDPOINT, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'X-Api-Key': apiKey,
        'X-Api-Resource-Id': resourceIdFor(speaker),
        'X-Api-Request-Id': randomUUID(),
        // 官方文档：设为 * 会返回计费的字符数（响应里的 usage.text_words）
        'X-Control-Require-Usage-Tokens-Return': '*'
      },
      body: JSON.stringify(body),
      signal: controller.signal
    })
  } catch (err) {
    clearTimeout(timer)
    return { bytes: Buffer.alloc(0), status: 0, ms: Date.now() - started, code: '', lastCode: '', raw: '', error: '请求失败：' + err.message, body, billedWords: 0 }
  }

  const chunks = []
  let total = 0
  let code = ''
  /** 流里出现过的**最后一个** code（成功的 0 / 20000000 也记下来 —— 表里要看它） */
  let lastCode = ''
  let raw = ''
  let error = ''
  /** 见过"code 0 + 空句子 + 无音频"，要跟"字段名不对"区分开（见下面 error 的说明） */
  let emptyNote = false
  /** 收到过 code 0 / 20000000 的成功回包（哪怕一个字节音频都没有） */
  let sawOk = false
  /** 官方文档的 usage.text_words：本次请求计费的文本字数（含标点） */
  let billedWords = 0
  const decoder = new TextDecoder()
  let buffered = ''

  try {
    for await (const part of response.body) {
      buffered += decoder.decode(part, { stream: true })
      // SSE：一行一条；有的实现会带 "data:" 前缀
      let index
      while ((index = buffered.indexOf('\n')) >= 0) {
        const line = buffered.slice(0, index).trim()
        buffered = buffered.slice(index + 1)
        if (!line) continue
        const payload = line.startsWith('data:') ? line.slice(5).trim() : line
        if (DUMP) console.log('    [sse] ' + line.slice(0, 300))
        if (!payload.startsWith('{')) {
          if (raw.length < 400) raw += payload + ' '
          continue
        }
        let message
        try {
          message = JSON.parse(payload)
        } catch {
          if (raw.length < 400) raw += payload + ' '
          continue
        }
        if (message.code !== undefined && message.code !== null) lastCode = String(message.code)
        if (message.code !== undefined && message.code !== null && String(message.code) !== '0') {
          // code 为 0 / 20000000 才算成功
          if (String(message.code) !== '20000000') {
            code = String(message.code)
            error = explainCode(message.code, message.message) || (message.message || '')
            if (raw.length < 400) raw += JSON.stringify(message).slice(0, 400) + ' '
          }
        }
        if (typeof message.data === 'string' && message.data) {
          const bytes = Buffer.from(message.data, 'base64')
          chunks.push(bytes)
          total += bytes.length
          if (total > MAX_BYTES) {
            error = '响应超过上限 ' + MAX_BYTES + ' 字节，已中断'
            break
          }
        }
        // 服务端明确回了一句"空句子"（sentence.text 为空、没有音频）：这是"音色没念出东西"的特征
        if (!message.data && message.sentence && !message.sentence.text) emptyNote = true
        // 也见过不带 sentence 字段的版本（只有 code 0 + data:null），所以"收到过成功回包"也算数
        if (String(message.code) === '0' || String(message.code) === '20000000') sawOk = true
        // 官方文档：usage.text_words = 本次请求计费的文本字数（含标点）
        if (message.usage && Number(message.usage.text_words) > 0) billedWords = Number(message.usage.text_words)
        if (message.message && !code && String(message.code || '0') !== '0') raw += String(message.message).slice(0, 200)
      }
      if (error) break
    }
  } catch (err) {
    if (!error) error = '读流失败：' + err.message
  } finally {
    clearTimeout(timer)
  }

  if (!error && total === 0 && !code) {
    /*
     * 实测（2026-09）：**英文音色念中英混排的文本，服务端就是这样回的** ——
     *   data: {"code":0,"message":"","data":null,"sentence":{"phonemes":[],"text":"","words":[]}}
     *   data: {"code":20000000,"message":"OK","data":null}
     * 也就是 code=0（成功）、句子为空、一个字节音频都没有。它**不报错**。
     * 所以业务代码绝不能把"code 0"当成成功 —— 那会表现为"点朗读，什么都不响，也不提示"。
     */
    error = emptyNote || sawOk
      ? '服务端返回了空句子（code 0，但没有音频）：这个音色没念出这段文本 —— ' +
        '最常见也最坑的成因是用**英文音色**去念中英混排（实测 100% 空、且不报错）。' +
        '混排请用支持混读的中文音色；业务代码里"code 0"**不能**当成成功。'
      : '没拿到任何音频数据（响应里没有 data 字段）'
  }
  return { bytes: Buffer.concat(chunks), status: response.status, ms: Date.now() - started, code, lastCode, raw: raw.trim(), error, billedWords, body }
}

/* ---------------------------------------------------------------- 输出 */

const apiKey = arg('api-key', process.env.DSH_DOUBAO_API_KEY || '')
if (!apiKey) {
  console.error('缺少 API Key：用 --api-key <key>，或设环境变量 DSH_DOUBAO_API_KEY。')
  console.error('（先 --list-voices 看候选音色表也行，那一步不发请求。）')
  finish(2)
}
const redacted = apiKey.slice(0, 8) + '…' + apiKey.slice(-4)

async function runOne({
  label, speaker, text, explicitLanguage, enableLanguageDetector = false, loudnessRate = 0, out,
  instructions = [], instructionShape = 'array', resourceId = null
}) {
  const effectiveResourceId = resourceId || resourceIdFor(speaker)
  console.log(`\n=== ${label} ===`)
  console.log(`  文本        ${JSON.stringify(text)}`)
  console.log(`  音色        ${speaker}`)
  console.log(`  ResourceId  ${effectiveResourceId}${resourceId ? '（--resource-id 覆盖）' : '（按音色后缀推断，见 ）'}`)
  console.log(`  explicit_language  ${explicitLanguage ? JSON.stringify(explicitLanguage) : '(不传 = 官方"正常中英混读")'}`)
  if (loudnessRate) console.log(`  loudness_rate  ${loudnessRate}（响度补偿，见 provider 里的 LoudnessRate 表）`)
  if (enableLanguageDetector) console.log('  enable_language_detector  true')
  console.log(`  语音指令    ${instructions.length === 0 || instructionShape === 'none'
    ? '(不带 context_texts)'
    : `shape=${instructionShape} ${JSON.stringify(instructions)}`}`)
  const result = await fetchSpeech({
    apiKey, speaker, text, explicitLanguage, enableLanguageDetector, loudnessRate,
    instructions, instructionShape, resourceId: effectiveResourceId
  })
  if (process.argv.includes('--show-body')) {
    console.log('  请求体      ' + JSON.stringify(result.body))
    console.log('  additions   ' + result.body.req_params.additions)
  }
  console.log(`  HTTP ${result.status}   耗时 ${result.ms}ms   音频 ${result.bytes.length} 字节` +
    (result.lastCode ? `   code ${result.lastCode}` : '') +
    (result.billedWords ? `   计费 ${result.billedWords} 字` : ''))
  if (result.error) {
    console.log(`  ✗ 失败：${result.error}`)
    if (result.code) console.log(`    豆包错误码 ${result.code} → ${explainCode(result.code)}`)
    if (result.raw) console.log(`    响应片段：${result.raw.slice(0, 300)}`)
    return { ok: false, ...result }
  }
  const mime = sniffMime(result.bytes)
  console.log(`  嗅探 MIME   ${mime}`)
  fs.mkdirSync(path.dirname(out), { recursive: true })
  fs.writeFileSync(out, result.bytes)
  console.log(`  已保存      ${path.relative(ROOT, out)}  ← 打开听一下`)
  return { ok: true, mime, ...result }
}

/*
 * 分支必须是**互斥**的 if/else。
 * 教训：`finish()` 只设 exitCode、不终止执行（这样才不会在 fetch 的连接上崩，见它的注释），
 * 所以早先写成"平行 if"时，--mixed-test 跑完还会接着跑一次单次连通性 —— 多花一次计费请求，
 * 输出里也会凭空多出一段单次测试。
 */
const MODE = process.argv.includes('--list-voices')
  ? 'voices'
  : process.argv.includes('--mixed-test')
    ? 'mixed'
    : process.argv.includes('--tone-matrix')
      ? 'tone'
      : process.argv.includes('--repeat-check')
        ? 'repeat'
        : process.argv.includes('--measure')
          ? 'measure'
          : 'single'

/* ==========================================================================
   语气指令（context_texts）实测矩阵
   ========================================================================== */

/**
 * 量 mp3 的真实时长（拿无头 Edge 的 decodeAudioData 解出来，不是拿字节数估）。
 *
 * 这一段的出处是 0.1.x 参考实现里的同名测量脚本 `probe-voice-loudness.mjs`（那里还顺带算电平）
 * —— ⚠️ 那个脚本**不在本仓库**，这里只是沿用它的测量约定：
 * 本仓库不为测量引入任何 mp3 解码依赖，浏览器自带解码器，而程序里播放用的也是它。
 * 为什么走 CDP 而不是 `--dump-dom`：解码是异步的，`--dump-dom` 在 load 之后就 dump，拿到的还是 pending。
 *
 * 量时长在这里是**检查标准的一部分**：语气指令万一让服务端只念半句、或者干脆没生效，
 * "字节数差不多"看不出来，时长对得上才说明念的是同一段文本。
 */
async function measureDurations(files) {
  if (files.length === 0) return new Map()
  const EDGE = 'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe'
  const clips = files.map((file) => ({ name: path.basename(file), b64: fs.readFileSync(file).toString('base64') }))
  const port = 9560 + Math.floor(Math.random() * 200)
  const profile = path.join(ROOT, 'logs', 'doubao-tone', 'edge-profile')
  const page = path.join(ROOT, 'logs', 'doubao-tone', '_measure.html')
  fs.mkdirSync(path.dirname(page), { recursive: true })
  fs.writeFileSync(page, '<!doctype html><meta charset="utf-8"><title>tone</title>', 'utf8')

  const edge = spawn(EDGE, [
    '--headless=new', '--disable-gpu', '--no-first-run', '--no-default-browser-check',
    '--remote-debugging-port=' + port,
    '--user-data-dir=' + profile,
    'file:///' + page.replace(/\\/g, '/')
  ], { stdio: 'ignore', windowsHide: true })

  try {
    const target = await waitForTarget(port)
    const socket = new WebSocket(target.webSocketDebuggerUrl)
    await new Promise((resolve, reject) => {
      socket.addEventListener('open', resolve, { once: true })
      socket.addEventListener('error', reject, { once: true })
    })
    const expression = `(async () => {
      const clips = ${JSON.stringify(clips)}
      const toBuffer = (b64) => {
        const bin = atob(b64)
        const out = new Uint8Array(bin.length)
        for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i)
        return out.buffer
      }
      const ctx = new AudioContext()
      const rows = []
      for (const clip of clips) {
        try {
          const audio = await ctx.decodeAudioData(toBuffer(clip.b64))
          const data = audio.getChannelData(0)
          let sum = 0, peak = 0
          for (let i = 0; i < data.length; i++) { sum += data[i] * data[i]; const a = Math.abs(data[i]); if (a > peak) peak = a }
          /*
           * 有效语音电平（20ms 分帧、剔除比整段 RMS 低 25 dB 的静音帧）—— 与参考实现的
           * 参考实现那个 probe-voice-loudness.mjs 的同一约定：整段 RMS 会把停顿算进去，
           * 而"语气"这类差别往往先体现在停顿长度与能量包络上。
           */
          const frame = Math.round(audio.sampleRate * 0.02)
          const frameDb = []
          for (let start = 0; start + frame <= data.length; start += frame) {
            let acc = 0
            for (let i = 0; i < frame; i++) { const v = data[start + i]; acc += v * v }
            const rms = Math.sqrt(acc / frame)
            if (rms > 0) frameDb.push(20 * Math.log10(rms))
          }
          const wholeDb = 20 * Math.log10(Math.sqrt(sum / data.length) || 1e-9)
          const gate = wholeDb - 25
          const active = frameDb.filter((d) => d > gate)
          const activeRms = active.length ? Math.sqrt(active.reduce((acc, d) => acc + Math.pow(10, d / 10), 0) / active.length) : 0
          rows.push({
            name: clip.name, ok: true, seconds: audio.duration,
            rmsDb: wholeDb, activeDb: activeRms > 0 ? 20 * Math.log10(activeRms) : -999,
            peakDb: peak > 0 ? 20 * Math.log10(peak) : -999,
            voicedFrames: active.length, totalFrames: frameDb.length
          })
        } catch (err) {
          rows.push({ name: clip.name, ok: false, error: String(err) })
        }
      }
      return JSON.stringify(rows)
    })()`
    const result = await new Promise((resolve, reject) => {
      socket.addEventListener('message', (event) => {
        const message = JSON.parse(event.data)
        if (message.id !== 1) return
        if (message.error) reject(new Error(message.error.message))
        else if (message.result.exceptionDetails) reject(new Error('页面里抛异常：' + (message.result.exceptionDetails.exception?.description || '')))
        else resolve(message.result.result.value)
      })
      socket.send(JSON.stringify({ id: 1, method: 'Runtime.evaluate', params: { expression, awaitPromise: true, returnByValue: true } }))
      setTimeout(() => reject(new Error('测量超时（30s）')), 30000)
    })
    socket.close()
    return new Map(JSON.parse(result).map((row) => [row.name, row]))
  } finally {
    try { edge.kill() } catch { /* 已经退了 */ }
  }
}

/** 等 DevTools 端点可用（Edge 起来要几百毫秒）—— 与参考实现的 `probe-voice-loudness.mjs` 同一套 */
async function waitForTarget(port) {
  const deadline = Date.now() + 15000
  while (Date.now() < deadline) {
    try {
      const response = await fetch(`http://127.0.0.1:${port}/json/list`)
      const targets = await response.json()
      const page = targets.find((t) => t.type === 'page' && t.webSocketDebuggerUrl)
      if (page) return page
    } catch { /* 还没起来 */ }
    await new Promise((resolve) => setTimeout(resolve, 200))
  }
  throw new Error('Edge 的调试端口没起来')
}

/* ==========================================================================
   重复性对照（repeat-check）：同一个请求连发 N 次，看字节数/时长/哈希散不散
   ========================================================================== */

if (MODE === 'repeat') {
  /*
   * 为什么必须有这个对照：矩阵靠"带指令 vs 不带指令的字节数/时长差"来判断指令生没生效，
   * 而这个判断**默认了"同样的请求 → 同样的音频"**。实测里这个前提很可疑 ——
   * 同一段 `苹果`（Vivi、无指令、loudness_rate 不同两次运行）拿到过 10029 与 11757 字节。
   * 不先把"抖动量"量出来，就可能把随机抖动当成"指令生效了"（或反过来）。
   */
  const speaker = arg('speaker', APP_SPEAKER_ZH)
  const text = arg('text', '苹果')
  const language = arg('language', 'zh-cn')
  const loudness = Number(arg('loudness', '-50'))
  const instructions = argAll('instruction')
  /*
   * ⚠️ shape 必须从参数读，**不能写死 array**：这个分支的用处正是"量传错了会怎样"，
   * 写死之后 --instruction-shape toplevel/string 会被不报错地忽略，量出来的还是正确形状的结果
   * （本轮就踩过一次：两次"错形状"实验其实都按正确形状发了，直到打印行里看见 shape=array）。
   */
  const instructionShape = arg('instruction-shape', instructions.length > 0 ? 'array' : 'none')
  const repeat = Math.max(2, Number(arg('repeat', '3')) || 3)
  const outDir = path.resolve(arg('out-dir', path.join(ROOT, 'logs', 'doubao-tone', '_repeat')))
  const label = instructions.length > 0 ? `带指令(shape=${instructionShape})` : '不带指令'
  console.log(`重复性对照：${repeat} 次同样的请求（${label}）—— 音色 ${speaker}，文本 ${JSON.stringify(text)}`)
  console.log(`  Key ${redacted}`)

  const rows = []
  for (let i = 1; i <= repeat; i++) {
    const out = path.join(outDir, `r${i}.mp3`)
    const result = await runOne({
      label: `第 ${i} 次｜${label}`, speaker, text, explicitLanguage: language,
      loudnessRate: loudness, instructions, instructionShape, out
    })
    const hash = result.ok ? createHash('sha256').update(result.bytes).digest('hex').slice(0, 16) : ''
    rows.push({ i, code: result.lastCode || ('✗' + (result.code || '')), bytes: result.bytes.length, hash, error: result.error, file: result.ok ? out : null })
  }

  let durations = new Map()
  try { durations = await measureDurations(rows.filter((r) => r.file).map((r) => r.file)) } catch (err) { console.log('（时长没量成：' + err.message + '）') }

  console.log('\n=== 重复性对照表 ===')
  console.log('  ' + '次'.padEnd(4) + 'code'.padEnd(11) + '字节'.padStart(8) + '时长'.padStart(9) + '语音电平'.padStart(10) + '   sha256[0:16]')
  for (const row of rows) {
    const measured = row.file ? durations.get(path.basename(row.file)) : null
    console.log('  ' + String(row.i).padEnd(4) + row.code.padEnd(11) + String(row.bytes).padStart(8) +
      (measured && measured.ok ? measured.seconds.toFixed(2) + 's' : (row.file ? '量不出' : '—')).padStart(9) +
      (measured && measured.ok ? measured.activeDb.toFixed(1) + ' dB' : '').padStart(10) +
      '   ' + row.hash + (row.error ? '   ' + row.error.slice(0, 36) : ''))
  }
  const uniqueBytes = new Set(rows.map((r) => r.bytes))
  const uniqueHash = new Set(rows.filter((r) => r.hash).map((r) => r.hash))
  const seconds = rows.map((r) => (r.file && durations.get(path.basename(r.file)) || {}).seconds).filter((s) => typeof s === 'number')
  const spread = seconds.length > 1 ? (Math.max(...seconds) - Math.min(...seconds)) : 0
  console.log(`\n字节数出现 ${uniqueBytes.size} 种取值：${[...uniqueBytes].join(' / ')}`)
  console.log(`时长跨度 ${spread.toFixed(2)}s（${seconds.map((s) => s.toFixed(2)).join(' / ')}）`)
  console.log(`sha256 出现 ${uniqueHash.size} 种取值` +
    (uniqueHash.size === 1 ? '（**完全一样**：同样的请求 = 同样的音频，字节/哈希可以直接当检查标准）'
      : '（**不一样**：合成有随机性，字节数与哈希的差异不能单独证明"指令生效了"；' +
        '只有当某一组的变化**大于**这里的抖动幅度时才算证据）'))
  console.log(`样本在 ${path.relative(ROOT, outDir)}\\r*.mp3`)
  finish(rows.every((r) => r.bytes > 0) ? 0 : 1)
} else if (MODE === 'measure') {
  /*
   * 为什么有它：合成要花钱、还要等，而"听之前先看一眼时长"是常事
   * （例如人耳对比几份样本、确认哪几份长度差得远）。这里只读文件、不发请求。
   * 用法：node tools/probe-doubao.mjs --measure logs/doubao-tone
   */
  const dir = path.resolve(arg('measure', path.join(ROOT, 'logs', 'doubao-tone')))
  const files = fs.readdirSync(dir).filter((name) => name.toLowerCase().endsWith('.mp3')).sort()
    .map((name) => path.join(dir, name))
  if (files.length === 0) {
    console.error('这个目录里没有 mp3：' + dir)
    finish(2)
  } else {
    const measured = await measureDurations(files)
    console.log(`=== 量 ${path.relative(ROOT, dir)} 里的 ${files.length} 个样本（不发请求）===`)
    console.log('  ' + '文件'.padEnd(40) + '字节'.padStart(8) + '时长'.padStart(9) + '语音电平'.padStart(10))
    const groups = new Map()
    for (const file of files) {
      const row = measured.get(path.basename(file)) || { ok: false }
      console.log('  ' + path.basename(file).padEnd(40) + String(fs.statSync(file).size).padStart(8) +
        (row.ok ? row.seconds.toFixed(2) + 's' : '解码失败').padStart(9) +
        (row.ok ? row.activeDb.toFixed(1) + ' dB' : '').padStart(10))
      if (!row.ok) continue
      // 按"去掉了份号的名字"分组，给出区间 —— 判断"这一组和那一组分不分得开"就看它
      const key = path.basename(file).replace(/-\d+\.mp3$/, '')
      if (!groups.has(key)) groups.set(key, [])
      groups.get(key).push(row.seconds)
    }
    console.log('\n=== 时长区间（同组各份 min~max）===')
    for (const [key, list] of groups) {
      console.log('  ' + key.padEnd(40) + list.map((s) => s.toFixed(2)).join(' / ') +
        '   (' + Math.min(...list).toFixed(2) + '~' + Math.max(...list).toFixed(2) + 's)')
    }
    console.log('\n⚠️ 合成结果**本身有随机性**（同一请求连发几次都不一样），所以区间重叠 = 这个量分不开两组，')
    console.log('   别拿它当"语气变了没有"的证据 —— 那件事只能靠耳朵（本目录的样本就是给人听的）。')
  }
} else if (MODE === 'tone') {
  /*
   * 本诊断脚本最重要的用途。
   * 词典正文是中英混排的，而 explicit_language 是"只念这个语种"的**限制**而不是提示 ——
   * 不先跑这一遍，就会交付一个"点朗读只出声念了英文"的 bug（最难查的那种）。
   */
  /*
   * 为什么要做这张矩阵（而不是"发一次看它没报错就算生效"）：
   * `context_texts` 是**提示词**，服务端对它的处理没有可断言的返回值 ——
   * 传了不生效、或者传错位置被忽略，从 HTTP 200 / code 0 上完全看不出来。
   * 所以只能逐组拿**字节数 + 解码出来的真实时长 + 计费字数**去比，
   * 并且**存成文件让人耳判**（"平淡不平淡"是主观的，代码判不了）。
   */
  const speakerEn = arg('speaker-en', APP_SPEAKER_EN)
  const speakerZh = arg('speaker-zh', APP_SPEAKER_ZH)
  /* 与应用一致：内置响度表里这两个默认音色都是 -50（见 DoubaoSpeech.LoudnessRate） */
  const loudness = Number(arg('loudness', '-50'))
  /*
   * 每个变体合成几份。
   *
   * 为什么默认 2 而不是 1：**同一请求的合成结果本身是随机的**（实测同参数连发 3 次，
   * 时长 1.06 / 1.22 / 1.46s）。只留一份的话，用户可能正好碰上"特别夸张"或"特别平"的那次，
   * 于是把随机当成了指令的效果。多份并存，让人耳自己判断"差多少"。
   */
  const samples = Math.max(1, Number(arg('samples', '2')) || 2)
  const outDir = path.resolve(arg('out-dir', path.join(ROOT, 'logs', 'doubao-tone')))
  const fullSentenceZh = '他昨天特别特别痛心地说，这件事他永远不会忘记。'
  const fullSentenceEn = 'He said, with a heavy heart, that he would never forget it.'

  /* [组名, 音色, 文本, explicit_language, 该组要试的指令候选, 这个组合成几份] */
  const cases = [
    ['zh-单词-苹果', speakerZh, '苹果', 'zh-cn', ['A', 'B'], samples],
    ['en-单词-apple', speakerEn, 'apple', 'en', ['A', 'B'], samples],
    ['en-词组-get-up', speakerEn, 'get up', 'en', ['A', 'B'], samples],
    ['zh-整句', speakerZh, fullSentenceZh, 'zh-cn', ['A'], samples],
    // 英文整句只留一份：它是"句子那边别被改坏"的旁证，主证据是中文整句那一对
    ['en-整句', speakerEn, fullSentenceEn, 'en', ['A'], 1]
  ]
  const requests = cases.reduce(
    (sum, [, , , , candidates, count]) => sum + count * (1 + candidates.length), 0)
  console.log(`语气指令矩阵：${cases.length} 组、共 ${requests} 次请求（样本都很短）。Key ${redacted}`)
  console.log(`指令候选：${TONE_CANDIDATES.map(([id, lang, text]) => `${id}(${lang})=${JSON.stringify(text)}`).join('  ')}`)
  console.log(`每个变体合成 ${samples} 份（合成结果有随机性，单份会误导耳朵；文件名末尾是份号）`)

  const rows = []
  const savedFiles = []
  for (const [name, speaker, text, language, candidates, count] of cases) {
    const variants = [['无指令', null],
      ...candidates.map((id) => [id === 'A' ? '有指令' : '有指令英文候选', TONE_CANDIDATES.find(([cid]) => cid === id)[2]])]
    for (const [tag, instruction] of variants) {
      for (let i = 1; i <= count; i++) {
        const out = path.join(outDir, `${name}-${tag}-${i}.mp3`)
        const result = await runOne({
          label: `${name}｜${tag} #${i}`,
          speaker, text, explicitLanguage: language,
          loudnessRate: loudness,
          instructions: instruction ? [instruction] : [],
          instructionShape: 'array',
          out
        })
        rows.push({
          name, tag, sample: i, speaker,
          code: result.ok ? (result.lastCode || '(未报)') : '✗' + (result.code || ''),
          bytes: result.bytes.length,
          ms: result.ms,
          billedWords: result.billedWords,
          error: result.error,
          file: result.ok ? out : null
        })
        if (result.ok) savedFiles.push(out)
      }
    }
  }

  let durations = new Map()
  try {
    durations = await measureDurations(savedFiles)
  } catch (err) {
    console.log('\n（时长没量成：' + err.message + ' —— 表格里留空，其余结论不受影响）')
  }

  console.log('\n=== 语气指令矩阵（"无指令 / 有指令"按组对比；时长是解码出来的真实时长）===')
  console.log('  ' + '组'.padEnd(18) + '变体'.padEnd(18) + '份'.padEnd(4) + 'code'.padEnd(11) +
    '字节'.padStart(8) + '时长'.padStart(9) + '计费字'.padStart(8))
  for (const row of rows) {
    const measured = row.file ? durations.get(path.basename(row.file)) : null
    const seconds = measured && measured.ok ? measured.seconds.toFixed(2) + 's' : (row.file ? '量不出' : '—')
    console.log('  ' + row.name.padEnd(18) + row.tag.padEnd(18) + String(row.sample).padEnd(4) + row.code.padEnd(11) +
      String(row.bytes).padStart(8) + seconds.padStart(9) + String(row.billedWords || 0).padStart(8) +
      (row.error ? '   ' + row.error.slice(0, 40) : ''))
  }
  /*
   * 每组、每个变体各报一次时长区间 —— 这是给"人耳判断"当参考的：
   * 区间**重叠**就说明这个量分不开两组，别拿它当效果证据（见 --repeat-check 的说明）。
   */
  console.log('\n=== 时长区间（同组同变体各份的 min~max）===')
  const byVariant = new Map()
  for (const row of rows) {
    const key = row.name + '｜' + row.tag
    const seconds = row.file && (durations.get(path.basename(row.file)) || {}).seconds
    if (typeof seconds !== 'number') continue
    if (!byVariant.has(key)) byVariant.set(key, [])
    byVariant.get(key).push(seconds)
  }
  for (const [key, list] of byVariant) {
    console.log('  ' + key.padEnd(38) + list.map((s) => s.toFixed(2)).join(' / ') +
      '   (' + Math.min(...list).toFixed(2) + '~' + Math.max(...list).toFixed(2) + 's)')
  }

  const failed = rows.filter((row) => row.bytes === 0)
  console.log(`
音频都在 ${path.relative(ROOT, outDir)}\\（名字 = 组-变体-份号）。
请**自己听**这几对，判断语气有没有变平淡 —— 这件事代码判不了，而且**合成结果本身有随机性**
（同一请求连发几次都不一样），所以每个条件下都放了多份，别只看一份：
  · zh-单词-苹果-无指令-*.mp3      vs zh-单词-苹果-有指令-*.mp3
  · en-单词-apple-无指令-*.mp3     vs en-单词-apple-有指令-*.mp3
  · en-词组-get-up-无指令-*.mp3    vs en-词组-get-up-有指令-*.mp3
  · zh-整句-无指令-*.mp3           vs zh-整句-有指令-*.mp3   ← 这一对是查"整句有没有被改坏"
  · 另有 -有指令英文候选-*.mp3：同一条意思但用英文写，用来对比"指令语言会不会有影响"`)

  finish(failed.length === 0 ? 0 : 1)
} else if (MODE === 'mixed') {
  /*
   * 本诊断脚本最重要的用途。
   * 词典正文是中英混排的，而 explicit_language 是"只念这个语种"的**限制**而不是提示 ——
   * 不先跑这一遍，就会交付一个"点朗读只出声念了英文"的 bug（最难查的那种）。
   */
  const speakerEn = arg('speaker-en', DEFAULT_SPEAKER_EN)
  const speakerZh = arg('speaker-zh', DEFAULT_SPEAKER_ZH)
  console.log(`这会发 5 次请求（按字符计费，样本很短）。Key ${redacted}，文本固定为：`)
  console.log(`  ${MIXED_SAMPLE}`)

  const cases = [
    ['A', '英文音色 + 不传', speakerEn, '', false],
    ['B', '英文音色 + en（对照组：中文会被跳过）', speakerEn, 'en', false],
    ['C', '中文音色 + 不传（的首选方案）', speakerZh, '', false],
    ['D', '中文音色 + zh-cn', speakerZh, 'zh-cn', false],
    ['E', '中文音色 + 不传 + 自动语种检测', speakerZh, '', true]
  ]
  const rows = []
  for (const [id, label, speaker, language, detector] of cases) {
    const out = path.join(ROOT, 'logs', `doubao-mixed-${id}.mp3`)
    const result = await runOne({ label: `${id}｜${label}`, speaker, text: MIXED_SAMPLE, explicitLanguage: language, enableLanguageDetector: detector, out })
    rows.push([id, label, result.ok ? String(result.bytes.length) : '失败', result.ok ? result.ms + 'ms' : result.error.slice(0, 48)])
  }
  console.log('\n=== 结论表 ===')
  for (const row of rows) console.log('  ' + row.map((c, i) => (i === 0 ? c.padEnd(2) : String(c).padEnd(46))).join(' '))
  console.log(`
请试听 A~E 五个文件（logs/doubao-mixed-*.mp3），重点确认：
  ① 中文部分有没有被念出来（A/B 尤其要听）
  ② 英文单词发音是否自然（C/D 用的中文音色念英文好不好听）
听完再把 的音色选择填回去。`)
  finish(rows.every((r) => r[2] !== '失败') ? 0 : 1)
} else if (MODE === 'single') {
  const speaker = arg('speaker', DEFAULT_SPEAKER_EN)
  const text = arg('text', 'apple')
  const explicitLanguage = arg('language', '')
  const instructions = argAll('instruction')
  const instructionShape = arg('instruction-shape', instructions.length > 0 ? 'array' : 'none')
  const resourceId = arg('resource-id', null)
  const out = path.resolve(arg('out', path.join(ROOT, 'logs', 'doubao-probe.mp3')))
  console.log(`API Key ${redacted}（只回显头尾，不写进任何文件）`)

  const single = await runOne({
    label: '单次连通性',
    speaker,
    text,
    explicitLanguage,
    loudnessRate: Number(arg('loudness', '0')) || 0,
    instructions,
    instructionShape,
    resourceId,
    out
  })
  if (!single.ok) {
    console.log(`
下一步：
  · 403 / 鉴权失败 → 检查 API Key 是否正确，以及**服务是否已开通、实名认证是否完成**
  · 45000030 / requested resource not granted → 账号下没开通这个音色 / 服务：
    控制台「语音合成大模型」要开通，并在音色管理里开启要用的**试用音色**（未开启就用不了）
  · quota / 用量用完 → 去控制台开通正式版
  · 40000001 → 音色 ID 与 Resource ID 不配套（本项目只用 2.0 音色 seed-tts-2.0）`)
    finish(1)
  } else {
    console.log('\n通。下一步建议跑 --mixed-test（5 次请求，样本很短），把"混排到底能不能全念"验掉。')
  }
}
