/*
 * buddy_diag.c -- Linux 2.4.20 的最小 Buddy Allocator 仪表盘模块
 *
 * 输出位置: /proc/buddy_diag
 *
 * 使用方式:
 *   insmod buddy-diag.o
 *   cat /proc/buddy_diag
 *   rmmod buddy_diag
 *
 * 设计要点:
 * - 不直接访问 contig_page_data/pgdat_list, 因为 2.4.x 中它们不一定导出给模块。
 * - 启动时扫描 mem_map[] (已导出) 来收集所有 zone 指针并去重。
 * - 读取 free_list 时持有 zone->lock, 避免并发修改导致链表损坏。
 */

#include <linux/module.h>    /* 模块加载/卸载相关接口 */
#include <linux/kernel.h>    /* printk, KERN_INFO 等内核基础接口 */
#include <linux/errno.h>     /* 错误码: -ENOMEM 等 */
#include <linux/proc_fs.h>   /* /proc 文件系统接口 */
#include <linux/mm.h>        /* mem_map, zone_t, MAX_ORDER 等内存管理定义 */
#include <linux/list.h>      /* 内核双向链表 list_head */

#define BUDDY_DIAG_NAME "buddy_diag"  /* /proc 条目名称 */
#define BUDDY_MAX_ZONES 32            /* 最多记录的 zone 数量上限 */

static struct proc_dir_entry *buddy_proc; /* /proc/buddy_diag 对应的目录项指针 */
static zone_t *buddy_zones[BUDDY_MAX_ZONES]; /* 保存去重后的 zone 指针数组 */
static int buddy_zone_count;               /* 当前收集到的 zone 数量 */

static int buddy_zone_known(zone_t *zone) /* 判断 zone 是否已被收集 */
{
	int i; /* 循环索引 */

	for (i = 0; i < buddy_zone_count; i++) { /* 线性扫描已知数组 */
		if (buddy_zones[i] == zone)         /* 指针相同则表示已存在 */
			return 1;                        /* 已存在 */
	}
	return 0; /* 不存在 */
}

static void buddy_collect_zones(void) /* 扫描 mem_map[] 收集所有 zone 指针 */
{
	unsigned long i; /* 物理页索引 */
	zone_t *zone;    /* 当前页所属的 zone */

	buddy_zone_count = 0; /* 重置已收集的 zone 数量 */
	if (!mem_map || !max_mapnr) { /* mem_map 未初始化或页数未知 */
		printk(KERN_WARNING "buddy_diag: mem_map not ready\n"); /* 警告日志 */
		return; /* 无法收集 zone */
	}

	for (i = 0; i < max_mapnr; i++) {      /* 遍历所有物理页描述符 */
		zone = mem_map[i].zone;             /* 取该页所属 zone 指针 */
		if (!zone)                          /* 为空则跳过 */
			continue;
		if (buddy_zone_known(zone))         /* 已收集则跳过 */
			continue;
		if (buddy_zone_count >= BUDDY_MAX_ZONES) { /* 超出上限 */
			printk(KERN_WARNING "buddy_diag: zone list truncated\n"); /* 警告 */
			break; /* 终止收集 */
		}
		buddy_zones[buddy_zone_count++] = zone; /* 记录新 zone */
	}
}

static int buddy_buf_printf(char *buf, int size, int *pos,
			    const char *fmt, ...) /* 向 proc 输出缓冲安全追加 */
{
	va_list args; /* 可变参数列表 */
	int written;  /* 实际写入长度 */

	if (*pos >= size) /* 缓冲已满 */
		return 0;      /* 不再写入 */

	va_start(args, fmt); /* 初始化可变参数 */
	written = vsnprintf(buf + *pos, size - *pos, fmt, args); /* 安全写入 */
	va_end(args);        /* 结束可变参数 */

	if (written < 0) /* 写入失败 */
		return 0;
	if (written >= size - *pos) /* 发生截断 */
		*pos = size;            /* 直接标记为满 */
	else
		*pos += written;        /* 记录新位置 */
	return written;             /* 返回本次写入长度 */
}

