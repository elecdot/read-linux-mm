#ifndef _LINUX_MM_H
#define _LINUX_MM_H

#include <linux/sched.h>
#include <linux/errno.h>

#ifdef __KERNEL__

#include <linux/config.h>
#include <linux/string.h>
#include <linux/list.h>
#include <linux/mmzone.h>
#include <linux/swap.h>
#include <linux/rbtree.h>

extern unsigned long max_mapnr;
extern unsigned long num_physpages;
extern void * high_memory;
extern int page_cluster;
/* The inactive_clean lists are per zone. */
extern struct list_head active_list;
extern struct list_head inactive_list;

#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/atomic.h>

/*
 * Linux kernel virtual memory manager primitives.
 * The idea being to have a "virtual" mm in the same way
 * we have a virtual fs - giving a cleaner interface to the
 * mm details, and allowing different kinds of memory mappings
 * (from shared memory to executable loading to arbitrary
 * mmap() functions).
 */

/*
 * This struct defines a memory VMM memory area. There is one of these
 * per VM-area/task.  A VM area is any part of the process virtual memory
 * space that has a special rule for the page-fault handlers (ie a shared
 * library, the executable area etc).
 */
/**
 * @brief 虚拟内存区域 (VMA) 描述符。
 * 
 * 该结构体定义了进程虚拟地址空间中的一个连续区域。每个任务/虚拟内存区域都有一个这样的结构。
 * VMA 是进程虚拟内存空间中具有特殊缺页处理规则的任何部分（例如共享库、可执行代码段等）。
 * 
 * @note 内核使用 Slab 分配器中的 vm_area_cachep 来分配此结构。
 * @ref memory-area-management
 */
struct vm_area_struct {
	struct mm_struct * vm_mm;	//!< 指向该区域所属的地址空间 (mm_struct)。
	unsigned long vm_start;		//!< 该区域在 vm_mm 中的起始虚拟地址。
	unsigned long vm_end;		//!< 该区域在 vm_mm 中的结束虚拟地址（不包含该地址）。

	/* 按照地址排序的每个任务的 VMA 链表 */
	struct vm_area_struct *vm_next; //!< 指向进程 VMA 链表中的下一个区域。

	pgprot_t vm_page_prot;		//!< 该 VMA 的访问权限（页保护位）。
	unsigned long vm_flags;		//!< 标志位，定义了区域的属性（如 VM_READ, VM_WRITE 等）。

	rb_node_t vm_rb;            //!< 用于在红黑树中组织 VMA，以实现快速查找。

	/*
	 * 对于具有地址空间和后备存储的区域，
	 * 属于 address_space->i_mmap{,shared} 链表之一；
	 * 对于共享内存区域，则是附加列表；否则不使用。
	 */
	struct vm_area_struct *vm_next_share;
	struct vm_area_struct **vm_pprev_share;

	/* 处理此结构的操作函数指针 */
	struct vm_operations_struct * vm_ops; //!< 指向该区域特定操作的函数表（如缺页处理）。

	/* 后备存储相关信息： */
	unsigned long vm_pgoff;		//!< 在 vm_file 中的偏移量，以 PAGE_SIZE 为单位。
	struct file * vm_file;		//!< 该区域映射到的文件（如果是匿名映射则为 NULL）。
	unsigned long vm_raend;		//!< 预读结束地址。
	void * vm_private_data;		//!< 私有数据（曾用于共享内存的 vm_pte）。
};

/*
 * vm_flags..
 */
#define VM_READ		0x00000001	/* currently active flags */
#define VM_WRITE	0x00000002
#define VM_EXEC		0x00000004
#define VM_SHARED	0x00000008

#define VM_MAYREAD	0x00000010	/* limits for mprotect() etc */
#define VM_MAYWRITE	0x00000020
#define VM_MAYEXEC	0x00000040
#define VM_MAYSHARE	0x00000080

#define VM_GROWSDOWN	0x00000100	/* general info on the segment */
#define VM_GROWSUP	0x00000200
#define VM_SHM		0x00000400	/* shared memory area, don't swap out */
#define VM_DENYWRITE	0x00000800	/* ETXTBSY on write attempts.. */

#define VM_EXECUTABLE	0x00001000
#define VM_LOCKED	0x00002000
#define VM_IO           0x00004000	/* Memory mapped I/O or similar */

					/* Used by sys_madvise() */
