---
related:
- "[Fix-Mapped Linear Addresses](fix-mapped-linear-addresses.md)"
- "[Permanent Kernel Mappings (kmap)](permanent-kernel-mappings.md)"
- "[Kernel Mappings of High-Memory Page Frames](highmem-kernel-mapping.md)"
- "[Zoned Page Frame Allocator](zoned-page-frame-allocator.md)"
- "[Zone-Based Memory Management](zone-based-memory-management.md)"
- "[Page Descriptor (mem_map)](mm-core-variables.md)"
- "[Address Translation](address-translation.md)"
tags:
- memory-management
- high-memory
- kernel-mapping
- kmap_atomic
- FIXMAP
- page-frame
- atomic-context
sources:
- "[include/asm-i386/highmem.h](/linux/include/asm-i386/highmem.h)"
- "[include/asm-i386/fixmap.h](/linux/include/asm-i386/fixmap.h)"
- "[mm/highmem.c](/linux/mm/highmem.c)"
---

/*! \page temporary-kernel-mappings Temporary Kernel Mappings (kmap_atomic)

This page introduces:
临时内核映射（kmap_atomic）是内核在中断或原子上下文中访问高端内存的机制, 用于能够非睡眠获取high_mem。它使用 FIXMAP 区的 CPU 私有槽位，保证原子性和高效性，但生命周期短暂且不能睡眠。
*/

# 临时内核映射 (Temporary Kernel Mappings)

## 一句话总结 (In a Word)

**临时内核映射** (`kmap_atomic()` / `kunmap_atomic()`) 是用于在中断或原子上下文中快速访问高端内存的映射机制。
它在 FIXMAP 区为每个 CPU 分配专用槽位，绝不睡眠，适合极短暂的页面访问。

## Why this Concept? 核心问题：中断上下文中无法使用 kmap()

### 为什么需要 kmap_atomic？

```
问题：中断处理程序中访问高端内存页
  ├─ 不能使用 kmap()：
  │   ├─ 可能睡眠等待槽位
  │   └─ 中断上下文不允许睡眠 (会破坏内核状态)
  │
  └─ 解决方案：kmap_atomic()
      ├─ 绝不睡眠（spinlock + 预分配槽位）
      ├─ 超快访问（仅设置一个 PTE + TLB flush）
      └─ 原子性保证
```

### FIXMAP 区的关键特点

FIXMAP（Fixed Mapping）是编译期分配的特殊虚拟地址区，位于内核地址空间的最高处，紧邻 PKMAP 区下方。

**地址分布**：
```
0xFFFF_FFFF ┌──────────────────────┐
            │ FIXMAP 起始          │ 0xffffe000
            ├──────────────────────┤
            │ [CPU 0 kmap槽位 0]   │ FIX_KMAP_BEGIN + 0
            │ [CPU 0 kmap槽位 1]   │ FIX_KMAP_BEGIN + 1
            │ [CPU 0 kmap槽位 ...] │
            ├──────────────────────┤
            │ [CPU 1 kmap槽位 0]   │ FIX_KMAP_BEGIN + KM_TYPE_NR
            │ [CPU 1 kmap槽位 1]   │
            │ [CPU 1 kmap槽位 ...] │
            ├──────────────────────┤
            │ ...其他CPU的槽位...  │
            ├──────────────────────┤
            │ 其他 FIXMAP 条目     │ (APIC, I/O APIC等)
            │ (编译期固定)         │
0xFE00_0000 └──────────────────────┘ FIXMAP_BASE ~ 0xffffe000
```

**核心常数**：
- `FIXMAP_BASE = 0xffe00000`：FIXMAP 区起始地址
- `KM_TYPE_NR`：每个 CPU 的槽位数（通常为 4-8）
- 总槽位数：`KM_TYPE_NR × NR_CPUS`（例如：8 × 4 CPU = 32 个槽位）
- 每个槽位：4KB（一页）

## Deep Dive

### 1. km_type 枚举（槽位类型）

