#ifndef _LINUX_MMZONE_H
#define _LINUX_MMZONE_H

#ifdef __KERNEL__
#ifndef __ASSEMBLY__

#include <linux/config.h>
#include <linux/spinlock.h>
#include <linux/list.h>

/*
 * Free memory management - zoned buddy allocator.
 */

#ifndef CONFIG_FORCE_MAX_ZONEORDER
#define MAX_ORDER 10
#else
#define MAX_ORDER CONFIG_FORCE_MAX_ZONEORDER
#endif

typedef struct free_area_struct {
	struct list_head	free_list;
	unsigned long		*map;
} free_area_t;

struct pglist_data;

/*
 * On machines where it is needed (eg PCs) we divide physical memory
 * into multiple physical zones. On a PC we have 3 zones:
 *
 * ZONE_DMA	  < 16 MB	ISA DMA capable memory
 * ZONE_NORMAL	16-896 MB	direct mapped by the kernel
 * ZONE_HIGHMEM	 > 896 MB	only page cache and user processes
 */
/**
 * @brief Zone descriptor. Manages watchdog watermarks and free areas for buddy system page allocation.
 * 
 * Linux kernel must deal with two hardware constraints of the 80x86 architecture:
 * 1. DMA (Direct Memory Access) capable (for old ISA-based devices) memory below 16MB. @see dma
 * 2. In modern 32-bit systems, not all physical memory (up to 4GB) can be directly mapped
 *    into the kernel's virtual address space due to address space limitations.
 * 
 * - ZONE_DMA + ZONE_NORMAL -> directly accessed by the kernel through the linear mapping
 * in the fourth GB of the linear address space. @see temp-note.
 * - ZONE_HIGHMEM -> conversely. Only page cache and user processes. @ref highmem_kernel_mapping
 * 
 * @note Many fields of the zone structure are used for page allocation and reclamation. @see Chapter reclaim
 * @ref zone-based-memory-management
 */
typedef struct zone_struct {
	/*
	 * Commonly accessed fields:
	 */
	spinlock_t		lock;                   //! Spinlock to protect zone's data structures during concurrent access.
	unsigned long		free_pages;    		//! Number of free pages in this zone.
	unsigned long		pages_min;          //! Number of reserved pages of the zone @ref reserved_page_pool
	unsigned long		pages_low;          //! Low watermark of page frame reclaiming; also used by the zone allocator as a threshold value @ref zone_allocator
	unsigned long		pages_high;         //! High watermark of page frame reclaiming; same as `pages_low`, @ref zone_allocator
	
	int			need_balance;               //! A needs writeback flag for the kswapd daemon to indicate if this zone needs balancing.

	/*
	 * free areas of different sizes
	 */
	free_area_t		free_area[MAX_ORDER];   //! @ref buddy-system free area lists for different orders.

	/*
	 * Discontig memory support fields.
	 */
	struct pglist_data	*zone_pgdat;      //! Back pointer to the pglist_data (node) that owns this zone (contig_page_data, of course).
	struct page		*zone_mem_map;        //! Pointer to the page descriptor array for all page frames in this zone.
	unsigned long		zone_start_paddr;  //! Starting physical address (the real physical memory address) of this zone.
	unsigned long		zone_start_mapnr;  //! Starting index/offset in the global mem_map array; used to map page frames to descriptors in this zone.

	/*
	 * rarely used fields:
	 */
	char			*name;                //! Zone name for debugging purposes, e.g., "DMA", "Normal", "HighMem".
	unsigned long		size;             //! Total number of page frames in this zone.
	/**
	 * @warning Lost a bunch of fields here in comparison to Linux 2.6.
	 */
} zone_t;

#define ZONE_DMA		0
#define ZONE_NORMAL		1
#define ZONE_HIGHMEM		2
#define MAX_NR_ZONES		3 //! Maximum number of memory zones supported.

/*
 * One allocation request operates on a zonelist. A zonelist
 * is a list of zones, the first one is the 'goal' of the
 * allocation, the other zones are fallback zones, in decreasing
 * priority.
 *
 * Right now a zonelist takes up less than a cacheline. We never
 * modify it apart from boot-up, and only a few indices are used,
 * so despite the zonelist table being relatively big, the cache
 * footprint of this construct is very small.
 * 缓存效率的设计保证：
 * - 只存指针不存数据: 32位系统16字节，64位系统32字节，都远小于缓存行(64字节)
 * - 启动后只读: 运行时只查询不修改，避免缓存失效
 * - NULL终止数组: 虽然有MAX_NR_ZONES+1个位置，但实际只用几个，内存占用固定且小
 * 结果：整个zonelist可在一次缓存命中时加载，加快内存分配时的zone优先级查询
 */
