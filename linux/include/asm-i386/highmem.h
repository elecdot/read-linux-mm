/*
 * highmem.h: virtual kernel memory mappings for high memory
 *
 * Used in CONFIG_HIGHMEM systems for memory pages which
 * are not addressable by direct kernel virtual addresses.
 *
 * Copyright (C) 1999 Gerhard Wichert, Siemens AG
 *		      Gerhard.Wichert@pdb.siemens.de
 *
 *
 * Redesigned the x86 32-bit VM architecture to deal with 
 * up to 16 Terabyte physical memory. With current x86 CPUs
 * we now support up to 64 Gigabytes physical RAM.
 *
 * Copyright (C) 1999 Ingo Molnar <mingo@redhat.com>
 */

#ifndef _ASM_HIGHMEM_H
#define _ASM_HIGHMEM_H

#ifdef __KERNEL__

#include <linux/config.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <asm/kmap_types.h>
#include <asm/pgtable.h>

#ifdef CONFIG_DEBUG_HIGHMEM
#define HIGHMEM_DEBUG 1
#else
#define HIGHMEM_DEBUG 0
#endif

/* declarations for highmem.c */
extern unsigned long highstart_pfn, highend_pfn;

extern pte_t *kmap_pte;
extern pgprot_t kmap_prot;
/** @brief Pointer to the PKMAP page table (PTE array)
 * 
 * Points to the first page table entry (PTE) in the PKMAP region.
 * Dynamically modified to map high-memory pages to virtual addresses in PKMAP.
 * Used by kmap()/kunmap() to establish virtual→physical mappings.
 */
extern pte_t *pkmap_page_table;

extern void kmap_init(void) __init;

/*
 * Right now we initialize only a single pte table. It can be extended
 * easily, subsequent pte tables have to be allocated in one physical
 * chunk of RAM.
 */
/** @brief Starting virtual address of the PKMAP (Page Kernel MAPping) region
 *
 * PKMAP is a 2-4MB dynamic mapping area in kernel address space used by kmap()
 * to temporarily map high-memory page frames to virtual addresses.
 */
#define PKMAP_BASE (0xfe000000UL)
#ifdef CONFIG_X86_PAE
/** @brief Maximum number of simultaneous permanent kernel mappings
 *
 * With PAE (Physical Address Extension): 512 slots = 2MB PKMAP region
 * Each slot maps one 4KB page frame.
 */
#define LAST_PKMAP 512
#else
/** @brief Maximum number of simultaneous permanent kernel mappings (non-PAE)
 *
 * Without PAE: 1024 slots = 4MB PKMAP region
 */
#define LAST_PKMAP 1024
#endif

/** @brief Bit mask for cycling through PKMAP slots (LAST_PKMAP - 1)
 *
 * Used in circular buffer logic: slot_nr = (slot_nr + 1) & LAST_PKMAP_MASK
 */
#define LAST_PKMAP_MASK (LAST_PKMAP-1)

/** @brief Convert PKMAP virtual address to slot number
 * @param virt Virtual address in PKMAP region
 * @return Slot index (0 to LAST_PKMAP-1)
 */
#define PKMAP_NR(virt)  ((virt-PKMAP_BASE) >> PAGE_SHIFT)

/** @brief Convert slot number to PKMAP virtual address
 * @param nr Slot index (0 to LAST_PKMAP-1)
 * @return Virtual address in PKMAP region
 */
#define PKMAP_ADDR(nr)  (PKMAP_BASE + ((nr) << PAGE_SHIFT))

extern void * FASTCALL(kmap_high(struct page *page));
extern void FASTCALL(kunmap_high(struct page *page));

/**
 * @brief Establish permanent kernel mapping for a page
 * @param page Page to be mapped
 * @return (void*) Kernel virtual address corresponding to the page
 * @note 本质是一个调用 kmap_high() 的防御性编程, 确保：1. 非中断上下文调用 2. 低内存页直接返回地址
 */
