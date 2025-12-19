/*
 * linux/mm/slab.c
 * Written by Mark Hemment, 1996/97.
 * (markhe@nextd.demon.co.uk)
 *
 * kmem_cache_destroy() + some cleanup - 1999 Andrea Arcangeli
 *
 * Major cleanup, different bufctl logic, per-cpu arrays
 *	(c) 2000 Manfred Spraul
 *
 * An implementation of the Slab Allocator as described in outline in;
 *	UNIX Internals: The New Frontiers by Uresh Vahalia
 *	Pub: Prentice Hall	ISBN 0-13-101908-2
 * or with a little more detail in;
 *	The Slab Allocator: An Object-Caching Kernel Memory Allocator
 *	Jeff Bonwick (Sun Microsystems).
 *	Presented at: USENIX Summer 1994 Technical Conference
 *
 *
 * The memory is organized in caches, one cache for each object type.
 * (e.g. inode_cache, dentry_cache, buffer_head, vm_area_struct)
 * Each cache consists out of many slabs (they are small (usually one
 * page long) and always contiguous), and each slab contains multiple
 * initialized objects.
 *
 * Each cache can only support one memory type (GFP_DMA, GFP_HIGHMEM,
 * normal). If you need a special memory type, then must create a new
 * cache for that memory type.
 *
 * In order to reduce fragmentation, the slabs are sorted in 3 groups:
 *   full slabs with 0 free objects
 *   partial slabs
 *   empty slabs with no allocated objects
 *
 * If partial slabs exist, then new allocations come from these slabs,
 * otherwise from empty slabs or new slabs are allocated.
 *
 * kmem_cache_destroy() CAN CRASH if you try to allocate from the cache
 * during kmem_cache_destroy(). The caller must prevent concurrent allocs.
 *
 * On SMP systems, each cache has a short per-cpu head array, most allocs
 * and frees go into that array, and if that array overflows, then 1/2
 * of the entries in the array are given back into the global cache.
 * This reduces the number of spinlock operations.
 *
 * The c_cpuarray may not be read with enabled local interrupts.
 *
 * SMP synchronization:
 *  constructors and destructors are called without any locking.
 *  Several members in kmem_cache_t and slab_t never change, they
 *	are accessed without any locking.
 *  The per-cpu arrays are never accessed from the wrong cpu, no locking.
 *  The non-constant members are protected with a per-cache irq spinlock.
 *
 * Further notes from the original documentation:
 *
 * 11 April '97.  Started multi-threading - markhe
 *	The global cache-chain is protected by the semaphore 'cache_chain_sem'.
 *	The sem is only needed when accessing/extending the cache-chain, which
 *	can never happen inside an interrupt (kmem_cache_create(),
 *	kmem_cache_shrink() and kmem_cache_reap()).
 *
 *	To prevent kmem_cache_shrink() trying to shrink a 'growing' cache (which
 *	maybe be sleeping and therefore not holding the semaphore/lock), the
 *	growing field is used.  This also prevents reaping from a cache.
 *
 *	At present, each engine can be growing a cache.  This should be blocked.
 *
 */

#include	<linux/config.h>
#include	<linux/slab.h>
#include	<linux/interrupt.h>
#include	<linux/init.h>
#include	<linux/compiler.h>
#include	<asm/uaccess.h>

/*
 * DEBUG	- 1 for kmem_cache_create() to honour; SLAB_DEBUG_INITIAL,
 *		  SLAB_RED_ZONE & SLAB_POISON.
 *		  0 for faster, smaller code (especially in the critical paths).
 *
 * STATS	- 1 to collect stats for /proc/slabinfo.
 *		  0 for faster, smaller code (especially in the critical paths).
 *
 * FORCED_DEBUG	- 1 enables SLAB_RED_ZONE and SLAB_POISON (if possible)
 */

#ifdef CONFIG_DEBUG_SLAB
#define	DEBUG		1
#define	STATS		1
#define	FORCED_DEBUG	1
#else
#define	DEBUG		0
#define	STATS		0
#define	FORCED_DEBUG	0
#endif

/*
 * Parameters for kmem_cache_reap
 */
#define REAP_SCANLEN	10
#define REAP_PERFECT	10

/* Shouldn't this be in a header file somewhere? */
#define	BYTES_PER_WORD		sizeof(void *)

/* Legal flag mask for kmem_cache_create(). */
#if DEBUG
# define CREATE_MASK	(SLAB_DEBUG_INITIAL | SLAB_RED_ZONE | \
			 SLAB_POISON | SLAB_HWCACHE_ALIGN | \
			 SLAB_NO_REAP | SLAB_CACHE_DMA | \
			 SLAB_MUST_HWCACHE_ALIGN)
#else
# define CREATE_MASK	(SLAB_HWCACHE_ALIGN | SLAB_NO_REAP | \
			 SLAB_CACHE_DMA | SLAB_MUST_HWCACHE_ALIGN)
#endif

/*
 * kmem_bufctl_t:
 *
 * Bufctl's are used for linking objs within a slab
 * linked offsets.
 *
 * This implementaion relies on "struct page" for locating the cache &
 * slab an object belongs to.
 * This allows the bufctl structure to be small (one int), but limits
 * the number of objects a slab (not a cache) can contain when off-slab
 * bufctls are used. The limit is the size of the largest general cache
 * that does not use off-slab slabs.
 * For 32bit archs with 4 kB pages, is this 56.
 * This is not serious, as it is only for large objects, when it is unwise
 * to have too many per slab.
 * Note: This limit can be raised by introducing a general cache whose size
 * is less than 512 (PAGE_SIZE<<3), but greater than 256.
 */

#define BUFCTL_END 0xffffFFFF
#define	SLAB_LIMIT 0xffffFFFE
typedef unsigned int kmem_bufctl_t;

/* Max number of objs-per-slab for caches which use off-slab slabs.
 * Needed to avoid a possible looping condition in kmem_cache_grow().
 */
static unsigned long offslab_limit;

/*
 * slab_t
 *
 * Manages the objs in a slab. Placed either at the beginning of mem allocated
 * for a slab, or allocated from an general cache.
 * Slabs are chained into three list: fully used, partial, fully free slabs.
 */
/**
 * @brief Slab 描述符 (slab_t)
 * 
 * 该结构体负责管理一个具体的 Slab（由 2^gfporder 个连续页框组成）。
 * 它记录了该 Slab 的内存布局、在用对象统计以及空闲对象的管理链表。
 * 
 * **内存布局**：
 * - 对象存储区(s_mem)：紧跟在 slab_t 结构体之后，是实际存放对象的内存区域。
 * - bufctl 数组：紧跟在对象存储区(inuse)之后，是一个“数组模拟链表”，用于管理空闲对象。
 * 
 * 通过挂载到 kmem_cache_t 的 slabs_full、slabs_partial 和 slabs_free 链表，
 * Slab 分配器能够高效地跟踪和管理不同分配状态的 Slab，从而优化内存利用率和分配性能。
 */
typedef struct slab_s {
	struct list_head	list;       //!< 链表节点：对应挂载到 cachep 的 slabs_full/partial/free 链表之一。
	unsigned long		colouroff;  //!< 该 Slab 的着色偏移量：即 s_mem 相对于页面起始地址的偏移。
	void			*s_mem;		    //!< 指向该 Slab 中第一个对象的起始地址（已包含着色偏移）。
	unsigned int		inuse;		//!< 统计：当前该 Slab 中已分配（在用）的对象数量。
	//! 就是一个指针数组(静态链表), 代替在对象内部插入指针作为链表使用 @see `slab_bufctl()`
	kmem_bufctl_t		free;       //!< 空闲链表头：存放第一个空闲对象的索引（bufctl 数组的下标）。
} slab_t;

/**
 * @brief 获取 Slab 的 bufctl 数组起始地址 (空闲对象链表头)
 * 
 * 设计逻辑：
 * bufctl 数组紧跟在 slab_t 结构体之后。它是一个“数组模拟链表”的结构：
 * 数组的第 i 个元素存放的是第 i 个对象之后的下一个空闲对象的索引。
 * 这种设计避免了在对象内部插入指针，从而支持任何大小的对象且不破坏对齐。
 */
#define slab_bufctl(slabp) \
	((kmem_bufctl_t *)(((slab_t*)slabp)+1))

/*
 * cpucache_t
 *
 * Per cpu structures
 * The limit is stored in the per-cpu structure to reduce the data cache
 * footprint.
 */
/**
 * @brief Per-CPU 对象缓存结构
 * 
 * 存储在每个 CPU 的私有数据中，用于实现分配/释放的快速路径。
 * limit 存储在这里可以减少数据缓存占用。
 */
typedef struct cpucache_s {
	unsigned int avail; /**< 当前缓存中可用的对象数量 */
	unsigned int limit; /**< 缓存允许存储的最大对象数量 */
} cpucache_t;

/**
 * @brief 获取 Per-CPU 缓存中的对象指针数组起始地址
 * 
 * 对象指针紧跟在 cpucache_t 结构体之后。
 */
#define cc_entry(cpucache) \
	((void **)(((cpucache_t*)(cpucache))+1))

/**
 * @brief 获取当前 CPU 对应的 Per-CPU 缓存数据
 */
#define cc_data(cachep) \
	((cachep)->cpudata[smp_processor_id()])
/*
 * kmem_cache_t
 *
 * manages a cache.
 */

#define CACHE_NAMELEN	20	/* max name length for a slab cache */

/**
 * @brief Cache 描述符 (kmem_cache_t)
 * 
 * 该结构体是 Slab 分配器的核心管理单元。每种对象类型（如 inode, task_struct）
 * 都有一个独立的 kmem_cache_t 实例。
 * 
 * **Slab 管理机制**：
 * Cache 通过三个双向循环链表来组织和管理其下的所有 Slab：
 * 1. slabs_partial：部分分配的 Slab。这是分配时的首选，因为它既能满足需求又能提高内存利用率。
 * 2. slabs_free：完全空闲的 Slab。当 partial 链表为空时，从此链表取出一个 Slab 使用。
 * 3. slabs_full：已完全填满的 Slab。分配器会跳过这些 Slab，直到其中有对象被释放。
 * 
 * **状态流转**：
 * - 分配对象时：优先从 partial 找；若无，则从 free 找并移入 partial；若仍无，则调用 kmem_cache_grow 申请新页。
 * - 释放对象时：若 Slab 从 full 变为有空闲，移入 partial；若 Slab 变为全空，移入 free（或根据策略直接释放回伙伴系统）。
 */
struct kmem_cache_s {
/* 1) 每次分配与释放相关的字段 (Hot Path) */
    /* Cache维护的三个Slab链表，用于不同分配状态的对象管理 */
	struct list_head	slabs_full;    //!< 已满 Slab 链表：所有对象都已分配。
	struct list_head	slabs_partial; //!< 部分空闲 Slab 链表：包含已分配和未分配对象，分配首选。
	struct list_head	slabs_free;    //!< 完全空闲 Slab 链表：不包含已分配对象，可被回收。
	unsigned int		objsize;       //!< 单个对象的大小（包含对齐填充）。
	unsigned int	 	flags;	       //!< 静态标志位（如 SLAB_HWCACHE_ALIGN）。
	unsigned int		num;	       //!< 每个 Slab 中包含的对象数量。
	spinlock_t		spinlock;      //!< 保护该 Cache 描述符的自旋锁。
#ifdef CONFIG_SMP
	unsigned int		batchcount;    //!< SMP (Symmetric Multi-Processing) 环境下，本地缓存与全局链表批量交换对象的数量。
#endif

/* 2) Slab 添加/移除()相关的字段 (Intermediate Path) */
	unsigned int		gfporder;      //!< 每个 Slab 占用的页框阶数（2^gfporder 个页框）。

	unsigned int		gfpflags;      //!< 分配页框时强制使用的 GFP 标志（如 GFP_DMA）。

