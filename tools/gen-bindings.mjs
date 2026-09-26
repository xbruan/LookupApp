#!/usr/bin/env node
// 绑定生成脚本 —— 从唯一的接口定义文件 abi/lookup.abi.json 生成四份产物：
//   native/include/dsh_lookup.h · web/src/shared/abi.ts · shell/Lookup.Interop/DshLookup.g.cs · docs/api/abi.md
// 用法：
//   node tools/gen-bindings.mjs          生成 + 校验
//   node tools/gen-bindings.mjs --check  只校验、不改文件（CI 用；有差异则非零退出）
//   node tools/gen-bindings.mjs --list   只列接口清单（秒回，不写文件）
// 产物头部带 GENERATED 标记，禁止手改 —— 改了会在下一次 --check 里被抓住。
// ⚠️ 校验在生成之前跑完：宁可一条错误都不生成，也不要产出一份看起来对的绑定（会产错的生成脚本，错会被复制到每一份产物）。

import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const HERE = path.dirname(fileURLToPath(import.meta.url))
const ROOT = path.resolve(HERE, '..')
const SPEC_PATH = path.join(ROOT, 'abi', 'lookup.abi.json')

const argv = process.argv.slice(2)
const CHECK = argv.includes('--check')
const LIST = argv.includes('--list')

// ---------------------------------------------------------------- 读取与校验

function readSpec() {
  if (!fs.existsSync(SPEC_PATH)) fail([`接口定义不存在：${rel(SPEC_PATH)}`])
  try {
    return JSON.parse(fs.readFileSync(SPEC_PATH, 'utf8'))
  } catch (e) {
    fail([`接口定义不是合法 JSON：${e.message}`])
  }
}

const errors = []

function err(message) {
  errors.push(message)
}

/** 生成本身依赖的类型名字集合（含 C 里直接写的原生类型） */
/** C 内建类型只有两个：`void` 与 `opaque`。
 *  ⚠️ `void` 在返回值位置与参数位置含义不同：返回值位置的 void 一律渲染成 `void`（见 cType）；
 *  参数里的裸内存必须用 `opaque`（映射串就是 `void *`）—— 一个映射串表达不了这两件事。 */
const NATIVE_TYPES = {}
const NATIVE_NAMES = new Set()

/** 接口定义里 types 的形状：`{ comment, map: { 名字 → 定义 } }`。
 *  单独开一层 `map` 是故意的 —— 说明文字与类型定义不混在同一层，免得「有个叫 comment 的类型」这种误会。 */
function typeMap(spec) {
  return spec.types?.map ?? {}
}

/** 一个类型名是否「认识」：types.map 里定义的，或 enums 里的枚举。 */
function knownType(spec, name) {
  return !!typeDef(spec, name)
}

/** 取类型的定义（enum 前缀 `enum`；其余查 types.map）。认不出来返回 undefined。
 *  枚举的三语言映射由生成脚本统一约定，接口定义里只写 `enum` 名、不重复三个字段：
 *  C 里枚举名要带 enum 关键字，C# 与 TS 都用 PascalCase 的 `Dsh…`。 */
function typeDef(spec, name) {
  if (spec.enums?.[name]) {
    return { kind: 'enum', c: `enum ${name}`, cs: csEnumName(name), ts: csEnumName(name) }
  }
  const def = typeMap(spec)[name]
  if (def && def.kind) return def
  return undefined
}

/** `dsh_origin` → `DshOrigin`（枚举类型名的统一约定，见 typeDef 的注释） */
function csEnumName(name) {
  return name.split('_').map((w) => w.charAt(0).toUpperCase() + w.slice(1)).join('')
}

/** 函数名 → C# 方法名：`dsh_dict_open` → `DictOpen` */
function csMethodName(fnName) {
  return fnName
    .split('_')
    .filter((w) => w !== 'dsh')
    .map((w) => w.charAt(0).toUpperCase() + w.slice(1))
    .join('')
}

/** 枚举取值名 → C# 成员名：`DSH_ORIGIN_INPUT` → `Input`（去掉与枚举名重复的前缀） */
function csValueName(enumValueName) {
  const parts = enumValueName.split('_')
  // 去掉 DSH_ 与紧接着的类型段（ORIGIN / STAGE / SURFACE / CHIP / SCRIPT / AUDIO）
  let i = parts[0] === 'DSH' ? 1 : 0
  if (i < parts.length && parts[i].length > 1) i++
  const tail = parts.slice(i)
  const name = tail.length > 0 ? tail : parts
  return name.map((w) => w.charAt(0).toUpperCase() + w.slice(1).toLowerCase()).join('')
}