static inline void *kmap(struct page *page)
{
	//! Use `kmap_atomic` in interrupt context (cannot sleep)
	if (in_interrupt())
		BUG();
    //! IF page is lowmem, just return direct mapping address
	if (page < highmem_start_page)
		return page_address(page);
    //! ELSE, process highmem page via kmap_high()
	return kmap_high(page);
}

/**
 * @brief Destroys a permanent kernel mapping established previously by kmap() (**Used by a process in pairs**)
 * @param page Page to be unmapped
 * @return void (side-effect only)
 * @note 与kmap()相同，本质是一个调用 kunmap_high() 的防御性编程, 确保：1. 非中断上下文调用 2. 低内存页直接返回地址
 */
static inline void kunmap(struct page *page)
{
	if (in_interrupt())
		BUG();
	if (page < highmem_start_page)
		return;
	kunmap_high(page);
}

/*
 * The use of kmap_atomic/kunmap_atomic is discouraged - kmap/kunmap
 * gives a more generic (and caching) interface. But kmap_atomic can
 * be used in IRQ contexts, so in some (very limited) cases we need
 * it.
 */
/**
 * @brief Establish a temporary atomic kernel mapping for a page
 * @param page The page to map
 * @param type The slot type (km_type) to use for this CPU
 * @return The virtual address of the mapping
 * @note This function is atomic and can be used in interrupt context.
 * @see @ref temporary-kernel-mappings
 */
static inline void *kmap_atomic(struct page *page, enum km_type type)
{
	enum fixed_addresses idx;
	unsigned long vaddr;

	if (page < highmem_start_page)
		return page_address(page);

    //! 1. Retrieved though `smp_processor_id()` to specify what fix-mapped linear address has to be used to map the request page.
	//! i.e., which macro slot defined in km_type enum to use for this CPU
	idx = type + KM_TYPE_NR*smp_processor_id();
	vaddr = __fix_to_virt(FIX_KMAP_BEGIN + idx);  //!< 2. Retrieve virtual address for the fixmap index
#if HIGHMEM_DEBUG
	if (!pte_none(*(kmap_pte-idx)))
		BUG();
#endif
	set_pte(kmap_pte-idx, mk_pte(page, kmap_prot)); //!< 3. Set the page table entry for the mapping @see `kmap()`, the bits Present, Accessed+Read+Write+Dirty
	// @see `flush_all_zero_pkmaps()`： `kmap()在map_new_virtual时` 若无法满足时才会调用刷新函数
	// 同时，由于这里是原子映射，必然需要刷新。@see `kunmap_atomic()`：释放时不会做任何事，还有拿到旧的PTE的风险
	__flush_tlb_one(vaddr);                         //!< 4. Flush TLB entry for the new mapping

	return (void*) vaddr;
}

/**
 * @brief Destroy a temporary atomic kernel mapping
 * @param kvaddr The virtual address to unmap
 * @param type The slot type used during mapping
 * @see @ref temporary-kernel-mappings
 * @warning Seems this function would be factored out after Linux 2.6.
 *          Include decrease `preempt_count` and TIF_NEED_RESCHED check.
 */
static inline void kunmap_atomic(void *kvaddr, enum km_type type)
{
#if HIGHMEM_DEBUG
	unsigned long vaddr = (unsigned long) kvaddr;
	enum fixed_addresses idx = type + KM_TYPE_NR*smp_processor_id();

	if (vaddr < FIXADDR_START) // FIXME
		return;

	if (vaddr != __fix_to_virt(FIX_KMAP_BEGIN+idx))
		BUG();

	/*
	 * force other mappings to Oops if they'll try to access
	 * this pte without first remap it
	 */
	pte_clear(kmap_pte-idx);
	__flush_tlb_one(vaddr);
#endif
}

#endif /* __KERNEL__ */

#endif /* _ASM_HIGHMEM_H */
