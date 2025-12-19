/*
 *  linux/mm/vmscan.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *
 *  Swap reorganised 29.12.95, Stephen Tweedie.
 *  kswapd added: 7.1.96  sct
 *  Removed kswapd_ctl limits, and swap out as many pages as needed
 *  to bring the system back to freepages.high: 2.4.97, Rik van Riel.
 *  Zone aware kswapd started 02/00, Kanoj Sarcar (kanoj@sgi.com).
 *  Multiqueue VM started 5.8.00, Rik van Riel.
 */

#include <linux/slab.h>
#include <linux/kernel_stat.h>
#include <linux/swap.h>
#include <linux/swapctl.h>
#include <linux/smp_lock.h>
#include <linux/pagemap.h>
#include <linux/init.h>
#include <linux/highmem.h>
#include <linux/file.h>
#include <linux/compiler.h>

#include <asm/pgalloc.h>

/*
 * The "priority" of VM scanning is how much of the queues we
 * will scan in one go. A value of 6 for DEF_PRIORITY implies
 * that we'll scan 1/64th of the queues ("queue_length >> 6")
 * during a normal aging round.
 */
#define DEF_PRIORITY (6)

/*
 * The swap-out function returns 1 if it successfully
 * scanned all the pages it was asked to (`count').
 * It returns zero if it couldn't do anything,
 *
 * rss may decrease because pages are shared, but this
 * doesn't count as having freed a page.
 */

/**
 * try_to_swap_out - 尝试换出一个特定的页面（解除 PTE 映射）
 * @param mm 进程内存描述符
 * @param vma 虚拟内存区域
 * @param address 虚拟地址
 * @param page_table PTE 指针
 * @param page 页面描述符
 * @param classzone 目标内存区
 *
 * @return 如果页面可以被释放（引用计数降为 0 或仅剩 swap cache 引用），返回 1，否则返回 0。
 *
 * 核心逻辑（拆解）：
 */
/* mm->page_table_lock is held. mmap_sem is not held */
static inline int try_to_swap_out(struct mm_struct * mm, struct vm_area_struct* vma, unsigned long address, pte_t * page_table, struct page *page, zone_t * classzone)
{
	pte_t pte;
	swp_entry_t entry;

	/* 1. 检查访问位 (Accessed Bit)
	 * 如果页面最近被访问过，或者被锁定（VM_LOCKED），则给它第二次机会。
	 * ptep_test_and_clear_young 会清除硬件 PTE 中的 Accessed 位。
	 */
	if ((vma->vm_flags & VM_LOCKED) || ptep_test_and_clear_young(page_table)) {
		mark_page_accessed(page); // 标记为已访问，可能会被移回 active_list
		return 0;
	}

	/* 2. 检查是否在活跃链表
	 * 如果页面已经在 Active 状态，不应该在这里被换出。
	 */
	if (PageActive(page))
		return 0;

	/* 3. 检查内存区压力
	 * 如果页面所属的 zone 并不是我们当前想要平衡的 zone，跳过。
	 */
	if (!memclass(page->zone, classzone))
		return 0;

	/* 4. 尝试锁定页面
	 * 如果页面已被锁定（可能正在 IO），跳过。
	 */
	if (TryLockPage(page))
		return 0;

	/* 5. 解除映射 (Unmap)
	 * 从这里开始，我们决定要"干掉"这个 PTE 了。
	 * 先刷新缓存，然后清除 PTE，最后刷新 TLB。
	 */
	flush_cache_page(vma, address);
	pte = ptep_get_and_clear(page_table);
	flush_tlb_page(vma, address);

	/* 6. 同步脏位
	 * 如果硬件 PTE 标记为脏，需要同步到 page->flags 中。
	 */
	if (pte_dirty(pte))
		set_page_dirty(page);

	/* 7. 处理已经在 Swap Cache 中的页面
	 * 如果页面已经在交换缓存中，说明磁盘上已经有一份拷贝了。
	 * 我们只需要把 PTE 指向那个交换条目即可。
	 */
	if (PageSwapCache(page)) {
		entry.val = page->index;
		swap_duplicate(entry); // 增加交换条目的引用计数
set_swap_pte:
		set_pte(page_table, swp_entry_to_pte(entry)); // 将 PTE 设置为交换条目
drop_pte:
		mm->rss--; // 进程驻留集大小减 1
		UnlockPage(page);
		{
			/* 检查页面是否可以被完全释放
			 * page_count == 2 通常意味着：1个来自 swap cache，1个来自当前正在处理的引用。
			 */
			int freeable = page_count(page) - !!page->buffers <= 2;
			page_cache_release(page); // 释放当前引用
			return freeable;
		}
	}

	/* 8. 处理文件映射页面 (Page Cache)
	 * 如果 page->mapping 不为空，说明这是一个文件映射页面。
	 * 我们只需要解除 PTE 映射，页面本身还留在 Page Cache 中，
	 * 后续由 shrink_cache() 来决定是否真正释放它。
	 */
	if (page->mapping)
		goto drop_pte;

	/* 9. 处理干净的匿名页
	 * 理论上匿名页不应该是干净的（除非刚分配还没写），
	 * 如果是干净的，直接丢弃。
	 */
	if (!PageDirty(page))
		goto drop_pte;

	/* 10. 处理带有 Buffer 的页面
	 * 匿名页通常没有 buffer，如果有，可能是并发竞争导致的异常情况，保留它。
	 */
	if (page->buffers)
		goto preserve;

	/* 11. 真正的换出逻辑 (Dirty Anonymous Page)
	 * 这是一个脏的、匿名的、需要交换空间的页面。
	 */
	for (;;) {
		entry = get_swap_page(); // 从交换分区分配一个条目
		if (!entry.val)
			break; // 没交换空间了...
		/* 
		 * 将页面添加到 Swap Cache。
		 * 这会建立页面与交换条目的关联。
		 */
		if (add_to_swap_cache(page, entry) == 0) {
			SetPageUptodate(page);
			set_page_dirty(page); // 标记为脏，确保后续会被写回磁盘
			goto set_swap_pte;
		}
		/* 如果添加失败（可能发生了竞争），释放交换条目并重试 */
		swap_free(entry);
	}

	/* No swap space left: 恢复 PTE，假装没发生过 */
preserve:
	set_pte(page_table, pte);
	UnlockPage(page);
	return 0;
}

