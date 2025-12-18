---
related:
- "[Memory Area Management](./memory-area-management.md)"
- "[The Zoned Page Frame Allocator](zoned-page-frame-allocator.md)"
- "[Bootmem Allocator](./bootmem-allocator.md)"
- "[Zone-Based Memory Management](./zone-based-memory-management.md)"
- "[Page Frame](./EXAMPLE.md)"
tags:
- memory-management
- core-variables
- page-allocation
- memory-reclamation
sources:
- "[linux/mm/page_alloc.c](/linux/mm/page_alloc.c)"
- "[linux/include/linux/mm.h](/linux/include/linux/mm.h)"
- "[linux/include/linux/mmzone.h](/linux/include/linux/mmzone.h)"
- "[linux/mm/bootmem.c](/linux/mm/bootmem.c)"
---

/*! \page mm-core-variables Core Memory Management Variables

This page introduces:
Linux 2.4.18 内存管理子系统中的核心全局变量和数据结构，用于跟踪物理内存分配状态、管理页框、维护页面活跃性，以及支持 NUMA 架构。这些变量是分配、释放、回收和统计物理内存的基础。
*/

# Core Memory Management Variables

## In a Word

Core MM variables（核心内存管理变量）是 Linux 内核内存管理子系统中的关键全局状态，包括物理页框数组、内存节点链表、活跃/非活跃页列表、以及各类统计计数。它们一起形成了物理内存的"数据库"，支撑分配、释放、回收和统计。

## Why This Concept

理解这些变量对于掌握内存管理流程至关重要：
- **分配路径**：分配器遍历 `pgdat_list`、zone 结构体来寻找空闲页框。
- **回收路径**：`active_list` 和 `inactive_list` 驱动页面淘汰和交换。
- **统计与平衡**：`nr_free_pages`、`nr_active_pages` 等计数用于内存压力评估和 zone 平衡。
- **架构兼容性**：`pgdat_list` 支持 NUMA 和非连续内存架构。

## Deep Dive

### 0. Page/Zone/Node 三层关系

```
contig_page_data (单节点)
  ├─ node_zones[MAX_NR_ZONES]
  │   └─ zone_t (ZONE_DMA | ZONE_NORMAL | ZONE_HIGHMEM)
  │       ├─ zone_pgdat ──→ 回指 contig_page_data
  │       └─ zone_mem_map ──→ page[]
  │           └─ page->zone ──→ 指向所属 zone
  └─ node_mem_map ──→ 全局 page 数组
```

**关键链接**：
- `page->zone`: page 知道自己的 zone（单向）
- `zone->zone_pgdat`: zone 回指 node（双向）
- `page→node` 必须通过 zone 中转（`page->zone->zone_pgdat`）
- `zone/node` 都知道自己所在的pagelist起点

**NOTE**:
- page 尚未采用 reserved flags 的策略 point to zone & node.

---

### 1. mem_map（物理页框数组）

**作用**：内核对所有物理页框的"数据库"。

```c
extern struct page * mem_map;  // 在 linux/include/linux/mm.h 声明
```

**初始化**：
- 在 `linux/mm/numa.c` 的 `free_area_init_node()` 中初始化为 `PAGE_OFFSET`。
- 实际的 `struct page` 数组由 `alloc_bootmem_node()` 在启动期间分配。

**用途**：
- 通过物理页框号（PFN）作为索引快速访问对应的 `struct page` 结构。
- 例：`struct page *page = mem_map + pfn;`
- 宏 `MAP_NR(addr)` 用于从虚拟地址反向计算页框号。

**关键性质**：
- 数组跨越全局虚拟地址空间（从 `PAGE_OFFSET` 开始）。
- 每个 zone 通过 `zone->zone_mem_map` 指向 `mem_map` 中对应的偏移。

---

### 2. pgdat_list（内存节点链表）

**作用**：NUMA 架构中多个内存节点的链表。

```c
extern pg_data_t *pgdat_list;  // 在 linux/mm/page_alloc.c 定义
```

