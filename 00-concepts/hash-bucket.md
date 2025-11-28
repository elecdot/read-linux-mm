---
related:
- "[Pagecache Strategy](../00-concepts/pagecache-strategy.md)"
tags:
- data-structure
- hash-table
- kernel-technique
sources:
- "[include/linux/mm.h](/linux/include/linux/mm.h)"
- "[mm/filemap.c](/linux/mm/filemap.c)"
---
/*! \page hash_bucket Intrusive Hash Bucket with Double Pointer

This page introduces:
Hash bucket with double-pointer（双指针哈希桶）是一种优雅的链表技巧，通过让每个节点存储"指向前驱 next 指针的指针"而非直接指向前驱，实现 O(1) 的链表节点移除，无需遍历。这是 Linux 内核中被广泛采用的设计模式。
*/


# Intrusive Hash Bucket with Double Pointer

## In a Word

Hash bucket with double-pointer（双指针哈希桶）是一种优雅的链表技巧，通过让每个节点存储"指向前驱 next 指针的指针"而非直接指向前驱，实现 O(1) 的链表节点移除，无需遍历。这是 Linux 内核中被广泛采用的设计模式。

## Why This Concept

传统链表的节点移除需要 O(n) 时间来寻找前驱节点。但许多内核数据结构需要快速移除某个已知位置的节点。双指针技巧巧妙地利用"指向指针的指针"，让移除操作变为 O(1)，同时保持内存效率。这种技巧在 pagecache、进程管理、网络栈等处广泛使用。

## Deep Dive

### 问题：传统链表的 O(n) 移除瓶颈

```c
// 传统 C 链表节点
struct node {
    int data;
    struct node *next;
};

// 移除某个节点需要找到前驱
void remove_node(struct node *target, struct node **head)
{
    struct node *current = *head;
    struct node *prev = NULL;
    
    // 遍历找前驱 → O(n)！
    while (current && current != target) {
        prev = current;
        current = current->next;
    }
    
    if (current) {
        if (prev)
            prev->next = current->next;
        else
            *head = current->next;
    }
}
```

**问题**：即使已知要移除的节点位置，也必须从链表头遍历找前驱。在高频操作中这很昂贵。

### 解决方案：双指针技巧

不存储指向前驱节点的指针，而是存储**指向前驱 next 指针的指针**：

```c
struct node {
    int data;
    struct node *next;
    struct node **pprev;    // ← 指向前驱的 next 指针
};
```

**关键洞察**：
- 链表头也是一个"指向 next 的指针" → `*head` 就是 `struct node **`
- 桶头 `page_hash_table[i]` 就是 `struct page **`
- 所以用同样的技巧可以处理链表头和中间节点！


### 可视化：双指针链表的内存布局

```
【传统链表】

     ┌──→ [ B ]──next──→ [ C ]──next──→ NULL
     │     ↑
  prev │     pprev (直接指向前驱，无法指向头)

[ A ]──next─┘

问题：移除 B 需要找到 A


【双指针链表】

     ┌──────────────────────────────────────┐
     │                                      ↓
桶头 [ &next ]  [ A ]──next──→[ B ]──next──→[ C ]──next──→ NULL
     ↑            ↑              ↑              ↑
     │            │              │              │
  pprev        pprev          pprev          pprev
     │            │              │              │
     └────────────┴──────────────┴──────────────┘

A.pprev = &(bucket_head)     // 桶头也是一个指针
B.pprev = &(A.next)         // 指向 A 的 next 字段
C.pprev = &(B.next)         // 指向 B 的 next 字段

关键：所有 pprev 都指向"某个 next 字段"，包括桶头！
```

### 移除节点：O(1) 的魔法

```c
// Linux 2.4 的实现
static inline void remove_page_from_hash_queue(struct page *page)
{
    struct page *next = page->next_hash;
    struct page **pprev = page->pprev_hash;
    
    if (next)
        next->pprev_hash = pprev;      // 后继的 pprev 指向前驱的 next
    *pprev = next;                     // 前驱的 next 指向后继
    page->pprev_hash = NULL;
}
```

