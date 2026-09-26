namespace Lookup.App
{
    using System;
    using System.Drawing;
    using System.IO;
    using System.Reflection;
    using System.Runtime.InteropServices;
    using System.Windows.Forms;

    /// <summary>
    /// **托盘图标** —— 参考实现的 `src/Tray/TrayIcon.cs` 搬过来的那一套。
    ///
    /// 行为两条（**与参考实现不同，别照抄参考实现**）：**右键单击** → 弹菜单（菜单是自绘窗口
    /// <see cref="TrayMenuWindow"/>、不是系统菜单，所以不用 `NotifyIcon.ContextMenuStrip`）；
    /// **左键双击** → **只呼出**胶囊（<see cref="FloatingWindow.TraySummonFloating"/>：吸附则取消
    /// 吸附 / 收入托盘则显示 / 已显示则不动，**永远不会隐藏**，真正的开关仍是托盘菜单第一项）；
    /// **左键单击什么都不做**（单击就动窗口会让"点一下看看"把界面收走）。
    ///
    /// ⚠️ 图标与参考实现同一份资源（`assets/tray.png` / `assets/app.ico`，由 csproj 嵌进来），
    ///    取法两条：托盘用 `tray.png` → `Bitmap.GetHicon()` → `Icon.FromHandle`，
    ///    **句柄要自己销毁**（`Icon.FromHandle` 不接管生命周期，否则 GDI 句柄泄漏）；
    ///    应用图标要 `new Icon(stream, SystemInformation.IconSize)`（直接 `new Icon(stream)` 只有 32×32）。
    ///    两条都取不到才退回 `SystemIcons.Application` —— 那是**兜底**，不是常态。
    /// </summary>
    internal sealed class TrayIcon : IDisposable
    {
        private readonly NotifyIcon _notify;
        private readonly Action _onMenu;
        private readonly Action _onToggleFloating;
        private IntPtr _iconHandle = IntPtr.Zero;
        private Icon _icon;

        internal TrayIcon(Action onMenu, Action onToggleFloating)
        {
            _onMenu = onMenu;
            _onToggleFloating = onToggleFloating;
            _notify = new NotifyIcon
            {
                Icon = LoadIcon(),
                Text = "查词 · 悬浮词典",
                Visible = true,
            };
            /* 只认**右键**（见类注释）：左键那两下也会走到这里，靠这一句挡掉 ——
             * 双击的"两下"因此不会各弹一次菜单。 */
            _notify.MouseUp += (sender, e) =>
            {
                if (e.Button != MouseButtons.Right) return;
                var menu = _onMenu;
                if (menu != null) menu();
            };
            /* 左键双击 = **呼出胶囊**（吸附则取消吸附 / 收进托盘则显示出来 / 已经显示则不动）。
             *
             * ⚠️ 用 `MouseDoubleClick` 而**不是** `DoubleClick`：后者是 WinForms 给的便利
             *    事件、签名是 `EventHandler`（`e` 里**没有** `Button`），拿不到是哪个键。
             */
            _notify.MouseDoubleClick += (sender, e) =>
            {
                if (e.Button != MouseButtons.Left) return;
                var summon = _onToggleFloating;
                if (summon != null) summon();
            };
        }

        /// <summary>
        /// 这个托盘图标**实际用的是哪一份资源**：`tray.png`（正常）/ `app.ico`（托盘那份取不到）
        /// / `SystemIcons.Application`（都没取到 —— 兜底）。
        ///
        /// 自检第 ⑩ 节读它：报告里能看出"图标是不是真搬进来了"，
        /// 而不是靠肉眼看托盘（跑验收那台机器常是锁屏 / 远程会话，见项目规范）。
        /// </summary>
        internal string IconSource { get; private set; } = "SystemIcons.Application";

        internal bool Visible
        {
            get { return _notify.Visible; }
            set { _notify.Visible = value; }
        }

        /// <summary>取托盘图标：先 `assets/tray.png`，取不到退回 <see cref="LoadAppIcon"/></summary>
        private Icon LoadIcon()
        {
            var bytes = AssetBytes("assets/tray.png");
            if (bytes != null && bytes.Length > 0)
            {
                try
                {
                    using (var stream = new MemoryStream(bytes))
                    using (var bitmap = new Bitmap(stream))
                    {
                        _iconHandle = bitmap.GetHicon();
                        _icon = Icon.FromHandle(_iconHandle);
                        IconSource = "tray.png";
                        return _icon;
                    }
                }
                catch (Exception err)
                {
                    Console.Error.WriteLine("[tray] 托盘图标加载失败: " + err.Message);
                }
            }
            _icon = LoadAppIcon();
            IconSource = _icon == SystemIcons.Application ? "SystemIcons.Application" : "app.ico";
            return _icon;
        }

        /// <summary>
        /// 应用图标（管理窗 / 任务栏用），取自打包时嵌进来的多尺寸 ICO。
        /// **给 <see cref="ManagerWindow"/> 用** —— 与参考实现同一个用途。
        /// </summary>
        internal static Icon LoadAppIcon()
        {
            var bytes = AssetBytes("assets/app.ico");
            if (bytes != null && bytes.Length > 0)
            {
                try
                {
                    using (var stream = new MemoryStream(bytes))
                    {
                        // 让系统挑最合适的那一种情况（见类注释）
                        return new Icon(stream, SystemInformation.IconSize);
                    }
                }
                catch (Exception err)
                {
                    Console.Error.WriteLine("[tray] 应用图标加载失败: " + err.Message);
                }
            }
            return SystemIcons.Application;
        }

        /// <summary>
        /// 读一份**嵌进 exe** 的资源（名字就是 csproj 里 `LogicalName` 那个，如 `assets/tray.png`）。
        /// 取不到回 null —— 调用方按"这一份没有"处理，不抛。
        /// </summary>
        private static byte[] AssetBytes(string logicalName)
        {
            try
            {
                using (var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream(logicalName))
                {
                    if (stream == null) return null;
                    using (var buffer = new MemoryStream())
                    {
                        stream.CopyTo(buffer);
                        return buffer.ToArray();
                    }
                }
            }
            catch (Exception err)
            {
                Console.Error.WriteLine("[tray] 读不到资源 " + logicalName + "：" + err.Message);
                return null;
            }
        }

        public void Dispose()
        {
            try
            {
                _notify.Visible = false;
                _notify.Dispose();
            }
            catch (Exception) { }
            /*
             * ⚠️ `Icon.FromHandle` **不接管** HICON 的生命周期：不销毁就是一次 GDI 句柄泄漏
             *    （参考实现也在这里 DestroyIcon，见它那份 Dispose）。
             */
            if (_iconHandle != IntPtr.Zero)
            {
                DestroyIcon(_iconHandle);
                _iconHandle = IntPtr.Zero;
            }
            _icon = null;
        }

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool DestroyIcon(IntPtr handle);
    }
}
