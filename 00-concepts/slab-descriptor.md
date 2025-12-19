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

### 3. 管理空闲对象：`bufctl` (空闲账本)

`bufctl` 是 Slab 分配器中管理空闲对象的核心机制，被形象地称为“空闲对象的账本”。

#### 3.1 物理位置
账本紧跟在 `slab_t` 描述符之后。内核通过宏 `slab_bufctl(slabp)` 定位它：
- **On-Slab**：`slab_t` + `bufctl` 数组位于 Slab 页面的头部。
- **Off-Slab**：它们一起存放在外部的通用 Cache（如 `size-32`）中。

#### 3.2 逻辑实现：数组模拟链表
`bufctl` 实际上是一个 `unsigned int` 类型的数组，其长度等于该 Slab 中的对象总数。它实现了一个高效的**静态链表**：
- **初始化**：当 Slab 创建时，`bufctl[i]` 被设置为 `i+1`，最后一个元素设置为 `BUFCTL_END`。
- **取货 (Alloc)**：
    1. 读取 `slab->free` 获取当前空闲对象索引（假设为 `n`）。
    2. 将 `slab->free` 更新为 `bufctl[n]`（即下一个排队的对象）。
- **退货 (Free)**：
    1. 用户归还对象 `x`。
    2. 将 `bufctl[x]` 指向当前的 `slab->free`。
    3. 将 `slab->free` 更新为 `x`（头插法入栈）。

#### 3.3 为什么这样设计？
1. **不占空间**：不需要在对象内部存储指针，支持极小对象且不破坏对齐。
2. **缓存友好**：数组在内存中连续，CPU 预取效率高。
3. **安全隔离**：管理数据与对象数据分离，减少相互干扰。

### 4. 关键宏

- **`slab_bufctl(slabp)`**：获取指向该 Slab 的 `bufctl` 数组起始位置的指针。其实现为 `((kmem_bufctl_t *)(((slab_t*)slabp)+1))`。

### 5. 对象管理逻辑 (Object Management Logic)

`slab_t` 通过以下字段的配合，实现了对 Slab 内部对象的精准控制：

#### 5.1 定位对象：`s_mem`
- `s_mem` 指向 Slab 中第一个对象的起始地址。
- **计算公式**：第 `i` 个对象的地址 = `s_mem + (i * cachep->objsize)`。
- 这种简单的算术运算极大地加快了对象定位速度。

#### 5.2 追踪状态：`inuse`
- `inuse` 记录了当前 Slab 中已分配对象的数量。
- **满员判定**：当 `inuse == cachep->num` 时，该 Slab 会被移入 `cachep->slabs_full` 链表。
- **全空判定**：当 `inuse == 0` 时，该 Slab 会被移入 `cachep->slabs_free` 链表，成为被回收的首选。

#### 5.3 快速分配：`free` 与 `bufctl`
- `free` 始终指向当前 Slab 中“最容易拿到的”那个空闲对象的索引。
- 分配过程本质上是一个**出栈**操作：
    1. 弹出 `free` 指向的对象。
    2. 从 `bufctl[free]` 中获取下一个“栈顶”。
- 释放过程本质上是一个**入栈**操作：
    1. 将归还对象的 `bufctl` 指向当前的 `free`。
    2. 将 `free` 更新为归还对象的索引。

## See Also

- [Cache Descriptor](./cache-descriptor.md)
- [Object Descriptor](./object-descriptor.md)
