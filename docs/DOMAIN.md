# Domain Model

## Card Hierarchy

`Card` 是所有桌面卡片的共同基类。它只承载所有类型都需要的状态，不直接决定内容来源。

```text
Card
  |-- ApplicationCard
  |-- FolderMappingCard
  |-- ReferenceCard
  `-- TodoCard
```

这些名称是当前内部工作名称，最终用户界面中的名称可以独立调整。

### Common Card State

所有 Card 共享：

- 稳定 ID 和类型标识。
- 所属显示器或虚拟工作区。
- 位置、尺寸、层级和可见状态。
- 支持的能力集合。
- 实例级入口偏好。
- 实例级外观偏好。
- 创建、更新和删除生命周期。

### ApplicationCard

ApplicationCard 管理自己的内容目录。将项目加入卡片时，文件可以按照卡片的归属策略移动到该目录。

它表达“这些项目由此卡片管理”，而不是简单展示某个外部目录。

移动、重命名、删除和跨磁盘行为必须在实现前单独定义，不能隐含在拖放代码中。

### FolderMappingCard

FolderMappingCard 映射一个外部文件夹，卡片内容以该文件夹当前状态为准。

- 外部文件夹新增或删除项目时，卡片可以同步变化。
- 卡片不拥有该文件夹的文件。
- 卡片上的操作是否允许反向修改文件夹，需要作为实例策略明确配置。

### ReferenceCard

ReferenceCard 保存用户明确加入的文件引用，不移动原文件。它适合表达“把这些项目放在一起看”，而不是表达文件归属。

- 每个项目保存稳定引用信息和当前可解析状态。
- 原文件移动或删除后，卡片应能表达失效状态。
- 引用集合与外部目录内容不自动等同。

如果后续验证表明 FolderMappingCard 与 ReferenceCard 的用户体验可以统一，可以保留同一个 Mapping 入口，在内部使用不同的来源策略；不应因此复制两套 Card 基础实现。

### TodoCard

TodoCard 保存待办项目及其完成状态。它复用 Card 的布局、外观、入口和多屏能力，但内容模型不依赖文件系统。

## Source And Ownership

卡片类型和内容来源是两个相关但不同的概念：

```text
Card
  |-- common layout / appearance / chrome
  `-- content source and ownership policy
```

内容来源决定项目从哪里来，归属策略决定加入、移动、删除和失效如何处理。把两者拆开，可以避免“映射文件夹”和“引用集合”重复实现全部 Card 行为。

## Naming

内部代码统一使用 `Card`，避免继续使用 `Box`、`Drawer` 或 `Container` 混称。用户可见名称、类型图标和描述属于 Presentation 与本地化层，不反向决定领域类型名。
