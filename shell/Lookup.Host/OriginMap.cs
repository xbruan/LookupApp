namespace Lookup.Host
{
    using Lookup.Interop;

    /// <summary>查词入口名（origin）→ 内核枚举：全仓库唯一的一张映射表，取值同内核的 dsh_origin。</summary>
    /// <remarks>两条调用路共用它，出错策略各自留在调用方（点号协议抛异常、页面协议落 input），别合并。</remarks>
    internal static class OriginMap
    {
        /// <summary>认得出就回 true 并给出枚举；认不出回 false，由调用方决定「抛」还是「落 input」。</summary>
        internal static bool Try(string name, out DshOrigin origin)
        {
            switch (name)
            {
                case "input":
                case "":
                case null:
                    origin = DshOrigin.Input;
                    return true;
                case "selection":
                    origin = DshOrigin.Selection;
                    return true;
                case "link":
                    origin = DshOrigin.Link;
                    return true;
                case "back":
                    origin = DshOrigin.Back;
                    return true;
                case "history":
                    origin = DshOrigin.History;
                    return true;
                default:
                    origin = DshOrigin.Input;
                    return false;
            }
        }
    }
}
