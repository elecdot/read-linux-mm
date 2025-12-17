---
related:
  - "[Reserved Page Pool](../00-concepts/reserved-page-pool.md)"
  - "[Zone-based Memory Management](../00-concepts/zone-based-memory-management.md)"
tags:
  - memory-management
  - sysctl
  - tuning
  - buffer-cache
  - swap
  - readahead
sources:
  - "[kernel/sysctl.c](../../linux/kernel/sysctl.c)"
  - "[fs/buffer.c](../../linux/fs/buffer.c)"
  - "[mm/swap.c](../../linux/mm/swap.c)"
  - "[mm/memory.c](../../linux/mm/memory.c)"
  - "[mm/filemap.c](../../linux/mm/filemap.c)"
---

# Linux 2.4.18 MM Sysctl Playground

## In a Word

Linux 2.4.18 提供了一组有趣的 `/proc/sys/vm/` 参数，允许你在运行时调整内存管理行为。这些参数控制缓冲区刷新策略、交换行为、页表缓存、预读策略等关键机制，是理解和调优系统内存行为的好工具。

## Interactive Parameters

### 1. `bdflush` - 缓冲区后台刷新控制（最有趣！）

**路径**：`/proc/sys/vm/bdflush`

**用途**：控制脏缓冲区何时被写回磁盘。

**参数格式**：9 个整数，格式为：
```bash
cat /proc/sys/vm/bdflush
40 0 0 0 5 30 60 0 0
```

**每个参数的含义**：

| 索引 | 参数名 | 默认值 | 范围 | 说明 |
|------|--------|--------|------|------|
| 0 | `nfract` | 40 | 0-100 | 脏缓冲区达到缓存的百分比时，异步唤醒 bdflush（40% = 触发异步回写） |
| 1 | `dummy1` | 0 | 10-50000 | 已弃用（兼容性保留） |
| 2 | `dummy2` | 0 | 5-20000 | 已弃用（兼容性保留） |
| 3 | `dummy3` | 0 | 25-20000 | 未使用 |
| 4 | `interval` | 5*HZ | 0-10000*HZ | kupdate 线程刷新的间隔时间（单位 jiffies，默认 5 秒） |
| 5 | `age_buffer` | 30*HZ | 1*HZ-6000*HZ | 缓冲区在被强制刷新前的最大年龄（默认 30 秒） |
| 6 | `nfract_sync` | 60 | 0-100 | 脏缓冲区达到缓存的百分比时，**同步**等待刷新（60% = 阻塞进程） |
| 7 | `dummy4` | 0 | 0 | 未使用 |
| 8 | `dummy5` | 0 | 0 | 未使用 |

**默认值含义**：
```
脏缓冲 < 40% of cache  → bdflush 异步执行，不阻塞应用
脏缓冲 40-60%         → bdflush 正在工作，应用继续
脏缓冲 > 60%          → 应用被阻塞，必须等待 bdflush 完成
```

**试玩例子**：

```bash
# 查看当前设置
cat /proc/sys/vm/bdflush

# 让系统更激进地刷新缓冲（防止脏数据堆积）
# 改为 30% 触发异步，50% 同步阻塞
echo "30 0 0 0 5 30 50" > /proc/sys/vm/bdflush

# 让系统更保守（允许更多脏数据，减少 I/O 开销，但掉电风险高）
echo "60 0 0 0 10 60 80" > /proc/sys/vm/bdflush

# 缩短 kupdate 间隔，让周期性刷新更频繁（每 2 秒一次）
echo "40 0 0 0 2 30 60" > /proc/sys/vm/bdflush
```

---

### 2. `overcommit_memory` - 内存过度承诺策略

**路径**：`/proc/sys/vm/overcommit_memory`

**用途**：控制是否允许应用请求超出物理内存的虚拟内存。

**可选值**：
- `0`（默认）：启发式过度承诺。内核根据可用内存估计能分配多少
- `1`：总是允许 overcommit（激进）
- `2`：不允许 overcommit（保守）

**试玩**：

```bash
# 查看当前值
cat /proc/sys/vm/overcommit_memory

# 设为激进模式（允许应用分配超过物理内存）
echo 1 > /proc/sys/vm/overcommit_memory

# 改回保守模式
echo 0 > /proc/sys/vm/overcommit_memory
```

---

### 3. `kswapd` - 页面回收守护进程参数

**路径**：`/proc/sys/vm/kswapd`

**用途**：控制 kswapd 的行为。

**参数格式**：通常是 2-3 个整数，内容依赖于实现。

```bash
cat /proc/sys/vm/kswapd
512 8 8
```

可能的含义：
- 基础睡眠间隔
- 每次扫描的页数
- 其他调度参数

**注意**：具体参数含义在 2.4.18 中不够明确，需要阅读源码。

---

### 4. `pagetable_cache` - 页表缓存水位

**路径**：`/proc/sys/vm/pagetable_cache`

**用途**：控制释放的页表项何时被回收。

**参数格式**：2 个整数（低水位 和 高水位）
```bash
cat /proc/sys/vm/pagetable_cache
25 50
```

**含义**：
- 当缓存页表项 < 25 时，停止回收
- 当缓存页表项 > 50 时，开始回收

**试玩**：

