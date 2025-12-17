---
related:
- "[Zone Selection Strategy](zone-selection-gfp.md)"
- "[Buddy System](buddy-system.md)"
- "[Zone-Based Memory Management](zone-based-memory-management.md)"
- "[Page Flags](page-flags.md)"
tags:
- memory-management
- allocator
- page-frame
- watermarks
sources:
- "[mm/page_alloc.c](/linux/mm/page_alloc.c)"
- "[include/linux/mmzone.h](/linux/include/linux/mmzone.h)"
---

/*! \page zoned-page-frame-allocator The Zoned Page Frame Allocator

This page introduces:
分区页框分配器（Zoned Page Frame Allocator）是 Linux 内存管理的核心组件，负责处理所有的物理页分配请求。它位于伙伴系统之上，根据请求的标志（GFP flags）选择合适的内存区域（Zone），并根据内存水位（Watermarks）决定是直接分配、唤醒交换守护进程（kswapd）还是进行直接回收。
*/

# The Zoned Page Frame Allocator

## In a Word

分区页框分配器（Zoned Page Frame Allocator）是内核中负责分配物理页面的核心机制，其入口函数为 `__alloc_pages`。它接收分配请求（大小和标志），遍历预先构建的区域列表（Zonelist），在满足水位（Watermark）条件的前提下，调用伙伴系统（Buddy System）从特定区域分配内存。如果内存不足，它还负责触发页面回收机制。若请求是单个页面, @see "The Per-CPU Page Frame Cache".

@see figure8-2

## Why This Concept

单纯的伙伴系统只能在一个具体的内存区域内分配内存，而内核的内存请求是多样化的（如需要 DMA 内存、高端内存或普通内存）。分区页框分配器充当了"调度员"的角色：
1.  **策略与机制分离**：它将"从哪里分配"的策略（由 Zonelist 决定）与"如何分配"的机制（由 Buddy System 实现）结合起来。
2.  **内存压力管理**：它不仅仅是分配内存，还负责监控各个区域的内存压力。当空闲内存低于阈值时，它会启动回收流程，确保系统不会因为内存耗尽而崩溃。
3.  **回退机制**：当首选区域无法满足请求时，它能自动尝试备用区域，提高了分配的成功率。

## Deep Dive

### Requesting and releasing page frames (API层)

Page frames can be requested by using six slightly different functions and macros. Unless otherwise stated, they return the linear address of the first allocated page or return NULL if the allocation failed.

>[!important] 这里包括宏/函数，但逻辑上是同一层：*用户API接口*

```c
alloc_pages(gfp_mask, order)
alloc_page(gfp_mask)
__get_free_pages(gfp_mask, order)
__get_free_page(gfp_mask)
get_zeroed_page(gfp_mask)
__get_dma_pages(gfp_mask, order)
// Release functions / macros
__free_pages(page, order)
free_pages(addr, order)
__free_page(page)
__free_page(addr)
```

---

### 核心函数：`__alloc_pages`

所有的物理页分配请求（如 `alloc_pages`, `get_free_pages`）最终都会调用 `mm/page_alloc.c` 中的 `__alloc_pages` 函数。

```c
struct page * __alloc_pages(unsigned int gfp_mask, unsigned int order, zonelist_t *zonelist)
```

-   `gfp_mask`: 分配标志，描述了请求的性质（如是否可以睡眠、是否需要 DMA 等）。
-   `order`: 请求的阶数，即需要 $2^{order}$ 个连续页框。
-   `zonelist`: 允许尝试分配的区域列表，按优先级排序。

### 分配流程

分配器的工作流程可以分为两个主要阶段：**快速路径（Fast Path）** 和 **慢速路径（Slow Path）**。

#### 1. 快速路径 (Fast Path)

这是最理想的情况，系统内存充足。

1.  **遍历 Zonelist**：分配器从 `zonelist` 的第一个区域（首选区域）开始遍历。
2.  **检查水位 (Watermarks)**：对于每个区域，检查其空闲页数是否高于 `pages_low` 水位。
    -   如果 `free_pages > pages_low`，说明该区域内存相对充足。
3.  **尝试分配**：调用 `rmqueue()`（伙伴系统接口）尝试从该区域分配页框。
4.  **成功返回**：如果分配成功，直接返回页结构指针。

#### 2. 慢速路径 (Slow Path)

如果所有区域都无法满足快速路径的条件（即内存都低于 `pages_low`），分配器进入慢速路径，采取更积极的措施。

1.  **唤醒 kswapd**：首先唤醒后台页面交换守护进程 `kswapd`，让它异步地回收内存。
2.  **降低标准再次尝试**：
    -   重新遍历 Zonelist，这次使用更低的水位标准 `pages_min`。
    -   如果 `free_pages > pages_min`，尝试 `rmqueue()`。
3.  **直接回收 (Direct Reclaim)**：
    -   如果调用者允许等待（`gfp_mask & __GFP_WAIT`），当前进程会主动调用 `try_to_free_pages()`，尝试同步回收内存。
    -   回收后，再次尝试分配。
4.  **紧急分配**：
    -   如果是实时任务或设置了 `PF_MEMALLOC` 标志（如内存回收线程本身），允许突破 `pages_min` 水位进行分配，使用保留的紧急内存池。

### 关键数据结构回顾

-   **`zonelist_t`**: 包含了一个 `zone_t *` 数组，定义了分配尝试的顺序。例如，对于 `GFP_KERNEL`（普通内核分配），顺序通常是 `ZONE_NORMAL` -> `ZONE_DMA`。
-   **`zone_t` 水位**:
    -   `pages_min`: 绝对最小保留值，低于此值通常只允许紧急分配。
    -   `pages_low`: 触发 `kswapd` 回收的阈值。
    -   `pages_high`: `kswapd` 停止回收的目标阈值。

### 与其他组件的关系

-   **Buddy System**: 分区分配器决定"从哪个 Zone 分配"，而 Buddy System (`rmqueue`) 负责"在 Zone 内找到具体的空闲块"。
-   **Zone Selection**: `GFP` 标志决定了传入 `__alloc_pages` 的 `zonelist` 是哪一个。
-   **Kswapd**: 当分配器发现内存紧张时，它是触发内存回收机制的扳机。

## Reference Code

核心逻辑位于 `linux/mm/page_alloc.c`:

```c
// 伪代码简化版
struct page * __alloc_pages(...) {
    // 1. 第一次尝试：使用 pages_low 水位
    foreach (zone in zonelist) {
        if (zone->free_pages > zone->pages_low)
            if (page = rmqueue(zone, order)) return page;
    }

    // 2. 唤醒 kswapd
    wakeup_kswapd();

    // 3. 第二次尝试：使用 pages_min 水位
    foreach (zone in zonelist) {
        if (zone->free_pages > zone->pages_min)
            if (page = rmqueue(zone, order)) return page;
    }

    // 4. 只有允许等待才进行后续处理
    if (gfp_mask & __GFP_WAIT) {
        try_to_free_pages(gfp_mask);
        // 回收后再次尝试...
    }
    
    return NULL;
}
```
