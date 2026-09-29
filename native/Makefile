# 内核构建（在 WSL 里跑；见 tools/wsl-build.sh）
#
# 设计约定：
#   · 默认目标默认「编译 + 自检 + 跑单元测试」，一条 make 就是一次完整验收；
#   · -Wall -Wextra -Werror -pedantic 全程打开 —— 接口定义承诺"生成的声明是合法 C"，
#     那就得有个开关能让它当场证明，而不是靠人看；
#   · ASan/UBSan 走 `make asan`，不需要动源码。
#
# 目标一览：
#   make            编译 + 接口定义自检 + 单元测试
#   make check      只做接口定义编译自检（34 条签名的类型检查）
#   make test       只跑单元测试
#   make asan       用 ASan + UBSan 重编并跑单元测试
#   make cover      gcov 覆盖率（输出到 build/cover/）
#   make clean

CC       ?= cc
# -pthread 编译与链接都要带：活分配登记表用 pthread_mutex 保护
CFLAGS   ?= -std=c11 -Wall -Wextra -Werror -pedantic -O1 -g -pthread
CPPFLAGS += -Iinclude -Isrc
# `src/audio/dsh_speex.c` 要 `#include "speex/speex.h"`（libspeex 的公共头）——
# 所以这条 -I 必须在这里。**但"谁能用"不由 -I 决定**：`check` 里有一条检查标准盯着它
# （只许 `src/audio/` 那一层 include），与当年 SQLite 那条同一个套路。
# ⚠️ 只加头文件目录，不加库 —— 库那一份是 `$(VENDOR_OBJS)`（见下面单独那条规则）。
CPPFLAGS += -Ivendor/speex/include
# 测试用词典的目录（**0.2.0 自己的冻结副本**，不是参考实现/testdata）。
# 冻结的原因与逐字节清单见 tools/freeze-fixtures.ps1。
# 用 abspath 而不是相对路径：测试二进制在 build/bin/ 下跑，相对路径会对不上。
CPPFLAGS += -DDSH_TESTDATA_DIR='"$(abspath ../testdata)"'
# 内核源码目录：个别测试要**读源码**做静态检查标准（例如"兜底通道自己不判汉字"这条 ——
# 参考实现那一侧的同一道门也是扫源码，见参考实现的 tools/FallbackProbe ⑥）。
# 用 abspath 的理由同上。
CPPFLAGS += -DDSH_SRC_DIR='"$(abspath src)"'
LDFLAGS  ?= -pthread -lm
# `-lm` 是 libspeex 要的（`stereo.c` / `vbr.c` 用 `log` / `pow`）。它不是"第三方依赖" ——
# 数学函数是 **C 标准库**的一部分，只是 POSIX 上单独一个 `-lm` 而已
# （Windows/MinGW 那边由 msvcrt 提供，不必也不该写 `-lm`）。
BUILD    := build
BIN      := $(BUILD)/bin
OBJ      := $(BUILD)/obj
# ⚠️ `BUILD` 要用 `=` 之外的方式覆盖才管用（下面 `asan` 那一档就是这么做的）——
#    它必须是**命令行变量**，所以这里不能写 `:=` 把它钉死。
#    为什么需要"换个目录编"：make **不跟踪编译参数**。`make asan` 之后所有 .o 都带
#    `-fsanitize=…`，而紧接着一次 `make test` 会认为"全都最新"、直接跑那些 ASan 二进制 ——
#    于是**普通的 B 级门里冒出 LeakSanitizer 报告**（看起来像"测试失败了"，
#    其实是"这一轮跑的根本不是普通构建"）。第三十七轮就是这么被绊了一下的：
#    分开目录之后两边永远各编各的。

# 内核实现：src/ 顶层与各子目录（compress/ crypto/ text/ …）都要收。
# 子目录按相对路径保留在目标名里（src/text/foo.c → build/obj/text/foo.o），
# 这样不同子目录下的同名文件不会互相覆盖。
SRCS     := $(wildcard src/*.c src/*/*.c)
OBJS     := $(patsubst src/%.c,$(OBJ)/%.o,$(SRCS))

