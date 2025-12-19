---
related:
- "[Cache Descriptor](./cache-descriptor.md)"
- "[Slab Descriptor](./slab-descriptor.md)"
- "[Object Descriptor](./object-descriptor.md)"
- "[Buddy System](./buddy-system.md)"
- "[Zone-Based Memory Management](./zone-based-memory-management.md)"
- "[Memory Area Management](./memory-area-management.md)"
tags:
- memory-management
- slab-allocator
- object-caching
- kmalloc
sources:
- "[linux/mm/slab.c](/linux/mm/slab.c)"
- "[linux/include/linux/slab.h](/linux/include/linux/slab.h)"
---

/*! \page slab-allocator Slab Allocator

This page introduces:
Slab 分配器是 Linux 内核中用于高效管理小块内存和常用内核对象的机制。它建立在伙伴系统之上，通过缓存已初始化的对象来减少分配开销，并有效解决内核内部的微小内存碎片问题。
*/

# Slab Allocator

## In a Word

Slab Allocator（Slab 分配器）是 Linux 内核的对象缓存机制。它从伙伴系统申请大块内存（通常是一个页框），并将其划分为多个固定大小的小块（Objects），用于存放内核常用的数据结构（如 `inode`, `task_struct` 等）。

## Why This Concept

伙伴系统虽然能高效分配页框，但在处理小块内存（如几十字节）时存在两个问题：
1.  **内部碎片**：如果只申请 100 字节却分配一个 4KB 的页框，会造成巨大浪费。
2.  **初始化开销**：内核频繁创建和销毁相同类型的对象。Slab 通过“缓存”已初始化的对象，避免了重复初始化的 CPU 耗时。

## Deep Dive

Slab 系统可以总结为 **“一个中心、三个层级、三条链表、三级分配”**。

### 1. 一个中心：对象缓存 (Object Caching)
Slab 的核心哲学是：**“与其销毁并重新创建，不如洗净并暂存”**。它通过缓存已经初始化好的内核对象（如 `task_struct`），极大地减少了 CPU 在构造和析构上的开销。

### 2. 三个层级 (The Three-Tier Hierarchy)
Slab 系统像一个三级金字塔，从上到下分别是：

| 层级 | 对应结构 | 角色 | 职责 |
| :--- | :--- | :--- | :--- |
| **管理层 (Management)** | `kmem_cache_t` | **连锁店总部** | 制定策略（对齐、着色）、管理全局链表、处理 SMP 本地缓存。 |
| **仓库层 (Warehouse)** | `slab_t` | **具体仓库** | 管理物理页框、维护“空闲账本” (`bufctl`)、统计在用对象。 |
| **货物层 (Object)** | `Object` | **实际商品** | 最终交付给内核使用的内存块，具备对齐、红区保护和着色偏移。 |

### 3. 三条链表 (The Three-List Strategy)
每个 Cache 内部通过三条链表来管理其下的所有 Slab，实现内存利用率的最大化：
- **`slabs_partial` (半满)**：**最活跃**。分配时的首选，保证已经开辟的仓库被优先填满。
- **`slabs_free` (全空)**：**备用库**。当半满链表用完时，从这里调拨。
- **`slabs_full` (全满)**：**暂存区**。不参与分配，直到有人退货。

### 4. 三级分配路径 (The Three-Step Allocation)
为了追求极致性能，分配一个对象会经历以下路径：
1. **L1: Per-CPU Cache (极速)**：无锁操作，直接从当前 CPU 的私有数组取货。
2. **L2: Global Slab Lists (快速)**：加锁操作，从 `partial` 或 `free` 链表中取货。
3. **L3: Buddy System (慢速)**：调用 `kmem_cache_grow`，向伙伴系统批发新的页框。

