# Notebook: Noncontiguous Memory Area Management (vmalloc) Exploration

- **Date**: 2025-12-19
- **Goal**: Understand the mechanism of mapping noncontiguous physical pages into a contiguous linear address space in Linux 2.4.18.
- **Status**: In Progress

## Session Log

### 2025-12-19: Initialization

- __DONE__(1): Verified completion of Slab Allocator documentation.
- __DONE__(2): Identified core files for vmalloc: `linux/mm/vmalloc.c`, `linux/include/linux/vmalloc.h`.
- __DONE__(3): Created a conceptual overview of Noncontiguous Memory Area Management in [00-concepts/noncontiguous-memory-area.md](00-concepts/noncontiguous-memory-area.md).
- __DONE__(4): Analyzed `get_vm_area()` and how `vmlist` is managed.
- __DONE__(5): Analyzed `vmalloc_area_pages()` and page table manipulation.
- __DONE__(6): Created callflows for [vmalloc](02-callflows/vmalloc.yml) and [vfree](02-callflows/vfree.yml).
- __DONE__(7): Annotated `struct vm_struct` in [linux/include/linux/vmalloc.h](linux/include/linux/vmalloc.h) with detailed comments.
- __DONE__(8): Annotated `vmalloc_area_pages`, `alloc_area_pmd`, and `alloc_area_pte` in [linux/mm/vmalloc.c](linux/mm/vmalloc.c).
- __DONE__(9): Annotated the entire `vfree` series (`vfree`, `vmfree_area_pages`, `free_area_pmd`, `free_area_pte`) in [linux/mm/vmalloc.c](linux/mm/vmalloc.c).
- __DONE__(10): Annotated the top-level `vmalloc` and `__vmalloc` functions, completing the system overview.
- __DONE__(11): Consolidated all technical insights into the master concept note [noncontiguous-memory-area.md](00-concepts/noncontiguous-memory-area.md) with fine-grained function details.
- __DONE__(12): Verified Doxygen documentation and callflows.

## Notes

- `vm_struct` is the core descriptor for each noncontiguous memory area.
- `vmlist` is a global linked list of `vm_struct` instances, protected by `vmlist_lock`.
- `vmalloc` uses `GFP_KERNEL | __GFP_HIGHMEM`, meaning it can allocate from high memory.
- The linear address range for vmalloc is typically between `VMALLOC_START` and `VMALLOC_END`.
