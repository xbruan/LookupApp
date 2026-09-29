// 一次性：打印测试用词典变体的头部关键属性（决定我们的对照测试要覆盖哪些路）。
// 用法：node tools/dump-variant-headers.mjs [目录]
//
// 默认目录是**仓库内**的 testdata/variants/（那 13 个格式矩阵样本已冻结进来），
// 路径从脚本自身位置推出来。以前这里写死的是一条绝对路径，指向兄弟目录 0.1.3 ——
// 于是换台机器、或者样本冻结之后，这个诊断脚本都会不声不响地指到别处去。
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
const def = path.resolve(here, '..', 'testdata', 'variants')
const dir = process.argv[2] || def
if (!fs.existsSync(dir)) { console.error('目录不存在：' + dir); process.exit(2) }

for (const f of fs.readdirSync(dir).filter((x) => /\.mdx$/i.test(x)).sort()) {
  const b = fs.readFileSync(path.join(dir, f))
  const h = b.readUInt32BE(0)
  const head = b.subarray(4, 4 + h).toString('utf16le')
  const g = (k) => {
    const m = head.match(new RegExp(k + '="([^"]*)"'))
    return m ? m[1] : '-'
  }
  console.log(
    f.padEnd(30),
    ('ver=' + g('GeneratedByEngineVersion')).padEnd(9),
    ('Enc=' + g('Encrypted')).padEnd(8),
    ('Encoding=' + g('Encoding')).padEnd(14),
    'Title=' + g('Title'),
  )
}
