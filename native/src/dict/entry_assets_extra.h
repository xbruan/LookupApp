/* ==========================================================================
 * 0.2.0 对词条正文的**唯一一处有意追加** —— 与 `entry_assets.h` 分开放。
 *
 * ── 为什么必须分开（别把这段挪进 entry_assets.h）───────────────────────────
 *
 * `native/src/dict/entry_assets.h` 是**自动生成的文件**：`tools/make-entry-assets.ps1` 让 0.1.3 的
 * 参考实现（`EntryDocument.cs` 的 `BaseStyle` / `BridgeScript`）**原样吐出来**。它全部价值就在
 * 「逐字等于参考实现」——标准答案文件（测试）按
 * `prefix + DSH_ENTRY_BASE_STYLE + mid + DSH_ENTRY_BRIDGE_SCRIPT + suffix` 拼回参考实现那份
 * 完整文档、核它的 SHA-256，然后才跟 C 版逐字节比。
 *
 * 往那份自动生成的文件里改一行有两个坏处：**生成脚本一跑就被覆盖**；而且那条「与参考实现逐字节一致」
 * 的检查标准会**悄悄失效**（模板与实现一起改，照样绿）。所以这一版的增量单独放在这里，
 * 由 `dsh_entry_doc_build` 作为**第二个 `<script>`** 追加在参考脚本之后；
 * 标准答案文件那条检查标准同步改成「**插进这一段**之后与参考实现逐字节一致」，
 * 并仍然先核参考那份的 SHA-256。
 *
 * ── 它管什么 ────────────────────────────────────────────────────────────
 *
 * 词条里点了一下、可什么都没发生。两条路在 0.1.3 时代是**完全不报错地**的
 * （用户为「点了没反应」报过三次），第三条是「看着可点、其实没人接」：
 *   ① 词典自己写的 `<a>` **没有 href**；
 *   ② 带锚点的链接，而本文档里**没有这个锚点**；
 *   ③ 光标是手型、既不是链接、页面上也没有任何脚本接住它。
 * 前两条查一下就知道；第三条**必须等 60ms 看页面到底变没变**才敢说 —— 词典自己的 `.js`
 * （0.2.0 起放行，见 `docs/design/不带mdd的词典与机器翻译开发指导.md` §A5）本来就会接管一部分点击
 * （浮球 / 折叠 / 页签），它接管成功时页面必然有变化，那时多嘴比不报错地还坏。
 *
 * 报给宿主的是**代码**（`reason`），那句话由界面那一层说（`web/src/floating/main.ts`）——
 * 与参考脚本「只传数据、不传中文」同一条约定。
 *
 * ⚠️ 这一段是 C 字符串：里面**不许出现半角双引号**（会把字符串截断），JS 字符串一律单引号。
 * ========================================================================== */

#ifndef DSH_ENTRY_ASSETS_EXTRA_H
#define DSH_ENTRY_ASSETS_EXTRA_H

/**
 * 追加段前面的那道缝：参考脚本的 `</script>` 与**这一段**的 `<script>`。
 *
 * 单独拿出来是因为标准答案文件那条检查标准要用它：C 版文档 = 参考那份的字节
 * `prefix + BASE_STYLE + mid + BRIDGE_SCRIPT` + 本宏 + `DSH_ENTRY_BRIDGE_EXTRA` + `suffix`。
 */
#define DSH_ENTRY_EXTRA_OPEN "</script>\n<script>"