```c
// include/asm-i386/kmap_types.h
enum km_type {
    KM_BOUNCE_READ,      // 槽位 0
    KM_SKB_SUNRPC_DATA,  // 槽位 1
    KM_SKB_DATA_SOFTIRQ, // 槽位 2
    KM_USER0,            // 槽位 3
    KM_USER1,            // 槽位 4
    KM_BIO_SRC_IRQ,      // 槽位 5
    KM_BIO_DST_IRQ,      // 槽位 6
    KM_PTE0,             // 槽位 7
    KM_PTE1,             // 槽位 8
    // ...
    KM_TYPE_NR           // 总槽位数（通常为 8 或 16）
};
```

**设计含义**：
- 不同的 `km_type` 对应不同的"执行路径"或"场景"
- 例如：网络软中断用 `KM_SKB_DATA_SOFTIRQ`，页表操作用 `KM_PTE0`
- 防止同一槽位在嵌套执行中被覆盖（虽然不是绝对保证，但减少冲突概率）

### 2. fixed_addresses 与 FIXMAP 索引

```c
// include/asm-i386/fixmap.h
enum fixed_addresses {
    FIX_APIC_BASE,            // 第 0 个条目
    FIX_IO_APIC_BASE_0,       // ...
    // ...
    FIX_KMAP_BEGIN,           // kmap_atomic 的首个条目
    FIX_KMAP_END = FIX_KMAP_BEGIN + (KM_TYPE_NR*NR_CPUS - 1),
    // ...
    FIX_HOLE,                 // 最后一个（标记结束）
    __end_of_fixed_addresses  // 终止符
};
```

**槽位计算**：
```c
enum fixed_addresses idx = FIX_KMAP_BEGIN + (type + KM_TYPE_NR * smp_processor_id());
```

这个公式确保：
- 不同 CPU 使用不同槽位（`* smp_processor_id()`）
- 同一 CPU 的不同类型使用不同槽位（`+ type`）
- 各槽位物理上不相邻，避免 TLB 预取干扰

### 3. kmap_pte 与 kmap_prot

```c
// include/asm-i386/highmem.h
pte_t *kmap_pte;           // 指向 FIXMAP 页表首个 PTE
pgprot_t kmap_prot;        // FIXMAP 页表项的权限（通常为 PAGE_KERNEL）

// 初始化（arch/i386/mm/init.c）
kmap_pte = kmap_pte_init();  // 获取 FIXMAP 的 PMD/PTE
kmap_prot = PAGE_KERNEL;
```

## 实现细节：kmap_atomic 和 kunmap_atomic

### kmap_atomic 流程

```c
static inline void *kmap_atomic(struct page *page, enum km_type type)
{
    enum fixed_addresses idx;
    unsigned long vaddr;

    // 1. 快速路径：低内存页直接返回
    if (page < highmem_start_page)
        return page_address(page);

    // 2. 计算此 CPU 在 FIXMAP 中的槽位索引
    idx = type + KM_TYPE_NR * smp_processor_id();
    
    // 3. 虚拟地址转换（从索引到虚拟地址）
    vaddr = __fix_to_virt(FIX_KMAP_BEGIN + idx);

    // 4. 调试检查：槽位应该是空的（未被占用）
#if HIGHMEM_DEBUG
    if (!pte_none(*(kmap_pte - idx)))
        BUG();
#endif

    // 5. 设置 PTE：将虚拟地址绑定到物理页
    //    kmap_pte - idx: 指向该槽位的 PTE（倒序排列）
    //    mk_pte(page, kmap_prot): 创建 PTE，权限为 PAGE_KERNEL
    set_pte(kmap_pte - idx, mk_pte(page, kmap_prot));

    // 6. TLB flush：刷新 TLB，确保 CPU 缓存新的映射
    __flush_tlb_one(vaddr);

    // 7. 返回虚拟地址
    return (void*) vaddr;
}
```

**关键点**：
- **无自旋锁**：不需要 spinlock（与 kmap 的 kmap_lock 对比），因为每个 CPU 有专属槽位
- **无睡眠**：全是原子操作（PTE 设置 + TLB flush）
- **超快速**：仅修改一个 PTE 和 TLB 项

### kunmap_atomic 流程

