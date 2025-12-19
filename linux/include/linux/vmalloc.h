#ifndef __LINUX_VMALLOC_H
#define __LINUX_VMALLOC_H

#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/spinlock.h>

#include <asm/pgtable.h>

/* bits in vm_struct->flags */
#define VM_IOREMAP	0x00000001	/* ioremap() and friends */
#define VM_ALLOC	0x00000002	/* vmalloc() */

/**
 * @brief 非连续内存区域描述符 (vm_struct)
 * 
 * 该结构体用于管理内核虚拟地址空间中的非连续内存区域（即 vmalloc 区域）。
 * 每一个通过 vmalloc() 或 ioremap() 申请的区域都会对应一个 vm_struct 实例。
 * 
 * **管理机制**：
 * - 所有的 vm_struct 实例都存放在一个全局单向链表 `vmlist` 中。
 * - 链表按虚拟地址 `addr` 从小到大排序，方便在 `get_vm_area()` 中查找空闲的地址空洞。
 * - 该链表由全局读写锁 `vmlist_lock` 保护。
 * 
 * **安全特性**：
 * - `size` 字段通常比实际请求的大小多出一个 PAGE_SIZE (4KB)。
 * - 这个额外的页面被称为 "Guard Page"（安全空洞），它不建立物理映射，用于捕捉缓冲区溢出。
 */
struct vm_struct {
	unsigned long flags;     //!< 区域标志：VM_ALLOC (vmalloc 分配) 或 VM_IOREMAP (硬件映射)。
	void * addr;             //!< 该区域在内核虚拟地址空间中的起始地址。
	unsigned long size;      //!< 区域的总大小（包含末尾 4KB 的 Guard Page）。
	struct vm_struct * next; //!< 指向下一个非连续内存区域描述符，构成全局链表 vmlist。
};

extern struct vm_struct * get_vm_area (unsigned long size, unsigned long flags);
extern void vfree(void * addr);
extern void * __vmalloc (unsigned long size, int gfp_mask, pgprot_t prot);
extern long vread(char *buf, char *addr, unsigned long count);
extern void vmfree_area_pages(unsigned long address, unsigned long size);
extern int vmalloc_area_pages(unsigned long address, unsigned long size,
                              int gfp_mask, pgprot_t prot);

/*
 *	Allocate any pages
 */
 
/**
 * @brief 分配非连续内存区域 (标准入口)
 * 
 * 这是内核中最常用的非连续内存分配接口。
 * 
 * **默认参数解析**：
 * 1. gfp_mask: `GFP_KERNEL | __GFP_HIGHMEM`
 *    - 允许在分配过程中睡眠（GFP_KERNEL）。
 *    - 优先从高端内存（Highmem）区域分配物理页，以节省宝贵的低端内存（Normal zone）。
 * 2. prot: `PAGE_KERNEL`
 *    - 设置标准的内核读写权限。
 * 
 * @param size 请求分配的大小。
 * @return void* 返回分配到的线性地址，失败返回 NULL。
 */
static inline void * vmalloc (unsigned long size)
{
	return __vmalloc(size, GFP_KERNEL | __GFP_HIGHMEM, PAGE_KERNEL);
}

/*
 *	Allocate ISA addressable pages for broke crap
 */

/**
 * @brief 分配适用于 DMA 的非连续内存区域
 * 
 * 强制从 ZONE_DMA 区域分配物理页，用于那些有物理寻址限制的旧设备。
 */
static inline void * vmalloc_dma (unsigned long size)
{
	return __vmalloc(size, GFP_KERNEL|GFP_DMA, PAGE_KERNEL);
}

/*
 *	vmalloc 32bit PA addressable pages - eg for PCI 32bit devices
 */
 
static inline void * vmalloc_32(unsigned long size)
{
	return __vmalloc(size, GFP_KERNEL, PAGE_KERNEL);
}

/*
 * vmlist_lock is a read-write spinlock that protects vmlist
 * Used in mm/vmalloc.c (get_vm_area() and vfree()) and fs/proc/kcore.c.
 */
extern rwlock_t vmlist_lock;

extern struct vm_struct * vmlist;
#endif

