namespace Lookup.App
{
    using System;
    using Microsoft.Win32;

    /// <summary>
    /// **开机自启动** —— 参考实现的 `src/Startup.cs` 搬过来的那一套。登记位置是
    /// `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`（只动当前用户、不需要管理员权限），
    /// 值名沿用产品名（便于用户在"任务管理器 → 启动"里认出来）。
    ///
    /// ⚠️ 两条照抄的细节：写进去的命令行**带 `--autostart`**，而 0.2.0 的壳不消费这个标记、
    ///    `AppOptions.Parse` 只是**接受**它 —— **别删那个空 case，否则自启动必挂**；
    ///    旧 Electron 版留下的条目名（`lookup`）**关掉时要一起清**，否则用户关了开关、
    ///    下次开机还是被旧条目拉起来。
    /// </summary>
    internal static class Startup
    {
        private const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
        private const string ValueName = "查词";
        private const string LegacyValueName = "lookup";
        private const string Argument = "--autostart";

        internal static bool IsLoginAtStartup()
        {
            try
            {
                using (var key = Registry.CurrentUser.OpenSubKey(RunKey, false))
                {
                    var value = (key == null) ? null : key.GetValue(ValueName) as string;
                    return !string.IsNullOrWhiteSpace(value) &&
                           value.IndexOf(ExecutablePath(), StringComparison.OrdinalIgnoreCase) >= 0;
                }
            }
            catch (Exception)
            {
                return false;
            }
        }

        /// <summary>写完之后把**系统里的真实状态**回给调用方（写不进去时它不是我们说了算）</summary>
        internal static bool SetLoginAtStartup(bool enabled)
        {
            try
            {
                using (var key = Registry.CurrentUser.CreateSubKey(RunKey))
                {
                    if (key == null) return IsLoginAtStartup();
                    if (enabled)
                    {
                        key.SetValue(ValueName, "\"" + ExecutablePath() + "\" " + Argument);
                    }
                    else
                    {
                        key.DeleteValue(ValueName, false);
                        key.DeleteValue(LegacyValueName, false);
                    }
                }
            }
            catch (Exception)
            {
                /* 读不到/写不了就如实回真实状态，不假装成功 */
            }
            return IsLoginAtStartup();
        }

        private static string ExecutablePath()
        {
            try
            {
                return System.Reflection.Assembly.GetEntryAssembly().Location;
            }
            catch (Exception)
            {
                return AppDomain.CurrentDomain.BaseDirectory + "Lookup.App.exe";
            }
        }
    }
}
