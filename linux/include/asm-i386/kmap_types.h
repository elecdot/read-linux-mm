#ifndef _ASM_KMAP_TYPES_H
#define _ASM_KMAP_TYPES_H

/**
 * @brief Types of temporary kernel mappings (kmap_atomic)
 *
 * Each type corresponds to a specific usage scenario to avoid slot
 * contention within the same CPU.
 * @note The kernel use this to ensure that the same window is never
 *       used by two kernel control paths at the same time. 
 * @see @ref temporary-kernel-mappings
 */
enum km_type {
	KM_BOUNCE_READ,
	KM_SKB_DATA,
	KM_SKB_DATA_SOFTIRQ,
	KM_USER0,
	KM_USER1,
	KM_TYPE_NR
};

#endif