	size_t			colour;		//!< 缓存着色范围：用于微调 Slab 起始偏移。
	unsigned int		colour_off;	//!< 缓存着色偏移量：每个颜色等级对应的字节偏移。
	unsigned int		colour_next;	//!< 下一个新创建的 Slab 将使用的颜色等级。
	kmem_cache_t		*slabp_cache;  //!< 如果是 Off-Slab 模式，指向存放 Slab 描述符的 Cache。
	unsigned int		growing;       //!< 标志位：表示该 Cache 是否正在增长（正在申请新 Slab）。
	unsigned int		dflags;		//!< 动态标志位。

	/** 构造函数：在创建新 Slab 时初始化每个对象 */
	void (*ctor)(void *, kmem_cache_t *, unsigned long);

	/** 析构函数：在销毁 Slab 时调用 */
	void (*dtor)(void *, kmem_cache_t *, unsigned long);

	unsigned long		failures;      //!< 统计：分配失败的次数。

/* 3) Cache 创建/销毁相关的字段 (Cold Path) */
	char			name[CACHE_NAMELEN]; //!< Cache 的名称（显示在 /proc/slabinfo 中）。
	struct list_head	next;                //!< 将所有 Cache 描述符链接在一起的全局链表。
#ifdef CONFIG_SMP
/* 4) Per-CPU Cache 数据 注意这里Linux 2.6 版本引入了不同的逻辑 */
	cpucache_t		*cpudata[NR_CPUS];   //!< 每个 CPU 私有的对象缓存，用于减少锁竞争。
#endif
#if STATS
	/* 统计信息字段 */
	unsigned long		num_active;      //!< 当前活跃（已分配）的对象总数。
	unsigned long		num_allocations; //!< 累计分配次数。
	unsigned long		high_mark;       //!< 活跃对象数量的历史最高点。
	unsigned long		grown;           //!< 累计增长（新增 Slab）次数。
	unsigned long		reaped;          //!< 累计回收（释放 Slab）次数。
	unsigned long 		errors;          //!< 累计错误次数。
#ifdef CONFIG_SMP
	atomic_t		allochit;        //!< 本地缓存命中次数。
	atomic_t		allocmiss;       //!< 本地缓存缺失次数。
	atomic_t		freehit;         //!< 释放到本地缓存的次数。
	atomic_t		freemiss;        //!< 释放时本地缓存已满的次数。
#endif
#endif
};

/* internal c_flags */
#define	CFLGS_OFF_SLAB	0x010000UL	/* slab management in own cache */
#define	CFLGS_OPTIMIZE	0x020000UL	/* optimized slab lookup */

/* c_dflags (dynamic flags). Need to hold the spinlock to access this member */
#define	DFLGS_GROWN	0x000001UL	/* don't reap a recently grown */

#define	OFF_SLAB(x)	((x)->flags & CFLGS_OFF_SLAB)
#define	OPTIMIZE(x)	((x)->flags & CFLGS_OPTIMIZE)
#define	GROWN(x)	((x)->dlags & DFLGS_GROWN)

#if STATS
#define	STATS_INC_ACTIVE(x)	((x)->num_active++)
#define	STATS_DEC_ACTIVE(x)	((x)->num_active--)
#define	STATS_INC_ALLOCED(x)	((x)->num_allocations++)
#define	STATS_INC_GROWN(x)	((x)->grown++)
#define	STATS_INC_REAPED(x)	((x)->reaped++)
#define	STATS_SET_HIGH(x)	do { if ((x)->num_active > (x)->high_mark) \
					(x)->high_mark = (x)->num_active; \
				} while (0)
#define	STATS_INC_ERR(x)	((x)->errors++)
#else
#define	STATS_INC_ACTIVE(x)	do { } while (0)
#define	STATS_DEC_ACTIVE(x)	do { } while (0)
#define	STATS_INC_ALLOCED(x)	do { } while (0)
#define	STATS_INC_GROWN(x)	do { } while (0)
#define	STATS_INC_REAPED(x)	do { } while (0)
#define	STATS_SET_HIGH(x)	do { } while (0)
#define	STATS_INC_ERR(x)	do { } while (0)
#endif

#if STATS && defined(CONFIG_SMP)
#define STATS_INC_ALLOCHIT(x)	atomic_inc(&(x)->allochit)  /**< @brief 统计：分配命中本地缓存 */
#define STATS_INC_ALLOCMISS(x)	atomic_inc(&(x)->allocmiss) /**< @brief 统计：分配未命中本地缓存 */
#define STATS_INC_FREEHIT(x)	atomic_inc(&(x)->freehit)   /**< @brief 统计：释放命中本地缓存 */
#define STATS_INC_FREEMISS(x)	atomic_inc(&(x)->freemiss)  /**< @brief 统计：释放未命中本地缓存 (缓存满) */
#else
#define STATS_INC_ALLOCHIT(x)	do { } while (0)
#define STATS_INC_ALLOCMISS(x)	do { } while (0)
#define STATS_INC_FREEHIT(x)	do { } while (0)
#define STATS_INC_FREEMISS(x)	do { } while (0)
#endif

#if DEBUG
/* Magic nums for obj red zoning.
 * Placed in the first word before and the first word after an obj.
 */
#define	RED_MAGIC1	0x5A2CF071UL	/* when obj is active */
#define	RED_MAGIC2	0x170FC2A5UL	/* when obj is inactive */

/* ...and for poisoning */
#define	POISON_BYTE	0x5a		/* byte value for poisoning */
#define	POISON_END	0xa5		/* end-byte of poisoning */

#endif

/* maximum size of an obj (in 2^order pages) */
#define	MAX_OBJ_ORDER	5	/* 32 pages */

/*
 * Do not go above this order unless 0 objects fit into the slab.
 */
#define	BREAK_GFP_ORDER_HI	2
#define	BREAK_GFP_ORDER_LO	1
static int slab_break_gfp_order = BREAK_GFP_ORDER_LO;

/*
 * Absolute limit for the gfp order
 */
#define	MAX_GFP_ORDER	5	/* 32 pages */


/* Macros for storing/retrieving the cachep and or slab from the
 * global 'mem_map'. These are used to find the slab an obj belongs to.
 * With kfree(), these are used to find the cache which an obj belongs to.
 */
#define	SET_PAGE_CACHE(pg,x)  ((pg)->list.next = (struct list_head *)(x))
#define	GET_PAGE_CACHE(pg)    ((kmem_cache_t *)(pg)->list.next)
#define	SET_PAGE_SLAB(pg,x)   ((pg)->list.prev = (struct list_head *)(x))
#define	GET_PAGE_SLAB(pg)     ((slab_t *)(pg)->list.prev)

/* Size description struct for general caches. */
typedef struct cache_sizes {
	size_t		 cs_size;
	kmem_cache_t	*cs_cachep;
	kmem_cache_t	*cs_dmacachep;
} cache_sizes_t;

static cache_sizes_t cache_sizes[] = {
#if PAGE_SIZE == 4096
	{    32,	NULL, NULL},
#endif
	{    64,	NULL, NULL},
	{   128,	NULL, NULL},
	{   256,	NULL, NULL},
	{   512,	NULL, NULL},
	{  1024,	NULL, NULL},
	{  2048,	NULL, NULL},
	{  4096,	NULL, NULL},
	{  8192,	NULL, NULL},
	{ 16384,	NULL, NULL},
	{ 32768,	NULL, NULL},
	{ 65536,	NULL, NULL},
	{131072,	NULL, NULL},
	{     0,	NULL, NULL}
};

/* internal cache of cache description objs */
static kmem_cache_t cache_cache = {
	slabs_full:	LIST_HEAD_INIT(cache_cache.slabs_full),
	slabs_partial:	LIST_HEAD_INIT(cache_cache.slabs_partial),
	slabs_free:	LIST_HEAD_INIT(cache_cache.slabs_free),
	objsize:	sizeof(kmem_cache_t),
	flags:		SLAB_NO_REAP,
	spinlock:	SPIN_LOCK_UNLOCKED,
	colour_off:	L1_CACHE_BYTES,
	name:		"kmem_cache",
};

/* Guard access to the cache-chain. */
static struct semaphore	cache_chain_sem;

/* Place maintainer for reaping. */
static kmem_cache_t *clock_searchp = &cache_cache;

#define cache_chain (cache_cache.next)

#ifdef CONFIG_SMP
/*
 * chicken and egg problem: delay the per-cpu array allocation
 * until the general caches are up.
 */
static int g_cpucache_up;

static void enable_cpucache (kmem_cache_t *cachep);
static void enable_all_cpucaches (void);
#endif

/* Cal the num objs, wastage, and bytes left over for a given slab size. */
static void kmem_cache_estimate (unsigned long gfporder, size_t size,
		 int flags, size_t *left_over, unsigned int *num)
{
	int i;
	size_t wastage = PAGE_SIZE<<gfporder;
	size_t extra = 0;
	size_t base = 0;

	if (!(flags & CFLGS_OFF_SLAB)) {
		base = sizeof(slab_t);
		extra = sizeof(kmem_bufctl_t);
	}
	i = 0;
	while (i*size + L1_CACHE_ALIGN(base+i*extra) <= wastage)
		i++;
	if (i > 0)
		i--;

	if (i > SLAB_LIMIT)
		i = SLAB_LIMIT;

	*num = i;
	wastage -= i*size;
	wastage -= L1_CACHE_ALIGN(base+i*extra);
	*left_over = wastage;
}

/* Initialisation - setup the `cache' cache. */
void __init kmem_cache_init(void)
{
	size_t left_over;

	init_MUTEX(&cache_chain_sem);
	INIT_LIST_HEAD(&cache_chain);

	kmem_cache_estimate(0, cache_cache.objsize, 0,
			&left_over, &cache_cache.num);
	if (!cache_cache.num)
		BUG();

	cache_cache.colour = left_over/cache_cache.colour_off;
	cache_cache.colour_next = 0;
}


/* Initialisation - setup remaining internal and general caches.
 * Called after the gfp() functions have been enabled, and before smp_init().
 */
void __init kmem_cache_sizes_init(void)
{
	cache_sizes_t *sizes = cache_sizes;
	char name[20];
	/*
	 * Fragmentation resistance on low memory - only use bigger
	 * page orders on machines with more than 32MB of memory.
	 */
	if (num_physpages > (32 << 20) >> PAGE_SHIFT)
		slab_break_gfp_order = BREAK_GFP_ORDER_HI;
	do {
		/* For performance, all the general caches are L1 aligned.
		 * This should be particularly beneficial on SMP boxes, as it
		 * eliminates "false sharing".
		 * Note for systems short on memory removing the alignment will
		 * allow tighter packing of the smaller caches. */
		sprintf(name,"size-%Zd",sizes->cs_size);
		if (!(sizes->cs_cachep =
			kmem_cache_create(name, sizes->cs_size,
					0, SLAB_HWCACHE_ALIGN, NULL, NULL))) {
			BUG();
		}

		/* Inc off-slab bufctl limit until the ceiling is hit. */
		if (!(OFF_SLAB(sizes->cs_cachep))) {
			offslab_limit = sizes->cs_size-sizeof(slab_t);
			offslab_limit /= 2;
		}
		sprintf(name, "size-%Zd(DMA)",sizes->cs_size);
		sizes->cs_dmacachep = kmem_cache_create(name, sizes->cs_size, 0,
			      SLAB_CACHE_DMA|SLAB_HWCACHE_ALIGN, NULL, NULL);
		if (!sizes->cs_dmacachep)
			BUG();
		sizes++;
	} while (sizes->cs_size);
}

int __init kmem_cpucache_init(void)
{
#ifdef CONFIG_SMP
	g_cpucache_up = 1;
	enable_all_cpucaches();
#endif
	return 0;
}

__initcall(kmem_cpucache_init);

/* Interface to system's page allocator. No need to hold the cache-lock.
 */