### 5. 核心管理技术
- **缓存着色 (Cache Colouring)**：通过微调 Slab 起始偏移，让不同仓库的对象在 CPU L1 Cache 中均匀分布，避免硬件冲突。详见 [对齐与着色](#11-对齐与着色-alignment--coloring)。
- **空闲账本 (bufctl)**：使用“数组模拟链表”管理空闲对象，不占对象空间，缓存友好。
- **On/Off-Slab 灵活性**：根据对象大小自动决定管理元数据是放在页内还是页外。

### 6. 专用缓存 vs 通用缓存与自举性 (Caches & Self-Bootstraping)
Slab 缓存根据用途分为两大类，这种分类也支撑了系统的**自举性**（即“用 Slab 管理 Slab”）：

- **专用缓存 (Specific Caches)**：
    - **定义**：由内核模块通过 `kmem_cache_create` 显式创建。它专门为某种特定的、频繁分配的小型数据结构（如 `task_struct`, `mm_struct`）服务。
    - **优势**：支持自定义构造函数（ctor），对象在分配时已处于“半就绪”状态，性能最优。
    - **自举实例**：所有的 **Cache 描述符 (`kmem_cache_t`)** 本身也是由 Slab 分配的，它们存放在名为 `cache_cache` 的专用缓存中。
- **通用缓存 (General Caches)**：
    - **定义**：由系统在初始化阶段（`kmem_cache_init`）预先创建并预留的一系列固定大小的缓存（如 `size-32`, `size-64`... 直到 `size-131072`）。
    - **用途**：主要为 `kmalloc` 提供底层支持。
    - **自举实例**：在 **Off-Slab** 模式下，**Slab 描述符 (`slab_t`)** 和账本会搬出仓库，去这些通用缓存中申请“宿舍”。

### 7. kmalloc 的本质
`kmalloc` 是内核中最常用的内存分配接口，它的行为类似于用户态的 `malloc`，但其底层完全依赖于**通用缓存**：
- **功能**：当你需要一段连续的、大小不确定的内存（且不需要特定的构造初始化）时，使用 `kmalloc`。
- **原理**：当你调用 `kmalloc(size, flags)` 时，内核并不会真的去切分页面，而是通过 `kmem_find_general_cachep` 快速定位到一个最接近且能装下 `size` 的**通用缓存**，并从中弹出一个对象给你。
- **局限**：由于是通用性质，它无法利用构造函数优化，且可能存在一定的内部碎片（例如申请 65 字节却分配了 128 字节的对象）。

### 8. SMP 优化 (Per-CPU Caches)
在多处理器环境下，为了减少锁竞争，每个 Cache 为每个 CPU 维护了一个本地对象数组（`cpudata`）。
- **分配**：优先从当前 CPU 的本地缓存中取货（无锁）。
- **释放**：优先还回到当前 CPU 的本地缓存中（无锁）。
- **平衡**：只有当本地缓存为空或溢出时，才会批量与全局 Slab 链表交换对象（加锁）。

### 9. 仓库扩容与页框绑定 (Growth & Page Binding)
当 Cache 耗尽时，会调用 `kmem_cache_grow` 向伙伴系统“批发”页框。这里有一个关键的设计：
- **反向映射**：新申请的每个页框（`struct page`）都会通过 `SET_PAGE_CACHE` 和 `SET_PAGE_SLAB` 宏，将其 `list.next` 和 `list.prev` 指针分别指向所属的 **Cache 描述符** 和 **Slab 描述符**。
- **意义**：这使得 `kfree(ptr)` 能够实现“盲放”。内核只需通过 `virt_to_page(ptr)` 找到页描述符，就能瞬间知道这个对象属于哪个 Cache，从而将其正确归还。

### 10. 典型用例 (Typical Use Cases)

#### 场景 A：为自定义结构创建“专用仓库”
如果你在写一个驱动，需要频繁分配 `struct my_device_data`，你应该使用专用缓存：
```c
// 1. 在模块初始化时创建仓库
kmem_cache_t *my_cachep;
my_cachep = kmem_cache_create("my_objects", sizeof(struct my_device_data), 
                              0, SLAB_HWCACHE_ALIGN, my_ctor, NULL);

// 2. 需要时从仓库取货
struct my_device_data *obj = kmem_cache_alloc(my_cachep, GFP_KERNEL);

// 3. 用完后归还
kmem_cache_free(my_cachep, obj);
```

#### 场景 B：临时申请一段内存
如果你只需要临时申请 100 字节存放一个字符串，直接使用 `kmalloc`：
```c
// 自动从 size-128 通用缓存中分配
char *buf = kmalloc(100, GFP_KERNEL);

// 归还（无需指定 cache，系统会自动通过页描述符找回）
kfree(buf);
```

### 11. 对齐与着色 (Alignment & Coloring)

这是 Slab 分配器为了压榨硬件性能而设计的两个精妙机制：

#### A. 硬件对齐 (Alignment)
- **原理**：通过 `SLAB_HWCACHE_ALIGN` 标志，Slab 会确保每个对象的起始地址都对齐到 CPU 的 **L1 Cache Line**（32字节）。
- **目的**：防止一个对象跨越两个 Cache Line。如果对象跨行，CPU 需要两次内存访问才能加载完数据，性能减半。

#### B. Slab 着色 (Slab Coloring)

> 核心实现见`kmem_cache_grow`

- **痛点 (Cache Alias)**：现代 CPU 的 L1 Cache 是组相联的。如果所有 Slab 都从页框的 `0` 偏移处开始存放对象，那么不同 Slab 中相同索引的对象（例如 Slab A 的第一个对象和 Slab B 的第一个对象）极大概率会映射到 **同一个 Cache Line**。当内核交替访问这些对象时，会频繁触发 Cache 冲突失效，导致性能剧降。
- **方案**：
    1. 每个页框在存放对象前，先空出一小段“空白区”。
    2. 第一个 Slab 空出 0 字节，第二个空出 64 字节，第三个空出 128 字节……这个偏移量就是 **“颜色” (Color)**。
    3. 这样，不同仓库的对象在物理内存中的相对偏移就错开了，从而在 L1 Cache 中也能“均匀分布”。
- **实现**：在 `kmem_cache_grow` 中，通过 `cachep->colour_next` 轮询分配颜色，确保系统中的 Slab 颜色分布均匀。

## See Also

-   `kmalloc` / `kfree`：内核最常用的内存分配接口。
-   `/proc/slabinfo`：查看系统当前所有 Slab 缓存状态的工具。
