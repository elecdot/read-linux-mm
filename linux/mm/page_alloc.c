/*
 *  linux/mm/page_alloc.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *  Swap reorganised 29.12.95, Stephen Tweedie
 *  Support of BIGMEM added by Gerhard Wichert, Siemens AG, July 1999
 *  Reshaped it to be a zoned allocator, Ingo Molnar, Red Hat, 1999
 *  Discontiguous memory support, Kanoj Sarcar, SGI, Nov 1999
 *  Zone balancing, Kanoj Sarcar, SGI, Jan 2000
# */

/**
 * @file linux/mm/page_alloc.c
 * @brief Zoned buddy page allocator: allocation, freeing, and zone accounting.
 *
 * @ref buddy-system
 * @ref zone-based-memory-management
 *
 * @code This is just a example, you may want to change it @endcode
 * This translation of the kernel's page allocator implements the zoned
 * buddy allocator used to manage physical pages. It provides routines to
 * allocate and free page blocks of various orders, coalesce freed blocks
 * with their buddies, maintain per-zone free lists and accounting, and
 * handle slow-path balancing (including interaction with kswapd).
 *
 * The file contains the fast-path buddy operations (rmqueue/expand), the
 * freeing path (__free_pages_ok  and helpers), and higher-level allocation
 * helpers such as __alloc_pages, __get_free_pages and get_zeroed_page.
 *
 * Notes:
 * - Many functions assume kernel invariants (spinlocks, current->flags,
 *   page reference counts). They can call BUG() on severe inconsistencies.
 * - This source is intended for reading and learning; it mirrors the
 *   behavior of historical Linux mm code and uses kernel-specific helpers.
 */

#include <linux/config.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/swapctl.h>
#include <linux/interrupt.h>
#include <linux/pagemap.h>
#include <linux/bootmem.h>
#include <linux/slab.h>
#include <linux/compiler.h>

int nr_swap_pages;
int nr_active_pages;
int nr_inactive_pages;
struct list_head inactive_list;
struct list_head active_list;
pg_data_t *pgdat_list;

static char *zone_names[MAX_NR_ZONES] = { "DMA", "Normal", "HighMem" };
static int zone_balance_ratio[MAX_NR_ZONES] __initdata = { 128, 128, 128, };
static int zone_balance_min[MAX_NR_ZONES] __initdata = { 20 , 20, 20, };
static int zone_balance_max[MAX_NR_ZONES] __initdata = { 255 , 255, 255, };

/*
 * Free_page() adds the page to the free lists. This is optimized for
 * fast normal cases (no error jumps taken normally).
 *
 * The way to optimize jumps for gcc-2.2.2 is to:
 *  - select the "normal" case and put it inside the if () { XXX }
 *  - no else-statements if you can avoid them
 *
 * With the above two rules, you get a straight-line execution path
 * for the normal case, giving better asm-code.
 */

#define memlist_init(x) INIT_LIST_HEAD(x) // 初始化链表头. @see linux/include/linux/list.h
//! 在链表头部添加一个节点.
#define memlist_add_head list_add
#define memlist_add_tail list_add_tail
//! 从链表中删除一个节点.
#define memlist_del list_del
//! list_entry 是container_of的一个特化版本，用于从某个成员获取包含它的结构体指针.
#define memlist_entry list_entry
#define memlist_next(x) ((x)->next)
#define memlist_prev(x) ((x)->prev)

/*
 * Temporary debugging check.
 * @brief Checks if a page descriptor belongs to the specified zone and is within its range.
 */
#define BAD_RANGE(zone,x) (((zone) != (x)->zone) || (((x)-mem_map) < (zone)->zone_start_mapnr) || (((x)-mem_map) >= (zone)->zone_start_mapnr+(zone)->size))

/*
 * __NOTE__: Buddy system. Hairy. You really aren't expected to understand this
 *
 * Hint: -mask = 1+~mask
 */

static void FASTCALL(__free_pages_ok (struct page *page, unsigned int order));

/**
 * @brief Free a block of pages and return it to the buddy allocator.
 * @ref buddy-system
 *
 * @param page Pointer to the first struct page of the block to free.
 * @param order Order of the block to free (0 = single page, 1 = two pages,
 *              etc.).
 * @return void
 *
 * @note The caller must have dropped the page's reference count to zero
 *       (so the page is no longer in use). Typically callers invoke
 *       @c put_page_testzero (or similar) before calling this routine.
 *       This function may place pages on the per-task local freelist
 *       (if the caller allows local freelists) or the global zone free
 *       lists while holding the zone lock. It performs internal checks
 *       and will BUG() on serious inconsistencies (e.g. pages still
 *       mapped, locked, active, or part of the swap cache).
 */
