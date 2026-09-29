#!/bin/sh
# =============================================================================
# 标准答案文件对照测试 · **跑当前 C 实现**（默认）/ **再生成基线**（--regen）
#
#   sh tools/run-golden.sh <仓库的 WSL 路径> [清单文件]     默认：只编、只跑 C 侧
#   sh tools/run-golden.sh <仓库的 WSL 路径> --regen        特权：编参考实现、重写基线
#
# ── 默认路径（日常回归、交付前都用它）────────────────────────────────────────
#   ① 编 C 侧诊断脚本 → tools/golden/golden-dump-c.c（链 native/ 源码）
#   ② 跑它              → tools/golden/out/golden-c.json
#   **完全不碰 .NET**：不编 C#、不跑参考实现、不需要那台机器上装过 dotnet。
#   比对由 tools/check-golden.sh 紧接着做：
#     python3 tools/golden/compare-golden.py \
#       tools/golden/out/golden-c.json tools/golden/baseline/golden-baseline.json
#   判决标准是"逐字节 + 一张已确认差异的白名单"，白名单在 compare-golden.py 顶部。
#
# ── --regen：**特权操作**，别顺手跑 ──────────────────────────────────────────
#   ① 编参考实现（tools/golden/GoldenDump.csproj，链仓库内**只读副本**
#      reference/0.1.3-parser/src/Dictionary/）→ bin/Release/net48/GoldenDump.exe
#   ② 跑它 → tools/golden/out/golden-cs.json
#   ③ 覆盖 tools/golden/baseline/golden-baseline.json，并重写
#      tools/golden/baseline/provenance.md（来源 / 参考实现逐文件 SHA256 / 清单哈希 / 日期 / 命令）
#
# ⚠️ **为什么基线只能由 C# 侧产出、绝不能由 C 侧产出**（用户 2026-09 定的约定）：
#   基线是"标准答案"。让**正在被测的 C 实现**自己产出标准答案，就是自己出题自己判卷 ——
#   它改错了，基线跟着错，而这道 gate 会**继续通过**。所以 --regen 里**一个字节都不读**
#   C 侧的输出；C 侧只在默认路径上跟基线比。
#
# 这道 gate 为什么是整个仓库最要紧的：内核自己的测试只证明"它自己前后一致"，
# **不证明它与 0.1.3 一致**。重写最典型的死法就是"99% 一样、1% 悄悄不一样"，
# 而这一仓库历史上最贵的几个 bug（@@@LINK 落点、书名 vs 文件名、归一化索引）
# 恰好都属于"读代码看不出来、只有跟参考实现对照测试才发现"的那一类。
#
# 两边怎么产出：
#   C 侧   → tools/golden/golden-dump-c.c（链内核源码）
#   C# 侧  → tools/golden/golden-dump.cs（链仓库内的参考实现副本，**只在 --regen 时**）
# 两边都输出同一形状的规范化 JSON。
#
# ⚠️ 测试用词典清单只在测试用词典**数量与顺序**上：顺序必须一致，否则 diff 全是噪声。
set -e

usage() {
  echo "用法: sh tools/run-golden.sh <仓库的 WSL 路径> [清单文件] [--regen]" >&2
  exit 2
}

[ -n "$1" ] || usage
ROOT="$1"
shift

REGEN=0
LIST=""
for arg in "$@"; do
  case "$arg" in
    --regen) REGEN=1 ;;
    -*) echo "看不懂的参数：$arg" >&2; usage ;;
    *) LIST="$arg" ;;
  esac
done
LIST="${LIST:-$ROOT/tools/golden/fixtures.txt}"

NATIVE="$ROOT/native"
OUT="$ROOT/tools/golden/out"
BASELINE_DIR="$ROOT/tools/golden/baseline"
BASELINE="$BASELINE_DIR/golden-baseline.json"
PROVENANCE="$BASELINE_DIR/provenance.md"
REF_MANIFEST="$ROOT/reference/0.1.3-parser/SHA256.txt"
mkdir -p "$OUT"

if [ ! -f "$LIST" ]; then
  echo "找不到测试用词典清单：$LIST" >&2
  echo "（它应当随仓库一起提交；清单的规矩见它自己的注释）" >&2
  exit 2
fi

