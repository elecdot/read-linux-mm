---
related:
  - "[Zone-based Memory Management](../00-concepts/zone-based-memory-management.md)"
  - "[Address Translation](../00-concepts/address-translation.md)"
  - "[Physical-Virtual Memory](../00-concepts/phys-virt.md)"
tags:
  - memory-management
  - highmem
  - address-space
  - kernel-mapping
sources:
  - "[include/linux/mmzone.h](../../linux/include/linux/mmzone.h)"
  - "[include/linux/mm.h](../../linux/include/linux/mm.h)"
  - "[Linux 2.4 内存管理 - 高端内存映射]"
---

/*! \page highmem_kernel_mapping Kernel Mapping of High-Memory Page Frames

高端内存（High Memory）是指在 32 位 x86 系统上，超过 896MB 的物理内存。由于内核地址空间限制（4GB 线性地址空间且需要保留 1GB 供内核），无法直接映射到内核虚拟地址空间。因此，内核采用**动态映射**技术，仅在需要时将高端内存页框临时映射到内核虚拟地址空间的固定区域（`ZONE_HIGHMEM`），以便访问这些页框。
*/

# Kernel Mapping of High-Memory Page Frames

## In a Word

高端内存（High Memory）是指在 32 位 x86 系统上，超过 896MB 的物理内存。由于内核地址空间限制（4GB 线性地址空间且需要保留 1GB 供内核），无法直接映射到内核虚拟地址空间。因此，内核采用**动态映射**技术，仅在需要时将高端内存页框临时映射到内核虚拟地址空间的固定区域（`ZONE_HIGHMEM`），以便访问这些页框。

## Why This Concept

在现代 32 位系统中，物理 RAM 可达 4GB，但内核虚拟地址空间只有 1GB（通常 0xC0000000 ~ 0xFFFFFFFF）。这导致并非所有物理内存都能被内核直接访问。对于超出 896MB 的物理内存：

- **DMA 区域** (< 16 MB)：用于 ISA 设备，直接映射到内核地址空间
- **NORMAL 区域** (16 MB ~ 896 MB)：直接线性映射到内核地址空间的第四 GB
- **HIGHMEM 区域** (> 896 MB)：无法直接映射，需要特殊处理

理解高端内存映射机制对掌握 Linux 内存管理的完整图景至关重要，特别是在优化缓存管理和页面交换时。

## Deep Dive

### 硬件约束

80x86 架构存在两个关键约束：

1. **DMA 限制**：老旧的 ISA 设备只能访问 16MB 以下的物理内存。
2. **地址空间限制**：32 位系统的线性地址空间总计4GB。

### 内核地址空间分布

在 Linux 2.4 x86 32 位系统中的虚拟地址空间布局：

```
0xFFFFFFFF +------------------+
           |  固定映射区        |
           | (FIXMAP/HIGHMEM) | ~4MB
           | 高端内存临时映射    |
           | APIC、BIOS等      |
0xFFC00000 +------------------+
           |                  |
           |  ZONE_NORMAL     |
           | (直接内核映射)     | ~896MB
           |  线性映射区       |
           |  (可直接访问)     |
0xC0000000 +------------------+
           |                  |
           |   用户空间        |
           | (进程虚拟地址)     | ~3GB
           |                  |
0x00000000 +------------------+
```

**分布说明**：

- **0x00000000 ~ 0xBFFFFFFF**（3GB）：用户空间 - 每个进程有独立副本
- **0xC0000000 ~ 0xFFFFFFFF**（1GB）：内核空间 - 全局共享
  - **0xC0000000 ~ (0xC0000000 + NORMAL_SIZE)**：`ZONE_NORMAL` 的线性映射（~896 MB）
  - **0xFFC00000 ~ 0xFFFFFFFF**：固定映射区（高端内存临时映射、APIC、BIOS 等）

### 高端内存的动态映射

当内核需要访问高端内存中的页框时：

1. **申请映射**：调用 `kmap()` 或 `kmap_atomic()`，从固定映射区获取一个临时虚拟地址
2. **建立页表项**：设置临时虚拟地址到目标物理页框的页表映射
3. **访问数据**：通过临时虚拟地址读写页框内容
4. **解除映射**：调用 `kunmap()` 或 `kunmap_atomic()`，释放临时映射槽位

### 数据结构

在 [mmzone.h](../../linux/include/linux/mmzone.h) 中定义：

```c
typedef struct zone_struct {
    // ...
    spinlock_t  lock;
    unsigned long free_pages;        // 区域内的空闲页数
    // ...
} zone_t;

#define ZONE_DMA      0
#define ZONE_NORMAL   1
#define ZONE_HIGHMEM  2
#define MAX_NR_ZONES  3
```

- **ZONE_DMA**：< 16 MB，可供 ISA 设备 DMA 使用
- **ZONE_NORMAL**：16 MB ~ 896 MB，内核直接访问
- **ZONE_HIGHMEM**：> 896 MB，需动态映射

### 实际意义

- **页面缓存**：文件 I/O 缓冲可以存储在高端内存中，减少 `ZONE_NORMAL` 压力
- **用户进程**：用户页面优先分配在高端内存
- **内核操作**：对高端内存的临时访问通过 `kmap()` 完成，映射后自动释放

## Related Concepts

- [Zone-based Memory Management](zone-based-memory-management.md)：内存区域划分策略
- [Address Translation](address-translation.md)：地址转换机制
- [Physical-Virtual Memory](phys-virt.md)：物理与虚拟地址关系
