---
related:
- "[Topic Link](/path/to/topic.md)"
tags:
- daily-log
---

# 2025-11-22: 第一次笔记尝试和基本概念理解

## Progress Today

- DONE: 读do_page_fault()的基本框架并注释


## Findings
- do_page_fault()函数在arch/i386/mm/fault.c;
- 入口在arch/i386/kernel/entry.S

- 页面失效分为保护性失效和有效性失效，内核调用均使用`do_page_fault()`;
- 有效性失效：P=0出发的中断，通过`handle_mm_fault()`找到缺页装入物理内存;
- 保护性失效：非法访问引起的;

- `vm_area_struct`结构体中的`vm_flags`域用于区别失效类型，并对应不同的处理函数;

- good area之前尚未开始区分 fault 类型，而是在做合法性判断：
- 若发生在中断上下文或当前任务无内核线程（mm）进入分支`no_context`;
- 高地址访问（≥ TASK_SIZE）需检查是否 vmalloc 区，否则非法;
- 根据 `find_vma()` 判断 fault 地址是否落在有效的虚拟内存区域;
- 若属于栈区域，结合 `VM_GROWSDOWN` 判断是否允许下扩;
- 检查用户态栈访问是否超过 `esp + 32`（stack overflow 检查）;
- 如需要扩栈，通过 `expand_stack()` 尝试更新 vma;

- 可以下载插件**ASM Code Lens**高亮`.S`文件的代码;


## Blockers
No Blockers


## Next Steps

- TODO: 读good area，bad area和`no_context`分支的详细逻辑
- TODO：读`vm_area_struct`结构体


