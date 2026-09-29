using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading.Tasks;

namespace Lookup.Dictionary;

internal sealed class LoadedDictionary
{
    internal StoredDictionary Stored;
    internal DictionaryInfo Info;
    internal MdxFile Mdx;
    internal List<MddFile> Mdds = new List<MddFile>();
}

/// <summary>词条解析结果（内部使用）</summary>
internal sealed class ResolvedEntry
{
    internal bool Found;
    internal string KeyText;
    internal string Definition;
    internal string LinkedTo;
}

/// <summary>
/// 词典引擎：负责 .mdx / .mdd 的加载、检索与资源读取。
/// 逻辑与 Electron 版的 engine.ts 一一对应，行为刻意保持一致
/// （包括"未命中时不偷偷换成另一个词"这条规则）。
/// </summary>
internal sealed class DictionaryEngine
{
    /// <summary>跟随 @@@LINK= 跳转的最大层数，防止词典内循环引用</summary>
    private const int MaxLinkDepth = 8;

    private readonly object _gate = new object();
    private readonly Dictionary<string, LoadedDictionary> _loaded = new Dictionary<string, LoadedDictionary>();
    private readonly Dictionary<string, Task<LoadedDictionary>> _pending = new Dictionary<string, Task<LoadedDictionary>>();
    /// <summary>每本词典 `.mdx` 头里的书名（读一次就留着，见 <see cref="HeaderTitleOf"/>）</summary>
    private readonly Dictionary<string, string> _headerTitles = new Dictionary<string, string>(StringComparer.Ordinal);

    /* ------------------------------ 元数据 ------------------------------ */

    /// <summary>
    /// 建立词典记录（只读元数据，不加载索引）。
    /// **文件必须真的在** —— `FileInfo` 对不存在的文件不抛异常（时间戳给 1601 年的默认值），
    /// 不挡这一下，"导入一条不存在的路径"就会不报错地成功（见 `开发记录-0.1.x.md` §59）。
    /// </summary>
    internal StoredDictionary Describe(string mdxPath, IEnumerable<string> mddPaths)
    {
        var full = Path.GetFullPath(mdxPath);
        var stat = new FileInfo(full);
        if (!stat.Exists) throw new FileNotFoundException("找不到词典文件：" + full, full);
        var id = HashId(full);
        return new StoredDictionary
        {
            Id = id,
            Title = Path.GetFileNameWithoutExtension(full),
            MdxPath = full,
            MddPaths = (mddPaths ?? Enumerable.Empty<string>())
                .Select(Path.GetFullPath)
                .Distinct(StringComparer.OrdinalIgnoreCase)
                .ToList(),
            AddedAt = new DateTimeOffset(stat.LastWriteTimeUtc).ToUnixTimeMilliseconds()
        };
    }

    internal static string HashId(string fullPath)
    {
        using (var sha1 = SHA1.Create())
        {
            var bytes = sha1.ComputeHash(Encoding.UTF8.GetBytes(fullPath));
            var builder = new StringBuilder(40);
            foreach (var b in bytes) builder.Append(b.ToString("x2"));
            return builder.ToString(0, 16);
        }
    }

    /// <summary>
    /// 扫描 mdx 同目录下同名的所有资源卷。
    /// 命名约定来自 MDict 生态：若主文件是 `foo.mdx`，则资源可能是 `foo.mdd`、`foo.1.mdd`、`foo.2.mdd`…
    /// </summary>
    internal List<string> DiscoverMddFiles(string mdxPath)
    {
        var directory = Path.GetDirectoryName(Path.GetFullPath(mdxPath));
        var baseName = Path.GetFileNameWithoutExtension(mdxPath);
        if (string.IsNullOrEmpty(directory) || !Directory.Exists(directory)) return new List<string>();

        var pattern = new Regex("^" + Regex.Escape(baseName) + @"(?:\.\d+)?\.mdd$", RegexOptions.IgnoreCase);
        try
        {
            return Directory.GetFiles(directory)
                .Where(file => pattern.IsMatch(Path.GetFileName(file)))
                .OrderBy(file => VolumeIndex(Path.GetFileName(file)))
                .ThenBy(file => file, StringComparer.OrdinalIgnoreCase)
                .ToList();
        }
        catch (Exception err)
        {
            Console.Error.WriteLine("[engine] 扫描资源库失败: " + err.Message);
            return new List<string>();
        }
    }

    private static int VolumeIndex(string fileName)
    {
        var match = Regex.Match(fileName, @"\.(\d+)\.mdd$", RegexOptions.IgnoreCase);
        return match.Success ? int.Parse(match.Groups[1].Value) : 0;
    }

    internal bool IsLoaded(string id)
    {
        lock (_gate) return _loaded.ContainsKey(id);
    }

    internal bool IsPending(string id)
    {
        lock (_gate) return _pending.ContainsKey(id);
    }

    /* ------------------------------ 加载 ------------------------------ */

    /// <summary>
    /// 加载词典索引。
    ///
    /// 解析本身是同步的（但会把整个词块索引建起来，大词典也要几百毫秒），
    /// 所以一律放到线程池上跑，别卡住消息循环。
    /// </summary>
    internal Task<LoadedDictionary> LoadAsync(StoredDictionary stored)
    {
        lock (_gate)
        {
            if (_loaded.TryGetValue(stored.Id, out var existing)) return Task.FromResult(existing);
            if (_pending.TryGetValue(stored.Id, out var inFlight)) return inFlight;

            var task = Task.Run(() => LoadCore(stored));
            _pending[stored.Id] = task;
            task.ContinueWith(t =>
            {
                lock (_gate)
                {
                    _pending.Remove(stored.Id);
                    if (t.Status == TaskStatus.RanToCompletion && t.Result != null) _loaded[stored.Id] = t.Result;
                }
                if (t.IsFaulted)
                {
                    Console.Error.WriteLine("[engine] 词典加载失败 " + stored.MdxPath + ": " + t.Exception?.GetBaseException().Message);
                }
            }, TaskScheduler.Default);
            return task;
        }
    }

