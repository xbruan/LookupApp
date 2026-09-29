// 一次性诊断：用**独立实现**（不依赖 C# 与 C）核对 MDict 的分区偏移与键信息块，给 C 解析器
// （native/src/dict/dsh_mdx.c）当参照 —— 两边同源但分开写，才能互相验。
//   node tools/probe-mdx-layout.mjs [词典.mdx ...]
import fs from 'node:fs'
import path from 'node:path'
import zlib from 'node:zlib'
import { fileURLToPath } from 'node:url'

const HERE = path.dirname(fileURLToPath(import.meta.url))
const ROOT = path.resolve(HERE, '..')
const DIR = process.argv[2] ? path.dirname(path.resolve(process.argv[2])) : path.join(ROOT, 'testdata')
const files = process.argv.length > 2
  ? process.argv.slice(2).map((p) => path.resolve(p))
  : fs.readdirSync(DIR).filter((f) => /\.mdx$/i.test(f)).sort().map((f) => path.join(DIR, f))

function readNum(b, off, w) {
  if (off + w > b.length) return 0
  if (w === 1) return b[off]
  if (w === 2) return b.readUInt16BE(off)
  if (w === 4) return b.readUInt32BE(off)
  let hi = 0
  for (let i = 0; i < 4; i++) hi = (hi << 8) | b[off + i]
  let lo = 0
  for (let i = 4; i < 8; i++) lo = (lo << 8) | b[off + i]
  return hi * 2 ** 32 + lo
}

