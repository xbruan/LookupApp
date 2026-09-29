using System;
using System.Collections.Generic;

namespace Lookup.Dictionary;

/// <summary>MDict 头部（&lt;Dictionary ... /&gt; 或 &lt;Library_Data ... /&gt; 里的属性）</summary>
public sealed class MdxHeader
{
    internal MdxHeader(string title, string description, string encoding, string decodingEncoding,
        string generatedByEngineVersion, string requiredEngineVersion, string format,
        string keyCaseSensitive, int encrypted, string rawXml)
    {
        Title = title;
        Description = description;
        Encoding = encoding;
        DecodingEncoding = decodingEncoding;
        GeneratedByEngineVersion = generatedByEngineVersion;
        RequiredEngineVersion = requiredEngineVersion;
        Format = format;
        KeyCaseSensitive = keyCaseSensitive;
        Encrypted = encrypted;
        RawXml = rawXml;
    }

    public string Title { get; }

    public string Description { get; }

    /// <summary>
    /// 头部写的字符集（UTF-8 / GBK / BIG5 / UTF-16 ...）；头部没写时为空串的 .mdd 会返回 UTF-16。
    /// 注意这是“写的值”，实际用来解码的名字见 <see cref="DecodingEncoding"/>。
    /// </summary>
    public string Encoding { get; }

    /// <summary>实际用于解码词条名与正文的字符集：GBK/GB2312 归一为 GB18030，.mdd 固定 UTF-16</summary>
    public string DecodingEncoding { get; }

    /// <summary>生成引擎版本，如 "2.0" / "1.2"；缺失时为空串</summary>
    public string GeneratedByEngineVersion { get; }

    public string RequiredEngineVersion { get; }

    public string Format { get; }

    /// <summary>头部 KeyCaseSensitive 属性值，缺失时为 "No"（与参考实现的默认值一致）</summary>
    public string KeyCaseSensitive { get; }

    /// <summary>0 = 不加密；1 = 记录块加密；2 = 键信息块加密</summary>
    public int Encrypted { get; }

    /// <summary>头部原始 XML 文本</summary>
    public string RawXml { get; }
}

/// <summary>一次命中：词典里的真实词条名 + 正文</summary>
public sealed class MdxEntry
{
    public MdxEntry(string keyText, string definition)
    {
        KeyText = keyText;
        Definition = definition;
    }

    /// <summary>词典里命中的真实词条名（不是查询词）</summary>
    public string KeyText { get; }

    /// <summary>词条正文（已按词典字符集解码；@@@LINK= 重定向也原样返回）</summary>
    public string Definition { get; }
}

/// <summary>
/// .mdx 词典。构造时只解析头部与两个索引区（不读词条正文），
/// 查询时才按需解压命中的词块/记录块，并用 LRU 缓存住最近用过的块。
/// </summary>
public sealed class MdxFile : IDisposable
{
    private readonly MdictCore _core;

    public MdxFile(string path)
    {
        _core = new MdictCore(path);
        Header = new MdxHeader(
            title: _core.Attributes.TryGetValue("Title", out var title) ? title : string.Empty,
            description: _core.Attributes.TryGetValue("Description", out var description) ? description : string.Empty,
            // 头部没写 Encoding 时（.mdd、老词典）返回实际生效的字符集，避免调用方拿到空串
            encoding: FirstNonEmpty(GetAttribute("Encoding"), _core.EffectiveEncoding),
            decodingEncoding: _core.EffectiveEncoding,
            generatedByEngineVersion: FirstNonEmpty(GetAttribute("GeneratedByEngineVersion"), string.Empty),
            requiredEngineVersion: FirstNonEmpty(GetAttribute("RequiredEngineVersion"), string.Empty),
            format: FirstNonEmpty(GetAttribute("Format"), string.Empty),
            keyCaseSensitive: FirstNonEmpty(GetAttribute("KeyCaseSensitive"), "No"),
            encrypted: _core.Encrypted,
            rawXml: _core.RawXml);
        Path = path;
        KeyCount = _core.KeyCount;
    }

    /// <summary>词典文件路径</summary>
    public string Path { get; }

    public MdxHeader Header { get; }

    /// <summary>词条总数（取自键区头部，不依赖是否读过全部词条）</summary>
    public long KeyCount { get; }

    /// <summary>
    /// 精确命中，未命中返回 null。
    /// 大小写敏感——与 js-mdict 一致，调用方需要自己做大小写变体（例如 Apple/APPLE）。
    /// </summary>
    public MdxEntry Lookup(string word)
    {
        if (string.IsNullOrEmpty(word)) return null;
        if (!_core.TryLookupKey(word, out var hit)) return null;
        return new MdxEntry(hit.Text, _core.GetDefinition(hit.BlockIndex, hit.EntryIndex));
    }

    /// <summary>
    /// 前缀命中（词条名以 word 开头，序数序比较，大小写敏感）。
    /// 从命中的词块开始向后扫，跨块的前缀会一直扫到越过该前缀为止。
    /// </summary>
    public IEnumerable<MdxEntry> Prefix(string word)
    {
        foreach (var hit in _core.PrefixHits(word))
        {
            yield return new MdxEntry(hit.Text, _core.GetDefinition(hit.BlockIndex, hit.EntryIndex));
        }
    }

    /// <summary>
    /// 编辑距离不超过 maxDistance 的候选（最多 limit 个，按距离升序）。
    /// 候选范围与 js-mdict 的 fuzzy_search 一致：只在“查询词所在的词块”里找。
    /// </summary>
    public IEnumerable<MdxEntry> Fuzzy(string word, int limit, int maxDistance)
    {
        if (string.IsNullOrEmpty(word) || limit <= 0) yield break;
        // 参考实现只接受 0..5 的距离，超出直接返回空
        if (maxDistance < 0 || maxDistance > 5) yield break;

        var blockIndex = _core.AssociateBlockIndex(word);
        if (blockIndex < 0) yield break;

        var entries = _core.GetKeyBlock(blockIndex);
        var target = _core.StripKey(word);
        var candidates = new List<KeyValuePair<int, int>>();

        for (var i = 0; i < entries.Length; i++)
        {
            var distance = MdictCore.LevenshteinDistance(_core.StripKey(entries[i].Text), target);
            if (distance <= maxDistance) candidates.Add(new KeyValuePair<int, int>(distance, i));
        }

        // 同距离时保持块内原顺序，等价于 JS 里 Array.prototype.sort 的稳定性
        candidates.Sort((x, y) => x.Key != y.Key ? x.Key - y.Key : x.Value - y.Value);

        var take = Math.Min(limit, candidates.Count);
        for (var i = 0; i < take; i++)
        {
            var entryIndex = candidates[i].Value;
            yield return new MdxEntry(entries[entryIndex].Text, _core.GetDefinition(blockIndex, entryIndex));
        }
    }

    /// <summary>解析过程中的告警（索引不自洽、adler32 不符等），正常情况下为空</summary>
    public IReadOnlyList<string> Warnings => _core.Warnings;

    internal MdictCore Core => _core;

    private string GetAttribute(string name)
    {
        return _core.Attributes.TryGetValue(name, out var value) ? value : null;
    }

    private static string FirstNonEmpty(string value, string fallback)
    {
        return string.IsNullOrEmpty(value) ? fallback : value;
    }

    public void Dispose()
    {
        _core.Dispose();
    }
}
