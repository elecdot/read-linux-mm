---
related:
- "[Hash Bucket](../00-concepts/hash-bucket.md)"
- "[Page Frame](../00-concepts/EXAMPLE.md)"
tags:
- memory-management
- pagecache
- linux-design
sources:
- "[include/linux/mm.h](/linux/include/linux/mm.h)"
- "[mm/filemap.c](/linux/mm/filemap.c)"
---
/*! \page pagecache_strategy Pagecache Management Strategy

This page introduces:
Linux 2.4 pagecache strategy（页缓存管理策略）通过结合两个独立的数据结构——address_space 列表（逻辑关联）和 hash 桶（查询优化）——在单个 page 对象上实现高效的文件页管理。这是 Linux 高性能文件 I/O 的关键设计。
*/


# Pagecache Management Strategy

## In a Word

Linux 2.4 pagecache strategy（页缓存管理策略）通过结合两个独立的数据结构——address_space 列表（逻辑关联）和 hash 桶（查询优化）——在单个 page 对象上实现高效的文件页管理。这是 Linux 高性能文件 I/O 的关键设计。

## Why This Concept

现代操作系统的性能瓶颈往往不在 CPU，而在存储 I/O。pagecache 是内核在内存中维护的文件数据副本，目的是避免重复的磁盘访问。但缓存的管理本身也要很高效：

1. **快速查询**：给定文件和偏移，快速判断页是否在缓存中
2. **快速遍历**：给定文件，快速遍历其所有页（例如刷写脏页）
3. **快速移除**：内存紧张时快速淘汰某个页

Linux 2.4 的设计通过两层结构巧妙地解决了这个"鱼与熊掌"的问题。

## Deep Dive

### 两层数据结构的设计思想

一个 page 对象在缓存中被两个独立的链表同时维护：

1. **Logical Association（逻辑关联）：address_space 列表**
   - 链表头：`address_space->clean_pages` / `address_space->dirty_pages`
   - 字段：`struct page->list`
   - 语义：这个页属于哪个文件（inode）或内存映射区域
   - 用途：顺序遍历、批量操作、生命周期管理

2. **Query Optimization（查询优化）：hash 桶链表**
   - 链表头：`page_hash_table[hash(address_space, index)]`
   - 字段：`struct page->next_hash / pprev_hash`（双指针技巧）
   - 语义：给定 (address_space, index) 快速查找页
   - 用途：O(1) 缓存查询、O(1) 页面移除

### 可视化：两个视图的同时存在

```
【文件系统状态】

/etc/passwd (address_space A)    /etc/hosts (address_space B)
       │                               │
       ├─→ page[0]                    ├─→ page[20]
       │    ├─→ page[1]               │    ├─→ page[21]
       │    └─→ page[2]               │    └─→ page[22]
       
   (按文件组织，维护逻辑关系)


【Hash 桶视图（全局 page_hash_table[]）】

Hash Bucket[0]: page[1] → page[22] → NULL
                (/etc/passwd[1] 和 /etc/hosts[22] 碰撞)

Hash Bucket[1]: page[0] → NULL
                (/etc/passwd[0])

Hash Bucket[2]: page[2] → page[21] → page[20] → NULL
                (混合不同文件的页)

(按 hash(address_space, index) 分布，优化查询)
```

**关键观察**：
- 同一个 page 对象同时在两个链表中
- 在 address_space 列表中的位置取决于文件内的偏移序列
- 在 hash 桶中的位置纯粹取决于哈希值，与逻辑关系无关
- 两个链表的遍历顺序完全独立

### 内存布局：Page 对象的双视角

```
【单个 page 结构体】

struct page {
    struct list_head list;              ← address_space 列表
    struct address_space *mapping;      ← 指向所属文件
    unsigned long index;                ← 文件内偏移
    
    struct page *next_hash;             ← hash 桶：前向指针
    struct page **pprev_hash;           ← hash 桶：反向指针（双指针）
    
    atomic_t count;                     ← 引用计数
    unsigned long flags;                ← 状态标志
    // ... 其他字段
};


【两个视图的完整示例】

Address-space 列表视图（遍历 /etc/passwd 的所有页）：
  /etc/passwd.mapping->list
          ↓
   page[0] ↔ page[1] ↔ page[2] ↔ ... ↔ page[100]
        ↑
  用于：批量读取、刷写脏页、释放文件


Hash 桶视图（查询 /etc/passwd 的第 100 页）：
  page_hash_table[hash(/etc/passwd, 100)]
          ↓
   ... → page[100] → ...
        ↑
  用于：O(1) 缓存查询
```

### 完整操作流程

#### 场景：应用程序连续读取文件 /etc/passwd

**步骤 1：缓存查询（使用 hash 桶）**

```
内核问：/etc/passwd 的第 100 页在缓存里吗？

哈希计算：
  bucket_index = hash(passwd_address_space, 100) % hash_table_size
  假设结果是 bucket[5]

链表遍历：
  current = page_hash_table[5]
  while (current) {
    if (current->mapping == passwd_aspace && current->index == 100)
      return current;  // 缓存命中！O(1)～O(链表长度)，平均 O(1)
    current = current->next_hash;
  }
  // 缓存未命中，需要从磁盘读取
```

