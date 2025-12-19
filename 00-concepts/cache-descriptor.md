---
related:
- "[Slab Allocator](./slab-allocator.md)"
- "[Slab Descriptor](./slab-descriptor.md)"
- "[Object Descriptor](./object-descriptor.md)"
tags:
- memory-management
- slab-allocator
- cache-descriptor
- kmem_cache_t
sources:
- "[linux/mm/slab.c](/linux/mm/slab.c)"
- "[linux/include/linux/slab.h](/linux/include/linux/slab.h)"
---

/*! \page cache-descriptor Cache Descriptor (kmem_cache_t)

This page introduces:
Cache Descriptor（缓存描述符）是 Slab 分配器的核心管理结构，由 `struct kmem_cache_s`（即 `kmem_cache_t`）表示。它负责管理特定类型对象的所有 Slab 链表、统计信息以及多处理器环境下的本地缓存。
*/

# Cache Descriptor

## In a Word

Cache Descriptor (`kmem_cache_t`) 是 Slab 分配器的“大脑”。每一种被缓存的对象类型（如 `inode`）都有一个对应的 `kmem_cache_t` 实例，用于追踪该类型对象在内存中的分配状态。

## Why This Concept

内核需要一种方式来隔离不同类型的对象分配，以减少碎片并提高效率。Cache Descriptor 提供了这种隔离：
- 它维护了三个 Slab 链表（Full, Partial, Empty），决定了分配请求的去向。
- 它存储了对象的元数据（大小、对齐方式、构造函数）。
- 它实现了 Per-CPU 缓存，解决了多核竞争问题。

## Deep Dive

### 1. 核心数据结构：`struct kmem_cache_s`

在 `linux/mm/slab.c` 中定义，主要包含以下部分：

#### Slab 链表管理
- `slabs_full`：已满 Slab 链表。
- `slabs_partial`：部分空闲 Slab 链表（分配首选）。
- `slabs_free`：完全空闲 Slab 链表。

#### 对象元数据
- `objsize`：单个对象的大小。
- `num`：每个 Slab 中包含的对象数量。
- `gfporder`：每个 Slab 占用的页框数（$2^{gfporder}$）。

#### 性能与同步
- `spinlock`：保护非 Per-CPU 成员的自旋锁。
- `cpudata[NR_CPUS]`：指向 `cpucache_t` 的指针数组，实现 Per-CPU 缓存。
- `batchcount`：SMP 环境下，从全局缓存批量移动对象的数量。

#### 缓存着色 (Cache Colouring)
- `colour`：着色范围。
- `colour_off`：着色偏移量。
- `colour_next`：下一个 Slab 使用的颜色。

### 2. 关键函数

```c
/** @brief 初始化 Slab 分配器（在系统启动早期调用） prototype */
extern void kmem_cache_init(void);

/** @brief 初始化通用大小的 Cache（用于 kmalloc） */
extern void kmem_cache_sizes_init(void);

/** @brief 创建一个新的对象 Cache */
extern kmem_cache_t *kmem_cache_create(const char *, size_t, size_t, unsigned long,
				       void (*)(void *, kmem_cache_t *, unsigned long),
				       void (*)(void *, kmem_cache_t *, unsigned long));

/** @brief 销毁一个对象 Cache */
extern int kmem_cache_destroy(kmem_cache_t *);

/** @brief 收缩 Cache，释放所有完全空闲的 Slab (主动回收)*/
extern int kmem_cache_shrink(kmem_cache_t *);

/** @brief 从 Cache 中分配一个对象 */
extern void *kmem_cache_alloc(kmem_cache_t *, int);

/** @brief 将对象释放回 Cache */
extern void kmem_cache_free(kmem_cache_t *, void *);
```

#### A. 创建：`kmem_cache_create`
这是 Slab 分配器的“配置中心”，负责计算最经济的内存布局。
1.  **参数校验与对齐**：
    *   严禁在中断中调用（涉及内存分配可能睡眠）。
    *   对象大小至少为机器字长（`BYTES_PER_WORD`），最大不超过 32 个页框。
    *   默认按字长对齐，若设置 `SLAB_HWCACHE_ALIGN` 则按 L1 Cache Line 对齐。
