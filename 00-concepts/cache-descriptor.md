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

- **`kmem_cache_create()`**：创建一个新的 Cache。需要指定对象名称、大小、对齐方式以及构造/析构函数。
- **`kmem_cache_destroy()`**：销毁一个 Cache。只有当所有 Slab 都为空时才能成功。
- **`kmem_cache_alloc()`**：从 Cache 中分配一个对象。
- **`kmem_cache_free()`**：将对象释放回 Cache。

## See Also

- [Slab Allocator](./slab-allocator.md)
- [Slab Descriptor](./slab-descriptor.md)
