namespace Lookup.Host
{
    using System;
    using System.Collections.Generic;
    using System.Globalization;
    using System.IO;
    using System.Text;
    using System.Threading.Tasks;
    using Lookup.Interop;

    /// <summary>
    /// **参考实现那套桥协议**的适配层 —— 界面是参考实现的（`web/**` 原样搬过来的资产，一个字都不改）。
    /// 它**只做两件事**：拆信封、把方法名指到内核（或壳）的能力上；**判断一个字都不许写在这里**。
    /// 方法名必须与 `web/bridge.js` 里页面调的那一串**逐字对上**（静态检查盯着；协议逐字见那份文件顶上那段）。
    /// </summary>
    internal static class ShellBridge
    {
        /// <summary>
        /// 这一条是不是参考实现那套桥的请求（`kind` 为 `req` / `notify` 都归这一层）。
        /// </summary>
        internal static bool IsBridgeRequest(string requestJson)
        {
            var kind = VirtualHost.JsonString(requestJson, "kind");
            return kind == "req" || kind == "notify";
        }

        /// <summary>
        /// 处理一条桥请求。返回 **null 表示「这条不该回包」**：`reply:false` 与 `kind:'notify'` 都是这一种
        /// （上游见到 null 就不发）；null 还有**第二种含义** —— 路由把这条**接管**了（要联网的几条走
        /// 两段式，`DeferredReply` 稍后自己回）。两者在上游看来一样，区别只在路由内部那个 `Taken` 标志。
        /// </summary>
        internal static string Handle(IntPtr engine, string requestJson, IShellHost shell,
                                     Action<string> replySink = null)
        {
            string method;
            long id;
            var args = new List<string>();
            var wantsReply = true;

            try
            {
                var kind = VirtualHost.JsonString(requestJson, "kind");
                method = VirtualHost.JsonString(requestJson, "method");
                if (string.IsNullOrEmpty(method)) return null;   // 参考实现：没有方法名就当没收到
                id = VirtualHost.JsonLong(requestJson, "id");
                args = SplitArray(RawValue(requestJson, "args"));
                if (kind == "notify") wantsReply = false;
                else if (RawValue(requestJson, "reply") != null)
                {
                    wantsReply = VirtualHost.JsonBool(requestJson, "reply", true);
                }
            }
            catch (Exception err)
            {
                return Fail(0, "请求读不动：" + err.Message);
            }

            var host = shell ?? NoShellHost.Instance;

            if (!wantsReply)
            {
                /*
                 * 发完不管那一种（拖拽、上报形状、托盘菜单点一下…）**也要**处理 —— 只是不回包，
                 * 出错只记一行，不回给页面（页面本来也不等回话）。
                 */
                try { Route(engine, method, args, host, null); }
                catch (Exception) { /* 发完不管的那一档，失败没有回话的地方 */ }
                return null;
            }

            var deferred = new DeferredReply(id, replySink);
            try
            {
                var result = Route(engine, method, args, host, deferred);
                /* 路由接管了（要联网那条路）：它自己会回，这里什么都不发 */
                if (deferred.Taken) return null;
                return Ok(id, result);
            }
            catch (InvalidOperationException err)
            {
                // 包装层抛出来的就是内核的 last_error（人话）
                return deferred.Taken ? (string)null : Fail(id, err.Message);
            }
            catch (Exception err)
            {
                return deferred.Taken ? (string)null : Fail(id, err.Message);
            }
        }

        /*
         * **两段式**：后台只发 HTTP，回到 UI 线程做内核那一段并由 `reply` 回包（回包也只能从 UI 线程发）。
         * ⚠️ 内核**没有并发守卫**，所以后台那一段**绝不许碰内核**：`http` 只发请求、不解释结果。
         * `http` 里抛异常这里也要兜住并回一句人话，否则页面那边的 promise 会一直挂着。
         */
        private static void RunHttpDeferred<T>(IShellHost shell, DeferredReply reply,
                                            Func<T> http,
                                            Func<T, string> accept)
        {
            reply.Taken = true;
            var host = shell ?? NoShellHost.Instance;
            Task.Run(delegate
            {
                T got = default(T);
                string failure = null;
                try { got = http(); }
                catch (Exception err) { failure = err.Message; }

                host.PostToUiThread(delegate
                {
                    if (failure != null)
                    {
                        reply.Fail(failure);
                        return;
                    }
                    try { reply.Complete(accept(got)); }
                    catch (Exception err) { reply.Fail(err.Message); }
                });
            });
        }

        /*
         * `translate:text` 的**收尾**（要调内核，所以只能在 UI 线程上跑）。
         * 两个入口（`reply == null` 的同步那条 / 两段式那条）必须**逐字一样**，否则「界面里翻译」与
         * 「脚本里翻译」会给出不同的答案 —— 那是最难查的一类分叉。
         */
        private static string AcceptTranslate(IntPtr engine, string plan, TranslateHttp.Reply reply)
        {
            var accepted = Engine(engine, delegate (out string j) {
                Dsh.TranslateAccept(engine, plan, reply.Status, reply.Body, reply.ElapsedMs, out j);
            });
            if (accepted == null && reply.Status == 0)
            {
                // 真·没发出去（断网/超时）：内核那一侧有专门那句话，这里只兜住形状
                return "{\"ok\":false,\"message\":" + Dispatch.Quote(reply.Why ?? "翻译请求没发出去") +
                       ",\"translation\":\"\",\"sourceLanguage\":\"\",\"targetLanguage\":\"\"," +
                       "\"sourceLabel\":\"\",\"targetLabel\":\"\",\"detected\":\"\",\"tokens\":0," +
                       "\"cachedItems\":0,\"fetchedItems\":0,\"elapsedMs\":" + reply.ElapsedMs + "}";
            }
            return WithEntryUrl(accepted);
        }

        /** `translate:test` 的收尾（同上：抽出来给两个入口共用） */
        private static string AcceptTranslateTest(IntPtr engine, string plan, TranslateHttp.Reply reply)
        {
            var accepted = Engine(engine, delegate (out string j) {
                Dsh.TranslateAccept(engine, plan, reply.Status, reply.Body, reply.ElapsedMs, out j);
            });
            return "{\"ok\":" + (ShellBridge.Flag(accepted, "ok", false) ? "true" : "false") +
                   ",\"message\":" + (ShellBridge.Text(accepted, "message") == null
                       ? "\"\"" : Dispatch.Quote(ShellBridge.Text(accepted, "message"))) +
                   ",\"elapsedMs\":" + reply.ElapsedMs +
                   ",\"tokens\":" + (ShellBridge.Num(accepted, "tokens") ?? 0).ToString(CultureInfo.InvariantCulture) +
                   ",\"sample\":\"apple\",\"translation\":" +
                   (ShellBridge.Text(accepted, "translation") == null
                       ? "\"\"" : Dispatch.Quote(ShellBridge.Text(accepted, "translation"))) + "}";
        }

        /*
         * 查词通道里「自动翻译」那一步的收尾（要调内核 → 只能在 UI 线程上跑）；两个出口共用。
         * `fallback` 是链给的那份词条载荷：翻译**任何一步不成就原样退回去**（界面据此显示终态页），
         * **绝不许装作翻过了**。
         */
        private static string FinishLookupTranslate(IntPtr engine, string plan, TranslateHttp.Reply sent,
                                                    string fallback)
        {
            var accepted = Engine(engine, delegate (out string j) {
                Dsh.TranslateAccept(engine, plan, sent.Status, sent.Body, sent.ElapsedMs, out j);
            });
            if (accepted == null) return fallback;

            var withUrl = WithEntryUrl(accepted);
            return Engine(engine, delegate (out string j) {
                Dsh.TranslatePayload(engine, withUrl, ShellBridge.Text(withUrl, "entryUrl"), out j);
            });
        }

        /*
         * `debug:slowReply <毫秒>`：只为 那条异步管线 —— 在**别的线程**上睡一会儿、再回到 UI 线程回包。
         * 真慢的那两条（机器翻译 / 在线发音）都要真 Key，而「干活那一段在不在 UI 线程上」得在没有 Key
         * 的机器上也能验。⚠️ 回包带**两个线程号**（`workThread` / `replyThread`）供自检对账；
         * **别拿时间差当检查标准**（同步版也能绿）；不做业务、不写设置、不联网。
         */
        private static string SlowReply(IShellHost shell, DeferredReply reply, List<string> args)
        {
            var ms = 0;
            if (args.Count > 0 && args[0] != null) int.TryParse(args[0], out ms);
            if (ms < 0) ms = 0;
            if (ms > 10000) ms = 10000;
            if (reply == null)
            {
                /* 没有回包口的那条路（诊断）：就地睡 —— 它没有界面要保活 */
                System.Threading.Thread.Sleep(ms);
                return "{\"sleptMs\":" + ms.ToString(CultureInfo.InvariantCulture) +
                       ",\"workThread\":" + Environment.CurrentManagedThreadId.ToString(CultureInfo.InvariantCulture) +
                       ",\"replyThread\":" + Environment.CurrentManagedThreadId.ToString(CultureInfo.InvariantCulture) + "}";
            }
            RunHttpDeferred(shell, reply,
                delegate {
                    System.Threading.Thread.Sleep(ms);
                    return new TranslateHttp.Reply
                    {
                        Status = 0, Body = null, ElapsedMs = ms, Why = null,
                        ThreadId = Environment.CurrentManagedThreadId,
                    };
                },
                delegate (TranslateHttp.Reply r) {
                    return "{\"sleptMs\":" + r.ElapsedMs.ToString(CultureInfo.InvariantCulture) +
                           ",\"workThread\":" + r.ThreadId.ToString(CultureInfo.InvariantCulture) +
                           ",\"replyThread\":" + Environment.CurrentManagedThreadId.ToString(CultureInfo.InvariantCulture) + "}";
                });
            return null;
        }