/**
 * @brief Slab 与伙伴系统的接口：申请物理页框
 * 
 * 当 Slab 仓库需要扩容时，调用此函数向伙伴系统“批发”内存。
 * 
 * @param cachep 指向需要扩容的 Cache
 * @param flags  分配标志（如 GFP_KERNEL）
 * @return void* 返回申请到的连续页框的起始虚拟地址
 */
static inline void * kmem_getpages (kmem_cache_t *cachep, unsigned long flags)
{
	void	*addr;

	/*
	 * If we requested dmaable memory, we will get it. Even if we
	 * did not request dmaable memory, we might get it, but that
	 * would be relatively rare and ignorable.
	 */
	/* 1. 叠加 Cache 预设的标志
	 * 例如，如果这个 Cache 在创建时指定了 SLAB_CACHE_DMA，
	 * 那么这里会自动加上 GFP_DMA，确保从 DMA 区域分配。
	 * @see `__kmem_cache_alloc()->kmem_cache_alloc_head()` 前面如果这两者不匹配会直接报错
	 *      这里只是冗余检查。
	 */
	flags |= cachep->gfpflags;

	/* 2. 调用zone based buddy system核心接口
	 * 根据 cachep->gfporder（通常是 0，即 1 页；大对象可能是多页）
	 * 申请 2^gfporder 个连续页框。
	 */
	addr = (void*) __get_free_pages(flags, cachep->gfporder);
	/* Assume that now we have the pages no one else can legally
	 * messes with the 'struct page's.
	 * However vm_scan() might try to test the structure to see if
	 * it is a named-page or buffer-page.  The members it tests are
	 * of no interest here.....
	 */
	return addr;
}

/* Interface to system's page release. */
/**
 * @brief Slab 与伙伴系统的接口：释放物理页框
 * 
 * 当一个 Slab 仓库被销毁或缩减时，将其占用的页框还给伙伴系统。
 */
static inline void kmem_freepages (kmem_cache_t *cachep, void *addr)
{
	unsigned long i = (1<<cachep->gfporder);
	struct page *page = virt_to_page(addr);

	/* free_pages() does not clear the type bit - we do that.
	 * The pages have been unlinked from their cache-slab,
	 * but their 'struct page's might be accessed in
	 * vm_scan(). Shouldn't be a worry.
	 */
	/* 1. 清除 Slab 标记
	 * 在归还给伙伴系统之前，必须遍历所有2^gfporder个页框，
	 * 清除 PG_slab 标志，表示这些页不再属于 Slab 系统。
	 */
	while (i--) {
		PageClearSlab(page);
		page++;
	}

	/* 2. 归还全局
	 * 调用伙伴系统的 free_pages，将内存重新放回 Zone 的空闲链表。
	 */
	free_pages((unsigned long)addr, cachep->gfporder);
}

#if DEBUG
static inline void kmem_poison_obj (kmem_cache_t *cachep, void *addr)
{
	int size = cachep->objsize;
	if (cachep->flags & SLAB_RED_ZONE) {
		addr += BYTES_PER_WORD;
		size -= 2*BYTES_PER_WORD;
	}
	memset(addr, POISON_BYTE, size);
	*(unsigned char *)(addr+size-1) = POISON_END;
}

static inline int kmem_check_poison_obj (kmem_cache_t *cachep, void *addr)
{
	int size = cachep->objsize;
	void *end;
	if (cachep->flags & SLAB_RED_ZONE) {
		addr += BYTES_PER_WORD;
		size -= 2*BYTES_PER_WORD;
	}
	end = memchr(addr, POISON_END, size);
	if (end != (addr+size-1))
		return 1;
	return 0;
}
#endif

/* Destroy all the objs in a slab, and release the mem back to the system.
 * Before calling the slab must have been unlinked from the cache.
 * The cache-lock is not held/needed.
 */
static void kmem_slab_destroy (kmem_cache_t *cachep, slab_t *slabp)
{
	if (cachep->dtor
#if DEBUG
		|| cachep->flags & (SLAB_POISON | SLAB_RED_ZONE)
#endif
	) {
		int i;
		for (i = 0; i < cachep->num; i++) {
			void* objp = slabp->s_mem+cachep->objsize*i;
#if DEBUG
			if (cachep->flags & SLAB_RED_ZONE) {
				if (*((unsigned long*)(objp)) != RED_MAGIC1)
					BUG();
				if (*((unsigned long*)(objp + cachep->objsize
						-BYTES_PER_WORD)) != RED_MAGIC1)
					BUG();
				objp += BYTES_PER_WORD;
			}
#endif
			if (cachep->dtor)
				(cachep->dtor)(objp, cachep, 0);
#if DEBUG
			if (cachep->flags & SLAB_RED_ZONE) {
				objp -= BYTES_PER_WORD;
			}	
			if ((cachep->flags & SLAB_POISON)  &&
				kmem_check_poison_obj(cachep, objp))
				BUG();
#endif
		}
	}

	kmem_freepages(cachep, slabp->s_mem-slabp->colouroff);
	if (OFF_SLAB(cachep))
		kmem_cache_free(cachep->slabp_cache, slabp);
}

/**
 * kmem_cache_create - Create a cache.
 * @name: A string which is used in /proc/slabinfo to identify this cache.
 * @size: The size of objects to be created in this cache.
 * @offset: The offset to use within the page.
 * @flags: SLAB flags
 * @ctor: A constructor for the objects.
 * @dtor: A destructor for the objects.
 *
 * Returns a ptr to the cache on success, NULL on failure.
 * Cannot be called within a int, but can be interrupted.
 * The @ctor is run when new pages are allocated by the cache
 * and the @dtor is run before the pages are handed back.
 * The flags are
 *
 * %SLAB_POISON - Poison the slab with a known test pattern (a5a5a5a5)
 * to catch references to uninitialised memory.
 *
 * %SLAB_RED_ZONE - Insert `Red' zones around the allocated memory to check
 * for buffer overruns.
 *
 * %SLAB_NO_REAP - Don't automatically reap this cache when we're under
 * memory pressure.
 *
 * %SLAB_HWCACHE_ALIGN - Align the objects in this cache to a hardware
 * cacheline.  This can be beneficial if you're counting cycles as closely
 * as davem.
 */
/** @brief 创建一个新的装对象 Cache (核心入口api)
 *
 * 该函数是 Slab 分配器的管理入口，通过一系列复杂的布局计算来初始化 kmem_cache_t 描述符：
 * 1. 严格校验：首先检查调用上下文（不能在中断中）以及参数的合法性（如对象大小不能超过限制，
 *    析构函数必须配合构造函数使用等）。
 * 2. 描述符获取：从专门管理描述符的全局缓存 `cache_cache` 中分配一个新的 `kmem_cache_t` 实例。
 * 3. 尺寸对齐：根据硬件特性调整对象大小。如果设置了 SLAB_HWCACHE_ALIGN，则按 L1 Cache Line 对齐；
 *    否则至少按机器字长（BYTES_PER_WORD）对齐，以防止跨行访问性能下降。
 * 4. 策略选择（On/Off-Slab）：根据对象大小决定管理结构（slab_t）的存放位置。如果对象较大
 *    （超过页面的 1/8），则倾向于将管理结构放在 Slab 页面之外（Off-Slab），以提高页面利用率。
 * 5. 布局估算（核心逻辑）：通过一个循环不断尝试增加 `gfporder`（从 0 开始），调用 `kmem_cache_estimate` 
 *    计算在当前页框阶数下能容纳的对象数量和剩余空间。目标是在减少内部碎片（剩余空间 < 1/8 页面）和
 *    控制页框阶数（避免过大的连续物理内存请求）之间取得平衡。
 * 6. 缓存着色（Cache Colouring）：利用估算出的剩余空间（left_over），计算着色偏移量 `colour_off` 
 *    和最大颜色数 `colour`。这使得不同 Slab 的对象在硬件缓存中能均匀分布，减少冲突。
 * 7. 链表注册：初始化 Cache 的各个 Slab 链表（full, partial, free），并将其挂载到全局 `cache_chain` 中。
 *
 * @param name   Cache 的名称。
 * @param size   对象的大小。
 * @param offset 在页面内的偏移量（通常为 0）。
 * @param flags  SLAB 标志位。
 * @param ctor   构造函数。
 * @param dtor   析构函数。
 * @return kmem_cache_t* 成功返回 Cache 指针，失败返回 NULL。
 */
