namespace Lookup.App
{
    using System;
    using System.Collections.Generic;
    using System.IO;
    using System.Text;
    using System.Threading;
    using Lookup.Host;
    using Microsoft.Web.WebView2.Core;

    /// <summary>
    /// **WebView2 ↔ 宿主适配层**：只做两件事 —— `WebResourceRequested` → `VirtualHost.Serve`、
    /// `WebMessageReceived` → `Dispatch.Handle`。**一行路由判断都不许有**（不解析 URL、不看方法名）：
    /// 一旦出现 `if (url.Contains("__sound__"))`，壳里就长出了第二份业务约定。
    /// ⚠️ 只拦 `lookup.local` 与 `&lt;词典 id&gt;.dictres.invalid` 两个虚拟站点，其余 http/https 一律 `403`
    /// （记在 <see cref="BlockedUrls"/> 里）—— 理由是**信任边界**：词条正文是外部内容，放行它里面
    /// 一个 `&lt;img src="https://某处/像素.png"&gt;` 就等于把"用户查了什么词"发出去；非 http(s) 的
    /// （`about:` / `data:` / `blob:`）**一律不碰**，拦它们只会把播放器和内联资源弄坏。
    /// 三条纪律：① 字节不过桥（桥只走文本，字节走 URL）；② 绝不让异常过边界（抛进 WebView2 的
    /// 事件回调就是整个渲染进程崩掉）；③ 不猜、不编（`VirtualResponse.Reason` 是内核给的人话，原样发）。
    /// </summary>
    internal sealed class WebViewBridge
    {
        private readonly IntPtr _engine;
        private readonly ShellAssetSource _shell;
        /// <summary>页面做不了、只有窗口做得了的那两件事（挑文件 / 设置变了要重装热键）</summary>
        private readonly IShellHost _shellHost;
        /* ── 回包口要用的两样（`Attach` 时抓；见 `PostReply`）──────────────────────── */
        /// <summary>接上的那个 core（回包用；导航换页时不会变）</summary>
        private CoreWebView2 _core;
        /// <summary>UI 线程的同步上下文（后台线程回包时用来切回去）</summary>
        private SynchronizationContext _uiContext;

        /* ── 现场实测结果（自检那道 检查 读它们；全部只在 UI 线程上改，所以不需要锁）──────── */
        internal int EntryRequests;        // 走 `/__entry__` 几次
        internal int EntryStatus;          // 最后一次给的什么状态码
        internal long EntryBytes;          // 最后一次给出去多少字节
        internal int ResourceRequests;     // 走资源路由（含 `/__sound__/`）几次
        internal int ResourceStatus;       // 最后一次资源应答的状态码（206 / 200 / 404…）
        internal long ResourceBytes;       // 最后一次资源应答给了多少字节
        internal long ResourceTotal;       // 最后一次资源应答报的**总长**（206 的 `Content-Range` 里那个数）
        /// <summary>最后一次资源请求的**路径**（`/__sound__/beep__gb_1.wav` 这种）。
        /// 断言"要的是哪一条录音"用它 —— 只看计数只能知道"要过东西"。</summary>
        internal string LastResourcePath;
        internal int RangeRequests;        // 带 `Range` 头几次
        /// <summary>
        /// **每一次**虚拟主机应答的现场（`方法 地址 → 状态 / 字节数`，按先后顺序）：
        /// "最后一次"会被后面几节覆盖掉，只有**逐次的流水**答得了"某条请求到底发出去过没有"。
        /// </summary>
        internal readonly List<string> RequestLog = new List<string>();
        internal int RequestsSeen;         // 一共进来过几次（不被 16 条上限影响）
        /// <summary>子框架每一次导航的成败（`WebErrorStatus` 会说是哪一类失败）</summary>
        internal readonly List<string> FrameNavigations = new List<string>();
        internal int BlockedRequests;      // 不属于两个虚拟站点、被挡回去几次
        internal readonly List<string> BlockedUrls = new List<string>();
        internal readonly List<string> Failures = new List<string>();

        internal WebViewBridge(IntPtr engine, string webRoot, IShellHost shellHost)
        {
            _engine = engine;
            _shellHost = shellHost ?? NoShellHost.Instance;
            _shell = ShellAssetSource.FromDirectory(webRoot);
        }

        internal ShellAssetSource Shell { get { return _shell; } }