function validate(spec) {
  errors.length = 0

  // ── meta
  const meta = spec.meta ?? {}
  for (const key of ['name', 'version', 'abiVersion', 'soname']) {
    if (meta[key] === undefined || meta[key] === null || meta[key] === '') err(`meta.${key} 缺失`)
  }
  if (meta.abiVersion !== undefined && !Number.isInteger(meta.abiVersion)) err('meta.abiVersion 必须是整数')

  // ── types
  const types = typeMap(spec)
  const typeNames = Object.keys(types)
  if (typeNames.length === 0) err('types.map 里一个类型都没有')
  for (const name of typeNames) {
    const def = types[name]
    for (const lang of ['c', 'cs', 'ts']) {
      if (!def[lang]) err(`types.${name} 缺 ${lang} 映射（每个类型三语言都必须能映射）`)
    }
    if (def.kind === 'handle') {
      if (!def.handleOf) err(`types.${name} 是 handle，必须写 handleOf（对应哪个 destroys 目标）`)
      if (!String(def.c).includes('*')) {
        err(`types.${name} 是 handle，C 映射必须是指针（如 ${def.c}）`)
      }
    }
    if (def.kind === 'string' && def.ownership === 'callee' && !def.release) {
      // 出参字符串必须写清谁来释放 —— 这是跨语言最容易漏的一条
      err(`types.${name} 标了 ownership=callee，必须写 release（宿主拿什么还）`)
    }
  }
  for (const required of ['void', 'opaque']) {
    if (!types[required]) err(`types.map 缺 ${required}（接口定义的基本约定要用到它）`)
  }

  // ── enums
  const enums = spec.enums ?? {}
  const enumValueOwner = new Map() // 取值名 → 它属于哪个 enum
  for (const [name, def] of Object.entries(enums)) {
    const values = def.values ?? []
    if (values.length === 0) err(`enums.${name} 没有取值`)
    const seenName = new Set()
    const seenValue = new Set()
    for (const v of values) {
      if (!v.name) err(`enums.${name} 有一项没有 name`)
      if (!Number.isInteger(v.value)) err(`enums.${name}.${v.name} 的 value 必须是整数`)
      if (seenName.has(v.name)) err(`enums.${name} 的取值名重复：${v.name}`)
      if (seenValue.has(v.value)) err(`enums.${name} 的取值 ${v.value} 重复（${v.name}）`)
      seenName.add(v.name)
      seenValue.add(v.value)
      if (!v.name.startsWith('DSH_')) err(`enums.${name}.${v.name} 必须以 DSH_ 开头（跨语言才不撞名）`)
      // 真正要防的是「两个 enum 用了同一个取值名」—— C 里会重定义；前缀怎么写留给作者，
      // 但同一个名字不许出现在两个 enum 里。
      if (enumValueOwner.has(v.name)) {
        err(`取值名 ${v.name} 在 enums.${enumValueOwner.get(v.name)} 与 enums.${name} 里都出现了（C 里会重定义）`)
      } else {
        enumValueOwner.set(v.name, name)
      }
    }
    if (def.comment === undefined) err(`enums.${name} 缺 comment（每个 enum 都要说清它是什么）`)
  }

  // ── constants
  const constants = spec.constants ?? {}
  const constNames = new Set()
  for (const c of constants.values ?? []) {
    if (!c.name) err('constants.values 有一项缺 name')
    if (constNames.has(c.name)) err(`常量名重复：${c.name}`)
    constNames.add(c.name)
    if (!c.name.startsWith('DSH_')) err(`常量 ${c.name} 必须以 DSH_ 开头`)
    if (!types[c.type]) err(`常量 ${c.name} 的类型 ${c.type} 不在 types 里`)
    if (c.value === undefined) err(`常量 ${c.name} 缺 value`)
    if (c.comment === undefined) err(`常量 ${c.name} 缺 comment（这个数是给谁用的？）`)
  }
  const seps = constants.separators
  if (seps) {
    for (const s of seps.values ?? []) {
      if (!Number.isInteger(s.value)) err(`separators.${s.name} 的码点必须是整数`)
      if (typeof s.char !== 'string' || s.char.codePointAt(0) !== s.value) {
        err(`separators.${s.name} 的 char 与 value 对不上（char=${JSON.stringify(s.char)} value=${s.value}）`)
      }
    }
    // 连字符绝不许混进来（硬约定：连字符是合法词条字符，不许当分隔点剥掉）
    for (const forbidden of [0x002d, 0x2010, 0x2013, 0x2014]) {
      if ((seps.values ?? []).some((s) => s.value === forbidden)) {
        err(`separators 里混进了连字符 U+${forbidden.toString(16).toUpperCase()} —— 连字符是合法词条字符（well-known），不许当分隔点剥掉`)
      }
    }
  }

  // ── functions
  const fns = spec.functions ?? []
  if (fns.length === 0) err('functions 是空的')
  const fnNames = new Set()
  for (const fn of fns) {
    if (!fn.name) { err('functions 有一项缺 name'); continue }
    if (fnNames.has(fn.name)) err(`函数名重复：${fn.name}`)
    fnNames.add(fn.name)
    if (!fn.name.startsWith('dsh_')) err(`函数 ${fn.name} 必须以 dsh_ 开头`)
    if (!fn.group) err(`函数 ${fn.name} 缺 group（分组用来说明它属于哪一层）`)
    if (fn.comment === undefined) err(`函数 ${fn.name} 缺 comment（这是给人读的接口定义）`)
    if (!Number.isInteger(fn.since)) err(`函数 ${fn.name} 缺 since（ABI 版本号）`)
    if (!fn.returns) err(`函数 ${fn.name} 缺 returns`)
    // void 只能当**返回值**（那是合法 C）；参数里的裸内存要用 opaque。
    // ⚠️ 别把这条写成「returns 不许是 void」—— 那个方向判反了：void 返回值正是常规写法。
    if ((fn.params ?? []).some((p) => p.type === 'void')) {
      err(`函数 ${fn.name} 有参数类型写成 void —— 参数里的裸内存请用 opaque`)
    }

    const isErrorReturn = fn.returns === 'dsh_error'
    if (!knownType(spec, fn.returns)) {
      err(`函数 ${fn.name} 的返回类型 ${fn.returns} 不认识（既不在 types 也不在 enums 里）`)
    }
    if (!isErrorReturn && fn.returns !== 'void' && fn.ownership === undefined) {
      err(`函数 ${fn.name} 直接返回 ${fn.returns}，必须写 ownership（这块内存谁负责）`)
    }
    if (fn.ownership === 'callee' && !fn.release) {
      err(`函数 ${fn.name} 标了 ownership=callee，必须写 release`)
    }
    if (fn.release && fn.release !== 'dsh_release') {
      err(`函数 ${fn.name} 的 release 只能是 dsh_release（内核统一释放，宿主不许多写一种还法）`)
    }
    if (fn.destroys && !typeMap(spec)[fn.destroys]) {
      err(`函数 ${fn.name} 的 destroys=${fn.destroys} 不在 types.map 里`)
    }
    if (fn.destroys && fn.ownership !== 'caller-destroys') {
      err(`函数 ${fn.name} 写了 destroys，ownership 必须是 caller-destroys`)
    }

    // 出参必须带 direction:'out'，入参不许带 —— 否则生成的签名会一半对一半错
    const params = fn.params ?? []
    const outHandles = params.filter((p) => p.direction === 'out')
    for (const p of params) {
      if (!p.name) err(`函数 ${fn.name} 有参数缺 name`)
      if (!p.type) err(`函数 ${fn.name} 的参数 ${p.name} 缺 type`)
      else if (!knownType(spec, p.type)) {
        err(`函数 ${fn.name} 的参数 ${p.name} 类型 ${p.type} 不在 types / enums 里`)
      }
      if (p.direction && p.direction !== 'out' && p.direction !== 'in') {
        err(`函数 ${fn.name} 的参数 ${p.name} 的 direction 只能是 in/out`)
      }
      if (p.type && knownType(spec, p.type)) {
        const def = typeDef(spec, p.type)
        // 内核分配的内存一律走出参；入参不许是「要内核释放的 json」
        if (p.direction !== 'out' && def.kind === 'string' && def.ownership === 'callee') {
          err(`函数 ${fn.name} 的参数 ${p.name} 是 ${p.type}（callee 拥有），只能当出参`)
        }
        // 枚举与标量当出参没有意义（调用方不会为了拿一个 enum 传指针），提前挡住
        if (p.direction === 'out' && (def.kind === 'enum')) {
          err(`函数 ${fn.name} 的参数 ${p.name} 是枚举 ${p.type}，不该当出参`)
        }
      }
    }
    if (isErrorReturn && outHandles.length === 0 && fn.noOutParams !== true) {
      err(`函数 ${fn.name} 返回 dsh_error 却没有任何出参 —— 那调用方拿不到东西；确实如此请写 noOutParams:true`)
    }

    // 句柄类出参必须写 destroys，否则宿主不知道谁来关 —— 这就是内存泄漏的来源
    for (const p of outHandles) {
      const def = typeDef(spec, p.type)
      if (def && def.kind === 'handle' && !fn.destroys) {
        err(`函数 ${fn.name} 出了句柄 ${p.name}（${p.type}），必须写 destroys 说明谁来关`)
      }
    }
  }

  // ── 交叉引用：enum 的 values 名字不许与常量撞名
  const allEnumValues = new Set()
  for (const def of Object.values(enums)) for (const v of def.values ?? []) allEnumValues.add(v.name)
  for (const n of constNames) {
    if (allEnumValues.has(n)) err(`常量 ${n} 与某个 enum 取值撞名（C 里会重定义）`)
  }
  for (const name of Object.keys(spec.types ?? {})) {
    if (allEnumValues.has(name)) err(`类型 ${name} 与某个 enum 取值撞名`)
  }

  return errors.length === 0
}

