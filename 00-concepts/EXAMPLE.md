---
related:
- "[Zone Allocator](../00-concepts/EXAMPLE.md)"
tags:
- memory-management
- physical-memory
sources:
- "[include/linux/mm.h](/linux/include/linux/mm.h)"
- "[Ref book](https://url)
---

# Page Frame

## In a Word

A page frame is a fixed-size block of physical memory. The kernel treats all of physical RAM as a collection of these frames, which serve as containers for holding pages of data (from processes, the kernel, or the file cache). In Linux 2.4, a page frame is typically 4KB.

## Why This Concept

Understanding the page frame is fundamental because it's the smallest unit of physical memory that the memory management subsystem works with. When the kernel needs to allocate memory, it does so by finding and assigning one or more free page frames. All memory mapping, swapping, and caching revolves around managing these frames.

## Deep Dive

Each page frame in the system is represented by a `struct page`. The kernel maintains an array of these structures, called `mem_map`, which acts as a database for all physical page frames. This array allows the kernel to quickly determine the status of any frame—whether it's free, which process owns it, or if it's part of the buffer cache. This `struct page` is the cornerstone of physical memory management.