#define VM_SEQ_READ	0x00008000	/* App will access data sequentially */
#define VM_RAND_READ	0x00010000	/* App will not benefit from clustered reads */

#define VM_DONTCOPY	0x00020000      /* Do not copy this vma on fork */
#define VM_DONTEXPAND	0x00040000	/* Cannot expand with mremap() */
#define VM_RESERVED	0x00080000	/* Don't unmap it from swap_out */

#define VM_STACK_FLAGS	0x00000177

#define VM_READHINTMASK			(VM_SEQ_READ | VM_RAND_READ)
#define VM_ClearReadHint(v)		(v)->vm_flags &= ~VM_READHINTMASK
#define VM_NormalReadHint(v)		(!((v)->vm_flags & VM_READHINTMASK))
#define VM_SequentialReadHint(v)	((v)->vm_flags & VM_SEQ_READ)
#define VM_RandomReadHint(v)		((v)->vm_flags & VM_RAND_READ)

/* read ahead limits */
extern int vm_min_readahead;
extern int vm_max_readahead;

/*
 * mapping from the currently active vm_flags protection bits (the
 * low four bits) to a page protection mask..
 */
extern pgprot_t protection_map[16];


/*
 * These are the virtual MM functions - opening of an area, closing and
 * unmapping it (needed to keep files on disk up-to-date etc), pointer
 * to the functions called when a no-page or a wp-page exception occurs. 
 */
struct vm_operations_struct {
	void (*open)(struct vm_area_struct * area);
	void (*close)(struct vm_area_struct * area);
	struct page * (*nopage)(struct vm_area_struct * area, unsigned long address, int unused);
};

/*
 * Each physical page in the system has a struct page associated with
 * it to keep track of whatever it is we are using the page for at the
 * moment. Note that we have no way to track which tasks are using
 * a page.
 *
 * Try to keep the most commonly accessed fields in single cache lines
 * here (16 bytes or greater).  This ordering should be particularly
 * beneficial on 32-bit processors.
 *
 * The first line is data used in page cache lookup, the second line
 * is used for linear searches (eg. clock algorithm scans). 
 *
 * TODO: make this structure smaller, it could be as small as 32 bytes.
 */
/** __DONE__(gzh): 物理内存页框结构体 mem_map_t 的定义
 * @brief Physical memory page frame descriptor.
 * 
 * Each physical page is managed through this structure, which tracks the page's state,
 * ownership, and usage.(See @ref page_flags) The structure is optimized for 
 * page cache lookup and linear searches (e.g., clock algorithm scans).
 * 
 * For instance:
 * 1. it must be able to distinguish the page frames that are used to contain
 * pages that belong to processes from those that contain kernel code or data structures.
 * 2. it must track whether a page is "free". @see ULK P295
 * 
 * @note Each descriptor is 32 bytes long. (the space required by `mem_map` is less than 1%)
 * @note Try to keep the most commonly accessed fields in single cache lines (32 bytes).
 * 
 * @see `include/asm-i386/page.h`
 * @code
 * virt_to_page(kaddr)
 * // this macro yields the addressof the page descriptor associated 
 * // with the linear address addr.
 * @endcode
 * 
 * @see `pfn_to_page(pfn)` macro yields the address of the page descriptor
 * associated with the page frame having number pfn.
 */
