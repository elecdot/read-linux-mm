---
related:
- "[Page Frame](../00-concepts/EXAMPLE.md)"
tags:
- source-map
- page-allocation
sources:
- "[mm/page_alloc.c](/linux/mm/page_alloc.c)"
---

# Source Map: mm/page_alloc.c

## File Overview

这个文件是 Linux 2.4 物理页分配系统的核心。它实现了 buddy system（伙伴系统）算法，负责分配与回收页框；并通过把物理内存划分成不同的 zone（DMA、Normal、HighMem）来适配不同的硬件限制。

## Key Data Structures

- **`struct zone_struct`**：表示一个内存区域（例如 `ZONE_NORMAL`），包含指向各级 `free_area` 链表的指针、同步锁以及统计信息。
- **`struct free_area`**：每个 zone 内的一个数组，管理不同阶次（order）的空闲页块；`free_area[0]` 管理 1 页块，`free_area[1]` 管理 2 页块，以此类推。
- **`struct page`**：表示单个物理页框的核心结构。`page_alloc.c` 通过其字段（如 `list`、`count` 等）跟踪页面状态。

## Core Functions

### `__alloc_pages()`

- **Purpose**：核心分配函数，用于获取一段连续的物理页框，实现 buddy 算法逻辑。
- **Inputs/Outputs**：
    - `gfp_mask`：控制分配行为的标志（例如 `GFP_KERNEL`、`__GFP_DMA`）。
    - `order`：请求的阶次，表示需要 2^order 个连续页。
    - Return：成功时返回第一个 `struct page` 指针，失败返回 `NULL`。
- **Typical Callers**：`alloc_pages()` 作为外部主要入口调用它。
- **Related Files**：`include/linux/mm.h`，`mm/slab.c`。

### `__free_pages()`

- **Purpose**：释放一段页块，并在可能时与相邻的“伙伴”合并成更大的连续块。
- **Inputs/Outputs**：
    - `page`：指向待释放的页块的首页。
    - `order`：该页块的阶次（包含 2^order 页）。
- **Typical Callers**：`free_pages()`。
- **Related Files**：`include/linux/mm.h`。
