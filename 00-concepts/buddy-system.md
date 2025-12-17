```markdown
---
related:
- "[The Zoned Page Frame Allocator](zoned-page-frame-allocator.md)"
- "[Zone-Based Memory Management](zone-based-memory-management.md)"
- "[Page Flags](page-flags.md)"
tags:
- memory-management
- page-allocation
- physical-memory
- allocator
sources:
- "[linux/mm/page_alloc.c](/linux/mm/page_alloc.c)"
- "[include/linux/mmzone.h](/linux/include/linux/mmzone.h)"
---
/*! \page buddy-system Buddy System

This page introduces:
伙伴系统（Buddy System）是 Linux 内核中用于管理和分配物理页框的核心机制。它通过将页框分组为 2 的幂次方（order），按层级（树形）组织内存块，实现高效的分配与回收，同时自动合并相邻的空闲块以减少碎片。
*/

# Buddy System

## In a Word

伙伴系统（Buddy System）是 Linux 内核用于管理物理页框分配和释放的经典算法。它将页框按 2 的幂次方大小分组，维护多个不同"阶数"（order）的空闲页框链表，支持快速分配、高效回收和自动合并相邻空闲块的功能。

## Why This Concept

内核需要一个高效、可靠的物理内存分配器来满足各种大小的内存请求。简单的线性链表管理会导致严重的外部碎片问题；而伙伴系统通过阶数化的分组和自动合并，既能快速定位合适大小的内存块，又能在释放时主动合并相邻块，从而显著减少内存碎片。这使其成为现代操作系统内存管理的标准选择。

## Deep Dive

### 核心概念：阶数与块大小

伙伴系统的关键思想是将页框按 2 的幂次方分组。每个分组大小被称为一个"阶数"（order）：

- **Order 0**: 1 page (4 KB)
- **Order 1**: 2 pages (8 KB)
- **Order 2**: 4 pages (16 KB)
- **Order n**: $2^n$ pages ($2^n \times 4\text{ KB}$)

在 Linux 2.4.18 中，`MAX_ORDER` 通常定义为 10，即最大支持 $2^{10} = 1024$ 个页框（约 4 MB）的连续块分配。

### 数据结构

伙伴系统的核心数据结构是 `free_area_t`，定义在 `include/linux/mmzone.h` 中：

```c
typedef struct free_area_struct {
	struct list_head	free_list;   // 该阶数所有空闲块的链表头
	unsigned long		*map;        // 位图，标记该阶数块的分配/空闲状态
} free_area_t;
```

在每个内存区域（zone）的 `zone_struct` 中，维护了一个大小为 `MAX_ORDER` 的数组：

```c
free_area_t		free_area[MAX_ORDER];
```

每个 `free_area[i]` 管理所有 order 为 i 的空闲页框块。

### 分配过程（Allocation）

当内核请求分配 2^order 个连续页框时：

1. **查找合适的块**：首先在 `free_area[order]` 的链表中寻找一个空闲块。
   
2. **向上查找**：如果 order 级别没有空闲块，依次尝试更大的阶数 `free_area[order+1]`、`free_area[order+2]` 等，直到找到一个空闲块。

3. **分割大块**（Split）：找到大块后，通过 `expand()` 函数逐级分割：
   - 将找到的大块从其链表中移除
   - 将其分为两个"伙伴"块，一个分配给请求者，另一个放回较小阶数的空闲链表中
   - 递归进行分割，直到达到所需的 order

### 释放过程（Freeing）

当释放一个已分配的页框块时，通过 `__free_pages_ok()` 函数执行：

1. **初始化释放状态**：将该块标记为空闲，添加到对应阶数的空闲链表中。

2. **检查并合并伙伴**（Coalescing）：
   - 计算该块的"伙伴"块位置（通过 XOR 操作 page_idx）
   - 检查伙伴块是否也空闲（通过位图查询）
   - 若伙伴块空闲，则两块合并成一个更大的块，移除伙伴块，将合并后的块添加到更高阶数的链表中
   - 向上递归进行合并，直到找到已分配的伙伴或达到最高阶数

```c
// 伙伴计算示例（在 __free_pages_ok 中）
buddy1 = base + (page_idx ^ -mask);  // XOR 运算得到伙伴块的位置
```

### 伙伴定位算法

伙伴块的位置由逻辑计算确定，无需额外信息：

- 对于 order o 的块，包含 $2^o$ 个页框
- 块的起始页框索引为 `page_idx`
- 伙伴块的起始页框索引为 `page_idx ^ (1 << (o+1))`，即翻转第 (o+1) 位

这个设计不仅节省了存储空间，还使伙伴查找成为常数时间操作。

### 时间与空间复杂度

- **分配时间**：最坏情况 $O(\log N)$，其中 N 是最大块大小相对于页框大小的比值
- **释放时间**：最坏情况 $O(\log N)$，取决于合并链的长度
- **内存开销**：每个阶数需要一个位图，总开销约为 $\sum_{i=0}^{MAX\_ORDER-1} \frac{\text{zone\_size}}{2^i} \text{ bits}$，实际上相对较小

### 与Zone的关联

每个内存区域（zone）都有独立的伙伴系统实例。这样的设计允许内核为不同用途（DMA、普通、高端内存）维护独立的分配池，符合区域化内存管理的设计理念。

### 限制与改进

- **外部碎片**：虽然伙伴系统通过合并减少碎片，但仍可能存在外部碎片，尤其在某些大小的请求模式下
- **内部碎片**：分配 2^order 个页框时，若实际需求不足，会产生浪费
- **改进方案**：Linux 2.6+ 后引入了 **Slab 分配器**和 **Slub 分配器** 等更细粒度的分配机制，用于小对象分配；伙伴系统主要负责页级别的分配

## Related Concepts

- **[Zone-Based Memory Management](zone-based-memory-management.md)**：伙伴系统在每个区域内独立运作。
- **[Page Flags](page-flags.md)**：页框的状态标志与伙伴系统的操作息息相关。
- **Free Area Structure**：伙伴系统的核心容器，存储不同阶数的空闲块。

## Code References

- **核心实现**：[linux/mm/page_alloc.c - `__free_pages_ok()`](linux/mm/page_alloc.c)（释放与合并）
- **快速路径**：[linux/mm/page_alloc.c - `rmqueue()`](linux/mm/page_alloc.c)（分配）
- **分割函数**：[linux/mm/page_alloc.c - `expand()`](linux/mm/page_alloc.c)（大块分割）

```