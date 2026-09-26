namespace Lookup.App
{
    using System;
    using System.IO;
    using System.Threading.Tasks;
    using Microsoft.Web.WebView2.Core;
    using Microsoft.Web.WebView2.WinForms;

    /// <summary>
    /// **两个窗口共用的 WebView2 初始化**（悬浮窗与词库管理窗）—— 两处各写一份迟早会分叉，
    /// 而分叉的症状极难查（"词条页只解析出前 3920 字节"就是漏了 `--disable-site-isolation-trials`）。
    ///
    /// ⚠️ 三个浏览器开关非有不可：`--disable-site-isolation-trials`（站点隔离会把词条那个跨源
    ///    iframe 放进另一个进程 OOPIF，宿主的 `WebResourceRequested` **收不到跨进程 iframe 发的
    ///    请求** → 词条里的样式/图片/录音全取不到）；关掉 `CalculateNativeWinOcclusion` 与两个
    ///    backgrounding 开关（悬浮窗常被别的窗口盖住，Chromium 会据此把帧率与加载队列一起降下来）；
    ///    `--autoplay-policy=no-user-gesture-required`（词典自带录音要在没有用户手势时也能出声）。
    ///    这是**有意识的取舍**：内容只来自本进程内的应答，词条本身仍被 sandbox + CSP 关着。
    /// </summary>
    internal static class WebViewSetup
    {
        /// <summary>建好环境、把桥接上、导航到某一页；返回那个 `CoreWebView2`</summary>
        internal static async Task<CoreWebView2> StartAsync(WebView2 web, string configDir, string url,
                                                            Action<CoreWebView2> attach, int debugPort)
        {
            var userData = Path.Combine(configDir, "webview2");
            Directory.CreateDirectory(userData);
            var env = await CoreWebView2Environment.CreateAsync(null, userData, BrowserOptions(debugPort));
            await web.EnsureCoreWebView2Async(env);
            ApplySettings(web.CoreWebView2);
            if (attach != null) attach(web.CoreWebView2);

            var nav = WaitNavigationAsync(web.CoreWebView2);
            web.CoreWebView2.Navigate(url);
            var err = await nav;
            if (err != null)
            {
                throw new InvalidOperationException("页面打不开（" + url + "）：" + err);
            }
            return web.CoreWebView2;
        }

        /// <summary>
        /// 等一次导航走完，返回 null 表示成功。
        ///
        /// ⚠️ **必须是 await，绝不许在 UI 线程上 `Wait()`** —— 消息泵一停摆 `NavigationCompleted`
        ///    就永远送不到，症状是"连 NavigateToString 都超时"。
        /// </summary>
        internal static Task<string> WaitNavigationAsync(CoreWebView2 core)
        {
            var tcs = new TaskCompletionSource<string>();
            EventHandler<CoreWebView2NavigationCompletedEventArgs> handler = null;
            handler = (a, b) =>
            {
                core.NavigationCompleted -= handler;
                tcs.TrySetResult(b.IsSuccess ? null : b.WebErrorStatus.ToString());
            };
            core.NavigationCompleted += handler;
            return tcs.Task;
        }

        private static CoreWebView2EnvironmentOptions BrowserOptions(int debugPort)
        {
            /* DevTools 协议端口：只有显式给了 --debug-port 才开（脚本要接进来）*/
            var debug = (debugPort > 0) ? " --remote-debugging-port=" + debugPort : "";
            return new CoreWebView2EnvironmentOptions
            {
                AdditionalBrowserArguments =
                    "--disable-features=CalculateNativeWinOcclusion,IsolateSandboxedIframes " +
                    "--disable-site-isolation-trials " +
                    "--disable-background-timer-throttling " +
                    "--disable-renderer-backgrounding " +
                    "--disable-backgrounding-occluded-windows " +
                    "--autoplay-policy=no-user-gesture-required" + debug,
            };
        }

        /// <summary>
        /// `CoreWebView2Settings` —— 逐条照抄参考实现（参考实现 `ConfigureSettings`）。
        /// `IsBuiltInErrorPageEnabled = false` 关掉的是 `chrome-error://chromewebdata/` 那张
        /// 内置错误页（导航失败时会出现它，而它**不可读**）。
        /// </summary>
        private static void ApplySettings(CoreWebView2 core)
        {
            var settings = core.Settings;
            settings.IsWebMessageEnabled = true;
            settings.AreDefaultScriptDialogsEnabled = false;
            settings.IsStatusBarEnabled = false;
            settings.AreDefaultContextMenusEnabled = false; // 右键菜单由渲染进程自绘
            settings.IsZoomControlEnabled = false;
            settings.IsBuiltInErrorPageEnabled = false;
            settings.IsSwipeNavigationEnabled = false;
            settings.IsPinchZoomEnabled = false;
            settings.AreBrowserAcceleratorKeysEnabled = false;
            try
            {
                settings.IsGeneralAutofillEnabled = false;
                settings.IsPasswordAutosaveEnabled = false;
            }
            catch (Exception) { /* 旧版运行时没有这两个属性 */ }
        }
    }
}
