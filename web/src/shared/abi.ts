// ==========================================================================
// GENERATED — DO NOT EDIT.
// 由 tools/gen-bindings.mjs 从 abi/lookup.abi.json 生成（TypeScript 接口定义）。
// 界面只许从这里读常量与类型：凡是内核也知道的数，禁止在 UI 里再抄一份。
// ==========================================================================

export const DSH_ABI_VERSION = 2
export const DSH_VERSION = '0.2.0'

/** C 侧的错误码（内核返回值）。0 = 成功。 */
export const DshError = {
  /** 成功 */
  DSH_OK: 0,
  /** 参数不合法（空指针 / 空词 / 越界） */
  DSH_E_INVALID_ARG: -1,
  /** 文件不存在或不是 .mdx */
  DSH_E_NOT_FOUND: -2,
  /** 能打开但不是合法 MDict */
  DSH_E_FORMAT: -3,
  /** 读写失败 */
  DSH_E_IO: -4,
  /** 内存不足 */
  DSH_E_OOM: -5,
  /** 句柄状态不对（未加载就查、已释放还再用） */
  DSH_E_STATE: -6,
  /** 同一句柄上有并发调用（内核多数对象不是线程安全的） */
  DSH_E_BUSY: -7,
  /** 这一版还没接（如实报，不许装作成功） */
  DSH_E_NOT_IMPLEMENTED: -8,
} as const

/** 一次查询从哪条入口来。只有 input / selection 允许跑兜底通道。 */
export type DshOrigin =
  | 'input' // 输入框回车 / 查找按钮
  | 'selection' // 正文里选中文字
  | 'link' // 词条里的 entry:// 链接（不跑通道）
  | 'back' // 返回上一词条（不跑通道）
  | 'history' // 从查词历史回放（不跑通道）

/** 通道走到哪一步。界面据此显示进度。 */
export type DshStage =
  | 'start' // 还没开始
  | 'afterlookup' // 在当前词典查过了
  | 'afterprobe' // 问过别的词典了
  | 'aftersuggest' // 联想候选摆出来了
  | 'done' // 走到链的尽头了

/** 候选摆在哪儿。 */
export type DshSurface =
  | 'none'
  | 'list'
  | 'toast'

/** 词条页底部的出路按钮。按钮文字由内核给，界面只画。 */
export type DshChipAction =
  | 'borrow' // 用《X》查（借查，不切当前词典）。⚠️ 2026-09 起**这一档永不出现**：借查已经是通道里的自动一步，不该再让用户点一下去做同一件事（参考实现的 D1）。值留着不动枚举编号。
  | 'recheck' // 再问一遍（有没问完的词典时必须给）
  | 'translate' // 翻译这个词

/** 主导字形（语种判定的第一步）。 */
export type DshScript =
  | 'han'
  | 'latin'
  | 'other'

/** 三层音源。排序由内核定，界面不许自己排。 */
export type DshAudioSource =
  | 'dict' // 词典自带原录音（.mdd）
  | 'system' // 系统语音（离线合成）
  | 'online' // 在线发音（默认关）

/** 内核常量。UI 与工具一律读这里，禁止再抄字面量。 */
export const DshConstants = {
  /** 候选列表最多画几行。参考实现里这个数抄在三个文件里（那个坑），现在只有一处。 */
  maxListRows: 8,
  /** 选中文本按词计数的阈值：≤4 个词就写词名，≥5 个词说『所选文本』。 */
  selectionWordMax: 4,
  /** 查词历史上限（最多保留多少条）。 */
  historyLimit: 5000,
  /** 历史下拉一次取多少条（滚到底再取下一页）。 */
  historyPage: 60,
  /** 借查探路的单本预算（毫秒）。超预算算『没问完』，绝不许并进『没有』。 */
  probeBudgetMs: 120,
  /** 长文本朗读每段的目标字数（优先在句末断开）。 */
  speechChunkChars: 300,
  /** 发音缓存的内存上限（12 MB）。 */
  audiocacheMemBytes: 12582912,
  /** 发音缓存的磁盘上限（64 MB），超了按最久没用过的先删。 */
  audiocacheDiskBytes: 67108864,
} as const

/** 音节分隔点码点（顺序即优先级）。故意不含连字符。 */
export const DSH_SEPARATOR_CODEPOINTS: readonly number[] = [
  0x00b7, // · U+00B7 MIDDLE DOT（LDOCE 那种写法）
  0x2027, // ‧ U+2027 HYPHENATION POINT
  0x30fb, // ・ U+30FB KATAKANA MIDDLE DOT
  0x00ad, // ­ U+00AD SOFT HYPHEN（复制粘贴会带出来）
  0x02c8, // ˈ U+02C8 MODIFIER LETTER VERTICAL LINE（主重音，用户 2026-09 点名要的那个符号）：词典把词头写成 ˈæpl 时，它**永远不是词条名的一部分** —— 只在『原样问不到』的第二遍里被去掉，所以加了它不会误伤任何真实词条
  0x02cc, // ˌ U+02CC MODIFIER LETTER LOW VERTICAL LINE（次重音）：同 U+02C8 一条约定，两个一起加（只加一个等于留一半的坑）
]

/** 接口定义里所有接口的名字（诊断用：确保诊断问的接口真的存在）。 */
export const DSH_FUNCTIONS = [
  'dsh_version',
  'dsh_abi_version',
  'dsh_release',
  'dsh_last_error_message',
  'dsh_engine_create',
  'dsh_engine_destroy',
  'dsh_engine_settings_get',
  'dsh_engine_settings_set',
  'dsh_engine_dict_list',
  'dsh_engine_dict_add',
  'dsh_engine_dict_remove',
  'dsh_engine_dict_rename',
  'dsh_engine_dict_move',
  'dsh_engine_dict_set_current',
  'dsh_engine_resolve',
  'dsh_engine_suggest',
  'dsh_engine_lookup',
  'dsh_engine_probe',
  'dsh_engine_borrow',
  'dsh_engine_entry_document',
  'dsh_engine_resource',
  'dsh_text_analyze',
  'dsh_text_strip_separators',
  'dsh_language_detect',
  'dsh_language_label',
  'dsh_speech_speaker_label',
  'dsh_speech_plan',
  'dsh_audio_apply_gain',
  'dsh_speech_dict_audio',
  'dsh_speech_dict_samples',
  'dsh_speech_online_plan',
  'dsh_speech_online_test_plan',
  'dsh_speech_online_accept',
  'dsh_speech_gains',
  'dsh_speech_gains_set',
  'dsh_audio_prepare',
  'dsh_history_query',
  'dsh_history_clear',
  'dsh_translate_status',
  'dsh_translate_plan',
  'dsh_translate_accept',
  'dsh_translate_clear_cache',
  'dsh_translate_payload',
  'dsh_dict_open',
  'dsh_dict_close',
  'dsh_dict_info',
  'dsh_dict_contains',
  'dsh_dict_fetch',
  'dsh_dict_keys',
] as const
