namespace Lookup.App
{
    using System;
    using System.Collections.Generic;
    using System.Drawing;
    using System.Globalization;
    using System.Text;
    using System.Windows.Forms;
    using Lookup.Host;

    /// <summary>
    /// **悬浮窗的几何与形状**（胶囊位置 → 窗口矩形 → 窗口 Region → 拖动 → 吸边）：
    /// 界面照用参考实现那一份、逐条照抄，**不许重新设计**；位置**不做持久化**，每次回默认位置。
    /// ⚠️ 抗锯齿圆角与投影由 <see cref="ChromeWindow"/> 那张分层窗画，Region 只能裁硬边。
    /// </summary>
    internal sealed class FloatingLayout
    {
        /* ── 几何常量 ─────────────────────────────────────────────────────── */
        internal const double PillWidth = 464;
        internal const double PillHeight = 52;
        /// <summary>投影宽度：投影由外壳层按真实形状画，不在几何里留白（所以是 0）</summary>
        private const double Shadow = 0;
        private const double EdgeInset = 0;
        private const double AbsorbDistance = 28;
        internal const int AbsorbDuration = 190;
        private const int StripThickness = 8;
        private const int StripWindowDepth = 64;
        private const double MaxPanelRatio = 0.78;
        /// <summary>窗口 Region 比卡片边界**往里缩**这么多像素：硬边落在外壳的实心填充里</summary>
        private const int RegionInset = 2;

        private readonly Form _form;
        private readonly Action<string, string> _emit;

        private double _scale = 1.0;
        private double _pillX;
        private double _pillY;
        private double _pillTopInWindow;
        private string _edge;                 // null / "left" / "right" / "top"
        private bool _absorbed;
        private bool _docked;
        private double _layoutPanelHeight;
        private string _layoutDirection = "down";
        private List<Shape> _regions;
        private string _applied;
        private Rectangle _physicalBounds = Rectangle.Empty;
        private bool _appliedSent;
        private bool _dragging;
        private bool _dragPending;
        private PointF? _dragOriginCursor;
        private PointF? _dragOriginPill;
        private int _absorbToken;
        private readonly List<double> _dragLatencies = new List<double>();
        /// <summary>失焦之后多久自动吸边</summary>
        private const int AutoAbsorbDelayMs = 1000;
        private readonly Timer _autoAbsorbTimer;

        /// <summary>等页面首帧的兜底时长（毫秒）：页面万一没发 `boot:ready`，到点照样摆出来</summary>
        private const int RevealFallbackMs = 3000;
        /// <summary>
        /// 页面首帧还没出来时窗口停在**屏幕外**的落点（`null` = 已经在真实位置）。
        /// ★ 为什么是"屏幕外"而不是"藏起来"：藏起来的窗口 WebView2 不渲染（rAF 不跑），
        ///   页面那声 `boot:ready` 就永远等不到；屏幕外 + 仍是显示状态才两全。
        /// </summary>
        private Point? _parkAt;
        private Timer _revealTimer;

        internal FloatingLayout(Form form, Action<string, string> emit)
        {
            _form = form;
            _emit = emit;
            /* 失焦满一秒（还没重新拿到焦点）就把胶囊吸回最近的边缘 */
            _autoAbsorbTimer = new Timer { Interval = AutoAbsorbDelayMs };
            _autoAbsorbTimer.Tick += delegate
            {
                _autoAbsorbTimer.Stop();
                AutoAbsorbIfIdle();
            };
            ResetToDefault();
        }

        /// <summary>一块"真正会画出东西"的矩形（窗口坐标，DIP）</summary>
        internal sealed class Shape
        {
            internal double X, Y, W, H, R;
            internal string Fill, Border;
        }

        internal double Scale { get { return _scale; } }

        /// <summary>
        /// 现在是不是**吸附在屏幕边缘**（只剩一条窄条那样）；托盘图标左键双击那条"呼出"按它分类。
        /// </summary>
        internal bool IsAbsorbed { get { return _absorbed; } }

        /* ── DIP ↔ 物理像素（主显示器左上角是 DIP (0,0)）───────────────────── */

        private static Point _virtualOrigin = Point.Empty;
        private static bool _originCaptured;

        private static Point VirtualOrigin
        {
            get
            {
                if (!_originCaptured)
                {
                    _virtualOrigin = Screen.PrimaryScreen.Bounds.Location;
                    _originCaptured = true;
                }
                return _virtualOrigin;
            }
        }

        private double ToDipX(int physicalX) { return (physicalX - VirtualOrigin.X) / _scale; }
        private double ToDipY(int physicalY) { return (physicalY - VirtualOrigin.Y) / _scale; }
        private int ToPhysicalX(double dipX) { return (int)Math.Round(VirtualOrigin.X + dipX * _scale); }
        private int ToPhysicalY(double dipY) { return (int)Math.Round(VirtualOrigin.Y + dipY * _scale); }

        private Rectangle ToDip(Rectangle physical)
        {
            return new Rectangle(
                (int)Math.Round(ToDipX(physical.X)),
                (int)Math.Round(ToDipY(physical.Y)),
                (int)Math.Round(physical.Width / _scale),
                (int)Math.Round(physical.Height / _scale));
        }

