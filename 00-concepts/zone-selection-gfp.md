---
related:
- "[The Zoned Page Frame Allocator](zoned-page-frame-allocator.md)"
- "[Zone-Based Memory Management](zone-based-memory-management.md)"
- "[Buddy System](buddy-system.md)"
- "[MM Core Variables](mm-core-variables.md)"
tags:
- memory-management
- zone-allocator
- allocation-flags
- physical-memory
sources:
- "[include/linux/mm.h](/linux/include/linux/mm.h)"
- "[include/linux/mmzone.h](/linux/include/linux/mmzone.h)"
- "[mm/page_alloc.c](/linux/mm/page_alloc.c)"
---

/*! \page zone-selection-gfp Zone Selection Strategy: Zonelist + GFP Flags

This page introduces:
内核在分配页框时，通过 GFP（Get Free Pages）标志位来指定偏好的内存区域。每个可能的 GFP 组合都对应一个预先构建好的 zonelist，其中包含该请求可以使用的区域及其优先级顺序。这种机制确保了内存分配既能满足设备的硬件约束，又能有效利用内存。
*/

# Zone Selection Strategy: Zonelist + GFP Flags

## In a Word

内核通过在分配掩码（allocation mask）中编码 **GFP 标志**（低 4 比特），来指定内存分配的区域偏好。每个不同的 GFP 标志组合对应一个预构建的 **zonelist**（区域列表）。分配器根据该 zonelist 按优先级顺序遍历区域，实现了"从最受限区域开始，逐级回退"的策略。

## Why This Concept

当内核需要分配内存时，不是简单地"从任意区域拿"，而是需要明确表达自己的需求：
- DMA 驱动说："我需要来自 ZONE_DMA 的内存"
- 普通进程说："给我 ZONE_NORMAL 或 ZONE_HIGHMEM 的内存都可以"
- 内核代码说："我只能用 ZONE_NORMAL"

通过 GFP 标志和预构建的 zonelist，内核实现了 **灵活、高效的内存分配决策**，同时保留了稀缺的 DMA 内存供真正需要的设备使用。

## Deep Dive

### GFP 标志位定义

在 `include/linux/mm.h` 中定义了区域控制位：

```c
/* Zone modifiers in GFP_ZONEMASK (see linux/mmzone.h - low four bits) */
#define __GFP_DMA      0x01     /* 标记请求来自 ZONE_DMA */
#define __GFP_HIGHMEM  0x02     /* 标记请求来自 ZONE_HIGHMEM */

#define GFP_ZONEMASK   0x0f     /* 低 4 比特用于区域选择（保留扩展空间） */
```

这两个标志位被称为"区域修饰符"（Zone modifiers），与其他标志位（如 `__GFP_WAIT`、`__GFP_IO`）正交组合：

```c
/* 常见的分配策略组合 */
#define GFP_ATOMIC     (__GFP_HIGH)                         /* 无等待，不含DMA/HIGHMEM标记 → ZONE_NORMAL */
#define GFP_KERNEL     (__GFP_HIGH | __GFP_WAIT | ...)      /* 标准内核分配 → ZONE_NORMAL */
#define GFP_HIGHMEM    (... | __GFP_HIGHMEM)                /* 高端内存优先 → ZONE_HIGHMEM */
#define GFP_DMA        __GFP_DMA                            /* DMA 专用 → ZONE_DMA */
#define GFP_NOHIGHIO   (__GFP_HIGH | __GFP_WAIT | ...)      /* 禁用高端内存I/O */
```

### Zonelist 数组与预构建机制

每个 NUMA 节点的 `pg_data_t` 中存在一个 zonelist 数组：

```c
typedef struct pglist_data {
    ...
    zonelist_t node_zonelists[GFP_ZONEMASK+1];  /* 16 个 zonelist */
    ...
} pg_data_t;
```

