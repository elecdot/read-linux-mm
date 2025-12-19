---
related:
- "[Memory Area Management](./memory-area-management.md)"
- "[Address Translation](./address-translation.md)"
- "[Buddy System](./buddy-system.md)"
- "[Highmem Kernel Mapping](./highmem-kernel-mapping.md)"
tags:
- memory-management
- vmalloc
- noncontiguous-memory
- kernel-address-space
sources:
- "[linux/mm/vmalloc.c](/linux/mm/vmalloc.c)"
- "[linux/include/linux/vmalloc.h](/linux/include/linux/vmalloc.h)"
- "[linux/include/asm-i386/pgtable.h](/linux/include/asm-i386/pgtable.h)"
---

/*! \page noncontiguous-memory-area Noncontiguous Memory Area Management

This page introduces:
Noncontiguous Memory Area Management（非连续内存区域管理）是 Linux 内核在虚拟地址空间中分配连续区域，但其背后的物理页框在物理内存中并不连续的机制。这主要通过 `vmalloc` 系列函数实现。
*/

# Noncontiguous Memory Area Management

## In a Word

Noncontiguous Memory Area Management（非连续内存区域管理）允许内核申请一块在 **线性地址空间连续** 但 **物理地址空间不连续** 的内存区域。这主要用于当内核需要大块内存，但物理内存已经碎片化，无法提供足够大的连续页框序列时。

## Why This Concept

1.  **应对物理碎片**：随着系统运行，物理内存会产生碎片。`vmalloc` 可以通过修改页表，将零散的物理页框映射到一段连续的虚拟地址。
2.  **大块内存需求**：某些内核组件（如模块加载、大型缓冲区）需要比伙伴系统能提供的最大连续块（通常是 4MB）更大的内存。
3.  **访问高显存/IO空间**：`ioremap` 也利用了这一机制将非连续的硬件寄存器区域映射到内核虚拟地址空间。

## Deep Dive

### 1. 线性地址布局

在 x86 架构中，内核线性地址空间（3GB-4GB）被划分为几个部分。非连续内存区域位于 `VMALLOC_START` 和 `VMALLOC_END` 之间。

-   **`VMALLOC_START`**：通常在物理内存映射区（`high_memory`）之后，留有一个 8MB 的安全空洞（`VMALLOC_OFFSET`）。
-   **`VMALLOC_END`**：取决于是否开启了 `CONFIG_HIGHMEM`。如果开启，则止于 `PKMAP_BASE` 之前；否则止于 `FIXADDR_START` 之前。

### 2. 核心数据结构：`struct vm_struct`

每个非连续内存区域由一个 `struct vm_struct` 描述：

```c
struct vm_struct {
    unsigned long flags;    /* 标志位：VM_ALLOC (vmalloc), VM_IOREMAP (ioremap) */
    void * addr;            /* 区域在虚拟地址空间中的起始地址 */
    unsigned long size;     /* 区域大小（包含 4KB 的安全空洞） */
    struct vm_struct * next;/* 指向下一个区域的指针（单向链表） */
};
```

所有的 `vm_struct` 实例通过全局链表 `vmlist` 组织，并由 `vmlist_lock` 读写锁保护。

### 3. 核心函数深度解析 (Function-by-Function Deep Dive)

#### A. 分配阶段：从虚拟到物理的编织

##### 1. `get_vm_area(size, flags)`：虚拟地址空间的“房产中介”
*   **角色**：在 `VMALLOC_START` 到 `VMALLOC_END` 之间找到一块足够大的连续空闲区域。
*   **输入参数**：
    *   `size`: 请求的字节数。
    *   `flags`: 区域类型（如 `VM_ALLOC` 表示 vmalloc，`VM_IOREMAP` 表示 ioremap）。
*   **核心逻辑**：
    1.  **大小修正**：将请求的 `size` 加上一个 `PAGE_SIZE`（即 Guard Page）。
    2.  **加锁查找**：持有 `vmlist_lock` 写锁，遍历全局链表 `vmlist`。
    3.  **First-fit 算法**：寻找两个相邻 `vm_struct` 之间的空隙。如果 `next->addr - (prev->addr + prev->size) >= size`，则找到了合适的坑位。
    4.  **边界处理**：如果链表为空或到结尾还没找到，则检查最后一个元素到 `VMALLOC_END` 之间的空间。
    5.  **创建描述符**：分配一个新的 `struct vm_struct`，填入起始地址和大小，插入链表。
