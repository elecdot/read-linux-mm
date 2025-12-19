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

Object 是内核请求分配的实际内存块。在 Slab 分配器中，Object 被整齐地排列在 Slab 页面中，并通过 `bufctl` 数组进行逻辑上的串联管理。Object 并不是一个显式的 C 语言结构体，而是一个逻辑上的内存区域。

## Why This Concept

理解 Object 的布局对于理解内存对齐和性能优化至关重要：
- **对齐 (Alignment)**：确保对象起始地址符合硬件缓存行（L1/L2 Cache Line）要求，减少伪共享。
- **着色 (Colouring)**：通过微调不同 Slab 的起始偏移，使不同 Slab 的对象映射到不同的硬件缓存行，避免缓存冲突。
- **生命周期管理**：通过构造函数（Constructor）实现对象的“预初始化”。
- **调试支持**：通过红区（Red Zone）和毒化（Poisoning）检测内存越界和非法访问。

## Deep Dive

### 1. Slab 内部布局 (On-Slab 模式)

在 On-Slab 模式下，管理元数据和对象共享同一个物理页框。其精确布局如下：

```
[ Slab Descriptor (slab_t) ]
[ kmem_bufctl_t 数组 (索引链表) ]
[ Alignment Padding (对齐填充) ]
[ Object 0 ]
  ├── [ Red Zone (Optional) ]
  ├── [ Actual Data ]
  └── [ Red Zone (Optional) ]
[ Object 1 ]
...
[ Object N ]
[ Unused Space (剩余空间，用于着色) ]
```

### 2. 对象的尺寸计算 (White-box Analysis)

在 `kmem_cache_create` 中，对象的实际尺寸 `objsize` 会经历多次调整：
1. **基础对齐**：向上对齐到机器字长（`BYTES_PER_WORD`）。
2. **红区保护**：如果开启 `SLAB_RED_ZONE`，对象大小增加 2 个字长，分别放在对象前后。
3. **硬件对齐**：如果开启 `SLAB_HWCACHE_ALIGN`，对象大小向上对齐到 L1 Cache Line。

### 3. 缓存着色 (Cache Colouring)

为了防止不同 Slab 的对象在 CPU 缓存中发生冲突，Slab 分配器利用 Slab 尾部的剩余空间（`left_over`）进行“着色”：
- **原理**：每个新创建的 Slab 会获得一个不同的 `colour_next`。
- **偏移**：Slab 的第一个对象起始地址会偏移 `colour_next * colour_off`。
- **效果**：这使得不同 Slab 的对象在物理页内的相对偏移不同，从而映射到不同的 Cache Line。

### 4. 调试机制：Red Zone & Poisoning

- **Red Zone (红区)**：在对象前后填充特定的魔数（`RED_MAGIC1`）。分配时检查，释放时检查。如果魔数被改写，说明发生了缓冲区溢出。
- **Poisoning (毒化)**：在对象释放后，用特定花纹（`0x5a`）填充整个对象。如果再次分配时发现花纹被破坏，说明发生了“释放后使用”（Use-after-free）错误。

### 5. 构造与析构 (ctor/dtor)

- **`ctor` (Constructor)**：仅在 Slab 刚从伙伴系统申请回来、初始化对象时调用一次。每个对象都会调用一次构造函数。这用于初始化那些在对象的整个生命周期内保持不变的成员（如锁、链表头）。
- **`dtor` (Destructor)**：仅在整个 Slab 被释放回伙伴系统时调用。
- **性能优化**：对象在释放回 Cache 时**不调用**析构函数，保持“热”状态（Hot State），下次分配时直接使用，无需重新初始化。

## See Also

- [Cache Descriptor](./cache-descriptor.md)
- [Slab Descriptor](./slab-descriptor.md)
- [Zoned Page Frame Allocator](./zoned-page-frame-allocator.md)