/* mm->page_table_lock is held. mmap_sem is not held */
static inline int swap_out_pmd(struct mm_struct * mm, struct vm_area_struct * vma, pmd_t *dir, unsigned long address, unsigned long end, int count, zone_t * classzone)
{
	pte_t * pte;
	unsigned long pmd_end;

	if (pmd_none(*dir))
		return count;
	if (pmd_bad(*dir)) {
		pmd_ERROR(*dir);
		pmd_clear(dir);
		return count;
	}
	
	pte = pte_offset(dir, address);
	
	pmd_end = (address + PMD_SIZE) & PMD_MASK;
	if (end > pmd_end)
		end = pmd_end;

	do {
		if (pte_present(*pte)) {
			struct page *page = pte_page(*pte);

			if (VALID_PAGE(page) && !PageReserved(page)) {
				count -= try_to_swap_out(mm, vma, address, pte, page, classzone);
				if (!count) {
					address += PAGE_SIZE;
					break;
				}
			}
		}
		address += PAGE_SIZE;
		pte++;
	} while (address && (address < end));
	mm->swap_address = address;
	return count;
}

/* mm->page_table_lock is held. mmap_sem is not held */
static inline int swap_out_pgd(struct mm_struct * mm, struct vm_area_struct * vma, pgd_t *dir, unsigned long address, unsigned long end, int count, zone_t * classzone)
{
	pmd_t * pmd;
	unsigned long pgd_end;

	if (pgd_none(*dir))
		return count;
	if (pgd_bad(*dir)) {
		pgd_ERROR(*dir);
		pgd_clear(dir);
		return count;
	}

	pmd = pmd_offset(dir, address);

	pgd_end = (address + PGDIR_SIZE) & PGDIR_MASK;	
	if (pgd_end && (end > pgd_end))
		end = pgd_end;
	
	do {
		count = swap_out_pmd(mm, vma, pmd, address, end, count, classzone);
		if (!count)
			break;
		address = (address + PMD_SIZE) & PMD_MASK;
		pmd++;
	} while (address && (address < end));
	return count;
}

