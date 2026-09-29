/**
 * 前端构建：`web/src/**` → `web/dist/*.js`。
 *
 * 与 0.1.3 的 `build.mjs` 同一个思路（一个 esbuild 调用，不引 electron-vite 那套）：
 * WebView2 版没有"渲染进程"，前端就是一组静态资源，构建链越短越不容易出问题。
 *
 * ⚠️ **产物不嵌进 exe**。这一版是"壳从根目录读"（见 `shell/Lookup.Host/ShellAssets.cs`
 *    顶上那段）：开发时改前端不用重编 C#，而且这一层能被 直接驱动。
 *    所以这里打完就完事 —— 不像 0.1.3 那样还得 `build.ps1` 把 dist 嵌进去。
 */
import { build, context } from 'esbuild'
import { fileURLToPath } from 'node:url'
import path from 'node:path'

const here = path.dirname(fileURLToPath(import.meta.url))
const watch = process.argv.includes('--watch')

/**
 * 每个 HTML 入口一个 bundle（显式写输出名，免得几个 main.ts 撞名）。
 *
 * ⚠️ **这一层与 0.1.3 逐字相同**（三个入口、同一套 `src/**` 布局）：
 *    用户 2026-09 定的约定是「界面是 0.1.3 那几千行攒下来的资产，内核改成 C、
 *    中间那层壳按需重写」—— 所以 `web/**` 是**原样搬过来**的，不在这里改造。
 **/
const entries = {
  floating: 'src/floating/main.ts',
  manager: 'src/manager/main.ts',
  'tray-menu': 'src/tray-menu/main.ts',
}

const options = {
  absWorkingDir: here,
  entryPoints: entries,
  outdir: 'dist',
  bundle: true,
  format: 'esm',
  // 前端只在 WebView2（Chromium）里跑，不需要为旧浏览器转译
  target: ['chrome110'],
  platform: 'browser',
  sourcemap: false,
  minify: !watch,
  legalComments: 'none',
  logLevel: 'info',
}

if (watch) {
  const ctx = await context(options)
  await ctx.watch()
  console.log('[web] watching…')
} else {
  await build(options)
  console.log('[web] build ok')
}
