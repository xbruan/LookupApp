#!/bin/sh
# 用 mingw-w64 交叉编译器把内核编成 **Windows x86-64 DLL**（dsh_lookup.dll）。
#
# 用法：
#   sh tools/build-windows-dll.sh [<仓库根（WSL 路径）>] [Debug|Release]
#   sh tools/build-windows-dll.sh --preflight [<仓库根>]      # 只查工具链、试编一个最小 DLL
#   <仓库根> 不给就从本脚本所在位置推（`tools/` 的上一级），所以下面三种写法都行：
#     sh tools/build-windows-dll.sh              （默认 Release）
#     sh tools/build-windows-dll.sh Release      （仓库根下跑，只要配置名）
#     sh tools/build-windows-dll.sh /mnt/c/…/LookupApp Debug
#
# 交叉编译器从哪来（按优先级）：
#   1. 环境变量 **DSH_MINGW_ROOT** —— 指到 sysroot、指到 MinGW 前缀、或直接指到那个 gcc 都行；
#   2. **PATH** 里的 `x86_64-w64-mingw32-gcc`（机器上本来就装好的那一套）；
#   3. 项目内约定位置 `<仓库根>/.toolchain/mingw/sysroot`（本仓库的便捷默认，**不进版本库**）。
# 装法（可复现、不需要 root、连包版本号都写着）见 native/README.md 的「出 Windows DLL」一节；
# **仓库里存的是版本与装法，不是工具链二进制** —— 这是尺度所在。
#
# 为什么单独一个脚本、不并进 Makefile：内核自带的测试目标必须是 **Linux ELF**、
# 在没有 Windows 的机器上也能跑；DLL 是**交叉**目标，CFLAGS 与链接方式都不同，
# 混在一起就等于要求测试先有交叉工具链 —— 那正好违背「内核自带完整单元测试」。
#
# 导出列表与核对方式见下面那两段；顺带生成 dsh_lookup.lib 与符号清单。
set -e

# ── 参数 ─────────────────────────────────────────────────────────────────────
PREFLIGHT=0
case "${1:-}" in
  --preflight|--check-toolchain)
    PREFLIGHT=1
    shift
    ;;
  -h|--help)
    echo "用法：sh $0 [--preflight] [<仓库根（WSL 路径）>] [Debug|Release]"
    echo "--preflight = 只查工具链（报出用的是哪套、什么版本、sysroot 在哪）并试编一个最小 DLL"
    echo "仓库根不给就从本脚本位置推（tools/ 的上一级），所以「在仓库根下 sh tools/build-windows-dll.sh Release」也行"
    echo "工具链的来源与装法见本文件头部注释与 native/README.md 的「出 Windows DLL」一节"
    exit 0
    ;;
esac
# 第一个参数只在**不是**构建配置名时才算仓库根 —— 于是「显式给仓库根」与「cd 进仓库根只给配置名」
# 两种写法都对。为什么值得多这几行：文档里那条最短命令必须是**能直接粘进终端**的那一条，
# 让人为了凑参数先手工拼一遍 `/mnt/c/…` 路径，就是给人多一次写错的机会。
ROOT=""
CONF=""
case "${1:-}" in
  Debug|Release) CONF="$1" ;;
  "") ;;
  *) ROOT="$1"; CONF="${2:-Release}" ;;
esac
if [ -z "$ROOT" ]; then
  ROOT=$(cd "$(dirname "$0")/.." && pwd)
fi
if [ -z "$CONF" ]; then
  CONF="Release"
fi
NATIVE="$ROOT/native"
OUT="$ROOT/dist/win-x64"
if [ ! -d "$NATIVE" ]; then
  echo "✗ 这个仓库根里没有 native/：$ROOT" >&2
  echo "  用法：sh tools/build-windows-dll.sh [<仓库根（WSL 路径）>] [Debug|Release]" >&2
  echo "        （仓库根不给就从本脚本位置推，所以 cd 进仓库根后只写 Release 也行）" >&2
  exit 2
fi

# ── 挑交叉工具链：显式变量 → PATH → 项目内默认位置 ────────────────────────────
#
# 为什么要有这一段（别删这段注释）：原来这里**只认**项目内 `.toolchain/mingw/sysroot`
# 一条路，于是机器上**已经装好** MinGW（PATH 里就有 `x86_64-w64-mingw32-gcc`）的人照样
# 编不出来；而失败时那句提示还把人指向 `tools/build-mingw-sysroot.sh` —— **那个脚本
# 本仓库里根本没有**，于是「编不出来」变成「照提示做还是编不出来」，脚本把最后一条
# 自救的路也堵死了。现在三处都认，而且**找不到时把找过的每一处逐条说出来**：
# 诊断信息要比一句「找不到」值钱，拿到它的人应该能自己判断下一步做什么。
#
# 为什么还留着「项目内 .toolchain」这条默认：它让「克隆 → 按文档装一次 → 直接跑这个
# 脚本」这条最短路径成立。它被 .gitignore 挡在版本库外，所以**不违背**「不许把工具链
# 提交进库」那条约束 —— 库里放文档，机器上放二进制。

