---
related:
- "[Page Frame](../00-concepts/EXAMPLE.md)"
- "[Swappable Page](../00-concepts/swappable-page.md)"
tags:
- memory-management
- page-state
- synchronization
sources:
- "[include/linux/mm.h L269-315](/linux/include/linux/mm.h)"
- "[Kernel v2.4.18 Memory Management]"
---

/*! \page page_flags Page Flags & State Management

页面标志（Page Flags）是内核用来跟踪物理页框状态的一组位标志，存储在 `struct page` 的 `flags` 字段中。这些标志用来控制页面的生命周期、I/O 操作、缓存管理及内存回收策略。

*/

# Page Flags

## In a Word

页面标志（Page Flags）是 `struct page` 中的位标志集合，用来记录页框的各种状态——例如是否被锁定、是否脏、是否在活跃列表中等。通过这些标志，内核可以高效地管理页面的生命周期和执行各种内存操作。

## Why This Concept

理解页面标志对于深入掌握内存管理至关重要。每个页框都需要在其生命周期中经历多个状态转换：
- **I/O 状态**：页面是否被锁定、读取是否完成、是否发生错误
- **缓存状态**：页面是否脏、是否需要写回磁盘
- **回收状态**：页面是否在活跃/非活跃列表、是否被引用
- **特殊用途**：页面是否被保留、是否为 slab 分配器使用

这些标志共同形成了页面状态机，驱动着 kswapd、页面写回、缓存管理等重要子系统的运行。

## Deep Dive

### 页面标志定义

Linux 2.4.18 定义了以下页面标志（位索引），见 `linux/include/linux/mm.h` L269-284：

| 标志名 | 位索引 | 含义 | 备注 |
|--------|--------|------|------|
| `PG_locked` | 0 | 页面被锁定 | 表示页面正在进行 I/O 操作，不应被触碰 |
| `PG_error` | 1 | I/O 错误 | 在页面 I/O 过程中发生错误 |
| `PG_referenced` | 2 | 页面被引用 | 通过页表或缓存哈希表访问过，用于回收算法 |
| `PG_uptodate` | 3 | 页面内容有效 | 表示页面内容已从磁盘读取并可用 |
| `PG_dirty` | 4 | 页面已修改 | 页面内容被修改，需要写回磁盘 |
| `PG_unused` | 5 | 未使用 | 保留位 |
| `PG_lru` | 6 | 在 LRU 列表中 | 页面在活跃或非活跃列表中 |
| `PG_active` | 7 | 页面活跃 | 页面在活跃列表中，不应被回收 |
| `PG_slab` | 8 | Slab 分配器 | 页面被 slab 分配器使用 |
| `PG_skip` | 10 | 架构特定 | 仅在 sparc/sparc64 上使用，用来跳过地址空间的某些部分 |
| `PG_highmem` | 11 | 高内存页 | 页面在高内存区域，需要 kmap 映射才能访问 |
| `PG_checked` | 12 | 已检查 | 标记为废弃（2.5 版本前移除） |
| `PG_arch_1` | 13 | 架构特定 | 架构相关的页面状态位，通用代码保证新进入页缓存时清零 |
| `PG_reserved` | 14 | 保留页 | 页面被保留，永不交换，通常用于内核代码、固件等 |
| `PG_launder` | 15 | 待洗页 | 由 VM pressure 写出的页面，标记为需要处理 |

### 标志访问宏

内核提供了一套宏来便捷地访问和修改这些标志。主要分为三类：

#### 1. 测试标志
```c
PageLocked(page)        // test_bit(PG_locked, &(page)->flags)
PageDirty(page)         // test_bit(PG_dirty, &(page)->flags)
Page_Uptodate(page)     // test_bit(PG_uptodate, &(page)->flags)
PageReferenced(page)    // test_bit(PG_referenced, &(page)->flags)
PageActive(page)        // test_bit(PG_active, &(page)->flags)
PageSlab(page)          // test_bit(PG_slab, &(page)->flags)
PageReserved(page)      // test_bit(PG_reserved, &(page)->flags)
```

