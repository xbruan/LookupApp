namespace Lookup.App
{
    using System;
    using System.Drawing;
    using System.Threading.Tasks;
    using System.Windows.Forms;
    using Lookup.Host;
    using Microsoft.Web.WebView2.Core;
    using Microsoft.Web.WebView2.WinForms;

    /// <summary>
    /// **托盘菜单窗** —— 参考实现的 `src/Windows/TrayMenuWindow.cs` 搬过来的那一套。
    ///
    /// 它是**弹出菜单**不是普通窗口，三条照抄参考实现的规矩：① **尺寸由页面量**
    /// （`traymenu:measured`）后再摆，摆之前藏着（否则会先在屏幕左上角闪一下）；
    /// ② **底边贴光标上方**（像系统托盘菜单）：`y = 光标.Y - 高 - 6`，四边夹在工作区里；
    /// ③ **刻意不抢前台** —— 一抢，Windows 托盘那个"隐藏图标"浮出面板就会失活缩回去，
    ///    代价是收不到 `Deactivate`，"点到别处就关"因此靠全局鼠标钩子（<see cref="OutsideClickHook"/>）。
    /// 另两条：**刚关掉 400ms 内不许再弹**（右键会先让窗口失焦隐藏、随后才触发右键）；
    /// 页面还没装好时先记住"要发 `traymenu:open`"，装好了再补发（不然第一次弹出来是空的）。
    /// </summary>
    internal sealed class TrayMenuWindow : Form, IShellHost
    {
        internal const string StartUrl =
            "https://" + VirtualHost.ShellDomain + "/tray-menu.html";

        /// <summary>菜单底边与光标之间的空隙</summary>
        private const int Gap = 6;
        /// <summary>刚关掉之后多久内不许再弹（毫秒）</summary>
        private const int ReopenGuardMs = 400;
        private const int CornerRadius = 12;
        private const int RegionInset = 2;

        private readonly AppOptions _opts;
        private readonly IntPtr _engine;
        private readonly WebView2 _web = new WebView2();
        private readonly WebViewBridge _bridge;
        private readonly OutsideClickHook _outsideClick = new OutsideClickHook();

        private bool _started;
        private bool _pageLoaded;
        private bool _openPending;
        private bool _placing;
        private DateTime _hiddenAt = DateTime.MinValue;
        private Point _anchor;
        private double _scale = 1.0;
        private int _measuredWidth;
        private int _measuredHeight;
        private string _fill = "";
        private string _border = "";
        private bool _dark;

        /// <summary>外壳层（抗锯齿圆角 + 投影）—— 与管理窗、悬浮窗用的是同一个 <see cref="ChromeWindow"/></summary>
        private readonly ChromeWindow _chrome = new ChromeWindow();

        internal TrayMenuWindow(IntPtr engine, AppOptions opts)
        {
            _opts = opts;
            _engine = engine;
            _bridge = new WebViewBridge(engine, opts.WebRoot, this);

            FormBorderStyle = FormBorderStyle.None;
            StartPosition = FormStartPosition.Manual;
            ShowInTaskbar = false;
            TopMost = true;
            MinimizeBox = false;
            MaximizeBox = false;
            AutoScaleMode = AutoScaleMode.None;
            Text = "查词菜单";
            BackColor = Color.White;

            _web.Dock = DockStyle.Fill;
            _web.DefaultBackgroundColor = Color.White;
            Controls.Add(_web);

            _outsideClick.IsInside = IsPointInside;
            _outsideClick.OutsideClicked = OnOutsideClick;

            Deactivate += (s, e) =>
            {
                /*
                 * 正常情况下收不到这一条（窗口不抢前台）—— 万一系统还是给了：摆窗口那一刻的
                 * z 序/激活变化**不算**"点到别处"（参考实现的 `_placing` 同）。
                 *
                 * ⚠️ 这里必须叫 <see cref="HideMenu"/>，**不许写裸 `Hide()`**：参考实现那份的方法
                 *    原名就叫 `Hide()`，搬来改名成 `HideMenu()` 之后，裸 `Hide()` 会**悄悄**变成
                 *    WinForms 的 `Form.Hide()` —— 只收宿主窗口、**外壳层（投影/圆角）留在屏幕上**；
                 *    两个 `Hide` 都合法，编译器一声不吭。
                 */
                if (!_placing) HideMenu();
            };
        }

        internal WebViewBridge Bridge { get { return _bridge; } }
        internal bool IsOpen { get { return Visible; } }

        /* ── 显示与摆放 ─────────────────────────────────────────────────────── */

        /// <summary>`debug:showTrayMenu` / 托盘图标点一下 → 弹菜单</summary>
        internal void ShowMenu()
        {
            if ((DateTime.UtcNow - _hiddenAt).TotalMilliseconds < ReopenGuardMs) return;

            _anchor = Native.CursorPosition();

            if (!_started)
            {
                _started = true;
                var handle = Handle; // 先建句柄（别在默认位置闪一下）
                Show();
                Hide();
                StartWebViewAsync();
            }

            // 摆窗口的过程中会有各种 z 序/激活变化，别让 Deactivate 把菜单当场收掉
            _placing = true;
            BeginInvoke((MethodInvoker)delegate { _placing = false; });

            if (_pageLoaded) Emit("traymenu:open", "null");
            else _openPending = true;
        }

        /// <summary>
        /// `traymenu:measured`：页面量好内容尺寸后回调，主进程据此摆窗口再显示。
        ///
        /// ⚠️ 页面报的是 **CSS 像素（DIP）**、窗口要的是**物理像素**：直接拿数字当物理像素，
        ///    125% 缩放的屏幕上菜单只剩内容的 80% 高，**最后一项（"退出"）会被裁掉**。
        /// </summary>
        internal void Place(double width, double height, string fill, string border, string theme)
        {
            _scale = Native.ScaleAt(_anchor);
            if (_scale <= 0) _scale = 1.0;

            var w = Math.Max(80, (int)Math.Round(width * _scale));
            var h = Math.Max(40, (int)Math.Round(height * _scale));
            _measuredWidth = w;
            _measuredHeight = h;
            if (!string.IsNullOrWhiteSpace(fill)) _fill = fill;
            if (!string.IsNullOrWhiteSpace(border)) _border = border;
            _dark = theme == "dark";

            var work = Native.WorkAreaAt(_anchor);
            // 像系统托盘菜单那样：菜单**底边贴在光标上方**
            var x = Clamp(_anchor.X, work.X + 4, work.X + work.Width - w - 4);
            var y = Clamp(_anchor.Y - h - Gap, work.Y + 4, work.Y + work.Height - h - 4);

            Native.SetWindowPos(Handle, IntPtr.Zero, x, y, w, h,
                                Native.SWP_NOZORDER | Native.SWP_NOACTIVATE);
            if (!Visible)
            {
                Show();
                Native.SetWindowPos(Handle, IntPtr.Zero, x, y, w, h,
                                    Native.SWP_NOZORDER | Native.SWP_NOACTIVATE | Native.SWP_SHOWWINDOW);
            }
            TopMost = true;
            Bounds = new Rectangle(x, y, w, h);

            ApplyRegion();
            UpdateChrome();
            _outsideClick.Install();
        }

        /// <summary>
        /// 把菜单**两层一起**收掉：外壳层（投影 / 圆角）+ 宿主窗口，并卸掉那个全局鼠标钩子。
        ///
        /// ⚠️ **收菜单只有这一条正路** —— 三处调用（"点到别处"、页面叫收、失活）都必须走它。
        ///    单独写 `Hide()` 只会收掉宿主窗口、把外壳层留在屏幕上（见 构造函数里 `Deactivate`
        ///    那段注释：参考实现 → 0.2.0 搬家时就是这么漏的）。
        /// </summary>
        internal void HideMenu()
        {
            // 与参考实现的 `Hide()` 同一条守卫：已经收着就什么都不做
            // （否则每叫一次都会把"刚关掉 400ms 内不许再弹"的那个时刻往后推）
            if (!Visible) return;
            _hiddenAt = DateTime.UtcNow;
            _outsideClick.Uninstall();
            _chrome.Hide();
            Hide();
        }

        /// <summary>整窗一块卡片（与"被裁成圆角的窗口 Region"配套：Region 管命中、这一层管观感）</summary>
        private void UpdateChrome()
        {
            if (!IsHandleCreated || !Visible || _measuredWidth <= 0 || _measuredHeight <= 0)
            {
                _chrome.Hide();
                return;
            }
            _chrome.UpdateWindowCard(Handle, Bounds, _measuredWidth, _measuredHeight,
                                     (float)(CornerRadius * _scale),
                                     CssColor.Parse(_fill, Color.FromArgb(0xF7, 0xF8, 0xFC)),
                                     CssColor.Parse(_border, Color.FromArgb(0xE4, 0xE6, 0xEE)),
                                     _dark, false);
            _chrome.RefreshZOrder(Handle);
        }

        /// <summary>`debug:window("tray")` 的现场</summary>
        internal string DescribeSelf()
        {
            var chromeShape = Native.DescribeShape(_chrome.Handle, new System.Collections.Generic.List<Point>());
            var sb = new System.Text.StringBuilder();
            sb.Append("{\"present\":true,\"role\":\"tray\",\"visible\":")
              .Append(Visible ? "true" : "false");
            /*
             * 托盘菜单**必须是 false** —— 它一抢前台，Windows 托盘的"隐藏图标"浮出面板
             * 就会失活缩回去（别的软件的托盘菜单都不会这样）。
             */
            sb.Append(",\"foregroundIsSelf\":")
              .Append(Native.GetForegroundWindow() == Handle ? "true" : "false");
            sb.Append(",\"bounds\":{\"x\":").Append(Left).Append(",\"y\":").Append(Top)
              .Append(",\"width\":").Append(Width).Append(",\"height\":").Append(Height).Append('}');
            sb.Append(",\"regionBox\":null,\"probes\":{}");
            sb.Append(",\"chrome\":{\"present\":true,\"visible\":")
              .Append(Native.IsWindowVisible(_chrome.Handle) ? "true" : "false");
            sb.Append(",\"bounds\":{\"x\":").Append(chromeShape.Bounds.X)
              .Append(",\"y\":").Append(chromeShape.Bounds.Y)
              .Append(",\"width\":").Append(chromeShape.Bounds.Width)
              .Append(",\"height\":").Append(chromeShape.Bounds.Height).Append("}}");
            sb.Append(",\"applied\":null}");
            return sb.ToString();
        }

        private static int Clamp(int value, int min, int max)
        {
            if (max < min) return min;
            return Math.Min(Math.Max(value, min), max);
        }

        /// <summary>窗口 Region：整块卡片往里缩 2px 的圆角矩形（好让外壳的实心填充盖住硬边）</summary>
        private void ApplyRegion()
        {
            if (!IsHandleCreated || _measuredWidth <= 0 || _measuredHeight <= 0) return;
            var radius = Math.Max(0, (int)Math.Round(CornerRadius * _scale) - RegionInset) * 2;
            var region = Native.CreateRoundRectRgn(RegionInset, RegionInset,
                                                   _measuredWidth - RegionInset + 1,
                                                   _measuredHeight - RegionInset + 1,
                                                   radius, radius);
            if (region == IntPtr.Zero) return;
            if (Native.SetWindowRgn(Handle, region, true) != 0) return; // 成功则归系统
            Native.DeleteObject(region);
        }

        private bool IsPointInside(int x, int y)
        {
            if (!Visible) return false;
            return Bounds.Contains(x, y);
        }

        private void OnOutsideClick()
        {
            try { if (Visible) HideMenu(); }
            catch (Exception) { /* 钩子里不许抛 */ }
        }

        private async void StartWebViewAsync()
        {
            try
            {
                var core = await WebViewSetup.StartAsync(_web, _opts.ConfigDir, StartUrl,
                                                         _bridge.Attach, _opts.DebugPort);
                _pageLoaded = true;
                if (_openPending)
                {
                    _openPending = false;
                    Emit("traymenu:open", "null");
                }
            }
            catch (Exception err)
            {
                _bridge.Note("托盘菜单页面打不开：" + err.Message);
            }
        }

        /* ── `IShellHost`：这个窗口只做菜单自己那几条 ─────────────────────────── */

        public void Emit(string name, string payloadJson)
        {
            try
            {
                var core = _web.CoreWebView2;
                if (core == null) return;
                core.PostWebMessageAsJson(ShellBridge.Event(name, payloadJson));
            }
            catch (Exception err) { _bridge.Note("托盘菜单推事件失败：" + err.Message); }
        }

        /// <summary>
        /// **广播**一条事件：交给**悬浮窗**那一份去扇出（它推给自己 + 管理窗 + 托盘菜单）。
        /// 托盘菜单这一页每次弹出都会重新问一遍状态，所以它本来就不依赖这类事件 ——
        /// 但既然 `IShellHost` 有这条，就照同一套约定实现，别留一个"只有这里没接"的坑。
        /// </summary>
        public void Broadcast(string name, string payloadJson)
        {
            var host = ManagerWindow.AppHost;
            if (host != null && !ReferenceEquals(host, this))
            {
                host.Broadcast(name, payloadJson);
                return;
            }
            Emit(name, payloadJson);
        }

        public void TrayMenuHide() { HideMenu(); }
        /** `debug:showTrayMenu` —— 从别的窗口叫它也一样弹（锚点是光标）*/
        public void TrayMenuShow() { ShowMenu(); }
        public string TrayState() { return ManagerWindow.AppHost.TrayState(); }
        public bool TraySetLogin(bool enabled) { return ManagerWindow.AppHost.TraySetLogin(enabled); }
        public void TrayToggleFloating() { ManagerWindow.AppHost.TrayToggleFloating(); }
        public void TrayResetPill() { ManagerWindow.AppHost.TrayResetPill(); }
        public void TrayQuit() { ManagerWindow.AppHost.TrayQuit(); }
        public void FocusSelf() { ManagerWindow.AppHost.FocusSelf(); }
        public void TrayMenuPlace(double w, double h, string fill, string border, string theme)
        {
            Place(w, h, fill, border, theme);
        }
        /// <summary>`shape:set` 到托盘菜单这一种情况：参考实现里它也只用来调一下 Region</summary>
        public void SetShape(string regionsJson, string theme, bool focused) { ApplyRegion(); }
        /// <summary>`boot:ready` 是**悬浮窗**那一条；托盘菜单的尺寸由页面 `traymenu:measured` 决定，不走这条。</summary>
        public void BootReady() { }

        /* 窗口级（悬浮窗那套）：托盘菜单不参与 */
        public string LayoutInfo() { return "{}"; }
        public void SetLayout(string requestJson) { }
        public void DragPrepare() { }
        public void DragStart() { }
        public void DragMove(double? sentAtMs) { }
        public void DragEnd() { }
        public void ExpandFromEdge() { }
        public void CollapseToEdge() { }
        public void ResetPill() { }
        public void AbsorbToEdge(string edgeWire) { }
        public void HideWindow() { HideMenu(); }
        public string DescribeWindow(string role)
        {
            if (role == "tray") return DescribeSelf();
            return ManagerWindow.AppHost.DescribeWindow(role);
        }
        public string DragStats() { return "{\"frames\":0,\"median\":0,\"p95\":0,\"max\":0,\"samples\":[]}"; }
        public void PlacePill(double x, double y) { }

        /* app 级：转发给应用那一侧（与 `ManagerWindow` 同一个套路）*/
        public string PickDictionaryFiles() { return ManagerWindow.AppHost.PickDictionaryFiles(); }
        public string ConfigDirPath() { return ManagerWindow.AppHost.ConfigDirPath(); }
        /// <summary>要联网那几条路的后台线程靠它切回 UI 线程（见 `DeferredReply` 那段说明）</summary>
        public void PostToUiThread(Action action) { ManagerWindow.AppHost.PostToUiThread(action); }
        public void WriteClipboard(string text) { ManagerWindow.AppHost.WriteClipboard(text); }
        public string ReadClipboard() { return ManagerWindow.AppHost.ReadClipboard(); }
        public void RevealInPath(string path) { ManagerWindow.AppHost.RevealInPath(path); }
        public void RequestClose() { ManagerWindow.AppHost.RequestClose(); }
        public void ResolveClose(string choice, bool remember) { ManagerWindow.AppHost.ResolveClose(choice, remember); }
        public void ImportDictionary(string path) { ManagerWindow.AppHost.ImportDictionary(path); }
        public void OpenManager(string tab) { ManagerWindow.AppHost.OpenManager(tab); }
        public void ManagerClose() { ManagerWindow.AppHost.ManagerClose(); }
        public void ManagerMinimize() { ManagerWindow.AppHost.ManagerMinimize(); }
        public string ManagerConsumeInitialTab() { return ManagerWindow.AppHost.ManagerConsumeInitialTab(); }
        public void ManagerBeginDrag() { ManagerWindow.AppHost.ManagerBeginDrag(); }
        public void ManagerSurface(string fill, string border, string theme, bool focused)
        {
            ManagerWindow.AppHost.ManagerSurface(fill, border, theme, focused);
        }
    }
}
