using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Text;
using System.Text.RegularExpressions;

namespace Lookup.Dictionary;

/// <summary>块内一个词条：词条名 + 该词条正文在“解压后的记录空间”里的起始偏移</summary>
internal struct KeyEntry
{
    internal string Text;
    internal long RecordStart;
}

/// <summary>key block info 里描述的一个词块</summary>
internal sealed class KeyBlockInfo
{
    internal int Index;
    internal string FirstKey;          // 原始值（v2.0 带一个结尾的 \0，与参考实现一致）
    internal string LastKey;
    internal string FirstKeyTrimmed;   // 去掉结尾 \0，用于范围比较
    internal string LastKeyTrimmed;
    internal long PackSize;
    internal long UnpackSize;
    internal long PackOffset;          // 相对 _keyBlockStartOffset
    internal long UnpackOffset;        // 相对解压后的词条空间
    internal long EntryCount;
    internal long EntryOffset;         // 该块第一个词条在全局词条序号里的下标
}

/// <summary>record block info 里描述的一个记录块</summary>
internal sealed class RecordBlockInfo
{
    internal int Index;
    internal long PackSize;
    internal long UnpackSize;
    internal long PackOffset;          // 相对 _recordBlockStartOffset
    internal long UnpackOffset;        // 相对解压后的记录空间
}

/// <summary>一次命中的词条（对外的 MdxEntry 还要再去取正文）</summary>
internal struct KeyHit
{
    internal int BlockIndex;
    internal int EntryIndex;
    internal string Text;
}

/// <summary>
/// MDict（.mdx/.mdd）通用解析核心，逐行对照 <c>js-mdict/dist/cjs/mdict-base.js</c> + <c>mdict.js</c>。
///
/// 与参考实现最大的结构差异：参考实现把全部词条读进内存再排序（20 万词条也照读），
/// 这里按需解压——构造时只读头部 + 两个索引区，查询时只解压命中的词块，
/// 并用 LRU 缓存住最近用过的块。因此内存占用与词典大小基本无关。
/// </summary>
internal sealed class MdictCore : IDisposable
{
    // 词块解压后约 32KB（big.mdx），缓存 64 个约 2MB；记录块解压后约 23KB，缓存 32 个约 0.8MB。
    // 取这个数量是因为「顺序浏览整本词典」时也要避免反复解压同一块，同时总内存保持在个位 MB。
    private const int KeyBlockCacheSize = 64;
    private const int RecordBlockCacheSize = 32;

    private const string EncodingUtf16 = "UTF-16";
    private const string EncodingUtf8 = "UTF-8";
    private const string EncodingBig5 = "BIG5";
    private const string EncodingGb18030 = "GB18030";

    // 块的 4 字节压缩类型字段在小端机器上是 02 00 00 00（参考实现按 hex 字符串比较），
    // 这里按大端读取，所以常量是 0x02000000
    private const uint CompressNone = 0x00000000;
    private const uint CompressLzo = 0x01000000;
    private const uint CompressZlib = 0x02000000;

    private static readonly Regex AttributePattern = new Regex(
        "([A-Za-z0-9_]+)=\"([^\"]*)\"",
        RegexOptions.Compiled);

    // 与 utils.js 的 REGEXP_STRIPKEY 等价（原正则末尾的 () 只是为了让 '$1' 变成空串）
    private static readonly Regex StripKeyMdx = new Regex("[().,\\-&、 '/\\\\@_$!]", RegexOptions.Compiled);
    private static readonly Regex StripKeyMdd = new Regex("([.][^.]*$)|[()., '/@]", RegexOptions.Compiled);

    private readonly string _path;
    private readonly FileStream _stream;
    private readonly object _ioLock = new object();
    private readonly LruCache<int, KeyEntry[]> _keyBlockCache;
    private readonly LruCache<int, byte[]> _recordBlockCache;
    private readonly List<string> _warnings = new List<string>();
    private readonly object _warningsLock = new object();

    private long[] _blockFirstRecordStart;   // 懒计算：每个词块第一个词条的记录偏移

    internal MdictCore(string path)
    {
        _path = path ?? throw new ArgumentNullException(nameof(path));
        IsMdd = string.Equals(Path.GetExtension(path)?.TrimStart('.'), "mdd", StringComparison.OrdinalIgnoreCase);
        Extension = IsMdd ? "mdd" : "mdx";

        _keyBlockCache = new LruCache<int, KeyEntry[]>(KeyBlockCacheSize);
        _recordBlockCache = new LruCache<int, byte[]>(RecordBlockCacheSize);

        _stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete, 64 * 1024, FileOptions.RandomAccess);