    private LoadedDictionary LoadCore(StoredDictionary stored)
    {
        var mdx = new MdxFile(stored.MdxPath);
        var mdds = new List<MddFile>();
        foreach (var mddPath in stored.MddPaths ?? new List<string>())
        {
            try
            {
                if (File.Exists(mddPath)) mdds.Add(new MddFile(mddPath));
            }
            catch (Exception err)
            {
                Console.Error.WriteLine("[engine] 资源库加载失败 " + mddPath + ": " + err.Message);
            }
        }

        var header = mdx.Header;
        var fileSize = 0L;
        try { fileSize = new FileInfo(stored.MdxPath).Length; } catch { /* 文件可能已被移走 */ }

        var info = new DictionaryInfo
        {
            Id = stored.Id,
            Title = PickTitle(header, stored),
            MdxPath = stored.MdxPath,
            MddPaths = new List<string>(stored.MddPaths ?? new List<string>()),
            AddedAt = stored.AddedAt,
            FileName = Path.GetFileName(stored.MdxPath),
            FileSize = fileSize,
            EntryCount = mdx.KeyCount,
            Encoding = string.IsNullOrEmpty(header?.Encoding) ? "UTF-8" : header.Encoding,
            ParserVersion = string.IsNullOrEmpty(header?.GeneratedByEngineVersion) ? "2.0" : header.GeneratedByEngineVersion,
            Status = "ready",
            Current = false
        };

        return new LoadedDictionary { Stored = stored, Info = info, Mdx = mdx, Mdds = mdds };
    }

    internal void Unload(string id)
    {
        LoadedDictionary dict;
        lock (_gate)
        {
            if (!_loaded.TryGetValue(id, out dict)) return;
            _loaded.Remove(id);
            _pending.Remove(id);
            // 头里的书名也跟着忘掉：这本可能是"移走又换了一份文件"回来的，
            // 下次问的时候重新读一次头（读一次很便宜，见 HeaderTitleOf）
            _headerTitles.Remove(id);
        }
        try { dict.Mdx?.Dispose(); } catch { /* 忽略关闭异常 */ }
        foreach (var mdd in dict.Mdds) { try { mdd.Dispose(); } catch { /* 忽略 */ } }
    }

    /* ------------------------------ 运行期信息 ------------------------------ */

    internal DictionaryInfo DescribeRuntime(StoredDictionary stored)
    {
        LoadedDictionary dict;
        lock (_gate) _loaded.TryGetValue(stored.Id, out dict);
        if (dict != null)
        {
            var info = dict.Info;
            return new DictionaryInfo
            {
                Id = info.Id, Title = info.Title, MdxPath = info.MdxPath, MddPaths = info.MddPaths,
                AddedAt = info.AddedAt, FileName = info.FileName, FileSize = info.FileSize,
                EntryCount = info.EntryCount, Encoding = info.Encoding, ParserVersion = info.ParserVersion,
                Status = info.Status, Current = false, ErrorMessage = info.ErrorMessage
            };
        }

        return new DictionaryInfo
        {
            Id = stored.Id,
            /*
             * ⚠️ 这里原来是 `stored.Title`（= 文件名去掉后缀），是"同一本词典两个名字"的根源：
             * 已加载时报头里的书名、没加载时报文件名。**界面上一律用同一个来源** ——
             * 头里的书名读一次缓存起来，与已加载那条路走同一个 `ComposeTitle`。
             * 用户 2026-09 报的"借查按钮上的词典名不是词库列表里那个名字"就是它。
             */
            Title = TitleOf(stored),
            MdxPath = stored.MdxPath,
            MddPaths = new List<string>(stored.MddPaths ?? new List<string>()),
            AddedAt = stored.AddedAt,
            FileName = Path.GetFileName(stored.MdxPath),
            FileSize = 0,
            EntryCount = 0,
            Encoding = string.Empty,
            ParserVersion = string.Empty,
            Status = IsPending(stored.Id) ? "loading" : "pending",
            Current = false
        };
    }

    private static string PickTitle(MdxHeader header, StoredDictionary stored)
    {
        return ComposeTitle(stored.CustomTitle, header?.Title, stored);
    }

    /// <summary>
    /// 标题的**唯一**合成约定：用户改的名字 → `.mdx` 头里的书名 → 文件名。
    ///
    /// 抽出来是因为"头里的书名"有两个到手方式：词典**已加载**时手上有 <see cref="MdxHeader"/>，
    /// **没加载**时得现读一次（见 <see cref="HeaderTitleOf"/>）。两条路必须走同一个约定 ——
    /// 否则同一本词典会在"加载/未加载"两种状态下报出两个名字（见 <see cref="TitleOf"/>）。
    /// </summary>
    private static string ComposeTitle(string customTitle, string headerTitle, StoredDictionary stored)
    {
        // 用户改过名字就一律听用户的（清空则退回自动标题）
        if (!string.IsNullOrWhiteSpace(customTitle)) return customTitle.Trim();

        if (!string.IsNullOrWhiteSpace(headerTitle))
        {
            var trimmed = HtmlUtils.StripHtml(headerTitle).Trim();
            if (trimmed.Length > 0 && trimmed != "Title (No HTML Code allowed)" &&
                trimmed != "Title (No HTML code allowed)")
            {
                return trimmed;
            }
        }
        return stored.Title;
    }