function fail(messages, mode = 'generate') {
  if (mode === 'check') {
    console.error('接口定义检查失败（--check 只读不写）：')
  } else {
    console.error('接口定义校验失败：一个文件都没写（磁盘上若已有自动生成的文件，那是上一次的，保持原样）。')
  }
  for (const m of messages) console.error('  ✗ ' + m)
  process.exit(1)
}

// ---------------------------------------------------------------- 渲染：C 头

const GENERATED_C = (what) =>
  `/* ==========================================================================\n` +
  ` * GENERATED — DO NOT EDIT.\n` +
  ` * 由 tools/gen-bindings.mjs 从 abi/lookup.abi.json 生成（${what}）。\n` +
  ` * 改接口定义请改 abi/lookup.abi.json，然后跑：node tools/gen-bindings.mjs\n` +
  ` * ========================================================================== */\n`

function renderHeader(spec) {
  const m = spec.meta
  const L = []
  L.push(GENERATED_C('C 头文件'))
  L.push(`#ifndef DSH_LOOKUP_H`)
  L.push(`#define DSH_LOOKUP_H`)
  L.push('')
  L.push(`/* ${m.name} ABI v${m.abiVersion} · 内核版本 ${m.version} */`)
  L.push('')
  L.push('#include <stddef.h>')
  L.push('#include <stdint.h>')
  L.push('')
  L.push('#ifdef __cplusplus')
  L.push('extern "C" {')
  L.push('#endif')
  L.push('')
  L.push(`#define DSH_ABI_VERSION ${m.abiVersion}`)
  L.push(`#define DSH_VERSION_STRING "${m.version}"`)
  L.push('')

  // 字符串与内存约定（跨语言最容易出事，写在头文件最前面）
  L.push('/* ── 内存与字符串约定 ─────────────────────────────────────────────────────')
  for (const line of wrapComment(m.memoryRule, 68)) L.push(line)
  for (const line of wrapComment(m.errorRule, 68)) L.push(line)
  L.push(` * 字符串一律 ${m.stringEncoding} 且以 \\0 结尾；调用约定 ${m.callingConvention}。`)
  L.push(' * ---------------------------------------------------------------------- */')
  L.push('')
  // dsh_release 由接口定义里的 core 分组生成（不在这里手写 —— 手写会与接口定义里那份重复声明）

  // 不透明句柄（前置声明）
  const handles = Object.entries(typeMap(spec)).filter(([, d]) => d && d.kind === 'handle')
  if (handles.length) {
    L.push('/* ── 不透明句柄（宿主不许解释它的位）────────────────────────────────────── */')
    for (const [, d] of handles) {
      const base = String(d.c).replace(/\s*\*+\s*$/, '').trim()
      L.push(`typedef struct ${base} ${base};`)
    }
    L.push('')
  }

  // enums
  const enumNames = Object.keys(spec.enums ?? {})
  if (enumNames.length) {
    L.push('/* ── 枚举 ──────────────────────────────────────────────────────────────── */')
    for (const [name, def] of Object.entries(spec.enums)) {
      L.push(...cComment(wrapComment(def.comment ?? '', 74)))
      L.push(`typedef enum ${name} {`)
      const vals = def.values
      for (let i = 0; i < vals.length; i++) {
        const v = vals[i]
        const comma = i === vals.length - 1 ? '' : ','
        const head = `  ${v.name} = ${v.value}${comma}`.padEnd(38)
        L.push(v.comment ? `${head}/* ${v.comment} */` : head.trimEnd())
      }
      L.push(`} ${name};`)
      L.push('')
    }
  }

  // constants
  const consts = spec.constants ?? {}
  if ((consts.values ?? []).length) {
    L.push('/* ── 常量（凡是别的文件里也抄了一份的数，都在这里）───────────────────────── */')
    if (consts.comment) L.push(...cComment(wrapComment(consts.comment, 74)))
    for (const c of consts.values) {
      const suffix = /usize|u32/.test(c.type) ? 'u' : ''
      if (c.comment) L.push(...cComment(wrapComment(c.comment, 74)))
      L.push(`#define ${c.name} ${c.value}${suffix}`)
    }
    L.push('')
  }

  // separators：单独一节，因为它是「清单」不是「标量」，而且要能逐码点核对
  if (consts.separators) {
    const s = consts.separators
    L.push('/* ── 音节分隔点 ──────────────────────────────────────────────────────────')
    if (s.comment) for (const line of wrapComment(s.comment, 74)) L.push(line)
    L.push(' * ---------------------------------------------------------------------- */')
    for (const v of s.values) {
      const line = `#define ${v.name} 0x${v.value.toString(16).toUpperCase().padStart(4, '0')}u`
      L.push(v.comment ? `${line.padEnd(44)}/* ${v.comment} */` : line)
    }
    L.push('')
    L.push('/* 清单本身（顺序即优先级）：供"逐个码点核对"的检查标准用，别在别处再抄一份。 */')
    L.push(`static const uint32_t DSH_SEPARATOR_CODEPOINTS[] = {`)
    L.push('  ' + s.values.map((v) => `${v.name}`).join(', '))
    L.push('};')
    L.push(`#define DSH_SEPARATOR_COUNT ${s.values.length}u`)
    L.push('')
  }

  // functions 按 group 分节
  const groups = []
  for (const fn of spec.functions) {
    if (!groups.includes(fn.group)) groups.push(fn.group)
  }
  const GROUP_TITLE = {
    core: '核心：版本、内存、错误',
    engine: '引擎：设置、词库、句柄',
    lookup: '查词：落点、联想、通道、正文、资源',
    text: '文本：字形、分隔点、语种',
    speech: '发音：音源规划、词典原录音、音频预处理',
    history: '历史：分页查询与清空',
    translate: '机器翻译',
    parser: '解析层：单本 .mdx（工具与对照测试用，产品路径走引擎）',
  }
  for (const g of groups) {
    L.push(`/* ── ${GROUP_TITLE[g] ?? g} ${'─'.repeat(Math.max(0, 68 - (GROUP_TITLE[g] ?? g).length))} */`)
    for (const fn of spec.functions.filter((f) => f.group === g)) {
      L.push(renderCFunction(spec, fn))
    }
  }

  L.push('#ifdef __cplusplus')
  L.push('} /* extern "C" */')
  L.push('#endif')
  L.push('')
  L.push('#endif /* DSH_LOOKUP_H */')
  return L.join('\n') + '\n'
}

