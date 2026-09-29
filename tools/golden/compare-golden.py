"""标准答案文件逐字节比对（带"已确认差异"白名单）。

用法：python3 tools/golden/compare-golden.py <C 侧.json> <对手.json>

默认那道 gate（tools/check-golden.sh / tools/golden-gate.ps1）的第二个参数就是
**冻结基线** tools/golden/baseline/golden-baseline.json —— 它由参考实现（C#）产出，
来源记录见 tools/golden/baseline/provenance.md。所以下面管它叫"C# 侧"。

为什么要它，而不是直接 `cmp`：
--------------------
21 本测试用词典里 **20 本**是逐字节一致的，剩下 1 本（`v2-multiblock.mdx`）有**两处**
差异 —— 而这两处**责任在参考实现那一边**，见下面的 WHITELIST 说明。
直接 `cmp` 会把"我们与参考实现对齐"和"这一处我们是对的、参考实现是错的"
混成一个红叉；而把这两条从测试用词典清单里删掉，等于**把唯一覆盖"记录横跨记录块边界"
的测试用词典移出对照测试** —— 那是拿掉覆盖，不是修好差异。

所以这份脚本的约定是：**逐字节比，只放行下面写死的那两处，多一处都不放行。**
白名单外的任何差异（包括键、词块索引、警告、条数）照旧直接失败。

WHITELIST 的依据（现场实测结果，别凭印象改）
--------------------------------------
`v2-multiblock.mdx` 的 3 个记录块按字节数均匀切分，于是有两条记录**横跨块边界**
（`apply` 的字节区间是 [205, 292)，而第 0 块只到 221；`banana` 是 [384, 454)，
第 1 块只到 442）。参考实现 `MdictCore.GetRecordBytes` **只从"记录起点所在的
那一块"取字节**（`relativeEnd` 被夹到该块末尾），所以这两条被截断成
16 / 54 字节；C 内核按记录偏移**拼接两块**，给出完整的 87 / 70 字节。

三份证据都指向 C 是对的、参考实现截断了：
  1. 测试用词典的生成脚本（0.1.3 里的 `tools/MdxProbe/make-variants.mjs` —— 那支脚本本身
     没有随样本一起收进本仓库，样本已冻结在 `testdata/variants/`）写的 `recordStarts`
     是**绝对**偏移（`cursor` 跨全部记录累加），记录区间就该是 [205, 292)；
  2. 参考实现自己的 `FirstRecordStartOfBlock(1)` 返回 **292**（正是跨块时该用的
     那个值），它的 `GetDefinition` 却没用上 —— 同一个块边界上有两个来源；
  3. C 侧这两条解出来是完整的 HTML（`…</div></div>\0`），参考实现停在半个标签里
     （`<div class="entr`）。
所以白名单记的是**已知且已判定责任方**的差异，不是"我们放自己一马"。

（这条差异的完整经过在 0.2.0 的开发记录「标准答案文件对照测试 · 已知差异」一节里；
那份开发记录不在本仓库，本仓库里这三条证据与测试用词典本身都已冻结。）
"""

import json
import sys

# (测试用词典名, 记录下标, C 侧字节数, C# 侧字节数)
#
# ⚠️ 这两个数是**实测**的、而且量的是"解码成 UTF-8 之后的字节数"（含记录末尾那个
# U+0000）：C 侧 71 / 66，C# 侧 16 / 54。别拿"记录跨度"（87 / 70）来填 ——
# 那是**原始字节**跨度，中文一个字 3 字节，两者不相等。写错会被检查标准当场挡下来
# （第一次就写成了 87 / 70，比对直接报"与白名单不符"）。
WHITELIST = [
    ('v2-multiblock.mdx', 2, 71, 16),
    ('v2-multiblock.mdx', 4, 66, 54),
]

FIELDS = ('isMdd', 'version', 'encrypted', 'encoding', 'numWidth', 'title', 'keyCount',
          'keyBlockCount', 'blockOrderMonotone')


