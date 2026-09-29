#!/usr/bin/env node
/*
 * `diff-png.mjs` —— **两张 PNG 逐像素比**（不需要任何图像依赖）。
 *
 *   node tools/diff-png.mjs <a.png> <b.png>            # 只报差异（有差异就非零退出）
 *   node tools/diff-png.mjs <a.png> <b.png> --rows     # 再打印"哪几行差了多少像素"
 *
 * ## 为什么要有它（它是被一次真事逼出来的）
 *
 * 视觉类改动在本仓库的规矩是"**先出图交人确认，再落地**"（项目规范，
 * 出图办法见 `docs/design/图标设计与改进指导.md`）。出图那条路是
 * "把待落地的 CSS **注入**页面 → 拍图"（因为落不落地还没定），落地之后再拍一张。
 * 于是必然要回答一个问题：**"落地后的渲染，与给人看过、批准的那张图，是不是同一张？"**
 *
 * 2026-09 那一轮就靠这个比对抓到了一处真差异：出图脚本里为了压掉旧规则写了一句
 * `border-color: transparent`，**把新加的那条虚线一起压掉了**（同优先级、后写的赢），
 * 于是"当前词典"那一行少一条虚线 —— 肉眼在两倍缩放的图上根本看不出来，
 * 逐像素一比就只剩那一行（426 个像素，`y=500`）。改掉注入脚本之后**逐字节相同**。
 *
 * 所以它是"落地核对"这一步的机械依据：**不比人眼，比像素**。
 *
 * ## 怎么比
 *
 * 本仓库没有图像处理依赖（`node_modules` 只有 3 个包），所以走**本机 headless Edge**：
 * 生成一张临时 HTML，把两张图各画进 canvas，读 `getImageData` 逐个像素比，
 * 结果写进 DOM，再用 `--dump-dom` 取回来。`--allow-file-access-from-files` 是必需的 ——
 * 少了它 file:// 的图会把 canvas 弄脏（tainted），`getImageData` 直接抛 SecurityError。
 *
 * ⚠️ 两张图的尺寸必须一样；不一样直接报"尺寸不同"（那种情况下逐像素比没有意义）。
 */
import { execFileSync } from 'node:child_process'
import { mkdtempSync, readFileSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import path from 'node:path'
import { pathToFileURL } from 'node:url'

const EDGE = 'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe'

const args = process.argv.slice(2)
const files = args.filter((a) => !a.startsWith('--'))
const wantRows = args.includes('--rows')
if (files.length !== 2) {
  console.log('用法: node tools/diff-png.mjs <a.png> <b.png> [--rows]')
  process.exit(2)
}

const [a, b] = files.map((f) => path.resolve(f))
const dir = mkdtempSync(path.join(tmpdir(), 'dsh-diff-png-'))
const page = path.join(dir, 'diff.html')
const url = (p) => pathToFileURL(p).href

writeFileSync(
  page,
  `<!doctype html><html><head><meta charset="utf-8"></head><body><pre id="o">…</pre>
<script>
const load = (src) => new Promise((res, rej) => {
  const i = new Image(); i.onload = () => res(i); i.onerror = () => rej(new Error('读不到 ' + src)); i.src = src;
});
(async () => {
  const out = {};
  try {
    const A = await load(${JSON.stringify(url(a))});
    const B = await load(${JSON.stringify(url(b))});
    out.sizeA = [A.width, A.height];
    out.sizeB = [B.width, B.height];
    if (A.width !== B.width || A.height !== B.height) {
      out.verdict = '尺寸不同，逐像素比没有意义';
    } else {
      const px = (img) => {
        const c = document.createElement('canvas');
        c.width = img.width; c.height = img.height;
        const x = c.getContext('2d');
        x.drawImage(img, 0, 0);
        return x.getImageData(0, 0, img.width, img.height).data;
      };
      const da = px(A), db = px(B), W = A.width;
      let n = 0, minX = 1e9, minY = 1e9, maxX = -1, maxY = -1, first = null;
      const rows = {};
      for (let i = 0; i < da.length; i += 4) {
        if (da[i] === db[i] && da[i+1] === db[i+1] && da[i+2] === db[i+2] && da[i+3] === db[i+3]) continue;
        n++;
        const x = (i / 4) % W, y = Math.floor(i / 4 / W);
        rows[y] = (rows[y] || 0) + 1;
        if (x < minX) minX = x; if (y < minY) minY = y;
        if (x > maxX) maxX = x; if (y > maxY) maxY = y;
        if (!first) first = { x, y, a: [da[i],da[i+1],da[i+2],da[i+3]], b: [db[i],db[i+1],db[i+2],db[i+3]] };
      }
      out.diffPixels = n;
      out.totalPixels = A.width * A.height;
      out.bbox = n ? { minX, minY, maxX, maxY } : null;
      out.firstDiff = first;
      out.rows = rows;
      out.verdict = n === 0 ? '逐像素相同' : '有差异';
    }
  } catch (e) { out.error = String(e && e.message || e); }
  document.getElementById('o').textContent = JSON.stringify(out);
})();
</script></body></html>`,
  'utf8'
)

const dom = execFileSync(
  EDGE,
  [
    '--headless=new', '--disable-gpu', '--allow-file-access-from-files',
    '--virtual-time-budget=8000', '--dump-dom', url(page),
  ],
  { encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] }
)

const m = dom.match(/\{"sizeA".*?\}<\/pre>/s) || dom.match(/\{"sizeA".*?\}/s)
if (!m) {
  console.log('拿不到比对结果（headless Edge 没给出 DOM）：')
  console.log(dom.slice(0, 400))
  process.exit(2)
}
const r = JSON.parse(m[0].replace(/<\/pre>$/, ''))

console.log(`A: ${path.basename(a)}  ${r.sizeA[0]}×${r.sizeA[1]}`)
console.log(`B: ${path.basename(b)}  ${r.sizeB[0]}×${r.sizeB[1]}`)
if (r.error) { console.log('出错：' + r.error); process.exit(2) }
if (r.verdict === '尺寸不同，逐像素比没有意义') { console.log(r.verdict); process.exit(1) }

console.log(`差异像素 ${r.diffPixels} / ${r.totalPixels}`)
if (r.diffPixels) {
  console.log(`差异范围 x ${r.bbox.minX}–${r.bbox.maxX} / y ${r.bbox.minY}–${r.bbox.maxY}`)
  console.log(`第一个差异点 (${r.firstDiff.x},${r.firstDiff.y})：A=${r.firstDiff.a}  B=${r.firstDiff.b}`)
  if (wantRows) {
    const rows = Object.entries(r.rows).sort((p, q) => Number(p[0]) - Number(q[0]))
    console.log(`涉及 ${rows.length} 行：` + rows.map(([y, n]) => `y${y}=${n}`).join(' '))
  }
}
console.log(r.verdict)
process.exit(r.diffPixels === 0 ? 0 : 1)