        private Rectangle CurrentWorkAreaPhysical()
        {
            var center = new Point(ToPhysicalX(_pillX + PillWidth / 2), ToPhysicalY(_pillY + PillHeight / 2));
            return Native.WorkAreaAt(center);
        }

        private Rectangle CurrentWorkAreaDip() { return ToDip(CurrentWorkAreaPhysical()); }

        /// <summary>按胶囊所在显示器取缩放；启动时按主显示器取一次</summary>
        private void RefreshScale()
        {
            var center = new Point(
                (int)Math.Round(VirtualOrigin.X + (_pillX + PillWidth / 2) * _scale),
                (int)Math.Round(VirtualOrigin.Y + (_pillY + PillHeight / 2) * _scale));
            var scale = Native.ScaleAt(center);
            _scale = scale > 0 ? scale : 1.0;
        }

        private void RefreshScaleFromPrimary()
        {
            var bounds = Screen.PrimaryScreen.Bounds;
            var scale = Native.ScaleAt(new Point(bounds.X + bounds.Width / 2, bounds.Y + bounds.Height / 2));
            _scale = scale > 0 ? scale : 1.0;
        }

        /* ── 对外：页面要的那几条 ─────────────────────────────────────────── */

        /// <summary>`layout:info` —— 页面开局就得知道的东西（工作区 / 缩放 / 胶囊在哪 / 胶囊在窗口里多高）</summary>
        internal string LayoutInfo()
        {
            var work = CurrentWorkAreaDip();
            var sb = new StringBuilder();
            sb.Append("{\"workArea\":{\"x\":").Append(Num(work.X)).Append(",\"y\":").Append(Num(work.Y));
            sb.Append(",\"width\":").Append(Num(work.Width)).Append(",\"height\":").Append(Num(work.Height));
            sb.Append("},\"scaleFactor\":").Append(Num(_scale));
            sb.Append(",\"edge\":").Append(_edge == null ? "null" : Dispatch.Quote(_edge));
            sb.Append(",\"pillX\":").Append(Num(_pillX));
            sb.Append(",\"pillY\":").Append(Num(_pillY));
            /*
             * `pillTop` 必须一开始就给得出来：只走 `layout:applied` 的话，页面加载完到第一条事件
             * 之间会有一小段 pillTop=0，那段时间算出来的展开方向是错的。
             * ⚠️ 要用**记着**的那个值、不是现算 —— 吸入成窄条时窗口是 64px 的窄条几何，现算会把页面里记着的位置带偏。
             */
            sb.Append(",\"pillTop\":").Append(Num(_pillTopInWindow)).Append('}');
            return sb.ToString();
        }

        /// <summary>`layout:set`（页面请求改变内容尺寸）</summary>
        internal void SetLayout(string requestJson)
        {
            _layoutPanelHeight = Math.Max(0, Math.Round(ShellBridge.Num(requestJson, "panelHeight") ?? 0));
            var direction = ShellBridge.Text(requestJson, "direction");
            _layoutDirection = direction == "up" ? "up" : "down";
            ApplyLayout(false);
        }

        /// <summary>窗口显示**之前**先摆一次位。</summary>
        internal void SetLayoutInitial() { ApplyLayout(true); }

        /// <summary>`shape:set`（页面按真实 DOM 上报的可见形状，窗口坐标 DIP）</summary>
        internal void SetShape(string regionsJson, string theme, bool focused)
        {
            _regions = ParseShapes(regionsJson);
            _darkTheme = theme == "dark";
            _focused = focused;
            ApplyRegion(_physicalBounds, _regions);
            UpdateChrome(_physicalBounds, _regions);
        }

        /// <summary>
        /// 外壳层（<see cref="ChromeWindow"/>）—— 抗锯齿圆角与投影由它画，由 `FloatingWindow` 建好接进来。
        /// 没有它也能跑（窗口 Region 照样把形状裁对），只是边缘是硬的、没有投影。
        /// </summary>
        internal ChromeWindow Chrome;

        /// <summary>
        /// 把卡片形状交给外壳层重画。
        /// ⚠️ **吸入成窄条时不需要** —— 那条 8px 的窄条上看不出抗锯齿，投影反而会糊在屏幕边缘上；
        ///    宿主窗口没显示时也绝不能亮出来（否则"关了悬浮窗、投影还留在屏幕上"）。
        /// </summary>
        private void UpdateChrome(Rectangle bounds, List<Shape> regions)
        {
            var chrome = Chrome;
            if (chrome == null) return;
            if (_absorbed || regions == null || regions.Count == 0 || !_form.Visible)
            {
                chrome.Hide();
                return;
            }

            var cards = new List<ChromeWindow.Card>();
            foreach (var rect in regions)
            {
                if (rect.W < 1 || rect.H < 1) continue;
                var card = new ChromeWindow.Card
                {
                    Rect = new RectangleF((float)(rect.X * _scale), (float)(rect.Y * _scale),
                                          (float)(rect.W * _scale), (float)(rect.H * _scale)),
                    Radius = (float)(rect.R * _scale),
                    Fill = CssColor.Parse(rect.Fill, Color.White),
                    Border = CssColor.Parse(rect.Border, Color.Transparent),
                };
                /*
                 * ⚠️ 卡片的 `Rect` 是**窗口客户区坐标**（物理像素），**不是屏幕坐标** ——
                 *    外壳层自己会加上窗口原点，这里换算一次（DIP × 缩放）就够，别再多减一次窗口原点。
                 */
                cards.Add(card);
            }
            chrome.Update(_form.Handle, bounds, cards, _darkTheme, _focused);
        }

