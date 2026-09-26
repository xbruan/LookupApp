namespace Lookup.Host
{
    using System;
    using System.Globalization;
    using System.Text;
    using Lookup.Interop;

    /// <summary>
    /// **前端的唯一入口**：把一条 JSON 请求派发到内核的某一组接口，把内核的 JSON 原样包成应答
    /// （`{"id":1,"method":…,"params":{…}}` → `{"id":1,"ok":true,"result":…}`；出错则 `ok:false` + `error.message`）。
    /// 三条纪律之一：**字节不过桥** —— 图片 / 音频 / 字体走 `dictres.invalid` 那条资源路由
    /// （几 MB 音频塞进 JSON 再 base64 一遍是这一层最容易犯的错），所以这里**故意不提供** `lookup.resource`。
    /// 之二：**绝不把异常抛过边界**（内核错误码在这里翻成 `ok:false`，否则内核的错误模型漏进视图层）；
    /// 之三：**内核的 JSON 原样带出去**（不解析、不重排、不裁字段，一旦开始理解业务字段，界面就又多一个业务逻辑来源）。
    /// </summary>
    /// <summary>
    /// **页面做不了、只有壳做得了的那几件事**（`Dispatch` 到壳的一条窄缝）：挑文件（要点系统文件对话框）、
    /// 配置目录的路径（只有壳知道这一次跑的是哪个目录）、写剪贴板（正文住在 `sandbox` 的跨源 iframe 里，
    /// `navigator.clipboard` 那一套在那儿用不了，而"复制选中的文字"是正文那排浮层的动作之一）。
    /// ⚠️ 这一层**只转述**、不做判断：挑回来的路径原样交给内核（`engine.dictAdd` 自己判"能不能读、是不是 .mdx"）。
    /// </summary>
    internal interface IShellHost
    {
        /// <summary>弹一次"选词典"对话框，回选中的路径数组 JSON（取消 = `[]`）</summary>
        string PickDictionaryFiles();

        /**
         * 把一段活**丢回 UI 线程**去做。
         *
         * 为什么这一层需要它：机器翻译与在线发音是**两段式**的 —— 中间的 HTTP 在线程池上，
         * 而前后两段（内核 plan / accept）必须回到 UI 线程（内核没有并发守卫；回包也只能在 UI 线程发）。
         * ⚠️ 已经在 UI 线程上时**直接执行**（`InvokeRequired == false`）：`NoShellHost` 与假壳都靠这条保持同步执行。
         */
        void PostToUiThread(Action action);

        /**
         * 应用级设置**目录**的完整路径（`%APPDATA%\LookupApp`；`--config` 给过就是那一个）。
         *
         * 为什么由壳给：只有壳知道这一次跑的是哪个目录（含"从 `%APPDATA%\查词` 迁移"那一步的结果）。
         * 页面拿它两用：显示在「常规 → 数据」那一组里，以及「打开配置目录」点下去时交给 `RevealInPath`。
         */
        string ConfigDirPath();

        /**
         * **广播**一条事件给所有开着的窗口（不是只回发起请求的那一页）。
         *
         * ⚠️ 为什么需要它：`ManagerWindow.Emit` 只推自己那一页（**不转发**），而"词库变了 / 语音设置变了 / 翻译设置变了"这类事件**多半是在选项窗里发起的** —— 只回给调用方的话，**悬浮窗永远收不到**，它上面那些跟着设置变的东西就停在旧值上。
         * ⚠️ 反过来：**只为"某一页自己发起的那个动作"而存在**的事件继续用 `Emit`（`traymenu:login-changed` 就是）；而 `FloatingWindow.Emit` 本来就会顺带转发给管理窗与托盘菜单 —— 「广播」不是另一套扇出，只是"从哪一扇窗发起都走同一条路"的约定统一。
         */
        void Broadcast(string name, string payloadJson);

        /**
         * 把一段文字放进系统剪贴板（正文里「复制选中的文字」那条路）。
         * 空串按"清空剪贴板"处理 —— WinForms 的 `SetText` 不接受空串。
         */
        void WriteClipboard(string text);

        /// <summary>
        /// 读系统剪贴板里的文字（右键菜单的「粘贴」用它）。没有文字就回空串 ——
        /// 读不到**不是**错误（剪贴板里放着一张图也是读不到文字）。
        /// </summary>
        string ReadClipboard();

        /// <summary>
        /// 在资源管理器里定位一个文件 / 打开一个目录（管理窗里「打开所在文件夹」用它）。
        /// 路径不存在就什么都不做。
        /// </summary>
        void RevealInPath(string path);

        /// <summary>
        /// 宿主 → 页面推一条事件（参考实现那套桥里的 `{kind:'evt', name, payload}`）。
        /// 为什么它属于"壳"：**发消息是窗口层的能力**（只有窗口手里有 `CoreWebView2`），
        /// 而"什么时候该发哪一条"由搬过来的界面决定；这一层只负责把它送出去，`payload` 是**已经拼好的 JSON**。
        /// </summary>
        void Emit(string name, string payloadJson);

        /* ── 窗口几何与形状：页面知道"内容该多高、形状长什么样"，但"窗口摆在哪、多大、
         * Region 怎么裁、拖动怎么跟手"只有窗口层做得了。参数一律用 **参考实现的线格式 JSON**
         * （`LayoutRequest` / `ShapeRect[]`）—— 壳这一层只转发，不重新定义一套。 */

        /// <summary>`layout:info`：页面开局就要知道的布局信息（工作区 / 缩放 / 胶囊在哪）</summary>
        string LayoutInfo();

        /// <summary>`layout:set`（发完不管）：`{width,panelHeight,direction,align,regions}`</summary>
        void SetLayout(string requestJson);

        /// <summary>`shape:set`（发完不管）：`[{x,y,w,h,r,fill,border},…]` + 主题 + 有没有焦点</summary>
        void SetShape(string regionsJson, string theme, bool focused);

        void DragPrepare();
        void DragStart();
        void DragMove(double? sentAtMs);
        void DragEnd();

        /// <summary>`edge:expand` / `edge:collapse` / `floating:reset` / `debug:absorb`</summary>
        void ExpandFromEdge();
        void CollapseToEdge();
        void ResetPill();
        void AbsorbToEdge(string edgeWire);

        /// <summary>`window:hide`</summary>
        void HideWindow();

        /// <summary>`window:requestClose`：按设置里的 closeBehavior 决定问一句还是直接办</summary>
        void RequestClose();

        /// <summary>`window:resolveClose`：用户在"关闭时怎么办"那个对话框上选了什么</summary>
        void ResolveClose(string choice, bool remember);

        /// <summary>`debug:window`：窗口矩形 / Region / 采样点命中（**问系统，不靠截图**）</summary>
        string DescribeWindow(string role);

        /// <summary>`debug:dragStats`：拖动的跟手统计</summary>
        string DragStats();

        /// <summary>`debug:placePill`：直接把胶囊摆到某个位置（DIP）</summary>
        void PlacePill(double x, double y);

        /// <summary>`debug:importDictionary`：按路径加一本（不弹对话框）</summary>
        void ImportDictionary(string path);

        /* ── 词库管理窗（参考实现的第二扇窗）：这一组**必须由应用那一侧实现**（它知道另一扇窗在不在、
         * 开没开），而不是窗口自己 —— 所以接口里只有"请打开 / 请关掉"这种说法，
         * 没有"窗口对象"这种概念（操作系统相关的那一层不该认识彼此）。 */

        /// <summary>`manager:open`：打开（或唤回）管理窗，可以指定翻到哪一页</summary>
        void OpenManager(string tab);

        /// <summary>`manager:close`（其实是收起来，不是销毁）</summary>
        void ManagerClose();

        /// <summary>`manager:minimize`</summary>
        void ManagerMinimize();

        /// <summary>`manager:initialTab`：渲染进程开机时来拉一次"这次要翻到哪页"（取过就清）</summary>
        string ManagerConsumeInitialTab();

        /// <summary>`manager:drag`：标题栏按下 → 交给系统跑移动循环</summary>
        void ManagerBeginDrag();

        /// <summary>`manager:surface`：渲染进程上报界面底色与焦点（管理窗整窗一块卡片）</summary>
        void ManagerSurface(string fill, string border, string theme, bool focused);

        /* ── 托盘菜单（参考实现的第三扇窗）──────────────────────────────────────── */

        /// <summary>弹出托盘菜单（托盘图标点一下 / `debug:showTrayMenu`）</summary>
        void TrayMenuShow();

        /// <summary>`traymenu:close`：把菜单收掉</summary>
        void TrayMenuHide();

        /// <summary>`traymenu:measured`：页面量好的内容尺寸（**DIP**）+ 底色 + 主题</summary>
        void TrayMenuPlace(double width, double height, string fill, string border, string theme);

        /// <summary>`traymenu:state`：悬浮窗现在可见吗 + 有没有登记开机自启动</summary>
        string TrayState();

        /// <summary>`traymenu:login`：登记 / 取消开机自启动，回**系统里的真实状态**</summary>
        bool TraySetLogin(bool enabled);

        /// <summary>`traymenu:toggle`：悬浮窗在就收起来、不在就唤出来</summary>
        void TrayToggleFloating();

        /// <summary>`traymenu:reset`：把胶囊摆回默认位置</summary>
        void TrayResetPill();

        /// <summary>`traymenu:quit`：退出程序</summary>
        void TrayQuit();

        /// <summary>
        /// `debug:focus(role)`：把某个窗口**明确**拿到前台。
        /// 为什么需要它：**靠开关别的窗口来挪焦点不可靠**（置顶窗口藏起来时前台可能落到别的程序上）；
        /// 而页面里"选中文字那排浮层"自带 `if (!document.hasFocus()) return`（这是对的：用户在看别的窗口时不该突然冒出来），
        /// 所以"验那条路"必须先把焦点交给那个窗口，否则永远验不出来。
        /// </summary>
        void FocusSelf();
    }

    /// <summary>没有壳的时候用（诊断 / 里直接喂 JSON 那种）：三件事都如实说做不到</summary>
    internal sealed class NoShellHost : IShellHost
    {
        internal static readonly NoShellHost Instance = new NoShellHost();
        public string PickDictionaryFiles()
        {
            throw new InvalidOperationException("这一版没有能弹文件对话框的壳（用 --dict 传词典路径）");
        }
        public string ConfigDirPath()
        {
            throw new InvalidOperationException("这一版没有壳，读不到配置目录");
        }

        /// <summary>没有壳 = 没有别的线程在等我们，直接就地做（与"没有窗口"那条路一致）</summary>
        public void PostToUiThread(Action action)
        {
            if (action != null) action();
        }
        public void WriteClipboard(string text)
        {
            throw new InvalidOperationException("这一版没有能写剪贴板的壳");
        }
        public string ReadClipboard()
        {
            throw new InvalidOperationException("这一版没有能读剪贴板的壳");
        }
        public void RevealInPath(string path)
        {
            throw new InvalidOperationException("这一版没有能打开资源管理器的壳（用 --dict 传词典路径）");
        }
        public void Emit(string name, string payloadJson) { /* 没有窗口要通知的 */ }
        public void Broadcast(string name, string payloadJson) { /* 同上：没有窗口，没有要广播的 */ }
        public string LayoutInfo()
        {
            throw new InvalidOperationException("这一版没有窗口（用 --dict 起壳才有界面）");
        }
        public void SetLayout(string requestJson) { /* 没有窗口要摆的 */ }
        public void SetShape(string regionsJson, string theme, bool focused) { }
        public void DragPrepare() { }
        public void DragStart() { }
        public void DragMove(double? sentAtMs) { }
        public void DragEnd() { }
        public void ExpandFromEdge() { }
        public void CollapseToEdge() { }
        public void ResetPill() { }
        public void AbsorbToEdge(string edgeWire) { }
        public void HideWindow() { }
        public void RequestClose() { }
        public void ResolveClose(string choice, bool remember) { }
        public string DescribeWindow(string role) { return "{\"present\":false}"; }
        public string DragStats() { return "{\"frames\":0,\"median\":0,\"p95\":0,\"max\":0,\"samples\":[]}"; }
        public void PlacePill(double x, double y) { }
        public void ImportDictionary(string path)
        {
            throw new InvalidOperationException("这一版没有能弹文件对话框的壳");
        }
        public void OpenManager(string tab)
        {
            throw new InvalidOperationException("这一版没有词库管理窗（用 --dict 传词典路径）");
        }
        public void ManagerClose() { }
        public void ManagerMinimize() { }
        public string ManagerConsumeInitialTab() { return ""; }
        public void ManagerBeginDrag() { }
        public void ManagerSurface(string fill, string border, string theme, bool focused) { }
        public void TrayMenuShow() { }
        public void TrayMenuHide() { }
        public void TrayMenuPlace(double width, double height, string fill, string border, string theme) { }
        public string TrayState() { return "{\"floatingVisible\":false,\"loginAtStartup\":false}"; }
        public bool TraySetLogin(bool enabled) { return false; }
        public void TrayToggleFloating() { }
        public void TrayResetPill() { }
        public void TrayQuit() { }
        public void FocusSelf() { }
    }

    internal static class Dispatch
    {
        /// <summary>处理一条请求（`requestJson` 是前端发来的那一串）</summary>
        internal static string Handle(IntPtr engine, string requestJson)
        {
            return Handle(engine, requestJson, NoShellHost.Instance);
        }

        /// <summary>处理一条请求（带壳的那条缝；见 <see cref="IShellHost"/>）</summary>
        /// <param name="reply">
        /// **稍后自己回包**用的口子（可选）。给 `null` 表示调用方只接受"当场回一条"——
        /// 于是"要联网"的那几条路会退回**同步**做（诊断 / 单元验收没有界面要保活，同步更简单）。
        /// ⚠️ 这个口子**必须是线程安全的**（后台线程会调它），见 `WebViewBridge.PostReply`。
        /// </param>
        internal static string Handle(IntPtr engine, string requestJson, IShellHost shell,
                                     Action<string> reply = null)
        {
            /*
             * 两条协议**并存，不是替换**（分工见 `ShellBridge` 顶上那段）：
             *   ① `{kind:'req' | 'notify', method, args, reply}` —— 参考实现那套桥，搬过来的界面（`web/bridge.js`）走这条；
             *   ② 下面这一整段点号协议 —— 0.2.0 自己的，脚本与 在用。靠 `kind` 一眼分得开，不必猜。
             */
            if (ShellBridge.IsBridgeRequest(requestJson))
            {
                return ShellBridge.Handle(engine, requestJson, shell ?? NoShellHost.Instance, reply);
            }

            string method = null;
            long id = 0;
            string paramsJson = null;

            try
            {
                if (string.IsNullOrWhiteSpace(requestJson))
                    return Fail(0, "请求是空的");
                method = VirtualHost.JsonString(requestJson, "method");
                if (string.IsNullOrEmpty(method)) return Fail(0, "请求里没有 method");
                id = VirtualHost.JsonLong(requestJson, "id");
                paramsJson = RawObject(requestJson, "params");
            }
            catch (Exception err)
            {
                return Fail(0, "请求读不动：" + err.Message);
            }

            try
            {
                var result = Route(engine, method, paramsJson, shell ?? NoShellHost.Instance);
                return Ok(id, result);
            }
            catch (InvalidOperationException err)
            {
                // 包装层抛出来的就是内核的 last_error（人话）
                return Fail(id, err.Message);
            }
            catch (Exception err)
            {
                return Fail(id, err.Message);
            }
        }

        /// <summary>路由表 —— **方法名到内核接口**，这是这一层唯一的"知识"</summary>
        private static string Route(IntPtr engine, string method, string p, IShellHost shell)
        {
            switch (method)
            {
                /* ── shell（不是内核的活、也不是页面能干的；见 IShellHost）─────── */
                case "shell.pickDictionaries":
                    return shell.PickDictionaryFiles();
                /* 配置目录的路径：页面拿它显示 + 递给 `shell.openPath` 打开那一格 */
                case "shell.configDir":
                    return Quote(shell.ConfigDirPath());
                /*
                 * 写剪贴板（正文那排浮层的「复制」）。⚠️ **空串是合法入参**（= 清空剪贴板），
                 * 所以这里**不能**用 `Need` —— 用 Need 会把「没传 text」当成空串，悄悄清掉用户的剪贴板。
                 */
                case "shell.writeClipboard":
                    {
                        var text = Maybe(p, "text");
                        if (text == null) throw new InvalidOperationException("缺少参数：text");
                        shell.WriteClipboard(text);
                        /* 回一段实测结果：验收要能分开"页面调了"与"真的写进去了" */
                        return "{\"ok\":true,\"bytes\":" +
                               Encoding.UTF8.GetByteCount(text).ToString(CultureInfo.InvariantCulture) +
                               "}";
                    }
                /* 无边框窗口那两件事不用接：界面用的是参考实现那一份 */
                /* ── core ─────────────────────────────────────────────────────── */
                case "core.version":
                    return Quote(Dsh.Version());
                case "core.abi":
                    return "{\"abi\":" + Dsh.KernelAbiVersion().ToString(CultureInfo.InvariantCulture) +
                           ",\"expected\":" + Dsh.AbiVersion.ToString(CultureInfo.InvariantCulture) +
                           ",\"ok\":" + (Dsh.KernelAbiVersion() == Dsh.AbiVersion ? "true" : "false") + "}";

                /* ── engine ───────────────────────────────────────────────────── */
                case "engine.settingsGet":
                    return EngineCall(engine, (out string j) => Dsh.EngineSettingsGet(engine, out j));
                case "engine.settingsSet":
                    return EngineCall(engine, (out string j) => Dsh.EngineSettingsSet(engine, Need(p, "patch"), out j));
                case "engine.dictList":
                    return EngineCall(engine, (out string j) => Dsh.EngineDictList(engine, out j));
                case "engine.dictAdd":
                    return EngineCall(engine, (out string j) => Dsh.EngineDictAdd(engine, Need(p, "pathsJson"), out j));
                case "engine.dictRemove":
                    return EngineCall(engine, (out string j) => Dsh.EngineDictRemove(engine, Need(p, "dictId"), out j));
                case "engine.dictRename":
                    return EngineCall(engine, (out string j) => Dsh.EngineDictRename(engine, Need(p, "dictId"), Need(p, "name"), out j));
                case "engine.dictSetCurrent":
                    return EngineCall(engine, (out string j) => Dsh.EngineDictSetCurrent(engine, Maybe(p, "dictId"), out j));

                /* ── history（查词历史；落盘在 `<配置目录>/history.jsonl`）
                 * ⚠️ 分页怎么算**不在这里**：`offset`/`limit` 原样交给内核（`Dsh.HistoryQuery`），界面只按页取；
                 *    这一层照旧只做「路由 + 错误翻译」，库打不开时内核自己回错误码 + 一句人话。 */
                case "history.query":
                    return EngineCall(engine, (out string j) => Dsh.HistoryQuery(
                        engine, (int)VirtualHost.JsonLong(p, "offset"),
                        (int)VirtualHost.JsonLong(p, "limit"), out j));
                case "history.clear":
                    return EngineCall(engine, (out string j) => Dsh.HistoryClear(engine, out j));

                /* ── lookup（`resource` 刻意不在表里：字节不过桥）────────────── */
                case "lookup.resolve":
                    return EngineCall(engine, (out string j) => Dsh.EngineResolve(engine, Maybe(p, "dictId"), Need(p, "text"), out j));
                case "lookup.suggest":
                    return EngineCall(engine, (out string j) => Dsh.EngineSuggest(engine, Need(p, "text"), out j));
                case "lookup.lookup":
                    return EngineCall(engine, (out string j) => Dsh.EngineLookup(engine, Need(p, "text"), Origin(p), Maybe(p, "dictId"), out j));
                case "lookup.probe":
                    return EngineCall(engine, (out string j) => Dsh.EngineProbe(engine, Need(p, "text"), Need(p, "dictId"), (int)VirtualHost.JsonLong(p, "budgetMs"), out j));
                case "lookup.entryDocument":
                    return EngineCall(engine, (out string j) => Dsh.EngineEntryDocument(engine, Need(p, "dictId"), Need(p, "keyText"), out j));

                /* ── text ─────────────────────────────────────────────────────── */
                case "text.analyze":
                    return PlainCall((out string j) => Dsh.TextAnalyze(Need(p, "text"), out j));
                case "text.stripSeparators":
                    return PlainCall((out string j) => Dsh.TextStripSeparators(Need(p, "text"), out j));
                case "text.languageDetect":
                    return EngineCall(engine, (out string j) => Dsh.LanguageDetect(engine, Need(p, "text"), Maybe(p, "dictId"), out j));

                /* ── speech ───────────────────────────────────────────────────── */
                case "speech.dictAudio":
                    return EngineCall(engine, (out string j) => Dsh.SpeechDictAudio(engine, Need(p, "dictId"), Need(p, "keyText"), out j));
                /*
                 * 朗读的规划：**三层音源的排序全在内核**（词典自带 → 在线 → 系统离线）。
                 * ⚠️ 壳只做两件"平台能力"的事：把要念的文本原样递进去、把**本机装了哪些离线音色**探测出来递进去；
                 *    「走哪一层、哪个语种配哪个嗓子、能不能点、不能点时那句人话」**一个字都不在壳里**。
                 * ⚠️ 音色表**不接受页面传参**（页面不可能比壳更清楚本机装了什么嗓子）：走 `Speech.Plan`，它自己探并**无条件覆盖** `voicesJson`。
                 */
                case "speech.plan":
                    return EngineCall(engine, (out string j) => j = Speech.Plan(
                        engine, Need(p, "text"), Maybe(p, "dictId")));

                /*
                 * 真的念：**照着 `speech.plan` 的答案出声**，回一段可以直接塞给 `<audio src>` 的地址
                 * （`{ok,url,mime,source,bytes,chunks,cached,reason,why}`）。
                 * ⚠️ 这一条**不返回字节**（音频走宿主发的 `https://…`）：桥那条通道的纪律是字节不过桥。
                 */
                case "speech.speak":
                    return EngineCall(engine, (out string j) => j = Speech.Speak(
                        engine, Need(p, "text"), Maybe(p, "dictId")));

                default:
                    throw new InvalidOperationException("不认识的方法：" + method);
            }
        }

        internal delegate void EngineCallDelegate(out string json);

        /// <summary>调一次内核、把它给的 JSON 原样取出来（统一走包装层，不碰 IntPtr）</summary>
        private static string EngineCall(IntPtr engine, EngineCallDelegate call)
        {
            if (engine == IntPtr.Zero) throw new InvalidOperationException("引擎还没建起来");
            string json;
            call(out json);
            return (json == null) ? "null" : json;
        }

        /// <summary>
        /// 不需要引擎的那几条（纯函数：文本分析 / 去分隔点）。
        /// ⚠️ 它们**不该**被 `EngineCall` 兜着 —— 那会给它们加一条"必须有引擎"的隐性前置：
        /// 于是"还没建引擎时分析一段文字"会报"引擎还没建起来"（而它压根不需要引擎）。
        /// </summary>
        private static string PlainCall(EngineCallDelegate call)
        {
            string json;
            call(out json);
            return (json == null) ? "null" : json;
        }

        /// <summary>
        /// 点号协议里的 `origin` —— 映射表在 <see cref="OriginMap"/>（**只有那一份**）。
        ///
        /// ⚠️ 出错策略与页面那条路**不一样，而且是刻意的**：这边**认不出就抛**
        ///    （诊断 / 验收喂的是人造 JSON，写错一个词应当当场炸，不要默默跑兜底通道）；
        ///    页面那条（`ShellBridge.Origin`）认不出落 `input`（用户界面只可能送那五个词，
        ///    真出了意外也不该让"查词"整条挂掉）。
        /// </summary>
        private static DshOrigin Origin(string p)
        {
            var text = Maybe(p, "origin");
            DshOrigin origin;
            if (OriginMap.Try(text, out origin)) return origin;
            throw new InvalidOperationException("origin 认不出来：" + text);
        }

        /// <summary>必填参数（缺了就如实报，不要拿空串顶上去 —— 那会变成"查了个空词"）</summary>
        private static string Need(string paramsJson, string name)
        {
            var value = Maybe(paramsJson, name);
            if (value == null) throw new InvalidOperationException("缺少参数：" + name);
            return value;
        }

        private static string Maybe(string paramsJson, string name)
        {
            return VirtualHost.JsonString(paramsJson, name);
        }

        /// <summary>把内核给的 JSON 原样塞进 result（不解析、不重排）</summary>
        private static string Ok(long id, string resultJson)
        {
            var sb = new StringBuilder();
            sb.Append("{\"id\":").Append(id.ToString(CultureInfo.InvariantCulture)).Append(",\"ok\":true,\"result\":");
            sb.Append(string.IsNullOrEmpty(resultJson) ? "null" : resultJson);
            sb.Append('}');
            return sb.ToString();
        }

        private static string Fail(long id, string message)
        {
            return "{\"id\":" + id.ToString(CultureInfo.InvariantCulture) + ",\"ok\":false,\"error\":{\"message\":" +
                   Quote(message) + "}}";
        }

        /// <summary>把一段文本包成 JSON 字符串（够用：转义引号、反斜杠与控制字符）</summary>
        internal static string Quote(string text)
        {
            var sb = new StringBuilder("\"");
            foreach (var c in text ?? "")
            {
                switch (c)
                {
                    case '"': sb.Append("\\\""); break;
                    case '\\': sb.Append("\\\\"); break;
                    case '\n': sb.Append("\\n"); break;
                    case '\r': sb.Append("\\r"); break;
                    case '\t': sb.Append("\\t"); break;
                    default:
                        if (c < 0x20) sb.Append("\\u").Append(((int)c).ToString("x4", CultureInfo.InvariantCulture));
                        else sb.Append(c);
                        break;
                }
            }
            sb.Append('"');
            return sb.ToString();
        }

        /// <summary>把 `"params":{…}` 那一段**原样**抠出来（嵌套括号要配对，不能靠找下一个 `}`）</summary>
        internal static string RawObject(string json, string key)
        {
            if (json == null) return null;
            var pat = "\"" + key + "\"";
            var at = json.IndexOf(pat, StringComparison.Ordinal);
            if (at < 0) return null;
            at += pat.Length;
            while (at < json.Length && (json[at] == ' ' || json[at] == ':')) at++;
            if (at >= json.Length || json[at] != '{') return null;

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
                if (c == '{') depth++;
                else if (c == '}')
                {
                    depth--;
                    if (depth == 0) return json.Substring(at, i - at + 1);
                }
            }
            return null;
        }
    }
}