/* mm->page_table_lock is held. mmap_sem is not held */
static inline int swap_out_vma(struct mm_struct * mm, struct vm_area_struct * vma, unsigned long address, int count, zone_t * classzone)
{
	pgd_t *pgdir;
	unsigned long end;

	/* Don't swap out areas which are reserved */
	if (vma->vm_flags & VM_RESERVED)
		return count;

	pgdir = pgd_offset(mm, address);

	end = vma->vm_end;
	if (address >= end)
		BUG();
	do {
		count = swap_out_pgd(mm, vma, pgdir, address, end, count, classzone);
		if (!count)
			break;
		address = (address + PGDIR_SIZE) & PGDIR_MASK;
		pgdir++;
	} while (address && (address < end));
	return count;
}

/* Placeholder for swap_out(): may be updated by fork.c:mmput() */
struct mm_struct *swap_mm = &init_mm;

/**
 * swap_out_mm - 扫描特定 mm 的虚拟内存区域 (VMA)
 * @param mm 目标内存描述符
 * @param count 还需要换出的页面数量
 * @param mmcounter mm 计数器（用于 swap_out 中的循环控制）
 * @param classzone 目标内存区
 *
 * 逻辑：
 * 1. 锁定 mm->page_table_lock。
 * 2. 从 mm->swap_address 开始寻找 VMA。
 * 3. 遍历 VMA 链表，调用 swap_out_vma。
 * 4. 更新 mm->swap_address 以便下次继续。
 */
static inline int swap_out_mm(struct mm_struct * mm, int count, int * mmcounter, zone_t * classzone)
{
	unsigned long address;
	struct vm_area_struct* vma;

	/*
	 * Find the proper vm-area after freezing the vma chain 
	 * and ptes.
	 */
	spin_lock(&mm->page_table_lock);
	address = mm->swap_address;
	if (address == TASK_SIZE || swap_mm != mm) {
		/* We raced: don't count this mm but try again */
		++*mmcounter;
		goto out_unlock;
	}
	vma = find_vma(mm, address);
	if (vma) {
		if (address < vma->vm_start)
			address = vma->vm_start;

		for (;;) {
			count = swap_out_vma(mm, vma, address, count, classzone);
			vma = vma->vm_next;
			if (!vma)
				break;
			if (!count)
				goto out_unlock;
			address = vma->vm_start;
		}
	}
	/* Indicate that we reached the end of address space */
	mm->swap_address = TASK_SIZE;

out_unlock:
	spin_unlock(&mm->page_table_lock);
	return count;
}

/**
 * swap_out - 页面回收的"终极手段"：将进程占用的页面解除映射并换出
 * @param priority 优先级（决定扫描强度，但在 swap_out 中主要用于循环次数控制）
 * @param gfp_mask 分配掩码
 * @param classzone 目标内存区
 *
 * 逻辑：
 * 1. 遍历系统中的所有进程的 mm_struct（通过 mmlist 链表）。
 * 2. 使用全局变量 swap_mm 记录上次扫描到的位置（"指点"机制），确保公平性。
 * 3. 对选中的 mm，调用 swap_out_mm 尝试换出页面。
 * 4. 如果换出了足够的页面（SWAP_CLUSTER_MAX），则返回成功。
 */
static int FASTCALL(swap_out(unsigned int priority, unsigned int gfp_mask, zone_t * classzone));
static int swap_out(unsigned int priority, unsigned int gfp_mask, zone_t * classzone)
{
	int counter, nr_pages = SWAP_CLUSTER_MAX;
	struct mm_struct *mm;

	counter = mmlist_nr; // 系统中 mm 的总数
	do {
		// 检查是否需要调度，避免长时间占用 CPU
		if (unlikely(current->need_resched)) {
			__set_current_state(TASK_RUNNING);
			schedule();
		}

		spin_lock(&mmlist_lock);
		mm = swap_mm;
		// 如果当前 mm 已经扫描完（swap_address == TASK_SIZE）或者是 init_mm，则跳到下一个
		while (mm->swap_address == TASK_SIZE || mm == &init_mm) {
			mm->swap_address = 0; // 重置扫描地址，为下一轮做准备
			mm = list_entry(mm->mmlist.next, struct mm_struct, mmlist);
			if (mm == swap_mm) // 跑了一圈都没找到可扫描的 mm
				goto empty;
			swap_mm = mm; // 更新全局"指点"
		}

		/* Make sure the mm doesn't disappear when we drop the lock.. */
		atomic_inc(&mm->mm_users); // 增加引用计数，防止 mm 在扫描期间被销毁
		spin_unlock(&mmlist_lock);

		// 真正开始对这个 mm 进行扫描换出
		nr_pages = swap_out_mm(mm, nr_pages, &counter, classzone);

		mmput(mm); // 释放引用计数

		if (!nr_pages) // 如果已经换出了足够的页面，大功告成
			return 1;
	} while (--counter >= 0);

	return 0;

empty:
	spin_unlock(&mmlist_lock);
	return 0;
}