```bash
# 更激进地缓存页表（减少分配开销，增加内存占用）
echo "50 100" > /proc/sys/vm/pagetable_cache

# 更节省内存
echo "10 30" > /proc/sys/vm/pagetable_cache
```

---

### 5. `page-cluster` - 交换聚类大小

**路径**：`/proc/sys/vm/page-cluster`

**用途**：控制交换 I/O 时一次性交换多少页。

**默认值**：
- 小内存系统（< 16MB）：2（交换 2^2 = 4 页）
- 大内存系统（>= 16MB）：3（交换 2^3 = 8 页）

**试玩**：

```bash
# 查看当前值
cat /proc/sys/vm/page-cluster

# 增加到 4（交换 16 页，减少 I/O 次数但延迟可能增加）
echo 4 > /proc/sys/vm/page-cluster

# 减到 1（交换 2 页，更频繁的 I/O 但延迟更短）
echo 1 > /proc/sys/vm/page-cluster
```

**实际影响**：修改这个参数会影响交换吞吐量与响应延迟的平衡。

---

### 6. `min-readahead` & `max-readahead` - 文件预读范围

**路径**：
- `/proc/sys/vm/min-readahead`
- `/proc/sys/vm/max-readahead`

**用途**：控制文件读取时的预读窗口大小。

**默认值**：
- `min-readahead`：3（最少预读 3 页）
- `max-readahead`：31（最多预读 31 页）

**试玩**：

```bash
# 查看当前值
cat /proc/sys/vm/min-readahead
cat /proc/sys/vm/max-readahead

# 禁用预读（最小值为 0 会有特殊效果）
echo 0 > /proc/sys/vm/min-readahead
echo 3 > /proc/sys/vm/max-readahead

# 激进预读（大量顺序读时有利）
echo 8 > /proc/sys/vm/min-readahead
echo 63 > /proc/sys/vm/max-readahead
```

---

## 真实使用场景

### 场景 1：数据库服务器（写入密集）

```bash
# 更激进地写回脏缓冲，防止突然写入波澜
echo "30 0 0 0 3 20 50" > /proc/sys/vm/bdflush

# 页表缓存更积极
echo "50 100" > /proc/sys/vm/pagetable_cache

# 交换聚类增大（如果有交换的话）
echo 4 > /proc/sys/vm/page-cluster
```

### 场景 2：文件服务器（读取密集）

```bash
# 更激进的预读
echo 10 > /proc/sys/vm/min-readahead
echo 63 > /proc/sys/vm/max-readahead

# 允许更多脏数据堆积（减少 I/O 开销）
echo "50 0 0 0 10 60 80" > /proc/sys/vm/bdflush
```

### 场景 3：实时系统（低延迟）

```bash
# 快速刷新缓冲，防止长延迟
echo "20 0 0 0 2 10 40" > /proc/sys/vm/bdflush

# 减少预读（防止缓存导致的延迟）
echo 1 > /proc/sys/vm/min-readahead
echo 8 > /proc/sys/vm/max-readahead

# 保守的内存过度承诺
echo 0 > /proc/sys/vm/overcommit_memory
```

---

## 试验指南

### 查看所有 MM 相关参数

```bash
ls -la /proc/sys/vm/
```

### 获取当前参数状态

```bash
for param in bdflush overcommit_memory kswapd pagetable_cache page-cluster min-readahead max-readahead; do
    echo "=== $param ==="
    cat /proc/sys/vm/$param
done
```

### 持久化设置（编辑 `/etc/sysctl.conf`）

```bash
# 永久修改（重启后生效）
vm.bdflush = 30 0 0 0 5 30 50
vm.overcommit_memory = 1
vm.page-cluster = 4
vm.min-readahead = 3
vm.max-readahead = 31
```

然后运行：
```bash
sysctl -p
```

### 监控效果

```bash
# 查看缓冲区状态
cat /proc/meminfo | grep -i buffer

# 查看交换使用
cat /proc/swaps

# 实时监控 I/O
vmstat 1
```

---

## 警告与注意事项

⚠️ **不要随意修改这些参数**：
- 过度激进的 `bdflush` 设置会导致频繁 I/O，降低吞吐量
- 过度保守的设置会导致内存压力过大，触发 OOM
- `page-cluster` 过大会导致交换延迟过长
- `overcommit_memory = 1` 在掉电时风险极高

✅ **最佳实践**：
1. 在测试环境先验证参数效果
2. 使用 `vmstat`、`iostat`、`top` 等工具监控变化
3. 记录原始值，便于回滚
4. 仅在充分测试后才编辑 `/etc/sysctl.conf`

---

## 相关概念

- [Reserved Page Pool](reserved-page-pool.md)：理解 kswapd 和水位机制的前提
- [Zone-based Memory Management](zone-based-memory-management.md)：zone 层级的内存管理
- [Page Flags & State Management](page-flags.md)：页面状态标志

---

## 进阶阅读

查看源码中 bdflush 的触发条件：
```bash
# 搜索 bdflush 的实际调用点
rg "flush_dirty_buffers|bdflush" /path/to/linux/fs/buffer.c
```

了解 kswapd 的行为：
```bash
# 查看 kswapd 源码
grep -r "kswapd" /path/to/linux/mm/ | head -20
```