        /// <summary>
        /// 路由表 —— **方法名到内核 / 壳的能力**，这是这一层唯一的「知识」。
        /// 还没接的那几条**当场说清楚是哪一条**（不许不报错地、不返回空值假装成功），搬家的中间态必须一眼看得见。
        /// 与 `Dispatch` 那套点号协议**并存**、不是替换：那条是诊断与 不经过界面约定的直路。
        /// </summary>
        private static string Route(IntPtr engine, string method, List<string> args, IShellHost shell,
                                    DeferredReply reply)
        {
            switch (method)
            {
                /* ── 词典（10）────────────────────────────────────────────────── */
                /*
                 * 词库清单：内核给的字段 → 界面那份 `DictionaryInfo`，**只搬字段、不判断**。
                 * `current` ← 设置的 `currentDictId`；`status`/`errorMessage` ← 内核的 `loaded` 与
                 * `unavailable`/`note`；`entryCount`/`encoding`/`version` 内核按需开一次头就给了。
                 */
                case "dict:list":
                    return DictList(engine);

                /*
                 * 加词典：挑文件是**窗口层**的活（`PickDictionaryFiles` 弹一次选文件对话框），选中之后
                 * **原样**交给内核（「这文件是不是词典」由内核判）；`canceled` 只有壳知道（取消不是失败）。
                 * ⚠️ 内核**区分不出「新加」与「更新」**、也**没有 `orphans` 这个概念**：`added` 给 N 个
                 * 只有长度的占位对象，`updated`/`orphans` 给空数组 —— 不编内容。
                 */
                case "dict:add":
                    {
                        var picked = shell.PickDictionaryFiles();
                        var paths = SplitArray(picked);
                        if (paths.Count == 0)
                            return "{\"added\":[],\"updated\":[],\"orphans\":[],\"canceled\":true}";

                        string addedJson;
                        Engine(engine, delegate (out string j) { Dsh.EngineDictAdd(engine, picked, out j); },
                               out addedJson);

                        var sb = new StringBuilder("{\"added\":[");
                        for (var i = 0; i < CountOf(addedJson, "added"); i++)
                        {
                            if (i > 0) sb.Append(',');
                            sb.Append("{}");
                        }
                        /*
                         * ★ **加不进去的那几本要把「为什么」带给界面**：内核给的是
                         * `{"added":N,"failed":[{"path":…,"reason":…}]}`，把失败的那几本**按文件名 + 原因**
                         * 塞进 `orphans` 即可 —— 页面那句「N 个文件无法识别：…」本来就把它列出来，界面不用改。
                         */
                        sb.Append("],\"updated\":[],\"orphans\":[");
                        var failedItems = SplitArray(RawValue(addedJson, "failed"));
                        for (var i = 0; i < failedItems.Count; i++)
                        {
                            var path = DecodeString(RawValue(failedItems[i], "path"));
                            var reason = DecodeString(RawValue(failedItems[i], "reason"));
                            var name = string.IsNullOrEmpty(path) ? "（没有路径）" : Path.GetFileName(path);
                            if (string.IsNullOrEmpty(name)) name = path;
                            var shown = string.IsNullOrEmpty(reason) ? name : (name + "（" + reason + "）");
                            if (i > 0) sb.Append(',');
                            sb.Append(Dispatch.Quote(shown));
                        }
                        sb.Append("],\"canceled\":false}");
                        EmitDicts(shell, engine);
                        return sb.ToString();
                    }

                case "dict:remove":
                    {
                        Engine(engine, delegate (out string j) { Dsh.EngineDictRemove(engine, ArgStr(args, 0), out j); },
                               out var _);
                        var list = DictList(engine);
                        EmitDicts(shell, engine);
                        return list;
                    }
                case "dict:setCurrent":
                    {
                        Engine(engine, delegate (out string j) { Dsh.EngineDictSetCurrent(engine, ArgStr(args, 0), out j); },
                               out var _);
                        var list = DictList(engine);
                        EmitDicts(shell, engine);
                        return list;
                    }
                case "dict:rename":
                    {
                        // 空串 = 恢复自动标题（内核同约定，见 abi 里 `dsh_engine_dict_rename` 的说明）
                        Engine(engine, delegate (out string j) { Dsh.EngineDictRename(engine, ArgStr(args, 0), ArgStr(args, 1), out j); },
                               out var _);
                        var list = DictList(engine);
                        EmitDicts(shell, engine);
                        return list;
                    }
                case "dict:move":
                    {
                        /*
                         * 词典排序：`delta` 负数往前、正数往后；**到边界由内核夹住** ——
                         * 这里不判边界（那是业务规则，与 `dict:rename` 的「空串 = 恢复默认名」同一条分工）。
                         */
                        var delta = (int)(ArgNum(args, 1) ?? 0);
                        Engine(engine, delegate (out string j) { Dsh.EngineDictMove(engine, ArgStr(args, 0), delta, out j); },
                               out var _);
                        var list = DictList(engine);
                        EmitDicts(shell, engine);
                        return list;
                    }

                case "dict:suggest":
                    // 第 2 个参数（行数上限）壳**吞掉**不搬：内核的上限是常量 `DSH_MAX_LIST_ROWS`。
                    return Engine(engine, delegate (out string j) { Dsh.EngineSuggest(engine, ArgStr(args, 0), out j); });

                case "dict:lookup":
                    {
                        /*
                         * 参数逐位相同：0 = 词，1 = 词典 id（可空），2 = 入口（可空 = input）；返回值**逐字段同名**。
                         * ★ 查词通道的自动翻译：链给 `needsTranslate` 时该翻，而**发请求是壳的事**
                         * （内核零依赖、没有 socket）—— 链说该翻 → 壳翻 → 拿译文拼一份词条载荷，
                         * 界面那一侧一个字都不用认识「自动翻译」。
                         */
                        var lookupReply = Engine(engine, delegate (out string j) {
                            Dsh.EngineLookup(engine, ArgStr(args, 0), Origin(ArgStr(args, 2)),
                                             ArgStr(args, 1), out j);
                        });
                        if (!ShellBridge.Flag(lookupReply, "needsTranslate", false)) return lookupReply;

                        var word = ShellBridge.Text(lookupReply, "query") ?? "";
                        var title = ShellBridge.Text(lookupReply, "dictTitle");
                        var plan = Engine(engine, delegate (out string j) {
                            Dsh.TranslatePlan(engine, word, title, out j);
                        });
                        /* 计划没成（开关关着 / 没 Key / 语种不支持）：**不许装作翻过了** ——
                         * 把链给的那份载荷原样交回去（界面据此显示终态页与那条可点的出路）。*/
                        if (!ShellBridge.Flag(plan, "needsHttp", false)) return lookupReply;

                        /*
                         * ★ 这一条也要走**两段式**：它是最常撞上的联网路（查一个词典里没有的词 →
                         * 自动翻译），在这里同步发请求会让界面僵住几秒。
                         */
                        var planForAccept = plan;
                        var fallback = lookupReply;
                        if (reply != null && !reply.Taken)
                        {
                            var url = ShellBridge.Text(plan, "url");
                            var headers = TranslateHttp.ParseHeaders(ShellBridge.Raw(plan, "headers"));
                            var body = ShellBridge.Text(plan, "body");
                            RunHttpDeferred(shell, reply,
                                delegate { return TranslateHttp.Post(url, headers, body); },
                                delegate (TranslateHttp.Reply sent) {
                                    return FinishLookupTranslate(engine, planForAccept, sent, fallback);
                                });
                            return null;
                        }

                        var sent0 = TranslateHttp.Post(
                            ShellBridge.Text(plan, "url"),
                            TranslateHttp.ParseHeaders(ShellBridge.Raw(plan, "headers")),
                            ShellBridge.Text(plan, "body"));
                        return FinishLookupTranslate(engine, plan, sent0, fallback);
                    }

                case "dict:resolve":
                    {
                        /*
                         * 界面要的是 `string | null`（**落点本身**），内核回的是整个对象 —— 只取 `landed`
                         * （JSON 字符串或 null）原样返回。
                         * ⚠️ 这里**不写查词历史**（内核的 resolve 也不写）：用户还没点那个 chip。
                         */
                        var json = Engine(engine, delegate (out string j) {
                            Dsh.EngineResolve(engine, ArgStr(args, 1), ArgStr(args, 0), out j);
                        });
                        return RawValue(json, "landed") ?? "null";
                    }

                case "dict:probe":
                    {
                        /*
                         * 「这本词典里有没有这个词」的**轻查询**：问一句就走，那本词典**不许**留在内存里。
                         * 参数逐位相同；三态词表**两边本来就一样**（`ok|timeout|error|missing`），只是改名
                         * （`status→state`、`dictTitle→title`、`reason→message`，另补 `dictId`）。
                         * ⚠️ `timeout` / `error` **绝不许并成 `missing`**，壳一个字节都不许「简化」。
                         */
                        var dictId = ArgStr(args, 1) ?? "";
                        var json = Engine(engine, delegate (out string j) {
                            Dsh.EngineProbe(engine, ArgStr(args, 0), dictId,
                                            (int)(ArgNum(args, 2) ?? 0), out j);
                        });
                        return "{\"state\":" + (RawValue(json, "status") ?? "\"error\"") +
                               ",\"landed\":" + (RawValue(json, "landed") ?? "null") +
                               ",\"dictId\":" + Dispatch.Quote(dictId) +
                               ",\"title\":" + (RawValue(json, "dictTitle") ?? "\"\"") +
                               ",\"message\":" + (RawValue(json, "reason") ?? "\"\"") + "}";
                    }

                /*
                 * 借查：**去别的词典里问一遍**（终态页那条「再问一遍」）；参数逐位相同。
                 * ⚠️ 「问哪些本、预算多大、`timeout`/`error` 算不算问完」这套约定**全在内核** ——
                 * 壳不许拿 `dsh_engine_probe` 自己循环一遍。回包形状只转发界面读得懂的键，
                 * **少一个键就少一处要跟着改的地方**（内核多给的那几个是实测结果，这里不转发）。
                 */
                case "dict:borrow":
                    {
                        var recheck = ArgBool(args, 1, false) ? 1 : 0;
                        var json = Engine(engine, delegate (out string j) {
                            Dsh.EngineBorrow(engine, ArgStr(args, 0), null, recheck, out j);
                        });
                        return "{\"hitId\":" + (RawValue(json, "hitId") ?? "\"\"") +
                               ",\"hitTitle\":" + (RawValue(json, "hitTitle") ?? "\"\"") +
                               ",\"unconfirmed\":" + (RawValue(json, "unconfirmed") ?? "[]") + "}";
                    }

                /* ── 历史（2）────────────────────────────────────────────────── */
                /*
                 * 两条都**直通**：内核回的是界面那份 `{items,total}` 的超集，且 `at` 已经是 **UTC 毫秒**
                 * （界面读的是 `new Date(item.at)`，单位对得上）；界面不读 `hasMore` 等字段。
                 */
                case "history:get":
                    return Engine(engine, delegate (out string j) {
                        Dsh.HistoryQuery(engine, (int)(ArgNum(args, 0) ?? 0),
                                         (int)(ArgNum(args, 1) ?? 0), out j);
                    });
                case "history:clear":
                    return Engine(engine, delegate (out string j) { Dsh.HistoryClear(engine, out j); });

                /* ── 布局与窗口（13）──────────────────────────────────────────── */
                /*
                 * 这一组**一律走壳那一侧**（`IShellHost` 的窗口那几条）：页面知道内容多高、形状长什么样，
                 * 但「窗口摆在哪、多大、Region 怎么裁」只有窗口层做得了。
                 * 参数按参考实现的线格式**原样**递下去，壳不重新定义一套。
                 */
                case "layout:info":
                    return shell.LayoutInfo();
                case "layout:set":
                    shell.SetLayout(Arg(args, 0));
                    return null;
                case "shape:set":
                    shell.SetShape(Arg(args, 0), ArgStr(args, 1), ArgBool(args, 2, false));
                    return null;
                case "drag:prepare":
                    shell.DragPrepare();
                    return null;
                case "drag:start":
                    shell.DragStart();
                    return null;
                case "drag:move":
                    shell.DragMove(ArgNum(args, 0));
                    return null;
                case "drag:end":
                    shell.DragEnd();
                    return null;
                case "edge:expand":
                    shell.ExpandFromEdge();
                    return null;
                case "edge:collapse":
                    shell.CollapseToEdge();
                    return null;
                case "floating:reset":
                    shell.ResetPill();
                    return null;
                case "window:hide":
                    shell.HideWindow();
                    return null;
                case "window:requestClose":
                    shell.RequestClose();
                    return null;
                case "window:resolveClose":
                    shell.ResolveClose(ArgStr(args, 0), ArgBool(args, 1, false));
                    return null;

                /* ── 管理窗（6）──────────────────────────────────────────────── */
                /*
                 * `manager:open` **从悬浮窗那一页来**，其余五条**从管理窗自己那一页来**；
                 * 两边的「壳」是两扇窗各自的 `IShellHost`，所以这里只管转发 —— 这一层不认识「窗口对象」。
                 */
                case "manager:open":
                    shell.OpenManager(ArgStr(args, 0));
                    return "true";
                case "manager:close":
                    shell.ManagerClose();
                    return null;
                case "manager:minimize":
                    shell.ManagerMinimize();
                    return null;
                case "manager:drag":
                    shell.ManagerBeginDrag();
                    return null;
                case "manager:surface":
                    // 参考实现里这一条对 `manager` 就是"整窗一块卡片"，没有逐元素形状
                    shell.ManagerSurface(ArgStr(args, 0), ArgStr(args, 1), ArgStr(args, 2),
                                         ArgBool(args, 3, false));
                    return null;
                case "manager:initialTab":
                    {
                        var tab = shell.ManagerConsumeInitialTab();
                        return (string.IsNullOrEmpty(tab)) ? "null" : Dispatch.Quote(tab);
                    }

                /* ── 托盘菜单（9）────────────────────────────────────────────── */
                /*
                 * 这一组**几乎都是从托盘菜单那一页来的**（只有 `measured` 是「页面量好了、
                 * 你把窗口摆到那儿去」）；所以壳这一层只转发 —— 窗口怎么开、注册表怎么写都在那一侧。
                 */
                case "traymenu:toggle":
                    shell.TrayToggleFloating();
                    return null;
                case "traymenu:manager":
                    shell.OpenManager(ArgStr(args, 0));
                    return null;
                case "traymenu:reset":
                    shell.TrayResetPill();
                    return null;
                case "traymenu:quit":
                    shell.TrayQuit();
                    return null;
                case "traymenu:close":
                    shell.TrayMenuHide();
                    return null;
                case "traymenu:state":
                    return shell.TrayState();
                case "traymenu:login":
                    {
                        /*
                         * 勾选项：写完把**系统里的真实状态**回给菜单 ——
                         * 注册表可能被策略挡住，写不进去时不能假装成功
                         * （参考实现的注释专门写了这一条）。
                         */
                        var actual = shell.TraySetLogin(ArgBool(args, 0, false));
                        if (shell != null)
                        {
                            shell.Emit("traymenu:login-changed", actual ? "true" : "false");
                        }
                        return actual ? "true" : "false";
                    }
                case "traymenu:clear-history":
                    /*
                     * 「清空查词历史」—— 这一条**不用问窗口**：历史在内核里，
                     * 这一层手里就有引擎（参考实现里它调的是 `Settings.ClearHistory`）。
                     */
                    return Engine(engine, delegate (out string j) { Dsh.HistoryClear(engine, out j); });
                case "traymenu:measured":
                    {
                        /*
                         * 页面量好的内容尺寸（**DIP**）+ 底色 + 主题 → 主进程据此摆窗口再显示。
                         * ⚠️ 单位换算（DIP → 物理像素）在**窗口那一侧**做：
                         *    参考实现当年就是在这儿直接把 DIP 当物理像素用，
                         *    125% 缩放的屏幕上菜单最后一项被裁掉（看着像少了一项）。
                         */
                        var sizeJson = Arg(args, 0);
                        shell.TrayMenuPlace(ShellBridge.Num(sizeJson, "width") ?? 0,
                                            ShellBridge.Num(sizeJson, "height") ?? 0,
                                            ShellBridge.Text(sizeJson, "fill"),
                                            ShellBridge.Text(sizeJson, "border"),
                                            ShellBridge.Text(sizeJson, "theme"));
                        return null;
                    }

                /* ── 发音（6 / 8）────────────────────────────────────────────── */
                /* ── 发音：真的出声那两条（音源排序与"能不能念"全在内核）──────────
                 *
                 * 壳只做两件**平台能力**的事：探本机装了哪些嗓子、照着内核的答案出声。
                 * `dsh_speech_plan` 已经把三层音源的顺序（词典自带 → 在线 → 系统离线）
                 * 与"挑哪个嗓子、切几段"都算好了。
                 *
                 * ⚠️ `online` 那一层**内核规划、外壳发**（`Speech.Speak` 的 online 分支：
                 *    内核 `dsh_speech_online_plan` 说发什么 → 壳 POST → 内核 `_accept` 解 SSE）——
                 *    起三层音源就都是真的了。这一段旧文案还写着"在线还没接"，
                 *    搬完没改，已更正。
                 */
                case "speech:speak":
                    /*
                     * 参数与参考实现 **逐位相同**：0=文本 1=词典名 2=音源 3=音色 4=语种
                     * 5=词典 id 6=keyText 7=这一次的响度覆盖 8=这一次的增益覆盖。
                     *
                     * ⚠️ 第 2/3/4/7/8 这几个**以前被丢掉了**（只读了 0 与 5），于是界面上
                     *    「平衡音量」拿 `loudness: 0` / `gainDb: 0` 去量"中性电平"时，
                     *    量到的其实是**当前设置值**下的电平 —— 算出来的补偿值会偏
                     *    （参考实现的 `SpeakOptions` 是有这些的）。
                     *    现在把它们装成一个 `overrides_json` 交给内核：**键在 = 强制这个值**。
                     *
                     * ★ 2026-09-24：**在线那一层走两段式**（见下方）——原来它在这条
                     *    UI 线程的调用里同步发 HTTP，在线合成慢的时候整扇窗最长 8 秒不动。
                     */
                    {
                        Speech.OnlinePending pending = null;
                        var immediate = Engine(engine, delegate (out string j) {
                            j = Speech.BeginSpeak(engine, ArgStr(args, 0), ArgStr(args, 5),
                                                  OverridesJson(args), out pending);
                        });
                        /* 不需要联网（词典录音 / 系统合成 / 缓存命中 / 用不了）：当场就是终态 */
                        if (pending == null) return immediate;
                        if (reply == null || reply.Taken)
                        {
                            /* 没有回包口那条路（诊断 / 单元验收）：就地发，保持"调用即结果" */
                            return Speech.FinishSpeak(engine, pending, Speech.PostOnline(pending));
                        }
                        var slot = pending;
                        RunHttpDeferred(shell, reply,
                            delegate { return Speech.PostOnline(slot); },
                            delegate (TranslateHttp.Reply r) {
                                return Speech.FinishSpeak(engine, slot, r);
                            });
                        return null;
                    }

                /*
                 * 词条正文里点了某个喇叭（`sound://` / `snd://`）：放**具体的某一段**
                 * （可能是例句），所以不挑口音、也不兜底换音源 —— 要的就是那一段（参考实现的原话）。
                 */                case "speech:sound":
                    return Engine(engine, delegate (out string j) {
                        j = Speech.Sound(engine, ArgStr(args, 0), ArgStr(args, 1));
                    });

                /* ── 发音：下面几条要内核先补东西────────────── */
                case "speech:status":
                    /*
                     * 参数与参考实现逐位相同：0=文本 1=词典名 2=词典 id 3=词条名。
                     * 后两个只在"这条路要放词典自带录音"时用得上（内核据此找那条录音）。
                     */
                    return SpeechStatusBuilder.Build(engine, ArgStr(args, 0), ArgStr(args, 2),
                                                     ArgStr(args, 1), ArgStr(args, 3));
                case "speech:setGains":
                    {
                        /*
                         * 写音量增益（参考实现的 `speech:setGains`）。
                         *
                         * 参数逐位相同：`args[0]` 是一整个 patch 对象
                         * （`{dictGainDb?, systemGainDb?}`，两个键都是**可空数字**）。
                         * 三种语义（传数字 / 传 null / 没这个键）与归一化都在内核里 ——
                         * 壳**不许**在心里把它化简成"两个数"，那正是把"没传"当成"清零"的地方。
                         *
                         * ⚠️ 词典 id 传 **null**：增益是"按**当前词典**存"的，而"当前是哪本"
                         *    在设置里（内核按 `currentDictId` 取、取不到兜底第一本）——
                         *    参考实现那一版用的就是 `CurrentDictionary()`，不是这条消息带来的入参。
                         *
                         * 回的是**整份 `speech:status` 视图**（含 `voiceGains`）：界面拿它重排滑块，
                         * 不必再补问一次（而"再问一次"会撞上那条过期回包的竞态）。
                         */
                        var patch = Arg(args, 0) ?? "{}";
                        if (patch == "null") patch = "{}";
                        Engine(engine, delegate (out string j) {
                            Dsh.SpeechGainsSet(engine, null, patch, Speech.VoiceCount(), out j);
                        });
                        return SpeechStatusBuilder.Build(engine, "", null, null, null);
                    }
                case "speech:testDoubao":
                    /*
                     * 「检测凭据」的语音那一半（管理窗那一页）：拿一个样本词**真发一次**
                     * 合成请求，逐项报回结果。
                     *
                     * 参数与参考实现逐位相同：0=音色（可空）1=语种（可空）。
                     * 两个都空 = 按设置里的英文 + 中文**各测一次**（那个"测几项"的约定
                     * 在内核，不在这一层）。
                     *
                     * ⚠️ 它会**真联网、按字符计费**，所以只由用户点按钮触发 ——
                     *    但它**不受"在线总开关"限制**：用户当然应该先测通了再打开它。
                     *
                     * ★ 2026-09-24：这几项也要发 HTTP（英文 + 中文各一次），所以与
                     *    `speech:speak` / `translate:*` 一样走**两段式** ——
                     *    否则点一下「检测凭据」，界面会僵住到两项都发完为止。
                     */
                    {
                        var plans = Speech.TestPlans(engine, ArgStr(args, 0), ArgStr(args, 1));
                        var needsHttp = false;
                        for (var i = 0; i < plans.Count; i++)
                        {
                            if (Speech.TestNeedsHttp(plans[i])) needsHttp = true;
                        }
                        if (!needsHttp || reply == null || reply.Taken)
                        {
                            /* 没有一项要发 / 没有回包口：就地做（与原来一模一样） */
                            var sb = new StringBuilder();
                            sb.Append('[');
                            for (var i = 0; i < plans.Count; i++)
                            {
                                if (i > 0) sb.Append(',');
                                sb.Append(Speech.OneTest(engine, plans[i]));
                            }
                            sb.Append(']');
                            return sb.ToString();
                        }
                        var list = plans;
                        RunHttpDeferred(shell, reply,
                            delegate {
                                /* 后台：只发 HTTP（不需要发的那些给 null，收尾那一段自己识别）*/
                                var replies = new List<TranslateHttp.Reply?>();
                                for (var i = 0; i < list.Count; i++)
                                {
                                    replies.Add(Speech.TestNeedsHttp(list[i])
                                                    ? Speech.PostTest(list[i])
                                                    : null);
                                }
                                return replies;
                            },
                            delegate (List<TranslateHttp.Reply?> replies) {
                                /* UI 线程：逐项收尾（kernel accept 在里面）*/
                                var sb = new StringBuilder();
                                sb.Append('[');
                                for (var i = 0; i < list.Count; i++)
                                {
                                    if (i > 0) sb.Append(',');
                                    sb.Append(Speech.OneTest(engine, list[i], replies[i]));
                                }
                                sb.Append(']');
                                return sb.ToString();
                            });
                        return null;
                    }
                case "speech:dictSamples":
                    /*
                     * 挑几条**真有录音**的词条（参考实现的「平衡音量」那条路的样本清单）。
                     *
                     * 「挑哪几条、每条真有没有录音、什么语种、扫了几条、哪句人话」**全在内核**
                     * （`dsh_speech_dict_samples`）；这一层只给每条补上可播地址 ——
                     * 地址怎么拼是外壳域的事（与 `speech:speak` 的 dict 那一层同一条规矩）。
                     *
                     * ⚠️ 今天**没有产品流程调它**（界面上的「平衡音量」按按需求删掉了，
                     *    参考实现亦然）：接口留着、诊断 `--dict-samples` 在验它。
                     *    **别看到"没人调"就把它删掉。**
                     */
                    return Speech.DictSamples(engine);

                /* ── 机器翻译（5）── 内核说"该发什么 / 回包是什么意思"，壳只发字节 ── */
                case "translate:status":
                    return Engine(engine, delegate (out string j) { Dsh.TranslateStatus(engine, out j); });

                /*
                 * 真翻一次。**这一条是"内核—平台—内核"那三段式的样子**（见 ShellBridge 顶上那段）：
                 *   ① 内核 `plan`：说该发给谁、带哪些头、请求体是什么；命中缓存时它**直接给译文**
                 *      （`needsHttp=false`），失败时它给的是**界面那份结果形状**（人话在 `message` 里）；
                 *   ② 这一段：把 plan 给的东西**原样**发出去（`TranslateHttp.Post`，它不解析任何东西）；
                 *   ③ 内核 `accept`：把 HTTP 状态 + 回包 + 耗时交回去，翻成人话、写缓存。
                 *
                 * ⚠️ 这一层**不认识失败的原因**（没开开关 / 没填 Key / 这门语言不支持），
                 *    也不认识回包的结构 —— 它只会"发"与"交回去"。
                 */
                case "translate:text":
                    {
                        var text = ArgStr(args, 0);
                        var dictTitle = ArgStr(args, 1);
                        var plan = Engine(engine, delegate (out string j) {
                            Dsh.TranslatePlan(engine, text, dictTitle, out j);
                        });

                        // 缓存命中 / 失败：原样交回去 —— 但**伪词条的地址要补上**（见 WithEntryUrl）
                        if (!ShellBridge.Flag(plan, "needsHttp", false)) return WithEntryUrl(plan);

                        /*
                         * ★ 这一条要发 HTTP：走**两段式**（后台发、回 UI 线程收尾），
                         *   否则整个界面会僵住最长 8 秒（`TranslateHttp.TimeoutMs`）。
                         * `reply == null`（诊断 / 单元验收那条点号协议）时退回同步做 ——
                         * 那边没有界面要保活，同步更简单，也保持"调用即结果"的形状。
                         */
                        var planForAccept = plan;
                        if (reply != null && !reply.Taken)
                        {
                            var url = ShellBridge.Text(plan, "url");
                            var headers = TranslateHttp.ParseHeaders(ShellBridge.Raw(plan, "headers"));
                            var body = ShellBridge.Text(plan, "body");
                            RunHttpDeferred(shell, reply,
                                delegate { return TranslateHttp.Post(url, headers, body); },
                                delegate (TranslateHttp.Reply r) {
                                    return AcceptTranslate(engine, planForAccept, r);
                                });
                            return null;
                        }

                        var reply0 = TranslateHttp.Post(
                            ShellBridge.Text(plan, "url"),
                            TranslateHttp.ParseHeaders(ShellBridge.Raw(plan, "headers")),
                            ShellBridge.Text(plan, "body"));
                        return AcceptTranslate(engine, plan, reply0);
                    }

                /*
                 * 「检测凭据」：**绕过缓存**真发一次请求（§B7 那条硬要求：检测要合并成一个按钮报多行）。
                 * 与 `translate:text` 的差别只有一处 —— 用一个固定样本词、并强制发请求。
                 */
                case "translate:test":
                    {
                        var plan = Engine(engine, delegate (out string j) {
                            Dsh.TranslatePlan(engine, "apple", null, out j);
                        });
                        if (!ShellBridge.Flag(plan, "needsHttp", false))
                        {
                            /* 没 Key / 语言不支持：把计划给的人话原样报上去（`message` 就是原因）*/
                            return "{\"ok\":false,\"message\":" + Dispatch.Quote(ShellBridge.Text(plan, "message")) +
                                   ",\"elapsedMs\":0,\"tokens\":0,\"sample\":\"apple\",\"translation\":\"\"}";
                        }
                        if (reply != null && !reply.Taken)
                        {
                            var url = ShellBridge.Text(plan, "url");
                            var headers = TranslateHttp.ParseHeaders(ShellBridge.Raw(plan, "headers"));
                            var body = ShellBridge.Text(plan, "body");
                            var planForAccept = plan;
                            RunHttpDeferred(shell, reply,
                                delegate { return TranslateHttp.Post(url, headers, body); },
                                delegate (TranslateHttp.Reply r) {
                                    return AcceptTranslateTest(engine, planForAccept, r);
                                });
                            return null;
                        }
                        var reply0 = TranslateHttp.Post(
                            ShellBridge.Text(plan, "url"),
                            TranslateHttp.ParseHeaders(ShellBridge.Raw(plan, "headers")),
                            ShellBridge.Text(plan, "body"));
                        return AcceptTranslateTest(engine, plan, reply0);
                    }

                /*
                 * 改翻译设置。界面的三种就是 §B8 那三个键（`dictionaryFirst` 已在 2026-09 删掉，
                 * 它是"勾了没作用"的开关）—— 壳把整份 patch 包进 `{"translate":…}` 递下去，
                 * **归一化与默认值由内核定**（这一版 `translate` 还是设置模型里的"未识别字段"，
                 * 原样搬运；正式建模时一起搬（那两条"还没做的"）。
                 */
                case "translate:setSettings":
                    {
                        var patch = Arg(args, 0);
                        if (string.IsNullOrEmpty(patch) || patch == "null") patch = "{}";
                        var json = Engine(engine, delegate (out string j) {
                            Dsh.EngineSettingsSet(engine, "{\"translate\":" + patch + "}", out j);
                        });
                        var applied = ShellBridge.Raw(json, "translate") ?? "{}";
                        /* **广播**（不是只回调用方）：翻译设置是在选项窗里改的，悬浮窗也要跟着变 */
                        if (shell != null) shell.Broadcast("translate:settings-changed", applied);
                        return applied;
                    }

                case "translate:clearCache":
                    // 清缓存这一条界面**不看返回值**（清完它重问一次 status）；照参考实现回 true
                    Engine(engine, delegate (out string j) { Dsh.TranslateClearCache(engine, out j); });
                    return "true";

                /* ── 调试钩子（9）────────────────────────────────────────────── */
                case "debug:window":
                    // 角色：floating（默认）/ manager / tray —— 参考实现的 `debug:window` 同
                    return shell.DescribeWindow(ArgStr(args, 0) ?? "floating");
                case "debug:absorb":
                    shell.AbsorbToEdge(ArgStr(args, 0));
                    return "true";
                case "debug:placePill":
                    shell.PlacePill(ArgNum(args, 0) ?? 0, ArgNum(args, 1) ?? 0);
                    return "true";
                case "debug:dragStats":
                    return shell.DragStats();
                case "debug:importDictionary":
                    /*
                     * 按路径直接加一本（不弹对话框）—— 自动化用它摆场面。
                     * ⚠️ 与参考实现同一条纪律：**受 `LOOKUP_DEBUG_HOOKS` / `LOOKUP_DEBUG_PORT` 管**，
                     *    没开就当场拒（不能让它变成一条"谁都能悄悄往词库里塞东西"的路）。
                     */
                    if (!DebugHooksEnabled())
                        throw new InvalidOperationException("调试钩子未启用");
                    shell.ImportDictionary(ArgStr(args, 0));
                    return DictList(engine);
                case "debug:unloadDictionary":
                    /*
                     * 「把一本词典从内存里请出去、词库里留着」—— 参考实现有这个钩子，
                     * 存在的理由只有一个：**导入那条路会顺手加载**，所以"导入过"就等于
                     * "已经加载过"，而借查那个 bug 偏偏只在**没加载**时出现。
                     *
                     * ⚠️ 0.2.0 的内核**没有对外的"卸载一本"接口**（`dsh_dict_close` 关的是一个
                     *    词典句柄，不是引擎里那套"哪几本已加载"）。按硬规则①，这个判断在核心里，
                     *    壳不许自己造一个 —— 所以这一条**如实说还没接**（那条
                     *    借查接口与它同一批活）。
                     */
                    if (!DebugHooksEnabled())
                        throw new InvalidOperationException("调试钩子未启用");
                    return Todo(method);
                case "debug:focus":
                    // 参考实现同：靠开关别的窗口挪焦点不可靠，所以给一个明确的钩子
                    shell.FocusSelf();
                    return "true";
                case "debug:showTrayMenu":
                    shell.TrayMenuShow();
                    return "true";
                case "debug:hideTrayMenu":
                    shell.TrayMenuHide();
                    return "true";
                /* 只为 那条异步管线（见 SlowReply 的说明）；不做业务、不联网 */
                case "debug:slowReply":
                    return SlowReply(shell, reply, args);
                /*
                 * `debug:thread`：**这条请求是在哪条托管线程上被处理的**（2026-09-24 加）。
                 * 自检拿它当"UI 线程是哪一条"的基准，与 `debug:slowReply` 回的 `workThread`
                 * 对账 —— "干活那一段不在 UI 线程上"这件事只有对账才判得了（时间差不作数）。
                 */
                case "debug:thread":
                    return "{\"thread\":" +
                           Environment.CurrentManagedThreadId.ToString(CultureInfo.InvariantCulture) + "}";

                /* ── 系统集成（3）────────────────────────────────────────────── */
                case "clipboard:read":
                    // 回一段**文本**（JSON 字符串）；读不到就是空串，不是错误
                    return Dispatch.Quote(shell.ReadClipboard() ?? "");
                case "clipboard:write":
                    shell.WriteClipboard(ArgStr(args, 0));
                    return null;   // 参考实现这边回的是 null（界面不等值）
                case "shell:openPath":
                    shell.RevealInPath(ArgStr(args, 0));
                    return "true";
                /*
                 * 配置目录的路径（2026-09 加，给「常规 → 数据」那一组用）：
                 * 页面拿它显示"设置 / 历史 / 缓存都在哪儿"，并把同一个字符串递给 `shell:openPath`。
                 *
                 * ⚠️ 它与上面那条在**两张路由表**里各有一份：页面走的是这张（冒号名，
                 *    `ShellBridge`），诊断 / 走的是 `Dispatch` 那张（点号名
                 *    `shell.configDir`）。两张表各有一条断言盯着（见 `Runner.cs`）。
                 */
                case "shell:configDir":
                    return Dispatch.Quote(shell.ConfigDirPath() ?? "");

                /* ── 偏好（2）────────────────────────────────────────────────── */
                case "settings:get":
                    // 界面只读 `closeBehavior` 与 `showFloatingOnStartup`（bridge.js 里那两条
                    // `.then(settings => …)`），其余字段是超集，留着不碍事
                    return Engine(engine, delegate (out string j) { Dsh.EngineSettingsGet(engine, out j); });

                case "settings:setCloseBehavior":
                    {
                        /*
                         * 界面递的是三个线名之一：`quit` / `tray` / 别的（= 询问）。
                         * 内核的规范取值就是 `quit|tray|ask`，**判断照旧不在壳里** ——
                         * 壳只把那个字符串包成一条补丁递下去，规范化由内核做。
                         */
                        var wire = ArgStr(args, 0);
                        var value = wire == "quit" ? "quit" : wire == "tray" ? "tray" : "ask";
                        var json = Engine(engine, delegate (out string j) {
                            Dsh.EngineSettingsSet(engine, "{\"closeBehavior\":\"" + value + "\"}", out j);
                        });
                        return RawValue(json, "closeBehavior") ?? Dispatch.Quote(value);
                    }

                case "settings:setShowFloatingOnStartup":
                    {
                        /*
                         * 「启动时显示悬浮窗」（2026-09 加）：与 `closeBehavior` 同一档的
                         * **窗口级**设置，住在设置文件顶层（内核建了模，坏值会落回默认）。
                         * 壳只把页面递来的那个布尔值包成补丁 —— **默认值、坏值怎么退由内核说了算**。
                         *
                         * ⚠️ 回给页面的是**内核认可的那个值**（不是"我们刚递下去的那个"）：
                         *    写不进去时页面要靠它把勾选退回去，不许"勾上了其实没生效"。
                         * ⚠️ 它**下一次启动**才影响显示（这一次已经在跑了）。
                         */
                        var json = Engine(engine, delegate (out string j) {
                            Dsh.EngineSettingsSet(
                                engine,
                                "{\"showFloatingOnStartup\":" + (ArgBool(args, 0, true) ? "true" : "false") + "}",
                                out j);
                        });
                        var actual = RawValue(json, "showFloatingOnStartup");
                        return actual == "false" ? "false" : "true";
                    }

                /* ── 发音：改设置 / 清缓存 ───────────────────────────────────── */
                case "speech:setSettings":
                    {
                        /*
                         * 界面递的是一整个 `Partial<SpeechSettings>`（rate / accent / voiceId /
                         * doubao* 那几个）。内核把这些键**全部建模**在设置的 `speech` 节下，
                         * 所以壳只做一件事：把它包进 `{"speech": …}` 再递下去（规范化、夹范围、
                         * 清 `defaultLanguage` 都是内核的事）。
                         * 回给界面的是 `speech` 那一节（界面拿它重排表单）。
                         */
                        var patch = Arg(args, 0);
                        if (string.IsNullOrEmpty(patch) || patch == "null") patch = "{}";
                        var json = Engine(engine, delegate (out string j) {
                            Dsh.EngineSettingsSet(engine, "{\"speech\":" + patch + "}", out j);
                        });
                        /*
                         * 设置一变，**两扇窗都要跟上** —— 参考实现两边都发这个事件。
                         * ⚠️ 原来写的是 `shell.Emit`（只回发起方那一页），而这条请求是**选项窗**发的，
                         *    于是悬浮窗一条都收不到（2026-09 实测：订阅计数 **0**）——
                         *    与"切词典灰字不变"是同一个 bug 的第二次出现，所以一起走 `Broadcast`。
                         */
                        if (shell != null) shell.Broadcast("speech:settings-changed", RawValue(json, "speech"));
                        return RawValue(json, "speech") ?? "null";
                    }
                case "speech:clearCache":
                    // 清掉壳里那块**内存**合成缓存（磁盘缓存内核还没实现，见）
                    Speech.ClearCache();
                    return "true";

                /* ── 文本（1）────────────────────────────────────────────────── */
                case "text:script":
                    {
                        /*
                         * 界面要的是那**三种**之一（`'han' | 'latin' | 'other'`），内核回的是一整个
                         * `{script,hasSeparatorDots,withoutSeparatorDots,wordCount,isSingleChar}`。
                         * 只取 `script`。判定**在内核**（`dsh_language.c`），壳一个字都不判 ——
                         * 界面之所以问，是因为"汉字输入不在当前词典做联想"（参考实现  B 第 2 条）。
                         */
                        var json = Engine(engine, delegate (out string j) { Dsh.TextAnalyze(ArgStr(args, 0), out j); });
                        return RawValue(json, "script") ?? "\"other\"";
                    }

                default:
                    throw new InvalidOperationException("未知的接口: " + method);
            }
        }

