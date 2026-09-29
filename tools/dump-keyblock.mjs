// 一次性：把一本 .mdx 的词块解出来，逐条打印 [记录偏移][键名]。
// 用法：node 0.2.0/tools/dump-keyblock.mjs <词典.mdx>
import fs from 'node:fs'
import path from 'node:path'
import zlib from 'node:zlib'

const file = path.resolve(process.argv[2])
const b = fs.readFileSync(file)
const rd = (buf, off, w) => {
  let v = 0
  for (let i = 0; i < w; i++) v = v * 256 + buf[off + i]
  return v
}
const headerLen = b.readUInt32BE(0)
const head = b.subarray(4, 4 + headerLen).toString('utf16le')
const ver = parseFloat((head.match(/GeneratedByEngineVersion="([^"]*)"/) || [, ''])[1])
const w = ver >= 2 ? 8 : 4
const keyHeaderEnd = headerLen + 8 + (ver >= 2 ? 8 * 5 + 4 : 4 * 4)
const kip = rd(b, headerLen + 8 + w * 3, w)
const kbp = rd(b, headerLen + 8 + w * 4, w)
const keyInfoStart = keyHeaderEnd
const keyBlockStart = keyInfoStart + kip
const ctype = b.readUInt32BE(keyInfoStart)
const keyInfo = ctype === 0x02000000 ? zlib.inflateSync(b.subarray(keyInfoStart + 8, keyInfoStart + kip)) : b.subarray(keyInfoStart + 8, keyInfoStart + kip)

// 解析第一个键块的 pack/unpack
const ws = w / 4
let off = 0
const entryCount = rd(keyInfo, off, w); off += w
let firstSize = rd(keyInfo, off, ws); off += ws
firstSize = ver >= 2 ? firstSize + 1 : firstSize
off += firstSize
let lastSize = rd(keyInfo, off, ws); off += ws
lastSize = ver >= 2 ? lastSize + 1 : lastSize
off += lastSize
const packSize = rd(keyInfo, off, w); off += w
const unpackSize = rd(keyInfo, off, w)

const packed = b.subarray(keyBlockStart, keyBlockStart + packSize)
const ct = packed.readUInt32BE(0)
const kbData = ct === 0x02000000 ? zlib.inflateSync(packed.subarray(8)) : packed.subarray(8)
console.log(`文件 ${path.basename(file)}：词块 entryCount=${entryCount} pack=${packSize} unpack=${unpackSize}，实解出 ${kbData.length} 字节`)
console.log('词块内容（hex）：')
console.log(kbData.toString('hex').replace(/(.{64})/g, '$1\n'))

const kw = 1
let p = 0
for (let i = 0; i < entryCount && p + w < kbData.length; i++) {
  const rec = rd(kbData, p, w)
  p += w
  let end = -1
  for (let q = p; q + kw <= kbData.length; q += kw) {
    if (kbData[q] === 0) { end = q; break }
  }
  if (end < 0) { console.log(`  第 ${i} 条：没有终止符，停在 ${p}`); break }
  const key = kbData.subarray(p, end).toString('utf8')
  p = end + kw
  console.log(`  第 ${i} 条：偏移=${rec} 键名=${JSON.stringify(key)}（读完于 ${p}/${kbData.length}）`)
}