function renderCFunction(spec, fn) {
  const sig = cFunctionSignature(spec, fn)
  const commentLines = []
  if (fn.comment) commentLines.push(...wrapComment(fn.comment, 74))
  if (fn.ownership === 'callee') commentLines.push(` * 出参由内核分配：调用方用 ${fn.release}() 还给内核。`)
  if (fn.destroys) commentLines.push(` * 调用方用 ${destroyName(fn.destroys)}() 关掉它。`)
  const out = [...cComment(commentLines), sig, '']
  return out.join('\n')
}

/** destroys 的目标类型 → 关闭函数名 */
function destroyName(typeName) {
  return typeName === 'engine' ? 'dsh_engine_destroy' : typeName === 'dict' ? 'dsh_dict_close' : '(未知)'
}

/** 渲染一个 C 声明的类型部分。C 的映射串**自带它应有的星号**（`void *` / `char *`），
 *  出参只统一**多加一层**（`char *` → `char **`）。
 *  不设「星号深度整数」那种字段：同一个 `void` 在返回值与参数两种位置需要不同的深度，一处字段表达不了。 */
function cDecl(spec, name, extraStars) {
  const def = typeDef(spec, name)
  if (!def) return `/* 未知类型 ${name} */`
  const stars = '*'.repeat(extraStars ?? 0)
  return stars ? `${def.c} ${stars}`.trim() : def.c
}

/** 入参是只读的：字符串与字节缓冲就是 const 指针；标量不加。
 *  `opaque` 例外 —— 它是「要还给内核的裸内存」，本来就该是 `void *`，不加 const。 */
function cType(spec, name, direction) {
  const def = typeDef(spec, name)
  if (def && def.kind === 'prim' && def.c === 'void') return 'void'
  const decl = cDecl(spec, name, direction === 'out' ? 1 : 0)
  const readonly =
    direction !== 'out' && def && (def.kind === 'string' || (def.kind === 'buffer' && name !== 'opaque'))
  return readonly ? `const ${decl}` : decl
}

/** C 声明的**唯一**合成点：`类型串 + 声明串 → 合法声明`。
 *  规则只有一条：类型尾部的星号与声明头部的星号合成**一个**星号串（`char *` + `*name` → `char **name`）。
 *  返回值、参数、函数名都走这里 —— 分头写就会产出 `char * *out` 这种能编过但不像人写的东西。 */
function cDeclarator(spec, typeName, direction, declarator) {
  const t = cType(spec, typeName, direction)
  const ti = t.indexOf('*')
  const base = (ti >= 0 ? t.slice(0, ti) : t).trim()
  const typeStars = ti >= 0 ? t.slice(ti).replace(/\s+/g, '') : ''
  const dm = declarator.match(/^(\**)(.*)$/)
  const stars = typeStars + dm[1]
  // ⚠️ 没有星号时**必须**留一个空格：`size_t` + `offset` 会拼成 `size_toffset`，
  // 语法上仍是「合法的 C」（那是个不存在的类型名），要到用的时候才报 —— 只能靠形状自检咬住。
  return stars ? `${base} ${stars}${dm[2]}` : `${base} ${dm[2]}`
}

/** 渲染一个 C 参数：`类型 名字` */
function cParam(spec, p) {
  return cDeclarator(spec, p.type, p.direction, p.name)
}

/** 渲染整条函数声明 */
function cFunctionSignature(spec, fn) {
  const params = (fn.params ?? []).map((p) => cParam(spec, p)).join(', ')
  return `${cType(spec, fn.returns)} ${fn.name}(${params || 'void'});`
}

/** 生成后的**自检**：产物形状必须像人写的 C —— 生成脚本的价值就是不让人去核对签名，所以它得先咬住自己。
 *  咬的是三种坏形状：星号没合并、类型与星号之间多空格、标量类型与参数名粘住。
 *  ⚠️ **不要加「返回值星号后面不许有空格」那种检查标准**：`const char * dsh_version(void);` 是对的 C，检查标准写错比没有检查标准更坏。 */
function checkOutputShape(name, text) {
  const problems = []
  text.split('\n').forEach((line, i) => {
    const no = i + 1
    const code = line.trim().startsWith('/*') || line.trim().startsWith('*') ? '' : line
    const hit = (message) => problems.push(`${name}:${no} ${message}：${line.trim().slice(0, 110)}`)
    if (/\*\s+\*/.test(code)) hit('星号没合并（出现 "* *"）')
    if (/\w\s{2,}\*/.test(code) || /\*\s{2,}\w/.test(code)) hit('类型与星号之间有多余空格')
    // C 标识符被粘住：标量类型名 + 小写参数名
    for (const bad of code.matchAll(/(size_t|uint32_t|int32_t)[a-z_]\w*/g)) {
      if (bad[1] !== 'size_t' || bad[0] !== 'size_t') hit(`标量类型与名字粘住了（${bad[0]}）`)
    }
  })
  return problems
}

/** 按宽度折中文注释（中文按 2 列算）。返回**已经带 `* ` 前缀**的行，调用方直接拼。 */
function wrapComment(text, width) {
  const words = String(text).split(/\s+/).filter(Boolean)
  const lines = []
  let cur = ''
  const w = (s) => [...s].reduce((n, ch) => n + (ch.codePointAt(0) > 0x2e80 ? 2 : 1), 0)
  for (const word of words) {
    const next = cur ? cur + ' ' + word : word
    if (w(next) > width && cur) { lines.push(cur); cur = word } else { cur = next }
  }
  if (cur) lines.push(cur)
  return lines.map((l) => ` * ${l}`)
}

/** 把一个注释（字符串或已折好的行数组）渲染成 C 注释块：一行折成单行、多行折成多行块。
 *  dsh_release 这类「手写」声明也走它。 */
function cComment(lines) {
  const arr = (Array.isArray(lines) ? lines : [lines]).filter((l) => l !== undefined && l !== null && l !== '')
  if (arr.length === 0) return []
  if (arr.length === 1) {
    const one = String(arr[0]).replace(/^ \* ?/, '')
    return [`/* ${one} */`]
  }
  return ['/*', ...arr, ' */']
}

// ---------------------------------------------------------------- 渲染：TS