static void __free_pages_ok (struct page *page, unsigned int order)
{
	unsigned long index, page_idx, mask, flags;
	free_area_t *area;
	struct page *base;
	zone_t *zone;

	/* Yes, think what happens when other parts of the kernel take 
	 * a reference to a page in order to pin it for io. -ben
	 */
	// 如果页面还在 LRU 中，先移除 @see lru
	if (PageLRU(page))
		lru_cache_del(page);

	if (page->buffers)
		BUG();
	if (page->mapping)
		BUG();
	if (!VALID_PAGE(page))
		BUG();
	if (PageSwapCache(page))
		BUG();
	if (PageLocked(page))
		BUG();
	if (PageLRU(page))
		BUG();
	if (PageActive(page))
		BUG();
	// 清除 referenced 和 dirty 标志，以防止页面被错误地认为是脏页或被引用过.
	page->flags &= ~((1<<PG_referenced) | (1<<PG_dirty));

	/** 
	 * 这里如果进程设置了 PF_FREE_PAGES 标志，表示允许使用本地空闲页列表进行“慢路径”释放，
	 * 即释放到自己的本地列表，只有自己清楚这个page被释放了，而不是直接放回全局空闲列表。
	 * 这样可以减少对全局锁的争用，提高释放效率，提高缓存局部性。 否则，直接返回全局空闲列表。
	 * @see `balance_classzone()`: 对于那些没被复用的页面，进程在退出回收逻辑前，必须把它们还给系统。
	 * (依然是调用`__free_pages_ok`, 前提`current->flags &= ~(PF_MEMALLOC | PF_FREE_PAGES); // 清除“正在回收”的标记`)
	 */
	if (current->flags & PF_FREE_PAGES)
		goto local_freelist;
 back_local_freelist:

	zone = page->zone;

	/*
	 * mask = ...11111100 (以order=2为例)
	 * 1. ~mask 用于检查对齐: page_idx & ~mask
	 * 2. -mask 等于 1 << order, 用于计算块大小和定位伙伴: page_idx ^ -mask
	 */
	mask = (~0UL) << order;
	base = zone->zone_mem_map;
	page_idx = page - base;           // 计算页面在zone内的页索引.
	if (page_idx & ~mask)
		BUG();
	index = page_idx >> (1 + order);  // 计算页面在当前阶的位图中的索引.

	area = zone->free_area + order;   // 获取对应阶的 free_area 结构体.

	spin_lock_irqsave(&zone->lock, flags);

	zone->free_pages -= mask;         //! = zone->free_pages + (2^order) 增加空闲页计数.

	// 直到达到了最大阶
	while (mask + (1 << (MAX_ORDER-1))) {
		struct page *buddy1, *buddy2;

		if (area >= zone->free_area + MAX_ORDER)
			BUG();
		//! `test_and_set`: 翻转buddy map中的对应位，如果翻转后是1("相同状态", 即表示伙伴此时也空闲)，则继续合并.
		if (!__test_and_change_bit(index, area->map))
			/*
			 * the buddy page is still allocated.
			 */
			break;
		/*
		 * Move the buddy up one level.
		 */
		buddy1 = base + (page_idx ^ -mask); //! 直接把 page_idx 的第 order 位取反就是伙伴页的索引. (还记得怎么写二进制吗?:)
		buddy2 = base + page_idx;
		if (BAD_RANGE(zone,buddy1))
			BUG();
		if (BAD_RANGE(zone,buddy2))
			BUG();

	    // 把伙伴从当前order的空闲链表中删除. (伙伴之前是空闲的,记得吗?)
		memlist_del(&buddy1->list);
		// 升 order 递归检查
		mask <<= 1;
		area++;
		index >>= 1;
		page_idx &= mask;
	}
	// 把合并后的大块放回对应order的空闲链表.
	memlist_add_head(&(base + page_idx)->list, &area->free_list);

	spin_unlock_irqrestore(&zone->lock, flags);
	return;

 local_freelist:
	if (current->nr_local_pages)
		goto back_local_freelist;
	if (in_interrupt())
		goto back_local_freelist;		

    list_add(&page->list, &current->local_pages); // 挂到进程自己的链表上
    page->index = order;                          // 记录阶数
    current->nr_local_pages++;                    // 私有计数加1
}

/**
 * @brief Toggles the bit in the buddy bitmap for a specific block.
 * 
 * In Linux 2.4, each bit in the `free_area->map` represents a pair of buddies.
 * Toggling the bit indicates that one of the buddies has changed its state (allocated or freed).
 * If the bit becomes 0 after toggling during a free operation, it means both buddies are now free
 * and can be coalesced.
 * 
 * @param index The page index of the block.
 * @param order The order of the block.
 * @param area  Pointer to the free_area_t structure.
 * 
 * @note For example, if order=2 (4 pages), index(page)=12, 4th block node in free_area->free_list
 *       then the buddy pair represented is pages 8-11 and 12-15. buddy 2, 3rd & 4th pages block. 
 *       This pair corresponds to the 1st bit in the bitmap.
 *       In this case, you toggle (XOR) bit 1 (1 = 12 >> (2 + 1))
 */
