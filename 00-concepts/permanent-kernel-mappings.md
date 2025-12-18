---
related:
- "[Kernel Mappings of High-Memory Page Frames](highmem-kernel-mapping.md)"
- "[Zoned Page Frame Allocator](zoned-page-frame-allocator.md)"
- "[Zone-Based Memory Management](zone-based-memory-management.md)"
- "[Page Descriptor (mem_map)](mm-core-variables.md)"
- "[Address Translation](address-translation.md)"
tags:
- memory-management
- high-memory
- kernel-mapping
- kmap
- page-frame
- linear-address
sources:
- "[include/linux/highmem.h](/linux/include/linux/highmem.h)"
- "[mm/highmem.c](/linux/mm/highmem.c)"
- "[arch/i386/mm/init.c](/linux/arch/i386/mm/init.c)"
---

/*! \page permanent-kernel-mappings Permanent Kernel Mappings (kmap)

This page introduces:
永久内核映射（kmap/kunmap）是内核用于访问高端内存页框的长期映射机制。它在 PKMAP 区（Page Kernel MAPping）中动态分配虚拟地址槽位，支持进程上下文中的灵活映射，但可能会睡眠等待可用槽位。
*/

# 永久内核映射 (Permanent Kernel Mappings)

## 一句话总结 (In a Word)

**永久内核映射** (`kmap()` / `kunmap()`) 是用于在进程上下文中访问高端内存页框的映射机制。
它在 PKMAP 区（2-4MB 的动态映射窗口）中为每个高端页分配虚拟地址，支持同时多页映射，但在槽位耗尽时会阻塞等待。

## 核心问题 (Core Problem)

### 为什么需要 kmap？

```
高端内存(>896MB)     内核线性地址空间
  ┌─────────────┐    ┌──────────────┐
  │  Page H1    │    │  FIXMAP      │ 固定槽位(4-8个)
  │  Page H2    │──→ │  PKMAP       │ 动态槽位(512-1024个) ← kmap() 使用
  │  Page H3    │    │  VMALLOC     │ 虚拟连续
  │  ...        │    └──────────────┘
  └─────────────┘
    无固定映射       内核可寻址区
```

高端内存的页框没有固定的内核线性地址，必须通过 **临时建立页表项** 的方式来映射。
`kmap()` 的目标是在 PKMAP 区中找到（或创建）一个映射项，将虚拟地址与物理页关联。

## 深入理解 (Deep Dive)

### PKMAP 区的设计

PKMAP 区（Page Kernel MAPping）位于内核地址空间的最高位，是专为高端内存映射保留的动态映射区。

**地址分布**：
```
0xFFFF_FFFF ┌──────────────────────┐
            │ FIXMAP (~32KB)       │ ← kmap_atomic 的固定槽位
            ├──────────────────────┤ 0xFFFF_E000
            │                      │
            │ PKMAP (2-4MB)        │ ← kmap() 的动态槽位
            │ [LAST_PKMAP 槽位]    │
            │                      │
0xFE00_0000 ├──────────────────────┤ PKMAP_BASE = 0xfe000000
            │ VMALLOC (~120MB)     │
            ├──────────────────────┤
0xC080_0000 │ Direct Mapping (896MB)
```

**关键常数**：
- `PKMAP_BASE = 0xfe000000`：PKMAP 区起始地址
- `LAST_PKMAP = 512` (PAE) 或 `1024` (非 PAE)：最大同时映射页数
- PKMAP 大小：512 × 4KB = 2MB (PAE) 或 1024 × 4KB = 4MB (非 PAE)

### 核心数据结构

#### 1. `pkmap_page_table`（页表指针）
```c
// 声明位置: include/asm-i386/highmem.h
extern pte_t *pkmap_page_table;

// 定义位置: mm/highmem.c
pte_t * pkmap_page_table;

// 初始化: arch/i386/mm/init.c
pgd = swapper_pg_dir + __pgd_offset(PKMAP_BASE);
pmd = pmd_offset(pgd, PKMAP_BASE);
pte = pte_offset(pmd, PKMAP_BASE);
pkmap_page_table = pte;  // 指向 PKMAP 页表的首个 PTE
```

**作用**：指向 PKMAP 区页表的起点。通过修改这个页表，可以改变 PKMAP 区虚拟地址与物理页的对应关系。

