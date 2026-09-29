using System;
using System.Collections.Generic;

namespace Lookup.Dictionary;

/// <summary>
/// 从一本**已打开的 .mdx** 里挑"像词条"的候选名 —— 真去读词块，但只要 <see cref="MdxFile"/>，
/// 不认识 StoredDictionary / 设置（那是 DictionaryEngine 的事）。
///
/// 为什么单独一个文件：`DictionaryEngine` 依赖 Models / 配置那一长串，
/// B 级诊断脚本链不动；而"取样到底取到哪几条"恰恰是必须拿**真词典文件**验的东西
/// （策略层只能验"该取哪些位置"，验不了"这些位置上真读得出词条名"）。
/// tools/DictSampleProbe 链的就是这个文件 + 解析器。
///
/// 挑选规则见 <see cref="IndexSampling"/>（均匀撒点 + 词条名粗筛）。
/// </summary>
internal static class DictSampler
{
    /// <summary>
    /// 均匀取出至多 maxScan 个候选词条名（去重、保序）。
    /// 取不满只可能是因为"这些位置上都没有像词条的键"或"块读不出来"，而不是"词条不够"
    /// —— 小词典（哪怕只有 1 个词块）也照样能取满 min(maxScan, 词条总数) 个。
    /// </summary>
    internal static List<string> SampleKeys(MdxFile mdx, int maxScan)
    {
        var result = new List<string>();
        if (mdx == null || maxScan <= 0) return result;

        var core = mdx.Core;
        var counts = BlockEntryCounts(mdx);
        if (counts.Length == 0) return result;

        var seen = new HashSet<string>(StringComparer.Ordinal);
        foreach (var slot in IndexSampling.Slots(counts, maxScan))
        {
            KeyEntry[] entries;
            try { entries = core.GetKeyBlock(slot.BlockIndex); }
            catch (Exception err)
            {
                Console.Error.WriteLine("[dict] 取样读词块失败（第 " + slot.BlockIndex + " 块）: " + err.Message);
                continue;
            }
            if (entries == null || entries.Length == 0) continue;

            var start = slot.EntryIndex < entries.Length ? slot.EntryIndex : entries.Length - 1;
            var name = FindHeadword(entries, start);
            if (name != null && seen.Add(name)) result.Add(name);
        }
        return result;
    }

    /// <summary>
    /// 按**索引顺序**逐条列出"像词条"的名字（惰性、可中途 break）。
    ///
    /// 用途是取样那批的**兜底补扫**：均匀撒点是一张网，网眼之间可能正好漏掉
    /// "录音集中在某一段"的词典；这一步负责把剩下的地方按顺序补上。
    /// 惰性是刻意的 —— 大词典有几十万条，调用方靠时间预算随时 break，
    /// 所以这里**不能**先物化成一张全表。
    ///
    /// 谁在用：今天**没有产品流程**（「平衡音量」已经不再量内置录音，
    /// 约定见 App.DictSamplesAsync 的注释）；这个能力留着备用。
    /// </summary>
    internal static IEnumerable<string> EnumerateKeys(MdxFile mdx)
    {
        if (mdx == null) yield break;

        var core = mdx.Core;
        var infos = core.KeyBlockInfos;
        if (infos == null) yield break;

        var seen = new HashSet<string>(StringComparer.Ordinal);
        for (var block = 0; block < infos.Length; block++)
        {
            KeyEntry[] entries;
            try { entries = core.GetKeyBlock(block); }
            catch (Exception err)
            {
                // 坏块跳过整块：补扫是"尽力而为"，不该因为一块坏了把整趟掀翻
                Console.Error.WriteLine("[dict] 补扫读词块失败（第 " + block + " 块）: " + err.Message);
                continue;
            }
            if (entries == null) continue;

            foreach (var entry in entries)
            {
                var text = IndexSampling.CleanKey(entry.Text);
                if (!IndexSampling.LooksLikeHeadword(text)) continue;
                if (seen.Add(text)) yield return text;
            }
        }
    }

    /// <summary>每块有多少条词条（用于把"全书序号"映射回"第几块第几条"）</summary>
    private static long[] BlockEntryCounts(MdxFile mdx)
    {
        var infos = mdx.Core.KeyBlockInfos;
        if (infos == null || infos.Length == 0) return new long[0];

        var counts = new long[infos.Length];
        for (var i = 0; i < infos.Length; i++) counts[i] = infos[i] == null ? 0 : infos[i].EntryCount;
        return counts;
    }

    /// <summary>
    /// 从块内 start 开始找第一个"像词条"的名字（找不到就绕回块首，块内不会漏）。
    ///
    /// 为什么要绕：均匀撒点给出的位置可能正好落在符号、缩写、`@@@LINK=` 上
    /// （序数序里这类键与普通词条混在一起），原地往后找一条最近的可用名字即可 ——
    /// 采样位置因此只决定"扫到书的哪一段"，不决定"能不能取到名字"。
    /// </summary>
    private static string FindHeadword(KeyEntry[] entries, int start)
    {
        for (var offset = 0; offset < entries.Length; offset++)
        {
            var text = IndexSampling.CleanKey(entries[(start + offset) % entries.Length].Text);
            if (IndexSampling.LooksLikeHeadword(text)) return text;
        }
        return null;
    }
}
