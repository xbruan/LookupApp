#!/usr/bin/env node
/**
 * 生成「带脚本的、不带 .mdd 的词典」测试用词典（D 级：只有真实程序里才同时有 WebView2、CSP、iframe sandbox 与虚拟主机路由）：
 *   sibling.mdx（单条词条、没有 .mdd）+ sibling.js（必须真在词条 iframe 里跑到）+ sibling-control.mjs（同一写法、只差扩展名，必须取不到 —— 这才证明是白名单那一行在起作用）。
 *  用法 node tools/make-sibling-fixture.mjs [输出目录]（默认 testdata/tmp-sibling）；词条名 siblingprobe 与两个界面标记 id 跟 shell/Lookup.App/SelfCheck.cs 成对，改名要两处一起改；别改成 import 参考实现/tools/MdxProbe/make-variants.mjs（那会把参考实现的 js-mdict 依赖拉进来）。
 */
import fs from 'node:fs'
import path from 'node:path'
import zlib from 'node:zlib'
import { fileURLToPath } from 'node:url'

const HERE = path.dirname(fileURLToPath(import.meta.url))
const ROOT = path.resolve(HERE, '..')
const OUT = path.resolve(process.argv[2] || path.join(ROOT, 'testdata', 'tmp-sibling'))

const WORD = 'siblingprobe'

/** 词条正文：两个界面标记各由**同目录的脚本**去改；两个 `<script src>` 只差扩展名 */
const DEF =
  '<div class="entry"><h1>siblingprobe</h1>' +
  '<p>同目录脚本词典：这一页的两个记号由同目录散放的脚本改写（见 tools/make-sibling-fixture.mjs）。</p>' +
  '<div id="sibling-marker">pending</div>' +
  '<div id="sibling-control">pending</div>' +
  '<script src="sibling.js"></script>' +
  '<script src="sibling-control.mjs"></script>' +
  '</div>'

const SCRIPT_JS = "document.getElementById('sibling-marker').textContent = 'script-ran';\n"
const SCRIPT_MJS = "document.getElementById('sibling-control').textContent = 'mjs-ran';\n"

/* ── MDX v2.0 / UTF-8 / zlib / 不加密 / 单键块单记录块 ── */

const NUM_WIDTH = 8

function writeNumber(value, width) {
  const buffer = Buffer.alloc(width)
  if (width === 1) buffer.writeUInt8(value, 0)
  else if (width === 2) buffer.writeUInt16BE(value, 0)
  else if (width === 4) buffer.writeUInt32BE(value, 0)
  else if (width === 8) buffer.writeBigUInt64BE(BigInt(value), 0)
  else throw new Error('不支持的字段宽度 ' + width)
  return buffer
}

/** 块开头的 4 字节压缩类型字段是**小端**（zlib = 02 00 00 00） */
function writeBlockType(type) {
  const buffer = Buffer.alloc(4)
  buffer.writeUInt32LE(type, 0)
  return buffer
}

/** Node 没有 `zlib.adler32`，用 deflate 尾部那 4 字节校验和取（与 zlib 实现一致） */
function adler32(data) {
  const deflated = zlib.deflateSync(Buffer.from(data))
  return deflated.readUInt32BE(deflated.length - 4)
}

function zlibBlock(payload) {
  return Buffer.concat([
    writeBlockType(2),
    writeNumber(adler32(payload), 4),
    zlib.deflateSync(Buffer.from(payload))
  ])
}