**步骤 2：缓存未命中，从磁盘读取**

```
分配新页框 page_new，从磁盘读取数据到该页框

【加入 address_space 列表】
  list_add(&page_new->list, &passwd_aspace->clean_pages);
  page_new->mapping = passwd_aspace;
  page_new->index = 100;
  passwd_aspace->nrpages++;
  
  结果：/etc/passwd 的页列表包含新页
       clean_pages ↔ ... ↔ page[100] ↔ ...


【加入 hash 桶】
  hash_ptr = &page_hash_table[5];
  add_page_to_hash_queue(page_new, hash_ptr);
  
  结果：page_hash_table[5] → page_new → (原先的页)
```

**步骤 3：缓存命中（后续访问相同页）**

```
用户应用再次读 /etc/passwd 的第 100 页

哈希查询过程相同：
  bucket_index = hash(passwd_aspace, 100) = 5
  
立即找到 page_new！
  直接使用内存数据，无需磁盘 I/O
  性能飙升（内存访问 < 纳秒 vs 磁盘访问 ~ 毫秒）
```

**步骤 4：文件操作例：刷写所有脏页**

```
需求：将 /etc/passwd 的所有脏页写回磁盘

使用 address_space 列表（不是 hash 桶）：
  for_each_page_in_mapping(passwd_aspace, page) {  // 顺序遍历
    if (PageDirty(page))
      write_page_to_disk(page);
  }

这里 hash 桶帮不上忙，因为我们不知道各个脏页的"索引"
address_space 列表提供了快速的"所有页"视图
```

**步骤 5：页面淘汰（内存紧张时的 LRU 清理）**

```
内核决定移除 page_new 以释放内存

【从 address_space 列表移除】
  list_del(&page_new->list);
  page_new->mapping = NULL;
  passwd_aspace->nrpages--;


【从 hash 桶移除（利用双指针 O(1) 移除）】
  remove_page_from_hash_queue(page_new);
  
  // 内部实现：
  // *pprev_hash = next_hash;  一行代码，O(1)！
```

### 为什么需要两个结构？

| 操作 | Hash 桶 | Address_space 列表 |
|------|---------|----------|
| "文件 X 的页 Y 在缓存吗？" | ✓ O(1) | ✗ O(n_file) |
| "刷写文件 X 的所有脏页" | ✗ 无序 | ✓ O(n_file) |
| "移除某个已知页" | ✓ O(1) | ✗ O(n_file) |
| "遍历某文件所有页" | ✗ 混合多文件 | ✓ O(n_file) |
| "LRU 淘汰" | ✓ 快速移除 | ✓ 维持顺序 |

**结论**：Hash 桶优化查询，address_space 列表维持逻辑关系。单靠一个结构无法高效完成所有操作。

### 性能对比：无 Hash 桶 vs 有 Hash 桶

```
【场景】系统有 100 万个页框，分属 10000 个文件，
       平均每文件 100 页

任务：查询文件 X 的第 50 页是否在缓存


方案 A：仅用 address_space 列表（无 hash 桶）

  遍历文件 X 的所有页框：
    平均需要扫描 50 个页框 → O(n_file)
    时间：~微秒级别（还不是毫秒，但已经很慢）
    
  系统级性能：
    每秒文件访问次数可能下降 10 倍
    应用响应时间可能增加 100 倍（磁盘 I/O 被迫增加）


方案 B：使用 hash 桶（Linux 2.4）

  哈希查询：
    计算 hash → 找桶 → 遍历桶中链表
    平均需要检查 1-2 个页框 → O(1)
    时间：纳秒级别！
    
  系统级性能：
    缓存命中率显著提高
    磁盘 I/O 显著降低
    应用响应时间稳定
```

### 设计权衡

```
选择：使用两个独立结构 vs 使用单一数据结构

权衡点：

1. 内存开销
   - 每个 page 多用：next_hash + pprev_hash（2 个指针）
   - 但避免了复杂的双向链表（也是 2 个指针）
   - 所以内存开销 ≈ 零

2. 代码复杂度
   - 需要维护两个链表 → 加了复杂性
   - 但每个链表本身很简单 → 清晰易懂
   - 总体可控

3. 性能收益
   - Hash 桶带来的 O(1) 查询 vs O(n) 查询：收益巨大
   - 即使多用 2 个指针的成本也值得
```

## Related Concepts

- "[Hash Bucket](../00-concepts/hash-bucket.md)" - the double-pointer technique used by pagecache
- Intrusive data structures and in-place operations
- LRU cache eviction policies

## Implementation Details in Linux 2.4

关键数据结构：
- `page_hash_table[]` - 全局 hash 桶数组
- `struct page` - 嵌入 `next_hash` 和 `pprev_hash`
- `struct address_space` - 含 `clean_pages` 和 `dirty_pages` 链表头

关键操作：
- `add_page_to_hash_queue()` / `remove_page_from_hash_queue()`
- `add_page_to_inode_queue()` / `remove_page_from_inode_queue()`