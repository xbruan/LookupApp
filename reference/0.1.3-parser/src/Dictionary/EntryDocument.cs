using System;
using System.Globalization;

namespace Lookup.Dictionary;

internal sealed class EntryDocumentOptions
{
    internal string DictId;
    /// <summary>MDX 中的词条 HTML 片段</summary>
    internal string Definition;
    /// <summary>是否挂了 .mdd 资源库（决定是否给出资源缺失提示）</summary>
    internal bool HasResources;
    /// <summary>未命中时的提示文案（可信 HTML，由引擎生成）</summary>
    internal string Notice;
}

/// <summary>
/// 把词条片段包装成一个自包含的 HTML 文档，供 &lt;iframe&gt; 直接加载。
///
/// 与 Electron 版唯一的结构差异是资源协议：WebView2 **不支持**拦自定义 scheme
/// （实测导航到 `dictres://…` 会被直接 ConnectionAborted，WebResourceRequested 根本收不到），
/// 所以改用 `https://&lt;dictId&gt;.dictres.invalid/`，由 WebView2 的 WebResourceRequested
/// 在发出网络请求之前就地应答。请求永远不会离开进程，也永远不会真的解析 DNS。
/// dictId 依然放在 **host** 位置，这样词条里 `/images/a.png` 这类根相对路径
/// 经 `&lt;base&gt;` 解析后依然落在同一本词典上。
/// </summary>
internal static class EntryDocument
{
    /// <summary>词条资源用的"域名后缀"，.invalid 是 RFC 2606 保留的、永不解析的顶级域</summary>
    internal const string ResourceDomain = "dictres.invalid";

    /// <summary>外壳（胶囊/面板/管理窗）自己的虚拟站点</summary>
    internal const string ShellDomain = "lookup.local";

    internal static string OriginFor(string dictId) =>
        "https://" + dictId + "." + ResourceDomain;

    internal static string BaseFor(string dictId) =>
        OriginFor(dictId) + "/";

    /// <summary>词条正文文档的路径（放在 host 上，所以资源路径不会和它冲突）</summary>
    internal const string EntryPath = "/__entry__";

    internal static string EntryUrlFor(string dictId, string word) =>
        OriginFor(dictId) + EntryPath + "?word=" + Uri.EscapeDataString(word ?? string.Empty);

    internal static string Build(EntryDocumentOptions options)
    {
        var baseHref = BaseFor(options.DictId);
        var body = string.IsNullOrEmpty(options.Notice)
            ? options.Definition ?? string.Empty
            : "<div class=\"lookup-notice\">" + options.Notice + "</div>";

        return "<!doctype html>\n" +
               "<html lang=\"zh-CN\">\n" +
               "<head>\n" +
               "<meta charset=\"utf-8\">\n" +
               "<meta http-equiv=\"Content-Security-Policy\" content=\"" + Csp + "\">\n" +
               "<base href=\"" + baseHref + "\">\n" +
               "<style>" + BaseStyle + "</style>\n" +
               "</head>\n" +
               "<body class=\"lookup-entry\" data-has-resources=\"" + (options.HasResources ? "1" : "0") + "\">\n" +
               body + "\n" +
               // 空状态里的"出路"按钮（借查 / 翻译）由宿主送进来，见 BridgeScript 里的 renderChips
               "<div class=\"lookup-chips\" id=\"lookupChips\" hidden></div>\n" +
               "<script>" + BridgeScript + "</script>\n" +
               "</body>\n" +
               "</html>";
    }

    /// <summary>
    /// CSP 只放行词典资源域与内联，杜绝词条把请求发到公网。
    /// （iframe 用 sandbox="allow-scripts" 且不给 allow-same-origin，拿到的是 opaque origin，
    /// 所以这里必须显式列出资源域，而不能指望 'self'。）
    /// </summary>
    private static readonly string Csp =
        "default-src 'none'; " +
        "img-src https://*." + ResourceDomain + " data: blob:; " +
        "media-src https://*." + ResourceDomain + " data: blob:; " +
        "style-src 'unsafe-inline' https://*." + ResourceDomain + "; " +
        "font-src https://*." + ResourceDomain + " data:; " +
        "script-src 'unsafe-inline' https://*." + ResourceDomain + "; " +
        "connect-src 'none'; frame-src 'none'; object-src 'none'; form-action 'none'; " +
        "base-uri https://*." + ResourceDomain + ";";