    /// <summary>
    /// `.mdx` 头里的书名（原始值，null = 那个头里没有书名），**问一次就缓存**。
    ///
    /// 为什么要读它：**标题不许在"加载/未加载"两种状态下取不同的来源**。
    /// 未加载时原来退回 `stored.Title`（落库时记的文件名去掉后缀），于是同一本词典出现两个名字 ——
    /// 用户 2026-09 报的就是这个：「当前词典没找到，出现借词典查的那个按钮，
    /// 按钮上显示的词典名没有用词库列表显示的名字」。借查那条路**故意不加载**词典
    /// （问一句就走，见 `DictionaryProbe`），所以它读的正是那个假的来源。
    ///
    /// 为什么要缓存：`DescribeRuntime` 每次 `dict:list` 广播都会**逐本**调一遍，
    /// 每本都去开一次文件读头，在"词库里几十本"时是白费的开销。
    /// 而读一次头的代价实测很小（冷 41 ms / 热 &lt;1 ms，见 `DictionaryProbe` 的说明），
    /// 所以第一问读一次、之后留着 —— 与 `MdictCore` 的索引缓存同一个思路（**每本只付一次**）。
    /// </summary>
    private string HeaderTitleOf(StoredDictionary stored)
    {
        lock (_gate)
        {
            if (_headerTitles.TryGetValue(stored.Id, out var cached)) return cached;
        }

        string raw = null;
        try
        {
            using (var mdx = new MdxFile(stored.MdxPath)) raw = mdx.Header?.Title;
        }
        catch (Exception err)
        {
            // 读不到就当"头里没有书名"，退回文件名 —— 与"文件打不开"那条路一致，不抛给界面
            Console.Error.WriteLine("[engine] 读词典头失败 " + stored.MdxPath + ": " + err.Message);
        }

        var title = string.IsNullOrWhiteSpace(raw) ? null : raw;
        lock (_gate) _headerTitles[stored.Id] = title;
        return title;
    }

    /// <summary>
    /// 运行期标题 —— 与 <see cref="PickTitle"/> 同一个约定，但**不要求词典已经加载**。
    ///
    /// 界面上一律用它：词库列表、标题栏那个小胶囊、"用《…》查"那个按钮、未命中的提示页。
    /// 谁都不许再退回 `stored.Title`（那是**文件名**，只在头里真没有书名时才是对的）。
    /// </summary>
    internal string TitleOf(StoredDictionary stored)
    {
        if (stored == null) return string.Empty;
        // 用户改的名字是设置里的实时值，优先判它 —— 免得缓存把改名挡住
        if (!string.IsNullOrWhiteSpace(stored.CustomTitle)) return stored.CustomTitle.Trim();
        return ComposeTitle(stored.CustomTitle, HeaderTitleOf(stored), stored);
    }

    /// <summary>
    /// 词典改名之后刷新运行期标题。
    ///
    /// 光改 settings.json 是不够的：已经加载的词典把标题缓存在 Info 里，
    /// 查词结果（DictTitle）、悬浮窗的提示文字都读的是它。
    /// </summary>
    internal void Retitle(StoredDictionary stored)
    {
        if (stored == null) return;
        lock (_gate)
        {
            if (!_loaded.TryGetValue(stored.Id, out var dict)) return;
            dict.Info.Title = PickTitle(dict.Mdx?.Header, stored);
        }
    }

    /* ------------------------------ 检索 ------------------------------ */

