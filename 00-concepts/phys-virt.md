---
related:
- "[address-translation](./address-translation.md)"
- "[mm-core-variables](./mm-core-variables.md)"
- "[zone-based-memory-management](./zone-based-memory-management.md)"
tags:
- memory-management
- physical-memory
- virtual-memory
- buddy-allocator
sources:
- "[linux/mm/page_alloc.c](/linux/mm/page_alloc.c)"
- "[linux/include/linux/mm.h](/linux/include/linux/mm.h)"
- "[linux/include/asm-i386/page.h](/linux/include/asm-i386/page.h)"
---

/*! \page physical-virtual-distinction Physical and Virtual Address Usage in Memory Management

This page introduces:
在 Linux 2.4.18 内存管理中，物理地址和虚拟地址的使用策略：伙伴分配器通过虚拟地址驱动页框管理，但资源标识、页表、设备 DMA 必须使用物理地址。本文通过项目代码实例说明这一二分法。
*/

# Physical and Virtual Address Usage in Memory Management

## In a Word

物理-虚拟地址二分法（Physical-Virtual Duality）是 Linux MM 的设计核心：**伙伴系统通过虚拟地址管理 struct page 数组，但硬件交互（页表、DMA）、内存标识（PFN）必须使用物理地址**。理解这一点是正确使用和扩展 MM 代码的前提。

## Why This Concept

在 Linux 2.4.18 的代码中：
- 如果你想理解 `free_area_init_core()` 为什么同时使用 `zone_start_paddr` 和 `mem_map`
- 如果你想知道为什么伙伴系统（`rmqueue`, `__free_pages_ok`）只操作虚拟地址
- 如果你想正确实现 DMA 驱动或页表操作

你都需要理解这种二分法。

## Deep Dive

### 1. 项目中的核心场景：zone 初始化

最清晰的例子在 `free_area_init_core()`（page_alloc.c:730-870）：

**物理地址的角色**（标识 zone 在物理内存中的位置）：
```c
// page_alloc.c:831
zone->zone_start_paddr = zone_start_paddr;
```

**虚拟地址的角色**（实际访问 struct page 数组）：
```c
// page_alloc.c:831
zone->zone_mem_map = mem_map + offset;
```

**地址转换的应用**（初始化每个 page 的虚拟映射）：
```c
// page_alloc.c:844-847
for (i = 0; i < size; i++) {
    struct page *page = mem_map + offset + i;           // ← 虚拟地址访问
    if (j != ZONE_HIGHMEM)
        page->virtual = __va(zone_start_paddr);         // ← 物理→虚拟转换
    zone_start_paddr += PAGE_SIZE;
}
```

**这三行代码的含义**：
1. `mem_map + offset + i` — 虚拟地址，用来访问 struct page 元数据
2. `zone_start_paddr` — 物理地址，仅用来标识物理位置
3. `__va()` — 转换函数，将物理地址转为虚拟地址存储

### 2. 伙伴系统中的虚拟地址驱动

分配时（`rmqueue`，page_alloc.c:254-286）：
```c
page = memlist_entry(curr, struct page, list);  // 虚拟地址遍历链表
index = page - zone->zone_mem_map;              // 虚拟地址计算索引
page = expand(zone, page, index, order, curr_order, area);  // 虚拟地址操作
```

释放时（`__free_pages_ok`，page_alloc.c:126-212）：
```c
struct page *base = zone->zone_mem_map;         // 虚拟地址
unsigned long page_idx = page - base;           // 虚拟地址计算
memlist_add_head(&(base + page_idx)->list, ...);  // 虚拟地址更新
```

**关键观察**：伙伴系统的整个分配和释放逻辑完全基于虚拟地址和指针计算。物理地址根本不出现在这些函数中。

### 3. 物理地址仅在需要时提取

当需要物理地址时（用于页表、DMA、统计），才从 struct page 中提取：