/* proc 读取回调: 用户读 /proc/buddy_diag 时触发 */
static int buddy_diag_read(char *buf, char **start, off_t offset,
			   int len, int *eof, void *unused)
{
	int i, order; /* i: zone 索引, order: buddy 阶数 */
	int pos = 0;  /* 当前写入位置 */

	(void)start;  /* 2.4 的 read_proc 需要该参数, 这里不用 */
	(void)unused; /* 预留参数, 这里不用 */

	if (offset > 0) { /* 仅支持一次性读取 */
		*eof = 1;     /* 标记 EOF */
		return 0;     /* 不再输出 */
	}

	if (buddy_zone_count == 0) { /* 未发现 zone */
		buddy_buf_printf(buf, len, &pos,
				 "buddy_diag: no zones discovered\n"); /* 提示信息 */
		*eof = 1; /* EOF */
		return pos; /* 返回已写入长度 */
	}

	buddy_buf_printf(buf, len, &pos, "Buddy Free Blocks per Order:\n"); /* 标题 */
	for (i = 0; i < buddy_zone_count; i++) { /* 逐个 zone 输出 */
		unsigned long counts[MAX_ORDER]; /* 每个 order 的块数 */
		unsigned long total_pages = 0;   /* 该 zone 的空闲页总数 */
		unsigned long flags;             /* 自旋锁中断标志 */
		struct list_head *head;          /* 当前 free_list 头 */
		struct list_head *lp;            /* 链表遍历指针 */
		zone_t *zone = buddy_zones[i];   /* 当前 zone */

		if (!zone) /* 防御式检查 */
			continue;

		spin_lock_irqsave(&zone->lock, flags); /* 加锁防并发修改 */
		for (order = 0; order < MAX_ORDER; order++) { /* 遍历各阶 */
			unsigned long nr = 0; /* 当前 order 的空闲块数 */

			head = &zone->free_area[order].free_list; /* 取 free_list 头 */
			list_for_each(lp, head)                   /* 逐节点计数 */
				nr++;
			counts[order] = nr;                       /* 保存块数 */
			total_pages += nr << order;               /* 折算空闲页数 */
		}
		spin_unlock_irqrestore(&zone->lock, flags); /* 解锁恢复中断 */

		buddy_buf_printf(buf, len, &pos, "Zone %s:\n",
				 zone->name ? zone->name : "unknown"); /* 输出 zone 名 */
		for (order = 0; order < MAX_ORDER; order++) { /* 输出每个阶 */
			buddy_buf_printf(buf, len, &pos,
					 "  order %2d: blocks=%lu pages=%lu\n",
					 order, counts[order],
					 counts[order] << order); /* 块数与页数 */
		}
		buddy_buf_printf(buf, len, &pos,
				 "  total free pages=%lu\n", total_pages); /* 小结 */
	}

	*eof = 1;  /* 告诉 proc 读取结束 */
	return pos; /* 返回写入长度 */
}

static int __init buddy_diag_init_module(void) /* 模块加载入口 */
{
	buddy_collect_zones(); /* 收集 zone 列表 */

	buddy_proc = create_proc_entry(BUDDY_DIAG_NAME, 0444, NULL); /* 创建 /proc 节点 */
	if (!buddy_proc) /* 创建失败 */
		return -ENOMEM; /* 返回内存不足 */
	buddy_proc->read_proc = buddy_diag_read; /* 设置 read 回调 */
	buddy_proc->owner = THIS_MODULE;         /* 防止模块被提前卸载 */

	printk(KERN_INFO "buddy_diag: loaded\n"); /* 打印加载日志 */
	return 0; /* 加载成功 */
}

static void __exit buddy_diag_exit_module(void) /* 模块卸载入口 */
{
	if (buddy_proc) /* /proc 节点存在则删除 */
		remove_proc_entry(BUDDY_DIAG_NAME, NULL);
	printk(KERN_INFO "buddy_diag: unloaded\n"); /* 打印卸载日志 */
}

module_init(buddy_diag_init_module); /* 绑定加载入口 */
module_exit(buddy_diag_exit_module); /* 绑定卸载入口 */
MODULE_LICENSE("GPL");              /* 模块许可证声明 */