empty:
	spin_unlock(&mmlist_lock);
	return 0;
}

static int FASTCALL(shrink_cache(int nr_pages, zone_t * classzone, unsigned int gfp_mask, int priority));
/**
 * @brief 页面回收的核心引擎 (终极审判)
 * 
 * 该函数扫描 `inactive_list`，尝试释放指定数量的页面。它是 Linux 2.4 内存管理中
 * 最复杂、最关键的函数之一，涉及脏页写回、缓冲区释放、解除映射等多种逻辑。
 * 
 * 扫描与处理流程：
 * 1. **扫描限制**：通过 `max_scan` 限制扫描深度，防止在长链表上耗费过多时间。
 * 2. **调度避让**：检查 `need_resched`，确保回收过程不会导致系统失去响应。
 * 3. **页面状态检查**：
 *    - 忽略正在被使用的页面 (`page_count > 1`)。
 *    - 忽略不属于目标 `classzone` 的页面。
 * 4. **锁竞争处理**：如果页面被锁定，且正在进行 `Launder` (清洗/写回)，则视情况等待。
 * 5. **脏页处理 (Dirty Pages)**：
 *    - 如果页面是脏的，启动异步写回 (`writepage`)。
 *    - 页面不会立即释放，而是标记为 `Launder` 并移到链表头，等待写回完成。
 * 6. **缓冲区处理 (Buffer Pages)**：
 *    - 调用 `try_to_release_page` 尝试释放关联的 `buffer_head`。
 *    - 如果释放成功且页面无映射，则该页可以直接回收。
 * 7. **解除映射 (Mapped Pages)**：
 *    - 如果扫描过程中发现映射页过多 (`max_mapped` 耗尽)，触发 `swap_out`。
 * 8. **最终剥离**：
 *    - 将干净、未映射、无缓冲区的页面从 Page Cache 或 Swap Cache 中移除。
 *    - 减少引用计数，将其彻底归还给伙伴系统。
 * 
 * @param nr_pages 目标释放页面数。
 * @param classzone 目标内存区域。
 * @param gfp_mask 分配标志。
 * @param priority 优先级（值越小，扫描强度越大）。
 * @return int 尚未完成的释放目标数。
 */