#define MARK_USED(index, order, area) \
	__change_bit((index) >> (1+(order)), (area)->map)

/**
 * @brief Recursively splits a larger block into smaller buddies until the target order is reached.
 * 
 * @param zone  The memory zone.
 * @param page  The starting page of the large block.
 * @param index The page index of the block.
 * @param low   The target order requested.
 * @param high  The current order of the large block.
 * @param area  The free_area_t corresponding to the 'high' order.
 * @return struct page* The pointer to the allocated block of 'low' order.
 */
static inline struct page * expand (zone_t *zone, struct page *page,
	 unsigned long index, int low, int high, free_area_t * area)
{
	unsigned long size = 1 << high; // 当前块的大小（页数）

	//! 不断地将大块分左右，左边放回，右边继续拆，直到拆到目标order为止.
	while (high > low) {
		if (BAD_RANGE(zone,page))
			BUG();
		// order--
		area--;
		high--;
		size >>= 1;
        // 切左留右把拆下的buddy（左半边）放回对应order的空闲链表
		memlist_add_head(&(page)->list, &(area)->free_list);
		MARK_USED(index, high, area); // 改变状态
		index += size;  // 留下右半边的页块
		page += size;   // 同上
	}
	if (BAD_RANGE(zone,page))
		BUG();
    // 最终返回右边块的起始页指针.
	return page;
}

static FASTCALL(struct page * rmqueue(zone_t *zone, unsigned int order));
/**
 * @brief Removes a block of pages from the zone's free lists.
 * 
 * This is the core allocation function of the buddy system. It searches for a free block
 * starting from the requested order and moving up to MAX_ORDER. If a larger block is found,
 * it is split using @see `expand()`.
 * 
 * @param zone  The zone to allocate from.
 * @param order The requested order (2^order pages).
 * @return struct page* Pointer to the first page of the allocated block, or NULL if failed.
 */
static struct page * rmqueue(zone_t *zone, unsigned int order)
{
	free_area_t * area = zone->free_area + order; // free_area[order]
	unsigned int curr_order = order;
	struct list_head *head, *curr; // iterators
	unsigned long flags; // spinlock flags
	struct page *page;   // page to return

	// `spin_lock_irqsave`在获取自旋锁的同时保存中断状态并禁用本地中断，防止死锁.
	// 保护zone的free_area和free_pages等数据结构.
	spin_lock_irqsave(&zone->lock, flags);
	do {
		head = &area->free_list;
		curr = memlist_next(head); // curr = head->next

		if (curr != head) {
			// 如果在当前order的free_list但凡有一个空闲块，就分配它:
			unsigned int index;

			// 返回链表节点字段`struct list_head list` = `curr` 的 `struct page`的指针
			// 因为这里是用一个非结构体的指针去获取结构体指针，所以用到了container_of的特化版本list_entry.
			page = memlist_entry(curr, struct page, list);
			if (BAD_RANGE(zone,page)) // If page is not in zone range, BUG().
				BUG();
			memlist_del(curr);        // 从空闲链表中删除该节点.
			index = page - zone->zone_mem_map; // 计算page在zone内的页索引.
			if (curr_order != MAX_ORDER-1)     // 如果不是最高阶, 更新位图.(最高阶不需要合并, 没有位图维护)
				MARK_USED(index, curr_order, area);
			zone->free_pages -= 1UL << order;  // 分配了2^order个页框, 更新zone的空闲页计数.

			//! 调用expand拆分大块为小块，直到达到请求的order. 注意这里 page 不是原始的page了.
			page = expand(zone, page, index, order, curr_order, area);
			spin_unlock_irqrestore(&zone->lock, flags);

			set_page_count(page, 1); // __NOTE__: 设置page引用计数为1，表示已分配.
			if (BAD_RANGE(zone,page))
				BUG();
			if (PageLRU(page))
				BUG();
			if (PageActive(page))
				BUG();
			return page;	
		}
		// 没有找到合适的块，尝试更高阶的块.
		curr_order++;
		area++; // area = zone->free_area[curr_order] 注意这里自增显然更快
	} while (curr_order < MAX_ORDER);
	spin_unlock_irqrestore(&zone->lock, flags);

	// 没有找到合适的块，返回NULL. 这时候意味着需要进入慢路径处理了.
	return NULL;
}

#ifndef CONFIG_DISCONTIGMEM
struct page *_alloc_pages(unsigned int gfp_mask, unsigned int order)
{
	return __alloc_pages(gfp_mask, order,
		contig_page_data.node_zonelists+(gfp_mask & GFP_ZONEMASK));
}
#endif

