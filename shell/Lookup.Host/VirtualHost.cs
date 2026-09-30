namespace Lookup.Host
{
    using System;
    using System.Collections.Generic;
    using System.Globalization;
    using System.Text;
    using Lookup.Interop;

    /// <summary>一次虚拟请求 —— 宿主把各家 WebView 的请求**归一化**成这个形状。</summary>
    internal sealed class VirtualRequest
    {
        public string Url = "";
        public string Method = "GET";
        /// <summary>`Range` 头（可空）</summary>
        public string Range;
        /// <summary>`If-None-Match` 头（可空）</summary>
        public string IfNoneMatch;
    }

    /// <summary>
    /// 一次虚拟应答 —— **平台无关**：宿主再把它翻成各平台自己的响应类型。这么切是因为
    /// 「状态码怎么发」是平台的事，而「这段字节从哪来、总共多长、身份证是什么」是产品约定（内核给的）。
    /// </summary>
    internal sealed class VirtualResponse
    {
        public int Status = 200;
        public string ReasonPhrase = "OK";
        public byte[] Body;                       // 可空
        public string ContentType = "text/plain; charset=utf-8";
        public string ETag;                       // 可空
        public long Total;                        // 资源总长（206 / 416 用）
        public string Reason = "";                // 人话（诊断；也进 404 的正文）
        public readonly List<string> Headers = new List<string>();

        public bool IsOk { get { return Status >= 200 && Status < 300; } }
    }

    /// <summary>
    /// **虚拟资源主机**：把词条正文里 `https://&lt;词典 id&gt;.dictres.invalid/…` 的请求接到内核的
    /// `resource` / `entry_document` / `audio_prepare` 上（外壳站点 `lookup.invalid` 下另有 `__speech__`）。
    /// `.invalid` 是 RFC 2606 保留的**永不解析**顶级域；词典 id 放在 host 位置，于是词条里
    /// `/images/a.png` 这类根相对路径经 `&lt;base&gt;` 解析后仍落在**同一本词典**上，请求永不离开进程。
    /// 三条路由：`/__entry__?word=…` → `entry_document`（动态生成，**不参与**协商缓存）；
    /// `/__sound__/&lt;键名&gt;` → 取录音字节 → `audio_prepare`；其余一律当资源 → `resource`。
    /// ⚠️ **Range 的字节由内核切**（`dsh_engine_resource` 收 offset/length），这里只负责
    /// `206 / 416 / 304` 这些 HTTP 语义 —— 切错了播放器与字体都会莫名坏掉。
    /// </summary>
    internal static class VirtualHost
    {
        internal const string ResourceDomain = "dictres.invalid";
        /// <summary>
        /// 外壳自己的虚拟站点名（宿主把这些页面当网址交给 WebView2）。
        ///
        /// ★ **不许用 `.local` 结尾的名字**（历史上是 `lookup.local`）：`.local` 是 mDNS / LLMNR 的
        ///   保留名，解析要**等超时**才失败（本机实测 `Resolve-DnsName lookup.local` = 3312 ms、
        ///   `Dns.GetHostEntry('lookup.local')` = 2743 ms），而 WebView2 **每新建一个页面都要过一遍主机解析** ——
        ///   于是"启动后那两秒的空窗"与"第一次弹托盘菜单要等 2 秒"就是这么来的。换成 `.invalid`
        ///   之后同一台机器实测：页面 `domInteractive` 2043 ms → **101 ms**、首次弹托盘菜单
        ///   2131 ms → **220 ms**。
        ///   `.invalid` 是 RFC 2606 保留顶级域，**保证**解析立即失败（本机 52~70 ms），
        ///   与词典资源域 <see cref="ResourceDomain"/> 用的是同一种保留 TLD —— 两处一致才不会再踩。
        /// </summary>
        internal const string ShellDomain = "lookup.invalid";
        internal const string SoundRoute = "/__sound__/";
        internal const string EntryPath = "/__entry__";

        /// <summary>
        /// 宿主**合成**出来的语音走这条路由（`https://lookup.invalid/__speech__/&lt;键&gt;`）——
        /// 桥那条通道的纪律是**字节不过桥**，音频一律由宿主发；而这一段不属于任何一本词典，
        /// 所以挂在外壳自己的站点下，与 `*.dictres.invalid` 分开。
        /// </summary>
        internal const string SpeechRoute = "/__speech__/";

        /// <summary>这个 URL 是不是外壳自己的站点（`https://lookup.invalid/…`）</summary>
        internal static bool IsShellUrl(string url, out string path)
        {
            path = null;
            if (string.IsNullOrEmpty(url)) return false;
            Uri uri;
            if (!Uri.TryCreate(url, UriKind.Absolute, out uri)) return false;
            if (!string.Equals(uri.Scheme, "https", StringComparison.OrdinalIgnoreCase)) return false;
            if (!string.Equals(uri.Host, ShellDomain, StringComparison.OrdinalIgnoreCase)) return false;
            path = uri.AbsolutePath;
            return true;
        }

        /// <summary>
        /// 这个 URL 是不是词典资源请求；是的话拆出词典 id / 路径 / 查询串。`lookup.invalid`（外壳自己的
        /// 虚拟站点）**不算** —— 那是宿主自己的静态资源，归它自己发；这里只回一个明确的 404，
        /// 免得「外壳页面取不到」被误当成内核坏了。
        /// </summary>
        internal static bool TryDictId(string url, out string dictId, out string path, out string query)
        {
            dictId = null;
            path = null;
            query = null;
            if (string.IsNullOrEmpty(url)) return false;

            Uri uri;
            if (!Uri.TryCreate(url, UriKind.Absolute, out uri)) return false;
            if (!string.Equals(uri.Scheme, "https", StringComparison.OrdinalIgnoreCase)) return false;

            var suffix = "." + ResourceDomain;
            var host = uri.Host;
            if (!host.EndsWith(suffix, StringComparison.OrdinalIgnoreCase)) return false;

            var id = host.Substring(0, host.Length - suffix.Length);
            if (id.Length == 0 || id.IndexOf('.') >= 0) return false;   // 只许一层子域

            dictId = id;
            path = Uri.UnescapeDataString(uri.AbsolutePath);
            query = uri.Query;
            return true;
        }

        /// <summary>处理一次请求（`engine` 是内核引擎句柄；失败一律如实写进 `Reason`）</summary>
        internal static VirtualResponse Serve(IntPtr engine, VirtualRequest req)
        {
            return Serve(engine, req, null);
        }

        /// <summary>
        /// 处理一次请求。两个虚拟站点分工明确：`*.dictres.invalid` 发**词典**的资源（走内核），
        /// `lookup.invalid` 发**外壳自己**的页面（走 <paramref name="shell"/>）。这么分是**信任边界**：
        /// 外壳的资源是我们自己发的、可以放行脚本；词典的资源是外部内容、只放行被动资源。
        /// </summary>
        internal static VirtualResponse Serve(IntPtr engine, VirtualRequest req,
                                              ShellAssetSource shell)
        {
            // 外壳自己的站点：先判它，免得 `lookup.invalid` 被当成「不是词典域」的 404
            if (req != null && IsShellUrl(req.Url, out var shellPath))
            {
                // ⚠️ 合成的语音**也**挂在 `lookup.invalid` 下，所以这一条必须排在「把路径交给外壳静态
                //    资源」**之前** —— 否则它会被当成不存在的静态文件去 404（「点了朗读没声音」）。
                if (shellPath.StartsWith(SpeechRoute, StringComparison.Ordinal))
                {
                    return ServeSpeech(shellPath.Substring(SpeechRoute.Length));
                }
                // ⚠️ **词典自带的原录音在外壳站点上还有第二个出口**：
                //      `https://lookup.invalid/__sound__/<词典 id>/<mdd 键名>`
                // 外壳页面的 CSP 是 `default-src 'self'` —— 它取不到也放不了跨域的 dictres 录音，
                // 所以同一条录音必须有两个出口：宿主页走这条，词条正文（跨源 iframe）走词典域那条；
                // 两条落到**同一段实现**，检查标准只有一份。
                // ⚠️ 这一条同样必须排在「把路径当静态资源」**之前**，否则 404（🔊 点了没声音）。
                if (shellPath.StartsWith(SoundRoute, StringComparison.Ordinal))
                {
                    var rest = shellPath.Substring(SoundRoute.Length);
                    var cut = rest.IndexOf('/');
                    if (cut > 0)
                    {
                        return ServeDictSound(engine, ShellAssetSource.Decode(rest.Substring(0, cut)),
                                              ShellAssetSource.Decode(rest.Substring(cut + 1)));
                    }
                    var bad = new VirtualResponse
                    {
                        Status = 400,
                        ReasonPhrase = "Bad Request",
                        Reason = "音频地址不完整：需要 /__sound__/<词典 id>/<键名>"
                    };
                    bad.Body = Encoding.UTF8.GetBytes(bad.Reason);
                    return bad;
                }
                if (shell == null)
                {
                    var none = new VirtualResponse
                    {
                        Status = 404,
                        ReasonPhrase = "Not Found",
                        Reason = "这一版没有配置外壳资源目录（lookup.invalid 上没有东西可发）"
                    };
                    none.Body = Encoding.UTF8.GetBytes(none.Reason);
                    return none;
                }
                return shell.Serve(ShellAssetSource.Decode(shellPath));
            }

            var res = new VirtualResponse();
            string dictId, path, query;
            if (req == null || !TryDictId(req.Url, out dictId, out path, out query))
            {
                res.Status = 404;
                res.ReasonPhrase = "Not Found";
                res.Reason = "这个地址不属于词典资源域（https://<词典 id>." + ResourceDomain + "/…）";
                res.Body = Encoding.UTF8.GetBytes(res.Reason);
                return res;
            }

            // ① 词条正文文档：**动态生成**，所以不参与协商缓存（每次都要来问一句）
            if (path == EntryPath)
            {
                // ★ **译文伪词条**先分开处理：虚拟词典 `translate` 的文档躺在 `TranslateDocs`
                //   那张表里（键是内核给的内容哈希）。不先分开处理就会走到「让内核去查这本词典」
                //   那条路上 —— 词库里没有叫 translate 的一本，用户看到的是一句 404。
                if (string.Equals(dictId, TranslateDocs.DictId, StringComparison.Ordinal))
                {
                    return TranslateDocs.Serve(QueryValue(query, "tok"));
                }
                var word = QueryValue(query, "word");
                try
                {
                    string json;
                    Dsh.EngineEntryDocument(engine, dictId, word, out json);
                    // ⚠️ 内核给的是**一个 JSON 信封**（`{html, plainText, resourceBase, audioKeys[]}`），
                    //    而这条路由要的是**里面那份 HTML**。整封当正文发出去的症状极隐蔽：词条页照样
                    //    「有东西」（浏览器把那段 JSON 当 HTML 渲染），状态码 / CORS 全对，但**桥接脚本
                    //    有语法错误、不报错地不执行** —— 词条里的 `entry://` 链接点了没反应、喇叭没声音。
                    var html = JsonString(json, "html") ?? "";
                    res.Status = 200;
                    res.ContentType = "text/html; charset=utf-8";
                    res.Body = Encoding.UTF8.GetBytes(html);
                    res.Headers.Add("Cache-Control: no-cache");
                    // ⚠️ 这一条**必须有**：词条正文住在 `*.dictres.invalid` 这个**跨源**地址下，外壳
                    //    页面想读它的字节就得过 CORS。少了它 iframe 照样显示（导航不是 CORS 请求），
                    //    但 `fetch(entryUrl)` 会**不报错地失败** —— 而壳那条 检查 正是靠这条 fetch 来钉
                    //    「页面拿到的字节 == 宿主发出去的字节」。
                    res.Headers.Add("Access-Control-Allow-Origin: *");
                }
                catch (Exception err)
                {
                    res.Status = 404;
                    res.ReasonPhrase = "Not Found";
                    res.Reason = err.Message;
                    res.Body = Encoding.UTF8.GetBytes(res.Reason);
                }
                return res;
            }

            // ② 词典自带录音：词条正文里的 `sound://` / `snd://` 被桥接脚本改写成这条路由
            if (path.StartsWith(SoundRoute, StringComparison.Ordinal))
            {
                return ServeDictSound(engine, dictId, Uri.UnescapeDataString(path.Substring(SoundRoute.Length)));
            }

            // ③ 其余一律当资源（样式 / 字体 / 图片…）
            return ServeResource(engine, dictId, path, req);
        }

        /// <summary>
        /// **词典自带的那段录音**（`.mdd` 里的原录音）—— 两个出口共用的实现：宿主页的播放器走
        /// `https://lookup.invalid/__sound__/&lt;词典 id&gt;/&lt;键名&gt;`，词条正文自己走词典域那条；
        /// 分开是 CSP 的缘故。取字节 / 认格式 / 那句话只在这一处，两条路不会各说一套。
        /// </summary>
        internal static VirtualResponse ServeDictSound(IntPtr engine, string dictId, string key)
        {
            var res = new VirtualResponse();
            byte[] raw;
            string why;
            if (!TryResourceBytes(engine, dictId, key, out raw, out why))
            {
                return NotFound(res, why);
            }
            // 音频预处理：认不出来就**当场如实说**（不许让播放器报错误码）
            byte[] playable;
            string mime;
            string audioWhy;
            if (!PrepareAudio(raw, out playable, out mime, out audioWhy))
            {
                // ⚠️ **这里必须用内核给的那句话**，不许自己编一句「这段录音放不了」—— 内核说得比
                //    泛泛之词具体（「这段录音是 Speex（.spx）…」/「无法识别的音频格式（xxx）。」），
                //    编一句就把「到底哪儿不对」藏起来，用户只会以为录音坏了。
                return NotFound(res, string.IsNullOrEmpty(audioWhy) ? "这段录音放不了" : audioWhy);
            }
            res.Status = 200;
            res.ContentType = mime;
            res.Body = playable;
            res.Total = playable.Length;
            res.Headers.Add("Accept-Ranges: bytes");
            res.Headers.Add("Access-Control-Allow-Origin: *");
            res.Headers.Add("Cache-Control: no-cache");
            return res;
        }

        /// <summary>
        /// 发一段**宿主合成**的语音（见 <see cref="Speech"/>）—— 这里只负责发字节：合成、缓存键、
        /// 那一句人话全在那边（「念什么」是内核判的，「怎么念」是本机语音引擎的事）。
        /// 键找不到就说「再点一次」：键是**内容寻址**的（上界 64 条），被挤掉了不是错误。
        /// </summary>
        private static VirtualResponse ServeSpeech(string key)
        {
            var res = new VirtualResponse();
            byte[] wav;
            if (!Speech.TryGet(Uri.UnescapeDataString(key ?? ""), out wav) || wav == null)
            {
                return NotFound(res, "这段合成语音已经不在了（再点一次「朗读」就有了）。");
            }
            res.Status = 200;
            res.ContentType = Speech.MimeOf(Uri.UnescapeDataString(key ?? ""));
            res.Body = wav;
            res.Total = wav.Length;
            res.Headers.Add("Accept-Ranges: bytes");
            res.Headers.Add("Access-Control-Allow-Origin: *");
            res.Headers.Add("Cache-Control: no-cache");
            return res;
        }

        /// <summary>
        /// 一条「没有 / 放不了」的 404：正文就是那句人话，**并且带 CORS**。
        /// ⚠️ CORS 不是顺手加的：这两条 404 是给**外壳页面**读的 —— `<audio>` 放不出来时前端会回头
        /// `fetch` 这条地址把内核那句人话读出来显示；少了它那次 fetch 会被挡掉（喇叭没声音也没说为什么）。
        /// </summary>
        private static VirtualResponse NotFound(VirtualResponse res, string why)
        {
            res.Status = 404;
            res.ReasonPhrase = "Not Found";
            res.Reason = why;
            res.Body = Encoding.UTF8.GetBytes(why ?? "");
            res.Headers.Add("Access-Control-Allow-Origin: *");
            return res;
        }

        /// <summary>资源那条路：ETag → 304；Range → 206 / 416；其余 200 / 404</summary>
        private static VirtualResponse ServeResource(IntPtr engine, string dictId, string path,
                                                     VirtualRequest req)
        {
            var res = new VirtualResponse();

            // 先探一遍「有没有、多长、身份证是什么」（这一次不要字节）
            string key = path.TrimStart('/');
            long total = 0;
            string mime = "application/octet-stream";
            string etag = null;
            string why = null;
            bool found = ProbeResource(engine, dictId, key, out total, out mime, out etag, out why);
            if (!found)
            {
                res.Status = 404;
                res.ReasonPhrase = "Not Found";
                res.Reason = why;
                res.Body = Encoding.UTF8.GetBytes(why);
                return res;
            }
            res.Total = total;
            res.ETag = etag;

            // 协商缓存：命中就回 304（**一个字节都不读**）
            if (MatchesETag(req.IfNoneMatch, etag))
            {
                res.Status = 304;
                res.ReasonPhrase = "Not Modified";
                res.Headers.Add("Access-Control-Allow-Origin: *");
                res.Headers.Add("Cache-Control: no-cache");
                return res;
            }

            long start = 0;
            long length = 0;
            bool unsatisfiable = false;
            bool hasRange = TryParseRange(req.Range, total, out start, out length, out unsatisfiable);

            if (hasRange && unsatisfiable)
            {
                res.Status = 416;
                res.ReasonPhrase = "Range Not Satisfiable";
                res.Headers.Add("Content-Range: bytes */" + total.ToString(CultureInfo.InvariantCulture));
                res.Headers.Add("Accept-Ranges: bytes");
                res.Headers.Add("Access-Control-Allow-Origin: *");
                return res;
            }

            if (hasRange && length == 0)
            {
                // `bytes=-N` 那种后缀写法要先知道总长再回头切一刀（少见，值得多一次调用）
                length = Math.Min(-length, total);
                start = total - length;
            }

            UIntPtr offset = hasRange ? (UIntPtr)(ulong)start : UIntPtr.Zero;
            UIntPtr want = hasRange ? (UIntPtr)(ulong)length : UIntPtr.Zero;

            IntPtr bytes = IntPtr.Zero;
            UIntPtr len = UIntPtr.Zero;
            string meta;
            try
            {
                Dsh.EngineResource(engine, dictId, key, offset, want, out bytes, out len, out meta);
            }
            catch (Exception err)
            {
                res.Status = 500;
                res.ReasonPhrase = "Internal Error";
                res.Reason = err.Message;
                res.Body = Encoding.UTF8.GetBytes(err.Message);
                return res;
            }

            byte[] slice = new byte[(ulong)len];
            if (bytes != IntPtr.Zero && slice.Length > 0) System.Runtime.InteropServices.Marshal.Copy(bytes, slice, 0, slice.Length);
            if (bytes != IntPtr.Zero) DshRaw.dsh_release(bytes);

            res.Body = slice;
            res.ContentType = mime;
            res.Headers.Add("Accept-Ranges: bytes");
            // 词条正文在 opaque origin 里，字体这些资源需要 CORS
            res.Headers.Add("Access-Control-Allow-Origin: *");
            // no-cache = 「每次都要来问一句」，**不是**「别缓存」；配上 ETag 才是协商缓存。
            // ⚠️ 不许改成 max-age：词典 id 虽是内容哈希，但同一 id 的资源仍可能在卷之间移动，
            //    长缓存会拿到旧资源。
            res.Headers.Add("Cache-Control: no-cache");
            if (!string.IsNullOrEmpty(etag)) res.Headers.Add("ETag: " + etag);

            if (hasRange)
            {
                long end = start + slice.Length - 1;
                if (end < start) end = start;
                res.Status = 206;
                res.ReasonPhrase = "Partial Content";
                res.Headers.Add("Content-Range: bytes " + start.ToString(CultureInfo.InvariantCulture) +
                                "-" + end.ToString(CultureInfo.InvariantCulture) + "/" +
                                total.ToString(CultureInfo.InvariantCulture));
            }
            else
            {
                res.Status = 200;
                res.Headers.Add("Content-Length: " + slice.Length.ToString(CultureInfo.InvariantCulture));
            }
            return res;
        }

        /// <summary>只问「有没有、多长、身份证」（不读字节）</summary>
        private static bool ProbeResource(IntPtr engine, string dictId, string key, out long total,
                                          out string mime, out string etag, out string why)
        {
            total = 0;
            mime = "application/octet-stream";
            etag = null;
            why = "";
            IntPtr bytes = IntPtr.Zero;
            UIntPtr len = UIntPtr.Zero;
            string meta;
            try
            {
                // 取 0 字节：内核照样给 total / mime / etag，只是不带内容
                Dsh.EngineResource(engine, dictId, key, UIntPtr.Zero, UIntPtr.Zero, out bytes, out len, out meta);
            }
            catch (Exception err)
            {
                why = err.Message;
                return false;
            }
            if (bytes != IntPtr.Zero) DshRaw.dsh_release(bytes);
            if (meta == null) { why = "内核没给出资源元信息"; return false; }

            bool found = meta.Contains("\"found\":true");
            total = JsonLong(meta, "total");
            var m = JsonString(meta, "mime");
            if (!string.IsNullOrEmpty(m)) mime = m;
            etag = JsonString(meta, "etag");
            if (!found) why = JsonString(meta, "reason");
            if (string.IsNullOrEmpty(why)) why = "这本词典里没有这个资源（" + key + "）";
            return found;
        }

        /// <summary>取一段资源的原始字节（`__sound__` 那条路由、以及它在外壳站点上的出口用）</summary>
        internal static bool TryResourceBytes(IntPtr engine, string dictId, string key, out byte[] raw,
                                             out string why)
        {
            raw = null;
            why = "";
            IntPtr bytes = IntPtr.Zero;
            UIntPtr len = UIntPtr.Zero;
            string meta;
            try
            {
                Dsh.EngineResource(engine, dictId, key, UIntPtr.Zero, UIntPtr.Zero, out bytes, out len, out meta);
            }
            catch (Exception err)
            {
                why = err.Message;
                return false;
            }
            if (bytes == IntPtr.Zero)
            {
                why = JsonString(meta, "reason");
                if (string.IsNullOrEmpty(why)) why = "取不到这段资源（" + key + "）";
                return false;
            }
            raw = new byte[(ulong)len];
            if (raw.Length > 0) System.Runtime.InteropServices.Marshal.Copy(bytes, raw, 0, raw.Length);
            DshRaw.dsh_release(bytes);
            return true;
        }

        /// <summary>
        /// 音频预处理：交给内核嗅探 / 解。回 false 时 <paramref name="why"/> 是**内核那句人话**，
        /// 调用方必须原样把它发出去，不许换成「这段录音放不了」这种话（那会把「到底哪儿不对」藏起来）。
        /// </summary>
        internal static bool PrepareAudio(byte[] raw, out byte[] playable, out string mime,
                                         out string why)
        {
            playable = null;
            mime = "audio/wav";
            why = "";
            if (raw == null || raw.Length == 0)
            {
                why = "音频内容是空的。";
                return false;
            }
            IntPtr input = System.Runtime.InteropServices.Marshal.AllocHGlobal(raw.Length);
            IntPtr outBytes = IntPtr.Zero;
            try
            {
                System.Runtime.InteropServices.Marshal.Copy(raw, 0, input, raw.Length);
                UIntPtr outLen = UIntPtr.Zero;
                string meta;
                Dsh.AudioPrepare(input, (UIntPtr)(ulong)raw.Length, out outBytes, out outLen, out meta);
                if (outBytes == IntPtr.Zero || (ulong)outLen == 0)
                {
                    // 不可播这一种情况内核**只给 reason、不给字节**（见 dsh_speech_api.c 末尾那段）
                    why = JsonString(meta, "reason");
                    return false;
                }
                playable = new byte[(ulong)outLen];
                System.Runtime.InteropServices.Marshal.Copy(outBytes, playable, 0, playable.Length);
                var got = JsonString(meta, "mime");
                if (!string.IsNullOrEmpty(got)) mime = got;
                return true;
            }
            catch (Exception err)
            {
                // 内核那一层自己兜得住；真抛出来也只如实说，绝不把异常抛过边界
                why = "音频整备失败：" + err.Message;
                playable = null;
                return false;
            }
            finally
            {
                if (outBytes != IntPtr.Zero) DshRaw.dsh_release(outBytes);
                System.Runtime.InteropServices.Marshal.FreeHGlobal(input);
            }
        }

        /// <summary>
        /// 解析 `Range` 头（只做单段）：`bytes=0-499` / `bytes=500-` / `bytes=-500` 三种写法都要认，
        /// 多段（`bytes=0-99,200-299`）**刻意不支持** —— 真按多段回要拼 multipart，而我们这儿都是
        /// 几十 KB 的小资源，整体回 200 更简单也更稳。
        /// `hasRange = false` = 没有 Range 或写法不认识 → 回 200；`unsatisfiable = true` = 越界 → 回 416。
        /// ⚠️ 后缀写法这里返回 `length` 为**负数**（存的是「最后多少字节」），因为此刻还不知道总长；
        ///    调用方拿到 total 之后再换成 start/length。
        /// </summary>
        internal static bool TryParseRange(string header, long total, out long start, out long length,
                                           out bool unsatisfiable)
        {
            start = 0;
            length = 0;
            unsatisfiable = false;
            if (string.IsNullOrEmpty(header)) return false;
            var text = header.Trim();
            if (!text.StartsWith("bytes=", StringComparison.OrdinalIgnoreCase)) return false;
            var spec = text.Substring("bytes=".Length).Trim();
            if (spec.IndexOf(',') >= 0) return false;          // 多段：不支持
            var dash = spec.IndexOf('-');
            if (dash < 0) return false;

            var first = spec.Substring(0, dash).Trim();
            var last = spec.Substring(dash + 1).Trim();

            if (first.Length == 0)
            {
                long suffix;
                if (!long.TryParse(last, NumberStyles.Integer, CultureInfo.InvariantCulture, out suffix) ||
                    suffix <= 0) return false;
                if (total <= 0) { unsatisfiable = true; return true; }
                length = -Math.Min(suffix, total);             // 负数 = "最后 N 字节"（见上面那段）
                start = 0;
                return true;
            }

            long from;
            if (!long.TryParse(first, NumberStyles.Integer, CultureInfo.InvariantCulture, out from) ||
                from < 0) return false;
            if (from >= total) { unsatisfiable = true; return true; }

            long to;
            if (last.Length == 0) to = total - 1;
            else if (!long.TryParse(last, NumberStyles.Integer, CultureInfo.InvariantCulture, out to) ||
                     to < from) return false;
            if (to >= total) to = total - 1;

            start = from;
            length = to - from + 1;
            return true;
        }

        /// <summary>`If-None-Match` 与我们的 ETag 是否命中：浏览器可能发多个（逗号分隔）、
        /// 可能发 `*`、也可能发弱比较的 `W/` 前缀那种，所以逐项比对并容忍 `W/`，别写成一个 `==`。</summary>
        internal static bool MatchesETag(string header, string etag)
        {
            if (string.IsNullOrEmpty(header) || string.IsNullOrEmpty(etag)) return false;
            foreach (var raw in header.Split(','))
            {
                var candidate = raw.Trim();
                if (candidate.Length == 0) continue;
                if (candidate == "*") return true;
                if (candidate.StartsWith("W/", StringComparison.Ordinal)) candidate = candidate.Substring(2).Trim();
                if (string.Equals(candidate, etag, StringComparison.Ordinal)) return true;
            }
            return false;
        }

        /// <summary>取查询串里一个参数（值要**解码**：词条名可能带空格或中文）</summary>
        internal static string QueryValue(string query, string name)
        {
            if (string.IsNullOrEmpty(query)) return "";
            var text = query.StartsWith("?", StringComparison.Ordinal) ? query.Substring(1) : query;
            foreach (var piece in text.Split('&'))
            {
                var eq = piece.IndexOf('=');
                if (eq <= 0) continue;
                if (!string.Equals(piece.Substring(0, eq), name, StringComparison.Ordinal)) continue;
                return Uri.UnescapeDataString(piece.Substring(eq + 1).Replace('+', ' '));
            }
            return "";
        }

        // ── 极简 JSON 取值（内核那几份元信息都是平坦的小对象）──

        internal static string JsonString(string json, string key)
        {
            if (json == null) return null;
            var pat = "\"" + key + "\":\"";
            var at = json.IndexOf(pat, StringComparison.Ordinal);
            if (at < 0) return null;
            return JsonStringAt(json, at + pat.Length);
        }

        /// <summary>
        /// 取一个数组里**每一项**的某个字符串字段（`Speech` 取内核切好的那几段用它）。
        /// ⚠️ `stopKey` 不是可有可无的：数组里那几段文本**自己可能带 `]`**，所以「扫到 `]` 为止」是错的，
        ///    得**扫到数组后面那个兄弟键为止**（`dsh_speech_plan` 里 chunks 后面紧跟 chunkCount）。
        /// ⚠️ 那个键的字节序列可以直接找：JSON 字符串里的引号一律先转义，一段文本**内部**不可能
        ///    原样出现它，于是扫到的每一处都是真的键。
        /// </summary>
        internal static List<string> JsonStringList(string json, string arrayKey, string itemKey,
                                                    string stopKey)
        {
            var list = new List<string>();
            if (json == null) return list;
            var open = json.IndexOf("\"" + arrayKey + "\":[", StringComparison.Ordinal);
            if (open < 0) return list;
            var end = json.IndexOf("\"" + stopKey + "\":", open, StringComparison.Ordinal);
            if (end < 0) end = json.Length;
            var pat = "\"" + itemKey + "\":\"";
            var at = open;
            while (true)
            {
                var hit = json.IndexOf(pat, at, StringComparison.Ordinal);
                if (hit < 0 || hit >= end) break;
                list.Add(JsonStringAt(json, hit + pat.Length));
                at = hit + pat.Length;
            }
            return list;
        }

        /// <summary>从开引号**之后**那个位置读一个 JSON 字符串（含反转义）</summary>
        private static string JsonStringAt(string json, int at)
        {
            var sb = new StringBuilder();
            for (var i = at; i < json.Length; i++)
            {
                var c = json[i];
                if (c == '\\' && i + 1 < json.Length)
                {
                    var n = json[i + 1];
                    if (n == 'n') sb.Append('\n');
                    else if (n == 't') sb.Append('\t');
                    else if (n == 'r') sb.Append('\r');
                    else if (n == 'u' && i + 5 < json.Length)
                    {
                        int code;
                        if (int.TryParse(json.Substring(i + 2, 4), NumberStyles.HexNumber,
                                         CultureInfo.InvariantCulture, out code))
                        {
                            sb.Append((char)code);
                            i += 4;
                        }
                    }
                    else sb.Append(n);
                    i++;
                    continue;
                }
                if (c == '"') break;
                sb.Append(c);
            }
            return sb.ToString();
        }

        internal static long JsonLong(string json, string key)
        {
            if (json == null) return 0;
            var pat = "\"" + key + "\":";
            var at = json.IndexOf(pat, StringComparison.Ordinal);
            if (at < 0) return 0;
            at += pat.Length;
            var end = at;
            while (end < json.Length && (char.IsDigit(json[end]) || json[end] == '-')) end++;
            long value;
            return long.TryParse(json.Substring(at, end - at), NumberStyles.Integer,
                                 CultureInfo.InvariantCulture, out value) ? value : 0;
        }

        /// <summary>
        /// 取一个布尔字段；**找不到就回 <paramref name="fallback"/>**（不是 false）—— 读设置那一侧
        /// 「字段不在」与「字段是 false」是两件事（内核的补丁语义就是照这条分的），并成一个 false
        /// 会把「没设过」读成「关掉了」。
        /// </summary>
        internal static bool JsonBool(string json, string key, bool fallback)
        {
            if (json == null) return fallback;
            var pat = "\"" + key + "\":";
            var at = json.IndexOf(pat, StringComparison.Ordinal);
            if (at < 0) return fallback;
            at += pat.Length;
            if (string.CompareOrdinal(json, at, "true", 0, 4) == 0) return true;
            if (string.CompareOrdinal(json, at, "false", 0, 5) == 0) return false;
            return fallback;
        }
    }
}