        private bool _darkTheme;
        private bool _focused;

        /// <summary>`drag:prepare`：记下起点（拖动像素精确、不吃掉阈值位移）</summary>
        internal void DragPrepare()
        {
            _dragPending = true;
            _dragging = false;
            _absorbed = false;
            _docked = false;
            _dragLatencies.Clear();
            _dragOriginCursor = CursorDip();
            _dragOriginPill = new PointF((float)_pillX, (float)_pillY);
        }

        internal void DragStart()
        {
            if (!_dragPending)
            {
                // 正常流程一定先 prepare；万一没有就现采一次，保证不会用到过期的起点
                _dragOriginCursor = CursorDip();
                _dragOriginPill = new PointF((float)_pillX, (float)_pillY);
            }
            _dragging = true;
            _edge = null;
        }

        /// <summary>`drag:move`：按光标位移挪胶囊，再让窗口跟着走</summary>
        internal void DragMove(double? sentAtMs)
        {
            if (!_dragging || _dragOriginCursor == null || _dragOriginPill == null) return;
            var cursor = CursorDip();
            _pillX = _dragOriginPill.Value.X + (cursor.X - _dragOriginCursor.Value.X);
            _pillY = _dragOriginPill.Value.Y + (cursor.Y - _dragOriginCursor.Value.Y);
            ClampPillIntoWorkArea();
            ApplyLayout(false);
            if (sentAtMs.HasValue)
            {
                // 窗口这时候已经挪完了（ApplyLayout 里 SetWindowPos 已执行）——
                // 量的是"手指动到窗口真的挪过去"这一段，即拖动跟不跟手
                var latency = NowMs() - sentAtMs.Value;
                if (latency >= 0 && _dragLatencies.Count < 240) _dragLatencies.Add(latency);
            }
        }

        /// <summary>`drag:end`：靠近左/右/上边缘就吸入，否则停在原地（底边不响应）</summary>
        internal void DragEnd()
        {
            var wasDragging = _dragging;
            _dragging = false;
            _dragPending = false;
            _dragOriginCursor = null;
            _dragOriginPill = null;
            if (!wasDragging) return;

            var edge = NearestAbsorbEdge();
            if (edge != null)
            {
                AbsorbToEdge(edge);
                return;
            }
            _edge = null;
            _docked = false;
            ApplyLayout(false);
        }

        /// <summary>拖动的跟手统计（`debug:dragStats`）</summary>
        internal string DragStats()
        {
            var samples = new List<double>(_dragLatencies);
            samples.Sort();
            double median = 0, p95 = 0, max = 0;
            if (samples.Count > 0)
            {
                median = Math.Round(samples[samples.Count / 2], 2);
                p95 = Math.Round(samples[Math.Min(samples.Count - 1, (int)(samples.Count * 0.95))], 2);
                max = Math.Round(samples[samples.Count - 1], 2);
            }
            var sb = new StringBuilder();
            sb.Append("{\"frames\":").Append(samples.Count.ToString(CultureInfo.InvariantCulture));
            sb.Append(",\"median\":").Append(Num(median)).Append(",\"p95\":").Append(Num(p95));
            sb.Append(",\"max\":").Append(Num(max)).Append(",\"samples\":[");
            for (var i = 0; i < samples.Count; i++)
            {
                if (i > 0) sb.Append(',');
                sb.Append(Num(samples[i]));
            }
            sb.Append("]}");
            return sb.ToString();
        }

        /// <summary>`edge:expand`：鼠标移到窄条上，把界面弹出来</summary>
        internal void ExpandFromEdge()
        {
            if (!_absorbed) return;
            var token = ++_absorbToken;
            _absorbed = false;
            _docked = true;
            _layoutPanelHeight = 0;

            ApplyLayout(false);
            _applied = MakeApplied(null, "out", _edge == "left" ? "left" : "right", true, _edge);
            Emit(_applied);

            Delay(AbsorbDuration, delegate
            {
                if (token != _absorbToken) return;
                _applied = MakeApplied(null, null, _edge == "left" ? "left" : "right", true, _edge);
                Emit(_applied);
            });
        }

        /// <summary>`edge:collapse`：鼠标离开，重新吸回当前贴着的边缘</summary>
        internal void CollapseToEdge()
        {
            if (!_docked || _edge == null) return;
            AbsorbToEdge(_edge);
        }

        /// <summary>`floating:reset`</summary>
        internal void ResetToDefault()
        {
            RefreshScaleFromPrimary();
            var spot = DefaultPillPosition();
            _edge = null;
            _absorbed = false;
            _docked = false;
            _pillX = spot.X;
            _pillY = spot.Y;
            _layoutPanelHeight = 0;
            _regions = null;
            _applied = MakeApplied();
            RefreshScale();
            ApplyLayout(false);
        }

        internal void PlacePill(double x, double y)
        {
            _pillX = x;
            _pillY = y;
            _edge = null;
            _absorbed = false;
            _docked = false;
            RefreshScale();
            ApplyLayout(false);
        }

