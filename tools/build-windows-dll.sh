#!/bin/sh
# 用项目内的 mingw sysroot 把内核编成 **Windows x86-64 DLL**（dsh_lookup.dll）。
# 用法：sh build-windows-dll.sh <0.2.0 的 WSL 路径> [Debug|Release]
#
# 为什么单独一个脚本、不并进 Makefile：内核自带的测试目标必须是 **Linux ELF**、
# 在没有 Windows 的机器上也能跑；DLL 是**交叉**目标，CFLAGS 与链接方式都不同，
# 混在一起就等于要求测试先有交叉工具链 —— 那正好违背「内核自带完整单元测试」。
#
# 导出列表与核对方式见下面那两段；顺带生成 dsh_lookup.lib 与符号清单。
set -e
ROOT="$1"
CONF="${2:-Release}"
NATIVE="$ROOT/native"
TOOL="$ROOT/.toolchain/mingw"
SYS="$TOOL/sysroot"
OUT="$ROOT/dist/win-x64"
mkdir -p "$OUT"

CC="$SYS/usr/bin/x86_64-w64-mingw32-gcc-posix"
[ -x "$CC" ] || CC="$SYS/usr/bin/x86_64-w64-mingw32-gcc-win32"
INC="$SYS/usr/x86_64-w64-mingw32/include"
LIB="$SYS/usr/x86_64-w64-mingw32/lib"
[ -x "$CC" ] || { echo "找不到交叉 gcc，先跑 tools/build-mingw-sysroot.sh" >&2; exit 2; }

cd "$NATIVE"
OBJ="$ROOT/build/win-$CONF"
rm -rf "$OBJ"
mkdir -p "$OBJ"

if [ "$CONF" = "Debug" ]; then
  OPT="-O0 -g"
else
  OPT="-O2 -DNDEBUG"
fi

# 内核是纯 C11；Windows 下用 cdecl（接口定义里写着），MinGW 的默认调用约定就是 cdecl。
# `-Ivendor/speex/include`：`src/audio/dsh_speex.c` 要 include libspeex 的公共头
# （「只许 src/audio/ 用」那条边界由 构建脚本 的 check 盯着，不是靠 -I）。
CFLAGS="-std=c11 -Wall -Wextra -Werror -pedantic $OPT -Iinclude -Isrc -Ivendor/speex/include"
CFLAGS="$CFLAGS -DDsh_EXPORTS"      # 留着给将来可能的条件编译；当前不依赖它
CFLAGS="$CFLAGS -I$INC"

echo "── 编译内核（$CONF，目标 x86_64-w64-windows-gnu）──"
SOURCES=$(find src -name '*.c' | sort)
COUNT=0
for s in $SOURCES; do
  o="$OBJ/$(echo "$s" | tr '/' '_' | sed 's/\.c$/.o/')"
  "$CC" $CFLAGS -c "$s" -o "$o"
  COUNT=$((COUNT + 1))
done
echo "  编了 $COUNT 个 .c"

# ── 第三方那一份（官方 libspeex 1.2.1 的解码路径）──────────────────────────
#
# ⚠️ 本仓库对自己代码用的 `-Wall -Wextra -Werror -pedantic` **不适用**于它
#    （与 构建脚本 同一条约定），所以这里**单独一组参数**，而且只对它生效。
# ⚠️ `-DHAVE_CONFIG_H` 不能少（`EXPORT` 住在那份 config.h 里，少了它上游一行都编不过）；
#    `-include dsh_override.h` 把上游 `_speex_fatal` 的 `exit(1)` 换掉。
# ⚠️ 这里**不带 `-lm`**：`log`/`pow` 由 msvcrt 提供（MinGW 上不必也不该写 -lm）。
SPEEX_CFLAGS="-std=c11 $OPT -w -DHAVE_CONFIG_H \
  -Ivendor/speex/include -Ivendor/speex/libspeex -Ivendor/speex -include dsh_override.h"
