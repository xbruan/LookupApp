#!/usr/bin/env node
/**
 * 豆包**机器翻译大模型**的连通性诊断脚本（硬前置）：先把「Key 错 / 权限没开 / 语种代码错 / 解析写错」分开说成人话，
 * 免得第一次真实请求发生在业务代码点翻译那一刻时分不清是哪一种（没开通 `volc.speech.mt` 的症状与「Key 填错」几乎一样）。
 * ⚠️ 会**真的联网、真的按 token 计费**，默认只翻一句；打印 source/target/resource-id、状态码、耗时、译文与 `usage.total_tokens`。
 *
 * 用法（在 `0.2.0/` 下跑）：
 *   node tools/probe-mt.mjs --api-key <key> --text 苹果 --target en
 *   node tools/probe-mt.mjs --api-key <key> --text apple --target zh   # 不传 --source = 自动检测
 *   node tools/probe-mt.mjs --text 苹果 --target en                    # 不给 Key：从 settings.json 读
 *   LOOKUP_USER_DATA=<临时配置目录> node tools/probe-mt.mjs --text 苹果 --target en   # 从那个目录的 settings.json 读
 *   node tools/probe-mt.mjs --text 苹果 --target en --json             # 把原始回包也打出来
 *
 * 接口规格（照官方文档，别照网上老教程）：POST https://openspeech.bytedance.com/api/v3/machine_translation/matx_translate
 *   `X-Api-Key` 与语音同一把；**`X-Api-Resource-Id` 是 `volc.speech.mt`**（TTS 是 `seed-tts-2.0`，**不是同一个值**），
 *   另带 `X-Api-Request-Id` 与 `Content-Type: application/json`；体 `{ source_language?, target_language, text_list }`，
 *   回 `{ code, data: { translation_list: [{ translation, detected_source_language, usage }] } }`。
 */
import { randomUUID } from 'node:crypto'
import { existsSync, readFileSync } from 'node:fs'
import path from 'node:path'

function arg(name, fallback = null) {
  const index = process.argv.indexOf('--' + name)
  return index >= 0 && process.argv[index + 1] ? process.argv[index + 1] : fallback
}

const ENDPOINT = arg('endpoint', 'https://openspeech.bytedance.com/api/v3/machine_translation/matx_translate')
/** ⚠️ 与语音的 `seed-tts-2.0` **不是**同一个值；它是个固定值，界面上不给用户改 */
const RESOURCE_ID = 'volc.speech.mt'

/** ⚠️ MT 的错误码表与 TTS **不是同一张**，别复用 */
const ERROR_CODES = {
  20000000: { ok: true, text: '成功' },
  45000001: { ok: false, text: '请求参数错误（例如目标语种没指定）—— 通常是代码 bug，不是用户的问题' },
  45000130: { ok: false, text: '请求载荷过大 —— 正常不该出现：应该在客户端按 ≤16 条、每条 ≤1024 tokens 先切分' },
  55000001: { ok: false, text: '翻译服务内部错误，稍后重试' }
}

/**
 * ⚠️ 鉴权/权限类要**先按报文定性**，不能只查错误码表：`code: 55000000` 其实是服务端「整个鉴权/权限家族的外壳」，
 * 而码表里写的是「服务内部错误，稍后重试」—— 实测（HTTP 500）撞到过「没开通 `volc.speech.mt`」与「Key 是错的」两种，
 * 给用户的下一步动作完全不同（一个去控制台开通、一个去改 Key），所以必须分开说，分不出来才退回码表。
 */
function explain(status, payload) {
  const code = payload && typeof payload.code === 'number' ? payload.code : null
  const message = (payload && payload.message) || ''

  if (/not\s*granted|not\s*subscribed|not\s*activated|no\s*permission|permission\s*denied|未开通/i.test(message)) {
    return (
      '鉴权/权限失败 —— **账号没有开通「机器翻译大模型」（volc.speech.mt）**。' +
      '语音合成与机器翻译是两个独立权限，同一把 Key 也要分别开通；' +
      '没开通的症状与"Key 填错"几乎一样，很容易误判。控制台：https://console.volcengine.com/speech/new/setting/apikeys' +
      (message ? `\n         服务端原话：${message}` : '')
    )
  }
  if (/x-api-key|api[-_ ]?key|authentication|unauthorized|invalid\s*credential|鉴权|密钥/i.test(message) || status === 401) {
    return (
      '**Key 不对**（服务端拒了这把 Key）。' +
      '它和语音合成共用同一把（settings.json 的 `volcengine.apiKey`），' +
      '所以先在「选项 → 语音」页核对那把 Key，别去查网络或额度。' +
      (message ? `\n         服务端原话：${message}` : '')
    )
  }
  if (status === 403) return '这个 Key 没有调用翻译的权限（403）—— 与"没开通 volc.speech.mt"是同一类事'
  if (code !== null && ERROR_CODES[code]) return ERROR_CODES[code].text
  if (status === 404) return '端点不存在（URL 写错了？）'
  if (status >= 500) return '服务端错误，稍后重试'
  return message || '（没有可解释的信息，见原始回包）'
}

/**
 * 读本机已填的那把 Key —— 与语音共用（`volcengine.apiKey`，暂时兼容读 `speech.doubaoApiKey`）。
 * 配置目录两处：`LOOKUP_USER_DATA`（验收用的临时配置口子）优先，否则 `%APPDATA%\LookupApp`（用户真正的那个）。
 */
