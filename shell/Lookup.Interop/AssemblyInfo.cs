using System.Runtime.CompilerServices;

/*
 * 生成出来的绑定（`DshLookup.g.cs`）里那两个类型是 `internal` —— 那是刻意的：
 * 它们是**壳内部使用**的东西，不该被别的程序集看见（否则内核的 ABI 会从壳里漏出去，
 * 哪天有人绕过宿主适配层直接调内核，纪律就断了）。
 *
 * 但"内部使用"与"只有同一个程序集能用"是两回事。这里把三个程序集显式列出来：
 *   · Lookup.Host      宿主适配层（虚拟资源主机等）
 *   · Lookup.App       壳本体（0.2.0 的窗口）—— 它建引擎、销毁引擎，是第一手的调用方
 *   ·    那份验收（它要调包装层 —— 而壳将来用的就是包装层，
 *                      测试要是绕开它自己拼 IntPtr，"包装层对不对"就永远验不到）
 *
 * ⚠️ **这个文件是手写的，不许被覆盖**：生成脚本只写 `DshLookup.g.cs`。
 *    往名单里加程序集之前先问一句"它是不是壳的一部分" —— 名单越长，
 *    "INTERNAL" 这层保护就越薄。
 */
[assembly: InternalsVisibleTo("Lookup.Host")]
[assembly: InternalsVisibleTo("Lookup.App")]
[assembly: InternalsVisibleTo("WindowsDllTest")]
