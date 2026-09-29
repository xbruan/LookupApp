using System;

namespace Lookup.Dictionary;

/// <summary>
/// .mdd 资源库。结构与 .mdx 完全相同，区别在于词条名固定按 UTF-16LE 处理、
/// 正文是原始二进制（图片/音频/字体）而不是 HTML 文本。
/// </summary>
public sealed class MddFile : IDisposable
{
    private readonly MdictCore _core;

    public MddFile(string path)
    {
        _core = new MdictCore(path);
        Path = path;
        KeyCount = _core.KeyCount;
    }

    public string Path { get; }

    /// <summary>资源键总数</summary>
    public long KeyCount { get; }

    /// <summary>
    /// 按资源键取原始字节，未命中返回 null。
    /// 注意 MDD 里的键名通常带前导反斜杠（如 <c>\style.css</c>），要与词典里存的键完全一致才能命中。
    /// </summary>
    public byte[] Locate(string key)
    {
        if (string.IsNullOrEmpty(key)) return null;
        if (!_core.TryLookupKey(key, out var hit)) return null;
        return _core.GetRecordRaw(hit.BlockIndex, hit.EntryIndex);
    }

    /// <summary>
    /// 只问"这个键在不在"，**不读记录块**。
    ///
    /// 做发音规划时用得上：一个词条可能挂着八九条音频键（词目英/美 + 若干例句），
    /// 而规划是每次查词后都要算一遍的轻操作 —— 挨个把音频字节解出来太浪费，
    /// 先只看键在不在，真正要出声时再用 <see cref="Locate"/> 取字节。
    /// </summary>
    public bool Contains(string key)
    {
        if (string.IsNullOrEmpty(key)) return false;
        return _core.TryLookupKey(key, out _);
    }

    /// <summary>解析过程中的告警（索引不自洽、adler32 不符等），正常情况下为空</summary>
    public System.Collections.Generic.IReadOnlyList<string> Warnings => _core.Warnings;

    internal MdictCore Core => _core;

    public void Dispose()
    {
        _core.Dispose();
    }
}