*   **关键点**：它只负责“占坑”，并不分配任何物理内存。返回的是一个 `vm_struct` 指针。

##### 2. `vmalloc_area_pages(address, size, gfp_mask, prot)`：页表映射的“总指挥”
*   **角色**：为 `get_vm_area` 找好的虚拟地址建立对应的物理页表映射。
*   **核心逻辑**：
    1.  **三级遍历**：它通过循环调用 `alloc_area_pmd` 来处理每一个 PGD 目录项。
    2.  **内核页表**：它修改的是 `init_mm.pgd`，即内核的主页表。
    3.  **地址推进**：每次处理一个 PGD 项（通常映射 4MB 区域），地址指针向前推进。
*   **关键点**：如果映射过程中任何一级分配失败，它会返回错误，触发上层的回滚清理。

##### 3. `alloc_area_pmd` & `alloc_area_pte`：递归下钻的“施工队”
*   **角色**：分别处理中间目录 (PMD) 和页表项 (PTE)。
*   **核心逻辑 (`alloc_area_pte`)**：
    1.  **释放锁以允许睡眠**：这是最精彩的地方。在调用 `alloc_page` 分配物理页之前，它会调用 `spin_unlock(&init_mm.page_table_lock)`。因为 `alloc_page` 可能会因为内存压力进入睡眠，而持有自旋锁时是不允许睡眠的。
    2.  **分配物理页**：调用伙伴系统的 `alloc_page` 获取一个页框。
    3.  **重新加锁并填表**：分配成功后，重新获得 `page_table_lock`，使用 `set_pte` 将物理地址填入页表。
    4.  **原子性检查**：重新拿锁后，会检查该位置是否已经被别人填过了（虽然在 vmalloc 这种私有区域不太可能，但这是标准流程）。
*   **关键点**：这种“放锁-分配-拿锁”的模式是内核处理长耗时操作的典型范式。

##### 4. `__vmalloc(size, gfp_mask, prot)`：整个流程的“大管家”
*   **角色**：协调上述所有步骤，提供最终接口。
*   **核心逻辑**：
    1.  `PAGE_ALIGN` 对齐大小。
    2.  调用 `get_vm_area` 获取虚拟空间。
    3.  调用 `vmalloc_area_pages` 循环分配物理页并建立映射。
    4.  **错误处理**：如果 `vmalloc_area_pages` 失败，立即调用 `vfree` 释放已经分配的部分。

#### B. 释放阶段：拆除与回收的艺术

##### 1. `vfree(addr)`：释放操作的“总入口”
*   **角色**：回收由 `vmalloc` 分配的所有资源。
*   **核心逻辑**：
    1.  **查找并摘除**：持有 `vmlist_lock` 写锁，从 `vmlist` 中找到 `addr` 对应的 `vm_struct` 并将其从链表中移除。
    2.  **调用清理**：调用 `vmfree_area_pages` 进行实际的页表拆除。
    3.  **释放描述符**：最后 `kfree` 掉 `vm_struct` 结构体。

##### 2. `vmfree_area_pages(address, size)`：页表拆除的“爆破组”
*   **角色**：遍历指定区域的页表，解除映射并释放物理页。
*   **核心逻辑**：
    1.  **前置刷新**：在开始拆除前调用 `flush_cache_all()`。
    2.  **逐级清理**：通过 `free_area_pmd` 和 `free_area_pte` 递归清理。
    3.  **后置刷新**：全部清理完成后，调用 `flush_tlb_all()`。
*   **关键点**：必须先刷 Cache 再改页表，最后刷 TLB，顺序不可颠倒。

##### 3. `free_area_pte`：物理页的“回收站”
*   **角色**：处理最底层的 PTE 项。
*   **核心逻辑**：
    1.  使用 `ptep_get_and_clear` 原子地读取并清除 PTE。
    2.  **物理回收**：如果该页存在 (`pte_present`)，调用 `__free_page` 将物理页还给伙伴系统。