SPEEX_COUNT=0
for s in vendor/speex/libspeex/*.c; do
  o="$OBJ/vendor_speex_$(basename "$s" .c).o"
  "$CC" $SPEEX_CFLAGS -c "$s" -o "$o"
  SPEEX_COUNT=$((SPEEX_COUNT + 1))
done
echo "  另编了 $SPEEX_COUNT 个第三方 .c（vendor/speex/，单独参数）"

# 导出方式：**由接口定义生成显式导出列表（.def）**，不用 `--export-all-symbols` ——
# 内核内部有一堆 `dsh_` 前缀的跨文件函数（`dsh_mdx_open`、`dsh_json_new` …），全导出会把
# 它们也放进 PE 导出表，「接口定义 vs 实现细节」在二进制层面就分不清了，而本项目的立身
# 之本正是**接口定义只有一处来源**；也不必逐个写 __declspec(dllexport)（那是「加接口忘了
# 加导出」的漏点，症状是 P/Invoke 到运行时才报 EntryPointNotFoundException）。
#
echo "── 链接 dsh_lookup.dll ──"
OBJS=$(find "$OBJ" -name '*.o' | sort)

# ⚠️ 导出列表必须只含**真的存在**的符号：把没实现的写进 .def，链接器会报
#    `cannot export dsh_xxx: symbol not defined` 并失败。所以先用 nm 从 .o 里取回已定义的
#    符号、再与接口定义取交集 —— 于是「接口定义里加了接口、实现还没写」不会把 Windows
#    构建弄红，而「实现写了、却漏了导出」依然会被下面的反向核对抓住。
DEF="$OBJ/exports.def"
python3 - "$ROOT" "$DEF" "$OBJ" <<'PY'
import json, os, re, subprocess, sys
root, out, objdir = sys.argv[1], sys.argv[2], sys.argv[3]
with open(os.path.join(root, 'abi', 'lookup.abi.json'), encoding='utf-8') as f:
    spec = json.load(f)
abi = [fn['name'] for fn in spec['functions']]

objs = []
for dirpath, _dirs, files in os.walk(objdir):
    for fn in files:
        if fn.endswith('.o'):
            objs.append(os.path.join(dirpath, fn))
defined = set()
for o in sorted(objs):
    out_nm = subprocess.run(['nm', '-g', '--defined-only', o], capture_output=True, text=True).stdout
    for line in out_nm.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[1] in ('T', 'D', 'R', 'B'):
            defined.add(parts[2])

present = [n for n in abi if n in defined]
absent = [n for n in abi if n not in defined]

with open(out, 'w', encoding='utf-8') as f:
    f.write('; GENERATED from abi/lookup.abi.json by tools/build-windows-dll.sh\n')
    f.write('; 只列**已实现**的接口（未实现的写进去会让链接失败）\n')
    f.write('EXPORTS\n')
    for n in present:
        f.write('    %s\n' % n)

print('  接口定义 %d 条，已实现 %d 条 → 导出列表 %s' % (len(abi), len(present), os.path.basename(out)))
if absent:
    print('  未实现（如实列出）：%s' % ', '.join(absent))
with open(os.path.join(objdir, 'present.txt'), 'w', encoding='utf-8') as f:
    f.write('\n'.join(present))
PY

"$CC" -shared -o "$OUT/dsh_lookup.dll" $OBJS \
  "$DEF" \
  -Wl,--out-implib,"$OUT/dsh_lookup.lib" \
  -static-libgcc -L"$LIB"

SIZE=$(stat -c %s "$OUT/dsh_lookup.dll")
echo "  ✅ $OUT/dsh_lookup.dll（$SIZE 字节）"

# ⚠️ 产物目录里那份 .def 必须是**本轮生成**的这一份：陈旧副本会让照它核对
#    「这一版导出了什么」的人得出错的结论。
cp "$DEF" "$OUT/dsh_lookup.def"
echo "  ✅ $OUT/dsh_lookup.def（$(grep -c . "$DEF") 行，本轮生成的导出列表）"

echo
echo "── 导出符号核对 ──"
python3 - "$ROOT" "$OUT/dsh_lookup.dll" "$OUT/dsh_lookup.lib" <<'PY'
import json, re, subprocess, sys, os
root, dll, implib = sys.argv[1], sys.argv[2], sys.argv[3]
with open(os.path.join(root, 'abi', 'lookup.abi.json'), encoding='utf-8') as f:
    spec = json.load(f)
all_names = [fn['name'] for fn in spec['functions']]

# 从 PE 导出表里读符号名（比解析 .def 更接近真相）
out = subprocess.run(['objdump', '-p', dll], capture_output=True, text=True).stdout
exported = set(re.findall(r'\[\s*\d+\]\s+(\w+)', out))

present = [n for n in all_names if n in exported]
absent = [n for n in all_names if n not in exported]

print("  接口定义 %d 条；DLL 导出 %d 条；未实现（这一版还没接）%d 条" % (len(all_names), len(present), len(absent)))

# 反向检查：**不属于接口定义**的 dsh_ 符号一个都不该出去 —— 内核内部的
#   dsh_mdx_* / dsh_json_* / dsh_mem_* 都留在内部，不污染二进制接口。
stray = sorted(s for s in exported if s.startswith('dsh_') and s not in set(all_names))
if stray:
    print("  ✗ 导出了接口定义之外的 dsh_ 符号（%d 个）：" % len(stray))
    for s in stray:
        print("    · %s" % s)
    sys.exit(1)

if not present:
    print("  ✗ 一条都没导出 —— 导出列表有问题")
    sys.exit(1)

if absent:
    print("  未实现的（如实列出来，不算失败）：")
    print("    " + ", ".join(absent))

print("  ✅ 导出表只含接口定义里的接口（无内部符号泄漏）；已实现的 %d 条全在" % len(present))
PY

echo
echo "产物："
ls -l "$OUT"