/** 上面那个追加段（`<script>` 与 `</script>` 之间那一段 JS） */
static const char DSH_ENTRY_BRIDGE_EXTRA[] =
  "\n"
  "(function () {\n"
  "  // 宿主靠 source 认这条消息（与桥接脚本同一个来源名）\n"
  "  function post(payload) {\n"
  "    try { parent.postMessage(Object.assign({ source: 'lookup-entry' }, payload), '*'); } catch (e) {}\n"
  "  }\n"
  "\n"
  "  // 本文档的词条名（与桥接脚本同一处来源：宿主用 ?word= 给的）\n"
  "  var CURRENT_WORD = '';\n"
  "  try {\n"
  "    var matched = /[?&]word=([^&]*)/.exec(location.search || '');\n"
  "    if (matched) CURRENT_WORD = decodeURIComponent(matched[1]);\n"
  "  } catch (e) {}\n"
  "\n"
  "  /*\n"
  "   * 「点完之后页面**真的没变**」——检查标准不是「这个元素看着不像链接」。\n"
  "   * 词典自己的脚本（.js）本来就会接管一部分点击（浮球 / 折叠 / 页签），\n"
  "   * 它接管成功时页面必然有变化；那时**不许**多嘴（每次点浮球都弹一句，比不报错地还坏）。\n"
  "   * 所以拿一个计数器盯着 DOM 变化，再配上滚动位置与地址栏 hash 一起判。\n"
  "   */\n"
  "  var domChanges = 0;\n"
  "  try {\n"
  "    if (window.MutationObserver && document.body) {\n"
  "      new MutationObserver(function () { domChanges++; })\n"
  "        .observe(document.body, { childList: true, subtree: true, attributes: true, characterData: true });\n"
  "    }\n"
  "  } catch (e) {}\n"
  "\n"
  "  function scrollTop() {\n"
  "    try { return window.scrollY || window.pageYOffset || document.documentElement.scrollTop || 0; }\n"
  "    catch (e) { return 0; }\n"
  "  }\n"
  "\n"
  "  // 光标是手型 = 用户以为这里能点（cursor 会继承，所以只看点到的那个元素就够）\n"
  "  function looksClickable(el) {\n"
  "    try { return !!el && window.getComputedStyle(el).cursor === 'pointer'; } catch (e) { return false; }\n"
  "  }\n"
  "\n"
  "  // 那排出路按钮是**宿主**放进来的，点它有自己的处理器，不归这条检查标准管\n"
  "  function insideChips(el) {\n"
  "    var box = document.getElementById('lookupChips');\n"
  "    return !!(box && el && (el === box || box.contains(el)));\n"
  "  }\n"
  "\n"
  "  // 本文档里有没有这个锚点（与桥接脚本的 scrollToFragment 同一条找法：先 id 再 name）\n"
  "  function hasFragment(fragment) {\n"
  "    if (!fragment) return false;\n"
  "    try {\n"
  "      if (document.getElementById(fragment)) return true;\n"
  "      var named = document.getElementsByName(fragment);\n"
  "      return !!(named && named.length);\n"
  "    } catch (e) { return false; }\n"
  "  }\n"
  "\n"
  "  /*\n"
  "   * 这次点击是不是要**换词条**？是的话不归这里管：宿主要么装上新词条、要么自己说一句\n"
  "   * 「没找到」。判法与桥接脚本同约定（词为空、或就是本文档自己 → 不换词条）。\n"
  "   */\n"
  "  function looksUpAnotherWord(raw) {\n"
  "    if (raw.indexOf('entry://') !== 0) return false;\n"
  "    // ⚠️ 用 'entry://'.length（8），别写 7 —— 写错了词会变成「/」，于是**整条锚点分支被跳过**\n"
  "    //    （第一版就是 7，诊断脚本 `--dead-clicks` 当场照出 badanchor 不弹）\n"
  "    var body = raw.slice('entry://'.length);\n"
  "    var hash = body.indexOf('#');\n"
  "    var word = hash < 0 ? body : body.slice(0, hash);\n"
  "    return !!word && word !== CURRENT_WORD;\n"
  "  }\n"
  "\n"
  "  /*\n"
  "   * 点了却没反应的三条路（前两条在 0.1.3 时代完全不报错地，用户为「点了没反应」报过三次）：\n"
  "   *   · 词典自己写的 <a> **没有 href** —— 看着是链接，点下去什么都不做；\n"
  "   *   · 带锚点的链接，而本文档里**没有这个锚点**（页面内导航写错了 / 词典换过版本）；\n"
  "   *   · 光标是手型，可既不是链接、页面上也没有任何脚本接住它。\n"
  "   *\n"
  "   * 监听挂在 **window 的捕获阶段**：它比文档里所有处理器（含词典自己的脚本）都先跑，\n"
  "   * 所以快照取到的是「点下去之前」的状态，「变没变」才判得准。\n"
  "   */\n"
  "  window.addEventListener('click', function (event) {\n"
  "    if (event.button && event.button !== 0) return;\n"
  "    if (insideChips(event.target)) return;\n"
  "\n"
  "    var node = event.target;\n"
  "    while (node && node !== document && !(node.tagName === 'A')) node = node.parentNode;\n"
  "    var anchor = (node && node !== document && node.tagName === 'A') ? node : null;\n"
  "\n"
  "    if (anchor) {\n"
  "      var raw = anchor.getAttribute('href') || '';\n"
  "      if (!raw) {\n"
  "        post({ type: 'dead-click', reason: 'no-href' });\n"
  "        return;\n"
  "      }\n"
  "      if (looksUpAnotherWord(raw)) return;\n"
  "      // 只认条目内跳转（entry://… 与 #…）：sound:// / http(s):// 那几条路各有各的归属\n"
  "      if (raw.indexOf('entry://') !== 0 && raw.charAt(0) !== '#') return;\n"
  "      var at = raw.indexOf('#');\n"
  "      var fragment = at < 0 ? '' : raw.slice(at + 1);\n"
  "      if (fragment && !hasFragment(fragment)) post({ type: 'dead-click', reason: 'no-anchor' });\n"
  "      return;\n"
  "    }\n"
  "\n"
  "    if (!looksClickable(event.target)) return;\n"
  "    var changes = domChanges;\n"
  "    var scrolled = scrollTop();\n"
  "    var hashBefore = location.hash || '';\n"
  "    setTimeout(function () {\n"
  "      if (domChanges !== changes) return;\n"
  "      if (scrollTop() !== scrolled) return;\n"
  "      if ((location.hash || '') !== hashBefore) return;\n"
  "      post({ type: 'dead-click', reason: 'nothing-happened' });\n"
  "    }, 60);\n"
  "  }, true);\n"
  "})();\n"
  ""
;

#endif /* DSH_ENTRY_ASSETS_EXTRA_H */
