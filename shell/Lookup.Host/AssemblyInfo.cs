using System.Runtime.CompilerServices;

/*
 * 宿主适配层的类型是 `internal`（与绑定那一层同一个理由：都是**壳内部**的东西），
 * 这里显式放行两个自己人： 要**直接驱动**虚拟主机来做，
 * Lookup.App 是壳本体。⚠️ 两者**必须**走同一个 `VirtualHost` / `Dispatch`，不许自己再写一份路由。
 */
[assembly: InternalsVisibleTo("WindowsDllTest")]
[assembly: InternalsVisibleTo("Lookup.App")]