        /* ══════════════════════════════════════════════════════════════════════
           词库清单：内核的字段 → 界面 `DictionaryInfo` 的字段
           ══════════════════════════════════════════════════════════════════════ */

        /// <summary>把内核的词库清单翻成界面那份 `DictionaryInfo[]`（只搬字段，不判断）</summary>
        private static string DictList(IntPtr engine)
        {
            string listJson;
            Engine(engine, delegate (out string j) { Dsh.EngineDictList(engine, out j); }, out listJson);

            var current = "";
            try
            {
                var settings = Engine(engine, delegate (out string j) { Dsh.EngineSettingsGet(engine, out j); });
                current = Text(RawValue(settings, "currentDictId")) ?? "";
            }
            catch (Exception) { /* 设置读不出来时"没有当前词典"是安全的默认 */ }

            var items = SplitArray(listJson);
            var sb = new StringBuilder("[");
            for (var i = 0; i < items.Count; i++)
            {
                var it = items[i];
                var id = Text(RawValue(it, "id")) ?? "";
                var unavailable = Text(RawValue(it, "unavailable")) ?? "";
                var loaded = RawValue(it, "loaded") == "true";

                string status, error = null;
                if (unavailable == "missing")
                {
                    // 文件不在了：界面那一屏认 `status === 'error'` + 非空 `errorMessage`
                    status = "error";
                    error = Text(RawValue(it, "note")) ?? "";
                }
                else if (loaded) status = "ready";
                /*
                 * ★ **"没加载"不等于"正在载入"**（用户 2026-09 报的：输入框右侧灰字里
                 *    "词典名后面永远跟着「· 载入中」"）。
                 *
                 * 0.2.0 的词典是**按需加载**的（第一次在它里面查词时才读进来），
                 * 所以"当前词典"在大多数时刻本来就**没加载** —— 原来这一支给的是
                 * `pending`，而界面把 `pending` 读成"载入中"（参考实现的约定：那一版导入时
                 * 顺手 WarmUp，所以 pending 真的是个短暂的过渡态）⇒ 于是那句话**永远挂着**。
                 *
                 * 检查标准只有一条：**这一本现在能不能用**。文件在、也没被标成坏掉，那它就是
                 * 能用的（点下去会按需读进来），所以给 `ready`；真的用不了（文件丢了 /
                 * 头坏了）走上面那一支 `error` —— 那一支才该报。
                 */
                else status = "ready";

                if (i > 0) sb.Append(',');
                sb.Append("{\"id\":").Append(Dispatch.Quote(id));
                sb.Append(",\"title\":").Append(RawValue(it, "title") ?? "null");

                var custom = RawValue(it, "customTitle");
                if (custom != null && custom != "null") sb.Append(",\"customTitle\":").Append(custom);

                sb.Append(",\"mdxPath\":").Append(RawValue(it, "mdxPath") ?? "null");
                sb.Append(",\"mddPaths\":").Append(RawValue(it, "mddPaths") ?? "[]");
                sb.Append(",\"addedAt\":").Append(RawValue(it, "addedAt") ?? "0");
                sb.Append(",\"fileName\":").Append(RawValue(it, "fileName") ?? "null");
                sb.Append(",\"fileSize\":").Append(RawValue(it, "fileSize") ?? "0");
                /*
                 * ⚠️ 这三个字段**内核给什么就照抄什么**（那一轮改）：
                 *    它们原来是被写死的 `0/""/""`（那会儿内核清单不读文件头，注释里写着
                 *    "要真值得按需问 `dsh_dict_info`……等管理窗那一轮再说"）。
                 *    现在内核清单**按需开一次头**就把书名 / 条目数 / 编码 / 版本读回来了
                 *    （见 `dsh_engine.c` 的 `emit_dict_item`），壳这一层只搬字段、一个字都不判。
                 */
                sb.Append(",\"entryCount\":").Append(RawValue(it, "entryCount") ?? "0");
                sb.Append(",\"encoding\":").Append(RawValue(it, "encoding") ?? "\"\"");
                sb.Append(",\"version\":").Append(RawValue(it, "version") ?? "\"\"");
                sb.Append(",\"status\":").Append(Dispatch.Quote(status));
                sb.Append(",\"current\":").Append(id.Length > 0 && id == current ? "true" : "false");
                if (error != null) sb.Append(",\"errorMessage\":").Append(Dispatch.Quote(error));
                sb.Append('}');
            }
            sb.Append(']');
            return sb.ToString();
        }