/**
 * @brief Zonelist indicating the priority order of zones for page allocation.
 * First zone is the preferred one; others are fallbacks in decreasing priority.
 * 
 * fallback hierarchy: ZONE_NORMAL -> ZONE_DMA:
 * - ZONE_DMA Preservation for devices needing DMA-capable memory.
 * - Watermark-based allocation: allocator tries higher zones first, falling back to lower ones if needed.
 * - Pressure Signaling: kswapd daemon uses zonelists to determine zones need balancing.
 *
 * @ref zone-selection-gfp
 */
typedef struct zonelist_struct {
	zone_t * zones [MAX_NR_ZONES+1]; // NULL delimited
} zonelist_t;

//! Low four bits of GFP mask used to select memory zones. @ref zone-selection-gfp
#define GFP_ZONEMASK	0x0f

/*
 * The pg_data_t structure is used in machines with CONFIG_DISCONTIGMEM
 * (mostly NUMA machines?) to denote a higher-level memory zone than the
 * zone_struct denotes.
 *
 * On NUMA machines, each NUMA node would have a pg_data_t to describe
 * it's memory layout.
 *
 * XXX: we need to move the global memory statistics (active_list, ...)
 *      into the pg_data_t to properly support NUMA.
 */
struct bootmem_data;
/**
 * @brief Each node in a NUMA system has a descriptor of pg_data_t. All
 * node descriptors are stored in a singly linkded list starting from pgdat_list.
 * The physical memory of each node is divided into several zones, each represented
 * by a zone_t structure within the pg_data_t.
 * 
 * The time needed by a given CPU to access pages within a single node
 * is the same.
 */
typedef struct pglist_data {
	/* === Node Management === */
	unsigned long node_start_paddr;             //! Physical address of the first page frame in this node.
	unsigned long node_start_mapnr;             //! Index of the first page descriptor in the global mem_map (physical page descriptors) array for this node. @code page = &node->node_mem_map[(physical_address / PAGE_SIZE) - node->node_start_mapnr]; @endcode
	int node_id;                                //! Unique identifier for this NUMA node.
	struct pglist_data *node_next;              //! Pointer to the next node descriptor in the NUMA node list.

	/* === Zone Management === */
	zone_t node_zones[MAX_NR_ZONES]; 			//! Array of zone descriptors of this node.
	zonelist_t node_zonelists[GFP_ZONEMASK+1];  //! Array of zonelist data structures used by the page allocator. @ref zone-selection-gfp
	int nr_zones;                               //! Number of zones in this node.

	/* === Page Management === */
	struct page *node_mem_map;                  //! Pointer to the "Array" of page descriptors of this node.
	unsigned long *valid_addr_bitmap;           //! Bitmap indicating valid physical address ranges in this node.

	/* === Initialization === */
	struct bootmem_data *bdata;                 //! Used in the kernel initialization phase to manage boot memory.

	/*
	 * There are missed field compared to Linux 2.6, e.g. `kswapd_awit`:
	 * Wait queue for the kswapd pageout daemon @see section "Periodic Reclaiming"
	 */
} pg_data_t;

extern int numnodes;
//! __EXTERN__: NUMA系统中所有节点的pg_data_t结构体链表头指针.
extern pg_data_t *pgdat_list;

#define memclass(pgzone, classzone)	(((pgzone)->zone_pgdat == (classzone)->zone_pgdat) \
			&& ((pgzone) <= (classzone)))

/*
 * The following two are not meant for general usage. They are here as
 * prototypes for the discontig memory code.
 */
struct page;
extern void show_free_areas_core(pg_data_t *pgdat);
extern void free_area_init_core(int nid, pg_data_t *pgdat, struct page **gmap,
  unsigned long *zones_size, unsigned long paddr, unsigned long *zholes_size,
  struct page *pmap);

extern pg_data_t contig_page_data;

#ifndef CONFIG_DISCONTIGMEM

#define NODE_DATA(nid)		(&contig_page_data)
#define NODE_MEM_MAP(nid)	mem_map

#else /* !CONFIG_DISCONTIGMEM */

#include <asm/mmzone.h>

#endif /* !CONFIG_DISCONTIGMEM */

/** @brief 用于对齐内存映射数组的宏定义.
 * @param x 需要对齐的地址或大小.
 * @return 返回对齐后的地址或大小.
 * @note 该宏通过检查 x 是否已经是 mem_map_t 类型的整数倍, 如果不是,
 */
#define MAP_ALIGN(x)	((((x) % sizeof(mem_map_t)) == 0) ? (x) : ((x) + \
		sizeof(mem_map_t) - ((x) % sizeof(mem_map_t))))

#endif /* !__ASSEMBLY__ */
#endif /* __KERNEL__ */
#endif /* _LINUX_MMZONE_H */
