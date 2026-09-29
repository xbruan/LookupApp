#!/bin/sh
# 在 WSL 里跑 B 级 gate（`make test` / `make asan`）。
# ⚠️ 单独写成脚本是为了绕开「PowerShell → bash -c 里带中文路径」那层转义
#    （直接写在 `bash -lc "…"` 里会被 PowerShell 吃掉反斜杠）。
# 用法：wsl.exe -- bash /mnt/c/.../0.2.0/tools/wsl-make-test.sh [asan]
#
# ⚠️ **退出码就是这道 gate 的实测结果**，所以结尾**不许**写成 `make … || true`（那样编译
#    失败或某个测试崩了、脚本照样退出 0）。崩掉的真实现象是输出里**少了那一组的汇总行**
#    （崩掉的二进制打不出实测结果）—— 所以退出码还要与逐组实测结果交叉核对。
set -e
cd "$(dirname "$0")/.."
cd native
target="${1:-test}"
rc=0
make "$target" > /tmp/dsh-b-$target.txt 2>&1 || rc=$?
# 逐文件那一行 + 编译错误，**不截断**（截断了就看不出少了哪一组）
grep -E '：|项，失败|共 |error|Error|警告' /tmp/dsh-b-$target.txt || true
# 交叉核对：**该出现的组数**（`TEST_RUN` 里每个二进制正好打一行 `…：N 项，失败 M`）
groups=$(grep -cE '项，失败' /tmp/dsh-b-$target.txt || true)
echo "----- 逐组实测结果 $groups 行；完整输出留在 /tmp/dsh-b-$target.txt（$(wc -l < /tmp/dsh-b-$target.txt) 行）-----"
if [ "$groups" -eq 0 ]; then
  echo "⚠️ 一行逐组实测结果都没有 —— 这一趟根本没跑起来（编译失败？），不能当成门绿"
fi
exit "$rc"
