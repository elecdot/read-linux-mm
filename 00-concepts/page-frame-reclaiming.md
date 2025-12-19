---
related:
- "[Buddy System](./buddy-system.md)"
- "[The Zone Allocator](./the-zone-allocator.md)"
- "[Pagecache Strategy](./pagecache-strategy.md)"
- "[Swappable Page](./swappable-page.md)"
tags:
- memory-management
- page-reclaim
- kswapd
- lru
sources:
- "[linux/mm/vmscan.c](/linux/mm/vmscan.c)"
- "[linux/include/linux/mm.h](/linux/include/linux/mm.h)"
- "[linux/include/linux/swap.h](/linux/include/linux/swap.h)"
---

# Page Frame Reclaiming (页框回收) 终极深度解析

## In a Word

Page Frame Reclaiming（页框回收）是 Linux 内核在内存紧张时，通过释放不再使用的页面或将不常用的页面交换到磁盘，从而腾出物理页框给更紧急任务的机制。

---

## 0. 整体架构：回收的“三道防线”

在深入细节前，必须理解内核回收内存的三个层次，它们构成了系统的生存保障体系：

1.  **后台异步回收 (kswapd)**：当内存开始紧张（触及警戒线）时，`kswapd` 在后台悄悄工作，尽量不影响进程运行。
2.  **同步直接回收 (Direct Reclaim)**：当内存极度匮乏（触及崩溃线）时，申请内存的进程会被“逮捕”，被迫停下手中的活儿，亲自去释放页面。
3.  **终极杀招 (OOM Killer)**：如果前两招都失灵，内核只能通过杀死某个进程来释放内存。

---

## 1. 触发信号：水位线 (Watermarks) 的艺术

内核如何感知“紧张”？答案在 `zone_t` 结构中的三条水位线：

-   **`pages_high` (安全线)**：内存充足。`kswapd` 努力的目标。一旦空闲页达到此线，`kswapd` 就可以安心睡觉。
-   **`pages_low` (警戒线)**：内存开始吃紧。当空闲页低于此线时，伙伴系统会唤醒 `kswapd`。
-   **`pages_min` (崩溃线)**：内存见底。此时触发**同步直接回收**，系统响应速度会明显下降。

---

## 2. 情报系统：LRU 链表与页面老化机制

既然要回收，该收谁的？内核通过 **LRU (Least Recently Used)** 链表来追踪页面的“活跃度”。

### A. 双链表结构
-   **`active_list` (活跃链表)**：存放热点数据。
-   **`inactive_list` (不活跃链表)**：存放回收候选者。

### B. 老化逻辑：`refill_inactive` (给页面第二次机会)
这是页面从“活跃”转为“不活跃”的过程：
1.  **逆序扫描**：从 `active_list` 尾部（最老的页）开始。
2.  **引用检查**：检查 `PG_referenced` 位。
    -   **如果置位**：说明最近被访问过。清除该位，移回 `active_list` 头部（续命）。
    -   **如果未置位**：移入 `inactive_list`。
3.  **关键细节**：移入不活跃链表时，会**重新设置** `PG_referenced`。这为页面在被真正回收前留出了最后一次“自救”机会。

---

## 3. 执行机构：核心回收引擎的协作

当 `kswapd` 或直接回收路径开始工作时，它们会调用以下核心引擎：

### A. `shrink_cache`：回收的“绞刑架”
它直接作用于 `inactive_list`，逻辑极其严密：
1.  **调度避让**：每扫描一些页面就检查 `need_resched`，保证系统不卡顿。
2.  **脏页处理**：如果是脏页，启动异步写回 (`writepage`)，标记为 `PG_launder`，等下一轮变干净后再收。
3.  **缓冲区剥离**：调用 `try_to_release_page` 释放 `buffer_head`。**注意**：只有没有缓冲区的页面才能被物理释放。
4.  **映射页瓶颈**：如果发现扫描了很多页都是被进程映射的（Mapped），说明单纯清理缓存已不够，必须动用重武器。

### B. `swap_out`：解除映射的“重武器”
当缓存回收遇到瓶颈，内核开始强制解除进程对页面的映射：
1.  **公平选择 (Finger)**：通过全局变量 `swap_mm` 轮流扫描进程，避免总是针对同一个进程。
2.  **解除映射**：清除 PTE，刷新 TLB。
3.  **Swap Cache 桥梁**：
    -   如果是匿名页，分配磁盘交换空间。
    -   将页面放入 **Swap Cache**。此时页面变为了“无映射但有备份”的状态，可以被 `shrink_cache` 顺利回收。

---

## 4. 调度中心：kswapd 与优先级控制

### A. kswapd 的自我修养
-   **死锁规避 (`PF_MEMALLOC`)**：`kswapd` 标记有此标志，允许它在回收过程中无视水位限制申请微量内存。这保证了“救火员不会因为没水喝而渴死”。
-   **平衡之道**：它会不断调用回收引擎，直到所有 Zone 的空闲页都回升到 `pages_high`。

### B. 优先级 (Priority) 的阶梯
内核回收不是一成不变的，而是根据压力动态调整：
-   **初始状态**：`priority = 6`。只扫描不活跃链表的 1/6。
-   **压力升级**：如果回收不到 32 页，`priority` 减小，扫描深度指数级增加。
-   **最终决裂**：当 `priority` 降至 0 仍无果，触发 **OOM Killer**。

---

## 5. 总结：一个页面的“轮回”之路

为了讲清楚连贯性，我们可以跟踪一个页面的生命周期：

1.  **出生**：被进程申请，进入 `active_list`。
2.  **中年**：随着时间推移移向链表尾部。`refill_inactive` 发现它没被访问，将其降级到 `inactive_list`。
3.  **危机**：内存紧张，`shrink_cache` 扫描到它。
    -   如果是**文件页**：直接被释放，页框回到伙伴系统。
    -   如果是**匿名页**：`swap_out` 介入，将其内容写到磁盘，页面进入 Swap Cache。
4.  **终结**：页面变干净且无映射，`shrink_cache` 将其彻底从缓存中剥离。
5.  **重生**：物理页框回到伙伴系统，等待下一次分配。

### 3. 交换机制 (Swap Subsystem)
负责管理交换分区（Swap Partition/File），处理匿名页的换出（Swap Out）和换入（Swap In）。

### 4. 缓存收缩 (Cache Shrinking)
除了通用的页面回收，内核还注册了许多专门的“收缩器”（Shrinkers），用于回收特定类型的内核对象（如 VFS 缓存）。

## Core Logic: The "Watermark" Trigger

回收通常由 Zone 的水位（Watermarks）触发：
-   `pages_high`：安全水位。`kswapd` 停止工作的目标。
-   `pages_low`：警戒水位。开始唤醒 `kswapd` 进行后台回收。
-   `pages_min`：绝望水位。进入紧急状态，触发同步回收（Direct Reclaim）。

---
## __TODO__
- [ ] 深入分析 `active_list` 与 `inactive_dirty_list` 的转换逻辑。
- [ ] 阅读 `mm/vmscan.c` 中的 `shrink_cache` 函数。
- [ ] 探究 `kswapd` 的主循环逻辑。
