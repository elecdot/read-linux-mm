/*
 *  linux/mm/vmalloc.c
 *
 *  Copyright (C) 1993  Linus Torvalds
 *  Support of BIGMEM added by Gerhard Wichert, Siemens AG, July 1999
 *  SMP-safe vmalloc/vfree/ioremap, Tigran Aivazian <tigran@veritas.com>, May 2000
 */

#include <linux/config.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/spinlock.h>
#include <linux/highmem.h>
#include <linux/smp_lock.h>

#include <asm/uaccess.h>
#include <asm/pgalloc.h>

/** 保护 vmlist 链表的全局读写锁 */
rwlock_t vmlist_lock = RW_LOCK_UNLOCKED;

/** 
 * 非连续内存区域的全局链表头。
 * 链表中的元素按虚拟地址 addr 升序排列。
 */
struct vm_struct * vmlist;

/**
 * @brief 在页表项 (PTE) 级别解除映射并释放物理页
 * 
 * 这是 vfree 释放过程的最底层。它负责：
 * 1. 清除页表项 (PTE)，切断虚拟地址与物理地址的联系。
 * 2. 归还物理页框给伙伴系统。
 * 
 * @param pmd     指向所属的中间页目录项。
 * @param address 区域内的起始线性地址。
 * @param size    要释放的区域大小。
 */
static inline void free_area_pte(pmd_t * pmd, unsigned long address, unsigned long size)
{
	pte_t * pte;
	unsigned long end;

	if (pmd_none(*pmd))
		return;
	if (pmd_bad(*pmd)) {
		pmd_ERROR(*pmd);
		pmd_clear(pmd);
		return;
	}
	/* 1. 获取该地址对应的第一个 PTE 指针 */
	pte = pte_offset(pmd, address);
	address &= ~PMD_MASK;
	end = address + size;
	if (end > PMD_SIZE)
		end = PMD_SIZE;

	do {
		pte_t page;
		/* 2. 原子性地清除 PTE 并获取旧值
		 * ptep_get_and_clear 是关键，它确保了在多核环境下清除操作的原子性。
		 */
		page = ptep_get_and_clear(pte);
		address += PAGE_SIZE;
		pte++;
		if (pte_none(page))
			continue;

		/* 3. 归还物理页
		 * 如果该页在内存中（present）且不是保留页，则调用 __free_page 归还给伙伴系统。
		 */
		if (pte_present(page)) {
			struct page *ptpage = pte_page(page);
			if (VALID_PAGE(ptpage) && (!PageReserved(ptpage)))
				__free_page(ptpage);
			continue;
		}
		/* 理论上内核页表不应该被交换出去 (Swapped out) */
		printk(KERN_CRIT "Whee.. Swapped out page in kernel page table\n");
	} while (address < end);
}

/**
 * @brief 在中间页目录 (PMD) 级别递归释放
 */
static inline void free_area_pmd(pgd_t * dir, unsigned long address, unsigned long size)
{
	pmd_t * pmd;
	unsigned long end;

	if (pgd_none(*dir))
		return;
	if (pgd_bad(*dir)) {
		pgd_ERROR(*dir);
		pgd_clear(dir);
		return;
	}
	/* 1. 获取该地址对应的第一个 PMD 指针 */
	pmd = pmd_offset(dir, address);
	address &= ~PGDIR_MASK;
	end = address + size;
	if (end > PGDIR_SIZE)
		end = PGDIR_SIZE;

	do {
		/* 2. 递归进入 PTE 级别进行清理 */
		free_area_pte(pmd, address, end - address);
		address = (address + PMD_SIZE) & PMD_MASK;
		pmd++;
	} while (address < end);
}

/**
 * @brief 解除内核虚拟地址空间的映射并释放物理页 (核心入口)
 * 
 * 该函数是 vmalloc_area_pages 的逆过程，负责“拆除”页表并回收内存。
 * 
 * **核心流程**：
 * 1. 缓存同步：在修改页表前，先刷新 CPU 缓存 (flush_cache_all)。
 * 2. 逐级拆除：从 PGD 开始，递归向下清除 PMD 和 PTE。
 * 3. 物理回收：在最底层 (PTE) 将物理页框还给伙伴系统。
 * 4. TLB 刷新：全部清除后，刷新 TLB (flush_tlb_all)，确保 CPU 不再使用旧的映射。
 * 
 * @param address 线性地址起点。
 * @param size    区域大小（包含 Guard Page）。
 */
