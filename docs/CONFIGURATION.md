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
  `-- card placements (relative display work-area DIP coordinates)
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

当前原型使用 `JsonConfigStore` 保存 schema 4 JSON。已知字段包括 `schemaVersion`、`storage.root`、`cards` 和 `workspace.placements`；Card 的存储目录仍记录为相对于 `storage.root` 的路径。v1 -> v2 迁移会显式补齐 `cards` 字段，v2 -> v3 增加可选的 Card 内容偏好和应用项目顺序，v3 -> v4 将线性顺序转换为独立的自定义槽位；缺失字段使用中等图标、隐藏名称和自适应尺寸。写入时读取并保留未知字段。Windows 使用临时文件、`FlushFileBuffers` 和原子替换；发布前保留 `settings.json.bak`，发布失败不会覆盖最后有效配置。

`cards` 保存 Card 身份、类型、可见/展开状态、Chrome 入口偏好、外观偏好和类型专属内容；`workspace.placements` 只保存原始布局。当前显示器拓扑与 Projection 不写入配置，启动后由平台适配器重新提供。

Card 外观当前保存 `preset`、`opacity` 和 DIP 单位的 `cornerRadius`。预设至少包含纯白、透明黑和珠宝炫彩；`jewel` 是珠宝炫彩的稳定名称，旧的 `pearl-pink` 作为兼容别名使用同一渲染。设置界面必须用可见色块展示预设，文字只作为辅助名称或无障碍标签，不能用文本列表代替色块选择。圆角半径按 Card 实例保存，并限制在 0-128 DIP。

Card 内容偏好按实例保存 `itemSize`、`showItemNames`、`sizeMode`、`fixedColumns` 和 `fixedRows`。图标规格使用 `small`、`medium`、`large`、`extraLarge` 四个稳定值，并同时决定图标像素、槽位宽高和名称区域；关闭名称后槽位高度同步缩短。`adaptive` 按当前卡片宽度计算列数，`fixed` 使用明确的列数、行数和容量边界。

Application Card 保存 `sortMode` 和 `itemPlacements`。`custom` 使用相对文件名及从零开始的 `column`、`row` 稀疏槽位；名称、大小和项目类型使用升序，修改日期使用降序，名称作为稳定兜底。自动排序只按当前列数生成临时投影，绝不覆盖自定义槽位，因此切回 `custom` 会恢复原位置。

`workspace.placements` 保存 Placement 的稳定 ID、Card ID、Display Target、相对工作区矩形和层级。当前只接受 schema version 4；旧版本必须通过显式迁移后才能写回，未来版本直接拒绝，不能让旧版本猜测新结构。

主配置损坏、无法读取或在写入中断后缺失时，读取会尝试最后有效的 `.bak` 文件，并在 `ApplicationConfig::recoveredFromBackup` 中标记来源。未来版本错误不会回退到旧备份，以避免静默降级或覆盖新格式。

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
