# Notebook: Page Frame Reclaiming Exploration

- **Date**: 2025-12-19
- **Goal**: Understand the Page Frame Reclaiming (页框回收) mechanism in Linux 2.4.18, including LRU management, kswapd, and page aging.
- **Status**: Initializing

## Session Log

### 2025-12-19: Initialization

- __DONE__(1): Create initial concept file for Page Frame Reclaiming.
- __DONE__(2): Identified core files and functions in `linux/mm/vmscan.c` and related headers.
- __DONE__(3): Understand the LRU list structure (`active_list`, `inactive_list`).
- __DONE__(4): Annotated LRU macros in `linux/include/linux/swap.h` and logic in `linux/mm/swap.c`.
- __DONE__(5): Annotated `refill_inactive` and `shrink_cache` in `linux/mm/vmscan.c` (Dissection level).
- __DONE__(6): Updated concept doc with deep dive into LRU core functions.
- __DONE__(7): Analyze `kswapd` lifecycle and waking conditions.
- __DONE__(8): Annotated `kswapd` and related balancing functions in `vmscan.c`.
- __DONE__(9): Dissected `swap_out` and `try_to_swap_out` mechanism.
- __DONE__(10): Briefly analyzed the `priority` escalation mechanism.
- __TODO__(11): Summarize the interaction between `shrink_cache` and `swap_out`.

## Notes

- Page reclaiming is triggered when the number of free pages in a zone falls below a threshold.
- Linux 2.4 uses a simplified LRU-like scheme with active and inactive lists.