void vmfree_area_pages(unsigned long address, unsigned long size)
{
	pgd_t * dir;
	unsigned long end = address + size;

	/* 1. 获取内核 PGD 目录项起点 */
	dir = pgd_offset_k(address);

	/* 2. 刷新 CPU 缓存
	 * 在解除映射前，必须确保所有通过该虚拟地址写入的数据都已同步到物理内存。
	 */
	flush_cache_all();

	do {
		/* 3. 递归进入 PMD 级别释放 */
		free_area_pmd(dir, address, end - address);
		address = (address + PGDIR_SIZE) & PGDIR_MASK;
		dir++;
	} while (address && (address < end));

	/* 4. 刷新 TLB (Translation Lookaside Buffer)
	 * 这是最关键的一步！必须通知所有 CPU 核心：这些虚拟地址的映射已经失效了。
	 */
	flush_tlb_all();
}

/**
 * @brief 在页表项 (PTE) 级别分配物理页并建立映射
 * 
 * 这是 vmalloc 映射过程的最底层。它负责：
 * 1. 逐页申请物理页框。
 * 2. 将物理页框的地址填入对应的 PTE 项。
 * 
 * @note 性能与锁：在调用 alloc_page 之前必须释放 page_table_lock，
 *       因为分配物理页可能会导致进程睡眠，而持有自旋锁时禁止睡眠。
 */
static inline int alloc_area_pte (pte_t * pte, unsigned long address,
			unsigned long size, int gfp_mask, pgprot_t prot)
{
	unsigned long end;

	address &= ~PMD_MASK;
	end = address + size;
	if (end > PMD_SIZE)
		end = PMD_SIZE;
	do {
		struct page * page;
		/* 1. 释放锁：准备进入可能睡眠的分配路径 */
		spin_unlock(&init_mm.page_table_lock);
		
		/* 2. 批发物理页：向伙伴系统申请一个页框 */
		page = alloc_page(gfp_mask);
		
		/* 3. 重新加锁：准备修改内核页表 */
		spin_lock(&init_mm.page_table_lock);
		
		if (!pte_none(*pte))
			printk(KERN_ERR "alloc_area_pte: page already exists\n");
		if (!page)
			return -ENOMEM;

		/* 4. 建立映射：将物理页地址和保护属性合成 PTE 并写入 */
		set_pte(pte, mk_pte(page, prot));
		
		address += PAGE_SIZE;
		pte++;
	} while (address < end);
	return 0;
}

/**
 * @brief 在中间页目录 (PMD) 级别递归分配
 */
static inline int alloc_area_pmd(pmd_t * pmd, unsigned long address, unsigned long size, int gfp_mask, pgprot_t prot)
{
	unsigned long end;

	address &= ~PGDIR_MASK;
	end = address + size;
	if (end > PGDIR_SIZE)
		end = PGDIR_SIZE;
	do {
		/* 1. 确保下级 PTE 表存在：若不存在则分配 */
		pte_t * pte = pte_alloc(&init_mm, pmd, address);
		if (!pte)
			return -ENOMEM;
		/* 2. 递归进入 PTE 级别 */
		if (alloc_area_pte(pte, address, end - address, gfp_mask, prot))
			return -ENOMEM;
		address = (address + PMD_SIZE) & PMD_MASK;
		pmd++;
	} while (address < end);
	return 0;
}

/**
 * @brief 为非连续内存区域建立完整的页表映射 (核心入口)
 * 
 * 该函数负责将 `get_vm_area` 找出的线性地址空间与零散的物理页框“缝合”在一起。
 * 
 * **核心流程**：
 * 1. 确定起点：从内核主页表 `init_mm.pgd` 开始。
 * 2. 逐级下钻：PGD -> PMD -> PTE。
 * 3. 物理分配：在最底层调用 `alloc_page` 获取物理内存。
 * 4. 缓存同步：映射完成后调用 `flush_cache_all` 确保 CPU 缓存一致性。
 * 
 * @param address 线性地址起点。
 * @param size    区域大小。
 * @param gfp_mask 分配物理页时的标志。
 * @param prot    页表项的保护属性（如 PAGE_KERNEL）。
 */
inline int vmalloc_area_pages (unsigned long address, unsigned long size,
                               int gfp_mask, pgprot_t prot)
{
	pgd_t * dir;
	unsigned long end = address + size;
	int ret;

	/* 1. 获取内核 PGD 目录项 */
	dir = pgd_offset_k(address);
	
	/* 2. 获取内核页表全局锁 */
	spin_lock(&init_mm.page_table_lock);
	do {
		pmd_t *pmd;
		
		/* 3. 确保 PMD 目录存在 */
		pmd = pmd_alloc(&init_mm, dir, address);
		ret = -ENOMEM;
		if (!pmd)
			break;

		ret = -ENOMEM;
		/* 4. 进入 PMD 级别分配 */
		if (alloc_area_pmd(pmd, address, end - address, gfp_mask, prot))
			break;

		address = (address + PGDIR_SIZE) & PGDIR_MASK;
		dir++;

		ret = 0;
	} while (address && (address < end));
	
	/* 5. 释放锁并同步缓存 */
	spin_unlock(&init_mm.page_table_lock);
	flush_cache_all();
	return ret;
}
	} while (address && (address < end));
	spin_unlock(&init_mm.page_table_lock);
	flush_cache_all();
	return ret;
}

