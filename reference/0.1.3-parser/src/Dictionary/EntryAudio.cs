using System;
using System.Collections.Generic;
using System.Text.RegularExpressions;

namespace Lookup.Dictionary;

/// <summary>
/// 词条正文里的一条"自带音频"引用（`sound://` / `snd://` / &lt;audio name=…&gt;）。
///
/// 只描述**词条里写了什么**，不含任何解码知识 —— 这个键最后能不能出声
/// 由 <see cref="Lookup.Audio.AudioCodec"/> 按扩展名/字节判定。
/// </summary>
internal sealed class EntryAudioRef
{
    /// <summary>词条里写的键名（原样，如 `apple__gb_1.spx`、`GB_brelasdeapple.spx`）</summary>
    internal string Key;
    /// <summary>口音：uk / us / null（认不出来）</summary>
    internal string Accent;
    /// <summary>看着像例句音频（例句与词目发音要分开，前者不参与统一发音按钮）</summary>
    internal bool Example;
    /// <summary>从哪儿发现的：href / audio-name / audio-src（诊断用）</summary>
    internal string FoundBy;
}

/// <summary>
/// 从词条 HTML 里把发音引用抠出来，并猜它对应哪种口音。
///
/// 为什么必须从词条里抠、而不是"按词名猜文件名"：LDOCE5 的音档名是
/// `GB_brelasdeapple.spx` 这种内部 ID，牛津高阶是 `apple__gb_1`，
/// 两家的命名规则完全不同，只有词条自己写的那个键才是真话。
/// </summary>
internal static class EntryAudio
{
    /// <summary>`sound://x` / `snd://x`（两种 scheme 都是 MDict 生态里的"播放这段音频"）</summary>
    private static readonly Regex SchemePattern = new Regex(
        @"(?:sound|snd)://([^""'\s>)<>,]+)", RegexOptions.IgnoreCase | RegexOptions.Compiled);

    /// <summary>&lt;audio eid="apple_audio_1" name="apple__gb_1"&gt;：牛津高阶那种"音频占位元素"</summary>
    private static readonly Regex AudioTagPattern = new Regex(
        @"<audio\b[^>]*>", RegexOptions.IgnoreCase | RegexOptions.Compiled);

    private static readonly Regex NameAttributePattern = new Regex(
        @"\bname\s*=\s*(?:""([^""]*)""|'([^']*)')", RegexOptions.IgnoreCase | RegexOptions.Compiled);

    /// <summary>例句音频的键名特征（各家命名不同，这里只收实测见过的那几种）</summary>
    private static readonly Regex LdoceExampleKey = new Regex(@"^p\d+__", RegexOptions.IgnoreCase | RegexOptions.Compiled);

    /// <summary>
    /// 按出现顺序抠出全部音频引用（已去重，保留第一次出现的顺序）。
    /// 顺序有意义：认不出英/美时按"先出现的当前者"挑。
    /// </summary>
    internal static List<EntryAudioRef> Extract(string html)
    {
        var result = new List<EntryAudioRef>();
        if (string.IsNullOrEmpty(html)) return result;

        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        void Push(string rawKey, string foundBy)
        {
            var key = Normalize(rawKey);
            if (key.Length == 0) return;
            if (!seen.Add(key)) return;
            result.Add(new EntryAudioRef
            {
                Key = key,
                Accent = ClassifyAccent(key),
                Example = IsExampleKey(key),
                FoundBy = foundBy
            });
        }

        foreach (Match match in SchemePattern.Matches(html))
        {
            Push(match.Groups[1].Value, "href");
        }

        foreach (Match tag in AudioTagPattern.Matches(html))
        {
            var name = NameAttributePattern.Match(tag.Value);
            if (!name.Success) continue;
            Push(name.Groups[1].Success ? name.Groups[1].Value : name.Groups[2].Value, "audio-name");
        }

        return result;
    }

    /// <summary>去掉 scheme 前缀、URL 编码与前导斜杠，得到 mdd 里那个键名</summary>
    internal static string Normalize(string raw)
    {
        var key = (raw ?? string.Empty).Trim();
        if (key.Length == 0) return string.Empty;
        try { key = Uri.UnescapeDataString(key); } catch { /* 解不开就按原样用 */ }
        return key.TrimStart('\\', '/').Trim();
    }

    /// <summary>
    /// 猜口音。按分隔符切成 token 再比对，避免 "us" 这种短标记在长名字里误伤
    /// （`apple__gb_1` → apple / gb / 1）。
    /// </summary>
    internal static string ClassifyAccent(string key)
    {
        var tokens = Tokenize(key);
        foreach (var token in tokens)
        {
            switch (token)
            {
                case "gb":
                case "bre":
                case "brs":
                case "uk":
                case "brit":
                    return "uk";
                case "us":
                case "uss":
                case "ams":
                case "nam":
                case "ame":
                    return "us";
            }
        }
        return null;
    }

    /// <summary>
    /// 是不是例句音频。
    ///
    /// 例句也要能点着响（用户点的是那一条例句的喇叭），但**不能**参与统一发音按钮 ——
    /// 否则按钮念出来的是某个例句而不是这个词。
    /// </summary>
    internal static bool IsExampleKey(string key)
    {
        if (string.IsNullOrEmpty(key)) return false;
        if (LdoceExampleKey.IsMatch(key)) return true;
        foreach (var token in Tokenize(key))
        {
            switch (token)
            {
                case "gbs":
                case "uss":
                case "brs":
                case "ams":
                case "eps":
                case "exa":
                    return true;
            }
        }
        return false;
    }

    /// <summary>口音的中文说法（界面提示用）</summary>
    internal static string AccentLabel(string accent)
    {
        switch (accent)
        {
            case "uk": return "英音";
            case "us": return "美音";
            default: return "发音";
        }
    }

    private static List<string> Tokenize(string key)
    {
        var tokens = new List<string>();
        foreach (var piece in Regex.Split(key ?? string.Empty, @"[^0-9A-Za-z]+"))
        {
            if (piece.Length > 0) tokens.Add(piece.ToLowerInvariant());
        }
        return tokens;
    }
}
