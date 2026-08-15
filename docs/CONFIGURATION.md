# Configuration Model

## Goals

- 支持全局默认、类型默认和实例覆盖。
- 支持多显示器布局恢复。
- 支持格式演进和可验证迁移。
- 写入中断时不损坏最后一次有效配置。
- 配置结构表达产品概念，不泄露 UI 框架对象。

## Logical Layers

```text
ApplicationSettings
  `-- application-wide defaults

CardTypeDefaults
  `-- defaults supplied by each card type

CardInstanceSettings
  |-- chrome preferences
  |-- appearance preferences
  |-- behavior preferences
  `-- type-specific settings

WorkspaceLayout
  |-- display mappings
  `-- card placements
```

## Resolution Rules

- 实例设置只保存与上层默认不同的覆盖值。
- 删除实例覆盖后，立即重新继承当前上层默认。
- 未知字段在兼容读取时应保留，避免旧版本覆盖新版本信息。
- 配置合并规则必须确定且可单元测试。

## Persistence Principles

- 每个持久化根对象包含格式版本。
- 使用临时文件、刷新和原子替换完成关键写入。
- 迁移必须可重复执行，并保留失败前的有效数据。
- 高频交互先更新内存状态，再合并和延迟持久化。
- 缓存可以删除重建，配置和布局不能依赖缓存才能恢复。

当前原型使用 `JsonConfigStore` 保存版本化 JSON。已知字段为 `schemaVersion` 和 `storage.root`，写入时读取并保留未知字段。Windows 使用临时文件、`FlushFileBuffers` 和原子替换；发布失败会删除临时文件并保留旧配置。

## Physical Layout

最终文件格式将在存储原型后确定。逻辑上至少区分：

```text
Config/
  settings
  layouts
Cache/
Logs/
Updates/
```

物理格式可以借鉴 PixPin 的扁平键和值记录方式，但必须以 Desto 的配置继承、实例身份和迁移需求为准。