        /// <summary>把桥接到一个已经起来的 CoreWebView2 上（**必须在 UI 线程上叫**）</summary>
        internal void Attach(CoreWebView2 core)
        {
            /* 记下 UI 线程的同步上下文与那个 core：回包口（`PostReply`）要能在**任意线程**上叫 ——
             * 要联网那几条路的后台线程做完 HTTP 之后就是从这个口子回的。 */
            _core = core;
            _uiContext = SynchronizationContext.Current;
            core.AddWebResourceRequestedFilter("*", CoreWebView2WebResourceContext.All);
            core.WebResourceRequested += OnResourceRequested;
            core.WebMessageReceived += OnWebMessage;
            /* 子框架（词条那一页住在 `#reader` 这个**跨源 iframe** 里）。
             * ⚠️ **必须在这里订阅**（= 导航之前）：页面一加载 WebView2 就为它建好了框架、`FrameCreated`
             *    也就发过了 —— 等自检开始再订阅只会拿到 0 个。宿主的 `ExecuteScriptAsync` 只跑在
             *    **主框架**上、够不着词条页，要驱动它只能靠这里的框架句柄。 */
            core.FrameCreated += (s, e) =>
            {
                Frames.Add(e.Frame);
                /* 子框架**每一次导航的成败**也记下来：`ExecuteScriptAsync` 只能告诉我们"框架里的地址
                 * 是个错误页"，错误页里的地址读不出原因，而 `WebErrorStatus` 会当场说是哪一类
                 * （`ConnectionAborted` / `BlockedByResponse` / `OperationCanceled`…）。 */
                try
                {
                    var pending = "";
                    e.Frame.NavigationStarting += (fs, fe) => { pending = fe.Uri; };
                    e.Frame.NavigationCompleted += (fs, fe) =>
                    {
                        if (FrameNavigations.Count < 24)
                        {
                            FrameNavigations.Add((pending.Length > 0 ? pending : "(地址没记到)") +
                                                 " → " +
                                                 (fe.IsSuccess ? "成功" : "失败 " + fe.WebErrorStatus));
                        }
                    };
                }
                catch (Exception err) { Note("订不上框架导航事件：" + err.Message); }
            };
        }

        /// <summary>见过的子框架（按创建顺序；失效的那些调用时会抛，调用方自己接住）</summary>
        internal readonly List<CoreWebView2Frame> Frames = new List<CoreWebView2Frame>();

        private void OnResourceRequested(object sender, CoreWebView2WebResourceRequestedEventArgs e)
        {
            var core = (CoreWebView2)sender;
            try
            {
                var uri = e.Request.Uri;
                Uri parsed;
                if (!Uri.TryCreate(uri, UriKind.Absolute, out parsed) ||
                    !(parsed.Scheme == Uri.UriSchemeHttp || parsed.Scheme == Uri.UriSchemeHttps))
                {
                    return; // 非 http(s)：不碰，交回 WebView2
                }

                string dictId = null, path = null, query = null;
                var isShell = VirtualHost.IsShellUrl(uri, out var shellPath);
                var isDict = !isShell && VirtualHost.TryDictId(uri, out dictId, out path, out query);
                if (!isShell && !isDict)
                {
                    BlockedRequests++;
                    if (BlockedUrls.Count < 16) BlockedUrls.Add(uri);
                    Answer(core, e, new VirtualResponse
                    {
                        Status = 403,
                        ReasonPhrase = "Forbidden",
                        Reason = "这个地址不属于外壳或词典资源域，壳不放行",
                        Body = Encoding.UTF8.GetBytes("这个地址不属于外壳或词典资源域，壳不放行"),
                    });
                    return;
                }

                var req = new VirtualRequest
                {
                    Url = uri,
                    Method = e.Request.Method,
                    Range = HeaderOrNull(e.Request.Headers, "Range"),
                    IfNoneMatch = HeaderOrNull(e.Request.Headers, "If-None-Match"),
                };
                if (!string.IsNullOrEmpty(req.Range)) RangeRequests++;
                if (isDict)
                {
                    if (path == VirtualHost.EntryPath) EntryRequests++;
                    else { ResourceRequests++; LastResourcePath = path; }
                }

                var res = VirtualHost.Serve(_engine, req, _shell);
                RequestsSeen++;
                if (RequestLog.Count < 24)
                {
                    RequestLog.Add(e.Request.Method + " " + uri + " → " + res.Status + " / " +
                                   (res.Body == null ? 0 : res.Body.Length) + " B");
                }
                if (isDict && path == VirtualHost.EntryPath)
                {
                    EntryStatus = res.Status;
                    EntryBytes = res.Body == null ? 0 : res.Body.Length;
                    /* 逐次那一份在 `RequestLog` 里（它本来就分得清"iframe 那次导航"与"页面 fetch 那一次"）*/
                }
                else if (isDict)
                {
                    ResourceStatus = res.Status;
                    ResourceBytes = res.Body == null ? 0 : res.Body.Length;
                    ResourceTotal = res.Total;
                }
                if (!res.IsOk && res.Status != 304 && res.Status != 404)
                {
                    Note("资源请求 " + uri + " 回了 " + res.Status + "：" + res.Reason);
                }
                Answer(core, e, res);
            }
            catch (Exception err)
            {
                // 绝不让异常抛回 WebView2 —— 那是一个渲染进程级别的崩溃
                Note("处理资源请求时出错：" + err.Message);
                try
                {
                    Answer(core, e, new VirtualResponse
                    {
                        Status = 500,
                        ReasonPhrase = "Internal Error",
                        Reason = err.Message,
                        Body = Encoding.UTF8.GetBytes(err.Message),
                    });
                }
                catch (Exception) { /* 已经没什么能做的了 */ }
            }
        }

