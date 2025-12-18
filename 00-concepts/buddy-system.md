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

内核需要一个高效、可靠的物理内存分配器来满足各种大小的内存请求。简单的线性链表管理会导致严重的外部碎片问题（大量空间非连续地分散在各个小空间之中）；而伙伴系统通过阶数化的分组和自动合并，既能快速定位合适大小的内存块，又能在释放时主动合并相邻块，从而显著减少内存碎片。

### 为什么不选择直接把外部碎片映射成连续的？

虽然通过修改页表可以将不连续的物理页框映射到连续的线性地址空间，但内核仍然倾向于分配物理上连续的页框，原因如下：

1.  **硬件限制（如 DMA）**：某些硬件（如 ISA DMA）忽略分页机制，直接访问物理地址总线，因此要求缓冲区在物理内存中必须连续。
2.  **性能开销（TLB 刷新）**：频繁修改页表会导致 CPU 必须刷新 TLB（Translation Lookaside Buffer），增加内存访问延迟。
3.  **大页支持**：连续的物理内存允许内核使用 4 MB 的大页映射，显著减少 TLB 未命中次数，提升性能。

## Design Philosophy

为了在复杂的内核环境中保持高效和稳定，伙伴系统的设计遵循了以下几个宏观原则：

-   **确定性与可逆性**：拆分（Split）和合并（Coalesce）路径是完全确定且对称的。这种设计避免了内存状态的歧义，确保系统在频繁分配和释放后，总能通过合并恢复到初始的大块状态。
-   **延迟拆分与积极合并**：系统只在当前阶数无空闲块时才拆分大块（按需拆分），但在释放时会立即尝试与伙伴合并（积极合并）。这种“贪婪”的合并策略是为了尽可能保留大块连续内存，对抗外部碎片。
-   **管理区隔离（Zone Isolation）**：每个管理区（DMA, Normal, HighMem）拥有独立的伙伴系统实例。这使得内核可以针对不同的硬件约束独立管理内存，互不干扰。
-   **缓存局部性（LIFO 原则）**：在分配和释放时，系统倾向于操作链表的头部。刚释放的页框（热页）更有可能被立即重新分配，从而提高 CPU 缓存的命中率。
-   **极简元数据**：通过物理地址对齐和位图（Bitmap）而非复杂的树结构来管理状态，极大地节省了内核常驻内存的开销。

## Deep Dive

### 1. 核心原理：阶数与对齐

伙伴系统将空闲页框划分为 11 个链表，每个链表管理固定大小的内存块。块的大小被称为**阶数（Order）**：

-   **Order 0**: 1 page (4 KB)
-   **Order 1**: 2 pages (8 KB)
-   **Order 10**: 1024 pages (4 MB)
-   **对齐规则**：一个大小为 $2^k$ 个页框的块，其起始页框的物理地址必须是该块大小的整数倍。例如，16 个页框（64 KB）大小的块，其起始地址必须能被 $16 \times 4096$ 整除。

### 2. 核心数据结构

伙伴系统是基于管理区（Zone）实现的，每个 `zone_t` 都有自己的伙伴系统实例。