# ── 编 C 侧诊断脚本（不启动任何界面；只链解析器那几块）─────────────────────────
echo "── 编 C 侧诊断脚本 ──"
cc -std=c11 -Wall -Wextra -Werror -pedantic -O1 -I"$NATIVE/include" -I"$NATIVE/src" \
   -o "$OUT/golden-dump-c" "$ROOT/tools/golden/golden-dump-c.c" \
   "$NATIVE/src/core.c" "$NATIVE/src/mem_registry.c" "$NATIVE/src/json_writer.c" \
   "$NATIVE/src/compress/dsh_inflate.c" "$NATIVE/src/compress/dsh_lzo1x.c" \
   "$NATIVE/src/crypto/dsh_ripemd128.c" \
   "$NATIVE/src/dict/dsh_mdx.c" "$NATIVE/src/dict/dsh_dict.c" \
   "$NATIVE/src/text/dsh_textcodec.c" "$NATIVE/src/text/dsh_textcodec_tables.c" \
   "$NATIVE/src/platform/dsh_file.c" "$NATIVE/src/platform/dsh_mapfile.c" "$NATIVE/src/platform/dsh_sleep.c" \
   -pthread

# 清单里的路径是**相对仓库根**的斜杠形式（这份清单两边共用，见它的注释）。
#
# ⚠️ **两边要的路径形式不一样，所以各给一份**（这一版为此红了两轮）：
#   · **C# 侧**是 Windows 上的 .NET：它把 WSL 的 `/mnt/c/...` 当成"当前盘根下的
#     mnt/c/..."，报「未能找到路径 C:\mnt\c\... 的一部分」；要 `C:/...`。
#   · **C 侧**是 WSL 里的 ELF：它不认 `C:/...`（那是 Windows 的写法），要 `/mnt/c/...`。
#   于是 `to_win_path` 只给 C# 侧用（默认路径上根本用不到），C 侧拿到的是 WSL 绝对路径。
to_win_path() {
  case "$1" in
    /mnt/*)
      drive=$(printf '%s' "$1" | cut -c6 | tr 'a-z' 'A-Z')
      rest=$(printf '%s' "$1" | cut -c7-)
      printf '%s:/%s' "$drive" "$rest"
      ;;
    *) printf '%s' "$1" ;;
  esac
}

FILES_CS=""
COUNT=0
# ⚠️ `set --` 必须先清空：在脚本里 `set -- "$@" ...` 会把**脚本自身的路径**（$0）
#    也当成一个位置参数带进去 —— 于是程序收到的第一个"词典"是脚本自己，
#    报"文件太小或读不到头部长度"。第一次就是这么错的。
set --
while IFS= read -r line; do
  line=$(printf '%s' "$line" | tr -d '\r')
  case "$line" in ''|'#'*) continue ;; esac
  dir=$(dirname "$line")
  base=$(basename "$line")
  absdir=$(cd "$ROOT/$dir" && pwd)
  # ⚠️ **用位置参数传路径、不要拼成字符串再用 $VAR 展开**：
  # 仓库路径与测试用词典路径**可能有空格与中文**（以前那份工作目录里就有「查词软件」三个字），
  # $VAR 展开会按空格拆成多个参数，于是程序拿到半截路径、报"打不开文件"——
  # 而错误信息看起来像测试用词典坏了。
  set -- "$@" "$absdir/$base"
  FILES_CS="$FILES_CS $(to_win_path "$absdir/$base")"
  COUNT=$((COUNT + 1))
done < "$LIST"

echo "── 对照测试 $COUNT 本测试用词典 ──"

echo "── 跑 C 侧（当前实现）──"
"$OUT/golden-dump-c" "$@" > "$OUT/golden-c.json"
echo "  $OUT/golden-c.json（$(stat -c %s "$OUT/golden-c.json") 字节）"

if [ "$REGEN" != 1 ]; then
  echo
  echo "── C 侧跑完了；接下来跟**冻结基线**比 ──"
  echo "  python3 tools/golden/compare-golden.py $OUT/golden-c.json $BASELINE"
  exit 0
fi

# =============================================================================
# --regen：再生成基线（特权操作。**它不读上面那份 C 侧输出**）
# =============================================================================
echo
echo "╔══════════════════════════════════════════════════════════════════════╗"
echo "║ ⚠️  --regen：**再生成标准答案文件基线** —— 这是特权操作             ║"
echo "║     它会覆盖 tools/golden/baseline/golden-baseline.json（标准答案）  ║"
echo "║     只有「参考实现确实变了」（换了/改了 reference/0.1.3-parser/）时  ║"
echo "║     才该跑；基线只由 C# 参考实现产出，C 侧的输出一个字节都不读       ║"
echo "╚══════════════════════════════════════════════════════════════════════╝"

# ── ① 编参考实现（Windows 的 dotnet SDK；WSL 里一般没有 SDK，走 interop 调 dotnet.exe）──
find_dotnet() {
  if [ -n "$DOTNET" ]; then printf '%s' "$DOTNET"; return 0; fi
  c=$(command -v dotnet 2>/dev/null || true)
  if [ -n "$c" ] && "$c" --list-sdks >/dev/null 2>&1; then printf '%s' "$c"; return 0; fi
  # 本机的 SDK 装在 Windows 用户目录里；用通配符匹配，免得把用户名写死进脚本。
  for c in /mnt/c/Users/*/.dotnet/dotnet.exe; do
    [ -x "$c" ] || continue
    if "$c" --list-sdks >/dev/null 2>&1; then printf '%s' "$c"; return 0; fi
  done
  return 1
}

