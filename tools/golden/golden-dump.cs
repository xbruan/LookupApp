// ============================================================================
// 标准答案文件生成脚本（**不是内核的一部分**，只用于对照测试）
//
//   powershell -File tools/make-golden.ps1       编它（链仓库内那份只读参考实现，net48）
//   golden-dump.exe <词典.mdx ...>                跑它（输出 JSON 到 stdout）
//
// 它把 0.1.3 的**参考实现**（C# 的 MdictCore，仓库内只读副本
// reference/0.1.3-parser/src/Dictionary/）对一本 .mdx 的全部输出，
// 规范化成一份确定的 JSON 写到 stdout。C 内核的对应诊断脚本产出同样的形状，
// 两边逐字节比对 —— 这才是"与参考实现对齐"的证据（不是"我们自己也解得开"）。
//
// 为什么把 MdictCore 的源码链进来、而不是调 0.1.3 那个编好的 exe：
//   · 那个 exe 是 GUI 程序，命令行接口不是为对照测试设计的；
//   · 链源码能让我逐字段取到内部实测结果（词块索引、每条记录的取值），
//     而这些正是"同一份数据的两个来源"最容易分叉的地方。
//
// ⚠️ 输出必须**确定**（deterministic）：键按词块序（不排序、不依赖 hash 遍历），
//    记录文本按原样（不 trim），警告按产生顺序。
// ============================================================================

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using Lookup.Dictionary;

internal static class GoldenDump
{
    private static string JsonEscape(string s)
    {
        if (s == null) return "null";
        var sb = new StringBuilder(s.Length + 8);
        sb.Append('"');
        foreach (char c in s)
        {
            switch (c)
            {
                case '"': sb.Append("\\\""); break;
                case '\\': sb.Append("\\\\"); break;
                case '\b': sb.Append("\\b"); break;
                case '\f': sb.Append("\\f"); break;
                case '\n': sb.Append("\\n"); break;
                case '\r': sb.Append("\\r"); break;
                case '\t': sb.Append("\\t"); break;
                default:
                    if (c < 0x20) sb.Append("\\u").Append(((int)c).ToString("x4", CultureInfo.InvariantCulture));
                    else sb.Append(c);
                    break;
            }
        }
        sb.Append('"');
        return sb.ToString();
    }

    private static int Main(string[] args)
    {
        if (args.Length == 0)
        {
            Console.Error.WriteLine("用法: golden-dump.exe <词典.mdx> [更多.mdx ...]");
            return 2;
        }

        var sb = new StringBuilder();
        sb.Append("{\"files\":[");
        bool firstFile = true;

        foreach (string path in args)
        {
            if (!firstFile) sb.Append(',');
            firstFile = false;

            MdxFile mdx;
            try
            {
                mdx = new MdxFile(path);
            }
            catch (Exception ex)
            {
                // 打不开也要如实记下来 —— 对照测试时"两边都打不开"也是一种一致
                sb.Append("{\"name\":").Append(JsonEscape(Path.GetFileName(path)));
                sb.Append(",\"open\":false,\"reason\":").Append(JsonEscape(ex.Message)).Append('}');
                continue;
            }

            using (mdx)
            {
                var core = mdx.Core;   // internal，但与它同程序集（源码一起编）
                sb.Append("{\"name\":").Append(JsonEscape(Path.GetFileName(path)));
                sb.Append(",\"open\":true");

                // ── 头信息（只放**与解析有关**的那几项；不含路径这种随环境变的）──
                sb.Append(",\"info\":{");
                sb.Append("\"isMdd\":").Append(core.IsMdd ? "true" : "false");
                sb.Append(",\"version\":").Append(JsonEscape(
                    double.IsNaN(core.Version) ? "NaN" : core.Version.ToString("0.0", CultureInfo.InvariantCulture)));
                sb.Append(",\"encrypted\":").Append(core.Encrypted.ToString(CultureInfo.InvariantCulture));
                sb.Append(",\"encoding\":").Append(JsonEscape(core.EffectiveEncoding));
                sb.Append(",\"numWidth\":").Append(core.NumWidth.ToString(CultureInfo.InvariantCulture));
                sb.Append(",\"title\":").Append(JsonEscape(mdx.Header.Title ?? ""));
                sb.Append(",\"keyCount\":").Append(core.KeyCount.ToString(CultureInfo.InvariantCulture));
                sb.Append(",\"keyBlockCount\":").Append(core.KeyBlockInfos.Length.ToString(CultureInfo.InvariantCulture));
                sb.Append(",\"blockOrderMonotone\":").Append(core.BlockOrderMonotone ? "true" : "false");
                sb.Append('}');

                // ── 词块索引：每块的条目数 + 首尾词 + 压缩/解压大小 ──────────
                sb.Append(",\"keyBlocks\":[");
                for (int i = 0; i < core.KeyBlockInfos.Length; i++)
                {
                    var kb = core.KeyBlockInfos[i];
                    if (i > 0) sb.Append(',');
                    sb.Append("{\"entryCount\":").Append(kb.EntryCount.ToString(CultureInfo.InvariantCulture));
                    sb.Append(",\"first\":").Append(JsonEscape(kb.FirstKeyTrimmed));
                    sb.Append(",\"last\":").Append(JsonEscape(kb.LastKeyTrimmed));
                    sb.Append(",\"packSize\":").Append(kb.PackSize.ToString(CultureInfo.InvariantCulture));
                    sb.Append(",\"unpackSize\":").Append(kb.UnpackSize.ToString(CultureInfo.InvariantCulture));
                    sb.Append('}');
                }
                sb.Append(']');

                // ── 全部键 + 记录（按词块序，不排序）────────────────────────
                sb.Append(",\"keys\":[");
                bool firstKey = true;
                int emitted = 0;
                for (int bi = 0; bi < core.KeyBlockInfos.Length; bi++)
                {
                    var entries = core.GetKeyBlock(bi);
                    for (int ei = 0; ei < entries.Length; ei++)
                    {
                        if (!firstKey) sb.Append(',');
                        firstKey = false;
                        sb.Append(JsonEscape(entries[ei].Text));
                        emitted++;
                    }
                }
                sb.Append(']');
                sb.Append(",\"emittedKeys\":").Append(emitted.ToString(CultureInfo.InvariantCulture));

                sb.Append(",\"records\":[");
                firstKey = true;
                for (int bi = 0; bi < core.KeyBlockInfos.Length; bi++)
                {
                    var entries = core.GetKeyBlock(bi);
                    for (int ei = 0; ei < entries.Length; ei++)
                    {
                        if (!firstKey) sb.Append(',');
                        firstKey = false;
                        sb.Append(JsonEscape(core.GetDefinition(bi, ei)));
                    }
                }
                sb.Append(']');

                // ── 警告（实测结果不一致之类）：按产生顺序 ─────────────────────
                sb.Append(",\"warnings\":[");
                var warns = core.Warnings;
                for (int i = 0; i < warns.Count; i++)
                {
                    if (i > 0) sb.Append(',');
                    sb.Append(JsonEscape(warns[i]));
                }
                sb.Append(']');

                sb.Append('}');
            }
        }

        sb.Append("]}");
        Console.Out.Write(sb.ToString());
        Console.Out.Flush();
        return 0;
    }
}