**执行流程（移除 B）：**

```
初始：*bucket_head → A → B → C → NULL

步骤 1：next = B->next_hash = C
       pprev = B->pprev_hash = &A.next

步骤 2：C->pprev_hash = &A.next

步骤 3：*(&A.next) = C
        
结果：*bucket_head → A → C → NULL

一行代码 `*pprev = next` 搞定一切！
- 无需知道前驱节点是谁
- 无需知道自己在链表中间、头部还是尾部
- 同样代码处理所有情况
```

**为什么这么神奇**：

因为 `pprev` 本质上就是一个引用（指向某个包含指针的地址），赋值给它就相当于修改了那个指针。无论那个指针来自链表节点还是桶头，效果完全相同。

### 与传统链表的对比

| 操作 | 传统链表 | 双指针链表 |
|------|---------|----------|
| 查找节点 | O(n) | O(n) |
| 插入节点（已知位置） | O(1) | O(1) |
| 移除节点（已知位置） | O(n) | O(1) |
| 内存开销 | `next` | `next + pprev` |
| 处理链表头的特殊性 | 需要额外代码 | 统一代码 |

### Hash 桶的构造

```c
// 使用双指针的通用哈希桶插入
static void add_page_to_hash_queue(struct page *page, struct page **p)
{
    struct page *next = *p;
    
    *p = page;                          // page 成为新头
    page->next_hash = next;             // 指向原头
    page->pprev_hash = p;               // pprev 指向桶头指针
    if (next)
        next->pprev_hash = &page->next_hash;  // 原头的 pprev 指向 page 的 next
}
```

**插入的三个步骤**（假设插入到桶头）：

```
初始：*p → A → B → NULL

步骤 1：page.next_hash = A
步骤 2：*p = page
步骤 3：A.pprev_hash = &page.next_hash

结果：*p → page → A → B → NULL
        ↑         ↑
        └─────────┘ 通过 pprev_hash 互相连接
```

**妙处**：无需区分插入位置（桶头 vs 中间），都用同一套逻辑。

### 双指针的应用范围

这种技巧在 Linux 内核中广泛使用：

- **Pagecache**：`struct page` 的 `next_hash / pprev_hash`
- **进程表**：哈希表中的进程链表
- **网络栈**：套接字哈希表、连接表
- **内存管理**：各种内核数据结构的快速移除

核心原因：**一旦获得节点的指针，无需额外遍历即可 O(1) 移除**。

## Related Concepts

- "[Pagecache Strategy](../00-concepts/pagecache-strategy.md)" - how Linux uses hash buckets with address_space lists
- Intrusive data structures (nodes embed list pointers)

## Code Examples in Linux 2.4

```c
// mm/filemap.c - pagecache hash table operations
static void add_page_to_hash_queue(struct page * page, struct page **p)
{
    struct page *next = *p;
    *p = page;
    page->next_hash = next;
    page->pprev_hash = p;
    if (next)
        next->pprev_hash = &page->next_hash;
    atomic_inc(&page_cache_size);
}

static inline void remove_page_from_hash_queue(struct page * page)
{
    struct page *next = page->next_hash;
    struct page **pprev = page->pprev_hash;
    if (next)
        next->pprev_hash = pprev;
    *pprev = next;
    page->pprev_hash = NULL;
    atomic_dec(&page_cache_size);
}
```

## Key Insights

1. **指向指针的指针**：`pprev` 的妙处在于它统一处理了桶头和中间节点
2. **内存效率**：相比双向链表少用一个指针（没有 `prev`），相比单链表多用一个指针（为了 O(1) 移除）
3. **无需前驱遍历**：一旦持有节点指针，移除操作是 O(1)
4. **Intrusive 设计**：链表指针嵌入数据结构本身，无需额外分配链表节点