# ── 第三方源码（**第二处，也是有意识的一处破例**）────────────────────────────
#
# `vendor/speex/`：官方 **libspeex 1.2.1** 的解码路径，只给 `src/audio/` 用 ——
# Chromium（WebView2）**不解码 Speex**，而 LDOCE5 那类词典的自带录音全是 Ogg Speex，
# 所以"把 `.spx` 变成能播的东西"只能自己解。约定与边界写在 `vendor/README.md`。
#
# ⚠️ 它**不适用**本仓库对自己代码的那套 `-Werror -pedantic`：那是给自己写的规矩，
#    拿它去卡 30 个上游文件只会得到几百条无意义的告警。所以这里**单开一条规则、
#    单开一组参数**，而且只对它生效。边界由下面 `check` 里那条检查标准钉住
#    （和当年 SQLite 那条同一个套路 —— 那次破例已经撤销，见）。
#
# ⚠️ `-DHAVE_CONFIG_H` **不能少**：上游每个 `.c` 都是 `#ifdef HAVE_CONFIG_H #include
#    "config.h" #endif`，而 `EXPORT` 就住在那份 config.h 里。少了它不会不报错地退化 ——
#    会当场报 62 处 `expected ';' before 'void'`（见 vendor/speex/libspeex/config.h 顶上那段）。
# ⚠️ `-include dsh_override.h`：把上游 `_speex_fatal` 里的 `exit(1)` 换掉（见那个文件顶上）。
VENDOR_SRCS := $(wildcard vendor/speex/libspeex/*.c)
VENDOR_OBJS := $(patsubst vendor/speex/libspeex/%.c,$(OBJ)/vendor/speex/%.o,$(VENDOR_SRCS))
VENDOR_CFLAGS ?= -std=c11 -O2 -g -w -DHAVE_CONFIG_H \
  -Ivendor/speex/include -Ivendor/speex/libspeex -Ivendor/speex -include dsh_override.h

TEST_SRCS := $(wildcard tests/test_*.c)
TEST_BINS := $(patsubst tests/%.c,$(BIN)/%,$(TEST_SRCS))

# 要跑的可执行文件 = 全部测试 + 接口定义自检。
# ⚠️ 不要写成 `$(wildcard $(BIN)/test_*)` 再靠 `test:` 的显式前置去编 —— 那样
#    「已经编好但没被前置列表点名」的测试会被**悄悄跳过**（这一版真踩过：
#    test_inflate 编出来了却没跑，`make` 报的是"全部通过"）。
TEST_RUN := $(TEST_BINS) $(BIN)/abi_compile_check

.PHONY: all check test asan cover clean distclean
all: check test

# 目标子目录要先建出来：src/text/foo.c → build/obj/text/foo.o 需要 build/obj/text 存在。
# 用「order-only 前置」（`|` 右边）声明"目录只是个前提、它的时间戳不参与判断"，
# 否则新建目录会让所有目标每次都重编。
#
# ⚠️⚠️ `-MMD -MP` + 结尾的 `-include $(OBJS:.o=.d)` 是**必须的**：少了它，改**头文件**
#     不会重编任何 `.c`，`make` 会拿着**陈旧的目标文件**去跑测试。
#     那一轮真踩过：往 `entry_assets_extra.h`（一段**字符串常量**）里加了三行注释，
#     `dsh_entry_doc.o` 没重编 → `entry_doc` 那组报了 8 条红，而红的是"文档与期望差 235 字节"
#     —— 看起来像检查标准坏了，其实是构建系统的锅。
#     （测试本身是**直接编源码**的（见下面那条规则），所以它们永远是新编译的 ——
#      于是一旦内核的 .o 陈旧，症状就是"测试说内核不对"，更绕。）
$(OBJ)/%.o: src/%.c | $(OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -MMD -MP -c $< -o $@

# 第三方那一份：**单独的参数**（见上面 VENDOR_CFLAGS 那段说明）
$(OBJ)/vendor/speex/%.o: vendor/speex/libspeex/%.c | $(OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(VENDOR_CFLAGS) -MMD -MP -c $< -o $@

# 自动依赖：每个 .o 旁边多一个 .d（`-MMD` 生成），把它 include 进来，
# 于是"改了某个 .h"就能让所有包含它的 .c 重编。`-` 前缀 = 第一次还没有 .d 时不报错。
-include $(OBJS:.o=.d) $(VENDOR_OBJS:.o=.d)

$(BIN)/%: tests/%.c $(OBJS) $(VENDOR_OBJS) | $(BIN)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(OBJS) $(VENDOR_OBJS) -o $@ $(LDFLAGS)

$(OBJ) $(BIN):
	@mkdir -p $@

# ① 接口定义编译自检：证明生成的头文件合法、且 34 条签名的类型自洽
#
# ⚠️ 顺带一条**静态检查标准**：第三方那份（`vendor/speex/`）**只许 `src/audio/` 那一层用**。
#    这是有意识的一处破例（见 `vendor/README.md`）—— 内核其余部分（解析 / 查找 /
#    设置 / 历史 / 文本）仍然零依赖，谁顺手 include 进来就在这几行被挡下。
#    写成 grep 而不是 C 里的检查标准，是因为它要扫的是**全部源码**，
#    而 C 测试只看得见自己链进来的那些翻译单元。
check: $(BIN)/abi_compile_check
	@echo "── 接口定义编译自检 ──"
	@$(BIN)/abi_compile_check
	@echo "── 第三方源码的使用边界 ──"
	@bad=$$(grep -rl 'speex/speex' src --include='*.c' --include='*.h' | grep -v '^src/audio/' || true); \
	if [ -n "$$bad" ]; then \
	  echo "以下文件也 include 了 libspeex 的公共头（只许 src/audio/ 用）："; echo "$$bad"; exit 1; \
	fi; \
	echo "  ✓ 只有 src/audio/ 那一层在用 libspeex"

# ④ **文件访问的边界**（2026-09 加，A 级、秒级）
#
# 为什么要有这一条：内核收进来的路径是 **UTF-8**，而 Win32 的**窄字符** API
# （`fopen` / `GetFileAttributesExA` / `MoveFileExA` / `CreateFileA`…）按**进程的 ANSI 代码页**
# 解这些字节 —— 只有那台机器的 ANSI 代码页恰好是 UTF-8（65001）时才碰巧对。
# 2026-09 用户在**另外两台电脑**上接连撞了两次：写设置失败（`fopen` 写临时文件）、
# **加不进词典**（`fopen` 读 .mdx 算 id）—— 两次都是"业务层自己直开了文件"。
# 所以规矩定死：**业务层不许自己开文件**，一律走 `platform/dsh_file.h`
# （它在那里把 UTF-8 转成 UTF-16 再调宽字符 API）。
#
# ⚠️ 这条检查**只是 A 级**（扫源码，不启动程序），但它是**唯一能在本机发现这一类问题的**手段：
#    本机 ANSI 代码页正好是 65001，那些窄字符调用在本地**照样跑得好好的**，
#    B/B′/C/D 四道检查一条都照不出来。
	@echo "── 文件访问的边界 ──"
	@bad=$$(grep -rnE '\b(fopen|freopen|_wfopen|_open|_wopen|_stat|_wstat|access|remove|rename|GetFileAttributesA|CreateFileA|MoveFileExA|DeleteFileA|FindFirstFileA)[[:space:]]*\(' \
	          src --include='*.c' \
	        | grep -v '^src/platform/' \
	        | grep -vE ':[0-9]+:[[:space:]]*(\*|//|/\*)' || true); \
	if [ -n "$$bad" ]; then \
	  echo "以下**绕过了 platform 层**直接开文件 / 改文件（ANSI 代码页不是 UTF-8 的机器上会翻车）："; \
	  echo "$$bad"; \
	  echo "改法：走 platform/dsh_file.h（那里把 UTF-8 转 UTF-16 再调宽字符 API）——"; \
	  echo "      读文件用 dsh_file_open_read，写盘用 dsh_file_write_atomic / dsh_file_append。"; \
	  echo "注意：fclose 是允许的（关自己开的流）；上面只列「开 / 改」这一类调用。"; \
	  exit 1; \
	fi; \
	echo "  ✓ 业务层没有直接开文件（都走 platform/dsh_file.h）"

# ④b **平台层自己也不许用 A 版（ANSI）的 Win32 路径 API**（2026-09-24 加）
#
# 为什么上面那条不够：它**排除 `src/platform/`**（那一层本来就是"开文件的地方"），
# 于是平台层自己用了窄字符 API 就没人管 —— `dsh_dir_scan` 的 `FindFirstFileA` 就是这么
# 漏了几十轮的。它比 `fopen` 那一类更隐蔽：**资源那条路早就是宽字符的**，
# 所以症状不是"全都坏了"，而是"资源明明取到了、文档却说这本词典没有资源"
# （`dsh_sibling_has_any` 返回 0 → 词条正文的 `data-has-resources="0"`）。
# 本机 ANSI 代码页是 65001，本地永远照不出来 —— 所以这条 A 级检查是**唯一**手段。
#
# ⚠️ 只查**路径类**的 A 版 API（`…A` 结尾那一批）；`fopen` / `remove` / `stat`
#    这些**必须留着**（POSIX 那边的实现就是它们，见 dsh_file.c 的 `#else` 分支）。
# ⚠️ `CreateFileMappingA` 不在表里：它那个路径参数传的是 NULL（用不上路径编码）。
	@echo "── 平台层的宽字符边界（A 版 Win32 路径 API）──"
	@bad=$$(grep -rnE '\b(GetFileAttributesA|CreateFileA|MoveFileExA|DeleteFileA|FindFirstFileA|FindNextFileA|GetFullPathNameA|GetModuleFileNameA|GetTempPathA|CreateDirectoryA|RemoveDirectoryA|GetCurrentDirectoryA|SetCurrentDirectoryA|GetShortPathNameA|CopyFileA|MoveFileA)[[:space:]]*\(' \
	          src/platform --include='*.c' \
	        | grep -vE ':[0-9]+:[[:space:]]*(\*|//|/\*)' || true); \
	if [ -n "$$bad" ]; then \
	  echo "平台层用了 A 版（ANSI）的 Win32 路径 API —— ANSI 代码页不是 UTF-8 的机器上会翻车："; \
	  echo "$$bad"; \
	  echo "改法：路径先 path_to_wide() 转 UTF-16，再调对应的 W 版；拿回来的宽字符文件名"; \
	  echo "      用 wide_to_path() 转回 UTF-8（两个助手都在 dsh_file.c 顶部）。"; \
	  exit 1; \
	fi; \
	echo "  ✓ 平台层没有用 A 版路径 API（都走 W 版 + UTF-8 转换）"

# ② 内核单元测试
# ⚠️ 必须 `$${PIPESTATUS[0]}` 式地取 **每个测试自己** 的退出码：
#    一个 test_*.c 崩了（段错误）就说明内核坏了，不能因为后面那个测试过了就整体算绿。
# ⚠️ 顺带修一处：`test` 必须自己把接口定义自检那个可执行文件也编出来 ——
#    TEST_RUN 里点了它的名，但 TESTS_BINS 里没有它，于是 `make asan`（它先 clean 再 test）
#    永远会走到「build/bin/abi_compile_check: not found」这一步而整体变红。
test: $(TEST_BINS) $(BIN)/abi_compile_check
	@echo "── 内核单元测试 ──"
	@rc=0; for t in $(TEST_RUN); do \
	  echo "· $$t"; \
	  if ! $$t; then rc=1; echo "  ↑ 这个测试失败了（退出码 $${?}）"; fi; \
	done; \
	if [ $$rc -ne 0 ]; then echo "有测试失败"; exit 1; fi

# ⚠️ ASan 与覆盖率**各在自己的构建目录里编**（`BUILD=build-asan` / `build-cover`）：
#    make **不跟踪编译参数**，所以"`make asan` 之后再 `make test`"会认为全都最新、
#    直接跑一堆 ASan 二进制 —— 普通的 B 级门里于是冒出 LeakSanitizer 报告，
#    看起来像"测试失败了"（第三十七轮就是这么被绊了一下的）。
asan:
	@$(MAKE) --no-print-directory BUILD=build-asan clean
	@$(MAKE) --no-print-directory BUILD=build-asan test \
	  CFLAGS="-std=c11 -Wall -Wextra -Werror -pedantic -O1 -g -pthread -fsanitize=address,undefined -fno-omit-frame-pointer"

cover:
	@$(MAKE) --no-print-directory BUILD=build-cover clean
	@$(MAKE) --no-print-directory BUILD=build-cover test \
	  CFLAGS="-std=c11 -Wall -Wextra -Werror -pedantic -O0 -g -pthread --coverage"
	@mkdir -p build-cover/cover
	@gcov -o build-cover/obj $(SRCS) >/dev/null 2>&1 || true
	@mv -f *.gcov build-cover/cover/ 2>/dev/null || true
	@echo "覆盖率报告在 build-cover/cover/"

clean:
	@rm -rf $(BUILD)

# 三个构建目录一起清（普通 / ASan / 覆盖率）—— `clean` 只清当前那个，
# 因为 `make asan` 自己会去清 `build-asan`，不该顺手把普通构建也删了。
distclean:
	@rm -rf build build-asan build-cover
