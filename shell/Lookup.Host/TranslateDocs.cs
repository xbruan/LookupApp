namespace Lookup.Host
{
    using System;
    using System.Collections.Generic;
    using System.Text;

    /// <summary>
    /// **译文伪词条的文档表**。译文**伪装成一个正常词条**（虚拟 `dictId` 不会与真词典撞车，真词典的 id 是内容哈希），
    /// 是为了不用把「朗读、复制、进返回栈、回退」这四件事各写第二遍；文档内容全在**内核**里，外壳只做一件事 —— 把那份 HTML
    /// 存进下面这张表、按令牌发回去。⚠️ 令牌是内核给的**内容哈希**（不是随机数）→ 同一段译文同一个令牌，这张表天然幂等；容量 20 份。
    /// </summary>
    internal static class TranslateDocs
    {
        /// <summary>虚拟词典 id（真词典的 id 是内容哈希，所以这个词永远不会撞车）</summary>
        internal const string DictId = "translate";

        /// <summary>伪词条正文的域名（`TryDictId` 认它，`dictId` 就是上面那个）</summary>
        internal const string Host = DictId + "." + VirtualHost.ResourceDomain;

        /// <summary>留最近多少份文档（参考实现同一条约定）</summary>
        private const int Capacity = 20;

        private static readonly object Gate = new object();
        private static readonly Dictionary<string, string> Documents =
            new Dictionary<string, string>(StringComparer.Ordinal);
        private static readonly LinkedList<string> Order = new LinkedList<string>();

        /// <summary>存一份文档（按内核给的令牌），同时淘汰最旧的那份</summary>
        internal static void Store(string token, string html)
        {
            if (string.IsNullOrEmpty(token)) return;
            lock (Gate)
            {
                if (Documents.ContainsKey(token))
                {
                    /* 同一个令牌 = 同一份文档（内容寻址），重放不改变任何东西 */
                    Documents[token] = html ?? "";
                    return;
                }
                Documents[token] = html ?? "";
                Order.AddFirst(token);
                while (Documents.Count > Capacity && Order.Count > 0)
                {
                    var last = Order.Last;
                    Order.RemoveLast();
                    Documents.Remove(last.Value);
                }
            }
        }

        /// <summary>按令牌取回文档；取不到（重启过 / 淘汰了）回 null</summary>
        internal static string Html(string token)
        {
            if (string.IsNullOrEmpty(token)) return null;
            lock (Gate)
            {
                string html;
                return Documents.TryGetValue(token, out html) ? html : null;
            }
        }

        /// <summary>伪词条的文档地址（令牌放查询串，与真词条的 `?word=` 同一个位置）</summary>
        internal static string UrlFor(string token)
        {
            return "https://" + Host + VirtualHost.EntryPath + "?tok=" +
                   Uri.EscapeDataString(token ?? "");
        }

        /// <summary>现在存着几份（自检与诊断读它）</summary>
        internal static int Count
        {
            get { lock (Gate) { return Documents.Count; } }
        }

        internal static void Clear()
        {
            lock (Gate)
            {
                Documents.Clear();
                Order.Clear();
            }
        }

        /// <summary>
        /// 处理 `/__entry__?tok=…`：把那份文档发出去。
        /// ⚠️ 取不到时**不是 500** —— 那多半是上一次运行留下的地址（表在内存里、重启就空）或翻了几十个词之后被淘汰了，
        /// 回一句人话用户就知道再翻一次就好。
        /// </summary>
        internal static VirtualResponse Serve(string token)
        {
            var res = new VirtualResponse();
            var html = Html(token);
            if (html == null)
            {
                res.Status = 404;
                res.ReasonPhrase = "Not Found";
                res.Reason = "这份译文已经不在了（翻译过的内容只留最近 20 条，重启后也会清空）—— 再翻一次就有了。";
                res.Body = Encoding.UTF8.GetBytes(res.Reason);
                return res;
            }
            res.Status = 200;
            res.ContentType = "text/html; charset=utf-8";
            res.Body = Encoding.UTF8.GetBytes(html);
            res.Headers.Add("Cache-Control: no-cache");
            /*
             * 与词条那条路由同一条理由：文档住在 `*.dictres.invalid`（**跨源**），少了它 iframe 照样显示，
             * 但「页面拿到的字节 == 宿主发出去的字节」那条检查标准会不报错地失败。
             */
            res.Headers.Add("Access-Control-Allow-Origin: *");
            return res;
        }
    }
}