        internal void AbsorbToEdge(string edge)
        {
            if (edge != "left" && edge != "right" && edge != "top") return;
            var work = CurrentWorkAreaPhysical();
            var token = ++_absorbToken;

            _edge = edge;
            _docked = false;
            _absorbed = false;
            _layoutPanelHeight = 0;

            // 胶囊的静止位置对齐到该边缘，弹出时就用这个位置
            var workDip = ToDip(work);
            if (edge == "left") _pillX = workDip.X + EdgeInset;
            else if (edge == "right") _pillX = workDip.X + workDip.Width - PillWidth - EdgeInset;
            else _pillY = workDip.Y + EdgeInset;

            ApplyLayout(false);
            _applied = MakeApplied(null, "in", edge == "left" ? "left" : "right", false, edge);
            Emit(_applied);

            Delay(AbsorbDuration, delegate
            {
                if (token != _absorbToken) return;
                _absorbed = true;
                _applied = MakeApplied();
                ApplyLayout(false);
                // 窄条形状由几何兜底给出，这里主动同步一次，免得等页面上报
                ApplyRegion(_physicalBounds, null);
            });
        }

        /// <summary>
        /// 启动时把窗口停到**屏幕外**，等页面报"首帧已经出来了"（`boot:ready`）再摆回真实位置。
        /// ⚠️ 必须在窗口**显示之前**调（`Application.Run` 会按当前位置把窗口显示出来）。
        /// ★ 这是"打开先闪一层阴影 / 空壳"那个毛病的堵法：承载窗口与外壳层**一起**停在屏幕外，
        ///   页面首帧一到，<see cref="RevealAfterBoot"/> 在同一轮里把两层一起摆回来 ——
        ///   用户看到的第一帧就已经是完整内容，而不是"先空壳、再内容"。
        /// </summary>
        internal void ParkOffScreen()
        {
            var screen = SystemInformation.VirtualScreen;
            _parkAt = new Point(screen.Right + 400, screen.Top + 40);
            ApplyLayout(false);

            /*
             * 兜底表：页面万一没发 `boot:ready`（脚本报错 / 环境怪），到点照样摆出来 ——
             * 宁可晚一点看到，也不能让用户以为程序没启动。
             */
            if (_revealTimer == null)
            {
                _revealTimer = new Timer { Interval = RevealFallbackMs };
                _revealTimer.Tick += delegate
                {
                    _revealTimer.Stop();
                    RevealAfterBoot();
                };
            }
            _revealTimer.Stop();
            _revealTimer.Start();
        }

        /// <summary>
        /// `boot:ready`：页面首帧出来了 —— 把窗口从屏幕外摆回真实位置。
        /// ⚠️ 必须走 <see cref="ApplyLayout"/>（宿主窗口与外壳层都在它里面摆），
        ///    单独挪宿主会把投影留在屏幕外（或反过来）。
        /// </summary>
        internal void RevealAfterBoot()
        {
            if (_revealTimer != null) _revealTimer.Stop();
            if (!_parkAt.HasValue) return;
            _parkAt = null;
            ApplyLayout(false);
        }

        /// <summary>显示：先把几何摆好再 Show，然后补一次布局（顺序不能反）</summary>
        internal void ShowWindow(bool focusInput)
        {
            /* ★ 用户要它，就必须出现在真实位置：先解除"停在屏幕外"（不管首帧信号来没来）。 */
            if (_revealTimer != null) _revealTimer.Stop();
            _parkAt = null;

            if (_absorbed) ExpandFromEdge();
            else ClampPillIntoWorkArea();

            ApplyLayout(false);

            if (!_form.Visible)
            {
                _form.Show();
                Native.SetWindowPos(_form.Handle, IntPtr.Zero, 0, 0, 0, 0,
                    Native.SWP_NOMOVE | Native.SWP_NOSIZE | Native.SWP_NOZORDER |
                    Native.SWP_NOACTIVATE | Native.SWP_SHOWWINDOW);
            }
            _form.TopMost = true;
            ApplyLayout(false);

            /*
             * 外壳层要压在承载网页的窗口**下面**：显示之后重排一次 z 序。
             * ⚠️ 顺序不能反：`ApplyLayout` 里那一次外壳更新会被"宿主不可见就不亮"的保护挡掉，
             *    必须**等窗口真的显示出来之后再补一次**，否则重新唤出后是没有投影的。
             */
            if (Chrome != null) Chrome.RefreshZOrder(_form.Handle);

            if (focusInput)
            {
                try
                {
                    _form.Activate();
                    Native.SetForegroundWindow(_form.Handle);
                }
                catch (Exception) { /* 前台锁定失败也无所谓 */ }
                // 页面据此把焦点放进输入框
                var payload = "null";
                if (_emit != null) _emit("floating:focus-input", payload);
            }
        }

        internal void HideWindow()
        {
            // 外壳层必须一起收 —— 否则窗口没了、投影还留在屏幕上
            if (Chrome != null) Chrome.Hide();
            _form.Hide();
            if (Chrome != null) Chrome.HideIfOwnerHidden(_form.Handle);
        }

