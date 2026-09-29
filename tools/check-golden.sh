#!/bin/sh
# =============================================================================
# 标准答案文件对照测试 —— **一条命令跑完整道 gate**（日常与交付前使用）：
#
#   sh tools/check-golden.sh <仓库的 WSL 路径>
#   sh tools/check-golden.sh <仓库的 WSL 路径> --regen     # 先重写基线，再顺手比一遍
#
# 默认路径**只跑当前 C 实现**，然后跟**冻结基线**逐字节比：
#   ① sh tools/run-golden.sh <root>      编 C 诊断脚本、跑它 → tools/golden/out/golden-c.json
#   ② python3 tools/golden/compare-golden.py <C 侧> tools/golden/baseline/golden-baseline.json
#
# **不需要 .NET**：不编 C#、不跑参考实现 —— 这正是"默认回归只依赖 C 侧"这条约定。
# 基线本身由参考实现（C#）产出、且**只在显式 --regen 时**才重写；它的来源记录见
# tools/golden/baseline/provenance.md，规矩见同目录 README.md。
#
# 退出码 = 比对那一步的退出码（差异不在白名单里就是非零）。
# =============================================================================
set -e

if [ -z "$1" ]; then
  echo "用法: sh tools/check-golden.sh <仓库的 WSL 路径> [清单文件] [--regen]" >&2
  exit 2
fi
ROOT="$1"
shift

REGEN=0
LIST=""
for arg in "$@"; do
  case "$arg" in
    --regen) REGEN=1 ;;
    -*) echo "看不懂的参数：$arg" >&2; exit 2 ;;
    *) LIST="$arg" ;;
  esac
done
LIST="${LIST:-$ROOT/tools/golden/fixtures.txt}"

BASELINE="$ROOT/tools/golden/baseline/golden-baseline.json"
OUT="$ROOT/tools/golden/out"

# 基线缺了就**明着退**，不要"什么都比不了却退出 0"——那正是这道 gate 最坏的失败模式。
if [ ! -f "$BASELINE" ] && [ "$REGEN" != 1 ]; then
  echo "找不到冻结基线：$BASELINE" >&2
  echo "（再生成它：sh tools/run-golden.sh <root> --regen —— 特权操作，要 dotnet SDK）" >&2
  exit 2
fi

if [ "$REGEN" = 1 ]; then
  sh "$ROOT/tools/run-golden.sh" "$ROOT" "$LIST" --regen
else
  sh "$ROOT/tools/run-golden.sh" "$ROOT" "$LIST"
fi

if [ ! -f "$BASELINE" ]; then
  echo "run-golden.sh 跑完了，但仍然没有基线：$BASELINE" >&2
  exit 2
fi

echo
echo "── 与冻结基线逐字节比（基线来源：tools/golden/baseline/provenance.md）──"
python3 "$ROOT/tools/golden/compare-golden.py" "$OUT/golden-c.json" "$BASELINE"
