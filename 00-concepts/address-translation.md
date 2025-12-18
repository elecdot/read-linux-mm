---
related:
- "[Memory Area Management](./memory-area-management.md)"
- "[mm-core-variables](./mm-core-variables.md)"
- "[zone-based-memory-management](./zone-based-memory-management.md)"
tags:
- memory-management
- address-translation
- architecture-abstraction
- kernel-virtual-memory
sources:
- "[linux/include/asm-i386/page.h](/linux/include/asm-i386/page.h)"
- "[linux/include/linux/mm.h](/linux/include/linux/mm.h)"
- "[linux/mm/page_alloc.c](/linux/mm/page_alloc.c)"
---

/*! \page address-translation Physical-to-Virtual Address Translation

This page introduces:
内核中物理地址和虚拟地址的转换机制，通过 `__va()` 和 `__pa()` 宏实现架构无关的地址映射，支持直接内核映射和高内存处理。
*/

# Physical-to-Virtual Address Translation Strategy

## In a Word

Address translation strategy（地址转换策略）是 Linux 内核通过 `__va()` 和 `__pa()` 宏实现物理地址与内核虚拟地址之间转换的机制。它隐藏了不同架构的内存布局差异，提供统一的接口用于页框初始化、访问和管理。

## Why This Concept

内核需要在两个地址空间之间频繁转换：
- **物理地址**：硬件真实的内存地址（如来自 BIOS、页表项）。
- **虚拟地址**：内核代码运行的地址空间（从 `PAGE_OFFSET` 开始）。

不同架构的内存布局差异很大（直接映射、分段、高内存等），使用 `__va()` 和 `__pa()` 抽象这些差异，使得核心内存管理代码可以保持架构无关性。

## Deep Dive

### 核心概念

#### 直接内核映射（Direct Kernel Mapping）

在大多数架构（i386、x86_64、ARM）上，内核维护一个**直接映射区**：

```
物理地址          内核虚拟地址
0x00000000 -----> PAGE_OFFSET + 0x00000000  (c0000000 on i386)
0x00001000 -----> PAGE_OFFSET + 0x00001000
0x10000000 -----> PAGE_OFFSET + 0x10000000
...
```

这个映射在启动时由引导程序或内核建立，允许内核通过简单的加法直接访问所有低端内存（ZONE_DMA 和 ZONE_NORMAL）。

#### PAGE_OFFSET 常量

`PAGE_OFFSET` 是架构相关的虚拟地址偏移：
- **i386**: `0xc0000000`（3GB，用户空间 0-3GB，内核 3GB-4GB）
- **x86_64**: `0xffff810000000000`（高地址映射）
- **ARM**: `0xc0000000`（3GB）
- **S390x**: `0x0`（恒等映射，物理 = 虚拟）

---

### `__va()` 宏：物理地址 → 虚拟地址

**用途**：将物理地址转换为可以被内核代码直接使用的虚拟地址。

**i386 实现**：
```c
#define __va(x)  ((void *)((unsigned long)(x) + PAGE_OFFSET))
```

**示例**：
```c
unsigned long paddr = 0x01000000;  // 物理地址，16MB
void *vaddr = __va(paddr);         // 结果：0xc1000000（虚拟地址）

// 现在可以直接通过 vaddr 访问该物理页框的内容
unsigned char *data = (unsigned char *)vaddr;
*data = 42;  // 直接写入物理内存
```

**应用场景**：
1. **页框初始化**（page_alloc.c:845）：
   ```c
   if (j != ZONE_HIGHMEM)
       page->virtual = __va(zone_start_paddr);
   ```
   此处将 zone 的物理起始地址转换为虚拟地址，存储在 `struct page->virtual` 中，以便后续快速访问。

2. **Bootmem 分配**：
   ```c
   bdata->node_bootmem_map = phys_to_virt(mapstart << PAGE_SHIFT);
   // 其中 phys_to_virt() 本质上就是 __va()
   ```

---

### `__pa()` 宏：虚拟地址 → 物理地址

**用途**：将内核虚拟地址反向转换为物理地址。

**i386 实现**（通常）：
```c
#define __pa(x)  ((unsigned long)(x) - PAGE_OFFSET)
```

**示例**：
```c
void *vaddr = 0xc1000000;      // 虚拟地址
unsigned long paddr = __pa(vaddr);  // 结果：0x01000000（物理地址）

// 用于获取页框号
unsigned long pfn = __pa(page_address) >> PAGE_SHIFT;
```

**应用场景**：
1. **获取 PFN（页框号）**：
   ```c
   struct page *page = ...;
   unsigned long pfn = (__pa(page) - mem_map) / sizeof(struct page);
   ```

2. **页表项设置**：
   ```c
   unsigned long *pte = ...;
   *pte = __pa(page_address) | PTE_PRESENT | PTE_WRITE;
   ```

---

### 架构变异

不同架构有不同的内存布局，因此 `__va()` 和 `__pa()` 的实现也不同：