        /* ── `debug:window`：窗口形状只能**问系统**（截图会被锁屏/别的窗口挡掉）─ */

        internal string DescribeWindow()
        {
            var hwnd = _form.Handle;
            var sb = new StringBuilder();
            if (hwnd == IntPtr.Zero) return "{\"present\":false}";

            var preliminary = Native.DescribeShape(hwnd, new List<Point>());
            var bounds = preliminary.Bounds;

            // 窗口上下预留了面板的空间，所以"窗口中心"通常不可见；该命中的是胶囊那一块
            var probes = new List<KeyValuePair<string, Point>>
            {
                new KeyValuePair<string, Point>("topLeft", new Point(bounds.Left + 2, bounds.Top + 2)),
                new KeyValuePair<string, Point>("topRight", new Point(bounds.Right - 3, bounds.Top + 2)),
                new KeyValuePair<string, Point>("bottomLeft", new Point(bounds.Left + 2, bounds.Bottom - 3)),
                new KeyValuePair<string, Point>("bottomRight", new Point(bounds.Right - 3, bounds.Bottom - 3)),
                new KeyValuePair<string, Point>("center", new Point(bounds.Left + bounds.Width / 2,
                                                                   bounds.Top + bounds.Height / 2)),
                new KeyValuePair<string, Point>("pillCenter", new Point(
                    bounds.Left + (int)Math.Round(PillWidth * _scale / 2),
                    bounds.Top + (int)Math.Round((_pillTopInWindow + PillHeight / 2) * _scale))),
            };

            var points = new List<Point>();
            foreach (var pair in probes) points.Add(pair.Value);
            var shape = Native.DescribeShape(hwnd, points);

            sb.Append("{\"present\":true,\"role\":\"floating\",\"scale\":").Append(Num(_scale));
            /*
             * `dpi` 是**这个窗口**的 DPI（`GetDpiForWindow`）：120 = 125% 屏上 PerMonitorV2 生效了，
             * 96 = 进程被判为 DPI 不感知（系统虚拟化的值）。与 `scale`（由 `GetDpiForMonitor` 算出的
             * 布局缩放）是**两个独立来源** —— 拿它验"那份 DPI 声明到底生效没有"。
             */
            sb.Append(",\"dpi\":").Append(Native.DpiOfWindow(hwnd));
            sb.Append(",\"visible\":").Append(Native.IsWindowVisible(hwnd) ? "true" : "false");
            sb.Append(",\"foregroundIsSelf\":")
              .Append(Native.GetForegroundWindow() == hwnd ? "true" : "false");
            sb.Append(",\"bounds\":{\"x\":").Append(shape.Bounds.X).Append(",\"y\":").Append(shape.Bounds.Y);
            sb.Append(",\"width\":").Append(shape.Bounds.Width).Append(",\"height\":").Append(shape.Bounds.Height).Append('}');
            sb.Append(",\"regionBox\":").Append(shape.RegionBox.IsEmpty
                ? "null"
                : "{\"x\":" + shape.RegionBox.X + ",\"y\":" + shape.RegionBox.Y +
                  ",\"width\":" + shape.RegionBox.Width + ",\"height\":" + shape.RegionBox.Height + "}");
            sb.Append(",\"probes\":{");
            for (var i = 0; i < probes.Count; i++)
            {
                bool inside;
                if (i > 0) sb.Append(',');
                inside = shape.Probes.TryGetValue(probes[i].Value.X + "," + probes[i].Value.Y, out inside) && inside;
                sb.Append('"').Append(probes[i].Key).Append("\":").Append(inside ? "true" : "false");
            }
            sb.Append("},\"placement\":{\"pillX\":").Append(Num(_pillX))
              .Append(",\"pillY\":").Append(Num(_pillY))
              .Append(",\"edge\":").Append(_edge == null ? "null" : Dispatch.Quote(_edge))
              .Append(",\"absorbed\":").Append(_absorbed ? "true" : "false").Append('}');
            /* 外壳层（分层窗口）也报出去：验收要能确认它跟着宿主窗口一起显示 / 隐藏 */
            var chrome = Chrome;
            if (chrome == null)
            {
                sb.Append(",\"chrome\":{\"present\":false}");
            }
            else
            {
                var chromeShape = Native.DescribeShape(chrome.Handle, new List<Point>());
                sb.Append(",\"chrome\":{\"present\":true,\"visible\":")
                  .Append(Native.IsWindowVisible(chrome.Handle) ? "true" : "false");
                sb.Append(",\"bounds\":{\"x\":").Append(chromeShape.Bounds.X)
                  .Append(",\"y\":").Append(chromeShape.Bounds.Y)
                  .Append(",\"width\":").Append(chromeShape.Bounds.Width)
                  .Append(",\"height\":").Append(chromeShape.Bounds.Height).Append("}}");
            }
            sb.Append(",\"applied\":").Append(_applied ?? "null").Append('}');
            return sb.ToString();
        }

        /* ── 内部：几何 ───────────────────────────────────────────────────── */

        private (double X, double Y) DefaultPillPosition()
        {
            var work = ToDip(Screen.PrimaryScreen.WorkingArea);
            return (Math.Round(work.X + (work.Width - PillWidth) / 2), Math.Round(work.Y + EdgeInset));
        }

