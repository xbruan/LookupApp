"""算冻结测试用词典的 SHA-256（词典内容哈希 id）参照值。

用途：`native/tests/test_dict_id.c` 要拿**真测试用词典的实测 id** 与这个脚本的实测结果对照。
不手抄长散列值 —— 抄错一位就变成"测试在验证一个错的东西"。

    python3 tools/golden/dict-id-vectors.py
"""
import hashlib
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TESTDATA = os.path.join(ROOT, "testdata")

NAMES = ["test.mdx", "link.mdx", "titled.mdx", "kana.mdx", "tall.mdx", "audio.mdx",
         "v2-multiblock.mdx"]


def main() -> int:
    print("测试用词典       字节      SHA-256（= 词典 id）")
    for name in NAMES:
        path = os.path.join(TESTDATA, name)
        if not os.path.exists(path):
            print("%-20s 缺文件" % name)
            continue
        h = hashlib.sha256()
        size = 0
        with open(path, "rb") as fp:
            while True:
                chunk = fp.read(64 * 1024)
                if not chunk:
                    break
                h.update(chunk)
                size += len(chunk)
        print("%-20s %8d  %s" % (name, size, h.hexdigest()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
