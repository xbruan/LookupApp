namespace Lookup.Host
{
    using System;
    using System.IO;
    using System.Net;
    using System.Text;

    /// <summary>
    /// **翻译那个「把字节发出去」的动作** —— 这一层就是全部，且**判断一个字都不许留在这里**：发给谁 / 带哪三个头 / 请求体
    /// 由 `dsh_translate_plan` 定，回包含义 / 错误码人话 / 要不要写缓存由 `dsh_translate_accept` 定 —— 所以它**只搬运**，
    /// 不解析 JSON、不看业务码、不改字段。⚠️ 唯一一处平台事实是 TLS 1.2（不显式要会被火山引擎直接拒）；⚠️ **超时必须有**，超时回 `status = 0`。
    /// </summary>
    internal static class TranslateHttp
    {
        /// <summary>整个请求的上限（毫秒）</summary>
        internal const int TimeoutMs = 8000;

        /// <summary>一次应答：状态码 + 正文 + 耗时（耗时是给界面显示与「检测凭据」用的）</summary>
        internal struct Reply
        {
            internal int Status;      // 0 = 根本没发出去（断网 / 超时 / 地址不对）
            internal string Body;
            internal int ElapsedMs;
            internal string Why;      // status = 0 时的人话原因（排错用；不进界面）
            /// <summary>
            /// 发这一次请求的**托管线程号**。⚠️ 只给 **自检**用（`debug:slowReply` 拿它证明「干活那一段不在 UI 线程上」）——
            /// 业务代码一个字都不读它，别拿它做判断（它是诊断数据，不是状态）。
            /// </summary>
            internal int ThreadId;
        }

        /// <summary>
        /// 按 plan 说的把请求发出去。
        /// @param url      plan 给的地址
        /// @param headers  plan 给的头，形状是 `[[名字, 值], …]`
        /// @param body     plan 给的请求体（UTF-8 JSON）
        /// </summary>
        internal static Reply Post(string url, string[][] headers, string body)
        {
            var reply = new Reply();
            /* 诊断实测结果：这一次请求是在哪条托管线程上发的（只有 自检读，见 Reply.ThreadId）*/
            reply.ThreadId = Environment.CurrentManagedThreadId;
            var watch = System.Diagnostics.Stopwatch.StartNew();
            HttpWebRequest request = null;

            try
            {
                /*
                 * TLS 1.2 —— 只在这里设一次（静态字段，重复设是幂等的）。
                 * 用 `|=` 而不是 `=`：别的组件可能已经要了别的版本，别把它们的抹掉。
                 */
                try
                {
                    ServicePointManager.SecurityProtocol |= SecurityProtocolType.Tls12;
                }
                catch (Exception) { /* 老系统认不出这个枚举值：那就按系统默认来 */ }

                if (string.IsNullOrEmpty(url))
                {
                    reply.Why = "plan 没给地址";
                    return reply;
                }

                request = (HttpWebRequest)WebRequest.Create(url);
                request.Method = "POST";
                request.Timeout = TimeoutMs;
                request.ReadWriteTimeout = TimeoutMs;
                request.ContentType = "application/json";
                request.KeepAlive = false;

                var payload = Encoding.UTF8.GetBytes(body ?? "");
                request.ContentLength = payload.Length;

                if (headers != null)
                {
                    foreach (var pair in headers)
                    {
                        if (pair == null || pair.Length < 2) continue;
                        /* Content-Type 走上面那行（HttpWebRequest 不认重复设置它）*/
                        if (string.Equals(pair[0], "Content-Type", StringComparison.OrdinalIgnoreCase))
                            continue;
                        request.Headers[pair[0]] = pair[1];
                    }
                }

                using (var stream = request.GetRequestStream())
                {
                    stream.Write(payload, 0, payload.Length);
                }

                using (var response = (HttpWebResponse)request.GetResponse())
                {
                    reply.Status = (int)response.StatusCode;
                    reply.Body = ReadAll(response);
                }
            }
            catch (WebException err)
            {
                /*
                 * 4xx / 5xx 会走到这里（.NET 把非 2xx 当异常），**但这不是「没答上来」** —— 服务端其实答了，
                 * 只是状态码不是 200；正文里往往有它给的原因，所以取出来当正常回包交下去，让内核去翻人话。
                 */
                var response = err.Response as HttpWebResponse;
                if (response != null)
                {
                    try
                    {
                        reply.Status = (int)response.StatusCode;
                        reply.Body = ReadAll(response);
                    }
                    catch (Exception inner)
                    {
                        reply.Why = "读回包失败：" + inner.Message;
                    }
                    finally
                    {
                        response.Close();
                    }
                }
                else
                {
                    // 真·没答上来：超时 / DNS / 断网 / TLS 握手失败
                    reply.Status = 0;
                    reply.Why = err.Status + "：" + err.Message;
                }
            }
            catch (Exception err)
            {
                reply.Status = 0;
                reply.Why = err.Message;
            }
            finally
            {
                watch.Stop();
                reply.ElapsedMs = (int)watch.ElapsedMilliseconds;
                if (request != null) { try { request.Abort(); } catch (Exception) { } }
            }

            return reply;
        }

        private static string ReadAll(HttpWebResponse response)
        {
            var stream = response.GetResponseStream();
            if (stream == null) return "";
            using (var reader = new StreamReader(stream, Encoding.UTF8))
            {
                return reader.ReadToEnd();
            }
        }

        /// <summary>
        /// 把 plan 里 `headers` 那一段（`[[名字, 值], …]`）解析成二维数组。
        /// 之所以要解析，是因为 plan 是**内核给的 JSON**，而平台层要按它发请求；
        /// 解析的是**结构**不是业务 —— 值怎么来的、要不要发，全是内核定的。
        /// </summary>
        internal static string[][] ParseHeaders(string headersJson)
        {
            var outer = ShellBridge.SplitArray(headersJson);
            var list = new System.Collections.Generic.List<string[]>();
            foreach (var item in outer)
            {
                var inner = ShellBridge.SplitArray(item);
                if (inner.Count < 2) continue;
                list.Add(new[] { ShellBridge.ArgStr(inner, 0), ShellBridge.ArgStr(inner, 1) });
            }
            return list.ToArray();
        }
    }
}