function keyFromSettings() {
  const override = process.env.LOOKUP_USER_DATA || ''
  const dir = override || path.join(process.env.APPDATA || '', '查词')
  const file = path.join(dir, 'settings.json')
  const from = override ? 'LOOKUP_USER_DATA=' + override : '%APPDATA%\\查词'
  if (!existsSync(file)) return { key: '', from: `(没有 settings.json：${file}，来自 ${from})` }
  try {
    const json = JSON.parse(readFileSync(file, 'utf8'))
    const fresh = json && json.volcengine && json.volcengine.apiKey
    if (fresh) return { key: String(fresh), from: `${from} 的 settings.json → volcengine.apiKey` }
    const legacy = json && json.speech && json.speech.doubaoApiKey
    if (legacy) {
      return { key: String(legacy), from: `${from} 的 settings.json → speech.doubaoApiKey（旧路径，兼容读）` }
    }
    return { key: '', from: `${from} 的 settings.json 里两个字段都是空的` }
  } catch (err) {
    return { key: '', from: `${from} 的 settings.json 读不动：${err.message}` }
  }
}

/*
 * 整个请求流程包在 main() 里用**返回值**当退出码，而不是到处 `process.exit()`：
 * `fetch` 之后立刻硬退，Node 在 Windows 上会抛 libuv 的 async handle 断言 —— 它会把**真正的报错**淹掉
 * （实测就盖在「权限没开通」那句人话上面）。交给事件循环自己排空就没这个问题。
 */
async function main() {
  const target = arg('target', 'en')
  const source = arg('source', null)
  const texts = process.argv.includes('--text') ? [arg('text')] : arg('texts', '苹果').split('|')
  let apiKey = arg('api-key', '')
  let keyFrom = '--api-key 参数'
  if (!apiKey) {
    const found = keyFromSettings()
    apiKey = found.key
    keyFrom = found.from
  }

  console.log('=== 豆包机器翻译诊断脚本（0.2.0）===')
  console.log('端点        ' + ENDPOINT)
  console.log('Resource-Id ' + RESOURCE_ID + '   （语音那条是 seed-tts-2.0，两个值不同）')
  console.log('source      ' + (source || '(不传 = 让服务端自动检测)'))
  console.log('target      ' + target)
  console.log('文本        ' + JSON.stringify(texts))
  console.log(
    'Key         ' +
      (apiKey
        ? `${apiKey.slice(0, 6)}…${apiKey.slice(-4)}（共 ${apiKey.length} 字符，来自：${keyFrom}）`
        : `（没有！${keyFrom}）`)
  )

  if (!apiKey) {
    console.error('\n没有 Key：用 --api-key <key> 传，或先在程序的「选项 → 语音」里填一把（两处共用同一把）。')
    return 2
  }

  const body = { target_language: target, text_list: texts }
  if (source) body.source_language = source
  const headers = {
    'X-Api-Key': apiKey,
    'X-Api-Resource-Id': RESOURCE_ID,
    'X-Api-Request-Id': randomUUID(),
    'Content-Type': 'application/json'
  }

  console.log('\n--- 真发一次请求（会联网、会按 token 计费） ---')
  const started = Date.now()
  let status = 0
  let raw = ''
  let payload = null
  try {
    const response = await fetch(ENDPOINT, { method: 'POST', headers, body: JSON.stringify(body) })
    status = response.status
    raw = await response.text()
    try {
      payload = JSON.parse(raw)
    } catch {
      payload = null
    }
  } catch (err) {
    console.log(`\n连接失败（还没到业务层）：${err.message}`)
    console.log('可能是网络/代理问题 —— 这一条与"权限没开"无关。')
    return 1
  }
  const elapsed = Date.now() - started

  console.log(`HTTP ${status}   耗时 ${elapsed} ms   X-Api-Request-Id 已发（排错时给火山看）`)
  if (process.argv.includes('--json')) console.log('原始回包：' + raw)

  const code = payload && typeof payload.code === 'number' ? payload.code : null
  const ok = status === 200 && code === 20000000

  if (!ok) {
    console.log('\n❌ 没成功。')
    console.log('  code    ' + (code === null ? '(回包里没有 code)' : code))
    console.log('  message ' + ((payload && payload.message) || '(无)'))
    console.log('  人话    ' + explain(status, payload))
    return 1
  }

  const list = (payload.data && payload.data.translation_list) || []
  console.log('\n✅ 成功。')
  list.forEach((item, index) => {
    console.log(`  [${index}] ${JSON.stringify(texts[index])} → ${JSON.stringify(item.translation)}`)
    if (item.detected_source_language) console.log(`      服务端识别为：${item.detected_source_language}`)
    const usage = item.usage || {}
    console.log(
      `      usage: prompt=${usage.prompt_tokens} completion=${usage.completion_tokens} total=${usage.total_tokens}`
    )
  })
  console.log(
    '\n结论：Key 可用、`volc.speech.mt` 权限已开通、端点与请求/回包结构都对得上 —— 翻译那条路可以照这个验。'
  )
  return 0
}

main()
  .then((code) => {
    process.exitCode = typeof code === 'number' ? code : 0
  })
  .catch((err) => {
    console.error('诊断脚本自己出错：' + (err && err.stack ? err.stack : err))
    process.exitCode = 1
  })