static struct page * FASTCALL(balance_classzone(zone_t *, unsigned int, unsigned int, int *));
static struct page * balance_classzone(zone_t * classzone, unsigned int gfp_mask, unsigned int order, int * freed)
{
	struct page * page = NULL;
	int __freed = 0;

	if (!(gfp_mask & __GFP_WAIT))
		goto out;
	if (in_interrupt())
		BUG();

	current->allocation_order = order;
	current->flags |= PF_MEMALLOC | PF_FREE_PAGES;

	__freed = try_to_free_pages(classzone, gfp_mask, order);

	current->flags &= ~(PF_MEMALLOC | PF_FREE_PAGES);

	if (current->nr_local_pages) {
		struct list_head * entry, * local_pages;
		struct page * tmp;
		int nr_pages;

		local_pages = &current->local_pages;

		if (likely(__freed)) {
			/* pick from the last inserted so we're lifo */
			entry = local_pages->next;
			do {
				tmp = list_entry(entry, struct page, list);
				if (tmp->index == order && memclass(tmp->zone, classzone)) {
					list_del(entry);
					current->nr_local_pages--;
					set_page_count(tmp, 1);
					page = tmp;

					if (page->buffers)
						BUG();
					if (page->mapping)
						BUG();
					if (!VALID_PAGE(page))
						BUG();
					if (PageSwapCache(page))
						BUG();
					if (PageLocked(page))
						BUG();
					if (PageLRU(page))
						BUG();
					if (PageActive(page))
						BUG();
					if (PageDirty(page))
						BUG();

					break;
				}
			} while ((entry = entry->next) != local_pages);
		}

		nr_pages = current->nr_local_pages;
		/* free in reverse order so that the global order will be lifo */
		while ((entry = local_pages->prev) != local_pages) {
			list_del(entry);
			tmp = list_entry(entry, struct page, list);
			__free_pages_ok(tmp, tmp->index);
			if (!nr_pages--)
				BUG();
		}
		current->nr_local_pages = 0;
	}
 out:
	*freed = __freed;
	return page;
}

/*
 * This is the 'heart' of the zoned buddy allocator:
 */
/**
 * @brief High-level entry point for page frame allocation.
 * 
 * This function implements the core logic of the zoned buddy allocator. It iterates through
 * the provided @p zonelist and attempts to allocate a block of pages of the requested @p order.
 * If initial allocation fails, it triggers kswapd and may perform direct reclamation.
 * 
 * @param gfp_mask Allocation flags (e.g., GFP_KERNEL, GFP_ATOMIC).
 * @param order    The order of the allocation (2^order pages).
 * @param zonelist The list of zones to try for allocation, in priority order.
 * @return struct page* Pointer to the first page of the allocated block, or NULL if failed.
 */
struct page * __alloc_pages(unsigned int gfp_mask, unsigned int order, zonelist_t *zonelist)
{
	unsigned long min;
	zone_t **zone, * classzone;
	struct page * page;
	int freed;

	zone = zonelist->zones;
	classzone = *zone;
	min = 1UL << order;
	for (;;) {
		zone_t *z = *(zone++);
		if (!z)
			break;

		min += z->pages_low;
		if (z->free_pages > min) {
			page = rmqueue(z, order);
			if (page)
				return page;
		}
	}

	/**
	 * Allocation failed across all zones. Signal kswapd to perform page reclamation
	 * by setting the need_balance flag on the classzone, which indicates memory pressure.
	 */
	classzone->need_balance = 1;
	mb();
	if (waitqueue_active(&kswapd_wait))
		wake_up_interruptible(&kswapd_wait);

	zone = zonelist->zones;
	min = 1UL << order;
	for (;;) {
		unsigned long local_min;
		zone_t *z = *(zone++);
		if (!z)
			break;

		local_min = z->pages_min;
		if (!(gfp_mask & __GFP_WAIT))
			local_min >>= 2;
		min += local_min;
		if (z->free_pages > min) {
			page = rmqueue(z, order);
			if (page)
				return page;
		}
	}

	/* here we're in the low on memory slow path */

rebalance:
	if (current->flags & (PF_MEMALLOC | PF_MEMDIE)) {
		zone = zonelist->zones;
		for (;;) {
			zone_t *z = *(zone++);
			if (!z)
				break;

			page = rmqueue(z, order);
			if (page)
				return page;
		}
		return NULL;
	}

	/* Atomic allocations - we can't balance anything */
	if (!(gfp_mask & __GFP_WAIT))
		return NULL;

	page = balance_classzone(classzone, gfp_mask, order, &freed);
	if (page)
		return page;

	zone = zonelist->zones;
	min = 1UL << order;
	for (;;) {
		zone_t *z = *(zone++);
		if (!z)
			break;

		min += z->pages_min;
		if (z->free_pages > min) {
			page = rmqueue(z, order);
			if (page)
				return page;
		}
	}