/**
 * @brief 在内核虚拟地址空间中查找并分配一段空闲区域 (核心逻辑)
 * 
 * 该函数负责在 `VMALLOC_START` 到 `VMALLOC_END` 之间寻找一段足够大的连续线性地址空间。
 * 它是 vmalloc() 和 ioremap() 的共同起点，核心逻辑如下：
 * 
 * 1. 描述符分配：首先通过 kmalloc 分配一个 `vm_struct` 结构体，用于记录该区域的元数据。
 * 2. 安全空洞 (Guard Page)：在请求的 `size` 基础上额外增加一个 PAGE_SIZE。
 *    这确保了每个 vmalloc 区域之间至少有一个不可访问的物理页，用于捕捉越界访问。
 * 3. 线性地址搜索 (First-fit 算法)：
 *    - 遍历全局有序链表 `vmlist`。
 *    - 寻找两个已分配区域之间的“空隙”（Hole）。
 *    - 如果空隙大小满足 `size` 要求，则选定该起始地址 `addr`。
 * 4. 链表插入：将新分配的 `area` 插入到 `vmlist` 的相应位置，保持链表按地址升序排列。
 * 5. 边界检查：确保分配的地址不会超过 `VMALLOC_END`。
 * 
 * @param size  请求分配的字节数（不含 Guard Page）。
 * @param flags 区域标志（VM_ALLOC 或 VM_IOREMAP）。
 * @return struct vm_struct* 指向分配好的区域描述符，失败返回 NULL。
 */
struct vm_struct * get_vm_area(unsigned long size, unsigned long flags)
{
	unsigned long addr;
	struct vm_struct **p, *tmp, *area;

	/* 1. 为描述符申请内存 */
	area = (struct vm_struct *) kmalloc(sizeof(*area), GFP_KERNEL);
	if (!area)
		return NULL;

	/* 2. 预留 Guard Page
	 * 实际分配的线性空间比用户请求的多一页，这一页不映射物理内存。
	 */
	size += PAGE_SIZE;
	addr = VMALLOC_START;

	write_lock(&vmlist_lock);

	/* 3. 寻找合适的地址空洞 (Address Hole Searching)
	 * vmlist 是按 addr 升序排列的。我们尝试在相邻的两个 vm_struct 之间
	 * 找到一个 >= size 的间隙(因为升序所以只需要遍历即可). 用来装这个线性空间
	 */
	for (p = &vmlist; (tmp = *p) ; p = &tmp->next) {
		if ((size + addr) < addr) // 溢出检查
			goto out;
		/* 如果当前搜索起点 addr 加上 size 后，还没碰到下一个区域的起点，
		 * 说明找到了足够大的空隙。
		 */
		if (size + addr <= (unsigned long) tmp->addr)
			break;
		/* 否则，将搜索起点更新为当前区域的末尾（注意 tmp->size 已包含其自身的 Guard Page） */
		addr = tmp->size + (unsigned long) tmp->addr;

		/* 检查是否超出了 vmalloc 区域的上限 */
		if (addr > VMALLOC_END-size)
			goto out;
	}

	/* 4. 填充描述符并插入链表 */
	area->flags = flags;
	area->addr = (void *)addr;
	area->size = size;
	area->next = *p;
	*p = area; // 将新区域插入链表，保持有序

	write_unlock(&vmlist_lock);
	return area;

out:
	write_unlock(&vmlist_lock);
	kfree(area);
	return NULL;
}

/**
 * @brief 释放非连续内存区域 (用户级入口)
 * 
 * 该函数是 vmalloc() 的逆过程。它负责：
 * 1. 查找描述符：在全局链表 `vmlist` 中找到对应的 `vm_struct`。
 * 2. 摘除节点：将该区域从全局链表中移除，防止其他进程再访问。
 * 3. 物理释放：调用 `vmfree_area_pages` 真正清理页表并归还物理页。
 * 4. 描述符回收：释放 `vm_struct` 结构体自身的内存。
 * 
 * @param addr 要释放的区域起始线性地址。
 */