typedef struct page {
	//! @ref page_flags 中定义的各种状态标志位.
	unsigned long flags;		/* atomic flags, some possibly
					   updated asynchronously */
	/** 
	 * @brief This points to the list of pages in the same `mapping` (inode):
	 * @code
	 * page ──→ [mapping] ──→ address_space (in inode) ─────┐
	 *   └──→ [list] ──→ page list (clean/dirty/locked) ←───┘
	 * @endcode
	 * list_head contains the doubly linked list pointers to link this page
	 * into the inode's page lists (clean_pages, dirty_pages, locked_pages).
	 * @note 参考书里的`prev`和`next`字段应该是在这里被封装进 list_head 结构体里的.
	 * @note If the page is free (page->count == 0), this list is used for
	 * the free list management.
	 */
	struct list_head list;		/* ->mapping has some page lists. */
	/** 从page链接到它所属的address_space(通常在指向定义在 @ref inode 里对应的address_space).
	 * 表示一个文件在内存中所有的页框集合空间.
	 */
	struct address_space *mapping;	/* The inode (or ...) we belong to. */
	//! 在mapping中的偏移(单位: @code PAGE_CACHE_SIZE @endcode).
	unsigned long index;		/* Our offset within mapping. */
	//! @ref hash_buckets 中的下一个页框.
	struct page *next_hash;		/* Next page sharing our hash bucket in
					   the pagecache hash table. */
	//! 该页框被某些进程或内核引用的计数. (0--空闲)
	atomic_t count;			/* Usage count, see below. */
	//! LRU 算法链表,表示该页框在活跃/非活跃链表中的位置. 由 pagemap_lru_lock 维护.
	struct list_head lru;		/* Pageout list, eg. active_list;
					   protected by pagemap_lru_lock !! */
	//! Wait queue for tasks waiting on page I/O or lock operations.
	wait_queue_head_t wait;		/* Page locked?  Stand in line... */
	//! @ref hash_buckets 中该页框的前一个页框指针的地址(用于O(1)删除).
	struct page **pprev_hash;	/* Complement to *next_hash. */
	//! (如果该页为缓冲区)指向该页框对应的缓冲区头结构体链表.
	struct buffer_head * buffers;	/* Buffer maps us to a disk block. */
	//! 内存页框的内核虚拟地址(如果该页为高端内存则为NULL).
	void *virtual;			/* Kernel virtual address (NULL if
					   not kmapped, ie. highmem) */
	/**	 指向该页框所属的内存区域(zone).
	 * @note Linux 2.4 引入, 支持 zone-based buddy allocator.
	 * @note page→zone→node: 页通过 zone 间接访问节点.
	 */
	struct zone_struct *zone;	/* Memory zone we are in. */
} mem_map_t;

/*
 * Methods to modify the page usage count.
 *
 * What counts for a page usage:
 * - cache mapping   (page->mapping)
 * - disk mapping    (page->buffers)
 * - page mapped in a task's page tables, each mapping
 *   is counted separately
 *
 * Also, many kernel routines increase the page count before a critical
 * routine so they can be sure the page doesn't go away from under them.
 */
/**
 * It seems the `get_page()` increase the count,
 * but the `page_count()` returns the current count without incrementing.
 * Moreover, `put_page()` decrease the count.
 * `put_page_testzero()` will decrease the count and return true if the count reaches zero.
 * (test whether you are not the last user of the page after decrementing the count)
 */
#define get_page(p)		atomic_inc(&(p)->count) //! returns the current reference count of the page.
#define put_page(p)		__free_page(p)
#define put_page_testzero(p) 	atomic_dec_and_test(&(p)->count)
#define page_count(p)		atomic_read(&(p)->count)
#define set_page_count(p,v) 	atomic_set(&(p)->count, v)

