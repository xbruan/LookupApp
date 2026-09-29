using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Text;
using Lookup.Dictionary;

namespace Lookup.Tools.EntryAssets
{
    /// <summary>
    /// 从参考实现里抽出词条正文的"资产"与标准答案文件。用法：
    ///   EntryAssets.exe &lt;资产头文件&gt; &lt;标准答案文件头文件&gt;
    ///
    /// 两个产物：
    ///   · `native/src/dict/entry_assets.h`  —— `DSH_ENTRY_BASE_STYLE` / `DSH_ENTRY_BRIDGE_SCRIPT`
    ///   · `native/tests/entry_doc_vectors.h` —— 若干份完整文档（`Build` 的输出）当标准答案文件
    ///
    /// ⚠️ 两段字符串是从 `private const` 字段**反射读出来**的（`GetRawConstantValue`），
    ///    不是从源码里正则抠的 —— 抠字符串迟早会在某个转义符上出错，而反射拿到的是
    ///    编译器看到的那个值：**和运行时用的完全是同一份**。
    /// </summary>
    internal static class Program
    {
        private sealed class DocCase
        {
            internal string Label;
            internal string DictId;
            internal string Definition;
            internal string Notice;
            internal bool HasResources;
        }

        private static string ReadPrivateConst(string name)
        {
            var field = typeof(EntryDocument).GetField(name,
                BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Public);
            if (field == null) throw new InvalidOperationException("参考实现里找不到字段：" + name);
            var value = field.GetRawConstantValue() as string;
            if (value == null) throw new InvalidOperationException("字段不是字符串常量：" + name);
            return value;
        }