        private void OnWebMessage(object sender, CoreWebView2WebMessageReceivedEventArgs e)
        {
            var core = (CoreWebView2)sender;
            string reply;
            try
            {
                /* ★ 第四个参数是"**稍后自己回包**"的口子：要联网的那几条路（机器翻译 / 在线发音 /
                 * 检测凭据）会把 HTTP 丢到线程池、回 UI 线程再回包 —— **不许在这个 UI 线程上同步发
                 * HTTP**（最长 8 秒界面不动），所以这个口子**必须是线程安全的**（见 `PostReply`）。 */
                reply = Dispatch.Handle(_engine, e.WebMessageAsJson, _shellHost, PostReply);
            }
            catch (Exception err)
            {
                // Dispatch 自己已经兜过一层；这里再兜一层是给"连 Dispatch 都没进去"那种情况
                reply = "{\"id\":0,\"ok\":false,\"error\":{\"message\":" + Dispatch.Quote(err.Message) + "}}";
            }

            /* null = **这一条不该回包**，两种情形都走这里：① `reply:false`（页面的 `send()`）与
             * `kind:'notify'` —— 页面根本不等人回话；② **路由接管了**（要联网那条路自己会在后台
             * 做完之后调 `PostReply`）。 */
            if (reply == null) return;

            PostReplyNow(reply);
        }

        /// <summary>
        /// **回包口**：页面能收到，并且**哪个线程调都行**。
        /// ⚠️ `PostWebMessageAsJson` 只能在 UI 线程发，所以这里不是 UI 线程就 `Post` 回去 ——
        ///    用的是 `Attach` 时抓的那个 `SynchronizationContext`（那时确实在 UI 线程上）。
        /// </summary>
        private void PostReply(string json)
        {
            var ctx = _uiContext;
            if (ctx != null && SynchronizationContext.Current != ctx)
            {
                ctx.Post(delegate { PostReplyNow(json); }, null);
                return;
            }
            PostReplyNow(json);
        }

        private void PostReplyNow(string json)
        {
            try
            {
                var core = _core;
                if (core == null) return;   /* 还没接上（`Attach` 之前）：没有页面可回，丢掉 */
                core.PostWebMessageAsJson(json);
            }
            catch (Exception err)
            {
                Note("回不了页面：" + err.Message);
            }
        }

        private static void Answer(CoreWebView2 core, CoreWebView2WebResourceRequestedEventArgs e,
                                   VirtualResponse res)
        {
            var body = (res.Body != null && res.Body.Length > 0) ? new MemoryStream(res.Body) : null;
            e.Response = core.Environment.CreateWebResourceResponse(
                body, res.Status, res.ReasonPhrase, HeaderText(res));
        }

        private static string HeaderText(VirtualResponse res)
        {
            var sb = new StringBuilder();
            if (res.Body != null && res.Body.Length > 0 && !string.IsNullOrEmpty(res.ContentType))
            {
                sb.Append("Content-Type: ").Append(res.ContentType).Append("\r\n");
            }
            foreach (var h in res.Headers) sb.Append(h).Append("\r\n");
            return sb.ToString();
        }

        /// <summary>
        /// 取一个请求头（没有就回 null）。
        /// ⚠️ `CoreWebView2HttpRequestHeaders` 上**没有** `TryGetHeader` —— 只有 `Contains` + `GetHeader`，
        ///    而 `GetHeader` 对不存在的名字会抛，所以必须先 `Contains`。
        /// </summary>
        private static string HeaderOrNull(CoreWebView2HttpRequestHeaders headers, string name)
        {
            return headers.Contains(name) ? headers.GetHeader(name) : null;
        }

        /// <summary>记一条"宿主这一侧出的问题"（自检末尾会把它们打出来）。
        internal void Note(string text)
        {
            if (Failures.Count < 32) Failures.Add(text);
        }
    }
}
