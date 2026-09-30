namespace Lookup.Host
{
    using System;
    using System.Collections.Generic;
    using System.Globalization;
    using System.IO;
    using System.Text;

    /// <summary>
    /// 外壳页面（HTML / CSS / JS / 字体图标）的取用入口 —— 由 `https://lookup.invalid/…` 那条虚拟站点用它。
    /// 与词典资源域（`*.dictres.invalid`，走内核）分开是**信任边界**：外壳的资源是我们自己发的、可以放行脚本，
    /// 词典的资源是外部内容、只放行被动资源（见 `dict/dsh_sibling.c` 顶上那段）。从根目录读而不嵌 exe：改前端不用重编 C#，能直接驱动这一层。
    /// </summary>
    internal sealed class ShellAssetSource
    {
        /// <summary>外壳资源的根目录；null = 这一版不提供壳资源（`lookup.invalid` 一律 404）</summary>
        internal string Root;

        /// <summary>路径是空的时候给哪一份文档（浏览器地址栏只打域名时）</summary>
        internal const string DefaultDocument = "floating.html";

        /// <summary>浏览器每次都会来要它 —— 我们没提供，静静回 404，别刷日志</summary>
        internal const string FaviconPath = "favicon.ico";

        internal static ShellAssetSource FromDirectory(string root)
        {
            return new ShellAssetSource { Root = string.IsNullOrEmpty(root) ? null : root };
        }

        private static readonly Dictionary<string, string> MimeMap =
            new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
        {
            [".html"] = "text/html; charset=utf-8",
            [".css"] = "text/css; charset=utf-8",
            [".js"] = "text/javascript; charset=utf-8",
            [".mjs"] = "text/javascript; charset=utf-8",
            [".json"] = "application/json; charset=utf-8",
            [".svg"] = "image/svg+xml",
            [".png"] = "image/png",
            [".jpg"] = "image/jpeg",
            [".ico"] = "image/x-icon",
            [".woff2"] = "font/woff2",
            [".ttf"] = "font/ttf"
        };

        /// <summary>扩展名 → MIME（与参考实现的 `ShellAssets.MimeFor` 同一张表）</summary>
        internal static string MimeFor(string path)
        {
            var dot = path == null ? -1 : path.LastIndexOf('.');
            var ext = dot >= 0 ? path.Substring(dot) : "";
            string mime;
            return MimeMap.TryGetValue(ext, out mime) ? mime : "application/octet-stream";
        }

        /// <summary>
        /// 取一份外壳资源。**四道 检查 与同目录散放的文件那条路同约定**（那是本仓库唯一「按界面给的路径去读本地文件」的地方）：
        /// ① 拒绝对路径 / 盘符 / UNC；② 归一化之后必须仍在根目录内（挡住 `..`）；③ 逐段拒绝重解析点；④ 必须是普通文件。
        /// ⚠️ 与那条路**不同**的一点：这里扩展名**不设白名单** —— 外壳自己的前端就要 `.js`（两个不同的信任主体，见类顶上那段）。
        /// </summary>
        internal VirtualResponse Serve(string path)
        {
            var res = new VirtualResponse();
            if (string.IsNullOrEmpty(Root))
            {
                res.Status = 404;
                res.ReasonPhrase = "Not Found";
                res.Reason = "这一版没有配置外壳资源目录（lookup.invalid 上没有东西可发）";
                return res;
            }

            var clean = (path ?? "").Replace('\\', '/').TrimStart('/');
            if (clean.Length == 0) clean = DefaultDocument;

            // favicon：浏览器每次都会来要，静静 404（别刷日志、也别当错误）
            if (string.Equals(clean, FaviconPath, StringComparison.OrdinalIgnoreCase))
            {
                res.Status = 404;
                res.ReasonPhrase = "Not Found";
                res.Reason = "没有提供 favicon";
                return res;
            }

            // ① 绝对路径 / 盘符 / UNC
            if (clean.StartsWith("//", StringComparison.Ordinal))
            {
                return Deny(res, "外壳资源的路径不合法（UNC）");
            }
            if (clean.Length > 1 && clean[1] == ':') return Deny(res, "外壳资源的路径不合法（盘符）");

            // ② 路径段里不许出现 `..`（**逐段判**：`a..b.css` 是合法文件名，包含式判断会把它一起挡掉）
            var segments = clean.Split('/');
            foreach (var seg in segments)
            {
                if (seg == "..") return Deny(res, "外壳资源的路径跑出根目录了");
            }

            string candidate;
            try
            {
                var rootFull = Path.GetFullPath(Root);
                candidate = Path.GetFullPath(Path.Combine(rootFull,
                    clean.Replace('/', Path.DirectorySeparatorChar)));
                if (!IsInside(rootFull, candidate)) return Deny(res, "外壳资源的路径跑出根目录了");
            }
            catch (Exception err)
            {
                return Deny(res, "外壳资源的路径不合法：" + err.Message);
            }

            // ③ 真实路径兜底：根目录**之下**的每一段都不许是重解析点
            if (TouchesReparsePoint(Path.GetFullPath(Root), candidate))
            {
                return Deny(res, "外壳资源的路径上有符号链接 / 目录联接");
            }

            // ④ 必须是普通文件
            if (!File.Exists(candidate))
            {
                res.Status = 404;
                res.ReasonPhrase = "Not Found";
                res.Reason = "外壳资源不存在：" + clean;
                return res;
            }

            try
            {
                res.Status = 200;
                res.ContentType = MimeFor(candidate);
                res.Body = File.ReadAllBytes(candidate);
                res.Total = res.Body.Length;
                // no-cache = 「每次都要来问一句」（外壳资源跟着 exe 走，用户升级以后本来就要重取）
                res.Headers.Add("Cache-Control: no-cache");
                res.Headers.Add("Access-Control-Allow-Origin: *");
            }
            catch (Exception err)
            {
                return Deny(res, "外壳资源读不出来：" + err.Message);
            }
            return res;
        }

        private static VirtualResponse Deny(VirtualResponse res, string why)
        {
            res.Status = 404;
            res.ReasonPhrase = "Not Found";
            res.Reason = why;
            return res;
        }

        /// <summary>`path` 是否真的落在 `dir` 里面（要求分隔符边界对齐）</summary>
        internal static bool IsInside(string dir, string path)
        {
            var root = dir.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            if (path.Length <= root.Length) return false;
            if (!path.StartsWith(root, StringComparison.OrdinalIgnoreCase)) return false;
            var next = path[root.Length];
            return next == Path.DirectorySeparatorChar || next == Path.AltDirectorySeparatorChar;
        }

        /// <summary>从根目录往下逐段看有没有重解析点（根目录自己与它的祖先不参与）</summary>
        private static bool TouchesReparsePoint(string dir, string path)
        {
            try
            {
                var root = dir.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
                var rest = path.Substring(root.Length)
                               .TrimStart(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
                var segments = rest.Split(new[] { Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar },
                                          StringSplitOptions.RemoveEmptyEntries);
                var current = root;
                foreach (var seg in segments)
                {
                    current = Path.Combine(current, seg);
                    var attrs = File.GetAttributes(current);
                    if ((attrs & FileAttributes.ReparsePoint) != 0) return true;
                }
                return false;
            }
            catch
            {
                // 查不动属性（权限、竞态）就当作不安全 —— 这条路上宁可少一个资源，也不能多读一个文件
                return true;
            }
        }

        /// <summary>路径里的 `%XX` 解码（外壳页面名一般用不到，但统一走一遍更省心）</summary>
        internal static string Decode(string path)
        {
            if (string.IsNullOrEmpty(path)) return "";
            try { return Uri.UnescapeDataString(path); }
            catch (Exception) { return path; }
        }

        internal string Describe()
        {
            return string.IsNullOrEmpty(Root) ? "（没有配置外壳资源目录）" : Root;
        }
    }
}