/*
 * Various page->flags bits:
 *
 * PG_reserved is set for special pages, which can never be swapped
 * out. Some of them might not even exist (eg empty_bad_page)...
 *
 * Multiple processes may "see" the same page. E.g. for untouched
 * mappings of /dev/null, all processes see the same page full of
 * zeroes, and text pages of executables and shared libraries have
 * only one copy in memory, at most, normally.
 *
 * For the non-reserved pages, page->count denotes a reference count.
 *   page->count == 0 means the page is free.
 *   page->count == 1 means the page is used for exactly one purpose
 *   (e.g. a private data page of one process).
 *
 * A page may be used for kmalloc() or anyone else who does a
 * __get_free_page(). In this case the page->count is at least 1, and
 * all other fields are unused but should be 0 or NULL. The
 * management of this page is the responsibility of the one who uses
 * it.
 *
 * The other pages (we may call them "process pages") are completely
 * managed by the Linux memory manager: I/O, buffers, swapping etc.
 * The following discussion applies only to them.
 *
 * A page may belong to an inode's memory mapping. In this case,
 * page->mapping is the pointer to the inode, and page->index is the
 * file offset of the page, in units of PAGE_CACHE_SIZE.
 *
 * A page may have buffers allocated to it. In this case,
 * page->buffers is a circular list of these buffer heads. Else,
 * page->buffers == NULL.
 *
 * For pages belonging to inodes, the page->count is the number of
 * attaches, plus 1 if buffers are allocated to the page, plus one
 * for the page cache itself.
 *
 * All pages belonging to an inode are in these doubly linked lists:
 * mapping->clean_pages, mapping->dirty_pages and mapping->locked_pages;
 * using the page->list list_head. These fields are also used for
 * freelist managemet (when page->count==0).
 *
 * There is also a hash table mapping (mapping,index) to the page
 * in memory if present. The lists for this hash table use the fields
 * page->next_hash and page->pprev_hash.
 *
 * All process pages can do I/O:
 * - inode pages may need to be read from disk,
 * - inode pages which have been modified and are MAP_SHARED may need
 *   to be written to disk,
 * - private pages which have been modified may need to be swapped out
 *   to swap space and (later) to be read back into memory.
 * During disk I/O, PG_locked is used. This bit is set before I/O
 * and reset when I/O completes. page->wait is a wait queue of all
 * tasks waiting for the I/O on this page to complete.
 * PG_uptodate tells whether the page's contents is valid.
 * When a read completes, the page becomes uptodate, unless a disk I/O
 * error happened.
 *
 * For choosing which pages to swap out, inode pages carry a
 * PG_referenced bit, which is set any time the system accesses
 * that page through the (mapping,index) hash table. This referenced
 * bit, together with the referenced bit in the page tables, is used
 * to manipulate page->age and move the page across the active,
 * inactive_dirty and inactive_clean lists.
 *
 * Note that the referenced bit, the page->lru list_head and the
 * active, inactive_dirty and inactive_clean lists are protected by
 * the pagemap_lru_lock, and *NOT* by the usual PG_locked bit!
 *
 * PG_skip is used on sparc/sparc64 architectures to "skip" certain
 * parts of the address space.
 *
 * PG_error is set to indicate that an I/O error occurred on this page.
 *
 * PG_arch_1 is an architecture specific page state bit.  The generic
 * code guarantees that this bit is cleared for a page when it first
 * is entered into the page cache.
 *
 * PG_highmem pages are not permanently mapped into the kernel virtual
 * address space, they need to be kmapped separately for doing IO on
 * the pages. The struct page (these bits with information) are always
 * mapped into kernel address space...
 */
#define PG_locked		 0	/* Page is locked. Don't touch. */
#define PG_error		 1
#define PG_referenced		 2
#define PG_uptodate		 3
#define PG_dirty		 4
#define PG_unused		 5
#define PG_lru			 6
#define PG_active		 7
#define PG_slab			 8
#define PG_skip			10
#define PG_highmem		11
#define PG_checked		12	/* kill me in 2.5.<early>. */
#define PG_arch_1		13
#define PG_reserved		14
#define PG_launder		15	/* written out by VM pressure.. */

/* Make it prettier to test the above... */
#define UnlockPage(page)	unlock_page(page)
#define Page_Uptodate(page)	test_bit(PG_uptodate, &(page)->flags)
#define SetPageUptodate(page)	set_bit(PG_uptodate, &(page)->flags)
#define ClearPageUptodate(page)	clear_bit(PG_uptodate, &(page)->flags)
#define PageDirty(page)		test_bit(PG_dirty, &(page)->flags)
#define SetPageDirty(page)	set_bit(PG_dirty, &(page)->flags)
#define ClearPageDirty(page)	clear_bit(PG_dirty, &(page)->flags)
#define PageLocked(page)	test_bit(PG_locked, &(page)->flags)
#define LockPage(page)		set_bit(PG_locked, &(page)->flags)
#define TryLockPage(page)	test_and_set_bit(PG_locked, &(page)->flags)
#define PageChecked(page)	test_bit(PG_checked, &(page)->flags)
#define SetPageChecked(page)	set_bit(PG_checked, &(page)->flags)
#define PageLaunder(page)	test_bit(PG_launder, &(page)->flags)
#define SetPageLaunder(page)	set_bit(PG_launder, &(page)->flags)

extern void FASTCALL(set_page_dirty(struct page *));

/*
 * The first mb is necessary to safely close the critical section opened by the
 * TryLockPage(), the second mb is necessary to enforce ordering between
 * the clear_bit and the read of the waitqueue (to avoid SMP races with a
 * parallel wait_on_page).
 */