        private static double Clamp(double value, double min, double max)
        {
            if (max < min) return min;
            return Math.Min(Math.Max(value, min), max);
        }

        private void ClampPillIntoWorkArea()
        {
            var work = ToDip(CurrentWorkAreaPhysical());
            const double margin = 40;
            _pillX = Clamp(_pillX, work.X - PillWidth + margin, work.X + work.Width - margin);
            _pillY = Clamp(_pillY, work.Y, work.Y + work.Height - PillHeight);
        }

        /// <summary>松手后该吸到哪条边；底边不参与（拖到下方不触发任何动作）</summary>
        private string NearestAbsorbEdge()
        {
            var work = ToDip(CurrentWorkAreaPhysical());
            var candidates = new List<KeyValuePair<string, double>>
            {
                new KeyValuePair<string, double>("left", _pillX - work.X),
                new KeyValuePair<string, double>("right", work.X + work.Width - (_pillX + PillWidth)),
                new KeyValuePair<string, double>("top", _pillY - work.Y),
            };
            candidates.Sort((a, b) => a.Value.CompareTo(b.Value));
            return candidates[0].Value <= AbsorbDistance ? candidates[0].Key : null;
        }

        /* ── 窗口焦点：推给页面 + 失焦自动吸边 ─────────────────────────────── */

        /// <summary>
        /// 窗口的焦点变了（<see cref="FloatingWindow"/> 的 `Activated` / `Deactivate` 转进来）。
        /// ⚠️ 两件事都必须在这儿做，漏掉任一条 → "点别的程序之后浮层与右键菜单挂在桌面上不收、胶囊也不自己吸回边缘"：
        /// ① 推 `floating:window-focus` 给页面（页面里的 `window.blur` **实测收不到**，"失焦就把浮层收掉"只能靠这一路信号）；
        /// ② 失焦起 1 秒的表，到点还没焦点、也没展开面板就把胶囊**吸回最近的边缘**，重新拿到焦点就把表停掉。
        /// </summary>
        internal void OnWindowFocus(bool focused)
        {
            if (_emit != null)
            {
                _emit("floating:window-focus", focused ? "{\"focused\":true}" : "{\"focused\":false}");
            }
            _autoAbsorbTimer.Stop();
            if (!focused) _autoAbsorbTimer.Start();
        }

        /// <summary>失焦满一秒后：确实还没焦点、也没有展开面板，就吸回最近的边缘</summary>
        private void AutoAbsorbIfIdle()
        {
            if (_absorbed || !_form.Visible) return;
            if (_form.ContainsFocus) return;
            /* 面板开着（比如关闭询问对话框）时不吸：那是用户正在操作的东西 */
            if (_layoutPanelHeight > 0) return;
            var edge = NearestAbsorbEdge();
            if (edge == null) return;
            AbsorbToEdge(edge);
        }

        private PointF CursorDip()
        {
            var cursor = Native.CursorPosition();
            return new PointF((float)ToDipX(cursor.X), (float)ToDipY(cursor.Y));
        }

        private static long NowMs()
        {
            return (long)(DateTime.UtcNow - new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc))
                .TotalMilliseconds;
        }

        private void Delay(int milliseconds, Action action)
        {
            var timer = new Timer { Interval = Math.Max(1, milliseconds) };
            timer.Tick += delegate
            {
                timer.Stop();
                timer.Dispose();
                try { action(); }
                catch (Exception) { /* 动画回调失败不该把进程弄崩 */ }
            };
            timer.Start();
        }

        /* ── 内部：布局与 Region ──────────────────────────────────────────── */

        /// <summary>
        /// 由胶囊位置与内容尺寸推导窗口几何，必要时挪窗口，然后通知页面。
        /// ⚠️ **窗口上下各预留出面板可能占用的空间**，于是展开面板时窗口完全不动：窗口一移动/变高，
        ///    WebView2 的表面还没重画完，屏幕上会短暂留着"按旧尺寸绘制"的那一帧（胶囊会闪一下）。
        ///    多出来的区域由 Region 裁掉。
        /// </summary>
        private void ApplyLayout(bool initial)
        {
            if (!_form.IsHandleCreated) return;
            RefreshScale();

            Rectangle bounds;
            string applied;

            if (_absorbed)
            {
                bounds = StripBoundsPhysical();
                applied = MakeApplied(null, null, _edge == "left" ? "left" : "right", false, _edge, true);
            }
            else
            {
                Rectangle b;
                string a;
                NormalGeometry(out b, out a);
                bounds = b;
                applied = a;
            }

            /*
             * ★ 页面首帧还没出来时（`_parkAt` 非空）窗口停在屏幕外：**几何照常按真实矩形算**
             *   （Region / 页面拿到的 `layout:applied` 都不受影响），真正 SetWindowPos 的是屏幕外那一点。
             *   首帧一到，`RevealAfterBoot` 把它一次摆回来。
             */
            var place = _parkAt.HasValue
                ? new Rectangle(_parkAt.Value.X, _parkAt.Value.Y, bounds.Width, bounds.Height)
                : bounds;

            if (place != _physicalBounds)
            {
                _physicalBounds = place;
                Native.SetWindowPos(_form.Handle, IntPtr.Zero, place.X, place.Y, place.Width, place.Height,
                    Native.SWP_NOZORDER | Native.SWP_NOACTIVATE | Native.SWP_NOCOPYBITS);
            }

            ApplyRegion(place, _regions);

            /*
             * 外壳层也要跟着走：窗口挪了/尺寸变了，投影得跟过去。
             * ⚠️ 这里**每一帧都调**（哪怕位图能复用）—— 拖动时窗口一直在动，分层位图跟着窗口走就够。
             * ⚠️ 传的是 `place`（**真实窗口矩形**）：停在屏幕外时外壳必须跟着停 ——
             *    只挪宿主不挪它，投影就会孤零零留在屏幕上（那正是"先闪一层阴影"的现场）。
             */
            UpdateChrome(place, _regions);

            if (!_appliedSent || _applied != applied)
            {
                // 第一次一定要发：页面是在加载完才订阅的，而窗口创建时就"发"过一次（那次没人听）
                _appliedSent = true;
                _applied = applied;
                if (!initial) Emit(applied);
            }
        }