static int shrink_cache(int nr_pages, zone_t * classzone, unsigned int gfp_mask, int priority)
{
	struct list_head * entry;
	int max_scan = nr_inactive_pages / priority;
	int max_mapped = min((nr_pages << (10 - priority)), max_scan / 10);

	spin_lock(&pagemap_lru_lock);
	while (--max_scan >= 0 && (entry = inactive_list.prev) != &inactive_list) {
		struct page * page;

		/* 1. 延迟管理：如果需要调度，先释放锁并让出 CPU */
		if (unlikely(current->need_resched)) {
			spin_unlock(&pagemap_lru_lock);
			__set_current_state(TASK_RUNNING);
			schedule();
			spin_lock(&pagemap_lru_lock);
			continue;
		}

		page = list_entry(entry, struct page, lru);

		if (unlikely(!PageLRU(page)))
			BUG();
		if (unlikely(PageActive(page)))
			BUG();

		/* 2. 链表维护：将页面移到链表头部，表示已处理，防止死循环扫描 */
		list_del(entry);
		list_add(entry, &inactive_list);

		/* 3. 引用计数检查：如果计数为 0，说明页面正在被释放过程中 */
		if (unlikely(!page_count(page)))
			continue;

		/* 4. 区域检查：只回收属于目标 Zone 或更低 Zone 的页面 */
		if (!memclass(page->zone, classzone))
			continue;

		/* 5. 映射检查：如果页面被映射了，或者有多个引用且有映射，跳转到映射处理逻辑 */
		if (!page->buffers && (page_count(page) != 1 || !page->mapping))
			goto page_mapped;

		/*
		 * 6. 锁定状态处理：
		 * 如果页面被锁定，可能正在进行 I/O。
		 * 如果设置了 Launder 且允许文件系统操作，则等待 I/O 完成。
		 */
		if (unlikely(TryLockPage(page))) {
			if (PageLaunder(page) && (gfp_mask & __GFP_FS)) {
				page_cache_get(page);
				spin_unlock(&pagemap_lru_lock);
				wait_on_page(page);
				page_cache_release(page);
				spin_lock(&pagemap_lru_lock);
			}
			continue;
		}

		/* 7. 脏页异步写回：
		 * 如果页面是脏的，且可以释放（引用计数为1），则启动 writepage。
		 * 注意：这里只启动写回，不等待，页面会留在 LRU 中。
		 */
		if (PageDirty(page) && is_page_cache_freeable(page) && page->mapping) {
			int (*writepage)(struct page *);

			writepage = page->mapping->a_ops->writepage;
			if ((gfp_mask & __GFP_FS) && writepage) {
				ClearPageDirty(page);
				SetPageLaunder(page);
				page_cache_get(page);
				spin_unlock(&pagemap_lru_lock);

				writepage(page);
				page_cache_release(page);

				spin_lock(&pagemap_lru_lock);
				continue;
			}
		}

		/* 8. 释放缓冲区：
		 * 许多页面（如文件系统元数据）带有 buffer_heads。
		 * 必须先释放这些小块内存，页面才能被回收。
		 */
		if (page->buffers) {
			spin_unlock(&pagemap_lru_lock);

			page_cache_get(page);

			if (try_to_release_page(page, gfp_mask)) {
				if (!page->mapping) {
					/* 匿名页且缓冲区已释放：直接回收 */
					spin_lock(&pagemap_lru_lock);
					UnlockPage(page);
					__lru_cache_del(page);

					page_cache_release(page);

					if (--nr_pages)
						continue;
					break;
				} else {
					/* 页面仍在缓存中，重新获取 LRU 锁继续后续剥离逻辑 */
					page_cache_release(page);
					spin_lock(&pagemap_lru_lock);
				}
			} else {
				/* 释放失败（可能缓冲区正忙），解锁并跳过 */
				UnlockPage(page);
				page_cache_release(page);

				spin_lock(&pagemap_lru_lock);
				continue;
			}
		}

		/* 9. 最终剥离阶段：需要持有 pagecache_lock */
		spin_lock(&pagecache_lock);

		/* 再次确认页面是否可以安全释放 */
		if (!page->mapping || !is_page_cache_freeable(page)) {
			spin_unlock(&pagecache_lock);
			UnlockPage(page);
page_mapped:
			/* 映射页计数器：如果太多映射页，说明需要 swap_out 来解除映射 */
			if (--max_mapped >= 0)
				continue;

			spin_unlock(&pagemap_lru_lock);
			swap_out(priority, gfp_mask, classzone);
			return nr_pages;
		}

		/* 如果写回还没完成，不能释放 */
		if (PageDirty(page)) {
			spin_unlock(&pagecache_lock);
			UnlockPage(page);
			continue;
		}

		/* 10. 从缓存中移除：
		 * 根据页面类型，从 inode 页面树或 swap cache 中删除。
		 */
		if (likely(!PageSwapCache(page))) {
			__remove_inode_page(page);
			spin_unlock(&pagecache_lock);
		} else {
			swp_entry_t swap;
			swap.val = page->index;
			__delete_from_swap_cache(page);
			spin_unlock(&pagecache_lock);
			swap_free(swap);
		}

		/* 11. 功德圆满：从 LRU 移除并释放物理页框 */
		__lru_cache_del(page);
		UnlockPage(page);

		page_cache_release(page);

		if (--nr_pages)
			continue;
		break;
	}
	spin_unlock(&pagemap_lru_lock);

	return nr_pages;
}

/*
 * This moves pages from the active list to
 * the inactive list.
 *
 * We move them the other way when we see the
 * reference bit on the page.
 */