        /// <summary>
        /// 词库变了：**广播给所有开着的窗口**（参考实现的 `App.BroadcastDictionaries()`）。
        ///
        /// ⚠️ 这里**不能**只发给发起请求的那一页：用户是在选项窗里切词典 / 改名的，
        /// 而"当前词典名"那行灰字长在**悬浮窗**上 —— 只回给调用方的话它永远不变
        /// （用户 2026-09 报的 bug）。扇出由 App 那一侧做（Host 这一层看不见窗口，
        /// 见 `IShellHost.BroadcastDictionaries` 的说明）。
        /// </summary>
        private static void EmitDicts(IShellHost shell, IntPtr engine)
        {
            if (shell == null) return;
            shell.Broadcast("dictionaries:changed", DictList(engine));
        }

        /// <summary>`"文本"` → 文本（不是字符串就给 null）</summary>
        private static string Text(string raw)
        {
            return DecodeString(raw);
        }

        /// <summary>对象里 `"key": 数字` 的整数值（没有 / 不是数字 = 0）。只用于实测结果，不用于判断。</summary>
        private static int CountOf(string json, string key)
        {
            var raw = RawValue(json, key);
            double value;
            if (!string.IsNullOrEmpty(raw) &&
                double.TryParse(raw, NumberStyles.Float, CultureInfo.InvariantCulture, out value))
            {
                return (int)value;
            }
            return 0;
        }