    /// <summary>
    /// 输入联想。
    ///
    /// 策略是拿牛津高阶第 9 版（20 万词条）实测出来的：
    /// - `prefix` 可靠，用于前缀补全；
    /// - 编辑距离（短词 1、长词 2）用于兜拼写错误；
    /// - 不做"关联词"，实测会返回落点所在键块里的一堆无关词。
    /// </summary>
    internal List<SuggestionItem> Suggest(StoredDictionary stored, string query, int limit)
    {
        var result = new List<SuggestionItem>();
        LoadedDictionary dict;
        lock (_gate) _loaded.TryGetValue(stored.Id, out dict);

        var text = (query ?? string.Empty).Trim();
        if (dict == null || text.Length == 0) return result;

        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        void Push(string word, string kind)
        {
            if (result.Count >= limit || string.IsNullOrEmpty(word)) return;
            if (!seen.Add(word)) return;
            result.Add(new SuggestionItem { Word = word, Kind = kind });
        }

        // 1. 精确命中（含大小写变体：词典 lookup 是大小写敏感的，Apple 查不到 apple）
        foreach (var variant in CaseVariants(text))
        {
            try
            {
                var exact = dict.Mdx.Lookup(variant);
                if (exact != null && exact.Definition != null)
                {
                    Push(exact.KeyText, "exact");
                    break;
                }
            }
            catch { /* 换下一个变体 */ }
        }

        // 2. 前缀补全
        try
        {
            foreach (var item in dict.Mdx.Prefix(text))
            {
                if (result.Count >= limit) break;
                Push(item.KeyText, "prefix");
            }
        }
        catch (Exception err) { Console.Error.WriteLine("[engine] 前缀查询异常: " + err.Message); }

        /*
         * 3. 拼写纠正：短词用编辑距离 1，长词放宽到 2。
         *
         * ⚠️ **单个字符的查询一律不做这一步** —— 用户 2026-09 报的 bug 就出在这里。
         *
         * 为什么：编辑距离 ≤1 对**单字符**查询是个**恒真**条件 —— 任何单个字符改一处
         * 就等于另一个单字符；而候选只在"查询词所在的那个词块"里找（`MdxReader.Fuzzy`
         * 只扫一个块，这是为了对齐参考实现），于是"拼写纠正"退化成
         * **"把这个词块里所有单字符词条按块内顺序列出来"**。
         * 实测（`node tools/SuggestProbe/run.mjs <词典> --diag 囧`）：
         *   · 《牛津高阶英汉双解词典(第9版)》：`囧` → `Z / ® / α / β / γ / δ / ε / ζ ……`
         *     （那个词块正好是词表末尾的附录：`Z` 在第 56 条、`®` 第 399、`α` 第 417……）；
         *   · 《新世纪汉英大词典》：同一个 `囧` → `囤 / 囥 / 囫 / 园 / 囮 / 困 / 囱 ……`
         *     （都是口字旁的单字，看着"像那么回事"，其实同样是垃圾 —— 更误导）。
         * 而且单个字符本来**没有什么"拼写"可纠正**：输入一半时该走的是**前缀补全**（第 2 步）。
         *
         * ⚠️ 改的是这一层（**产品策略**），不是 `MdxReader.Fuzzy` —— 那个原语要与
         * js-mdict 的 `fuzzy_search` 逐项对齐（`tools/MdxProbe/compare.mjs` 在盯着），
         * 参考实现本身也有这个恒真问题，照抄它是对的；"要不要用它"是我们自己的事。
         */
        if (result.Count < limit && text.Length > 1)
        {
            var gap = text.Length <= 4 ? 1 : 2;
            try
            {
                foreach (var item in dict.Mdx.Fuzzy(text, limit, gap))
                {
                    if (result.Count >= limit) break;
                    Push(item.KeyText, "fuzzy");
                }
            }
            catch (Exception err) { Console.Error.WriteLine("[engine] 模糊查询异常: " + err.Message); }
        }

        return result;
    }

    /// <summary>精确查词；未命中时给出提示信息</summary>
    internal EntryPayload Lookup(StoredDictionary stored, string word)
    {
        LoadedDictionary dict;
        lock (_gate) _loaded.TryGetValue(stored.Id, out dict);
        var query = (word ?? string.Empty).Trim();

        if (dict == null) return Payload(stored, null, query, "词库尚未加载完成，请稍候重试。");
        if (query.Length == 0) return Payload(stored, dict, query, "请输入要查询的词。");

        ResolvedEntry resolved;
        try
        {
            resolved = Resolve(dict, query);
        }
        catch (Exception err)
        {
            return Payload(stored, dict, query, "查询出错：" + err.Message);
        }

        if (!resolved.Found || resolved.Definition == null)
        {
            return Payload(stored, dict, query, NotFoundText(dict, query, resolved.LinkedTo));
        }

        return new EntryPayload
        {
            Query = query,
            KeyText = resolved.KeyText,
            DictId = stored.Id,
            DictTitle = dict.Info.Title,
            EntryUrl = EntryDocument.EntryUrlFor(stored.Id, resolved.KeyText),
            PlainText = HtmlUtils.StripHtml(resolved.Definition),
            Found = true,
            LinkedTo = resolved.LinkedTo
        };
    }

    /// <summary>
    /// 把这个词解析成「**词典里最终会落到的那条词条**」；查不到就回 null。
    ///
    /// 检查标准与 <see cref="Lookup"/> 走的是同一条 <see cref="Resolve"/>
    /// （大小写变体逐个试 + 逐层跟随 `@@@LINK=` 重定向），所以
    /// `ResolveKey(w)` 与 `Lookup(w).KeyText` 对同一个词**必然是同一个答案**。
    /// 这条不变式正是它存在的理由，见下面第 ③ 条。
    ///
    /// 为什么不让界面自己比字符串、或者干脆拿 `Lookup` 探路：
    ///   ① `App.cs` 的 `dict:lookup` 一旦命中就往查词历史里写一条 —— 探路会把历史刷满；
    ///   ② `Lookup` 为了渲染正文会把整条释义 `StripHtml` 成纯文本跨桥回传，只为问一个落点；
    ///   ③ **界面自己比字符串一定有一类输入是错的**：词典会把名词复数、动词过去式
    ///      这类变形形式**重定向到原型词条**（`apples` → `@@@LINK=apple`），
    ///      于是"选中的是不是当前词条"根本不是字符串问题，而是**解析落点**问题 ——
    ///      拿 `word != entry.keyText` 去判，`apples` 会被当成"另一个词"，
    ///      点下去却是把同一篇正文重载一遍（回到顶部、还多压一层假的返回栈）。
    ///      用户 2026-09 报的就是这一条。所以判定必须问词典本身，而且要**问落点**、
    ///      不能只问"在不在" —— 只问在不在，`apples` 同样会答"在"。
    ///
    /// 全程只做字典查表：不碰历史、不动缓存、不产 HTML。
    /// </summary>
    internal string ResolveKey(StoredDictionary stored, string word)
    {
        LoadedDictionary dict;
        lock (_gate) _loaded.TryGetValue(stored.Id, out dict);

        var query = (word ?? string.Empty).Trim();
        if (dict == null || query.Length == 0) return null;

        try
        {
            var resolved = Resolve(dict, query);
            return resolved.Found && resolved.Definition != null ? resolved.KeyText : null;
        }
        catch (Exception err)
        {
            Console.Error.WriteLine("[engine] 解析落点异常: " + err.Message);
            return null;
        }
    }

