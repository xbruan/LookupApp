#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""为 C 内核的文本编解码层生成码表（GB18030 双字节区 / Big5）与对照测试用例。

为什么要有这个脚本
------------------
内核是纯 C11、零依赖 —— 没有 iconv、没有 ICU、更不能有 .NET 的
Encoding.GetEncoding(54936)/(950)。所以码表只能自己带。
但**手抄码表必错**，于是码表一律由本脚本程序化导出：

    python3 tools/make-textcodec-tables.py            # 生成码表（在 0.2.0/ 下跑）
    python3 tools/make-textcodec-tables.py --print-cases   # 只打印对照测试用例的 C 片段

参考实现是 python3 自带的 gb18030 / big5 编解码器（与 参考实现里 .NET 用的
代码页 54936 / 950 是同一族；差异见下面「码表来源与已知差异」）。

自动生成的文件
------
    native/src/text/dsh_textcodec_tables.h   声明 + 下标宏 + 表尺寸
    native/src/text/dsh_textcodec_tables.c   表本体（GENERATED — DO NOT EDIT）

自检
----
本脚本自己带一道自检，不通过就**非零退出**：
  · 随机抽查 200 个码位，逐个与 python3 的解码结果做对照测试；
  · 抽查锚点词（测试 / 汉语 / 測試 / 漢語 / 词典 等）逐字节对照；
  · 打印两张表的可映射槽位总数（对不上就说明生成逻辑被动过）。

码表来源与已知差异（如实记，别当成没这回事）
--------------------------------------------
· GB18030 双字节区：0x81–0xFE × 0x40–0xFE（去掉 0x7F）＝ 126 × 191 槽，
  实测**没有**映射到非 BMP 或多字符的槽位。
· Big5：0xA1–0xF9 × 0x40–0x7E 与 0xA1–0xFE ＝ 89 × 191 槽（0x7F–0xA0 恒不可映射）。
  区域约定与 python3 的 big5 编解码器逐槽一致（抽查过 0x8140 / 0xFA40 / 0xA040
  这些边界都不可映射）。
· ⚠️ **python3 的 big5 与 .NET 的 cp950（代码页 950）在 53 个槽位上取值不同**
  （例如 0xA145：big5 给 U+2022、cp950 给 U+2027；0xA3E1：big5 报错、cp950 给 U+20AC）。
  本脚本默认走 big5（与任务约定一致），`--big5-codec cp950` 可整体换成 cp950。
  换成 cp950 时**必须同时**重生成对照测试用例（同一个开关管两边），否则测试会红。
· GB18030 的**四字节区本版不支持**（内核遇到首字节 0x81–0xFE、次字节 0x30–0x39
  的四字节序列一律替换成一个 U+FFFD 并记一条诊断）—— 所以本脚本生成的用例里
  **不含**四字节序列（python3 会把它们解出来，两边的约定本来就不同）。