**结构体**（`struct pglist_data`）：
```c
typedef struct pglist_data {
    zone_t node_zones[MAX_NR_ZONES];      // 该节点的 3 个 zone (DMA/Normal/HighMem)
    struct pglist_data *node_next;         // 链表指针，指向下一个节点
    struct page *node_mem_map;             // 该节点的 mem_map 起点
    unsigned long node_start_paddr;        // 节点物理内存起始地址
    unsigned long node_start_mapnr;        // 节点在全局 mem_map 中的起始索引
    unsigned long node_size;               // 节点包含的总页数
    struct bootmem_data *bdata;            // 启动时内存分配器数据
} pg_data_t;
```

**初始化**：
- 在 `bootmem.c` 的 `init_bootmem_core()` 中，新节点被链接到 `pgdat_list`。
- 单处理器系统（非 NUMA）：只有一个节点 `contig_page_data`。

**用途**：
- **内存分配**：`__alloc_bootmem()` 遍历 `pgdat_list`，从每个节点查找空闲页。
- **内存统计**：计数函数（如 `nr_free_pages()`）遍历所有节点的所有 zone。
- **NUMA 感知**：允许分配器偏好本地 NUMA 节点的内存。

---

### 3. active_list & inactive_list（页面活跃性列表）

**作用**：跟踪物理页的最近使用情况，驱动页面淘汰和交换。

```c
extern struct list_head active_list;
extern struct list_head inactive_list;
```

**初始化**：
- 在 `page_alloc.c` 的 `free_area_init_core()` 中：
  ```c
  INIT_LIST_HEAD(&active_list);
  INIT_LIST_HEAD(&inactive_list);
  ```

**语义**：
- **active_list**：最近被访问过（经常使用）的页。保留在内存中，优先不淘汰。
- **inactive_list**：最近没被访问（使用不频繁）的页。候选淘汰对象。

**页面流转**：
```
新分配的页
    ↓
加入 active_list（或直接分配给进程）
    ↓
（时间推移，该页使用减少）
    ↓
移动到 inactive_list（LRU 扫描）
    ↓
（当内存压力增大时）
    ↓
从 inactive_list 淘汰、swap 出去、或释放
```

**关键函数**：
- `lru_cache_add(page)` 将页加入 active_list。
- `lru_cache_del(page)` 从列表移除。
- `kswapd` 守护进程定期扫描 `inactive_list` 进行回收。

---

### 4. 内存统计计数器

#### nr_free_pages
```c
extern unsigned long num_physpages;  // 系统总物理页数
```
- 在 zone 初始化时设置。
- 通过 `nr_free_pages()` 函数动态计算当前可用页数（遍历所有 zone 的 `free_pages`）。

#### nr_active_pages & nr_inactive_pages
```c
int nr_active_pages;
int nr_inactive_pages;
```
- 分别记录 `active_list` 和 `inactive_list` 中的页数。
- 用于内存压力评估和分页决策。

#### nr_swap_pages
```c
int nr_swap_pages;
```
- 系统当前可用的交换空间页数。
- 影响是否可以继续交换页面。

---

### 5. Zone 内的 free_area 位图与链表

**作用**：伙伴分配器的核心数据结构，快速定位空闲页块。

```c
typedef struct zone_struct {
    // ...
    free_area_t free_area[MAX_ORDER];  // 10 个不同大小的空闲块列表
} zone_t;

typedef struct free_area_struct {
    struct list_head free_list;        // 链接该阶次的所有空闲块
    unsigned long *map;                // 位图，跟踪该阶次的伙伴对状态
} free_area_t;
```

**工作原理**：
- `free_area[order]` 管理大小为 2^order 个页框的空闲块。
- `free_list` 链接所有空闲块（通过 `struct page->list` 指针）。
- `map` 位图用于快速判断伙伴页是否空闲，加速合并。

**分配流程**：
1. 在 `rmqueue()` 中从请求的 order 开始扫描 `free_area[order..MAX_ORDER-1]`。
2. 若找到空闲块，调用 `expand()` 递归分割至请求的大小。
3. 从链表移除已分配块。