    /// <summary>生成词条正文 HTML 文档，供资源协议返回给 iframe</summary>
    internal string RenderEntry(StoredDictionary stored, string word)
    {
        LoadedDictionary dict;
        lock (_gate) _loaded.TryGetValue(stored.Id, out dict);

        if (dict == null)
        {
            return EntryDocument.Build(new EntryDocumentOptions
            {
                DictId = stored.Id,
                Definition = string.Empty,
                Notice = "词库尚未加载完成，请稍候重试。",
                HasResources = false
            });
        }

        ResolvedEntry resolved;
        try
        {
            resolved = Resolve(dict, (word ?? string.Empty).Trim());
        }
        catch (Exception err)
        {
            return EntryDocument.Build(new EntryDocumentOptions
            {
                DictId = stored.Id,
                Definition = string.Empty,
                Notice = "查询出错：" + HtmlUtils.EscapeHtml(err.Message),
                HasResources = HasResources(stored.Id)
            });
        }

        if (!resolved.Found || resolved.Definition == null)
        {
            return EntryDocument.Build(new EntryDocumentOptions
            {
                DictId = stored.Id,
                Definition = string.Empty,
                Notice = NotFoundHtml(dict, (word ?? string.Empty).Trim(), resolved.LinkedTo),
                HasResources = HasResources(stored.Id)
            });
        }

        return EntryDocument.Build(new EntryDocumentOptions
        {
            DictId = stored.Id,
            Definition = resolved.Definition,
            HasResources = HasResources(stored.Id)
        });
    }

    /// <summary>
    /// 跟随 @@@LINK= 解析出最终词条。
    /// 注意这里不做"没命中就自动跳到最接近的词"——那会把拼错的查询悄悄换成另一个词条，
    /// 反而让人以为查到了。改为在未命中的提示里列出候选（见 NotFoundHtml）。
    ///
    /// ⚠️ **只用 `dict.Mdx`**（不碰 `Info` / `Mdds` / gate）—— 正因为如此，
    /// <see cref="DictionaryProbe"/> 才能用一个"临时词典"复用同一条解析，
    /// 于是"一次性查询说会落到 A"与"真跳过去显示 B"不可能不一致。
    /// 改动这里时请注意别引入别的依赖。
    /// </summary>
    internal ResolvedEntry Resolve(LoadedDictionary dict, string query)
    {
        var cursor = query;
        string linkedTo = null;

        for (var depth = 0; depth < MaxLinkDepth; depth++)
        {
            MdxEntry hit = null;
            foreach (var variant in CaseVariants(cursor))
            {
                try
                {
                    var attempt = dict.Mdx.Lookup(variant);
                    if (attempt != null && attempt.Definition != null)
                    {
                        hit = attempt;
                        break;
                    }
                }
                catch { /* 换下一个变体 */ }
            }

            if (hit == null || hit.Definition == null)
            {
                return new ResolvedEntry { Found = false, KeyText = query, Definition = null, LinkedTo = linkedTo };
            }

            var redirect = ParseLinkRedirect(hit.Definition);
            if (redirect != null && redirect != cursor)
            {
                linkedTo = redirect;
                cursor = redirect;
                continue;
            }
            return new ResolvedEntry { Found = true, KeyText = hit.KeyText, Definition = hit.Definition, LinkedTo = linkedTo };
        }

        return new ResolvedEntry { Found = false, KeyText = query, Definition = null, LinkedTo = linkedTo };
    }

    /// <summary>未命中时的提示：说明没找到，并把拼写候选列成可点击的 entry:// 链接</summary>
    private string NotFoundHtml(LoadedDictionary dict, string query, string linkedTo)
    {
        var parts = new List<string>();
        if (!string.IsNullOrEmpty(linkedTo))
        {
            parts.Add("未找到词条“" + HtmlUtils.EscapeHtml(query) + "”。词典内部跳转目标 <code>" +
                      HtmlUtils.EscapeHtml(linkedTo) + "</code> 不存在。");
        }
        else
        {
            parts.Add("未在《" + HtmlUtils.EscapeHtml(dict.Info.Title) + "》中找到“" + HtmlUtils.EscapeHtml(query) + "”。");
        }

        var candidates = Suggest(dict.Stored, query, 6)
            .Where(item => !string.Equals(item.Word, query, StringComparison.OrdinalIgnoreCase))
            .ToList();

        if (candidates.Count > 0)
        {
            var links = string.Join(string.Empty, candidates.Select(item =>
                "<a href=\"entry://" + Uri.EscapeDataString(item.Word) + "\">" + HtmlUtils.EscapeHtml(item.Word) + "</a>"));
            parts.Add("<div class=\"lookup-suggests\">你是不是想找：" + links + "</div>");
        }
        return string.Concat(parts);
    }

    private string NotFoundText(LoadedDictionary dict, string query, string linkedTo)
    {
        if (!string.IsNullOrEmpty(linkedTo))
        {
            return "未找到词条“" + query + "”。词典内部跳转目标 " + linkedTo + " 不存在。";
        }
        return "未在《" + dict.Info.Title + "》中找到“" + query + "”。";
    }