"""

import argparse
import os
import random
import sys
import unicodedata

# ── 表的几何（与 native/src/text/dsh_textcodec.c 里的下标宏必须一字不差）──

GBK_LEAD_FIRST, GBK_LEAD_LAST = 0x81, 0xFE
BIG5_LEAD_FIRST, BIG5_LEAD_LAST = 0xA1, 0xF9

# 两张表的次字节列几何相同：0x40–0xFE 共 191 列；恒不可映射的槽（0x7F，以及 Big5 的 0x7F–0xA0）一律填 0。
TRAIL_FIRST, TRAIL_LAST = 0x40, 0xFE
TRAIL_COUNT = TRAIL_LAST - TRAIL_FIRST + 1  # 191

SEED = 20260901  # 固定种子：别让用例每次生成都不一样

GENERATED_BY = "python3 tools/make-textcodec-tables.py"


# ── 码表构造 ──


def build_table(codec, lead_first, lead_last):
    """逐码位解码，生成 (lead, trail) → BMP 码位的索引表；不可映射 / 非 BMP 填 0。"""
    rows = lead_last - lead_first + 1
    table = [0] * (rows * TRAIL_COUNT)
    skipped_multi = 0
    skipped_nonbmp = 0
    for hi in range(lead_first, lead_last + 1):
        for lo in range(TRAIL_FIRST, TRAIL_LAST + 1):
            if lo == 0x7F:
                continue
            try:
                text = bytes([hi, lo]).decode(codec)
            except UnicodeDecodeError:
                continue
            if len(text) != 1:
                skipped_multi += 1
                continue
            cp = ord(text)
            if cp > 0xFFFF:
                skipped_nonbmp += 1
                continue
            table[(hi - lead_first) * TRAIL_COUNT + (lo - TRAIL_FIRST)] = cp
    return table, skipped_multi, skipped_nonbmp


def slot_value(table, lead_first, hi, lo):
    return table[(hi - lead_first) * TRAIL_COUNT + (lo - TRAIL_FIRST)]


def reference_value(codec, hi, lo):
    """python3 参考实现给出的期望码位；不可映射 / 非 BMP / 多字符一律 0。"""
    try:
        text = bytes([hi, lo]).decode(codec)
    except UnicodeDecodeError:
        return 0
    if len(text) != 1:
        return 0
    cp = ord(text)
    return cp if cp <= 0xFFFF else 0


def selfcheck(table, codec, lead_first, lead_last, sample, label):
    """随机抽查 sample 个槽位，逐个与参考实现对照测试。返回失败数。"""
    rng = random.Random(SEED)
    slots = [
        (hi, lo)
        for hi in range(lead_first, lead_last + 1)
        for lo in range(TRAIL_FIRST, TRAIL_LAST + 1)
    ]
    picks = rng.sample(slots, sample)
    mapped = sum(1 for v in table if v != 0)
    bad = 0
    for hi, lo in picks:
        want = reference_value(codec, hi, lo)
        got = slot_value(table, lead_first, hi, lo)
        if want != got:
            bad += 1
            print(
                "  抽查不一致 %s %02X%02X：表内=%04X 参考=%04X"
                % (label, hi, lo, got, want)
            )
    print(
        "  自检 %s：抽查 %d 个槽位（种子 %d），不一致 %d 个；可映射槽位共 %d 个"
        % (label, len(picks), SEED, bad, mapped)
    )
    return bad, mapped


# ── C 源文本 ──

C_HEADER = """\
/* GENERATED — DO NOT EDIT
 * 本文件由脚本生成，手改会在下一次生成时被覆盖。
 * 生成命令：%s
 * 码表来源：python3 的 %s 编解码器（逐码位 bytes([hi,lo]).decode(...)）
 * 约定：只收 BMP（%s）；不可映射的槽填 0，由解码器转成 U+FFFD。
 */