        try
        {
            ReadHeader();
            ReadKeyHeader();
            ReadKeyInfos();
            ReadRecordHeader();
            ReadRecordInfos();
            DetectBlockOrder();
        }
        catch
        {
            _stream.Dispose();
            throw;
        }
    }

    // ---------------------------------------------------------------- 元信息

    internal string Extension { get; }
    internal bool IsMdd { get; }
    internal string RawXml { get; private set; } = string.Empty;
    internal IReadOnlyDictionary<string, string> Attributes { get; private set; } = new Dictionary<string, string>();

    /// <summary>头部写的字符集（与参考实现的 meta.encoding 对齐：GBK/GB2312 会归一成 GB18030，mdd 固定 UTF-16）</summary>
    internal string EffectiveEncoding { get; private set; } = EncodingUtf8;

    /// <summary>头部 GeneratedByEngineVersion，解析不出来时是 NaN（参考实现里 NaN 会走 v1.2 分支）</summary>
    internal double Version { get; private set; } = double.NaN;

    /// <summary>头部 Encrypted：0=不加密，1=记录块加密，2=键信息块加密</summary>
    internal int Encrypted { get; private set; }

    internal int NumWidth { get; private set; } = 4;

    internal long KeyCount { get; private set; }
    internal long KeyBlockCount { get; private set; }
    internal long KeyInfoPackedSize { get; private set; }
    internal long KeyInfoUnpackSize { get; private set; }
    internal long KeyBlockPackedSize { get; private set; }
    internal long RecordBlockCount { get; private set; }
    internal long RecordEntriesNum { get; private set; }
    internal long RecordInfoCompSize { get; private set; }
    internal long RecordBlockCompSize { get; private set; }
    internal long TotalRecordUnpackedSize { get; private set; }

    internal KeyBlockInfo[] KeyBlockInfos { get; private set; } = new KeyBlockInfo[0];
    internal RecordBlockInfo[] RecordBlockInfos { get; private set; } = new RecordBlockInfo[0];

    /// <summary>
    /// 相邻词块的首尾词在序数序上是否单调。单调时可以用二分定位词块（真实词典都成立）；
    /// 不单调说明这个词典的文件内键序很奇怪，查询会退化成全块扫描以保证正确性。
    /// </summary>
    internal bool BlockOrderMonotone { get; private set; } = true;

    /// <summary>缓存/校验统计，仅供对照测试与诊断使用</summary>
    internal int ChecksumChecked { get; private set; }
    internal int ChecksumMismatched { get; private set; }

    internal IReadOnlyList<string> Warnings
    {
        get
        {
            lock (_warningsLock) return _warnings.ToArray();
        }
    }

    /// <summary>记一条诊断信息（查询可能来自多个线程，所以加锁；最多留 64 条）</summary>
    private void Warn(string message)
    {
        lock (_warningsLock)
        {
            if (_warnings.Count < 64) _warnings.Add(message);
        }
    }

    internal int KeyBlockCacheCount => _keyBlockCache.Count;
    internal int RecordBlockCacheCount => _recordBlockCache.Count;

    private Encoding _decoder = new UTF8Encoding(false);

    // ---------------------------------------------------------------- 文件读取

    private byte[] ReadAt(long offset, int length)
    {
        if (length <= 0) return new byte[0];
        var buffer = new byte[length];
        var read = 0;
        lock (_ioLock)
        {
            _stream.Seek(offset, SeekOrigin.Begin);
            while (read < length)
            {
                var n = _stream.Read(buffer, read, length - read);
                if (n <= 0) break;
                read += n;
            }
        }
        if (read != length)
        {
            // 参考实现的 FileScanner 会返回“实际读到的长度”，这里保持一致
            Array.Resize(ref buffer, read);
        }
        return buffer;
    }

    private static uint ReadUInt32Be(byte[] b, int offset)
    {
        return (uint)((b[offset] << 24) | (b[offset + 1] << 16) | (b[offset + 2] << 8) | b[offset + 3]);
    }

    /// <summary>
    /// 等价于 utils.b2n(buffer.slice(offset, offset + width))：
    /// width 只可能是 1/2/4/8（numWidth 与 numWidth/4），长度不足时参考实现返回 0。
    /// </summary>
    private static long ReadNumber(byte[] b, int offset, int width)
    {
        if (offset < 0 || offset + width > b.Length) return 0;
        switch (width)
        {
            case 1:
                return b[offset];
            case 2:
                return (b[offset] << 8) | b[offset + 1];
            case 4:
                return ReadUInt32Be(b, offset);
            case 8:
                ulong high = 0;
                for (var i = 0; i < 4; i++) high = (high << 8) | b[offset + i];
                // 参考实现会拒绝超过 2^53 的值（JS 数值精度），这里同样不声不响地截断
                if (high > 0x1FFFFF) throw new InvalidDataException("MDict 变长整数超过 2^53，文件可能已损坏");
                ulong low = 0;
                for (var i = 4; i < 8; i++) low = (low << 8) | b[offset + i];
                return (long)((high << 32) + low);
            default:
                return 0;
        }
    }

    private long ReadNumber(byte[] b, ref int offset)
    {
        var value = ReadNumber(b, offset, NumWidth);
        offset += NumWidth;
        return value;
    }

    // ---------------------------------------------------------------- 头部

    private long _headerEndOffset;
    private long _keyHeaderStartOffset;
    private long _keyHeaderEndOffset;
    private long _keyBlockInfoStartOffset;
    private long _keyBlockInfoEndOffset;
    private long _keyBlockStartOffset;
    private long _recordHeaderStartOffset;
    private long _recordHeaderEndOffset;
    private long _recordInfoStartOffset;
    private long _recordBlockStartOffset;

    private void ReadHeader()
    {
        var sizeBuffer = ReadAt(0, 4);
        if (sizeBuffer.Length < 4) throw new InvalidDataException("文件太小，不是有效的 MDict 词典：" + _path);
        var headerByteSize = (int)ReadUInt32Be(sizeBuffer, 0);
        if (headerByteSize <= 0) throw new InvalidDataException("MDict 头部长度非法：" + headerByteSize);
        var headerBuffer = ReadAt(4, headerByteSize);

        // 参考实现一律按 UTF-16LE 解码头部。官方格式确实都是 UTF-16LE，
        // 但少数第三方工具写的是 UTF-8，所以这里做一次自动判定：
        // 只有 UTF-16LE 解不出任何属性时才退到 UTF-8（参考实现在那种文件上什么也读不到）。
        var utf16Text = new UnicodeEncoding(false, false).GetString(headerBuffer);
        var attributes = ParseAttributes(utf16Text);
        RawXml = utf16Text;
        if (attributes.Count == 0)
        {
            var utf8Text = new UTF8Encoding(false).GetString(headerBuffer);
            var utf8Attributes = ParseAttributes(utf8Text);
            if (utf8Attributes.Count > 0)
            {
                attributes = utf8Attributes;
                RawXml = utf8Text;
            }
        }

        // 头部结尾 = 4 字节长度 + 正文 + 4 字节 adler32（参考实现不校验头部 adler32）
        _headerEndOffset = headerByteSize + 8;
        _keyHeaderStartOffset = _headerEndOffset;

        Attributes = attributes;

        Version = ParseFloatPrefix(Get("GeneratedByEngineVersion"));
        NumWidth = Version >= 2.0 ? 8 : 4;

        Encrypted = ParseEncrypted(Get("Encrypted"));

        var encodingAttr = Get("Encoding");
        if (IsMdd)
        {
            // .mdd 一律按 UTF-16LE 处理（参考实现同样强制覆盖）
            EffectiveEncoding = EncodingUtf16;
        }
        else if (string.IsNullOrEmpty(encodingAttr))
        {
            EffectiveEncoding = EncodingUtf8;
        }
        else if (encodingAttr == "GBK" || encodingAttr == "GB2312")
        {
            // GBK 是 GB18030 的子集，参考实现（TextDecoder('gb18030')）也这么做
            EffectiveEncoding = EncodingGb18030;
        }
        else if (string.Equals(encodingAttr, "big5", StringComparison.OrdinalIgnoreCase))
        {
            EffectiveEncoding = EncodingBig5;
        }
        else
        {
            var lower = encodingAttr.ToLowerInvariant();
            EffectiveEncoding = lower == "utf16" || lower == "utf-16" ? EncodingUtf16 : EncodingUtf8;
        }

        _decoder = ResolveEncoding(EffectiveEncoding);
    }

    private static Encoding ResolveEncoding(string name)
    {
        switch (name)
        {
            case EncodingUtf16:
                return new UnicodeEncoding(false, false);
            case EncodingGb18030:
                // net48 自带这个代码页（54936），无需额外依赖
                return Encoding.GetEncoding(54936);
            case EncodingBig5:
                return Encoding.GetEncoding(950);
            default:
                return new UTF8Encoding(false);
        }
    }

    /// <summary>等价于 utils.parseHeader：正则抓 \w+="..."，再做 &amp;xx; 反转义</summary>
    private static Dictionary<string, string> ParseAttributes(string text)
    {
        var result = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (Match match in AttributePattern.Matches(text))
        {
            // 同名属性后者覆盖前者，与 Object.assign 一致
            result[match.Groups[1].Value] = UnescapeEntities(match.Groups[2].Value);
        }
        return result;
    }

    private static string UnescapeEntities(string text)
    {
        // 顺序必须和参考实现一致：&amp; 放最后，这样 &amp;lt; 会解成 &lt; 而不是 <
        return text.Replace("&lt;", "<").Replace("&gt;", ">").Replace("&quot;", "\"").Replace("&amp;", "&");
    }

    private string Get(string name)
    {
        if (Attributes.TryGetValue(name, out var value)) return value;
        // 大小写容错：只在完全同名的属性不存在时才生效，不影响与参考实现的一致性
        foreach (var pair in Attributes)
        {
            if (string.Equals(pair.Key, name, StringComparison.OrdinalIgnoreCase)) return pair.Value;
        }
        return null;
    }

    /// <summary>parseFloat 语义：取前导数字前缀，解析不出来返回 NaN</summary>
    private static double ParseFloatPrefix(string text)
    {
        if (string.IsNullOrEmpty(text)) return double.NaN;
        var i = 0;
        while (i < text.Length && char.IsWhiteSpace(text[i])) i++;
        var start = i;
        if (i < text.Length && (text[i] == '+' || text[i] == '-')) i++;
        while (i < text.Length && char.IsDigit(text[i])) i++;
        if (i < text.Length && text[i] == '.')
        {
            i++;
            while (i < text.Length && char.IsDigit(text[i])) i++;
        }
        if (i < text.Length && (text[i] == 'e' || text[i] == 'E'))
        {
            var j = i + 1;
            if (j < text.Length && (text[j] == '+' || text[j] == '-')) j++;
            var digits = j;
            while (j < text.Length && char.IsDigit(text[j])) j++;
            if (j > digits) i = j;
        }
        var candidate = text.Substring(start, i - start);
        return double.TryParse(candidate, NumberStyles.Float, CultureInfo.InvariantCulture, out var value) ? value : double.NaN;
    }

    /// <summary>parseInt(x, 10) 语义；解析失败（NaN）时参考实现等同于 0</summary>
    private static int ParseEncrypted(string text)
    {
        if (string.IsNullOrEmpty(text) || text == "No") return 0;
        if (text == "Yes") return 1;

        var i = 0;
        while (i < text.Length && char.IsWhiteSpace(text[i])) i++;
        var start = i;
        if (i < text.Length && (text[i] == '+' || text[i] == '-')) i++;
        while (i < text.Length && char.IsDigit(text[i])) i++;
        if (i == start) return 0;
        return int.TryParse(text.Substring(start, i - start), NumberStyles.Integer, CultureInfo.InvariantCulture, out var value) ? value : 0;
    }

    // ---------------------------------------------------------------- 键区头部

    private void ReadKeyHeader()
    {
        _keyHeaderStartOffset = _headerEndOffset;
        var headerMetaSize = Version >= 2.0 ? 8 * 5 : 4 * 4;
        var buffer = ReadAt(_keyHeaderStartOffset, headerMetaSize);
        var offset = 0;

        KeyBlockCount = ReadNumber(buffer, ref offset);
        KeyCount = ReadNumber(buffer, ref offset);
        if (Version >= 2.0)
        {
            // v1.2 没有这个字段：键信息块不压缩，也就无所谓解压后大小
            KeyInfoUnpackSize = ReadNumber(buffer, ref offset);
        }
        KeyInfoPackedSize = ReadNumber(buffer, ref offset);
        KeyBlockPackedSize = ReadNumber(buffer, ref offset);

        // v2.0 在键区头部之后还有一个 4 字节 adler32
        _keyHeaderEndOffset = _keyHeaderStartOffset + headerMetaSize + (Version >= 2.0 ? 4 : 0);
    }

    // ---------------------------------------------------------------- 键信息块

    private void ReadKeyInfos()
    {
        _keyBlockInfoStartOffset = _keyHeaderEndOffset;
        var packedBuffer = ReadAt(_keyBlockInfoStartOffset, (int)KeyInfoPackedSize);
        _keyBlockInfoEndOffset = _keyBlockInfoStartOffset + KeyInfoPackedSize;
        _keyBlockStartOffset = _keyBlockInfoEndOffset;
        _recordBlockStartOffset = _keyBlockInfoEndOffset + KeyBlockPackedSize;

        var buffer = DecodeKeyBlockInfo(packedBuffer);

        var list = new List<KeyBlockInfo>((int)Math.Max(0, KeyBlockCount));
        var offset = 0;
        long entriesCount = 0;
        long packAccumulator = 0;
        long unpackAccumulator = 0;

        for (long kb = 0; kb < KeyBlockCount; kb++)
        {
            var info = new KeyBlockInfo { Index = (int)kb };
            var blockWordCount = ReadNumber(buffer, ref offset);

            // 词条名长度字段只有 numWidth/4 字节（v2.0 = 2 字节，v1.2 = 1 字节）
            var firstWordSize = ReadNumber(buffer, offset, NumWidth / 4);
            offset += NumWidth / 4;
            firstWordSize = AdjustWordSize(firstWordSize);
            info.FirstKey = Decode(buffer, offset, (int)firstWordSize);
            offset += (int)firstWordSize;

            var lastWordSize = ReadNumber(buffer, offset, NumWidth / 4);
            offset += NumWidth / 4;
            lastWordSize = AdjustWordSize(lastWordSize);
            info.LastKey = Decode(buffer, offset, (int)lastWordSize);
            offset += (int)lastWordSize;

            info.PackSize = ReadNumber(buffer, ref offset);
            info.UnpackSize = ReadNumber(buffer, ref offset);

            info.FirstKeyTrimmed = TrimNul(info.FirstKey);
            info.LastKeyTrimmed = TrimNul(info.LastKey);
            info.PackOffset = packAccumulator;
            info.UnpackOffset = unpackAccumulator;
            info.EntryCount = blockWordCount;
            info.EntryOffset = entriesCount;

            list.Add(info);

            entriesCount += blockWordCount;
            packAccumulator += info.PackSize;
            unpackAccumulator += info.UnpackSize;
        }

        if (packAccumulator != KeyBlockPackedSize)
        {
            Warn($"词块压缩总大小 {packAccumulator} 与键区头部记录 {KeyBlockPackedSize} 不一致");
        }
        if (entriesCount != KeyCount)
        {
            Warn($"键信息块里统计的词条数 {entriesCount} 与键区头部记录 {KeyCount} 不一致");
        }

        KeyBlockInfos = list.ToArray();
    }

    /// <summary>
    /// 词条名长度：v2.0 会在长度里补上结尾的 \0（UTF-16 补 2 字节），v1.2 不补。
    /// </summary>
    private long AdjustWordSize(long size)
    {
        if (Version >= 2.0)
        {
            return EffectiveEncoding == EncodingUtf16 ? (size + 1) * 2 : size + 1;
        }
        return EffectiveEncoding == EncodingUtf16 ? size * 2 : size;
    }

    private byte[] DecodeKeyBlockInfo(byte[] packedBuffer)
    {
        if (!(Version >= 2.0))
        {
            // v1.2 的键信息块不压缩，也没有 4 字节压缩类型
            return packedBuffer;
        }

        if (packedBuffer.Length < 8) throw new InvalidDataException("键信息块长度不足 8 字节");

        var compressType = ReadUInt32Be(packedBuffer, 0);
        var checksum = ReadUInt32Be(packedBuffer, 4);
        var buffer = packedBuffer;

        // Encrypted="2"：键信息块整块被 fast_decrypt 加密（前 8 字节保留）
        if (Encrypted == 2) buffer = MdictCrypto.MdxDecrypt(buffer);

        var payload = Slice(buffer, 8, buffer.Length - 8);
        switch (compressType)
        {
            case CompressNone:
                return payload;
            case CompressLzo:
                var lzo = Lzo1x.Decompress(payload);
                CheckChecksum(checksum, lzo);
                return lzo;
            case CompressZlib:
                var inflate = ZlibInflate(payload, KeyInfoUnpackSize);
                CheckChecksum(checksum, inflate);
                return inflate;
            default:
                throw new InvalidDataException($"无法识别的键信息块压缩类型：0x{compressType:X8}");
        }
    }

    // ---------------------------------------------------------------- 记录区头部与索引

    private void ReadRecordHeader()
    {
        _recordHeaderStartOffset = _keyBlockInfoEndOffset + KeyBlockPackedSize;
        var length = Version >= 2.0 ? 4 * 8 : 4 * 4;
        var buffer = ReadAt(_recordHeaderStartOffset, length);
        var offset = 0;

        RecordBlockCount = ReadNumber(buffer, ref offset);
        RecordEntriesNum = ReadNumber(buffer, ref offset);
        RecordInfoCompSize = ReadNumber(buffer, ref offset);
        RecordBlockCompSize = ReadNumber(buffer, ref offset);

        _recordHeaderEndOffset = _recordHeaderStartOffset + length;
        _recordInfoStartOffset = _recordHeaderEndOffset;

        if (RecordEntriesNum != KeyCount)
        {
            Warn($"记录区头部记录的词条数 {RecordEntriesNum} 与键区 {KeyCount} 不一致（参考实现此处会断言失败）");
        }
    }

    private void ReadRecordInfos()
    {
        var buffer = ReadAt(_recordInfoStartOffset, (int)RecordInfoCompSize);
        var list = new List<RecordBlockInfo>((int)Math.Max(0, RecordBlockCount));
        var offset = 0;
        long packAccumulator = 0;
        long unpackAccumulator = 0;

        for (long i = 0; i < RecordBlockCount; i++)
        {
            var packSize = ReadNumber(buffer, ref offset);
            var unpackSize = ReadNumber(buffer, ref offset);
            list.Add(new RecordBlockInfo
            {
                Index = (int)i,
                PackSize = packSize,
                UnpackSize = unpackSize,
                PackOffset = packAccumulator,
                UnpackOffset = unpackAccumulator,
            });
            packAccumulator += packSize;
            unpackAccumulator += unpackSize;
        }

        if (packAccumulator != RecordBlockCompSize)
        {
            Warn($"记录块压缩总大小 {packAccumulator} 与记录区头部记录 {RecordBlockCompSize} 不一致");
        }

        RecordBlockInfos = list.ToArray();
        TotalRecordUnpackedSize = unpackAccumulator;
        // 记录区数据紧跟在记录索引之后（ReadKeyInfos 里给的只是“记录区头部”的起点）
        _recordBlockStartOffset = _recordInfoStartOffset + RecordInfoCompSize;
        _blockFirstRecordStart = new long[KeyBlockInfos.Length];
        for (var i = 0; i < _blockFirstRecordStart.Length; i++) _blockFirstRecordStart[i] = -1;
    }

    private void DetectBlockOrder()
    {
        for (var i = 1; i < KeyBlockInfos.Length; i++)
        {
            if (string.CompareOrdinal(KeyBlockInfos[i - 1].LastKeyTrimmed, KeyBlockInfos[i].FirstKeyTrimmed) > 0)
            {
                BlockOrderMonotone = false;
                Warn($"第 {i} 个词块的首词在序数序上早于前一块的尾词，查询会退化为全块扫描");
                break;
            }
        }

        BuildNormalizedIndex();
    }

    /*
     * 词块索引里的首尾词，可能是**归一化**过的形式。
     *
     * 实测 LDOCE5.mdx（157493 词条 / 116 词块）：索引里写的是
     *     bunnyboiler        ← 真实键 "bunny boiler"
     *     collocation000579  ← 真实键 "_collocation_000579"
     *     edinburgh          ← 真实键 "Edinburgh"
     *     hazlittwilliam     ← 真实键 "Hazlitt, William"
     * 也就是「只留字母数字 + 转小写」。逐个核对过全部 116 个块的首尾词，116/116 成立。
     *
     * 后果很严重：这些词典的索引区间和真实键不在同一个字符空间里，只按序数比较的话
     * 二分永远落不进任何区间 —— 现象就是"词条明明在词典里却查不到"：
     * LDOCE5 词条里 `entry://_collocation_100956` 这类链接（释义里的搭配/例句/词族框）
     * 全部跳不过去，大写词条（Edinburgh、GI）和带空格的多词条目（bunny boiler、
     * South America）也直接查不到。
     *
     * 归一化只用来**挑词块**，最终命中仍然要求键与查询词逐字符相等，
     * 所以这不会改变大小写敏感之类的既有语义，只会把"本该找得到"的键找回来。
     */
    private string[] _normalizedFirstKeys = new string[0];
    private string[] _normalizedLastKeys = new string[0];

    /// <summary>索引键的归一化形式：只留字母数字，其余（空格、下划线、点、连字符…）丢掉，并转小写</summary>
    internal static string NormalizeIndexKey(string text)
    {
        if (string.IsNullOrEmpty(text)) return text ?? string.Empty;
        var needsWork = false;
        for (var i = 0; i < text.Length; i++)
        {
            var ch = text[i];
            if (!char.IsLetterOrDigit(ch) || char.IsUpper(ch)) { needsWork = true; break; }
        }
        if (!needsWork) return text;

        var builder = new StringBuilder(text.Length);
        for (var i = 0; i < text.Length; i++)
        {
            var ch = text[i];
            // 只留字母数字：\p{L}\p{N}（含中日韩与带音标的拉丁字母），其余一律丢掉
            if (!char.IsLetterOrDigit(ch)) continue;
            builder.Append(char.ToLowerInvariant(ch));
        }
        return builder.ToString();
    }

    private void BuildNormalizedIndex()
    {
        _normalizedFirstKeys = new string[KeyBlockInfos.Length];
        _normalizedLastKeys = new string[KeyBlockInfos.Length];
        for (var i = 0; i < KeyBlockInfos.Length; i++)
        {
            _normalizedFirstKeys[i] = NormalizeIndexKey(KeyBlockInfos[i].FirstKeyTrimmed);
            _normalizedLastKeys[i] = NormalizeIndexKey(KeyBlockInfos[i].LastKeyTrimmed);
        }
    }

    /// <summary>索引区间（归一化形式）是否包含这个已经归一化过的词</summary>
    private bool ContainsNormalized(int blockIndex, string normalizedWord)
    {
        return string.CompareOrdinal(_normalizedFirstKeys[blockIndex], normalizedWord) <= 0 &&
               string.CompareOrdinal(_normalizedLastKeys[blockIndex], normalizedWord) >= 0;
    }

    /// <summary>
    /// 走了归一化定位才命中的次数（诊断用）。
    /// 大于 0 就说明这本词典的索引键是归一化形式 —— 光看索引本身判断不出来
    /// （索引里的键本来就是"去标点 + 小写"的样子），只有真去查一个带标点/大写的词才知道。
    /// </summary>
    internal int NormalizedHits { get; private set; }

    // ---------------------------------------------------------------- 压缩

    /// <summary>
    /// zlib 解压。net48 没有 ZLibStream，所以跳过 2 字节 zlib 头交给 DeflateStream，
    /// 尾部的 adler32 因为流已结束自然被忽略（adler32 另行校验）。
    /// </summary>
    private byte[] ZlibInflate(byte[] payload, long expectedSize)
    {
        if (payload.Length < 2) throw new InvalidDataException("zlib 数据长度不足");
        var capacity = expectedSize > 0 && expectedSize <= int.MaxValue ? (int)expectedSize : Math.Max(256, payload.Length * 4);
        try
        {
            using (var input = new MemoryStream(payload, 2, payload.Length - 2, false))
            using (var deflate = new DeflateStream(input, CompressionMode.Decompress))
            using (var output = new MemoryStream(capacity))
            {
                deflate.CopyTo(output);
                return output.ToArray();
            }
        }
        catch (InvalidDataException ex)
        {
            throw new InvalidDataException("zlib 解压失败：" + ex.Message, ex);
        }
    }

    private void CheckChecksum(uint expected, byte[] data)
    {
        if (expected == 0) return;
        ChecksumChecked++;
        var actual = MdictCrypto.Adler32(data, 0, data.Length);
        if (actual != expected)
        {
            ChecksumMismatched++;
            Warn($"解压数据 adler32 校验不符：期望 {expected:X8}，实际 {actual:X8}");
        }
    }

    // ---------------------------------------------------------------- 词块

    internal KeyEntry[] GetKeyBlock(int index)
    {
        return _keyBlockCache.GetOrAdd(index, i =>
        {
            var info = KeyBlockInfos[i];
            var packed = ReadAt(_keyBlockStartOffset + info.PackOffset, (int)info.PackSize);
            var data = UnpackKeyBlock(packed, info, i);
            return SplitKeyBlock(data);
        });
    }

    private byte[] UnpackKeyBlock(byte[] packed, KeyBlockInfo info, int index)
    {
        if (packed.Length < 8) throw new InvalidDataException($"第 {index} 个词块长度不足 8 字节");
        var compressType = ReadUInt32Be(packed, 0);
        var checksum = ReadUInt32Be(packed, 4);
        byte[] data;
        switch (compressType)
        {
            case CompressNone:
                return Slice(packed, 8, packed.Length - 8);
            case CompressLzo:
                data = Lzo1x.Decompress(Slice(packed, 8, packed.Length - 8));
                break;
            case CompressZlib:
                data = ZlibInflate(Slice(packed, 8, packed.Length - 8), info.UnpackSize);
                break;
            default:
                throw new InvalidDataException($"无法识别的词块压缩类型：0x{compressType:X8}");
        }
        if (data.Length != info.UnpackSize)
        {
            Warn($"第 {index} 个词块解压后 {data.Length} 字节，索引里写的是 {info.UnpackSize} 字节");
        }
        CheckChecksum(checksum, data);
        return data;
    }

    /// <summary>
    /// 切分词块：每项是 [numWidth 字节的记录偏移][词条名][\0]，
    /// 词条名的分隔符宽度由编码决定——UTF-16 词典（含所有 .mdd）是 2 字节，其余 1 字节。
    /// </summary>
    private KeyEntry[] SplitKeyBlock(byte[] block)
    {
        var width = EffectiveEncoding == EncodingUtf16 || IsMdd ? 2 : 1;
        var list = new List<KeyEntry>();
        var keyStartIndex = 0;

        while (keyStartIndex < block.Length)
        {
            var meaningOffset = ReadNumber(block, keyStartIndex, NumWidth);

            var keyEndIndex = -1;
            var i = keyStartIndex + NumWidth;
            while (i < block.Length)
            {
                var terminated = width == 1
                    ? block[i] == 0
                    : i + 1 < block.Length && block[i] == 0 && block[i + 1] == 0;
                if (terminated)
                {
                    keyEndIndex = i;
                    break;
                }
                i += width;
            }
            // 参考实现找不到结尾的 \0 就丢弃剩余数据（词块末尾的空词条也是这么处理的）
            if (keyEndIndex == -1) break;

            var keyTextOffset = keyStartIndex + NumWidth;
            list.Add(new KeyEntry
            {
                Text = Decode(block, keyTextOffset, keyEndIndex - keyTextOffset),
                RecordStart = meaningOffset,
            });

            keyStartIndex = keyEndIndex + width;
        }

        return list.ToArray();
    }

    // ---------------------------------------------------------------- 记录块

    internal byte[] GetRecordBlock(int index)
    {
        return _recordBlockCache.GetOrAdd(index, i =>
        {
            var info = RecordBlockInfos[i];
            var packed = ReadAt(_recordBlockStartOffset + info.PackOffset, (int)info.PackSize);
            return DecompressRecordBlock(packed, info, i);
        });
    }

    /// <summary>按块遍历解压后的记录数据（@@@LINK= 之类的全库扫描用）</summary>
    internal byte[] GetRecordBlockForScan(int index) => GetRecordBlock(index);

    private byte[] DecompressRecordBlock(byte[] packed, RecordBlockInfo info, int index)
    {
        if (packed.Length < 8) throw new InvalidDataException($"第 {index} 个记录块长度不足 8 字节");
        var compressType = ReadUInt32Be(packed, 0);
        var checksum = ReadUInt32Be(packed, 4);

        if (compressType == CompressNone)
        {
            // 未压缩块不做解密，与参考实现一致
            return Slice(packed, 8, packed.Length - 8);
        }

        // Encrypted="1"：记录块被加密，密钥同样由块内 4..8 字节派生
        var buffer = Encrypted == 1 ? MdictCrypto.MdxDecrypt(packed) : packed;
        var payload = Slice(buffer, 8, buffer.Length - 8);

        byte[] data;
        switch (compressType)
        {
            case CompressLzo:
                data = Lzo1x.Decompress(payload);
                break;
            case CompressZlib:
                data = ZlibInflate(payload, info.UnpackSize);
                break;
            default:
                throw new InvalidDataException($"无法识别的记录块压缩类型：0x{compressType:X8}");
        }

        if (data.Length != info.UnpackSize)
        {
            Warn($"第 {index} 个记录块解压后 {data.Length} 字节，索引里写的是 {info.UnpackSize} 字节");
        }

        // 加密词典（Encrypted=1）里 odler32 对不上几乎只有一个原因：密钥不对
        // （真实词典的密钥由用户注册信息派生，我们和参考实现一样拿不到）。
        // 这时宁可抛错让调用方看到「这个词典读不了」，也不要不报错地返回乱码；
        // 未加密的词典只记告警，避免个别坏块把整本词典拖死。
        if (Encrypted == 1 && checksum != 0 && MdictCrypto.Adler32(data, 0, data.Length) != checksum)
        {
            throw new InvalidDataException(
                $"第 {index} 个记录块的 adler32 校验失败：该词典标记为 Encrypted=\"1\"，但它的密钥需要用户注册信息，" +
                "无法用 MDict 的固定派生密钥解开（参考实现遇到这类词典会直接报错退出）");
        }

        CheckChecksum(checksum, data);
        return data;
    }

    /// <summary>等价于 mdict.js 的 reduceRecordBlockInfo：找到 recordStart 落在哪个记录块</summary>
    private int ReduceRecordBlockInfo(long recordStart)
    {
        var left = 0;
        var right = RecordBlockInfos.Length - 1;
        while (left <= right)
        {
            var mid = left + ((right - left) >> 1);
            if (recordStart >= RecordBlockInfos[mid].UnpackOffset) left = mid + 1;
            else right = mid - 1;
        }
        return left - 1;
    }

    private byte[] GetRecordBytes(long start, long end)
    {
        var blockIndex = ReduceRecordBlockInfo(start);
        if (blockIndex < 0) return new byte[0];
        var info = RecordBlockInfos[blockIndex];
        var block = GetRecordBlock(blockIndex);

        var relativeStart = start - info.UnpackOffset;
        var relativeEnd = end - info.UnpackOffset;
        if (relativeStart < 0) relativeStart = 0;
        // 参考实现用 slice(start, end)，越界会被截断，这里保持一致
        if (relativeEnd > block.Length) relativeEnd = block.Length;
        var length = relativeEnd - relativeStart;
        if (length <= 0) return new byte[0];

        var result = new byte[length];
        Buffer.BlockCopy(block, (int)relativeStart, result, 0, (int)length);
        return result;
    }

    /// <summary>某个词块第一个词条的记录偏移（末尾词条需要它来算正文结束位置）</summary>
    private long FirstRecordStartOfBlock(int index)
    {
        if (index >= KeyBlockInfos.Length) return TotalRecordUnpackedSize;
        var cached = _blockFirstRecordStart[index];
        if (cached >= 0) return cached;
        var entries = GetKeyBlock(index);
        var value = entries.Length > 0 ? entries[0].RecordStart : 0;
        _blockFirstRecordStart[index] = value;
        return value;
    }

    /// <summary>词条正文（已按词典字符集解码）</summary>
    internal string GetDefinition(int blockIndex, int entryIndex)
    {
        var entries = GetKeyBlock(blockIndex);
        if (entryIndex < 0 || entryIndex >= entries.Length) return null;
        var start = entries[entryIndex].RecordStart;

        long end;
        if (entryIndex + 1 < entries.Length) end = entries[entryIndex + 1].RecordStart;
        else if (blockIndex + 1 < KeyBlockInfos.Length) end = FirstRecordStartOfBlock(blockIndex + 1);
        else end = TotalRecordUnpackedSize;

        return Decode(GetRecordBytes(start, end));
    }

    /// <summary>原始正文（.mdd 用，不解码成字符串）</summary>
    internal byte[] GetRecordRaw(int blockIndex, int entryIndex)
    {
        var entries = GetKeyBlock(blockIndex);
        if (entryIndex < 0 || entryIndex >= entries.Length) return null;
        var start = entries[entryIndex].RecordStart;

        long end;
        if (entryIndex + 1 < entries.Length) end = entries[entryIndex + 1].RecordStart;
        else if (blockIndex + 1 < KeyBlockInfos.Length) end = FirstRecordStartOfBlock(blockIndex + 1);
        else end = TotalRecordUnpackedSize;

        return GetRecordBytes(start, end);
    }

    // ---------------------------------------------------------------- 查询

    /// <summary>
    /// 精确命中（大小写敏感，与参考实现一致）。
    /// 先按词块首尾词做二分，命中不到再对全部词块做一次“范围包含”扫描
    /// （只比对索引里的首尾词，不解压，代价极低），以覆盖块边界词。
    /// </summary>
    internal bool TryLookupKey(string word, out KeyHit hit)
    {
        hit = default;
        if (string.IsNullOrEmpty(word) || KeyBlockInfos.Length == 0) return false;

        var candidate = FindBlockForWord(word);
        if (candidate >= 0 && Contains(candidate, word) && TryFindInBlock(candidate, word, out var entryIndex))
        {
            hit = new KeyHit { BlockIndex = candidate, EntryIndex = entryIndex, Text = GetKeyBlock(candidate)[entryIndex].Text };
            return true;
        }

        for (var i = 0; i < KeyBlockInfos.Length; i++)
        {
            if (i == candidate || !Contains(i, word)) continue;
            if (TryFindInBlock(i, word, out entryIndex))
            {
                hit = new KeyHit { BlockIndex = i, EntryIndex = entryIndex, Text = GetKeyBlock(i)[entryIndex].Text };
                return true;
            }
        }

        /*
         * 归一化兜底：有些词典的词块索引里存的是"去掉标点 + 小写"的形式
         * （LDOCE5 就是，见 NormalizeIndexKey 的说明），序数比较落不进任何区间。
         * 这里用同一个归一化形式再定位一次 —— 索引区间对不上，块里的键却是原样的，
         * 所以落块之后照样按原词逐字符比对，找不到就是不匹配。
         */
        if (TryFindByNormalizedIndex(word, out hit)) return true;

        // 文件内键序不是序数序时，二分和范围扫描都可能落空，只能老实全扫一遍
        if (!BlockOrderMonotone)
        {
            for (var i = 0; i < KeyBlockInfos.Length; i++)
            {
                if (i == candidate) continue;
                if (TryFindInBlock(i, word, out entryIndex))
                {
                    hit = new KeyHit { BlockIndex = i, EntryIndex = entryIndex, Text = GetKeyBlock(i)[entryIndex].Text };
                    return true;
                }
            }
        }
        return false;
    }

    /// <summary>
    /// 用归一化后的索引区间定位词块，命中与否仍然按**原始词**逐字符判定。
    /// 只在归一化结果和原词不同（也就是原词里有标点/空格/大写）时才需要走这一趟。
    /// </summary>
    private bool TryFindByNormalizedIndex(string word, out KeyHit hit)
    {
        hit = default;
        if (KeyBlockInfos.Length == 0) return false;

        var normalized = NormalizeIndexKey(word);
        if (string.Equals(normalized, word, StringComparison.Ordinal) || normalized.Length == 0) return false;

        for (var i = 0; i < KeyBlockInfos.Length; i++)
        {
            if (!ContainsNormalized(i, normalized)) continue;
            if (TryFindInBlock(i, word, out var entryIndex))
            {
                hit = new KeyHit { BlockIndex = i, EntryIndex = entryIndex, Text = GetKeyBlock(i)[entryIndex].Text };
                NormalizedHits++;
                return true;
            }
        }
        return false;
    }

    /// <summary>
    /// 前缀命中。从“可能包含该前缀”的词块开始向后扫，
    /// 直到某个块的首词已经在序数序上越过前缀且不以它开头（说明后面不可能再有命中）。
    /// </summary>
    internal IEnumerable<KeyHit> PrefixHits(string word)
    {
        // 空前缀等于“列出全部词条”，参考实现只会返回第一个词块的内容（associate 的副作用），
        // 那不是一个有意义的行为，这里明确返回空。
        if (string.IsNullOrEmpty(word) || KeyBlockInfos.Length == 0) yield break;

        var start = FindBlockForWord(word);
        if (start < 0) start = 0;

        /*
         * 索引键是归一化形式的词典（见 NormalizeIndexKey），停止条件也得用归一化形式判：
         * 比如 LDOCE5 前缀 "bunny b" —— 索引里那个块的首词写作 "bunnyboiler"，
         * 序数比较它已经越过 "bunny b" 且不以它开头，按老逻辑当场就停了，
         * 于是 "bunny boiler" 这种带空格的词条永远不会出现在候选列表里。
         */
        var normalizedWord = NormalizeIndexKey(word);

        var scanned = 0;
        for (var i = start; i < KeyBlockInfos.Length; i++)
        {
            scanned++;
            if (scanned > 512 && !BlockOrderMonotone) break;   // 异常词典的兜底，避免退化成整表扫描

            var info = KeyBlockInfos[i];
            if (string.CompareOrdinal(info.FirstKeyTrimmed, word) > 0 &&
                !info.FirstKeyTrimmed.StartsWith(word, StringComparison.Ordinal) &&
                string.CompareOrdinal(_normalizedFirstKeys[i], normalizedWord) > 0 &&
                !_normalizedFirstKeys[i].StartsWith(normalizedWord, StringComparison.Ordinal))
            {
                break;
            }

            var entries = GetKeyBlock(i);
            for (var e = 0; e < entries.Length; e++)
            {
                if (entries[e].Text.StartsWith(word, StringComparison.Ordinal))
                {
                    yield return new KeyHit { BlockIndex = i, EntryIndex = e, Text = entries[e].Text };
                }
            }
        }
    }

    /// <summary>
    /// 复刻 mdict.js 的 lookupKeyBlockByWord(word, true)（即 associate）：
    /// 在“全部词条”的一维下标空间上二分，返回最后一次探测落到的词块。
    /// 模糊查询的范围就取决于它，所以这里不能换成别的定位方式。
    /// </summary>
    internal int AssociateBlockIndex(string word)
    {
        if (KeyBlockInfos.Length == 0) return -1;
        long left = 0;
        var right = KeyCount - 1;
        long mid = 0;
        while (left <= right)
        {
            mid = left + ((right - left) >> 1);
            var blockIndex = BlockOfGlobalIndex(mid, out var offsetInBlock);
            var entry = GetKeyBlock(blockIndex);
            if (offsetInBlock >= entry.Length) offsetInBlock = entry.Length - 1;
            var comparison = string.CompareOrdinal(word ?? string.Empty, entry[offsetInBlock].Text);
            if (comparison > 0) left = mid + 1;
            else if (comparison == 0) break;
            else right = mid - 1;
        }
        return BlockOfGlobalIndex(mid, out _);
    }

    private int BlockOfGlobalIndex(long globalIndex, out int offsetInBlock)
    {
        var low = 0;
        var high = KeyBlockInfos.Length - 1;
        var best = 0;
        while (low <= high)
        {
            var mid = low + ((high - low) >> 1);
            if (KeyBlockInfos[mid].EntryOffset <= globalIndex)
            {
                best = mid;
                low = mid + 1;
            }
            else
            {
                high = mid - 1;
            }
        }
        offsetInBlock = (int)(globalIndex - KeyBlockInfos[best].EntryOffset);
        if (offsetInBlock < 0) offsetInBlock = 0;
        return best;
    }

    /// <summary>最后一个“首词 &lt;= word”的词块下标（序数序），-1 表示 word 比所有首词都小</summary>
    private int FindBlockForWord(string word)
    {
        var low = 0;
        var high = KeyBlockInfos.Length - 1;
        var best = -1;
        while (low <= high)
        {
            var mid = low + ((high - low) >> 1);
            if (string.CompareOrdinal(KeyBlockInfos[mid].FirstKeyTrimmed, word) <= 0)
            {
                best = mid;
                low = mid + 1;
            }
            else
            {
                high = mid - 1;
            }
        }
        return best;
    }

    private bool Contains(int blockIndex, string word)
    {
        var info = KeyBlockInfos[blockIndex];
        return string.CompareOrdinal(info.FirstKeyTrimmed, word) <= 0 &&
               string.CompareOrdinal(info.LastKeyTrimmed, word) >= 0;
    }

    private bool TryFindInBlock(int blockIndex, string word, out int entryIndex)
    {
        var entries = GetKeyBlock(blockIndex);
        for (var i = 0; i < entries.Length; i++)
        {
            if (string.Equals(entries[i].Text, word, StringComparison.Ordinal))
            {
                entryIndex = i;
                return true;
            }
        }
        entryIndex = -1;
        return false;
    }

    // ---------------------------------------------------------------- 解码与字符串工具

    internal string Decode(byte[] data)
    {
        if (data == null || data.Length == 0) return string.Empty;
        return Decode(data, 0, data.Length);
    }

    internal string Decode(byte[] data, int offset, int count)
    {
        if (count <= 0) return string.Empty;
        var text = _decoder.GetString(data, offset, count);
        // TextDecoder('utf-8'/'utf-16le') 默认会吃掉开头的 BOM，这里保持一致
        if ((EffectiveEncoding == EncodingUtf8 || EffectiveEncoding == EncodingUtf16) &&
            text.Length > 0 && text[0] == '\uFEFF')
        {
            return text.Substring(1);
        }
        return text;
    }

    private static string TrimNul(string text)
    {
        return string.IsNullOrEmpty(text) ? text : text.TrimEnd('\0');
    }

    private static byte[] Slice(byte[] source, int offset, int count)
    {
        if (count <= 0) return new byte[0];
        if (offset == 0 && count == source.Length) return source;
        var result = new byte[count];
        Buffer.BlockCopy(source, offset, result, 0, count);
        return result;
    }

    /// <summary>等价于 utils.levenshteinDistance（注意：任一参数为空串时返回 9999，参考实现就是这样）</summary>
    internal static int LevenshteinDistance(string a, string b)
    {
        if (string.IsNullOrEmpty(a)) return 9999;
        if (string.IsNullOrEmpty(b)) return 9999;

        var m = a.Length;
        var n = b.Length;
        var previous = new int[n + 1];
        var current = new int[n + 1];
        for (var j = 0; j <= n; j++) previous[j] = j;

        for (var i = 1; i <= m; i++)
        {
            current[0] = i;
            for (var j = 1; j <= n; j++)
            {
                var cost = a[i - 1] == b[j - 1] ? 0 : 1;
                var deletion = previous[j] + 1;
                var insertion = current[j - 1] + 1;
                var substitution = previous[j - 1] + cost;
                var min = deletion < insertion ? deletion : insertion;
                current[j] = min < substitution ? min : substitution;
            }
            var swap = previous;
            previous = current;
            current = swap;
        }
        return previous[n];
    }

    /// <summary>等价于 mdict-base.js 的 strip（默认选项下 isStripKey/isCaseSensitive 都是 true）</summary>
    internal string StripKey(string key)
    {
        if (key == null) return string.Empty;
        var result = IsMdd
            ? StripKeyMdd.Replace(StripKeyMdd.Replace(key, "$1"), "$1").Replace('_', '!')
            : StripKeyMdx.Replace(key, string.Empty);
        return result.ToLowerInvariant().Trim();
    }

    public void Dispose()
    {
        _keyBlockCache.Clear();
        _recordBlockCache.Clear();
        _stream?.Dispose();
    }
}