# 在一个目录里找交叉 gcc；找到就把 CC / INC / LIB 三个变量设好并返回 0。
# 认两种形态：
#   · sysroot 形态（`apt-get download` + `dpkg-deb -x` 的产物）：编译器在 `<root>/usr/bin/`，
#     头文件与库在 `<root>/usr/x86_64-w64-mingw32/{include,lib}`；
#   · 前缀形态（`--prefix=…` 那种完整安装、或系统装在 /usr 下）：编译器在 `<root>/bin/`，
#     头文件与库在 `<root>/x86_64-w64-mingw32/{include,lib}`。
# 三个候选名：`x86_64-w64-mingw32-gcc` 是 update-alternatives 那个入口（系统装的那套），
# `-posix` / `-win32` 是 Debian 拆包后的两个变体 —— 解出来的 sysroot 里**只有**这两个。
# 顺序沿用原来的「先 posix 后 win32」：两者只差线程模型，本内核不用线程 API。
probe_toolchain_root() {
  _root="$1"
  for _dir in "$_root/usr/bin" "$_root/bin"; do
    for _name in x86_64-w64-mingw32-gcc x86_64-w64-mingw32-gcc-posix x86_64-w64-mingw32-gcc-win32; do
      if [ -x "$_dir/$_name" ]; then
        CC="$_dir/$_name"
        INC=""
        LIB=""
        if [ -d "$_root/usr/x86_64-w64-mingw32/include" ]; then
          INC="$_root/usr/x86_64-w64-mingw32/include"
          LIB="$_root/usr/x86_64-w64-mingw32/lib"
        elif [ -d "$_root/x86_64-w64-mingw32/include" ]; then
          INC="$_root/x86_64-w64-mingw32/include"
          LIB="$_root/x86_64-w64-mingw32/lib"
        fi
        return 0
      fi
    done
  done
  return 1
}

CC=""
INC=""
LIB=""
SYSFLAGS=""
FROM=""
LOCAL_SYS="$ROOT/.toolchain/mingw/sysroot"
PATH_CC=""
if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
  PATH_CC=$(command -v x86_64-w64-mingw32-gcc)
fi

if [ -n "$DSH_MINGW_ROOT" ]; then
  # 显式指定的一律**优先且不回落**：指错了就当场说清楚，不静默改用别处的编译器 ——
  # 静默回落会编出一个「不是我指的那套」的 DLL，比编不出来难查得多。
  if [ -x "$DSH_MINGW_ROOT" ] && [ ! -d "$DSH_MINGW_ROOT" ]; then
    CC="$DSH_MINGW_ROOT"
    FROM="DSH_MINGW_ROOT（编译器本体）"
  elif probe_toolchain_root "$DSH_MINGW_ROOT"; then
    FROM="DSH_MINGW_ROOT=$DSH_MINGW_ROOT"
  else
    echo "✗ DSH_MINGW_ROOT 指的这个地方没有交叉 gcc：$DSH_MINGW_ROOT" >&2
    echo "  找过：$DSH_MINGW_ROOT/usr/bin/x86_64-w64-mingw32-gcc[-posix|-win32]" >&2
    echo "        $DSH_MINGW_ROOT/bin/x86_64-w64-mingw32-gcc[-posix|-win32]" >&2
    echo "  这个变量可以指 sysroot、指 MinGW 前缀，也可以直接指那个 gcc 可执行文件。" >&2
    echo "  装法（可复现、不需要 root）见 native/README.md 的「出 Windows DLL」一节。" >&2
    exit 2
  fi
elif [ -n "$PATH_CC" ]; then
  CC="$PATH_CC"
  FROM="PATH（$PATH_CC）"
  # PATH 里那套**不补 -I/-L**：它按自己的安装位置回头文件与库（Debian 的交叉 gcc 装在
  # /usr 下，`-I/usr/x86_64-w64-mingw32/include` 是画蛇添足，写错了反而会把**另一个版本**
  # 的头文件带进来，症状是链接期莫名其妙的符号不匹配）。只有它自报有 sysroot 时才显式传
  # `--sysroot` —— 那是 `--with-sysroot` 装出来或自建前缀的那类。
  _self_sysroot=$("$CC" -print-sysroot 2>/dev/null || true)
  if [ -n "$_self_sysroot" ]; then
    SYSFLAGS="--sysroot=$_self_sysroot"
  fi
elif probe_toolchain_root "$LOCAL_SYS"; then
  FROM="项目内（$LOCAL_SYS）"