#### `free_area_t` (空闲区域)
定义在 [linux/include/linux/mmzone.h](linux/include/linux/mmzone.h#L21)：
```c
typedef struct free_area_struct {
	struct list_head	free_list;   // 该阶数所有空闲块的双向链表
	unsigned long		*map;        // 伙伴位图：用于追踪伙伴块的状态
} free_area_t;
```

在 `zone_t` 结构体中维护了一个数组：
```c
free_area_t free_area[MAX_ORDER]; // MAX_ORDER 通常为 10
```
**map详解**:

Linux 2.4 的位图设计非常精妙，它不记录单个块的分配状态，而是记录**一对伙伴的“平衡状态”**：

-   **映射关系**：每一位（1 bit）对应**一对伙伴**。
    -   对于阶数为 $k$ 的块，位图中的第 $n$ 位代表索引为 $n \cdot 2^{k+1}$ 和 $n \cdot 2^{k+1} + 2^k$ 的两个伙伴。
    -   在代码中通过 `(index) >> (1 + order)` 定位。
-   **状态含义**：
    -   **0**：伙伴双方状态一致（要么都空闲，要么都已分配）。
    -   **1**：伙伴双方状态不一（一个空闲，另一个已分配）。
-   **核心操作：取反 (Toggle)**：
    内核使用 `MARK_USED` 宏对位图进行翻转。无论是分配还是释放，只要操作了其中一个块，就对该位取反。
    -   **释放时**：如果取反后位由 `1` 变为 `0`，说明伙伴原本是空闲的，可以立即**合并**。
    -   **分配时**：如果取反后位由 `0` 变为 `1`，说明原本一致的状态（通常是全空闲）被打破。
-   **设计优势**：
    -   **节省空间**：位图大小仅为总页数的 $1/2$（按阶数递减）。
    -   **逻辑统一**：分配和释放共用一套取反逻辑，极大简化了状态机。

>最高阶没有位图(@see 见free_area_init_core())


### 3. 算法流程：分配 (Allocation)

当内核请求分配 $2^{order}$ 个页框时，调用 `rmqueue()`：

1.  **寻找空闲块**：从指定的 `order` 阶数开始，在 `free_area[order].free_list` 中查找。
2.  **向上追溯**：如果当前阶数没有空闲块，则查找 `order + 1`，以此类推，直到找到一个更大的空闲块。
3.  **大块拆分 (Split)**：通过 `expand()` 函数将大块递归拆分为两半（伙伴）。一半分配出去，另一半放回低一级的空闲链表中。

**示例**：请求 256 个页框（1 MB）：
-   若 256 阶链表为空，检查 512 阶。
-   若 512 阶有块，拆分为两个 256 阶块：一个分配，一个放入 256 阶链表。
-   若 512 阶也为空，检查 1024 阶，以此类推。

### 4. 算法流程：释放与合并 (Freeing & Coalescing)

当释放一个块时，调用 `__free_pages_ok()`，核心是**合并伙伴**：

1.  **寻找伙伴**：计算当前块的伙伴块地址。
2.  **判断合并条件**：
    -   两个块大小相同（同为 $b$）。
    -   物理地址连续。
    -   第一个块的起始地址必须是 $2 \times b$ 的整数倍。
3.  **递归合并**：如果伙伴也是空闲的，则将其从链表中移除，合并成一个 $2b$ 大小的块，并继续向上尝试合并。

### 5. 伙伴定位与位图算法

伙伴系统的精妙之处在于其高效的定位和状态追踪：

-   **伙伴定位 (XOR 技巧)**：
    对于阶数为 $k$、索引为 `idx` 的块，其伙伴的索引为 `idx ^ (1 << k)`。这仅仅是翻转了索引中的一个位。
-   **位图优化 (`MARK_USED`)**：
    为了节省空间，Linux 2.4 的位图 `map` 中，**每 1 位代表一对伙伴**。
    -   当一对伙伴中有一个被分配或释放时，对该位执行**取反**操作。
    -   如果取反后该位为 `1`：说明现在这对伙伴中有一个是空闲的。
    -   如果取反后该位为 `0`：说明现在这对伙伴要么全被占用，要么全空闲（在释放流程中，这表示可以合并）。

### 6. 性能与限制

-   **时间复杂度**：分配和释放均为 $O(\log N)$，其中 $N$ 为最大阶数。
-   **内部碎片**：由于只能分配 2 的幂次方大小，如果请求 5 个页框，必须分配 8 个，造成 3 个页框的浪费。
-   **外部碎片**：虽然有合并机制，但长期运行后仍可能出现大量不连续的小块，导致无法分配大块。

## Related Concepts

- **[Zone-Based Memory Management](zone-based-memory-management.md)**：伙伴系统在每个区域内独立运作。
- **[The Zoned Page Frame Allocator](zoned-page-frame-allocator.md)**：伙伴系统之上的高层分配接口。
- **[Page Flags](page-flags.md)**：页框的状态标志（如 `PG_referenced`）与分配逻辑相关。

## Code References

- **核心分配逻辑**：[linux/mm/page_alloc.c - `rmqueue()`](linux/mm/page_alloc.c)
- **大块拆分**：[linux/mm/page_alloc.c - `expand()`](linux/mm/page_alloc.c)
- **释放与合并**：[linux/mm/page_alloc.c - `__free_pages_ok()`](linux/mm/page_alloc.c)
- **数据结构定义**：[linux/include/linux/mmzone.h - `free_area_t`](linux/include/linux/mmzone.h)
