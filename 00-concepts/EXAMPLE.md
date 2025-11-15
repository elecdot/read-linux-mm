/*! \page example A Regular Example

This page introduces:
Page frame（物理页框）是物理内存中的固定大小块。内核把整个物理 RAM 看作由这些页框组成的集合，它们作为容器存放页面数据（来自进程、内核或文件缓存）。在 Linux 2.4 中，一个页框通常为 4KB。
*/

---
related:
- "[Zone Allocator](../00-concepts/EXAMPLE.md)"
tags:
- memory-management
- physical-memory
sources:
- "[include/linux/mm.h](/linux/include/linux/mm.h)"
- "[Ref book](https://url)"
---

# Page Frame

## In a Word

Page frame（物理页框）是物理内存中的固定大小块。内核把整个物理 RAM 看作由这些页框组成的集合，它们作为容器存放页面数据（来自进程、内核或文件缓存）。在 Linux 2.4 中，一个页框通常为 4KB。

## Why This Concept

理解 page frame 非常重要，因为它是内存管理子系统操作的最小物理内存单位。内核需要分配内存时，会查找并分配一个或多个空闲页框。内存映射、交换（swapping）以及缓存等机制都围绕着这些页框的管理展开。

## Deep Dive

系统中的每个页框都由一个 `struct page` 表示。内核维护了一个由这些结构体组成的数组，称为 `mem_map`，相当于所有物理页框的“数据库”。通过它，内核可以快速判断任一页框的状态——例如是否空闲、被哪个进程占用、是否属于 buffer cache 等。`struct page` 是物理内存管理的基石。