#define PageError(page)		test_bit(PG_error, &(page)->flags)
#define SetPageError(page)	set_bit(PG_error, &(page)->flags)
#define ClearPageError(page)	clear_bit(PG_error, &(page)->flags)
#define PageReferenced(page)	test_bit(PG_referenced, &(page)->flags)
#define SetPageReferenced(page)	set_bit(PG_referenced, &(page)->flags)
#define ClearPageReferenced(page)	clear_bit(PG_referenced, &(page)->flags)
#define PageTestandClearReferenced(page)	test_and_clear_bit(PG_referenced, &(page)->flags)
#define PageSlab(page)		test_bit(PG_slab, &(page)->flags)
#define PageSetSlab(page)	set_bit(PG_slab, &(page)->flags)
#define PageClearSlab(page)	clear_bit(PG_slab, &(page)->flags)
#define PageReserved(page)	test_bit(PG_reserved, &(page)->flags)

#define PageActive(page)	test_bit(PG_active, &(page)->flags)
#define SetPageActive(page)	set_bit(PG_active, &(page)->flags)
#define ClearPageActive(page)	clear_bit(PG_active, &(page)->flags)

#define PageLRU(page)		test_bit(PG_lru, &(page)->flags)
#define TestSetPageLRU(page)	test_and_set_bit(PG_lru, &(page)->flags)
#define TestClearPageLRU(page)	test_and_clear_bit(PG_lru, &(page)->flags)

#ifdef CONFIG_HIGHMEM
#define PageHighMem(page)		test_bit(PG_highmem, &(page)->flags)
#else
#define PageHighMem(page)		0 /* needed to optimize away at compile time */
#endif

#define SetPageReserved(page)		set_bit(PG_reserved, &(page)->flags)
#define ClearPageReserved(page)		clear_bit(PG_reserved, &(page)->flags)

/*
 * Error return values for the *_nopage functions
 */
#define NOPAGE_SIGBUS	(NULL)
#define NOPAGE_OOM	((struct page *) (-1))

/* The array of struct pages */
/** __EXTERN__: 所有物理内存页框的page结构都保存在数组mem_map[]中 @see `free_area_init()` */
extern mem_map_t * mem_map;

/*
 * There is only one page-allocator function, and two main namespaces to
 * it. The alloc_page*() variants return 'struct page *' and as such
 * can allocate highmem pages, the *get*page*() variants return
 * virtual kernel addresses to the allocated page(s).
 */
extern struct page * FASTCALL(_alloc_pages(unsigned int gfp_mask, unsigned int order));
extern struct page * FASTCALL(__alloc_pages(unsigned int gfp_mask, unsigned int order, zonelist_t *zonelist));
extern struct page * alloc_pages_node(int nid, unsigned int gfp_mask, unsigned int order);

/**
 * @brief Allocate 2^order contiguous pages and return a struct page pointer (or NULL).
 * @param gfp_mask Allocation flags (GFP_*)
 * @param order Power of two pages to allocate
 * @return struct page* Pointer to the first page structure, or NULL on failure
 */
static inline struct page * alloc_pages(unsigned int gfp_mask, unsigned int order)
{
	/*
	 * Gets optimized away by the compiler.
	 */
	if (order >= MAX_ORDER)
		return NULL;
	return _alloc_pages(gfp_mask, order);
}

/**
 * @brief Allocate a single page and return a struct page pointer.
 * @param gfp_mask Allocation flags
 * @return struct page* Pointer to the page structure, or NULL on failure
 * @note Simply a wrapper around alloc_pages() with order 0.
 */
#define alloc_page(gfp_mask) alloc_pages(gfp_mask, 0)

extern unsigned long FASTCALL(__get_free_pages(unsigned int gfp_mask, unsigned int order));
extern unsigned long FASTCALL(get_zeroed_page(unsigned int gfp_mask));

/**
 * @brief Allocate a single page and return its virtual address.
 * @param gfp_mask Allocation flags
 * @return unsigned long Virtual address of the allocated page, or 0 on failure
 * @note Simply a wrapper around __get_free_pages() with order 0.
 */
#define __get_free_page(gfp_mask) \
		__get_free_pages((gfp_mask),0)

/**
 * @brief Allocate 2^order pages from the DMA zone.
 * @param gfp_mask Allocation flags
 * @param order Power of two pages to allocate
 * @return unsigned long Virtual address of the allocated area, or 0 on failure
 */
