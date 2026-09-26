/* 活分配登记表 —— 实现「内核分配、内核释放」那条承诺的唯一手段。
 *
 * 为什么需要它：C 里没有可移植的办法给任意指针验身份 —— 想在指针前面放个魔数，
 * 可**外来指针**前面那段内存不属于我们，读它就是越界。所以只能登记在册：
 * 分配时记下地址与大小，释放时先查册。
 * 表满时**不覆盖**已有项：宁可分配失败报 OOM，也不许悄悄漏掉一个登记
 * （漏登记的指针在释放时会被当成外来的而拒绝释放，症状极难查）。
 * 登记表是全局的、内核可能被多线程调用，所以每次读写都要持锁；代价是每次 O(1)
 * 哈希 + 一把锁，而 `dsh_release` 只跑在对外接口的返回值上、不在热路径，可以接受。 */

#ifndef DSH_MEM_REGISTRY_H
#define DSH_MEM_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

/**
 * 登记一块由内核分配、将来可能交给宿主的记内存。
 *
 * @return 1 登记成功；0 登记失败（内存不足）。**调用方必须把这次分配当成失败**：
 *         没登记上的指针在 `dsh_release` 那边会被当成「外来的」而拒绝释放 —— 那就是一次悄悄泄漏。
 */
int dsh_mem_register(void *ptr, size_t size);

/** 这块内存是不是内核给的（会顺便校验大小是否相符）。是则**同时**把它从册上注销。 */
int dsh_mem_unregister(void *ptr, size_t *out_size);

/** 当前在册条数（测试与诊断用） */
size_t dsh_mem_live_count(void);

#endif /* DSH_MEM_REGISTRY_H */
