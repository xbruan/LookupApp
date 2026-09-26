namespace Lookup.App
{
    using System;
    using System.IO;
    using System.Text;

    /// <summary>
    /// 自检那份报告的出口：**同时**写控制台与一个文件。
    ///
    /// ⚠️ 为什么必须落**文件**：壳是 `WinExe`，PowerShell 起一个 GUI 程序**根本不等它退出** ——
    ///    `$LASTEXITCODE` 拿到的是**上一条命令的残留**（第一版 检查 因此"一条断言都没跑却报通过"），
    ///    而 `AttachConsole` 写父进程控制台又绕不过 PowerShell 的管道、抓不住。
    ///    约定定死：**结论落文件、脚本读文件**；退出码用 `Start-Process -Wait -PassThru` 拿。
    /// </summary>
    internal sealed class Report
    {
        private readonly StreamWriter _file;

        internal Report(string path)
        {
            if (string.IsNullOrEmpty(path)) return;
            var dir = Path.GetDirectoryName(path);
            if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
            // 不带 BOM：这份报告要能被 PowerShell 的 Get-Content -Encoding UTF8 原样读回来
            _file = new StreamWriter(path, false, new UTF8Encoding(false));
            _file.AutoFlush = true;
        }

        internal void Line(string text)
        {
            // 没有控制台（WinExe 常态）时 Console.Out 是个空设备，写它不会抛
            try { Console.WriteLine(text); }
            catch (Exception) { /* 没有控制台就算了，文件那份还在 */ }
            if (_file != null) _file.WriteLine(text);
        }

        internal void Close()
        {
            if (_file == null) return;
            try { _file.Flush(); _file.Dispose(); }
            catch (Exception) { /* 关不掉也不该把一次已经跑完的自检判红 */ }
        }
    }
}