#define __get_dma_pages(gfp_mask, order) \
		__get_free_pages((gfp_mask) | GFP_DMA,(order))

/*
 * The old interface name will be removed in 2.5:
 */
#define get_free_page get_zeroed_page

/*
 * There is only one 'core' page-freeing function.
 */
/**
 * @brief 这里使用FASTCALL调用约定以提高性能，同时保证同一调用序列。
 */
extern void FASTCALL(__free_pages(struct page *page, unsigned int order));

/**
 * @brief 这里使用FASTCALL调用约定以提高性能，同时保证同一调用序列。
 */
extern void FASTCALL(free_pages(unsigned long addr, unsigned int order));

/**
 * @brief Release a single page given its struct page pointer.
 * @param page Pointer to the page structure to release
 * @note Wrapper around __free_pages(page, 0)
 * @note Simply a wrapper around __free_pages() with order 0.
 */
#define __free_page(page) __free_pages((page), 0)

/**
 * @brief Release a single page given its virtual address.
 * @param addr Virtual address of the page to release
 * @note Wrapper around free_pages(addr, 0)
 * @note Simply a wrapper around free_pages() with order 0.
 */
#define free_page(addr) free_pages((addr),0)

extern void show_free_areas(void);
extern void show_free_areas_node(pg_data_t *pgdat);

extern void clear_page_tables(struct mm_struct *, unsigned long, int);

extern int fail_writepage(struct page *);
struct page * shmem_nopage(struct vm_area_struct * vma, unsigned long address, int unused);
struct file *shmem_file_setup(char * name, loff_t size);
extern void shmem_lock(struct file * file, int lock);
extern int shmem_zero_setup(struct vm_area_struct *);

extern void zap_page_range(struct mm_struct *mm, unsigned long address, unsigned long size);
extern int copy_page_range(struct mm_struct *dst, struct mm_struct *src, struct vm_area_struct *vma);
extern int remap_page_range(unsigned long from, unsigned long to, unsigned long size, pgprot_t prot);
extern int zeromap_page_range(unsigned long from, unsigned long size, pgprot_t prot);

extern int vmtruncate(struct inode * inode, loff_t offset);
extern pmd_t *FASTCALL(__pmd_alloc(struct mm_struct *mm, pgd_t *pgd, unsigned long address));
extern pte_t *FASTCALL(pte_alloc(struct mm_struct *mm, pmd_t *pmd, unsigned long address));
extern int handle_mm_fault(struct mm_struct *mm,struct vm_area_struct *vma, unsigned long address, int write_access);
extern int make_pages_present(unsigned long addr, unsigned long end);
extern int access_process_vm(struct task_struct *tsk, unsigned long addr, void *buf, int len, int write);
extern int ptrace_readdata(struct task_struct *tsk, unsigned long src, char *dst, int len);
extern int ptrace_writedata(struct task_struct *tsk, char * src, unsigned long dst, int len);
extern int ptrace_attach(struct task_struct *tsk);
extern int ptrace_detach(struct task_struct *, unsigned int);
extern void ptrace_disable(struct task_struct *);
extern int ptrace_check_attach(struct task_struct *task, int kill);

int get_user_pages(struct task_struct *tsk, struct mm_struct *mm, unsigned long start,
		int len, int write, int force, struct page **pages, struct vm_area_struct **vmas);

/*
 * On a two-level page table, this ends up being trivial. Thus the
 * inlining and the symmetry break with pte_alloc() that does all
 * of this out-of-line.
 */
static inline pmd_t *pmd_alloc(struct mm_struct *mm, pgd_t *pgd, unsigned long address)
{
	if (pgd_none(*pgd))
		return __pmd_alloc(mm, pgd, address);
	return pmd_offset(pgd, address);
}

extern int pgt_cache_water[2];
extern int check_pgt_cache(void);

extern void free_area_init(unsigned long * zones_size);
extern void free_area_init_node(int nid, pg_data_t *pgdat, struct page *pmap,
	unsigned long * zones_size, unsigned long zone_start_paddr, 
	unsigned long *zholes_size);
extern void mem_init(void);
extern void show_mem(void);
extern void si_meminfo(struct sysinfo * val);
extern void swapin_readahead(swp_entry_t);

extern struct address_space swapper_space;
#define PageSwapCache(page) ((page)->mapping == &swapper_space)