DOTNET_EXE=$(find_dotnet || true)
if [ -z "$DOTNET_EXE" ]; then
  echo "找不到能用的 dotnet SDK（试过 \$DOTNET、PATH、/mnt/c/Users/*/.dotnet/dotnet.exe）。" >&2
  echo "参考实现编不出来，基线**不会**被改动。可选：" >&2
  echo "  · 在 Windows 上先跑 powershell -File tools/make-golden.ps1（编好 exe 再回来跑 --regen）；" >&2
  echo "  · 或者给它一个显式路径：DOTNET=/path/to/dotnet sh tools/run-golden.sh <root> --regen" >&2
  exit 3
fi
DOTNET_VER=$("$DOTNET_EXE" --version 2>&1 | tr -d '\r' || true)
echo "── ① 编参考实现（$DOTNET_EXE，net48）──"
WIN_ROOT=$(to_win_path "$ROOT")
# ⚠️ 项目路径必须是 **Windows 形式**：dotnet.exe 是 Windows 程序，
#    给它 /mnt/c/... 它会当成"当前盘根下的 mnt/c/..."。
"$DOTNET_EXE" build "$WIN_ROOT/tools/golden/GoldenDump.csproj" -c Release -v quiet --nologo
EXE="$ROOT/tools/golden/bin/Release/net48/GoldenDump.exe"
[ -f "$EXE" ] || { echo "没编出参考实现探测器：$EXE" >&2; exit 3; }
echo "  ✅ $EXE"

# ── ② 跑参考实现 → 新基线 ────────────────────────────────────────────────────
echo "── ② 跑参考实现（$COUNT 本测试用词典）──"
# shellcheck disable=SC2086
"$EXE" $FILES_CS > "$OUT/golden-cs.json"
NEW_SIZE=$(stat -c %s "$OUT/golden-cs.json")
echo "  $OUT/golden-cs.json（$NEW_SIZE 字节）"

# 自检：新基线必须能读、且条数与清单一致 —— 否则不覆盖旧基线（宁可红，不留半份标准答案）
python3 - "$OUT/golden-cs.json" "$COUNT" <<'PY'
import json, sys
data = json.loads(open(sys.argv[1], 'rb').read().decode('utf-8', 'replace'))
want = int(sys.argv[2])
got = len(data.get('files', []))
if got != want:
    print('  ✗ 新基线里 %d 本测试用词典，清单里是 %d 本 —— 不覆盖旧基线' % (got, want))
    sys.exit(1)
print('  ✅ 新基线可读：%d 本测试用词典' % got)
PY

mkdir -p "$BASELINE_DIR"
# ── ③ 覆盖基线（先把"与上一份是否相同"记下来 —— 相同说明参考实现没变）──────────
if [ -f "$BASELINE" ]; then
  if cmp -s "$BASELINE" "$OUT/golden-cs.json"; then
    SAME="与上一份基线**逐字节相同**（参考实现与清单都没变过；这份 re-gen 是空跑）"
  else
    OLD_SIZE=$(stat -c %s "$BASELINE")
    SAME="与上一份**不同**（旧 $OLD_SIZE 字节 → 新 $NEW_SIZE 字节）—— 参考实现或清单动过，别放过这条"
    echo "  ⚠️ 新基线与旧基线不同（$OLD_SIZE → $NEW_SIZE 字节）："
    echo "     看差异：python3 tools/golden/compare-golden.py $OUT/golden-cs.json $BASELINE"
  fi
else
  SAME="本仓库此前没有基线：这一份是**首次**冻结（来源见下）"
fi
cp "$OUT/golden-cs.json" "$BASELINE"
echo "── ③ 基线已更新 ──"
echo "  $BASELINE（$(stat -c %s "$BASELINE") 字节，SHA256 $(sha256sum "$BASELINE" | cut -c1-16)…）"