        private static int Main(string[] args)
        {
            if (args.Length < 2)
            {
                Console.Error.WriteLine("用法：EntryAssets.exe <资产头文件> <标准答案文件头文件>");
                return 2;
            }

            var style = ReadPrivateConst("BaseStyle");
            var script = ReadPrivateConst("BridgeScript");

            /* ── ① 资产头文件 ─────────────────────────────────────────────── */
            var a = new StringBuilder();
            a.Append("/* ==========================================================================\n");
            a.Append(" * 【生成，不许手改】词条正文的两份资产 —— 由 0.1.3 的参考实现产出。\n");
            a.Append(" *\n");
            a.Append(" * 生成：powershell -File tools/make-entry-assets.ps1\n");
            a.Append(" * 来源：reference/0.1.3-parser/src/Dictionary/EntryDocument.cs\n");
            a.Append(" *       （0.1.3 参考实现的仓库内只读副本）的两个 `private const string`\n");
            a.Append(" *       （`BaseStyle` 与 `BridgeScript`，走反射读出来，**一个字节都没改**）\n");
            a.Append(" *\n");
            a.Append(" * ⚠️ 七百多行的 CSS + JS，**绝不许手抄或手改**：抄错一个转义符的症状是\n");
            a.Append(" *    词条页莫名其妙地坏（桥接不生效、样式丢一半），最难查的一类。\n");
            a.Append(" *    要改它们，先改参考实现（那一份是只读副本、0.1.3 已冻结），\n");
            a.Append(" *    或者在本仓库里另立一份并说明为什么与参考实现不同。\n");
            a.Append(" * ========================================================================== */\n\n");
            a.Append("#ifndef DSH_ENTRY_ASSETS_H\n#define DSH_ENTRY_ASSETS_H\n\n");
            a.Append("/*\n");
            a.Append(" * ⚠️ 这两段字符串**超过了 C99 要求编译器至少支持的长度**（4095 字符）。\n");
            a.Append(" *    C99 §5.2.4.1 只保证 4095，而真实编译器（gcc / clang / MSVC）都远不止；\n");
            a.Append(" *    我们的构建开着 `-pedantic -Werror`，所以在这里**就地**把这个警告关掉 ——\n");
            a.Append(" *    只关这一个文件里的这一条，且包在 `__GNUC__` 判断里（不认识这个 pragma 的\n");
            a.Append(" *    编译器连看都不会看到它，不会因为 -Wunknown-pragmas 变成错误）。\n");
            a.Append(" *\n");
            a.Append(" * 为什么不改成字节数组：那样确实没有长度上限，但**没法 diff** ——\n");
            a.Append(" * 这两段是要跟参考实现逐字节对得上的东西，可读性就是可验证性。\n");
            a.Append(" */\n");
            a.Append("#if defined(__GNUC__)\n#pragma GCC diagnostic ignored \"-Woverlength-strings\"\n#endif\n\n");
            a.Append("/** 词条正文的基础样式（参考实现 `EntryDocument.BaseStyle`，").Append(style.Length).Append(" 字符）*/\n");
            a.Append("static const char DSH_ENTRY_BASE_STYLE[] =\n");
            EmitLiteral(a, style);
            a.Append(";\n\n");
            a.Append("/** 注入词条正文的桥接脚本（参考实现 `EntryDocument.BridgeScript`，").Append(script.Length).Append(" 字符）*/\n");
            a.Append("static const char DSH_ENTRY_BRIDGE_SCRIPT[] =\n");
            EmitLiteral(a, script);
            a.Append(";\n\n#endif /* DSH_ENTRY_ASSETS_H */\n");
            File.WriteAllText(args[0], a.ToString(), new UTF8Encoding(false));

            /* ── ② 标准答案文件（完整文档）─────────────────────────────────────── */
            var cases = new List<DocCase>
            {
                new DocCase { Label = "plain", DictId = "abc123", Definition = "<p>apple</p>",
                              Notice = null, HasResources = true },
                new DocCase { Label = "no-resources", DictId = "abc123", Definition = "<p>apple</p>",
                              Notice = null, HasResources = false },
                new DocCase { Label = "notice", DictId = "d1", Definition = "",
                              Notice = "<b>没有这个词</b>", HasResources = true },
                new DocCase { Label = "notice-and-def", DictId = "d1", Definition = "<div>x</div>",
                              Notice = "提示", HasResources = false },
                new DocCase { Label = "both-empty", DictId = "d2", Definition = null,
                              Notice = null, HasResources = false },
                new DocCase { Label = "hash-id", DictId = "539a599f3d30819489d19024504647d0c6874a692c97b3539a294a37c195672b",
                              Definition = "<div class=\"entry\"><span>苹果</span></div>",
                              Notice = null, HasResources = true },
                new DocCase { Label = "quotes-and-crlf", DictId = "d3",
                              Definition = "<a href=\"entry://apple\">apple</a>\r\n<div id='x'>\"引号\"</div>",
                              Notice = null, HasResources = true },
                new DocCase { Label = "definition-with-nul", DictId = "d4",
                              Definition = "a\0b", Notice = null, HasResources = true },
            };

            var v = new StringBuilder();
            v.Append("/* ==========================================================================\n");
            v.Append(" * 【生成，不许手改】词条正文的标准答案表 —— 由 0.1.3 的参考实现\n");
            v.Append(" * `EntryDocument.Build` 产出（仓库内只读副本 reference/0.1.3-parser/）。\n");
            v.Append(" *\n");
            v.Append(" * 生成：powershell -File tools/make-entry-assets.ps1\n");
            v.Append(" * 消费方：native/tests/test_entry_doc.c（逐字节对照测试）\n");
            v.Append(" *\n");
            v.Append(" * ⚠️ 每份文档**不是**整份抄在这里（那会是一兆的十六进制），而是拆成三段：\n");
            v.Append(" *      prefix + DSH_ENTRY_BASE_STYLE + mid + DSH_ENTRY_BRIDGE_SCRIPT + suffix\n");
            v.Append(" *    那两段资产在 native/src/dict/entry_assets.h 里（同一份参考实现产出）。\n");
            v.Append(" *    生成器**当场断言**过：参考实现那份完整文档里，款式与脚本各只出现一次，\n");
            v.Append(" *    而且按这个拆法能**逐字节reconstruct**回原样 —— 所以这里还带着整份文档的\n");
            v.Append(" *    SHA-256：测试那边拼完先核哈希，证明\"拼出来的就是参考实现那份\"，\n");
            v.Append(" *    再去比对 C 版。少了这一步，这套拆法就成了自说自话。\n");
            v.Append(" * ========================================================================== */\n\n");
            v.Append("#ifndef DSH_ENTRY_DOC_VECTORS_H\n#define DSH_ENTRY_DOC_VECTORS_H\n\n#include <stddef.h>\n#include <stdint.h>\n\n");
            v.Append("typedef struct {\n");
            v.Append("  const char *label;\n");
            v.Append("  const char *dict_id;\n");
            v.Append("  const unsigned char *definition;\n  size_t definition_len;\n");
            v.Append("  const unsigned char *notice;\n  size_t notice_len;\n");
            v.Append("  int has_resources;\n");
            v.Append("  const unsigned char *prefix;\n  size_t prefix_len;\n");
            v.Append("  const unsigned char *mid;\n  size_t mid_len;\n");
            v.Append("  const unsigned char *suffix;\n  size_t suffix_len;\n");
            v.Append("  const unsigned char *expected_sha256; /* 32 字节，参考实现那份完整文档的哈希 */\n");
            v.Append("} dsh_entry_doc_vector;\n\n");
            var rows = new List<string>();
            for (var i = 0; i < cases.Count; i++)
            {
                var c = cases[i];
                var html = EntryDocument.Build(new EntryDocumentOptions
                {
                    DictId = c.DictId,
                    Definition = c.Definition,
                    Notice = c.Notice,
                    HasResources = c.HasResources
                });
                /* ⚠️ 参考实现是 C# 字符串（UTF-16），这里一律按 **UTF-8 字节**处理 —— 与 C 侧一致 */
                var htmlBytes = Encoding.UTF8.GetBytes(html);
                var styleBytes = Encoding.UTF8.GetBytes(style);
                var scriptBytes = Encoding.UTF8.GetBytes(script);
                var atStyle = IndexOf(htmlBytes, styleBytes);
                var atScript = IndexOf(htmlBytes, scriptBytes);
                if (atStyle < 0 || atScript < 0 || atStyle == IndexOf(htmlBytes, styleBytes, atStyle + 1) ||
                    atScript == IndexOf(htmlBytes, scriptBytes, atScript + 1))
                {
                    throw new InvalidOperationException(
                        "「" + c.Label + "」那份文档里款式或脚本不是恰好出现一次 —— 拆法不成立，" +
                        "生成器的说明要重新核对（别照旧写）。");
                }
                if (atStyle > atScript)
                {
                    throw new InvalidOperationException("「" + c.Label + "」里款式出现在脚本之后，拆法不成立");
                }
                var prefix = Slice(htmlBytes, 0, atStyle);
                var mid = Slice(htmlBytes, atStyle + styleBytes.Length, atScript);
                var suffix = Slice(htmlBytes, atScript + scriptBytes.Length, htmlBytes.Length);
                /* 自证：按这个拆法拼回去必须与原样逐字节相同 */
                var rebuilt = new List<byte>();
                rebuilt.AddRange(prefix);
                rebuilt.AddRange(styleBytes);
                rebuilt.AddRange(mid);
                rebuilt.AddRange(scriptBytes);
                rebuilt.AddRange(suffix);
                var back = rebuilt.ToArray();
                if (back.Length != htmlBytes.Length)
                {
                    throw new InvalidOperationException("「" + c.Label + "」拆完拼回去长度不对");
                }
                for (var k = 0; k < back.Length; k++)
                {
                    if (back[k] != htmlBytes[k])
                    {
                        throw new InvalidOperationException("「" + c.Label + "」拆完拼回去第 " + k + " 字节不一致");
                    }
                }
                var hash = System.Security.Cryptography.SHA256.Create().ComputeHash(htmlBytes);

                EmitLiteralAsBytes(v, "DOC_ID_" + i, Encoding.UTF8.GetBytes(c.DictId ?? string.Empty));
                EmitBytes(v, "DOC_DEF_" + i, Encoding.UTF8.GetBytes(c.Definition ?? string.Empty));
                EmitBytes(v, "DOC_NOTICE_" + i, Encoding.UTF8.GetBytes(c.Notice ?? string.Empty));
                EmitLiteralBytes(v, "DOC_PREFIX_" + i, prefix);
                EmitLiteralBytes(v, "DOC_MID_" + i, mid);
                EmitLiteralBytes(v, "DOC_SUFFIX_" + i, suffix);
                v.Append("static const unsigned char DOC_HASH_").Append(i).Append("[32] = {");
                for (var k = 0; k < hash.Length; k++)
                {
                    if (k > 0) v.Append(',');
                    v.Append("0x").Append(hash[k].ToString("X2"));
                }
                v.Append("};\n");
                rows.Add("  {\"" + c.Label + "\", \""
                         + (c.DictId ?? string.Empty) + "\", DOC_DEF_" + i + ", "
                         + Encoding.UTF8.GetByteCount(c.Definition ?? "") + ", DOC_NOTICE_" + i + ", "
                         + Encoding.UTF8.GetByteCount(c.Notice ?? "") + ", "
                         + (c.HasResources ? 1 : 0) + ",\n   DOC_PREFIX_" + i + ", " + prefix.Length
                         + ", DOC_MID_" + i + ", " + mid.Length + ", DOC_SUFFIX_" + i + ", "
                         + suffix.Length + ", DOC_HASH_" + i + "}");
            }
            v.Append("static const dsh_entry_doc_vector ENTRY_DOC_VECTORS[] = {\n");
            v.Append(string.Join(",\n", rows));
            v.Append("\n};\n");
            v.Append("#define ENTRY_DOC_VECTOR_COUNT ").Append(cases.Count).Append("\n\n");
            v.Append("#endif /* DSH_ENTRY_DOC_VECTORS_H */\n");
            File.WriteAllText(args[1], v.ToString(), new UTF8Encoding(false));

            Console.WriteLine("已写出 " + args[0] + "（BaseStyle " + style.Length + " 字符 / BridgeScript " +
                              script.Length + " 字符）");
            Console.WriteLine("已写出 " + args[1] + "（" + cases.Count + " 份完整文档）");
            /* 参考实现那句话也要照抄：两条路由的常量必须与内核里的一致 */
            Console.WriteLine("  OriginFor(\"abc123\") = " + EntryDocument.OriginFor("abc123"));
            Console.WriteLine("  EntryUrlFor(\"abc123\", \"apples\") = " +
                              EntryDocument.EntryUrlFor("abc123", "apples"));
            return 0;
        }