kmem_cache_t *
kmem_cache_create (const char *name, size_t size, size_t offset,
	unsigned long flags, void (*ctor)(void*, kmem_cache_t *, unsigned long),
	void (*dtor)(void*, kmem_cache_t *, unsigned long))
{
	const char *func_nm = KERN_ERR "kmem_create: ";
	size_t left_over, align, slab_size;
	kmem_cache_t *cachep = NULL;

	/*
	 * Sanity checks... these are all serious usage bugs.
	 */
	if ((!name) ||
		((strlen(name) >= CACHE_NAMELEN - 1)) ||
		// 1. 严禁在中断上下文中调用，因为该函数可能会睡眠等待内存分配
		// 2. 对象大小至少为机器字长（即能装下一个指针）（访问对齐到字长的内存效率最高），且不能超过最大限制（即 32 页 -- 128KB 已经很大了）
		// 3. 你用析构函数，必须提供构造函数
		in_interrupt() ||
		(size < BYTES_PER_WORD) ||
		(size > (1<<MAX_OBJ_ORDER)*PAGE_SIZE) ||
		(dtor && !ctor) ||
		(offset < 0 || offset > size))
			BUG();

#if DEBUG
//! 处理 SLAB_DEBUG_INITIAL, SLAB_RED_ZONE, SLAB_POISON 标志
// 分别代表：
// Red Zone：在对象前后插入保护区以检测越界；
// Poison：用特定模式填充内存以检测未初始化访问；（释放后使用的错误）
	if ((flags & SLAB_DEBUG_INITIAL) && !ctor) {
		/* No constructor, but inital state check requested */
		printk("%sNo con, but init state check requested - %s\n", func_nm, name);
		flags &= ~SLAB_DEBUG_INITIAL;
	}

	if ((flags & SLAB_POISON) && ctor) {
		/* request for poisoning, but we can't do that with a constructor */
		printk("%sPoisoning requested, but con given - %s\n", func_nm, name);
		flags &= ~SLAB_POISON;
	}
#if FORCED_DEBUG
	if ((size < (PAGE_SIZE>>3)) && !(flags & SLAB_MUST_HWCACHE_ALIGN))
		/*
		 * do not red zone large object, causes severe
		 * fragmentation.
		 */
		flags |= SLAB_RED_ZONE;
	if (!ctor)
		flags |= SLAB_POISON;
#endif
#endif

	/*
	 * Always checks flags, a caller might be expecting debug
	 * support which isn't available.
	 */
	// debug 标志
	if (flags & ~CREATE_MASK)
		BUG();

	/* Get cache's description obj. */
	//！ 调用 kmem_cache_alloc 从 cache_cache 全局缓存中分配一个 kmem_cache_t 描述符 (所有的kmem_cache_t 实际上也是由slab分配器分配的)
	cachep = (kmem_cache_t *) kmem_cache_alloc(&cache_cache, SLAB_KERNEL);
	if (!cachep)
		goto opps;
	memset(cachep, 0, sizeof(kmem_cache_t));

	/* 1. 强制字对齐 (Word Alignment)
	 * 确保对象大小是字长的倍数，防止某些架构在开启红区（Redzone）时出现非对齐访问。
	 */
	if (size & (BYTES_PER_WORD-1)) {
		size += (BYTES_PER_WORD-1);
		size &= ~(BYTES_PER_WORD-1);
		printk("%sForcing size word alignment - %s\n", func_nm, name);
	}
	
#if DEBUG
	if (flags & SLAB_RED_ZONE) {
		/*
		 * There is no point trying to honour cache alignment
		 * when redzoning.
		 */
		flags &= ~SLAB_HWCACHE_ALIGN;
		size += 2*BYTES_PER_WORD;	/* words for redzone */
	}
#endif
	align = BYTES_PER_WORD;
	if (flags & SLAB_HWCACHE_ALIGN)
		align = L1_CACHE_BYTES;

	/* 2. 决定 On-Slab 还是 Off-Slab
	 * 如果对象很大（超过页框的 1/8），则将管理元数据移到页外，以减少碎片。
	 */
	if (size >= (PAGE_SIZE>>3))
		flags |= CFLGS_OFF_SLAB;

	if (flags & SLAB_HWCACHE_ALIGN) {
		/* 3. 硬件缓存对齐 (L1 Cache Alignment)
		 * 调整对象大小，使其起始地址落在 Cache Line 的边界上。
		 */
		while (size < align/2)
			align /= 2;
		size = (size+align-1)&(~(align-1));
	}

	/* 4. 计算 Slab 的阶数 (gfporder) 和每 Slab 对象数 (num)
	 * 这是一个迭代过程，旨在平衡内存利用率和分配阶数。
	 */
	do {
		unsigned int break_flag = 0;
cal_wastage:
		kmem_cache_estimate(cachep->gfporder, size, flags,
						&left_over, &cachep->num);
		if (break_flag)
			break;
		if (cachep->gfporder >= MAX_GFP_ORDER)
			break;
		if (!cachep->num)
			goto next;
		if (flags & CFLGS_OFF_SLAB && cachep->num > offslab_limit) {
			/* Oops, this num of objs will cause problems. */
			cachep->gfporder--;
			break_flag++;
			goto cal_wastage;
		}

		/*
		 * Large num of objs is good, but v. large slabs are currently
		 * bad for the gfp()s.
		 */
		if (cachep->gfporder >= slab_break_gfp_order)
			break;

		if ((left_over*8) <= (PAGE_SIZE<<cachep->gfporder))
			break;	/* Acceptable internal fragmentation. */
next:
		cachep->gfporder++;
	} while (1);

	if (!cachep->num) {
		printk("kmem_cache_create: couldn't create cache %s.\n", name);
		kmem_cache_free(&cache_cache, cachep);
		cachep = NULL;
		goto opps;
	}
	slab_size = L1_CACHE_ALIGN(cachep->num*sizeof(kmem_bufctl_t)+sizeof(slab_t));

	/*
	 * If the slab has been placed off-slab, and we have enough space then
	 * move it on-slab. This is at the expense of any extra colouring.
	 */
	if (flags & CFLGS_OFF_SLAB && left_over >= slab_size) {
		flags &= ~CFLGS_OFF_SLAB;
		left_over -= slab_size;
	}

	/* 5. 计算着色参数 (Coloring Parameters)
	 * colour_off: 颜色步长（通常是对齐值的倍数）
	 * colour: 颜色的总数（即有多少种不同的偏移量可用）
	 */
	offset += (align-1);
	offset &= ~(align-1);
	if (!offset)
		offset = L1_CACHE_BYTES;
	cachep->colour_off = offset;
	cachep->colour = left_over/offset;

	/* init remaining fields */
	if (!cachep->gfporder && !(flags & CFLGS_OFF_SLAB))
		flags |= CFLGS_OPTIMIZE;

	cachep->flags = flags;
	cachep->gfpflags = 0;
	if (flags & SLAB_CACHE_DMA)
		cachep->gfpflags |= GFP_DMA;
	spin_lock_init(&cachep->spinlock);
	cachep->objsize = size;
	INIT_LIST_HEAD(&cachep->slabs_full);
	INIT_LIST_HEAD(&cachep->slabs_partial);
	INIT_LIST_HEAD(&cachep->slabs_free);

	if (flags & CFLGS_OFF_SLAB)
		cachep->slabp_cache = kmem_find_general_cachep(slab_size,0);
	cachep->ctor = ctor;
	cachep->dtor = dtor;
	/* Copy name over so we don't have problems with unloaded modules */
	strcpy(cachep->name, name);

#ifdef CONFIG_SMP
	if (g_cpucache_up)
		enable_cpucache(cachep);
#endif
	/* Need the semaphore to access the chain. */
	down(&cache_chain_sem);
	{
		struct list_head *p;

		list_for_each(p, &cache_chain) {
			kmem_cache_t *pc = list_entry(p, kmem_cache_t, next);

			/* The name field is constant - no lock needed. */
			if (!strcmp(pc->name, name))
				BUG();
		}
	}

	/* There is no reason to lock our new cache before we
	 * link it in - no one knows about it yet...
	 */
	list_add(&cachep->next, &cache_chain);
	up(&cache_chain_sem);
opps:
	return cachep;
}


#if DEBUG
/*
 * This check if the kmem_cache_t pointer is chained in the cache_cache
 * list. -arca
 */
static int is_chained_kmem_cache(kmem_cache_t * cachep)
{
	struct list_head *p;
	int ret = 0;

	/* Find the cache in the chain of caches. */
	down(&cache_chain_sem);
	list_for_each(p, &cache_chain) {
		if (p == &cachep->next) {
			ret = 1;
			break;
		}
	}
	up(&cache_chain_sem);

	return ret;
}
#else
#define is_chained_kmem_cache(x) 1
#endif

#ifdef CONFIG_SMP
/*
 * Waits for all CPUs to execute func().
 */
static void smp_call_function_all_cpus(void (*func) (void *arg), void *arg)
{
	local_irq_disable();
	func(arg);
	local_irq_enable();

	if (smp_call_function(func, arg, 1, 1))
		BUG();
}
typedef struct ccupdate_struct_s
{
	kmem_cache_t *cachep;
	cpucache_t *new[NR_CPUS];
} ccupdate_struct_t;

static void do_ccupdate_local(void *info)
{
	ccupdate_struct_t *new = (ccupdate_struct_t *)info;
	cpucache_t *old = cc_data(new->cachep);
	
	cc_data(new->cachep) = new->new[smp_processor_id()];
	new->new[smp_processor_id()] = old;
}

static void free_block (kmem_cache_t* cachep, void** objpp, int len);

static void drain_cpu_caches(kmem_cache_t *cachep)
{
	ccupdate_struct_t new;
	int i;

	memset(&new.new,0,sizeof(new.new));

	new.cachep = cachep;

	down(&cache_chain_sem);
	smp_call_function_all_cpus(do_ccupdate_local, (void *)&new);

	for (i = 0; i < smp_num_cpus; i++) {
		cpucache_t* ccold = new.new[cpu_logical_map(i)];
		if (!ccold || (ccold->avail == 0))
			continue;
		local_irq_disable();
		free_block(cachep, cc_entry(ccold), ccold->avail);
		local_irq_enable();
		ccold->avail = 0;
	}
	smp_call_function_all_cpus(do_ccupdate_local, (void *)&new);
	up(&cache_chain_sem);
}

#else
#define drain_cpu_caches(cachep)	do { } while (0)
#endif

static int __kmem_cache_shrink(kmem_cache_t *cachep)
{
	slab_t *slabp;
	int ret;

	drain_cpu_caches(cachep);

	spin_lock_irq(&cachep->spinlock);

	/* If the cache is growing, stop shrinking. */
	while (!cachep->growing) {
		struct list_head *p;

		p = cachep->slabs_free.prev;
		if (p == &cachep->slabs_free)
			break;

		slabp = list_entry(cachep->slabs_free.prev, slab_t, list);
#if DEBUG
		if (slabp->inuse)
			BUG();
#endif
		list_del(&slabp->list);

		spin_unlock_irq(&cachep->spinlock);
		kmem_slab_destroy(cachep, slabp);
		spin_lock_irq(&cachep->spinlock);
	}
	ret = !list_empty(&cachep->slabs_full) || !list_empty(&cachep->slabs_partial);
	spin_unlock_irq(&cachep->spinlock);
	return ret;
}

/**
 * kmem_cache_shrink - Shrink a cache.
 * @cachep: The cache to shrink.
 *
 * Releases as many slabs as possible for a cache.
 * To help debugging, a zero exit status indicates all slabs were released.
 */
int kmem_cache_shrink(kmem_cache_t *cachep)
{
	if (!cachep || in_interrupt() || !is_chained_kmem_cache(cachep))
		BUG();

	return __kmem_cache_shrink(cachep);
}

/**
 * kmem_cache_destroy - delete a cache
 * @cachep: the cache to destroy
 *
 * Remove a kmem_cache_t object from the slab cache.
 * Returns 0 on success.
 *
 * It is expected this function will be called by a module when it is
 * unloaded.  This will remove the cache completely, and avoid a duplicate
 * cache being allocated each time a module is loaded and unloaded, if the
 * module doesn't have persistent in-kernel storage across loads and unloads.
 *
 * The caller must guarantee that noone will allocate memory from the cache
 * during the kmem_cache_destroy().
 */
int kmem_cache_destroy (kmem_cache_t * cachep)
{
	if (!cachep || in_interrupt() || cachep->growing)
		BUG();

	/* Find the cache in the chain of caches. */
	down(&cache_chain_sem);
	/* the chain is never empty, cache_cache is never destroyed */
	if (clock_searchp == cachep)
		clock_searchp = list_entry(cachep->next.next,
						kmem_cache_t, next);
	list_del(&cachep->next);
	up(&cache_chain_sem);

	if (__kmem_cache_shrink(cachep)) {
		printk(KERN_ERR "kmem_cache_destroy: Can't free all objects %p\n",
		       cachep);
		down(&cache_chain_sem);
		list_add(&cachep->next,&cache_chain);
		up(&cache_chain_sem);
		return 1;
	}
#ifdef CONFIG_SMP
	{
		int i;
		for (i = 0; i < NR_CPUS; i++)
			kfree(cachep->cpudata[i]);
	}
#endif
	kmem_cache_free(&cache_cache, cachep);

	return 0;
}

/* Get the memory for a slab management obj. */
/**
 * @brief 获取并初始化 Slab 管理对象 (slab_t)
 * 
 * 该函数负责为新创建的 Slab 分配管理结构，并设置其初始状态。
 * 
 * @param cachep     所属的 Cache 描述符。
 * @param objp       Slab 物理页框的起始虚拟地址。
 * @param colour_off 当前 Slab 的着色偏移量。
 * @param local_flags 分配标志。
 * @return slab_t*   指向初始化好的 Slab 管理结构，失败返回 NULL。
 */
static inline slab_t * kmem_cache_slabmgmt (kmem_cache_t *cachep,
			void *objp, int colour_off, int local_flags)
{
	slab_t *slabp;
	
	if (OFF_SLAB(cachep)) {
		/* Slab management obj is off-slab. */
		/* 1. Off-Slab 模式：管理结构存储在外部的通用 Cache 中。
		 * 这种情况通常发生在对象很大，或者为了减少 Slab 内部碎片时。
		 */
		//! 这里调用 kmem_cache_alloc 从专门的 slabp_cache 中分配 slab_t 结构
		//! 体现了 Slab 分配器的自举能力
		slabp = kmem_cache_alloc(cachep->slabp_cache, local_flags);
		if (!slabp)
			return NULL;
	} else {
		/* FIXME: change to
			slabp = objp
		 * if you enable OPTIMIZE
		 */
		/* 2. On-Slab 模式：管理结构存储在 Slab 自身的页框内。
		 * 位置在：页框起始地址 + 着色偏移。
		 * 相当于挤掉了 Slab 内部的一部分对象空间，但节省了额外的内存分配开销。
		 */
		slabp = objp+colour_off;
		// 更新 colour_off，使其跳过管理结构和 bufctl 数组，指向真正的对象起始区域
		colour_off += L1_CACHE_ALIGN(cachep->num *
				sizeof(kmem_bufctl_t) + sizeof(slab_t));
	}
	slabp->inuse = 0;            // 初始在用对象数为 0
	slabp->colouroff = colour_off; // 记录最终的对象起始偏移
	slabp->s_mem = objp+colour_off; // 计算并存储第一个对象的起始地址

	return slabp;
}