static inline int is_page_cache_freeable(struct page * page)
{
	return page_count(page) - !!page->buffers == 1;
}

extern int can_share_swap_page(struct page *);
extern int remove_exclusive_swap_page(struct page *);

extern void __free_pte(pte_t);

/* mmap.c */
extern void lock_vma_mappings(struct vm_area_struct *);
extern void unlock_vma_mappings(struct vm_area_struct *);
extern void insert_vm_struct(struct mm_struct *, struct vm_area_struct *);
extern void __insert_vm_struct(struct mm_struct *, struct vm_area_struct *);
extern void build_mmap_rb(struct mm_struct *);
extern void exit_mmap(struct mm_struct *);

extern unsigned long get_unmapped_area(struct file *, unsigned long, unsigned long, unsigned long, unsigned long);

extern unsigned long do_mmap_pgoff(struct file *file, unsigned long addr,
	unsigned long len, unsigned long prot,
	unsigned long flag, unsigned long pgoff);

/** @brief do_mmap_pgoff 的内联封装版本
 *
 * 这是内核中创建内存映射的最常用入口。它对偏移量进行基本的溢出检查和对齐检查，
 * 然后将字节偏移量转换为页面偏移量，并调用核心实现函数 do_mmap_pgoff。
 *
 * @param file   指向被映射文件的指针（匿名映射为 NULL）。
 * @param addr   请求映射的起始虚拟地址。
 * @param len    映射区域的长度。
 * @param prot   页保护标志。
 * @param flag   映射标志。
 * @param offset 文件内的字节偏移量。
 * @return unsigned long 返回映射成功的起始虚拟地址，失败则返回错误码。
 * @see do_mmap_pgoff
 */
static inline unsigned long do_mmap(struct file *file, unsigned long addr,
	unsigned long len, unsigned long prot,
	unsigned long flag, unsigned long offset)
{
	// Basic overflow and alignment checks
	unsigned long ret = -EINVAL;
	if ((offset + PAGE_ALIGN(len)) < offset)
		goto out;
	if (!(offset & ~PAGE_MASK))
		ret = do_mmap_pgoff(file, addr, len, prot, flag, offset >> PAGE_SHIFT);
out:
	return ret;
}

extern int do_munmap(struct mm_struct *, unsigned long, size_t);

extern unsigned long do_brk(unsigned long, unsigned long);

static inline void __vma_unlink(struct mm_struct * mm, struct vm_area_struct * vma, struct vm_area_struct * prev)
{
	prev->vm_next = vma->vm_next;
	rb_erase(&vma->vm_rb, &mm->mm_rb);
	if (mm->mmap_cache == vma)
		mm->mmap_cache = prev;
}

static inline int can_vma_merge(struct vm_area_struct * vma, unsigned long vm_flags)
{
	if (!vma->vm_file && vma->vm_flags == vm_flags)
		return 1;
	else
		return 0;
}

struct zone_t;
/* filemap.c */
extern void remove_inode_page(struct page *);
extern unsigned long page_unuse(struct page *);
extern void truncate_inode_pages(struct address_space *, loff_t);

/* generic vm_area_ops exported for stackable file systems */
extern int filemap_sync(struct vm_area_struct *, unsigned long,	size_t, unsigned int);
extern struct page *filemap_nopage(struct vm_area_struct *, unsigned long, int);

/*
 * GFP bitmasks..
 */
/* Zone modifiers in GFP_ZONEMASK (see linux/mmzone.h - low four bits) */
/**
 * @brief "zone modifiers" for page allocation
 * 
 * @ref zone-selection-gfp
 */
#define __GFP_DMA	0x01
#define __GFP_HIGHMEM	0x02

/* Action modifiers - doesn't change the zoning */
#define __GFP_WAIT	0x10	/* Can wait and reschedule? */
#define __GFP_HIGH	0x20	/* Should access emergency pools? */
#define __GFP_IO	0x40	/* Can start low memory physical IO? */
#define __GFP_HIGHIO	0x80	/* Can start high mem physical IO? */
#define __GFP_FS	0x100	/* Can call down to low-level FS? */

