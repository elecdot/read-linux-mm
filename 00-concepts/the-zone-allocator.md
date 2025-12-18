---
related:
- "[Zoned Page Frame Allocator](zoned-page-frame-allocator.md)"
- "[Zone-Based Memory Management](zone-based-memory-management.md)"
- "[Buddy System](buddy-system.md)"
- "[Zone Selection Strategy](zone-selection-gfp.md)"
tags:
- memory-management
- allocator
- physical-memory
sources:
- "[mm/page_alloc.c](/linux/mm/page_alloc.c)"
- "[include/linux/mmzone.h](/linux/include/linux/mmzone.h)"
---

/*! \page the-zone-allocator The Zone Allocator

This page introduces:
分区分配器（Zone Allocator）是 Linux 物理内存管理的核心，它在伙伴系统（Buddy System）之上提供了一层逻辑，负责根据内存区域（Zones）的划分来处理页框的分配请求。它确保了不同硬件限制下的内存需求（如 DMA）能得到满足，并维护了系统的内存水位平衡。
*/

# The Zone Allocator

## In a Word

分区分配器（Zone Allocator）是 **Linux 物理内存分配的顶层接口**。它将物理内存划分为不同的管理区域（Zones），并根据分配请求的标志（GFP flags）和各区域的空闲水位（Watermarks），决定从哪个区域调用伙伴系统来获取物理页框。
正如在 [分区页框分配器](./zoned-page-frame-allocator.md) 中提到的：“对一组连续页框的所有请求最终都通过执行 `alloc_pages` 宏来处理。而这个宏最终会调用 `__alloc_pages()` 函数，它是分区分配器的核心。”

## Why This Concept

在 Linux 内核中，直接操作伙伴系统是不够的，因为伙伴系统通常只管理单一连续的内存空间。而现代计算机的物理内存往往具有异构性（如 DMA 限制、高端内存）。Zone Allocator 的存在解决了以下问题：
1.  **硬件兼容性**：通过 `ZONE_DMA` 满足老旧设备的寻址需求（“如果可能的话，它应该保护细小且珍贵的 `ZONE_DMA` 内存区域”）。
2.  **分配策略**：通过 `zonelist` 定义了分配失败时的回退路径（Fallback path）。
3.  **内存压力控制**：通过水位线（Watermarks）机制，在内存不足时触发 `kswapd` 或直接回收（当内存匮乏且允许阻塞当前进程时，在释放一些页框后重试分配）。
> 它应该保护预留页框池（参见前一节）。

## Deep Dive

Zone Allocator 实际上是 **分区页框分配器（Zoned Page Frame Allocator）** 的常用简称。它的核心逻辑封装在 `__alloc_pages` 函数中。

### 核心组件
-   **Zones**: 物理内存的逻辑划分（DMA, Normal, HighMem）。
-   **Zonelists**: 定义了分配尝试的优先级顺序。
-   **Watermarks**: 每个 Zone 维护的 `min`, `low`, `high` 三个水位线，用于衡量内存压力。
-   **Buddy System**: Zone Allocator 最终调用的底层分配引擎。

### 分配逻辑简述
当一个分配请求到达时，Zone Allocator 会：
1.  根据 `gfp_mask` 选择合适的 `zonelist`。
2.  按顺序扫描 `zonelist` 中的每个 Zone。
3.  检查该 Zone 的空闲页数是否满足水位要求。
4.  如果满足，调用伙伴系统的 `rmqueue` 函数进行分配。
5.  如果不满足，则根据策略决定是回退到下一个 Zone，还是唤醒 `kswapd` 进行回收。

*逻辑概括为*：
1. 温和尝试：看哪个 Zone 内存多（pages_low）。
2. 求助：唤醒 kswapd。
3. 强硬尝试：动用预留内存（pages_min）。
4. **亲自下场**：如果允许等待，自己去回收页面（balance_classzone）。
5. 特权阶层：如果是回收进程本身或 OOM 受害者，无视规则直接拿。

有关详细的函数调用流程和代码分析，请参阅 [Zoned Page Frame Allocator](zoned-page-frame-allocator.md)。