        /// <summary>
        /// 把内核给的译文伪词条**存起来并补上地址**（参考实现 `TranslateEntry.Put` + `UrlFor`）。
        ///
        /// 内核给的是 `entry:{dictId,dictTitle,fromCache,entryToken,entryHtml}`（
        /// `dsh_translate_api.c` 的 `build_entry`）—— 文档内容、虚拟 id、标题栏那句话**全在内核**，
        /// 这一层只做两件**平台**的事：把 HTML 存进那张表（`TranslateDocs`）、按令牌拼一条 URL。
        ///
        /// 回的是**原样那份 JSON + 三个界面要读的字段**（`entryUrl` / `dictId` / `dictTitle`）：
        /// 界面那条 `renderTranslation` 读的正是这三个（少了它们，伪词条的 iframe 会拿到
        /// `undefined` 地址 —— 症状是"点了翻译，正文框一片空白"）。
        /// </summary>
        private static string WithEntryUrl(string translateJson)
        {
            if (string.IsNullOrEmpty(translateJson)) return translateJson;
            var entry = ShellBridge.Raw(translateJson, "entry");
            if (string.IsNullOrEmpty(entry) || entry == "null") return translateJson;

            var token = ShellBridge.Text(entry, "entryToken") ?? "";
            var html = ShellBridge.Text(entry, "entryHtml") ?? "";
            if (token.Length == 0) return translateJson;
            TranslateDocs.Store(token, html);

            var extra = ",\"entryUrl\":" + Dispatch.Quote(TranslateDocs.UrlFor(token)) +
                        ",\"dictId\":" + (ShellBridge.Raw(entry, "dictId") ?? "\"\"") +
                        ",\"dictTitle\":" + (ShellBridge.Raw(entry, "dictTitle") ?? "\"\"") +
                        ",\"fromCache\":" + (ShellBridge.Flag(entry, "fromCache", false) ? "true" : "false");
            var at = translateJson.LastIndexOf('}');
            if (at < 0) return translateJson;
            return translateJson.Substring(0, at) + extra + "}";
        }