/**
 * @brief 补充不活跃链表 (LRU 页面老化与降级)
 * 
 * 该函数是 LRU 算法中“第二次机会”机制的核心实现。它负责将页面从 `active_list` 
 * 转移到 `inactive_list`，从而为 `shrink_cache` 提供回收候选者。
 * 
 * 核心逻辑：
 * 1. **逆序扫描**：从 `active_list` 的末尾（最老的页面）开始向前扫描。
 * 2. **引用检查 (Aging)**：
 *    - 如果页面被访问过 (`PG_referenced` 置位)，则认为它依然“活跃”。
 *    - 清除引用标志，并将其移回 `active_list` 的头部（重新开始计时）。
 * 3. **降级 (Demotion)**：
 *    - 如果页面未被引用，则将其从 `active_list` 移除，加入 `inactive_list`。
 *    - **关键点**：在加入不活跃链表时，会重新设置 `PG_referenced`。这为页面在
 *      不活跃链表中提供了“第二次机会”——如果它在被真正回收前再次被访问，
 *      `activate_page` 会将其重新拉回活跃链表。
 * 
 * @param nr_pages 计划转移的页面数量。
 */
static void refill_inactive(int nr_pages)
{
	struct list_head * entry;

	spin_lock(&pagemap_lru_lock);
	entry = active_list.prev; // 从链表尾部（最老页面）开始
	while (nr_pages && entry != &active_list) {
		struct page * page;

		page = list_entry(entry, struct page, lru);
		entry = entry->prev; // 提前获取上一个节点，防止 list_del 破坏遍历

		/* 1. 检查引用位：如果最近被访问过，则“续命” */
		if (PageTestandClearReferenced(page)) {
			list_del(&page->lru);
			list_add(&page->lru, &active_list); // 移到头部，重新排队
			continue;
		}

		/* 2. 降级逻辑：页面未被引用，准备移入不活跃链表 */
		nr_pages--;

		del_page_from_active_list(page);
		add_page_to_inactive_list(page);
		
		/* 
		 * 给它在不活跃链表里的“第二次机会”。
		 * 如果在 shrink_cache 扫描到它之前，它又被访问了，
		 * 那么它会被重新激活。
		 */
		SetPageReferenced(page);
	}
	spin_unlock(&pagemap_lru_lock);
}

static int FASTCALL(shrink_caches(zone_t * classzone, int priority, unsigned int gfp_mask, int nr_pages));
/** @brief 综合收缩各类内核缓存
 *
 * 该函数是页面回收过程中的核心步骤，它按顺序尝试从不同类型的缓存中释放内存：
 * 1. Slab 缓存回收：调用 kmem_cache_reap() 释放未使用的 slab 对象。
 * 2. 补充不活跃链表：调用 refill_inactive() 将页面从活跃链表转移到不活跃链表，
 *    以维持活跃/不活跃页面的比例（通常活跃占 2/3）。
 * 3. 页面缓存回收：调用 shrink_cache() 从不活跃链表中真正释放页面。
 * 4. VFS 缓存回收：收缩目录项（dcache）、索引节点（icache）以及配额（dqcache）缓存。
 *
 * @param classzone 目标内存区域。
 * @param priority  回收优先级（影响扫描强度）。
 * @param gfp_mask  分配标志，决定回收时的限制（如是否允许 I/O）。
 * @param nr_pages  期望释放的页面数量。
 * @return int 剩余尚未释放的页面目标数量。如果 <= 0 表示已完成目标。
 */
static int shrink_caches(zone_t * classzone, int priority, unsigned int gfp_mask, int nr_pages)
{
	int chunk_size = nr_pages;
	unsigned long ratio;

	nr_pages -= kmem_cache_reap(gfp_mask);
	if (nr_pages <= 0)
		return 0;

	nr_pages = chunk_size;
	/* try to keep the active list 2/3 of the size of the cache */
	ratio = (unsigned long) nr_pages * nr_active_pages / ((nr_inactive_pages + 1) * 2);
	refill_inactive(ratio);

	nr_pages = shrink_cache(nr_pages, classzone, gfp_mask, priority);
	if (nr_pages <= 0)
		return 0;

	shrink_dcache_memory(priority, gfp_mask);
	shrink_icache_memory(priority, gfp_mask);
#ifdef CONFIG_QUOTA
	shrink_dqcache_memory(DEF_PRIORITY, gfp_mask);
#endif

	return nr_pages;
}

