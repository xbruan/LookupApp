using System;
using System.Collections.Generic;
using System.IO;

namespace Lookup.Dictionary;

/// <summary>
/// 「不带 .mdd 的词典」的资源回落：`.mdx` 同目录散放着 css / ttf / png，**没有 `.mdd`**。
///
/// ## 为什么需要它
///
/// 《新世纪汉英大词典》就是一个文件夹：`xsjhy20oct2.mdx` 旁边放着 `xsjhy20oct2.css`、
/// `Bookerly.ttf`、`xsjhy20oct2.png`……词条正文第一行就是**裸文件名**引用
/// `<link rel="stylesheet" href="xsjhy20oct2.css">`。而 <see cref="DictionaryEngine.Resource"/>
/// 原来第一句就是 `if (dict.Mdds.Count == 0) return null` —— 不带 .mdd 的词典一个 `.mdd` 都没有，
/// 于是在这里短路 → WebHost 回 404 → **CSS 全丢、排版全无**（只剩裸文字）。
///
/// ## ⚠️ 这不是"多支持一种文件"，而是**一次信任边界变更**
///
/// 词条是**外部内容**。《新世纪汉英大词典》那份目录里还有 `xsjhy20oct2.js`（13 KB）与
/// `xsjhylibrary.js`（5 KB），而来路无从核实。今天它们跑不起来，只是因为资源 404 了 ——
/// **CSP 本来就允许 `script-src https://*.dictres.invalid`**（见 `EntryDocument`），
/// 所以"让同目录散放的文件通"这件事会**顺带把第三方脚本也打开**。
///
/// 那必须是一次**有意识的决定**，不能是副产品。所以这里只放行**被动资源**：
/// 样式表、字体、图片；**`.js` / `.mjs` / `.html` / `.htm` 一律不在白名单里**，
/// 取到它们只会得到 404（与今天的表现一致）。理由：
///   · 排版的主体在 CSS 里，脚本大概只负责交互（浮球、折叠、页签）；
///   · 这个程序从来没有执行过词典里的脚本，那是一条独立的信任边界；
///   · iframe 是 `sandbox="allow-scripts"` 且**没有** `allow-same-origin`（opaque origin），
///     脚本能跑却拿不到同源权限，很容易跑出"一半生效"的怪状态 —— 比完全不跑更难查。
///
/// 真要用到脚本时**单独评估**，别在这里顺手加一行 —— `tools/ui-static-check.mjs`
/// 有一条检查标准钉着"白名单里不许出现 `.js`"。
///
/// ## 安全约定（照 `docs/不带mdd的词典与机器翻译开发指导.md` §A7）
///
/// 这是**唯一**一处"按界面给的路径去读本地文件"的地方，所以四道检查都要有：
///   ① 扩展名白名单；
///   ② 拒绝绝对路径 / 盘符 / UNC（`C:\…`、`\\server\share`）；
///   ③ 归一化后必须仍在词典目录内（挡住 `..\..\`）；
///   ④ **真实路径**兜底：路径上任何一段是重解析点（符号链接 / 目录联接）就拒绝 ——
///      否则 `词典目录\link\win.ini` 在字符串上"在目录内"，实际读的却是 `C:\Windows\win.ini`。
///      .NET Framework 4.8 没有 `ResolveLinkTarget`，所以用 `FileAttributes.ReparsePoint`
///      逐段检查（这是 net48 上的等价物）。
/// </summary>
internal static class SiblingResource
{
    /// <summary>
    /// 允许从 `.mdx` 同目录散放读取的扩展名。
    ///
    /// **刻意不含 `.js` / `.mjs` / `.html` / `.htm`** —— 见类型注释里那段信任边界说明。
    /// 也不含音频扩展名：同目录散放的音频该由发音那条路（`FindResourceKey`）负责，
    /// 那边有它自己的候选顺序与格式判断，不该从资源路由旁路进来。
    /// </summary>
    private static readonly HashSet<string> Allowed = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
    {
        ".css",
        ".ttf", ".otf", ".woff", ".woff2", ".eot",
        ".png", ".jpg", ".jpeg", ".gif", ".svg", ".webp", ".bmp", ".ico", ".avif"
    };

    /// <summary>这个扩展名允不允许当同目录散放的文件读（`tools/ui-static-check.mjs` 也盯着这条）</summary>
    internal static bool IsAllowedExtension(string path)
    {
        if (string.IsNullOrEmpty(path)) return false;
        var ext = Path.GetExtension(path);
        return !string.IsNullOrEmpty(ext) && Allowed.Contains(ext);
    }

    /// <summary>词典目录 = `.mdx` 所在目录（`.mdx` 路径为空时回 null）</summary>
    internal static string DirectoryOf(string mdxPath)
    {
        if (string.IsNullOrEmpty(mdxPath)) return null;
        try
        {
            var full = Path.GetFullPath(mdxPath);
            return Path.GetDirectoryName(full);
        }
        catch
        {
            return null;
        }
    }