    /// <summary>
    /// 未命中（或词典还没加载好 / 查询出错）时的载荷：仍然给一个 entryUrl，
    /// 让 UI 能展示"未找到 + 拼写候选"的提示页。
    ///
    /// ⚠️ **词典名只有一个来源**：能拿到已加载的词典就用它的运行期标题，
    /// 拿不到就用 <see cref="TitleOf"/> —— 它同样走"用户改的名字 → **头里的书名** → 文件名"
    /// 这条约定（头里的书名读一次就缓存），**不再退回 `stored.Title`**。
    ///
    /// 这里原来写的是 `DictTitle = stored.Title`（**落库时记的那个标题 = 文件名去掉后缀**），
    /// 于是同一页上会出现两个名字：正文里那句提示用的是 `dict.Info.Title`
    /// （"未在《新世纪汉英大词典》中找到…"），而标题栏那个小胶囊用的是文件名
    /// （"xsjhy20oct2"）—— 用户 2026-09 报的就是这个（原话：
    /// "在顶部栏的字典名那里，并没有显示选项词库管理中显示的词典名，而是文件名去掉后缀"）。
    /// 管理窗显示的是运行期标题，所以它才是对的那个。
    /// </summary>
    private EntryPayload Payload(StoredDictionary stored, LoadedDictionary dict, string query, string message)
    {
        return new EntryPayload
        {
            Query = query,
            KeyText = query,
            DictId = stored.Id,
            DictTitle = dict != null ? dict.Info.Title : TitleOf(stored),
            EntryUrl = EntryDocument.EntryUrlFor(stored.Id, query),
            PlainText = string.Empty,
            Found = false,
            Message = message
        };
    }

    /* ------------------------------ 资源 ------------------------------ */

    /// <summary>
    /// 读取词条要用的资源。传入的是 URL 形式的相对路径，例如 `images/logo.png`。
    /// 返回 null 表示没找到（调用方回 404）。
    ///
    /// **两级查找**：
    ///   ① 全部 `.mdd` 卷（官方打包版）—— 命中就返回，优先；
    ///   ② `.mdx` **同目录散放的文件**（见 <see cref="SiblingResource"/>）—— 兜底。
    ///
    /// ②是这一版新加的：《新世纪汉英大词典》这类词典把 css / ttf / png 与 `.mdx` 散放在一起、
    /// **完全没有 `.mdd`**，而原来那句 `dict.Mdds.Count == 0` 的短路让它们连门都进不来
    /// —— 表现是"排版全无、只剩裸文字"。同一本词典同时有 `.mdd` 与同目录散放的文件时，
    /// `.mdd` 算"官方打包版"，所以它优先。
    /// </summary>
    internal byte[] Resource(string dictId, string resourcePath)
    {
        LoadedDictionary dict;
        lock (_gate) _loaded.TryGetValue(dictId, out dict);
        if (dict == null) return null;

        if (dict.Mdds.Count > 0)
        {
            foreach (var key in ResourceKeyCandidates(resourcePath))
            {
                foreach (var mdd in dict.Mdds)
                {
                    try
                    {
                        var hit = mdd.Locate(key);
                        if (hit != null && hit.Length > 0) return hit;
                    }
                    catch { /* 换下一个候选键名 */ }
                }
            }
        }

        return SiblingResourceBytes(dict, resourcePath);
    }

    /// <summary>从 `.mdx` 同目录读一个散放的文件（`SiblingResource` 负责路径与白名单判定）</summary>
    private byte[] SiblingResourceBytes(LoadedDictionary dict, string resourcePath)
    {
        var path = SiblingResource.Locate(dict.Stored == null ? null : dict.Stored.MdxPath, resourcePath);
        if (path == null) return null;

        try { return File.ReadAllBytes(path); }
        catch (Exception err)
        {
            Console.Error.WriteLine("[engine] 读取同目录散放的文件失败 " + path + ": " + err.Message);
            return null;
        }
    }

    /// <summary>
    /// 资源的**协商缓存身份证**（HTTP `ETag` 的值，带引号）；定位不到就回 null。
    ///
    /// ## 为什么必须有它
    ///
    /// 《新世纪汉英大词典》补齐思源字体之后，它同目录散放的文件里有 **13.8 MB 的 ttf**。
    /// 而词条 iframe **每查一次词就换一次 `src`**（带 `&t=` 防缓存）→ 新文档 = 重新请求全部资源，
    /// 而原来响应头写死 `Cache-Control: no-cache` 且**没有 ETag** → 浏览器的"重新验证"
    /// 必然退化成**整块重传**（还要先整块读进内存）。
    /// `.mdd` 里的资源通常只有几 KB~几百 KB，所以这条链路一直没暴露问题 ——
    /// **是不带 .mdd 的词典第一次把十几 MB 的资源带了进来**。
    ///
    /// ## 关键：这个方法**不读内容**
    ///
    /// `If-None-Match` 命中时直接回 304，连磁盘都不用读 —— 这才是收益所在。
    /// 所以身份证只能由"便宜的元数据"算出来：
    ///   · `.mdd` 命中：键名 + **资源卷文件**的长度与最后写入时间（`Contains` 只查键索引，**不解记录块**）；
    ///   · 同目录散放的文件：文件名 + 该文件的长度与最后写入时间（一次 `stat`）。
    ///
    /// ⚠️ **刻意不用长 `max-age`**：`dictId` 是"词典文件的**完整路径**"的哈希，
    /// 用户换掉同名的词典文件时它**不变**，长缓存会一直拿到旧资源。
    /// 协商缓存（每次都问一句"变了没"）才是对的。
    /// </summary>
    internal string ResourceETag(string dictId, string resourcePath)
    {
        if (string.IsNullOrEmpty(resourcePath)) return null;

        LoadedDictionary dict;
        lock (_gate) _loaded.TryGetValue(dictId, out dict);
        if (dict == null) return null;

        if (dict.Mdds.Count > 0)
        {
            foreach (var key in ResourceKeyCandidates(resourcePath))
            {
                foreach (var mdd in dict.Mdds)
                {
                    try
                    {
                        if (mdd.Contains(key)) return Tag("mdd", key, mdd.Path);
                    }
                    catch { /* 换下一个候选键名 */ }
                }
            }
        }

        var sibling = SiblingResource.Locate(dict.Stored == null ? null : dict.Stored.MdxPath, resourcePath);
        return sibling == null ? null : Tag("sibling", Path.GetFileName(sibling), sibling);
    }

