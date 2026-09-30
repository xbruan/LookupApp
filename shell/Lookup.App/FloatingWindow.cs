namespace Lookup.App
{
    using System;
    using System.Diagnostics;
    using System.Drawing;
    using System.IO;
    using System.Threading.Tasks;
    using System.Windows.Forms;
    using Lookup.Host;
    using Lookup.Interop;
    using Microsoft.Web.WebView2.Core;
    using Microsoft.Web.WebView2.WinForms;

    /// <summary>
    /// 0.2.0 的窗口本体：一个 `Form` + 一个铺满它的 WebView2（界面外观沿用参考实现那一份）。
    /// ⚠️ WebView2 的事件（`NavigationCompleted` / `WebMessageReceived`）都在 **UI 线程**上送，
    /// 所以在这条路里**绝不许同步 `Wait()`** —— 消息泵一停，事件永远送不到；等待一律 `await`
    /// （导航等待在 <see cref="WebViewSetup.WaitNavigationAsync"/>）。
    /// </summary>
    internal sealed class FloatingWindow : Form, IShellHost
    {
        internal const string StartUrl =
            "https://" + VirtualHost.ShellDomain + "/" + ShellAssetSource.DefaultDocument;

        private readonly AppOptions _opts;
        private readonly IntPtr _engine;
        private readonly WebView2 _web = new WebView2();
        private readonly WebViewBridge _bridge;
        private readonly TaskCompletionSource<CoreWebView2> _ready =
            new TaskCompletionSource<CoreWebView2>();

        internal FloatingWindow(IntPtr engine, AppOptions opts)
        {
            _opts = opts;
            _engine = engine;
            _bridge = new WebViewBridge(engine, opts.WebRoot, this);

            /*
             * 窗口属性：`FormBorderStyle.None`（形状由窗口 Region 裁出来，不由系统标题栏画）+
             * 不进任务栏 / Alt-Tab + `TopMost`（悬浮窗是桌面挂件）+ `AutoScaleMode.None`
             * （尺寸全按物理像素算好，不让 WinForms 再插一手）。
             * ⚠️ **不要 CS_DROPSHADOW**：系统投影是按**窗口矩形**画的，而悬浮窗比可见的胶囊高得多
             *    （上下预留了面板空间），会出现一圈围着透明区域的怪投影。
             */
            FormBorderStyle = FormBorderStyle.None;
            StartPosition = FormStartPosition.Manual;
            ShowInTaskbar = false;
            TopMost = true;
            MinimizeBox = false;
            MaximizeBox = false;
            AutoScaleMode = AutoScaleMode.None;
            Text = "查词";

            _web.Dock = DockStyle.Fill;
            // 页面的 body 是透明背景（`rgba(0,0,0,0)`），形状完全由 CSS + 窗口 Region 决定
            _web.DefaultBackgroundColor = Color.Transparent;
            Controls.Add(_web);

            _layout = new FloatingLayout(this, OnLayoutEvent);
            /*
             * ★ **窗口焦点那一路**（漏了就会"点别的程序之后浮层挂在桌面上不收、胶囊也不自己吸回去"）：
             * 焦点变化要推给页面（`floating:window-focus`）—— 页面里的 `window.blur` **实测收不到**，
             * "失焦就把浮层收掉"只能靠这一路信号；失焦还要起一个 1 秒的表，到点没焦点就吸回最近的边缘。
             * 两件事都在 `FloatingLayout.OnWindowFocus` 里，这里只负责把事件转过去。
             */
            Activated += delegate { _layout.OnWindowFocus(true); };
            Deactivate += delegate { _layout.OnWindowFocus(false); };
            /*
             * 外壳层：一张独立的分层窗口，压在承载网页的窗口**下面**，把页面报上来的卡片按真实形状重画 ——
             * 于是有了**抗锯齿圆角与投影**（窗口 Region 只能裁硬边，这就是 Region 要往里缩 2px 的原因）。
             */
            _layout.Chrome = new ChromeWindow();

            if (opts.NoShow)
            {
                // 自检：别抢焦点、别在任务栏上闪；窗口**仍然要存在** —— WebView2 的控制器挂在窗口上，藏起来就没有渲染进程。
                ShowInTaskbar = false;
                StartPosition = FormStartPosition.Manual;
                Location = new Point(-4000, -4000);
            }
            else
            {
                /*
                 * ★ **先摆位、再显示**：少了这一步，WinForms 会先把窗口摆在**默认位置 (0,0)**、按默认尺寸
                 * 显示出来，等页面起来、`layout:set` 到了才挪 —— 那一瞬间就是用户看到的"打开后左上角闪一下灰窗口"。
                 * ⚠️ `Handle` 必须先取一次：`ApplyLayout` 里有 `if (!_form.IsHandleCreated) return;`，
                 *    句柄没建出来它就什么都不做，等于这一步白搭。
                 */
                var handle = Handle;
                _layout.SetLayoutInitial();
                /*
                 * ★ **再摆到屏幕外**：几何已经算好了（Region / 布局 / 外壳都按真实矩形走），
                 *    但在页面报"首帧已经出来了"（`boot:ready`）之前，窗口**停在屏幕外** ——
                 *    于是用户不会先看到一层空壳 / 阴影再看到内容。见 `FloatingLayout.ParkOffScreen`。
                 *    ⚠️ 必须是"屏幕外 + 仍在显示状态"，不能改成藏起来：藏起来的窗口 WebView2
                 *       不渲染（rAF 不跑），页面那声 `boot:ready` 就永远等不到。
                 */
                _layout.ParkOffScreen();
            }

            /*
             * 「启动时显示悬浮窗」：**在窗口显示之前**就问一次（读内核设置），随后在 `Shown` 里据此把窗口收起来。
             */
            _startHidden = ReadStartHidden();

            Shown += OnShownAsync;
        }

        /// <summary>窗口的几何与形状（从参考实现抄下来的那一套，见 <see cref="FloatingLayout"/>）</summary>
        private readonly FloatingLayout _layout;

        /// <summary>`WS_EX_TOOLWINDOW`：不出现在 Alt-Tab（参考实现的 `ShellForm.CreateParams` 同）</summary>
        protected override CreateParams CreateParams
        {
            get
            {
                var cp = base.CreateParams;
                cp.ExStyle |= 0x00000080;
                cp.Style &= ~0x00C00000;
                return cp;
            }
        }

        /// <summary>把窗口层要发的事件转给页面（`layout:applied` / `floating:focus-input`）</summary>
        private void OnLayoutEvent(string name, string payloadJson)
        {
            Emit(name, payloadJson);
        }

        /// <summary>页面真的加载起来了（自检那道 检查 等它）。失败时这个 Task 会带着原因抛。</summary>
        internal Task<CoreWebView2> Ready { get { return _ready.Task; } }

        internal WebViewBridge Bridge { get { return _bridge; } }

        /*
         * 全局快捷键（划词查词那条路的入口）整条删掉了，**别再顺手加回来**：剥掉之后"外部程序里选中文字 →
         * 查这个词"只剩在**我们自己的窗口里**点「查这个词」那条路（`floating` 页面自己发起的 `origin: selection`）。
         * 老设置文件里那一节内核不认识，下次存盘自然消失（不给用户留一份看不懂的残留）。
         */
        /// <summary>
        /// 「启动时显示悬浮窗」那一格现在是**关着**的吗（内核设置里的顶层键 `showFloatingOnStartup`）。
        ///
        /// 在**构造时**就问一次、之后不再读：这一条只管"启动那一瞬间"，
        /// 用户在托盘里叫出来的窗口不受它影响（见 `dsh_settings.h` 那段说明）。
        /// </summary>
        private readonly bool _startHidden;

        /**
         * 读一次「启动时显示悬浮窗」。
         *
         * ⚠️ 读不出来（引擎没起来 / JSON 里没有这个键）一律按**显示** —— 那既是默认值，
         *    也是底线：一个读设置的小故障不该让用户开机后**看不到窗口**、还以为程序没启动。
         *    原因照样记一条（`_bridge.Note`），不许不声不响。
         */
        private bool ReadStartHidden()
        {
            try
            {
                string settings;
                Dsh.EngineSettingsGet(_engine, out settings);
                return !VirtualHost.JsonBool(settings, "showFloatingOnStartup", true);
            }
            catch (Exception err)
            {
                _bridge.Note("读「启动时显示悬浮窗」失败，按显示处理：" + err.Message);
                return false;
            }
        }

        private async void OnShownAsync(object sender, EventArgs e)
        {
            /*
             * ★ 「启动时不显示悬浮窗」（2026-09 新加的用户设置）在这里落地：
             * 窗口**必须**先 `Show()`（WebView2 的控制器挂在窗口句柄上，不显示就没有渲染进程，
             * 见上面 `NoShow` 那段），但这一句紧跟在 `Shown` 里、**第一个 await 之前** ——
             * 消息泵还没开始处理绘制，所以屏幕上从来没有出现过一帧胶囊，
             * 用户看不到"闪一下"。收起来之后与"收进托盘"是**同一个状态**：
             * 托盘图标还在，菜单里那一条变成「显示悬浮窗」，双击托盘图标也能叫出来（`TraySummonFloating`）。
             *
             * ⚠️ 别把这一句挪到 await 后面：那时页面已经画出来了 —— 用户会看到胶囊闪一下再消失。
             */
            if (_startHidden) _layout.HideWindow();
            try
            {
                /*
                 * WebView2 的建环境 / 开关 / 设置 / 等导航这一整段走 **WebViewSetup.StartAsync**
                 * （与词库管理窗、托盘菜单同一个入口）。历史上悬浮窗把这段逐行重抄了一份，
                 * 连浏览器开关参数串都是两份 —— 抽 WebViewSetup 就是为了防分叉（2026-09-24 收编）。
                 */
                var core = await WebViewSetup.StartAsync(_web, _opts.ConfigDir, StartUrl,
                                                         c => _bridge.Attach(c),
                                                         _opts != null ? _opts.DebugPort : 0);
                _ready.TrySetResult(core);
            }
            catch (Exception err)
            {
                _ready.TrySetException(new InvalidOperationException(
                    err.Message +
                    "；外壳资源目录 = " + (_opts.WebRoot ?? "（没找到 web/floating.html）"), err));
            }
        }

        /* ── `IShellHost`：页面做不了、只有窗口做得了的两件事 ─────────────────── */

        /// <summary>
        /// 这一次跑的是哪个配置目录（`--config` 给过就是那一个，否则是 `%APPDATA%\LookupApp`）。
        ///
        /// 页面拿它显示在「常规 → 数据」那一组里，以及「打开配置目录」按钮点下去时
        /// 交给 `RevealInPath` 打开 —— 所以这里给的是**真路径**，不是环境变量那种写法。
        /// 它由 `AppPaths` 决定（含"从 `%APPDATA%\查词` 迁移"那一步的结果）。
        /// </summary>
        public string ConfigDirPath()
        {
            return _opts.ConfigDir ?? "";
        }

        /// <summary>
        /// 把一段活丢回 UI 线程（要联网那几条路的**后台线程**做完 HTTP 之后用它回来）——
        /// 内核那一段与回包都只能在 UI 线程上做，见 `DeferredReply` 那段说明。
        /// 已经在 UI 线程上时**直接执行**（`InvokeRequired == false`）。
        /// </summary>
        public void PostToUiThread(Action action)
        {
            if (action == null) return;
            try
            {
                if (IsHandleCreated && InvokeRequired) BeginInvoke(action);
                else action();
            }
            catch (Exception err) { _bridge.Note("切回 UI 线程失败：" + err.Message); }
        }

        /// <summary>
        /// 弹一次"选词典"。**多选**（用户常常一次加好几本），过滤器只给 `.mdx`。
        ///
        /// ⚠️ 回来的路径**原样**交给内核（`engine.dictAdd`）—— 这一层不判
        ///    "这文件是不是词典"（那有内核的六种候选、id 哈希、卷关联一堆规则）。
        ///    用户取消就回 `[]`（不是报错：取消不是失败）。
        /// </summary>
        public string PickDictionaryFiles()
        {
            using (var dialog = new OpenFileDialog())
            {
                dialog.Title = "加词典";
                dialog.Filter = "MDict 词典 (*.mdx)|*.mdx|所有文件 (*.*)|*.*";
                dialog.Multiselect = true;
                dialog.CheckFileExists = true;
                if (dialog.ShowDialog(this) != DialogResult.OK) return "[]";
                var sb = new System.Text.StringBuilder();
                sb.Append('[');
                for (var i = 0; i < dialog.FileNames.Length; i++)
                {
                    if (i > 0) sb.Append(',');
                    sb.Append(Dispatch.Quote(dialog.FileNames[i]));
                }
                sb.Append(']');
                PickedFiles = sb.ToString();
                return PickedFiles;
            }
        }

        /*
         * 这里原来有 `SettingsChanged()`（设置改过 → 重新注册全局热键）。
         * 热键整条删掉之后它没有消费者了，于是**连接口一起删**：
         *   · `IShellHost.SettingsChanged`（`Dispatch.cs`）与它的 `shell.settingsChanged` 派发；
         *   · 管理窗 / 托盘菜单那两处转发；
         *   · DLL 检查里那两条"派发到壳上"的断言（/Runner.cs`）。
         * 留一个空方法在那儿比删掉更坏：读取代码的人会以为"设置变了还有谁需要知道"。
         */

        /// <summary>
        /// 把一段文字放进系统剪贴板（正文那排浮层的「复制」）。
        ///
        /// 为什么这条要走壳、而不是页面自己 `navigator.clipboard`：词条正文住在
        /// **sandbox 的跨源 iframe** 里 —— 那儿既没有安全上下文、也没有用户手势许可，
        /// 那一套 API 直接不可用（参考实现也是走 `api.writeClipboard` 这条窄缝）。
        ///
        /// ⚠️ 与参考实现的 `WriteClipboard` 同一条规矩：**空串走 `Clear()`**
        ///    （WinForms 的 `SetText` 不接受空串，会抛）。
        ///    失败**不抛到页面上**（写剪贴板失败不该把一次"复制"变成异常），
        ///    但**要留下实测结果**（`ClipboardWrites` / `ClipboardWhy`）—— 免得
        ///    "点了复制没反应"变成一句没有现场可查的抱怨。
        /// </summary>
        public void WriteClipboard(string text)
        {
            ClipboardBytes = System.Text.Encoding.UTF8.GetByteCount(text ?? "");
            ClipboardText = text ?? "";
            try
            {
                if (string.IsNullOrEmpty(text)) Clipboard.Clear();
                else Clipboard.SetText(text);
                ClipboardWrites++;
                ClipboardWhy = "";
            }
            catch (Exception err)
            {
                ClipboardWhy = err.Message;
                _bridge.Note("写剪贴板失败：" + err.Message);
            }
        }

        /// <summary>
        /// 读系统剪贴板里的文字（参考实现的 `clipboard:read`，输入框右键菜单的「粘贴」用它）。
        ///
        /// ⚠️ 读不到**不是**错误：剪贴板里放着一张图、或者刚被别的程序占着，都回空串 ——
        ///    参考实现就是这个约定（参考实现/src/App.cs:2128`：`ContainsText() ? GetText() : ""`，
        ///    异常也只是记一行）。所以这一条**不抛**给页面。
        /// </summary>
        public string ReadClipboard()
        {
            ClipboardReads++;
            try
            {
                var text = Clipboard.ContainsText() ? Clipboard.GetText() : string.Empty;
                ClipboardReadWhy = "";
                return text;
            }
            catch (Exception err)
            {
                ClipboardReadWhy = err.Message;
                _bridge.Note("读剪贴板失败：" + err.Message);
                return string.Empty;
            }
        }

        /// <summary>
        /// 在资源管理器里定位一个文件（选中的是文件）/ 打开一个目录
        /// （参考实现的 `shell:openPath`，管理窗里「打开所在文件夹」用它）。
        ///
        /// 逐条照抄参考实现（参考实现/src/App.cs:2205`）：文件走 `/select,"路径"`，
        /// 目录直接打开；两边都不存在就**什么都不做**（不是错误）。
        /// </summary>
        public void RevealInPath(string path)
        {
            if (string.IsNullOrWhiteSpace(path)) return;
            RevealedPath = path;
            try
            {
                if (File.Exists(path))
                {
                    Process.Start(new ProcessStartInfo("explorer.exe", "/select,\"" + path + "\"")
                    {
                        UseShellExecute = true,
                    });
                    RevealCount++;
                }
                else if (Directory.Exists(path))
                {
                    Process.Start(new ProcessStartInfo("explorer.exe", "\"" + path + "\"")
                    {
                        UseShellExecute = true,
                    });
                    RevealCount++;
                }
                else
                {
                    RevealWhy = "这个路径不存在：" + path;
                }
            }
            catch (Exception err)
            {
                RevealWhy = err.Message;
                _bridge.Note("定位文件失败：" + err.Message);
            }
        }

        /// <summary>
        /// 宿主 → 页面推一条事件（参考实现那套桥的 `{kind:'evt', name, payload}`）。
        ///
        /// ⚠️ **页面还没起来时不报错地丢掉**：热键、词库变更这些事可能发生在 WebView2 就绪之前，
        ///    那时候没有 `CoreWebView2` 可发 —— 抛出去只会让"启动路径"多一种崩法。
        ///    参考实现同一条规矩（参考实现/src/Bridge/WebHost.cs` 的 `Post` 里 `if (!_ready) return`）。
        /// </summary>
        public void Emit(string name, string payloadJson)
        {
            CardsEmitted++;
            /*
             * 再推一份给**管理窗**：参考实现里 `dictionaries:changed` / `settings:changed` /
             * `speech:settings-changed` / `translate:settings-changed` 都是**两扇窗都发**的
             * （在管理窗里改了设置，悬浮窗要跟着变；反过来也一样）。
             * 在这里转发一次，"谁发起的"就不影响另一扇窗收不收得到。
             */
            var manager = Manager;
            if (manager != null) manager.Emit(name, payloadJson);
            var tray = Tray;
            if (tray != null) tray.Emit(name, payloadJson);

            try
            {
                var core = _web.CoreWebView2;
                if (core == null) { EventsDropped++; return; }
                core.PostWebMessageAsJson(ShellBridge.Event(name, payloadJson));
                LastEventName = name;
            }
            catch (Exception err)
            {
                EventsDropped++;
                _bridge.Note("推事件 " + name + " 失败：" + err.Message);
            }
        }

        /// <summary>
        /// **广播**一条事件给所有开着的窗口（参考实现的 `App.BroadcastDictionaries()` 那套约定）。
        ///
        /// 扇出**就靠上面那个 `Emit`** —— 它本来就会推给自己 + 管理窗 + 托盘菜单
        /// （见 `Emit` 里那段说明），所以这里只喊一嗓子。
        ///
        /// ⚠️ 用户 2026-09 报的 bug 正是"只发给调用方"：在**选项窗**里切了词典，
        ///    而"当前词典名"那行灰字长在**悬浮窗**上 —— `ManagerWindow.Emit` 只推自己那一页
        ///    （不转发），于是悬浮窗永远不知道，灰字停在旧名字上。两扇窗发起时都走这里就齐了。
        /// </summary>
        public void Broadcast(string name, string payloadJson)
        {
            Emit(name, payloadJson);
        }

        /// <summary>推给页面的事件条数（自检那道 检查 读它）</summary>
        internal int CardsEmitted;
        /// <summary>页面还没起来（或者发失败）而丢掉的事件条数</summary>
        internal int EventsDropped;
        /// <summary>最近推出去的事件名</summary>
        internal string LastEventName = "";

        /* ── 窗口几何与形状：`IShellHost` 那一组（实现在 `FloatingLayout` 里）──────── */

        public string LayoutInfo() { return _layout.LayoutInfo(); }
        public void SetLayout(string requestJson) { _layout.SetLayout(requestJson); }
        public void SetShape(string regionsJson, string theme, bool focused)
        {
            ShapeReports++;
            _layout.SetShape(regionsJson, theme, focused);
        }

        /// <summary>`boot:ready`：页面首帧出来了 —— 把停在屏幕外的窗口一次摆回真实位置（宿主与外壳层同一轮里一起挪）。</summary>
        public void BootReady() { _layout.RevealAfterBoot(); }
        public void DragPrepare() { _layout.DragPrepare(); }
        public void DragStart() { _layout.DragStart(); }
        public void DragMove(double? sentAtMs) { _layout.DragMove(sentAtMs); }
        public void DragEnd() { _layout.DragEnd(); }
        public void ExpandFromEdge() { _layout.ExpandFromEdge(); }
        public void CollapseToEdge() { _layout.CollapseToEdge(); }
        public void ResetPill() { _layout.ResetToDefault(); }
        public void AbsorbToEdge(string edgeWire) { _layout.AbsorbToEdge(edgeWire); }
        public void HideWindow() { _layout.HideWindow(); }
        public string DescribeWindow(string role)
        {
            if (role == "manager")
            {
                var manager = Manager;
                return (manager == null) ? "{\"present\":false}" : manager.DescribeSelf();
            }
            if (role == "tray")
            {
                var tray = Tray;
                return (tray == null) ? "{\"present\":false}" : tray.DescribeSelf();
            }
            return _layout.DescribeWindow();
        }
        public string DragStats() { return _layout.DragStats(); }
        public void PlacePill(double x, double y) { _layout.PlacePill(x, y); }

        /// <summary>页面上报过几次形状（断言"形状真的来了"用它，而不是"窗口看着像"）</summary>
        internal int ShapeReports;

        /// <summary>
        /// `window:requestClose` —— 关窗口时怎么办，**问设置**（`closeBehavior`）。
        ///
        /// 逐条对照参考实现的 `App.RequestClose`：`quit` 直接退、`tray` 收起来、
        /// 其余（`ask`）推一条 `close:requested` 让页面弹那个"记住我的选择"的对话框。
        /// </summary>
        public void RequestClose()
        {
            var behavior = CloseBehavior();
            CloseRequests++;
            if (behavior == "quit") { Quit(); return; }
            if (behavior == "tray") { _layout.HideWindow(); return; }
            Emit("close:requested", "null");
        }

        /// <summary>`window:resolveClose`：用户在对话框上选了什么（`quit` / `tray` / `cancel`）</summary>
        public void ResolveClose(string choice, bool remember)
        {
            if (choice == "cancel") return;
            if (remember) SetCloseBehavior(choice == "quit" ? "quit" : "tray");
            if (choice == "quit") Quit();
            else _layout.HideWindow();
        }

        /// <summary>`debug:importDictionary`：按路径直接加一本（不弹对话框；钩子没开不能用）</summary>
        public void ImportDictionary(string path)
        {
            if (string.IsNullOrEmpty(path)) return;
            var paths = "[" + Dispatch.Quote(path) + "]";
            string ignored;
            try { Dsh.EngineDictAdd(_engine, paths, out ignored); }
            catch (Exception err) { _bridge.Note("导入失败：" + err.Message); }
            Emit("dictionaries:changed", null);
        }

        /// <summary>关窗口这条路上被问过几次（自检那道 检查 读它）</summary>
        internal int CloseRequests;

        /// <summary>
        /// 管理窗（懒建：`--no-show` 那一种情况不建它，自检不需要第二扇窗）。
        /// 由 `Program` 接上 —— 这一层只"转述"，不自己造窗口。
        /// </summary>
        internal ManagerWindow Manager;

        /* ── 管理窗那一组：悬浮窗只负责"请打开它"，其余（关/最小化/拖/底色）
         *    都是**管理窗自己**的事，在这里是空操作 ─────────────────────────── */

        public void OpenManager(string tab)
        {
            var manager = Manager;
            if (manager == null) throw new InvalidOperationException("这一版没有词库管理窗");
            ManagerOpens++;
            manager.OpenManager(tab);
        }

        /// <summary>`manager:open` 被叫过几次（自检那道 检查 读它）</summary>
        internal int ManagerOpens;

        public void ManagerClose() { }
        public void ManagerMinimize() { }
        public string ManagerConsumeInitialTab() { return ""; }
        public void ManagerBeginDrag() { }
        public void ManagerSurface(string fill, string border, string theme, bool focused) { }

        /* ── 托盘菜单那一组（参考实现的第三扇窗）──────────────────────────────── */

        /// <summary>托盘菜单窗（懒建：`--no-show` 与自检那一种情况不建它）</summary>
        internal TrayMenuWindow Tray;
        /// <summary>托盘图标</summary>
        internal TrayIcon TrayIcon;
        /// <summary>托盘菜单被弹过几次（自检那道 检查 读它）</summary>
        internal int TrayShows;
        /// <summary>开机自启动开关被写过几次（自检那道 检查 读它；**真的写注册表**）</summary>
        internal int LoginToggles;

        public void TrayMenuShow()
        {
            var tray = Tray;
            if (tray == null) throw new InvalidOperationException("这一版没有托盘菜单");
            TrayShows++;
            tray.ShowMenu();
        }

        public void TrayMenuHide() { if (Tray != null) Tray.HideMenu(); }

        public void TrayMenuPlace(double width, double height, string fill, string border, string theme)
        {
            if (Tray != null) Tray.Place(width, height, fill, border, theme);
        }

        public string TrayState()
        {
            var visible = Visible && WindowState != FormWindowState.Minimized;
            return "{\"floatingVisible\":" + (visible ? "true" : "false") +
                   ",\"loginAtStartup\":" + (Startup.IsLoginAtStartup() ? "true" : "false") + "}";
        }

        public bool TraySetLogin(bool enabled)
        {
            LoginToggles++;
            return Startup.SetLoginAtStartup(enabled);
        }

        public void TrayToggleFloating()
        {
            if (_layout != null && Visible) _layout.HideWindow();
            else if (_layout != null) _layout.ShowWindow(true);
        }

        /// <summary>
        /// **呼出**胶囊（托盘图标左键双击走的那个动作，用户 2026-09 定的三条）：
        ///
        ///   ① **吸附在屏幕边缘**（吸进去只剩一条窄条）→ **取消吸附**，把界面弹出来；
        ///   ② **收入托盘**（窗口收起来了）→ **显示出来**（焦点一并给输入框，与托盘菜单那项同）；
        ///   ③ **已经显示着**（也没吸附）→ **什么都不做**。
        ///
        /// ⚠️ 与上面那个 `TrayToggleFloating`（托盘菜单第一项「显示 / 隐藏悬浮窗」用的）
        ///    是**两个动作**：那个是开关（看得见就收起来），这个只管"叫出来"、**永远不会把窗口收走**
        ///    （原话："如果已经显示，则双击无动作"）。别把两个合成一个。
        /// </summary>
        public void TraySummonFloating()
        {
            if (_layout == null) return;
            if (!Visible)
            {
                /* ② 收入托盘 → 显示出来 */
                _layout.ShowWindow(true);
                return;
            }
            if (_layout.IsAbsorbed)
            {
                /* ① 吸附着 → 取消吸附（`ExpandFromEdge` 自己会把"没吸附"那一种情况空转掉） */
                _layout.ExpandFromEdge();
            }
            /* ③ 已经显示着、也没吸附 → 什么都不做 */
        }

        public void TrayResetPill() { if (_layout != null) _layout.ResetToDefault(); }

        /// <summary>
        /// `debug:focus`：把这个窗口**明确**拿到前台。
        /// 页面里"选中文字那排浮层"有 `if (!document.hasFocus()) return` 这道条件，
        /// 所以自动化要验那条路时得先把焦点给它（Windows 不一定听 —— 抢前台是有规矩的，
        /// 所以这个钩子只负责"尽力"，验的人自己确认 `document.hasFocus()`）。
        /// </summary>
        public void FocusSelf()
        {
            try
            {
                if (!Visible) Show();
                Activate();
                Native.SetForegroundWindow(Handle);
                Focus(); // 焦点落到 WebView2 上（页面才有 hasFocus）
            }
            catch (Exception err) { _bridge.Note("抢焦点失败：" + err.Message); }
        }

        public void TrayQuit()
        {
            try { Application.Exit(); }
            catch (Exception err) { _bridge.Note("退出失败：" + err.Message); }
        }

        private string CloseBehavior()
        {
            try
            {
                string settings;
                Dsh.EngineSettingsGet(_engine, out settings);
                return VirtualHost.JsonString(settings, "closeBehavior") ?? "ask";
            }
            catch (Exception) { return "ask"; }
        }

        private void SetCloseBehavior(string wire)
        {
            try
            {
                string ignored;
                Dsh.EngineSettingsSet(_engine, "{\"closeBehavior\":\"" + wire + "\"}", out ignored);
            }
            catch (Exception err) { _bridge.Note("写 closeBehavior 失败：" + err.Message); }
        }

        private void Quit()
        {
            try { Application.Exit(); }
            catch (Exception err) { _bridge.Note("退出失败：" + err.Message); }
        }

        /* ── 设置界面那条路的实测结果（自检那道 检查 读它们）──────────────────────────── */

        /// <summary>`shell.pickDictionaries` 最近一次给出的路径数组 JSON</summary>
        internal string PickedFiles;
        /// <summary>真的写成功过几次剪贴板</summary>
        internal int ClipboardWrites;
        /// <summary>最近一次写进去的字节数（UTF-8）</summary>
        internal int ClipboardBytes;
        /// <summary>最近一次写进去的文字（自检要断言"写的就是选中的那段"）</summary>
        internal string ClipboardText = "";
        /// <summary>最近一次写失败的原因（成功就空串）</summary>
        internal string ClipboardWhy = "";
        /// <summary>`clipboard:read` 被问过几次（参考实现界面搬过来之后新增的那条路）</summary>
        internal int ClipboardReads;
        /// <summary>最近一次读剪贴板失败的原因（成功就空串）</summary>
        internal string ClipboardReadWhy = "";
        /// <summary>`shell:openPath` 真的叫起过几次资源管理器</summary>
        internal int RevealCount;
        /// <summary>最近一次要定位的路径（断言"定位的是那一本"用）</summary>
        internal string RevealedPath;
        /// <summary>最近一次没能定位的原因（成功就空串）</summary>
        internal string RevealWhy = "";

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                _web.Dispose();
                /*
                 * ⚠️ 托盘图标不是本窗体的子控件，窗体 Dispose 不会顺带收它 ——
                 *    必须在这里显式释放，否则退出后托盘留"幽灵图标"、TrayIcon 里
                 *    的 DestroyIcon（防 GDI 泄漏）也永远走不到。历史上没人调它，2026-09-24 补。
                 */
                TrayIcon?.Dispose();
            }
            base.Dispose(disposing);
        }
    }
}