for (const file of files) {
  const b = fs.readFileSync(file)
  const name = path.basename(file, '.mdx')
  const headerLen = b.readUInt32BE(0)
  const head = b.subarray(4, 4 + headerLen).toString('utf16le')
  const attr = (k) => (head.match(new RegExp(k + '="([^"]*)"')) || [, ''])[1]
  const ver = parseFloat(attr('GeneratedByEngineVersion'))
  const w = ver >= 2 ? 8 : 4
  const encName = attr('Encoding') || '(空)'
  const utf16 = !/\.mdd$/i.test(file) && /^utf-?16/i.test(encName)

  const headerEnd = headerLen + 8
  const keyHeaderMeta = ver >= 2 ? 8 * 5 : 4 * 4
  const keyBlockCount = readNum(b, headerEnd, w)
  const keyCount = readNum(b, headerEnd + w, w)
  let o = headerEnd + w * 2
  const keyInfoUnpack = ver >= 2 ? readNum(b, o, w) : 0
  if (ver >= 2) o += w
  const keyInfoPacked = readNum(b, o, w)
  const keyBlockPacked = readNum(b, o + w, w)
  const keyInfoStart = headerEnd + keyHeaderMeta + (ver >= 2 ? 4 : 0)
  const keyBlockStart = keyInfoStart + keyInfoPacked
  const recordHeaderStart = keyBlockStart + keyBlockPacked

  const ctype = ver >= 2 ? b.readUInt32BE(keyInfoStart) : 0
  const keyInfoRaw = b.subarray(keyInfoStart, keyInfoStart + keyInfoPacked)
  const payload = ver >= 2 ? keyInfoRaw.subarray(8) : keyInfoRaw
  let keyInfo = payload
  if (ver >= 2) {
    if (ctype === 0x02000000) keyInfo = zlib.inflateSync(payload)
    else if (ctype === 0) keyInfo = payload
    else throw new Error('未知压缩类型 0x' + ctype.toString(16))
  }

  // 逐块解析键信息，核对累加值
  const ws = w / 4
  let off = 0
  let entriesAcc = 0, packAcc = 0, unpackAcc = 0
  const blocks = []
  for (let i = 0; i < keyBlockCount; i++) {
    const entryCount = readNum(keyInfo, off, w); off += w
    let firstSize = readNum(keyInfo, off, ws); off += ws
    firstSize = ver >= 2 ? (utf16 ? (firstSize + 1) * 2 : firstSize + 1) : (utf16 ? firstSize * 2 : firstSize)
    const firstKey = keyInfo.subarray(off, off + firstSize).toString(utf16 ? 'utf16le' : 'utf8').replace(/\0+$/, '')
    off += firstSize
    let lastSize = readNum(keyInfo, off, ws); off += ws
    lastSize = ver >= 2 ? (utf16 ? (lastSize + 1) * 2 : lastSize + 1) : (utf16 ? lastSize * 2 : lastSize)
    const lastKey = keyInfo.subarray(off, off + lastSize).toString(utf16 ? 'utf16le' : 'utf8').replace(/\0+$/, '')
    off += lastSize
    const packSize = readNum(keyInfo, off, w); off += w
    const unpackSize = readNum(keyInfo, off, w); off += w
    blocks.push({ entryCount, firstKey, lastKey, packSize, unpackSize, packOffset: packAcc, unpackOffset: unpackAcc, entryOffset: entriesAcc })
    entriesAcc += entryCount; packAcc += packSize; unpackAcc += unpackSize
  }

  // 记录区
  const recordHeaderLen = ver >= 2 ? 4 * 8 : 4 * 4
  const recordBlockCount = readNum(b, recordHeaderStart, w)
  const recordEntries = readNum(b, recordHeaderStart + w, w)
  const recordInfoComp = readNum(b, recordHeaderStart + w * 2, w)
  const recordBlockComp = readNum(b, recordHeaderStart + w * 3, w)
  const recordInfoStart = recordHeaderStart + recordHeaderLen
  const recordBlockStart = recordInfoStart + recordInfoComp

  console.log(`── ${name} ──────────────────────────────────────────`)
  console.log(`  版本=${ver}  编码=${encName}  numWidth=${w}  压缩类型=0x${ctype.toString(16)}`)
  console.log(`  头部 [0,${headerEnd})  键区头部 [${headerEnd},${keyInfoStart})`)
  console.log(`  键信息块 [${keyInfoStart},${keyBlockStart}) 解出 ${keyInfo.length} 字节（头里记 ${keyInfoUnpack}）`)
  console.log(`  词块   [${keyBlockStart},${recordHeaderStart}) 共 ${keyBlockPacked} 字节`)
  console.log(`  记录区头部 [${recordHeaderStart},${recordInfoStart}) 记录信息 [${recordInfoStart},${recordBlockStart}) 记录块 [${recordBlockStart}, ${b.length})`)
  console.log(`  词条数 头部记 ${keyCount} / 键信息累加 ${entriesAcc} ${keyCount === entriesAcc ? '✓' : '✗'}`)
  console.log(`  词块压缩总大小 头部记 ${keyBlockPacked} / 键信息累加 ${packAcc} ${keyBlockPacked === packAcc ? '✓' : '✗'}`)
  console.log(`  记录块数=${recordBlockCount} 记录条数=${recordEntries}（与词条数${recordEntries === keyCount ? '一致 ✓' : '不一致 ✗'}）`)
  console.log(`  记录信息合计 ${recordInfoComp}，记录块压缩合计 ${recordBlockComp}，文件尾余 ${b.length - recordBlockStart - recordBlockComp}`)
  console.log(`  前 3 块：`)
  for (const blk of blocks.slice(0, 3)) {
    console.log(`    entryCount=${blk.entryCount} first="${blk.firstKey}" last="${blk.lastKey}" pack=${blk.packSize} unpack=${blk.unpackSize}`)
  }
  // 单调性
  let mono = true
  for (let i = 1; i < blocks.length; i++) if (blocks[i - 1].lastKey > blocks[i].firstKey) { mono = false; break }
  console.log(`  键块序单调=${mono ? '✓' : '✗'}`)

  // 抽第一条词块的记录，打印它的键 + 偏移，给 C 侧对照测试用
  const kb0 = blocks[0]
  const packed0 = b.subarray(keyBlockStart + kb0.packOffset, keyBlockStart + kb0.packOffset + kb0.packSize)
  const ctype0 = packed0.readUInt32BE(0)
  let kbData
  if (ctype0 === 0x02000000) kbData = zlib.inflateSync(packed0.subarray(8))
  else if (ctype0 === 0) kbData = packed0.subarray(8)
  else { console.log(`  第一个词块压缩类型 0x${ctype0.toString(16)}（跳过）`); continue }
  console.log(`  第一个词块解出 ${kbData.length} 字节（头里记 ${kb0.unpackSize}）`)
  // ⚠️ 词块里的记录格式是 `记录偏移(numWidth) + 键名 + \0`，**没有长度字段** —— 照「长度在前」读会打出垃圾，
  //    连对 C 侧实现的判断都会被带偏（参考实现：js-mdict 的 splitKeyBlock）。
  const kw = utf16 ? 2 : 1
  let p = 0
  const entries = []
  for (let i = 0; i < Math.min(kb0.entryCount, 3); i++) {
    if (p + w >= kbData.length) break
    const rec = readNum(kbData, p, w)
    p += w
    let end = -1
    for (let q = p; q + kw <= kbData.length; q += kw) {
      if (kw === 1 ? kbData[q] === 0 : kbData[q] === 0 && kbData[q + 1] === 0) { end = q; break }
    }
    if (end < 0) break
    const key = kbData.subarray(p, end).toString(utf16 ? 'utf16le' : 'utf8')
    p = end + kw
    entries.push({ key, rec })
  }
  console.log(`  前几条：${entries.map((e) => `${e.key}@${e.rec}`).join('  ')}`)
}
