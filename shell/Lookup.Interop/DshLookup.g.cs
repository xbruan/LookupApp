// ==========================================================================
// GENERATED — DO NOT EDIT.
// 由 tools/gen-bindings.mjs 从 abi/lookup.abi.json 生成（C# P/Invoke 绑定）。
// 壳只许通过这一层调内核：凡是内核也知道的常量，禁止在 C# 里再抄一份。
// ==========================================================================

using System;
using System.Runtime.InteropServices;
using System.Text;

namespace Lookup.Interop
{
    /// <summary>内核返回的错误码（0 = 成功）。取值与 native/include/dsh_lookup.h 一致。</summary>
    internal enum DshError
    {
        /// <summary>成功</summary>
        OK = 0,
        /// <summary>参数不合法（空指针 / 空词 / 越界）</summary>
        INVALID_ARG = -1,
        /// <summary>文件不存在或不是 .mdx</summary>
        NOT_FOUND = -2,
        /// <summary>能打开但不是合法 MDict</summary>
        FORMAT = -3,
        /// <summary>读写失败</summary>
        IO = -4,
        /// <summary>内存不足</summary>
        OOM = -5,
        /// <summary>句柄状态不对（未加载就查、已释放还再用）</summary>
        STATE = -6,
        /// <summary>同一句柄上有并发调用（内核多数对象不是线程安全的）</summary>
        BUSY = -7,
        /// <summary>这一版还没接（如实报，不许装作成功）</summary>
        NOT_IMPLEMENTED = -8,
    }

    /// <summary>一次查询从哪条入口来。只有 input / selection 允许跑兜底通道 —— link / back / history 一律不跑，否则『退回一个查不到的词』会当场跳走（参考实现那个坑D11）。</summary>
    internal enum DshOrigin
    {
        /// <summary>输入框回车 / 查找按钮</summary>
        Input = 0,
        /// <summary>正文里选中文字</summary>
        Selection = 1,
        /// <summary>词条里的 entry:// 链接（不跑通道）</summary>
        Link = 2,
        /// <summary>返回上一词条（不跑通道）</summary>
        Back = 3,
        /// <summary>从查词历史回放（不跑通道）</summary>
        History = 4,
    }

    /// <summary>通道走到哪一步了。界面据此显示进度；主进程每推进一步就回一个 stage。</summary>
    internal enum DshStage
    {
        /// <summary>还没开始</summary>
        Start = 0,
        /// <summary>在当前词典查过了</summary>
        AfterLookup = 1,
        /// <summary>问过别的词典了</summary>
        AfterProbe = 2,
        /// <summary>联想候选摆出来了</summary>
        AfterSuggest = 3,
        /// <summary>走到链的尽头了</summary>
        Done = 4,
    }

    /// <summary>候选摆在哪儿。</summary>
    internal enum DshSurface
    {
        /// <summary></summary>
        None = 0,
        /// <summary>输入框下方列表</summary>
        List = 1,
        /// <summary>正文框底部提示（选区那一路）</summary>
        Toast = 2,
    }

    /// <summary>词条页底部的出路按钮。**按钮上那行字也由内核给**（见 EntryPayload 的 chips[]）—— 文案是产品约定，不是视图。</summary>
    internal enum DshChipAction
    {
        /// <summary>用《X》查（借查，不切当前词典）。⚠️ 2026-09 起**这一档永不出现**：借查已经是通道里的自动一步，不该再让用户点一下去做同一件事（参考实现的 D1）。值留着不动枚举编号。</summary>
        Borrow = 0,
        /// <summary>再问一遍（有没问完的词典时必须给）</summary>
        Recheck = 1,
        /// <summary>翻译这个词</summary>
        Translate = 2,
    }

    /// <summary>主导字形。语种判定的第一步只看这个 ——『什么算汉字』只有一处来源。</summary>
    internal enum DshScript
    {
        /// <summary></summary>
        Han = 0,
        /// <summary></summary>
        Latin = 1,
        /// <summary></summary>
        Other = 2,
    }

    /// <summary>三层音源。排序由内核定（原录音 → 系统语音 → 在线），界面不许自己排。</summary>
    internal enum DshAudioSource
    {
        /// <summary>词典自带原录音（.mdd）</summary>
        Dict = 0,
        /// <summary>系统语音（离线合成）</summary>
        System = 1,
        /// <summary>在线发音（默认关）</summary>
        Online = 2,
    }

    /// <summary>
    /// 内核的原始 P/Invoke 声明。**不要直接调这些方法** —— 用下面的包装：
    /// 包装负责"内核给的内存必须还给内核"这件事（每次取字符串都在 finally 里 dsh_release）。
    /// </summary>
    internal static class DshRaw
    {
        internal const string Dll = "dsh_lookup.dll";

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern IntPtr dsh_version();
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_abi_version();
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern void dsh_release(IntPtr ptr);
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern IntPtr dsh_last_error_message();

