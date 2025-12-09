---
related:
- "[Zone-Based Memory Management](./zone-based-memory-management.md)"
- "[Page Frame](./EXAMPLE.md)"
tags:
- memory-management
- boot-time-allocation
- physical-memory
sources:
- "[linux/mm/bootmem.c](/linux/mm/bootmem.c)"
- "[linux/include/linux/bootmem.h](/linux/include/linux/bootmem.h)"
---

/*! \page bootmem Bootmem Allocator

This page introduces:
Bootmem（启动内存分配器）是 Linux 内核在早期启动阶段使用的一个简单物理内存分配器。它通过位图跟踪页框分配状态，支持页框合并优化和 NUMA 感知，在启动过程中管理内存，待主内存管理系统就绪后完全卸载。
*/

# Bootmem Allocator

## In a Word

Bootmem（启动内存分配器）是 Linux 内核在早期启动阶段使用的一个简单物理内存分配器。它用来处理保留的系统内存、内存空洞，以及在主内存管理系统（如伙伴分配器）就绪前进行内存分配。

## Why This Concept

在内核启动过程中，复杂的内存管理系统（如伙伴分配器、SLUB 分配器等）还未初始化。此时内核需要为各种启动时结构（页表、进程表、zone 描述符等）分配内存。Bootmem 提供了一个轻量级的解决方案，在这个阶段之后就被完全丢弃，从而避免在主内存分配器中承载启动时的复杂性。

## Deep Dive

### 核心数据结构

Bootmem 使用**位图（bitmap）**来跟踪内存页框的分配状态。每一位代表一个物理页框：
- **0（未设置）** = 该页框空闲或未保留
- **1（已设置）** = 该页框已被分配或保留

相比链表或树形结构，位图实现简单、开销小，非常适合启动阶段的低资源环境。

```c
bootmem_data_t {
    unsigned long *node_bootmem_map;  // 位图指针
    unsigned long node_boot_start;    // 该 node 内存开始地址
    unsigned long node_low_pfn;       // 该 node 内存末尾的页框号
    unsigned long last_pos;           // 上次分配末尾的页框号（用于合并）
    unsigned long last_offset;        // 上次分配末尾的偏移（用于合并）
}
```

### 核心操作

**1. 初始化（`init_bootmem_core`）**
- 初始化位图为全 1（所有页框初始状态为"保留"）
- 建立该 node 的启动内存元数据
- 关联该 node 到全局 `pgdat_list`

**2. 保留内存（`reserve_bootmem_core`）**
- 标记指定地址范围的页框为已保留（设置位图的相应位为 1）
- 用于标记硬件保留区、BIOS 数据区等不可用的内存

**3. 分配内存（`__alloc_bootmem_core`）**
- 扫描位图找到足够数量的连续空闲页框
- 支持 `goal` 参数（优先在目标地址上方分配）和 `align` 参数（对齐约束）
- **关键特性：页框合并**（见下文）

**4. 释放内存（`free_bootmem_core`）**
- 清除位图中相应位，标记页框为空闲
- 启动时通常在初始化完成后调用，释放启动过程中动态分配的临时结构

**5. 回收所有启动内存（`free_all_bootmem_core`）**
- 扫描所有页框，将空闲页框加入主内存管理系统
- 释放位图本身占用的页框
- 清空 `node_bootmem_map` 指针，彻底卸载 bootmem 分配器

### 页框合并优化

Bootmem 的一个重要设计是**合并策略**：

当新分配请求紧接着前一次分配时（即新分配的起始页框 == 前一次分配的末尾页框 + 1），分配器会尝试在同一个物理页内重用前一次分配的剩余空间：

```c
if (align <= PAGE_SIZE
    && bdata->last_offset && bdata->last_pos+1 == start) {
    // 新分配可以与前一次分配的末尾合并，节省一个完整页框
    offset = (bdata->last_offset+align-1) & ~(align-1);
    remaining_size = PAGE_SIZE - offset;
    if (size < remaining_size) {
        // 新分配的大小 < 剩余空间，填充在同一页内
        bdata->last_offset = offset + size;
    } else {
        // 需要额外页框
        areasize = (remaining_size-size+PAGE_SIZE-1)/PAGE_SIZE;
        bdata->last_pos = start + areasize - 1;
        bdata->last_offset = (size - remaining_size);
    }
}
```

**权衡分析**：
- **大内存系统**：合并可能导致页内碎片（浪费小数部分页框），但总体内存充足，这种浪费可接受。
- **小内存系统**：页框对齐良好，通常能实现 100% 的利用率。

### 分配策略

1. **目标感知**：优先在 `goal` 地址上方分配，其次在低地址分配。
2. **两轮扫描**：
   - 第一轮：从 `preferred` 位置开始，以 `align` 为步长扫描
   - 若失败且之前设置了 `preferred`，重置为偏移量重新扫描全范围
3. **地址对齐**：确保分配块符合指定的对齐要求。

### 节点支持

Bootmem 支持 NUMA 和非连续内存架构：
- 每个内存 node 有独立的 `bootmem_data_t` 结构和位图
- `__alloc_bootmem` 会遍历所有 node，从第一个能满足分配的 node 分配
- `__alloc_bootmem_node` 在指定 node 上分配

### 生命周期

```
启动初期
  ↓
init_bootmem() ──→ 初始化位图，标记所有页为保留
  ↓
setup_arch() ──→ 调用 reserve_bootmem() 标记硬件保留区
  ↓
内核初始化期间 ──→ 多次调用 __alloc_bootmem() 分配各类结构
  ↓
主内存管理系统就绪
  ↓
free_all_bootmem() ──→ 扫描、回收、卸载 bootmem
  ↓
后续所有内存分配由伙伴分配器、slab 等接管
```

## Key Points

- **简单性**：基于位图，避免了复杂的数据结构和算法。
- **临时性**：启动后即被完全卸载，零运行时开销。
- **合并优化**：减少页框碎片，尤其在频繁小分配的场景下有效。
- **NUMA 感知**：支持非连续内存架构的多 node 场景。

## Related Concepts

- **Page Frame**：bootmem 的最小管理单位。
- **Zone Allocator**：bootmem 初始化完毕后接管内存的主要分配器。
- **Buddy System**：zone allocator 的核心实现机制。