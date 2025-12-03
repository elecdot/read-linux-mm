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
 * @brief Zone descriptor for zoned buddy allocator.
 *
 * `zone_t` 描述一个物理内存区域（ZONE_DMA/ZONE_NORMAL/ZONE_HIGHMEM）。每个 zone
 * 维护自己的伙伴系统空闲列表、阈值（min/low/high）以及分配/回退关系。分配器按 zone
 * 优先级尝试分配页，并根据水位（pages_min/low/high）进行平衡与回退。
 *
 * 字段语义：
 * - `lock`：保护本 zone 的空闲列表与统计。
 * - `free_pages`：当前可分配的页数（总计，含所有阶）。
 * - `pages_min/pages_low/pages_high`：水位阈值；用于触发回收与决定是否允许分配。
 * - `need_balance`：标记该 zone 需要进行平衡（如唤醒 kswapd）。
 * - `free_area[MAX_ORDER]`：各阶（order 0..MAX_ORDER-1）的伙伴空闲链表与位图。
 * - `zone_pgdat`：所属的节点描述符 `pg_data_t`。
 * - `zone_mem_map`：本 zone 覆盖的 `struct page` 起始地址（全局 `mem_map` 的一个片段）。
 * - `zone_start_paddr/zone_start_mapnr`：本 zone 覆盖的物理起始地址与全局 PFN 起始索引。
 * - `name`：人类可读的 zone 名称（如 "DMA", "Normal", "HighMem"）。
 * - `size`：本 zone 总页数。
 *
 * @see page_alloc.c: free_area_init_core(), rmqueue(), expand(); mmzone.h: pg_data_t
 */
typedef struct zone_struct {
	/* 常访问字段：快速路径计数与同步 */
	spinlock_t		lock;              //!< 保护本 zone 的空闲结构与统计
	unsigned long		free_pages;       //!< 可分配页数总计
	unsigned long		pages_min, pages_low, pages_high; //!< 水位阈值（最小、低、高）
	int			need_balance;       //!< 需要平衡（触发回收/唤醒 kswapd）

	/* 伙伴系统：不同阶的空闲列表与位图 */
	free_area_t		free_area[MAX_ORDER]; //!< 每阶的 free_list 与位图 map

	/* 不连续内存/NUMA 支持：定位到节点与本区的范围 */
	struct pglist_data	*zone_pgdat;     //!< 所属节点描述符
	struct page		*zone_mem_map;      //!< 本区覆盖的 `struct page` 起始地址
	unsigned long		zone_start_paddr;  //!< 本区物理起始地址（字节）
	unsigned long		zone_start_mapnr;  //!< 本区在全局 mem_map 中的起始 PFN 索引

	/* 低频字段：名称与大小 */
	char			*name;             //!< 名称："DMA"/"Normal"/"HighMem"
	unsigned long		size;             //!< 本区总页数
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