```c
// 从虚拟地址（struct page 指针）提取物理标识
unsigned long pfn = page_to_pfn(page);          // (page - mem_map)
unsigned long phys_addr = page_to_phys(page);   // pfn << PAGE_SHIFT
```

这些 PFN 的用途：
- **页表**：CPU MMU 需要物理地址查表
- **DMA**：设备需要物理地址访问内存
- **统计**：跟踪内存范围

但它们**不参与伙伴系统的分配/释放逻辑**。

### 4. 高内存的典型应用

项目代码中的 HighMem 处理展示了为什么需要区分两种地址：

```c
// page_alloc.c:845-847
if (j != ZONE_HIGHMEM)
    page->virtual = __va(zone_start_paddr);  // DMA/Normal 有直接虚拟映射
else
    page->virtual = 0;                       // HighMem 无直接映射
```

**含义**：
- **ZONE_DMA / ZONE_NORMAL**：直接映射区可用，`__va()` 可快速转换
- **ZONE_HIGHMEM**：超出直接映射范围，`page->virtual = 0`，后续需 `kmap()` 动态映射

这说明虚拟地址映射是有限的，但物理地址总是有的。

### 5. 核心原则总结

| 操作 | 使用哪种地址 | 项目中的例子 |
|------|------------|-----------|
| **访问 struct page** | 虚拟地址 | `mem_map + offset + i`（page_alloc.c:844） |
| **伙伴系统操作** | 虚拟地址 | `rmqueue()`、`__free_pages_ok()`的全过程 |
| **标识 zone 位置** | 物理地址 | `zone->zone_start_paddr`（page_alloc.c:831） |
| **页表操作** | 物理地址 | MMU 需要 PFN |
| **设备 DMA** | 物理地址 | 驱动需要物理地址 |
| **直接映射转换** | __va() / __pa() | 初始化 `page->virtual`（page_alloc.c:846） |

---

## 工程师判断法

当读代码时，快速判断使用哪种地址：

1. **这是访问 struct page 或指针运算吗？**
   → 虚拟地址（内核虚拟地址）

2. **这是填写页表条目或设置页表权限吗？**
   → 物理地址（PFN）

3. **这是 DMA 或硬件相关操作吗？**
   → 物理地址

4. **这涉及 __va() 或 __pa() 宏吗？**
   → 虚拟-物理地址转换

5. **这是在初始化 zone 或内存范围吗？**
   → 通常同时用到（物理标识，虚拟访问）

## Key Points

- **伙伴系统完全虚拟地址驱动**：分配和释放逻辑都通过 struct page 指针和虚拟地址计算进行
- **物理地址是后补**：仅在需要与硬件交互时提取（页表、DMA）
- **mem_map 是虚拟地址数组**：zone_mem_map 指向 mem_map 内的某个偏移
- **直接映射支撑转换**：__va() 能工作是因为直接映射区 (PAGE_OFFSET 开始) 的存在
- **高内存的特殊性**：超出直接映射范围，虚拟映射需动态建立
- **PFN = 标识符**：page_to_pfn() 和 page_to_phys() 用于提取标识，不用于访问

## Related Concepts

- **[Address Translation](./address-translation.md)**：__va() 和 __pa() 的实现与架构差异
- **[mm-core-variables](./mm-core-variables.md)**：mem_map 和虚拟地址管理
- **[Zone-Based Memory Management](./zone-based-memory-management.md)**：zone 结构与直接映射范围

## 如何在项目中应用这个理解

1. **阅读伙伴系统代码**（page_alloc.c:126-286）
   - 观察所有操作都使用虚拟地址（struct page 指针）
   - 注意没有物理地址参与分配/释放逻辑

2. **阅读初始化代码**（page_alloc.c:730-870）
   - 看 zone_start_paddr 如何标识物理位置
   - 看 zone_mem_map 如何指向虚拟地址数组
   - 看 __va() 如何在初始化时转换

3. **追踪驱动接口**（未在项目中，但相关）
   - DMA 驱动需要 page_to_phys() 提取物理地址
   - 页表操作需要 pfn 填入 PTE