	/* Don't let big-order allocations loop */
	if (order > 3)
		return NULL;

	/* Yield for kswapd, and try again */
	current->policy |= SCHED_YIELD;
	__set_current_state(TASK_RUNNING);
	schedule();
	goto rebalance;
}

/*
 * Common helper functions.
 */
/**
 * @brief Allocate 2^order pages and return their virtual (linear) address.
 * @param gfp_mask Allocation flags
 * @param order Power of two pages to allocate
 * @return unsigned long Virtual address of the allocated area, or 0 on failure
 * @note Simply `(unsigned long) page_address(alloc_pages(gfp_mask, order))`
 */
unsigned long __get_free_pages(unsigned int gfp_mask, unsigned int order)
{
	struct page * page;

	page = alloc_pages(gfp_mask, order);
	if (!page)                                  // 好代码
		return 0;
	return (unsigned long) page_address(page);
}

/**
 * @brief Allocate a single zero-filled page and return its virtual address.
 * @param gfp_mask Allocation flags
 * @return unsigned long Virtual address of the allocated page, or 0 on failure
 * @warning Simply use `alloc_pages(gfp_mask | __GFP_ZERO, 0)` instead after Linux 2.6.11
 */
unsigned long get_zeroed_page(unsigned int gfp_mask)
{
	struct page * page;

	page = alloc_pages(gfp_mask, 0);
	if (page) {
		void *address = page_address(page);
		clear_page(address);
		return (unsigned long) address;
	}
	return 0;
}

/**
 * @brief Free 2^order pages and return them to the buddy allocator.
 * @param page Pointer to the first struct page of the block to free
 * @param order Order of the block to free (0 = single page, 1 = two pages, etc.)
 * @return void
 * 
 * @warning **No validation on other pages in the block:** This function only checks
 *          the reference count of the FIRST page (via put_page_testzero()). If the
 *          caller mistakenly incremented the refcount of other pages in the block,
 *          this function will still free the entire block to the buddy allocator,
 *          resulting in memory leaks or corruption. Callers MUST ensure all pages
 *          in the block are released together as an atomic unit, never separately.
 *          This is modified after Linux 2.6.18, which introduced page->_count &
 *          page->_mapcount and compound page support to track multi-page allocations.
 */
void __free_pages(struct page *page, unsigned int order)
{
    /** 
	 * @note Checks if page is reserved and decrements refcount; only frees to buddy if 
     * PG_reserved flag is equal to 0 and count reaches zero
     */
	if (!PageReserved(page) && put_page_testzero(page))
		__free_pages_ok(page, order);
}

/**
 * @brief Free 2^order pages and return them to the buddy allocator, given a virtual address.
 * @param addr Virtual (linear) address of the pages to free
 * @param order Order of the block to free (0 = single page, 1 = two pages, etc.)
 * @return void
 * @note Simply converts virtual address to struct page via virt_to_page(), then calls __free_pages()
 */
void free_pages(unsigned long addr, unsigned int order)
{
	if (addr != 0)
		__free_pages(virt_to_page(addr), order);
}

/*
 * Total amount of free (allocatable) RAM:
 */
unsigned int nr_free_pages (void)
{
	unsigned int sum;
	zone_t *zone;
	pg_data_t *pgdat = pgdat_list;

	sum = 0;
	while (pgdat) {
		for (zone = pgdat->node_zones; zone < pgdat->node_zones + MAX_NR_ZONES; zone++)
			sum += zone->free_pages;
		pgdat = pgdat->node_next;
	}
	return sum;
}

/*
 * Amount of free RAM allocatable as buffer memory:
 */
unsigned int nr_free_buffer_pages (void)
{
	pg_data_t *pgdat = pgdat_list;
	unsigned int sum = 0;

	do {
		zonelist_t *zonelist = pgdat->node_zonelists + (GFP_USER & GFP_ZONEMASK);
		zone_t **zonep = zonelist->zones;
		zone_t *zone;

		for (zone = *zonep++; zone; zone = *zonep++) {
			unsigned long size = zone->size;
			unsigned long high = zone->pages_high;
			if (size > high)
				sum += size - high;
		}

		pgdat = pgdat->node_next;
	} while (pgdat);

	return sum;
}

#if CONFIG_HIGHMEM
unsigned int nr_free_highpages (void)
{
	pg_data_t *pgdat = pgdat_list;
	unsigned int pages = 0;

	while (pgdat) {
		pages += pgdat->node_zones[ZONE_HIGHMEM].free_pages;
		pgdat = pgdat->node_next;
	}
	return pages;
}
#endif

#define K(x) ((x) << (PAGE_SHIFT-10))

