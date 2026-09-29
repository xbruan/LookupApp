using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using Lookup.Dictionary;

namespace Lookup.Tools.HtmlVectors
{
    /// <summary>
    /// 把 0.1.3 的 `HtmlUtils`（参考实现，**不随本仓库发布**）在一批输入上的
    /// 输出**冻成 C 头文件**，给 C 内核的 `dsh_html_*` 当标准答案文件。
    /// 用法：`HtmlVectors.exe <输出路径>`。
    ///
    /// 为什么输出的是 C 头文件而不是 JSON：这几个函数是**纯字符串进出**，表里直接嵌
    /// 字节数组最省事（不用在测试里再引一个 JSON 读取器），而且自动生成的文件可以直接
    /// 用 `gcc` 编、可以 diff —— 与 `tools/make-textcodec-tables.py` 同一个套路。
    ///
    /// ⚠️ 字节一律按 **UTF-8 的十六进制**写出来（`0x3C,0x62,…`），不写 C 字符串字面量：
    ///    输入里有引号、反斜杠、`\0`、中文 —— 拼字面量迟早会拼错一处，
    ///    而拼错的症状是"标准答案文件自己不对"，最难查。
    ///
    /// ⚠️ 参考实现在**孤立代理项**（`&#xD800;`）上会抛异常（`ConvertFromUtf32` 的
    ///    硬性要求）。这类输入被单独收进 `HTML_REF_THROWS_*` 表 —— 它们是**已知差别**
    ///    （C 版如实保留原文），不进正常对照测试表，免得生成脚本自己崩掉。
    /// </summary>
    internal static class Program
    {
        private sealed class Case
        {
            internal string Label;
            internal string Input;
            internal Case(string label, string input) { Label = label; Input = input; }
        }

        /// <summary>一张要生成的对照测试表：前缀 / C 函数名 / 参考实现里对应的函数</summary>
        private sealed class Table
        {
            internal string Prefix;
            internal string CFunction;
            internal string ReferenceName;
            internal List<Case> Cases;
            internal Func<string, string> Reference;
        }

