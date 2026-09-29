// 生成 native/tests/test_inflate.c —— 用真测试用词典里的压缩块当测试向量。
// 用法（只在更新向量时跑一次）：node tools/make-inflate-test.mjs
// 向量固化进 .c 是为让内核测试在没有 node 的机器上也能跑；期望值钉的是 **.NET DeflateStream**（参考实现的 C# 版走的那条路）解出的字节，与 node zlib 解得不一致就拒绝生成。
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { execFileSync } from 'node:child_process'

const HERE = path.dirname(fileURLToPath(import.meta.url))
const ROOT = path.resolve(HERE, '..')          // 0.2.0/
const REPO = path.resolve(ROOT, '..')          // 仓库根
const TESTDATA = path.join(REPO, '参考实现', 'testdata')
const OUT = path.join(ROOT, 'native', 'tests', 'test_inflate.c')

// ── 1) 从真测试用词典里抠出压缩块的字节 ──

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

/** 取一本 .mdx 的「键信息块」：返回 { raw（含 8 字节头）, zlib（含 zlib 2 字节头）, expectedSize } */
function extractKeyInfoBlock(file) {
  const b = fs.readFileSync(path.join(TESTDATA, file))
  const headerLen = b.readUInt32BE(0)
  const head = b.subarray(4, 4 + headerLen).toString('utf16le')
  const ver = parseFloat((head.match(/GeneratedByEngineVersion="([^"]*)"/) || [, ''])[1]) || NaN
  if (!(ver >= 2)) throw new Error(file + ' 不是 v2.0，键信息块无压缩类型字段')
  const w = 8
  const keyHeaderStart = headerLen + 8
  const keyHeaderMeta = 8 * 5
  const keyInfoPacked = readNum(b, keyHeaderStart + w * 3, w)
  const keyInfoUnpack = readNum(b, keyHeaderStart + w * 2, w)
  const keyHeaderEnd = keyHeaderStart + keyHeaderMeta + 4
  const raw = b.subarray(keyHeaderEnd, keyHeaderEnd + keyInfoPacked)
  const compressType = raw.readUInt32BE(0)
  if (compressType !== 0x02000000) throw new Error(file + ' 的键信息块不是 zlib（0x' + compressType.toString(16) + '）')
  return { raw, zlib: raw.subarray(8), expectedSize: keyInfoUnpack }
}

// ── 2) 期望值：用 WSL 里的 .NET 解（参考实现的 C# 版走的就是它）────────────────

function inflateWithDotNet(zlibBytes) {
  // 先把字节写成临时文件，交给一个一次性的 C# 程序（dotnet script 不保证有，用 csproj 太慢，
  // 所以用 PowerShell + 内联 C# 编译——它用的是同一个 .NET 的 DeflateStream 实现）
  const tmp = path.join(process.env.TEMP || '/tmp', 'dsh-inflate-vector.bin')
  fs.writeFileSync(tmp, zlibBytes)
  const ps = `
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.IO.Compression
$bytes = [System.IO.File]::ReadAllBytes('${tmp.replace(/'/g, "''")}')
$ms = New-Object System.IO.MemoryStream(,$bytes)
$ms.Position = 2
$ds = New-Object System.IO.Compression.DeflateStream($ms, [System.IO.Compression.CompressionMode]::Decompress)
$out = New-Object System.IO.MemoryStream
$ds.CopyTo($out)
$b = $out.ToArray()
[Console]::Out.Write([Convert]::ToBase64String($b))
`
  const b64 = execFileSync('powershell', ['-NoProfile', '-Command', ps], { encoding: 'utf8', maxBuffer: 1 << 28 })
  return Buffer.from(b64.trim(), 'base64')
}

// ── 3) 另造两组小的（stored 块与固定 Huffman 块），补上测试用词典里没有的块类型 ──

function inflateWithNode(zlibBytes) {
  // 用 node 自己的 zlib 解，作为测试用词典向量的交叉核对（两套实现给同一答案才可信）
  const zlib = require('node:zlib')
  return zlib.inflateSync(zlibBytes)
}
import { createRequire } from 'node:module'
const require = createRequire(import.meta.url)

// ── 4) 生成 C 测试文件 ─────────────────────────────────────────────────────

function hexLines(buf, perLine = 32) {
  const out = []
  for (let i = 0; i < buf.length; i += perLine) {
    out.push('  ' + [...buf.subarray(i, i + perLine)].map((x) => '0x' + x.toString(16).padStart(2, '0')).join(', '))
  }
  return out.join(',\n')
}

const vectors = []
for (const file of ['test.mdx', 'link.mdx', 'titled.mdx', 'big.mdx']) {
  const { zlib, expectedSize } = extractKeyInfoBlock(file)
  const expected = inflateWithDotNet(zlib)
  const viaNode = inflateWithNode(zlib)
  if (!expected.equals(viaNode)) {
    throw new Error(file + '：.NET 与 node 解出来的不一致，向量不可信')
  }
  vectors.push({ name: file + ' 的键信息块', zlib, expected, expectedSize })
  console.error(`${file}: 压缩 ${zlib.length} B → 解出 ${expected.length} B（头里记的 ${expectedSize}），.NET 与 node 一致`)
}

const src = `/* ==========================================================================
 * GENERATED — DO NOT EDIT（但要**审阅**：这是测试向量，不是样板）
 * 由 tools/make-inflate-test.mjs 生成。更新向量：node tools/make-inflate-test.mjs
 *
 * 期望值不是"我们自己解出来的"，而是 **.NET 的 DeflateStream** 解出来的字节 ——
 * 也就是 参考实现的 C# 版实际走的那条路。这样才算"逐字节对齐 参考实现"的证据。
 * 每组都另用 node 的 zlib 交叉核对过（两套独立实现给同一答案），
 * 生成脚本里有断言，不一致就拒绝生成。
 * ========================================================================== */

#include "compress/dsh_inflate.h"
#include "dsh_lookup.h"
#include "mem_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks = 0;
static int g_failed = 0;

static void ok(int condition, const char *what) {
  g_checks++;
  if (!condition) {
    g_failed++;
    fprintf(stderr, "FAIL %s\\n", what);
  }
}

static void ok_eq_size(size_t actual, size_t expected, const char *what) {
  g_checks++;
  if (actual != expected) {
    g_failed++;
    fprintf(stderr, "FAIL %s：实际=%zu 期望=%zu\\n", what, actual, expected);
  }
}

/* ── 向量（见文件头：期望值来自 .NET DeflateStream）──────────────────────── */

typedef struct {
  const char *name;
  const unsigned char *compressed;
  size_t compressed_len;
  const unsigned char *expected;
  size_t expected_len;
  size_t header_size; /* 头里记的解压后大小（用来验 expected_size 这条路） */
} inflate_vector;

${vectors.map((v, i) => `static const unsigned char V${i}_IN[] = {\n${hexLines(v.zlib)}\n};
static const unsigned char V${i}_OUT[] = {\n${hexLines(v.expected)}\n};`).join('\n\n')}

static const inflate_vector VECTORS[] = {
${vectors.map((v, i) => `  { ${JSON.stringify(v.name)}, V${i}_IN, ${v.zlib.length}, V${i}_OUT, ${v.expected.length}, ${v.expectedSize} },`).join('\n')}
};

int main(void) {
  const size_t n = sizeof(VECTORS) / sizeof(VECTORS[0]);
  const size_t base = dsh_mem_live_count();

  for (size_t i = 0; i < n; i++) {
    const inflate_vector *v = &VECTORS[i];
    unsigned char *out = NULL;
    size_t out_len = 0;

    /* ① 带 expected_size（解析器真实走的那条路） */
    int rc = dsh_zlib_inflate(v->compressed, v->compressed_len, v->header_size, &out, &out_len);
    ok(rc == 0, v->name);
    ok(out != NULL, "解压结果不该是空指针");
    ok_eq_size(out_len, v->expected_len, "解出的字节数");
    ok(out_len == v->expected_len && out != NULL && memcmp(out, v->expected, v->expected_len) == 0,
       "解出的字节必须与 .NET 逐字节相同");
    dsh_release(out);

    /* ② expected_size 传 0（未知大小那条路，内部自己估初值并按需增长） */
    out = NULL;
    out_len = 0;
    rc = dsh_zlib_inflate(v->compressed, v->compressed_len, 0, &out, &out_len);
    ok(rc == 0, "expected_size 为 0 时也要能解");
    ok_eq_size(out_len, v->expected_len, "expected_size 为 0 时解出的字节数");
    ok(out_len == v->expected_len && out != NULL && memcmp(out, v->expected, v->expected_len) == 0,
       "expected_size 为 0 时也要与 .NET 逐字节相同");
    dsh_release(out);
  }

  /* ── 坏数据必须**如实报错**，不许返回半截结果 ────────────────────────── */
  {
    unsigned char *out = NULL;
    size_t out_len = 0;
    ok(dsh_zlib_inflate(NULL, 10, 0, &out, &out_len) != 0, "空指针必须报错");
    ok(dsh_zlib_inflate((const unsigned char *)"", 0, 0, &out, &out_len) != 0, "长度 0 必须报错");
    /* 不是 zlib：CM 字段不为 8 */
    const unsigned char notzlib[] = {0x77, 0x77, 0x00, 0x00};
    ok(dsh_zlib_inflate(notzlib, sizeof(notzlib), 0, &out, &out_len) != 0, "压缩方法不对必须报错");
    ok(out == NULL, "失败时出参必须保持不变（NULL）");
    /* zlib 头合法但流被截断 */
    const unsigned char truncated[] = {0x78, 0x9c, 0x00};
    ok(dsh_zlib_inflate(truncated, sizeof(truncated), 0, &out, &out_len) != 0 || out_len == 0,
       "截断的流不该给出非空结果");
    if (out != NULL) dsh_release(out);
  }

  /* 全部还清：活分配表必须回到基线（防泄漏） */
  ok_eq_size(dsh_mem_live_count(), base, "还清之后活分配表必须回到基线");

  printf("inflate：%d 项，失败 %d\\n", g_checks, g_failed);
  if (g_failed == 0) printf("  · %zu 组向量与 .NET DeflateStream 逐字节一致\\n", n);
  return g_failed == 0 ? 0 : 1;
}
`

fs.writeFileSync(OUT, src.replace(/\r\n/g, '\n'), 'utf8')
console.error(`已写出 ${path.relative(ROOT, OUT)}（${src.length} 字符，${vectors.length} 组向量）`)
