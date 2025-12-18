---
related:
- "[Slab Allocator](./slab-allocator.md)"
- "[Cache Descriptor](./cache-descriptor.md)"
- "[Object Descriptor](./object-descriptor.md)"
tags:
- memory-management
- slab-allocator
- slab-descriptor
- slab_t
sources:
- "[linux/mm/slab.c](/linux/mm/slab.c)"
---

/*! \page slab-descriptor Slab Descriptor (slab_t)

This page introduces:
Slab Descriptor（Slab 描述符）是管理单个 Slab 内部状态的数据结构，由 `struct slab_s`（即 `slab_t`）表示。它记录了 Slab 内对象的分配情况、起始地址以及空闲对象链表。
*/

# Slab Descriptor

## In a Word

Slab Descriptor (`slab_t`) 是单个 Slab 的“管家”。它负责追踪该 Slab 内哪些对象是空闲的，哪些是已分配的，并维护 Slab 在 Cache 链表中的位置。

## Why This Concept

一个 Cache 由多个 Slab 组成，内核需要精细管理每个 Slab：
- 知道 Slab 的物理内存起始位置（`s_mem`）。
- 快速找到下一个空闲对象（通过 `free` 索引和 `bufctl`）。
- 统计当前已使用的对象数量（`inuse`），以便在 Slab 变满或全空时移动链表。

## Deep Dive

### 1. 核心数据结构：`struct slab_s`

在 `linux/mm/slab.c` 中定义：

```c
typedef struct slab_s {
    struct list_head    list;       /* 链接到 Cache 的 slabs_xxx 链表 */
    unsigned long       colouroff;  /* 该 Slab 的着色偏移量 */
    void                *s_mem;     /* Slab 内第一个对象的起始地址 */
    unsigned int        inuse;      /* 当前已分配的对象数量 */
    kmem_bufctl_t       free;       /* 第一个空闲对象的索引 */
} slab_t;
```

### 2. 描述符的存放位置

Slab 描述符有两种存放策略，由 Cache 的 `flags` 决定：

-   **On-Slab (内置)**：描述符存放在 Slab 内存区域的起始处。
    -   优点：不需要额外分配内存。
    -   缺点：如果对象很大，可能会浪费空间。
-   **Off-Slab (外置)**：描述符存放在通用的通用 Cache（如 `size-32`）中。
    -   优点：Slab 页面可以完全用于存放对象。
    -   触发条件：通常当对象大小超过页面的 1/8 时使用。

### 3. 管理空闲对象：`bufctl`

在 Slab 描述符之后（如果是 On-Slab），紧跟着一个 `kmem_bufctl_t` 类型的数组。
-   这是一个简单的**静态链表**。
-   `slab->free` 指向第一个空闲对象的索引。
-   `bufctl[i]` 存储了第 `i` 个对象之后的下一个空闲对象的索引。
-   结束标记为 `BUFCTL_END` (0xffff)。

### 4. 关键宏

-   **`slab_bufctl(slabp)`**：获取指向该 Slab 的 `bufctl` 数组起始位置的指针。

## See Also

- [Cache Descriptor](./cache-descriptor.md)
- [Object Descriptor](./object-descriptor.md)