        private static int Main(string[] args)
        {
            var outPath = args.Length > 0 ? args[0] : "html_vectors.h";

            var strip = new List<Case>
            {
                // ── 基本 ──────────────────────────────────────────────────────
                new Case("empty", ""),
                new Case("plain", "apple"),
                new Case("bold", "<b>apple</b>"),
                new Case("nested-tags", "<div><b><i>apple</i></b></div>"),
                new Case("only-spaces", "   "),
                new Case("newline-only", "\n\n\n"),
                new Case("tab-only", "\t\t"),
                new Case("empty-after-strip", "<div></div>"),
                new Case("unclosed-tag", "a<b c"),
                new Case("tag-with-quoted-gt", "<a title=\"a>b\">y</a>"),
                new Case("self-closing-img", "x<img src=\"a.png\"/>y"),
                new Case("comment", "a<!-- c -->b"),
                new Case("doctype", "<!doctype html><p>x</p>"),
                new Case("cdata-ish", "a<![CDATA[x]]>b"),

                // ── br 的四种写法 ─────────────────────────────────────────────
                new Case("br", "a<br>b"),
                new Case("br-slash", "a<br/>b"),
                new Case("br-space-slash", "a<br />b"),
                new Case("br-upper", "a<BR>b"),
                new Case("br-pair", "a<br><br>b"),
                new Case("br-then-block", "a<br></p>b"),

                // ── 块级结束标签 ─────────────────────────────────────────────
                new Case("block-p", "<p>a</p><p>b</p>"),
                new Case("block-div-li-tr", "<div>a</div><li>b</li><tr>c</tr>"),
                new Case("block-headings", "<h1>a</h1><h6>b</h6>"),
                new Case("block-section-article", "<section>a</section><article>b</article>"),
                new Case("span-not-block", "<span>a</span><span>b</span>"),
                new Case("h7-not-block", "<h7>a</h7>"),

                // ── script / style 整段删掉 ──────────────────────────────────
                new Case("script-removed", "a<script>var x = 1 < 2;</script>b"),
                new Case("style-removed", "a<style>.x{color:red}</style>b"),
                new Case("script-upper", "a<SCRIPT>1</SCRIPT>b"),
                new Case("script-attr", "<script type=\"text/javascript\">x</script>c"),
                new Case("script-attr-gt", "<script src=\"a>b\">x</script>c"),
                new Case("script-unclosed", "a<script>x"),
                new Case("script-multiline", "<script>\nvar a = 1;\nvar b = 2;\n</script>t"),
                new Case("script-nested-lt", "<script>a<b</script>c"),
                new Case("style-gt-in-content", "<style>a{content:'>'}</style>b"),
                new Case("style-and-script", "<style>a</style>x<script>b</script>y"),
                new Case("only-style", "<style>x</style>"),

                // ── 实体 ─────────────────────────────────────────────────────
                new Case("entity-named-all", "&amp;&lt;&gt;&quot;&apos;"),
                new Case("entity-nbsp", "a&nbsp;b"),
                new Case("entity-copy-reg", "&copy;&reg;&hellip;&mdash;&ndash;"),
                new Case("entity-quotes", "&ldquo;a&rdquo; &lsquo;b&rsquo;"),
                new Case("entity-dec", "&#65;&#97;&#x42;"),
                new Case("entity-cjk-hex", "&#x4e2d;&#x6587;"),
                new Case("entity-upper-hex", "&#X41;"),
                new Case("entity-unknown-named", "&foo; &amp"),
                new Case("entity-mixed-amp", "&amp;lt;b&gt;"),
                new Case("entity-semicolon-missing", "&#65 x"),
                new Case("entity-overflow", "&#xFFFFFFFF;"),
                new Case("entity-above-max", "&#x110000;"),
                new Case("entity-dec-lead-zero", "&#0000065;"),
                new Case("entity-nul", "a&#0;b"),
                new Case("entity-amp-only", "a & b"),
                new Case("entity-case-insensitive", "&AMP;&Nbsp;"),
                new Case("entity-attr-then-stripped", "<a href=\"?q=&amp;x\">y</a>"),
                new Case("entity-becomes-text", "&lt;p&gt;x&lt;/p&gt;"),
                new Case("entity-cp-max", "&#x10FFFF;"),
                new Case("entity-emoji", "&#x1F600;"),

                // ── 空白与换行收敛 ───────────────────────────────────────────
                new Case("space-run", "a    b\t\tc"),
                new Case("space-run-nbsp", "a\u00a0\u00a0b"),
                new Case("space-lead-trail", "   a   "),
                new Case("blank-run", "a\n\n\n\nb"),
                new Case("blank-two", "a\n\nb"),
                new Case("crlf", "a\r\nb\rc"),
                new Case("line-trim", "  a  \n   b   "),
                new Case("tags-across-lines", "<p>\n a \n</p>\n<p>b</p>"),
                new Case("ideographic-space", "\u3000a\u3000\n\u3000b"),
                new Case("nbsp-only-line", "a\n\u00a0\nb"),

                // ── 一条像真词条的 ───────────────────────────────────────────
                new Case("realistic-entry",
                    "<link rel=\"stylesheet\" href=\"oale8.css\">" +
                    "<div class=\"entry\"><span class=\"hw\">apple</span><br>" +
                    "<span class=\"pr\">\u02c8\u00e6pl</span><br>" +
                    "<div class=\"def\">n. \u82f9\u679c &amp; \u82f9\u679c\u6811</div>" +
                    "<script>void(0)</script></div>"),
            };

            // 只验实体还原那一条（同一批输入跑 `DecodeEntities`）
            var decode = new List<Case>
            {
                new Case("empty", ""),
                new Case("plain", "apple"),
                new Case("named-all", "&amp;&lt;&gt;&quot;&apos;"),
                new Case("nbsp", "a&nbsp;b"),
                new Case("dec-and-hex", "&#65;&#x42;"),
                new Case("cjk", "&#x4e2d;&#x6587;"),
                new Case("unknown", "&foo;&amp"),
                new Case("upper", "&AMP;&#X41;"),
                new Case("overflow", "&#xFFFFFFFF;"),
                new Case("above-max", "&#x110000;"),
                new Case("nul", "a&#0;b"),
                new Case("no-semicolon", "&#65 x"),
                new Case("mixed", "&amp;lt;b&gt;"),
                new Case("tags-untouched", "<b>&amp;</b>"),
            };

            var escape = new List<Case>
            {
                new Case("empty", ""),
                new Case("plain", "apple"),
                new Case("all-five", "&<>\"'"),
                new Case("mixed", "a & b <c> \"d\" 'e'"),
                new Case("cjk", "\u4e2d\u6587 & \u82f9\u679c"),
                new Case("script", "<script>x</script>"),
            };

            var inline = new List<Case>
            {
                new Case("empty", ""),
                new Case("plain", "apple"),
                new Case("lower", "a</script>b"),
                new Case("upper", "a</SCRIPT>b"),
                new Case("mixed-case", "a</ScRiPt"),
                new Case("prefix-of-longer", "a</scripts>b"),
                new Case("two", "</script></script>"),
                new Case("no-match", "a/b c<d"),
            };

            // 参考实现在这些输入上**抛异常**（孤立代理项）：单独一张表
            var throws = new List<Case>
            {
                new Case("lone-surrogate-high", "&#xD800;"),
                new Case("lone-surrogate-low", "&#xDFFF;"),
            };

            var sb = new StringBuilder();
            sb.Append("/* ==========================================================================\n");
            sb.Append(" * 【生成，不许手改】HTML 标准答案表 —— 由 0.1.3 的参考实现产出。\n");
            sb.Append(" *\n");
            sb.Append(" * 生成：powershell -File tools/make-html-vectors.ps1\n");
            sb.Append(" * 来源：0.1.3 的参考实现（C#）里 `HtmlUtils.cs`（那份参考实现**不随本仓库\n");
            sb.Append(" * 发布**；接回来的步骤见 `tools/make-html-vectors.ps1` 找不到它时给出的提示）\n");
            sb.Append(" *\n");
            sb.Append(" * 字节一律写成十六进制（不是 C 字符串字面量）：输入里有引号、反斜杠、\n");
            sb.Append(" * 内嵌 U+0000 与中文，拼字面量迟早会拼错一处 —— 而拼错的症状是\n");
            sb.Append(" * \"标准答案文件自己不对\"，最难查。\n");
            sb.Append(" * ========================================================================== */\n\n");
            sb.Append("#ifndef DSH_HTML_VECTORS_H\n#define DSH_HTML_VECTORS_H\n\n#include <stddef.h>\n\n");
            sb.Append("typedef struct {\n");
            sb.Append("  const char *label;\n  const unsigned char *in;\n  size_t in_len;\n");
            sb.Append("  const unsigned char *out;\n  size_t out_len;\n} dsh_html_vector;\n\n");
            sb.Append("/** 只有输入的那种（参考实现会抛异常的那些）*/\n");
            sb.Append("typedef struct {\n  const char *label;\n  const unsigned char *in;\n");
            sb.Append("  size_t in_len;\n} dsh_html_input;\n\n");

            var tables = new List<Table>
            {
                new Table { Prefix = "STRIP", CFunction = "dsh_html_strip",
                            ReferenceName = "StripHtml", Cases = strip,
                            Reference = HtmlUtils.StripHtml },
                new Table { Prefix = "DECODE", CFunction = "dsh_html_decode_entities",
                            ReferenceName = "DecodeEntities", Cases = decode,
                            Reference = HtmlUtils.DecodeEntities },
                new Table { Prefix = "ESCAPE", CFunction = "dsh_html_escape",
                            ReferenceName = "EscapeHtml", Cases = escape,
                            Reference = HtmlUtils.EscapeHtml },
                new Table { Prefix = "INLINE", CFunction = "dsh_html_escape_for_inline_script",
                            ReferenceName = "EscapeForInlineScript", Cases = inline,
                            Reference = HtmlUtils.EscapeForInlineScript },
            };

            var counts = new Dictionary<string, int>();
            foreach (var t in tables)
            {
                var prefix = t.Prefix;
                var reference = t.Reference;
                sb.Append("/* ").Append(t.CFunction).Append(" —— 参考实现：")
                  .Append(t.ReferenceName).Append(" */\n");
                var labels = new List<string>();
                for (var i = 0; i < t.Cases.Count; i++)
                {
                    var c = t.Cases[i];
                    var got = reference(c.Input);
                    var inBytes = Encoding.UTF8.GetBytes(c.Input);
                    var outBytes = Encoding.UTF8.GetBytes(got ?? string.Empty);
                    Emit(sb, prefix + "_IN_" + i, inBytes);
                    Emit(sb, prefix + "_OUT_" + i, outBytes);
                    labels.Add("  {\"" + c.Label + "\", " + prefix + "_IN_" + i + ", " +
                               inBytes.Length + ", " + prefix + "_OUT_" + i + ", " +
                               outBytes.Length + "}");
                }
                sb.Append("static const dsh_html_vector HTML_").Append(prefix).Append("_VECTORS[] = {\n");
                sb.Append(string.Join(",\n", labels)).Append("\n};\n");
                sb.Append("#define HTML_").Append(prefix).Append("_VECTOR_COUNT ")
                  .Append(t.Cases.Count).Append("\n\n");
                counts[prefix] = t.Cases.Count;
            }

            // 参考实现会抛异常的那些输入：只记输入，C 版要"原样保留、不崩"
            sb.Append("/* ⚠️ 参考实现在这些输入上**抛异常**（`ConvertFromUtf32` 不收孤立代理项，\n");
            sb.Append(" *    而它没有被任何 try 接住）—— C 版如实保留原文。这一档**不进**对照测试表，\n");
            sb.Append(" *    只钉\"不崩、不吞\"。 */\n");
            var throwsLabels = new List<string>();
            for (var i = 0; i < throws.Count; i++)
            {
                var c = throws[i];
                string got;
                var threw = false;
                try { got = HtmlUtils.StripHtml(c.Input); }
                catch (Exception) { threw = true; got = null; }
                if (!threw)
                {
                    // 万一 .NET 的版本换了行为：如实报出来，**不许**悄悄改约定
                    throw new InvalidOperationException(
                        "参考实现在 `" + c.Label + "` 上竟然没抛异常（拿到 \"" + got + "\"）—— " +
                        "生成器里那条\"已知差别\"的说明要重新核对，别照旧写。");
                }
                var inBytes = Encoding.UTF8.GetBytes(c.Input);
                Emit(sb, "THROWS_IN_" + i, inBytes);
                throwsLabels.Add("  {\"" + c.Label + "\", THROWS_IN_" + i + ", " + inBytes.Length + "}");
            }
            sb.Append("static const dsh_html_input HTML_REF_THROWS[] = {\n");
            sb.Append(string.Join(",\n", throwsLabels)).Append("\n};\n");
            sb.Append("#define HTML_REF_THROWS_COUNT ").Append(throws.Count).Append("\n\n");
            sb.Append("#endif /* DSH_HTML_VECTORS_H */\n");

            File.WriteAllText(outPath, sb.ToString(), new UTF8Encoding(false));
            Console.WriteLine("已写出 " + outPath);
            foreach (var kv in counts)
            {
                Console.WriteLine("  " + kv.Key + "：" + kv.Value + " 组");
            }
            Console.WriteLine("  参考实现会抛异常：" + throws.Count + " 组（已知差别）");
            return 0;
        }

        private static void Emit(StringBuilder sb, string name, byte[] bytes)
        {
            sb.Append("static const unsigned char ").Append(name).Append("[] = {");
            for (var i = 0; i < bytes.Length; i++)
            {
                if (i > 0) sb.Append(',');
                sb.Append("0x").Append(bytes[i].ToString("X2"));
            }
            if (bytes.Length == 0) sb.Append("0x00"); /* C 里空数组不合法；长度那个字段说了算 */
            sb.Append("};\n");
        }
    }
}