        private void NormalGeometry(out Rectangle bounds, out string applied)
        {
            var work = CurrentWorkAreaPhysical();
            var workDip = ToDip(work);

            var maxPanel = Math.Max(0, Math.Round(workDip.Height * MaxPanelRatio) - PillHeight);
            var spaceAbove = Clamp(_pillY - workDip.Y, 0, maxPanel);
            var spaceBelow = Clamp(workDip.Y + workDip.Height - (_pillY + PillHeight), 0, maxPanel);

            var windowWidth = PillWidth;
            var windowHeight = spaceAbove + PillHeight + spaceBelow;

            var x = Clamp(_pillX, workDip.X, Math.Max(workDip.X, workDip.X + workDip.Width - windowWidth));
            var y = Clamp(_pillY - spaceAbove, workDip.Y, Math.Max(workDip.Y, workDip.Y + workDip.Height - windowHeight));

            bounds = new Rectangle(
                ToPhysicalX(x), ToPhysicalY(y),
                (int)Math.Round(windowWidth * _scale), Math.Max(1, (int)Math.Round(windowHeight * _scale)));

            // 横向：贴右边缘时内容向左生长，贴左边缘向右生长，其余优先向右
            string align;
            if (_edge == "right") align = "right";
            else if (_edge == "left") align = "left";
            else
            {
                var spaceRight = ToDipX(work.X + work.Width) - (_pillX + PillWidth);
                align = spaceRight >= 8 ? "left" : "right";
            }

            var direction = _layoutDirection == "up" || _layoutDirection == "down" ? _layoutDirection : "down";
            var pillTop = _pillY - y;
            _pillTopInWindow = pillTop;

            applied = MakeApplied(Math.Min(_layoutPanelHeight, maxPanel), null, align, _docked, _edge, false, direction, pillTop);
        }

        /// <summary>吸入成窄条时的窗口矩形：窗口开 64px 厚，只留 8px 可见</summary>
        private Rectangle StripBoundsPhysical()
        {
            var work = CurrentWorkAreaPhysical();
            var pillHeightPhysical = Math.Max(1, (int)Math.Round(PillHeight * _scale));
            var pillWidthPhysical = Math.Max(1, (int)Math.Round(PillWidth * _scale));
            if (_edge == "left")
            {
                return new Rectangle(work.X + StripThickness - StripWindowDepth, ToPhysicalY(_pillY),
                                     StripWindowDepth, pillHeightPhysical);
            }
            if (_edge == "top")
            {
                return new Rectangle(ToPhysicalX(_pillX), work.Y + StripThickness - StripWindowDepth,
                                     pillWidthPhysical, StripWindowDepth);
            }
            return new Rectangle(work.X + work.Width - StripThickness, ToPhysicalY(_pillY),
                                 StripWindowDepth, pillHeightPhysical);
        }

        /// <summary>
        /// 拼一份 `AppliedLayout`（推给页面的那份）。
        /// 字段：`width/panelHeight/direction/align/pillWidth/pillHeight/shadow/absorbed/absorbEdge/docked/edgeAnim/pillX/pillY/pillTop`。
        /// </summary>
        private string MakeApplied(double? panelHeight = null, string edgeAnim = null, string align = null,
                                   bool? docked = null, string absorbEdge = null, bool? absorbed = null,
                                   string direction = null, double? pillTop = null)
        {
            var sb = new StringBuilder();
            sb.Append("{\"width\":").Append(Num(PillWidth));
            sb.Append(",\"panelHeight\":").Append(Num(panelHeight ?? 0));
            sb.Append(",\"direction\":").Append(Dispatch.Quote(direction ?? _layoutDirection));
            sb.Append(",\"align\":").Append(Dispatch.Quote(align ?? "left"));
            sb.Append(",\"pillWidth\":").Append(Num(PillWidth));
            sb.Append(",\"pillHeight\":").Append(Num(PillHeight));
            sb.Append(",\"shadow\":").Append(Num(absorbed == true ? 0 : Shadow));
            sb.Append(",\"absorbed\":").Append((absorbed ?? _absorbed) ? "true" : "false");
            sb.Append(",\"absorbEdge\":").Append(
                absorbEdge == null ? "null" : Dispatch.Quote(absorbEdge));
            sb.Append(",\"docked\":").Append((docked ?? _docked) ? "true" : "false");
            sb.Append(",\"edgeAnim\":").Append(edgeAnim == null ? "null" : Dispatch.Quote(edgeAnim));
            // 胶囊位置每次都带上：页面靠它决定展开方向（拖完窗口不同步过去，下次展开就按旧位置判断）
            sb.Append(",\"pillX\":").Append(Num(_pillX));
            sb.Append(",\"pillY\":").Append(Num(_pillY));
            sb.Append(",\"pillTop\":").Append(Num(pillTop ?? _pillTopInWindow));
            sb.Append('}');
            return sb.ToString();
        }

