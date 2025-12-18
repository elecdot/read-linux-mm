---
related:
- "[Slab Allocator](./slab-allocator.md)"
- "[Cache Descriptor](./cache-descriptor.md)"
- "[Slab Descriptor](./slab-descriptor.md)"
tags:
- memory-management
- slab-allocator
- object
- constructor
sources:
- "[linux/mm/slab.c](/linux/mm/slab.c)"
- "[linux/include/linux/slab.h](/linux/include/linux/slab.h)"
---

/*! \page object-descriptor Object Descriptor & Layout

This page introduces:
Object（对象）是 Slab 分配器管理的最小单位。Object Descriptor 并不是一个显式的结构体，而是指对象在 Slab 内部的布局方式、对齐策略以及与之关联的构造/析构机制。
*/

# Object Descriptor & Layout

## In a Word

Object 是内核请求分配的实际内存块。在 Slab 分配器中，Object 被整齐地排列在 Slab 页面中，并通过 `bufctl` 数组进行逻辑上的串联管理。

## Why This Concept

理解 Object 的布局对于理解内存对齐和性能优化至关重要：
- **对齐 (Alignment)**：确保对象起始地址符合硬件缓存行（L1/L2 Cache Line）要求，减少伪共享。
- **着色 (Colouring)**：通过微调不同 Slab 的起始偏移，使不同 Slab 的对象映射到不同的硬件缓存行，避免缓存冲突。
- **生命周期管理**：通过构造函数（Constructor）实现对象的“预初始化”。

## Deep Dive

### 1. Slab 内部布局 (On-Slab 模式)

一个典型的 On-Slab 布局如下：

```
[ Slab Descriptor (slab_t) ]
[ kmem_bufctl_t 数组 (索引链表) ]
[ Alignment Padding (对齐填充) ]
[ Object 0 ]
[ Object 1 ]
...
[ Object N ]
[ Unused Space (剩余空间) ]
```

### 2. 对象状态管理

对象本身不包含管理元数据。其状态（空闲/已分配）由 [Slab Descriptor](./slab-descriptor.md) 中的 `inuse` 计数和 `bufctl` 数组共同维护。

- **分配时**：从 `slab->free` 获取索引，将 `slab->free` 更新为 `bufctl[index]`，`inuse++`。
- **释放时**：将 `bufctl[index]` 设为当前的 `slab->free`，将 `slab->free` 更新为 `index`，`inuse--`。

### 3. 构造与析构 (ctor/dtor)

在 `kmem_cache_create` 时，可以注册两个回调函数：
- **`ctor` (Constructor)**：当一个新的 Slab 被创建并划分为对象时，每个对象都会调用一次构造函数。这用于初始化那些在对象的整个生命周期内保持不变的成员（如锁、链表头）。
- **`dtor` (Destructor)**：当 Slab 被释放回伙伴系统时调用。

**注意**：在 Linux 2.4 中，对象被释放回 Cache 时**不会**调用析构函数，它保持“已初始化”状态，直到下次被分配或 Slab 被销毁。

### 4. 关键宏与定义

- **`SLAB_HWCACHE_ALIGN`**：标志位，要求对象按硬件缓存行对齐。
- **`kmem_bufctl_t`**：在 `slab.c` 中定义为 `unsigned int`，用于存储对象索引。

## See Also

- [Cache Descriptor](./cache-descriptor.md)
- [Slab Descriptor](./slab-descriptor.md)
