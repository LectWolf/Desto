# Storage And Ownership

## Storage Root

Desto 有一个用户可配置的存储根目录，例如：

```text
D:\DestoData
```

存储根是配置中的绝对路径，因为它是用户选择的外部位置。ApplicationCard 不保存绝对目录，而保存相对于该根目录的路径：

```text
StorageRoot = D:\DestoData
CardPath = cards\application-1
ResolvedPath = D:\DestoData\cards\application-1
```

这样移动整个存储根时，卡片配置不需要逐条改写，布局和内容身份也不会改变。

## Changing The Root

修改存储根不是简单改配置，而是一次迁移：

1. 预检查目标空间、权限和冲突。
2. 锁定涉及的存储操作。
3. 将旧根目录迁移到新根目录，或在跨磁盘时逐项复制并校验。
4. 成功后原子更新存储根配置。
5. 失败时保留旧配置并报告未完成的项目。

迁移过程中不能让 Card 指向一个配置上成功、实际内容不存在的位置。

当前实现由 `StorageRootMigrationService` 负责：`plan` 只接受空目标目录，并拒绝源目录与目标目录互相嵌套；`migrate` 在文件事务成功后读取完整配置，只替换存储根并保存，配置保存失败会逆序恢复已移动项目，不会清空 Card 或 Placement。

## Ownership Policies

| Card type | Source of truth | Default ownership |
| --- | --- | --- |
| ApplicationCard | Desto storage root | Desto manages files |
| MappingCard (folder mode) | One external folder | External folder owns files |
| MappingCard (references mode) | Explicit references | Original locations own files |
| TodoCard | Card data | Desto owns card data |

## Deleting An ApplicationCard

删除 ApplicationCard 必须是可解释的确认流程：

- 明确告知文件会被转移回桌面。
- 显示涉及的项目数量和可能的冲突数量。
- 桌面同名时使用确定且不覆盖的重命名策略。
- 所有移动完成或进入可恢复失败状态后，才删除卡片记录。
- 中断时保留迁移日志，支持继续处理或人工恢复。

MappingCard 删除时不移动外部文件，只删除映射或引用关系。

## External Paths

外部映射目录和引用文件不在 Desto 存储根内，不能强行改写成相对路径。它们需要保存可恢复的外部身份和路径信息；路径格式由 Windows 平台适配模块负责，领域模型不假设具体字符串格式。

## Mapping Exclusivity

一个外部源文件夹在同一 Desto 配置中只能被一个 MappingCard 占用。这个约束由存储/卡片注册模块统一检查，不能依赖界面层阻止重复创建。

## All-card Deletion Confirmation

所有 Card 的删除都必须经过二次确认。确认内容由删除预览提供，至少说明卡片名称、类型、影响项目数量和实际副作用：

- ApplicationCard 会将受管理文件退回桌面，并处理同名冲突。
- MappingCard 只删除映射或引用关系，不移动外部文件。
- TodoCard 只删除待办数据。

Presentation 不得提供绕过删除预览的快捷删除路径。