        private void Emit(string appliedJson)
        {
            if (_emit != null) _emit("layout:applied", appliedJson);
        }

        /// <summary>
        /// 用页面上报的矩形做窗口 Region。
        /// ⚠️ `ChromeWindow` 就在用：抗锯齿圆角与投影由那张分层窗盖在 Region 上面，别去找"缺失的外壳层"。
        /// </summary>
        private void ApplyRegion(Rectangle bounds, List<Shape> regions)
        {
            if (bounds.Width <= 0 || bounds.Height <= 0) return;

            var rects = regions;
            if (rects == null || rects.Count == 0) rects = DefaultRegions();

            // 吸入成窄条时不缩：那条 8px 的窄条没有描边，缩了反而只剩 6px
            var inset = _absorbed ? 0 : RegionInset;

            var combined = IntPtr.Zero;
            try
            {
                foreach (var rect in rects)
                {
                    // 页面报的是"元素在窗口里的位置"，裁到窗口内，免得 GetWindowRgn 读回来的形状比窗口还大
                    var left = (int)Math.Round(rect.X * _scale) + inset;
                    var top = (int)Math.Round(rect.Y * _scale) + inset;
                    var right = (int)Math.Round((rect.X + rect.W) * _scale) - inset;
                    var bottom = (int)Math.Round((rect.Y + rect.H) * _scale) - inset;

                    left = Math.Max(0, Math.Min(left, bounds.Width));
                    top = Math.Max(0, Math.Min(top, bounds.Height));
                    right = Math.Max(left + 1, Math.Min(right, bounds.Width));
                    bottom = Math.Max(top + 1, Math.Min(bottom, bounds.Height));
                    if (right <= left || bottom <= top) continue;

                    var radius = Math.Max(0, (int)Math.Round(rect.R * _scale) - inset) * 2;
                    var piece = Native.CreateRoundRectRgn(left, top, right, bottom, radius, radius);
                    if (piece == IntPtr.Zero) continue;

                    if (combined == IntPtr.Zero) combined = piece;
                    else
                    {
                        Native.CombineRgn(combined, combined, piece, Native.RGN_OR);
                        Native.DeleteObject(piece);
                    }
                }

                if (combined == IntPtr.Zero)
                {
                    combined = Native.CreateRectRgn(0, 0, bounds.Width, bounds.Height);
                }

                // SetWindowRgn 成功后区域归系统所有，不能再 DeleteObject
                if (Native.SetWindowRgn(_form.Handle, combined, true) != 0) combined = IntPtr.Zero;
            }
            finally
            {
                if (combined != IntPtr.Zero) Native.DeleteObject(combined);
            }
        }

        /// <summary>
        /// 页面还没上报形状时的兜底：**只兜一个胶囊**（兜成"整窗可见"会闪出一大块白板；胶囊的位置是几何算出来的、永远是对的）。
        /// </summary>
        private List<Shape> DefaultRegions()
        {
            var list = new List<Shape>();
            if (_absorbed)
            {
                if (_edge == "left")
                    list.Add(new Shape { X = StripWindowDepth - StripThickness, Y = 0, W = StripThickness, H = PillHeight, R = 3 });
                else if (_edge == "right")
                    list.Add(new Shape { X = 0, Y = 0, W = StripThickness, H = PillHeight, R = 3 });
                else
                    list.Add(new Shape { X = 0, Y = StripWindowDepth - StripThickness, W = PillWidth, H = StripThickness, R = 3 });
                return list;
            }

            list.Add(new Shape { X = 0, Y = _pillTopInWindow, W = PillWidth, H = PillHeight, R = PillHeight / 2 });
            return list;
        }

        /// <summary>把页面报上来的形状数组解析成 `Shape` 列表（`[{x,y,w,h,r,fill,border},…]`）</summary>
        private static List<Shape> ParseShapes(string json)
        {
            var list = new List<Shape>();
            foreach (var item in ShellBridge.SplitArray(json))
            {
                list.Add(new Shape
                {
                    X = ShellBridge.Num(item, "x") ?? 0,
                    Y = ShellBridge.Num(item, "y") ?? 0,
                    W = ShellBridge.Num(item, "w") ?? 0,
                    H = ShellBridge.Num(item, "h") ?? 0,
                    R = ShellBridge.Num(item, "r") ?? 0,
                    Fill = ShellBridge.Text(item, "fill"),
                    Border = ShellBridge.Text(item, "border"),
                });
            }
            return list;
        }

        private static string Num(double value)
        {
            return value.ToString("0.###", CultureInfo.InvariantCulture);
        }
    }
}