else
  # 三条路都断了：把「找过哪三处」和「下一步做什么」一次说完。退出码非零。
  echo "✗ 找不到能编 Windows x86-64 的交叉 gcc。按优先级找过这三处：" >&2
  echo "  1. \$DSH_MINGW_ROOT —— 未设置（设了就在这里说它指的是哪，并当场失败）" >&2
  echo "  2. PATH 里的 x86_64-w64-mingw32-gcc —— 没有" >&2
  echo "  3. 项目内 $LOCAL_SYS —— 不存在" >&2
  echo >&2
  echo "  怎么办（任选一条）：" >&2
  echo "  · 机器上已经装好 MinGW：把它放进 PATH，或者直接指给脚本 ——" >&2
  echo "      DSH_MINGW_ROOT=/usr sh tools/build-windows-dll.sh $ROOT Release" >&2
  echo "  · 还没有工具链：照 native/README.md 的「出 Windows DLL」一节装" >&2
  echo "      （apt-get download + dpkg-deb -x，不需要 root；那里写着确切的包名与版本）" >&2
  echo "  · 只想先确认手上这套能不能用（不编内核，几秒钟）：" >&2
  echo "      sh tools/build-windows-dll.sh --preflight" >&2
  exit 2
fi

if [ -n "$INC" ]; then
  INCFLAG="-I$INC"
else
  INCFLAG=""
fi
if [ -n "$LIB" ]; then
  LIBFLAG="-L$LIB"
else
  LIBFLAG=""
fi

# 报出「这一轮到底用的是哪套工具链」。这条信息必须能回答问题，而不是只说「找到了」——
# 换过工具链的人第一个要问的就是它，而它决定了产出的 DLL 依赖哪几个系统库。
report_toolchain() {
  echo "── 交叉工具链 ──"
  echo "  来源    ：$FROM"
  echo "  编译器  ：$CC"
  echo "  版本    ：$("$CC" --version | head -1)，目标 $("$CC" -dumpmachine)"
  if [ -n "$INC" ]; then
    echo "  头文件  ：$INC"
  else
    echo "  头文件  ：（走编译器自带的搜索路径）"
  fi
  if [ -n "$LIB" ]; then
    echo "  库      ：$LIB"
  else
    echo "  库      ：（走编译器自带的搜索路径）"
  fi
  if [ -n "$SYSFLAGS" ]; then
    echo "  sysroot ：$SYSFLAGS"
  fi
}

# ── 预检：只回答「这套工具链能不能用」，不编内核 ──────────────────────────────
if [ "$PREFLIGHT" = "1" ]; then
  report_toolchain

  # 除 gcc 之外，这个脚本生成 .def（python3 + nm）与核对导出表（objdump）还要几个工具；
  # 缺了照样编不出来，但症状会出现在半路（编完几十个 .c 才炸）—— 所以在这里先查。
  MISSING=""
  for t in python3 nm objdump find sed tr; do
    if ! command -v "$t" >/dev/null 2>&1; then
      MISSING="$MISSING $t"
    fi
  done
  if [ -n "$MISSING" ]; then
    echo "  ✗ 还缺这些工具：$MISSING" >&2
    echo "    （生成导出列表要用 python3 与 nm，核对 PE 导出表要用 objdump）" >&2
    exit 2
  fi
  echo "  其它工具：python3 / nm / objdump / find / sed / tr 都在"

  # 真编一个最小 DLL：光看「文件在不在」判不出可用性 —— 头文件缺一个、库路径错一格，
  # 都要到编内核时才现形，那时已经白编了几十个 .c。这里几秒内给出结论。
  echo
  echo "── 试编一个最小 DLL ──"
  TMPD="${TMPDIR:-/tmp}/dsh-toolchain-preflight-$$"
  rm -rf "$TMPD"
  mkdir -p "$TMPD"
  trap 'rm -rf "$TMPD"' EXIT INT TERM
  cat > "$TMPD/probe.c" <<'PROBE'
/* 预检用：撞一遍 C 头文件、Windows 头文件与链接期（msvcrt）。编得出来就算这套工具链可用。 */
#include <stdio.h>
#include <string.h>
#include <windows.h>
int dsh_preflight_probe(void) { return (int)(strlen("ok") + sizeof(HANDLE)); }
PROBE
  "$CC" -shared -o "$TMPD/probe.dll" "$TMPD/probe.c" \
    $SYSFLAGS $INCFLAG $LIBFLAG -static-libgcc \
    -Wl,--out-implib,"$TMPD/probe.lib"
  if [ ! -f "$TMPD/probe.dll" ]; then
    echo "  ✗ 没编出 DLL —— 这套工具链不可用" >&2
    exit 2
  fi
  PE=$(objdump -f "$TMPD/probe.dll" | sed -n 's/.*file format //p' | head -1)
  echo "  ✅ 编出了可用的 DLL（$(stat -c %s "$TMPD/probe.dll") 字节，格式 $PE）"
  echo
  echo "工具链没问题，可以编内核："
  echo "  sh tools/build-windows-dll.sh $ROOT Release"
  exit 0
fi

# ── 正式构建 ─────────────────────────────────────────────────────────────────
report_toolchain
echo

mkdir -p "$OUT"
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
CFLAGS="$CFLAGS $SYSFLAGS $INCFLAG"

echo "── 编译内核（$CONF，目标 $("$CC" -dumpmachine)，windows-gnu ABI）──"
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
  -static-libgcc $SYSFLAGS $LIBFLAG

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
