#!/bin/sh
# 直接跑一个 B 级测试二进制（不重编），看它自己吐什么。
# 用法：wsl.exe -- bash <0.2.0>/tools/wsl-run-test.sh history
set -e
cd "$(dirname "$0")/.."
cd native
name="${1:?用法: wsl-run-test.sh <测试名，如 history>}"
echo "── build/bin/test_$name ──"
set +e
"./build/bin/test_$name"
echo "exit=$?"
