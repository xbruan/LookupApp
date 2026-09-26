/* ==========================================================================
 * LZO1X 解压 —— MDict 的 CompressionType=lzo 词典靠它解词块与记录块。
 *
 * 为什么要自己写：C 标准库与 .NET 都没有内置的 LZO1X 解压，而 v1.2 格式（以及少数手机端
 * 工具生成的 v2.0 词典）用的就是它。
 *
 * 调用约定：
 *   · input / input_len 是**去掉 8 字节块头之后**的 LZO 流 —— MDX 的块头是 4 字节压缩
 *     类型 + 4 字节 adler32，解压器只看后面的流；
 *   · 成功返回 DSH_LZO1X_OK：*out_bytes 是 dsh_mem_alloc 出来的、**恰好** *out_len 字节
 *     的缓冲（*out_len 可以是 0，空结果也给一个合法指针，免得调用方特判 NULL）；
 *   · 失败返回非零错误码：*out_bytes 置 NULL、*out_len 置 0，原因由
 *     dsh_last_error_message() 给出（人话，中文）。
 *
 * 与本仓库其他模块的分工一致：本文件**不 include** 自动生成的 include/dsh_lookup.h，
 * 错误用 int 返回值 + dsh_set_last_error 表达，包装层再翻译成 DSH_E_*。
 *
 * 三条不许改的语义：
 *   1. **越界读一律当 0**（原版在数组尾巴之后读到 undefined，写进 Uint8Array 也就是 0）。
 *      C 里读越界是未定义行为，所以把这条语义落在 buf_at / out_at 两个取值函数里 ——
 *      只在损坏数据上才会走到，合法数据一个字节都不差；
 *   2. **输出上限 64MB**。超了按损坏数据报错，不去吃光内存；
 *   3. **遇到流结束标记就收工**，输出按实际写出的长度截断（不是按缓冲容量）。
 *
 * 唯一一处语义增补（只在非法数据上生效）：输入被截断时参考实现**不会终止**（实测 C# 版
 * 在单字节 0x00 上死循环，JS 原版跑满 60 秒也没返回）。C 里没有异常可以救出调用方，所以
 * 那一支改成「越界即报损坏」；合法数据走不到它（逃逸串后面必定还有一个非 0 字节）。
 * ========================================================================== */

#ifndef DSH_COMPRESS_LZO1X_H
#define DSH_COMPRESS_LZO1X_H

#include <stddef.h>
#include <stdint.h>

/* 错误码：本模块自己的小枚举，不占用对外接口定义的 DSH_E_* 编号 —— 包装层再翻译。 */
#define DSH_LZO1X_OK 0           /* 解压成功 */
#define DSH_LZO1X_E_INVALID_ARG 1 /* 参数不合法（空指针等），一个问题都不该由调用方猜 */
#define DSH_LZO1X_E_OOM 2         /* 内存不足（缓冲分不出来） */
#define DSH_LZO1X_E_CORRUPT 3     /* 数据损坏或超出 64MB 上限 */

/**
 * 解压一段 LZO1X 数据流。
 *
 * @param input      去掉 8 字节块头之后的 LZO 流（可以为 NULL，但此时 input_len 必须是 0）
 * @param input_len  流的字节数
 * @param out_bytes  出参：解出来的字节（dsh_mem_alloc 分配，调用方 dsh_release 还给内核）
 * @param out_len    出参：解出来的字节数（可以为 0）
 * @return DSH_LZO1X_OK 或上面那几个错误码
 */
int dsh_lzo1x_decompress(const uint8_t *input, size_t input_len,
                         uint8_t **out_bytes, size_t *out_len);

#endif /* DSH_COMPRESS_LZO1X_H */
