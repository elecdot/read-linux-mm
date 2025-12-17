---
related:
- "[Buddy System](buddy-system.md)"
- "[Zone Selection Strategy](zone-selection-gfp.md)"
tags:
- memory-management
- physical-memory
- memory-zones
sources:
- "[include/linux/mmzone.h](/linux/include/linux/mmzone.h)"
- "[mm/page_alloc.c](/linux/mm/page_alloc.c)"
- "[Documentation/memory.txt](/linux/Documentation/memory.txt)"
---
/*! \page zone-based-memory-management Zone-Based Memory Management

This page introduces:
Linux 为了解决不同硬件设备对物理内存寻址能力的差异，将物理内存划分为不同的管理“区域”（Zone）。每个区域服务于特定的地址范围和用途，确保系统能高效、正确地为所有任务（包括内核和外设）分配内存。
*/

# Zone-Based Memory Management

## In a Word

Linux 把物理内存划分为不同的“区域”（zones），如 `ZONE_DMA`、`ZONE_NORMAL` 和 `ZONE_HIGHMEM`，以应对不同硬件（尤其是老旧设备）的寻址能力限制。内核按区域来管理和分配物理页框。

## Why This Concept

在计算机体系结构中，并非所有设备都能访问全部物理内存。例如，一些老的 ISA 设备只能对最低的 16MB 物理内存执行直接内存访问（DMA）。如果没有区域划分，内核可能会从任何地方分配内存给该设备，导致 DMA 操作失败。通过将内存划分为不同区域，内核可以确保为有特定地址限制的请求（如 DMA）分配来自正确区域（`ZONE_DMA`）的内存，同时允许普通进程和内核任务使用其他区域的内存，从而提高了内存利用率和系统兼容性。

## Deep Dive

在 Linux 2.4.18 中，主要有三个内存区域，定义在 `include/linux/mmzone.h` 中。这个数量由宏 `MAX_NR_ZONES` 指定，在编译时固定为 3，代表了内核支持的内存区域类型的最大数量。这三个区域是：

1.  **`ZONE_DMA`**:
    -   **范围**: 通常是物理内存的最低 16MB。
    -   **用途**: 专门为 ISA 设备等只能在低地址范围进行 DMA 操作的设备预留。当设备驱动请求 DMA 内存时，内核会优先从这个区域分配。

2.  **`ZONE_NORMAL`**:
    -   **范围**: 在 32 位 x86 系统上，通常指从 16MB 到 896MB 的物理内存。
    -   **用途**: 这是内核能够直接、永久映射到其虚拟地址空间的“常规”内存。大多数内核数据结构和进程页表都从这个区域分配。它是最常用、最宝贵的内存区域。

3.  **`ZONE_HIGHMEM`**:
    -   **范围**: 在 32 位 x86 系统上，指超出 896MB 的所有物理内存，即“高端内存”。
    -   **用途**: 由于 32 位内核的虚拟地址空间有限（通常为 1GB），无法直接映射所有的物理内存。因此，`ZONE_HIGHMEM` 中的内存不能被永久映射。当内核需要访问这部分内存时，必须临时创建映射（通过 `kmap()` 等机制），用完后立即解除，以便让出有限的地址空间给其他高端内存页使用。

每个内存区域（zone）都由一个 `struct zone_struct` 结构体表示，它内部包含独立的伙伴系统（Buddy System）来管理属于该区域的空闲页框。当内核收到内存分配请求时（例如通过 `kmalloc` 或 `__get_free_pages`），它会根据分配标志（GFP flags）决定从哪个区域开始尝试分配。通常，分配会从最受限制的区域开始，如果失败则转向更大、更通用的区域，这确保了稀缺的低地址内存被保留给真正需要它的设备。