/*
 * Show free area list (used inside shift_scroll-lock stuff)
 * We also calculate the percentage fragmentation. We do this by counting the
 * memory on each free list with the exception of the first item on the list.
 */
void show_free_areas_core(pg_data_t *pgdat)
{
 	unsigned int order;
	unsigned type;
	pg_data_t *tmpdat = pgdat;

	printk("Free pages:      %6dkB (%6dkB HighMem)\n",
		K(nr_free_pages()),
		K(nr_free_highpages()));

	while (tmpdat) {
		zone_t *zone;
		for (zone = tmpdat->node_zones;
			       	zone < tmpdat->node_zones + MAX_NR_ZONES; zone++)
			printk("Zone:%s freepages:%6lukB min:%6lukB low:%6lukB " 
				       "high:%6lukB\n", 
					zone->name,
					K(zone->free_pages),
					K(zone->pages_min),
					K(zone->pages_low),
					K(zone->pages_high));
			
		tmpdat = tmpdat->node_next;
	}

	printk("( Active: %d, inactive: %d, free: %d )\n",
	       nr_active_pages,
	       nr_inactive_pages,
	       nr_free_pages());

	for (type = 0; type < MAX_NR_ZONES; type++) {
		struct list_head *head, *curr;
		zone_t *zone = pgdat->node_zones + type;
 		unsigned long nr, total, flags;

		total = 0;
		if (zone->size) {
			spin_lock_irqsave(&zone->lock, flags);
		 	for (order = 0; order < MAX_ORDER; order++) {
				head = &(zone->free_area + order)->free_list;
				curr = head;
				nr = 0;
				for (;;) {
					curr = memlist_next(curr);
					if (curr == head)
						break;
					nr++;
				}
				total += nr * (1 << order);
				printk("%lu*%lukB ", nr, K(1UL) << order);
			}
			spin_unlock_irqrestore(&zone->lock, flags);
		}
		printk("= %lukB)\n", K(total));
	}

#ifdef SWAP_CACHE_INFO
	show_swap_cache_info();
#endif	
}

void show_free_areas(void)
{
	show_free_areas_core(pgdat_list);
}

/*
 * Builds allocation fallback zone lists.
 */
/**
 * @brief 这是一个内部函数, 用于为每个 NUMA 节点构建 zonelist_t 结构体数组.
 */
