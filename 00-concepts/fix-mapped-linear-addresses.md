---
related:
- "[Temporary Kernel Mappings (kmap_atomic)](temporary-kernel-mappings.md)"
- "[Permanent Kernel Mappings (kmap)](permanent-kernel-mappings.md)"
- "[Kernel Mappings of High-Memory Page Frames](highmem-kernel-mapping.md)"
- "[Address Translation](address-translation.md)"
tags:
- memory-management
- kernel-mapping
- FIXMAP
- linear-address
- compile-time
sources:
- "[include/asm-i386/fixmap.h](/linux/include/asm-i386/fixmap.h)"
- "[arch/i386/mm/init.c](/linux/arch/i386/mm/init.c)"
---

/*! \page fix-mapped-linear-addresses Fix-Mapped Linear Addresses

This page introduces:
固定映射的线性地址（Fix-Mapped Linear Addresses）是内核地址空间中一组特殊的虚拟地址，其值在编译时就已确定，但可以动态地映射到任意物理地址。它们主要用于访问硬件寄存器（如 APIC）和实现高效的临时映射（kmap_atomic）。
*/

# 固定映射的线性地址 (Fix-Mapped Linear Addresses)

## 一句话总结 (In a Word)

**固定映射的线性地址**是一组在编译期预定义的常量虚拟地址，其对应的物理地址可以在运行时动态建立。它们位于内核线性地址空间的顶端（接近 `0xFFFFFFFF`），通过索引而非偏移量进行管理，避免了动态分配虚拟地址的开销。

## 为什么需要固定映射？ (Why This Concept)

在内核开发中，有些场景需要极其高效或在特定阶段访问内存：

1.  **编译期常量优化**：由于虚拟地址是常量，编译器可以直接将其硬编码到指令中，无需像动态分配（如 `vmalloc`）那样在运行时维护变量来存储地址。
2.  **早期启动访问**：在内存管理系统完全初始化之前，内核需要访问某些硬件（如 APIC）。固定映射提供了一种预定义的窗口。
3.  **原子性要求**：如 `kmap_atomic` 所需，固定映射允许在不睡眠的情况下快速建立映射，因为虚拟地址空间是预留好的，不存在“空间不足”导致的阻塞。

## 深入理解 (Deep Dive)

### 内存布局

固定映射区位于内核地址空间的最高端，紧随 `PKMAP` 区之后。

```text
0xFFFF_FFFF ┌──────────────────────────┐
            │ 预留 (通常 1 页)          │
            ├──────────────────────────┤ FIXADDR_TOP (0xFFFF_E000)
            │ FIXMAP 区域              │
            │ (从高向低增长)            │
            │ [Index 0: FIX_APIC_BASE] │
            │ [Index 1: FIX_IO_APIC]   │
            │ [Index n: FIX_KMAP_...]  │
            ├──────────────────────────┤ FIXADDR_START
            │ PKMAP 区域               │
            └──────────────────────────┘
```

### 核心机制：索引与转换

固定映射不使用简单的线性偏移（如 `__pa()` / `__va()`），而是使用一个枚举类型 `fixed_addresses` 作为索引。

#### 1. 索引定义 (`enum fixed_addresses`)
索引从 0 开始，但在虚拟地址空间中是从 `FIXADDR_TOP` **向低地址**排列的。

```c
// include/asm-i386/fixmap.h
enum fixed_addresses {
    FIX_APIC_BASE,      // 索引 0 -> 对应最高地址
    FIX_IO_APIC_BASE_0, // 索引 1
    // ...
    FIX_KMAP_BEGIN,     // 临时映射起始
    FIX_KMAP_END,
    __end_of_fixed_addresses
};
```

#### 2. 地址转换公式
虚拟地址的计算方式如下：
`Virtual Address = FIXADDR_TOP - (index << PAGE_SHIFT)`

在代码中由 `fix_to_virt()` 实现：
```c
static inline unsigned long fix_to_virt(const unsigned int idx) {
    return FIXADDR_TOP - (idx << PAGE_SHIFT);
}
```

### 建立映射：set_fixmap

内核通过 `set_fixmap(idx, phys)` 将一个物理页框关联到指定的固定映射索引上。

```c
// 核心实现通常在 arch/i386/mm/init.c 中的 __set_fixmap
void __set_fixmap (enum fixed_addresses idx, unsigned long phys, pgprot_t flags) {
    unsigned long address = __fix_to_virt(idx);
    pte_t *pte;

    // 1. 找到对应的页表项 (PTE)
    pte = kmap_get_fixmap_pte(address); 
    
    // 2. 设置 PTE
    if (phys) {
        set_pte(pte, pfn_pte(phys >> PAGE_SHIFT, flags));
    } else {
        pte_clear(pte);
    }

    // 3. 刷新 TLB
    __flush_tlb_one(address);
}
```

### 主要用途

1.  **硬件访问 (Fixed-purpose)**：
    -   `FIX_APIC_BASE`: 访问本地 CPU 的 APIC 寄存器。
    -   `FIX_IO_APIC_BASE_n`: 访问系统的 IO-APIC。
2.  **临时内核映射 (kmap_atomic)**：
    -   `FIX_KMAP_BEGIN` 到 `FIX_KMAP_END` 这一段区域被专门预留给 `kmap_atomic` 使用。
    -   每个 CPU 拥有自己的一组槽位，确保了并发访问的安全性。
3.  **启动参数与调试**：
    -   用于映射启动时的引导参数或特定的调试硬件。

## 固定映射 vs. 永久映射 (PKMAP)

| 特性 | 固定映射 (FIXMAP) | 永久映射 (PKMAP) |
| :--- | :--- | :--- |
| **地址确定性** | 编译时确定 (Constant) | 运行时确定 (Variable) |
| **管理方式** | 索引 (Index) | 链表/哈希表 + 引用计数 |
| **并发性** | CPU 私有槽位 (用于 kmap_atomic) | 全局共享 (需加锁) |
| **睡眠** | 绝不睡眠 | 可能睡眠 |
| **主要用途** | 硬件寄存器、中断内临时映射 | 进程上下文长时间映射 |

## 总结

**固定映射的线性地址**是 Linux 内核为了追求极致效率和满足特定硬件访问需求而设计的一种“硬编码”虚拟地址机制。它通过将虚拟地址空间的一部分“静态化”，简化了映射建立的过程，并为中断上下文访问高端内存提供了坚实的基础。
