---
related:
- "[Slab Allocator](./slab-allocator.md)"
- "[Address Translation](./address-translation.md)"
- "[Core MM Variables](./mm-core-variables.md)"
- "[Buddy System](./buddy-system.md)"
tags:
- memory-management
- virtual-memory
- vma
- vmalloc
sources:
- "[linux/include/linux/mm.h](/linux/include/linux/mm.h)"
- "[linux/include/linux/vmalloc.h](/linux/include/linux/vmalloc.h)"
- "[linux/mm/mmap.c](/linux/mm/mmap.c)"
- "[linux/mm/vmalloc.c](/linux/mm/vmalloc.c)"
---

/*! \page memory-area-management Memory Area Management

This page introduces:
Linux 内核如何管理虚拟地址空间中的连续区域。这包括进程地址空间中的虚拟内存区域（VMA）以及内核地址空间中的非连续内存区域（vmalloc）。这些机制允许内核以“区域”为单位高效地组织和保护内存。
*/

# Memory Area Management

## In a Word

Memory Area Management（内存区域管理）是 Linux 内核管理虚拟地址空间中连续区间的机制。它主要由两部分组成：用于进程地址空间的 **Virtual Memory Areas (VMA)** 和用于内核地址空间的 **Non-contiguous Memory Areas (vmalloc)**。

## Why This Concept

内存管理不仅涉及物理页框的分配，还涉及如何将这些页框组织成对进程或内核有意义的“区域”：
- **进程视角**：进程需要将地址空间划分为代码段、数据段、堆栈等，每个段有不同的访问权限（读、写、执行）。VMA 提供了这种抽象。
- **内核视角**：当物理内存高度碎片化，无法分配大块连续物理内存时，内核可以通过 vmalloc 在虚拟地址空间中“拼凑”出一块连续区域，这对于加载模块或大型缓冲区非常有用。

## Deep Dive

### 1. Virtual Memory Areas (VMA)

在进程地址空间中，每个连续的虚拟内存区间由 `struct vm_area_struct` 表示。

- **数据结构**：
    - **链表**：所有 VMA 按地址顺序链接，方便遍历（`vm_next`）。
    - **红黑树**：为了提高查找特定地址所属 VMA 的效率，2.4.18 内核使用了红黑树（`vm_rb`）。
- **核心属性**：
    - `vm_start`, `vm_end`：定义了区域的边界。
    - `vm_page_prot`：定义了该区域的访问权限。
    - `vm_ops`：指向一组函数指针，用于处理该区域的特定操作（如缺页异常处理 `nopage`）。

### 2. Non-contiguous Memory Areas (vmalloc)

内核有时需要分配大块内存，但并不要求物理上连续。`vmalloc` 机制通过修改页表，将不连续的物理页框映射到内核虚拟地址空间的连续区间（通常在 `VMALLOC_START` 到 `VMALLOC_END` 之间）。

- **数据结构**：
    - `struct vm_struct`：描述一个 vmalloc 区域。
    - **链表**：所有的 `vm_struct` 通过 `next` 指针链接在一起。
- **特点**：
    - **灵活性**：可以分配比伙伴系统能提供的最大连续块还要大的内存。
    - **开销**：由于需要修改页表并可能导致 TLB 抖动，其性能略低于直接映射的内存（如 `kmalloc`）。

### 3. 区别与联系

| 特性 | VMA (`vm_area_struct`) | vmalloc (`vm_struct`) |
| :--- | :--- | :--- |
| **所属空间** | 用户进程地址空间 | 内核虚拟地址空间 |
| **物理连续性** | 通常不连续（按需分配） | 不连续 |
| **查找结构** | 链表 + 红黑树 | 简单链表 |
| **主要用途** | 内存映射、堆栈、代码段 | 内核模块、大型缓冲区 |
