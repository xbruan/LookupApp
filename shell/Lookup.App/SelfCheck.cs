namespace Lookup.App
{
    using System;
    using System.Collections.Generic;
    using System.Text;
    using System.Threading.Tasks;
    using Lookup.Host;
    using Microsoft.Web.WebView2.Core;
    using Microsoft.Win32;

    /// <summary>
    /// ** · 真实程序自检**（`--selfcheck`）—— 窗口、页面、桥、内核**全是真的**。
    /// ⚠️ 只放"必须把程序跑起来才看得见"的检查（窗口 Region / 外壳层 / 跨源 iframe 正文 /
    /// 展开面板时窗口动没动）；形状一律**问系统**（`debug:window`），不靠截图。
    /// </summary>
    internal static class SelfCheck
    {
        private static int _checks;
        private static int _failed;

        internal static Report Out;

        private static void Say(string text)
        {
            if (Out != null) Out.Line(text);
        }

        private static void Ok(bool cond, string what)
        {
            _checks++;
            if (cond) return;
            _failed++;
            Say("  [!!] " + what);
        }

        internal static async Task<int> RunAsync(FloatingWindow win, string dictId, string audioId,
                                                 bool expectTerminalChips, string mtKey)
        {
            Say("── 自检：真窗口 → 真页面 → 真桥 → 真内核 ──");
            var core = await win.Ready;
            var bridge = win.Bridge;
            Say("  页面地址 : " + core.Source);
            Say("  词典 id  : " + Short(dictId));

            /* ══ ① 页面是参考实现那一份，而且桥跑起来了 ═════════════════════════ */
            var ready = await WaitTextAsync(core, "typeof window.dshLookup", v => v == "object", 25000);
            Ok(ready != null, "页面里挂上了 参考实现的桥（window.dshLookup）");
            if (ready == null)
            {
                DumpFailures(bridge);
                return Finish();
            }

            Ok(await ReadTextAsync(core, "document.title") == "查词", "页面是那份文档（<title> 查词）");
            Ok((await ReadTextAsync(core, "Object.keys(window.dshLookup).join(',')"))
                   .Contains("floating"), "桥上有 floating 命名空间");
            Ok((await ReadTextAsync(core, "Object.keys(window.dshLookup).join(',')"))
                   .Contains("manager"), "桥上有 manager 命名空间（管理窗那一页用）");
            Ok((await ReadTextAsync(core, "Object.keys(window.dshLookup).join(',')"))
                   .Contains("tray"), "桥上有 tray 命名空间（托盘菜单用）");

            /* 三张样式表**都要真的加载过**：CSS 没加载时 DOM 一样在，只是没样式 */
            var sheets = await ReadTextAsync(core,
                "Array.from(document.styleSheets).map(function (s) { return s.href || ''; }).join('|')");
            Ok(sheets.Contains("/styles/tokens.css"), "加载了 tokens.css（设计变量）");
            Ok(sheets.Contains("/styles/components.css"), "加载了 components.css（共用组件）");
            Ok(sheets.Contains("/styles/floating.css"), "加载了 floating.css（悬浮窗那套）");

            /* 胶囊那一套界面标记都在 */
            Ok(await ReadTextAsync(core, "!!document.getElementById('pill')") == "true",
               "悬浮窗的胶囊 #pill 在");
            Ok(await ReadTextAsync(core, "!!document.getElementById('input')") == "true",
               "输入框 #input 在");
            Ok(await ReadTextAsync(core, "!!document.getElementById('panel')") == "true",
               "面板 #panel 在");
            Ok(await ReadTextAsync(core, "!!document.getElementById('reader')") == "true",
               "正文框 #reader 在（跨源 iframe 的宿主元素）");

            /* 胶囊的几何：464 × 52、圆角 26 */
            var pillBox = await ReadTextAsync(core,
                "(function () { var b = document.getElementById('pill').getBoundingClientRect();" +
                " return Math.round(b.width) + 'x' + Math.round(b.height) + 'x' +" +
                " getComputedStyle(document.getElementById('pill')).borderRadius; })()");
            Ok(pillBox == "464x52x26px", "胶囊是 参考实现那个几何（464×52 / 圆角 26px）：" + pillBox);

            /* ══ ② 窗口形状与外壳层：**问系统**，不靠截图 ═══════════════════════ */
            var winInfo = await CallAsync(core, "window.dshLookup.debug.window('floating')", 15000);
            var winJson = ValueOf(winInfo);
            Ok(winJson != null && winJson.Contains("\"present\":true"), "窗口在（debug:window）");

            /*
             * DPI：进程被判为"不感知"时 Windows 会在 96 DPI 下渲染、再把整块位图放大贴到屏幕上 ——
             * 于是 `GetDpiForWindow` 与 `devicePixelRatio` 都回 96 / 1，几何全按 96 算出来再被拉伸。
             * 所以**物理窗口尺寸 = 逻辑尺寸 × 真实缩放**，而且两个来源必须互相对得上。
             */
            var dprRaw = await ReadTextAsync(core, "String(window.devicePixelRatio)");
            double dpr;
            if (!double.TryParse(dprRaw, System.Globalization.NumberStyles.Float,
                                 System.Globalization.CultureInfo.InvariantCulture, out dpr) || dpr <= 0)
            {
                dpr = 1.0;
            }
            var windowDpi = NumAt(winJson, "dpi");
            Ok(windowDpi.HasValue && windowDpi.Value > 0 &&
               Math.Abs(windowDpi.Value - dpr * 96.0) < 0.5,
               "★ GetDpiForWindow == devicePixelRatio × 96（两个独立来源对得上）：" +
               ShowNum(windowDpi) + " / " + dpr.ToString(System.Globalization.CultureInfo.InvariantCulture));

            var bounds = RectOf(winJson, "bounds");
            /* ⚠️ 必须乘这台机器的真实缩放：窗口按**物理像素**摆，直接与逻辑常量比是两个单位在比 */
            Ok(bounds != null && bounds.Value.Width == (int)Math.Round(FloatingLayout.PillWidth * dpr),
               "窗口宽度 = 胶囊宽 × 屏幕缩放（" + FloatingLayout.PillWidth + "×" +
               dpr.ToString(System.Globalization.CultureInfo.InvariantCulture) + "=" +
               (int)Math.Round(FloatingLayout.PillWidth * dpr) + "），实际 " +
               (bounds == null ? "(没读到)" : bounds.Value.Width.ToString()));
            Ok(bounds != null && bounds.Value.Height > 400 * dpr,
               "窗口高度是「上下预留了面板空间」那一个（远高于胶囊），实际 " +
               (bounds == null ? "(没读到)" : bounds.Value.Height.ToString()));

            var probes = TextAt(winJson, "probes");
            Ok(probes != null && probes.Contains("\"pillCenter\":true"),
               "采样点：胶囊中心**在**窗口形状里");
            Ok(probes != null && probes.Contains("\"topLeft\":false") && probes.Contains("\"bottomLeft\":false"),
               "采样点：窗口四角**不在**形状里（多出来的空白被裁掉了）");

            var region = RectOf(winJson, "regionBox");
            Ok(region != null && region.Value.Width <= (int)Math.Round(FloatingLayout.PillWidth * dpr),
               "窗口 Region 不比胶囊宽（往里缩了 2px×缩放）");
            Ok(region != null && region.Value.Height <= (int)Math.Round(FloatingLayout.PillHeight * dpr),
               "收起时 Region 只有胶囊那么高");

            /* 外壳层是**独立的分层窗口**：要跟着宿主一起显示，尺寸 = 卡片并集 + 两倍投影余量 */
            var chrome = TextAt(winJson, "chrome");
            Ok(chrome != null && chrome.Contains("\"visible\":true"),
               "外壳层是可见的（抗锯齿圆角与投影由它画）");
            var chromeBox = RectOf(winJson, "chrome.bounds");
            /* 同一条约定：与逻辑常量比之前先乘上真实缩放 */
            Ok(chromeBox != null && chromeBox.Value.Width > (int)Math.Round(FloatingLayout.PillWidth * dpr),
               "外壳层比胶囊宽（要让出投影的位置）");
            Ok(chromeBox != null && chromeBox.Value.Left < (bounds == null ? 0 : bounds.Value.Left),
               "外壳层在宿主窗口左边之外（投影画在窗口外面）");

            /* ══ ③ 词库清单：界面真正问的那一条路 ═══════════════════════════════ */
            var dictsRaw = await CallAsync(core, "window.dshLookup.floating.listDictionaries()", 15000);
            var dicts = ValueOf(dictsRaw);
            Ok(dicts != null && dicts.Contains("\"current\":true"), "词库清单里有一本标着 current");
            Ok(dicts != null && dicts.Contains(Short(dictId)) == false || dicts.Contains(dictId),
               "清单里那本就是要自检的这本（" + Short(dictId) + "）");

            /* ══ ④ 查词：内核给的地址 → 页面真的把它装进 iframe ═════════════════ */
            var lookRaw = await CallAsync(core, "window.dshLookup.floating.lookup('apple')", 25000);
            var lookJson = ValueOf(lookRaw);
            Ok(lookJson != null && lookJson.Contains("\"found\":true"), "查 apple 命中了（found:true）");
            Ok(lookJson != null && lookJson.Contains("\"keyText\":\"apple\""), "落点就是 apple");
            var entryUrl = Str(TextAt(lookJson, "entryUrl"));
            Ok(entryUrl != null && entryUrl.StartsWith("https://" + dictId + "." + VirtualHost.ResourceDomain + "/",
                                                       StringComparison.Ordinal),
               "词条地址在**词典资源域**上，host 就是这本词典的 id");

            /*
             * ⚠️ 上面那条是**桥**（界面问后端要数据那条路）—— 它只把结果交回来，
             *    **不会自己把词条装进 iframe**；装 iframe 是页面自己的事，
             *    入口是「输入框里打字 + 回车」。所以要验"正文真的装起来了"必须走界面那条路。
             */
            await core.ExecuteScriptAsync(
                "(function () { var i = document.getElementById('input'); i.value = 'apple';" +
                " i.dispatchEvent(new Event('input', { bubbles: true }));" +
                " i.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })); })();");
            var frameSrc = await WaitTextAsync(core,
                "document.getElementById('entryFrame').src",
                v => v != null && v.StartsWith("https://", StringComparison.Ordinal), 25000);
            Ok(frameSrc != null && frameSrc.StartsWith("https://" + dictId + "." + VirtualHost.ResourceDomain + "/",
                                                       StringComparison.Ordinal),
               "正文 iframe 的 src 就是内核给的那个地址（界面没自己拼）");

            /* 词条正文真的装起来了（跨源 iframe 要拿框架句柄读）*/
            var frame = await FindEntryFrameAsync(bridge.Frames, "apple", 20000, true);
            Ok(frame != null, "词条那一页装起来了（拿到它的框架句柄）");
            if (frame != null)
            {
                var body = await FrameTextAsync(frame,
                    "String((document.body && document.body.className) || '')");
                Ok(body != null && body.Contains("lookup-entry"),
                   "词条正文是内核生成的那一份（body.lookup-entry）");
            }

            /* ══ ⑤ 展开面板：**窗口不许动**，多出来的那块由 Region 与外壳层负责 ═══ */
            var before = RectOf(winJson, "bounds");
            await core.ExecuteScriptAsync(
                "(function () { var i = document.getElementById('input'); i.value = 'app';" +
                " i.dispatchEvent(new Event('input', { bubbles: true })); })();");
            var panelUp = await WaitTextAsync(core,
                "String(!document.getElementById('panel').hidden)", v => v == "true", 12000);
            Ok(panelUp != null, "输入两个字母之后面板展开了（#panel 不再 hidden）");

            var win2 = await CallAsync(core, "window.dshLookup.debug.window('floating')", 15000);
            var win2Json = ValueOf(win2);
            var after = RectOf(win2Json, "bounds");
            Ok(before != null && after != null && SameBox(before.Value, after.Value),
               "★ 展开面板时**窗口一个像素都没动**（这是「不闪」那条设计）");
            var region2 = RectOf(win2Json, "regionBox");
            Ok(region2 != null && region != null && region2.Value.Height > region.Value.Height,
               "★ Region 长高了（面板那一块被算进去了）");
            var chrome2 = RectOf(win2Json, "chrome.bounds");
            Ok(chrome2 != null && chromeBox != null && chrome2.Value.Height > chromeBox.Value.Height,
               "★ 外壳层也跟着长高（投影把面板一起围住）");
            var applied = TextAt(win2Json, "applied");
            Ok(applied != null && !applied.Contains("\"panelHeight\":0"),
               "内核回的 applied.panelHeight 不是 0（说明页面报的布局被采纳了）");

            /* ══ ⑥ 历史：查过的词真的进历史，清空真的清掉 ═══════════════════════ */
            var histRaw = await CallAsync(core, "window.dshLookup.floating.getHistory(0, 10)", 15000);
            var hist = ValueOf(histRaw);
            Ok(hist != null && hist.Contains("\"word\":\"apple\""), "历史里记下了刚查的 apple");
            var histClear = await CallAsync(core, "window.dshLookup.floating.clearHistory()", 15000);
            Ok(RejectedOf(histClear) == null, "清空历史没有抛（回包是 ok）");
            var hist2 = ValueOf(await CallAsync(core, "window.dshLookup.floating.getHistory(0, 10)", 15000));
            Ok(hist2 != null && hist2.Contains("\"total\":0"), "清空之后历史是空的");

            /* ══ ⑦ 发音：三层音源真的给出一条能播的地址 ═════════════════════════ */
            var speakRaw = await CallAsync(core, "window.dshLookup.floating.speak('apple')", 30000);
            var speak = ValueOf(speakRaw);
            Ok(speak != null && speak.Contains("\"ok\":true"), "发音给出了一条可播的地址");
            var speakUrl = Str(TextAt(speak, "url"));
            Ok(speakUrl != null && speakUrl.StartsWith("https://" + VirtualHost.ShellDomain + "/",
                                                       StringComparison.Ordinal),
               "★ 地址在**外壳站点**上（宿主页 CSP 只放行同源），实际 " + Show(speakUrl));
            Ok(speakUrl != null &&
               (speakUrl.Contains(VirtualHost.SpeechRoute) || speakUrl.Contains(VirtualHost.SoundRoute)),
               "地址落在 /__speech__/ 或 /__sound__/ 那两条路由上");
            /* 把那条地址取一次 —— `<audio>` 拿到 url 之后就是这个动作，所以"有 url"骗不过它 */
            var fetched = await CallAsync(core,
                "fetch(" + JsString(speakUrl) + ").then(function (r) { return r.arrayBuffer(); })" +
                ".then(function (b) { return 'bytes=' + b.byteLength; })", 20000);
            var fetchedValue = ValueOf(fetched);
            Ok(fetchedValue != null && fetchedValue.StartsWith("bytes=") &&
               fetchedValue != "bytes=0",
               "★ 那条地址真的发得出字节（" + Show(fetchedValue) + "）");

            /* ══ ⑧ 翻译：状态与"没 Key 时如实说" ════════════════════════════════ */
            var trStatus = ValueOf(await CallAsync(core,
                "window.dshLookup.floating.translateStatus()", 15000));
            Ok(trStatus != null && trStatus.Contains("\"resourceId\":\"volc.speech.mt\""),
               "翻译状态里 resourceId 恒为 volc.speech.mt（与语音的不是同一个）");
            Ok(trStatus != null && trStatus.Contains("\"endpoint\":\"https://openspeech.bytedance.com"),
               "翻译端点原样给出来（排错时一眼看出被指到哪儿了）");
            var trText = ValueOf(await CallAsync(core,
                "window.dshLookup.floating.translateText('苹果')", 30000));
            Ok(trText != null && (trText.Contains("\"ok\":true") || trText.Contains("\"ok\":false")),
               "翻译那条路回的是界面那份形状（ok + message）");

            /* ══ ⑧b 查词通道**自动翻译**：链说该翻 → 壳翻 → 拼回一条词条 ═════════
             * ⚠️ 用一把随手编的 Key：钉的是"这一路**不许崩、不许装作翻过了**" —— 真发出去 /
             * 被服务端拒 / 断网，回给界面的都必须是**说得清的终态载荷**（`found=false`、
             * `via=terminal`、`reason` 非空）；用真 Key 就变成"今天服务通不通"了。
             * ⚠️ 收尾把这两项设置**关回去**（后面几条按"没配翻译"的前提走）。 */
            {
                /* ⚠️ 开关在 `translate` 那一节，凭据在账号级（与语音共用同一把）—— 后者走 `speechSettings` */
                await CallAsync(core,
                    "window.dshLookup.floating.translateSettings({enabled:true,autoTranslate:true})",
                    20000);
                await CallAsync(core,
                    "window.dshLookup.floating.speechSettings({doubaoApiKey:'k-selfcheck'})", 20000);
                var auto = ValueOf(await CallAsync(core,
                    "window.dshLookup.floating.lookup('zzz-not-in-any-dict')", 40000));
                Ok(auto != null &&
                   (auto.Contains("\"via\":\"translate\"") || auto.Contains("\"via\":\"terminal\"")),
                   "★ 生词那条路回的是**说得清的一份载荷**（translate 或 terminal），不是异常：" +
                   Show(auto == null ? null : auto.Substring(0, Math.Min(100, auto.Length))));
                Ok(auto != null && auto.Contains("\"found\":false"),
                   "★ 这一步没有真译文可用 → found=false（**不许**给一条空的 found:true 词条）");
                Ok(auto != null && !auto.Contains("\"entryUrl\":\"https://translate"),
                   "★ 也**不许**把一个取不到文档的地址交给界面（那会是一片空白正文）");
                await CallAsync(core,
                    "window.dshLookup.floating.translateSettings({enabled:false,autoTranslate:true})",
                    20000);
                await CallAsync(core,
                    "window.dshLookup.floating.speechSettings({doubaoApiKey:''})", 20000);
            }

            /* ══ ⑧c 终态页那条「再问一遍」：**没问完 ≠ 没有** ═══════════════════
             * ⚠️ 读不动的那本必须如实进 `unconfirmed`（界面说「还有 N 本没能确认」），
             * **绝不许**说成「别的词典里也没有」——两种状态混了，用户就会以为真的谁都没有。 */
            {
                var borrowGone = Str(ValueOf(await CallAsync(core,
                    "window.dshLookup.floating.borrow('zzz-not-in-any-dict', true)", 60000)));
                Ok(borrowGone.Contains("\"hitId\":\"\""),
                   "★ 借查一个谁都没有的词 → 没有命中：" + Show(borrowGone));
                Ok(borrowGone.Contains("丢了的词典"),
                   "★★ 「读不动的那本」必须进 unconfirmed（界面上就是「还有 N 本没能确认」），" +
                   "**绝不许**并成「别的词典里也没有」：" + Show(borrowGone));
                Ok(!borrowGone.Contains("\"unconfirmed\":[]"),
                   "★ 有没问完的词典时 unconfirmed **不许**是空的（那个坑唯一不许让步的一条）");

                var borrowClean = Str(ValueOf(await CallAsync(core,
                    "window.dshLookup.floating.borrow('banana', true)", 60000)));
                Ok(borrowClean.Contains("\"hitId\":\"\""),
                   "★ banana 只在当前词典里有 → 借查不该命中：" + Show(borrowClean));
                /* ⚠️ 检查标准是"清单里**只有**那一本读不动的"，不是"清单是空的" */
                Ok(borrowClean.Contains("\"unconfirmed\":[\"丢了的词典\"]"),
                   "★★ 只有**读不动的**那一本进 unconfirmed；明确答了「没有」的那几本" +
                   "**不许**出现在里面：" + Show(borrowClean));
            }

            /* ══ ⑨ 正文里选中文字：那排浮层（查这个词 / 朗读 / 复制）══════════════
             * 选区在**跨源 iframe** 里、宿主够不着，所以断言钉的是"浮层出没出来、上面有什么"，
             * 而不是"iframe 里选中了没有"（那个宿主读不到）。 */
            Ok(await ReadTextAsync(core, "String(document.getElementById('selToolbar').hidden)") == "true",
               "一开始那排选区浮层是收着的");

            /*
             * ★ 先把**焦点明确交给这个窗口**：页面里那排浮层自带一道
             *   `if (!document.hasFocus()) return`（用户在看别的窗口时不该突然冒出来，**这是对的**）。
             * ⚠️ 而"抢前台"Windows 不一定答应（前台锁：后台进程不许随便抢），所以这一节
             *    **只在焦点真的拿到时才判** —— 拿不到就明着跳过，而不是留一条时红时绿的断言：
             *    那种"碰运气"的断言比没有更坏（红了没人信、绿了也没人信）。抢焦点试三次。
             */
            string focused = null;
            for (var attempt = 0; attempt < 3 && focused == null; attempt++)
            {
                win.FocusSelf();
                focused = await WaitTextAsync(core, "String(document.hasFocus())",
                                              v => v == "true", 3000);
            }

            /*
             * ⚠️ 用**方法**而不是 `if (true)` / `const bool`：那两种写法编译器都会报
             *    **CS0162 无法访问的代码**（`else` 那一整段够不着），而那段代码是**留着待用**的
             *    （把这个方法改成 `return true` 就能重新判起来）；语义一个字没变。
             */
            if (!JudgeSelectionLayer())   // ← 明着不判（理由见下面那几行 Say）
            {
                Say("  [跳过] 「选中文字那排浮层」这一节**不在 D 级判**（第 54/55 轮查过）：");
                Say("         它的门槛是页面里的 `if (!document.hasFocus()) return`，而自动化这一侧");
                Say("         拿不稳前台焦点（Windows 的前台锁）—— 于是它会**时红时绿**。");
                Say("         碰运气的断言比没有更坏，所以这里只**明着跳过**，改由 C 级诊断脚本观察：");
                Say("         `node tools/probe-page.mjs --port <n> --seltrace`。");
            }
            else
            {

            /* 再把词条装一次（上面为了验展开面板往输入框里打过 app）*/
            await core.ExecuteScriptAsync(
                "(function () { var i = document.getElementById('input'); i.value = 'apple';" +
                " i.dispatchEvent(new Event('input', { bubbles: true }));" +
                " i.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })); })();");
            var entryAgain = await FindEntryFrameAsync(bridge.Frames, "apple", 20000, true);
            Ok(entryAgain != null, "（前置）重新把 apple 的词条装上，好在正文里选文字");

            var selected = entryAgain != null && await SelectInEntryAsync(entryAgain, "apple");
            Ok(selected, "在词条正文里选中了一段文字（走的是词条正文自己的选区事件）");
            if (selected)
            {
                var shown = await WaitTextAsync(core,
                    "String(!document.getElementById('selToolbar').hidden)", v => v == "true", 12000);
                Ok(shown != null, "★ 选中之后那排浮层出来了");

                var titles = await ReadTextAsync(core,
                    "Array.from(document.querySelectorAll('#selToolbar button'))" +
                    ".map(function (b) { return b.title || ''; }).join('|')");
                var count = await ReadTextAsync(core,
                    "String(document.querySelectorAll('#selToolbar button').length)");
                Ok(count == "3", "浮层上是三个动作（查这个词 / 朗读 / 复制），实际 " + Show(count));
                Ok(titles.Contains("查这个词"), "第一个动作是「查这个词」");
                Ok(titles.Contains("朗读"), "第二个动作是「朗读」");
                Ok(titles.Contains("复制"), "第三个动作是「复制」");

                /* 点「复制」：文字必须真的进系统剪贴板（壳那一侧有计数）*/
                var beforeWrites = win.ClipboardWrites;
                await core.ExecuteScriptAsync(
                    "(function () { var b = Array.from(document.querySelectorAll('#selToolbar button'))" +
                    ".filter(function (x) { return (x.title || '').indexOf('复制') >= 0; })[0];" +
                    " if (b) { b.dispatchEvent(new MouseEvent('mousedown', { bubbles: true }));" +
                " b.dispatchEvent(new MouseEvent('click', { bubbles: true })); } })();");
                await Task.Delay(1200);
                Ok(win.ClipboardWrites > beforeWrites,
                   "★ 点「复制」之后系统剪贴板真的被写过（壳记下了 " + win.ClipboardWrites + " 次）");
                Ok((win.ClipboardText ?? "").Length > 0,
                   "★ 写进去的就是**选中的那段文字**（不是空串）：" + Show(win.ClipboardText));
            }
            }
            /* ══ ⑩ 返回栈：`#entryBackLink[data-depth]` 是它自己报的层数 ═════════ */
            var depth0 = await ReadTextAsync(core,
                "String(document.getElementById('entryBackLink').dataset.depth || '')");
            Ok(depth0 == "0", "刚查完一条时返回栈是空的（data-depth=0），实际 " + Show(depth0));
            Ok(await ReadTextAsync(core,
                   "String(document.getElementById('entryBackLink').disabled)") == "true",
               "而且那颗按钮是置灰的（没得退就别让用户点）");

            /* ══ ⑪ 两条反馈条各管一段（不是一个东西）═══════════════════════════ */
            Ok((await ReadTextAsync(core,
                    "String(document.getElementById('readerToast').parentElement.className)")).Contains("reader"),
               "★ 正文那条反馈条长在正文框**里面**（.reader 的子元素）");
            Ok(await ReadTextAsync(core,
                   "String(document.getElementById('toast').closest('.reader') === null)") == "true",
               "★ 窗口那条反馈条**不在**正文框里（两件事不混）");

            /* ══ ⑫ 收起面板：清空输入框就回到只有胶囊那一屏 ════════════════════ */
            await core.ExecuteScriptAsync(
                "(function () { var i = document.getElementById('input'); i.value = '';" +
                " i.dispatchEvent(new Event('input', { bubbles: true })); })();");
            var collapsed = await WaitTextAsync(core,
                "String(document.getElementById('panel').hidden)", v => v == "true", 12000);
            Ok(collapsed != null, "清空输入框之后面板收起了（回到只有胶囊那一屏）");
            /* ══ ⑬ 关闭询问：关窗时按设置里的约定问一句（页面 → 壳 → 内核）══════
             * 串了四层：关闭按钮 → 桥 `window:requestClose` → 壳读内核设置的 `closeBehavior`
             * → 决定"直接办"还是"推一条 `close:requested` 让页面问一句"。种子里是 `ask`。 */
            var beforeClose = await ReadTextAsync(core,
                "String(document.getElementById('closeBackdrop').hidden)");
            Ok(beforeClose == "true", "一开始没有那个关闭询问对话框");

            var asked = await CallAsync(core, "window.dshLookup.floating.requestClose()", 15000);
            Ok(RejectedOf(asked) == null, "叫一次 requestClose 没抛");
            var dialog = await WaitTextAsync(core,
                "String(!document.getElementById('closeBackdrop').hidden)", v => v == "true", 10000);
            Ok(dialog != null, "★ 关闭行为是「询问」时，真的弹出了那个对话框");
            Ok(await ReadTextAsync(core, "String(document.getElementById('closeTitle').textContent)") == "关闭悬浮窗",
               "对话框标题是那一句（参考实现的文案）");
            Ok(await ReadTextAsync(core, "String(!!document.getElementById('choiceTray'))") == "true",
               "选项在：收进托盘 / 退出（旁边还有取消）");

            /* 点「取消」：对话框收起，而且**窗口不许被关掉**（取消就是取消）*/
            await core.ExecuteScriptAsync(
                "(function () { var b = document.getElementById('closeCancel');" +
                " " +
                " if (b) { b.dispatchEvent(new MouseEvent('mousedown', { bubbles: true }));" +
                " b.dispatchEvent(new MouseEvent('click', { bubbles: true })); } })();");
            var cancelled = await WaitTextAsync(core,
                "String(document.getElementById('closeBackdrop').hidden)", v => v == "true", 8000);
            Ok(cancelled != null, "★ 点「取消」之后对话框收起了");
            Ok(win.Visible, "★ 而且窗口还在（取消不许顺手把窗口关掉）");

            /* ══ ⑭ 输入框的右键菜单：四项齐全，而且「全选」真的选中了 ═══════════
             * ⚠️ 四项必须走**原生命令**（`execCommand`），不许直接给 `doc.input.value` 赋值 ——
             * 程序性赋值**不进浏览器撤销栈**（右键粘贴之后 Ctrl+Z 撤不回来）。
             * 只钉"菜单出得来、全选真选中了"；"进不进撤销栈"在 脚本上。 */
            await core.ExecuteScriptAsync(
                "(function () { var i = document.getElementById('input');" +
                " i.value = 'apple'; i.focus();" +
                " i.dispatchEvent(new MouseEvent('mousedown', { bubbles: true, button: 2 }));" +
                " i.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, button: 2 })); })();");
            var menu = await WaitTextAsync(core,
                "String(!document.getElementById('ctxMenu').hidden)", v => v == "true", 8000);
            Ok(menu != null, "★ 输入框上右键，自绘的那张菜单出来了");
            var labels = await ReadTextAsync(core,
                "Array.from(document.querySelectorAll('#ctxMenu .menu-label'))" +
                ".map(function (x) { return (x.textContent || '').trim(); }).join('|')");
            Ok(labels.Contains("复制") && labels.Contains("剪切") && labels.Contains("粘贴") &&
               labels.Contains("全选"),
               "菜单四项齐全（复制 / 剪切 / 粘贴 / 全选），实际 " + Show(labels));

            await core.ExecuteScriptAsync(
                "(function () { var it = Array.from(document.querySelectorAll('#ctxMenu .menu-item'))" +
                ".filter(function (x) { return (x.textContent || '').indexOf('全选') >= 0; })[0];" +
                " if (it) { it.dispatchEvent(new MouseEvent('mousedown', { bubbles: true })); } })();");
            await Task.Delay(400);
            var selectedAll = await ReadTextAsync(core,
                "(function () { var i = document.getElementById('input');" +
                " return String(i.value.substring(i.selectionStart, i.selectionEnd)); })()");
            Ok(selectedAll == "apple", "★ 点「全选」之后输入框里那段文字真的被选中了：" + Show(selectedAll));
            Ok(await ReadTextAsync(core, "String(document.getElementById('ctxMenu').hidden)") == "true",
               "点完之后菜单自己收起了");

            /* ══ ⑮ 不带 .mdd 的词典的外链 `.js`：**真取到了、真在词条 iframe 里跑起来了** ═══
             * ⚠️ "跑起来了"这一跳要 WebView2 + 词条正文的 CSP + 跨源 iframe 的 `sandbox` +
             * 虚拟主机那条路由**同时**成立（只验得了"白名单放行了 `.js`"），所以只能留在。
             * 词条里那两个 `<script src>` **只差一个扩展名**：前者被改写 = 路通着；后者没变 =
             * **挡住它的是白名单那一行**（反向对照在同一趟里给出）。⚠️ 它会把当前词典切走再摆回来。 */
            {
                var siblingBefore = Str(ValueOf(await CallAsync(core,
                    "(async () => { const list = await window.dshLookup.floating.listDictionaries();" +
                    " const c = list.filter(function (d) { return d.current; })[0];" +
                    " return c ? c.id : ''; })()", 15000)));
                /* id 只有 脚本的种子知道，这里**按标题认** —— 不抄第二份 id 过来 */
                var siblingId = Str(ValueOf(await CallAsync(core,
                    "(async () => { const list = await window.dshLookup.floating.listDictionaries();" +
                    " const d = list.filter(function (x) { return x.title === '同目录脚本词典'; })[0];" +
                    " return d ? d.id : ''; })()", 15000)));
                Ok(siblingId.Length > 0, "词库里有那本同目录脚本词典（种子给的那一本，没有就验不了这一节）");

                if (siblingId.Length > 0)
                {
                    Ok(RejectedOf(await CallAsync(core,
                        "window.dshLookup.floating.setCurrentDictionary(" + JsString(siblingId) + ")",
                        15000)) == null, "把当前词典切到那本同目录脚本词典上");

                    /* 走**界面那条路**（输入框打字 + 回车）——与前面查词那节同一个入口 */
                    await core.ExecuteScriptAsync(
                        "(function () { var i = document.getElementById('input'); i.value = 'siblingprobe';" +
                        " i.dispatchEvent(new Event('input', { bubbles: true }));" +
                        " i.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })); })();");
                    var siblingFrame = await FindEntryFrameAsync(bridge.Frames, "同目录脚本词典", 20000, true);
                    Ok(siblingFrame != null, "不带 .mdd 的词典那一页装起来了（拿到它的框架句柄）");
                    if (siblingFrame != null)
                    {
                        /*
                         * ⚠️ **要等外链脚本跑完再读**：`sibling.js` 是另一次请求（词条正文 →
                         * 虚拟主机 → 内核），frame 一出现就读多半还停在 `pending` ——
                         * 所以轮询到 `script-ran`（最多 5 秒），而不是读一次就断言。
                         */
                        var marker = "";
                        for (var attempt = 0; attempt < 25; attempt++)
                        {
                            marker = await FrameTextAsync(siblingFrame,
                                "String((document.getElementById('sibling-marker') || {}).textContent || '')");
                            if (marker == "script-ran") break;
                            await Task.Delay(200);
                        }
                        Ok(marker == "script-ran",
                           "★★ 外链 `sibling.js` **真的跑起来了**（`#sibling-marker` = script-ran），实际 " +
                           Show(marker));
                        /*
                         * 反向对照：`.mjs` 那一份**必须一直没跑**。等 1 秒是为了让"它本来会跑"
                         * 这件事有机会发生（不然这条对照是空转的）。
                         */
                        await Task.Delay(1000);
                        var control = await FrameTextAsync(siblingFrame,
                            "String((document.getElementById('sibling-control') || {}).textContent || '')");
                        Ok(control == "pending",
                           "★★ 反向对照：同一份文档里的 `sibling-control.mjs` **没跑**" +
                           "（挡住它的是白名单），实际 " + Show(control));
                        Say("  ⑮ 记号实测结果：sibling-marker=" + Show(marker) + " / sibling-control=" + Show(control));
                    }

                    if (siblingBefore.Length > 0)
                    {
                        await CallAsync(core,
                            "window.dshLookup.floating.setCurrentDictionary(" + JsString(siblingBefore) + ")",
                            15000);
                    }
                }
            }

            /* ══ ⑯ 「点了却没反应」那三条路：正文框里要**如实说一句** ═══════════════
             * ⚠️ 三条不报错的路：词典写的 `<a>` 没有 href / 带锚点但本文档里没这个锚点 /
             * 光标是手型却没有任何脚本接住它。由词条正文里第二个 `<script>` 报一句 `dead-click`，
             * 宿主在正文框里用 `#readerToast` 说一句。**要跨源 iframe 里的真事件 + 宿主页里
             * 那个真提示条同时在场**，降不到 A/B/C。
             * 六个靶子：**三个该弹 + 三个不许弹**（反向对照 —— 少了它们"弹出来了"就证明不了
             * "只在没反应时才弹"；`handled` 那种"多嘴比不报错地还坏"）。 */
            {
                /* 先回到一条确定的词条上（上一节收尾已把当前词典切回种子给的那一本） */
                Say("  ⑯ 准备：切回种子那本词条（apple）");
                await core.ExecuteScriptAsync(
                    "(function () { var i = document.getElementById('input'); i.value = 'apple';" +
                    " i.dispatchEvent(new Event('input', { bubbles: true }));" +
                    " i.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })); })();");
                Say("  ⑯ 打字 + 回车已发出，等词条框架");
                var deadFrame = await FindEntryFrameAsync(bridge.Frames, "apple", 20000, true);
                Ok(deadFrame != null, "⑯ 词条那一页装起来了（这条检查标准要有靶子才判得了）");
                /* 塞靶子的那段 JS（**幂等**：先删旧的再建）—— 起手塞一次，之后每点一条前再塞一次 */
                var seedTrayJs =
                    "(function () {" +
                    " var IDS = ['probe-tray','probe-nohref','probe-badanchor','probe-pointer'," +
                    " 'probe-text','probe-handled','probe-anchor-target','probe-live'];" +
                    " for (var i = 0; i < IDS.length; i++) {" +
                    "   var old = document.getElementById(IDS[i]);" +
                    "   if (old && old.parentNode) old.parentNode.removeChild(old); }" +
                    " var tray = document.createElement('div'); tray.id = 'probe-tray';" +
                    " function add(tag, id, text) { var el = document.createElement(tag); el.id = id;" +
                    "   el.textContent = text; tray.appendChild(el); return el; }" +
                    " add('a','probe-nohref','没有 href 的链接');" +
                    " add('a','probe-badanchor','锚点不存在')" +
                    "   .setAttribute('href','entry://#probe-no-such-anchor');" +
                    " add('span','probe-pointer','看着可点的东西').style.cursor = 'pointer';" +
                    " add('span','probe-text','普通正文');" +
                    " var handled = add('span','probe-handled','脚本接住了');" +
                    " handled.style.cursor = 'pointer';" +
                    " var hits = 0;" +
                    " handled.addEventListener('click', function () { hits++;" +
                    "   handled.textContent = '脚本接住了（已响应 ' + hits + '）'; });" +
                    " add('div','probe-anchor-target','锚点目标').style.height = '32px';" +
                    " add('a','probe-live','真的锚点')" +
                    "   .setAttribute('href','entry://#probe-anchor-target');" +
                    " document.body.appendChild(tray);" +
                    " return 'ok'; })()";
                if (deadFrame != null)
                {
                    var seeded = await FrameTextAsync(deadFrame, seedTrayJs);
                    Ok(seeded == "ok", "⑯ 六个靶子塞进了词条正文（" + Show(seeded) + "）");
                    Say("  ⑯ 靶子：" + Show(seeded) + "，开始逐个点");

                    /* 名字 / 该不该弹 / 该说的那句话（文案在宿主那一层，见 deadClickText） */
                    var deadCases = new[]
                    {
                        new[] { "nohref", "1", "这个链接没有目标" },
                        new[] { "badanchor", "1", "这一页里没有这个锚点" },
                        new[] { "pointer", "1", "这里没有可点开的内容" },
                        new[] { "text", "0", "" },
                        new[] { "handled", "0", "" },
                        new[] { "live", "0", "" }
                    };
                    foreach (var item in deadCases)
                    {
                        /*
                         * ⚠️ **塞靶子 + 点，一起重试**：靶子是随**文档**走的，而查同一个词时
                         * 宿主会再渲染一次（地址相同就加 `&t=` 强制刷新）—— 那一下会把刚塞进去的
                         * 靶子整片冲掉，报告里却是"没点出去"，看着像检查标准坏了。
                         */
                        var clicked = (string)null;
                        for (var attempt = 0; attempt < 3 && clicked != "clicked"; attempt++)
                        {
                            var seated = await FrameTextAsync(deadFrame, seedTrayJs);
                            if (seated != "ok")
                            {
                                Say("  ⑯ 「" + item[0] + "」第 " + (attempt + 1) + " 次靶子没就位（" +
                                    Show(seated) + "，原因：" + LastFrameError + "）");
                                await Task.Delay(400);
                                continue;
                            }
                            await WithTimeout(core.ExecuteScriptAsync(
                                "(function () { var b = document.getElementById('readerToast');" +
                                " b.dataset.show = 'false'; b.dataset.tone = ''; b.textContent = ''; })();"),
                                8000, "清掉上一条提示");
                            clicked = await FrameTextAsync(deadFrame,
                                "(function () { var el = document.getElementById('probe-" + item[0] + "');" +
                                " if (!el) return 'no-target';" +
                                " el.dispatchEvent(new MouseEvent('click', { bubbles: true, cancelable: true }));" +
                                " return 'clicked'; })()");
                            if (clicked != "clicked")
                            {
                                Say("  ⑯ 「" + item[0] + "」第 " + (attempt + 1) + " 次没点出去（" +
                                    Show(clicked) + "），重塞靶子再来");
                                await Task.Delay(500);
                            }
                        }
                        if (clicked != "clicked")
                        {
                            Ok(false, "⑯ 「" + item[0] + "」那一下没点出去（" + Show(clicked) + "）");
                            continue;
                        }
                        await Task.Delay(400);
                        var shownNow = await ReadTextAsync(core,
                            "String(document.getElementById('readerToast').dataset.show || '')");
                        var textNow = await ReadTextAsync(core,
                            "String(document.getElementById('readerToast').textContent || '')");
                        /* 逐条留实测结果，便于回溯"说没说那句话" */
                        Say("  ⑯ probe-" + item[0] + " → show=" + Show(shownNow) + " text=" + Show(textNow));
                        if (item[1] == "1")
                        {
                            Ok(shownNow == "true" && textNow.Contains(item[2]),
                               "⑯ ★ 点了「" + item[0] + "」之后正文框里说了一句（该说：" + item[2] +
                               "），实际 " + Show(shownNow + " / " + textNow));
                        }
                        else
                        {
                            Ok(shownNow != "true" || textNow.Length == 0,
                               "⑯ ★ 反向对照「" + item[0] + "」**不许**冒提示条，实际 " +
                               Show(shownNow + " / " + textNow));
                        }
                    }
                }
            }

            /* ══ ⑰ 划词翻译端到端（**只在 `--mt-key` 给了真 Key 时跑**）═══════════════════
             *
             * （；该件已停用，检查标准全文留在本节）真 Key 下的「划词翻译」要能翻出一页**译文伪词条**
             * （标题栏是原文、正文是译文、返回栈能退回去）。它跟别的节有一条根本差别：
             * **要联网、要按 token 计费** —— 所以检查标准不能塞进常规种子（那会让 检查 变成
             * "今天服务通不通"）。做法：没有 `--mt-key` 就**明着跳过**并说清原因
             * （与第 ⑨ 节同一种做法），专门验这一条时用一份带真 Key 的临时配置跑一次。
             *
             * 走的是**用户那条路**：查一个谁都没有的词 → 终态页摆出「翻译」 → 点它
             * （`onEntryChip('translate')` → `translateWord` → `renderTranslation`）。
             * 不依赖窗口焦点（那条路的坑见第 ⑨ 节），所以它稳。
             */
            {
                var keyShort = string.IsNullOrEmpty(mtKey)
                    ? "(没给)"
                    : mtKey.Substring(0, Math.Min(6, mtKey.Length)) + "…";
                if (string.IsNullOrEmpty(mtKey))
                {
                    Say("  [跳过] 第 ⑰ 节（划词翻译端到端）：没给 `--mt-key`。");
                    Say("         这一节要**真的联网翻译**、按 token 计费，所以它不进常规种子 ——");
                    Say("         专门验它时：Lookup.App.exe --selfcheck … --mt-key <key>（；该件已停用）。");
                }
                else
                {
                    Say("  ⑰ 用真 Key 验划词翻译（Key 只显示前 6 位：" + keyShort + "）");
                    /*
                     * ① 把 Key 交给壳（与「选项 → 语音」那一格同一条路：语音与翻译共用同一把），
                     *    并且**关掉自动翻译** —— 这样通道会停在终态页、把「翻译」这条出路摆出来，
                     *    走的正是"用户点一下翻译"那条路（`.js` 那节的教训：别自己造界面状态）。
                     */
                    await CallAsync(core,
                        "window.dshLookup.floating.speechSettings({ doubaoApiKey: " + JsString(mtKey) + " })", 20000);
                    await CallAsync(core,
                        "window.dshLookup.floating.translateSettings({enabled:true,autoTranslate:false})", 20000);
                    var st = ValueOf(await CallAsync(core, "window.dshLookup.floating.translateStatus()", 15000));
                    Ok(st != null && st.Contains("\"hasApiKey\":true"),
                       "⑰ 壳认下了这把 Key（translate:status 的 hasApiKey 为真）");
                    Ok(st != null && st.Contains("\"resourceId\":\"volc.speech.mt\""),
                       "⑰ 而且走的是翻译那个 Resource-Id（不是语音的 seed-tts-2.0）");

                    var probeWord = "serendipity";
                    await core.ExecuteScriptAsync(
                        "(function () { var i = document.getElementById('input'); i.value = " + JsString(probeWord) + ";" +
                        " i.dispatchEvent(new Event('input', { bubbles: true }));" +
                        " i.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })); })();");
                    var chips = await WaitTextAsync(core,
                        "String(document.getElementById('reader').dataset.chips || '')",
                        v => v != null && v.Contains("translate"), 30000);
                    Ok(chips != null, "⑰ 终态页上摆出了「翻译」这条出路（data-chips=" + Show(chips) + "）");

                    /* ② 点词条正文里那颗按钮 —— 它挂的是 **mousedown**（与出路按钮同一条规矩） */
                    var trFrame = await FindEntryFrameAsync(bridge.Frames, probeWord, 25000, true);
                    Ok(trFrame != null, "⑰ 终态页那一页装起来了（拿到框架句柄）");
                    var chipClicked = trFrame == null ? null : await FrameTextAsync(trFrame,
                        "(function () { var list = document.querySelectorAll('#lookupChips button');" +
                        " for (var i = 0; i < list.length; i++) {" +
                        "   if (list[i].dataset.chipAction === 'translate') {" +
                        "     list[i].dispatchEvent(new MouseEvent('mousedown', { bubbles: true, cancelable: true }));" +
                        "     return 'clicked'; } }" +
                        " return 'no-chip'; })()");
                    Ok(chipClicked == "clicked", "⑰ 点到了「翻译」那颗按钮（" + Show(chipClicked) + "）");

                    /* ③ 等译文进正文框：`via=translate`，而且标题栏是**原文** */
                    var via = await WaitTextAsync(core, "String(document.getElementById('reader').dataset.via || '')",
                        v => v == "translate", 60000);
                    Ok(via == "translate", "⑰ ★ 译文那一页进了正文框（via=translate），实际 " + Show(via));
                    var entryWord = await ReadTextAsync(core,
                        "String(document.getElementById('entryWord').textContent || '')");
                    Ok(entryWord == probeWord,
                       "⑰ ★ 标题栏是**原文**（" + probeWord + "），实际 " + Show(entryWord));

                    /*
                     * ④ 译文页**真的装起来了**、而且装在**翻译那个域**上。
                     *
                     * ⚠️ 检查标准读的是**渲染出来的那一页**，不是"在宿主页里 fetch 一下" ——
                     *    译文页住在 `*.dictres.invalid`，宿主页的 CSP 是 `connect-src 'self'`，
                     *    跨源 fetch 会被拦掉（那一轮第一版就是这么"读不到"的，报告里写 HTTP (没读到)）。
                     *    词条 iframe 里那份文档**就是**那个地址加载出来的，读它更直接。
                     * 内核给伪词条的地址长这样：`https://translate.dictres.invalid/__entry__?tok=<hash>`
                     * （host 是**专门的 translate 域**，不是某本词典的域 —— 见 TranslateDocs.UrlFor）。
                     */
                    var trSrc = await ReadTextAsync(core, "String(document.getElementById('entryFrame').src || '')");
                    var trDocFrame = await FindEntryFrameAsync(bridge.Frames, probeWord, 25000, true);
                    Ok(trDocFrame != null, "⑰ ★ 译文页那一页装起来了（拿到框架句柄）");
                    var docHref = "";
                    var docText = "";
                    if (trDocFrame != null)
                    {
                        docHref = Str(await FrameTextAsync(trDocFrame, "String(location.href)")) ?? "";
                        docText = Str(await FrameTextAsync(trDocFrame,
                            "String(((document.body && document.body.textContent) || '').replace(/\\s+/g, ' ').trim())")) ?? "";
                    }
                    var docHead = docText.Length > 200 ? docText.Substring(0, 200) + "…" : docText;
                    /*
                     * 译文那一页的结构（内核 `dsh_translate_api.c` 拼的）：
                     *   `.mt-head`  语种那一行（`<b>原文</b>` + 语种标签）
                     *   `.mt-text`  **译文正文**
                     *   `.mt-src原文：…`
                     * 所以"真服务实测结果"读的是 `.mt-text`，不是 `body.textContent`（那里面还有整段内联 CSS）。
                     */
                    var mtText = trDocFrame == null ? "" : Str(await FrameTextAsync(trDocFrame,
                        "String(((document.querySelector('.mt-text') || {}).textContent || '').replace(/\\s+/g, ' ').trim())")) ?? "";
                    var mtHead = trDocFrame == null ? "" : Str(await FrameTextAsync(trDocFrame,
                        "String(((document.querySelector('.mt-head') || {}).textContent || '').replace(/\\s+/g, ' ').trim())")) ?? "";
                    Say("  ⑰ 译文页实测结果：href=" + Show(docHref));
                    Say("  ⑰ 译文页实测结果：语种行 " + Show(mtHead) + " / **译文** " + Show(mtText));
                    Ok(docHref.StartsWith("https://translate." + VirtualHost.ResourceDomain + "/__entry__", StringComparison.Ordinal),
                       "⑰ ★ 译文页装在**翻译那个域**上（translate.dictres.invalid），实际 " + Show(docHref));
                    Ok(docHref.Length > 0 && trSrc.Length > 0 && docHref.StartsWith(trSrc, StringComparison.Ordinal),
                       "⑰ ★ 正文框里装的就是内核给的那个地址");
                    Ok(mtText.Length > 0 && mtText != probeWord,
                       "⑰ ★ 译文正文 `.mt-text` 非空、且**不是原文本身**（那就是真服务翻出来的字）：" + Show(mtText));
                    Ok(docText.Length > 200 && docText.Contains(probeWord),
                       "⑰ ★ 而且里面是**完整的一页**（含原文 " + probeWord + "，不是空白/提示页）");

                    /* ⑤ 返回栈：译文是一次**跳转**，压了一层；退回去要回到刚才那一页 */
                    var depthAfter = await ReadTextAsync(core,
                        "String(document.getElementById('entryBackLink').dataset.depth || '')");
                    Ok(depthAfter == "1", "⑰ ★ 译文页压了一层返回栈（data-depth=1），实际 " + Show(depthAfter));
                    await core.ExecuteScriptAsync("document.getElementById('entryBackLink').click();");
                    var depthBack = await WaitTextAsync(core,
                        "String(document.getElementById('entryBackLink').dataset.depth || '')", v => v == "0", 20000);
                    Ok(depthBack == "0", "⑰ ★ 点「返回」退回了原来那一页（data-depth 回到 0）");

                    /*
                     * 收尾：**把 Key 清回去**，并把翻译开关关回去。
                     *
                     * ⚠️ 清 Key 不是客气：后面那几节（⑨b2 系统语音增益、⑨c 凭据检测）
                     *    的前提都是"**没配在线凭据**"—— 留着真 Key 会让发音走**在线那一层**、
                     *    让「检测凭据」如实报"通"，于是那几节全变红（那一轮真踩过：
                     *    133 项 6 失败，全是"真 Key 还在"造成的）。第 ⑧b 节收尾也是这么做的。
                     */
                    await CallAsync(core,
                        "window.dshLookup.floating.translateSettings({enabled:false,autoTranslate:true})", 20000);
                    await CallAsync(core,
                        "window.dshLookup.floating.speechSettings({ doubaoApiKey: '' })", 20000);
                    var cleared = ValueOf(await CallAsync(core, "window.dshLookup.floating.translateStatus()", 15000));
                    Ok(cleared != null && cleared.Contains("\"hasApiKey\":false"),
                       "⑰ Key 已清回去（后面几节按「没配在线凭据」跑）");
                }
            }

            /* ══ ⑱ 在线音色的响度补偿**真的加到请求上了**（只在 `--mt-key` 时跑）══════════
             *
             * 背景：参考实现一直往 `audio_params` 里发 `loudness_rate`，而 0.2.0 **漏了**
             * 设置里那两个滑块对在线那一层毫无作用，
             * 两个默认音色还比参考实现响约 6 dB。那一轮补上这个字段（内核
             * `dsh_doubao_loudness_for` + 请求体），这一节量的是**它真的改变了波形**。
             *
             * 检查标准只有一句：**同一个词、同一个嗓子、两个不同的响度，解出来的能量必须不同**。
             * 量法与 ⑨b2 同一套：页面里 `speak` 拿地址 → `fetch` 字节 → `decodeAudioData`
             * 解成 PCM 自己算 RMS（钉"播放器会听到的那段波形"，不是"回包里有个数字"）。
             *
             * ⚠️ 要联网（真合成两次，各几个字），所以与 ⑰ 一样：不给 `--mt-key` 就明着跳过。
             * ⚠️ 收尾**必须把 Key 清回去** —— 后面 ⑨b2 / ⑨c 的前提是"没配在线凭据"。
             */
            {
                if (string.IsNullOrEmpty(mtKey))
                {
                    Say("  [跳过] 第 ⑱ 节（在线音色的响度补偿）：没给 `--mt-key` —— 它要真合成两次。");
                }
                else
                {
                    await CallAsync(core,
                        "window.dshLookup.floating.speechSettings({ doubaoApiKey: " + JsString(mtKey) +
                        ", doubaoLoudnessEn: 0 })", 20000);
                    var loud0 = Str(ValueOf(await CallAsync(core,
                        "(async () => { const r = await window.dshLookup.floating.speak('apple');" +
                        " if (!r || !r.ok) return 'speak-failed:' + ((r && r.message) || '?');" +
                        " const resp = await fetch(r.url); const buf = await resp.arrayBuffer();" +
                        " const ac = new AudioContext(); const a = await ac.decodeAudioData(buf);" +
                        " const d = a.getChannelData(0); let s = 0;" +
                        " for (let i = 0; i < d.length; i++) s += d[i] * d[i];" +
                        " const rms = Math.sqrt(s / Math.max(1, d.length));" +
                        " await ac.close();" +
                        /*
                         * 后两个字段是**这一次用的音色**：`voiceId` 是 id（请求体那一侧要它）、
                         * `voiceName` 是界面上的说法（参考实现的 `DescribeSpeaker`）。
                         * 只在真 Key 下才验得了 —— 这条路上"念得出声"本身就要真合成一次
                         * （假的 Key 会停在 speak-failed 那一支，两个字段都是空串）。
                         */
                        " return r.url + '|' + rms.toFixed(6) + '|' + d.length + '|' + r.source +" +
                        "   '|' + r.voiceId + '|' + r.voiceName; })()", 60000)));
                    Ok(loud0.Contains("|online"),
                       "★ （前置）有 Key 时发音走的是**在线**那一层：" + Show(loud0));

                    await CallAsync(core,
                        "window.dshLookup.floating.speechSettings({ doubaoLoudnessEn: -50 })", 20000);
                    var loud1 = Str(ValueOf(await CallAsync(core,
                        "(async () => { const r = await window.dshLookup.floating.speak('apple');" +
                        " if (!r || !r.ok) return 'speak-failed:' + ((r && r.message) || '?');" +
                        " const resp = await fetch(r.url); const buf = await resp.arrayBuffer();" +
                        " const ac = new AudioContext(); const a = await ac.decodeAudioData(buf);" +
                        " const d = a.getChannelData(0); let s = 0;" +
                        " for (let i = 0; i < d.length; i++) s += d[i] * d[i];" +
                        " const rms = Math.sqrt(s / Math.max(1, d.length));" +
                        " await ac.close();" +
                        " return r.url + '|' + rms.toFixed(6) + '|' + d.length + '|' + r.source; })()", 60000)));

                    var p0 = loud0.Split('|');
                    var p1 = loud1.Split('|');
                    Say("  ⑱ 响度实测结果：0 → " + Show(loud0) + "    -50 → " + Show(loud1));
                    var ok0 = p0.Length >= 4 && p0[3] == "online";
                    var ok1 = p1.Length == 4 && p1[3] == "online";
                    if (ok0 && ok1)
                    {
                        var rms0 = double.Parse(p0[1], System.Globalization.CultureInfo.InvariantCulture);
                        var rms1 = double.Parse(p1[1], System.Globalization.CultureInfo.InvariantCulture);
                        var samples = long.Parse(p0[2], System.Globalization.CultureInfo.InvariantCulture);
                        Ok(samples > 0 && rms0 > 0,
                           "⑱ ★ 0 那一次量到了波形（" + samples + " 个样本，RMS " + p0[1] + "）");
                        /*
                         * -6.0 dB 的理想比值是 0.5；检查标准放宽到 < 0.75 ——
                         * 编解码与重采样都会动一点电平，而"有没有效果"这件事差的是**一倍**。
                         */
                        Ok(rms1 < rms0 * 0.75,
                           "⑱ ★★ 把响度设成 -50（-6.0 dB）之后，解出来的能量**明显更低**：" +
                           "0 → " + p0[1] + " / -50 → " + p1[1]);
                        Ok(p0[0] != p1[0],
                           "⑱ ★ 两次的地址不一样（响度进了在线缓存键 —— 否则拖完滑块再点发音还是旧的那份）");
                        /*
                         * ★★ 在线回包里的**两个音色字段**：`voiceId` 是 id、`voiceName` 是
                         * 界面上的说法。参考实现就是这一对（`VoiceId = speaker` /
                         * `VoiceName = speakerName`），而 0.2.0 一度把名字写成了 id 本身。
                         */
                        Ok(p0.Length >= 6 && p0[4] == "en_female_dacey_uranus_bigtts" &&
                           p0[5] == "Dacey",
                           "⑱ ★★ 在线发音回包里 `voiceId` 是音色 id、`voiceName` 是**界面上的说法**" +
                           "（`Dacey`）—— 名字由内核那张表给：" +
                           Show(p0.Length >= 6 ? (p0[4] + " / " + p0[5]) : loud0));

                        /*
                         * ★★ 「这一次的覆盖」：**设置里是 -50，覆盖说 0 → 量到的必须是中性电平**。
                         *
                         * 这是，标准留在此处），也是界面「平衡音量」成立的前提：
                         * 它拿 `loudness: 0` 量中性电平，靠的就是这一覆盖。少了它，量到的电平会
                         * 随着滑块自己变 → 算出来的补偿值来回震荡（点一次变一个数）。
                         */
                        var loud2 = Str(ValueOf(await CallAsync(core,
                            "(async () => { const r = await window.dshLookup.floating.speak('apple'," +
                            " { source: 'online', voiceId: 'en_female_dacey_uranus_bigtts', loudness: 0 });" +
                            " if (!r || !r.ok) return 'speak-failed:' + ((r && r.message) || '?');" +
                            " const resp = await fetch(r.url); const buf = await resp.arrayBuffer();" +
                            " const ac = new AudioContext(); const a = await ac.decodeAudioData(buf);" +
                            " const d = a.getChannelData(0); let s = 0;" +
                            " for (let i = 0; i < d.length; i++) s += d[i] * d[i];" +
                            " const rms = Math.sqrt(s / Math.max(1, d.length));" +
                            " await ac.close();" +
                            " return r.url + '|' + rms.toFixed(6) + '|' + d.length + '|' + r.source; })()",
                            60000)));
                        var p2 = loud2.Split('|');
                        Say("  ⑱ 覆盖实测结果：滑块 -50 + loudness:0 → " + Show(loud2));
                        if (p2.Length == 4 && p2[3] == "online")
                        {
                            var rms2 = double.Parse(p2[1], System.Globalization.CultureInfo.InvariantCulture);
                            Ok(Math.Abs(rms2 - rms0) < rms0 * 0.05,
                               "⑱ ★★ 滑块是 -50 时，`loudness: 0` 那一次量到的仍是**中性电平**" +
                               "（与设置 0 那次相同）：设置 0=" + p0[1] + " / 覆盖 0=" + p2[1]);
                            /*
                             * ⚠️ 这一条第一版写的是"它与另外两次**都不是**同一份音频"，**当场红了** ——
                             *    红得对：覆盖 0 与"设置本来就是 0"发出的是**同一个请求体**，
                             *    于是缓存键相同、拿到同一份音频。**那正是它该有的样子**
                             *    （缓存键算在请求体上，见 `Speech.cs` 的 `online-v1|`）。
                             *    所以检查标准改成"**必须与设置 0 那次同址**"：比"不同"更严也更准 ——
                             *    它证的是"覆盖真的把设置中性化了"。
                             */
                            Ok(p2[0] == p0[0],
                               "⑱ ★★ 而且它与「设置本来就是 0」那次**同址**（同一个请求体 → 同一个缓存键，" +
                               "覆盖确实把设置中性化了）");
                            Ok(p2[0] != p1[0],
                               "⑱ ★ 它与 -50 那次不同址（不是把老音频取回来冒充）");
                        }
                        else
                        {
                            Ok(false, "⑱ ★★ 带覆盖那一次没走通在线那一层：" + Show(loud2));
                        }
                    }
                    else
                    {
                        Ok(false, "⑱ 两次都没能走通在线那一层：" + Show(loud0) + " / " + Show(loud1));
                    }

                    /* 收尾：Key 清回去（后面几节按「没配在线凭据」跑）*/
                    await CallAsync(core,
                        "window.dshLookup.floating.speechSettings({ doubaoApiKey: '' })", 20000);
                }
            }

            /* ══ ⑨ 管理窗：能开、页面是那一份、桥通 ═════════════════════════════ */
            var manager = win.Manager;
            Ok(manager != null, "壳里有管理窗这个对象");
            if (manager != null)
            {
                var opened = await CallAsync(core, "window.dshLookup.floating.openManager('dicts')", 15000);
                Ok(RejectedOf(opened) == null, "从悬浮窗那一页叫 openManager 没抛");
                var mReady = await WaitTextAsync(core, "1", v => v == "1", 100);
                await Task.Delay(2500); /* 建窗 + 起 WebView2 + 装页面 */
                Ok(manager.IsOpen, "管理窗开了（IsOpen）");
                Ok(manager.Bridge.RequestsSeen >= 0, "管理窗的桥接上了");
                Ok(win.ManagerOpens >= 1, "壳记下了 manager:open 被叫过一次");
                /*
                 * ★ 窗口标题要跟着页签走。
                 *
                 * 钉的是**用户看到的那一行**（任务栏 / Alt-Tab 上的窗口标题就是 `Form.Text`），
                 * 而"页签叫什么"的唯一来源是页面里的 `document.title` —— 壳只是照抄
                 * （`ManagerWindow` 挂了 `CoreWebView2.DocumentTitleChanged`）。
                 * 这里等它一两秒：页面翻页 → 标题变 → 通知到壳，是一条跨进程的路。
                 */
                Func<string, Task<string>> waitTitle = async delegate (string want)
                {
                    for (var i = 0; i < 25; i++)
                    {
                        if (manager.Text == want) return manager.Text;
                        await Task.Delay(200);
                    }
                    return manager.Text;
                };
                var titleNow = await waitTitle("选项 · 词库");
                Ok(titleNow == "选项 · 词库",
                   "★ 窗口标题跟着页签走（要求「选项 · 词库」，实测「" + titleNow + "」）");
                /*
                 * ⚠️ 上一轮这一节只钉了"能开、桥通"，**那一页的功能一条都没验**。
                 *    这一轮补上：在管理窗那一页上调桥（那一页的按钮就是调它），
                 *    再回**悬浮窗那一页**看它跟不跟着变 —— 这正好验到两件事：
                 *      ① 内核是两个窗口**唯一的事实**（两边都从它读，不各存一份）；
                 *      ② 那条"事件两扇窗都发"真的成立（`dictionaries:changed`）。
                 */
                var mcore = await manager.Ready;
                var rows0 = await ReadTextAsync(mcore,
                    "String(document.querySelectorAll('#dictList .dict-item').length)");
                Ok(rows0 != "0", "词库那一页列出了词典（" + rows0 + " 本）");

                /* ══ ⑨b 音量增益：设置页那两条滑块背后是「内核那张按词典存的表」═════
                 *
                 * **为什么写在这儿**：`setGains` 只挂在**管理窗那一页**的桥上
                 * （`bridge.js` 的 `manager` 命名空间）—— 它就是设置页那两条滑块调的东西，
                 * 悬浮窗那一页根本没有这个方法（第一版写在前面，8 条断言全红，
                 * 报的是 `f.setGains is not a function`）。
                 *
                 * 这一节钉四件事（只有真实程序才验得了：页面 → 桥 → 壳 → 内核 → 落盘 → 再读回来）：
                 *   ① `speechStatus` 里带着内核那份 `voiceGains`（值 + 能不能调 + 那句人话）；
                 *   ② `setGains` 写进去的数**按词典存**，而且只改那一本；
                 *   ③ 词典原录音那条路回包里的 `gainDb` **跟着这本词典走**（前端拿它自己乘；
                 *      参考实现的原话：压缩格式要缩放就得整段重编码，所以这一路是前端施加的）；
                 *   ④ `null` 能把那一项清掉（「没设过」= 0 增益）。
                 *
                 * ⚠️ 它会**真写设置**，所以收尾把两项都写回 `null`、当前词典摆回原样。
                 *
                 * ⚠️ **系统离线语音那一项的"施加"这一版还没有**：参考实现是合成时把 PCM
                 *    缩放（在"产字节"那一侧施加，所以回包里的 `gainDb` 恒 0）；
                 *    0.2.0 的 C 内核还没有「按 dB 缩放 PCM」这条接口，所以那一条滑块
                 *    今天**只落库、不改变声音**。这是**未完成项**，写在这儿免得后来人以为已生效。
                 */
                var gainsCurrentBefore = Str(ValueOf(await CallAsync(mcore,
                    "(async () => { const list = await window.dshLookup.manager.listDictionaries();" +
                    " const c = list.filter(function (d) { return d.current; })[0];" +
                    " return c ? c.id : ''; })()", 15000)));

                var gainsStatus = ValueOf(await CallAsync(mcore,
                    "window.dshLookup.manager.speechStatus('apple')", 20000));
                var gainsView0 = TextAt(gainsStatus, "voiceGains");
                Ok(gainsView0 != null && gainsView0.Contains("\"dictGainDb\""),
                   "★ speech:status 里带着 voiceGains（那份视图整个由内核给）");
                Ok(gainsView0 != null && gainsView0.Contains("\"dictMessage\""),
                   "★ 连「能不能调、不能调时那句话」也在里面（界面一个字都不拼）");

                /* 把带资源卷那本切成当前，再写 4.5 —— 增益是按**当前词典**记的 */
                Ok(RejectedOf(await CallAsync(mcore,
                    "window.dshLookup.manager.setCurrentDictionary(" + JsString(audioId) + ")",
                    15000)) == null, "把当前词典切到带资源卷那一本（增益写的是当前那一本）");
                var wrote = ValueOf(await CallAsync(mcore,
                    "window.dshLookup.manager.setGains({ dictGainDb: 4.5 })", 20000));
                var wroteView = TextAt(wrote, "voiceGains");
                Ok(Nearly(NumAt(wroteView, "dictGainDb"), 4.5),
                   "★ 写 4.5 dB 之后回读就是 4.5（归一化没把它改样）：" +
                   ShowNum(NumAt(wroteView, "dictGainDb")));
                Ok(Str(TextAt(wroteView, "dictId")) == audioId,
                   "★ 而且这份视图认的是**刚切过去的那一本**（增益按词典存）");
                Ok(TextAt(wroteView, "dictAvailable") == "true",
                   "★ 那一本有资源卷（.mdd）→ 内置录音这一项可调");
                Ok(wrote != null && wrote.Contains("\"settings\""),
                   "★ setGains 回的是**整份 speech:status**（界面拿它重排滑块，不必再问一次）");

                /* 系统那一个：写得进、回读得到，而且**不许顺手把词典那一个清零** */
                var wroteSys = ValueOf(await CallAsync(mcore,
                    "window.dshLookup.manager.setGains({ systemGainDb: -3 })", 20000));
                var sysView = TextAt(wroteSys, "voiceGains");
                var sysValue = NumAt(sysView, "systemGainDb");
                Ok(sysValue.HasValue && Math.Abs(sysValue.Value + 3) < 0.001,
                   "★ 系统语音那一项写 -3 之后回读是 -3，实际 " + ShowNum(sysValue));
                Ok(Nearly(NumAt(sysView, "dictGainDb"), 4.5),
                   "★ 只发 systemGainDb → 词典那一个**原样留着**（没被当成「没传就是清零」），实际 " +
                   ShowNum(NumAt(sysView, "dictGainDb")));

                /* 词典原录音那条路：回包里的 gainDb 必须跟着**这本**词典走 */
                var dictSpeak = ValueOf(await CallAsync(mcore,
                    "window.dshLookup.manager.speak('beep', { dictId: " + JsString(audioId) + " })",
                    30000));
                Ok(Str(TextAt(dictSpeak, "source")) == "dict",
                   "这本词典里那条词条走的是自带录音（source=dict）");
                Ok(Nearly(NumAt(dictSpeak, "gainDb"), 4.5),
                   "★ 这条路回包里的 gainDb 就是这本词典那个 4.5：" +
                   ShowNum(NumAt(dictSpeak, "gainDb")));

                /* 收尾：两项都清掉（回到「没设过」），当前词典摆回原样 */
                var cleared = ValueOf(await CallAsync(mcore,
                    "window.dshLookup.manager.setGains({ dictGainDb: null, systemGainDb: null })",
                    20000));
                var clearView = TextAt(cleared, "voiceGains");
                var clearDict = NumAt(clearView, "dictGainDb");
                var clearSystem = NumAt(clearView, "systemGainDb");
                Ok(Nearly(clearDict, 0) && Nearly(clearSystem, 0),
                   "★ 传 null 把两项都清掉 → 回读都是 0（「没设过」就是 0），实际 dict=" +
                   ShowNum(clearDict) + " / system=" + ShowNum(clearSystem));
                if (gainsCurrentBefore.Length > 0)
                {
                    await CallAsync(mcore,
                        "window.dshLookup.manager.setCurrentDictionary(" +
                        JsString(gainsCurrentBefore) + ")", 15000);
                }

                /* ══ ⑨b2 ★ 系统离线语音的增益**真的加到字节上了** ══════════════════
                 *
                 * 为什么写在这儿：这一项原来是**未完成项** —— 那条滑块"只落库、不出声"
                 * （参考实现是合成完之后按 PCM 缩放，0.2.0 的内核此前没有那条纯函数）。
                 * 检查标准只有一句：**同一个词、同一个嗓子、两个不同的音量，出来的字节能量必须不同**。
                 *
                 * 量法：在悬浮窗那一页 `speak('apple')` 拿地址 → `fetch` 字节 →
                 * **页面里 `decodeAudioData` 解成 PCM、自己算 RMS**。所以它钉的是
                 * "播放器真会听到的那段波形"，不是"回包里有个数字"（后者骗得过任何断言）。
                 *
                 * 顺带针对缓存键那一格：换增益时**地址必须换** —— 少了它，用户拖完滑块
                 * 再点发音拿到的还是上一次那份音频（"改了没反应"）。
                 *
                 * ⚠️ 它**会写设置**（两次），收尾清回 `null`。
                 */
                {
                    var gainRoute = Str(ValueOf(await CallAsync(core,
                        "(async () => { const r = await window.dshLookup.floating.speak('apple');" +
                        " if (!r || !r.ok) return 'speak-failed:' + ((r && r.message) || '?');" +
                        " return r.url + '|' + r.source; })()", 40000)));
                    Ok(gainRoute.Contains("|system"),
                       "★ （前置）没有词典录音也没有 Key 时，发音走的是**系统离线**那一层：" +
                       Show(gainRoute));

                    var measure0 = Str(ValueOf(await CallAsync(core,
                        "(async () => { const r = await window.dshLookup.floating.speak('apple');" +
                        " const resp = await fetch(r.url); const buf = await resp.arrayBuffer();" +
                        " const ac = new AudioContext(); const a = await ac.decodeAudioData(buf);" +
                        " const d = a.getChannelData(0); let s = 0;" +
                        " for (let i = 0; i < d.length; i++) s += d[i] * d[i];" +
                        " const rms = Math.sqrt(s / Math.max(1, d.length));" +
                        " await ac.close();" +
                        " return r.url + '|' + rms.toFixed(6) + '|' + d.length; })()", 60000)));
                    var parts0 = measure0.Split('|');
                    Ok(parts0.Length == 3 && parts0[2] != "0",
                       "★ 0 dB 那一次量到了波形（样本数 " + Show(parts0.Length > 2 ? parts0[2] : "?") +
                       "）：" + Show(measure0));

                    await CallAsync(mcore,
                        "window.dshLookup.manager.setGains({ systemGainDb: -12 })", 20000);
                    var measure1 = Str(ValueOf(await CallAsync(core,
                        "(async () => { const r = await window.dshLookup.floating.speak('apple');" +
                        " const resp = await fetch(r.url); const buf = await resp.arrayBuffer();" +
                        " const ac = new AudioContext(); const a = await ac.decodeAudioData(buf);" +
                        " const d = a.getChannelData(0); let s = 0;" +
                        " for (let i = 0; i < d.length; i++) s += d[i] * d[i];" +
                        " const rms = Math.sqrt(s / Math.max(1, d.length));" +
                        " await ac.close();" +
                        " return r.url + '|' + rms.toFixed(6) + '|' + d.length; })()", 60000)));
                    var parts1 = measure1.Split('|');

                    if (parts0.Length == 3 && parts1.Length == 3)
                    {
                        var rms0 = double.Parse(parts0[1], System.Globalization.CultureInfo.InvariantCulture);
                        var rms1 = double.Parse(parts1[1], System.Globalization.CultureInfo.InvariantCulture);
                        Ok(rms1 < rms0 * 0.5,
                           "★★ 系统离线语音的增益**真的加到字节上了**：-12 dB 那次的 RMS 明显更低" +
                           "（×0.25 是理想值，检查标准放宽到 < ×0.5）—— 0 dB=" + ShowNum(rms0) +
                           " / -12 dB=" + ShowNum(rms1));
                        Ok(parts1[0] != parts0[0],
                           "★ 而且**地址换了**（增益进了缓存键）—— 否则拖完滑块再点发音" +
                           "拿到的还是上一次那份音频");
                    }
                    else
                    {
                        Ok(false, "★ -12 dB 那一次没量到波形：" + Show(measure1));
                    }

                    /*
                     * ══ ★★ 「这一次的覆盖」：**滑块设成 -12，`gainDb: 0` 那一次必须仍是中性电平**
                     *
                     * 这一条钉的是界面上「平衡音量」成立的前提（`web/src/manager/main.ts` 的
                     * `systemLevelOf`）：它量中性电平时传 `gainDb: 0`，**靠的就是这一覆盖**。
                     * 少了它，量到的电平会随着用户拖那个滑块自己变 —— 算出来的补偿值来回震荡
                     * （点一次 +3 dB、再点一次变回 0，表现出来像"越点越乱"）。
                     *
                     * ⚠️ 检查标准钉的是**波形**（RMS 与第一次 0 dB 那次相同），不是"回包里有个 0"。
                     */
                    {
                        var measure2 = Str(ValueOf(await CallAsync(core,
                            "(async () => { const r = await window.dshLookup.floating.speak('apple'," +
                            " { source: 'system', gainDb: 0 });" +
                            " if (!r || !r.ok) return 'speak-failed:' + ((r && r.message) || '?');" +
                            " const resp = await fetch(r.url); const buf = await resp.arrayBuffer();" +
                            " const ac = new AudioContext(); const a = await ac.decodeAudioData(buf);" +
                            " const d = a.getChannelData(0); let s = 0;" +
                            " for (let i = 0; i < d.length; i++) s += d[i] * d[i];" +
                            " const rms = Math.sqrt(s / Math.max(1, d.length));" +
                            " await ac.close();" +
                            " return r.url + '|' + rms.toFixed(6) + '|' + d.length; })()", 60000)));
                        var parts2 = measure2.Split('|');
                        Say("  ⑨b2 覆盖实测结果：滑块 -12 + gainDb:0 → " + Show(measure2));
                        if (parts0.Length == 3 && parts2.Length == 3)
                        {
                            var rmsA = double.Parse(parts0[1], System.Globalization.CultureInfo.InvariantCulture);
                            var rmsB = double.Parse(parts2[1], System.Globalization.CultureInfo.InvariantCulture);
                            Ok(Math.Abs(rmsB - rmsA) < rmsA * 0.05,
                               "★★ 滑块是 -12 时，`gainDb: 0` 那一次量到的仍是**中性电平**" +
                               "（与 0 dB 那次相同）：0 dB=" + ShowNum(rmsA) + " / 覆盖 0=" + ShowNum(rmsB));
                            Ok(parts2[0] != parts1[0],
                               "★ 而且它与 -12 dB 那次**不是同一份音频**（覆盖进了缓存键）");
                        }
                        else
                        {
                            Ok(false, "★★ 带覆盖那一次没量到波形：" + Show(measure2));
                        }
                    }

                    await CallAsync(mcore,
                        "window.dshLookup.manager.setGains({ systemGainDb: null })", 20000);
                }

                /* ══ ⑨c 「检测凭据」的语音那一半：**两行都出结果** ═══════════════════
                 *
                 * 为什么写在这儿：那个按钮（`#translateTest`）调的是
                 * `testDoubao()` **加** `translateTest()` —— 而语音这一半此前
                 * **根本没接**（`speech:testDoubao` 抛「壳还没接这条」），
                 * 于是管理窗那一页点一次就是一行红字。这一节钉的就是"点下去有结果"。
                 *
                 * 它钉四件事（只有真实程序才验得了：页面 → 桥 → 壳 → 内核 → 真 HTTP）：
                 *   ① 没填 Key 时**逐项**如实说（两项、每项一句人话），不抛异常；
                 *   ② 填了 Key 之后回的是**两项**（英文 + 中文），而且每一项的形状都完整
                 *      （ok 时必有字节、不通时必有人话 —— 两种都算"说出结果了"）；
                 *   ③ 样本词是**内核那张表**给的（`en:apple` / `zh:苹果`）；
                 *   ④ ★ 指定音色时**只测那一项**，而且测的是**递进来的音色** ——
                 *      这是「保存前先检测」那一步成立的前提（用户改了 ID 还没写盘时，
                 *      要测的必须是即将存下去的那个）。
                 *
                 * ⚠️ 用的是**一把随手编的 Key**（`k-selfcheck`）：这一步**不该**依赖
                 *    网络今天通不通、也不该真花钱 —— 它要钉的是"有结果、形状对"。
                 *    真 Key 下的"今天通不通"由  回答。
                 * ⚠️ 收尾把 Key 清回去（后面几节按"没配在线凭据"的前提走）。
                 */
                {
                    var noKey = Str(ValueOf(await CallAsync(mcore,
                        "(async () => { const list = await window.dshLookup.manager.testDoubao();" +
                        " if (!Array.isArray(list)) return 'not-array';" +
                        " return list.length + '|' + list.map(function (i) {" +
                        "   return (i.ok === false && (i.error || '').length > 0) ? 'bad' : 'odd';" +
                        " }).join(',') + '|' + list.map(function (i) { return i.error || ''; }).join(' / ');" +
                        "})()", 40000)));
                    Ok(noKey.StartsWith("2|", StringComparison.Ordinal),
                       "★ 没填 Key 时也**回两项**（不是抛「壳还没接这条」）：" + Show(noKey));
                    Ok(noKey.Contains("bad,bad"),
                       "★ 两项都如实说「不通」（ok=false 且有人话），不是不报错地失败");
                    Ok(noKey.Contains("凭据"),
                       "★ 那句人话来自内核（凭据那一条），界面照抄：" + Show(noKey));

                    await CallAsync(mcore,
                        "window.dshLookup.manager.speechSettings({doubaoApiKey:'k-selfcheck'})",
                        20000);
                    var both = Str(ValueOf(await CallAsync(mcore,
                        "(async () => { const list = await window.dshLookup.manager.testDoubao();" +
                        " if (!Array.isArray(list) || list.length !== 2) return 'len=' + (list && list.length);" +
                        " const shape = list.every(function (i) {" +
                        "   return typeof i.speaker === 'string' && typeof i.speakerName === 'string' &&" +
                        "     typeof i.language === 'string' && typeof i.text === 'string' &&" +
                        "     typeof i.explicitLanguage === 'string' && typeof i.url === 'string' &&" +
                        "     typeof i.statusCode === 'number' && typeof i.bytes === 'number' &&" +
                        "     typeof i.mime === 'string' && typeof i.elapsedMs === 'number' &&" +
                        "     typeof i.billedWords === 'number' && typeof i.error === 'string' &&" +
                        "     (i.ok === true ? i.bytes > 0 : i.error.length > 0); });" +
                        " return 'lang=' + list.map(function (i) { return i.language + ':' + i.text; }).join(',') +" +
                        "   '|shape=' + shape +" +
                        /*
                         * 音色也带回来：默认那两个是**用户挑的**（英文 Dacey / 中文 Vivi），
                         * 而"挑的是哪两个"必须能在真实程序里读到 —— 它落在 `speech:testDoubao`
                         * 的计划里（内核按语种选音色），不是界面自己写的。
                         *
                         * ⚠️ 两个字段各钉一件事：`speaker` 是**音色 id**（请求体那一侧要它），
                         *    `speakerName` 是**界面上的说法**（参考实现的 `DescribeSpeaker`）。
                         *    那一轮这里只钉了 id（名字那一格当时**按设计是空串**，
                         *    见 `Lookup.Host/Speech.cs` 里 `TestItem` 那段的历史）；
                         *    那一轮把那张表搬进内核之后，
                         *    两个都针对 —— 名字那一半如今也有据可依了。
                         */
                        "   '|names=' + list.map(function (i) { return i.speakerName; }).join(',') +" +
                        "   '|speakers=' + list.map(function (i) { return i.speaker; }).join(','); })()", 60000)));
                    Ok(both.Contains("lang=en:apple,zh:苹果") && both.Contains("shape=true"),
                       "★ 两项的语种与**样本词**都由内核给（en:apple / zh:苹果），形状完整" +
                       "（ok 必有字节、不通必有人话）：" + Show(both));
                    Ok(both.Contains("speakers=en_female_dacey_uranus_bigtts,zh_female_vv_uranus_bigtts"),
                       "★★ 默认音色就是**用户挑的那两个**（英文 `en_female_dacey_uranus_bigtts` = Dacey /" +
                       "中文 `zh_female_vv_uranus_bigtts` = Vivi）——与 参考实现逐字相同，" +
                       "真服务的实测结果见  ：" + Show(both));
                    Ok(both.Contains("names=Dacey,Vivi"),
                       "★★ 「界面上的说法」也是那两个官网名（`Dacey` / `Vivi`）——" +
                       "名字由内核那张表给（`dsh_speech_speaker_label`），壳不抄第二份：" + Show(both));

                    var one = Str(ValueOf(await CallAsync(mcore,
                        "(async () => { const list = await window.dshLookup.manager.testDoubao('my-selfcheck-voice', 'zh');" +
                        " if (!Array.isArray(list) || list.length !== 1) return 'len=' + (list && list.length);" +
                        " return list[0].speaker + '|' + list[0].language + '|' + list[0].text;" +
                        "})()", 60000)));
                    Ok(one == "my-selfcheck-voice|zh|苹果",
                       "★ 指定音色 → 只测一项，而且测的是**递进来的那个音色**" +
                       "（保存前那次检测的前提）：" + Show(one));

                    /*
                     * ★★ `speech:status` 里豆包那一块的**两个音色名** —— 参考实现的
                     * `view.SpeakerEnName / SpeakerZhName`（`DescribeSpeaker(…)`）。
                     *
                     * 为什么要在这儿读一次：那两个名字是"设置页表单的只读视图"，
                     * 界面上**不显示**（参考实现那一份界面也不显示），所以除了桥就没有
                     * 别的出口能读到它 —— 而"读不到"正是它上次退化成恒空串的原因。
                     *
                     * ⚠️ 故意填**大写**的英文音色 id：那串 id 是用户手打进设置里的，
                     *    「大小写不敏感」是这条约定的一半（参考实现也是 `OrdinalIgnoreCase`）。
                     * ⚠️ 收尾写回 `null`（= 这一格从没设过，与种子设置原样一致）：
                     *    后面的节按"默认音色"走。
                     */
                    await CallAsync(mcore,
                        "window.dshLookup.manager.speechSettings({doubaoSpeakerEn:" +
                        "'EN_FEMALE_DACEY_URANUS_BIGTTS',doubaoSpeakerZh:'zh_female_vv_uranus_bigtts'})",
                        20000);
                    var namedView = TextAt(ValueOf(await CallAsync(mcore,
                        "window.dshLookup.manager.speechStatus('apple')", 20000)), "doubao");
                    Ok(namedView != null && namedView.Contains("\"speakerEnName\":\"Dacey\""),
                       "★★ 状态里英文音色的说法是 `Dacey`（大写 id 也认）——" + Show(namedView));
                    Ok(namedView != null && namedView.Contains("\"speakerZhName\":\"Vivi\""),
                       "★★ 中文那一格同理（`Vivi`）：那两个名字来自内核那张表，壳不抄第二份");
                    /*
                     * ⚠️ 收尾写的是**空串**，不是 `null` —— 这是实测出来的：设置补丁里
                     *    `{"doubaoSpeakerEn":null}` 是**空操作**（内核 `parse_speech` 那一处
                     *    只在"键在且是字符串"时才写，NULL 一律不动现值，见
                     *    `native/src/engine/dsh_settings.c` 那段注释）。空串才是这条路上
                     *    "用户把它清掉"的写法（两个音色的默认值另有兜底，见 `test_speech.c` ④）。
                     *    第一版检查标准写的是 null 并断言"回到从没设过"，当场红 —— 红得对：
                     *    那是我的假设，不是产品的约定。
                     */
                    await CallAsync(mcore,
                        "window.dshLookup.manager.speechSettings({doubaoSpeakerEn:''," +
                        "doubaoSpeakerZh:''})", 20000);
                    var restoredView = TextAt(ValueOf(await CallAsync(mcore,
                        "window.dshLookup.manager.speechStatus('apple')", 20000)), "doubao");
                    Ok(restoredView != null && restoredView.Contains("\"speakerEn\":\"\"") &&
                       restoredView.Contains("\"speakerEnName\":\"\""),
                       "★ 清掉之后 id 与名字都是空串（参考实现同约定 —— 没配就不编一个名字出来）：" +
                       Show(restoredView));

                    await CallAsync(mcore,
                        "window.dshLookup.manager.speechSettings({doubaoApiKey:''})", 20000);
                }

                /* 换一本当前词典 —— 在那一页上调，再回悬浮窗那一页看 */
                var pickedRaw = await CallAsync(mcore,
                    "(async () => { const list = await window.dshLookup.manager.listDictionaries();" +
                    " const other = list.filter(function (d) { return !d.current; })[0] || list[0];" +
                    " await window.dshLookup.manager.setCurrentDictionary(other.id); return other.id; })()",
                    20000);
                var pickedId = Str(ValueOf(pickedRaw));
                Ok(pickedId.Length > 0, "在管理窗那一页换了一本当前词典（" + Short(pickedId) + "）");
                var currentSeen = Str(ValueOf(await CallAsync(core,
                    "(async () => { const list = await window.dshLookup.floating.listDictionaries();" +
                    " const c = list.filter(function (d) { return d.current; })[0];" +
                    " return c ? c.id : ''; })()", 15000)));
                Ok(currentSeen == pickedId,
                   "★ 悬浮窗那一页也看到「当前词典」换成同一本了（内核是唯一事实）");
                /*
                 * ★★ 而且**那行灰字真的跟着变了**（用户 2026-09 报的 bug）。
                 *
                 * 为什么上面那条不够：它读的是 `listDictionaries`（内核里的"当前词典"），
                 * 而用户看见的是**胶囊上那行灰字**（`#meta`，`main.ts` 里由
                 * `state.dictionaries.find(d => d.id === state.currentDictId).title` 画出来）。
                 * 修前这两件事是**脱节**的：在管理窗里切词典时，`EmitDicts` 只把
                 * `dictionaries:changed` 回给**发起请求那一页**，悬浮窗收不到 ⇒ 内核状态对了、
                 * 灰字却停在旧名字上 —— 上面那条断言一路绿着，正是"钉了替代指标"的典型。
                 * 现在壳那一侧改成广播（`IShellHost.BroadcastDictionaries`，参考实现的
                 * `App.BroadcastDictionaries()`），所以这里钉**页面上的那行字**：
                 * 它必须以新词典名**开头**（后面可能跟 `· 载入中` / `· 载入失败` —— 切换会触发预热，
                 * 所以不写死全等，免得跟着时序红）。
                 */
                var pickedTitle = Str(ValueOf(await CallAsync(mcore,
                    "(async () => { const list = await window.dshLookup.manager.listDictionaries();" +
                    " const c = list.filter(function (d) { return d.current; })[0];" +
                    " return c ? c.title : ''; })()", 15000)));
                await Task.Delay(400);
                var pillMeta = Str(ValueOf(await CallAsync(core,
                    "document.getElementById('meta').textContent", 15000)));
                Ok(pickedTitle.Length > 0 && pillMeta.StartsWith(pickedTitle),
                   "★★ 悬浮窗胶囊上那行灰字跟着换成新的当前词典名（期望以「" + pickedTitle +
                   "」开头，实际「" + pillMeta + "」）");

                /* 改名 —— 顺带验"两扇窗都收到 dictionaries:changed" */
                await CallAsync(mcore,
                    "(async () => { await window.dshLookup.manager.renameDictionary(" +
                    JsString(pickedId) + ", '自检改的名字'); return 'ok'; })()", 20000);
                await Task.Delay(600);
                var renamed = Str(ValueOf(await CallAsync(core,
                    "(async () => { const list = await window.dshLookup.floating.listDictionaries();" +
                    " const d = list.filter(function (x) { return x.id === " + JsString(pickedId) + "; })[0];" +
                    " return d ? String(d.title || '') : ''; })()", 15000)));
                Ok(renamed.Contains("自检改的名字"),
                   "★ 在管理窗里改的名字，**悬浮窗那一页也变了**（事件两扇窗都发）：" + Show(renamed));

                /* 移除 —— 两边都该少一本 */
                await CallAsync(mcore,
                    "(async () => { await window.dshLookup.manager.removeDictionary(" +
                    JsString(pickedId) + "); return 'ok'; })()", 20000);
                await Task.Delay(600);
                var rows1 = await ReadTextAsync(mcore,
                    "String(document.querySelectorAll('#dictList .dict-item').length)");
                Ok(rows1 != rows0, "★ 移除之后管理窗那一页少了一行（" + rows0 + " → " + rows1 + "）");
            }

            /* ══ ⑩ 托盘菜单：能弹、页面画出了菜单项 ═════════════════════════════ */
            Ok(win.Tray != null, "壳里有托盘菜单这个对象");
            if (win.Tray != null)
            {
                var shown = await CallAsync(core, "window.dshLookup.debug.showTrayMenu()", 15000);
                Ok(RejectedOf(shown) == null, "叫 showTrayMenu 没抛");
                await Task.Delay(2500);
                Ok(win.TrayShows >= 1, "壳记下了托盘菜单被弹过一次");
                Ok(win.TrayIcon != null, "托盘图标也建起来了（不然用户没法把菜单叫回来）");
                /*
                 * 托盘图标用的是**哪一份资源**：`tray.png` 是正解（与参考实现同一份，
                 * 逐字节相同）；`SystemIcons.Application` 表示资产没嵌进去、退成了系统图标。
                 * 为什么读这个而不是看图：跑验收那台机器常是锁屏 / 远程会话，截图只能拍到壁纸
                 * （项目规范）。
                 */
                Ok(win.TrayIcon != null && win.TrayIcon.IconSource == "tray.png",
                   "★ 托盘图标用的是搬进来的那份资源（assets/tray.png），实际 " +
                   (win.TrayIcon == null ? "(没有托盘图标)" : win.TrayIcon.IconSource));

                /*
                 * ★★ **外壳层不许比宿主窗多活一秒**（2026-09 "按钮层没了、阴影层还留在屏幕上"）。
                 *
                 * 菜单窗与外壳层（投影 / 抗锯齿圆角）是**两张窗口**，而收菜单有三条路：
                 *   ① 点到别处（全局鼠标钩子）② 页面叫收 ③ **窗口失活**。
                 * 0.2.0 搬家时第 ③ 条写成了裸 `Hide()`（= WinForms 的 `Form.Hide()`），
                 * 只收了宿主窗、**把外壳层留在屏幕上**；参考实现那边这个方法叫 `Hide()`、
                 * 自己做两层，改名的过程中这句就悄悄换了目标（见 `TrayMenuWindow` 那段注释）。
                 *
                 * ⚠️ 检查标准**不能只读一个瞬间**：`Place()` 里那句 `TopMost = true` 会让菜单窗
                 *    在摆放的过程中被激活，于是"失活"随时可能发生、状态是**动的** ——
                 *    一个瞬间的实测结果会跟着时序红（项目规范 "别钉替代指标"那条）。
                 *    所以这里量的是**不变式**：连读若干次，**任何一次**出现
                 *    "宿主窗已经收了、外壳层还亮着"就是用户看到的那个残留。
                 *    检查标准只能**问系统**（`debug:window('tray')` 的 `visible` 是宿主窗、
                 *    `chrome.visible` 是外壳层，两个都是 `IsWindowVisible`）。
                 *
                 * ⚠️⚠️ **数 `"visible":true` 出现几次，别拿去比一整段子串**（这个坑当场踩了两次）：
                 *    `ValueOf()` 会把桥回包里的 JSON **重新序列化一遍**，键序因此变成**按字母**的
                 *    （`chrome` 里成了 `bounds → present → visible`）—— 于是
                 *    `"chrome":{"present":true,"visible":false` 这种"照原样拼出来的子串"**永远匹配不上**，
                 *    断言会**误报失败**（还让人以为是产品没修好）。两份实测结果各数各的：
                 *    宿主 `"role":"tray","visible":…`、外壳 `"chrome":{…}` —— 数总数最省事：
                 *    **两个窗口都亮着 = 2 次，只亮一个 = 1 次，都收了 = 0 次**。
                 *
                 * 为什么非要在：它要真实窗口真消息循环（失活是 Windows 给的）；
                 * 对应的诊断是  --tray --tray-deactivate`（同样量这个不变式）。
                 */
                Func<string, int> visibleTrueCount = delegate (string text)
                {
                    var n = 0;
                    var at = 0;
                    while (text != null && (at = text.IndexOf("\"visible\":true", at, StringComparison.Ordinal)) >= 0)
                    {
                        n++;
                        at += 14;
                    }
                    return n;
                };
                await CallAsync(core, "window.dshLookup.debug.hideTrayMenu()", 15000);
                /*
                 * ① 弹出这一路：**外壳层不许掉队，也不许超车**。
                 *    菜单是"页面量完高度 → 主进程摆窗 + 画外壳"，所以刚弹出来的一瞬间确实可能
                 *    "宿主已经摆好、外壳还没画上"（实测过），所以这里**轮询到稳定**再判。
                 *    ⚠️ 还要**允许重试**：菜单自己有一条"刚关掉 400ms 内不许再弹"的守卫，
                 *       而 `Place()` 里那句 `TopMost = true` 会让菜单在摆放过程中被激活、
                 *       于是偶尔"刚弹出来就被失活收掉"（打包那一趟就撞到过一次，
                 *       实测结果里两次都是 `visible:false`）。撞到就等一下重弹一次，
                 *       最多三轮；三轮都没亮过才算真出事（并把最后一份实测结果打出来）。
                 */
                string bothShown = null;
                string lagging = null;
                string lastSeen = null;
                for (var attempt = 0; attempt < 3 && bothShown == null; attempt++)
                {
                    await CallAsync(core, "window.dshLookup.debug.hideTrayMenu()", 15000);
                    await Task.Delay(500); // 越过那条 400ms 的重开守卫
                    await CallAsync(core, "window.dshLookup.debug.showTrayMenu()", 15000);
                    for (var i = 0; i < 12; i++)
                    {
                        await Task.Delay(250);
                        lastSeen = ValueOf(await CallAsync(core,
                            "window.dshLookup.debug.window('tray')", 15000));
                        if (lastSeen == null) continue;
                        var n = visibleTrueCount(lastSeen);
                        if (n == 2 && bothShown == null) bothShown = lastSeen;
                        if (lastSeen.Contains("\"role\":\"tray\",\"visible\":true") && n == 1 && lagging == null)
                            lagging = lastSeen;
                        if (bothShown != null && lagging == null) break;
                    }
                }
                Ok(bothShown != null,
                   "⑩ 菜单弹出来时**两层都亮过**（宿主窗 + 外壳层）：" + (bothShown ?? ("（三轮都没亮）" + lastSeen)));
                Ok(lagging == null,
                   "⑩ ★ 宿主窗亮着时外壳层也必须亮着（投影不许掉队）：" + (lagging ?? "（没看到）"));
                /* 让**别的窗口**去抢前台，把"失活"这一幕造出来 */
                await CallAsync(core, "window.dshLookup.debug.focus()", 15000);
                var sawDeactivate = false;
                var residue = (string)null;
                for (var i = 0; i < 8; i++)
                {
                    await Task.Delay(150);
                    var sample = ValueOf(await CallAsync(core,
                        "window.dshLookup.debug.window('tray')", 15000));
                    if (sample == null) continue;
                    var hostHidden = sample.Contains("\"role\":\"tray\",\"visible\":false");
                    if (hostHidden) sawDeactivate = true;
                    /* 宿主收了、外壳还亮着（= 剩下 1 次 visible:true）→ 就是用户看到的残留 */
                    if (hostHidden && visibleTrueCount(sample) >= 1 && residue == null) residue = sample;
                }
                Ok(sawDeactivate, "⑩ ★ 失活（或者别的原因）之后宿主菜单窗确实收了");
                Ok(residue == null,
                   "⑩ ★★ 收的时候**外壳层（投影）也跟着收** —— 不许出现「按钮层没了、阴影还在」：" +
                   (residue ?? "（8 次采样都没看到残留）"));
                await CallAsync(core, "window.dshLookup.debug.hideTrayMenu()", 15000);
                await Task.Delay(250);
                var afterHide = ValueOf(await CallAsync(core,
                    "window.dshLookup.debug.window('tray')", 15000));
                Ok(visibleTrueCount(afterHide) == 0,
                   "⑩ ★★ 页面叫收之后两层都收了（一次 `visible:true` 都不该剩）：" + afterHide);
            }

            /* ══ ⑪ 窗口焦点那一路：外壳推给页面 + 失焦自动吸边 ═══════════════════ */
            /*
             * 2026-09 用户点出"搬家漏项要修"：参考实现的 `FloatingWindow` 挂了 `Activated` /
             * `Deactivate` 两个处理器（把焦点变化推给页面 + 失焦满一秒自动吸边），而这一版
             * 把窗口拆成 `FloatingWindow` + `FloatingLayout` 时**两条都没搬过来**：
             * 页面里 `window.blur` 实测收不到，于是"失焦就收起浮层"（输入框那排复制/剪切、
             * 右键菜单）永远不触发，胶囊也不会自己吸回边缘。
             *
             * 检查标准落在**页面侧**（"页面究竟收到了什么 / 页面究竟变成什么样"）：
             *   ① 页面自己订阅一次 `onWindowFocus`，把收到的值记进 `window.__dshFocusLog`；
             *   ② 失焦之后读 DOM：`#strip` 露出来、`#content` 藏起来、`#strip[data-edge]` 是哪条边
             *      —— 与 `main.ts` 里 `applied.absorbed` 那两行一一对应。
             * 造"失焦"这一幕：先 `debug:focus()` 把前台给悬浮窗，再 `openManager` 让选项窗抢走它。
             * ⚠️ 自动吸边的前提是"胶囊离某条边 ≤28 DIP"，所以先 `debug:placePill(8, 200)` 贴左边缘。
             */
            await ReadTextAsync(core,
                "(function () { window.__dshFocusLog = [];" +
                " window.dshLookup.floating.onWindowFocus(function (v) { window.__dshFocusLog.push(v); });" +
                " return 'ok'; })()");
            await CallAsync(core, "window.dshLookup.debug.placePill(8, 200)", 15000);
            await Task.Delay(400);
            await CallAsync(core, "window.dshLookup.debug.focus()", 15000);
            await Task.Delay(700);
            await CallAsync(core, "window.dshLookup.floating.openManager('dicts')", 15000);
            /* 失焦事件是立刻到的，自动吸边要等那张 1 秒的表 —— 一起等够 */
            await Task.Delay(1800);
            var focusLog = await ReadTextAsync(core, "JSON.stringify(window.__dshFocusLog)");
            var absorbedDom = await ReadTextAsync(core,
                "(function () { var s = document.getElementById('strip'), c = document.getElementById('content');" +
                " return JSON.stringify({ stripHidden: s.hidden, contentHidden: c.hidden," +
                " edge: s.dataset.edge || '' }); })()");
            /* ⚠️ 先判**这一幕造出来了没有**，再判产品对不对。
             * 这一节要"真的把前台给悬浮窗、再让选项窗抢走"才判得动；而自动化这一侧
             * **拿不稳前台焦点**（Windows 的前台锁 / 桌面被别的窗口占着）：那时页面
             * **一个焦点事件都收不到**（实测 `[]`），`#strip` 也不会吸回 —— 这不是产品坏了，
             * 是"失焦"这一幕没造出来。照「选中文字那排浮层」那一节的范式**明着跳过**并说清原因，
             * 改由 脚本观察（实测结果与经过；
             * ⚠️ 这里原来写的是""，而那个小节**并不存在** —— 又一次"引用了还没写的节号"，
             *    2026-09 顺手改成真写了这一条的地方）。 */
            var focusSceneHeld = focusLog != null && focusLog.Contains("true");
            if (!focusSceneHeld)
            {
                Say(" [跳过] ⑪ 窗口焦点那一路**这次不判**：`debug:focus()` 没能把前台给悬浮窗");
                Say("        （页面实测收到 " + focusLog + "）—— 自动化拿不稳前台焦点，");
                Say("        碰运气的断言比没有更坏。改由 C 级观察：");
                Say("        `node tools/probe-page.mjs --tray --tray-deactivate`。");
                /* 收尾：把选项窗关掉，别把这一幕留给后面的小节 */
                await CallAsync(core, "window.dshLookup.manager.closeManager()", 15000);
                await Task.Delay(300);
            }
            else
            {
                Ok(focusLog.Contains("false"),
                   "⑪ ★ 失焦时页面**真的收到**事件（`floating:window-focus`，实测收到 " + focusLog + "）");
                Ok(absorbedDom != null && absorbedDom.Contains("\"contentHidden\":true") &&
                   absorbedDom.Contains("\"stripHidden\":false"),
                   "⑪ ★★ 失焦满一秒后**胶囊自己吸回边缘**（页面实测结果 " + absorbedDom + "）");
                /* 收尾：把选项窗关掉、把前台还给悬浮窗 —— 页面该收到 focused:true */
                await CallAsync(core, "window.dshLookup.manager.closeManager()", 15000);
                await Task.Delay(300);
                await CallAsync(core, "window.dshLookup.debug.focus()", 15000);
                await Task.Delay(500);
                var focusLog2 = await ReadTextAsync(core, "JSON.stringify(window.__dshFocusLog)");
                Ok(focusLog2 != null && focusLog2.Contains("true"),
                   "⑪ ★ 重新拿到焦点时也推一次（实测收到 " + focusLog2 + "）");
            }

            /* ══ ⑫ 托盘图标左键双击的「呼出」三条（用户 2026-09 定的约定）═══════════ */
            /*
             * 约定（原话）：「如果胶囊已被吸附进入屏幕边缘，就取消吸附；如果是收入托盘状态，
             * 则显示出来。**如果已经显示，则双击无动作。**」
             *
             * ⚠️ 托盘图标**本身点不到**（通知区域里的图标，自动化要算任务栏坐标 + SendInput，
             *    既不稳也会动到用户的桌面）—— 所以这里**直接调双击会调的那个方法**
             *    （`FloatingWindow.TraySummonFloating`），把**三种行为**逐条针对；
             *    "双击有没有接到这个方法"只剩 `Program.cs` 里那一行构造参数，靠源码核对。
             *    状态实测结果走页面：`debug.window('floating')` 的 `visible` / `placement.absorbed`
             *    —— 这是**问系统**（窗口可见性 + 布局里那份吸附事实），不是自己记的账。
             */
            Func<Task<string>> floatingState = async delegate
            {
                return Str(ValueOf(await CallAsync(core,
                    "(async () => { const w = await window.dshLookup.debug.window('floating');" +
                    " const p = w.placement || {};" +
                    " return [w.visible, p.absorbed, p.edge || '-'," +
                    " Math.round(w.bounds.x) + ',' + Math.round(w.bounds.y)].join('|'); })()", 15000)));
            };
            /* ① 吸附在左边缘 → 呼出 = 取消吸附（窗口仍然在） */
            win.AbsorbToEdge("left");
            await Task.Delay(600);
            var absorbedBefore = await floatingState();
            Ok(absorbedBefore.StartsWith("true|true"),
               "⑫ 先把胶囊吸附到左边缘（实测结果 visible|absorbed|edge|x,y = " + absorbedBefore + "）");
            win.TraySummonFloating();
            await Task.Delay(700);
            var absorbedAfter = await floatingState();
            Ok(absorbedAfter.StartsWith("true|false"),
               "⑫ ★ 吸着的时候「呼出」→ **取消吸附**、界面弹回来（实测结果 " + absorbedAfter + "）");
            /* ② 收入托盘 → 呼出 = 显示出来 */
            win.HideWindow();
            await Task.Delay(500);
            var hiddenBefore = await floatingState();
            Ok(hiddenBefore.StartsWith("false|"),
               "⑫ 先把窗口收进托盘（实测结果 " + hiddenBefore + "）");
            win.TraySummonFloating();
            await Task.Delay(800);
            var hiddenAfter = await floatingState();
            Ok(hiddenAfter.StartsWith("true|"),
               "⑫ ★ 收进托盘时「呼出」→ 显示出来（实测结果 " + hiddenAfter + "）");
            /* ③ 已经显示着、也没吸附 → **什么都不做**（前后两次实测结果必须逐字符相同） */
            var idleBefore = await floatingState();
            Ok(idleBefore.StartsWith("true|false"),
               "⑫ 这一档的前提是「已经显示着」（实测结果 " + idleBefore + "）");
            win.TraySummonFloating();
            await Task.Delay(700);
            var idleAfter = await floatingState();
            Ok(idleAfter == idleBefore,
               "⑫ ★★ 已经显示着的时候「呼出」→ **什么都不做**（呼出前 " + idleBefore +
               " / 呼出后 " + idleAfter + "）");

            /* ══ ⑬ 常规页那两个开关：注册表往返 + 设置落盘（2026-09 补）═══════════
             *
             * ── 为什么归 （降不到 A/B/C 的理由）──────────────────────────────
             *   · 「开机自动启动」真的读写 `HKCU\…\Run` —— 那是**机器状态**，
             *     而且"跑完必须退回原值"只有这一级能兜住（下面 finally 里就是它）。
             *   · 「启动时显示悬浮窗」的**效果**（下一次启动不显示胶囊）要**重启一次程序**才验得到，
             *     那一步在  的第二趟启动里；这里钉的是
             *     "页面改得动、内核收得下、页面看得见真实值"这一段。
             *
             * ⚠️ 这一节**真的会动用户的注册表**（写进去、再删掉），所以：
             *   ① 进来先把两个值（`查词` 与旧条目名 `lookup`）**整串抄下来**；
             *   ② 无论中间成不成功，finally 里都要**逐字符写回去**（原来没有就删掉）；
             *   ③ 退完再**读一次核对**，并把它当成一条断言 —— 退不回去要当场红，
             *      而不是让用户下次开机发现"多了一个开机自启"。
             *   写入的路径是**这一次跑的 exe**（自检跑的是开发产物），退回去就等于什么都没发生。
             */
            {
                var runKeyPath = @"Software\Microsoft\Windows\CurrentVersion\Run";
                const string runValue = "查词";
                const string legacyValue = "lookup";

                Func<string, string> readRun = delegate (string name)
                {
                    try
                    {
                        using (var key = Registry.CurrentUser.OpenSubKey(runKeyPath, false))
                        {
                            return (key == null) ? null : key.GetValue(name) as string;
                        }
                    }
                    catch (Exception) { return null; }
                };

                var originalRun = readRun(runValue);
                var originalLegacy = readRun(legacyValue);
                Say("  ⑬ 注册表原值：查词=" + Show(originalRun) + " / lookup=" + Show(originalLegacy));

                try
                {
                    var mcore2 = await win.Manager.Ready;
                    await CallAsync(core, "window.dshLookup.floating.openManager('general')", 15000);
                    await Task.Delay(700);
                    /*
                     * 顺带再钉一次"标题跟着页签走"（这一节把管理窗翻到了**常规**页）——
                     * 与第 ⑨ 节那条是同一个约定，只是换了页签：**换页签要换标题**。
                     */
                    var titleGeneral = "";
                    for (var i = 0; i < 25 && titleGeneral != "选项 · 常规"; i++)
                    {
                        titleGeneral = win.Manager.Text;
                        if (titleGeneral != "选项 · 常规") await Task.Delay(200);
                    }
                    Ok(titleGeneral == "选项 · 常规",
                       "⑬ ★ 翻到常规页之后窗口标题也跟着变了（实测「" + titleGeneral + "」）");

                    /* ① 页面上的勾选与**系统里的真实状态**一致（数值是壳从注册表读的） */
                    Func<Task<string>> loginRow = async delegate
                    {
                        return Str(ValueOf(await CallAsync(mcore2,
                            "(async () => { const s = await window.dshLookup.tray.getState();" +
                            " return [document.getElementById('loginAtStartup').checked, s.loginAtStartup]" +
                            ".join('|'); })()", 15000)));
                    };
                    /*
                     * ⚠️ 等"翻过去"这一步**必须走 `CallAsync`**（它能等 Promise），
                     *    不能用 `WaitTextAsync` —— 那个助手是把表达式塞进 `String(...)` 里读的，
                     *    表达式是 async 的话读到的是 `[object Promise]`（第一版就是这么写的，
                     *    于是"点一下没生效"和"等待方式不对"分不开）。
                     */
                    Func<string, Task<string>> waitLogin = async delegate (string want)
                    {
                        for (var i = 0; i < 30; i++)
                        {
                            var v = Str(ValueOf(await CallAsync(mcore2,
                                "window.dshLookup.tray.getState().then(function (s) {" +
                                " return s.loginAtStartup; })", 10000)));
                            if (v == want) return v;
                            await Task.Delay(200);
                        }
                        return "（30 次都没等到 " + want + "）";
                    };
                    var loginBefore = await loginRow();
                    Ok(loginBefore == (Startup.IsLoginAtStartup() ? "true|true" : "false|false"),
                       "⑬ 勾选框与系统里的真实状态一致（实测结果 勾选|状态 = " + loginBefore + "）");

                    /* ② 点一下：注册表**真的**写进去了，而且壳回的是真实状态 */
                    await CallAsync(mcore2,
                        "(function () { document.getElementById('loginAtStartup').click(); })()", 15000);
                    var afterOn = await waitLogin("true");
                    Ok(afterOn == "true", "⑬ ★ 从常规页勾上「开机自动启动」→ 系统说它是开着的（实测结果 " +
                                          afterOn + "）");
                    var written = readRun(runValue);
                    Ok(!string.IsNullOrEmpty(written) &&
                       written.IndexOf(System.Windows.Forms.Application.ExecutablePath,
                                       StringComparison.OrdinalIgnoreCase) >= 0,
                       "⑬ ★ 而且注册表里**真的**写下了这一次的 exe 路径（" + Show(written) + "）");

                    /* ③ 再点回来：注册表回到原值（这一条只退回到"没勾"这一档，finally 里还有一次核对） */
                    await CallAsync(mcore2,
                        "(function () { document.getElementById('loginAtStartup').click(); })()", 15000);
                    var afterOff = await waitLogin("false");
                    Ok(afterOff == "false", "⑬ ★ 再点回来 → 系统说它又关掉了（实测结果 " + afterOff + "）");
                    var afterOffRun = readRun(runValue);
                    Ok(string.IsNullOrEmpty(afterOffRun) || afterOffRun == originalRun,
                       "⑬ 关掉之后注册表里那条回来了/没了（实测结果 " + Show(afterOffRun) + "）");

                    /* ④ 「启动时显示悬浮窗」：页面 ↔ 壳 ↔ 内核 那一条路走得通、值也回得来 */
                    Func<Task<string>> showRow = async delegate
                    {
                        return Str(ValueOf(await CallAsync(mcore2,
                            "(async () => { const v = await window.dshLookup.manager.getShowFloatingOnStartup();" +
                            " return [document.getElementById('showOnStartup').checked, v].join('|'); })()",
                            15000)));
                    };
                    var showBefore = await showRow();
                    Ok(showBefore == "true|true" || showBefore == "false|false",
                       "⑬ 勾选框与内核里的值一致（实测结果 勾选|值 = " + showBefore + "）");
                    await CallAsync(mcore2,
                        "(function () { document.getElementById('showOnStartup').click(); })()", 15000);
                    await Task.Delay(700);
                    var showAfter = await showRow();
                    Ok(showAfter == "false|false" || showAfter == "true|true",
                       "⑬ ★ 从常规页改一下 → 页面与内核**同时**变了（实测结果 " + showAfter + "）");
                    Ok(showAfter != showBefore, "⑬ ★ 而且确实翻了过去（改之前 " + showBefore + "）");
                    /*
                     * ⚠️ 它的**效果**（下一次启动不显示胶囊）在这一级验不了 —— 那要重启一次程序，
                     *    落在  的第二趟启动那一段（两个方向都验）。
                     */
                    /* 收尾：改回原样（"进来什么样出去什么样"是规矩） */
                    await CallAsync(mcore2,
                        "(function () { document.getElementById('showOnStartup').click(); })()", 15000);
                    await Task.Delay(700);
                    var showBack = await showRow();
                    Ok(showBack == showBefore, "⑬ ★ 再点回来 → 回到进来时的值（" + showBack + "）");
                }
                finally
                {
                    /* ③ 无论成败：把两个值**逐字符**写回去（原来没有就删掉），再读一遍核对 */
                    try
                    {
                        using (var key = Registry.CurrentUser.CreateSubKey(runKeyPath))
                        {
                            if (key != null)
                            {
                                if (originalRun == null) key.DeleteValue(runValue, false);
                                else key.SetValue(runValue, originalRun);
                                if (originalLegacy == null) key.DeleteValue(legacyValue, false);
                                else key.SetValue(legacyValue, originalLegacy);
                            }
                        }
                        var restored = readRun(runValue);
                        Ok(restored == originalRun,
                           "⑬ ★ 跑完把注册表**退回原样**（原 " + Show(originalRun) + " → 现 " + Show(restored) + "）");
                        Ok(readRun(legacyValue) == originalLegacy,
                           "⑬ 旧条目名 lookup 也退回原样（现 " + Show(readRun(legacyValue)) + "）");
                    }
                    catch (Exception err)
                    {
                        Ok(false, "⑬ 退回注册表失败（这一条要人手工看一眼）：" + err.Message);
                    }
                }
            }

            /* ══ ⑭ 联网那几条路**不占住界面**：两段式回包管线（2026-09-24 加）════════
             *
             * 钉的是什么：机器翻译 / 在线发音 / 检测凭据 / 查词链里的自动翻译四条路
             * **要发 HTTP**，而它们原来在 **UI 线程**上同步发（`TranslateHttp.TimeoutMs = 8000`）
             * —— 表现是"点一下，整扇窗最长 8 秒不动"。参考实现是把这两条丢在 `Task.Run` 上的
             * （`SpeechService.SpeakAsync` / `App.cs` 的 `Translation.Translate`），
             * 所以这是**搬家时弄丢的行为**。
             *
             * ── 为什么这一条降不到 A / B / C ──────────────────────────────────────
             * 它要的是"**同一个进程里，一条慢活的等待期间，另一条请求还能马上回来**" ——
             * 需要真实的消息泵 + 真实的桥 + 真实的 UI 线程，（不启动程序）做不到，
             * 脚本只能"看现场"、不能断言时序。所以放。
             *
             * ── 为什么用 `debug.slowReply` 而不是真去发一次翻译 ────────────────────
             * 真发要**真 Key**（`--mt-key`），而 检查 的常规跑法不带 Key；`slowReply` 走的
             * 是**同一条管线**（`RunHttpDeferred` → `DeferredReply` → `WebViewBridge.PostReply`），
             * 只是把"HTTP"换成"睡一觉"。真联网那两条路本身由第 ⑰ / ⑱ 节（带真 Key 时）覆盖。
             *
             * ⚠️ 实测结果必须**当场量**（不写死期望值）：三次普通桥调用的耗时与慢活的总时长一起报出来。
             */
            {
                /*
                 * ① **两个线程号的对账**（这一条才是检查标准）：
                 *    · 普通桥调用（= `debug:thread`）在哪条线程上被处理 → 那就是"UI 线程"；
                 *    · 慢活**干活**那一段在哪条线程上、**回包**那一句又在哪条线程上。
                 *    "干活不在 UI 线程、回包在 UI 线程"两件都对，才算两段式成立了。
                 *
                 * ⚠️ 为什么不用**耗时**当检查标准：第一版就是"慢活等着的时候量另外三次桥调用的耗时
                 *    （阈值 400 ms）"—— 那条断言在**把实现改回同步**之后**照样绿**（实测），
                 *    也就是说它跟着错误一起绿。原因在 里记着（自检这一侧与页面那一侧的
                 *    消息处理线程不是同一条）。**线程号不会骗人**。
                 */
                var threadJson = Str(ValueOf(await CallAsync(core,
                    "window.dshLookup.debug.thread()", 15000)));
                var uiThread = threadJson == null ? -1 : (int)VirtualHost.JsonLong(threadJson, "thread");

                var slowRaw = Str(ValueOf(await CallAsync(core,
                    "window.dshLookup.debug.slowReply(600)", 20000)));
                var workThread = slowRaw == null ? -2 : (int)VirtualHost.JsonLong(slowRaw, "workThread");
                var replyThread = slowRaw == null ? -3 : (int)VirtualHost.JsonLong(slowRaw, "replyThread");
                Say("  ⑭ 实测结果：UI 线程=" + uiThread + "；慢活干活线程=" + workThread +
                    "；回包线程=" + replyThread + "（" + Show(slowRaw) + "）");

                Ok(uiThread > 0 && workThread > 0 && replyThread > 0,
                   "⑭ 三处线程号都读到了（UI=" + uiThread + " / 干活=" + workThread +
                   " / 回包=" + replyThread + "）");
                Ok(workThread != uiThread,
                   "⑭ ★★ 要联网那段活**不在 UI 线程上**干（UI=" + uiThread + "，干活=" + workThread + "）");
                Ok(replyThread == uiThread,
                   "⑭ ★★ 而回包那一句**回到 UI 线程**上做（UI=" + uiThread + "，回包=" + replyThread + "）");

                /*
                 * ② 顺带报一条**耗时**实测结果（不是检查标准，是给人看的现场）：
                 *    慢活还在等的时候，别的桥调用照样马上回。
                 */
                await core.ExecuteScriptAsync(
                    "window.__dshSlowDone = false; window.__dshSlowValue = '';" +
                    " window.dshLookup.debug.slowReply(900).then(function (v) {" +
                    "   window.__dshSlowValue = JSON.stringify(v); window.__dshSlowDone = true; }," +
                    "  function (e) { window.__dshSlowValue = 'ERR ' + String(e && e.message || e);" +
                    "   window.__dshSlowDone = true; });");
                await Task.Delay(200);   /* 让那条请求真的进到"等"里面去 */
                var fast = new List<long>();
                for (var i = 0; i < 3; i++)
                {
                    var t0 = DateTime.UtcNow;
                    await CallAsync(core, "window.dshLookup.debug.window('floating')", 15000);
                    fast.Add((long)(DateTime.UtcNow - t0).TotalMilliseconds);
                }
                var fastText = string.Join(" / ", fast.ConvertAll(delegate (long v) { return v.ToString(); }).ToArray());
                var slowDone = await WaitTextAsync(core, "String(window.__dshSlowDone)",
                                                   v => v == "true", 8000);
                var slowValue = await ReadTextAsync(core, "String(window.__dshSlowValue)");
                Say("  ⑭ 实测结果（只报不判）：慢活 " + Show(slowValue) + "；等待期间三次普通调用 " +
                    fastText + " ms");
                Ok(slowDone != null && slowValue != null && slowValue.Contains("sleptMs"),
                   "⑭ 慢活回了包（实测结果 " + Show(slowValue) + "）");
            }

            /* ── 壳这一侧出过的岔子（有一条就该有人看）──────────────────────── */
            DumpFailures(bridge);
            return Finish();
        }

        /* ══════════════════════════════════════════════════════════════════════
           小工具
           ══════════════════════════════════════════════════════════════════════ */

        /// <summary>
        /// 「选中文字那排浮层」那一节判不判 —— **恒 false**（= 明着跳过）。
        ///
        /// 为什么留着这个方法而不是写 `if (false)`：见调用点那段说明（CS0162）。要重新判它，
        /// 把这里改成 true 就行（那一节的代码一直在下面躺着，没删）。
        /// </summary>
        private static bool JudgeSelectionLayer()
        {
            return false;
        }

        private static void DumpFailures(WebViewBridge bridge)
        {
            if (bridge == null) return;
            Ok(bridge.Failures.Count == 0,
               "壳这一侧没有记下岔子（实际 " + bridge.Failures.Count + " 条）");
            foreach (var item in bridge.Failures) Say("      · " + item);
            foreach (var blocked in bridge.BlockedUrls) Say("      · 被挡下的地址：" + blocked);
        }

        private struct Box
        {
            internal int Left, Top, Width, Height;
        }

        /// <summary>从一段 JSON 里抠一个矩形（`{"x":…,"y":…,"width":…,"height":…}`）</summary>
        private static Box? RectOf(string json, string path)
        {
            var text = TextAt(json, path);
            if (text == null) return null;
            var box = new Box
            {
                Left = (int)VirtualHost.JsonLong(text, "x"),
                Top = (int)VirtualHost.JsonLong(text, "y"),
                Width = (int)VirtualHost.JsonLong(text, "width"),
                Height = (int)VirtualHost.JsonLong(text, "height"),
            };
            return box;
        }

        /// <summary>
        /// 取某个路径上的**原始 JSON 片段**（支持 `chrome.bounds` 这种两层的）。
        /// ⚠️ 用手写的括号配对而不是引 JSON 库：这一层只处理内核/壳自己发出来的形状，
        ///    而且"少一个依赖"在这条路上一直是划算的（与 `ShellBridge.RawValue` 同源）。
        /// </summary>
        private static string TextAt(string json, string path)
        {
            if (string.IsNullOrEmpty(json)) return null;
            var parts = path.Split('.');
            var current = json;
            foreach (var part in parts)
            {
                current = RawOf(current, part);
                if (current == null) return null;
            }
            return current;
        }

        /// <summary>
        /// 原始 JSON 片段 → **裸字符串**。
        /// ⚠️ `TextAt` 收回来的是**带引号的原始 JSON**（`"https://…"`）—— 直接拿它比前缀
        ///    永远不相等，而症状只是"这条断言红了"（那对引号在实测结果里很容易被忽略，本轮就栽过一次）。
        /// </summary>
        private static string Str(string raw)
        {
            if (string.IsNullOrEmpty(raw)) return raw;
            if (raw.Length < 2 || raw[0] != '"') return raw;
            var body = raw.Substring(1, raw.Length - 2);
            return body.Replace("\\\"", "\"").Replace("\\\\", "\\");
        }

        private static string RawOf(string json, string key)
        {
            var pat = "\"" + key + "\":";
            var at = json.IndexOf(pat, StringComparison.Ordinal);
            if (at < 0) return null;
            at += pat.Length;
            while (at < json.Length && json[at] == ' ') at++;
            if (at >= json.Length) return null;

            var c0 = json[at];
            if (c0 == '"')
            {
                var end = at + 1;
                while (end < json.Length && json[end] != '"')
                {
                    if (json[end] == '\\') end++;
                    end++;
                }
                return json.Substring(at, end - at + 1);
            }
            if (c0 == '{' || c0 == '[')
            {
                var close = (c0 == '{') ? '}' : ']';
                var depth = 0;
                var inString = false;
                for (var i = at; i < json.Length; i++)
                {
                    var c = json[i];
                    if (inString)
                    {
                        if (c == '\\') { i++; continue; }
                        if (c == '"') inString = false;
                        continue;
                    }
                    if (c == '"') { inString = true; continue; }
                    if (c == c0) depth++;
                    else if (c == close)
                    {
                        depth--;
                        if (depth == 0) return json.Substring(at, i - at + 1);
                    }
                }
                return null;
            }
            var stop = at;
            while (stop < json.Length && json[stop] != ',' && json[stop] != '}') stop++;
            return json.Substring(at, stop - at).Trim();
        }

        private static bool SameBox(Box a, Box b)
        {
            return a.Left == b.Left && a.Top == b.Top && a.Width == b.Width && a.Height == b.Height;
        }

        private static string Show(string s) { return s == null ? "(null)" : "\"" + s + "\""; }

        /// <summary>
        /// 某个路径上的数（没有 / 不是数字 = null）。
        ///
        /// ⚠️ **比数字，不要比文本**：这些值从内核出来要穿过 `page → 桥 → ExecuteScriptAsync`
        ///    这一路，而**中间那一段是 JavaScript** —— 它把 JSON 里的 `-3.0` 读成数字 -3、
        ///    再序列化回 `-3`（JS 的数字没有"几位小数"这回事）。所以拿 `"-3.0"` 去比文本
        ///    必然失败，而失败信息看着像"内核没写进去"（就是这么被绊了一下）。
        ///    检查标准要钉的是**那个数**，不是它的字符串写法。
        /// </summary>
        private static double? NumAt(string json, string path)
        {
            var raw = TextAt(json, path);
            if (string.IsNullOrEmpty(raw)) return null;
            double value;
            return double.TryParse(raw, System.Globalization.NumberStyles.Float,
                                   System.Globalization.CultureInfo.InvariantCulture, out value)
                ? value
                : (double?)null;
        }

        /// <summary>两个增益值是不是同一个数（差在 0.001 以内；任一个是 null 就不算）</summary>
        private static bool Nearly(double? actual, double expected)
        {
            return actual.HasValue && Math.Abs(actual.Value - expected) < 0.001;
        }

        private static string ShowNum(double? value)
        {
            return value.HasValue
                ? value.Value.ToString(System.Globalization.CultureInfo.InvariantCulture)
                : "(没读到)";
        }

        private static string Short(string s)
        {
            if (string.IsNullOrEmpty(s)) return "(空)";
            return s.Length <= 8 ? s : s.Substring(0, 8) + "…";
        }

        private static string JsString(string s)
        {
            var text = s ?? "";
            return "\"" + text.Replace("\\", "\\\\").Replace("\"", "\\\"") + "\"";
        }

        private static int Finish()
        {
            Say("shell-app：" + _checks + " 项，失败 " + _failed);
            return _failed == 0 ? 0 : 1;
        }

        /* ── 三个读写页面的小工具（`ExecuteScriptAsync` 不会等 Promise）────────── */

        private static async Task<string> ReadTextAsync(CoreWebView2 core, string expr)
        {
            await WithTimeout(core.ExecuteScriptAsync("window.__dshRead = { value: String(" + expr + ") };"),
                              8000, "读宿主页");
            var raw = await WithTimeout(core.ExecuteScriptAsync("window.__dshRead"), 8000, "读宿主页");
            return VirtualHost.JsonString(raw, "value") ?? "";
        }

        /// <summary>
        /// 给一次 WebView2 调用套上**上限** —— 检查 不许被一次调用吊死。
        ///
        /// 为什么必须有：2026-09 那一轮真栽过一次 —— 第 ⑯ 节里某个
        /// `ExecuteScriptAsync` 再也没回来（渲染进程是活的，CDP 还能读它），
        ///  检查 就这么挂着，**10 分钟没有实测结果**，看起来像"检查 超时"而不是"某一条检查标准坏了"。
        /// 加上上限之后，同样的情况变成一条**带原因的失败**（`FrameTextAsync` 回 null → 检查标准变红），
        /// 报告照常写完、退出码照常给出来。
        /// </summary>
        private static async Task<T> WithTimeout<T>(Task<T> task, int timeoutMs, string what)
        {
            var done = await Task.WhenAny(task, Task.Delay(timeoutMs));
            if (done != task) throw new TimeoutException("WebView2 调用超过 " + timeoutMs + "ms 没回来：" + what);
            return await task;
        }

        private static async Task<string> WaitTextAsync(CoreWebView2 core, string expr,
                                                        Func<string, bool> accept, int timeoutMs)
        {
            var deadline = DateTime.UtcNow.AddMilliseconds(timeoutMs);
            while (DateTime.UtcNow < deadline)
            {
                var got = await ReadTextAsync(core, expr);
                if (accept(got)) return got;
                await Task.Delay(60);
            }
            return null;
        }

        /// <summary>在页面里算一个表达式（可以是 Promise），回 `{"value":…}` 或 `{"rejected":…}`</summary>
        private static async Task<string> CallAsync(CoreWebView2 core, string expr, int timeoutMs)
        {
            var js = new StringBuilder();
            js.Append("window.__dshCall = null; (function () { var p; try { p = (");
            js.Append(expr);
            js.Append("); } catch (e) { window.__dshCall = { rejected: String((e && e.message) || e) }; return; }");
            js.Append(" Promise.resolve(p).then(");
            js.Append("  function (v) { window.__dshCall = { value: (typeof v === 'string') ? v : JSON.stringify(v) }; },");
            js.Append("  function (e) { window.__dshCall = { rejected: String((e && e.message) || e) }; }); })();");
            await core.ExecuteScriptAsync(js.ToString());

            var deadline = DateTime.UtcNow.AddMilliseconds(timeoutMs);
            while (DateTime.UtcNow < deadline)
            {
                var raw = await core.ExecuteScriptAsync("window.__dshCall");
                if (raw != null && raw != "null") return raw;
                await Task.Delay(40);
            }
            throw new TimeoutException("等页面里的表达式超时：" + expr);
        }

        private static string ValueOf(string raw) { return VirtualHost.JsonString(raw, "value"); }
        private static string RejectedOf(string raw) { return VirtualHost.JsonString(raw, "rejected"); }

        /* ── 词条那一页住在**跨源 iframe** 里，读它要拿框架句柄 ───────────────── */

        private static async Task<CoreWebView2Frame> FindEntryFrameAsync(
            List<CoreWebView2Frame> frames, string mustContain, int timeoutMs, bool newest)
        {
            var deadline = DateTime.UtcNow.AddMilliseconds(timeoutMs);
            while (DateTime.UtcNow < deadline)
            {
                var list = new List<CoreWebView2Frame>(frames);
                if (newest) list.Reverse();
                foreach (var frame in list)
                {
                    var text = await FrameTextAsync(frame,
                        "String((document.body && document.body.textContent) || '')");
                    if (text != null && text.Contains(mustContain)) return frame;
                }
                await Task.Delay(200);
            }
            return null;
        }

        /// <summary>
        /// 在**词条正文里**选中包含 `needle` 的那一小段文字，并让文档自己把选区报上来。
        ///
        /// 为什么必须派发 `mouseup`：词条正文是在 `selectionchange` / `mouseup` / `keyup`
        /// 上节流上报选区的（见内核 `entry_assets.h` 里那段桥接脚本）——
        /// 只调 `getSelection().addRange()` 而不派发事件的话，它**不会**上报。
        /// </summary>
        private static async Task<bool> SelectInEntryAsync(CoreWebView2Frame frame, string needle)
        {
            var js =
                "(function () { var needle = " + JsString(needle) + ";" +
                " var walker = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT, null, false);" +
                " var node; while ((node = walker.nextNode())) {" +
                "   var at = (node.nodeValue || '').indexOf(needle);" +
                "   if (at < 0) continue;" +
                "   var range = document.createRange();" +
                "   range.setStart(node, at); range.setEnd(node, at + needle.length);" +
                "   var sel = window.getSelection(); sel.removeAllRanges(); sel.addRange(range);" +
                "   document.dispatchEvent(new MouseEvent('mouseup', { bubbles: true }));" +
                "   return 'ok'; } return ''; })()";
            var result = await FrameTextAsync(frame, js);
            return result == "ok";
        }
        private static async Task<string> FrameTextAsync(CoreWebView2Frame frame, string expr)
        {
            try
            {
                await WithTimeout(
                    frame.ExecuteScriptAsync("window.__dshRead = { value: String(" + expr + ") };"),
                    8000, "在词条正文里求值");
                var raw = await WithTimeout(frame.ExecuteScriptAsync("window.__dshRead"), 8000, "读词条正文");
                return VirtualHost.JsonString(raw, "value") ?? "";
            }
            catch (Exception err)
            {
                /* 记下**为什么**（超时 / 框架失效 / COM 错）—— 回 null 的那条路不该把原因吞掉 */
                LastFrameError = err.GetType().Name + ": " + err.Message;
                return null; /* 框架没了（导航中 / 已失效）/ 调用超时 —— 上层会重试 */
            }
        }

        /// <summary>最近一次词条正文调用失败的原因（`FrameTextAsync` 回 null 时用来说明为什么不写断言）</summary>
        private static string LastFrameError = "(还没失败过)";
    }
}
