/*
 *  linux/mm/swap.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 */

/*
 * This file contains the default values for the opereation of the
 * Linux VM subsystem. Fine-tuning documentation can be found in
 * linux/Documentation/sysctl/vm.txt.
 * Started 18.12.91
 * Swap aging added 23.2.95, Stephen Tweedie.
 * Buffermem limits added 12.3.98, Rik van Riel.
 */

#include <linux/mm.h>
#include <linux/kernel_stat.h>
#include <linux/swap.h>
#include <linux/swapctl.h>
#include <linux/pagemap.h>
#include <linux/init.h>

#include <asm/dma.h>
#include <asm/uaccess.h> /* for copy_to/from_user */
#include <asm/pgtable.h>

/* How many pages do we try to swap or page in/out together? */
int page_cluster;

pager_daemon_t pager_daemon = {
	512,	/* base number for calculating the number of tries */
	SWAP_CLUSTER_MAX,	/* minimum number of tries */
	8,	/* do swap I/O in clusters of this size */
};

/*
 * Move an inactive page to the active list.
 */
/**
 * @brief 将一个不活跃页面转移到活跃链表 (无锁版本)
 * 
 * 如果页面在 LRU 管理中且当前不活跃，则将其从 inactive_list 移至 active_list。
 */
static inline void activate_page_nolock(struct page * page)
{
	if (PageLRU(page) && !PageActive(page)) {
		del_page_from_inactive_list(page);
		add_page_to_active_list(page);
	}
}

/**
 * @brief 激活页面 (外部接口)
 * 
 * 当一个页面被访问（如通过 mark_page_accessed）且内核认为它足够“热”时调用。
 * 持有 pagemap_lru_lock 全局锁以保证链表操作的原子性。
 */
void activate_page(struct page * page)
{
	spin_lock(&pagemap_lru_lock);
	activate_page_nolock(page);
	spin_unlock(&pagemap_lru_lock);
}

/**
 * lru_cache_add: add a page to the page lists
 * @page: the page to add
 * 
 * @brief 将新页面首次加入 LRU 管理系统
 * 
 * 1. 检查并设置 PG_lru 标志，确保页面只被添加一次。
 * 2. 默认将新页面放入 inactive_list（给它一个观察期）。
 */
void lru_cache_add(struct page * page)
{
	if (!TestSetPageLRU(page)) {
		spin_lock(&pagemap_lru_lock);
		add_page_to_inactive_list(page);
		spin_unlock(&pagemap_lru_lock);
	}
}

/**
 * __lru_cache_del: remove a page from the page lists
 * @page: the page to add
 *
 * This function is for when the caller already holds
 * the pagemap_lru_lock.
 */
void __lru_cache_del(struct page * page)
{
	if (TestClearPageLRU(page)) {
		if (PageActive(page)) {
			del_page_from_active_list(page);
		} else {
			del_page_from_inactive_list(page);
		}
	}
}

/**
 * lru_cache_del: remove a page from the page lists
 * @page: the page to remove
 */
void lru_cache_del(struct page * page)
{
	spin_lock(&pagemap_lru_lock);
	__lru_cache_del(page);
	spin_unlock(&pagemap_lru_lock);
}

/*
 * Perform any setup for the swap system
 */
void __init swap_setup(void)
{
	unsigned long megs = num_physpages >> (20 - PAGE_SHIFT);

	/* Use a smaller cluster for small-memory machines */
	if (megs < 16)
		page_cluster = 2;
	else
		page_cluster = 3;
	/*
	 * Right now other parts of the system means that we
	 * _really_ don't want to cluster much more
	 */
}