/**
 * @brief 初始化 Slab 中的所有对象
 * 
 * 1. 遍历所有对象，计算其地址。
 * 2. 如果有构造函数，则调用它（体现了“对象缓存”的优势）。
 * 3. 初始化 bufctl 账本，将所有对象串成一个初始的空闲链表。
 */
static inline void kmem_cache_init_objs (kmem_cache_t * cachep,
			slab_t * slabp, unsigned long ctor_flags)
{
	int i;

	for (i = 0; i < cachep->num; i++) {
		void* objp = slabp->s_mem+cachep->objsize*i;
#if DEBUG
		if (cachep->flags & SLAB_RED_ZONE) {
			*((unsigned long*)(objp)) = RED_MAGIC1;
			*((unsigned long*)(objp + cachep->objsize -
					BYTES_PER_WORD)) = RED_MAGIC1;
			objp += BYTES_PER_WORD;
		}
#endif

		/*
		 * Constructors are not allowed to allocate memory from
		 * the same cache which they are a constructor for.
		 * Otherwise, deadlock. They must also be threaded.
		 */
		if (cachep->ctor)
			cachep->ctor(objp, cachep, ctor_flags);
#if DEBUG
		if (cachep->flags & SLAB_RED_ZONE)
			objp -= BYTES_PER_WORD;
		if (cachep->flags & SLAB_POISON)
			/* need to poison the objs */
			kmem_poison_obj(cachep, objp);
		if (cachep->flags & SLAB_RED_ZONE) {
			if (*((unsigned long*)(objp)) != RED_MAGIC1)
				BUG();
			if (*((unsigned long*)(objp + cachep->objsize -
					BYTES_PER_WORD)) != RED_MAGIC1)
				BUG();
		}
#endif
		slab_bufctl(slabp)[i] = i+1;
	}
	slab_bufctl(slabp)[i-1] = BUFCTL_END;
	slabp->free = 0;
}

/*
 * Grow (by 1) the number of slabs within a cache.  This is called by
 * kmem_cache_alloc() when there are no active objs left in a cache.
 */
/**
 * @brief 扩展 Cache 的容量 (增加一个新的 Slab)
 * 
 * 当 Cache 中没有空闲对象时，由 kmem_cache_alloc() 调用。
 * 该函数会向伙伴系统申请物理页框，并将其初始化为一个新的 Slab。
 * 
 * kmem_cache_grow 核心流程梳理：
 * 1. 延迟校验：为了保证 kmem_cache_alloc 的快速路径足够快，内核将标志位校验
 *            和上下文检查（如中断中分配必须是 ATOMIC）推迟到了这个慢速路径中。
 * 2. 着色处理 (Slab Coloring)：通过 colour_next 计算偏移量。这是 Slab 分配器的精髓之一，
 *                            通过让不同 Slab 的起始地址在 Cache Line 上错开,
 *                            避免多个 Slab 的对象竞争同一个 CPU Cache 槽位，从而提升性能。
 * 3. 保护机制 (growing 计数)：在向伙伴系统申请内存（kmem_getpages）时，由于可能发生睡眠，
 *                           内核必须释放当前 Cache 的自旋锁。为了防止在此期间 Cache 被收缩或销毁，
 *                           通过 growing++ 标记该 Cache 正在“生长”中。
 * 4. 反向映射建立：这是最关键的一步。通过 SET_PAGE_CACHE 和 SET_PAGE_SLAB，内
 *                核将分配到的物理页框与所属的 Cache 和 Slab 绑定。这样，当你调用 kfree(ptr) 时，
 *                内核只需通过 virt_to_page(ptr) 就能立刻知道该还给哪个 Cache。
 * 5. 对象初始化：调用 kmem_cache_init_objs，如果 Cache 定义了构造函数（ctor），对象会在此时被初始化。
 * 
 * @param cachep 指向要扩展的 Cache 描述符。
 * @param flags  分配标志位。
 * @return int   成功返回 1，失败返回 0。
 */
static int kmem_cache_grow (kmem_cache_t * cachep, int flags)
{
	slab_t	*slabp;
	struct page	*page;
	void		*objp;
	size_t		 offset;
	unsigned int	 i, local_flags;
	unsigned long	 ctor_flags;
	unsigned long	 save_flags;

	/* Be lazy and only check for valid flags here,
 	 * keeping it out of the critical path in kmem_cache_alloc().
	 */
	/* 1. 校验标志位：将校验逻辑放在这里是为了缩短 kmem_cache_alloc 的快速路径 */
	if (flags & ~(SLAB_DMA|SLAB_LEVEL_MASK|SLAB_NO_GROW))
		BUG();
	if (flags & SLAB_NO_GROW)
		return 0;

	/* 2. 严苛检查：如果在中断上下文中分配，必须带有 SLAB_ATOMIC 标志 */
	if (in_interrupt() && (flags & SLAB_LEVEL_MASK) != SLAB_ATOMIC)
		BUG();

	// 这里的标志是为了传递给构造函数，告知它当前的上下文
	// SLAB_CTOR_CONSTRUCTOR: 表示调用构造函数
	// SLAB_LEVEL_MASK: 用于提取分配上下文级别
	// SLAB_CTOR_ATOMIC: 表示当前处于原子上下文，可能不能睡眠
	ctor_flags = SLAB_CTOR_CONSTRUCTOR;
	local_flags = (flags & SLAB_LEVEL_MASK);
	if (local_flags == SLAB_ATOMIC)
		/* 告知构造函数当前是原子上下文，可能不能睡眠 */
		ctor_flags |= SLAB_CTOR_ATOMIC;

	/* About to mess with non-constant members - lock. */
	spin_lock_irqsave(&cachep->spinlock, save_flags);

	/* Get colour for the slab, and cal the next value. */
	/* 3. 计算着色 (Slab Coloring) 偏移量
	 * 通过偏移对象起始地址，使不同 Slab 的对象映射到不同的 CPU Cache Line，减少伪共享。
	 */
	offset = cachep->colour_next;
	cachep->colour_next++;
	if (cachep->colour_next >= cachep->colour)
		cachep->colour_next = 0;
	offset *= cachep->colour_off;
	cachep->dflags |= DFLGS_GROWN;

	/* 增加 growing 计数, 表示当前 Cache 正在“growing”，防止在分配页面的过程中该 Cache 被收缩 (shrink) 或销毁 */
	cachep->growing++;
	spin_unlock_irqrestore(&cachep->spinlock, save_flags);

	/* A series of memory allocations for a new slab.
	 * Neither the cache-chain semaphore, or cache-lock, are
	 * held, but the incrementing c_growing prevents this
	 * cache from being reaped or shrunk.
	 * Note: The cache could be selected in for reaping in
	 * kmem_cache_reap(), but when the final test is made the
	 * growing value will be seen.
	 */

	/* Get mem for the objs. */
	/* 4. 核心分配：向伙伴系统申请物理页框 (可能睡眠) */
	if (!(objp = kmem_getpages(cachep, flags)))
		goto failed;

	/* Get slab management. */
	/* 5. 获取 Slab 管理结构 (slab_t)
	 * 根据 Cache 配置，管理结构可能在 Slab 内部 (On-Slab) 或外部 (Off-Slab)。
	 */
	if (!(slabp = kmem_cache_slabmgmt(cachep, objp, offset, local_flags)))
	    // 如果 Slab 的获取失败，释放之前分配的页面
		goto opps1;

	/* Nasty!!!!!! I hope this is OK. */
	/* 6. 建立反向映射：让这些页面的 page->list.next/prev 指向所属的 Cache 和 Slab
	 * (list.next = Cache, list.prev = Slab)
	 * 这样通过任意一个对象的虚拟地址，就能快速找到它属于哪个 Cache。
	 * @warning Nasty... For real.
	 */
	i = 1 << cachep->gfporder;
	page = virt_to_page(objp);
	do {
		SET_PAGE_CACHE(page, cachep);
		SET_PAGE_SLAB(page, slabp);
		PageSetSlab(page);
		page++;
	} while (--i);

	/* 7. 初始化对象：调用构造函数并建立空闲链表 (bufctl) */
	kmem_cache_init_objs(cachep, slabp, ctor_flags);

	spin_lock_irqsave(&cachep->spinlock, save_flags);
	cachep->growing--;

	/* Make slab active. */
	/* 8. 激活 Slab：将其挂入 Cache 的全空链表 (slabs_free) */
	list_add_tail(&slabp->list, &cachep->slabs_free);
	STATS_INC_GROWN(cachep);
	cachep->failures = 0;

	spin_unlock_irqrestore(&cachep->spinlock, save_flags);
	return 1; // 1 -- 成功

opps1:
    // 释放之前分配的页面
	kmem_freepages(cachep, objp);
failed:
    // 减少 growing 计数，表示扩展操作失败
	spin_lock_irqsave(&cachep->spinlock, save_flags);
	cachep->growing--;
	spin_unlock_irqrestore(&cachep->spinlock, save_flags);
	return 0;
}

/*
 * Perform extra freeing checks:
 * - detect double free
 * - detect bad pointers.
 * Called with the cache-lock held.
 */

#if DEBUG
static int kmem_extra_free_checks (kmem_cache_t * cachep,
			slab_t *slabp, void * objp)
{
	int i;
	unsigned int objnr = (objp-slabp->s_mem)/cachep->objsize;

	if (objnr >= cachep->num)
		BUG();
	if (objp != slabp->s_mem + objnr*cachep->objsize)
		BUG();

	/* Check slab's freelist to see if this obj is there. */
	for (i = slabp->free; i != BUFCTL_END; i = slab_bufctl(slabp)[i]) {
		if (i == objnr)
			BUG();
	}
	return 0;
}
#endif

/**
 * @brief 分配前的检查：确保 DMA 标志与 Cache 属性匹配
 * 
 * 分配必须要求 Cache 的 GFP 标志与请求的 SLAB_DMA 标志一致。
 * 
 * @param cachep 目标 Cache
 * @param flags 分配标志
 */
static inline void kmem_cache_alloc_head(kmem_cache_t *cachep, int flags)
{
	if (flags & SLAB_DMA) {
		if (!(cachep->gfpflags & GFP_DMA))
			BUG();
	} else {
		if (cachep->gfpflags & GFP_DMA)
			BUG();
	}
}

/**
 * @brief 从指定的 Slab 中摘取一个对象 (分配的最后一步) (由 kmem_cache_alloc_one 宏调用)
 * 
 * 该函数执行具体的对象分配逻辑，包括：
 * 1. 统计更新：增加 Cache 的在用对象计数。
 * 2. 指针计算：根据 Slab 的空闲索引计算对象的虚拟地址。
 * 3. 账本更新：更新 Slab 的空闲链表（bufctl），指向下一个空闲对象。
 * 4. 状态迁移：如果 Slab 变满，将其从 partial 链表移至 full 链表。
 * 5. 调试校验：在 DEBUG 模式下执行 Poison 检查和 Red Zone 设置。
 * 
 * @param cachep 指向所属的 Cache 描述符。
 * @param slabp  指向要从中摘取对象的 Slab 描述符。
 * @return void* 返回分配到的对象起始地址。
 */