#### 2. `pkmap_count[]`（引用计数数组）
```c
// mm/highmem.c
static int pkmap_count[LAST_PKMAP];  // 512 或 1024 个元素

// 引用计数规则
// 0      ← 槽位未使用（可以分配）
// 1      ← 槽位已映射但无人使用（可以回收清理）
// n (n>=2) ← 槽位被 (n-1) 个使用者占用（不能回收）
```

**作用**：追踪每个 PKMAP 槽位的使用状态。

#### 3. `page.virtual`（页的虚拟地址字段）
```c
// struct page 中的字段
struct page {
    void *virtual;  // 虚拟地址
    // ...
};

// 对于低端页：存储直接映射地址
// 对于高端页：存储 kmap() 返回的虚拟地址，或 NULL（未映射）
```

**作用**：存储页的虚拟地址。通过 `page_address(page)` 宏获取：
```c
#define page_address(page) ((page)->virtual)
```

### kmap() 的工作流程

```c
void *kmap(struct page *page)
{
    // 1. 如果是低端内存页，直接返回固定映射地址
    if (!PageHighMem(page))
        return page_address(page);
    
    // 2. 对于高端内存，在 PKMAP 中寻找映射
    return kmap_high(page);
}

// kmap_high() 的伪代码
static inline void *kmap_high(struct page *page)
{
    unsigned long vaddr;
    
    // 2.1 检查是否已映射（page.virtual != NULL）
    if (page->virtual)
        return page->virtual;  // 已有映射，直接返回
    
    // 2.2 在 PKMAP 中寻找空闲槽位
    vaddr = map_new_virtual(page);
    
    // 2.3 建立页表映射
    set_pte(&pkmap_page_table[PKMAP_NR(vaddr)], 
            mk_pte(page, kmap_prot));
    
    // 2.4 记录虚拟地址到 page 结构
    page->virtual = vaddr;
    
    return vaddr;
}
```

### 槽位分配算法

关键函数：`map_new_virtual(page)`（位于 `mm/highmem.c`）

**寻找空闲槽位的流程**：
```
1. 从 last_pkmap_nr 开始，循环查找第一个 pkmap_count[i] == 0 的槽位
   last_pkmap_nr = (last_pkmap_nr + 1) & LAST_PKMAP_MASK;

2. 如果 last_pkmap_nr 绕回 0（遍历一圈）：
   → 调用 flush_all_zero_pkmaps()：清理所有 pkmap_count == 1 的槽位
   → 重新计数并继续寻找

3. 如果仍无空闲槽位且已尝试 LAST_PKMAP 次：
   → 当前进程 **睡眠** 等待 pkmap_map_wait 队列
   → 等待其他进程调用 kunmap() 释放槽位
   → 被唤醒后重新尝试
```

这就是 **kmap() 为什么会阻塞** 的原因——在高并发多页映射场景中，可能需要等待槽位释放。

### 槽位清理机制

`flush_all_zero_pkmaps()` 的作用：
```
对于每个 pkmap_count[i] == 1 的槽位：
1. 清除其 PTE（清除虚拟→物理映射）
2. 将 pkmap_count[i] 设为 0（标记为可用）
3. 清除对应 page 的 virtual 字段
4. 刷新 TLB（清除 CPU 缓存的地址转换）
```

### kunmap() 的工作流程

```c
void kunmap(struct page *page)
{
    unsigned long vaddr;
    int nr;
    
    // 1. 验证不在中断上下文（kmap 只能在进程上下文使用）
    if (in_interrupt())
        BUG();
    
    // 2. 低端页无需处理（从不映射）
    if (!PageHighMem(page))
        return;
    
    // 3. 递减引用计数
    vaddr = (unsigned long)page->virtual;
    nr = PKMAP_NR(vaddr);
    pkmap_count[nr]--;
    
    // 4. 如果引用计数变为 0，标记为可回收
    if (pkmap_count[nr] == 1) {
        // 注：count=1 表示"已映射但无人使用"，下次 flush 会清理
    }
    
    // 5. 唤醒等待 pkmap_map_wait 的其他进程
    wake_up(&pkmap_map_wait);
}
```

### 关键宏定义

| 宏 | 定义 | 说明 |
|-----|------|------|
| `PKMAP_BASE` | `0xfe000000UL` | PKMAP 区起始地址 |
| `LAST_PKMAP` | 512 / 1024 | 最大槽位数 |
| `LAST_PKMAP_MASK` | `LAST_PKMAP - 1` | 掩码，用于循环运算 |
| `PKMAP_NR(vaddr)` | `(vaddr - PKMAP_BASE) >> PAGE_SHIFT` | 虚拟地址 → 槽位号 |
| `PKMAP_ADDR(nr)` | `PKMAP_BASE + (nr << PAGE_SHIFT)` | 槽位号 → 虚拟地址 |
| `page_address(page)` | `((page)->virtual)` | 获取页的虚拟地址 |

