#!/bin/sh
# 用一个测试二进制取崩溃栈（有 gdb 用 gdb，没有就让 ASan 自己报）。
# 用法：wsl.exe -- bash <0.2.0>/tools/wsl-debug-test.sh history
set -e
cd "$(dirname "$0")/.."
cd native
name="${1:?用法: wsl-debug-test.sh <测试名>}"
if command -v gdb >/dev/null 2>&1; then
  echo "── gdb ./build/bin/test_$name ──"
  gdb -batch -ex run -ex bt --args "./build/bin/test_$name" || true
else
  echo "（没有 gdb）—— 改用 ASan 重编这一个测试"
  # ⚠️ 参数要与 Makefile 对齐 —— 这是**固化**下来的工具，构建约定变了就得跟着改，
  #    否则「要用它的时候它坏了」。
  SPEEX_DEF="-DHAVE_CONFIG_H -Ivendor/speex/include -Ivendor/speex/libspeex -Ivendor/speex -include dsh_override.h"
  mkdir -p build/obj-dbg
  for s in vendor/speex/libspeex/*.c; do
    o="build/obj-dbg/$(basename "$s" .c).o"
    [ -f "$o" ] || cc -std=c11 -O1 -g -w -fsanitize=address,undefined $SPEEX_DEF -c "$s" -o "$o"
  done
  cc -std=c11 -Wall -Wextra -O1 -g -pthread -fsanitize=address,undefined \
     -Iinclude -Isrc -Ivendor/speex/include \
     -DDSH_TESTDATA_DIR="\"$(cd ../testdata && pwd)\"" \
     -DDSH_SRC_DIR="\"$(pwd)/src\"" \
     "tests/test_$name.c" src/*.c src/*/*.c build/obj-dbg/*.o \
     -o "build/bin/test_$name.asan" -pthread -lm
  "./build/bin/test_$name.asan" || true
fi
