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
    /// **词库管理窗** —— 行为逐条对照参考实现的 `src/Windows/ManagerWindow.cs`，不重新设计：
    /// 窗口矩形就等于卡片本身（系统投影 + 圆角 12、往里缩 2 的 Region），边缘 6px 可拖着改大小，
    /// 首次打开摆到工作区正中、之后不动；关窗只是 Hide。
    /// </summary>
    internal sealed class ManagerWindow : Form, IShellHost
    {
        internal const string StartUrl =
            "https://" + VirtualHost.ShellDomain + "/manager.html";

        private const int CornerRadius = 12;
        private const int RegionInset = 2;
        private const int ResizeMargin = 6;

        private const int WM_NCHITTEST = 0x0084;
        private const int HTCLIENT = 1;
        private const int HTLEFT = 10;
        private const int HTRIGHT = 11;
        private const int HTTOP = 12;
        private const int HTTOPLEFT = 13;
        private const int HTTOPRIGHT = 14;
        private const int HTBOTTOM = 15;
        private const int HTBOTTOMLEFT = 16;
        private const int HTBOTTOMRIGHT = 17;

        private readonly AppOptions _opts;
        private readonly IntPtr _engine;
        private readonly WebView2 _web = new WebView2();
        private readonly WebViewBridge _bridge;
        private readonly TaskCompletionSource<CoreWebView2> _ready =
            new TaskCompletionSource<CoreWebView2>();

        private bool _started;
        /// <summary>"这次打开要翻到哪一页" —— 取一次就清（它只服务这一次打开，不是持久偏好）</summary>
        private string _requestedTab;
        private string _fill = "";
        private string _border = "";
        private bool _dark;
        private bool _focused;

        /// <summary>外壳层（抗锯齿圆角 + 投影），与悬浮窗共用 <see cref="ChromeWindow"/>；本窗每帧只报一张卡（`UpdateWindowCard`）</summary>
        private readonly ChromeWindow _chrome = new ChromeWindow();

        internal ManagerWindow(IntPtr engine, AppOptions opts)
        {
            _opts = opts;
            _engine = engine;
            _bridge = new WebViewBridge(engine, opts.WebRoot, this);

            FormBorderStyle = FormBorderStyle.None;
            StartPosition = FormStartPosition.Manual;
            ShowInTaskbar = true; // 管理窗是**普通窗口**：任务栏上该有它
            TopMost = false;      // 跟着别的窗口一起被盖住才正常
            MinimizeBox = true;
            MaximizeBox = true;
            AutoScaleMode = AutoScaleMode.None;
            /* 任务栏 / Alt-Tab 的图标与参考实现同一个来源（`assets/app.ico` 多尺寸版）；
             * 取不到时 `LoadAppIcon()` 自己退系统图标 —— 不在这儿判空。 */
            Icon = TrayIcon.LoadAppIcon();
            /* ⚠️ 尺寸常量按 **DIP** 写（宽度与悬浮窗的胶囊一致），而 WinForms 的 `ClientSize` /
             * `MinimumSize` 是**物理像素** —— 必须乘上主显示器的缩放，否则 125% 的屏上会小一圈。 */
            var scale = Native.ScaleAt(new Point(
                Screen.PrimaryScreen.Bounds.X + Screen.PrimaryScreen.Bounds.Width / 2,
                Screen.PrimaryScreen.Bounds.Y + Screen.PrimaryScreen.Bounds.Height / 2));
            if (scale <= 0) scale = 1.0;
            MinimumSize = new Size(
                (int)Math.Round(400 * scale),
                (int)Math.Round(420 * scale));
            ClientSize = new Size(
                (int)Math.Round(FloatingLayout.PillWidth * scale),
                (int)Math.Round(640 * scale));
            Text = "词库管理";
            BackColor = Color.White;

            _web.Dock = DockStyle.Fill;
            _web.DefaultBackgroundColor = Color.White;
            Controls.Add(_web);
        }

        internal WebViewBridge Bridge { get { return _bridge; } }
        internal Task<CoreWebView2> Ready { get { return _ready.Task; } }
        internal bool IsOpen { get { return Visible; } }

        /// <summary>窗口句柄建出来之后立刻按"一块圆角卡片"裁一次（参考实现同）</summary>
        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);
            ApplyRegion();
        }

        protected override void OnResize(EventArgs e)
        {
            base.OnResize(e);
            ApplyRegion();
            RefreshChrome();
        }

        /// <summary>
        /// ★ **窗口一挪，外壳（阴影 / 圆角那一层）必须跟着挪**：拖动走的是系统那条
        /// `WM_NCLBUTTONDOWN` + `HTCAPTION`，位置一直在变但**尺寸不变** → 只挂 `OnResize` 的话
        /// 一次都不会触发，阴影会留在原地。
        /// </summary>
        protected override void OnLocationChanged(EventArgs e)
        {
            base.OnLocationChanged(e);
            RefreshChrome();
        }

        protected override void OnShown(EventArgs e)
        {
            base.OnShown(e);
            RefreshChrome();
            // 外壳要压在承载网页的窗口**下面**：显示之后重排一次 z 序
            _chrome.RefreshZOrder(Handle);
        }

        protected override void OnActivated(EventArgs e)
        {
            base.OnActivated(e);
            _focused = true;
            RefreshChrome();
        }

        protected override void OnDeactivate(EventArgs e)
        {
            base.OnDeactivate(e);
            _focused = false;
            RefreshChrome();
        }

        /// <summary>无边框窗口默认没法用鼠标拖边缘改大小，这里补上命中测试（照参考实现 `ShellForm.WndProc` 的 `Resizable`）</summary>
        protected override void WndProc(ref Message m)
        {
            if (m.Msg == WM_NCHITTEST)
            {
                base.WndProc(ref m);
                if ((int)m.Result == HTCLIENT)
                {
                    var raw = unchecked((long)m.LParam);
                    var screenPoint = new Point(unchecked((short)raw), unchecked((short)(raw >> 16)));
                    var client = PointToClient(screenPoint);

                    var left = client.X <= ResizeMargin;
                    var right = client.X >= ClientSize.Width - ResizeMargin;
                    var top = client.Y <= ResizeMargin;
                    var bottom = client.Y >= ClientSize.Height - ResizeMargin;

                    if (left && top) m.Result = (IntPtr)HTTOPLEFT;
                    else if (right && top) m.Result = (IntPtr)HTTOPRIGHT;
                    else if (left && bottom) m.Result = (IntPtr)HTBOTTOMLEFT;
                    else if (right && bottom) m.Result = (IntPtr)HTBOTTOMRIGHT;
                    else if (left) m.Result = (IntPtr)HTLEFT;
                    else if (right) m.Result = (IntPtr)HTRIGHT;
                    else if (top) m.Result = (IntPtr)HTTOP;
                    else if (bottom) m.Result = (IntPtr)HTBOTTOM;
                }
                return;
            }
            base.WndProc(ref m);
        }

        /// <summary>把窗口裁成一块圆角卡片 —— 往里缩 2px 是为了让 `SetWindowRgn` 那道硬边落在卡片填充内部</summary>
        private void ApplyRegion()
        {
            if (!IsHandleCreated || ClientSize.Width <= 0 || ClientSize.Height <= 0) return;
            var radius = Math.Max(0, CornerRadius - RegionInset) * 2;
            var rgn = Native.CreateRoundRectRgn(RegionInset, RegionInset,
                                                ClientSize.Width - RegionInset,
                                                ClientSize.Height - RegionInset,
                                                radius, radius);
            if (rgn == IntPtr.Zero) return;
            if (Native.SetWindowRgn(Handle, rgn, true) != 0) rgn = IntPtr.Zero; // 成功则区域归系统
            if (rgn != IntPtr.Zero) Native.DeleteObject(rgn);
        }

        /// <summary>`manager:initialTab`：渲染进程开机时来拉一次"这次要翻到哪页"（取过就清）</summary>
        public string ManagerConsumeTab()
        {
            var tab = _requestedTab;
            _requestedTab = null;
            return tab ?? "";
        }

        /// <summary>
        /// 打开（或唤回）管理窗，可以指定"翻到哪一页"：窗口**已经开着**就直接推 `options:tab`；
        /// 窗口是**这次才建的**，此刻页面还在加载、推过去会丢 —— 只能把页码记下来等渲染进程
        /// 开机后来拉一次（`manager:initialTab`）。两条路必须一起用。
        /// </summary>
        public void OpenManager(string tab)
        {
            var wantsTab = !string.IsNullOrEmpty(tab);
            if (wantsTab) _requestedTab = tab;

            if (!_started)
            {
                _started = true;
                // 先建句柄（不显示），把位置摆好再 Show —— 否则会先在屏幕左上角闪一下
                var handle = Handle;
                CenterOnScreen();
                Show();
                StartWebViewAsync();
            }
            else
            {
                if (WindowState == FormWindowState.Minimized)
                {
                    WindowState = FormWindowState.Normal;
                    Show();
                }
                else
                {
                    Show();
                }
                // 页面已经在跑：直接推一条（渲染进程的订阅在它第一个 await 之前就注册好了）
                if (wantsTab) Emit("options:tab", "{\"tab\":" + Dispatch.Quote(tab) + "}");
            }

            try
            {
                Activate();
                Native.SetForegroundWindow(Handle);
            }
            catch (Exception err) { _bridge.Note("管理窗抢前台失败：" + err.Message); }
        }

        private async void StartWebViewAsync()
        {
            try
            {
                var core = await WebViewSetup.StartAsync(_web, _opts.ConfigDir, StartUrl,
                                                         _bridge.Attach, _opts.DebugPort);
                /* ★ **窗口标题跟着页面标题走**：WebView2 不会把页面标题写给 WinForms 窗体，
                 * 壳只照抄这条通知、**不拼字符串**（页面那边才是"页签叫什么"的唯一来源）。
                 * ⚠️ 只在**非空**时替换：导航过程中它会先报一个空标题，照抄会让标题闪一下空白。 */
                core.DocumentTitleChanged += delegate
                {
                    try
                    {
                        var title = core.DocumentTitle;
                        if (!string.IsNullOrWhiteSpace(title)) Text = title;
                    }
                    catch (Exception err) { _bridge.Note("改窗口标题失败：" + err.Message); }
                };
                /* ⚠️ 订阅之后必须**补读一次**：页面在导航完成之前就把标题写好了（`manager/main.ts`
                 * 一启动就 `switchTab('dicts')`），那时这条通知还没人听 —— 只挂订阅的话，
                 * 第一次打开选项窗看到的仍是写死的「词库管理」。 */
                var titleNow = core.DocumentTitle;
                if (!string.IsNullOrWhiteSpace(titleNow)) Text = titleNow;
                _ready.TrySetResult(core);
            }
            catch (Exception err)
            {
                _ready.TrySetException(err);
                _bridge.Note("管理窗页面打不开：" + err.Message);
            }
        }

        /// <summary>首次打开时摆到主显示器工作区正中（只在第一次做：之后用户挪到哪儿就留在哪儿）</summary>
        private void CenterOnScreen()
        {
            var primary = Screen.PrimaryScreen.Bounds;
            var work = Native.WorkAreaAt(new Point(primary.X + primary.Width / 2,
                                                   primary.Y + primary.Height / 2));
            var x = work.X + Math.Max(0, (work.Width - Width) / 2);
            var y = work.Y + Math.Max(0, (work.Height - Height) / 2);
            Location = new Point(x, y);
            Native.SetWindowPos(Handle, IntPtr.Zero, x, y, Width, Height,
                                Native.SWP_NOZORDER | Native.SWP_NOACTIVATE);
        }

        /* ── `IShellHost`：这个窗口那一半（窗口级全是空操作，app 级转发给 `AppHost`）──── */

        public void Emit(string name, string payloadJson)
        {
            try
            {
                var core = _web.CoreWebView2;
                if (core == null) return;
                core.PostWebMessageAsJson(ShellBridge.Event(name, payloadJson));
            }
            catch (Exception err) { _bridge.Note("管理窗推事件失败：" + err.Message); }
        }

        /// <summary>
        /// **广播**一条事件给所有开着的窗口。
        /// ⚠️ `Emit` **只推自己这一页**，而词库 / 语音 / 翻译设置的变更大半是在管理窗里发起的 ——
        /// 必须把话递给**悬浮窗**那一份，由它的 `Emit` 扇出给自己 + 管理窗 + 托盘菜单。
        /// </summary>
        public void Broadcast(string name, string payloadJson)
        {
            var host = AppHost;
            if (host != null && !ReferenceEquals(host, this))
            {
                host.Broadcast(name, payloadJson);
                return;
            }
            /* 没有悬浮窗那一路（理论上不会发生，Program.cs 里两扇窗是一起建的）：自己兜着发一次 */
            Emit(name, payloadJson);
        }

        public string ManagerConsumeInitialTab() { return ManagerConsumeTab(); }
        public void ManagerClose()
        {
            // 外壳层必须一起收 —— 否则窗口没了、投影还留在屏幕上
            _chrome.Hide();
            Hide();
        }
        public void ManagerMinimize() { WindowState = FormWindowState.Minimized; }
        public void ManagerBeginDrag() { Native.BeginWindowDrag(Handle); }

        /// <summary>渲染进程上报界面底色与焦点状态；页面是透明的，窗口底色就是卡片底色</summary>
        public void ManagerSurface(string fill, string border, string theme, bool focused)
        {
            if (!string.IsNullOrWhiteSpace(theme)) _dark = theme == "dark";
            if (!string.IsNullOrWhiteSpace(fill)) _fill = fill;
            if (!string.IsNullOrWhiteSpace(border)) _border = border;
            _focused = focused;
            RefreshChrome();
        }

        /// <summary>
        /// 按当前尺寸与上报的底色重画外壳（整窗一块圆角卡片）。
        /// ⚠️ 窗口 Region 管"点得到的范围"、外壳层管"看起来什么样"：那 2px 内缩就是让硬边落在实心填充内部。
        /// </summary>
        private void RefreshChrome()
        {
            if (!IsHandleCreated || !Visible) { _chrome.Hide(); return; }
            var bounds = new Rectangle(Left, Top, Width, Height);
            _chrome.UpdateWindowCard(Handle, bounds, ClientSize.Width, ClientSize.Height,
                                     (float)(CornerRadius * DeviceDpi / 96.0),
                                     CssColor.Parse(_fill, Color.FromArgb(0xF7, 0xF8, 0xFC)),
                                     CssColor.Parse(_border, Color.FromArgb(0xE4, 0xE6, 0xEE)),
                                     _dark, _focused);
        }

        /// <summary>`debug:window("manager")` 的现场（外壳层也报出去）</summary>
        internal string DescribeSelf()
        {
            var chromeShape = Native.DescribeShape(_chrome.Handle, new System.Collections.Generic.List<Point>());
            var sb = new System.Text.StringBuilder();
            sb.Append("{\"present\":true,\"role\":\"manager\",\"visible\":")
              .Append(Visible ? "true" : "false");
            sb.Append(",\"foregroundIsSelf\":")
              .Append(Native.GetForegroundWindow() == Handle ? "true" : "false");
            sb.Append(",\"bounds\":{\"x\":").Append(Left).Append(",\"y\":").Append(Top)
              .Append(",\"width\":").Append(Width).Append(",\"height\":").Append(Height).Append('}');
            sb.Append(",\"regionBox\":null");
            sb.Append(",\"probes\":{}");
            sb.Append(",\"chrome\":{\"present\":true,\"visible\":")
              .Append(Native.IsWindowVisible(_chrome.Handle) ? "true" : "false");
            sb.Append(",\"bounds\":{\"x\":").Append(chromeShape.Bounds.X)
              .Append(",\"y\":").Append(chromeShape.Bounds.Y)
              .Append(",\"width\":").Append(chromeShape.Bounds.Width)
              .Append(",\"height\":").Append(chromeShape.Bounds.Height).Append("}}");
            sb.Append(",\"applied\":null}");
            return sb.ToString();
        }

        /* 窗口级的能力（`layout:*` / `shape:*` / 拖悬浮窗…）：管理窗**不参与**。
         * ⚠️ 这几个**必须是空操作**，不能转发给悬浮窗 —— 管理窗整窗一块卡片、不需要逐元素形状，
         *    转发过去会把悬浮窗按管理窗的形状裁掉。 */

        public string LayoutInfo() { return "{}"; }
        public void SetLayout(string requestJson) { }
        public void SetShape(string regionsJson, string theme, bool focused) { }
        /// <summary>`boot:ready` 是**悬浮窗**那一条（它启动时停在屏幕外，等页面首帧再摆回来）；管理窗不参与。</summary>
        public void BootReady() { }
        public void DragPrepare() { }
        public void DragStart() { }
        public void DragMove(double? sentAtMs) { }
        public void DragEnd() { }
        public void ExpandFromEdge() { }
        public void CollapseToEdge() { }
        public void ResetPill() { }
        public void AbsorbToEdge(string edgeWire) { }
        public void HideWindow() { Hide(); }
        public string DescribeWindow(string role)
        {
            if (role == "manager") return DescribeSelf();
            return AppHost.DescribeWindow(role);
        }
        public string DragStats() { return "{\"frames\":0,\"median\":0,\"p95\":0,\"max\":0,\"samples\":[]}"; }
        public void PlacePill(double x, double y) { }

        /* app 级：转发给"应用那一侧"（这些不属于某个窗口）*/

        public string PickDictionaryFiles() { return AppHost.PickDictionaryFiles(); }
        public string ConfigDirPath() { return AppHost.ConfigDirPath(); }
        /// <summary>要联网那几条路的后台线程靠它切回 UI 线程（见 `DeferredReply` 那段说明）</summary>
        public void PostToUiThread(Action action) { AppHost.PostToUiThread(action); }
        public void WriteClipboard(string text) { AppHost.WriteClipboard(text); }
        public string ReadClipboard() { return AppHost.ReadClipboard(); }
        public void RevealInPath(string path) { AppHost.RevealInPath(path); }
        public void RequestClose() { AppHost.RequestClose(); }
        public void ResolveClose(string choice, bool remember) { AppHost.ResolveClose(choice, remember); }
        public void ImportDictionary(string path) { AppHost.ImportDictionary(path); }

        /* 托盘菜单那一组：管理窗不做这些，一律转给应用那一侧 */
        public void TrayMenuShow() { AppHost.TrayMenuShow(); }
        public void TrayMenuHide() { AppHost.TrayMenuHide(); }
        public void TrayMenuPlace(double width, double height, string fill, string border, string theme)
        {
            AppHost.TrayMenuPlace(width, height, fill, border, theme);
        }
        public string TrayState() { return AppHost.TrayState(); }
        public bool TraySetLogin(bool enabled) { return AppHost.TraySetLogin(enabled); }
        public void TrayToggleFloating() { AppHost.TrayToggleFloating(); }
        public void TrayResetPill() { AppHost.TrayResetPill(); }
        public void TrayQuit() { AppHost.TrayQuit(); }
        public void FocusSelf() { AppHost.FocusSelf(); }

        /// <summary>
        /// 应用级能力的中转站：由 `Program` 在起动时接上（见 <see cref="AppHost"/>）——
        /// 管理窗不自己持有一份"选文件 / 剪贴板 / 退出"的实现。
        /// </summary>
        internal static IShellHost AppHost = NoShellHost.Instance;
    }
}
