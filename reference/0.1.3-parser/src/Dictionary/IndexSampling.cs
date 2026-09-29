using System;

namespace Lookup.Dictionary;

/// <summary>
/// "在词典索引上取样本"的**纯策略**部分：该取哪几条词条、哪些键名值得拿去量。
///
/// 为什么单独一个零依赖文件（只有 System）：这两件都是纯函数，而用它们的
/// DictSampler 依赖解析器那一串，B 级诊断脚本链不动（tools/DictSampleProbe 只链它 + 解析器）。
/// 而这一层恰恰是最容易**悄悄**退化的：一旦"均匀跳"写成"从头往后扫"，
/// 表现只是"量出来的音量偏一点"，肉眼和人耳都看不出来。
///
/// ⚠️ 这里取样的单位是**词条**，不是词块 —— 见 <see cref="Slots"/>。
/// </summary>
internal static class IndexSampling
{
    /// <summary>一条候选的位置：第几块 + 块内第几条</summary>
    internal struct SampleSlot
    {
        internal int BlockIndex;
        internal int EntryIndex;
    }

    /// <summary>
    /// 按**整本书的词条序号**均匀撒点：把全书 N 条词条分成 maxScan 段，每段取该段开头那一条。
    ///
    /// 为什么是词条而不是词块（这里踩过一次，别再改回去）：
    /// 早先那版是"均匀挑词块"（每块一条），在一本只有 **1 个词块**的小词典上
    /// 只能给出 **1 个候选** —— 于是"量内置录音"实际只量了 1 条，
    /// "取中位数"退化成 n=1，单条录音偶发偏轻/偏响就会把增益带偏
    /// （实测 testdata/audio.mdx 就是 1 块 9 条，只回了 1 条样本）。
    /// **块数少不等于候选少**：块只是存储单位，候选要的是"整本书里均匀分布的若干条词条"。
    ///
    /// 返回的下标一定落在各自块内；块数比 maxScan 少时，多条候选自然落进同一个块
    /// （块内容会被解析器缓存住，所以不额外花钱）。
    ///
    /// 谁在用：今天**没有产品流程**（「平衡音量」已经不再量内置录音，
    /// 约定见 App.DictSamplesAsync 的注释）；能力与测试都留着。
    /// </summary>
    internal static SampleSlot[] Slots(long[] blockEntryCounts, int maxScan)
    {
        if (maxScan <= 0 || blockEntryCounts == null || blockEntryCounts.Length == 0)
        {
            return new SampleSlot[0];
        }

        // 累计偏移：第 i 块的第一条词条在全书里的序号
        var offsets = new long[blockEntryCounts.Length];
        var total = 0L;
        for (var i = 0; i < blockEntryCounts.Length; i++)
        {
            offsets[i] = total;
            var count = blockEntryCounts[i];
            if (count > 0) total += count;
        }
        if (total <= 0) return new SampleSlot[0];

        // 全书没那么多词条时就一条一个，别空转
        var steps = (int)Math.Min(maxScan, total);
        var slots = new SampleSlot[steps];
        for (var i = 0; i < steps; i++)
        {
            var ordinal = i * total / steps;
            var block = LocateBlock(offsets, ordinal);
            var entry = (int)(ordinal - offsets[block]);
            var count = blockEntryCounts[block];
            if (entry >= count) entry = (int)count - 1;
            if (entry < 0) entry = 0;
            slots[i] = new SampleSlot { BlockIndex = block, EntryIndex = entry };
        }
        return slots;
    }

    /// <summary>序号落在哪一块：每块首条的序号是升序的，直接二分</summary>
    private static int LocateBlock(long[] offsets, long ordinal)
    {
        var low = 0;
        var high = offsets.Length - 1;
        var found = 0;
        while (low <= high)
        {
            var mid = low + (high - low) / 2;
            if (offsets[mid] <= ordinal)
            {
                found = mid;
                low = mid + 1;
            }
            else
            {
                high = mid - 1;
            }
        }
        return found;
    }

    /// <summary>索引里的键名去掉结尾的 \0（个别 v2.0 词典会带）与首尾空白</summary>
    internal static string CleanKey(string text)
    {
        return (text ?? string.Empty).Replace("\0", string.Empty).Trim();
    }

    /// <summary>
    /// "这条键值得拿去量录音吗"的粗筛。检查标准刻意**便宜且宽**：
    /// 以字母（含中日韩、带音标的拉丁字母）开头、长度 2..24、不含空白、
    /// 不含 @ \ / : &lt; &gt; "（@@@LINK、资源路径、命名空间那些不是词条名）。
    ///
    /// 宁可漏掉一些真词条，也不要把噪声当样本：取样只是"挑几条量音量"，
    /// 少一条不影响结论，而拿一条根本不是词条的东西去解正文只会白花时间。
    /// </summary>
    internal static bool LooksLikeHeadword(string text)
    {
        if (string.IsNullOrEmpty(text) || text.Length < 2 || text.Length > 24) return false;
        if (!char.IsLetter(text[0])) return false;

        var hasLetter = false;
        for (var i = 0; i < text.Length; i++)
        {
            var ch = text[i];
            if (char.IsWhiteSpace(ch)) return false;
            if (ch == '@' || ch == '\\' || ch == '/' || ch == ':' || ch == '<' || ch == '>' || ch == '"') return false;
            if (char.IsLetter(ch)) hasLetter = true;
        }
        return hasLetter;
    }
}
