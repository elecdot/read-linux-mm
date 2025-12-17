---
related:
- "[Zoned Page Frame Allocator](zoned-page-frame-allocator.md)"
- "[Zone-Based Memory Management](zone-based-memory-management.md)"
- "[Page Descriptor (mem_map)](mm-core-variables.md)"
- "[Address Translation](address-translation.md)"
tags:
- memory-management
- high-memory
- kernel-mapping
- linear-address
- kmap
- page-frame
sources:
- "[include/linux/highmem.h](/linux/include/linux/highmem.h)"
- "[mm/highmem.c](/linux/mm/highmem.c)"
- "[mm/page_alloc.c](/linux/mm/page_alloc.c)"
---

/*! \page highmem-kernel-mapping Kernel Mappings of High-Memory Page Frames

This page introduces:
高端内存页框无法被直接映射到内核线性地址空间，需要通过临时映射机制（kmap/kunmap）来访问。
*/

# 高端内存页框的内核映射 (Kernel Mappings of High-Memory Page Frames)

## 一句话总结 (In a Word)

896 MB 以上的物理页框（ZONE_HIGHMEM）不被映射到内核线性地址空间。
必须通过 **动态映射**（Dynamic Mapping）机制，将高端物理页框临时映射到内核空间最后 128MB 中的特定区域（PKMAP/FIXMAP/VMALLOC），才能被内核访问。

## 核心问题：1GB 地址空间限制

在 32 位系统 4GB 地址空间中，内核只拥有 1GB（`0xC000_0000` ~ `0xFFFF_FFFF`）。

*   **低端内存 (Low Memory, 0~896MB)**: 直接映射（Direct Mapping），物理地址与虚拟地址固定偏移。
*   **高端内存 (High Memory, >896MB)**: 无固定映射，必须借用内核空间最后的 ~128MB 窗口进行访问。

### 内核地址空间布局 (高位 1GB)

```text
0xFFFF_FFFF ┌──────────────────────────┐
            │ FIXMAP (~32KB)           │ ← kmap_atomic (CPU独占, 极快)
            ├──────────────────────────┤
            │ PKMAP (2-4MB)            │ ← kmap (可能阻塞, 进程上下文)
            ├──────────────────────────┤
            │ VMALLOC (~120MB)         │ ← vmalloc (虚拟连续, 物理离散)
            ├──────────────────────────┤
0xC080_0000 │ 直接映射区 (896MB)       │ ← ZONE_NORMAL / ZONE_DMA
            ├──────────────────────────┤
0xC000_0000 │ 用户空间 (3GB)           │
0x0000_0000 └──────────────────────────┘
```

## 三种映射机制

### 1. 永久内核映射 (`kmap`)
*   **区域**: PKMAP (Page Kernel Mapping). 使用`pkmap_page_table`并定义
             了`LAST_PKMAP`--可用页表项(PTE,槽位)的总数
*   **大小**: 2MB (512 个槽位 `2MB = 512 * 4KB`) 或 4MB (1024 个槽位，PAE模式).
*   **行为**: 将页映射到虚拟地址。如果没有空闲槽位，**可能会睡眠**。
*   **适用**: 仅限进程上下文。
*   **API**: `void *kmap(struct page *page)` / `kunmap(page)`

### 2. 临时内核映射 (`kmap_atomic`)
*   **区域**: FIXMAP (Fixed Mapping).
*   **大小**: 非常小，每个 CPU 有固定数量的槽位 (KM_TYPE_NR)。
*   **行为**: **原子操作**，绝不睡眠。使用每 CPU 专用槽位。
*   **适用**: 中断处理程序、软中断 (softirqs)、临界区。
*   **API**: `void *kmap_atomic(page, type)` / `kunmap_atomic(vaddr, type)`

### 3. 非连续内存分配 (`vmalloc`)
*   **区域**: VMALLOC.
*   **大小**: ~120MB.
*   **行为**: 分配虚拟地址连续但物理页不连续的内存。
*   **适用**: 大块缓冲区、内核模块。开销较大（TLB 抖动）。
*   **API**: `void *vmalloc(size)` / `vfree(addr)`

## 决策指南

```text
当前上下文?
  ├─ 中断/原子上下文? ──→ kmap_atomic()
  └─ 进程上下文?
       ├─ 需要连续的虚拟地址数组? ──→ vmalloc()
       └─ 单页访问? ──→ kmap()
```

## 总结表

| 机制 | 适用上下文 | 能否睡眠? | 映射区域 | 限制 |
|------|-----------|----------|---------|------|
| **kmap** | 进程上下文 | 能 | PKMAP | 全局锁，槽位有限 |
| **kmap_atomic** | 任意 (含中断) | 否 | FIXMAP | 每 CPU 槽位，生命周期短 |
| **vmalloc** | 进程上下文 | 能 | VMALLOC | TLB 开销大，速度较慢 |
