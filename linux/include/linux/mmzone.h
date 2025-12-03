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
typedef struct zone_struct {
	/*
	 * Commonly accessed fields:
	 */
	spinlock_t		lock;
	unsigned long		free_pages;
	unsigned long		pages_min, pages_low, pages_high;
	int			need_balance;

	/*
	 * free areas of different sizes
	 */
	free_area_t		free_area[MAX_ORDER];

	/*
	 * Discontig memory support fields.
	 */
	struct pglist_data	*zone_pgdat;
	struct page		*zone_mem_map;
	unsigned long		zone_start_paddr;
	unsigned long		zone_start_mapnr;

	/*
	 * rarely used fields:
	 */
	char			*name;
	unsigned long		size;
} zone_t;

#define ZONE_DMA		0
#define ZONE_NORMAL		1
#define ZONE_HIGHMEM		2
#define MAX_NR_ZONES		3

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
 */
typedef struct zonelist_struct {
	zone_t * zones [MAX_NR_ZONES+1]; // NULL delimited
} zonelist_t;

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
 * @brief Node descriptor for discontiguous/NUMA memory.
 *
 * 描述一个内存节点（NUMA 节点或不连续内存的逻辑节点）的核心数据结构。
 * 在 CONFIG_DISCONTIGMEM/NUMA 上，每个节点都有一个 `pg_data_t`，用于：
 * - 保存该节点的各个内存区（zone）的元数据与空闲列表（buddy）。
 * - 维护按 GFP 掩码预先构建的回退分配列表（zonelists）。
 * - 记录该节点的 `mem_map`（`struct page` 数组）基址与范围信息，便于 PFN/索引换算。
 *
 * 关键语义：
 * - `node_zones[MAX_NR_ZONES]`：按 `ZONE_DMA/ZONE_NORMAL/ZONE_HIGHMEM` 索引的每节点 zone 集合。
 * - `node_zonelists[GFP_ZONEMASK+1]`：每种 GFP zoning 组合的首选+回退 zone 列表（以 NULL 结尾）。
 * - `node_mem_map`：该节点的 `struct page` 起始地址（可能与全局 `mem_map` 存在偏移关系）。
 * - `node_start_paddr/node_start_mapnr/node_size`：节点物理起始、全局 mem_map 起始索引（PFN）、页数。
 *
 * @note UMA/连续内存上使用 `contig_page_data`（见 `NODE_DATA(0)`），但概念与此结构一致。
 * @see page_alloc.c: free_area_init_core(), build_zonelists(); mm.h: mem_map
 */
typedef struct pglist_data {
	zone_t node_zones[MAX_NR_ZONES];			//!< 本节点的各 zone 描述符数组（按 ZONE_* 索引）
	zonelist_t node_zonelists[GFP_ZONEMASK+1];	//!< 预构建的按 GFP zoning 的回退分配列表
	int nr_zones;					//!< 已初始化的 zone 数量（最大索引+1）
	struct page *node_mem_map;			//!< 本节点的 `struct page` 基址（节点局部视角）
	unsigned long *valid_addr_bitmap;		//!< 稀疏/不连续内存：有效物理页位图（可为空）
	struct bootmem_data *bdata;			//!< 引导期分配器状态（bootmem）
	unsigned long node_start_paddr;			//!< 本节点首个页的物理起始地址（字节）
	unsigned long node_start_mapnr;			//!< 全局 mem_map 中本节点首个页的索引（PFN）
	unsigned long node_size;			//!< 本节点覆盖的页数（各 zone 页数之和）
	int node_id;					//!< NUMA 节点 ID（从 0 开始）
	struct pglist_data *node_next;			//!< 单向链表指向下一个节点；表头见 `pgdat_list`
} pg_data_t;

extern int numnodes;
extern pg_data_t *pgdat_list; //! __EXTERN__: 指向所有内存节点(pg_data_t)的链表表头

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

extern pg_data_t contig_page_data; //! __EXTERN__: @see page_alloc.c

#ifndef CONFIG_DISCONTIGMEM

#define NODE_DATA(nid)		(&contig_page_data)
#define NODE_MEM_MAP(nid)	mem_map

#else /* !CONFIG_DISCONTIGMEM */

#include <asm/mmzone.h>

#endif /* !CONFIG_DISCONTIGMEM */

#define MAP_ALIGN(x)	((((x) % sizeof(mem_map_t)) == 0) ? (x) : ((x) + \
		sizeof(mem_map_t) - ((x) % sizeof(mem_map_t))))

#endif /* !__ASSEMBLY__ */
#endif /* __KERNEL__ */
#endif /* _LINUX_MMZONE_H */