        /// <summary>把一段文本按**行**写成 C 字符串字面量（一行一个字面量，可读、可 diff）</summary>
        private static void EmitLiteral(StringBuilder sb, string text)
        {
            var lines = text.Replace("\r\n", "\n").Split('\n');
            for (var i = 0; i < lines.Length; i++)
            {
                sb.Append("  \"").Append(Escape(lines[i]));
                if (i < lines.Length - 1) sb.Append("\\n");
                sb.Append("\"");
                if (i < lines.Length - 1) sb.Append("\n");
            }
            sb.Append("\n");
        }

        private static string Escape(string line)
        {
            var sb = new StringBuilder(line.Length + 8);
            foreach (var ch in line)
            {
                switch (ch)
                {
                    case '\\': sb.Append("\\\\"); break;
                    case '"': sb.Append("\\\""); break;
                    case '\t': sb.Append("\\t"); break;
                    case '\r': sb.Append("\\r"); break;
                    default:
                        if (ch < 0x20 || ch == 0x7F) sb.Append("\\x").Append(((int)ch).ToString("X2"));
                        else sb.Append(ch);
                        break;
                }
            }
            return sb.ToString();
        }

        private static void EmitBytes(StringBuilder sb, string name, byte[] bytes)
        {
            sb.Append("static const unsigned char ").Append(name).Append("[] = {");
            for (var i = 0; i < bytes.Length; i++)
            {
                if (i > 0) sb.Append(',');
                sb.Append("0x").Append(bytes[i].ToString("X2"));
            }
            if (bytes.Length == 0) sb.Append("0x00");
            sb.Append("};\n");
        }