## 与其他映射机制的对比

| 特性 | kmap | kmap_atomic | vmalloc |
|------|------|------------|---------|
| **区域** | PKMAP (2-4MB) | FIXMAP (~32KB) | VMALLOC (~120MB) |
| **槽位数** | 512/1024 个 (共享) | 4-8 个 (CPU独占) | 连续虚拟地址 |
| **能否阻塞** | ✅ 能 | ❌ 否 | ✅ 能 |
| **适用上下文** | 仅进程上下文 | 任意(含中断) | 仅进程上下文 |
| **使用场景** | 单个/少数几个页的长期映射 | 极短暂的高速访问 | 大块连续虚拟地址 |
| **性能** | 中等 | 最快 | 慢(TLB开销) |

## 实际应用场景

### 场景1：页面缓存读取（进程上下文）
```c
// 文件系统从页缓存读取数据
void read_from_page_cache(struct page *page) {
    void *kaddr = kmap(page);      // 映射（可能阻塞）
    if (!kaddr) return;
    
    // 处理数据（可能需要一段时间）
    process_page_data(kaddr);
    
    kunmap(page);                  // 立即释放
}
```

### 场景2：多个高端页的顺序遍历
```c
for (i = 0; i < page_count; i++) {
    void *vaddr = kmap(pages[i]);
    // ... 使用 vaddr ...
    kunmap(pages[i]);  // 立即释放，为下一个页腾出槽位
}
```

### 场景3：递归或嵌套映射（要小心！）
```c
// ⚠️ 注意：嵌套 kmap 可能导致槽位枯竭

// 如果多层循环嵌套，可能同时需要多个槽位
for (...) {
    v1 = kmap(page1);
    for (...) {
        v2 = kmap(page2);  // 消耗更多槽位
        ...
        kunmap(page2);
    }
    kunmap(page1);
}

// 大深度嵌套时，优先使用 vmalloc 或其他方式
```

## 性能特性

**优点**：
- 支持**多个页同时映射**（512/1024 槽位）
- **灵活的生命周期**（从 kmap 到 kunmap 之间可以任意长）
- **进程上下文中无额外限制**（可以睡眠、分页等）

**缺点**：
- **可能阻塞**：在高并发场景槽位耗尽时会睡眠
- **槽位数有限**：最多 2-4MB，若映射更大区域需循环 kmap/kunmap
- **全局锁竞争**：所有 CPU 竞争 kmap_lock，可能成为瓶颈

## 初始化过程

### i386 架构的初始化

```c
// arch/i386/mm/init.c
void __init paging_init(void) {
    unsigned long vaddr;
    pte_t *pte;
    
    // 1. 为 PKMAP 区分配页表
    vaddr = PKMAP_BASE;
    fixrange_init(vaddr, vaddr + PAGE_SIZE*LAST_PKMAP, pgd_base);
    
    // 2. 获取 PKMAP 页表的首个 PTE
    pgd = swapper_pg_dir + __pgd_offset(vaddr);
    pmd = pmd_offset(pgd, vaddr);
    pte = pte_offset(pmd, vaddr);
    
    // 3. 保存到全局指针
    pkmap_page_table = pte;
}

// mm/highmem.c
void __init kmap_init(void) {
    // 初始化所有槽位计数器为 0（未使用）
    for (i = 0; i < LAST_PKMAP; i++)
        pkmap_count[i] = 0;
}
```

## 总结 (Summary)

| 特点 | 说明 |
|------|------|
| **什么** | 在 PKMAP 区（2-4MB）中为高端内存页动态分配虚拟地址 |
| **何时用** | 进程上下文中需要访问高端内存页，但不需原子操作 |
| **怎么用** | `void *vaddr = kmap(page); /* 使用 */ kunmap(page);` |
| **限制** | 只能在进程上下文；槽位有限（512/1024）；可能阻塞 |
| **对比** | `kmap_atomic` 更快但槽位少；`vmalloc` 虚拟连续但开销大 |

永久内核映射是现代内核处理高端内存的核心机制，平衡了性能、灵活性和内存使用效率。