def load(path):
    """两边的输出都可能有**非法 UTF-8 字节**（GBK/BIG5 词典的原文照原样吐出来），
    所以按 replace 解 —— 这一层的目的是比结构，不是比编码。"""
    return json.loads(open(path, 'rb').read().decode('utf-8', 'replace'))


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    a_all = load(sys.argv[1])
    b_all = load(sys.argv[2])

    if len(a_all['files']) != len(b_all['files']):
        print('  ✗ 测试用词典条数不同：C=%d C#=%d' % (len(a_all['files']), len(b_all['files'])))
        return 1

    allowed = {}
    for name, idx, ca, cb in WHITELIST:
        allowed.setdefault(name, {})[idx] = (ca, cb)
    used = set()

    problems = []
    exact = 0
    for fa, fb in zip(a_all['files'], b_all['files']):
        name = fa.get('name')
        if name != fb.get('name'):
            problems.append('%s: 测试用词典顺序不一致（C# 那边是 %s）' % (name, fb.get('name')))
            continue
        if fa.get('open') != fb.get('open'):
            problems.append('%s: open 不同 C=%s C#=%s（原因 C=%r C#=%r）'
                            % (name, fa.get('open'), fb.get('open'),
                               fa.get('reason'), fb.get('reason')))
            continue
        if not fa.get('open'):
            exact += 1
            continue

        ok = True
        for f in FIELDS:
            if fa['info'].get(f) != fb['info'].get(f):
                ok = False
                problems.append('%s: info.%s 不同 C=%r C#=%r'
                                % (name, f, fa['info'].get(f), fb['info'].get(f)))
        for f in ('keyBlocks', 'keys', 'warnings'):
            if fa.get(f) != fb.get(f):
                ok = False
                problems.append('%s: %s 不同（C %r / C# %r）'
                                % (name, f, fa.get(f), fb.get(f)))

        ra, rb = fa.get('records') or [], fb.get('records') or []
        if len(ra) != len(rb):
            ok = False
            problems.append('%s: 记录条数不同 C=%d C#=%d' % (name, len(ra), len(rb)))
        for i in range(min(len(ra), len(rb))):
            if ra[i] == rb[i]:
                continue
            expect = allowed.get(name, {}).get(i)
            if expect is None:
                ok = False
                problems.append('%s: records[%d] 不同（不在白名单里）'
                                % (name, i))
                continue
            used.add((name, i))
            la = len(ra[i] or '')
            lb = len(rb[i] or '')
            if (la, lb) != expect:
                ok = False
                problems.append('%s: records[%d] 的**字节数**与白名单不符：'
                                '实测 C=%d C#=%d，白名单写的是 C=%d C#=%d'
                                % (name, i, la, lb, expect[0], expect[1]))
                continue
            problems.append('%s: records[%d] 已知差异（白名单，责任在参考实现）C=%d 字节 / C#=%d 字节'
                            % (name, i, la, lb))
        if ok:
            exact += 1

    for name, idx, _ca, _cb in WHITELIST:
        if (name, idx) not in used:
            problems.append('%s: 白名单里的 records[%d] 这次**没有出现差异** —— '
                            '要么测试用词典变了、要么两边已经一致，白名单该删掉'
                            % (name, idx))

    print('── 比对结果（%d 本测试用词典）──' % len(a_all['files']))
    for p in problems:
        print('  · %s' % p)

    hard = [p for p in problems if '白名单' not in p or '不符' in p or '没有出现差异' in p]
    if hard:
        print('  ✗ 有 %d 处**不该有**的差异。' % len(hard))
        return 1

    print('  ✅ %d/%d 本测试用词典逐字节相同；另有 %d 处已确认差异（责任在参考实现，见本脚本的 WHITELIST）'
          % (exact, len(a_all['files']), len(WHITELIST)))
    return 0


sys.exit(main())
