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

### 1. 核心层级结构

Slab 分配器由三个主要层级组成：
-   **Cache (kmem_cache_t)**：每种对象类型都有一个独立的 Cache（如 `mm_struct` 缓存、`tcp_control_block` 缓存）。
-   **Slab**：Cache 由多个 Slab 组成。一个 Slab 通常由一个或多个连续的物理页框组成。
-   **Object**：Slab 被划分为多个大小相等的 Object，这是最终分配给内核组件的单位。

### 2. Slab 的三种状态

为了减少碎片并提高利用率，Cache 中的 Slab 被组织在三个链表中：
-   **Full**：Slab 中所有对象都已被分配。
-   **Partial**：Slab 中既有已分配对象，也有空闲对象。分配请求优先从这里满足。
-   **Empty**：Slab 中所有对象都是空闲的。当系统内存紧张时，这些 Slab 可以被释放回伙伴系统。

### 3. kmalloc 的实现

内核中常用的 `kmalloc` 实际上是建立在 Slab 分配器之上的。内核预先创建了一系列通用大小的 Cache（如 `size-32`, `size-64`, ..., `size-131072`）。当你调用 `kmalloc(100, ...)` 时，它会从最接近的 `size-128` 缓存中分配一个对象。

### 4. SMP 优化 (Per-CPU Caches)

在 Linux 2.4.18 中，为了减少多处理器环境下的锁竞争，每个 Cache 为每个 CPU 维护了一个本地对象数组（`cpudata`）。CPU 优先从自己的本地缓存中分配/释放对象，只有当本地缓存为空或溢出时，才会去操作全局的 Slab 链表并加锁。

## See Also

-   `kmalloc` / `kfree`：内核最常用的内存分配接口。
-   `/proc/slabinfo`：查看系统当前所有 Slab 缓存状态的工具。
