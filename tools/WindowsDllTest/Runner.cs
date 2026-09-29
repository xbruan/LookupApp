namespace Lookup.Interop.TestHarness
{
    using System;
    using System.Collections.Generic;
    using System.Linq;
    using System.Runtime.InteropServices;
    using System.Text;
    using Lookup.Interop;
    using Lookup.Host;

    /// <summary>Windows 侧验收：只调生成的包装层 Dsh.xxx，不碰 DshRaw、也不自己管指针 —— 壳将来用的就是它，
    /// 绕开它自己拼 IntPtr，「包装层对不对」就永远验不到。本文件与生成的绑定编进同一个程序集（generated 类型是 internal）；
    /// 这个类则必须是 public（PowerShell 看不见 internal 类型），而生成文件别为了好测改成 public。</summary>
    public static class Runner
    {
        private static int _checks;
        private static int _failed;

        private static void Ok(bool cond, string what)
        {
            _checks++;
            if (!cond) { _failed++; Console.WriteLine("FAIL " + what); }
        }

        /// <summary>断言 + 失败时把那份 JSON 打出来（派发那一段的失败光看断言看不出所以然）</summary>
        private static void OkJson(bool cond, string json, string what)
        {
            _checks++;
            if (cond) return;
            _failed++;
            Console.WriteLine("FAIL " + what);
            Console.WriteLine("      json=" + Truncate(json, 300));
        }

        public static int Run(string testData, string tempDir)
        {
            string testMdx = System.IO.Path.Combine(testData, "test.mdx");
            string linkMdx = System.IO.Path.Combine(testData, "link.mdx");
            string audioMdx = System.IO.Path.Combine(testData, "audio.mdx");
            string audioMdd = System.IO.Path.Combine(testData, "audio.mdd");

            // ── ① core 组 ──
            Ok(Dsh.KernelAbiVersion() == Dsh.AbiVersion,
               "dsh_abi_version 必须与接口定义里的 AbiVersion 一致");
            string ver = Dsh.Version();
            Ok(!string.IsNullOrEmpty(ver), "Version() 不该是空串");
            Console.WriteLine("  内核版本：" + ver + "，ABI v" + Dsh.KernelAbiVersion());

            DshRaw.dsh_release(IntPtr.Zero);   // 接口定义：NULL 是合法空操作
            Ok(true, "dsh_release(NULL) 不该崩");

            // 内核没给过的指针交给内核：必须被安全拒绝（不崩）并留下原因 ——
            // 误传 Marshal.AllocHGlobal 的指针时要能说清「这不是内核给的」，而不是让进程崩掉（那边连栈都没有）。
            IntPtr foreign = Marshal.AllocHGlobal(64);
            try
            {
                DshRaw.dsh_release(foreign);
                string why = Dsh.LastError();
                Ok(why != null && why.Contains("不是内核当前分配的"),
                   "释放外来指针必须被拒绝并说明原因，实际：" + (why ?? "(空)"));
                Console.WriteLine("  拒绝外来指针的原因：" + why);
            }
            catch (Exception ex)
            {
                Ok(false, "释放外来指针时抛了异常（内核不该抛）：" + ex.Message);
            }
            finally { Marshal.FreeHGlobal(foreign); }   // 它仍归我

            // ── ② parser 组：全部走生成的包装 ──
            IntPtr dict = IntPtr.Zero;
            try
            {
                Dsh.DictOpen(testMdx, out dict);       // 失败会抛（带内核的人话原因）
                Ok(dict != IntPtr.Zero, "DictOpen(test.mdx) 应当给出有效句柄");
            }
            catch (Exception ex)
            {
                Ok(false, "DictOpen(test.mdx) 抛了异常：" + ex.Message);
                return Finish();
            }

            try
            {
                // ⚠️ 先做不含内容读取的两条（contains / keys），再做 fetch：
                // 否则一个 AccessViolation 只说明「fetch 崩了」，而 fetch 内部串了查键 / 定位记录块 / 解压 / 解码四件事。
                string contains;
                Dsh.DictContains(dict, "apple", out contains);
                Console.WriteLine("  contains(apple): " + contains);
                Ok(contains != null && contains.Contains("\"found\":true"), "contains 应当命中 apple");

                string keysOnly;
                Dsh.DictKeys(dict, out keysOnly);
                Console.WriteLine("  keys: " + Truncate(keysOnly, 100));
                Ok(keysOnly != null && keysOnly.StartsWith("["), "DictKeys 应当返回 JSON 数组");

                string info;
                Dsh.DictInfo(dict, out info);
                Console.WriteLine("  info: " + info);
                Ok(info != null && info.Contains("\"entryCount\":6"),
                   "test.mdx 的词条数必须是 6（仓库级常量）");
                Ok(info != null && info.Contains("\"encoding\":\"UTF-8\""), "编码必须是 UTF-8");
                Ok(info != null && info.Contains("\"version\":\"2.0\""), "版本必须是 2.0");

                // 到这里都没碰「读记录」，下面这一条才进 fetch。
                Console.WriteLine("  （下面开始走 dsh_dict_fetch —— 会读记录并解压）");
                string fetch;
                Dsh.DictFetch(dict, "apple", out fetch);
                Console.WriteLine("  fetch(apple): " + Truncate(fetch, 180));
                Ok(fetch != null && fetch.Contains("\"found\":true"), "apple 应当查到");
                Ok(fetch != null && fetch.Contains("\"keyText\":\"apple\""), "规范键名应当是 apple");

                Dsh.DictFetch(dict, "zzzz-not-there", out fetch);
                Ok(fetch != null && fetch.Contains("\"found\":false"), "不存在的键应如实返回 found=false");

                Dsh.DictFetch(dict, "APPLE", out fetch);
                Ok(fetch != null && fetch.Contains("\"found\":false"),
                   "解析层大小写敏感：APPLE 不该命中 apple");

                Dsh.DictFetch(dict, "测试", out fetch);
                Ok(fetch != null && fetch.Contains("\"found\":true"), "中文键「测试」应当查到（UTF-8 往返）");

                string keys;
                Dsh.DictKeys(dict, out keys);
                Ok(keys != null && keys.StartsWith("["), "DictKeys 应当返回 JSON 数组");
                Ok(keys != null && keys.Contains("\"apple\""), "键列表里应当有 apple");
                Console.WriteLine("  keys: " + Truncate(keys, 100));

                /*
                 * ★ 音频测试用词典的键名清点：真实程序里点 🔊 报「这本词典里没有这个资源」时，
                 * 分不清是测试用词典真没有、还是内核那六种候选键名没找到 —— 这里直接把 audio.mdd 的键列出来看。
                 */
                {
                    IntPtr mdd = IntPtr.Zero;
                    var mddPath = audioMdd;
                    if (System.IO.File.Exists(mddPath))
                    {
                        try
                        {
                            Dsh.DictOpen(mddPath, out mdd);
                            string mddKeys;
                            Dsh.DictKeys(mdd, out mddKeys);
                            Console.WriteLine("  audio.mdd 的键： " + Truncate(mddKeys, 400));
                            Ok(mddKeys != null && mddKeys.Contains("beep__gb_1.wav"),
                               "⑨ ★ 音频卷里真有 beep 那条录音（真实程序里点 🔊 就是点它）");
                            /* 测试用词典里没有的键，界面那一侧只能如实说「没有」 */
                            Console.WriteLine("  · audio.mdd 里有 spxlink 吗： " +
                                              (mddKeys != null && mddKeys.Contains("spxlink")
                                                   ? "有"
                                                   : "**没有**（所以界面报「这本词典里没有这个资源」是对的）"));
                            Ok(mddKeys != null && mddKeys.Contains("speexword__gb_1.spx"),
                               "⑨ 音频卷里有那一段**真 Speex**（内核这一版解不了，界面要如实说）");
                        }
                        catch (Exception ex)
                        {
                            Ok(false, "打开 audio.mdd 抛了异常：" + ex.Message);
                        }
                        finally
                        {
                            if (mdd != IntPtr.Zero) Dsh.DictClose(mdd);
                        }
                    }
                    else
                    {
                        Ok(false, "找不到测试用词典 audio.mdd：" + mddPath);
                    }
                }
            }
            finally
            {
                Dsh.DictClose(dict);
            }

            // ── ③ @@@LINK 重定向（link.mdx 专门为它造的测试用词典）───────────────
            IntPtr link = IntPtr.Zero;
            try
            {
                Dsh.DictOpen(linkMdx, out link);
                string apples;
                Dsh.DictFetch(link, "apples", out apples);
                Console.WriteLine("  fetch(apples): " + Truncate(apples, 180));
                Ok(apples != null && apples.Contains("@@@LINK=apple"),
                   "apples 的记录内容应当原样含 @@@LINK=apple（解析层不跟跳转）");
            }
            catch (Exception ex)
            {
                Ok(false, "link.mdx 那条路抛了异常：" + ex.Message);
            }
            finally
            {
                if (link != IntPtr.Zero) Dsh.DictClose(link);
            }

            // 打开不存在的东西：包装层应当抛，且原因来自内核
            bool threw = false;
            IntPtr bogus = IntPtr.Zero;
            try { Dsh.DictOpen("Z:\\definitely\\not\\here.mdx", out bogus); }
            catch (InvalidOperationException ex) { threw = true; Console.WriteLine("  打开不存在路径：" + ex.Message); }
            Ok(threw, "不存在的路径必须抛（包装层的接口约定）");
            Ok(bogus == IntPtr.Zero, "失败时句柄必须保持 Zero");

            // ── ④ ★ 反复打开（★ 回归保护：同一进程里第二次以后打开词典）────────
            /*
             * 同一进程里第二次以后打开词典，pack_size / entry_count 偶尔读到垃圾。
             * 真凶是解码后那块缓冲被提前 dsh_release：地址随即被词块索引复用，活着的索引被注销。
             * 所以这一节钉的是触发条件 —— 连开 5 次，每次都要读得到正文。
             */
            int repeatOk = 0;
            for (int round = 0; round < 5; round++)
            {
                IntPtr again = IntPtr.Zero;
                try
                {
                    Dsh.DictOpen(testMdx, out again);
                    string againFetch;
                    Dsh.DictFetch(again, "apple", out againFetch);
                    if (againFetch != null && againFetch.Contains("\"keyText\":\"apple\"")) repeatOk++;
                }
                catch (Exception ex)
                {
                    Console.WriteLine("  第 " + (round + 1) + " 次重开失败：" + ex.Message);
                }
                finally { if (again != IntPtr.Zero) Dsh.DictClose(again); }
            }
            Ok(repeatOk == 5,
               "同一个进程里连开 5 次都必须能查到 apple（实际 " + repeatOk + "/5）—— 这是 的护栏");

            // ── ⑤ engine + lookup + speech：走包装层的**一条完整链** ──────────
            /* 引擎必须建在临时目录里（绝不碰用户真正的 %APPDATA%）；然后按壳将来会用的顺序走一遍整条链：
             * 加词典 → 解析落点 → 完整查词 → 取正文文档 → 取资源字节 → 词典原录音 → 音频预处理。
             */
            IntPtr engine = IntPtr.Zero;
            try
            {
                Dsh.EngineCreate(tempDir, out engine);
                Ok(engine != IntPtr.Zero, "EngineCreate 应当给出有效句柄");
            }
            catch (Exception ex)
            {
                Ok(false, "EngineCreate 抛了异常：" + ex.Message);
                return Finish();
            }

            try
            {
                string pathsJson = "[\"" + testMdx.Replace("\\", "\\\\") + "\",\"" +
                                   audioMdx.Replace("\\", "\\\\") + "\"]";
                string added;
                Dsh.EngineDictAdd(engine, pathsJson, out added);
                Console.WriteLine("  dict_add: " + Truncate(added, 160));
                Ok(added != null && added.Contains("\"added\":2"), "加两本词典应当都成功");

                string list;
                Dsh.EngineDictList(engine, out list);
                string testId = ExtractId(list, 0);
                string audioId = ExtractId(list, 1);
                Ok(testId != null && audioId != null, "清单里应当有两本词典的 id");
                Ok(list != null && list.Contains("\"loaded\":false"),
                   "★ 导入之后**不许**有任何一本常驻内存（资源卷更是懒打开）");

                Console.WriteLine("  （test.mdx id=" + Truncate(testId, 16) + "…，audio.mdx id=" +
                                  Truncate(audioId, 16) + "…）");

                // 落点：大小写变体 + 中文键，都走与壳同一条路
                string resolved;
                Dsh.EngineResolve(engine, testId, "APPLE", out resolved);
                Ok(resolved != null && resolved.Contains("\"landed\":\"apple\""),
                   "EngineResolve(APPLE) 应当落到 apple");
                Dsh.EngineResolve(engine, testId, "测试", out resolved);
                Ok(resolved != null && resolved.Contains("\"landed\":\"测试\""),
                   "EngineResolve(测试) 应当落到「测试」");

                // 完整查词（选区那条路：直接查、命中即止）
                string lookup;
                Dsh.EngineLookup(engine, "apple", DshOrigin.Selection, testId, out lookup);
                Console.WriteLine("  lookup(apple): " + Truncate(lookup, 200));
                Ok(lookup != null && lookup.Contains("\"found\":true"), "EngineLookup 应当命中");
                Ok(lookup != null && lookup.Contains("\"via\":\"current\""), "命中那一页 via=current");
                Ok(lookup != null && lookup.Contains("\"stage\":\"afterLookup\""),
                   "stage 说的是「哪一步给出的答复」");
                Ok(lookup != null && lookup.Contains("dictres.invalid/__entry__?word=apple"),
                   "★ 出参里带着词条地址（壳直接赋给 iframe.src，不必自己拼）");

                // 正文文档：壳直接喂 iframe 的那一份
                string doc;
                Dsh.EngineEntryDocument(engine, testId, "apple", out doc);
                Ok(doc != null && doc.Contains("<!doctype html>"), "EntryDocument 给的是完整文档");
                Ok(doc != null && doc.Contains("<base href=") && doc.Contains(".dictres.invalid/"),
                   "文档里带着 <base>（资源相对路径靠它落在同一本词典上）");
                Ok(doc != null && doc.Contains("lookupBridge"), "桥接脚本注进去了");

                // ★ 资源字节：**必须原样**（`.mdd` 是 UTF-16 编码，按文本解会把二进制毁掉）
                IntPtr bytes = IntPtr.Zero;
                UIntPtr len = UIntPtr.Zero;
                string meta;
                Dsh.EngineResource(engine, audioId, "\\beep__gb_1.wav", UIntPtr.Zero, UIntPtr.Zero,
                                   out bytes, out len, out meta);
                Console.WriteLine("  resource(\\beep__gb_1.wav): " + meta);
                Ok(bytes != IntPtr.Zero && (ulong)len > 44, "取得到那条 wav 的字节");
                Ok(meta != null && meta.Contains("\"found\":true"), "资源元信息说 found=true");
                if (bytes != IntPtr.Zero && (ulong)len >= 4)
                {
                    byte[] head = new byte[4];
                    Marshal.Copy(bytes, head, 0, 4);
                    Ok(head[0] == 0x52 && head[1] == 0x49 && head[2] == 0x46 && head[3] == 0x46,
                       "★ 前 4 个字节必须是 RIFF（wav）—— 这是 那个「资源被解码器毁掉」的护栏");
                }
                if (bytes != IntPtr.Zero) DshRaw.dsh_release(bytes);

                // 词典原录音：词条里挂着的那条键
                string audio;
                Dsh.SpeechDictAudio(engine, audioId, "beep", out audio);
                Console.WriteLine("  dict_audio(beep): " + audio);
                Ok(audio != null && audio.Contains("\"found\":true"), "beep 有自带录音");
                Ok(audio != null && audio.Contains("beep__gb_1.wav"), "录音键名对得上");

                // 设置：改一笔、读回来，并确认**落盘**在临时目录里
                string setOut;
                Dsh.EngineSettingsSet(engine, "{\"closeBehavior\":\"tray\"}", out setOut);
                string settings;
                Dsh.EngineSettingsGet(engine, out settings);
                Ok(settings != null && settings.Contains("\"closeBehavior\":\"tray\""),
                   "设置改了就应当读得回来");
                Ok(settings != null && settings.Contains("\"speech\""),
                   "设置里带着发音那一节（的那一节）");
                string settingsFile = System.IO.Path.Combine(tempDir, "settings.json");
                Ok(System.IO.File.Exists(settingsFile), "设置必须**落盘**（引擎建在临时目录里）");

                /*
                 * ★★ 配置目录名带非 ASCII 字符（中文）时也必须写得进去：内核收进来的是 UTF-8，
                 * 而 Win32 窄字符 API（fopen / CreateFileA 等）按进程的 ANSI 代码页解释这些字节。
                 * ⚠️ 本机 ANSI 代码页是 65001，所以修前它在本地也是绿的 —— 这是给别的机器用的防回归护栏。
                 */
                {
                    var cnDir = System.IO.Path.Combine(tempDir, "中文目录 与 空格");
                    System.IO.Directory.CreateDirectory(cnDir);
                    IntPtr cnEngine;
                    Dsh.EngineCreate(cnDir, out cnEngine);
                    Ok(cnEngine != IntPtr.Zero, "建一个「配置目录名带中文」的引擎");
                    if (cnEngine != IntPtr.Zero)
                    {
                        string cnOut;
                        Dsh.EngineSettingsSet(cnEngine, "{\"closeBehavior\":\"quit\"}", out cnOut);
                        Ok(System.IO.File.Exists(System.IO.Path.Combine(cnDir, "settings.json")),
                           "★★ 配置目录名是中文时，settings.json 照样落进那个目录");
                        string cnBack;
                        Dsh.EngineSettingsGet(cnEngine, out cnBack);
                        Ok(cnBack != null && cnBack.Contains("\"closeBehavior\":\"quit\""),
                           "★★ 而且读回来还是刚写的那一份（**写与读**都没被中文路径绊住）");
                        Dsh.EngineDestroy(cnEngine);
                    }

                    /* ★★ 词库文件放在中文目录里、文件名也是中文时也必须加得进来：这条路走 mapfile
                     * （旧代码用窄字符 CreateFileA），与上面那条设置断言是同一个病根的另一条路。
                     * ⚠️ 本机（ANSI 65001）修前也是绿的，是给别的机器用的防回归护栏。
                     */
                    {
                        var cnDictDir = System.IO.Path.Combine(tempDir, "词典目录 中文");
                        System.IO.Directory.CreateDirectory(cnDictDir);
                        var cnDict = System.IO.Path.Combine(cnDictDir, "测试词典 中文名.mdx");
                        System.IO.File.Copy(testMdx, cnDict, true);
                        IntPtr dictEngine;
                        /* ⚠️ 内核不建目录（调用方保证父目录存在，见 dsh_file.h）—— 这里必须先建，
                         *    否则会得到「打不开临时文件 …settings.json」，被误当成路径编码又出问题。 */
                        var cnEngineDir = System.IO.Path.Combine(tempDir, "中文引擎");
                        System.IO.Directory.CreateDirectory(cnEngineDir);
                        Dsh.EngineCreate(cnEngineDir, out dictEngine);
                        Ok(dictEngine != IntPtr.Zero, "建一个「引擎目录也带中文」的引擎");
                        if (dictEngine != IntPtr.Zero)
                        {
                            string dictAdded;
                            Dsh.EngineDictAdd(dictEngine,
                                "[\"" + cnDict.Replace("\\", "\\\\") + "\"]", out dictAdded);
                            Console.WriteLine("  dict_add(中文路径): " + Truncate(dictAdded, 200));
                            Ok(dictAdded != null && dictAdded.Contains("\"added\":1"),
                               "★★ 中文目录 + 中文文件名里的词典照样加得进来（added = 1）");
                            Ok(dictAdded != null && dictAdded.Contains("\"failed\":[]"),
                               "★★ 而且 `failed` 是空的（不是「加进来但打不开」）");
                            string cnList;
                            Dsh.EngineDictList(dictEngine, out cnList);
                            Ok(cnList != null && cnList.Contains("测试词典 中文名.mdx"),
                               "★★ 清单里能看见它，书名/文件名带中文也没问题");

                            /* ★★ 词条正文里的 data-has-resources 走的是目录扫描这条路（原来用 ANSI 版 FindFirstFileA）：
                             * 目录读不动 → 当空 ⇒ 这本词典被判成没有资源，症状是「资源明明取到了、文档却说没有」。
                             * 这里旁边放一个 .css，于是有没有散放资源只由那次扫描决定，值必须是 1。
                             * ⚠️ 与本节其它两条一样：本机（ANSI 65001）修前也是绿的，是给别的机器用的防回归护栏。
                             */
                            System.IO.File.WriteAllText(System.IO.Path.Combine(cnDictDir, "样式.css"),
                                                        "body{color:#000}\n", new System.Text.UTF8Encoding(false));
                            /* 这份引擎里只加过这一本，所以清单里第一个 id 就是它 */
                            var cnId = Lookup.Host.VirtualHost.JsonString(cnList, "id");
                            Ok(cnId != null, "★★ 从清单里取到这本词典的 id（" + (cnId ?? "(没取到)") + "）");
                            if (cnId != null)
                            {
                                string cnDoc;
                                Dsh.EngineEntryDocument(dictEngine, cnId, "apple", out cnDoc);
                                /* ⚠️ 回包是带 html 字段的那个信封（不是裸 HTML），要先取出 html 再找属性：
                                 *    直接对整段 JSON 找 data-has-resources 永远找不到（里面是转义过的）。 */
                                var cnHtml = Lookup.Host.VirtualHost.JsonString(cnDoc, "html");
                                /* 报出量到的那个值（不只报红）：这一条在别的机器上是修前必红的护栏，本机也要看得出文档说了什么。 */
                                var seen = System.Text.RegularExpressions.Regex.Match(
                                    cnHtml ?? "", "data-has-resources=\"[0-9]\"");
                                Ok(cnHtml != null && seen.Success &&
                                   seen.Value == "data-has-resources=\"1\"",
                                   "★★ 中文目录里的**散放资源**被扫到了（实测 " +
                                   (seen.Success ? seen.Value : "文档里没有这个属性") +
                                   "；id=" + cnId + "）");
                            }
                            /* 加不存在的路径：内核要如实给出 failed 里的 path / reason 两项。
                             * 壳那一侧（ShellBridge 的 dict:add）就靠这两个字段把「为什么加不进来」带给界面。 */
                            string bogusAdded;
                            Dsh.EngineDictAdd(dictEngine,
                                "[\"" + System.IO.Path.Combine(cnDictDir, "没有这本词典.mdx")
                                             .Replace("\\", "\\\\") + "\"]", out bogusAdded);
                            Console.WriteLine("  dict_add(不存在的路径): " + Truncate(bogusAdded, 200));
                            Ok(bogusAdded != null && bogusAdded.Contains("\"added\":0"),
                               "★★ 不存在的路径加不进来（added = 0）");
                            Ok(bogusAdded != null && bogusAdded.Contains("\"path\":") &&
                               bogusAdded.Contains("\"reason\":"),
                               "★★ 而且 `failed` 里带着**路径与原因**（壳靠这两个字段说明白为什么）");
                            Dsh.EngineDestroy(dictEngine);
                        }
                    }
                }

                // ── ⑥ 宿主适配层：虚拟资源主机（词条正文里那些 URL 全靠它）────
                /* 词条正文里的图片 / 样式 / 录音写的是 https://<词典 id>.dictres.invalid/…，
                 * 宿主在发出网络请求之前就地应答；这一段验那一层的 HTTP 语义（206 / 304 / 416 / 404），
                 * 而字节与身份（mime / etag / total）全部由内核给。
                 */
                var origin = "https://" + audioId + "." + VirtualHost.ResourceDomain;

                // ① 词条正文那条路由
                var docRes = VirtualHost.Serve(engine, new VirtualRequest
                {
                    Url = "https://" + testId + "." + VirtualHost.ResourceDomain + "/__entry__?word=apple"
                });
                Ok(docRes.Status == 200 && docRes.ContentType.StartsWith("text/html"),
                   "虚拟主机：/__entry__ 应当回 200 + text/html");
                Ok(docRes.Body != null && Encoding.UTF8.GetString(docRes.Body).Contains("<!doctype html>"),
                   "虚拟主机：正文文档内容对得上");
                /* ★ 发出去的必须是那份 HTML，不是内核那个 JSON 信封 —— 整封信封发出去时状态码、
                 * Content-Type、字节数、CORS 全对，浏览器也照渲染，但桥接脚本会不报错地不执行
                 * （JSON 里的换行是两个字符 → 内联脚本语法错误，且不会冒到宿主）：表现为链接点了没反应。
                 */
                {
                    var served = docRes.Body == null ? "" : Encoding.UTF8.GetString(docRes.Body);
                    Ok(served.StartsWith("<!doctype html>", StringComparison.Ordinal),
                       "★ 虚拟主机：/__entry__ 发的是**那份 HTML**（不是内核的 JSON 信封）");
                    Ok(served.Contains("documentElement.dataset.lookupBridge"),
                       "★ 词条正文里带着桥接脚本（第一行就是它打的标记）");
                    Ok(served.Contains("(function () {\n"),
                       "★ 桥接脚本里的换行是**真换行**（JSON 信封里那会是 `\\n` 两个字符，脚本直接语法错误）");
                }

                // ② 资源：整体 200 + 身份信息
                var full = VirtualHost.Serve(engine, new VirtualRequest
                {
                    Url = origin + "/%5Cbeep__gb_1.wav"          // 键名里的反斜杠是 URL 编码过的
                });
                Console.WriteLine("  虚拟主机 200：" + full.Status + " " + full.ContentType +
                                  "，total=" + full.Total + "，etag=" + (full.ETag ?? "(无)"));
                Ok(full.Status == 200, "虚拟主机：资源整体回 200");
                Ok(full.ContentType == "audio/wav", "虚拟主机：Content-Type 来自内核的 mime");
                Ok(full.Body != null && full.Body.Length == full.Total, "虚拟主机：整体返回时字节数等于 total");
                Ok(full.Body != null && full.Body.Length > 4 && full.Body[0] == 0x52 && full.Body[1] == 0x49,
                   "★ 虚拟主机：拿到的仍是**原始字节**（RIFF 开头，的护栏）");
                Ok(!string.IsNullOrEmpty(full.ETag), "虚拟主机：带 ETag（协商缓存的身份证）");
                Ok(full.Headers.Exists(h => h.StartsWith("Accept-Ranges:", StringComparison.Ordinal)),
                   "虚拟主机：声明支持 Range（播放器的 seek 靠它）");
                Ok(full.Headers.Exists(h => h.StartsWith("Access-Control-Allow-Origin:", StringComparison.Ordinal)),
                   "虚拟主机：带 CORS（词条正文是 opaque origin，字体等资源需要它）");

                // ③ Range → 206 + Content-Range
                var part = VirtualHost.Serve(engine, new VirtualRequest
                {
                    Url = origin + "/%5Cbeep__gb_1.wav",
                    Range = "bytes=0-43"
                });
                Console.WriteLine("  虚拟主机 206：" + part.Status + " " +
                                  part.Headers.Find(h => h.StartsWith("Content-Range", StringComparison.Ordinal)));
                Ok(part.Status == 206, "虚拟主机：带 Range 回 206");
                Ok(part.Body != null && part.Body.Length == 44, "虚拟主机：206 的字节数是区间长度");
                Ok(part.Body != null && part.Body[0] == 0x52 && part.Body[1] == 0x49,
                   "虚拟主机：206 的字节从第 0 字节开始（仍是 RIFF）");
                Ok(part.Headers.Exists(h => h == "Content-Range: bytes 0-43/" + full.Total),
                   "虚拟主机：Content-Range 与 total 对得上");

                // ④ 越界的 Range → 416
                var bad = VirtualHost.Serve(engine, new VirtualRequest
                {
                    Url = origin + "/%5Cbeep__gb_1.wav",
                    Range = "bytes=999999999-"
                });
                Ok(bad.Status == 416, "虚拟主机：越界的 Range 回 416");
                Ok(bad.Headers.Exists(h => h == "Content-Range: bytes */" + full.Total),
                   "虚拟主机：416 要带 `bytes */total`");

                // ⑤ 协商缓存：带上刚才那个 ETag → 304 且**不带正文**
                var cached = VirtualHost.Serve(engine, new VirtualRequest
                {
                    Url = origin + "/%5Cbeep__gb_1.wav",
                    IfNoneMatch = full.ETag
                });
                Ok(cached.Status == 304, "虚拟主机：If-None-Match 命中 → 304");
                Ok(cached.Body == null || cached.Body.Length == 0, "虚拟主机：304 不许带正文");
                var weak = VirtualHost.Serve(engine, new VirtualRequest
                {
                    Url = origin + "/%5Cbeep__gb_1.wav",
                    IfNoneMatch = "W/" + full.ETag
                });
                Ok(weak.Status == 304, "虚拟主机：弱比较（W/ 前缀）也要认");

                // ⑥ 录音那条路由（桥接脚本把 sound:// 改写成它）
                var sound = VirtualHost.Serve(engine, new VirtualRequest
                {
                    Url = origin + VirtualHost.SoundRoute + "beep__gb_1.wav"
                });
                Ok(sound.Status == 200 && sound.ContentType == "audio/wav",
                   "虚拟主机：/__sound__/<键名> 回 200 + audio/wav");

                // ⑦ 不存在的资源 / 不是词典域的地址
                var missing = VirtualHost.Serve(engine, new VirtualRequest
                {
                    Url = origin + "/no-such-thing.png"
                });
                Ok(missing.Status == 404, "虚拟主机：不存在的资源回 404");
                Ok(missing.Body != null && missing.Body.Length > 0, "虚拟主机：404 要带一句人话");
                var offDomain = VirtualHost.Serve(engine, new VirtualRequest
                {
                    Url = "https://example.com/a.png"
                });
                Ok(offDomain.Status == 404, "虚拟主机：不是词典资源域的地址回 404");

                // ⑧ Range 解析的三种写法（纯函数，单独钉）
                long rs, rl;
                bool unsat;
                Ok(VirtualHost.TryParseRange("bytes=10-19", 100, out rs, out rl, out unsat) &&
                       rs == 10 && rl == 10 && !unsat, "Range：bytes=10-19");
                Ok(VirtualHost.TryParseRange("bytes=90-", 100, out rs, out rl, out unsat) &&
                       rs == 90 && rl == 10 && !unsat, "Range：bytes=90-（到结尾）");
                Ok(VirtualHost.TryParseRange("bytes=-5", 100, out rs, out rl, out unsat) && rl == -5,
                   "Range：bytes=-5（后缀写法，此刻还不知道总长 → length 记负数）");
                Ok(!VirtualHost.TryParseRange("bytes=0-9,20-29", 100, out rs, out rl, out unsat),
                   "Range：多段刻意不支持（回 false → 调用方整体发 200）");
                Ok(VirtualHost.TryParseRange("bytes=200-", 100, out rs, out rl, out unsat) && unsat,
                   "Range：起点超过总长 → unsatisfiable（回 416）");
                Ok(!VirtualHost.TryParseRange("", 100, out rs, out rl, out unsat),
                   "Range：没有这个头 → false");

                // ── ⑦ 外壳自己的站点（lookup.local → ShellAssets）──────────
                /* 与词典资源域分开的理由是信任边界：外壳的资源是我们自己发的（可以放行脚本），
                 * 词典的资源是外部内容（只放行被动资源）。这里在临时目录里搭一份前端产物来验那条路，
                 * 含四道 gate 里最要紧的两道（.. 与绝对路径）。
                 */
                var shellRoot = System.IO.Path.Combine(tempDir, "web");
                System.IO.Directory.CreateDirectory(System.IO.Path.Combine(shellRoot, "sub"));
                System.IO.File.WriteAllText(System.IO.Path.Combine(shellRoot, "floating.html"),
                                            "<!doctype html><title>壳</title>");
                System.IO.File.WriteAllText(System.IO.Path.Combine(shellRoot, "app.js"), "console.log(1)");
                System.IO.File.WriteAllText(System.IO.Path.Combine(shellRoot, "sub", "a.css"), "a{}");
                // 根目录**之外**的一个文件：路径穿越要拿它当靶子
                System.IO.File.WriteAllText(System.IO.Path.Combine(tempDir, "secret.txt"), "secret");

                var shell = ShellAssetSource.FromDirectory(shellRoot);

                var shellDoc = VirtualHost.Serve(engine, new VirtualRequest
                {
                    Url = "https://lookup.local/floating.html"
                }, shell);
                Ok(shellDoc.Status == 200 && shellDoc.ContentType.StartsWith("text/html"),
                   "壳站点：floating.html 回 200 + text/html");
                Ok(shellDoc.Body != null && Encoding.UTF8.GetString(shellDoc.Body).Contains("壳"),
                   "壳站点：内容对得上");

                var shellDefault = VirtualHost.Serve(engine, new VirtualRequest
                {
                    Url = "https://lookup.local/"
                }, shell);
                Ok(shellDefault.Status == 200 && shellDefault.ContentType.StartsWith("text/html"),
                   "壳站点：根路径给默认文档（floating.html）");

                var shellJs = VirtualHost.Serve(engine, new VirtualRequest
                {
                    Url = "https://lookup.local/app.js"
                }, shell);
                Ok(shellJs.Status == 200 && shellJs.ContentType.StartsWith("text/javascript"),
                   "壳站点：.js 的 MIME 是 text/javascript（**外壳**可以发脚本）");

                var shellCss = VirtualHost.Serve(engine, new VirtualRequest
                {
                    Url = "https://lookup.local/sub/a.css"
                }, shell);
                Ok(shellCss.Status == 200 && shellCss.ContentType.StartsWith("text/css"),
                   "壳站点：子目录里的 .css 也发得出来");

                var shellFavicon = VirtualHost.Serve(engine, new VirtualRequest
                {
                    Url = "https://lookup.local/favicon.ico"
                }, shell);
                Ok(shellFavicon.Status == 404, "壳站点：favicon 静静回 404（浏览器每次都来要）");

                var shellMissing = VirtualHost.Serve(engine, new VirtualRequest
                {
                    Url = "https://lookup.local/nope.html"
                }, shell);
                Ok(shellMissing.Status == 404, "壳站点：不存在的页面回 404");

                // ★ 四道 gate：.. 与绝对路径都要挡下（拿根目录外面那个真文件当靶子）
                var escape1 = shell.Serve("../secret.txt");
                Ok(escape1.Status == 404 && escape1.Body == null,
                   "★ 壳站点：`../` 穿越必须挡下（外面那个文件存在也不许读到）");
                var escape2 = shell.Serve("sub/../../secret.txt");
                Ok(escape2.Status == 404 && escape2.Body == null, "★ 壳站点：`sub/../../` 也挡下");
                var escape3 = shell.Serve("C:\\Windows\\win.ini");
                Ok(escape3.Status == 404 && escape3.Body == null, "★ 壳站点：带盘符的绝对路径挡下");
                var escape4 = shell.Serve("//server/share/a.css");
                Ok(escape4.Status == 404 && escape4.Body == null, "★ 壳站点：UNC 路径挡下");

                // 名字里带点但不是路径段的文件名要放行（包含式判断会误伤）
                System.IO.File.WriteAllText(System.IO.Path.Combine(shellRoot, "a..b.css"), "b{}");
                var dotted = shell.Serve("a..b.css");
                Ok(dotted.Status == 200, "壳站点：`a..b.css` 是合法文件名（逐段判，不是包含式判）");

                // 没配外壳目录时：lookup.local 要回一句人话，而不是「不是词典域」
                var noShell = VirtualHost.Serve(engine, new VirtualRequest
                {
                    Url = "https://lookup.local/floating.html"
                });
                Ok(noShell.Status == 404 && noShell.Reason != null &&
                       noShell.Reason.Contains("外壳资源目录"),
                   "壳站点：没配目录时如实说「没有配置外壳资源目录」");

                Ok(ShellAssetSource.MimeFor("a.PNG") == "image/png", "壳站点：扩展名大小写不敏感");
                Ok(ShellAssetSource.MimeFor("noext") == "application/octet-stream",
                   "壳站点：认不出的扩展名 → octet-stream");

                // ── ⑧ 前端唯一入口：Dispatch（把一条 JSON 派发到内核）──────────
                /* 这一节验的是前端将来真正会用的那条通道。三条纪律：
                 * 内核的 JSON 原样带出去（这一层不许理解业务字段）；异常绝不抛过边界（前端只该看到 ok:false）；
                 * 缺参数 / 不认识的方法 / 坏 JSON 都要如实报，而不是不报错地当默认值。
                 */
                var dispatchDir = System.IO.Path.Combine(tempDir, "dispatch");
                System.IO.Directory.CreateDirectory(dispatchDir);
                IntPtr de = IntPtr.Zero;
                Dsh.EngineCreate(dispatchDir, out de);
                try
                {
                    var r1 = Dispatch.Handle(de, "{\"id\":7,\"method\":\"core.version\"}");
                    Ok(r1.Contains("\"id\":7") && r1.Contains("\"ok\":true") && r1.Contains("0.2.1"),
                       "派发：core.version 回 ok 且带内核版本");
                    Ok(r1.Contains("\"result\":\"0.2.1\""),
                       "派发：版本号是**内核给的**原样字符串");

                    var r2 = Dispatch.Handle(de, "{\"id\":1,\"method\":\"engine.dictList\"}");
                    OkJson(r2.Contains("\"ok\":true") && r2.Contains("\"result\":[]"), r2,
                       "派发：新建引擎的词库清单是空数组（内核那份 JSON 原样带出来）");

                    var addPaths = "{\"id\":2,\"method\":\"engine.dictAdd\",\"params\":{\"pathsJson\":" +
                                   Dispatch.Quote("[\"" + testMdx.Replace("\\", "\\\\") + "\"]") + "}}";
                    var r3 = Dispatch.Handle(de, addPaths);
                    Ok(r3.Contains("\"ok\":true") && r3.Contains("added"),
                       "派发：engine.dictAdd 走通（参数是嵌在 params 里的 JSON 串）");

                    // 从清单里取 id（壳的流程也是「加完再挑一本当当前词典」）
                    var listAfter = Dispatch.Handle(de, "{\"id\":11,\"method\":\"engine.dictList\"}");
                    var dictId = ExtractId(listAfter, 0);
                    Ok(dictId != null, "派发：从清单里取得到词典 id");
                    Dispatch.Handle(de, "{\"id\":12,\"method\":\"engine.dictSetCurrent\",\"params\":{\"dictId\":" +
                                        Dispatch.Quote(dictId) + "}}");

                    // 内核的业务字段不许被这一层改写：via / stage 原样在
                    var r4 = Dispatch.Handle(de, "{\"id\":3,\"method\":\"lookup.lookup\",\"params\":{" +
                                                 "\"text\":\"apple\",\"origin\":\"selection\"}}");
                    Ok(r4.Contains("\"ok\":true") && r4.Contains("\"via\":\"current\"") &&
                       r4.Contains("\"stage\":\"afterLookup\""),
                       "★ 派发：lookup.lookup 的业务字段原样带出来（这一层不解析业务）");

                    // ★ 设置补丁必须能改到 speech 那一节：这两节曾被当成内核认识的顶层键跳过，
                    //   于是界面上改口音 / 改音色 / 填 Key 一条都存不进去，而且不报错
                    var setAccent = Dispatch.Handle(de,
                        "{\"id\":13,\"method\":\"engine.settingsSet\",\"params\":{" +
                        "\"patch\":\"{\\\"speech\\\":{\\\"accent\\\":\\\"us\\\"}}\"}}");
                    Ok(setAccent.Contains("\"ok\":true") && setAccent.Contains("\"accent\":\"us\""),
                       "★ 派发：engine.settingsSet 打 speech 补丁**当场生效**（回读就是 us）");
                    var getAfter = Dispatch.Handle(de, "{\"id\":14,\"method\":\"engine.settingsGet\"}");
                    Ok(getAfter.Contains("\"accent\":\"us\""),
                       "★ 派发：再读一次设置，口音仍然是 us（不是只回显了补丁）");

                    var r5 = Dispatch.Handle(de, "{\"id\":4,\"method\":\"text.analyze\",\"params\":{\"text\":\"dic·tion·ar·y\"}}");
                    Ok(r5.Contains("\"ok\":true") && r5.Contains("withoutSeparatorDots"),                       "派发：text.analyze 不需要引擎也能用（纯函数那条路）");

                    /* ★ shell.xxx 那条窄缝：弹系统文件对话框、读配置目录、写剪贴板这几件事内核不管、页面也干不了，
                     * 它们经 Dispatch 到壳的 IShellHost。真实程序 gate 里第一条没法验（模态对话框会把自动化卡死在那一刻），
                     * 所以这里喂一个假壳，钉的是「派发真把话转给了壳、而且原样把壳的答复带回来」。
                     */
                    {
                        var fake = new FakeShellHost();
                        var pick = Dispatch.Handle(de,
                            "{\"id\":21,\"method\":\"shell.pickDictionaries\"}", fake);
                        Ok(pick.Contains("\"ok\":true") &&
                           pick.Contains("\\\\dicts\\\\a.mdx") &&
                           fake.Picked == 1,
                           "★ 派发：shell.pickDictionaries 走的是**壳**（假壳给的路径原样带回来）");

                        /* 配置目录那条窄缝（给「常规 → 数据」那一组用）：页面靠它显示路径 + 打开那一格。
                         * 回的是字符串（不是 JSON 对象），所以连引号转义一起验 —— 路径里带反斜杠是最常见的情形。
                         */
                        var cfg = Dispatch.Handle(de, "{\"id\":22,\"method\":\"shell.configDir\"}", fake);
                        Ok(cfg.Contains("\"ok\":true") && cfg.Contains("\\\\fake\\\\config") &&
                           fake.ConfigDirReads == 1,
                           "★ 派发：shell.configDir 把壳给的路径**原样**带回来（反斜杠转义过）");

                        /* ★ 写剪贴板（正文那排浮层的「复制」靠它）：词条正文在 sandbox 的跨源 iframe 里，
                         * navigator.clipboard 那一套在那儿用不了。
                         */
                        var clip = Dispatch.Handle(de,
                            "{\"id\":25,\"method\":\"shell.writeClipboard\",\"params\":{" +
                            "\"text\":\"选中的文字\"}}", fake);
                        Ok(clip.Contains("\"ok\":true") && fake.ClipboardWrites == 1 &&
                           fake.ClipboardText == "选中的文字",
                           "★ 派发：shell.writeClipboard 真的把那段文字交给了壳（原样，没改写）");
                        Ok(clip.Contains("\"bytes\":15"),
                           "★ 而且回一段实测值（15 = 那五个汉字按 UTF-8 算的字节数）—— " +
                           "验收靠它分开「页面调了」与「真的写进去了」");
                        /* 空串是合法入参（= 清空剪贴板），**缺 text** 才是错 */
                        var clipEmpty = Dispatch.Handle(de,
                            "{\"id\":26,\"method\":\"shell.writeClipboard\",\"params\":{\"text\":\"\"}}",
                            fake);
                        Ok(clipEmpty.Contains("\"ok\":true") && fake.ClipboardWrites == 2,
                           "★ 空串也照走（= 清空剪贴板）—— 不许当成「缺参数」");
                        var clipMissing = Dispatch.Handle(de,
                            "{\"id\":27,\"method\":\"shell.writeClipboard\",\"params\":{}}", fake);
                        Ok(clipMissing.Contains("\"ok\":false") &&
                           clipMissing.Contains("缺少参数：text") && fake.ClipboardWrites == 2,
                           "★ 而**没传 text** 如实报「缺少参数：text」（不许悄悄清掉用户的剪贴板）");
                        var clipNoShell = Dispatch.Handle(de,
                            "{\"id\":28,\"method\":\"shell.writeClipboard\",\"params\":{\"text\":\"x\"}}");
                        Ok(clipNoShell.Contains("\"ok\":false") && clipNoShell.Contains("剪贴板"),
                           "★ 没有壳时如实说做不到（不是抛异常、不是不报错地成功）");

                        /* 没有壳的时候（诊断脚本 / 默认那条路）：如实说做不到，不许崩、不许不报错地失败 */
                        var pickNoShell = Dispatch.Handle(de,
                            "{\"id\":23,\"method\":\"shell.pickDictionaries\"}");
                        Ok(pickNoShell.Contains("\"ok\":false") && pickNoShell.Contains("文件对话框"),
                           "★ 派发：没有壳时 shell.pickDictionaries 回人话（不是抛、不是悄悄过去）");
                        var cfgNoShell = Dispatch.Handle(de,
                            "{\"id\":24,\"method\":\"shell.configDir\"}");
                        Ok(cfgNoShell.Contains("\"ok\":false") && cfgNoShell.Contains("配置目录"),
                           "★ 派发：没有壳时 shell.configDir 回人话（不是抛、不是悄悄过去）");
                    }

                    /* ★ history.xxx 那两条：历史落在 <配置目录>/history.jsonl（一行一条 JSON 的追加文件）。
                     * 这里只验派发那条路 + 「查一个词之后历史里真有它、清空之后真的是空的」；
                     * 分页 / 去重窗口 / 上限 / 写放大 / 坏行容忍那些规则在 B 级的 tests/test_history.c，这一层不复述。
                     */
                    {
                        var hist = Dispatch.Handle(de,
                            "{\"id\":31,\"method\":\"history.query\",\"params\":{\"offset\":0,\"limit\":10}}");
                        Ok(hist.Contains("\"ok\":true") && hist.Contains("\"total\":"),
                           "★ 派发：history.query 回的是内核那份 JSON（total / items / hasMore）");
                        var cleared = Dispatch.Handle(de, "{\"id\":32,\"method\":\"history.clear\"}");
                        Ok(cleared.Contains("\"ok\":true") && cleared.Contains("\"total\":0"),
                           "★ 派发：history.clear 清空之后是 0 条");
                        Dispatch.Handle(de, "{\"id\":33,\"method\":\"lookup.lookup\",\"params\":{" +
                                            "\"text\":\"apple\",\"origin\":\"input\"}}");
                        var afterLookup = Dispatch.Handle(de,
                            "{\"id\":34,\"method\":\"history.query\",\"params\":{}}");
                        Ok(afterLookup.Contains("\"word\":\"apple\""),
                           "★ 查到一个词之后历史里就有它（经桥走完整条路）");
                        Dispatch.Handle(de, "{\"id\":35,\"method\":\"history.clear\"}");
                    }

                /* 朗读的规划：三层音源的排序在内核，壳只做两件平台能力的事 —— 递进要念的文本、
                 * 探测本机装了哪些离线音色。页面递来的 voicesJson 不作数（Speech.Plan 无条件覆盖），
                 * 因为页面在沙箱里根本问不到 SAPI；所以两个分支都要断言，不写「没嗓子就跳过」那种会不报错地变绿的写法。
                 */
                var installedVoices = ProbeInstalledVoices();
                bool hasEnglishVoice = false;
                foreach (var name in installedVoices)
                {
                    if (name.IndexOf("Zira", StringComparison.OrdinalIgnoreCase) >= 0 ||
                        name.IndexOf("David", StringComparison.OrdinalIgnoreCase) >= 0 ||
                        name.IndexOf("Mark", StringComparison.OrdinalIgnoreCase) >= 0 ||
                        name.IndexOf("English", StringComparison.OrdinalIgnoreCase) >= 0)
                    {
                        hasEnglishVoice = true;
                    }
                }
                Console.WriteLine("  本机离线音色：" + (installedVoices.Count == 0
                    ? "（一个都没问到）"
                    : string.Join(" / ", installedVoices.ToArray())));
                {
                    var plan1 = Dispatch.Handle(de,
                        "{\"id\":41,\"method\":\"speech.plan\",\"params\":{\"text\":\"apple\"}}");
                    Console.WriteLine("  speech.plan(apple): " + Truncate(plan1, 320));
                    /* ⚠️ 钉的必须是顶层那个 source：不能只找 source 等于 dict —— options 里每一项也带一个 source，
                     * 那样写会因为错的理由变绿（顶层其实是 none，而断言匹配到了 options 里那一条 dict、available 为 false）。
                     */
                    Ok(plan1.Contains("\"ok\":true") &&
                       plan1.Contains("\"result\":{\"source\":\"" +
                                      (hasEnglishVoice ? "system" : "none") + "\""),
                       "★ 派发：speech.plan 的音色表**由壳探**（本机" +
                       (hasEnglishVoice ? "有英文嗓子 → 落到 system" : "没有英文嗓子 → 如实 none") + "）");
                    Ok(plan1.Contains("\"chunkCount\":1"),
                       "★ 一个词 → 一段（切段也在内核里）");
                    Ok(plan1.Contains("本词条没有自带录音"),
                       "★ 而 dict 那一层如实说了为什么不通");

                    /* 页面递来的音色表不作数：这里递一把根本不存在的嗓子，壳若把它当回事就会挑中它。
                     * 检查标准钉的是答复里没有那个名字 —— 本机真有的嗓子随便叫什么都不会叫这个。
                     */
                    var plan2 = Dispatch.Handle(de,
                        "{\"id\":42,\"method\":\"speech.plan\",\"params\":{\"text\":\"ghost\"," +
                        "\"voicesJson\":\"[{\\\"id\\\":\\\"NO-SUCH-VOICE\\\",\\\"language\\\":\\\"en\\\"," +
                        "\\\"culture\\\":\\\"en-US\\\"}]\"}}");
                    Console.WriteLine("  speech.plan(ghost, 伪造音色表): " + Truncate(plan2, 320));
                    Ok(!plan2.Contains("NO-SUCH-VOICE"),
                       "★★ 页面递来的音色表**被忽略**（壳自己探；伪造的那把嗓子一个字段都没进答复）");
                    Ok(plan2.Contains("\"result\":{\"source\":\"" +
                                      (hasEnglishVoice ? "system" : "none") + "\""),
                       "★ 而顶层 source 仍然按**本机真实音色**判（" +
                       (hasEnglishVoice ? "system" : "none") + "）");

                    /* 没给音色表这个入参：壳照旧自己去探，「问不到」只在探测失败时出现 */
                    var plan3 = Dispatch.Handle(de,
                        "{\"id\":43,\"method\":\"speech.plan\",\"params\":{\"text\":\"ghost\"}}");
                    if (installedVoices.Count > 0)
                    {
                        Ok(!plan3.Contains("问不到本机的离线音色"),
                           "★ 壳探得到音色 → 那一层**不说**「问不到」（那是探测失败时才说的话）");
                    }
                    else
                    {
                        Ok(plan3.Contains("问不到本机的离线音色"),
                           "★ 壳探不到音色（本机没问到任何嗓子）→ 那一层如实说「问不到」");
                    }

                    /* 空文本那一种情况指的是「没有要念的文本」，不是「本机没有这个语种」——
                     * 少了这一条，界面上「朗读」按钮会在还没查过词时亮着。
                     */
                    var plan4 = Dispatch.Handle(de,
                        "{\"id\":44,\"method\":\"speech.plan\",\"params\":{\"text\":\"   \"}}");
                    Ok(plan4.Contains("\"result\":{\"source\":\"none\"") &&
                       plan4.Contains("没有要念的文本。"),
                       "★ 空文本/纯空白：三层一律不通，且给的是「没有要念的文本。」那句人话");

                    /* ══ 真的念：speech.speak（宿主那一半）—— 走词典那层回的地址是 /__sound__/<键名>，
                     * 走系统那层真的用本机 SAPI 合成一段 WAV 并回 /__speech__/<键>；接段必须对
                     * （RIFF 总长与 data 块长要与真实字节数一致，接错播放器只会给一句看不懂的错）；
                     * 念不出来时说的那句话是内核给的，不是宿主编的。
                     */
                    var speakEmpty = Dispatch.Handle(engine,
                        "{\"id\":51,\"method\":\"speech.speak\",\"params\":{\"text\":\"\"}}");
                    Console.WriteLine("  speech.speak(空文本): " + Truncate(speakEmpty, 320));
                    Ok(speakEmpty.Contains("\"result\":{\"ok\":false") &&
                       speakEmpty.Contains("没有要念的文本。"),
                       "★ 空文本：`ok:false` 而且那句人话是内核给的（宿主不许编）");
                    Ok(speakEmpty.Contains("\"url\":\"\""),
                       "★ 念不出来时**不给地址**（不给一个点了没反应的地址）");

                    var speakDict = Dispatch.Handle(engine,
                        "{\"id\":52,\"method\":\"speech.speak\",\"params\":{\"text\":\"beep\"," +
                        "\"dictId\":" + Dispatch.Quote(audioId) + "}}");
                    Console.WriteLine("  speech.speak(beep@audio): " + Truncate(speakDict, 320));
                    Ok(speakDict.Contains("\"result\":{\"ok\":true") &&
                       speakDict.Contains("\"source\":\"dict\""),
                       "★ 有原录音的词：speak 走 dict 那一层");
                    /* ⚠️ 地址必须在外壳站点上（不是词典域那条）、而且带词典 id 那一段：
                     * 宿主页的 CSP 是 default-src self，跨域的 *.dictres.invalid/__sound__/… 取不到也放不了
                     * （症状是「有 url、点下去没声音」）。两条路由都落在 VirtualHost.ServeDictSound 上。
                     */
                    Ok(speakDict.Contains("https://" + VirtualHost.ShellDomain + VirtualHost.SoundRoute +
                                          audioId + "/"),
                       "★ 回的地址是**外壳站点上那条** `/__sound__/<词典 id>/<键名>`（宿主页 CSP 只放行同源）");
                    Ok(speakDict.Contains("beep__gb_1.wav"),
                       "★ 键名就是那卷里的那一个（规划已经把键挑好了）");

                    /* 系统离线那一层：真的合成一段 WAV，再把那条地址取回来验内容 */
                    var speakSys = Dispatch.Handle(engine,
                        "{\"id\":53,\"method\":\"speech.speak\",\"params\":{\"text\":\"apple\"," +
                        "\"dictId\":" + Dispatch.Quote(testId) + "}}");
                    Console.WriteLine("  speech.speak(apple@test): " + Truncate(speakSys, 320));
                    var speakUrl = Lookup.Host.VirtualHost.JsonString(speakSys, "url");
                    var speakBytes = Lookup.Host.VirtualHost.JsonLong(speakSys, "bytes");
                    if (hasEnglishVoice)
                    {
                        Ok(speakSys.Contains("\"result\":{\"ok\":true") &&
                           speakSys.Contains("\"source\":\"system\""),
                           "★ 没有原录音、本机有英文嗓子 → speak 走 system 那一层（真的合成了）");
                        Ok(speakSys.Contains("\"mime\":\"audio/wav\"") && speakBytes > 44,
                           "★ 合成出来的是 WAV 而且不是空壳（> 44 字节那个头）");
                        Ok(speakUrl != null && speakUrl.StartsWith(
                               "https://" + VirtualHost.ShellDomain + VirtualHost.SpeechRoute,
                               StringComparison.Ordinal),
                           "★ 回的地址挂在外壳站点下（`https://lookup.local/__speech__/<键>`）");

                        /* 那条地址真的发得出字节，而且发出来的是一份**结构自洽**的 WAV */
                        var served = VirtualHost.Serve(engine, new VirtualRequest { Url = speakUrl }, shell);
                        Ok(served.Status == 200 && served.ContentType == "audio/wav",
                           "★ 那条地址由宿主发得出来（200 + audio/wav）");
                        Ok(served.Body != null && served.Body.Length == speakBytes,
                           "★ 发出去的字节数与 speak 报的 `bytes` 一致");
                        int dataAt, riffSize, dataSize;
                        Ok(RiffParts(served.Body, out dataAt, out riffSize, out dataSize) &&
                           riffSize == served.Body.Length - 8 &&
                           dataAt + dataSize == served.Body.Length,
                           "★ 发出去的那份是**自洽的 WAV**（RIFF 总长与 data 块长都对得上字节数）");

                        /* 同一段话再点一次：**命中暂存**，不再合成一遍（同一个键、同一个地址） */
                        var again = Dispatch.Handle(engine,
                            "{\"id\":54,\"method\":\"speech.speak\",\"params\":{\"text\":\"apple\"," +
                            "\"dictId\":" + Dispatch.Quote(testId) + "}}");
                        Ok(again.Contains("\"cached\":true") &&
                           string.Equals(Lookup.Host.VirtualHost.JsonString(again, "url"), speakUrl,
                                         StringComparison.Ordinal),
                           "★ 同一段话再念一次：命中暂存（`cached:true`，键一样、地址一样）");
                    }
                    else
                    {
                        Ok(speakSys.Contains("\"result\":{\"ok\":false") &&
                           speakSys.Contains("本机没有"),
                           "★ 本机没有英文嗓子 → 如实说念不了（那句话是内核给的）");
                    }

                    /* 长文本切段之后要接成一段（切段是内核的约定，接是宿主的手艺）：内核切成好几段，
                     * 宿主一段一段合成、把 data 顺次接起来、并把 RIFF 与 data 两处长度写对。
                     * ⚠️ 只断「字节数 > 单段」不够 —— 把两段 WAV 头尾一拼也是绿的，所以用 RiffParts 钉总长。
                     */
                    if (hasEnglishVoice)
                    {
                        var longText = string.Join(" ", System.Linq.Enumerable.Repeat(
                            "the quick brown fox jumps over the lazy dog", 30).ToArray());
                        var speakLong = Dispatch.Handle(engine,
                            "{\"id\":55,\"method\":\"speech.speak\",\"params\":{\"text\":" +
                            Dispatch.Quote(longText) + "}}");
                        var longUrl = Lookup.Host.VirtualHost.JsonString(speakLong, "url");
                        var longChunks = Lookup.Host.VirtualHost.JsonLong(speakLong, "chunks");
                        Console.WriteLine("  speech.speak(长文本): " + Truncate(speakLong, 260));
                        Ok(speakLong.Contains("\"result\":{\"ok\":true") &&
                           longChunks > 1,
                           "★ 长文本：内核切了好几段、宿主一段一段都合成了（chunks=" + longChunks + "）");
                        var longServed = VirtualHost.Serve(engine,
                            new VirtualRequest { Url = longUrl }, shell);
                        int longDataAt, longRiff, longData;
                        Ok(longServed.Status == 200 &&
                           RiffParts(longServed.Body, out longDataAt, out longRiff, out longData) &&
                           longRiff == longServed.Body.Length - 8 &&
                           longDataAt + longData == longServed.Body.Length,
                           "★★ 接成的那一段仍然是**自洽的 WAV**（两处长度都按接完的真实字节数写对了）");
                        Ok(longServed.Body.Length > (int)speakBytes,
                           "★ 而且比一个词那一段长（真的把几段接进去了，不是只发了第一段）");
                    }

                    /* 键不认识（缓存被挤掉 / 手输的地址）：404 加一句人话，不是空响应 */
                    var gone = VirtualHost.Serve(engine, new VirtualRequest
                    {
                        Url = "https://" + VirtualHost.ShellDomain + VirtualHost.SpeechRoute +
                              new string('0', 64)
                    }, shell);
                    Ok(gone.Status == 404 && gone.Reason != null && gone.Reason.Contains("再点一次"),
                       "★ 键不认识 → 404 并说「再点一次就有了」（暂存满了会到这儿，不是错误）");

                    /* ★ 在线那一层必须如实说，不许悄悄退回别的层：填了 Key 之后规划说走在线，
                     * speak 就必须回 ok:false + 一句人话，而不是偷偷用本机嗓子念一遍（那样用户会以为「在线通了」）。
                     * 这一段不联网（在线那条路连第一步都没走到）。
                     */
                    Dispatch.Handle(engine, "{\"id\":56,\"method\":\"engine.settingsSet\"," +
                                            "\"params\":{\"patch\":" +
                                            Dispatch.Quote("{\"speech\":{\"doubaoApiKey\":\"test-key-123\"}}") +
                                            "}}");
                    var speakOnline = Dispatch.Handle(engine,
                        "{\"id\":57,\"method\":\"speech.speak\",\"params\":{\"text\":\"apple\"," +
                        "\"dictId\":" + Dispatch.Quote(testId) + "}}");
                    Console.WriteLine("  speech.speak(填了假 Key → 在线那层): " +
                                      Truncate(speakOnline, 300));
                    /* ⚠️ 在线那一层已经接上了，所以拿一把假 Key 去问，回的是服务端自己的那句话；
                     * 要守的约定没变：source 仍是 online、绝不不报错地退回本机嗓子
                     * （退了的话用户会以为「豆包就是这么难听」，那是最难查的一类错觉）。
                     */
                    Ok(speakOnline.Contains("\"result\":{\"ok\":false") &&
                       speakOnline.Contains("\"source\":\"online\"") &&
                       !speakOnline.Contains("还没接上"),
                       "★ 规划走在线时**真的去问了服务端**（`source` 仍是 online，不悄悄退回本机嗓子）");
                    Ok(speakOnline.Contains("\"url\":\"\""),
                       "★ 而且**不给地址**（界面不会去播一段来路不明的声音）");
                    Dispatch.Handle(engine, "{\"id\":58,\"method\":\"engine.settingsSet\"," +
                                            "\"params\":{\"patch\":" +
                                            Dispatch.Quote("{\"speech\":{\"doubaoApiKey\":null}}") + "}}");
                }

                // 音频预处理：把那卷里的字节交给内核，问一句能不能直接播
                IntPtr audioBytes = IntPtr.Zero;
                UIntPtr audioLen = UIntPtr.Zero;
                string audioMeta;
                Dsh.EngineResource(engine, audioId, "\\beep__gb_1.wav", UIntPtr.Zero, UIntPtr.Zero,
                                   out audioBytes, out audioLen, out meta);
                if (audioBytes != IntPtr.Zero)
                {
                    IntPtr prepared = IntPtr.Zero;
                    UIntPtr preparedLen = UIntPtr.Zero;
                    Dsh.AudioPrepare(audioBytes, audioLen, out prepared, out preparedLen, out audioMeta);
                    Console.WriteLine("  audio_prepare: " + audioMeta);
                    Ok(audioMeta != null && audioMeta.Contains("\"kind\":\"wav\""),
                       "那段字节应当被认成 wav");
                    Ok(prepared != IntPtr.Zero && (ulong)preparedLen == (ulong)audioLen,
                       "能直接播的格式原样返回（长度不变）");
                    if (prepared != IntPtr.Zero) DshRaw.dsh_release(prepared);
                    DshRaw.dsh_release(audioBytes);
                }
                else
                {
                    Ok(false, "取不到音频字节，后面两条没法验");
                }

                /* ★ Speex 的跨编译器验证：同一个 .spx、同一份内核源码，Linux 侧用 gcc 编、这里用 MinGW 编，
                 * 解出来的 PCM 必须是同一串字节 —— 断言的哈希与 B 级 tests/test_audio.c 里钉的是同一个常量。
                 * 对不上也必须当场报出来（浮点在不同编译器下的末位差异），不许糊过去。
                 */
                {
                    IntPtr spxBytes = IntPtr.Zero;
                    UIntPtr spxLen = UIntPtr.Zero;
                    string spxMeta0;
                    Dsh.EngineResource(engine, audioId, "\\speexword__gb_1.spx", UIntPtr.Zero,
                                       UIntPtr.Zero, out spxBytes, out spxLen, out spxMeta0);
                    Ok(spxBytes != IntPtr.Zero && (ulong)spxLen > 1000,
                       "★ 取得到那段真 Speex（audio.mdd 里的 `\\speexword__gb_1.spx`）");
                    if (spxBytes != IntPtr.Zero)
                    {
                        IntPtr wav = IntPtr.Zero;
                        UIntPtr wavLen = UIntPtr.Zero;
                        string spxMeta;
                        Dsh.AudioPrepare(spxBytes, spxLen, out wav, out wavLen, out spxMeta);
                        Console.WriteLine("  audio_prepare(spx): " + spxMeta);
                        Ok(spxMeta != null && spxMeta.Contains("\"kind\":\"spx\"") &&
                           spxMeta.Contains("\"decoded\":true") &&
                           spxMeta.Contains("\"mime\":\"audio/wav\""),
                           "★ `.spx` 认成 spx、**解出来了**、而且按 `audio/wav` 发出去");
                        Ok(spxMeta != null && spxMeta.Contains("\"samples\":32000") &&
                           spxMeta.Contains("\"sampleRate\":16000"),
                           "★ 解出来 32000 个采样 / 16000 Hz（100 包 × 320）");
                        const string wantPcmSha =
                            "fc7d90b1d818b1567cd71878f400ceda3847e8dfcb5285ab1a2af38b16b76344";
                        if (wav != IntPtr.Zero && (ulong)wavLen > 44)
                        {
                            byte[] head = new byte[4];
                            Marshal.Copy(wav, head, 0, 4);
                            Ok(head[0] == 0x52 && head[1] == 0x49 && head[2] == 0x46 &&
                               head[3] == 0x46,
                               "★ 解出来的字节是真正的 WAV（RIFF 头）");
                            byte[] pcm = new byte[(long)wavLen - 44];
                            Marshal.Copy(new IntPtr(wav.ToInt64() + 44), pcm, 0, pcm.Length);
                            string got;
                            using (var sha = System.Security.Cryptography.SHA256.Create())
                            {
                                var d = sha.ComputeHash(pcm);
                                var sb = new System.Text.StringBuilder(d.Length * 2);
                                foreach (var b in d) sb.Append(b.ToString("x2"));
                                got = sb.ToString();
                            }
                            Console.WriteLine("  spx PCM SHA256: " + got);
                            Ok(got == wantPcmSha,
                               "★★ 解出来的 PCM 与 Linux（gcc）那一份**逐字节相同** —— " +
                               "同一个 golden 管两个平台（实际 " + got + "）");
                        }
                        else
                        {
                            Ok(false, "Speex 没解出 WAV 字节");
                        }
                        if (wav != IntPtr.Zero) DshRaw.dsh_release(wav);
                        DshRaw.dsh_release(spxBytes);
                    }

                    // 负例：坏码流必须**明确说解不了**，一个字节都不交出去
                    IntPtr brokenBytes = IntPtr.Zero;
                    UIntPtr brokenLen = UIntPtr.Zero;
                    string brokenMeta0;
                    Dsh.EngineResource(engine, audioId, "\\speexbroken__gb_1.spx", UIntPtr.Zero,
                                       UIntPtr.Zero, out brokenBytes, out brokenLen, out brokenMeta0);
                    if (brokenBytes != IntPtr.Zero)
                    {
                        IntPtr outBytes = IntPtr.Zero;
                        UIntPtr outLen = UIntPtr.Zero;
                        string brokenMeta;
                        Dsh.AudioPrepare(brokenBytes, brokenLen, out outBytes, out outLen,
                                         out brokenMeta);
                        Console.WriteLine("  audio_prepare(speexbroken): " + brokenMeta);
                        Ok(brokenMeta != null && brokenMeta.Contains("\"playable\":false") &&
                           brokenMeta.Contains("\"decoded\":false"),
                           "★ 坏掉的 Speex 如实报「解不了」（不假装能播）");
                        Ok(outBytes == IntPtr.Zero && (ulong)outLen == 0,
                           "★ 而且一个字节都不交出去");
                        if (outBytes != IntPtr.Zero) DshRaw.dsh_release(outBytes);
                        DshRaw.dsh_release(brokenBytes);
                    }
                    else
                    {
                        Ok(false, "取不到那段坏 Speex");
                    }
                }


                    // 错误路径：都要回 ok:false + 一句人话，绝不抛
                    var e1 = Dispatch.Handle(de, "{\"id\":5,\"method\":\"no.suchMethod\"}");
                    Ok(e1.Contains("\"ok\":false") && e1.Contains("不认识的方法"),
                       "★ 派发：不认识的方法回 ok:false（不是抛异常）");
                    var e2 = Dispatch.Handle(de, "{this is not json");
                    Ok(e2.Contains("\"ok\":false") && e2.Contains("method"),
                       "★ 派发：坏 JSON 也回 ok:false，并说清缺的是 method");
                    var e3 = Dispatch.Handle(de, "{\"id\":6,\"method\":\"lookup.resolve\",\"params\":{\"dictId\":null}}");
                    Ok(e3.Contains("\"ok\":false") && e3.Contains("缺少参数：text"),
                       "★ 派发：缺必填参数如实报（不拿空串顶上去 —— 那会变成「查了个空词」）");
                    /* ⚠️ 「词典里没有这本」不是内核错误：resolve 会成功返回一份带 reason 的载荷
                     *    （接口定义里明说 enabled 与 landed 是两件事），所以这里应当 ok:true。 */
                    var e4 = Dispatch.Handle(de, "{\"id\":8,\"method\":\"lookup.resolve\",\"params\":{\"text\":\"x\",\"dictId\":\"no-such\"}}");
                    Ok(e4.Contains("\"ok\":true") && e4.Contains("词库里没有"),
                       "★ 派发：「词库里没有这本」是**成功返回 + 一句 reason**，不是内核错误");

                    // 真正会失败的内核调用：删一本不存在的词典 → 错误码 → ok:false + 内核那句人话
                    var e4b = Dispatch.Handle(de, "{\"id\":81,\"method\":\"engine.dictSetCurrent\",\"params\":{\"dictId\":\"no-such\"}}");
                    OkJson(e4b.Contains("\"ok\":false") && e4b.Contains("no-such"), e4b,
                       "★ 派发：内核真报错时翻成 ok:false + 内核那句人话（带着 id）");
                    Ok(e4b.Contains("\"id\":81"), "派发：失败时也要把 id 原样带回（前端靠它配对）");
                    var e5 = Dispatch.Handle(IntPtr.Zero, "{\"id\":9,\"method\":\"engine.dictList\"}");
                    Ok(e5.Contains("\"ok\":false") && e5.Contains("引擎"),
                       "★ 派发：引擎为 0 时如实报（不是崩）");

                    // 这一层刻意**不提供** bytes 类接口：字节不过桥
                    var e6 = Dispatch.Handle(de, "{\"id\":10,\"method\":\"lookup.resource\",\"params\":{\"dictId\":\"x\",\"key\":\"y\"}}");
                    Ok(e6.Contains("\"ok\":false") && e6.Contains("不认识的方法"),
                       "★ 派发：`lookup.resource` **不在表里**（字节走资源路由，不过桥）");

                    // params 的抠取要配对括号（嵌套对象不能被第一个 } 截断）
                    var nested = Dispatch.RawObject("{\"params\":{\"a\":{\"b\":1},\"c\":2}}", "params");
                    Ok(nested == "{\"a\":{\"b\":1},\"c\":2}",
                       "派发：params 的抠取按括号配对（不是找第一个 }）");
                }
                finally
                {
                    if (de != IntPtr.Zero) Dsh.EngineDestroy(de);
                }

            }
            catch (Exception ex)
            {
                Ok(false, "engine 那条链抛了异常：" + ex.Message);
                Console.WriteLine(ex.ToString());
            }
            finally
            {
                if (engine != IntPtr.Zero) Dsh.EngineDestroy(engine);
            }

            return Finish();
        }

        /// <summary>本机装了哪些离线音色 —— 验收自己问一遍，不复用壳那份探测：
        /// 问壳就成了「拿实现验实现」（它说有什么就有什么），于是「壳真把本机那份表递进去了吗」不可判。</summary>
        private static List<string> ProbeInstalledVoices()
        {
            var list = new List<string>();
            try
            {
                using (var synth = new System.Speech.Synthesis.SpeechSynthesizer())
                {
                    foreach (var v in synth.GetInstalledVoices())
                    {
                        if (v.Enabled) list.Add(v.VoiceInfo.Name);
                    }
                }
            }
            catch (Exception err)
            {
                Console.WriteLine("  （本机问不到离线音色：" + err.Message + "）");
            }
            return list;
        }

        /// <summary>拆一个 WAV 的 fmt / data 块（验收自己拆，不复用壳体那段拼接代码）：
        /// 「接段接对了没有」只能这么判 —— 头尾一拼也是「更长的一段」，只有 RIFF 总长与 data 块长对得上才算对。</summary>
        private static bool RiffParts(byte[] body, out int dataAt, out int riffSize, out int dataSize)
        {
            dataAt = 0;
            riffSize = 0;
            dataSize = 0;
            if (body == null || body.Length < 12) return false;
            if (body[0] != 'R' || body[1] != 'I' || body[2] != 'F' || body[3] != 'F') return false;
            if (body[8] != 'W' || body[9] != 'A' || body[10] != 'V' || body[11] != 'E') return false;
            riffSize = body[4] | (body[5] << 8) | (body[6] << 16) | (body[7] << 24);
            int at = 12;
            bool found = false;
            while (at + 8 <= body.Length)
            {
                int size = body[at + 4] | (body[at + 5] << 8) | (body[at + 6] << 16) |
                           (body[at + 7] << 24);
                if (size < 0 || at + 8 + size > body.Length) return false;
                string id = System.Text.Encoding.ASCII.GetString(body, at, 4);
                if (id == "data")
                {
                    dataAt = at + 8;
                    dataSize = size;
                    found = true;
                }
                at += 8 + size + ((size % 2) == 1 ? 1 : 0);
            }
            return found;
        }

        /// <summary>从词库清单 JSON 里取第 index 本的 id（够用的极简取值器）</summary>
        private static string ExtractId(string list, int index)
        {
            if (list == null) return null;
            int at = -1;
            for (int i = 0; i <= index; i++)
            {
                at = list.IndexOf("\"id\":\"", at + 1, StringComparison.Ordinal);
                if (at < 0) return null;
            }
            at += 6;
            int end = list.IndexOf('"', at);
            return end > at ? list.Substring(at, end - at) : null;
        }

        /// <summary>假壳：D 级里替真实窗口接住 shell 的那几条 —— 它只记下「被问到了」、回一个能认出来的答复，
        /// 于是断言能分开「派发把话转给壳了吗」与「壳自己做得对不对」（后者只有真实窗口能验）。</summary>
        private sealed class FakeShellHost : Lookup.Host.IShellHost
        {
            internal int Picked;
            internal int ConfigDirReads;
            internal int ClipboardWrites;
            internal string ClipboardText;

            public string PickDictionaryFiles()
            {
                Picked++;
                return "[\"C:\\\\dicts\\\\a.mdx\",\"C:\\\\dicts\\\\b.mdx\"]";
            }

            /// <summary>配置目录：回一条带反斜杠的假路径 —— 那句话在 JSON 里要转义，
            /// 正好把「壳的字符串有没有原样带回去」一起验掉。</summary>
            public string ConfigDirPath()
            {
                ConfigDirReads++;
                return @"C:\fake\config";
            }

            /// <summary>切回 UI 线程：假壳没有窗口，就地执行（与 NoShellHost 同一条规矩）——
            /// 于是「要联网那几条路」在这里退回同步做，DLL 检查验的就是那条同步路。</summary>
            public void PostToUiThread(Action action)
            {
                if (action != null) action();
            }

            public void WriteClipboard(string text)
            {
                ClipboardWrites++;
                ClipboardText = text;
            }

            /// <summary>读剪贴板：回一段**认得出来**的假文本，不碰真的剪贴板（那条在真实程序 gate 里验）</summary>
            public string ReadClipboard()
            {
                ClipboardReads++;
                return "壳给的剪贴板内容";
            }

            public void RevealInPath(string path)
            {
                RevealedPath = path;
            }

            /// <summary>推事件：记下来（「派发有没有让壳通知界面」靠它判）</summary>
            public void Emit(string name, string payloadJson)
            {
                Emitted.Add(name + " " + (payloadJson ?? "null"));
            }

            /* ── 广播：词库 / 设置变了要通知所有开着的窗口 ──
             * 这里只记「被叫过、参数是什么」；实际投递属于窗口那一半，只在真实程序那道 gate 里验。
             * ⚠️ 这个替身原来漏了它 —— 缺一个接口成员会让这道门编译不过，而那道门没人跑到就一直没被发现。 */

            internal readonly List<string> Broadcasts = new List<string>();

            public void Broadcast(string name, string payloadJson)
            {
                Broadcasts.Add(name + " " + (payloadJson ?? "null"));
            }

            /* ── 窗口那一组（界面搬过来之后新增的）：只记「被叫了几次、参数是什么」──
             * 窗口几何与 Region 的真值只有真实窗口量得出来（D 级那道 gate），这一层管的是派发有没有把话原样转给壳。 */

            internal readonly List<string> WindowCalls = new List<string>();

            public string LayoutInfo()
            {
                WindowCalls.Add("layout:info");
                return "{\"workArea\":{\"x\":0,\"y\":0,\"width\":1920,\"height\":1040}," +
                       "\"scaleFactor\":1,\"edge\":null,\"pillX\":728,\"pillY\":0,\"pillTop\":0}";
            }

            public void SetLayout(string requestJson) { WindowCalls.Add("layout:set " + requestJson); }
            public void SetShape(string regionsJson, string theme, bool focused)
            {
                WindowCalls.Add("shape:set " + regionsJson + " theme=" + theme + " focused=" + focused);
            }
            public void DragPrepare() { WindowCalls.Add("drag:prepare"); }
            public void DragStart() { WindowCalls.Add("drag:start"); }
            public void DragMove(double? sentAtMs) { WindowCalls.Add("drag:move " + sentAtMs); }
            public void DragEnd() { WindowCalls.Add("drag:end"); }
            public void ExpandFromEdge() { WindowCalls.Add("edge:expand"); }
            public void CollapseToEdge() { WindowCalls.Add("edge:collapse"); }
            public void ResetPill() { WindowCalls.Add("floating:reset"); }
            public void AbsorbToEdge(string edgeWire) { WindowCalls.Add("debug:absorb " + edgeWire); }
            public void HideWindow() { WindowCalls.Add("window:hide"); }
            public void RequestClose() { WindowCalls.Add("window:requestClose"); }
            public void ResolveClose(string choice, bool remember)
            {
                WindowCalls.Add("window:resolveClose " + choice + " remember=" + remember);
            }
            public string DescribeWindow(string role) { return "{\"present\":false}"; }
            public string DragStats() { return "{\"frames\":3,\"median\":1.5,\"p95\":3,\"max\":4,\"samples\":[]}"; }
            public void PlacePill(double x, double y) { WindowCalls.Add("debug:placePill " + x + "," + y); }
            public void ImportDictionary(string path) { WindowCalls.Add("debug:importDictionary " + path); }

            /* ── 词库管理窗那一组（第二扇窗）：只记「被叫过、参数是什么」── */

            internal readonly List<string> ManagerCalls = new List<string>();
            internal string RequestedTab;

            public void OpenManager(string tab)
            {
                RequestedTab = tab;
                ManagerCalls.Add("manager:open " + tab);
            }
            public void ManagerClose() { ManagerCalls.Add("manager:close"); }
            public void ManagerMinimize() { ManagerCalls.Add("manager:minimize"); }
            public string ManagerConsumeInitialTab() { return RequestedTab ?? ""; }
            public void ManagerBeginDrag() { ManagerCalls.Add("manager:drag"); }
            public void ManagerSurface(string fill, string border, string theme, bool focused)
            {
                ManagerCalls.Add("manager:surface " + fill + " theme=" + theme + " focused=" + focused);
            }

            /* ── 托盘菜单那一组（第三扇窗）：同样只记「被叫过、参数是什么」── */

            internal readonly List<string> TrayCalls = new List<string>();
            internal bool LoginAtStartup;

            public void TrayMenuShow() { TrayCalls.Add("show"); }
            public void TrayMenuHide() { TrayCalls.Add("hide"); }
            public void TrayMenuPlace(double width, double height, string fill, string border, string theme)
            {
                TrayCalls.Add("place " + width + "x" + height + " theme=" + theme);
            }
            public string TrayState()
            {
                return "{\"floatingVisible\":true,\"loginAtStartup\":" +
                       (LoginAtStartup ? "true" : "false") + "}";
            }
            public bool TraySetLogin(bool enabled) { LoginAtStartup = enabled; return enabled; }
            public void TrayToggleFloating() { TrayCalls.Add("toggle"); }
            public void TrayResetPill() { TrayCalls.Add("reset"); }
            public void TrayQuit() { TrayCalls.Add("quit"); }
            public void FocusSelf() { TrayCalls.Add("focus"); }

            internal int ClipboardReads;
            internal string RevealedPath;
            internal readonly List<string> Emitted = new List<string>();
        }

        private static string Truncate(string s, int n)
        {
            if (s == null) return "(null)";
            return s.Length <= n ? s : s.Substring(0, n) + "…";
        }

        private static int Finish()
        {
            Console.WriteLine("windows-dll：" + _checks + " 项，失败 " + _failed);
            return _failed == 0 ? 0 : 1;
        }
    }
}
