---
related:
  - "[Zone-based Memory Management](../00-concepts/zone-based-memory-management.md)"
  - "[Page Flags & State Management](../00-concepts/page-flags.md)"
  - "[Core Memory Management Variables](../00-concepts/mm-core-variables.md)"
tags:
  - memory-management
  - page-allocation
  - memory-pressure
  - zone-watermark
sources:
  - "[include/linux/mmzone.h](../../linux/include/linux/mmzone.h)"
  - "[mm/page_alloc.c](../../linux/mm/page_alloc.c)"
  - "[Linux 2.4 内存管理 - 保留页池]"
---

/*! \page reserved_page_pool The Pool of Reserved Page Frames

内核在每个内存区域（zone）维护了一个保留页池（Reserved Page Pool）以应对内存压力。通过 `pages_min`、`pages_low`、`pages_high` 三个水位标记，内核确保即使在高内存占用的情况下，仍能为关键操作（如页面回收、I/O 缓冲等）分配足够的页框。

*/

# The Pool of Reserved Page Frames

## In a Word

保留页池（Reserved Page Pool）是每个内存区域（zone）维护的一个页框缓冲机制，通过三个水位标记（`pages_min`、`pages_low`、`pages_high`）来控制内存分配的积极性，确保即使系统内存紧张时仍能保证关键子系统的内存需求，防止内核陷入无内存可用的死局。防止一些不允许被阻塞的内存申请 (即 `GFP_ATOMIC` 请求).

## Why This Concept

在 Linux 内存管理中，简单地按需分配页框会导致严重问题：

- **死锁风险**：如果内存耗尽，页面回收程序（kswapd）可能无法获得内存来执行回收操作本身，形成死锁。
- **I/O 阻塞**：磁盘 I/O 完成中断处理可能需要分配内存（例如缓冲区），无法分配会导致 I/O 挂起。
- **级联失败**：一个关键操作的失败可能导致整个系统响应迟缓甚至崩溃。

通过维护保留页池，内核确保**即使在最恶劣的内存压力下，也能为最关键的内存分配请求保留足够的页框**。

## Deep Dive

### 三级水位机制

在 [mmzone.h](../../linux/include/linux/mmzone.h) 中定义的 `zone_struct` 包含三个关键字段：

```c
typedef struct zone_struct {
    // ...
    unsigned long pages_min;    //! 最小保留页数 - 硬底线
    unsigned long pages_low;    //! 低水位 - 触发 kswapd 唤醒
    unsigned long pages_high;   //! 高水位 - 停止 kswapd
    unsigned long free_pages;   //! 当前空闲页数
    // ...
} zone_t;
```

### 三个水位的语义

#### 1. `pages_min`（最小保留 - 硬底线）

**含义**：任何时刻该 zone 必须保持的最小空闲页数。

- **保护对象**：最关键的内存分配（例如页面回收所需的临时缓冲）
- **分配策略**：
  - 普通分配：`free_pages > pages_high` 时才允许
  - 原子分配（中断上下文）：`free_pages > pages_min` 时才允许
  - DMA 分配：有单独的 DMA 保留
- **意义**：即使系统内存紧张，也要**确保存在可用的页框供紧急分配**

#### 2. `pages_low`（低水位 - 唤醒阈值）

**含义**：空闲页数低于此值时，内核唤醒 kswapd 进行页面回收。

- **触发条件**：`free_pages <= pages_low`
- **作用**：主动回收内存，防止系统进入 `free_pages <= pages_min` 的危险状态
- **策略**：kswapd 被唤醒后尽量回收页面，直到 `free_pages >= pages_high`

#### 3. `pages_high`（高水位 - 停止阈值）

**含义**：空闲页数回升到此值后，停止页面回收。

- **触发条件**：`free_pages >= pages_high`
- **作用**：避免过度回收，给应用进程留出充分的内存
- **效果**：减少上下文切换和缓存压力

### 水位关系

```
pages_min  <  pages_low  <  pages_high

正常状态：
free_pages >= pages_high
    ↓
    应用进程正常分配，kswapd 休眠

内存压力：
pages_low < free_pages < pages_high
    ↓
    kswapd 被唤醒，开始回收

危险状态：
pages_min < free_pages <= pages_low
    ↓
    kswapd 加紧回收，避免进一步下降

紧急状态：
free_pages <= pages_min
    ↓
    原子分配被拒绝，可能导致 OOM
```

### 初始化与动态调整

#### 初始化时机（Linux 2.4.18 实现）

在系统启动期间调用 `free_area_init_core()` 时，根据 zone 的大小和 `zone_balance_ratio` 等参数计算初始水位。

**实现方式**（基于 `mm/page_alloc.c`）：