function buildDictionary(entries) {
  const terminator = Buffer.from([0])

  /* 记录区：每条正文后面跟一个 \0（与 test.mdx 一致） */
  const recordParts = entries.map(([, value]) => Buffer.concat([Buffer.from(value, 'utf8'), terminator]))
  const recordData = Buffer.concat(recordParts)
  const recordStarts = []
  let cursor = 0
  for (const part of recordParts) {
    recordStarts.push(cursor)
    cursor += part.length
  }
  const recordBlock = zlibBlock(recordData)

  /* 词块：记录偏移 + 键名 + \0 */
  const pieces = []
  entries.forEach(([key], index) => {
    pieces.push(writeNumber(recordStarts[index], NUM_WIDTH))
    pieces.push(Buffer.from(key, 'utf8'))
    pieces.push(terminator)
  })
  const keyRaw = Buffer.concat(pieces)
  const keyBlock = zlibBlock(keyRaw)
  const firstKey = Buffer.concat([Buffer.from(entries[0][0], 'utf8'), terminator])
  const lastKey = Buffer.concat([Buffer.from(entries[entries.length - 1][0], 'utf8'), terminator])

  /* 键信息块（其**自身**又是一个 zlib 块） */
  const keyInfoRaw = Buffer.concat([
    writeNumber(entries.length, NUM_WIDTH),
    writeNumber(firstKey.length - 1, 2),
    firstKey,
    writeNumber(lastKey.length - 1, 2),
    lastKey,
    writeNumber(keyBlock.length, NUM_WIDTH),
    writeNumber(keyRaw.length, NUM_WIDTH)
  ])
  const keyInfoBlock = zlibBlock(keyInfoRaw)

  /* 头部：UTF-16LE 的 XML，前后各一个长度字段与 adler32 */
  const attributes = {
    GeneratedByEngineVersion: '2.0',
    RequiredEngineVersion: '2.0',
    Encrypted: 'No',
    Encoding: 'UTF-8',
    Format: 'Html',
    Stripkey: 'Yes',
    KeyCaseSensitive: 'No',
    Title: '',
    Description: '',
    CreationDate: '2026-9-14',
    Compact: 'Yes',
    Compat: 'Yes',
    DataSourceFormat: '106',
    StyleSheet: '',
    Left2Right: 'Yes',
    RegisterBy: ''
  }
  const headerXml =
    '<Dictionary ' +
    Object.entries(attributes)
      .map(([key, value]) => `${key}="${value}"`)
      .join(' ') +
    ' />\u0000'
  const headerBytes = Buffer.from(headerXml, 'utf16le')
  const header = Buffer.concat([
    writeNumber(headerBytes.length, 4),
    headerBytes,
    writeNumber(adler32(headerBytes), 4)
  ])

  /* 键区头部 + 记录区头部 */
  const keyHeader = Buffer.concat([
    writeNumber(1, NUM_WIDTH), // 键块数
    writeNumber(entries.length, NUM_WIDTH),
    writeNumber(keyInfoRaw.length, NUM_WIDTH),
    writeNumber(keyInfoBlock.length, NUM_WIDTH),
    writeNumber(keyBlock.length, NUM_WIDTH)
  ])
  const recordHeader = Buffer.concat([
    writeNumber(1, NUM_WIDTH), // 记录块数
    writeNumber(entries.length, NUM_WIDTH),
    writeNumber(2 * NUM_WIDTH, NUM_WIDTH),
    writeNumber(recordBlock.length, NUM_WIDTH)
  ])
  const recordInfo = Buffer.concat([
    writeNumber(recordBlock.length, NUM_WIDTH),
    writeNumber(recordData.length, NUM_WIDTH)
  ])

  return Buffer.concat([
    header,
    keyHeader,
    writeNumber(adler32(keyInfoBlock), 4),
    keyInfoBlock,
    keyBlock,
    recordHeader,
    recordInfo,
    recordBlock
  ])
}

/* 词条必须按**序数序**排好（词块的二分查找建立在这个前提上）—— 这里只有一条，仍然断言一遍 */
const entries = [[WORD, DEF]]
const sorted = [...entries].sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0))
if (sorted.some((entry, index) => entry[0] !== entries[index][0])) {
  throw new Error('词条没有按序数序排列：' + entries.map((e) => e[0]).join(' / '))
}

fs.rmSync(OUT, { recursive: true, force: true })
fs.mkdirSync(OUT, { recursive: true })
const mdx = buildDictionary(sorted)
fs.writeFileSync(path.join(OUT, 'sibling.mdx'), mdx)
fs.writeFileSync(path.join(OUT, 'sibling.js'), SCRIPT_JS)
fs.writeFileSync(path.join(OUT, 'sibling-control.mjs'), SCRIPT_MJS)

console.log(`已生成 ${OUT}`)
console.log(`  sibling.mdx          ${mdx.length} 字节（1 个词条：${WORD}）`)
console.log('  sibling.js           会把 #sibling-marker 改成 script-ran（**必须能跑到**）')
console.log('  sibling-control.mjs  会把 #sibling-control 改成 mjs-ran（**必须取不到**，它是反向对照）')