void vfree(void * addr)
{
	struct vm_struct **p, *tmp;

	if (!addr)
		return;

	/* 1. 参数校验：vmalloc 分配的地址必须是页对齐的 */
	if ((PAGE_SIZE-1) & (unsigned long) addr) {
		printk(KERN_ERR "Trying to vfree() bad address (%p)\n", addr);
		return;
	}

	write_lock(&vmlist_lock);

	/* 2. 遍历链表寻找目标区域
	 * 我们需要找到 addr 匹配的那个 vm_struct。
	 */
	for (p = &vmlist ; (tmp = *p) ; p = &tmp->next) {
		if (tmp->addr == addr) {
			/* 3. 命中！将其从单向链表中摘除 */
			*p = tmp->next;

			/* 4. 核心清理：拆除页表并释放物理页
			 * 注意：这里传入的是 tmp->size，它包含了 Guard Page。
			 */
			vmfree_area_pages(VMALLOC_VMADDR(tmp->addr), tmp->size);
			
			write_unlock(&vmlist_lock);

			/* 5. 释放描述符：归还 vm_struct 占用的 slab 内存 */
			kfree(tmp);
			return;
		}
	}
	write_unlock(&vmlist_lock);
	printk(KERN_ERR "Trying to vfree() nonexistent vm area (%p)\n", addr);
}

/**
 * @brief 非连续内存分配的核心实现函数
 * 
 * 该函数将虚拟地址空间的分配与物理页表的映射结合在一起，实现了完整的 vmalloc 逻辑。
 * 
 * **核心步骤**：
 * 1. 参数对齐：将请求的大小向上对齐到页边界（PAGE_SIZE）。
 * 2. 虚拟占坑：调用 `get_vm_area` 在内核虚拟地址空间中找一块“空地”。
 * 3. 物理填充：调用 `vmalloc_area_pages` 逐页申请物理内存并修改页表。
 * 4. 错误处理：如果页表映射失败，必须调用 `vfree` 撤销之前的虚拟地址分配，防止内存泄漏。
 * 
 * @param size     请求分配的大小。
 * @param gfp_mask 物理页分配标志（如是否允许睡眠、是否使用高端内存）。
 * @param prot     页表项的保护属性。
 * @return void*   返回分配到的线性地址，失败返回 NULL。
 */
void * __vmalloc (unsigned long size, int gfp_mask, pgprot_t prot)
{
	void * addr;
	struct vm_struct *area;

	/* 1. 页面对齐：确保分配的大小是 PAGE_SIZE 的整数倍 */
	size = PAGE_ALIGN(size);
	if (!size || (size >> PAGE_SHIFT) > num_physpages) {
		BUG();
		return NULL;
	}

	/* 2. 预留线性地址空间 (虚拟占坑) */
	area = get_vm_area(size, VM_ALLOC);
	if (!area)
		return NULL;

	addr = area->addr;

	/* 3. 建立物理映射 (核心缝合)
	 * 这一步会循环调用 alloc_page 并修改页表。
	 */
	if (vmalloc_area_pages(VMALLOC_VMADDR(addr), size, gfp_mask, prot)) {
		/* 4. 容错处理：如果映射过程中途失败（如内存不足），
		 * 必须把已经占下的坑位和已分配的物理页全部清理掉。
		 */
		vfree(addr);
		return NULL;
	}

	/* 返回分配成功的线性地址 */
	return addr;
}

long vread(char *buf, char *addr, unsigned long count)
{
	struct vm_struct *tmp;
	char *vaddr, *buf_start = buf;
	unsigned long n;

	/* Don't allow overflow */
	if ((unsigned long) addr + count < count)
		count = -(unsigned long) addr;

	read_lock(&vmlist_lock);
	for (tmp = vmlist; tmp; tmp = tmp->next) {
		vaddr = (char *) tmp->addr;
		if (addr >= vaddr + tmp->size - PAGE_SIZE)
			continue;
		while (addr < vaddr) {
			if (count == 0)
				goto finished;
			*buf = '\0';
			buf++;
			addr++;
			count--;
		}
		n = vaddr + tmp->size - PAGE_SIZE - addr;
		do {
			if (count == 0)
				goto finished;
			*buf = *addr;
			buf++;
			addr++;
			count--;
		} while (--n > 0);
	}
finished:
	read_unlock(&vmlist_lock);
	return buf - buf_start;
}

long vwrite(char *buf, char *addr, unsigned long count)
{
	struct vm_struct *tmp;
	char *vaddr, *buf_start = buf;
	unsigned long n;

	/* Don't allow overflow */
	if ((unsigned long) addr + count < count)
		count = -(unsigned long) addr;

	read_lock(&vmlist_lock);
	for (tmp = vmlist; tmp; tmp = tmp->next) {
		vaddr = (char *) tmp->addr;
		if (addr >= vaddr + tmp->size - PAGE_SIZE)
			continue;
		while (addr < vaddr) {
			if (count == 0)
				goto finished;
			buf++;
			addr++;
			count--;
		}
		n = vaddr + tmp->size - PAGE_SIZE - addr;
		do {
			if (count == 0)
				goto finished;
			*addr = *buf;
			buf++;
			addr++;
			count--;
		} while (--n > 0);
	}
finished:
	read_unlock(&vmlist_lock);
	return buf - buf_start;
}
