# Domain Model

## Card Hierarchy

`Card` 是所有桌面卡片的共同基类。它只承载所有类型都需要的状态，不直接决定内容来源。

```text
Card
  |-- ApplicationCard
  |-- MappingCard
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

ApplicationCard 管理自己的内容目录。卡片只记录相对于 Desto 存储根的目录路径；将项目加入卡片时，文件可以按照卡片的归属策略移动到该目录。

它表达“这些项目由此卡片管理”，而不是简单展示某个外部目录。

删除 ApplicationCard 时，默认操作是先明确提示用户，再将受管理文件退回桌面。桌面存在同名项目时必须生成不覆盖原文件的重命名结果。

移动、重命名、删除和跨磁盘行为必须由存储事务模块负责，不能隐含在拖放代码中。

### MappingCard

FolderMappingCard 和 ReferenceCard 合并为 MappingCard。MappingCard 根据内容来源拥有三种状态：空、单一文件夹来源、显式引用集合。

- 空状态和单一文件夹来源可以在界面上显示为“文件夹映射”。
- 文件夹来源会实时反映外部文件夹，且默认允许从卡片反向修改源文件夹。
- 一个源文件夹不能同时被多个 MappingCard 占用。
- 反向修改仍是实例策略，可以按卡片关闭。

显式引用集合保存用户主动加入的文件引用，不移动原文件；原文件移动或删除后，卡片应能表达失效状态。内部使用 `References` 状态，用户可见名称暂不决定。

### TodoCard

TodoCard 保存待办项目及其完成状态。它复用 Card 的布局、外观、入口和多屏能力，但内容模型不依赖文件系统。

## Source And Ownership

卡片类型和内容来源是两个相关但不同的概念：

```text
Card
  |-- common layout / appearance / chrome
  `-- content source and ownership policy
```

内容来源决定项目从哪里来，归属策略决定加入、移动、删除和失效如何处理。MappingCard 只替换内容来源策略，不复制全部 Card 行为。

## Naming

内部代码统一使用 `Card`，避免继续使用 `Box`、`Drawer` 或 `Container` 混称。用户可见名称、类型图标和描述属于 Presentation 与本地化层，不反向决定领域类型名。