        /// <summary>调一次内核，把 JSON 取出来（`engine` 没建起来就如实报）</summary>
        private static string Engine(IntPtr engine, Dispatch.EngineCallDelegate call)
        {
            string json;
            Engine(engine, call, out json);
            return json;
        }

        private static void Engine(IntPtr engine, Dispatch.EngineCallDelegate call, out string json)
        {
            if (engine == IntPtr.Zero) throw new InvalidOperationException("引擎还没建起来");
            call(out json);
            if (json == null) json = "null";
        }

        /// <summary>
        /// 入口名 → 内核的 `dsh_origin`。
        /// 与 0.2.0 那套点号协议**同一张表**（`Dispatch.Origin`）—— 两处都是"名字翻译"，
        /// 不是判断："这个入口跑不跑兜底通道"是内核的规则（`dsh_fallback` 那张表），
        /// 壳只报来源。
        /// </summary>
        /// <summary>
        /// 页面协议里的 `origin` —— 映射表在 <see cref="OriginMap"/>（**只有那一份**）。
        ///
        /// ⚠️ 认不出落 `input`（与 `Dispatch.Origin` 的"抛"不同，理由见那个方法的注释）：
        ///    界面只可能送那五个词，真出了意外也不该让"查词"整条挂掉。
        /// </summary>
        private static DshOrigin Origin(string name)
        {
            DshOrigin origin;
            return OriginMap.Try(name, out origin) ? origin : DshOrigin.Input;
        }

