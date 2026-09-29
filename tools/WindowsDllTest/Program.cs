namespace Lookup.Interop.TestHarness
{
    using System;

    /// <summary>
    /// D 级验收入口：WindowsDllTest &lt;testdata 目录&gt; &lt;临时配置目录&gt;，退出码 0 全过 / 1 有断言失败 / 2 用法错。
    /// 断言本体在 <see cref="Runner"/>：「怎么跑」（进程、参数、退出码）与「验什么」分开，往 harness 里加东西才不至于越改越难读。
    /// </summary>
    internal static class Program
    {
        private static int Main(string[] args)
        {
            Console.WriteLine("WindowsDllTest：真 DLL + 生成的绑定 + 宿主适配层（D 级验收）");
            if (args.Length < 2)
            {
                Console.Error.WriteLine("用法：WindowsDllTest <testdata 目录> <临时配置目录>");
                return 2;
            }
            return Runner.Run(args[0], args[1]);
        }
    }
}