    /// <summary>
    /// 把界面给的资源路径解析成"词典目录下的那个真实文件"；不合法 / 不存在一律回 null。
    ///
    /// 传入的是 URL 形式的相对路径（`xsjhy20oct2.css`、`images/a.png`，可能 URL 编码）。
    /// </summary>
    internal static string Locate(string mdxPath, string resourcePath)
    {
        var dir = DirectoryOf(mdxPath);
        if (dir == null || string.IsNullOrEmpty(resourcePath)) return null;

        string decoded = resourcePath;
        try { decoded = Uri.UnescapeDataString(resourcePath); } catch { /* 保留原样 */ }

        // 查询串 / 锚点不属于路径（界面偶尔会带上，例如某些词典写了 `a.png?v=2`）
        var cut = decoded.IndexOfAny(new[] { '?', '#' });
        if (cut >= 0) decoded = decoded.Substring(0, cut);

        // ① 扩展名白名单。
        //    ⚠️ **`.js` 永远不在这里面** —— 见类型注释里那段信任边界说明，别顺手加回来。
        if (!IsAllowedExtension(decoded)) return null;

        // 统一分隔符
        var normalized = decoded.Replace('\\', '/');

        /*
         * ② 绝对路径里"真的跑到别处去"的两种形态，一律拒绝：
         *    · UNC：`\\server\share\a.css`  （归一化之后是 `//server/...`）
         *    · 盘符：`C:\Windows\win.ini`
         * ⚠️ 但**根相对**（`/images/a.png`）不能拒 —— 那是词典里的常见写法，
         *    `<base>` 把它解析到词典根，语义就是"词典目录下的 images/a.png"，
         *    与 `.mdd` 那条路（`ResourceKeyCandidates` 会给前导 `\`）是同一个意思。
         *    所以下面只是把前导斜杠去掉、当作相对路径处理，安全性由 ③④ 兜住。
         */
        if (normalized.StartsWith("//", StringComparison.Ordinal)) return null;
        if (normalized.Length > 1 && normalized[1] == ':') return null;

        var relative = normalized.TrimStart('/');
        if (relative.Length == 0) return null;

        string candidate;
        try { candidate = Path.GetFullPath(Path.Combine(dir, relative.Replace('/', Path.DirectorySeparatorChar))); }
        catch { return null; }

        // ③ 归一化后必须仍在词典目录内（`..\..\..\Windows\win.ini` 在这里被挡下）
        if (!IsInsideDirectory(candidate, dir)) return null;

        // ④ 真实路径兜底：逐段拒绝重解析点（符号链接 / 目录联接）
        if (TouchesReparsePoint(dir, candidate)) return null;

        try { return File.Exists(candidate) ? candidate : null; }
        catch { return null; }
    }

    /// <summary>词典目录下有没有**任何**同目录散放的文件可当资源用（决定 `HasResources`）</summary>
    internal static bool HasAny(string mdxPath)
    {
        var dir = DirectoryOf(mdxPath);
        if (dir == null) return false;
        try
        {
            if (!Directory.Exists(dir)) return false;
            foreach (var file in Directory.EnumerateFiles(dir))
            {
                if (IsAllowedExtension(file)) return true;
            }
        }
        catch { /* 目录读不动就当没有 */ }
        return false;
    }

    /// <summary>
    /// `path` 是否真的落在 `dir` 里面。用 OrdinalIgnoreCase（Windows 路径不区分大小写），
    /// 并且要求分隔符边界对齐 —— 否则 `C:\词典2` 会被当成"在 `C:\词典` 里面"。
    /// </summary>
    private static bool IsInsideDirectory(string path, string dir)
    {
        var root = dir.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        if (path.Length <= root.Length) return false;
        if (!path.StartsWith(root, StringComparison.OrdinalIgnoreCase)) return false;
        var next = path[root.Length];
        return next == Path.DirectorySeparatorChar || next == Path.AltDirectorySeparatorChar;
    }

    /// <summary>
    /// 从词典目录往下逐段看有没有重解析点。
    ///
    /// 为什么非要有这一条：`词典目录\link\win.ini` 里的 `link` 若是指向 `C:\Windows` 的**目录联接**，
    /// 那么字符串比对完全合法（前缀确实是词典目录），`File.Exists` 也成立 —— 但读出来的是系统文件。
    /// 所以"在目录内"必须是**真实路径**意义上的，而不是字符串意义上的。
    /// .NET Framework 4.8 没有 `File.ResolveLinkTarget`，只能靠 `ReparsePoint` 属性逐段判。
    /// </summary>
    private static bool TouchesReparsePoint(string dir, string path)
    {
        try
        {
            var root = dir.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            // 词典目录自己（以及它的每一级祖先）不参与判断 —— 用户完全可能把词典放在一个链接目录里，
            // 那是他自己选的存放位置，不是别人塞进来的路径。只判"目录之下的那几段"。
            var rest = path.Substring(root.Length).TrimStart(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            var segments = rest.Split(new[] { Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar }, StringSplitOptions.RemoveEmptyEntries);

            var current = root;
            for (var i = 0; i < segments.Length; i++)
            {
                current = Path.Combine(current, segments[i]);
                var attributes = File.GetAttributes(current);
                if ((attributes & FileAttributes.ReparsePoint) != 0) return true;
            }
            return false;
        }
        catch
        {
            // 查不动属性（权限、竞态）就当作不安全 —— 这条路上宁可少一个资源，也不能多读一个文件
            return true;
        }
    }
}