/** @brief Linux内存回收机制最终执行官 (被blance_classzone和kswapd调用)
 *
 * 这是直接页面回收（Direct Reclaim）和 kswapd 回收的核心入口函数：
 * 1. 优先级循环：从默认优先级（DEF_PRIORITY）开始，逐渐增加回收力度（减小 priority 值）。
 * 2. 缓存收缩：在每个优先级水平上调用 shrink_caches()，尝试从页面缓存（Page Cache）和
 *    各种内核缓存（如 slab, dcache, icache）中释放页面。
 * 3. 成功判定：如果 shrink_caches() 成功释放了足够的页面（nr_pages <= 0），则立即返回成功。
 * 4. 最终手段：如果遍历完所有优先级仍无法释放足够内存，则调用 out_of_memory() 触发 OOM Killer，
 *    通过杀死某个进程来释放内存。
 *
 * @param classzone 内存压力最大的首选区域。
 * @param gfp_mask  分配标志，影响回收行为（如是否允许文件 I/O）。
 * @param order     原始请求的分配阶数（在 2.4 中主要用于记录，不直接决定回收数量）。
 * @return int 成功释放页面返回 1，彻底失败（触发 OOM）返回 0。
 * @note 该函数会阻塞当前进程，直到回收完成或触发 OOM。
 */
int try_to_free_pages(zone_t *classzone, unsigned int gfp_mask, unsigned int order)
{
    int priority = DEF_PRIORITY; // 初始优先级（通常是 6）
    int nr_pages = SWAP_CLUSTER_MAX; // 目标回收数量（通常是 32 页）

    gfp_mask = pf_gfp_mask(gfp_mask);
    do {
        // 核心动作：收缩各种缓存（Page Cache, Slab, VFS Caches 等）
        // 优先级越低（数值越小），扫描的力度就越大
        nr_pages = shrink_caches(classzone, priority, gfp_mask, nr_pages);
        
        // 如果 nr_pages 降到 0 或以下，说明已经凑够了 32 页，大功告成
        if (nr_pages <= 0)
            return 1;
    } while (--priority); // 逐渐加大力度，直到 priority 降为 0

	/*
	 * Hmm.. Cache shrink failed - time to kill something?
	 * Mhwahahhaha! This is the part I really like. Giggle.
	 */
	//! 终极手段：触发 OOM Killer
	out_of_memory();
	return 0;
}

DECLARE_WAIT_QUEUE_HEAD(kswapd_wait);

/**
 * @brief 检查内存区域（Zone）及其下属区域是否需要平衡
 * 
 * 该函数从指定的 classzone 开始，向下遍历同一节点内的所有低级 Zone。
 * 如果发现任何一个 Zone 的空闲页数低于 `pages_high` 水位，则认为需要继续平衡。
 * 
 * @param classzone 目标内存区域。
 * @return int 1 表示需要平衡（压力大），0 表示已达标（健康）。
 */
static int check_classzone_need_balance(zone_t * classzone)
{
	zone_t * first_classzone;

	first_classzone = classzone->zone_pgdat->node_zones;
	while (classzone >= first_classzone) {
		/* 
		 * kswapd 的目标是将空闲内存恢复到 pages_high 水位。
		 * 只要有一个 Zone 还没达标，就不能停。
		 */
		if (classzone->free_pages > classzone->pages_high)
			return 0;
		classzone--;
	}
	return 1;
}

/**
 * @brief 对特定内存节点（pgdat）进行平衡处理
 * 
 * 遍历节点内的所有 Zone，对标记了 `need_balance` 的区域调用回收逻辑。
 * 
 * @param pgdat 内存节点描述符。
 * @return int 是否有任何 Zone 仍处于压力状态。
 */
static int kswapd_balance_pgdat(pg_data_t * pgdat)
{
	int need_more_balance = 0, i;
	zone_t * zone;

	/* 从最高级的 Zone 开始处理（如 HighMem -> Normal -> DMA） */
	for (i = pgdat->nr_zones-1; i >= 0; i--) {
		zone = pgdat->node_zones + i;
		if (unlikely(current->need_resched))
			schedule();

		/* 如果该 Zone 没有被标记为需要平衡，直接跳过 */
		if (!zone->need_balance)
			continue;

		/* 
		 * 核心回收动作：尝试释放页面。
		 * GFP_KSWAPD 标志告诉分配器这是后台回收，不要进行可能导致死锁的操作。
		 */
		if (!try_to_free_pages(zone, GFP_KSWAPD, 0)) {
			/* 
			 * 如果努力了还是没法释放页面（可能内存实在太紧），
			 * 暂时放弃该 Zone 的平衡，并强制睡眠 1 秒，避免 CPU 100% 忙转。
			 */
			zone->need_balance = 0;
			__set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(HZ);
			continue;
		}

		/* 
		 * 检查回收后的水位。
		 * 如果依然低于 pages_high，则标记 need_more_balance，准备下一轮迭代。
		 */
		if (check_classzone_need_balance(zone))
			need_more_balance = 1;
		else
			zone->need_balance = 0;
	}

	return need_more_balance;
}