static inline void build_zonelists(pg_data_t *pgdat)
{
	int i, j, k;

	for (i = 0; i <= GFP_ZONEMASK; i++) {
		zonelist_t *zonelist;
		zone_t *zone;

		zonelist = pgdat->node_zonelists + i;
		memset(zonelist, 0, sizeof(*zonelist));

		j = 0;
		k = ZONE_NORMAL;
		if (i & __GFP_HIGHMEM)
			k = ZONE_HIGHMEM;
		if (i & __GFP_DMA)
			k = ZONE_DMA;

		switch (k) {
			default:
				BUG();
			/*
			 * fallthrough:
			 */
			case ZONE_HIGHMEM:
				zone = pgdat->node_zones + ZONE_HIGHMEM;
				if (zone->size) {
#ifndef CONFIG_HIGHMEM
					BUG();
#endif
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
		zonelist->zones[j++] = NULL;
	} 
}

#define LONG_ALIGN(x) (((x)+(sizeof(long))-1)&~((sizeof(long))-1))

/*
 * Set up the zone data structures:
 *   - mark all pages reserved
 *   - mark all memory queues empty
 *   - clear the memory bitmaps
 */
/**
 * @brief MM initialization function for setting up memory zones and page structures.
 * 
 * @param nid Node id (NUMA node index) for which zones are initialized.
 * @param pgdat Pointer to the node's `pg_data_t` structure (node descriptor).
 * @param gmap Address of a `struct page *` where the allocated node `mem_map` is returned.
 * @param zones_size (per-zone) Array of sizes in pages for each zone on this node.
 * @param zone_start_paddr Physical start address (byte) of the first page in the node.
 * @param zholes_size Optional array (per-zone) of page counts that are unusable (holes); may be NULL.
 * @param lmem_map Optional preallocated `struct page *` mem_map; if NULL the function allocates and aligns one.
 * @return void
 * 
 * @ref physical-virtual-distinction
 * @see 00-concepts/swappable-page.md: 
 * 系统在初始化时将部分页框标记为不可对换的,防止内核数据被换出.
 */
void __init free_area_init_core(int nid, pg_data_t *pgdat, struct page **gmap,
	unsigned long *zones_size, unsigned long zone_start_paddr, 
	unsigned long *zholes_size, struct page *lmem_map)
{
	/* iterator for walking the new mem_map pages */
	struct page *p;
	/* general loop counters: 'i' for per-zone/order loops, 'j' for zone index */
	unsigned long i, j;
	/* size in bytes of the mem_map array for this node */
	unsigned long map_size;
	/* totalpages: sum of zones_size[]; realtotalpages subtracts holes */
	unsigned long totalpages, offset, realtotalpages;
	/* required alignment for zone start to keep buddy boundaries OK */
	const unsigned long zone_required_alignment = 1UL << (MAX_ORDER-1);

	/* __OVERDONE__(gzh): i386 架构下的页对齐检查. 非相关.
	 * @brief 保证 zone_start_paddr 地址是页对齐的.
	 *
	 * PAGE_MASK 在 i386 架构下定义为 0xFFFFF000, 区分页地址和页内偏移.
	 * @code
	 * PAGE_MASK = 0xFFFFF000
	 * ~PAGE_MASK = 0x00000FFF
	 * zone_start_paddr & ~PAGE_MASK = 0 	// 表示 zone_start_paddr
	 * 										// 的低 12 位全为 0, 即地址是页对齐的.
	 * @endcode
	 * PAGE_SHIFT = 12 表示页大小为 2^12 = 4096 字节 = 4KB, PAGE_MASK 用于地址的页对齐.
	 * 如果给定了一个地址, 要得到该地址所在页的起始地址,可以使用 PAGE_MASK 进行按位与操作:
	 * @code
	 * PAGE_MASK = ~((1UL << PAGE_SHIFT) - 1) = 0xFFFFF000
	 * unsigned long addr = 0x12345; // 示例地址
	 * unsigned long page_start = addr & PAGE_MASK;
	 * // 结果为 0x12000, 即该地址所在页的起始地址
	 * @endcode
	 * @see page.h 中对 PAGE_MASK 的定义.
	*/
	if (zone_start_paddr & ~PAGE_MASK)
		BUG();

	//! @brief 通过 zones_size 数组和 zholes_size 数组计算节点的实际可用页数.
	//! @ref zone-based-memory-management.md
	totalpages = 0;
	for (i = 0; i < MAX_NR_ZONES; i++) {
		unsigned long size = zones_size[i]; // zones_size 数组存储每个 zone 的页数.
		totalpages += size;
	}
	realtotalpages = totalpages; // totalpages 保护变量, 用于计算实际可用页数.
	if (zholes_size)
		for (i = 0; i < MAX_NR_ZONES; i++)
			realtotalpages -= zholes_size[i];
			
	printk("On node %d totalpages: %lu\n", nid, realtotalpages); // __PRINTK__

	// Init active/inactive page lists.
	//! active_list 和 inactive_list 用于管理活跃和非活跃页框的链表.
	//! __GLOBAL__: 是 global 变量, 在文件开头定义, 在这里初始化
	INIT_LIST_HEAD(&active_list);
	INIT_LIST_HEAD(&inactive_list);

	/**
	 * Some architectures (with lots of mem and discontinous memory
	 * maps) have to search for a good mem_map area:
	 * For discontigmem, the conceptual mem map array starts from 
	 * PAGE_OFFSET, we need to align the actual array onto a mem map 
	 * boundary, so that MAP_NR works.
	 * 
	 * @note conceptual mem map array: 指的是 mem_map 数组在内核虚拟地址空间中的理想位置.
	 * 即: 从内核的视角来看, 它用mem_map 数组来"连续"地表示物理内存页框, 即使实际的物理内存可能是不连续的.
	 * 通过将 mem_map 数组放置在 PAGE_OFFSET 之后并进行适当的对齐, 内核可以更方便地通过索引访问物理页框.
	 * 
	 * @note MAP_NR(addr): 是一个宏, 计算((addr) - PAGE_OFFSET) >> PAGE_SHIFT).
	 * 该宏用于将内核虚拟地址转换为对应的物理页框号.
	 */
    // 如果传入的 lmem_map 参数为空, 则将其分配为新的 mem_map, 否则直接使用传入的 lmem_map.
	map_size = (totalpages + 1)*sizeof(struct page); // +1 for possible rounding issues
	if (lmem_map == (struct page *)0) {
		lmem_map = (struct page *) alloc_bootmem_node(pgdat, map_size); //! 使用@ref bootmem分配器分配 lmem_map 内存.
		lmem_map = (struct page *)(PAGE_OFFSET + 
			MAP_ALIGN((unsigned long)lmem_map - PAGE_OFFSET)); // 保证加上 PAGE_OFFSET 后的地址是对齐的.
	}
    // 将分配好的内存空间全部赋给 pgdat->node_mem_map, 并初始化 pgdat 结构体的其他字段.
	*gmap = pgdat->node_mem_map = lmem_map;
	pgdat->node_size = totalpages;
	pgdat->node_start_paddr = zone_start_paddr;
	pgdat->node_start_mapnr = (lmem_map - mem_map);
	pgdat->nr_zones = 0;

	/*
	 * Initially all pages are reserved - free ones are freed
	 * up by free_all_bootmem() once the early boot process is
	 * done.
	 */
	/* Initialize every struct page: clear count, mark reserved, init lists */
	//! __NOTE__: page的初始化方式
	for (p = lmem_map; p < lmem_map + totalpages; p++) {
		set_page_count(p, 0);
		/* mark reserved so boot-time allocator won't hand them out */
		SetPageReserved(p);
		init_waitqueue_head(&p->wait);
		memlist_init(&p->list);
	}

	//! 计算 lmem_map 相对于全局 mem_map 的偏移量. mem_map 初始化见`free_area_init_node()` @ref mm-core-variables
	offset = lmem_map - mem_map;
	/**
	 * __NOTICE__: 为 page 所在的 zone 初始化 zone 结构体.
	 */  
	for (j = 0; j < MAX_NR_ZONES; j++) {
		zone_t *zone = pgdat->node_zones + j;
		unsigned long mask;
		unsigned long size, realsize;

		realsize = size = zones_size[j];
		if (zholes_size)
			realsize -= zholes_size[j];

		printk("zone(%lu): %lu pages.\n", j, size);
		zone->size = size;
		zone->name = zone_names[j];
		zone->lock = SPIN_LOCK_UNLOCKED;
		zone->zone_pgdat = pgdat;
		zone->free_pages = 0;
		zone->need_balance = 0;
		if (!size)
			continue;

		pgdat->nr_zones = j+1;

		mask = (realsize / zone_balance_ratio[j]);
		if (mask < zone_balance_min[j])
			mask = zone_balance_min[j];
		else if (mask > zone_balance_max[j])
			mask = zone_balance_max[j];
		zone->pages_min = mask;
		zone->pages_low = mask*2;
		zone->pages_high = mask*3;

		zone->zone_mem_map = mem_map + offset;
		zone->zone_start_mapnr = offset;
		zone->zone_start_paddr = zone_start_paddr;

		if ((zone_start_paddr >> PAGE_SHIFT) & (zone_required_alignment-1))
			printk("BUG: wrong zone alignment, it will crash\n");

		/* 这里为 zone 内的每个 page 设置其所属的 zone 和虚拟地址映射. */
		for (i = 0; i < size; i++) {
			struct page *page = mem_map + offset + i;
			page->zone = zone;
			if (j != ZONE_HIGHMEM)
				page->virtual = __va(zone_start_paddr);
			zone_start_paddr += PAGE_SIZE;
		}

		offset += size;
		/* __TODO__: Build buddy bitmaps for each free_area order until MAX_ORDER-1 */
		for (i = 0; ; i++) {
			unsigned long bitmap_size;

			memlist_init(&zone->free_area[i].free_list);
			if (i == MAX_ORDER-1) {
				zone->free_area[i].map = NULL; //!< @note 可以看到最高阶甚至没有位图
				break;
			}

			/*
			 * Page buddy system uses "index >> (i+1)",
			 * where "index" is at most "size-1".
			 *
			 * The extra "+3" is to round down to byte
			 * size (8 bits per byte assumption). Thus
			 * we get "(size-1) >> (i+4)" as the last byte
			 * we can access.
			 *
			 * The "+1" is because we want to round the
			 * byte allocation up rather than down. So
			 * we should have had a "+7" before we shifted
			 * down by three. Also, we have to add one as
			 * we actually _use_ the last bit (it's [0,n]
			 * inclusive, not [0,n[).
			 *
			 * So we actually had +7+1 before we shift
			 * down by 3. But (n+8) >> 3 == (n >> 3) + 1
			 * (modulo overflows, which we do not have).
			 *
			 * Finally, we LONG_ALIGN because all bitmap
			 * operations are on longs.
			 */
						/* number of bytes needed for the map at this order (rounded) */
						bitmap_size = (size-1) >> (i+4);
						bitmap_size = LONG_ALIGN(bitmap_size+1);
						/* allocate the bitmap used to track free buddies at this order */
						zone->free_area[i].map = 
							(unsigned long *) alloc_bootmem_node(pgdat, bitmap_size);
		}
	}
		/* finalize by building zonelists used by the allocator fallback paths */
		build_zonelists(pgdat);
}

/**
 * @brief This function delare the parameters that @ref free_area_init_core takes
 */
void __init free_area_init(unsigned long *zones_size)
{
	free_area_init_core(0, &contig_page_data, &mem_map, zones_size, 0, 0, 0);
}

static int __init setup_mem_frac(char *str)
{
	int j = 0;

	while (get_option(&str, &zone_balance_ratio[j++]) == 2);
	printk("setup_mem_frac: ");
	for (j = 0; j < MAX_NR_ZONES; j++) printk("%d  ", zone_balance_ratio[j]);
	printk("\n");
	return 1;
}

__setup("memfrac=", setup_mem_frac);