**释放与合并**：
1. 在 `__free_pages_ok()` 中，扫描位图检查伙伴是否空闲。
2. 若伙伴空闲，合并并向上推进（`MARK_USED`）。
3. 将最终合并块加入相应的 `free_area[order].free_list`。

---

### 6. Zone 平衡参数

```c
static int zone_balance_ratio[MAX_NR_ZONES] = { 128, 128, 128 };
static int zone_balance_min[MAX_NR_ZONES] = { 20, 20, 20 };
static int zone_balance_max[MAX_NR_ZONES] = { 255, 255, 255 };
```

**用途**：在 zone 初始化时计算 `pages_min`、`pages_low`、`pages_high`：
```c
zone->pages_min = mask;          // 绝对最小可用页数（触发 kswapd）
zone->pages_low = mask * 2;      // 低水位（开始考虑释放）
zone->pages_high = mask * 3;     // 高水位（停止释放）
```

**分配决策**：
- 若 `zone->free_pages > pages_min`：可以分配。
- 若 `zone->free_pages <= pages_min`：唤醒 `kswapd` 进行回收。
- 若 `zone->free_pages <= pages_low`：进行主动内存回收。

---

### 7. contig_page_data（单节点的默认数据）

```c
extern pg_data_t contig_page_data;  // 在 linux/arch/i386 中定义
```

**作用**：非 NUMA 系统（单处理器或 UMA）的唯一内存节点描述。

**初始化**：
- 在 `free_area_init()` 中调用 `free_area_init_core(0, &contig_page_data, ...)`。

**用途**：
- 单处理器系统中所有内存分配都通过 `contig_page_data` 的 zone 进行。
- `pgdat_list` 指向它作为起点。

---

## 分配与回收流程中的变量使用

### 分配路径（Allocation）

```
__alloc_pages(gfp_mask, order, zonelist)
  │
  ├─ 遍历 zonelist->zones[]
  │    └─ 对每个 zone 检查 zone->free_pages > pages_min
  │
  ├─ 若满足，调用 rmqueue(zone, order)
  │    ├─ 扫描 zone->free_area[order..MAX_ORDER-1]
  │    ├─ 从 free_list 取出页框
  │    └─ 更新 zone->free_pages
  │
  └─ 若无可用页
       └─ 唤醒 kswapd（设置 zone->need_balance）
```

**关键变量更新**：
- `zone->free_pages -= (1 << order)` ：zone 的空闲页数减少。
- `mem_map[index]->refcount++` ：页引用计数增加。

### 回收路径（Reclamation）

```
kswapd() 或 try_to_free_pages()
  │
  ├─ 扫描 inactive_list（LRU 顺序）
  │
  ├─ 对每个页：
  │    ├─ 检查是否可淘汰（无映射、无引用）
  │    ├─ 若可淘汰：
  │    │   ├─ swap 写出或释放
  │    │   └─ 从 inactive_list 移除（nr_inactive_pages--）
  │    │
  │    └─ 若不可淘汰：回到 active_list（nr_active_pages++）
  │
  └─ 更新 zone->free_pages
```

**关键变量更新**：
- `nr_inactive_pages--` / `nr_active_pages++` ：页面列表大小变化。
- `zone->free_pages += (1 << order)` ：zone 空闲页增加。
- `mem_map[index]->refcount--` ：页引用计数减少。

---

## Key Points

- **mem_map** 是全局页框数据库，通过 PFN 快速访问。
- **pgdat_list** 支持 NUMA，允许多节点内存管理。
- **active_list / inactive_list** 驱动 LRU 页面淘汰。
- **zone 内 free_area** 实现伙伴分配器的快速分配/合并。
- **zone 平衡参数** 控制何时触发回收和分配限制。
- **统计计数** 用于内存压力感知和分页决策。

## Related Concepts

- **[Zone-Based Memory Management](./zone-based-memory-management.md)**：zone 的详细设计。
- **[Bootmem Allocator](./bootmem-allocator.md)**：启动时初始化这些变量的分配器。
- **[Page Frame](./EXAMPLE.md)**：`struct page` 结构的细节。