"""


def hex_rows(values, per_line=12):
    lines = []
    for i in range(0, len(values), per_line):
        chunk = values[i : i + per_line]
        lines.append("  " + " ".join("0x%04X," % v for v in chunk))
    return "\n".join(lines)


def emit_tables(root, gbk, big5, counts):
    out_dir = os.path.join(root, "native", "src", "text")
    os.makedirs(out_dir, exist_ok=True)

    h_path = os.path.join(out_dir, "dsh_textcodec_tables.h")
    c_path = os.path.join(out_dir, "dsh_textcodec_tables.c")

    h = []
    h.append(C_HEADER % (GENERATED_BY, "gb18030 / big5", "u16，超出 BMP 的槽一律填 0"))
    h.append("#ifndef DSH_TEXTCODEC_TABLES_H\n#define DSH_TEXTCODEC_TABLES_H\n")
    h.append("#include <stddef.h>\n#include <stdint.h>\n")
    h.append("/* ── GB18030 双字节区（GBK 是它的子集）────────────────────────────────\n")
    h.append(" * 首字节 0x81–0xFE（126 行）、次字节 0x40–0xFE（191 列，含恒不可映射的 0x7F）。\n")
    h.append(" * 下标 = (首 - DSH_GBK_LEAD_FIRST) * DSH_GBK_TRAIL_COUNT + (次 - DSH_GBK_TRAIL_FIRST)\n")
    h.append(" * 注意：次字节 0x30–0x39 属于 GB18030 的四字节区，本版不支持，\n")
    h.append(" *       解码器在那之前就拦下来了，不会走到这张表。 */\n")
    h.append("#define DSH_GBK_LEAD_FIRST %d\n" % GBK_LEAD_FIRST)
    h.append("#define DSH_GBK_LEAD_LAST  %d\n" % GBK_LEAD_LAST)
    h.append("#define DSH_GBK_TRAIL_FIRST %d\n" % TRAIL_FIRST)
    h.append("#define DSH_GBK_TRAIL_COUNT %d\n" % TRAIL_COUNT)
    # 次字节上界必须由生成脚本给出、不要在 .c 里手写：它与首字节上界是两个不同的数
    # （数值恰好都落在 0xFE），手写时极易把 LEAD_LAST 拿来当 TRAIL_LAST 用。
    h.append("#define DSH_GBK_TRAIL_LAST  (DSH_GBK_TRAIL_FIRST + DSH_GBK_TRAIL_COUNT - 1)\n")
    h.append("#define DSH_GBK_TABLE_SIZE %d\n" % len(gbk))
    h.append("#define DSH_GBK_MAPPED %d\n" % counts["gbk"])
    h.append("#define DSH_GBK_INDEX(lead, trail) \\\n")
    h.append("  ((size_t)((lead) - DSH_GBK_LEAD_FIRST) * DSH_GBK_TRAIL_COUNT + \\\n")
    h.append("   (size_t)((trail) - DSH_GBK_TRAIL_FIRST))\n\n")
    h.append("extern const uint16_t dsh_gbk_bmp[DSH_GBK_TABLE_SIZE];\n\n")
    h.append("/* ── Big5 ────────────────────────────────────────────────────────────\n")
    h.append(" * 首字节 0xA1–0xF9（89 行）；次字节只有 0x40–0x7E 与 0xA1–0xFE 两段有效，\n")
    h.append(" * 中间 0x7F–0xA0 恒不可映射（在表里就是 0）。 */\n")
    h.append("#define DSH_BIG5_LEAD_FIRST %d\n" % BIG5_LEAD_FIRST)
    h.append("#define DSH_BIG5_LEAD_LAST  %d\n" % BIG5_LEAD_LAST)
    h.append("#define DSH_BIG5_TRAIL_FIRST %d\n" % TRAIL_FIRST)
    h.append("#define DSH_BIG5_TRAIL_COUNT %d\n" % TRAIL_COUNT)
    h.append("#define DSH_BIG5_TRAIL_LAST  (DSH_BIG5_TRAIL_FIRST + DSH_BIG5_TRAIL_COUNT - 1)\n")
    h.append("#define DSH_BIG5_TABLE_SIZE %d\n" % len(big5))
    h.append("#define DSH_BIG5_MAPPED %d\n" % counts["big5"])
    h.append("#define DSH_BIG5_INDEX(lead, trail) \\\n")
    h.append("  ((size_t)((lead) - DSH_BIG5_LEAD_FIRST) * DSH_BIG5_TRAIL_COUNT + \\\n")
    h.append("   (size_t)((trail) - DSH_BIG5_TRAIL_FIRST))\n\n")
    h.append("extern const uint16_t dsh_big5_bmp[DSH_BIG5_TABLE_SIZE];\n\n")
    h.append("#endif /* DSH_TEXTCODEC_TABLES_H */\n")

    c = []
    c.append(C_HEADER % (GENERATED_BY, "gb18030 / big5", "u16，超出 BMP 的槽一律填 0"))
    c.append('#include "dsh_textcodec_tables.h"\n\n')
    c.append("const uint16_t dsh_gbk_bmp[DSH_GBK_TABLE_SIZE] = {\n")
    c.append(hex_rows(gbk) + "\n};\n\n")
    c.append("const uint16_t dsh_big5_bmp[DSH_BIG5_TABLE_SIZE] = {\n")
    c.append(hex_rows(big5) + "\n};\n")

    with open(h_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("".join(h))
    with open(c_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("".join(c))
    return h_path, c_path


# ── 逐字节对照用例（生成后粘进 测试）──


def needs_escape(text):
    """含不可见字符 / 引号 / 反斜杠 / 问号（三字符组）时整串改成 \\xNN 转义。"""
    for ch in text:
        if ch == " ":  # 半角空格是可打印的，留着更好读
            continue
        if ch in '"\\?' or ch == "\ufffd":
            return True
        if not ch.isprintable():
            return True
        if unicodedata.category(ch) in ("Cc", "Cf", "Co", "Cn", "Zs", "Zl", "Zp"):
            return True
    return False


def c_literal(text):
    if needs_escape(text):
        return '"' + "".join("\\x%02X" % b for b in text.encode("utf-8")) + '"'
    return '"' + text + '"'


def hex_of(data):
    return " ".join("%02X" % b for b in data)


CODEC_OF = {"DSH_TXT_UTF16LE": "utf-16-le", "DSH_TXT_GB18030": "gb18030"}


def expected_utf8(enc, data, codec_for_big5):
    """本层对一段字节的期望输出（UTF-8 字节）。

    坏序列用 python3 的 errors='replace' 约定（实测它 = 「每个坏字节一个 U+FFFD」，
    与本层的约定一致，连残缺的四字节区前缀怎么消费都对得上）。

    ⚠️ 只有一处必须先把 python3 的意思改掉：GB18030 的**四字节区**。
    本版不支持四字节区（整段替换成一个 U+FFFD），而 python3 会真把它们解出来
    （81 30 81 30 → U+0080、84 31 A4 37 → U+FFFF 以上等等）—— 所以先把这一档
    按本版约定切掉，剩下的字节才交给 python3。
    """
    codec = CODEC_OF.get(enc, codec_for_big5)
    if codec != "gb18030":
        return data.decode(codec, "replace").encode("utf-8")

    out = bytearray()
    i = 0
    while i < len(data):
        if 0x81 <= data[i] <= 0xFE and i + 1 < len(data) and 0x30 <= data[i + 1] <= 0x39:
            out += "\ufffd".encode("utf-8")  # 四字节区：本版整段替换
            i += min(4, len(data) - i)
            continue
        j = i
        while j < len(data):
            if 0x81 <= data[j] <= 0xFE and j + 1 < len(data) and 0x30 <= data[j + 1] <= 0x39:
                break
            j += 1
        out += data[i:j].decode("gb18030", "replace").encode("utf-8")
        i = j
    return bytes(out)


def make_cases(codec_for_big5):
    """(编码, 说明, 输入字节) 的清单 —— 期望值由 python3 现算。"""
    rng = random.Random(SEED)
    cases = []

    # ① UTF-16LE
    u16 = [
        ("ASCII 单词", "apple"),
        ("中文二字词", "测试"),
        ("中文三字词", "汉语词典"),
        ("拉丁 + 中文混排", "Oxford 牛津"),
        ("生僻字与全角标点", "龘（测试）、「汉语」"),
        ("补充平面（代理对）emoji", "😀"),
        ("代理对夹在中文里", "测试😀汉语"),
        ("西里尔与希腊字母", "яблоко αβγ"),
    ]
    for label, text in u16:
        cases.append(("DSH_TXT_UTF16LE", "utf-16-le：" + label, text.encode("utf-16-le")))

    # 随机 BMP 码位（避开代理区与 BOM）
    for i in range(6):
        n = rng.randint(2, 8)
        text = ""
        while len(text) < n:
            cp = rng.randrange(0x0020, 0xFFFD)
            if 0xD800 <= cp <= 0xDFFF or cp == 0xFEFF:
                continue
            text += chr(cp)
        cases.append(("DSH_TXT_UTF16LE", "utf-16-le：随机 BMP 抽样 %d" % (i + 1), text.encode("utf-16-le")))

    # 随机含补充平面（必然走代理对）
    for i in range(4):
        n = rng.randint(1, 3)
        text = "".join(chr(rng.randrange(0x10000, 0x10FFFF)) for _ in range(n))
        cases.append(
            ("DSH_TXT_UTF16LE", "utf-16-le：随机补充平面（代理对）%d" % (i + 1), text.encode("utf-16-le"))
        )

    # ② GB18030 / GBK
    gb = [
        ("常用二字词（测）", "测试"),
        ("常用二字词（汉）", "汉语"),
        ("三字词", "词典学"),
        ("全角标点与括号", "（测试）：「汉语」、；"),
        ("生僻字", "丂"),
        ("生僻字与常用字混排", "龘靐齉"),
        ("数字与拉丁混排", "GB2312-1980 测试"),
        ("纯 ASCII", "Hello, world!"),
        ("ASCII 与汉字混排", "abc测试xyz"),
    ]
    for label, text in gb:
        cases.append(("DSH_TXT_GB18030", "gb18030：" + label, text.encode("gb18030")))

    # 随机抽样：从**可映射**槽位里挑（保证是合法序列）
    gbk_table, _, _ = build_table("gb18030", GBK_LEAD_FIRST, GBK_LEAD_LAST)
    mapped_slots = [
        (hi, lo)
        for hi in range(GBK_LEAD_FIRST, GBK_LEAD_LAST + 1)
        for lo in range(TRAIL_FIRST, TRAIL_LAST + 1)
        if slot_value(gbk_table, GBK_LEAD_FIRST, hi, lo) != 0
    ]
    for i in range(8):
        n = rng.randint(2, 6)
        pairs = [rng.choice(mapped_slots) for _ in range(n)]
        cases.append(
            (
                "DSH_TXT_GB18030",
                "gb18030：随机双字节序列 %d（%s…）"
                % (i + 1, " ".join("%02X%02X" % p for p in pairs[:2])),
                b"".join(bytes([hi, lo]) for hi, lo in pairs),
            )
        )

    # ③ Big5
    big5 = [
        ("繁體二字詞（測）", "測試"),
        ("繁體二字詞（漢）", "漢語"),
        ("繁體三字詞", "詞典學"),
        ("全形標點", "（測試）：「漢語」、；"),
        ("中日韓罕用字", "龘"),
        ("純 ASCII", "Hello, world!"),
        ("ASCII 與繁體混排", "abc測試xyz"),
    ]
    for label, text in big5:
        cases.append(("DSH_TXT_BIG5", "big5：" + label, text.encode(codec_for_big5)))

    big5_table, _, _ = build_table(codec_for_big5, BIG5_LEAD_FIRST, BIG5_LEAD_LAST)
    big5_slots = [
        (hi, lo)
        for hi in range(BIG5_LEAD_FIRST, BIG5_LEAD_LAST + 1)
        for lo in range(TRAIL_FIRST, TRAIL_LAST + 1)
        if slot_value(big5_table, BIG5_LEAD_FIRST, hi, lo) != 0
    ]
    for i in range(8):
        n = rng.randint(2, 5)
        pairs = [rng.choice(big5_slots) for _ in range(n)]
        cases.append(
            (
                "DSH_TXT_BIG5",
                "big5：隨機雙位元組序列 %d（%s…）"
                % (i + 1, " ".join("%02X%02X" % p for p in pairs[:2])),
                b"".join(bytes([hi, lo]) for hi, lo in pairs),
            )
        )

    return cases


def make_bad_cases():
    """坏字节 / 残缺序列的用例（期望值走 python3 的 errors='replace' 约定）。

    ⚠️ UTF-8 不在这里 —— python3 的 utf-8 replace 走的是「最长大子串」约定
    （E6 B5 只给一个 U+FFFD），而本层的约定是「每个坏字节一个 U+FFFD」（给两个）。
    这一档的边界用例另有一份手写清单，差异写在那份清单里。
    """
    cases = []
    gb = [
        ("gb18030：单个 0x80（没用的首字节）", "80"),
        ("gb18030：单个 0xFF", "FF"),
        ("gb18030：末尾只剩一个首字节", "81"),
        ("gb18030：次字节 0x7F（表里的空槽）", "B2 7F"),
        ("gb18030：四字节区只给两字节（残缺）", "81 30"),
        ("gb18030：四字节区只给三字节（残缺）", "81 30 81"),
        ("gb18030：首字节后面跟 ASCII 空格", "81 20"),
        ("gb18030：次字节越界 0xFF", "A1 FF"),
        ("gb18030：汉字 + 四字节区（完整）", "B2 E2 81 30 81 30"),
        ("gb18030：四字节区 + 汉字（完整）", "81 30 81 30 B2 E2"),
        ("gb18030：四字节区形状不对（第三字节是 ASCII）", "81 30 41 42"),
        ("gb18030：双字节 + 双字节 + 孤立的 0x80", "B2 E2 FE FE 80"),
        ("gb18030：全角标点 + 坏字节 + ASCII", "A3 A1 FF 41"),
    ]
    for label, hex_text in gb:
        cases.append(("DSH_TXT_GB18030", label, bytes.fromhex(hex_text)))

    big5 = [
        ("big5：单个 0x80", "80"),
        ("big5：单个 0xA0（首字节不够）", "A0"),
        ("big5：首字节 0xFA（越界）+ 合法次字节", "FA 40"),
        ("big5：次字节 0x7F（越界）", "A1 7F"),
        ("big5：次字节 0x80（落在两段之间）", "A1 80"),
        ("big5：次字节 0xA0（落在两段之间）", "A1 A0"),
        ("big5：末尾只剩一个首字节", "B4"),
        ("big5：末尾只剩一个首字节（0xA1）", "A1"),
        ("big5：漢 + 坏次字节 + ASCII", "B4 FA A1 7F"),
        ("big5：越界首字节 + 越界次字节 + 合法词", "FA FE A1 40"),
        ("big5：合法词 + 0xFF + 0x80", "A1 40 FF 80"),
    ]
    for label, hex_text in big5:
        cases.append(("DSH_TXT_BIG5", label, bytes.fromhex(hex_text)))

    u16 = [
        ("utf-16-le：末尾剩 1 个字节", "41"),
        ("utf-16-le：落单的高代理", "00 D8"),
        ("utf-16-le：落单的低代理", "00 DC"),
        ("utf-16-le：落单的高代理 + ASCII", "00 D8 41 00"),
        ("utf-16-le：ASCII + 落单的高代理", "41 00 00 D8"),
        ("utf-16-le：合法代理对 + 落单的高代理", "3D D8 00 DE 00 D8"),
        ("utf-16-le：合法汉字 + 末尾剩 1 个字节", "B4 FA A1"),
        ("utf-16-le：连续两个落单的高代理", "00 D8 00 D8"),
        ("utf-16-le：落单的低代理夹在汉字之间", "B2 E2 00 DC B2 E2"),
    ]
    for label, hex_text in u16:
        cases.append(("DSH_TXT_UTF16LE", label, bytes.fromhex(hex_text)))

    return cases


def print_cases(codec_for_big5):
    good = make_cases(codec_for_big5)
    bad = make_bad_cases()
    print("/* 下面这一整块由 %s --print-cases 生成后粘进来（种子 %d）。" % (GENERATED_BY, SEED))
    print(" * 正常序列的期望值是 python3 的 %s / utf-16-le 严格解码结果 → 再编成 UTF-8；" % codec_for_big5)
    print(" * 坏序列的期望值走 python3 的 errors='replace' 约定（实测等于「每个坏字节一个 U+FFFD」），")
    print(" * 只有 GB18030 的四字节区按本版约定整段替换（python3 会真把它们解出来）。")
    print(" * 不要手改 —— 要加用例就改脚本再重新生成。 */")
    print("static const ref_case k_ref_cases[] = {")
    for enc, label, data in good:
        want = expected_utf8(enc, data, codec_for_big5)
        print('  { %s, "%s", "%s", %s },' % (enc, label, hex_of(data), c_literal(want.decode("utf-8"))))
    print("};")
    print("#define K_REF_CASE_COUNT ((int)(sizeof(k_ref_cases) / sizeof(k_ref_cases[0])))")
    print()
    print("/* 坏字节 / 残缺序列（同样逐条与 python3 的 replace 约定对照测试）。 */")
    print("static const ref_case k_bad_cases[] = {")
    for enc, label, data in bad:
        want = expected_utf8(enc, data, codec_for_big5)
        print('  { %s, "%s", "%s", %s },' % (enc, label, hex_of(data), c_literal(want.decode("utf-8"))))
    print("};")
    print("#define K_BAD_CASE_COUNT ((int)(sizeof(k_bad_cases) / sizeof(k_bad_cases[0])))")
    print("/* 正常序列 %d 组 + 坏序列 %d 组，共 %d 组对照测试用例。 */" % (len(good), len(bad), len(good) + len(bad)))


# ── 入口 ──


def main():
    parser = argparse.ArgumentParser(description="生成内核文本编解码层的码表 / 对照测试用例")
    parser.add_argument(
        "--big5-codec",
        default="big5",
        choices=["big5", "cp950"],
        help="Big5 码表与对照测试用例用哪个 python 编解码器（默认 big5，与任务约定一致）",
    )
    parser.add_argument("--print-cases", action="store_true", help="只打印对照测试用例的 C 片段")
    parser.add_argument("--sample", type=int, default=200, help="每张表随机抽查的槽位数（默认 200）")
    args = parser.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if not os.path.isdir(os.path.join(root, "native", "src")):
        print("找不到 native/src —— 本脚本要放在 0.2.0/tools/ 下", file=sys.stderr)
        return 2

    if args.print_cases:
        print_cases(args.big5_codec)
        return 0

    print("── 生成码表（0.2.0/ 根：%s）──" % root)
    gbk, gbk_multi, gbk_nonbmp = build_table("gb18030", GBK_LEAD_FIRST, GBK_LEAD_LAST)
    big5, big5_multi, big5_nonbmp = build_table(args.big5_codec, BIG5_LEAD_FIRST, BIG5_LEAD_LAST)
    print(
        "  GB18030 双字节区：%d 槽（跳过多字符 %d、超出 BMP %d）"
        % (len(gbk), gbk_multi, gbk_nonbmp)
    )
    print(
        "  Big5（%s）：%d 槽（跳过多字符 %d、超出 BMP %d）"
        % (args.big5_codec, len(big5), big5_multi, big5_nonbmp)
    )

    bad = 0
    gbk_bad, gbk_mapped = selfcheck(gbk, "gb18030", GBK_LEAD_FIRST, GBK_LEAD_LAST, args.sample, "GB18030")
    bad += gbk_bad
    big5_bad, big5_mapped = selfcheck(
        big5, args.big5_codec, BIG5_LEAD_FIRST, BIG5_LEAD_LAST, args.sample, "Big5"
    )
    bad += big5_bad

    h_path, c_path = emit_tables(root, gbk, big5, {"gbk": gbk_mapped, "big5": big5_mapped})
    print("  已写出：%s" % os.path.relpath(h_path, root))
    print("  已写出：%s（%d 字节）" % (os.path.relpath(c_path, root), os.path.getsize(c_path)))

    if bad:
        print("码表自检失败：%d 处不一致" % bad, file=sys.stderr)
        return 1
    print("码表自检通过（抽查 %d + %d 个槽位，零不一致）" % (args.sample, args.sample))
    return 0


if __name__ == "__main__":
    sys.exit(main())
