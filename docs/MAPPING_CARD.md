# MappingCard

MappingCard 展示不归 Desto 所有的外部内容。删除 Card 只删除映射关系，不移动或删除外部文件。

## Source modes

- Empty：没有来源。拖入现有文件会建立引用，不移动文件。
- Folder：唯一映射一个绝对目录，目录内容变化会实时刷新 Card。
- References：保存一组稳定引用 ID 和绝对路径。目标移动或删除后，项目保留在 Card 中并显示 Missing 状态。

Folder 和 References 互斥。切换来源由 `ApplicationRuntime` 串行执行；只有新状态完整校验成功后才替换旧状态。

## Folder ownership

同一规范化目录只能被一个 MappingCard 占用。`domain::MappingSourceRegistry` 与 Runtime 状态一起恢复、更新和删除，界面层不参与维护该约束。恢复配置遇到重复目录时会整体拒绝，不留下半恢复状态。

Folder 模式的 `allowsSourceMutation` 是实例级开关：

- 开启时，拖入项目通过可回滚文件事务移动到来源目录；名称冲突使用 `name (n).ext`。
- 关闭时，不为该 Card 注册文件 Drop Target，也不允许从 Card 发起可能移动源文件的拖出。

References 模式只增加引用，拖放结果为 Copy，不移动原项目。

## Live refresh

`WindowsDirectoryChangeSource` 使用一个 IO completion port 工作线程监听全部 Folder MappingCard。每个目录持有一个文件句柄和 4 KiB 缓冲；60 ms 静默窗口合并重复事件，并按 Card ID 去重。

工作线程只提交 Card ID。`WindowsDesktopHost::requestCardItemsRefresh` 将请求合并后投递到宿主线程，目录枚举、Shell 元数据/图标解析、内容尺寸计算和 Layered Window 提交均在宿主线程完成。

References 不持有目录监听资源；每次刷新逐项检查目标，Missing 项目不会从引用集合中自动删除。