        /// <summary>
        /// 调试钩子开了没有（与参考实现同一条约定：`LOOKUP_DEBUG_PORT` 或 `LOOKUP_DEBUG_HOOKS`）。
        ///
        /// 为什么必须有一道闸：`debug:importDictionary` 这类钩子能**直接往词库里塞东西**，
        /// 不能让它变成"谁都能悄悄改用户词库"的一条路。
        /// </summary>
        private static bool DebugHooksEnabled()
        {
            return !string.IsNullOrEmpty(Environment.GetEnvironmentVariable("LOOKUP_DEBUG_HOOKS")) ||
                   !string.IsNullOrEmpty(Environment.GetEnvironmentVariable("LOOKUP_DEBUG_PORT"));
        }

        /// <summary>
        /// 「这一条还没接」。
        ///
        /// ⚠️ **必须吵**：搬家搬到一半时，最怕的是"界面看着好好的、点了没反应"。
        ///    这里回的是人话 + 指向进度那一节，页面会把它当异常抛出来（`bridge.js` 的
        ///    `ok:false` → `new Error(error)`），现场一眼能看出是哪一条没接。
        /// </summary>
        private static string Todo(string method)
        {
            throw new InvalidOperationException(
                "壳还没接这条：`" + method + "` —— 界面已经换成 参考实现那一份，" +
                "适配层还在按计划补（现在做完的是信封与路由表骨架）。");
        }

        /* ══════════════════════════════════════════════════════════════════════
           信封：应答与事件
           ══════════════════════════════════════════════════════════════════════ */

        /// <summary>`{kind:'res', id, ok:true, value:…}`（`value` 为 null 时给 JSON null，同参考实现）</summary>
        private static string Ok(long id, string valueJson)
        {
            var sb = new StringBuilder();
            sb.Append("{\"kind\":\"res\",\"id\":").Append(id.ToString(CultureInfo.InvariantCulture));
            sb.Append(",\"ok\":true,\"value\":");
            sb.Append(string.IsNullOrEmpty(valueJson) ? "null" : valueJson);
            sb.Append('}');
            return sb.ToString();
        }

        /// <summary>`{kind:'res', id, ok:false, error:"人话"}` —— `error` 是**字符串**（bridge.js 直接拿它 new Error）</summary>
        private static string Fail(long id, string message)
        {
            return "{\"kind\":\"res\",\"id\":" + id.ToString(CultureInfo.InvariantCulture) +
                   ",\"ok\":false,\"error\":" + Dispatch.Quote(message ?? "未知错误") + "}";
        }

        /// <summary>宿主 → 页面推一条事件（`{kind:'evt', name, payload}`）</summary>
        internal static string Event(string name, string payloadJson)
        {
            return "{\"kind\":\"evt\",\"name\":" + Dispatch.Quote(name) + ",\"payload\":" +
                   (string.IsNullOrEmpty(payloadJson) ? "null" : payloadJson) + "}";
        }

        /* ══════════════════════════════════════════════════════════════════════
           参数：`args` 是一个 JSON 数组
           ══════════════════════════════════════════════════════════════════════ */

        /// <summary>第 i 个参数的**原始 JSON**（越界 = null）</summary>
        internal static string Arg(List<string> args, int i)
        {
            return (args != null && i >= 0 && i < args.Count) ? args[i] : null;
        }

        /// <summary>第 i 个参数的文本（JSON 字符串才解；JSON null / 越界都给 null）</summary>
        internal static string ArgStr(List<string> args, int i)
        {
            return DecodeString(Arg(args, i));
        }

        /// <summary>第 i 个参数的数字（没有 / 不是数字 = null）</summary>
        internal static double? ArgNum(List<string> args, int i)
        {
            var raw = Arg(args, i);
            if (string.IsNullOrEmpty(raw) || raw == "null") return null;
            double value;
            if (double.TryParse(raw, NumberStyles.Float, CultureInfo.InvariantCulture, out value)) return value;
            return null;
        }

        /// <summary>第 i 个参数的布尔（没有 / 不是布尔 = fallback）</summary>
        internal static bool ArgBool(List<string> args, int i, bool fallback)
        {
            var raw = Arg(args, i);
            if (raw == "true") return true;
            if (raw == "false") return false;
            return fallback;
        }

        /// <summary>
        /// 把 `speech:speak` 那几个"这一次的覆盖"装成 `overrides_json`（内核的约定见
        /// `abi/lookup.abi.json` 里 `dsh_speech_plan` 的同名参数）。
        ///
        /// 位的对应关系照参考实现的 `SpeakOptions`：**2=音源 3=音色 4=语种 7=响度 8=增益**。
        /// 规则：**只有给了的才进 JSON**（键在 = 强制这个值，键不在 = 照设置）；
        /// 一个都没给就回 null（老调用方与老行为逐字相同）。
        ///
        /// ⚠️ 增益在这里就换算成 **dB×10 的整数**：接口定义没有浮点标量，而增益本来就取整到 0.1 dB
        ///    （与 `dsh_audio_apply_gain` 的入参同一条约定）。界面传的是小数（如 `0`、`-12`）。
        /// </summary>
        private static string OverridesJson(List<string> args)
        {
            var sb = new StringBuilder();
            var source = ArgStr(args, 2);
            if (!string.IsNullOrEmpty(source)) AppendJsonField(sb, "source", source);
            var voice = ArgStr(args, 3);
            if (!string.IsNullOrEmpty(voice)) AppendJsonField(sb, "voiceId", voice);
            var language = ArgStr(args, 4);
            if (!string.IsNullOrEmpty(language)) AppendJsonField(sb, "language", language);
            var loudness = ArgNum(args, 7);
            if (loudness.HasValue)
            {
                AppendJsonNumber(sb, "loudness",
                                 (long)Math.Round(loudness.Value, MidpointRounding.AwayFromZero));
            }
            var gain = ArgNum(args, 8);
            if (gain.HasValue)
            {
                AppendJsonNumber(sb, "gainTenthsDb",
                                 (long)Math.Round(gain.Value * 10.0, MidpointRounding.AwayFromZero));
            }
            if (sb.Length == 0) return null;
            return "{" + sb + "}";
        }

        private static void AppendJsonField(StringBuilder sb, string name, string value)
        {
            if (sb.Length > 0) sb.Append(',');
            sb.Append('"').Append(name).Append("\":\"").Append(Escape(value)).Append('"');
        }

        private static void AppendJsonNumber(StringBuilder sb, string name, long value)
        {
            if (sb.Length > 0) sb.Append(',');
            sb.Append('"').Append(name).Append("\":").Append(value.ToString(CultureInfo.InvariantCulture));
        }