        /// <summary>建一个引擎。userDataDir 是配置 / 缓存 / 索引的根目录（Windows 上通常是 %APPDATA%\LookupApp）。传 NULL 用内核的默认值。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_engine_create(IntPtr user_data_dir, out IntPtr out_engine);
        /// <summary>销毁引擎，释放它持有的一切（含所有已加载词典）。传 NULL 是合法的空操作。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_engine_destroy(IntPtr engine);
        /// <summary>取全部设置（JSON）。设置模型住内核里，界面不许自己拼一份、更不许拿内存里的设置回传给内核。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_engine_settings_get(IntPtr engine, out IntPtr out_json);
        /// <summary>改设置（局部补丁 JSON）。内核负责规范化（越界的语速、不认识的音色 ID、非法的枚举值都要被夹回合法范围），并负责落盘。返回规范化之后的完整设置。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_engine_settings_set(IntPtr engine, IntPtr patch_json, out IntPtr out_json);
        /// <summary>词库清单（JSON 数组）。每一项含 id / 显示名 / 路径 / 是否已加载 / 资源卷情况，以及 ★ `unavailable`（`""` / `"missing"`）与 `note`（**界面原样显示**的那句人话，可用时是空串）—— 「这一本现在还能不能用、不能时怎么恢复」由内核说（与 `dsh_history_query` 每一行**同一处实现**），界面不许自己拼那句话。「已被移出词库」那一档这里看不到（它压根不在清单里）：那一种由历史每一行的 `unavailable=removed` 说。id 是内容哈希，不是路径（0.2.0 的决定）——所以重命名文件、换目录都不会丢设置。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_engine_dict_list(IntPtr engine, out IntPtr out_json);
        /// <summary>导入若干 .mdx。paths_json 是 UTF-8 路径数组。返回 {added, failed:[{path,reason}]}。⚠️ 不存在的路径必须进 failed 并写明原因，绝不许记成 added（参考实现那个坑的形态）。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_engine_dict_add(IntPtr engine, IntPtr paths_json, out IntPtr out_json);
        /// <summary>从词库移除（不动硬盘文件）。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_engine_dict_remove(IntPtr engine, IntPtr dict_id, out IntPtr out_json);
        /// <summary>给词典起个别名。空串 = 恢复默认名。词典名的合成约定只在内核一处：改过的名 → .mdx 头里的书名 → 文件名。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_engine_dict_rename(IntPtr engine, IntPtr dict_id, IntPtr name, out IntPtr out_json);
        /// <summary>把一本词典在清单里挪 delta 位（负数往前、正数往后）。★ 2026-09 新加（ABI v2），**参考实现（参考实现）没有这个功能**，语义由本文件定义：到边界就**夹住**（第一本再往前 / 最后一本再往后 = 清单原样返回，仍算成功 —— 与界面上那两颗按钮在头尾置灰是一套）；delta 为 0 也是原样返回；id 不在清单里 → 报错（与 dict_rename 同一条）。当前词典**不受影响**（它记的是 id 不是下标）。⚠️ 顺序**不只是显示顺序**：「当前词典」那格为空时，查词 / 发音样本 / 增益等若干接口会**兜底取第一本**，所以把某一本挪到第一位会真的改变兜底行为。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_engine_dict_move(IntPtr engine, IntPtr dict_id, int delta, out IntPtr out_json);
        /// <summary>指定当前词典。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_engine_dict_set_current(IntPtr engine, IntPtr dict_id, out IntPtr out_json);
        /// <summary>问落点：这段文字在指定词典里会落到哪条词条。只查表、不落盘、不写历史、不常驻。用于『动手前先问落点』那种判定（参考实现那个坑：用字符串预测点下去的后果必错）。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_engine_resolve(IntPtr engine, IntPtr dict_id, IntPtr text, out IntPtr out_json);
        /// <summary>联想候选。三步由内核定：精确命中 → 前缀补全 → 拼写纠正；单字符查询不做拼写纠正（参考实现那个坑：编辑距离 ≤1 对单字符恒真）。行数上限由内核的 DSH_MAX_LIST_ROWS 定。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_engine_suggest(IntPtr engine, IntPtr text, out IntPtr out_json);
        /// <summary>完整查询（一条链走到底）：当前词典 → 借查别本 → 机器翻译 → 终态页。origin 决定跑不跑这条链。返回值里既有正文数据，也有界面要照抄的整句话与出路按钮。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_engine_lookup(IntPtr engine, IntPtr text, DshOrigin origin, IntPtr dict_id, out IntPtr out_json);
        /// <summary>问一句就走：在**别的**词典里查一次，答案拿回来，那本词典**不许**留在内存里（参考实现那个坑）。这条与『加载一本』必须分得开。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_engine_probe(IntPtr engine, IntPtr text, IntPtr dict_id, int budget_ms, out IntPtr out_json);
        /// <summary>借查：**去别的词典里问一遍**（终态页那条「再问一遍」）。参考实现是 `App.BorrowAsync`，这一份逐条照它的约定：① **跳过起点那一本**（入参为空 = 内核按『当前词典』取，『当前』空着时兜底第一本）；② 一本一本问、**问一句就走**（内部走 `dsh_engine_probe` → `dsh_dicts_borrow_once`，那本词典**不许**留在内存里）；③ **总预算**：自动那一次 300 ms（用户正等着看结果，不能一本一本串着问），`recheck` 时单本放大到 1500 ms、总预算 = 词长 × 1500 + 2000（参考实现同一公式 —— 用户刚点了按钮，他要的就是答案，可以等）；④ ★ **「没问完」绝不许并成「没有」**（那个坑）：预算用完 / `timeout` / `error` 的那几本进 `unconfirmed`（用**显示名**，界面直接列给人看），而 `missing`（这一本明确答没有）**不算没问完**。⚠️ 命中时**不加载**那一本，只回『在哪一本、落在哪条』—— 真正显示那一条由调用方带明确 dictId 再查一次（`dsh_engine_lookup`），参考实现也是这么分的。⚠️ 返回的 `asked`/`perDictMs`/`totalMs`/`elapsedMs` 是**实测结果**（诊断与排错要看它为什么没问完），界面不读。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_engine_borrow(IntPtr engine, IntPtr text, IntPtr skip_dict_id, int recheck, out IntPtr out_json);
        /// <summary>取一条词条的可渲染文档（HTML）与其资源。界面把它喂给沙箱 iframe。词条的 HTML 兼容性处理、.mdd 资源引用改写、sound:// 与 snd:// 的改写全在内核。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_engine_entry_document(IntPtr engine, IntPtr dict_id, IntPtr key_text, out IntPtr out_json);
        /// <summary>按 key 从 .mdd 资源卷（或词典旁边散放的文件）取一段资源字节。支持 Range：offset/length 让宿主实现 206/416。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_engine_resource(IntPtr engine, IntPtr dict_id, IntPtr key, UIntPtr offset, UIntPtr length, out IntPtr out_bytes, out UIntPtr out_len, out IntPtr out_meta_json);
        /// <summary>文本分析：字形（判语种第一步）、音节分隔点、去掉点之后的写法、按词计数。★ 这一条就是为了把 main.ts 里那三份重复实现收进内核（ADR-001）。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_text_analyze(IntPtr text, out IntPtr out_json);
        /// <summary>只做『去掉音节分隔点』这一件事。⚠️ 调用方必须遵守参考实现那个坑/38 的顺序：先拿原样文本问一次，问不到才用这里的返回值重问 —— 内核不替你改语义（ResolveKey 永远只认原样文本）。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_text_strip_separators(IntPtr text, out IntPtr out_text);
        /// <summary>判语种：字形 → 词典标题 → 用户设的默认语种。返回判定结果**与检查标准说明**（界面提示里要写『英语（按字形判断（拉丁字母））』，那句话由内核给，界面不拼）。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_language_detect(IntPtr engine, IntPtr text, IntPtr dict_id, out IntPtr out_json);
        /// <summary>语种码 → **中文名**（`"en"` → `"英语"`）。★ 这张表是**判定语种用的同一张表**（在 `text/dsh_language.c` 里），所以界面要显示语种名时**必须问内核**，不许在壳或界面里再存一份 —— 两处各存一份迟早会对不上（参考实现的注释专门点过这一条）。⚠️ 认不出的码回**空串**（不是拿码当名字），调用方自己决定怎么显示。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_language_label(IntPtr code, out IntPtr out_json);
        /// <summary>★ 豆包音色 id → **界面上的说法**（参考实现的 `DoubaoSpeech.DescribeSpeaker` + `DefaultSpeakerEnName/ZhName`）：两个默认音色回官网名 `Dacey` / `Vivi`，**其余一律回 id 本身**（不是空串 —— 界面上总得有个能认的东西）。为什么这条要进接口定义、而不是壳里再抄一张表：音色 id 与它的展示名**是同一份产品数据**，抄到壳里就是「同一个东西两个来源」（那个坑），迟早一处改了另一处没改。谁在用：`speech:status` 的两个 `speakerEnName`/`speakerZhName`、`speech:testDoubao` 每一项的 `speakerName`、在线发音回包里的 `voiceName`（`voiceId` 那格才是 id），以及内核自己拼的那句「为什么走这条」（`chosen.detail` / `why` 里显示的是名字，不是 `en_female_dacey_uranus_bigtts` 这种长串）。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_speech_speaker_label(IntPtr speaker, out IntPtr out_label);
        /// <summary>这次朗读怎么念：三层音源的排序与选择、挑哪个嗓子、要不要切段、切几段。★ 把 main.ts 的 speechPlan / splitForSpeech 收进内核（ADR-001）。顺序是**产品约定**（词典自带 → 在线 → 系统离线），界面不给选、也不许自己判。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_speech_plan(IntPtr engine, IntPtr text, IntPtr dict_id, IntPtr voices_json, IntPtr overrides_json, out IntPtr out_json);
        /// <summary>把一段 WAV 里的 16 位 PCM 按 dB 缩放（参考实现的 `GainMath.ApplyToWav`）。⚠️ 这是**系统离线语音那条路**的音量：参考实现就是在合成完、把字节交给播放器之前缩的（在『产字节』那一侧施加），所以那条路回包里的 `gainDb` **恒为 0** —— 前端再乘一次就是叠两遍。三条刻意的约定：① **顺序扫 RIFF 块**找 `fmt ` 与 `data`（不假设 44 字节头、也不假设只有一段 data —— SAPI 会插 `LIST`/`fact` 块，写死 44 会让增益『时灵时不灵』且错得隐蔽）；② **只动 16 位 PCM**，别的格式（8/24/32 位、浮点、压缩）**原样返回并说明**，不猜着改；③ **削顶不能不报错地** —— 想提的比峰值允许的多就夹回去，并把『从多少夹到多少、为什么』通过 `note` 说出去（悄悄削波听起来是破音，而用户没有任何线索去查）。⚙ 削顶保护的算法：`applied = min(请求值, 20·log10(1/峰值))`，再**向下**取整到 0.1 dB（宁可轻一点，也不要因为取整把峰值顶出满刻度）；**0 dB 一个字节都不动**（默认路径必须与『没有这个功能』逐字节相同）。⚠️ 传进来的 dB 是**已经归一化过**的（夹到 [-24,+12]、取整到 0.1 —— 那是设置层的活，见 `dsh_speech_gains`/`dsh_speech_gains_set`）。⚠️ 为什么这个参数是**整数**（dB×10）而不是浮点：接口定义里没有浮点标量类型，而增益在设置那一层本来就取整到 0.1 dB —— 传 dB×10 是**精确**的，也省掉一次跨 ABI 的浮点往返。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_audio_apply_gain(IntPtr bytes, UIntPtr len, int gain_tenths_db, out IntPtr out_bytes, out UIntPtr out_len, out IntPtr out_meta_json);
        /// <summary>这条词条自带的原录音在哪：按『常见扩展名优先、原始键垫底』在多卷 .mdd 里找，返回键名与可播放格式。例句录音**不算**词目发音（参考实现的硬约定）。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_speech_dict_audio(IntPtr engine, IntPtr dict_id, IntPtr key_text, out IntPtr out_json);
        /// <summary>从一本词典里挑几条**真的有录音**的词条（参考实现的 `App.DictSamplesAsync`）。⚙ 挑法逐条照参考实现：① **均匀撒网** —— 按**整本书的词条序号**分若干段、每段取开头那一条（⚠️ 单位是**词条**不是**词块**：按块取的话，一本只有 1 块的词典只能给出 1 个候选 —— 真词典 正是 1 块 9 条，实测踩过）；② 不够 6 条时**按索引顺序兜底补扫**（均匀撒点是一张网，网眼之间可能正好漏掉『录音集中在某一段』的词典）；③ 每条都**真解词条正文**去找录音，而且**只认词目发音**（例句不算 —— 量的是『点发音按钮会听到的那一段』，所以挑法与真正发音时完全一致：同一个 `dsh_speech_dict_audio`）；④ 三个上限：6 条 / 最多扫 60 个候选 / **最多 400 ms**（一次『解词条正文 + 到 .mdd 里找文件』实测十几毫秒，无上限地扫就是拿调用方的一次点击去跑后台任务）。⚠️ `ok=false` 时 message 是**三档不同的人话**：一本词典都没有 / 这本词典没有资源卷（.mdd）/ 有资源卷却扫不到（多半是音频卷没关联上）；扫不满 6 条但有一条以上时 `ok=true` 且 message 说清『只找到 N 条』。⚠️ 返回的是 **`audioKey`**（`.mdd` 里的键名），**可播地址由外壳拼**（`https://<外壳域>/__sound__/<词典 id>/<键名>`）—— 地址是平台形状，与 `dsh_speech_plan` 的 dict 那一层同一条规矩。⚠️ 今天**没有产品流程调它**（界面上的「平衡音量」按按需求删掉了，参考实现亦然）：接口、外壳接线与 B/标准都留着备用，**别看到『没人调』就删**。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_speech_dict_samples(IntPtr engine, IntPtr dict_id, out IntPtr out_json);
        /// <summary>在线语音（豆包 · 单向流式 HTTP）的**第一步：内核说该发什么**。三层音源里在线那一层的判断全在这儿：有没有配凭据与音色、这次该用哪个音色（**中英混排必须走中文音色** —— 实测拿英文音色念混排会得到空句子）、模型版本与音色配不配套（`seed-tts-2.0` / `seed-tts-1.0`）、请求体长什么样、四个头是什么。⚠️ 请求体里**刻意不写 `explicit_language`**：它的语义是"只念这个语种"，而词典正文中英混排是常态（见 docs/豆包语音合成接入方案.md 与 的实测）。⚠️ ok=false 时 reason 是人话，三种原因分开说（没填 Key / 没配音色 / 文本是空的）。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_speech_online_plan(IntPtr engine, IntPtr text, IntPtr dict_id, IntPtr overrides_json, out IntPtr out_json);
        /// <summary>在线语音的**检测凭据**那一条：管理窗「检测凭据」要发的那几次请求，该发什么。与 dsh_speech_online_plan 发的是**同一种东西**（同一份拼请求的代码），差别只在「念什么词、用哪个音色」由谁定 —— 这里由**界面正在填的那两个音色**定（用户改了 ID 还没写盘时，要测的必须是「即将存下去的那个」，测设置里存着的旧配置等于没测）。⚙ 逐项约定：① 给了 speaker 就只测它一项，语种用给的那个（认不出按 en），念的词从**内核自带的样本词表**取（`apple` / `苹果` …，与参考实现的 SampleWord 逐条相同）；② 没给 speaker 就**英文 + 中文各测一次**（两个音色的 Key / Resource 配套关系一样，但音色 ID 写错只有实测才发现）；③ 音色是空的 → 那一项 ok=false 且 reason 是「这个音色没填（这一项测不了）」——**「没东西可测」必须与「服务端说它不能用」分开说**，界面的判定（web/src/manager/main.ts 的 classifyDoubaoTest）就吃这一条；④ 没填 Key → 每一项的 reason 都是那句凭据提示。⚠️ 它**不受「在线总开关」限制**（先测通了再打开它，参考实现同一约定），但会**真联网**、按字符计费，所以只由用户点按钮触发。⚠️ 外壳在每一项上再补上 HTTP 的结果（statusCode/bytes/mime/elapsedMs/billedWords/error）与 speakerName —— 后者是**界面上的说法**（`Dacey` / `Vivi`；认不出就是 id 本身），由壳问 `dsh_speech_speaker_label` 得到（参考实现的 `DescribeSpeaker`），**壳里不许再抄一张表**（那个坑）。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_speech_online_test_plan(IntPtr engine, IntPtr speaker, IntPtr language, out IntPtr out_json);
        /// <summary>在线语音的**第二步：内核说回包是什么意思**。外壳把 plan 给的东西原样发出去，把 **HTTP 状态、整段回包正文、耗时**交回来 —— 解析 SSE 的 `data:` 行、把每段 base64 顺序拼成音频、错误码翻成人话，全在这里。⚠️ 回包是 **SSE（`data:` 行）**，所以外壳要**整段缓冲**再交进来，不许自己边收边解析。⚠️ **`code 0` 不等于成功**：实测"英文音色念中英混排"就是 code 0 + 零字节音频，所以"音频是空的"必须单独判成失败，且那句话要点出原因。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_speech_online_accept(IntPtr engine, IntPtr plan_json, int http_status, IntPtr response_body, int elapsed_ms, out IntPtr out_bytes, out UIntPtr out_len, out IntPtr out_json);
        /// <summary>音量增益的现状（界面那两条滑块的值）。★ **增益是"按词典"存的**（`speech.dictGainDb`：一本一个数）+ **全局一个**（`speech.systemGainDb`）—— 不同词典的录音本来就录得不一样响。归一化约定（夹到 [-24, +12] dB、取整到 0.1）在**写**那一条里做，这里只报现状。⚠️ `dictAvailable` / `dictMessage` / `systemAvailable` / `systemMessage` 是**判断 + 两句产品文案**：能不能校准、不能校准时缺什么，只有内核手里有检查标准（界面一个字都不拼）。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_speech_gains(IntPtr engine, IntPtr dict_id, int voice_count, out IntPtr out_json);
        /// <summary>改音量增益，返回改完之后的那份视图（调用方拿它重排滑块，不必再问一次）。⚠️ 两个键都是"**可空数字**"，三种语义必须分清：**传数字** = 设成这个值（会被夹到 [-24, +12] 并取整到 0.1）；**传 null** = 清掉这一项（回到"没设过"= 0 增益）；**整个键不传** = 不改（前端只发它要改的那一项）。`dictGainDb` 只改**那一本词典**的数（增益按词典存）。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_speech_gains_set(IntPtr engine, IntPtr dict_id, IntPtr patch_json, int voice_count, out IntPtr out_json);
        /// <summary>把一段音频字节整成播放器能播的格式：格式按魔数嗅探、Speex(.spx) 就地解成 16bit PCM WAV、认不出来就**当场如实报错**（不许让播放器报错误码、更不许拿 TTS 假装顶上）。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_audio_prepare(IntPtr bytes, UIntPtr len, out IntPtr out_bytes, out UIntPtr out_len, out IntPtr out_meta_json);
        /// <summary>查词历史（分页）。上限与每页条数由内核常量定。0.2.0 起历史落在 `<配置目录>/history.jsonl`（一行一条 JSON 的追加文件），界面只按页取、不许自己算分页。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_history_query(IntPtr engine, int offset, int limit, out IntPtr out_json);
        /// <summary>清空查词历史。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_history_clear(IntPtr engine, out IntPtr out_json);
        /// <summary>机器翻译的设置与凭据状态（**只读设置、不联网、不花钱**）。界面靠它决定「能不能显示翻译」以及「缺 Key 该怎么说明」。字段与参考实现的 TranslateStatus 逐字相同 —— 连 endpoint / resourceId 两个常量也一起给（排错时一眼看出请求被指到哪儿去了；resourceId 恒为 volc.speech.mt，它要在控制台**单独开通**，与语音的 seed-tts-2.0 不是一回事）。⚠️ `enabled`（用户自己的开关）与 `hasApiKey`（凭据有没有）**必须分开判**：界面把两者合成一个 disabled，就会把「我自己关的」与「客观不可用」混成同一种灰，用户会反复点那个开关。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_translate_status(IntPtr engine, out IntPtr out_json);
        /// <summary>机器翻译的**第一步：内核说该发什么**。内核零依赖、没有 socket，所以「把字节发出去」是平台层的活；但**判断一个字都不许留在平台层** —— 译成哪个语种、source 传不传、请求体长什么样、端点与三个头、这门语言支不支持，全在这里定。这一步的产物交给外壳原样发出去，回包再交给 dsh_translate_accept。⚠️ **命中缓存时不必发请求**：ok=true 且 cached=true，translation 就是译文。⚠️ ok=false 时 `message` 是人话，而且**四种原因分开说**（开关关着 / 没填 Key / 这门语言 MT 不支持 / 文本是空的）—— 合并成一句会让用户去改错的地方。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_translate_plan(IntPtr engine, IntPtr text, IntPtr dict_title, out IntPtr out_json);
        /// <summary>机器翻译的**第二步：内核说回包是什么意思**。外壳把 plan 给的东西原样发出去，然后把它拿回来的 **HTTP 状态、回包正文、耗时** 交回来 —— 解析译文、识别到的语种、计费 token、错误码翻成人话、写缓存全在这里做。⚠️ 错误码表**与 TTS 不是同一张**（这边成功是 20000000；TTS 那边同一个码含义不同），复用会把「成功」当「失败」。⚠️ **成功码但拿不到译文 → 不算成功**（宁可报错也不给一条空译文）。⚠️ 返回的是界面那份 TranslateResult（含 sourceLabel / targetLabel 两个中文名）—— 语言的中文名是产品文案，只许在内核这一处拼。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_translate_accept(IntPtr engine, IntPtr plan_json, int http_status, IntPtr response_body, int elapsed_ms, out IntPtr out_json);
        /// <summary>清空译文缓存（界面上那个「清空翻译缓存」）。返回 {"count":N}（清掉几条）。⚠️ 它是**必需项而不是优化**：MT 按 token 计费，而用户来回跳词条会把同一句翻很多次。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_translate_clear_cache(IntPtr engine, out IntPtr out_json);
        /// <summary>把一次翻译包成**正文框里那个词条的载荷**（参考实现的 `TranslatePayload` + `Annotate`）。用于查词通道自动翻译那一档：链说「该翻译」（`via=translate` + `needsTranslate=true`），外壳把请求发出去、把回包交给 `dsh_translate_accept`，再拿 accept 的结果与**伪词条地址**回来问这一条，得到一份与真词条**同形状**的载荷 —— 于是朗读、复制、返回栈一行都不用改就都生效（参考实现的原话：译文若另起一块 UI，这四件事就得各写第二遍）。⚠️ `entry_url` 由外壳给（浏览器地址里那个令牌是外壳的一次性表，内核不认识 socket 也不认识 URL 表），其余字段**一个字都不许在外壳拼**。⚠️ 翻译失败时（`ok=false`）这一步**不许**装作翻过了：回的是 `found=false` + `via=terminal` + 那句人话。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_translate_payload(IntPtr engine, IntPtr translate_json, IntPtr entry_url, out IntPtr out_json);
        /// <summary>打开一本 .mdx（解析层，不经引擎）。探测 / 逐字节对照 / 工具用得上；产品路径一律走引擎。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_dict_open(IntPtr path, out IntPtr out_dict);
        /// <summary>关掉一本词典。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_dict_close(IntPtr dict);
        /// <summary>词典头信息：书名、条目数、版本、加密标志、编码、资源卷命中的那几个。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_dict_info(IntPtr dict, out IntPtr out_json);
        /// <summary>词典里有没有这个键。⚠️ 只认精确命中（大小写变体算命中），**不联想** —— 这是 product 约定，不是解析层细节。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_dict_contains(IntPtr dict, IntPtr key, out IntPtr out_json);
        /// <summary>取一条记录的原始内容（未渲染）。@@@LINK 重定向在这里解开，并给出最终落点。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_dict_fetch(IntPtr dict, IntPtr key, out IntPtr out_json);
        /// <summary>枚举全部键（JSON 数组），按词典的序数序。逐字节对照与诊断的主力接口：拿它跟参考实现 / js-mdict 逐条比。</summary>
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]
        internal static extern int dsh_dict_keys(IntPtr dict, out IntPtr out_json);
    }

    /// <summary>
    /// 内核调用的包装层。内核与 Win32 一样，**从不抛异常**：
    /// 失败只体现为返回码。这里把失败翻译成 C# 异常，让壳的代码能正常用 try/catch；
    /// 需要"如实说原因"的场景（比如"这本词典为什么打不开"）请用 TryXxx 或读 LastError。
    /// </summary>
    internal static class Dsh
    {
        /// <summary>接口定义的 ABI 版本（常量）。壳启动时应当与内核报出来的对账。</summary>
        internal const int AbiVersion = 2;

        /// <summary>内核实际报出来的 ABI 版本；与 AbiVersion 不一致应当拒绝启动并给出可读原因。</summary>
        internal static int KernelAbiVersion() { return DshRaw.dsh_abi_version(); }

        /// <summary>本线程最近一次失败的人话说明（内核那边取回、这里已释放）。</summary>
        internal static string LastError()
        {
            IntPtr p = DshRaw.dsh_last_error_message();
            try { return PtrToString(p); } finally { DshRaw.dsh_release(p); }
        }

        /// <summary>内核版本字符串。</summary>
        internal static string Version()
        {
            IntPtr p = DshRaw.dsh_version();
            try { return PtrToString(p); } finally { DshRaw.dsh_release(p); }
        }

        /// <summary>把内核返回的 UTF-8 指针读成字符串。**调用方负责释放那个指针。**</summary>
        internal static string PtrToString(IntPtr p)
        {
            if (p == IntPtr.Zero) return null;
            int len = 0;
            while (Marshal.ReadByte(p, len) != 0) len++;
            byte[] buf = new byte[len];
            Marshal.Copy(p, buf, 0, len);
            return Encoding.UTF8.GetString(buf);
        }

        /// <summary>把 C# 字符串编成内核要的 UTF-8（以 \0 结尾）。</summary>
        internal static IntPtr StringToPtr(string s)
        {
            if (s == null) return IntPtr.Zero;
            byte[] buf = Encoding.UTF8.GetBytes(s);
            IntPtr p = Marshal.AllocHGlobal(buf.Length + 1);
            Marshal.Copy(buf, 0, p, buf.Length);
            Marshal.WriteByte(p, buf.Length, 0);
            return p;
        }

        /// <summary>失败时抛，带上内核给的人话原因。</summary>
        internal static void ThrowIfError(int rc)
        {
            if (rc == 0) return;
            string why = LastError();
            throw new InvalidOperationException(
                "内核返回 " + rc + "（" + (DshError)rc + "）" +
                (string.IsNullOrEmpty(why) ? "" : "：" + why));
        }

        // ── 带类型的包装 ────────────────────────────────────────────────
        // 为什么还要这一层：DshRaw 里全是 IntPtr，调用方每次都要自己把字符串编成
        // UTF-8、再保证释放 —— 那正是最容易漏的一步。包装把"编解码 + 所有权"
        // 收进库里一处，壳的代码里就只剩下业务。
        // 生成的规则：入参 utf8 → string；出参 json → out string（内核那份已释放）。

        /// <summary>建一个引擎。userDataDir 是配置 / 缓存 / 索引的根目录（Windows 上通常是 %APPDATA%\LookupApp）。传 NULL 用内核的默认值。</summary>
        internal static void EngineCreate(string user_data_dir, out IntPtr out_engine)
        {
            IntPtr user_data_dirPtr = StringToPtr(user_data_dir);
            try
            {
                int rc = DshRaw.dsh_engine_create(user_data_dirPtr, out out_engine);
                ThrowIfError(rc);
            }
            finally
            {
                Marshal.FreeHGlobal(user_data_dirPtr);
            }
        }

        /// <summary>销毁引擎，释放它持有的一切（含所有已加载词典）。传 NULL 是合法的空操作。</summary>
        internal static void EngineDestroy(IntPtr engine)
        {
            try
            {
                DshRaw.dsh_engine_destroy(engine);
            }
            finally
            {
            }
        }

        /// <summary>取全部设置（JSON）。设置模型住内核里，界面不许自己拼一份、更不许拿内存里的设置回传给内核。</summary>
        internal static void EngineSettingsGet(IntPtr engine, out string out_json)
        {
            IntPtr out_jsonPtr = IntPtr.Zero;
            out_json = null;
            try
            {
                int rc = DshRaw.dsh_engine_settings_get(engine, out out_jsonPtr);
                ThrowIfError(rc);
                out_json = PtrToString(out_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_jsonPtr);
                out_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                if (out_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_jsonPtr);
            }
        }

        /// <summary>改设置（局部补丁 JSON）。内核负责规范化（越界的语速、不认识的音色 ID、非法的枚举值都要被夹回合法范围），并负责落盘。返回规范化之后的完整设置。</summary>
        internal static void EngineSettingsSet(IntPtr engine, string patch_json, out string out_json)
        {
            IntPtr patch_jsonPtr = StringToPtr(patch_json);
            IntPtr out_jsonPtr = IntPtr.Zero;
            out_json = null;
            try
            {
                int rc = DshRaw.dsh_engine_settings_set(engine, patch_jsonPtr, out out_jsonPtr);
                ThrowIfError(rc);
                out_json = PtrToString(out_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_jsonPtr);
                out_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                Marshal.FreeHGlobal(patch_jsonPtr);
                if (out_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_jsonPtr);
            }
        }

        /// <summary>词库清单（JSON 数组）。每一项含 id / 显示名 / 路径 / 是否已加载 / 资源卷情况，以及 ★ `unavailable`（`""` / `"missing"`）与 `note`（**界面原样显示**的那句人话，可用时是空串）—— 「这一本现在还能不能用、不能时怎么恢复」由内核说（与 `dsh_history_query` 每一行**同一处实现**），界面不许自己拼那句话。「已被移出词库」那一档这里看不到（它压根不在清单里）：那一种由历史每一行的 `unavailable=removed` 说。id 是内容哈希，不是路径（0.2.0 的决定）——所以重命名文件、换目录都不会丢设置。</summary>
        internal static void EngineDictList(IntPtr engine, out string out_json)
        {
            IntPtr out_jsonPtr = IntPtr.Zero;
            out_json = null;
            try
            {
                int rc = DshRaw.dsh_engine_dict_list(engine, out out_jsonPtr);
                ThrowIfError(rc);
                out_json = PtrToString(out_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_jsonPtr);
                out_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                if (out_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_jsonPtr);
            }
        }

        /// <summary>导入若干 .mdx。paths_json 是 UTF-8 路径数组。返回 {added, failed:[{path,reason}]}。⚠️ 不存在的路径必须进 failed 并写明原因，绝不许记成 added（参考实现那个坑的形态）。</summary>
        internal static void EngineDictAdd(IntPtr engine, string paths_json, out string out_json)
        {
            IntPtr paths_jsonPtr = StringToPtr(paths_json);
            IntPtr out_jsonPtr = IntPtr.Zero;
            out_json = null;
            try
            {
                int rc = DshRaw.dsh_engine_dict_add(engine, paths_jsonPtr, out out_jsonPtr);
                ThrowIfError(rc);
                out_json = PtrToString(out_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_jsonPtr);
                out_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                Marshal.FreeHGlobal(paths_jsonPtr);
                if (out_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_jsonPtr);
            }
        }

        /// <summary>从词库移除（不动硬盘文件）。</summary>
        internal static void EngineDictRemove(IntPtr engine, string dict_id, out string out_json)
        {
            IntPtr dict_idPtr = StringToPtr(dict_id);
            IntPtr out_jsonPtr = IntPtr.Zero;
            out_json = null;
            try
            {
                int rc = DshRaw.dsh_engine_dict_remove(engine, dict_idPtr, out out_jsonPtr);
                ThrowIfError(rc);
                out_json = PtrToString(out_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_jsonPtr);
                out_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                Marshal.FreeHGlobal(dict_idPtr);
                if (out_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_jsonPtr);
            }
        }

        /// <summary>给词典起个别名。空串 = 恢复默认名。词典名的合成约定只在内核一处：改过的名 → .mdx 头里的书名 → 文件名。</summary>
        internal static void EngineDictRename(IntPtr engine, string dict_id, string name, out string out_json)
        {
            IntPtr dict_idPtr = StringToPtr(dict_id);
            IntPtr namePtr = StringToPtr(name);
            IntPtr out_jsonPtr = IntPtr.Zero;
            out_json = null;
            try
            {
                int rc = DshRaw.dsh_engine_dict_rename(engine, dict_idPtr, namePtr, out out_jsonPtr);
                ThrowIfError(rc);
                out_json = PtrToString(out_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_jsonPtr);
                out_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                Marshal.FreeHGlobal(dict_idPtr);
                Marshal.FreeHGlobal(namePtr);
                if (out_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_jsonPtr);
            }
        }

        /// <summary>把一本词典在清单里挪 delta 位（负数往前、正数往后）。★ 2026-09 新加（ABI v2），**参考实现（参考实现）没有这个功能**，语义由本文件定义：到边界就**夹住**（第一本再往前 / 最后一本再往后 = 清单原样返回，仍算成功 —— 与界面上那两颗按钮在头尾置灰是一套）；delta 为 0 也是原样返回；id 不在清单里 → 报错（与 dict_rename 同一条）。当前词典**不受影响**（它记的是 id 不是下标）。⚠️ 顺序**不只是显示顺序**：「当前词典」那格为空时，查词 / 发音样本 / 增益等若干接口会**兜底取第一本**，所以把某一本挪到第一位会真的改变兜底行为。</summary>
        internal static void EngineDictMove(IntPtr engine, string dict_id, int delta, out string out_json)
        {
            IntPtr dict_idPtr = StringToPtr(dict_id);
            IntPtr out_jsonPtr = IntPtr.Zero;
            out_json = null;
            try
            {
                int rc = DshRaw.dsh_engine_dict_move(engine, dict_idPtr, delta, out out_jsonPtr);
                ThrowIfError(rc);
                out_json = PtrToString(out_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_jsonPtr);
                out_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                Marshal.FreeHGlobal(dict_idPtr);
                if (out_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_jsonPtr);
            }
        }

        /// <summary>指定当前词典。</summary>
        internal static void EngineDictSetCurrent(IntPtr engine, string dict_id, out string out_json)
        {
            IntPtr dict_idPtr = StringToPtr(dict_id);
            IntPtr out_jsonPtr = IntPtr.Zero;
            out_json = null;
            try
            {
                int rc = DshRaw.dsh_engine_dict_set_current(engine, dict_idPtr, out out_jsonPtr);
                ThrowIfError(rc);
                out_json = PtrToString(out_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_jsonPtr);
                out_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                Marshal.FreeHGlobal(dict_idPtr);
                if (out_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_jsonPtr);
            }
        }

        /// <summary>问落点：这段文字在指定词典里会落到哪条词条。只查表、不落盘、不写历史、不常驻。用于『动手前先问落点』那种判定（参考实现那个坑：用字符串预测点下去的后果必错）。</summary>
        internal static void EngineResolve(IntPtr engine, string dict_id, string text, out string out_json)
        {
            IntPtr dict_idPtr = StringToPtr(dict_id);
            IntPtr textPtr = StringToPtr(text);
            IntPtr out_jsonPtr = IntPtr.Zero;
            out_json = null;
            try
            {
                int rc = DshRaw.dsh_engine_resolve(engine, dict_idPtr, textPtr, out out_jsonPtr);
                ThrowIfError(rc);
                out_json = PtrToString(out_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_jsonPtr);
                out_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                Marshal.FreeHGlobal(dict_idPtr);
                Marshal.FreeHGlobal(textPtr);
                if (out_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_jsonPtr);
            }
        }

        /// <summary>联想候选。三步由内核定：精确命中 → 前缀补全 → 拼写纠正；单字符查询不做拼写纠正（参考实现那个坑：编辑距离 ≤1 对单字符恒真）。行数上限由内核的 DSH_MAX_LIST_ROWS 定。</summary>
        internal static void EngineSuggest(IntPtr engine, string text, out string out_json)
        {
            IntPtr textPtr = StringToPtr(text);
            IntPtr out_jsonPtr = IntPtr.Zero;
            out_json = null;
            try
            {
                int rc = DshRaw.dsh_engine_suggest(engine, textPtr, out out_jsonPtr);
                ThrowIfError(rc);
                out_json = PtrToString(out_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_jsonPtr);
                out_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                Marshal.FreeHGlobal(textPtr);
                if (out_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_jsonPtr);
            }
        }

        /// <summary>完整查询（一条链走到底）：当前词典 → 借查别本 → 机器翻译 → 终态页。origin 决定跑不跑这条链。返回值里既有正文数据，也有界面要照抄的整句话与出路按钮。</summary>
        internal static void EngineLookup(IntPtr engine, string text, DshOrigin origin, string dict_id, out string out_json)
        {
            IntPtr textPtr = StringToPtr(text);
            IntPtr dict_idPtr = StringToPtr(dict_id);
            IntPtr out_jsonPtr = IntPtr.Zero;
            out_json = null;
            try
            {
                int rc = DshRaw.dsh_engine_lookup(engine, textPtr, origin, dict_idPtr, out out_jsonPtr);
                ThrowIfError(rc);
                out_json = PtrToString(out_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_jsonPtr);
                out_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                Marshal.FreeHGlobal(textPtr);
                Marshal.FreeHGlobal(dict_idPtr);
                if (out_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_jsonPtr);
            }
        }

        /// <summary>问一句就走：在**别的**词典里查一次，答案拿回来，那本词典**不许**留在内存里（参考实现那个坑）。这条与『加载一本』必须分得开。</summary>
        internal static void EngineProbe(IntPtr engine, string text, string dict_id, int budget_ms, out string out_json)
        {
            IntPtr textPtr = StringToPtr(text);
            IntPtr dict_idPtr = StringToPtr(dict_id);
            IntPtr out_jsonPtr = IntPtr.Zero;
            out_json = null;
            try
            {
                int rc = DshRaw.dsh_engine_probe(engine, textPtr, dict_idPtr, budget_ms, out out_jsonPtr);
                ThrowIfError(rc);
                out_json = PtrToString(out_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_jsonPtr);
                out_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                Marshal.FreeHGlobal(textPtr);
                Marshal.FreeHGlobal(dict_idPtr);
                if (out_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_jsonPtr);
            }
        }

        /// <summary>借查：**去别的词典里问一遍**（终态页那条「再问一遍」）。参考实现是 `App.BorrowAsync`，这一份逐条照它的约定：① **跳过起点那一本**（入参为空 = 内核按『当前词典』取，『当前』空着时兜底第一本）；② 一本一本问、**问一句就走**（内部走 `dsh_engine_probe` → `dsh_dicts_borrow_once`，那本词典**不许**留在内存里）；③ **总预算**：自动那一次 300 ms（用户正等着看结果，不能一本一本串着问），`recheck` 时单本放大到 1500 ms、总预算 = 词长 × 1500 + 2000（参考实现同一公式 —— 用户刚点了按钮，他要的就是答案，可以等）；④ ★ **「没问完」绝不许并成「没有」**（那个坑）：预算用完 / `timeout` / `error` 的那几本进 `unconfirmed`（用**显示名**，界面直接列给人看），而 `missing`（这一本明确答没有）**不算没问完**。⚠️ 命中时**不加载**那一本，只回『在哪一本、落在哪条』—— 真正显示那一条由调用方带明确 dictId 再查一次（`dsh_engine_lookup`），参考实现也是这么分的。⚠️ 返回的 `asked`/`perDictMs`/`totalMs`/`elapsedMs` 是**实测结果**（诊断与排错要看它为什么没问完），界面不读。</summary>
        internal static void EngineBorrow(IntPtr engine, string text, string skip_dict_id, int recheck, out string out_json)
        {
            IntPtr textPtr = StringToPtr(text);
            IntPtr skip_dict_idPtr = StringToPtr(skip_dict_id);
            IntPtr out_jsonPtr = IntPtr.Zero;
            out_json = null;
            try
            {
                int rc = DshRaw.dsh_engine_borrow(engine, textPtr, skip_dict_idPtr, recheck, out out_jsonPtr);
                ThrowIfError(rc);
                out_json = PtrToString(out_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_jsonPtr);
                out_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                Marshal.FreeHGlobal(textPtr);
                Marshal.FreeHGlobal(skip_dict_idPtr);
                if (out_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_jsonPtr);
            }
        }

        /// <summary>取一条词条的可渲染文档（HTML）与其资源。界面把它喂给沙箱 iframe。词条的 HTML 兼容性处理、.mdd 资源引用改写、sound:// 与 snd:// 的改写全在内核。</summary>
        internal static void EngineEntryDocument(IntPtr engine, string dict_id, string key_text, out string out_json)
        {
            IntPtr dict_idPtr = StringToPtr(dict_id);
            IntPtr key_textPtr = StringToPtr(key_text);
            IntPtr out_jsonPtr = IntPtr.Zero;
            out_json = null;
            try
            {
                int rc = DshRaw.dsh_engine_entry_document(engine, dict_idPtr, key_textPtr, out out_jsonPtr);
                ThrowIfError(rc);
                out_json = PtrToString(out_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_jsonPtr);
                out_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                Marshal.FreeHGlobal(dict_idPtr);
                Marshal.FreeHGlobal(key_textPtr);
                if (out_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_jsonPtr);
            }
        }

        /// <summary>按 key 从 .mdd 资源卷（或词典旁边散放的文件）取一段资源字节。支持 Range：offset/length 让宿主实现 206/416。</summary>
        internal static void EngineResource(IntPtr engine, string dict_id, string key, UIntPtr offset, UIntPtr length, out IntPtr out_bytes, out UIntPtr out_len, out string out_meta_json)
        {
            IntPtr dict_idPtr = StringToPtr(dict_id);
            IntPtr keyPtr = StringToPtr(key);
            IntPtr out_meta_jsonPtr = IntPtr.Zero;
            out_meta_json = null;
            try
            {
                int rc = DshRaw.dsh_engine_resource(engine, dict_idPtr, keyPtr, offset, length, out out_bytes, out out_len, out out_meta_jsonPtr);
                ThrowIfError(rc);
                out_meta_json = PtrToString(out_meta_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_meta_jsonPtr);
                out_meta_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                Marshal.FreeHGlobal(dict_idPtr);
                Marshal.FreeHGlobal(keyPtr);
                if (out_meta_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_meta_jsonPtr);
            }
        }

        /// <summary>文本分析：字形（判语种第一步）、音节分隔点、去掉点之后的写法、按词计数。★ 这一条就是为了把 main.ts 里那三份重复实现收进内核（ADR-001）。</summary>
        internal static void TextAnalyze(string text, out string out_json)
        {
            IntPtr textPtr = StringToPtr(text);
            IntPtr out_jsonPtr = IntPtr.Zero;
            out_json = null;
            try
            {
                int rc = DshRaw.dsh_text_analyze(textPtr, out out_jsonPtr);
                ThrowIfError(rc);
                out_json = PtrToString(out_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_jsonPtr);
                out_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                Marshal.FreeHGlobal(textPtr);
                if (out_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_jsonPtr);
            }
        }

        /// <summary>只做『去掉音节分隔点』这一件事。⚠️ 调用方必须遵守参考实现那个坑/38 的顺序：先拿原样文本问一次，问不到才用这里的返回值重问 —— 内核不替你改语义（ResolveKey 永远只认原样文本）。</summary>
        internal static void TextStripSeparators(string text, out string out_text)
        {
            IntPtr textPtr = StringToPtr(text);
            IntPtr out_textPtr = IntPtr.Zero;
            out_text = null;
            try
            {
                int rc = DshRaw.dsh_text_strip_separators(textPtr, out out_textPtr);
                ThrowIfError(rc);
                out_text = PtrToString(out_textPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_textPtr);
                out_textPtr = IntPtr.Zero;
            }
            finally
            {
                Marshal.FreeHGlobal(textPtr);
                if (out_textPtr != IntPtr.Zero) DshRaw.dsh_release(out_textPtr);
            }
        }

        /// <summary>判语种：字形 → 词典标题 → 用户设的默认语种。返回判定结果**与检查标准说明**（界面提示里要写『英语（按字形判断（拉丁字母））』，那句话由内核给，界面不拼）。</summary>
        internal static void LanguageDetect(IntPtr engine, string text, string dict_id, out string out_json)
        {
            IntPtr textPtr = StringToPtr(text);
            IntPtr dict_idPtr = StringToPtr(dict_id);
            IntPtr out_jsonPtr = IntPtr.Zero;
            out_json = null;
            try
            {
                int rc = DshRaw.dsh_language_detect(engine, textPtr, dict_idPtr, out out_jsonPtr);
                ThrowIfError(rc);
                out_json = PtrToString(out_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_jsonPtr);
                out_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                Marshal.FreeHGlobal(textPtr);
                Marshal.FreeHGlobal(dict_idPtr);
                if (out_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_jsonPtr);
            }
        }

        /// <summary>语种码 → **中文名**（`"en"` → `"英语"`）。★ 这张表是**判定语种用的同一张表**（在 `text/dsh_language.c` 里），所以界面要显示语种名时**必须问内核**，不许在壳或界面里再存一份 —— 两处各存一份迟早会对不上（参考实现的注释专门点过这一条）。⚠️ 认不出的码回**空串**（不是拿码当名字），调用方自己决定怎么显示。</summary>
        internal static void LanguageLabel(string code, out string out_json)
        {
            IntPtr codePtr = StringToPtr(code);
            IntPtr out_jsonPtr = IntPtr.Zero;
            out_json = null;
            try
            {
                int rc = DshRaw.dsh_language_label(codePtr, out out_jsonPtr);
                ThrowIfError(rc);
                out_json = PtrToString(out_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_jsonPtr);
                out_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                Marshal.FreeHGlobal(codePtr);
                if (out_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_jsonPtr);
            }
        }

        /// <summary>★ 豆包音色 id → **界面上的说法**（参考实现的 `DoubaoSpeech.DescribeSpeaker` + `DefaultSpeakerEnName/ZhName`）：两个默认音色回官网名 `Dacey` / `Vivi`，**其余一律回 id 本身**（不是空串 —— 界面上总得有个能认的东西）。为什么这条要进接口定义、而不是壳里再抄一张表：音色 id 与它的展示名**是同一份产品数据**，抄到壳里就是「同一个东西两个来源」（那个坑），迟早一处改了另一处没改。谁在用：`speech:status` 的两个 `speakerEnName`/`speakerZhName`、`speech:testDoubao` 每一项的 `speakerName`、在线发音回包里的 `voiceName`（`voiceId` 那格才是 id），以及内核自己拼的那句「为什么走这条」（`chosen.detail` / `why` 里显示的是名字，不是 `en_female_dacey_uranus_bigtts` 这种长串）。</summary>
        internal static void SpeechSpeakerLabel(string speaker, out string out_label)
        {
            IntPtr speakerPtr = StringToPtr(speaker);
            IntPtr out_labelPtr = IntPtr.Zero;
            out_label = null;
            try
            {
                int rc = DshRaw.dsh_speech_speaker_label(speakerPtr, out out_labelPtr);
                ThrowIfError(rc);
                out_label = PtrToString(out_labelPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_labelPtr);
                out_labelPtr = IntPtr.Zero;
            }
            finally
            {
                Marshal.FreeHGlobal(speakerPtr);
                if (out_labelPtr != IntPtr.Zero) DshRaw.dsh_release(out_labelPtr);
            }
        }

        /// <summary>这次朗读怎么念：三层音源的排序与选择、挑哪个嗓子、要不要切段、切几段。★ 把 main.ts 的 speechPlan / splitForSpeech 收进内核（ADR-001）。顺序是**产品约定**（词典自带 → 在线 → 系统离线），界面不给选、也不许自己判。</summary>
        internal static void SpeechPlan(IntPtr engine, string text, string dict_id, string voices_json, string overrides_json, out string out_json)
        {
            IntPtr textPtr = StringToPtr(text);
            IntPtr dict_idPtr = StringToPtr(dict_id);
            IntPtr voices_jsonPtr = StringToPtr(voices_json);
            IntPtr overrides_jsonPtr = StringToPtr(overrides_json);
            IntPtr out_jsonPtr = IntPtr.Zero;
            out_json = null;
            try
            {
                int rc = DshRaw.dsh_speech_plan(engine, textPtr, dict_idPtr, voices_jsonPtr, overrides_jsonPtr, out out_jsonPtr);
                ThrowIfError(rc);
                out_json = PtrToString(out_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_jsonPtr);
                out_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                Marshal.FreeHGlobal(textPtr);
                Marshal.FreeHGlobal(dict_idPtr);
                Marshal.FreeHGlobal(voices_jsonPtr);
                Marshal.FreeHGlobal(overrides_jsonPtr);
                if (out_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_jsonPtr);
            }
        }

        /// <summary>把一段 WAV 里的 16 位 PCM 按 dB 缩放（参考实现的 `GainMath.ApplyToWav`）。⚠️ 这是**系统离线语音那条路**的音量：参考实现就是在合成完、把字节交给播放器之前缩的（在『产字节』那一侧施加），所以那条路回包里的 `gainDb` **恒为 0** —— 前端再乘一次就是叠两遍。三条刻意的约定：① **顺序扫 RIFF 块**找 `fmt ` 与 `data`（不假设 44 字节头、也不假设只有一段 data —— SAPI 会插 `LIST`/`fact` 块，写死 44 会让增益『时灵时不灵』且错得隐蔽）；② **只动 16 位 PCM**，别的格式（8/24/32 位、浮点、压缩）**原样返回并说明**，不猜着改；③ **削顶不能不报错地** —— 想提的比峰值允许的多就夹回去，并把『从多少夹到多少、为什么』通过 `note` 说出去（悄悄削波听起来是破音，而用户没有任何线索去查）。⚙ 削顶保护的算法：`applied = min(请求值, 20·log10(1/峰值))`，再**向下**取整到 0.1 dB（宁可轻一点，也不要因为取整把峰值顶出满刻度）；**0 dB 一个字节都不动**（默认路径必须与『没有这个功能』逐字节相同）。⚠️ 传进来的 dB 是**已经归一化过**的（夹到 [-24,+12]、取整到 0.1 —— 那是设置层的活，见 `dsh_speech_gains`/`dsh_speech_gains_set`）。⚠️ 为什么这个参数是**整数**（dB×10）而不是浮点：接口定义里没有浮点标量类型，而增益在设置那一层本来就取整到 0.1 dB —— 传 dB×10 是**精确**的，也省掉一次跨 ABI 的浮点往返。</summary>
        internal static void AudioApplyGain(IntPtr bytes, UIntPtr len, int gain_tenths_db, out IntPtr out_bytes, out UIntPtr out_len, out string out_meta_json)
        {
            IntPtr out_meta_jsonPtr = IntPtr.Zero;
            out_meta_json = null;
            try
            {
                int rc = DshRaw.dsh_audio_apply_gain(bytes, len, gain_tenths_db, out out_bytes, out out_len, out out_meta_jsonPtr);
                ThrowIfError(rc);
                out_meta_json = PtrToString(out_meta_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_meta_jsonPtr);
                out_meta_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                if (out_meta_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_meta_jsonPtr);
            }
        }

        /// <summary>这条词条自带的原录音在哪：按『常见扩展名优先、原始键垫底』在多卷 .mdd 里找，返回键名与可播放格式。例句录音**不算**词目发音（参考实现的硬约定）。</summary>
        internal static void SpeechDictAudio(IntPtr engine, string dict_id, string key_text, out string out_json)
        {
            IntPtr dict_idPtr = StringToPtr(dict_id);
            IntPtr key_textPtr = StringToPtr(key_text);
            IntPtr out_jsonPtr = IntPtr.Zero;
            out_json = null;
            try
            {
                int rc = DshRaw.dsh_speech_dict_audio(engine, dict_idPtr, key_textPtr, out out_jsonPtr);
                ThrowIfError(rc);
                out_json = PtrToString(out_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_jsonPtr);
                out_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                Marshal.FreeHGlobal(dict_idPtr);
                Marshal.FreeHGlobal(key_textPtr);
                if (out_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_jsonPtr);
            }
        }

        /// <summary>从一本词典里挑几条**真的有录音**的词条（参考实现的 `App.DictSamplesAsync`）。⚙ 挑法逐条照参考实现：① **均匀撒网** —— 按**整本书的词条序号**分若干段、每段取开头那一条（⚠️ 单位是**词条**不是**词块**：按块取的话，一本只有 1 块的词典只能给出 1 个候选 —— 真词典 正是 1 块 9 条，实测踩过）；② 不够 6 条时**按索引顺序兜底补扫**（均匀撒点是一张网，网眼之间可能正好漏掉『录音集中在某一段』的词典）；③ 每条都**真解词条正文**去找录音，而且**只认词目发音**（例句不算 —— 量的是『点发音按钮会听到的那一段』，所以挑法与真正发音时完全一致：同一个 `dsh_speech_dict_audio`）；④ 三个上限：6 条 / 最多扫 60 个候选 / **最多 400 ms**（一次『解词条正文 + 到 .mdd 里找文件』实测十几毫秒，无上限地扫就是拿调用方的一次点击去跑后台任务）。⚠️ `ok=false` 时 message 是**三档不同的人话**：一本词典都没有 / 这本词典没有资源卷（.mdd）/ 有资源卷却扫不到（多半是音频卷没关联上）；扫不满 6 条但有一条以上时 `ok=true` 且 message 说清『只找到 N 条』。⚠️ 返回的是 **`audioKey`**（`.mdd` 里的键名），**可播地址由外壳拼**（`https://<外壳域>/__sound__/<词典 id>/<键名>`）—— 地址是平台形状，与 `dsh_speech_plan` 的 dict 那一层同一条规矩。⚠️ 今天**没有产品流程调它**（界面上的「平衡音量」按按需求删掉了，参考实现亦然）：接口、外壳接线与 B/标准都留着备用，**别看到『没人调』就删**。</summary>
        internal static void SpeechDictSamples(IntPtr engine, string dict_id, out string out_json)
        {
            IntPtr dict_idPtr = StringToPtr(dict_id);
            IntPtr out_jsonPtr = IntPtr.Zero;
            out_json = null;
            try
            {
                int rc = DshRaw.dsh_speech_dict_samples(engine, dict_idPtr, out out_jsonPtr);
                ThrowIfError(rc);
                out_json = PtrToString(out_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_jsonPtr);
                out_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                Marshal.FreeHGlobal(dict_idPtr);
                if (out_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_jsonPtr);
            }
        }

        /// <summary>在线语音（豆包 · 单向流式 HTTP）的**第一步：内核说该发什么**。三层音源里在线那一层的判断全在这儿：有没有配凭据与音色、这次该用哪个音色（**中英混排必须走中文音色** —— 实测拿英文音色念混排会得到空句子）、模型版本与音色配不配套（`seed-tts-2.0` / `seed-tts-1.0`）、请求体长什么样、四个头是什么。⚠️ 请求体里**刻意不写 `explicit_language`**：它的语义是"只念这个语种"，而词典正文中英混排是常态（见 docs/豆包语音合成接入方案.md 与 的实测）。⚠️ ok=false 时 reason 是人话，三种原因分开说（没填 Key / 没配音色 / 文本是空的）。</summary>
        internal static void SpeechOnlinePlan(IntPtr engine, string text, string dict_id, string overrides_json, out string out_json)
        {
            IntPtr textPtr = StringToPtr(text);
            IntPtr dict_idPtr = StringToPtr(dict_id);
            IntPtr overrides_jsonPtr = StringToPtr(overrides_json);
            IntPtr out_jsonPtr = IntPtr.Zero;
            out_json = null;
            try
            {
                int rc = DshRaw.dsh_speech_online_plan(engine, textPtr, dict_idPtr, overrides_jsonPtr, out out_jsonPtr);
                ThrowIfError(rc);
                out_json = PtrToString(out_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_jsonPtr);
                out_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                Marshal.FreeHGlobal(textPtr);
                Marshal.FreeHGlobal(dict_idPtr);
                Marshal.FreeHGlobal(overrides_jsonPtr);
                if (out_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_jsonPtr);
            }
        }

        /// <summary>在线语音的**检测凭据**那一条：管理窗「检测凭据」要发的那几次请求，该发什么。与 dsh_speech_online_plan 发的是**同一种东西**（同一份拼请求的代码），差别只在「念什么词、用哪个音色」由谁定 —— 这里由**界面正在填的那两个音色**定（用户改了 ID 还没写盘时，要测的必须是「即将存下去的那个」，测设置里存着的旧配置等于没测）。⚙ 逐项约定：① 给了 speaker 就只测它一项，语种用给的那个（认不出按 en），念的词从**内核自带的样本词表**取（`apple` / `苹果` …，与参考实现的 SampleWord 逐条相同）；② 没给 speaker 就**英文 + 中文各测一次**（两个音色的 Key / Resource 配套关系一样，但音色 ID 写错只有实测才发现）；③ 音色是空的 → 那一项 ok=false 且 reason 是「这个音色没填（这一项测不了）」——**「没东西可测」必须与「服务端说它不能用」分开说**，界面的判定（web/src/manager/main.ts 的 classifyDoubaoTest）就吃这一条；④ 没填 Key → 每一项的 reason 都是那句凭据提示。⚠️ 它**不受「在线总开关」限制**（先测通了再打开它，参考实现同一约定），但会**真联网**、按字符计费，所以只由用户点按钮触发。⚠️ 外壳在每一项上再补上 HTTP 的结果（statusCode/bytes/mime/elapsedMs/billedWords/error）与 speakerName —— 后者是**界面上的说法**（`Dacey` / `Vivi`；认不出就是 id 本身），由壳问 `dsh_speech_speaker_label` 得到（参考实现的 `DescribeSpeaker`），**壳里不许再抄一张表**（那个坑）。</summary>
        internal static void SpeechOnlineTestPlan(IntPtr engine, string speaker, string language, out string out_json)
        {
            IntPtr speakerPtr = StringToPtr(speaker);
            IntPtr languagePtr = StringToPtr(language);
            IntPtr out_jsonPtr = IntPtr.Zero;
            out_json = null;
            try
            {
                int rc = DshRaw.dsh_speech_online_test_plan(engine, speakerPtr, languagePtr, out out_jsonPtr);
                ThrowIfError(rc);
                out_json = PtrToString(out_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_jsonPtr);
                out_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                Marshal.FreeHGlobal(speakerPtr);
                Marshal.FreeHGlobal(languagePtr);
                if (out_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_jsonPtr);
            }
        }

        /// <summary>在线语音的**第二步：内核说回包是什么意思**。外壳把 plan 给的东西原样发出去，把 **HTTP 状态、整段回包正文、耗时**交回来 —— 解析 SSE 的 `data:` 行、把每段 base64 顺序拼成音频、错误码翻成人话，全在这里。⚠️ 回包是 **SSE（`data:` 行）**，所以外壳要**整段缓冲**再交进来，不许自己边收边解析。⚠️ **`code 0` 不等于成功**：实测"英文音色念中英混排"就是 code 0 + 零字节音频，所以"音频是空的"必须单独判成失败，且那句话要点出原因。</summary>
        internal static void SpeechOnlineAccept(IntPtr engine, string plan_json, int http_status, string response_body, int elapsed_ms, out IntPtr out_bytes, out UIntPtr out_len, out string out_json)
        {
            IntPtr plan_jsonPtr = StringToPtr(plan_json);
            IntPtr response_bodyPtr = StringToPtr(response_body);
            IntPtr out_jsonPtr = IntPtr.Zero;
            out_json = null;
            try
            {
                int rc = DshRaw.dsh_speech_online_accept(engine, plan_jsonPtr, http_status, response_bodyPtr, elapsed_ms, out out_bytes, out out_len, out out_jsonPtr);
                ThrowIfError(rc);
                out_json = PtrToString(out_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_jsonPtr);
                out_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                Marshal.FreeHGlobal(plan_jsonPtr);
                Marshal.FreeHGlobal(response_bodyPtr);
                if (out_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_jsonPtr);
            }
        }

        /// <summary>音量增益的现状（界面那两条滑块的值）。★ **增益是"按词典"存的**（`speech.dictGainDb`：一本一个数）+ **全局一个**（`speech.systemGainDb`）—— 不同词典的录音本来就录得不一样响。归一化约定（夹到 [-24, +12] dB、取整到 0.1）在**写**那一条里做，这里只报现状。⚠️ `dictAvailable` / `dictMessage` / `systemAvailable` / `systemMessage` 是**判断 + 两句产品文案**：能不能校准、不能校准时缺什么，只有内核手里有检查标准（界面一个字都不拼）。</summary>
        internal static void SpeechGains(IntPtr engine, string dict_id, int voice_count, out string out_json)
        {
            IntPtr dict_idPtr = StringToPtr(dict_id);
            IntPtr out_jsonPtr = IntPtr.Zero;
            out_json = null;
            try
            {
                int rc = DshRaw.dsh_speech_gains(engine, dict_idPtr, voice_count, out out_jsonPtr);
                ThrowIfError(rc);
                out_json = PtrToString(out_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_jsonPtr);
                out_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                Marshal.FreeHGlobal(dict_idPtr);
                if (out_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_jsonPtr);
            }
        }

        /// <summary>改音量增益，返回改完之后的那份视图（调用方拿它重排滑块，不必再问一次）。⚠️ 两个键都是"**可空数字**"，三种语义必须分清：**传数字** = 设成这个值（会被夹到 [-24, +12] 并取整到 0.1）；**传 null** = 清掉这一项（回到"没设过"= 0 增益）；**整个键不传** = 不改（前端只发它要改的那一项）。`dictGainDb` 只改**那一本词典**的数（增益按词典存）。</summary>
        internal static void SpeechGainsSet(IntPtr engine, string dict_id, string patch_json, int voice_count, out string out_json)
        {
            IntPtr dict_idPtr = StringToPtr(dict_id);
            IntPtr patch_jsonPtr = StringToPtr(patch_json);
            IntPtr out_jsonPtr = IntPtr.Zero;
            out_json = null;
            try
            {
                int rc = DshRaw.dsh_speech_gains_set(engine, dict_idPtr, patch_jsonPtr, voice_count, out out_jsonPtr);
                ThrowIfError(rc);
                out_json = PtrToString(out_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_jsonPtr);
                out_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                Marshal.FreeHGlobal(dict_idPtr);
                Marshal.FreeHGlobal(patch_jsonPtr);
                if (out_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_jsonPtr);
            }
        }

        /// <summary>把一段音频字节整成播放器能播的格式：格式按魔数嗅探、Speex(.spx) 就地解成 16bit PCM WAV、认不出来就**当场如实报错**（不许让播放器报错误码、更不许拿 TTS 假装顶上）。</summary>
        internal static void AudioPrepare(IntPtr bytes, UIntPtr len, out IntPtr out_bytes, out UIntPtr out_len, out string out_meta_json)
        {
            IntPtr out_meta_jsonPtr = IntPtr.Zero;
            out_meta_json = null;
            try
            {
                int rc = DshRaw.dsh_audio_prepare(bytes, len, out out_bytes, out out_len, out out_meta_jsonPtr);
                ThrowIfError(rc);
                out_meta_json = PtrToString(out_meta_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_meta_jsonPtr);
                out_meta_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                if (out_meta_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_meta_jsonPtr);
            }
        }

        /// <summary>查词历史（分页）。上限与每页条数由内核常量定。0.2.0 起历史落在 `<配置目录>/history.jsonl`（一行一条 JSON 的追加文件），界面只按页取、不许自己算分页。</summary>
        internal static void HistoryQuery(IntPtr engine, int offset, int limit, out string out_json)
        {
            IntPtr out_jsonPtr = IntPtr.Zero;
            out_json = null;
            try
            {
                int rc = DshRaw.dsh_history_query(engine, offset, limit, out out_jsonPtr);
                ThrowIfError(rc);
                out_json = PtrToString(out_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_jsonPtr);
                out_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                if (out_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_jsonPtr);
            }
        }

        /// <summary>清空查词历史。</summary>
        internal static void HistoryClear(IntPtr engine, out string out_json)
        {
            IntPtr out_jsonPtr = IntPtr.Zero;
            out_json = null;
            try
            {
                int rc = DshRaw.dsh_history_clear(engine, out out_jsonPtr);
                ThrowIfError(rc);
                out_json = PtrToString(out_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_jsonPtr);
                out_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                if (out_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_jsonPtr);
            }
        }

        /// <summary>机器翻译的设置与凭据状态（**只读设置、不联网、不花钱**）。界面靠它决定「能不能显示翻译」以及「缺 Key 该怎么说明」。字段与参考实现的 TranslateStatus 逐字相同 —— 连 endpoint / resourceId 两个常量也一起给（排错时一眼看出请求被指到哪儿去了；resourceId 恒为 volc.speech.mt，它要在控制台**单独开通**，与语音的 seed-tts-2.0 不是一回事）。⚠️ `enabled`（用户自己的开关）与 `hasApiKey`（凭据有没有）**必须分开判**：界面把两者合成一个 disabled，就会把「我自己关的」与「客观不可用」混成同一种灰，用户会反复点那个开关。</summary>
        internal static void TranslateStatus(IntPtr engine, out string out_json)
        {
            IntPtr out_jsonPtr = IntPtr.Zero;
            out_json = null;
            try
            {
                int rc = DshRaw.dsh_translate_status(engine, out out_jsonPtr);
                ThrowIfError(rc);
                out_json = PtrToString(out_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_jsonPtr);
                out_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                if (out_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_jsonPtr);
            }
        }

        /// <summary>机器翻译的**第一步：内核说该发什么**。内核零依赖、没有 socket，所以「把字节发出去」是平台层的活；但**判断一个字都不许留在平台层** —— 译成哪个语种、source 传不传、请求体长什么样、端点与三个头、这门语言支不支持，全在这里定。这一步的产物交给外壳原样发出去，回包再交给 dsh_translate_accept。⚠️ **命中缓存时不必发请求**：ok=true 且 cached=true，translation 就是译文。⚠️ ok=false 时 `message` 是人话，而且**四种原因分开说**（开关关着 / 没填 Key / 这门语言 MT 不支持 / 文本是空的）—— 合并成一句会让用户去改错的地方。</summary>
        internal static void TranslatePlan(IntPtr engine, string text, string dict_title, out string out_json)
        {
            IntPtr textPtr = StringToPtr(text);
            IntPtr dict_titlePtr = StringToPtr(dict_title);
            IntPtr out_jsonPtr = IntPtr.Zero;
            out_json = null;
            try
            {
                int rc = DshRaw.dsh_translate_plan(engine, textPtr, dict_titlePtr, out out_jsonPtr);
                ThrowIfError(rc);
                out_json = PtrToString(out_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_jsonPtr);
                out_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                Marshal.FreeHGlobal(textPtr);
                Marshal.FreeHGlobal(dict_titlePtr);
                if (out_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_jsonPtr);
            }
        }

        /// <summary>机器翻译的**第二步：内核说回包是什么意思**。外壳把 plan 给的东西原样发出去，然后把它拿回来的 **HTTP 状态、回包正文、耗时** 交回来 —— 解析译文、识别到的语种、计费 token、错误码翻成人话、写缓存全在这里做。⚠️ 错误码表**与 TTS 不是同一张**（这边成功是 20000000；TTS 那边同一个码含义不同），复用会把「成功」当「失败」。⚠️ **成功码但拿不到译文 → 不算成功**（宁可报错也不给一条空译文）。⚠️ 返回的是界面那份 TranslateResult（含 sourceLabel / targetLabel 两个中文名）—— 语言的中文名是产品文案，只许在内核这一处拼。</summary>
        internal static void TranslateAccept(IntPtr engine, string plan_json, int http_status, string response_body, int elapsed_ms, out string out_json)
        {
            IntPtr plan_jsonPtr = StringToPtr(plan_json);
            IntPtr response_bodyPtr = StringToPtr(response_body);
            IntPtr out_jsonPtr = IntPtr.Zero;
            out_json = null;
            try
            {
                int rc = DshRaw.dsh_translate_accept(engine, plan_jsonPtr, http_status, response_bodyPtr, elapsed_ms, out out_jsonPtr);
                ThrowIfError(rc);
                out_json = PtrToString(out_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_jsonPtr);
                out_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                Marshal.FreeHGlobal(plan_jsonPtr);
                Marshal.FreeHGlobal(response_bodyPtr);
                if (out_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_jsonPtr);
            }
        }

        /// <summary>清空译文缓存（界面上那个「清空翻译缓存」）。返回 {"count":N}（清掉几条）。⚠️ 它是**必需项而不是优化**：MT 按 token 计费，而用户来回跳词条会把同一句翻很多次。</summary>
        internal static void TranslateClearCache(IntPtr engine, out string out_json)
        {
            IntPtr out_jsonPtr = IntPtr.Zero;
            out_json = null;
            try
            {
                int rc = DshRaw.dsh_translate_clear_cache(engine, out out_jsonPtr);
                ThrowIfError(rc);
                out_json = PtrToString(out_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_jsonPtr);
                out_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                if (out_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_jsonPtr);
            }
        }

        /// <summary>把一次翻译包成**正文框里那个词条的载荷**（参考实现的 `TranslatePayload` + `Annotate`）。用于查词通道自动翻译那一档：链说「该翻译」（`via=translate` + `needsTranslate=true`），外壳把请求发出去、把回包交给 `dsh_translate_accept`，再拿 accept 的结果与**伪词条地址**回来问这一条，得到一份与真词条**同形状**的载荷 —— 于是朗读、复制、返回栈一行都不用改就都生效（参考实现的原话：译文若另起一块 UI，这四件事就得各写第二遍）。⚠️ `entry_url` 由外壳给（浏览器地址里那个令牌是外壳的一次性表，内核不认识 socket 也不认识 URL 表），其余字段**一个字都不许在外壳拼**。⚠️ 翻译失败时（`ok=false`）这一步**不许**装作翻过了：回的是 `found=false` + `via=terminal` + 那句人话。</summary>
        internal static void TranslatePayload(IntPtr engine, string translate_json, string entry_url, out string out_json)
        {
            IntPtr translate_jsonPtr = StringToPtr(translate_json);
            IntPtr entry_urlPtr = StringToPtr(entry_url);
            IntPtr out_jsonPtr = IntPtr.Zero;
            out_json = null;
            try
            {
                int rc = DshRaw.dsh_translate_payload(engine, translate_jsonPtr, entry_urlPtr, out out_jsonPtr);
                ThrowIfError(rc);
                out_json = PtrToString(out_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_jsonPtr);
                out_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                Marshal.FreeHGlobal(translate_jsonPtr);
                Marshal.FreeHGlobal(entry_urlPtr);
                if (out_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_jsonPtr);
            }
        }

        /// <summary>打开一本 .mdx（解析层，不经引擎）。探测 / 逐字节对照 / 工具用得上；产品路径一律走引擎。</summary>
        internal static void DictOpen(string path, out IntPtr out_dict)
        {
            IntPtr pathPtr = StringToPtr(path);
            try
            {
                int rc = DshRaw.dsh_dict_open(pathPtr, out out_dict);
                ThrowIfError(rc);
            }
            finally
            {
                Marshal.FreeHGlobal(pathPtr);
            }
        }

        /// <summary>关掉一本词典。</summary>
        internal static void DictClose(IntPtr dict)
        {
            try
            {
                DshRaw.dsh_dict_close(dict);
            }
            finally
            {
            }
        }

        /// <summary>词典头信息：书名、条目数、版本、加密标志、编码、资源卷命中的那几个。</summary>
        internal static void DictInfo(IntPtr dict, out string out_json)
        {
            IntPtr out_jsonPtr = IntPtr.Zero;
            out_json = null;
            try
            {
                int rc = DshRaw.dsh_dict_info(dict, out out_jsonPtr);
                ThrowIfError(rc);
                out_json = PtrToString(out_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_jsonPtr);
                out_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                if (out_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_jsonPtr);
            }
        }

        /// <summary>词典里有没有这个键。⚠️ 只认精确命中（大小写变体算命中），**不联想** —— 这是 product 约定，不是解析层细节。</summary>
        internal static void DictContains(IntPtr dict, string key, out string out_json)
        {
            IntPtr keyPtr = StringToPtr(key);
            IntPtr out_jsonPtr = IntPtr.Zero;
            out_json = null;
            try
            {
                int rc = DshRaw.dsh_dict_contains(dict, keyPtr, out out_jsonPtr);
                ThrowIfError(rc);
                out_json = PtrToString(out_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_jsonPtr);
                out_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                Marshal.FreeHGlobal(keyPtr);
                if (out_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_jsonPtr);
            }
        }

        /// <summary>取一条记录的原始内容（未渲染）。@@@LINK 重定向在这里解开，并给出最终落点。</summary>
        internal static void DictFetch(IntPtr dict, string key, out string out_json)
        {
            IntPtr keyPtr = StringToPtr(key);
            IntPtr out_jsonPtr = IntPtr.Zero;
            out_json = null;
            try
            {
                int rc = DshRaw.dsh_dict_fetch(dict, keyPtr, out out_jsonPtr);
                ThrowIfError(rc);
                out_json = PtrToString(out_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_jsonPtr);
                out_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                Marshal.FreeHGlobal(keyPtr);
                if (out_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_jsonPtr);
            }
        }

        /// <summary>枚举全部键（JSON 数组），按词典的序数序。逐字节对照与诊断的主力接口：拿它跟参考实现 / js-mdict 逐条比。</summary>
        internal static void DictKeys(IntPtr dict, out string out_json)
        {
            IntPtr out_jsonPtr = IntPtr.Zero;
            out_json = null;
            try
            {
                int rc = DshRaw.dsh_dict_keys(dict, out out_jsonPtr);
                ThrowIfError(rc);
                out_json = PtrToString(out_jsonPtr);
                // 取回来就还掉：内核给的内存必须还给内核（接口定义）
                DshRaw.dsh_release(out_jsonPtr);
                out_jsonPtr = IntPtr.Zero;
            }
            finally
            {
                if (out_jsonPtr != IntPtr.Zero) DshRaw.dsh_release(out_jsonPtr);
            }
        }
    }
}