#### i386（简单偏移）
```c
#define __va(x)  ((void *)((unsigned long)(x) + PAGE_OFFSET))
#define __pa(x)  ((unsigned long)(x) - PAGE_OFFSET)
```
- 直接映射区从 `PAGE_OFFSET` (0xc0000000) 开始
- 可访问最大 1GB 物理内存（ZONE_NORMAL）
- 超过的内存为 ZONE_HIGHMEM，需特殊处理

#### x86_64（高地址映射）
```c
#define __va(x)  ((void *)((unsigned long)(x) | __PAGE_OFFSET))
#define __pa(x)  ((unsigned long)(x) & ~__PAGE_OFFSET)
```
- `PAGE_OFFSET = 0xffff810000000000`（高地址）
- 可访问更大范围物理内存

#### M68K（自定义公式）
```c
#define __va(paddr)  ((void *)((unsigned long)(paddr) - m68k_memoffset))
#define __pa(vaddr)  ((unsigned long)(vaddr) + m68k_memoffset)
```
- 基于 `m68k_memoffset` 的动态计算

#### S390x（恒等映射）
```c
#define __va(x)  (void *)(x)
#define __pa(x)  ((unsigned long)(x))
```
- 物理地址 = 虚拟地址，无需转换

#### IA64（复杂映射）
```c
#define __va(x)  ((x) + PAGE_OFFSET)
// 带特殊位操作处理虚拟区域编码
```

---

### 高内存处理（HighMem）

在支持高内存的架构（如 i386，物理内存 > 896MB）上，超出直接映射范围的内存无法使用 `__va()` 直接访问：

```c
// 在 page_alloc.c:845 中
if (j != ZONE_HIGHMEM)
    page->virtual = __va(zone_start_paddr);  // ✓ 直接映射
else
    page->virtual = 0;  // ✗ 高内存无直接虚拟地址
```

**为什么**：
- i386 内核虚拟地址空间只有 1GB（0xc0000000 - 0xffffffff）
- 直接映射最多 1GB 物理内存（16-896MB 为 ZONE_NORMAL）
- 超出范围的物理内存（ZONE_HIGHMEM）需通过 kmap/kunmap 动态映射

**kmap 机制**：
```c
// 临时映射高内存页到内核虚拟地址空间
void *vaddr = kmap(highmem_page);  // 动态映射
// 使用 vaddr 访问页内容
kunmap(highmem_page);  // 解除映射
```

---

### 地址转换在初始化中的应用

在 `free_area_init_core()`（page_alloc.c）中：

```c
// 阶段 1：分配 mem_map 数组
lmem_map = (struct page *) alloc_bootmem_node(pgdat, map_size);
// alloc_bootmem_node 返回虚拟地址

// 阶段 2：对齐虚拟地址
lmem_map = (struct page *)(PAGE_OFFSET + 
    MAP_ALIGN((unsigned long)lmem_map - PAGE_OFFSET));

// 阶段 3：为每个 zone 设置虚拟地址映射
for (i = 0; i < size; i++) {
    struct page *page = mem_map + offset + i;
    page->zone = zone;
    
    // 仅对 ZONE_DMA 和 ZONE_NORMAL 设置虚拟地址
    if (j != ZONE_HIGHMEM)
        page->virtual = __va(zone_start_paddr);  // 使用 __va()
    
    zone_start_paddr += PAGE_SIZE;
}
```

**关键点**：
1. **Bootmem 分配返回虚拟地址**，无需再调用 `__va()`。
2. **zone_start_paddr 是物理地址**，需要用 `__va()` 转换为虚拟地址存储在 `page->virtual`。
3. **高内存页的 page->virtual 保持为 0**，表示无直接虚拟映射。

---

## 内存访问模式

### 直接映射（Direct Mapping）
适用于 ZONE_DMA 和 ZONE_NORMAL：
```
内核代码
  ↓
__va(physical_addr) → 虚拟地址
  ↓
通过 MMU 硬件查页表
  ↓
定位到相同的物理地址
  ↓
访问物理内存
```

一步到位，快速。

### 高内存访问（Highmem Access）
适用于 ZONE_HIGHMEM：
```
内核代码需要访问高内存页
  ↓
kmap(page) → 临时虚拟映射（动态建立页表）
  ↓
使用虚拟地址访问内容
  ↓
kunmap(page) → 解除映射
```

多个步骤，较慢但灵活。

---

## Key Points

- **`__va()` 和 `__pa()` 是架构抽象**，隐藏不同平台的地址转换细节。
- **直接映射**基于 `PAGE_OFFSET`，高效但范围有限。
- **高内存区**无直接映射，需动态 kmap/kunmap。
- **页框初始化**依赖 `__va()` 设置虚拟地址指针。
- **架构差异**包括直接偏移、位操作、自定义公式等。

## Related Concepts

- **[Zone-Based Memory Management](./zone-based-memory-management.md)**：Zone 划分与直接映射范围的关系。
- **[mm-core-variables](./mm-core-variables.md)**：`page->virtual` 字段的用途。