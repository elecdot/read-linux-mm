---
related:
- "[Address Space](../00-concepts/address-space.md)"
- "[Page Structure](../00-concepts/page-frame.md)"
tags:
- memory-management
- filesystem
- inode
sources:
- "[include/linux/fs.h](/linux/include/linux/fs.h)"
- "[fs/inode.c](/linux/fs/inode.c)"
---
/*! \page inode Inode Structure

This page introduces:
Inode (index node)是 Unix/Linux 文件系统中表示文件或目录的核心数据结构。它存储了文件的元数据（权限、大小、时间戳等）和数据位置信息，但不包含文件名本身。内核通过 inode 管理文件的访问、缓存和内存映射。
*/


# Inode

## In a Word

Inode（索引节点）是 Unix/Linux 文件系统的核心数据结构，代表一个文件或目录。它存储文件的元数据（大小、权限、所有者、时间戳）和数据块位置指针，但**不包含文件名**。内核通过 inode 号快速定位和管理文件。

## Why This Concept

理解 inode 对深入学习内存管理至关重要，因为：

1. **文件与内存的桥梁**：inode 中的 `i_mapping` 指向 `address_space` 结构，后者管理文件在内存中的页面缓存。
2. **页面与文件的绑定**：`struct page` 的 `mapping` 字段指向其所属的 inode，建立了页面与文件的双向关联。
3. **文件缓存机制**：内核利用 inode 的 `i_data` 结构管理文件页面的读写、回写和淘汰。
4. **VFS 层统一接口**：不同文件系统（ext2、ext3、NTFS 等）都通过通用 VFS inode 结构向内核暴露接口。

## Deep Dive

### 内存中的 Inode 结构 (Linux 2.4.18)

在 `struct inode` 中，关键字段包括：

| 字段 | 类型 | 用途 |
|------|------|------|
| `i_ino` | `unsigned long` | inode 号，在文件系统中唯一 |
| `i_dev` | `kdev_t` | inode 所在设备 |
| `i_mode` | `umode_t` | 文件类型和权限（drwx------） |
| `i_nlink` | `nlink_t` | 硬链接计数 |
| `i_uid` / `i_gid` | `uid_t` / `gid_t` | 所有者 UID/GID |
| `i_size` | `loff_t` | 文件大小（字节） |
| `i_atime` / `i_mtime` / `i_ctime` | `time_t` | 访问、修改、改变时间 |
| `i_mapping` | `struct address_space *` | **指向 address_space，管理页面缓存** |
| `i_data` | `struct address_space` | 嵌入的 address_space 结构 |
| `i_sb` | `struct super_block *` | 文件系统的超级块 |
| `i_op` / `i_fop` | 函数指针 | inode 和文件操作集 |
| `i_hash` / `i_list` / `i_dentry` | `struct list_head` | 哈希、缓存链表和目录项链表 |

### 磁盘 Inode vs 内存 Inode

- **磁盘 inode**：存储在文件系统中，格式由具体文件系统定义（ext3、ext2 等）。包含基本的元数据和数据块指针。
- **内存 inode**：`struct inode` 是 VFS 统一抽象，由各文件系统的驱动程序从磁盘 inode 填充。允许内核以统一方式处理所有文件系统。

### Inode 与 Page 的关系

```
inode (struct inode)
  ├── i_mapping → address_space
  │     └── 管理文件页面缓存
  │
  └── i_data (address_space)
        └── 包含文件数据的页面列表

page (struct page)
  └── mapping → inode 的 address_space
      └── 指回所属文件的 inode
```

当文件被读取或写入时，相关的页面通过 `page->mapping` 绑定到 inode，通过 `inode->i_data` 被组织成列表，允许内核：
- 快速定位文件的页面缓存
- 实现缓存一致性（write-back）
- 管理页面生命周期（淘汰、回收）

## Key Operations

- **`inode_hash`/`inode_list`**：维护全局 inode 缓存，快速查找已加载的 inode
- **`read_inode()`**：从磁盘读取 inode 数据并填充内存结构
- **`write_inode()`**：将内存 inode 更改写回磁盘
- **`drop_inode()`**：释放内存中的 inode，可能回写到磁盘