#define GFP_NOHIGHIO	(__GFP_HIGH | __GFP_WAIT | __GFP_IO)
#define GFP_NOIO	(__GFP_HIGH | __GFP_WAIT)
#define GFP_NOFS	(__GFP_HIGH | __GFP_WAIT | __GFP_IO | __GFP_HIGHIO)
/** 
 * An atomic request never blocks, they simply fails if memory is not immediately available
 * @note Used in some kernel control paths cannot be blocked while requesting memorys,
 * interrupt handlers, bottom halves, tasklets, etc.
 */
#define GFP_ATOMIC	(__GFP_HIGH)
#define GFP_USER	(             __GFP_WAIT | __GFP_IO | __GFP_HIGHIO | __GFP_FS)
#define GFP_HIGHUSER	(             __GFP_WAIT | __GFP_IO | __GFP_HIGHIO | __GFP_FS | __GFP_HIGHMEM)
#define GFP_KERNEL	(__GFP_HIGH | __GFP_WAIT | __GFP_IO | __GFP_HIGHIO | __GFP_FS)
#define GFP_NFS		(__GFP_HIGH | __GFP_WAIT | __GFP_IO | __GFP_HIGHIO | __GFP_FS)
#define GFP_KSWAPD	(             __GFP_WAIT | __GFP_IO | __GFP_HIGHIO | __GFP_FS)

/* Flag - indicates that the buffer will be suitable for DMA.  Ignored on some
   platforms, used as appropriate on others */

#define GFP_DMA		__GFP_DMA

/** @brief 根据当前进程上下文调整分配标志, 规范化手段。
 *
 * 该函数检查当前进程的标志位，并根据需要屏蔽掉某些分配行为：
 * - 如果进程设置了 PF_NOIO 标志（例如在块设备驱动或文件系统 I/O 路径中），
 *   则必须禁止在分配/回收过程中再次触发 I/O 或文件系统操作，以防止死锁。
 *
 * @param gfp_mask 原始分配标志。
 * @return unsigned int 调整后的分配标志。
 */
static inline unsigned int pf_gfp_mask(unsigned int gfp_mask)
{
	/* avoid all memory balancing I/O methods if this task cannot block on I/O */
	if (current->flags & PF_NOIO)
		gfp_mask &= ~(__GFP_IO | __GFP_HIGHIO | __GFP_FS);

	return gfp_mask;
}
	
/* vma is the first one with  address < vma->vm_end,
 * and even  address < vma->vm_start. Have to extend vma. */
static inline int expand_stack(struct vm_area_struct * vma, unsigned long address)
{
	unsigned long grow;

	/*
	 * vma->vm_start/vm_end cannot change under us because the caller is required
	 * to hold the mmap_sem in write mode. We need to get the spinlock only
	 * before relocating the vma range ourself.
	 */
	address &= PAGE_MASK;
 	spin_lock(&vma->vm_mm->page_table_lock);
	grow = (vma->vm_start - address) >> PAGE_SHIFT;
	if (vma->vm_end - address > current->rlim[RLIMIT_STACK].rlim_cur ||
	    ((vma->vm_mm->total_vm + grow) << PAGE_SHIFT) > current->rlim[RLIMIT_AS].rlim_cur) {
		spin_unlock(&vma->vm_mm->page_table_lock);
		return -ENOMEM;
	}
	vma->vm_start = address;
	vma->vm_pgoff -= grow;
	vma->vm_mm->total_vm += grow;
	if (vma->vm_flags & VM_LOCKED)
		vma->vm_mm->locked_vm += grow;
	spin_unlock(&vma->vm_mm->page_table_lock);
	return 0;
}

/* Look up the first VMA which satisfies  addr < vm_end,  NULL if none. */
extern struct vm_area_struct * find_vma(struct mm_struct * mm, unsigned long addr);
extern struct vm_area_struct * find_vma_prev(struct mm_struct * mm, unsigned long addr,
					     struct vm_area_struct **pprev);

/* Look up the first VMA which intersects the interval start_addr..end_addr-1,
   NULL if none.  Assume start_addr < end_addr. */
static inline struct vm_area_struct * find_vma_intersection(struct mm_struct * mm, unsigned long start_addr, unsigned long end_addr)
{
	struct vm_area_struct * vma = find_vma(mm,start_addr);

	if (vma && end_addr <= vma->vm_start)
		vma = NULL;
	return vma;
}

extern struct vm_area_struct *find_extend_vma(struct mm_struct *mm, unsigned long addr);

#endif /* __KERNEL__ */

#endif