static inline void * kmem_cache_alloc_one_tail (kmem_cache_t *cachep,
						slab_t *slabp)
{
	void *objp;

	// 更新统计信息：增加已分配总数、当前活跃数，并尝试更新最高水位线。
	//! 注意这里STATS模式下才会有内存检查
	STATS_INC_ALLOCED(cachep);
	STATS_INC_ACTIVE(cachep);
	STATS_SET_HIGH(cachep);


	/* 1. 增加 Slab 的在用对象计数 */
	slabp->inuse++;

	/* 2. 计算对象指针
	 * s_mem 是 Slab 中第一个对象的起始地址。
	 * free 是当前第一个空闲对象的索引。
	 * 地址 = 起始地址 + 索引 * 对象大小。
	 */
	objp = slabp->s_mem + slabp->free * cachep->objsize;

	/* 3. 更新空闲链表
	 * slab_bufctl(slabp) 返回一个数组，记录了空闲对象的链式关系。
	 * 将 slabp->free 指向下一个空闲对象的索引。
	 */
	slabp->free = slab_bufctl(slabp)[slabp->free];

	/* 4. 检查 Slab 是否已满
	 * 如果 free 索引达到了 BUFCTL_END，说明这是最后一个空闲对象。
	 * 此时该 Slab 变满，需要将其从 Cache 的 slabs_partial 链表移到 slabs_full 链表。
	 */
	if (unlikely(slabp->free == BUFCTL_END)) {
		list_del(&slabp->list);
		list_add(&slabp->list, &cachep->slabs_full);
	}

#if DEBUG
	/* 5. 调试模式下的安全检查 */
	if (cachep->flags & SLAB_POISON)
		// 检查对象是否被非法篡改（Poison 校验）
		if (kmem_check_poison_obj(cachep, objp))
			BUG();

	if (cachep->flags & SLAB_RED_ZONE) {
		/* 设置分配后的 Red Zone 标志，并校验旧标志。
		 * Red Zone 用于检测缓冲区溢出。
		 */
		if (xchg((unsigned long *)objp, RED_MAGIC2) != RED_MAGIC1)
			BUG();
		if (xchg((unsigned long *)(objp + cachep->objsize -
			  BYTES_PER_WORD), RED_MAGIC2) != RED_MAGIC1)
			BUG();
		// 跳过头部的 Red Zone 字节，返回给用户的地址从这里开始
		objp += BYTES_PER_WORD;
	}
#endif
	return objp;
}

/**
 * @brief 从全局 Slab 链表中分配一个对象 (慢速路径)
 * 
 * 该宏在持有 Cache 自旋锁的情况下调用，负责从 partial 或 free 链表中寻找可用 Slab。
 * 使用 #define 形式是为了能直接使用 goto 跳转到调用者 (__kmem_cache_alloc) 中的标签。
 * 
 * @param cachep 指向目标 Cache 描述符。
 * @return void* 指向分配到的对象起始地址，失败跳转到 alloc_new_slab 标签。
 * @note GCC 的 Statement Expression
 */
#define kmem_cache_alloc_one(cachep)				\
({								\
	struct list_head * slabs_partial, * entry;		\
	slab_t *slabp;						\
								\
	/* 1. 首先尝试从“部分满”的 Slab 链表中获取 */	\
	slabs_partial = &(cachep)->slabs_partial;		\
	entry = slabs_partial->next;				\
	if (unlikely(entry == slabs_partial)) {			\
		/* 2. 如果没有半满的，则查看“全空”的 Slab 链表 */	\
		struct list_head * slabs_free;			\
		slabs_free = &(cachep)->slabs_free;		\
		entry = slabs_free->next;			\
		if (unlikely(entry == slabs_free))		\
			/* 3. 仓库彻底空了，跳转到增长逻辑 (kmem_cache_grow) */ \
			goto alloc_new_slab;			\
								\
		/* 4. 找到了一个全空的 Slab，将其移动到 partial 链表 （从全空删除，加入部分满） */ \
		list_del(entry);				\
		list_add(entry, slabs_partial);			\
	}							\
								\
	/* 5. 此时 entry 指向一个至少有一个空闲对象的 Slab */	\
	slabp = list_entry(entry, slab_t, list);		\
	/* 6. 调用 tail 函数执行具体的对象摘取和账本更新 */	\
	kmem_cache_alloc_one_tail(cachep, slabp);		\
})

#ifdef CONFIG_SMP
/**
 * @brief 批量从全局 Slab 链表填充 Per-CPU 缓存
 * 
 * 当 Per-CPU 缓存为空时，该函数被调用以从全局链表中一次性获取 `batchcount` 个对象。
 * 这种批量操作可以有效平摊获取全局自旋锁 (`cachep->spinlock`) 的开销。
 * 
 * @param cachep 指向目标 Cache 描述符。
 * @param cc     指向当前 CPU 的本地缓存。
 * @param flags  分配标志位。
 * @return void* 返回分配到的第一个对象，如果分配失败则返回 NULL。
 */
void* kmem_cache_alloc_batch(kmem_cache_t* cachep, cpucache_t* cc, int flags)
{
	int batchcount = cachep->batchcount;

	spin_lock(&cachep->spinlock);
	while (batchcount--) {
		struct list_head * slabs_partial, * entry;
		slab_t *slabp;

		/* 1. 尝试从“部分满”链表获取 Slab */
		slabs_partial = &(cachep)->slabs_partial;
		entry = slabs_partial->next;
		if (unlikely(entry == slabs_partial)) {
			/* 2. 如果没有部分满的，尝试从“全空”链表获取 */
			struct list_head * slabs_free;
			slabs_free = &(cachep)->slabs_free;
			entry = slabs_free->next;
			if (unlikely(entry == slabs_free))
				break; /* 仓库彻底空了，退出循环 */

			/* 3. 找到全空 Slab，将其迁移至 partial 链表 */
			list_del(entry);
			list_add(entry, slabs_partial);
		}

		/* 4. 从选定的 Slab 中摘取一个对象并放入 Per-CPU 缓存数组 */
		slabp = list_entry(entry, slab_t, list);
		cc_entry(cc)[cc->avail++] =
				kmem_cache_alloc_one_tail(cachep, slabp);
	}
	spin_unlock(&cachep->spinlock);

	/* 5. 如果成功填充了对象，返回其中一个给调用者 */
	if (cc->avail)
		return cc_entry(cc)[--cc->avail];
	return NULL;
}
#endif

/**
 * @brief 从 Cache 中分配对象的底层实现 (核心逻辑)
 * 
 * 该函数实现了 Slab 分配器的“三级跳”分配策略，旨在最大化分配效率并减少锁竞争，核心逻辑如下：
 * 1. 快速路径 (Per-CPU Cache)：在 SMP 环境下，首先尝试从当前 CPU 的私有缓存 `cpucache_t` 中获取对象。
 *    这一步只需要关闭本地中断，无需获取全局自旋锁，是性能最高路径。
 * 2. 批量填充 (Batch Alloc)：如果本地缓存为空，调用 `kmem_cache_alloc_batch` 尝试从全局 Slab 链表中
 *    一次性搬运 `batchcount` 个对象到本地缓存，以平摊获取全局锁的开销。
 * 3. 慢速路径 (Slab Lists)：在非 SMP 或本地缓存缺失时，调用 `kmem_cache_alloc_one`。它会按顺序检查：
 *    - `slabs_partial`：寻找有空闲对象的 Slab。
 *    - `slabs_free`：如果没有半满 Slab，则从全空 Slab 链表中取出一个，并将其迁移至 `slabs_partial`。
 * 4. 仓库增长 (Grow)：如果所有 Slab 链表均为空，则跳转至 `alloc_new_slab`，调用 `kmem_cache_grow` 
 *    向伙伴系统申请新的物理页框来创建新 Slab。
 * 
 * @param cachep 指向目标 Cache 描述符。
 * @param flags  分配标志位（如 SLAB_KERNEL, SLAB_ATOMIC 等）。
 * @return void* 指向分配到的对象起始地址，失败返回 NULL。
 */
static inline void * __kmem_cache_alloc (kmem_cache_t *cachep, int flags)
{
	unsigned long save_flags;
	void* objp;

    /* 1. 预处理：检查 DMA 标志的一致性
     * 确保申请时的 SLAB_DMA 标志与 Cache 自身的属性匹配。
     * 如果 Cache 创建时没说要 DMA，但申请时要，或者反之，都会触发 BUG()。
     */
	kmem_cache_alloc_head(cachep, flags);

try_again:
    // 屏蔽本地中断并保存标志位，进入临界区。
    // 这是为了防止在操作 Per-CPU 缓存时被中断处理程序打断导致数据不一致。
	local_irq_save(save_flags);
#ifdef CONFIG_SMP
    {
        // 获取当前 CPU 对应的本地对象缓存 (cpucache_t)
        cpucache_t *cc = cc_data(cachep);

        if (cc) {
            // --- 第一级：本地缓存命中 (L1 Path) ---
            if (cc->avail) {
                STATS_INC_ALLOCHIT(cachep); // 统计命中次数
                // LIFO 栈式分配：从 avail 数组末尾取出一个对象指针
                objp = cc_entry(cc)[--cc->avail];
            } else {
                // --- 第二级：本地缓存缺失，尝试批量填充 (L2 Path) ---
                STATS_INC_ALLOCMISS(cachep);
                // 调用 batch 函数从全局 Slab 链表中搬运一批对象到 cc 中
                objp = kmem_cache_alloc_batch(cachep, cc, flags);
                if (!objp)
                    // 如果全局 Slab 也没有空闲对象了，跳转到“增长”逻辑
                    goto alloc_new_slab_nolock;
            }
        } else {
            /* 
             * 如果该 Cache 没有配置 Per-CPU 缓存（通常是极小的 Cache 或初始化阶段），
             * 则退化到使用全局自旋锁的单对象分配。
             */
            spin_lock(&cachep->spinlock);
            objp = kmem_cache_alloc_one(cachep);
            spin_unlock(&cachep->spinlock);
        }
    }
#else
    //! 如果只有单 CPU，则直接调用 kmem_cache_alloc_one 从全局链表分配。
	objp = kmem_cache_alloc_one(cachep);
#endif
    // 释放本地中断标志，离开临界区。并返回分配到的对象指针。
	local_irq_restore(save_flags);
	return objp;
// 执行 宏`kmem_cache_alloc_one` 时发现所有 Slab 链表均为空，跳转到这里。准备增长 Cache 逻辑(kmem_cache_grow)。
alloc_new_slab:
#ifdef CONFIG_SMP
    // 如果是从 kmem_cache_alloc_one 里的 goto 跳过来的，需要先释放锁
	spin_unlock(&cachep->spinlock);
alloc_new_slab_nolock:
#endif
    // 冗余的本地中断保护，确保增长逻辑在临界区内执行。（kmem_cache_grow 可能导致睡眠）
	local_irq_restore(save_flags);

    // --- 第三级：仓库增长 (L3 Path) ---
    // 调用 kmem_cache_grow 向伙伴系统申请新的页框并初始化为新的 Slab
    if (kmem_cache_grow(cachep, flags))
        /* 
         * 申请成功后，重新回到 try_again。
         * 此时全局链表里已经有了新的对象，再试着分配一次,大概率能在前两级路径中成功分配。
         */
        goto try_again;

    // 如果伙伴系统也拿不出内存了，分配彻底失败。
    return NULL;
}

/*
 * Release an obj back to its cache. If the obj has a constructed
 * state, it should be in this state _before_ it is released.
 * - caller is responsible for the synchronization
 */

#if DEBUG
# define CHECK_NR(pg)						\
	do {							\
		if (!VALID_PAGE(pg)) {				\
			printk(KERN_ERR "kfree: out of range ptr %lxh.\n", \
				(unsigned long)objp);		\
			BUG();					\
		} \
	} while (0)
# define CHECK_PAGE(page)					\
	do {							\
		CHECK_NR(page);					\
		if (!PageSlab(page)) {				\
			printk(KERN_ERR "kfree: bad ptr %lxh.\n", \
				(unsigned long)objp);		\
			BUG();					\
		}						\
	} while (0)

#else
# define CHECK_PAGE(pg)	do { } while (0)
#endif