内容包括：
```c
// 伪代码示意
contig_page_data.node_zonelists = [
    [GFP 组合0] → [zone列表0],
    [GFP 组合1] → [zone列表1],
    [GFP 组合2] → [zone列表2],
    ...
]
```

在系统启动时，`build_zonelists()` 为所有 16 个可能的 GFP 组合预构建相应的 zonelist：

```c
static inline void build_zonelists(pg_data_t *pgdat)
{
    int i, j, k;

    for (i = 0; i <= GFP_ZONEMASK; i++) {
        zonelist_t *zonelist;
        zone_t *zone;

        zonelist = pgdat->node_zonelists + i;
        memset(zonelist, 0, sizeof(*zonelist));

        j = 0;
        k = ZONE_NORMAL;  /* 默认从 ZONE_NORMAL 开始 */
        if (i & __GFP_HIGHMEM)
            k = ZONE_HIGHMEM;
        if (i & __GFP_DMA)
            k = ZONE_DMA;

        /* switch 语句实现 fallthrough，构建递级 zonelist */
        switch (k) {
            default:
                BUG();
            /*
             * fallthrough:  关键！通过 case 标签的连续执行实现回退链
             */
            case ZONE_HIGHMEM:
                zone = pgdat->node_zones + ZONE_HIGHMEM;
                if (zone->size) {
                    zonelist->zones[j++] = zone;
                }
            case ZONE_NORMAL:
                zone = pgdat->node_zones + ZONE_NORMAL;
                if (zone->size)
                    zonelist->zones[j++] = zone;
            case ZONE_DMA:
                zone = pgdat->node_zones + ZONE_DMA;
                if (zone->size)
                    zonelist->zones[j++] = zone;
        }
        zonelist->zones[j++] = NULL;  /* 数组以 NULL 结尾 */
    }
}
```

**关键观察**：通过 C 语言的 switch fallthrough 特性，同一个 zonelist 中可以包含多个 zone。例如：
- `GFP_KERNEL & GFP_ZONEMASK = 0x00` → 索引 0 → zonelist 包含 [ZONE_NORMAL, ZONE_DMA, NULL]
- `GFP_HIGHMEM & GFP_ZONEMASK = 0x02` → 索引 2 → zonelist 包含 [ZONE_HIGHMEM, ZONE_NORMAL, ZONE_DMA, NULL]
- `GFP_DMA & GFP_ZONEMASK = 0x01` → 索引 1 → zonelist 包含 [ZONE_DMA, NULL]

### 分配时的 Zonelist 查询

分配函数通过 GFP 掩码的低 4 比特快速定位 zonelist：

```c
struct page *_alloc_pages(unsigned int gfp_mask, unsigned int order)
{
    return __alloc_pages(gfp_mask, order,
        contig_page_data.node_zonelists + (gfp_mask & GFP_ZONEMASK));
    /*                                      ^^^^^^^^^^^^^^^^^^^^^^^^ */
    /*                                      直接索引到预构建的 zonelist */
}
```

关于 `__alloc_pages` 的详细分配逻辑（水位检查、唤醒 kswapd 等），请参阅 **[The Zoned Page Frame Allocator](zoned-page-frame-allocator.md)**。

然后分配器逐个遍历该 zonelist 中的 zone：

```c
struct page * __alloc_pages(unsigned int gfp_mask, unsigned int order, zonelist_t *zonelist)
{
    ...
    zone = zonelist->zones;
    for (;;) {
        zone_t *z = *(zone++);
        if (!z)
            break;
        
        min += z->pages_low;
        if (z->free_pages > min) {
            page = rmqueue(z, order);
            if (page)
                return page;  /* 成功分配，立即返回 */
        }
    }
    ...
}
```

### 分配决策表

