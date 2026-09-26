namespace Lookup.App
{
    using System;
    using System.Collections.Generic;
    using System.IO;

    /// <summary>
    /// 壳的启动参数。
    ///
    /// ⚠️ `--config` / `LOOKUP_USER_DATA` / `--web` 三个口子必须有：内核把设置写在配置目录里，
    ///    而验收**绝不许**碰用户真正的 `%APPDATA%\LookupApp`（那会改掉他自己的设置）；
    ///    页面根目录开发时在仓库里（改前端不用重编 C#）、打包后在 exe 旁边，所以按
    ///    "参数 → 环境变量 → exe 旁边 → 往上找开发布局"四步找，找不到就如实说找不到。
    /// </summary>
    internal sealed class AppOptions
    {
        internal string WebRoot;
        internal string ConfigDir;
        /// <summary>
        /// 手写的一份 `settings.json` 种子：建引擎**之前**放进配置目录。
        ///
        /// ⚠️ 用途只有一个：自检要造出「有一本词典的路径是假的」这种事实，而 `engine.dictAdd`
        ///    会**正确地**挡下不存在的路径 —— 种子是唯一入口，别去掉。
        /// </summary>
        internal string SettingsSeed;
        /// <summary>自检那份报告写到哪儿（空 = 只写控制台）。见 <see cref="Report"/> 顶上那段。</summary>
        internal string ReportPath;
        /// <summary>
        /// 机器翻译的**真 Key**（`--mt-key`，空 = 没给）：只给"要真的联网翻一次、按 token 计费"
        /// 的那一条自检用，所以**不进常规种子**，不给就明着跳过。
        /// ⚠️ 它只从命令行来：不落仓库、不落日志（报告里只写"给了/没给"）。
        /// </summary>
        internal string MtKey;
        internal readonly List<string> DictPaths = new List<string>();
        internal bool SelfCheck;
        internal bool NoShow;
        internal int TimeoutMs = 30000;

        /// <summary>
        /// DevTools 协议端口（0 = 不开）。
        ///
        /// 用途只有一个：**脚本**要接进来直接读/驱动页面 ——
        /// 渲染层那些"只有真实程序才知道"的事实（跨源 iframe 里发生了什么、内联脚本跑没跑、
        /// 控制台里有没有报错）走这条路比"再改一次自检、再跑整道 检查"便宜得多。
        ///
        /// ⚠️ 它**不是**给用户的功能：默认关着，只有显式给了 `--debug-port` 才开；
        ///    与参考实现的 `LOOKUP_DEBUG_PORT` 是同一个口子（那边也是环境变量控制）。
        /// </summary>
        internal int DebugPort;

        internal const string WebRootEnv = "LOOKUP_WEB_ROOT";
        internal const string ConfigDirEnv = "LOOKUP_USER_DATA";
        internal const string DebugPortEnv = "LOOKUP_DEBUG_PORT";

        internal static AppOptions Parse(string[] args)
        {
            var o = new AppOptions();
            for (var i = 0; i < args.Length; i++)
            {
                var a = args[i];
                switch (a)
                {
                    case "--web": o.WebRoot = Next(args, ref i, a); break;
                    case "--config": o.ConfigDir = Next(args, ref i, a); break;
                    case "--dict": o.DictPaths.Add(Next(args, ref i, a)); break;
                    case "--settings": o.SettingsSeed = Next(args, ref i, a); break;
                    /*
                     * 真 Key 那条路（**只给"专门验机器翻译端到端"用**；，
                     * 该件已停用、待办随 拍板取消 —— 语义见 SelfCheck 第 ⑰ 节）：
                     * 自检第 ⑰ 节要**联网、按 token 计费**，所以它**不进常规种子** ——
                     * 不传这个参数就明着跳过那一节（检查 照旧是离线的）。
                     * ⚠️ 这把 Key **只从命令行来**，绝不写进仓库里的任何文件（也不打印）。
                     */
                    case "--mt-key": o.MtKey = Next(args, ref i, a); break;
                    case "--report": o.ReportPath = Next(args, ref i, a); break;
                    case "--timeout": o.TimeoutMs = int.Parse(Next(args, ref i, a)); break;
                    case "--debug-port": o.DebugPort = int.Parse(Next(args, ref i, a)); break;
                    /*
                     * ⚠️ 第三十九轮试过"自检不带 `--no-show`"（图的是页面拿得到焦点），
                     * 第四十轮**退回来了** —— 用户 2026-09 拍板：
                     * **界面用参考实现那一份原样的**，0.2.0 自己那一套界面不往下做了。
                     * 所以这里回到"自检不打扰用户"的形态（离屏 + 不进任务栏），
                     * 等界面换成参考实现那份之后再按那一份的行为重定。
                     */
                    case "--selfcheck": o.SelfCheck = true; o.NoShow = true; break;
                    case "--no-show": o.NoShow = true; break;
                    /*
                     * ⚠️ `--autostart` 只作标记接受、不消费：开机自启动写进注册表 Run 键的命令行
                     *    带它（见 Startup.SetLoginAtStartup），历史上 Parse 不认它 → 开机拉起
                     *    当场抛异常、窗口根本不建（自启动必挂）。壳目前不需要区分"开机拉起来的"，
                     *    所以收下但什么都不做 —— **删掉这个 case 等于再次弄坏自启动**。
                     */
                    case "--autostart": break;
                    default:
                        throw new ArgumentException("不认识的参数：" + a);
                }
            }

            if (string.IsNullOrEmpty(o.WebRoot)) o.WebRoot = Environment.GetEnvironmentVariable(WebRootEnv);
            if (string.IsNullOrEmpty(o.WebRoot)) o.WebRoot = FindWebRoot(AppContext.BaseDirectory);

            if (string.IsNullOrEmpty(o.ConfigDir)) o.ConfigDir = Environment.GetEnvironmentVariable(ConfigDirEnv);
            if (string.IsNullOrEmpty(o.ConfigDir))
            {
                var roaming = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
                o.ConfigDir = Path.Combine(roaming, ConfigDirName);
                MigrateLegacyConfigDir(Path.Combine(roaming, LegacyConfigDirName), o.ConfigDir);
            }
            return o;
        }

        /// <summary>
        /// 配置目录名（**只许 ASCII**）。
        ///
        /// ⚠️ 用户 2026-09 点名要求：「修改一下软件在 AppData\Roaming 下创建的文件夹名，
        ///    把它改成英文的，不要出现『查词』这样的中文字符。」
        ///    动机是真问题：内核那一层收的是 **UTF-8** 路径，而 Win32 的窄字符 API 按
        ///    进程的 **ANSI 代码页**解释它 —— 在一台 ANSI 代码页不是 UTF-8 的机器上，
        ///    `%APPDATA%\查词\settings.json` 会变成乱码路径，**任何一次写设置都失败**
        ///    （2026-09 用户在另一台电脑上撞到的「保存增益失败 … 打不开临时文件」就是它）。
        ///    内核那一侧已经把文件操作全换成宽字符 API
        ///    （那才是根治），这里改名是**第二道保险**：路径本身不带非 ASCII 字符。
        /// </summary>
        internal const string ConfigDirName = "LookupApp";

        /// <summary>改名之前的目录名（参考实现与 0.2.0 的旧版本都用它，含非 ASCII 字符）</summary>
        internal const string LegacyConfigDirName = "查词";

        /// <summary>
        /// 值得从老目录搬过来的**用户数据**（白名单）。
        ///
        /// ⚠️ 为什么用白名单而不是"所有文件"：实测 `%APPDATA%\查词` 顶上还躺着一堆
        ///    WebView2 的缓存（`Preferences` / `Local State` / `DIPS` / `DevToolsActivePort` /
        ///    `declarative_performance_observer.db`…）—— 搬过去只会在新目录里再脏一份。
        ///    ⚠️ 以后内核要往配置目录里加新文件，**记得把它加进这张表**
        ///    （否则老用户升级上来会丢它）。
        /// </summary>
        private static readonly string[] LegacyUserFiles =
        {
            "settings.json", /* 设置：词典列表、当前词典、发音、翻译、悬浮窗位置… */
            "history.jsonl", /* 查词历史（参考实现起就在这个文件里） */
            "mtcache.json",  /* 机器翻译的缓存 */
        };

        /// <summary>
        /// 把老目录（`%APPDATA%\查词`）里的**用户数据**复制一份到新目录（`%APPDATA%\LookupApp`）。
        ///
        /// 四条约定：
        ///   · **只在"新目录还不存在"时做一次** —— 用户已经在新目录里用过之后，绝不许再往回搬；
        ///   · **只复制"我们自己的"那几个文件**（下面 `LegacyUserFiles` 那份白名单），
        ///     不是"复制所有文件" —— 实测老目录顶上还躺着一堆 **WebView2 的缓存**
        ///     （`Preferences` / `Local State` / `DIPS` / `DevToolsActivePort` / `*.db`…），
        ///     那是参考实现那版浏览器留下的垃圾，跟着搬过去只会在新目录里再脏一份；
        ///   · **是复制不是搬家** —— `早期各版` 仍然在用 `%APPDATA%\查词`
        ///     （冻结版没有改名），搬走会让那三版一起失忆；
        ///   · 单个文件失败只记一条日志，**绝不许拦住启动**。
        ///
        /// 只在**默认**配置目录这一条路上调用（`--config` / `LOOKUP_USER_DATA` 指定的目录一概不动）——
        /// 否则自检与验收会把用户真正的设置搬进临时目录，反过来也会污染验收。
        /// </summary>
        private static void MigrateLegacyConfigDir(string legacyDir, string targetDir)
        {
            try
            {
                if (!Directory.Exists(legacyDir)) return;
                if (Directory.Exists(targetDir)) return;
                Directory.CreateDirectory(targetDir);
                var copied = 0;
                foreach (var name in LegacyUserFiles)
                {
                    var source = Path.Combine(legacyDir, name);
                    if (!File.Exists(source)) continue;
                    try
                    {
                        File.Copy(source, Path.Combine(targetDir, name), false);
                        copied++;
                    }
                    catch (Exception err)
                    {
                        /* 单个文件搬不过来不该拦住启动 —— 落一条日志就好 */
                        Console.Error.WriteLine("[paths] 旧设置搬不过来：" + name + " — " + err.Message);
                    }
                }
                if (copied > 0)
                {
                    Console.WriteLine("[paths] 已把旧配置目录里的 " + copied + " 个用户文件复制到 " +
                                      ConfigDirName + "（旧目录保留，参考实现还在用）");
                }
            }
            catch (Exception err)
            {
                Console.Error.WriteLine("[paths] 迁移旧配置目录失败：" + err.Message);
            }
        }

        private static string Next(string[] args, ref int i, string flag)
        {
            if (i + 1 >= args.Length) throw new ArgumentException(flag + " 后面要跟一个值");
            return args[++i];
        }

        /// <summary>
        /// 往上找"哪一层里有 `web/floating.html`"。
        ///
        /// 开发时 exe 在 `shell/Lookup.App/bin/Release/net48/`，往上四层就是 ``。
        /// 找的是**那个文件**而不是那个目录 —— 只判目录存在的话，随便一个空的 `web/` 都会被认下来，
        /// 症状是"页面 404"而不是"你说错了根目录"，很难查。
        /// </summary>
        internal static string FindWebRoot(string startDir)
        {
            var dir = startDir;
            for (var depth = 0; depth < 8 && !string.IsNullOrEmpty(dir); depth++)
            {
                var candidate = Path.Combine(dir, "web");
                if (File.Exists(Path.Combine(candidate, "floating.html"))) return candidate;
                var parent = Path.GetDirectoryName(dir.TrimEnd(
                    Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));
                if (parent == null || parent == dir) break;
                dir = parent;
            }
            return null;
        }

        internal string Describe()
        {
            return "外壳资源 : " + (WebRoot ?? "（没找到 web/floating.html）") + Environment.NewLine +
                   "配置目录 : " + ConfigDir + Environment.NewLine +
                   "自检模式 : " + (SelfCheck ? "是" : "否");
        }
    }
}