#### 2. 设置标志
```c
SetPageDirty(page)      // set_bit(PG_dirty, &(page)->flags)
SetPageUptodate(page)   // set_bit(PG_uptodate, &(page)->flags)
SetPageReferenced(page) // set_bit(PG_referenced, &(page)->flags)
SetPageActive(page)     // set_bit(PG_active, &(page)->flags)
SetPageLaunder(page)    // set_bit(PG_launder, &(page)->flags)
```

#### 3. 清除标志
```c
ClearPageDirty(page)        // clear_bit(PG_dirty, &(page)->flags)
ClearPageUptodate(page)     // clear_bit(PG_uptodate, &(page)->flags)
ClearPageReferenced(page)   // clear_bit(PG_referenced, &(page)->flags)
ClearPageActive(page)       // clear_bit(PG_active, &(page)->flags)
```

#### 4. 锁操作（特殊处理）
```c
LockPage(page)          // set_bit(PG_locked, &(page)->flags)
TryLockPage(page)       // test_and_set_bit(PG_locked, &(page)->flags)
UnlockPage(page)        // unlock_page(page) [实现在其他文件]
```

#### 5. 原子测试-清除组合
```c
PageTestandClearReferenced(page)
                        // test_and_clear_bit(PG_referenced, &(page)->flags)
TestSetPageLRU(page)    // test_and_set_bit(PG_lru, &(page)->flags)
TestClearPageLRU(page)  // test_and_clear_bit(PG_lru, &(page)->flags)
```

### 关键标志组合用法

#### I/O 操作流程
1. 开始 I/O：`LockPage(page)` 设置 `PG_locked`
2. I/O 完成：清除 `PG_locked`，根据结果设置 `PG_uptodate` 或 `PG_error`
3. 等待 I/O：进程在 `page->wait` 上睡眠，直到 `PG_locked` 被清除

#### 页面缓存管理
- `PG_uptodate`：页面从磁盘读入后设置，表示内容有效
- `PG_dirty`：页面被修改时设置，表示需要写回
- `PG_launder`：由页面写回程序设置，标记正在处理的脏页

#### 内存回收
- `PG_referenced`：页表或缓存访问时设置，由 `pagemap_lru_lock` 保护（不是 `PG_locked`）
- `PG_active`：在活跃列表中的页面，对应 `active_list` 全局列表
- `PG_lru`：页面在 LRU 列表中，由 `pagemap_lru_lock` 保护

### 保护机制

关键点：**`PG_referenced`、`page->lru` 和 LRU 列表受 `pagemap_lru_lock` 保护，而非 `PG_locked`**（见 mm.h L305-307 注释）。这是因为 kswapd 扫描 LRU 列表时不应阻塞在页面锁上。

### 特殊页面类型

#### 保留页（PG_reserved）
- 永不交换（never swapped out）
- 通常用于内核代码、固件、特殊硬件内存等
- 通过 `SetPageReserved()` 和 `ClearPageReserved()` 管理

#### Slab 页（PG_slab）
- 由 slab 分配器使用
- 内存中的小对象分配通过 slab 缓存实现

#### 高内存页（PG_highmem）
- 在配置 `CONFIG_HIGHMEM` 时生效
- 高内存页不能永久映射到内核虚拟地址空间
- 需要通过 kmap 临时映射进行 I/O 操作

### 宏定义位置

所有宏定义见 `linux/include/linux/mm.h`：
- 标志定义：L269-284
- 测试/设置宏：L313-342
- 其他组合宏：L318-352

## Related Concepts

- **Page Cache**：使用 `PG_dirty`、`PG_uptodate` 等标志管理磁盘缓存
- **LRU Replacement**：使用 `PG_active`、`PG_referenced` 进行页面回收
- **Swapping**：通过 `PG_locked`、`PG_error` 控制交换操作
- **Slab Allocator**：使用 `PG_slab` 标记 slab 页