```c
// zone_balance_ratio 数组定义了不同类型 zone 的平衡比例
static int zone_balance_ratio[MAX_NR_ZONES] = { 32, 32, 8 };
static int zone_balance_min[MAX_NR_ZONES] = { 10, 10, 10 };
static int zone_balance_max[MAX_NR_ZONES] = { 255, 255, 255 };

// 初始化时的计算方式
mask = (realsize / zone_balance_ratio[j]);
if (mask < zone_balance_min[j])
    mask = zone_balance_min[j];
else if (mask > zone_balance_max[j])
    mask = zone_balance_max[j];

zone->pages_min = mask;              // 设置最小值
zone->pages_low = mask * 2;          // 低水位为最小值的 2 倍
zone->pages_high = mask * 3;         // 高水位为最小值的 3 倍
```

这意味着水位值依赖于 zone 的实际大小（`realsize`）和比例参数，而非简单的固定百分比。

#### 动态调整

- **目的**：根据当前系统状态调整分配的激进程度
- **触发**：在 zone 平衡期间，考虑内存压力和 zone 大小
- **函数**：`kswapd` 等内存管理后台程序可能进行动态调整

### 分配路径中的检查

#### 普通分配（进程上下文）

```c
if (free_pages > pages_high) {
    /* 正常分配，不触发回收 */
    allocate_page();
} else if (free_pages > pages_low) {
    /* 唤醒 kswapd，但仍允许分配 */
    wakeup_kswapd();
    allocate_page();
} else if (free_pages > pages_min) {
    /* 唤醒 kswapd，直接回收 */
    direct_reclaim();
    allocate_page();
} else {
    /* 无法分配 - pages_min 已到底线 */
    return NULL;  // 或阻塞等待
}
```

#### 原子分配（中断上下文）

```c
if (free_pages > pages_min) {
    /* 允许分配，保护 pages_min 以下的页框 */
    allocate_page();
} else {
    /* 拒绝分配 - 保留最后的紧急页框 */
    return NULL;
}
```

### 实际意义与影响

#### 防止内存耗尽死锁

- 没有水位机制：内存完全耗尽 → kswapd 无法分配内存来回收 → 系统无法恢复
- 有水位机制：始终预留 `pages_min` 个页框 → kswapd 可以使用这些页框来执行回收 → 系统可恢复

#### 平衡系统响应性和应用性能

- `pages_high` 过高：应用程序频繁受回收打扰，性能差
- `pages_high` 过低：内存可能突然耗尽，导致 OOM
- 合理设置：使系统在性能与安全之间找到平衡

#### 不同 zone 的差异化保护

- **ZONE_DMA**：DMA 操作对内存位置有严格要求，因此保留比例可能更高
- **ZONE_NORMAL**：普通内核内存，保留比例中等
- **ZONE_HIGHMEM**：用户页面，保留比例可能较低

### 与其他概念的关系

#### 与 kswapd 的关系

- kswapd 不断监控各 zone 的 `free_pages` 与 `pages_low` 关系
- 当 `free_pages <= pages_low` 时被唤醒
- 持续回收直到 `free_pages >= pages_high`

#### 与页面标志的关系

- `PG_reserved` 标志标记的是**单个页面**的属性（永不交换）
- `pages_min` 机制保护的是**zone 级别**的最小可用页数
- 两者都用于保护关键资源，但作用层级不同

#### 与直接回收的关系

- 当 `pages_min < free_pages <= pages_low` 时，触发直接回收
- 分配线程在获得足够页框后才继续执行
- 这避免了异步回收不及时导致的分配失败

## Related Concepts

- [Zone-based Memory Management](zone-based-memory-management.md)：区域划分和分配策略
- [Page Flags & State Management](page-flags.md)：页面标志与 `PG_reserved` 区别
- [Core Memory Management Variables](mm-core-variables.md)：全局内存统计变量

## Historical Notes：2.4.18 版本特性

### 为什么没有 `min_free_kbytes`？

Linux 2.4.18 版本中，**不存在 `min_free_kbytes` sysctl 参数**。这个接口是在更后期的内核版本（约 2.5/2.6 之后）才引入的。

**2.4.18 的设计**：
- 水位参数在**编译时**通过 `zone_balance_ratio` 等数组硬编码
- 初始化阶段根据实际 zone 大小动态计算
- 不支持运行时通过 `/proc` 或 `sysctl` 调整

**后期内核的改进**（2.5+ 版本）：
- 引入 `/proc/sys/vm/min_free_kbytes` 参数
- 允许系统管理员在运行时动态调整保留页池大小
- 提供更细粒度的内存压力控制

### 版本兼容性提示

本文档描述的所有行为都**严格基于 Linux 2.4.18**。在阅读更新版本的内核代码时，会发现：
- 水位计算机制已改变
- 存在 `min_free_kbytes` 等运行时可调参数
- 内存回收策略可能有新的优化

若在学习更新版本内核时感到困惑，请查阅对应版本的文档。