# ── ④ 来源记录 ──────────────────────────────────────────────────────────────
# ⚠️ 这一份是**机械生成**的：改了它没意义，下次 --regen 会整份覆盖。
FIX_SHA=$(sha256sum "$LIST" | cut -d' ' -f1)
REF_SHA=$(sha256sum "$REF_MANIFEST" 2>/dev/null | cut -d' ' -f1)
[ -n "$REF_SHA" ] || REF_SHA='（缺清单）'
BASE_SHA=$(sha256sum "$BASELINE" | cut -d' ' -f1)
EXE_SHA=$(sha256sum "$EXE" | cut -d' ' -f1)
GIT_HEAD=$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo '（不是 git 工作区）')
{
  echo '# 标准答案文件基线的来源记录'
  echo
  echo '> ⚠️ **本文件由 `tools/run-golden.sh --regen` 机械生成，别手改** —— 下次 --regen 会整份覆盖。'
  echo '> 基线的规矩、白名单的含义、以及"为什么要与 C 侧分开"见同目录 `README.md`。'
  echo
  echo '"标准答案"如果没有来源记录就等于没有标准：谁也说不清它出自哪一份参考实现、'
  echo '哪一份清单、哪一天。所以下面这些字段是**强制**记下来的。'
  echo
  echo '## 一、这一份是怎么生成的'
  echo
  echo '| 项 | 值 |'
  echo '| --- | --- |'
  echo "| 生成时间（UTC） | $(date -u '+%Y-%m-%d %H:%M:%S') |"
  echo "| 生成的命令 | \`sh tools/run-golden.sh <仓库的 WSL 路径> --regen\`（特权操作，显式调用） |"
  echo "| 产出它的程序 | \`tools/golden/bin/Release/net48/GoldenDump.exe\`（SHA256 \`$EXE_SHA\`） |"
  echo "| 参考实现 | \`reference/0.1.3-parser/src/Dictionary/\`（0.1.3 的冻结副本，**只读**） |"
  echo "| 参考实现清单 | \`reference/0.1.3-parser/SHA256.txt\`（SHA256 \`$REF_SHA\`） |"
  echo "| 测试用词典清单 | \`tools/golden/fixtures.txt\`（SHA256 \`$FIX_SHA\`，共 $COUNT 条） |"
  echo "| 基线文件 | \`golden-baseline.json\`（$NEW_SIZE 字节，SHA256 \`$BASE_SHA\`） |"
  echo "| 与上一份基线 | $SAME |"
  # ⚠️ 只记**版本**，不记 `dotnet` 装在哪：这是一份要进版本库的来源记录，
  #    而 "SDK 装在哪台机器的哪个家目录下" 既复现不了任何东西，又把**操作者的用户名**写进了公开仓库
  #    （`$DOTNET_EXE` 在 WSL 里长这样：`/mnt/c/Users/<用户名>/.dotnet/dotnet.exe`）。
  echo "| dotnet SDK | $DOTNET_VER |"
  echo "| git HEAD | \`$GIT_HEAD\` |"
  echo
  echo '## 二、文件构成'
  echo
  echo '```text'
  echo 'golden-baseline.json   标准答案本体（一份 JSON 装全部测试用词典，按 fixtures.txt 的**顺序**）'
  echo 'provenance.md          本文件：这一份基线是怎么来的'
  echo 'README.md              基线的规矩（人工维护，不被 --regen 覆盖）'
  echo '```'
  echo
  echo '## 三、参考实现的逐文件 SHA256'
  echo
  echo '基线是这些字节产出的 —— 它们一变，基线就不再代表任何东西（哪怕比对仍然是绿的）。'
  echo
  echo '```text'
  grep -v '^#' "$REF_MANIFEST" | grep -v '^[[:space:]]*$' | sed 's/^/  /'
  echo '```'
  echo
  echo '## 四、用这份基线比什么'
  echo
  echo '```sh'
  echo '# 默认路径（不需要 .NET）：编 C 侧 → 跟这份基线比'
  echo 'sh tools/check-golden.sh <仓库的 WSL 路径>'
  echo '```'
  echo
  echo '判决标准是"逐字节 + 一张已确认差异的白名单"，白名单写在 `tools/golden/compare-golden.py` 顶部，'
  echo '并附了现场实测证据（谁对、谁错、为什么）。**白名单外的任何差异都是失败**。'
} > "$PROVENANCE"
echo "── ④ 来源记录已重写 ──"
echo "  $PROVENANCE"
echo
echo "下一步：sh tools/check-golden.sh $ROOT   （跑当前 C 实现，跟刚写下的基线比）"