/**
 * @brief 将一个对象释放回全局 Slab 链表 (底层实现)
 * 
 * 该函数执行具体的对象归还逻辑，包括：
 * 1. 查找归属：通过虚拟地址找到所属的 Slab 描述符。
 * 2. 调试清理：在 DEBUG 模式下执行 Red Zone 校验和 Poison 填充。
 * 3. 账本归还：将对象索引压回 Slab 的空闲链表 (LIFO)。
 * 4. 链表迁移：根据 Slab 的在用状态，将其在 full/partial/free 链表间移动。
 * 
 * @param cachep 指向所属的 Cache 描述符。
 * @param objp   指向要释放的对象。
 */
static inline void kmem_cache_free_one(kmem_cache_t *cachep, void *objp)
{
	slab_t* slabp;

	CHECK_PAGE(virt_to_page(objp));    // 校验对象指针的合法性
	/* reduces memory footprint
	 *
	if (OPTIMIZE(cachep))
		slabp = (void*)((unsigned long)objp&(~(PAGE_SIZE-1)));
	 else
	 */
	/* 1. 获取要释放对象所属的 Slab 描述符
	 * GET_PAGE_SLAB 宏通过 page->list.prev 快速获取（在 kmem_cache_grow 中设置）。
	 */
	slabp = GET_PAGE_SLAB(virt_to_page(objp));

#if DEBUG
	/* 2. 调试模式下的安全检查与清理 */
	if (cachep->flags & SLAB_DEBUG_INITIAL)
		/* Need to call the slab's constructor so the
		 * caller can perform a verify of its state (debugging).
		 * Called without the cache-lock held.
		 */
		// 调用构造函数进行状态校验
		cachep->ctor(objp, cachep, SLAB_CTOR_CONSTRUCTOR|SLAB_CTOR_VERIFY);

	if (cachep->flags & SLAB_RED_ZONE) {
		// 校验 Red Zone 是否被破坏，同时检测双重释放 (Double Free)
		objp -= BYTES_PER_WORD;
		if (xchg((unsigned long *)objp, RED_MAGIC1) != RED_MAGIC2)
			/* Either write before start, or a double free. */
			BUG();
		if (xchg((unsigned long *)(objp+cachep->objsize -
				BYTES_PER_WORD), RED_MAGIC1) != RED_MAGIC2)
			/* Either write past end, or a double free. */
			BUG();
	}
	if (cachep->flags & SLAB_POISON)
		// 用特定模式填充对象，防止释放后访问 (Use-After-Free)
		kmem_poison_obj(cachep, objp);
	if (kmem_extra_free_checks(cachep, slabp, objp))
		return;
#endif

	/* 3. 将对象归还给 Slab 的空闲链表 (LIFO 顺序) */
	{
		// 计算对象在 Slab 中的索引
		unsigned int objnr = (objp-slabp->s_mem)/cachep->objsize;

		// 将当前 free 索引存入 bufctl[objnr]，然后让 free 指向 objnr
		slab_bufctl(slabp)[objnr] = slabp->free;
		slabp->free = objnr;
	}
	
	// 更新 Cache 的活跃对象统计
	STATS_DEC_ACTIVE(cachep);
	
	/* 4. 维护 Slab 链表状态 (Slab Chain Fixup) */
	{
		int inuse = slabp->inuse;
		// 减少在用计数
		if (unlikely(!--slabp->inuse)) {
			/* 情况 A：Slab 变为空闲 (Empty)
			 * 将其从 partial 或 full 链表中移除，挂入 slabs_free 链表。
			 */
			list_del(&slabp->list);
			list_add(&slabp->list, &cachep->slabs_free);
		} else if (unlikely(inuse == cachep->num)) {
			/* Was full. */
			list_del(&slabp->list);
			list_add(&slabp->list, &cachep->slabs_partial);
		}
	}
}

#ifdef CONFIG_SMP
/**
 * @brief 批量释放对象到全局 Slab (不加锁版本)
 */
static inline void __free_block (kmem_cache_t* cachep,
							void** objpp, int len)
{
	for ( ; len > 0; len--, objpp++)
		kmem_cache_free_one(cachep, *objpp);
}

/**
 * @brief 批量释放对象到全局 Slab (加锁版本)
 * 
 * 用于将 Per-CPU 缓存中的一批对象归还给全局 Slab 链表。
 */
static void free_block (kmem_cache_t* cachep, void** objpp, int len)
{
	spin_lock(&cachep->spinlock);
	__free_block(cachep, objpp, len);
	spin_unlock(&cachep->spinlock);
}
#endif

/*
 * __kmem_cache_free
 * called with disabled ints
 */
/**
 * @brief 释放对象的核心实现 (处理 Per-CPU 缓存)
 * 
 * 该函数实现了释放对象的“三级跳”逆过程：
 * 1. 第一级：释放到本地缓存 (L1 Path) —— 如果本地缓存未满，直接压栈。
 * 2. 第二级：批量卸载 (Batch Drain) —— 如果本地缓存满了，先将一批对象还给全局 Slab，再压入当前对象。
 * 3. 第三级：直接释放 (L3 Path) —— 在非 SMP 或无本地缓存时，直接调用 kmem_cache_free_one。
 * 
 * @param cachep 指向目标 Cache 描述符。
 * @param objp   指向要释放的对象。
 * @note 调用者必须已关闭本地中断。
 */
static inline void __kmem_cache_free (kmem_cache_t *cachep, void* objp)
{
#ifdef CONFIG_SMP
	cpucache_t *cc = cc_data(cachep);

	CHECK_PAGE(virt_to_page(objp));
	if (cc) {
		int batchcount;
		/* --- 第一级：释放到本地缓存 --- */
		if (cc->avail < cc->limit) {
			STATS_INC_FREEHIT(cachep);
			cc_entry(cc)[cc->avail++] = objp;
			return;
		}
		
		/* --- 第二级：本地缓存满，执行批量卸载 --- */
		STATS_INC_FREEMISS(cachep);
		batchcount = cachep->batchcount;
		// 将本地缓存底部的 batchcount 个对象移出
		cc->avail -= batchcount;
		// 调用 free_block 将这批对象归还给全局 Slab 链表
		free_block(cachep, &cc_entry(cc)[cc->avail], batchcount);
		
		// 现在本地缓存有空间了，将当前对象压入
		cc_entry(cc)[cc->avail++] = objp;
		return;
	} else {
		// 如果没有配置 Per-CPU 缓存，直接释放
		free_block(cachep, &objp, 1);
	}
#else
	/* 非 SMP 系统：直接调用单对象释放逻辑 */
	kmem_cache_free_one(cachep, objp);
#endif
}

/**
 * kmem_cache_alloc - Allocate an object
 * @cachep: The cache to allocate from.
 * @flags: See kmalloc().
 *
 * Allocate an object from this cache.  The flags are only relevant
 * if the cache has no available objects.
 */
/** @brief 从指定的 Cache 中分配一个对象
 *
 * 这是 Slab 分配器的核心分配接口。它首先尝试从本地 CPU 缓存（SMP 环境下）
 * 或当前 Cache 的空闲 Slab 中获取对象。如果所有 Slab 都已满，则会调用
 * kmem_cache_grow() 向伙伴系统申请新的页框来创建新 Slab。
 *
 * @param cachep 指向要从中分配对象的 Cache 描述符。
 * @param flags  分配标志（如 GFP_KERNEL, GFP_ATOMIC），透传给伙伴系统。
 * @return void* 返回指向分配对象的指针，失败则返回 NULL。
 * @note 该函数是 kmalloc() 的底层支撑。
 */
void * kmem_cache_alloc (kmem_cache_t *cachep, int flags)
{
	return __kmem_cache_alloc(cachep, flags);
}

/**
 * kmalloc - allocate memory
 * @size: how many bytes of memory are required.
 * @flags: the type of memory to allocate.
 *
 * kmalloc is the normal method of allocating memory
 * in the kernel.
 *
 * The @flags argument may be one of:
 *
 * %GFP_USER - Allocate memory on behalf of user.  May sleep.
 *
 * %GFP_KERNEL - Allocate normal kernel ram.  May sleep.
 *
 * %GFP_ATOMIC - Allocation will not sleep.  Use inside interrupt handlers.
 *
 * Additionally, the %GFP_DMA flag may be set to indicate the memory
 * must be suitable for DMA.  This can mean different things on different
 * platforms.  For example, on i386, it means that the memory must come
 * from the first 16MB.
 */
void * kmalloc (size_t size, int flags)
{
	cache_sizes_t *csizep = cache_sizes;

	for (; csizep->cs_size; csizep++) {
		if (size > csizep->cs_size)
			continue;
		return __kmem_cache_alloc(flags & GFP_DMA ?
			 csizep->cs_dmacachep : csizep->cs_cachep, flags);
	}
	return NULL;
}

/**
 * kmem_cache_free - Deallocate an object
 * @cachep: The cache the allocation was from.
 * @objp: The previously allocated object.
 *
 * Free an object which was previously allocated from this
 * cache.
 */
/**
 * @brief 将对象释放回指定的 Cache (外部接口)
 *
 * 该函数是 Slab 分配器释放对象的公共入口。它负责：
 * 1. 基础校验：在 DEBUG 模式下检查对象是否合法。
 * 2. 中断保护：关闭本地中断以保护 Per-CPU 缓存操作。
 * 3. 核心释放：调用 __kmem_cache_free 进入实际释放流程。
 *
 * @param cachep 指向对象所属的 Cache 描述符。
 * @param objp   指向要释放的对象指针。
 * @return void
 */
void kmem_cache_free (kmem_cache_t *cachep, void *objp)
{
	unsigned long flags;
#if DEBUG
	/* 1. 调试校验：确保指针在合法页框内，且该页框确实属于这个 Cache */
	CHECK_PAGE(virt_to_page(objp));
	if (cachep != GET_PAGE_CACHE(virt_to_page(objp)))
		BUG();
#endif

	/* 2. 屏蔽本地中断：防止在操作 Per-CPU 缓存时被中断处理程序打断 */
	local_irq_save(flags);
	
	/* 3. 进入核心释放路径 */
	__kmem_cache_free(cachep, objp);
	
	/* 4. 恢复中断 */
	local_irq_restore(flags);
}

/**
 * kfree - free previously allocated memory
 * @objp: pointer returned by kmalloc.
 *
 * Don't free memory not originally allocated by kmalloc()
 * or you will run into trouble.
 */
void kfree (const void *objp)
{
	kmem_cache_t *c;
	unsigned long flags;

	if (!objp)
		return;
	local_irq_save(flags);
	CHECK_PAGE(virt_to_page(objp));
	c = GET_PAGE_CACHE(virt_to_page(objp));
	__kmem_cache_free(c, (void*)objp);
	local_irq_restore(flags);
}

kmem_cache_t * kmem_find_general_cachep (size_t size, int gfpflags)
{
	cache_sizes_t *csizep = cache_sizes;

	/* This function could be moved to the header file, and
	 * made inline so consumers can quickly determine what
	 * cache pointer they require.
	 */
	for ( ; csizep->cs_size; csizep++) {
		if (size > csizep->cs_size)
			continue;
		break;
	}
	return (gfpflags & GFP_DMA) ? csizep->cs_dmacachep : csizep->cs_cachep;
}

#ifdef CONFIG_SMP