        /** JSON 字符串转义（界面那边给的是音色 id / 语种码，正常不会有特殊字符，但别赌）*/
        private static string Escape(string text)
        {
            var sb = new StringBuilder(text.Length + 8);
            foreach (var ch in text)
            {
                switch (ch)
                {
                    case '"': sb.Append("\\\""); break;
                    case '\\': sb.Append("\\\\"); break;
                    case '\n': sb.Append("\\n"); break;
                    case '\r': sb.Append("\\r"); break;
                    case '\t': sb.Append("\\t"); break;
                    default: sb.Append(ch); break;
                }
            }
            return sb.ToString();
        }

        /* ══════════════════════════════════════════════════════════════════════
           从一段 JSON 里读字段（给**壳那一侧**用）
           ══════════════════════════════════════════════════════════════════════
           `layout:set` 递下来的是**一整个对象**（`{width,panelHeight,direction,align,regions}`）、
           `shape:set` 递下来的是一**个数组**，都是参考实现的线格式（见那边的 `Models.cs`）。
           窗口层要读它们，但 `Lookup.App` 里没有 JSON 库 —— 也不该为它引一个：
           这两个小读取器与上面那套 args 助手是同一件事，放在一处就够了。 */

        /** `{"key":值}` → 那个值的**原始 JSON**（对象 / 数组 / 字符串都按配对取；没有 = null）。
         *
         *  用途：内核给的那几段（`headers` 数组、`speech` 子对象、`translate` 那一节）
         *  要**原样**递给别处（HTTP 头、界面、设置补丁），壳不该先解析再拼回来 ——
         *  那样就等于在这一层重新理解了一遍载荷。 */
        internal static string Raw(string json, string key)
        {
            return RawValue(json, key);
        }

        /// <summary>`{"key":"文本"}` → 文本（没有 / 不是字符串 = null）</summary>
        internal static string Text(string json, string key)
        {
            return DecodeString(RawValue(json, key));
        }

        /// <summary>`{"key":数字}` → 数字（没有 / 不是数字 = null）</summary>
        internal static double? Num(string json, string key)
        {
            var raw = RawValue(json, key);
            if (string.IsNullOrEmpty(raw) || raw == "null") return null;
            double value;
            if (double.TryParse(raw, NumberStyles.Float, CultureInfo.InvariantCulture, out value)) return value;
            return null;
        }

        /// <summary>`{"key":真值}` → 布尔（没有 = fallback）</summary>
        internal static bool Flag(string json, string key, bool fallback)
        {
            var raw = RawValue(json, key);
            if (raw == "true") return true;
            if (raw == "false") return false;
            return fallback;
        }

        /// <summary>
        /// 把一个 JSON 数组切成**每个元素的原始 JSON**（字符串、对象、数组都算一个元素）。
        ///
        /// ⚠️ 不能用"找下一个逗号"那种偷懒写法：`args` 里常常整段是对象
        ///    （`speech:setSettings` 的第 0 个参数就是一整个 patch 对象），
        ///    里面带逗号，切错了会把一个对象拆成两半。
        ///
        /// ⚠️ **最后一个元素是靠 `]` 收尾的**（前面那些靠 `,`）—— 这一条第一版写错了：
        ///    收尾那一步只在 `depth == 1` 时记账，而 `]` 会把 depth 减到 0，
        ///    于是**只有一个参数的调用**（`clipboard:write("x")`、`scriptOf("苹果")`）
        ///    整个参数被丢掉 —— 症状是"写剪贴板变成清空剪贴板""分析文本说 text 不能为空"。
        ///    是 脚本当场发现的。
        /// </summary>
        internal static List<string> SplitArray(string json)
        {
            var items = new List<string>();
            if (string.IsNullOrEmpty(json)) return items;
            var text = json.Trim();
            if (text.Length < 2 || text[0] != '[') return items;

            var depth = 0;
            var inString = false;
            var start = -1;
            // 元素的**终点**：字符串是那个收尾引号、容器是那个收尾括号、标量是它最后一个字符。
            // ⚠️ 必须单独记 —— 第一版是"遇到 `,` / `]` 就把切到那儿为止"，于是
            //    字符串元素被切成了 `"文本"]`（把收尾的 `]` 也带进去了），
            //    写进剪贴板的内容就多两个字符。同样是诊断当场发现的。
            var end = -1;
            for (var i = 0; i < text.Length; i++)
            {
                var c = text[i];
                if (inString)
                {
                    if (c == '\\') { i++; continue; }
                    if (c == '"') { inString = false; if (start >= 0) end = i; }
                    continue;
                }
                if (c == '"')
                {
                    inString = true;
                    if (start < 0 && depth == 1) { start = i; end = i; }
                    continue;
                }
                if (c == '[' || c == '{')
                {
                    depth++;
                    if (start < 0 && depth == 2) { start = i; end = i; }
                    continue;
                }
                if (c == ']' || c == '}')
                {
                    depth--;
                    if (start >= 0 && depth == 1)
                    {
                        // 一个容器元素收尾（它的起点是 `{` / `[`）
                        end = i;
                        Take(items, text, ref start, ref end);
                    }
                    else if (start >= 0 && depth == 0)
                    {
                        // 整个数组收尾：最后一个元素在这儿结束（终点上面已经记好了）
                        Take(items, text, ref start, ref end);
                    }
                    if (depth <= 0) break;
                    continue;
                }
                if (c == ',' && depth == 1)
                {
                    // 只有容器元素需要在这儿收尾（字符串/标量的终点上面已经记好了）
                    if (start >= 0 && end < i) end = i - 1;
                    Take(items, text, ref start, ref end);
                    continue;
                }
                if (depth == 1)
                {
                    if (start < 0 && !char.IsWhiteSpace(c)) { start = i; end = i; }
                    else if (start >= 0 && !char.IsWhiteSpace(c)) end = i;
                }
            }
            return items;
        }

        /// <summary>取一个元素（从 start 到 end，含两端），取完清位</summary>
        private static void Take(List<string> items, string text, ref int start, ref int end)
        {
            if (start < 0) return;
            if (end < start) end = start;
            if (end >= text.Length) end = text.Length - 1;
            var piece = text.Substring(start, end - start + 1).Trim();
            if (piece.Length > 0) items.Add(piece);
            start = -1;
            end = -1;
        }

        /// <summary>`"key": 值` 那一段的原始 JSON（对象与数组都按配对取；没有 = null）</summary>
        private static string RawValue(string json, string key)
        {
            if (json == null) return null;
            var pat = "\"" + key + "\"";
            var at = json.IndexOf(pat, StringComparison.Ordinal);
            if (at < 0) return null;
            at += pat.Length;
            while (at < json.Length && (json[at] == ' ' || json[at] == ':')) at++;
            if (at >= json.Length) return null;

            var c0 = json[at];
            if (c0 == '"')
            {
                var end = at + 1;
                while (end < json.Length)
                {
                    if (json[end] == '\\') { end += 2; continue; }
                    if (json[end] == '"') break;
                    end++;
                }
                return json.Substring(at, Math.Min(end, json.Length - 1) - at + 1);
            }
            if (c0 == '{' || c0 == '[')
            {
                var close = c0 == '{' ? '}' : ']';
                var depth = 0;
                var inString = false;
                for (var i = at; i < json.Length; i++)
                {
                    var c = json[i];
                    if (inString)
                    {
                        if (c == '\\') { i++; continue; }
                        if (c == '"') inString = false;
                        continue;
                    }
                    if (c == '"') { inString = true; continue; }
                    if (c == c0) depth++;
                    else if (c == close)
                    {
                        depth--;
                        if (depth == 0) return json.Substring(at, i - at + 1);
                    }
                }
                return null;
            }

            var stop = at;
            while (stop < json.Length && json[stop] != ',' && json[stop] != '}') stop++;
            return json.Substring(at, stop - at).Trim();
        }

        /// <summary>`"文本"` → 文本（顺带解转义）；不是字符串就给 null</summary>
        private static string DecodeString(string raw)
        {
            if (string.IsNullOrEmpty(raw) || raw[0] != '"') return null;
            if (raw.Length < 2) return "";
            var body = raw.Substring(1, raw.Length - 2);
            if (body.IndexOf('\\') < 0) return body;

            var sb = new StringBuilder(body.Length);
            for (var i = 0; i < body.Length; i++)
            {
                var c = body[i];
                if (c != '\\' || i + 1 >= body.Length) { sb.Append(c); continue; }
                var n = body[++i];
                switch (n)
                {
                    case 'n': sb.Append('\n'); break;
                    case 'r': sb.Append('\r'); break;
                    case 't': sb.Append('\t'); break;
                    case 'b': sb.Append('\b'); break;
                    case 'f': sb.Append('\f'); break;
                    case 'u':
                        {
                            if (i + 4 >= body.Length) { sb.Append(n); break; }
                            int code;
                            if (int.TryParse(body.Substring(i + 1, 4), NumberStyles.HexNumber,
                                             CultureInfo.InvariantCulture, out code))
                            {
                                sb.Append((char)code);
                                i += 4;
                            }
                            else sb.Append(n);
                            break;
                        }
                    case '"': sb.Append('"'); break;
                    case '\\': sb.Append('\\'); break;
                    case '/': sb.Append('/'); break;
                    default: sb.Append(n); break;
                }
            }
            return sb.ToString();
        }
    }
}