/**
 * @brief kswapd 的平衡调度引擎
 * 
 * 循环遍历系统中所有的内存节点，直到所有节点的 Zone 都达到健康水位。
 */
static void kswapd_balance(void)
{
	int need_more_balance;
	pg_data_t * pgdat;

	do {
		need_more_balance = 0;
		pgdat = pgdat_list;
		do
			need_more_balance |= kswapd_balance_pgdat(pgdat);
		while ((pgdat = pgdat->node_next));
	} while (need_more_balance);
}

/**
 * @brief 判断 kswapd 是否可以进入睡眠
 * 
 * 只有当所有节点的所有 Zone 都不再需要平衡时，kswapd 才能休息。
 */
static int kswapd_can_sleep_pgdat(pg_data_t * pgdat)
{
	zone_t * zone;
	int i;

	for (i = pgdat->nr_zones-1; i >= 0; i--) {
		zone = pgdat->node_zones + i;
		if (!zone->need_balance)
			continue;
		return 0; // 只要有一个 Zone 标记了 need_balance，就不能睡
	}

	return 1;
}

static int kswapd_can_sleep(void)
{
	pg_data_t * pgdat;

	pgdat = pgdat_list;
	do {
		if (kswapd_can_sleep_pgdat(pgdat))
			continue;
		return 0;
	} while ((pgdat = pgdat->node_next));

	return 1;
}

/**
 * @brief kswapd 后台回收守护进程的主函数
 * 
 * kswapd 是一个内核线程，负责在后台异步回收页面，以维持系统有足够的空闲内存。
 * 它通过等待 `kswapd_wait` 队列被唤醒（通常由 `__alloc_pages` 在发现水位过低时触发）。
 */
int kswapd(void *unused)
{
	struct task_struct *tsk = current;
	DECLARE_WAITQUEUE(wait, tsk);

	/* 脱离父进程环境，转为守护进程 */
	daemonize();
	strcpy(tsk->comm, "kswapd");
	sigfillset(&tsk->blocked); // 屏蔽所有信号
	
	/*
	 * 设置 PF_MEMALLOC 标志：
	 * 这告诉内存分配器，kswapd 自身在分配内存时可以无视水位限制。
	 * 理由：kswapd 是为了释放内存而运行的，如果它因为申请一点点管理内存而被阻塞，
	 * 就会导致系统彻底死锁（递归回收）。
	 */
	tsk->flags |= PF_MEMALLOC;

	/*
	 * kswapd 主循环
	 */
	for (;;) {
		__set_current_state(TASK_INTERRUPTIBLE);
		add_wait_queue(&kswapd_wait, &wait);

		mb(); // 内存屏障，确保状态可见性
		if (kswapd_can_sleep())
			/* 
			 * 如果目前没有 Zone 喊“渴”，kswapd 就进入睡眠。
			 * 唤醒点：__alloc_pages() -> wakeup_kswapd()
			 */
			schedule();

		__set_current_state(TASK_RUNNING);
		remove_wait_queue(&kswapd_wait, &wait);

		/* 
		 * 核心任务：平衡内存水位。
		 * 它会不断调用 try_to_free_pages 直到 free_pages > pages_high。
		 */
		kswapd_balance();
		
		/* 刷新磁盘任务队列，确保异步写回的页面能尽快落盘 */
		run_task_queue(&tq_disk);
	}
}

static int __init kswapd_init(void)
{
	printk("Starting kswapd\n");
	swap_setup();
	kernel_thread(kswapd, NULL, CLONE_FS | CLONE_FILES | CLONE_SIGNAL);
	return 0;
}

module_init(kswapd_init)
