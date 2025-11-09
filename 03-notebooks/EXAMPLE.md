---
related:
- "[Initial Setup & Documentation](../03-notebooks/EXAMPLE.md)"
- "[DOCUMENTING.md](../DOCUMENTING.md)"
tags:
- daily-log
- setup
---

# 2025-11-08: 初始搭建与文档整理

## Progress Today / 今日进展

- DONE: 克隆 `read-linux-mm` 仓库并浏览整体目录结构。
- DONE: 阅读 `CONTEXT.md` 与 `DOCUMENTING.md` 了解项目目标与约定。
- DONE: 为各文档目录创建初始 `EXAMPLE.md` 模板文件。

## Findings / 发现

- `02-callflows` 中的 YAML 结构可扩展性很好，后续可以写个 Python 脚本解析并用 Graphviz 生成流程图——一个不错的小项目。
- `01-source-map` 的 “Reading Recipe” 形式有助于对函数做结构化笔记，促使保持简洁。

## Blockers / 阻碍

- 暂无。初始化过程顺利。

## Next Steps / 下一步

- TODO: 编写 `00-concepts/zones.md` 概念文档。
- TODO: 开始阅读 `mm/page_alloc.c` 并在 `01-source-map` 中建立对应 source map。