| GFP 标志 | 二进制 | zonelist 索引 | zone 优先级顺序 | 适用场景 |
|----------|-------|---------------|-----------------|---------|
| `GFP_KERNEL` | 0000 | 0 | NORMAL → DMA | 一般内核分配 |
| `GFP_DMA` | 0001 | 1 | DMA | ISA 设备 DMA 缓冲 |
| `GFP_HIGHMEM` | 0010 | 2 | HIGHMEM → NORMAL → DMA | 页面缓存、用户页 |
| `GFP_ATOMIC` | 0000 | 0 | NORMAL → DMA | 中断/自旋锁下分配 |
| `GFP_NOHIGHIO` | 0000 | 0 | NORMAL → DMA | 禁用高端内存IO |
| 预留 | 0100-1111 | 4-15 | 可扩展 | 未来扩展空间 |

### 关键设计原则

1. **预构建与快速查询**：zonelist 在启动时一次性构建，分配时只需 O(1) 索引，避免了运行时计算。

2. **优雅的回退**（Graceful Degradation）：通过 switch fallthrough 自动构建优先级链。`GFP_HIGHMEM` 请求首先尝试 ZONE_HIGHMEM，失败时自动回退到 ZONE_NORMAL 和 ZONE_DMA。

3. **DMA 内存保护**：非 DMA 请求（如 `GFP_KERNEL`）只能在 ZONE_DMA 成为最后手段时才能使用，保证了稀缺的 DMA 内存不被浪费。

4. **二进制标志的通用性**：GFP 标志用两个比特表示三个 zone，剩余比特空间可供未来扩展（当前保留为 0）。

## 实际案例

### 案例1：网络驱动请求 DMA 内存

```c
/* 在网卡驱动中 */
struct page *page = alloc_pages(GFP_ATOMIC | GFP_DMA, 0);
/* 
 * 此时：
 * - gfp_mask & GFP_ZONEMASK = 0x01
 * - 查询 zonelists[1]
 * - zonelist 仅包含 [ZONE_DMA]
 * - 必须从 ZONE_DMA 分配，无回退选项
 */
```

### 案例2：内核通常的分配

```c
/* 在内核代码中 */
struct page *page = alloc_pages(GFP_KERNEL, 2);  /* 分配 4 页 */
/* 
 * 此时：
 * - gfp_mask & GFP_ZONEMASK = 0x00
 * - 查询 zonelists[0]
 * - zonelist 包含 [ZONE_NORMAL, ZONE_DMA]
 * - 优先从 ZONE_NORMAL，若不足再尝试 ZONE_DMA
 */
```

### 案例3：页面缓存分配

```c
/* 在文件系统代码中 */
struct page *page = alloc_pages(GFP_HIGHUSER, 0);
/* 
 * 此时：
 * - gfp_mask & GFP_ZONEMASK = 0x02
 * - 查询 zonelists[2]
 * - zonelist 包含 [ZONE_HIGHMEM, ZONE_NORMAL, ZONE_DMA]
 * - 优先高端内存（用户进程空间），可回退到普通和 DMA 区域
 */
```

代码表达（case优先级）：
```c
case ZONE_HIGHMEM:
    zone = pgdat->node_zones + ZONE_HIGHMEM;
    if (zone->size) {
        zonelist->zones[j++] = zone;
    }
case ZONE_NORMAL:
    zone = pgdat->node_zones + ZONE_NORMAL;
    if (zone->size)
        zonelist->zones[j++] = zone;
case ZONE_DMA:
    zone = pgdat->node_zones + ZONE_DMA;
    if (zone->size)
        zonelist->zones[j++] = zone;
```

## 与伙伴系统的关系

每个 zone 内部都有一个独立的伙伴系统（Buddy System），管理该 zone 内的空闲页框：

```
zonelist 选择 zone
    ↓
zone 的伙伴系统分配具体页框
    ↓
rmqueue(zone, order) 操作 zone->free_area[order]
```

zonelist 决定"从哪个 zone"，伙伴系统决定"如何从该 zone 中分配"。

## 与 NUMA 的关系

在 NUMA 系统中，每个节点（node）都有自己的 `node_zonelists` 数组。同一 GFP 值在不同节点中会产生不同的 zonelist（因为各节点的 zone 大小不同）。