    /// <summary>`"<来源>:<名字>:<长度>-<最后写入时间>"`；元数据读不到就回 null（宁可不缓存，也别给错身份证）</summary>
    private static string Tag(string source, string name, string filePath)
    {
        try
        {
            var info = new FileInfo(filePath);
            if (!info.Exists) return null;
            return "\"" + source + ":" + name + ":" +
                   info.Length.ToString(CultureInfo.InvariantCulture) + "-" +
                   info.LastWriteTimeUtc.Ticks.ToString(CultureInfo.InvariantCulture) + "\"";
        }
        catch
        {
            return null;
        }
    }

    /// <summary>
    /// 这本词典有没有可用资源（`.mdd` 卷，或者 `.mdx` 同目录散放的文件）。
    ///
    /// ⚠️ 它和 <see cref="Resource"/> 必须**同时改**：不带 .mdd 的词典原来在这里被判成"没有资源"，
    /// 而它被填进词条正文的 `data-has-resources` —— 两条判定互相矛盾的话，
    /// 会出现"资源明明取到了、文档却说这本词典没有资源"这种自相矛盾的状态。
    /// </summary>
    internal bool HasResources(string dictId)
    {
        LoadedDictionary dict;
        lock (_gate) _loaded.TryGetValue(dictId, out dict);
        if (dict == null) return false;
        if (dict.Mdds.Count > 0) return true;
        return SiblingResource.HasAny(dict.Stored == null ? null : dict.Stored.MdxPath);
    }

    /// <summary>
    /// 在一本词典的全部资源卷里找**第一个存在**的候选键名（不读记录块）。
    ///
    /// 候选顺序由调用方给：发音那边要按"能播的扩展名优先、.spx 垫底"排，
    /// 而顺序属于格式知识、不属于资源查找，所以这里只认顺序、不认扩展名。
    /// 返回实际命中的那个键名（带前导 `\`，与 mdd 里写的一致），都没命中返回 null。
    /// </summary>
    internal string FindResourceKey(StoredDictionary stored, IEnumerable<string> candidateKeys)
    {
        if (stored == null || candidateKeys == null) return null;

        LoadedDictionary dict;
        lock (_gate) _loaded.TryGetValue(stored.Id, out dict);
        if (dict == null || dict.Mdds.Count == 0) return null;

        foreach (var raw in candidateKeys)
        {
            foreach (var key in ResourceKeyCandidates(raw))
            {
                foreach (var mdd in dict.Mdds)
                {
                    try
                    {
                        if (mdd.Contains(key)) return key;
                    }
                    catch { /* 换下一个候选 */ }
                }
            }
        }
        return null;
    }

    /// <summary>按已经在库里的键名取原始字节（发音路由用；键名来自 <see cref="FindResourceKey"/>）</summary>
    internal byte[] ReadResource(StoredDictionary stored, string key)
    {
        if (stored == null || string.IsNullOrEmpty(key)) return null;
        return Resource(stored.Id, key);
    }

    /// <summary>取当前词条的 HTML（发音规划要从里面抠 sound:// 链接）</summary>
    internal string EntryHtml(StoredDictionary stored, string word)
    {
        LoadedDictionary dict;
        lock (_gate) _loaded.TryGetValue(stored.Id, out dict);
        if (dict == null) return null;
        try
        {
            var resolved = Resolve(dict, (word ?? string.Empty).Trim());
            return resolved.Found ? resolved.Definition : null;
        }
        catch (Exception err)
        {
            Console.Error.WriteLine("[engine] 取词条正文失败: " + err.Message);
            return null;
        }
    }

    /// <summary>
    /// 在整本词典的索引上**均匀**取样若干词条名（只读键块，不解正文、不碰记录区）。
    ///
    /// 取样规则与"哪些名字值得拿去量"的检查标准在 <see cref="IndexSampling"/> 与
    /// <see cref="DictSampler"/> 里 —— 那两个文件不依赖 Models，所以有拿**真词典文件**跑的
    /// B 级诊断脚本（tools/DictSampleProbe）。这里只负责"找到已加载的那本词典，把活交出去"。
    ///
    /// 返回的是**候选**：这里只保证"名字看着像词条"，它到底有没有录音由调用方
    /// 挨个去解（见 App.DictSamplesAsync）。所以数量上限刻意等于 maxScan，
    /// 而不是"调用方想要几条样本" —— 混为一谈的话，前几条恰好没录音就没得可挑了。
    /// 真正的**时间预算**在调用方那边按 Stopwatch 掐（引擎不知道调用方还能花多久）。
    ///
    /// 谁在用：今天**没有产品流程**（「平衡音量」已经不再量内置录音，
    /// 约定与原因见 App.DictSamplesAsync 的注释）；接口与测试都留着，将来直接可用。
    /// </summary>
    internal List<string> SampleKeys(StoredDictionary stored, int maxScan)
    {
        var dict = LoadedDict(stored);
        return dict == null ? new List<string>() : DictSampler.SampleKeys(dict.Mdx, maxScan);
    }