function renderTs(spec) {
  const m = spec.meta
  const L = []
  L.push(`// ${'='.repeat(74)}`)
  L.push(`// GENERATED — DO NOT EDIT.`)
  L.push(`// 由 tools/gen-bindings.mjs 从 abi/lookup.abi.json 生成（TypeScript 接口定义）。`)
  L.push(`// 界面只许从这里读常量与类型：凡是内核也知道的数，禁止在 UI 里再抄一份。`)
  L.push(`// ${'='.repeat(74)}`)
  L.push('')
  L.push(`export const DSH_ABI_VERSION = ${m.abiVersion}`)
  L.push(`export const DSH_VERSION = '${m.version}'`)
  L.push('')
  L.push('/** C 侧的错误码（内核返回值）。0 = 成功。 */')
  L.push(`export const DshError = {`)
  for (const v of spec.enums.dsh_error.values) {
    if (v.comment) L.push(`  /** ${v.comment} */`)
    L.push(`  ${v.name}: ${v.value},`)
  }
  L.push(`} as const`)
  L.push('')
  L.push('/** 一次查询从哪条入口来。只有 input / selection 允许跑兜底通道。 */')
  L.push(`export type DshOrigin =`)
  for (const v of spec.enums.dsh_origin.values) {
    L.push(`  | '${tsOriginName(v.name)}'${v.comment ? ` // ${v.comment}` : ''}`)
  }
  L.push('')
  L.push('/** 通道走到哪一步。界面据此显示进度。 */')
  L.push(`export type DshStage =`)
  for (const v of spec.enums.dsh_stage.values) {
    L.push(`  | '${tsValueName('DSH_STAGE_', v.name)}'${v.comment ? ` // ${v.comment}` : ''}`)
  }
  L.push('')
  L.push('/** 候选摆在哪儿。 */')
  L.push(`export type DshSurface =`)
  for (const v of spec.enums.dsh_surface.values) {
    L.push(`  | '${tsValueName('DSH_SURFACE_', v.name)}'`)
  }
  L.push('')
  L.push('/** 词条页底部的出路按钮。按钮文字由内核给，界面只画。 */')
  L.push(`export type DshChipAction =`)
  for (const v of spec.enums.dsh_chip_action.values) {
    L.push(`  | '${tsValueName('DSH_CHIP_', v.name)}'${v.comment ? ` // ${v.comment}` : ''}`)
  }
  L.push('')
  L.push('/** 主导字形（语种判定的第一步）。 */')
  L.push(`export type DshScript =`)
  for (const v of spec.enums.dsh_script.values) {
    L.push(`  | '${tsValueName('DSH_SCRIPT_', v.name)}'`)
  }
  L.push('')
  L.push('/** 三层音源。排序由内核定，界面不许自己排。 */')
  L.push(`export type DshAudioSource =`)
  for (const v of spec.enums.dsh_audio_source.values) {
    L.push(`  | '${tsValueName('DSH_AUDIO_', v.name)}'${v.comment ? ` // ${v.comment}` : ''}`)
  }
  L.push('')
  L.push('/** 内核常量。UI 与工具一律读这里，禁止再抄字面量。 */')
  L.push(`export const DshConstants = {`)
  for (const c of spec.constants.values) {
    if (c.comment) L.push(`  /** ${c.comment} */`)
    L.push(`  ${camel(c.name.replace(/^DSH_/, ''))}: ${c.value},`)
  }
  L.push(`} as const`)
  L.push('')
  if (spec.constants.separators) {
    const s = spec.constants.separators
    L.push('/** 音节分隔点码点（顺序即优先级）。故意不含连字符。 */')
    L.push(`export const DSH_SEPARATOR_CODEPOINTS: readonly number[] = [`)
    for (const v of s.values) L.push(`  0x${v.value.toString(16).padStart(4, '0')}, // ${v.char} ${v.comment ?? ''}`)
    L.push(`]`)
    L.push('')
  }
  L.push('/** 接口定义里所有接口的名字（诊断脚本用：确保诊断脚本问的接口真的存在）。 */')
  L.push(`export const DSH_FUNCTIONS = [`)
  for (const fn of spec.functions) L.push(`  '${fn.name}',`)
  L.push(`] as const`)
  L.push('')
  return L.join('\n')
}

function tsOriginName(enumName) {
  return enumName.replace(/^DSH_ORIGIN_/, '').toLowerCase()
}
function tsValueName(prefix, enumName) {
  return enumName.replace(new RegExp('^' + prefix), '').toLowerCase().replace(/_/g, '')
    .replace(/^([a-z])/, (c) => c)
}
function camel(s) {
  return s.toLowerCase().replace(/_([a-z0-9])/g, (_, c) => c.toUpperCase())
}

// ---------------------------------------------------------------- 渲染：C#

/** 渲染 C# 的 P/Invoke 绑定（给 Windows 现有的 C# 壳用）。四条设计约定：
 *  ① `internal` 而不是 `public`（生成的类型不该被当 API 用，换实现只改这个文件）；② `SetLastError = false`（内核错误走 dsh_error，不查 Win32 GetLastError）；
 *  ③ `IntPtr` + 显式 UTF-8 编解码，不用 `[MarshalAs(...)]` 自动转换（那会藏住所有权，内核给的内存必须还给内核）；④ 不用插值字符串 —— 宿主语言版本可能只到 C# 5。 */
function renderCs(spec) {
  const m = spec.meta
  const L = []
  L.push(`// ${'='.repeat(74)}`)
  L.push(`// GENERATED — DO NOT EDIT.`)
  L.push(`// 由 tools/gen-bindings.mjs 从 abi/lookup.abi.json 生成（C# P/Invoke 绑定）。`)
  L.push(`// 壳只许通过这一层调内核：凡是内核也知道的常量，禁止在 C# 里再抄一份。`)
  L.push(`// ${'='.repeat(74)}`)
  L.push('')
  L.push('using System;')
  L.push('using System.Runtime.InteropServices;')
  L.push('using System.Text;')
  L.push('')
  L.push('namespace Lookup.Interop')
  L.push('{')
  L.push('    /// <summary>内核返回的错误码（0 = 成功）。取值与 native/include/dsh_lookup.h 一致。</summary>')
  L.push('    internal enum DshError')
  L.push('    {')
  for (const v of spec.enums.dsh_error.values) {
    const csName = v.name.replace(/^DSH_E_/, '').replace(/^DSH_/, '')
    L.push(`        /// <summary>${v.comment ?? ''}</summary>`)
    L.push(`        ${csName} = ${v.value},`)
  }
  L.push('    }')
  L.push('')
  // 除 dsh_error 之外的枚举（dsh_origin 等）也要给 C# 类型，
  // 否则带枚举参数的那条接口在 C# 里编不过（找不到那个类型）。
  for (const [name, def] of Object.entries(spec.enums)) {
    if (name === 'dsh_error') continue
    const csName = csEnumName(name)
    L.push(`    /// <summary>${(def.comment ?? '').replace(/\n/g, ' ')}</summary>`)
    L.push(`    internal enum ${csName}`)
    L.push('    {')
    for (const v of def.values) {
      L.push(`        /// <summary>${v.comment ?? ''}</summary>`)
      L.push(`        ${csValueName(v.name)} = ${v.value},`)
    }
    L.push('    }')
    L.push('')
  }
  L.push('    /// <summary>')
  L.push('    /// 内核的原始 P/Invoke 声明。**不要直接调这些方法** —— 用下面的包装：')
  L.push('    /// 包装负责"内核给的内存必须还给内核"这件事（每次取字符串都在 finally 里 dsh_release）。')
  L.push('    /// </summary>')
  L.push('    internal static class DshRaw')
  L.push('    {')
  L.push(`        internal const string Dll = "${m.soname}.dll";`)
  L.push('')
  L.push('        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]')
  L.push('        internal static extern IntPtr dsh_version();')
  L.push('        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]')
  L.push('        internal static extern int dsh_abi_version();')
  L.push('        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]')
  L.push('        internal static extern void dsh_release(IntPtr ptr);')
  L.push('        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]')
  L.push('        internal static extern IntPtr dsh_last_error_message();')
  L.push('')
  for (const fn of spec.functions) {
    if (fn.name === 'dsh_version' || fn.name === 'dsh_abi_version' || fn.name === 'dsh_release' ||
        fn.name === 'dsh_last_error_message') continue
    L.push(`        /// <summary>${(fn.comment ?? '').replace(/\n/g, ' ')}</summary>`)
    L.push(`        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, SetLastError = false)]`)
    L.push(`        internal static extern int ${fn.name}(${csParams(spec, fn)});`)
  }
  L.push('    }')
  L.push('')
  L.push('    /// <summary>')
  L.push('    /// 内核调用的包装层。内核与 Win32 一样，**从不抛异常**：')
  L.push('    /// 失败只体现为返回码。这里把失败翻译成 C# 异常，让壳的代码能正常用 try/catch；')
  L.push('    /// 需要"如实说原因"的场景（比如"这本词典为什么打不开"）请用 TryXxx 或读 LastError。')
  L.push('    /// </summary>')
  L.push('    internal static class Dsh')
  L.push('    {')
  L.push('        /// <summary>接口定义的 ABI 版本（常量）。壳启动时应当与内核报出来的对账。</summary>')
  L.push(`        internal const int AbiVersion = ${m.abiVersion};`)
  L.push('')
  L.push('        /// <summary>内核实际报出来的 ABI 版本；与 AbiVersion 不一致应当拒绝启动并给出可读原因。</summary>')
  L.push('        internal static int KernelAbiVersion() { return DshRaw.dsh_abi_version(); }')
  L.push('')
  L.push('        /// <summary>本线程最近一次失败的人话说明（内核那边取回、这里已释放）。</summary>')
  L.push('        internal static string LastError()')
  L.push('        {')
  L.push('            IntPtr p = DshRaw.dsh_last_error_message();')
  L.push('            try { return PtrToString(p); } finally { DshRaw.dsh_release(p); }')
  L.push('        }')
  L.push('')
  L.push('        /// <summary>内核版本字符串。</summary>')
  L.push('        internal static string Version()')
  L.push('        {')
  L.push('            IntPtr p = DshRaw.dsh_version();')
  L.push('            try { return PtrToString(p); } finally { DshRaw.dsh_release(p); }')
  L.push('        }')
  L.push('')
  L.push('        /// <summary>把内核返回的 UTF-8 指针读成字符串。**调用方负责释放那个指针。**</summary>')
  L.push('        internal static string PtrToString(IntPtr p)')
  L.push('        {')
  L.push('            if (p == IntPtr.Zero) return null;')
  L.push('            int len = 0;')
  L.push('            while (Marshal.ReadByte(p, len) != 0) len++;')
  L.push('            byte[] buf = new byte[len];')
  L.push('            Marshal.Copy(p, buf, 0, len);')
  L.push('            return Encoding.UTF8.GetString(buf);')
  L.push('        }')
  L.push('')
  L.push('        /// <summary>把 C# 字符串编成内核要的 UTF-8（以 \\0 结尾）。</summary>')
  L.push('        internal static IntPtr StringToPtr(string s)')
  L.push('        {')
  L.push('            if (s == null) return IntPtr.Zero;')
  L.push('            byte[] buf = Encoding.UTF8.GetBytes(s);')
  L.push('            IntPtr p = Marshal.AllocHGlobal(buf.Length + 1);')
  L.push('            Marshal.Copy(buf, 0, p, buf.Length);')
  L.push('            Marshal.WriteByte(p, buf.Length, 0);')
  L.push('            return p;')
  L.push('        }')
  L.push('')
  L.push('        /// <summary>失败时抛，带上内核给的人话原因。</summary>')
  L.push('        internal static void ThrowIfError(int rc)')
  L.push('        {')
  L.push('            if (rc == 0) return;')
  L.push('            string why = LastError();')
  L.push('            throw new InvalidOperationException(')
  L.push('                "内核返回 " + rc + "（" + (DshError)rc + "）" +')
  L.push('                (string.IsNullOrEmpty(why) ? "" : "：" + why));')
  L.push('        }')
  L.push('')
  L.push('        // ── 带类型的包装 ────────────────────────────────────────────────')
  L.push('        // 为什么还要这一层：DshRaw 里全是 IntPtr，调用方每次都要自己把字符串编成')
  L.push('        // UTF-8、再保证释放 —— 那正是最容易漏的一步。包装把"编解码 + 所有权"')
  L.push('        // 收进库里一处，壳的代码里就只剩下业务。')
  L.push('        // 生成的规则：入参 utf8 → string；出参 json → out string（内核那份已释放）。')

  // 按「入参 utf8→string 入、出参 json→out string」生成包装
  //
  // ⚠️ 这四个**不生成**包装（上面已手写好）：`dsh_release` / `dsh_version` / `dsh_last_error_message`
  //    （包装层自己的基础设施）与 `dsh_abi_version` —— 漏排它会生成与常量同名的 `AbiVersion()`，
  //    编 C# 时当场 CS0102（类型已经包含该成员的定义）。
  const wrappers = spec.functions.filter(
    (fn) =>
      fn.name !== 'dsh_release' &&
      fn.name !== 'dsh_version' &&
      fn.name !== 'dsh_last_error_message' &&
      fn.name !== 'dsh_abi_version',
  )
  for (const fn of wrappers) {
    const methodName = csMethodName(fn.name)
    const inParams = []
    const callArgs = []
    const releases = []   // 需要在 finally 里释放的入参指针
    for (const p of fn.params ?? []) {
      const def = typeDef(spec, p.type)
      const isOut = p.direction === 'out'
      if (isOut) {
        if (def?.kind === 'string') {
                    callArgs.push('out ' + p.name + 'Ptr')
        } else {
          // 非字符串出参（句柄、长度、字节缓冲）暂时只暴露原样，由壳按需处理
                    callArgs.push('out ' + p.name)
        }
        continue
      }
      if (def?.kind === 'string') {
        inParams.push('string ' + p.name)
        callArgs.push(p.name + 'Ptr')
        releases.push(p.name + 'Ptr')
      } else if (def?.kind === 'enum') {
        inParams.push(def.cs + ' ' + p.name)
        callArgs.push(p.name)
      } else {
        inParams.push(def?.cs + ' ' + p.name)
        callArgs.push(p.name)
      }
    }

    const retType = fn.returns === 'dsh_error' ? 'void' : (typeDef(spec, fn.returns)?.cs ?? 'int')
    const retDecl = retType === 'void' ? 'void' : retType
    // 出参的对外类型：与原始声明**共用同一套映射**（见 csParams 的注释）。
    // 字符串出参对外是 `out string`（内核那份在库里就释放掉）；其余原样透传。
    const outDecls = (fn.params ?? [])
      .filter((p) => p.direction === 'out')
      .map((p) => {
        const def = typeDef(spec, p.type)
        if (def?.kind === 'string') return `out string ${p.name}`
        if (def?.kind === 'handle' || def?.kind === 'buffer') return `out IntPtr ${p.name}`
        return `out ${def?.cs} ${p.name}`
      })
    L.push('')
    L.push(`        /// <summary>${(fn.comment ?? '').replace(/\n/g, ' ')}</summary>`)
    L.push(`        internal static ${retDecl} ${methodName}(${inParams.concat(outDecls).join(', ')})`)
    L.push('        {')
    for (const name of releases) {
      L.push(`            IntPtr ${name} = StringToPtr(${name.slice(0, -3)});`)
    }
    const stringOuts = (fn.params ?? [])
      .filter((p) => p.direction === 'out' && typeDef(spec, p.type)?.kind === 'string')
      .map((p) => p.name)
    for (const name of stringOuts) {
      L.push(`            IntPtr ${name}Ptr = IntPtr.Zero;`)
      L.push(`            ${name} = null;`)
    }
    L.push('            try')
    L.push('            {')
    const callArgsFinal = (fn.params ?? []).map((p) => {
      const def = typeDef(spec, p.type)
      if (p.direction === 'out') {
        return def?.kind === 'string' ? 'out ' + p.name + 'Ptr' : 'out ' + p.name
      }
      if (def?.kind === 'string') return p.name + 'Ptr'
      return p.name
    })
    // 返回形状有三种，别混（这一版混过一次：`dsh_engine_destroy` 的 C# 返回是 void，
    // 包装体里却写了 `return result;`，编译器直接报"返回关键字后面不得有对象表达式"）：
    //   · returns = dsh_error 或 void  → C# 返回 void，调原始声明后只检查错误、不返回东西
    //   · 其余（utf8 / i32 / …）      → C# 返回对应类型，取回结果返回
    const returnsValue = fn.returns !== 'dsh_error' && fn.returns !== 'void'
    if (fn.returns === 'dsh_error') {
      L.push(`                int rc = DshRaw.${fn.name}(${callArgsFinal.join(', ')});`)
      L.push('                ThrowIfError(rc);')
    } else if (returnsValue) {
      L.push(`                var result = DshRaw.${fn.name}(${callArgsFinal.join(', ')});`)
    } else {
      L.push(`                DshRaw.${fn.name}(${callArgsFinal.join(', ')});`)
    }
    for (const name of stringOuts) {
      L.push(`                ${name} = PtrToString(${name}Ptr);`)
      L.push('                // 取回来就还掉：内核给的内存必须还给内核（接口定义）')
      L.push(`                DshRaw.dsh_release(${name}Ptr);`)
      L.push(`                ${name}Ptr = IntPtr.Zero;`)
    }
    if (returnsValue) L.push('                return result;')
    L.push('            }')
    L.push('            finally')
    L.push('            {')
    for (const name of releases) {
      L.push(`                Marshal.FreeHGlobal(${name});`)
    }
    for (const name of stringOuts) {
      // 万一上面还没取（例如抛了异常），这里兜住，避免漏掉内核那份
      L.push(`                if (${name}Ptr != IntPtr.Zero) DshRaw.dsh_release(${name}Ptr);`)
    }
    L.push('            }')
    L.push('        }')
  }

  L.push('    }')
  L.push('}')
  return L.join('\n') + '\n'
}

/** C# 的参数列表。
 *
 * ⚠️ **原始声明与包装层必须共用同一套映射** —— 这一版把它们分头写过，于是
 * 原始声明里句柄出参是 `IntPtr`（其实是"指向句柄的指针"）、而包装层按 `out IntPtr`
 * 去调，编译器报"最匹配的重载具有一些无效参数"。规则收在一处的成本远低于这种错。
 *
 * 映射规则：
 *   入参 utf8/json → `IntPtr`（不是 string：UTF-8 的编解码与所有权要显式，不能交给自动 marshalling）
 *   入参 enum      → 生成的枚举类型
 *   出参 json/utf8 → `out IntPtr`（内核那份内存由调用方 dsh_release）
 *   出参 handle    → `out IntPtr`
 *   出参 其它标量  → `out <cs 类型>`
 */
function csParams(spec, fn) {
  return (fn.params ?? []).map((p) => {
    const def = typeDef(spec, p.type)
    if (p.direction === 'out') {
      if (def?.kind === 'handle' || def?.kind === 'string' || def?.kind === 'buffer') return `out IntPtr ${p.name}`
      return `out ${def?.cs ?? 'IntPtr'} ${p.name}`
    }
    if (def?.kind === 'enum') return `${def.cs} ${p.name}`
    return `${def?.cs ?? 'IntPtr'} ${p.name}`
  }).join(', ')
}

// ---------------------------------------------------------------- 渲染：文档

function renderDoc(spec) {
  const m = spec.meta
  const L = []
  L.push(`# 内核接口定义（ABI v${m.abiVersion} · ${m.name} ${m.version}）`)
  L.push('')
  L.push(`> 本文件由 \`tools/gen-bindings.mjs\` 从 [\`abi/lookup.abi.json\`](../../abi/lookup.abi.json) 生成。**不要手改。**`)
  L.push('')
  L.push(m.comment)
  L.push('')
  L.push('## 约定')
  L.push('')
  L.push(`- **内存**：${m.memoryRule}`)
  L.push(`- **错误**：${m.errorRule}`)
  L.push(`- **字符串**：${m.stringEncoding}，以 \`\\0\` 结尾。`)
  L.push(`- **调用约定**：${m.callingConvention}。`)
  L.push('')
  L.push('## 常量')
  L.push('')
  L.push('| 常量 | 值 | 说明 |')
  L.push('| --- | --- | --- |')
  for (const c of spec.constants.values) L.push(`| \`${c.name}\` | ${c.value} | ${c.comment ?? ''} |`)
  L.push('')
  if (spec.constants.separators) {
    L.push('### 音节分隔点')
    L.push('')
    L.push(spec.constants.separators.comment)
    L.push('')
    L.push('| 常量 | 码点 | 字符 | 说明 |')
    L.push('| --- | --- | --- | --- |')
    for (const v of spec.constants.separators.values) {
      L.push(`| \`${v.name}\` | U+${v.value.toString(16).toUpperCase().padStart(4, '0')} | \`${v.char}\` | ${v.comment ?? ''} |`)
    }
    L.push('')
  }
  L.push('## 枚举')
  L.push('')
  for (const [name, def] of Object.entries(spec.enums)) {
    L.push(`### \`${name}\``)
    L.push('')
    if (def.comment) { L.push(def.comment); L.push('') }
    L.push('| 取值 | 值 | 说明 |')
    L.push('| --- | --- | --- |')
    for (const v of def.values) L.push(`| \`${v.name}\` | ${v.value} | ${v.comment ?? ''} |`)
    L.push('')
  }
  L.push('## 接口')
  L.push('')
  const GROUP_TITLE = {
    core: '核心', engine: '引擎', lookup: '查词', text: '文本',
    speech: '发音', history: '历史', translate: '机器翻译', parser: '解析层',
  }
  const groups = []
  for (const fn of spec.functions) if (!groups.includes(fn.group)) groups.push(fn.group)
  for (const g of groups) {
    L.push(`### ${GROUP_TITLE[g] ?? g}`)
    L.push('')
    for (const fn of spec.functions.filter((f) => f.group === g)) {
      L.push(`#### \`${fn.name}\``)
      L.push('')
      if (fn.comment) { L.push(fn.comment); L.push('') }
      L.push('```c')
      const params = (fn.params ?? []).map((p) => cParam(spec, p)).join(', ')
      L.push(`${cType(spec, fn.returns)} ${fn.name}(${params || 'void'});`)
      L.push('```')
      L.push('')
      if ((fn.params ?? []).length) {
        L.push('| 参数 | 类型 | 方向 | 说明 |')
        L.push('| --- | --- | --- | --- |')
        for (const p of fn.params) {
          L.push(`| \`${p.name}\` | \`${p.type}\` | ${p.direction === 'out' ? '出' : '入'} | ${p.comment ?? ''} |`)
        }
        L.push('')
      }
      const notes = []
      if (fn.ownership === 'callee') notes.push(`出参由内核分配，调用方用 \`${fn.release}()\` 还给内核。`)
      if (fn.destroys) notes.push(`调用方用 \`${destroyName(fn.destroys)}()\` 关掉句柄。`)
      if (fn.since) notes.push(`ABI v${fn.since} 起可用。`)
      if (notes.length) { L.push(notes.join(' ')); L.push('') }
    }
  }
  return L.join('\n')
}

// ---------------------------------------------------------------- 输出

function rel(p) { return path.relative(ROOT, p).replace(/\\/g, '/') }

function writeOrCheck(file, content) {
  const abs = path.join(ROOT, file)
  const old = fs.existsSync(abs) ? fs.readFileSync(abs, 'utf8') : null
  const same = old === content
  if (CHECK) {
    if (!same) {
      const detail = old === null ? '文件不存在' : `内容不一致（旧 ${old.length} 字符 / 新 ${content.length} 字符）`
      err(`${file} 与接口定义不同步：${detail}`)
    }
    return { file, same, bytes: Buffer.byteLength(content, 'utf8') }
  }
  fs.mkdirSync(path.dirname(abs), { recursive: true })
  // 统一 LF、UTF-8 无 BOM —— 参考实现为"换行不统一"踩过
  fs.writeFileSync(abs, content.replace(/\r\n/g, '\n'), 'utf8')
  return { file, same, bytes: Buffer.byteLength(content, 'utf8') }
}

/**
 * 手写的内核头文件之间**不许有同名函数**。
 *
 * 为什么要有这条：`json_reader.c` 第一版把释放函数也叫 `dsh_json_free`，
 * 而写入器（`json_writer.c`）早就占了这个名字 —— 生成脚本与编译器**都不报错**，
 * 一直到链接期才以 `multiple definition of 'dsh_json_free'` 暴露出来，
 * 而那条报错指向的是链接器与两个 .o，不是"你撞名了"。
 *
 * 只查 `native/src/**` 下**手写**的头文件（生成的 `dsh_lookup.h` 不在内：
 * 它的名字由接口定义保证唯一，而"接口定义里两条同名的接口"该由接口定义校验挡）。
 * 一行里出现两次同名的（如 `int f(void); int f(void);`）只记一次。
 */
function checkHandwrittenHeaderCollisions() {
  const problems = []
  const seen = new Map() // 函数名 -> [头文件, …]
  const walk = (dir) => {
    for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
      const p = path.join(dir, e.name)
      if (e.isDirectory()) { walk(p); continue }
      if (!e.name.endsWith('.h')) continue
      if (e.name === 'dsh_lookup.h') continue // 自动生成的文件
      const rel = path.relative(ROOT, p).replace(/\\/g, '/')
      const text = fs.readFileSync(p, 'utf8')
      const names = new Set()
      for (const line of text.split('\n')) {
        // 只看"形如声明"的行：以 dsh_ 开头的标识符 + 一对圆括号 + 以 ; 收尾
        const m = line.match(/^\s*(?:[A-Za-z_][\w \t*]*?)\b(dsh_[a-z0-9_]+)\s*\([^;]*\)\s*;/)
        if (m) names.add(m[1])
      }
      for (const n of names) {
        if (!seen.has(n)) seen.set(n, [])
        seen.get(n).push(rel)
      }
    }
  }
  walk(path.join(ROOT, 'native/src'))
  for (const [name, files] of seen) {
    if (files.length > 1) problems.push(`  ${name}：${files.join(' 与 ')}`)
  }
  return problems
}

function main() {
  const spec = readSpec()

  if (LIST) {
    console.log(`接口定义 ${spec.meta.name} v${spec.meta.version} · ABI v${spec.meta.abiVersion}`)
    console.log(`类型 ${Object.keys(spec.types).length} · 枚举 ${Object.keys(spec.enums).length} · 常量 ${spec.constants.values.length}${spec.constants.separators ? ' + 分隔点 ' + spec.constants.separators.values.length : ''} · 接口 ${spec.functions.length}`)
    const groups = []
    for (const fn of spec.functions) if (!groups.includes(fn.group)) groups.push(fn.group)
    for (const g of groups) {
      const fns = spec.functions.filter((f) => f.group === g)
      console.log(`  ${g.padEnd(10)} ${String(fns.length).padStart(2)} 个`)
      for (const fn of fns) console.log(`      ${fn.name}`)
    }
    return
  }

  if (!validate(spec)) fail(errors)

  const outputs = [
    ['native/include/dsh_lookup.h', renderHeader(spec)],
    ['web/src/shared/abi.ts', renderTs(spec)],
    ['shell/Lookup.Interop/DshLookup.g.cs', renderCs(spec)],
    ['docs/api/abi.md', renderDoc(spec)],
  ]
  const shapeProblems = outputs.flatMap(([name, text]) => checkOutputShape(name, text))
  if (shapeProblems.length) {
    fail(['生成的产物形状不对（这是生成脚本自己的 bug，不是接口定义的问题）：', ...shapeProblems], CHECK ? 'check' : 'generate')
  }
  const dupProblems = checkHandwrittenHeaderCollisions()
  if (dupProblems.length) {
    fail(['手写的内核头文件之间有**同名函数**（会在链接期以 "multiple definition" 暴露出来，'
      + '而且报错指向的是链接器、不是撞名的那两处）：', ...dupProblems], CHECK ? 'check' : 'generate')
  }

  const written = outputs.map(([file, content]) => writeOrCheck(file, content))

  if (CHECK) {
    if (errors.length) fail(errors, 'check')
    console.log('接口定义与自动生成的文件一致：')
    for (const o of written) console.log(`  ✓ ${o.file} (${o.bytes} B)`)
    return
  }

  console.log(`从 abi/lookup.abi.json 生成 ${written.length} 个文件（接口定义校验 + 产物自检都通过）。`)
  for (const o of written) console.log(`  ${o.same ? '=' : '+'} ${o.file} (${o.bytes} B)`)
  console.log(`\n接口 ${spec.functions.length} 个 / 枚举 ${Object.keys(spec.enums).length} 个 / 常量 ${spec.constants.values.length} 个。`)
}

main()