2.  **描述符分配**：从全局 `cache_cache` 中分配一个 `kmem_cache_t` 实例并清零。
3.  **On/Off-Slab 决策**：
    *   如果对象大小 $\ge$ 页面大小的 1/8，则将管理元数据（`slab_t` 和 `bufctl`）移到页面之外（Off-Slab），以减少页面内的碎片。
    *   Off-Slab 模式下，会调用 `kmem_find_general_cachep` 寻找一个合适的通用 Cache 来存放这些元数据。
4.  **布局估算迭代 (`kmem_cache_estimate`)**：
    *   从 `gfporder = 0` 开始尝试，计算在当前页框阶数下能容纳的对象数量 `num`。
    *   **退出条件**：内部碎片（`left_over`）占总空间的比例 $\le 1/8$，或者 `gfporder` 达到上限。
    *   **限制**：如果 `num` 超过 `offslab_limit`，会尝试降低阶数以保证管理效率。
5.  **着色计算 (Coloring)**：
    *   利用估算剩下的 `left_over` 空间，计算偏移步长 `colour_off`（通常是对齐值的倍数）。
    *   计算最大颜色数 `colour = left_over / colour_off`。
    *   这使得不同 Slab 的对象在硬件 Cache 中均匀分布，避免多核竞争同一 Cache Line。
6.  **SMP 初始化**：如果系统已就绪，调用 `enable_cpucache` 为每个 CPU 分配私有的 `cpucache_t`。
7.  **全局注册**：获取 `cache_chain_sem` 信号量，检查重名后将 Cache 加入全局 `cache_chain` 链表。

#### B. 销毁：`kmem_cache_destroy`
1.  **脱离链表**：获取全局信号量，将 Cache 从 `cache_chain` 中移除。如果 `clock_searchp` 指向该 Cache，需将其移向下一个节点。
2.  **强制收缩**：调用 `__kmem_cache_shrink`。如果返回非 0（表示仍有对象在用），则销毁失败，将 Cache 重新挂回链表并报错。
3.  **释放 Per-CPU 缓存**：在 SMP 环境下，遍历并释放所有 CPU 的 `cpudata` 结构。
4.  **释放描述符**：调用 `kmem_cache_free(&cache_cache, cachep)` 将描述符归还给系统。

#### C. 收缩：`kmem_cache_shrink`
1.  **刷洗缓存 (`drain_cpu_caches`)**：
    *   分配一个新的 `ccupdate_struct_t` 结构。
    *   使用 `smp_call_function_all_cpus` 发起处理器间中断 (IPI)。
    *   在每个 CPU 上执行 `do_ccupdate_local`，将本地缓存中的对象指针交换出来。
    *   在发起者 CPU 上统一调用 `free_block` 将这些对象归还给全局 Slab 链表。
2.  **释放空闲 Slab**：
    *   持有 Cache 自旋锁，遍历 `slabs_free` 链表。
    *   只要 Cache 没有处于增长状态 (`growing`)，就不断从链表末尾取出 Slab。
    *   调用 `kmem_slab_destroy`：执行析构函数 (`dtor`) -> 释放物理页框 (`kmem_freepages`) -> 如果是 Off-Slab 则释放元数据。

#### D. 全局回收：`kmem_cache_reap`
这是由 `kswapd` 定期调用的“内存清道夫”，采用**时钟算法**。
1.  **扫描起点**：从上次停止的 `clock_searchp` 开始，最多扫描 `REAP_SCANLEN` 个 Cache。
2.  **资格审查**：排除正在增长、刚刚增长过（`DFLGS_GROWN`）或标记为不可回收（`SLAB_NO_REAP`）的 Cache。
3.  **评分系统 (Scoring)**：
    *   基础分：`pages = 空闲Slab数 * 2^gfporder`。
    *   **惩罚项**：如果有构造函数 (`ctor`) 或属于高阶分配 (`gfporder > 0`)，分数乘以 0.8（即 `(pages*4+1)/5`）。
    *   记录分数最高的 Cache 为 `best_cachep`。
4.  **早期退出**：如果某个 Cache 的分数达到 `REAP_PERFECT`，直接停止扫描开始回收。
5.  **50% 回收策略**：
    *   锁定 `best_cachep`。
    *   只释放其 `slabs_free` 链表中 **50%** 的 Slab。
    *   这种“贪婪但克制”的设计是为了保留一部分热点内存，防止系统出现频繁申请/释放页框的“抖动”现象。
6.  **更新时钟**：将 `clock_searchp` 指向下一个待扫描的 Cache。

## See Also

- [Slab Allocator](./slab-allocator.md)
- [Slab Descriptor](./slab-descriptor.md)