    /// <summary>
    /// 按索引顺序惰性列出"像词条"的名字（取样那批凑不满候选数时的兜底补扫用）。
    /// 惰性且可中途 break，见 <see cref="DictSampler.EnumerateKeys"/>。
    ///
    /// 谁在用：同 <see cref="SampleKeys"/> —— 今天没有产品流程调它。
    /// </summary>
    internal IEnumerable<string> EnumerateKeys(StoredDictionary stored)
    {
        var dict = LoadedDict(stored);
        return dict == null ? Enumerable.Empty<string>() : DictSampler.EnumerateKeys(dict.Mdx);
    }

    /// <summary>取已加载的那本词典；还没加载就返回 null（调用方自己决定是等还是报错）</summary>
    private LoadedDictionary LoadedDict(StoredDictionary stored)
    {
        if (stored == null) return null;
        lock (_gate) return _loaded.TryGetValue(stored.Id, out var dict) ? dict : null;
    }

    internal void DisposeAll()
    {
        List<LoadedDictionary> all;
        lock (_gate)
        {
            all = _loaded.Values.ToList();
            _loaded.Clear();
            _pending.Clear();
        }
        foreach (var dict in all)
        {
            try { dict.Mdx?.Dispose(); } catch { /* 忽略 */ }
            foreach (var mdd in dict.Mdds) { try { mdd.Dispose(); } catch { /* 忽略 */ } }
        }
    }

    /* ------------------------------ 内部工具 ------------------------------ */

    /// <summary>
    /// lookup 的大小写变体。
    /// MDict 的 lookup 是大小写敏感的（即使词典头部写着 KeyCaseSensitive=No），
    /// 实测牛津高阶里 Apple 查不到 apple，所以按常见写法逐个试。
    /// </summary>
    internal static IEnumerable<string> CaseVariants(string word)
    {
        var text = (word ?? string.Empty).Trim();
        if (text.Length == 0) yield break;

        var variants = new List<string> { text };
        var lower = text.ToLowerInvariant();
        var upper = text.ToUpperInvariant();
        var capitalized = char.ToUpperInvariant(lower[0]) + lower.Substring(1);
        foreach (var variant in new[] { lower, capitalized, upper })
        {
            if (!variants.Contains(variant, StringComparer.Ordinal)) variants.Add(variant);
        }
        foreach (var variant in variants) yield return variant;
    }

    /// <summary>命中的定义若形如 `@@@LINK=目标词`，返回目标词</summary>
    internal static string ParseLinkRedirect(string definition)
    {
        if (string.IsNullOrEmpty(definition)) return null;
        var trimmed = definition.TrimStart();
        const string prefix = "@@@LINK=";
        if (!trimmed.StartsWith(prefix, StringComparison.Ordinal)) return null;
        var rest = trimmed.Substring(prefix.Length);
        var lineEnd = rest.IndexOfAny(new[] { '\r', '\n' });
        /*
         * 前后三下 Trim，顺序不能省：
         *   ① `.Trim()`     去常规空白；
         *   ② `.Trim('\0')` **去结尾的 NUL** —— 这条以前漏了，是个真 bug：
         *      MDX 的记录区里每条正文后面常缀一个 `\0`（本仓库自己的测试用词典生成脚本就是这么写的，
         *      注释里写着"与 test.mdx 一致"），而 `string.Trim()` 只去空白，
         *      **`char.IsWhiteSpace('\0')` 是 false** —— 于是目标词会变成 `"run\0"`，
         *      再拿它去 `Lookup` 就查不到，**整个重定向不报错地失效**（面板上表现为"这个词查不到"）。
         *      长期没被发现，是因为此前 5 份测试用词典里**一条 LINK 词条都没有**
         *      （`@@@LINK` 出现 0 次），这条路径从来没被跑到过。
         *      回归素材：testdata/link.mdx 的 `ran` 那条（照默认写法生成，正文带 NUL）。
         *   ③ 再来一遍 `.Trim()`：`"apple \0"` 先被 ① 卡住（尾字符是 NUL 不是空白），
         *      经 ② 变成 `"apple "`，得再修一次才干净。
         */
        var target = (lineEnd >= 0 ? rest.Substring(0, lineEnd) : rest).Trim().Trim('\0').Trim();
        return target.Length > 0 ? target : null;
    }

    private static IEnumerable<string> ResourceKeyCandidates(string resourcePath)
    {
        var decoded = resourcePath ?? string.Empty;
        try { decoded = Uri.UnescapeDataString(decoded); } catch { /* 保留原样 */ }

        var clean = decoded.TrimStart('\\', '/');
        var backslash = clean.Replace('/', '\\');
        var forward = clean.Replace('\\', '/');

        var candidates = new List<string>
        {
            "\\" + backslash,
            "\\" + forward,
            backslash,
            forward,
            "/\\" + backslash,
            decoded
        };

        var seen = new HashSet<string>(StringComparer.Ordinal);
        foreach (var candidate in candidates)
        {
            if (!string.IsNullOrEmpty(candidate) && seen.Add(candidate)) yield return candidate;
        }
    }
}
