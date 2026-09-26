namespace Lookup.App
{
    using System;
    using System.Runtime.InteropServices;

    /// <summary>
    /// **全局鼠标钩子**：鼠标在菜单外面按一下就把菜单收掉。
    ///
    /// ⚠️ 为什么非要它：托盘菜单**刻意不抢前台** —— 一抢，Windows 托盘那个"隐藏图标"浮出面板
    ///    就会当场失活缩回去；代价是菜单收不到 `Deactivate`，于是"点到别处就关掉"只能自己盯。
    ///
    /// ⚠️ 钩子回调跑在**装钩子的那个线程**上（这里是 UI 线程）—— 里面只许做一件事：判断要不要
    ///    收菜单；**不许**做耗时的事（做慢了整台机器的鼠标都跟着卡），而且必须 `CallNextHookEx`。
    /// </summary>
    internal sealed class OutsideClickHook : IDisposable
    {
        private const int WH_MOUSE_LL = 14;
        private const int WM_LBUTTONDOWN = 0x0201;
        private const int WM_RBUTTONDOWN = 0x0204;
        private const int WM_MBUTTONDOWN = 0x0207;

        [StructLayout(LayoutKind.Sequential)]
        private struct MSLLHOOKSTRUCT
        {
            public Native.POINT pt;
            public uint mouseData;
            public uint flags;
            public uint time;
            public IntPtr dwExtraInfo;
        }

        private delegate IntPtr HookProc(int code, IntPtr wParam, IntPtr lParam);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern IntPtr SetWindowsHookExW(int idHook, HookProc callback, IntPtr module, uint threadId);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool UnhookWindowsHookEx(IntPtr hook);

        [DllImport("user32.dll")]
        private static extern IntPtr CallNextHookEx(IntPtr hook, int code, IntPtr wParam, IntPtr lParam);

        [DllImport("kernel32.dll")]
        private static extern IntPtr GetModuleHandleW(string name);

        private IntPtr _hook = IntPtr.Zero;
        private HookProc _callback; // 必须留住引用，否则会被 GC 掉（钩子回调是原生代码在调）

        /// <summary>点到这些矩形之外就收菜单（窗口坐标不需要 —— 给的是**屏幕**坐标）</summary>
        internal Func<int, int, bool> IsInside;

        /// <summary>判定"这一下点在菜单外面"时叫它</summary>
        internal Action OutsideClicked;

        internal void Install()
        {
            if (_hook != IntPtr.Zero) return;
            _callback = OnMouse;
            try
            {
                /* ⚠️ `hMod` 要传**本模块的句柄**：托管代码在 .NET Framework 上跑 LL 钩子时，
                 *    传 IntPtr.Zero 在老系统上会失败（`SetWindowsHookEx` 回 0）。 */
                _hook = SetWindowsHookExW(WH_MOUSE_LL, _callback, GetModuleHandleW(null), 0);
            }
            catch (Exception)
            {
                _hook = IntPtr.Zero;
            }
        }

        internal void Uninstall()
        {
            if (_hook == IntPtr.Zero) return;
            try { UnhookWindowsHookEx(_hook); } catch (Exception) { }
            _hook = IntPtr.Zero;
            _callback = null;
        }

        private IntPtr OnMouse(int code, IntPtr wParam, IntPtr lParam)
        {
            try
            {
                if (code >= 0)
                {
                    var message = wParam.ToInt32();
                    if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN)
                    {
                        var data = (MSLLHOOKSTRUCT)Marshal.PtrToStructure(lParam, typeof(MSLLHOOKSTRUCT));
                        var inside = IsInside;
                        if (inside != null && !inside(data.pt.X, data.pt.Y))
                        {
                            var outside = OutsideClicked;
                            if (outside != null) outside();
                        }
                    }
                }
            }
            catch (Exception)
            {
                /* 钩子里绝不许把异常抛出去（那会把整台机器的鼠标卡住） */
            }
            return CallNextHookEx(_hook, code, wParam, lParam);
        }

        public void Dispose()
        {
            Uninstall();
        }
    }
}