```c
static inline void kunmap_atomic(void *kvaddr, enum km_type type)
{
#if HIGHMEM_DEBUG
    unsigned long vaddr = (unsigned long) kvaddr;
    enum fixed_addresses idx = type + KM_TYPE_NR * smp_processor_id();

    // 1. 快速路径检查：如果不在 FIXMAP 范围，直接返回
    if (vaddr < FIXADDR_START)
        return;

    // 2. 调试检查：虚拟地址必须精确匹配（防止混淆）
    if (vaddr != __fix_to_virt(FIX_KMAP_BEGIN + idx))
        BUG();

    // 3. 清除 PTE（移除映射）
    pte_clear(kmap_pte - idx);

    // 4. 再次 TLB flush（确保移除生效）
    __flush_tlb_one(vaddr);
#endif
}
```

**关键点**：
- **调试模式才实际清除**：在 `HIGHMEM_DEBUG` 关闭时，`kunmap_atomic` 几乎是空操作（只是返回）
- **为什么？**释放后立即重用槽位，下一次 `set_pte` 会覆盖旧值，不必手动清除
- **但调试模式下**会显式清除，以便立即发现使用后再访问的 bug

## 使用场景与限制

### 何时使用 kmap_atomic

```
当前执行上下文？
  ├─ 中断处理程序（top-half）
  │   ├─ 硬件中断
  │   ├─ 软中断（softirq）
  │   └─ 使用 kmap_atomic ✓
  │
  ├─ 已禁用抢占的临界区（spinlock 持有时）
  │   └─ 使用 kmap_atomic ✓ (实际是同样的约束)
  │
  └─ 进程上下文
      ├─ 已持有 spinlock？ → kmap_atomic（保持原子性）
      └─ 否则？           → kmap()（更通用）
```

### 典型使用场景

#### 场景 1：网络驱动中断处理

```c
// 网络驱动接收中断
static irqreturn_t rx_irq_handler(int irq, void *dev_id, struct pt_regs *regs)
{
    struct page *page = alloc_page(GFP_ATOMIC);  // 从高端内存分配
    void *data = kmap_atomic(page, KM_SKB_DATA_SOFTIRQ);
    
    // 在中断上下文中安全访问页数据
    memcpy(data, hw_buffer, PAGE_SIZE);
    
    kunmap_atomic(data, KM_SKB_DATA_SOFTIRQ);
    // 页的生命周期继续...
    return IRQ_HANDLED;
}
```

#### 场景 2：页表操作（TLB shootdown）

```c
// 修改远程 CPU 的页表时
void zap_pte_range(struct mm_struct *mm, pmd_t *pmd, ...)
{
    pte_t *ptep = (pte_t *)kmap_atomic(pte_page, KM_PTE0);
    // 在原子上下文中修改页表项
    *ptep = pte_mkold(*ptep);
    kunmap_atomic(ptep, KM_PTE0);
}
```

#### 场景 3：IO 完成处理

```c
// 磁盘 I/O 中断处理程序
void disk_completion_irq(...)
{
    struct page *page = pending_pages[i];
    void *buf = kmap_atomic(page, KM_BIO_DST_IRQ);
    
    // 复制 DMA 缓冲到页面
    copy_from_dma_buffer(buf, dma_addr, 512);
    
    kunmap_atomic(buf, KM_BIO_DST_IRQ);
}
```

## 性能对比：kmap vs kmap_atomic

| 特性 | kmap | kmap_atomic |
|------|------|------------|
| **可睡眠** | 是（可能等待槽位） | 否（绝不睡眠） |
| **可用上下文** | 进程上下文 | 中断/原子 |
| **映射区域** | PKMAP (2-4MB) | FIXMAP (~32KB) |
| **槽位数量** | 512-1024 | 8-16（每 CPU） |
| **速度** | 较慢（可能发生争抢） | 超快（专属槽位） |
| **冲突可能** | 低（全局槽位池） | 非常低（CPU 私有） |
| **适用场景** | 长期映射、进程上下文 | 短期映射、中断上下文 |

## TLB 刷新策略：即时同步 vs 延迟批处理

这是一个关键的设计差异，体现了内核在不同场景下对性能与一致性的权衡：

- **kmap_atomic (即时同步)**:
    - **原因**: 处理的是非阻塞的即时请求，且槽位极少（每 CPU 仅几个）。由于请求来源与槽位是一对一对应的（通过 `km_type`），同一个槽位可能在极短时间内被重复利用。
    - **行为**: 为了保证原子上下文中的数据一致性，必须在映射时立即刷新 TLB (`__flush_tlb_one`)。这确保了中断处理程序等紧急路径能立即看到正确的物理页内容，防止访问到上一个使用者留下的旧数据。

