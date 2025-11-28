---
related:
- "[free_area_init()](../linux/mm/page_alloc.c)"
tags:
- concept
- protection
sources:
- "ref book S11.10.1 paragraph1"
---

# 可换页区 & 不可换页区

## In a Word

有一部分不可换页区负责存内核和静态数据,保证这些数据的完整,
除此之外大部分都是存用户进程和内核动态分配的页面的动态内存.
In this case, `page->flags = PG_reserved` (@see mm.h/typedef struct page -> flags)

## Why This Concept

-

## Deep Dive

Linux系统把物理内存分为可换页区和非换页区。在物理内存的低端是非换页区,用来
保存内核正文和启动时分配的内核数据以及静态分配的数据。在系统初始化时内核就将这些
页框标记为不可对换的(见mm/page_ alloc.c 中的函数 free area_init ()),以防止其
他页面失效时系统的误操作,从而导致将保留内核信息的页换出内存。除此之外的大部分物
理内存都作为可换页区,它用来存放所有用户进程的页面和内核动态分配的页面,这部分就
是动态内存。
