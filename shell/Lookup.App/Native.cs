namespace Lookup.App
{
    using System;
    using System.Collections.Generic;
    using System.Drawing;
    using System.Runtime.InteropServices;
    using System.Windows.Forms;

    /// <summary>
    /// **平台那一小撮 Win32 调用**（窗口几何 / 窗口 Region / 光标 / 显示器）。
    ///
    /// 窗口形状那一整套（无边框 + 按上报的矩形裁 Region + 拖动 + 吸边）是参考实现的资产，
    /// 逐条对照参考实现/src/Native.cs`，只搬这一版用得到的。
    ///
    /// ⚠️ 这里**只有平台事实，没有业务约定**：什么算"贴边"、面板多高、胶囊多宽、窗口怎么按
    ///    胶囊位置摆 —— 都在 `FloatingLayout` 里。
    /// </summary>
    internal static class Native
    {
        /* ── SetWindowPos 的旗标 ──────────────────────────────────────────────── */
        internal const uint SWP_NOSIZE = 0x0001;
        internal const uint SWP_NOMOVE = 0x0002;
        internal const uint SWP_NOZORDER = 0x0004;
        internal const uint SWP_NOACTIVATE = 0x0010;
        internal const uint SWP_SHOWWINDOW = 0x0040;
        internal const uint SWP_NOCOPYBITS = 0x0100;

        internal const uint MONITOR_DEFAULTTONEAREST = 2;
        internal const int MDT_EFFECTIVE_DPI = 0;
        internal const int RGN_OR = 2;

        [StructLayout(LayoutKind.Sequential)]
        internal struct POINT
        {
            public int X;
            public int Y;
        }

        [StructLayout(LayoutKind.Sequential)]
        internal struct RECT
        {
            public int Left;
            public int Top;
            public int Right;
            public int Bottom;

            internal Rectangle ToRectangle()
            {
                return Rectangle.FromLTRB(Left, Top, Right, Bottom);
            }
        }

        [StructLayout(LayoutKind.Sequential)]
        internal struct MONITORINFO
        {
            public int cbSize;
            public RECT rcMonitor;
            public RECT rcWork;
            public int dwFlags;
        }

        [DllImport("user32.dll")]
        internal static extern bool GetCursorPos(out POINT point);

        [DllImport("user32.dll")]
        internal static extern IntPtr MonitorFromPoint(POINT pt, uint flags);

        [DllImport("user32.dll")]
        internal static extern bool GetMonitorInfo(IntPtr monitor, ref MONITORINFO info);

        [DllImport("user32.dll")]
        internal static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int cx, int cy, uint flags);

        [DllImport("user32.dll")]
        internal static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);

        [DllImport("user32.dll")]
        internal static extern bool IsWindowVisible(IntPtr hwnd);

        [DllImport("user32.dll")]
        internal static extern IntPtr GetForegroundWindow();

        [DllImport("user32.dll")]
        internal static extern bool SetForegroundWindow(IntPtr hwnd);

        [DllImport("user32.dll")]
        internal static extern int GetWindowRgn(IntPtr hwnd, IntPtr region);

        [DllImport("user32.dll")]
        internal static extern int SetWindowRgn(IntPtr hwnd, IntPtr region, bool redraw);

        [DllImport("gdi32.dll")]
        internal static extern IntPtr CreateRoundRectRgn(int left, int top, int right, int bottom,
                                                         int widthEllipse, int heightEllipse);

        [DllImport("gdi32.dll")]
        internal static extern IntPtr CreateRectRgn(int left, int top, int right, int bottom);

        [DllImport("gdi32.dll")]
        internal static extern int CombineRgn(IntPtr dest, IntPtr src1, IntPtr src2, int mode);

        [DllImport("gdi32.dll")]
        internal static extern bool DeleteObject(IntPtr obj);

        [DllImport("gdi32.dll")]
        internal static extern bool PtInRegion(IntPtr region, int x, int y);

        [DllImport("gdi32.dll")]
        internal static extern int GetRgnBox(IntPtr region, out RECT rect);

        [DllImport("shcore.dll")]
        private static extern int GetDpiForMonitor(IntPtr monitor, int dpiType, out uint dpiX, out uint dpiY);

        /* ── DPI 感知（PerMonitorV2）──────────────────────────────────────────
         *
         * ⚠️ 主力是 `app.manifest`（`dpiAwareness = PerMonitorV2`），下面两个是**核验与兜底**：
         *    一个能问出"这个窗口现在的 DPI 是多少"，一个能在建窗口之前补一次声明。
         */

        private const int DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = -4;

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool SetProcessDpiAwarenessContext(IntPtr value);

        [DllImport("user32.dll")]
        private static extern uint GetDpiForWindow(IntPtr hwnd);

        /// <summary>
        /// 兜底：**建任何窗口之前**声明逐显示器 DPI 感知（PerMonitorV2）。
        ///
        /// ⚠️ 主力是 `app.manifest`（它在**进程启动时**生效，连 WinForms 自己的初始化都吃得到）；
        ///    这里只是万一清单没被嵌进去（换了构建方式 / 被宿主剥离）时的第二道。
        /// ⚠️ 已经声明过时 Windows **不允许中途改**，这个调用直接回 false —— 那不算错；
        ///    Win10 1703 之前的系统没有这个导出，会抛 `EntryPointNotFoundException` —— 按"没成"处理。
        /// </summary>
        internal static bool TryEnablePerMonitorV2()
        {
            try
            {
                return SetProcessDpiAwarenessContext(
                    new IntPtr(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
            }
            catch (Exception)
            {
                return false;
            }
        }

        /// <summary>
        /// 某个窗口的 DPI（PerMonitorV2 下就是**真实** DPI：125% 的屏 → 120）。
        ///
        /// **检查标准 ② 用的就是它**：DPI 不感知的进程只会拿到 96（系统虚拟化过的值），
        /// 所以"回 120 还是回 96"能直接说明那份声明到底生效没有。
        /// 取不到（老系统 / 还没建句柄）回 0 —— 调用方按"读不到"处理，别拿 0 当 96 用。
        /// </summary>
        internal static uint DpiOfWindow(IntPtr hwnd)
        {
            if (hwnd == IntPtr.Zero) return 0;
            try
            {
                return GetDpiForWindow(hwnd);
            }
            catch (Exception)
            {
                return 0;
            }
        }

        /* ── 辅助（与参考实现的 `Native` 同名同语义）──────────────────────────── */

        /// <summary>光标所在的物理像素坐标</summary>
        internal static Point CursorPosition()
        {
            POINT pt;
            return GetCursorPos(out pt) ? new Point(pt.X, pt.Y) : Point.Empty;
        }

        /// <summary>某个物理像素点落在哪个显示器的工作区（已扣掉任务栏），单位物理像素</summary>
        internal static Rectangle WorkAreaAt(Point physicalPoint)
        {
            var monitor = MonitorFromPoint(new POINT { X = physicalPoint.X, Y = physicalPoint.Y },
                                            MONITOR_DEFAULTTONEAREST);
            if (monitor == IntPtr.Zero) return Screen.PrimaryScreen.WorkingArea;
            var info = new MONITORINFO { cbSize = Marshal.SizeOf(typeof(MONITORINFO)) };
            if (!GetMonitorInfo(monitor, ref info)) return Screen.PrimaryScreen.WorkingArea;
            return info.rcWork.ToRectangle();
        }

        /// <summary>显示器的 DPI 缩放（1.0 = 96 DPI）；查不到就回 1.0</summary>
        internal static double ScaleAt(Point physicalPoint)
        {
            var monitor = MonitorFromPoint(new POINT { X = physicalPoint.X, Y = physicalPoint.Y },
                                            MONITOR_DEFAULTTONEAREST);
            if (monitor != IntPtr.Zero)
            {
                uint dpiX, dpiY;
                if (GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, out dpiX, out dpiY) == 0 && dpiX > 0)
                {
                    return dpiX / 96.0;
                }
            }
            return 1.0;
        }

        [DllImport("user32.dll")]
        private static extern bool ReleaseCapture();

        [DllImport("user32.dll")]
        private static extern IntPtr SendMessage(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam);

        private const int WM_NCLBUTTONDOWN = 0x00A1;
        private const int HTCAPTION = 2;

        /// <summary>
        /// 让系统接管一次"拖动窗口"。
        ///
        /// WebView2 里 `-webkit-app-region: drag` 是不生效的（那是 Electron 的私货），
        /// 所以窗口标题栏的拖动改成：渲染进程在标题栏上按下鼠标时喊一声，
        /// 这里把捕获交给系统，由它跑自带的移动循环（参考实现 `Native.BeginWindowDrag` 同）。
        /// </summary>
        internal static void BeginWindowDrag(IntPtr hwnd)
        {
            if (hwnd == IntPtr.Zero) return;
            ReleaseCapture();
            SendMessage(hwnd, WM_NCLBUTTONDOWN, new IntPtr(HTCAPTION), IntPtr.Zero);
        }

        /* ── 分层窗口（外壳层画抗锯齿圆角与投影要用）────────────────────────────── */

        internal const int AC_SRC_OVER = 0x00;
        internal const int AC_SRC_ALPHA = 0x01;
        internal const int ULW_ALPHA = 0x00000002;

        [StructLayout(LayoutKind.Sequential)]
        internal struct SIZE
        {
            public int cx;
            public int cy;
            public SIZE(int w, int h) { cx = w; cy = h; }
        }

        [StructLayout(LayoutKind.Sequential, Pack = 1)]
        internal struct BLENDFUNCTION
        {
            public byte BlendOp;
            public byte BlendFlags;
            public byte SourceConstantAlpha;
            public byte AlphaFormat;
        }

        [DllImport("user32.dll")]
        internal static extern IntPtr GetDC(IntPtr hwnd);

        [DllImport("user32.dll")]
        internal static extern int ReleaseDC(IntPtr hwnd, IntPtr hdc);

        [DllImport("gdi32.dll")]
        internal static extern IntPtr CreateCompatibleDC(IntPtr hdc);

        [DllImport("gdi32.dll")]
        internal static extern bool DeleteDC(IntPtr hdc);

        [DllImport("gdi32.dll")]
        internal static extern IntPtr SelectObject(IntPtr hdc, IntPtr obj);

        [DllImport("user32.dll")]
        internal static extern bool UpdateLayeredWindow(IntPtr hwnd, IntPtr dstDc, ref POINT dst,
                                                        ref SIZE size, IntPtr srcDc, ref POINT src,
                                                        int colorKey, ref BLENDFUNCTION blend,
                                                        int flags);

        /* ── 窗口形状观测（靠它，**不靠截图**）──────────────────────────── */


        internal sealed class WindowShape
        {
            internal Rectangle Bounds;
            internal Rectangle RegionBox;
            /// <summary>每个采样点是否落在窗口形状内，键是"x,y"</summary>
            internal Dictionary<string, bool> Probes = new Dictionary<string, bool>();
        }

        /// <summary>
        /// 读回窗口矩形与窗口 Region，并对若干采样点做"在不在形状里"的判定。
        ///
        /// 为什么必须这么量：屏幕截图会被锁屏 / 别的置顶窗口挡掉，而"窗口形状对不对"
        /// 这件事只能问系统 —— 窗口 Region 是 Windows 自己持有的数据，问它等于问真相。
        /// （参考实现早就是这么做的，见那份项目规范 的「调试钩子」那一节。）
        /// </summary>
        internal static WindowShape DescribeShape(IntPtr hwnd, IEnumerable<Point> probes)
        {
            var shape = new WindowShape();
            if (hwnd == IntPtr.Zero) return shape;

            RECT rect;
            if (GetWindowRect(hwnd, out rect)) shape.Bounds = rect.ToRectangle();

            var region = CreateRectRgn(0, 0, 1, 1);
            try
            {
                if (GetWindowRgn(hwnd, region) != 0)
                {
                    RECT box;
                    if (GetRgnBox(region, out box) != 0) shape.RegionBox = box.ToRectangle();
                }
                foreach (var point in probes)
                {
                    // 传进来的是屏幕坐标，窗口 Region 是窗口坐标，这里换算一次
                    shape.Probes[point.X + "," + point.Y] =
                        PtInRegion(region, point.X - shape.Bounds.X, point.Y - shape.Bounds.Y);
                }
            }
            finally
            {
                DeleteObject(region);
            }
            return shape;
        }
    }
}