    private const string BaseStyle = @"
:root { color-scheme: light; }
*, *::before, *::after { box-sizing: border-box; }
html { background: transparent; }
/*
 * 滚动条**永远占好位**（即「短词条也留那条 10px 的槽」）。
 *
 * 不这么写会有一个只在**短词条**上现形的怪相：正文不够长时浏览器不画滚动条，
 * 于是右边只剩上面那 6px（那 6px 是「16 − 10」为了给滚动条让位才这么小的），
 * 而左边是 16px —— 同一页文字左边空一条、右边贴着边，看着像版式坏了。
 * `overflow-y: scroll` 让滚动条槽**恒在**，两侧就都是 16px；
 * 槽本身是透明的（见下面 ::-webkit-scrollbar-track），不滚动时也不会有滚动条画出来。
 *
 * 代价如实记：**短词条**的正文宽度会比原来少 10px（长词条本来就滚动，一个像素不变）——
 * 换来的是左右对称。见 开发记录 §71。
 */
html { overflow-y: scroll; }
body {
  margin: 0;
  padding: 14px 16px 20px;
  /* 右侧只留 6px：滚动条自己占 ~10px，6+10 才和左边那 16px 看起来一样宽。
     用户 2026-09：「内容框的 scroll bar 距离右边缘太远，滚动条区域占用了很多内容框空间」——
     但**这条必须带 !important**：字典自带的 CSS（.mdd / 同目录散放的 style.css）会整条覆盖
     body 的 padding（实测某本测试用词典把它压成四条 12px），而「右边给滚动条留多少」是宿主的版式，
     不该由词典说了算。改这个数之前先看下面 ::-webkit-scrollbar 的宽度，两者是配套的
     （见 开发记录 §68）。 */
  padding-right: 6px !important;
  background: transparent;
  color: #1f2430;
  font-family: ""Segoe UI Variable Text"", ""Segoe UI"", ""Microsoft YaHei UI"", ""Microsoft YaHei"", system-ui, -apple-system, sans-serif;
  font-size: 14px;
  line-height: 1.68;
  -webkit-font-smoothing: antialiased;
  overflow-wrap: anywhere;
  word-break: break-word;
}
body > *:first-child { margin-top: 0 !important; }
body > *:last-child { margin-bottom: 0 !important; }
a { color: #2f6df6; text-decoration: none; }
a:hover { text-decoration: underline; }
img, video, canvas { max-width: 100%; height: auto; }
audio { max-width: 100%; }
table { border-collapse: collapse; max-width: 100%; }
hr { border: 0; border-top: 1px solid rgba(20, 24, 40, .1); margin: 14px 0; }
code, kbd, samp { font-family: ""Cascadia Code"", Consolas, ""SFMono-Regular"", monospace; font-size: .92em; }
.lookup-notice {
  padding: 14px 16px;
  border-radius: 12px;
  background: #fff6ed;
  color: #8a4b12;
  font-size: 13.5px;
  line-height: 1.7;
}
.lookup-notice code { padding: 1px 5px; border-radius: 5px; background: rgba(138, 75, 18, .1); }
.lookup-suggests {
  margin-top: 10px;
  padding-top: 10px;
  border-top: 1px dashed rgba(138, 75, 18, .28);
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  align-items: center;
}
.lookup-suggests a {
  display: inline-block;
  padding: 2px 10px;
  border-radius: 999px;
  background: rgba(138, 75, 18, .1);
  color: #8a4b12;
  text-decoration: none;
}
.lookup-suggests a:hover { background: rgba(138, 75, 18, .2); text-decoration: none; }
/*
 * 「出路」按钮：这个词在当前词典里没查到的时候，宿主会送来一两个能走下去的方向
 * （在别的词典里查一次 / 让机器翻译一下）。
 * 视觉上贴着上面那句提示走，所以是一排小胶囊，不是一个「工具栏」。
 */
.lookup-chips {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  margin-top: 14px;
}
.lookup-chip {
  font: inherit;
  font-size: 13px;
  padding: 5px 14px;
  border-radius: 999px;
  border: 1px solid rgba(47, 109, 246, .28);
  background: rgba(47, 109, 246, .08);
  color: #2f6df6;
  cursor: pointer;
}
.lookup-chip:hover { background: rgba(47, 109, 246, .16); }
::-webkit-scrollbar { width: 10px; height: 10px; }
::-webkit-scrollbar-track { background: transparent; }
::-webkit-scrollbar-thumb {
  background: rgba(90, 100, 125, .3);
  border: 3px solid transparent;
  border-radius: 8px;
  background-clip: padding-box;
}
::-webkit-scrollbar-thumb:hover { background: rgba(90, 100, 125, .5); background-clip: padding-box; }
";

    /// <summary>注入到词条正文里的桥接脚本（与 Electron 版逻辑一致）</summary>
    private const string BridgeScript = @"
(function () {
  // 打一个标记：自动化验证可以据此判断桥接脚本到底跑没跑起来
  try { document.documentElement.dataset.lookupBridge = '1'; } catch (e) {}

  // 资源根地址从 <base> 读取，脚本本身不需要知道 dictId
  var RES_BASE = '';
  try {
    var baseEl = document.querySelector('base');
    if (baseEl && baseEl.href) RES_BASE = baseEl.href;
  } catch (e) {}

  // 本文档对应哪个词条（宿主用 ?word= 给的）：用来判断「链接是不是指回本词条」
  var CURRENT_WORD = '';
  try {
    var matched = /[?&]word=([^&]*)/.exec(location.search || '');
    if (matched) CURRENT_WORD = decodeURIComponent(matched[1]);
  } catch (e) {}

  /*
   * 词典 id：藏在 <base> 的 host 里（https://<词典 id>.dictres.invalid/）。
   * 宿主页的播放器要用它拼「外壳域上的那份音频地址」——宿主页的 CSP 只放行同源，
   * 直接把词条域这个地址丢过去会被拦掉。文档自己播时用下面的 soundUrl（同域，不受影响）。
   */
  var DICT_ID = '';
  try {
    var hostMatch = /^https:\/\/([^.]+)\./.exec(RES_BASE);
    if (hostMatch) DICT_ID = hostMatch[1];
  } catch (e) {}

  /*
   * 词典自带的音频。词条里两种写法都见过：
   *   sound://GB_brelasdeapple.spx   （LDOCE5）
   *   snd://apple__gb_1.spx          （牛津高阶第 9 版）
   * 两者都是「播放这段话」，统一按同一个键名处理。
   */
  function soundKeyOf(raw) {
    return String(raw || '').replace(/^(?:sound|snd):\/\//i, '').replace(/^[\\\/]+/, '');
  }
  function isSoundRef(raw) {
    return /^(?:sound|snd):\/\//i.test(String(raw || ''));
  }
  function soundUrl(raw) {
    var key = soundKeyOf(raw);
    if (!key) return '';
    // 键名要编码：mdd 里带空格、中文、反斜杠的资源都有
    return RES_BASE + '__sound__/' + encodeURIComponent(key);
  }

  /*
   * 有些词典不是靠 <a href> 播，而是自己写 JS 建 <audio> 或 new Audio(地址)。
   * 这些地址在浏览器里是取不到的（sound:// 不是真协议），所以顺手改写成上面那条路由，
   * 让词典自带的播放逻辑也能用 —— 我们把「取字节」这件事接过来，播放仍旧交给它自己。
   */
  function rewriteSoundSources(root) {
    try {
      var scope = root || document;
      var nodes = scope.querySelectorAll('[src]');
      for (var i = 0; i < nodes.length; i++) {
        var value = nodes[i].getAttribute('src');
        if (isSoundRef(value)) nodes[i].setAttribute('src', soundUrl(value));
      }
    } catch (e) {}
  }
  try {
    var NativeAudio = window.Audio;
    if (typeof NativeAudio === 'function') {
      var SoundAudio = function (src) {
        var target = isSoundRef(src) ? soundUrl(src) : src;
        return new NativeAudio(target);
      };
      SoundAudio.prototype = NativeAudio.prototype;
      window.Audio = SoundAudio;
    }
  } catch (e) {}
  rewriteSoundSources(document);
  window.addEventListener('load', function () { rewriteSoundSources(document); });
  setTimeout(function () { rewriteSoundSources(document); }, 400);

  function post(payload) {
    try { parent.postMessage(Object.assign({ source: 'lookup-entry' }, payload), '*'); } catch (e) {}
  }

  /*
   * 正文里选了文字要告诉宿主：宿主据此在选区旁边浮出「朗读 / 复制」那排按钮。
   *
   * 为什么必须由这边上报：正文在跨源 sandbox iframe 里，宿主**够不着它的 Selection**；
   * 选区的位置也只能在这边量（iframe 视口坐标），宿主再加上 iframe 自己的位置，
   * 就换算成了宿主页坐标。
   *
   * 节流 60ms：拖选时 selectionchange 会连着来，不节流则宿主那边每来一条都要重建按钮。
   * 滚动也要跟着报一次 —— 选区在视口里的位置变了，浮层得跟着挪，否则会和文字对不上。
   */
  var selectionTimer = null;
  function reportSelection() {
    var text = '';
    var rect = null;
    try {
      var selection = window.getSelection ? window.getSelection() : null;
      if (selection && selection.rangeCount > 0 && !selection.isCollapsed) {
        text = String(selection.toString() || '')
          .replace(/[\s\u3000]+/g, ' ')
          .replace(/^\s+|\s+$/g, '');
        var box = selection.getRangeAt(0).getBoundingClientRect();
        rect = { left: box.left, top: box.top, width: box.width, height: box.height, bottom: box.bottom };
      }
    } catch (e) { text = ''; rect = null; }

    /*
     * 选中的文字要**整段报上来** —— 朗读功能就是「选多少念多少」（长文本由宿主切成几段接着念）。
     *
     * 这里的上限只防「把整页都选上」这种极端情况：20000 字已经远超任何人想听的量
     * （约二十分钟的朗读），真被截到时浮层的提示里会写明「只念前 20000 字」。
     * 以前这里是 300 字，用户报的「选长一点的文本读不完」就是它。
     */
    var clipped = text.length > 20000;
    if (clipped) text = text.slice(0, 20000);
    /*
     * scrollY 一起报：浮层上那个「查词」按钮按下去就换词条了，宿主得把
     * 「跳走之前这篇正文读到哪儿了」和当前词条一起压进返回栈，
     * 点「返回上一词条」退回来时才不用又从词条顶端重看一遍。
     *
     * 跟 entry:// 链接那条路（下面 post lookup 的 scrollY）是同一件事、同一个来源，
     * 都只能在这边现读 —— 宿主够不着跨域 iframe 的 scrollTop。
     * 顺带说清楚：这是**第三个**要 scrollY 的地方，所以边上那个
     * 只报「在不在顶部」的 scroll-state 布尔量不能顶替它。
     */
    var payload = {
      type: 'selection',
      text: text,
      clipped: clipped,
      empty: text.length === 0,
      scrollY: Math.round(scrollOffset())
    };
    if (rect) payload.rect = rect;
    post(payload);
  }
  function scheduleSelectionReport() {
    if (selectionTimer) clearTimeout(selectionTimer);
    selectionTimer = setTimeout(function () { selectionTimer = null; reportSelection(); }, 60);
  }
  document.addEventListener('selectionchange', scheduleSelectionReport);
  document.addEventListener('mouseup', scheduleSelectionReport, true);
  document.addEventListener('keyup', scheduleSelectionReport, true);
  /*
   * 「用户又要开始选东西了」：宿主拿它清掉「这段文字刚被用掉」的记账，
   * 于是**重新选同一段**也能照常浮出按钮（不然点了「复制」之后再选一次就没反应）。
   * 只认左键：右键是要弹菜单的。
   */
  document.addEventListener('mousedown', function (event) {
    if (event.button !== 0) return;
    post({ type: 'selection-start' });
  }, true);
  window.addEventListener('scroll', function () {
    // 有选区时才需要跟着挪；没选区的话滚动跟它无关（别白刷消息）
    var selection = window.getSelection ? window.getSelection() : null;
    if (selection && selection.rangeCount > 0 && !selection.isCollapsed) scheduleSelectionReport();
  }, { passive: true });

  // 把正文实际高度报给宿主，让展开面板贴合内容而不是留一大片空白。
  // 加 6px 迟滞，避免和宿主的高度调整互相触发造成抖动。
  var lastHeight = -1;
  function reportHeight() {
    try {
      var h = Math.max(
        document.body ? document.body.scrollHeight : 0,
        document.documentElement ? document.documentElement.scrollHeight : 0
      );
      if (Math.abs(h - lastHeight) < 6) return;
      lastHeight = h;
      post({ type: 'height', value: h });
    } catch (e) {}
  }
  window.addEventListener('load', reportHeight);
  window.addEventListener('resize', reportHeight);
  if (window.ResizeObserver) {
    try { new ResizeObserver(reportHeight).observe(document.documentElement); } catch (e) {}
  }
  setTimeout(reportHeight, 50);
  setTimeout(reportHeight, 240);

  /*
   * 正文滚到哪儿了。宿主（悬浮窗）够不着跨域 iframe 的 scrollTop，
   * 但「回顶部」按钮的灰/亮得跟着它变，所以由这边上报。
   * 只在「是否在顶部」这个状态真的翻转时才发，滚动过程中不会刷屏。
   */
  var lastAtTop = null;
  function scrollOffset() {
    return window.scrollY || window.pageYOffset || document.documentElement.scrollTop || 0;
  }
  function reportScrollState() {
    var atTop = scrollOffset() <= 2;
    if (atTop === lastAtTop) return;
    lastAtTop = atTop;
    post({ type: 'scroll-state', atTop: atTop });
  }
  window.addEventListener('scroll', reportScrollState, { passive: true });
  setTimeout(reportScrollState, 60);

  /*
   * 跳到**本文档内**的锚点。这是词典里的「页面内快速导航」：
   * 多词性词条顶部那排 noun / verb，或者搭配框旁边的义项编号，点的都是它。
   *
   * 目标一般写成 <a name=...>（MDict 词条的老习惯，LDOCE5 就是），
   * 也有写成 id 的，所以先按 id 找、再按 name 找。
   * 注意：这段脚本在 C# 侧是用「原样字符串」嵌入的，里面不能出现半角双引号，
   * 字符串一律用单引号。
   */
  function scrollToFragment(fragment) {
    if (!fragment) return false;
    var target = null;
    try {
      target = document.getElementById(fragment);
      if (!target) {
        var named = document.getElementsByName(fragment);
        if (named && named.length) target = named[0];
      }
    } catch (e) {}
    if (!target) return false;
    // 等一帧再滚：词条刚加载完时高度还在变，立刻滚会停在错的位置
    var run = function () {
      try { target.scrollIntoView({ block: 'start' }); }
      catch (e) { try { target.scrollIntoView(); } catch (e2) {} }
    };
    if (window.requestAnimationFrame) window.requestAnimationFrame(run); else run();
    // 地址栏也同步上（除非已经是这个锚点），方便用 :target 之类的样式
    try { if (location.hash !== '#' + fragment) location.hash = fragment; } catch (e) {}
    return true;
  }

  /*
   * 解析链接目标，返回 { word, fragment }：
   *   entry://word          跨词条跳转
   *   entry://word#frag     跨词条跳转 + 落在某个义项上
   *   entry://#frag         本文档内跳转（LDOCE5 的页面内导航就是这种）
   *   #frag                 同上，只是没写 entry://
   * 返回 null 表示这个 href 不归这里管。
   */
  function parseTarget(raw) {
    var body;
    if (raw.indexOf('entry://') === 0) body = raw.slice('entry://'.length);
    else if (raw.charAt(0) === '#') return { word: '', fragment: raw.slice(1) };
    else return null;

    var hash = body.indexOf('#');
    if (hash < 0) return { word: body, fragment: '' };
    return { word: body.slice(0, hash), fragment: body.slice(hash + 1) };
  }

  function isWordChar(ch) {
    if (!ch) return false;
    var code = ch.charCodeAt(0);
    // 撇号（直/弯）与连字符算词内字符，don't / easy-care 才不会被切成两半
    if (code === 39 || code === 45 || code === 8217) return true;
    return /[0-9A-Za-z\u00c0-\u024f\u4e00-\u9fff]/.test(ch);
  }

  /** 鼠标位置下的文字光标（Chromium 两套 API 都试一遍） */
  function caretAt(x, y) {
    try {
      if (document.caretRangeFromPoint) {
        var range = document.caretRangeFromPoint(x, y);
        if (range) return { node: range.startContainer, offset: range.startOffset };
      }
      if (document.caretPositionFromPoint) {
        var position = document.caretPositionFromPoint(x, y);
        if (position) return { node: position.offsetNode, offset: position.offset };
      }
    } catch (e) {}
    return null;
  }

  /*
   * 取鼠标点到的那**一个词**。
   *
   * 牛津高阶的交叉引用里有这么一类链接：href 写的是整条词组（take care of），
   * 而这条词组并不是词库里的词条 —— 它只是别处某个义项里的一段，
   * 所以整条词组查不到（「词组里的每个词各自挂着整条词组的 href」看着就像
   * 好几个可跳转的词连在一起）。这时按用户的直觉退一步：查他点到的那个词。
   */
  function wordAtPoint(x, y, element) {
    var caret = caretAt(x, y);
    if (caret && caret.node && caret.node.nodeType === 3) {
      var text = caret.node.nodeValue || '';
      var start = caret.offset;
      var end = caret.offset;
      while (start > 0 && isWordChar(text.charAt(start - 1))) start--;
      while (end < text.length && isWordChar(text.charAt(end))) end++;
      var hit = text.slice(start, end);
      if (hit) return hit;
    }
    // 拿不到光标位置（例如脚本触发的点击）就退回锚文本的第一个词
    var label = (element && element.textContent ? element.textContent : '').replace(/^\s+|\s+$/g, '');
    var parts = label.split(/[\s,;<>\/]+/);
    for (var i = 0; i < parts.length; i++) {
      if (parts[i]) return parts[i];
    }
    return '';
  }

  document.addEventListener('click', function (event) {
    var node = event.target;
    while (node && node !== document && !(node.tagName === 'A')) node = node.parentNode;
    if (!node || node === document || node.tagName !== 'A') return;
    var raw = node.getAttribute('href') || '';
    if (!raw) return;

    var target = parseTarget(raw);
    if (target) {
      /*
       * 一律 preventDefault。文档里有 <base>，`#xxx` 会被解析成
       * 「base 地址 + #xxx」（那是另一个文档）—— 交给浏览器默认行为的话，
       * 一点页面内导航链接就把整个词条页面导航没了。
       */
      event.preventDefault();
      var word = target.word;
      var fragment = target.fragment;
      try { word = decodeURIComponent(word); } catch (e) {}
      try { fragment = decodeURIComponent(fragment); } catch (e) {}

      /*
       * 两种「不用换词条」的情况，就地滚过去就行：
       *   - 链接里没写词（`entry://#noun`，LDOCE5 的页面内导航就是这种）
       *   - 写的词就是本文档自己（`entry://point#verb` 出现在 point 词条里）
       * 后者要是也交给宿主去查一遍，等于把同一个词条重新加载一次再滚，
       * 白闪一下不说，宿主的「自链接不记录返回栈」那条规则还会把它吞掉。
       */
      if (!word || word === CURRENT_WORD) {
        if (fragment) scrollToFragment(fragment);
        else { try { window.scrollTo(0, 0); } catch (e) {} }
        return;
      }
      /*
       * 词组链接顺手把「鼠标点到的那个词」也带上：整条词组查不到时，
       * 宿主会拿它再查一次（见 wordAtPoint 的说明）。
       *
       * scrollY 是「跳走之前这篇正文读到哪儿了」：宿主把它和当前词条一起压进返回栈，
       * 点「返回」退回来时再还原到这个位置（而不是又回到词条顶端）。
       * 必须在这里现读 —— 宿主够不着跨域 iframe 的 scrollTop，
       * 而边上的 scroll 上报只报「在不在顶部」这一个布尔量。
       */
      var fallbackWord = word.indexOf(' ') >= 0 ? wordAtPoint(event.clientX, event.clientY, node) : '';
      post({
        type: 'lookup',
        word: word,
        fragment: fragment,
        fallbackWord: fallbackWord,
        scrollY: Math.round(scrollOffset())
      });
      return;
    }
    if (isSoundRef(raw)) {
      /*
       * 词典自带的录音（词目发音、例句发音都走这里）。
       * 交给宿主播：宿主页里那个复用的 <audio> 才是全局唯一的播放器，
       * 同一时刻只该有一个声音在响 —— 点例句喇叭会顶掉正在念的词目发音，这是对的。
       */
      event.preventDefault();
      post({ type: 'play-sound', key: soundKeyOf(raw), dictId: DICT_ID, url: soundUrl(raw) });
      return;
    }
    if (/^https?:/i.test(raw)) {
      event.preventDefault();
      post({ type: 'open-external-link', url: raw });
      return;
    }
    // 其余未知协议（如 oalecd://）一律拦截，避免 iframe 被导航走
    if (!/^javascript:/i.test(raw)) event.preventDefault();
  }, true);

  // 宿主（悬浮窗）转发过来的指令
  window.addEventListener('message', function (event) {
    var data = event.data;
    if (!data || data.source !== 'lookup-host') return;
    if (data.type === 'scroll' && typeof data.delta === 'number') {
      try {
        window.scrollBy(0, data.delta);
      } catch (e) {
        try { document.documentElement.scrollTop += data.delta; } catch (e2) {}
      }
      return;
    }
    // 跨词条跳转带了锚点：新词条装好之后由宿主喊一声，落回指定的义项
    if (data.type === 'scroll-to' && typeof data.fragment === 'string') {
      scrollToFragment(data.fragment);
      return;
    }
    /*
     * 从别的词条「返回」回来：还原到当初离开这篇正文时读到的那一行，
     * 而不是又从词条顶端重新看起（位置是宿主返回栈里存着的，见上面 post lookup 的 scrollY）。
     *
     * 为什么要分几次落：图片是后加载的，文档装好那一刻还没长到最终高度，
     * 一次性滚过去可能被「顶」回去一小截。所以先落一次，过一会儿再补两次 ——
     * 但每次补之前先看一眼当前位置是不是还等于我们上次设的值：
     * 不等就说明用户已经自己滚走了，那就不再插手（免得跟他抢滚动条）。
     */
    if (data.type === 'scroll-to-y' && typeof data.y === 'number') {
      var wanted = Math.max(0, data.y);
      var applied = -1;
      var attempts = 0;
      var restoreScroll = function () {
        attempts++;
        if (attempts > 1 && applied >= 0 && Math.abs(scrollOffset() - applied) > 24) return;
        try { window.scrollTo(0, wanted); } catch (e) {}
        // 回读一次：文档不够高时浏览器会把值夹住，记住夹住之后的真实位置
        applied = scrollOffset();
        setTimeout(reportScrollState, 30);
      };
      restoreScroll();
      setTimeout(restoreScroll, 180);
      setTimeout(restoreScroll, 420);
      return;
    }
    // 宿主点了「回顶部」
    if (data.type === 'scroll-top') {
      try { window.scrollTo({ top: 0, behavior: 'smooth' }); }
      catch (e) { try { window.scrollTo(0, 0); } catch (e2) {} }
      // 平滑滚动是异步的，先按「已经在顶部」回报一次，滚动结束后的 scroll 事件会再校准
      setTimeout(reportScrollState, 60);
      setTimeout(reportScrollState, 400);
      return;
    }
    /*
     * 宿主送来「这个词没查到，还能怎么办」的几个出路（见 renderChips）。
     *
     * 为什么按钮长在**词条正文里**、而不是宿主页里：这一排东西是接着
     * 「未在《…》中找到 X」那句提示往下读的，长在同一篇文档里高度才会被一起量进去
     * （reportHeight 量的是整个文档），也不会把正文挤掉一截。
     * 更要紧的是**滚动位置**：点击时要把「这篇读到哪儿了」一起报给宿主压栈，
     * 而宿主够不着跨域 iframe 的 scrollTop —— 文档里的按钮顺手就带上了（和链接点击同一个来源）。
     */
    if (data.type === 'chips') {
      renderChips(data.items);
      return;
    }
  });

  /* ------------------------------ 出路按钮 ------------------------------ */

  var chips = document.getElementById('lookupChips');
  function renderChips(items) {
    if (!chips) return;
    while (chips.firstChild) chips.removeChild(chips.firstChild);
    if (!items || !items.length) {
      chips.hidden = true;
      reportHeight();
      return;
    }
    for (var i = 0; i < items.length; i++) {
      (function (item) {
        if (!item || !item.label) return;
        var button = document.createElement('button');
        button.type = 'button';
        button.className = 'lookup-chip';
        button.textContent = item.label;
        button.setAttribute('data-chip-action', item.action || '');
        button.title = item.hint || item.label;
        /*
         * 用 mousedown 而不是 click：与正文里那些 entry:// 链接同一条规矩 ——
         * click 要等 mouseup，中间那一下可能先把选区/焦点弄乱；
         * 而这里点下去就是要跳走，早一步决定没有副作用。
         */
        button.addEventListener('mousedown', function (event) {
          event.preventDefault();
          post({
            type: 'chip',
            action: item.action || '',
            word: item.word || '',
            scrollY: Math.round(scrollOffset())
          });
        });
        chips.appendChild(button);
      })(items[i]);
    }
    chips.hidden = false;
    // 按钮多一行，文档就高了一点 —— 重新量一次，别让面板把这一排裁掉
    reportHeight();
  }
})();
";
}
