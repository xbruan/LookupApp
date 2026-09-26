namespace Lookup.App
{
    using System;
    using System.Collections.Generic;
    using System.IO;
    using System.Runtime.InteropServices;
    using System.Text;
    using System.Windows.Forms;
    using Lookup.Interop;

    /// <summary>
    /// 0.2.0 壳的入口。正常启动 `Lookup.App.exe`；自检 `--selfcheck --web &lt;web 目录&gt;
    /// --config &lt;临时目录&gt; --dict &lt;a.mdx&gt;`（退出码：0 = 全过，1 = 断言失败，2 = 用法 / 环境不对）。
    /// ⚠️ 自检**必须**给 `--config` 一个临时目录 —— 内核把设置写在配置目录里，绝不许碰用户真正的
    ///    `%APPDATA%\LookupApp`（旧名 `%APPDATA%\查词`）。
    /// ⚠️ 这里是 `WinExe`，自检**自己接控制台**（`AttachConsole(ATTACH_PARENT_PROCESS)`，借不到就
    ///    `AllocConsole`）：少了它，自检全绿却一行实测结果都看不到。
    /// </summary>
    internal static class Program
    {
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool AttachConsole(int processId);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool AllocConsole();

        private const int AttachParentProcess = -1;

        [STAThread]
        private static int Main(string[] args)
        {
            /* ⚠️ 这两句必须在**任何窗口 / 任何句柄**之前 —— 它们管的是进程级设置：
             * `TryEnablePerMonitorV2()` 是 `app.manifest` 里 `PerMonitorV2` 的兜底；少了它进程被判为
             * **DPI 不感知**，Windows 把窗口连 WebView2 里的文字一起按位图放大（发虚），几何也全错。
             * `EnableVisualStyles()` 管 Common-Controls 视觉样式（参考实现有，不许漏）。 */
            Native.TryEnablePerMonitorV2();
            Application.EnableVisualStyles();

            AppOptions opts;
            try
            {
                opts = AppOptions.Parse(args);
            }
            catch (Exception err)
            {
                AttachConsoleIfPossible();
                Console.Error.WriteLine("参数不对：" + err.Message);
                return 2;
            }

            if (opts.SelfCheck) return RunSelfCheck(opts);
            return RunWindow(opts);
        }

        private static void AttachConsoleIfPossible()
        {
            if (!AttachConsole(AttachParentProcess)) AllocConsole();
        }

        /// <summary>正常启动时 `--dict` 那一次导入的结果（`{"added":N,"failed":[…],"error":"…"}`）；
        /// 诊断读它 —— 命令行给了词典、用户却看到空词库时，这句话就是现场。</summary>
        internal static string StartupDictResult;

        /* ── 正常启动 ───────────────────────────────────────────────────────── */

        private static int RunWindow(AppOptions opts)
        {
            IntPtr engine;
            try
            {
                Dsh.EngineCreate(opts.ConfigDir, out engine);
            }
            catch (Exception err)
            {
                MessageBox.Show("内核起不来：" + err.Message, "查词",
                                MessageBoxButtons.OK, MessageBoxIcon.Error);
                return 1;
            }

            /* `--dict` 在正常启动时也要**真的加进去**（不许像原来那样被不报错地忽略）。
             * ⚠️ "加了之后谁是当前"**不由这里决定** —— 内核在"还没有当前词典"时自己把新加的这本
             * 设成当前。这一层只如实报加了几本、哪几本没加成（`failed` 里带原因），
             * 失败了也不拦着用户开窗口（他还能在设置界面里手动加）。 */
            if (opts.DictPaths.Count > 0)
            {
                try
                {
                    string added;
                    Dsh.EngineDictAdd(engine, JsonArray(opts.DictPaths), out added);
                    StartupDictResult = added;
                }
                catch (Exception err)
                {
                    StartupDictResult = "{\"added\":0,\"failed\":[],\"error\":\"" + err.Message + "\"}";
                }
            }

            try
            {
                var win = new FloatingWindow(engine, opts);
                /* 词库管理窗：**懒建**（自检那一种情况不建它）。两个静态挂钩把它们接起来：
                 *   · `win.Manager` —— 悬浮窗页面点「管理」时打开哪一扇；
                 *   · `ManagerWindow.AppHost` —— 管理窗要"选文件 / 剪贴板 / 退程序"时找谁（那些是
                 *     **应用**的能力，不属于任何一扇窗）。 */
                /* ⚠️ **不许按 `--no-show` 跳过建这两扇窗**：它们都是**懒**的（管理窗的 WebView2 第一次
                 * `manager:open` 才起、托盘菜单第一次弹才起），建对象本身不显示任何东西 ——
                 * 跳过去就等于"管理窗与托盘菜单任何一级测试都覆盖不到"。 */
                if (true)
                {
                    win.Manager = new ManagerWindow(engine, opts);
                    ManagerWindow.AppHost = win;
                    /* 托盘菜单与托盘图标：**懒建**（菜单窗的 WebView2 第一次弹出来才起）。
                     * 鼠标分工：**右键单击 → 弹菜单**，**左键双击 → 呼出胶囊**（`FloatingWindow.TraySummonFloating`，
                     * **不是**那个开关），左键单击什么都不做。菜单是自绘窗口，不走 NotifyIcon / ContextMenuStrip。 */
                    win.Tray = new TrayMenuWindow(engine, opts);
                    win.TrayIcon = new TrayIcon(win.TrayMenuShow, win.TraySummonFloating);
                }
                win.Ready.ContinueWith(t =>
                {
                    if (!t.IsFaulted) return;
                    var why = t.Exception == null ? "（没有原因）" : t.Exception.GetBaseException().Message;
                    MessageBox.Show(why, "查词", MessageBoxButtons.OK, MessageBoxIcon.Error);
                }, System.Threading.Tasks.TaskScheduler.FromCurrentSynchronizationContext());
                Application.Run(win);
                return 0;
            }
            finally
            {
                Dsh.EngineDestroy(engine);
            }
        }

        /* ── 自检（"真实程序"那道 检查）─────────────────────────────────────── */

        private static int RunSelfCheck(AppOptions opts)
        {
            SelfCheck.Out = new Report(opts.ReportPath);
            var say = new Action<string>(SelfCheck.Out.Line);
            try { Console.OutputEncoding = Encoding.UTF8; }
            catch (Exception) { /* 没有控制台就算了，报告文件那份是主要的 */ }

            say("Lookup.App 自检");
            say(opts.Describe());
            say("");

            if (opts.DictPaths.Count == 0 && string.IsNullOrEmpty(opts.SettingsSeed))
            {
                say("[!!] 自检要先给至少一本词典：--dict <x.mdx>（或者给一份 --settings 种子）");
                SelfCheck.Out.Close();
                return 2;
            }
            if (string.IsNullOrEmpty(opts.WebRoot))
            {
                say("[!!] 没找到外壳资源目录（要么用 --web 指，要么让 exe 上面几层里有 web/floating.html）");
                SelfCheck.Out.Close();
                return 2;
            }

            /* `--settings` 种子：把一份手写的 `settings.json` 先放进配置目录、**再**建引擎。
             * ⚠️ 必须在 `EngineCreate` **之前**落盘 —— 引擎是在建的当口读它的。
             * 造"路径是假的词典"只有这个入口：`engine.dictAdd` 会**正确地**挡下不存在的路径。 */
            if (!string.IsNullOrEmpty(opts.SettingsSeed))
            {
                try
                {
                    Directory.CreateDirectory(opts.ConfigDir);
                    File.Copy(opts.SettingsSeed,
                              Path.Combine(opts.ConfigDir, "settings.json"), true);
                    say("  已放入设置种子: " + opts.SettingsSeed);
                }
                catch (Exception err)
                {
                    say("[!!] 设置种子放不进去：" + err.Message);
                    SelfCheck.Out.Close();
                    return 1;
                }
            }

            IntPtr engine;
            try
            {
                Dsh.EngineCreate(opts.ConfigDir, out engine);
            }
            catch (Exception err)
            {
                say("[!!] 内核起不来：" + err.Message);
                SelfCheck.Out.Close();
                return 1;
            }

            var rc = 1;
            try
            {
                if (opts.DictPaths.Count > 0)
                {
                    var pathsJson = JsonArray(opts.DictPaths);
                    string added;
                    Dsh.EngineDictAdd(engine, pathsJson, out added);
                    say("  dict_add: " + added);
                    if (added == null || !added.Contains("\"added\":" + opts.DictPaths.Count))
                    {
                        say("[!!] 词典一本都没进去，自检没有意义");
                        return 1;
                    }
                }

                string list;
                Dsh.EngineDictList(engine, out list);
                /* 种子那条路要**三**本：test.mdx + audio.mdx + 一本路径是假的（用来造"没问完"） */
                var wantIds = string.IsNullOrEmpty(opts.SettingsSeed) ? opts.DictPaths.Count : 3;
                var ids = AllIds(list, wantIds);
                if (ids.Count < wantIds)
                {
                    say("[!!] 词库里取不到全部 id（拿到 " + ids.Count + " 个，要 " + wantIds + " 个）");
                    return 1;
                }
                /* ⚠️ 两本的分工是**断言**要求的，不是随便挑的：第一本（test.mdx）验"输入框回车 → 词条"；
                 * 第二本（audio.mdx + audio.mdd）验资源那条路的 Range —— `test.mdx` 没有 `.mdd`，
                 * 资源路由在它身上根本走不到。少给一本下面会当场报出来，不许不报错地少验两条。 */
                if (ids.Count < 2)
                {
                    say("[!!] 自检要两本测试用词典：--dict test.mdx --dict audio.mdx（第二本用来验资源路由）");
                    return 2;
                }
                var dictId = ids[0];
                var audioId = ids[1];
                Dsh.EngineDictSetCurrent(engine, dictId, out _);
                say("  当前词典: " + dictId);
                say("  资源词典: " + audioId);
                say("");

                var win = new FloatingWindow(engine, opts);
                /* 词库管理窗：**懒建**（自检那一种情况不建它）。两个静态挂钩把它们接起来：
                 *   · `win.Manager` —— 悬浮窗页面点「管理」时打开哪一扇；
                 *   · `ManagerWindow.AppHost` —— 管理窗要"选文件 / 剪贴板 / 退程序"时找谁（那些是
                 *     **应用**的能力，不属于任何一扇窗）。 */
                /* ⚠️ **不许按 `--no-show` 跳过建这两扇窗**：它们都是**懒**的（管理窗的 WebView2 第一次
                 * `manager:open` 才起、托盘菜单第一次弹才起），建对象本身不显示任何东西 ——
                 * 跳过去就等于"管理窗与托盘菜单任何一级测试都覆盖不到"。 */
                if (true)
                {
                    win.Manager = new ManagerWindow(engine, opts);
                    ManagerWindow.AppHost = win;
                    /* 托盘菜单与托盘图标：**懒建**（菜单窗的 WebView2 第一次弹出来才起）。
                     * 鼠标分工：**右键单击 → 弹菜单**，**左键双击 → 呼出胶囊**（`FloatingWindow.TraySummonFloating`，
                     * **不是**那个开关），左键单击什么都不做。菜单是自绘窗口，不走 NotifyIcon / ContextMenuStrip。 */
                    win.Tray = new TrayMenuWindow(engine, opts);
                    win.TrayIcon = new TrayIcon(win.TrayMenuShow, win.TraySummonFloating);
                }
                var done = false;
                var result = 1;
                win.Shown += async (s, e) =>
                {
                    try
                    {
                        result = await SelfCheck.RunAsync(win, dictId, audioId,
                                                          !string.IsNullOrEmpty(opts.SettingsSeed),
                                                          opts.MtKey);
                    }
                    catch (Exception err)
                    {
                        say("  [!!] 自检自己出错：" + err);
                        result = 1;
                    }
                    done = true;
                    win.Close();
                };
                Application.Run(win);
                rc = done ? result : 1;
            }
            catch (Exception err)
            {
                say("[!!] 自检出错：" + err);
                rc = 1;
            }
            finally
            {
                Dsh.EngineDestroy(engine);
                SelfCheck.Out.Close();
            }
            return rc;
        }

        private static string JsonArray(IList<string> paths)
        {
            var sb = new StringBuilder("[");
            for (var i = 0; i < paths.Count; i++)
            {
                if (i > 0) sb.Append(',');
                sb.Append('"').Append(paths[i].Replace("\\", "\\\\").Replace("\"", "\\\"")).Append('"');
            }
            sb.Append(']');
            return sb.ToString();
        }

        /// <summary>词库清单 JSON 里的 id 清单（**按加入顺序** —— 断言认这个顺序）</summary>
        private static List<string> AllIds(string list, int expected)
        {
            var ids = new List<string>();
            if (list == null) return ids;
            var at = -1;
            while (ids.Count < expected)
            {
                at = list.IndexOf("\"id\":\"", at + 1, StringComparison.Ordinal);
                if (at < 0) break;
                at += 6;
                var end = list.IndexOf('"', at);
                if (end <= at) break;
                ids.Add(list.Substring(at, end - at));
            }
            return ids;
        }
    }
}
