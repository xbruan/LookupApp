using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

namespace Lookup.Dictionary;

/// <summary>
/// 「一次性落点查询」的结果：只为一次判定而打开一本词典，用完就放掉。
/// 三态是刻意的 —— <see cref="Ok"/> 为假时**不能**被当成"这本词典里没有这个词"。
/// </summary>
internal sealed class ProbeOutcome
{
    /// <summary>查询本身跑完了（**与"命中了"是两回事**：Ok 为真而 Landed 为 null = 确实没有这个词）</summary>
    internal bool Ok;
    /// <summary>落点（词典的规范键名）；<see cref="Ok"/> 为真且它是 null 时才表示"这本里没有"</summary>
    internal string Landed;
    /// <summary>打不开 / 解析失败时的人话原因</summary>
    internal string Message;
}

/// <summary>
/// 给「在别的词典里查一次」这个按钮用的**轻查询**：打开 → 查一条 → 放掉。
///
/// ## 为什么需要它（这是用户 2026-09 报的那个体验问题的解法）
///
/// 空状态里那句「用《某某词典》查「某某」」要回答的问题是"**别的词典里有没有这个词**"，
/// 而此前唯一的办法是 `dict:resolve` → `EnsureLoadedAsync` → `LoadAsync`，
/// 那会把词典**永久留在 `_loaded` 里**。于是当时前端只能加一条 `status === 'ready'` 的过滤，
/// 结果就是：**只有当前词典被预热过，别的词典一律给不出这个按钮**
/// （用户报的现象：在用英汉词典时，汉英词典那本不会出现）。
///
/// 实测（`tools/…` 里那次一次性测量，三本真词典）：
///   · 只读索引打开一本 24 MB / 24 万词的词典约 41 ms（冷）/ 0 ms（系统页缓存命中）；
///   · 查一条键 0.4~6 ms；**整个过程不碰 `.mdd`**（哪怕那本挂着 2 GB 资源卷）；
///   · 释放后托管内存回到基线（0.3 → 2.3 → 1.1 MB，反复开合不累积）。
/// 所以"问一句"很便宜，**不需要**为了这个按钮把整个词库都加载起来。
///
/// ## 三条纪律
///
/// 1. **不进 `_loaded`、不建常驻缓存**：这里自己 new 一个 <see cref="MdxFile"/>，
///    它的词块/记录块缓存随它一起释放（`MdictCore` 的缓存是**每个实例一个**）。
/// 2. **判定与真跳同源**：复用同一个 <see cref="DictionaryEngine.Resolve"/>，
///    所以"这里说会落到 A、真跳过去却是 B"不可能发生。
/// 3. **失败不许冒充"没有"**：打不开、目录不在、文件损坏都回
///    <see cref="ProbeOutcome.Ok"/> = false + 人话原因；调用方要如实说"未能确认"。
///    （"不报错地把失败说成没有"是本仓库反复点名的坏模式。）
/// </summary>
internal static class DictionaryProbe
{
    /// <summary>
    /// 打开 <paramref name="stored"/> 这本词典、查一次 <paramref name="word"/> 的落点，然后放掉。
    ///
    /// **不开 `.mdd`**：资源卷与"这个词在不在"无关，而它们可能是几 GB
    /// （实测：给一本 24 MB 的词典配 2 GB 资源卷，开 mdd 与不开 mdd 对查词毫无影响）。
    /// </summary>
    internal static ProbeOutcome Resolve(DictionaryEngine engine, StoredDictionary stored, string word)
    {
        var outcome = new ProbeOutcome();
        var query = (word ?? string.Empty).Trim();
        if (query.Length == 0)
        {
            outcome.Message = "没有要查的词";
            return outcome;
        }

        MdxFile mdx = null;
        try
        {
            mdx = new MdxFile(stored.MdxPath);
            /*
             * 临时词典：只带 Mdx —— 判定这条路（Resolve）只用它。
             * Info 的标题走 `engine.TitleOf`（与界面**同一个来源**：头里的书名，读一次就缓存）——
             * 这里**不许写成 `stored.Title`**，那会让"没加载"这条路上的名字退回文件名。
             */
            var temp = new LoadedDictionary
            {
                Stored = stored,
                Info = new DictionaryInfo { Id = stored.Id, Title = engine.TitleOf(stored), MdxPath = stored.MdxPath },
                Mdx = mdx
            };
            var resolved = engine.Resolve(temp, query);
            outcome.Ok = true;
            outcome.Landed = resolved != null && resolved.Found ? resolved.KeyText : null;
            return outcome;
        }
        catch (Exception err)
        {
            outcome.Ok = false;
            outcome.Message = DescribeOpenFailure(stored, err);
            return outcome;
        }
        finally
        {
            try { mdx?.Dispose(); } catch { /* 释放失败不影响判定 */ }
        }
    }

    /// <summary>打开失败时的**人话**：这类原因用户能自己处理，别只说"出错了"</summary>
    private static string DescribeOpenFailure(StoredDictionary stored, Exception err)
    {
        var name = Path.GetFileName(stored.MdxPath ?? string.Empty);
        if (err is FileNotFoundException || err is DirectoryNotFoundException)
        {
            return "文件不在原来的位置了（" + name + "）";
        }
        if (err is UnauthorizedAccessException)
        {
            return "没有权限读这个文件（" + name + "）";
        }
        if (err is IOException)
        {
            return "读这个文件时出错（" + name + "）：" + err.Message;
        }
        return "打不开这本词典（" + name + "）：" + err.Message;
    }
}
