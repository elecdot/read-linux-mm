---
related:
- "[Page Frame](../00-concepts/EXAMPLE.md)"
tags:
- source-map
- page-allocation
sources:
- "[mm/page_alloc.c](/linux/mm/page_alloc.c)"
---

# Source Map: mm/page_alloc.c

## File Overview

This file is the heart of the physical page allocation system in Linux 2.4. It implements the buddy system algorithm, which is responsible for allocating and freeing page frames. It manages physical memory by dividing it into zones (DMA, Normal, HighMem) to handle different hardware constraints.

## Key Data Structures

- **`struct zone_struct`**: Represents a memory zone (e.g., `ZONE_NORMAL`). It contains pointers to the `free_area` lists, a lock for synchronization, and statistics.
- **`struct free_area`**: An array within each zone that manages blocks of free pages. `free_area[0]` manages 1-page blocks, `free_area[1]` manages 2-page blocks, and so on.
- **`struct page`**: The core structure representing a single physical page frame. `page_alloc.c` uses its fields (`list`, `count`, etc.) to track the state of each page.

## Core Functions

### `__alloc_pages()`

- **Purpose**: This is the main workhorse for allocating a block of contiguous physical pages. It implements the core buddy algorithm logic.
- **Inputs/Outputs**:
    - `gfp_mask`: Flags that control allocation behavior (e.g., `GFP_KERNEL`, `__GFP_DMA`).
    - `order`: The size of the allocation, as a power of two (2^`order` pages).
    - Returns a pointer to the first `struct page` of the allocated block, or `NULL` on failure.
- **Typical Callers**: `alloc_pages()`, which is the main external interface.
- **Related Files**: `include/linux/mm.h`, `mm/slab.c`.

### `__free_pages()`

- **Purpose**: Frees a block of pages and merges it with adjacent free blocks (buddies) if possible, to form larger contiguous blocks.
- **Inputs/Outputs**:
    - `page`: Pointer to the first page of the block to free.
    - `order`: The size of the block (2^`order` pages).
- **Typical Callers**: `free_pages()`.
- **Related Files**: `include/linux/mm.h`.
