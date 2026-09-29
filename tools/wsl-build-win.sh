#!/bin/sh
# 交叉编 Windows x64 的 DLL（工具链的来源顺序与装法见 tools/build-windows-dll.sh 与 native/README.md）
#
# ⚠️ 这个脚本**只做一件事：把构建脚本的退出码原样传出去**。
#
# 它原来是一行：
#     sh tools/build-windows-dll.sh "$(pwd)" Release 2>&1 | tail -n 12
# 而管道最后那个命令是 `tail` —— **构建失败它也返回 0**。于是"编不出来"在调用方看来是成功，
# 而 `set -e` 也救不了（它看的是整条管道的退出码，不是管道里第一个命令的）。
# 这正是本项目最忌讳的那一类失败：**不报错地失败**（谁会去怀疑一个退出 0 的构建脚本？）。
#
# 所以改成"**先落盘 → 取退出码 → 再决定怎么展示**"：成功时照旧只给一小截够看的输出，
# 失败时**完整打出来**（失败的输出不许截断 —— 截断正是让人看不出错在哪的原因）。
set -e
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
LOG="${TMPDIR:-/tmp}/dsh-win-dll-build.log"

rc=0
sh tools/build-windows-dll.sh "$ROOT" Release > "$LOG" 2>&1 || rc=$?

if [ "$rc" -eq 0 ]; then
  tail -n 12 "$LOG"
else
  echo "✗ 交叉编译失败（退出码 $rc）—— 完整输出如下（未截断）：" >&2
  cat "$LOG" >&2
fi
exit "$rc"