*   **关键点**：它不仅清除了页表项，还真正释放了物理内存。

### 4. 并发与同步 (Concurrency)

在多处理器 (SMP) 环境下，`vmalloc` 必须处理好竞态条件，主要涉及两把锁：

1.  **`vmlist_lock` (读写锁)**：
    *   **保护对象**：全局链表 `vmlist`。
    *   **使用场景**：`get_vm_area` 查找空隙时使用写锁；`vfree` 移除节点时使用写锁。
2.  **`init_mm.page_table_lock` (自旋锁)**：
    *   **保护对象**：内核主页表（PGD/PMD/PTE）。
    *   **使用场景**：`vmalloc_area_pages` 和 `vmfree_area_pages` 修改页表项时必须持有。
    *   **特殊设计**：在 `alloc_area_pte` 中，为了允许 `alloc_page` 睡眠，内核会暂时释放此锁。这体现了内核在“保护数据”与“系统响应性”之间的权衡。

### 5. 函数速查表 (Function Summary Table)

| 函数名 | 所在文件 | 主要职责 | 锁需求 |
| :--- | :--- | :--- | :--- |
| `get_vm_area` | `vmalloc.c` | 查找并预留虚拟地址空间 | `vmlist_lock` (写) |
| `vmalloc_area_pages` | `vmalloc.c` | 建立虚拟地址到物理页的映射 | `page_table_lock` |
| `alloc_area_pte` | `vmalloc.c` | 分配物理页并填充 PTE | `page_table_lock` (中途释放) |
| `__vmalloc` | `vmalloc.c` | 封装分配全过程 | 无 (内部处理) |
| `vfree` | `vmalloc.c` | 释放虚拟空间及物理页 | `vmlist_lock` (写) |
| `vmfree_area_pages` | `vmalloc.c` | 拆除页表映射 | `page_table_lock` |
| `free_area_pte` | `vmalloc.c` | 清除 PTE 并释放物理页 | `page_table_lock` |

### 5. 典型执行流：以 `vmalloc(100KB)` 为例

1.  **对齐**：100KB 向上对齐为 104KB (26页)。
2.  **占坑**：`get_vm_area` 申请 104KB + 4KB (Guard Page) = 108KB 的虚拟空间。
3.  **循环分配**：
    *   `vmalloc_area_pages` 开始工作。
    *   进入 `alloc_area_pte`。
    *   **放锁** -> `alloc_page` (第1页) -> **拿锁** -> 填入页表。
    *   ... 重复 26 次 ...
4.  **同步**：`flush_cache_all()`。
5.  **返回**：返回起始线性地址。

### 6. 衍生函数：`vmalloc_dma` 与 `ioremap`

*   **`vmalloc_dma(size)`**：与 `vmalloc` 逻辑完全一致，唯一的区别是它在调用 `vmalloc_area_pages` 时传入的 `gfp_mask` 包含 `GFP_DMA`。这确保了分配的物理页都在 DMA 区域内。
*   **`ioremap(offset, size)`**：它也调用 `get_vm_area` 来占坑，但它**不分配**物理页。它直接将硬件寄存器的物理地址映射到找好的虚拟空间中。

### 7. 安全机制：Guard Page

为了检测缓冲区溢出，`vmalloc` 在每个分配的区域末尾都会额外预留一个 4KB 的页框（不建立映射）。如果内核代码访问了这个“空洞”，会触发 Page Fault，从而帮助开发者定位 Bug。

## Comparison: kmalloc vs vmalloc

| 特性 | `kmalloc` | `vmalloc` |
| :--- | :--- | :--- |
| **物理连续性** | 必须连续 | 不一定连续 |
| **线性连续性** | 连续 | 连续 |
| **分配上限** | 受伙伴系统限制 (较小) | 很大 (受虚拟地址空间限制) |
| **性能开销** | 小（仅偏移计算） | 大（需要修改页表、刷新 TLB） |
| **适用场景** | 频繁分配的小对象、DMA | 模块加载、大缓冲区 |

---
## 延伸阅读
*   [Buddy System](buddy-system.md) - 物理页框的来源。
*   [Slab Allocator](slab-allocator.md) - 另一种基于物理连续内存的分配器。