- **kmap (延迟批处理)**:
    - **原因**: 拥有较大的槽位池（1024 个），且主要用于进程上下文。
    - **行为**: `kunmap` 并不立即刷新 TLB，只是减少引用计数。只有当所有槽位全部用完时，才会通过 `flush_all_zero_pkmaps()` 一次性清除所有计数为 1 的 PTE 并执行全局 TLB 刷新 (`flush_tlb_all`)。
    - **优势**: 这本质上是一种**缓存机制**。通过将 1024 次可能的刷新操作合并为一次，显著减少了昂贵的 TLB 刷新开销。

## 常见陷阱与注意事项

### ❌ 陷阱 1：嵌套使用同一 km_type

```c
// 危险！同一槽位被重用，覆盖第一个映射
void *addr1 = kmap_atomic(page1, KM_USER0);
void *addr2 = kmap_atomic(page2, KM_USER0);  // 槽位冲突！addr1 失效
use_data(addr1);  // BUG：page1 映射已被销毁
kunmap_atomic(addr2, KM_USER0);
kunmap_atomic(addr1, KM_USER0);
```

**解决**：使用不同的 `km_type`
```c
void *addr1 = kmap_atomic(page1, KM_USER0);
void *addr2 = kmap_atomic(page2, KM_USER1);  // 不同槽位
use_data(addr1);  // OK
kunmap_atomic(addr2, KM_USER1);
kunmap_atomic(addr1, KM_USER0);
```

### ❌ 陷阱 2：持有过长时间

```c
// 不好：映射了太长时间，阻塞其他使用者
void *addr = kmap_atomic(page, KM_USER0);
while (process_large_buffer(addr)) {  // 长时间持有映射
    // ...
}
kunmap_atomic(addr, KM_USER0);
```

**原因**：
- 虽然 kmap_atomic 本身很快，但长期占用槽位会导致其他需要该类型槽位的代码被迫等待（通过 IPI）
- 中断延迟增加

**解决**：尽快释放
```c
void *addr = kmap_atomic(page, KM_USER0);
// 仅做必要操作
memcpy(local_buf, addr, min_size);
kunmap_atomic(addr, KM_USER0);

// 在映射外处理数据
process_buffer(local_buf);
```

### ❌ 陷阱 3：混淆生命周期

```c
// 错误：使用了已释放的映射
void *addr = kmap_atomic(page, KM_USER0);
kunmap_atomic(addr, KM_USER0);
memcpy(dst, addr, 4096);  // addr 不再有效！
```

**保证规则**：
- 映射的虚拟地址仅在 `kmap_atomic` 和 `kunmap_atomic` 之间有效
- 释放后不能再访问

## 与 PKMAP 的关系

### 任务分工

| 映射方式 | 责任区域 | 访问频率 | 争抢情况 |
|---------|--------|--------|---------|
| **kmap** (PKMAP) | 数据页、元数据、缓存 | 较低频 | 全局竞争（spinlock） |
| **kmap_atomic** (FIXMAP) | 短期临时访问 | 非常高频 | 无竞争（CPU 私有） |

### 性能优化思路

```
高端内存访问模式分布：
  ├─ 大部分：极短暂访问（毫秒级）
  │   └─ 优化方案：kmap_atomic (FIXMAP)
  │
  └─ 少数：长期访问（毫秒以上）
      └─ 优化方案：kmap (PKMAP)
```

## 总结

**临时内核映射 (kmap_atomic)** 是高效访问高端内存的关键机制：

1. **设计理念**：每个 CPU 一小部分私有、固定虚拟地址（FIXMAP），避免全局竞争
2. **性能优势**：原子操作，不涉及睡眠或自旋锁，适合中断处理程序
3. **使用关键**：正确选择 `km_type` 以避免槽位冲突，及时释放以免阻塞其他路径
4. **与 kmap 互补**：二者结合覆盖所有高端内存访问场景

对比永久映射（kmap），临时映射的约束更严（不能睡眠、槽位少、生命周期短），但换来的是极致的性能与原子性保证。