/* called with cache_chain_sem acquired.  */
static int kmem_tune_cpucache (kmem_cache_t* cachep, int limit, int batchcount)
{
	ccupdate_struct_t new;
	int i;

	/*
	 * These are admin-provided, so we are more graceful.
	 */
	if (limit < 0)
		return -EINVAL;
	if (batchcount < 0)
		return -EINVAL;
	if (batchcount > limit)
		return -EINVAL;
	if (limit != 0 && !batchcount)
		return -EINVAL;

	memset(&new.new,0,sizeof(new.new));
	if (limit) {
		for (i = 0; i< smp_num_cpus; i++) {
			cpucache_t* ccnew;

			ccnew = kmalloc(sizeof(void*)*limit+
					sizeof(cpucache_t), GFP_KERNEL);
			if (!ccnew)
				goto oom;
			ccnew->limit = limit;
			ccnew->avail = 0;
			new.new[cpu_logical_map(i)] = ccnew;
		}
	}
	new.cachep = cachep;
	spin_lock_irq(&cachep->spinlock);
	cachep->batchcount = batchcount;
	spin_unlock_irq(&cachep->spinlock);

	smp_call_function_all_cpus(do_ccupdate_local, (void *)&new);

	for (i = 0; i < smp_num_cpus; i++) {
		cpucache_t* ccold = new.new[cpu_logical_map(i)];
		if (!ccold)
			continue;
		local_irq_disable();
		free_block(cachep, cc_entry(ccold), ccold->avail);
		local_irq_enable();
		kfree(ccold);
	}
	return 0;
oom:
	for (i--; i >= 0; i--)
		kfree(new.new[cpu_logical_map(i)]);
	return -ENOMEM;
}

static void enable_cpucache (kmem_cache_t *cachep)
{
	int err;
	int limit;

	/* FIXME: optimize */
	if (cachep->objsize > PAGE_SIZE)
		return;
	if (cachep->objsize > 1024)
		limit = 60;
	else if (cachep->objsize > 256)
		limit = 124;
	else
		limit = 252;

	err = kmem_tune_cpucache(cachep, limit, limit/2);
	if (err)
		printk(KERN_ERR "enable_cpucache failed for %s, error %d.\n",
					cachep->name, -err);
}

static void enable_all_cpucaches (void)
{
	struct list_head* p;

	down(&cache_chain_sem);

	p = &cache_cache.next;
	do {
		kmem_cache_t* cachep = list_entry(p, kmem_cache_t, next);

		enable_cpucache(cachep);
		p = cachep->next.next;
	} while (p != &cache_cache.next);

	up(&cache_chain_sem);
}
#endif

/**
 * kmem_cache_reap - Reclaim memory from caches.
 * @gfp_mask: the type of memory required.
 *
 * Called from do_try_to_free_pages() and __alloc_pages()
 */
int kmem_cache_reap (int gfp_mask)
{
	slab_t *slabp;
	kmem_cache_t *searchp;
	kmem_cache_t *best_cachep;
	unsigned int best_pages;
	unsigned int best_len;
	unsigned int scan;
	int ret = 0;

	if (gfp_mask & __GFP_WAIT)
		down(&cache_chain_sem);
	else
		if (down_trylock(&cache_chain_sem))
			return 0;

	scan = REAP_SCANLEN;
	best_len = 0;
	best_pages = 0;
	best_cachep = NULL;
	searchp = clock_searchp;
	do {
		unsigned int pages;
		struct list_head* p;
		unsigned int full_free;

		/* It's safe to test this without holding the cache-lock. */
		if (searchp->flags & SLAB_NO_REAP)
			goto next;
		spin_lock_irq(&searchp->spinlock);
		if (searchp->growing)
			goto next_unlock;
		if (searchp->dflags & DFLGS_GROWN) {
			searchp->dflags &= ~DFLGS_GROWN;
			goto next_unlock;
		}
#ifdef CONFIG_SMP
		{
			cpucache_t *cc = cc_data(searchp);
			if (cc && cc->avail) {
				__free_block(searchp, cc_entry(cc), cc->avail);
				cc->avail = 0;
			}
		}
#endif

		full_free = 0;
		p = searchp->slabs_free.next;
		while (p != &searchp->slabs_free) {
			slabp = list_entry(p, slab_t, list);
#if DEBUG
			if (slabp->inuse)
				BUG();
#endif
			full_free++;
			p = p->next;
		}

		/*
		 * Try to avoid slabs with constructors and/or
		 * more than one page per slab (as it can be difficult
		 * to get high orders from gfp()).
		 */
		pages = full_free * (1<<searchp->gfporder);
		if (searchp->ctor)
			pages = (pages*4+1)/5;
		if (searchp->gfporder)
			pages = (pages*4+1)/5;
		if (pages > best_pages) {
			best_cachep = searchp;
			best_len = full_free;
			best_pages = pages;
			if (pages >= REAP_PERFECT) {
				clock_searchp = list_entry(searchp->next.next,
							kmem_cache_t,next);
				goto perfect;
			}
		}
next_unlock:
		spin_unlock_irq(&searchp->spinlock);
next:
		searchp = list_entry(searchp->next.next,kmem_cache_t,next);
	} while (--scan && searchp != clock_searchp);

	clock_searchp = searchp;

	if (!best_cachep)
		/* couldn't find anything to reap */
		goto out;

	spin_lock_irq(&best_cachep->spinlock);
perfect:
	/* free only 50% of the free slabs */
	best_len = (best_len + 1)/2;
	for (scan = 0; scan < best_len; scan++) {
		struct list_head *p;

		if (best_cachep->growing)
			break;
		p = best_cachep->slabs_free.prev;
		if (p == &best_cachep->slabs_free)
			break;
		slabp = list_entry(p,slab_t,list);
#if DEBUG
		if (slabp->inuse)
			BUG();
#endif
		list_del(&slabp->list);
		STATS_INC_REAPED(best_cachep);

		/* Safe to drop the lock. The slab is no longer linked to the
		 * cache.
		 */
		spin_unlock_irq(&best_cachep->spinlock);
		kmem_slab_destroy(best_cachep, slabp);
		spin_lock_irq(&best_cachep->spinlock);
	}
	spin_unlock_irq(&best_cachep->spinlock);
	ret = scan * (1 << best_cachep->gfporder);
out:
	up(&cache_chain_sem);
	return ret;
}

#ifdef CONFIG_PROC_FS
/* /proc/slabinfo
 *	cache-name num-active-objs total-objs
 *	obj-size num-active-slabs total-slabs
 *	num-pages-per-slab
 */
#define FIXUP(t)				\
	do {					\
		if (len <= off) {		\
			off -= len;		\
			len = 0;		\
		} else {			\
			if (len-off > count)	\
				goto t;		\
		}				\
	} while (0)

static int proc_getdata (char*page, char**start, off_t off, int count)
{
	struct list_head *p;
	int len = 0;

	/* Output format version, so at least we can change it without _too_
	 * many complaints.
	 */
	len += sprintf(page+len, "slabinfo - version: 1.1"
#if STATS
				" (statistics)"
#endif
#ifdef CONFIG_SMP
				" (SMP)"
#endif
				"\n");
	FIXUP(got_data);

	down(&cache_chain_sem);
	p = &cache_cache.next;
	do {
		kmem_cache_t	*cachep;
		struct list_head *q;
		slab_t		*slabp;
		unsigned long	active_objs;
		unsigned long	num_objs;
		unsigned long	active_slabs = 0;
		unsigned long	num_slabs;
		cachep = list_entry(p, kmem_cache_t, next);

		spin_lock_irq(&cachep->spinlock);
		active_objs = 0;
		num_slabs = 0;
		list_for_each(q,&cachep->slabs_full) {
			slabp = list_entry(q, slab_t, list);
			if (slabp->inuse != cachep->num)
				BUG();
			active_objs += cachep->num;
			active_slabs++;
		}
		list_for_each(q,&cachep->slabs_partial) {
			slabp = list_entry(q, slab_t, list);
			if (slabp->inuse == cachep->num || !slabp->inuse)
				BUG();
			active_objs += slabp->inuse;
			active_slabs++;
		}
		list_for_each(q,&cachep->slabs_free) {
			slabp = list_entry(q, slab_t, list);
			if (slabp->inuse)
				BUG();
			num_slabs++;
		}
		num_slabs+=active_slabs;
		num_objs = num_slabs*cachep->num;

		len += sprintf(page+len, "%-17s %6lu %6lu %6u %4lu %4lu %4u",
			cachep->name, active_objs, num_objs, cachep->objsize,
			active_slabs, num_slabs, (1<<cachep->gfporder));

#if STATS
		{
			unsigned long errors = cachep->errors;
			unsigned long high = cachep->high_mark;
			unsigned long grown = cachep->grown;
			unsigned long reaped = cachep->reaped;
			unsigned long allocs = cachep->num_allocations;

			len += sprintf(page+len, " : %6lu %7lu %5lu %4lu %4lu",
					high, allocs, grown, reaped, errors);
		}
#endif
#ifdef CONFIG_SMP
		{
			cpucache_t *cc = cc_data(cachep);
			unsigned int batchcount = cachep->batchcount;
			unsigned int limit;

			if (cc)
				limit = cc->limit;
			else
				limit = 0;
			len += sprintf(page+len, " : %4u %4u",
					limit, batchcount);
		}
#endif
#if STATS && defined(CONFIG_SMP)
		{
			unsigned long allochit = atomic_read(&cachep->allochit);
			unsigned long allocmiss = atomic_read(&cachep->allocmiss);
			unsigned long freehit = atomic_read(&cachep->freehit);
			unsigned long freemiss = atomic_read(&cachep->freemiss);
			len += sprintf(page+len, " : %6lu %6lu %6lu %6lu",
					allochit, allocmiss, freehit, freemiss);
		}
#endif
		len += sprintf(page+len,"\n");
		spin_unlock_irq(&cachep->spinlock);
		FIXUP(got_data_up);
		p = cachep->next.next;
	} while (p != &cache_cache.next);
got_data_up:
	up(&cache_chain_sem);

got_data:
	*start = page+off;
	return len;
}

/**
 * slabinfo_read_proc - generates /proc/slabinfo
 * @page: scratch area, one page long
 * @start: pointer to the pointer to the output buffer
 * @off: offset within /proc/slabinfo the caller is interested in
 * @count: requested len in bytes
 * @eof: eof marker
 * @data: unused
 *
 * The contents of the buffer are
 * cache-name
 * num-active-objs
 * total-objs
 * object size
 * num-active-slabs
 * total-slabs
 * num-pages-per-slab
 * + further values on SMP and with statistics enabled
 */
int slabinfo_read_proc (char *page, char **start, off_t off,
				 int count, int *eof, void *data)
{
	int len = proc_getdata(page, start, off, count);
	len -= (*start-page);
	if (len <= count)
		*eof = 1;
	if (len>count) len = count;
	if (len<0) len = 0;
	return len;
}

#define MAX_SLABINFO_WRITE 128
/**
 * slabinfo_write_proc - SMP tuning for the slab allocator
 * @file: unused
 * @buffer: user buffer
 * @count: data len
 * @data: unused
 */
int slabinfo_write_proc (struct file *file, const char *buffer,
				unsigned long count, void *data)
{
#ifdef CONFIG_SMP
	char kbuf[MAX_SLABINFO_WRITE+1], *tmp;
	int limit, batchcount, res;
	struct list_head *p;
	
	if (count > MAX_SLABINFO_WRITE)
		return -EINVAL;
	if (copy_from_user(&kbuf, buffer, count))
		return -EFAULT;
	kbuf[MAX_SLABINFO_WRITE] = '\0'; 

	tmp = strchr(kbuf, ' ');
	if (!tmp)
		return -EINVAL;
	*tmp = '\0';
	tmp++;
	limit = simple_strtol(tmp, &tmp, 10);
	while (*tmp == ' ')
		tmp++;
	batchcount = simple_strtol(tmp, &tmp, 10);

	/* Find the cache in the chain of caches. */
	down(&cache_chain_sem);
	res = -EINVAL;
	list_for_each(p,&cache_chain) {
		kmem_cache_t *cachep = list_entry(p, kmem_cache_t, next);

		if (!strcmp(cachep->name, kbuf)) {
			res = kmem_tune_cpucache(cachep, limit, batchcount);
			break;
		}
	}
	up(&cache_chain_sem);
	if (res >= 0)
		res = count;
	return res;
#else
	return -EINVAL;
#endif
}
#endif
