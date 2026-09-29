using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;

namespace Lookup.Dictionary;

/// <summary>极小的 HTML 文本工具，避免为了一点字符串处理引入依赖</summary>
internal static class HtmlUtils
{
    private static readonly Dictionary<string, string> NamedEntities = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
    {
        ["amp"] = "&", ["lt"] = "<", ["gt"] = ">", ["quot"] = "\"", ["apos"] = "'",
        ["nbsp"] = "\u00a0", ["copy"] = "©", ["reg"] = "®", ["hellip"] = "…",
        ["mdash"] = "—", ["ndash"] = "–", ["ldquo"] = "“", ["rdquo"] = "”",
        ["lsquo"] = "‘", ["rsquo"] = "’"
    };

    private static readonly Regex EntityPattern = new Regex(@"&(#x?[0-9a-fA-F]+|[a-zA-Z]+);", RegexOptions.Compiled);
    private static readonly Regex ScriptStylePattern = new Regex(@"<(script|style)\b[^>]*>[\s\S]*?</\1>", RegexOptions.IgnoreCase | RegexOptions.Compiled);
    private static readonly Regex BrPattern = new Regex(@"<br\s*/?>", RegexOptions.IgnoreCase | RegexOptions.Compiled);
    private static readonly Regex BlockEndPattern = new Regex(@"</(p|div|li|tr|h[1-6]|section|article)>", RegexOptions.IgnoreCase | RegexOptions.Compiled);
    private static readonly Regex TagPattern = new Regex(@"<[^>]+>", RegexOptions.Compiled);
    private static readonly Regex SpaceRunPattern = new Regex(@"[ \t\u00a0]+", RegexOptions.Compiled);
    private static readonly Regex BlankRunPattern = new Regex(@"\n{3,}", RegexOptions.Compiled);
    private static readonly Regex ScriptClosePattern = new Regex(@"</(script)", RegexOptions.IgnoreCase | RegexOptions.Compiled);

    internal static string DecodeEntities(string text)
    {
        if (string.IsNullOrEmpty(text)) return text;
        return EntityPattern.Replace(text, match =>
        {
            var body = match.Groups[1].Value;
            if (body.StartsWith("#x", StringComparison.OrdinalIgnoreCase))
            {
                if (int.TryParse(body.Substring(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var hex) &&
                    hex >= 0 && hex <= 0x10FFFF)
                {
                    return char.ConvertFromUtf32(hex);
                }
                return match.Value;
            }
            if (body.StartsWith("#", StringComparison.Ordinal))
            {
                if (int.TryParse(body.Substring(1), NumberStyles.Integer, CultureInfo.InvariantCulture, out var dec) &&
                    dec >= 0 && dec <= 0x10FFFF)
                {
                    return char.ConvertFromUtf32(dec);
                }
                return match.Value;
            }
            return NamedEntities.TryGetValue(body, out var named) ? named : match.Value;
        });
    }

    /// <summary>把词条 HTML 转成便于复制的纯文本</summary>
    internal static string StripHtml(string html)
    {
        if (string.IsNullOrEmpty(html)) return string.Empty;

        var text = ScriptStylePattern.Replace(html, string.Empty);
        text = BrPattern.Replace(text, "\n");
        text = BlockEndPattern.Replace(text, "\n");
        text = TagPattern.Replace(text, string.Empty);
        text = DecodeEntities(text);
        text = text.Replace("\r\n", "\n").Replace('\r', '\n');
        text = SpaceRunPattern.Replace(text, " ");
        text = BlankRunPattern.Replace(text, "\n\n");

        var lines = text.Split('\n');
        var builder = new StringBuilder(text.Length);
        for (var i = 0; i < lines.Length; i++)
        {
            if (i > 0) builder.Append('\n');
            builder.Append(lines[i].Trim());
        }
        return builder.ToString().Trim();
    }

    internal static string EscapeHtml(string text)
    {
        if (string.IsNullOrEmpty(text)) return string.Empty;
        var builder = new StringBuilder(text.Length + 16);
        foreach (var ch in text)
        {
            switch (ch)
            {
                case '&': builder.Append("&amp;"); break;
                case '<': builder.Append("&lt;"); break;
                case '>': builder.Append("&gt;"); break;
                case '"': builder.Append("&quot;"); break;
                case '\'': builder.Append("&#39;"); break;
                default: builder.Append(ch); break;
            }
        }
        return builder.ToString();
    }

    /// <summary>防止词典内容里的 `&lt;/script&gt;` 之类把注入脚本截断</summary>
    internal static string EscapeForInlineScript(string text)
    {
        return string.IsNullOrEmpty(text) ? text : ScriptClosePattern.Replace(text, "<\\/$1");
    }
}
