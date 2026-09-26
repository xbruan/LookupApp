namespace Lookup.Host
{
    using System;
    using System.Globalization;
    using System.Threading;

    /// <summary>
    /// **一条稍后自己回的应答** —— 给要联网的那几条路用：HTTP 那段阻塞调用（最长 8 秒）丢线程池、内核的 plan / accept
    /// 留在 UI 线程，回来再回包。⚠️ 它**必须在 UI 线程上叫**（后台段回来先走 `IShellHost.PostToUiThread`）、各只回**一条**；
    /// ⚠️ 也不给内核加锁 —— 两段式之后**后台线程根本不碰内核**，能不加锁就不加锁。
    /// </summary>
    internal sealed class DeferredReply
    {
        private readonly long _id;
        private readonly Action<string> _post;
        private int _sent;

        internal DeferredReply(long id, Action<string> post)
        {
            _id = id;
            _post = post;
        }

        /// <summary>true = 这一条已经被我接管，`Dispatch` 不要再回包</summary>
        internal bool Taken;

        /// <summary>回一条成功（`resultJson` 就是原来那个「结果」 JSON；空串按 `null` 处理）</summary>
        internal void Complete(string resultJson)
        {
            Send("{\"kind\":\"res\",\"id\":" + _id.ToString(CultureInfo.InvariantCulture) +
                 ",\"ok\":true,\"value\":" + (string.IsNullOrEmpty(resultJson) ? "null" : resultJson) +
                 "}");
        }

        /// <summary>回一条失败（人话；与 `ShellBridge.Fail` 同一个形状）</summary>
        internal void Fail(string message)
        {
            Send("{\"kind\":\"res\",\"id\":" + _id.ToString(CultureInfo.InvariantCulture) +
                 ",\"ok\":false,\"error\":" + Dispatch.Quote(message ?? "未知错误") + "}");
        }

        private void Send(string json)
        {
            /* 只回一次：后台那一段如果既抛异常又走到了 Complete（或者将来有人手滑调两次），
               第二条会被页面当成「多出来的一条回包」（它的 pending 表里已经没有这个 id 了）*/
            if (Interlocked.Exchange(ref _sent, 1) != 0) return;
            if (_post != null) _post(json);
        }
    }
}