        /// <summary>短字符串（词典 id）用 C 字符串字面量就够 —— 但没有转义风险才行</summary>
        private static void EmitLiteralAsBytes(StringBuilder sb, string name, byte[] bytes)
        {
            sb.Append("static const char ").Append(name).Append("[] = ")
              .Append('"').Append(Escape(Encoding.UTF8.GetString(bytes))).Append("\";\n");
        }

        /// <summary>
        /// 三段模板用**转义过的 C 字符串字面量**（配一个显式长度）。
        ///
        /// ⚠️ 控制字符一律写成**三位八进制**（`\000`），不许用 `\x00` 这种十六进制转义：
        ///    十六进制转义是**贪婪**的，`"\x00b"` 会被读成 `\x00b` = 0x0B —— 一个字节之差，
        ///    而症状是"文档少了一个字符"，最难查。八进制最多吃三位，`"\000b"` 没有歧义。
        /// </summary>
        private static void EmitLiteralBytes(StringBuilder sb, string name, byte[] bytes)
        {
            sb.Append("static const unsigned char ").Append(name).Append("[] = \"")
              .Append(EscapeBytes(bytes)).Append("\";\n");
        }

        private static string EscapeBytes(byte[] bytes)
        {
            var sb = new StringBuilder(bytes.Length + 16);
            foreach (var b in bytes)
            {
                if (b == (byte)'\\') sb.Append("\\\\");
                else if (b == (byte)'"') sb.Append("\\\"");
                else if (b < 0x20 || b == 0x7F) sb.Append('\\').Append(Convert.ToString(b, 8).PadLeft(3, '0'));
                else if (b < 0x80) sb.Append((char)b);
                else
                {
                    /* 非 ASCII 字节按**八进制**写：源码里就不必依赖"文件是 UTF-8"这件事，
                     * 而且与参考实现给的那串字节逐字节相等（不经过任何编码转换）。 */
                    sb.Append('\\').Append(Convert.ToString(b, 8).PadLeft(3, '0'));
                }
            }
            return sb.ToString();
        }

        private static int IndexOf(byte[] haystack, byte[] needle, int from = 0)
        {
            for (var i = from; i + needle.Length <= haystack.Length; i++)
            {
                var hit = true;
                for (var k = 0; k < needle.Length; k++)
                {
                    if (haystack[i + k] != needle[k])
                    {
                        hit = false;
                        break;
                    }
                }
                if (hit) return i;
            }
            return -1;
        }

        private static byte[] Slice(byte[] bytes, int from, int to)
        {
            var outBytes = new byte[to - from];
            Array.Copy(bytes, from, outBytes, 0, to - from);
            return outBytes;
        }
    }
}
