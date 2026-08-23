# Configuration Model

卡片编辑页的用户可见分组、内容密度/宽度策略边界和当前回归清单见 [CARD_CONFIGURATION.md](CARD_CONFIGURATION.md)。

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

当前使用 `JsonConfigStore` 保存 schema 28 JSON。已知字段包括 `schemaVersion`、`storage.root`、`settings`、`cards` 和 `workspace.placements`；Todo 条目从 0.2.1 起保存在独立的 `todos/<card-id>.json`，主配置只保留 Todo Card 元数据和偏好。旧配置中的 `cards[].todo.items` 仍可兼容读取，并在下一次保存时迁移到独立文件。v1 -> v22 依次补齐 Card、稀疏布局、Todo、Mapping、系统设置、`widthSpan`、材质、设置页顺序和文件 Card 布局；后续版本继续通过显式迁移演进。写入时读取并保留未知字段。Windows 使用临时文件、`FlushFileBuffers` 和原子替换；发布前保留 `settings.json.bak`，发布失败不会覆盖最后有效配置。

`settings.runAtStartup` 只记录用户意图。Windows 适配层优先在 Task Scheduler 根目录维护当前用户任务 `Desto`：唯一触发器为 `TASK_TRIGGER_LOGON`，`ILogonTrigger::Delay` 留空，运行级别为 LUA，登录类型为 `TASK_LOGON_INTERACTIVE_TOKEN`，动作固定为当前可执行文件加 `--autostart`，工作目录为程序目录；重复启用会比较任务定义而不重写。成功注册任务后清理旧 `HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run` 值，关闭选项和卸载同时删除任务与旧值。部分 Windows 策略会拒绝普通用户创建计划任务；此时适配层保留 `Run` 回退并记录 `startup.zero_delay_task_unavailable.run_fallback`，不修改会影响全部登录启动应用的全局 `StartupDelayInMSec`。任务和回退均失败时不提交设置。

`settings.desktopDoubleClickAction` 可为 `none`、`icons`、`cards` 或 `all`。`settings.taskbarDoubleClickAction` 是开关：`none` 不处理任务栏双击；其他历史值会兼容读取并归一为 `current-display`，双击某个任务栏空白处时只回到该任务栏所在显示器的桌面。任务按钮、开始按钮和通知区域不会触发；不再保存或恢复窗口快照。

`cards` 保存 Card 身份、类型、可见/展开状态、Chrome 入口偏好、外观偏好和类型专属内容；`workspace.placements` 只保存原始布局。当前显示器拓扑与 Projection 不写入配置，启动后由平台适配器重新提供。

Card 外观当前保存 `preset`、`opacity` 和兼容用的 `cornerRadius`。应用级 `settings.globalCardCornerRadius` 是运行时权威值；设置界面提供直角 0 DIP、Windows 12 DIP、macOS 24 DIP 和饱满圆角 32 DIP 四种预设，并同步更新所有 Card。主题仍按 Card 实例保存，当前包含跟随系统、Win11 深色 Mica、浅色 Mica、品牌色和水晶透明五种色块；`system` 预设在系统深浅色变化时只重绘使用它的 Card，水晶透明色块写入 `transparent-white` 与当前底板 Alpha 0.20。

Card 内容偏好按实例保存 `itemSize`、`showItemNames`、`sizeMode`、`widthSpan`、兼容用 `fixedColumns`、`fixedRows` 和 `maximumVisibleRows`。默认跨度 4 投影为 6 个小图标、5 个中图标、4 个大图标或 3 个特大图标；文件 Card 的自适应网格外宽由投影列数和固定槽宽计算，固定网格和列表仍使用跨度轨道。Todo 的小/中/大保存为 4/5/6，并按 Large 文件网格公式投影为 244/299/354 DIP，不使用带 8 DIP 轨道间距的通用跨度外宽公式。因此不能把 `widthSpan` 简化为所有类型和模式完全相同的外框公式。小/中密度请求 Shell 小图标源，大/特大请求 Shell 中图标源，显示缩放不反向改变源档位。`adaptive` 按内容扩缩，`fixed` 使用明确宽高容量；两者都不提供鼠标自由缩放。当前没有数值动画偏好或时间驱动刷新字段。

设置主界面的自绘控件共享一条键盘焦点链。`Tab` / `Shift+Tab` 和方向键遍历，`Enter` / `Space` 激活；`Escape` 先关闭确认框、归档日历、二级菜单或下拉层，再隐藏设置窗口。归档日历由设置窗口自己绘制月份导航、星期头和日期格，不创建系统日期控件。键盘焦点复用控件热点高亮；归档搜索和动态编辑器使用统一的 `WindowsTextInput` 自绘窗口，保留选择、复制、粘贴、撤销、`Ctrl+A` 和 TSF 中文输入法，不创建原生 Edit。

`settings.cardOrder` 只控制设置页左侧 Card 列表顺序，不改变桌面 Placement 或 Z 顺序。长按拖动提交完整且去重的 Card ID 序列；新 Card 追加到末尾，删除时同步移除，旧配置按原 `cards` 数组顺序迁移。Card 的 `visible` 是实例显隐状态，关闭后只销毁该 Card 的 Surface，重新显示时按稳定 Card ID 恢复，不重建其他 HWND。

Application Card 保存 `sortMode` 和 `itemPlacements`。`custom` 使用相对文件名及从零开始的 `column`、`row` 稀疏槽位；名称、大小和项目类型使用升序，修改日期使用降序，名称作为稳定兜底。自动排序只按当前列数生成临时投影，绝不覆盖自定义槽位，因此切回 `custom` 会恢复原位置。

Mapping Card 使用同一套排序投影，但 `itemPlacements` 的键是规范化绝对源路径，确保不同目录中的同名文件可以独立定位。Application 与 Mapping 都单独保存 `presentationMode`；网格/列表切换只改变投影，不改变来源与自定义位置。

Mapping 的文件夹浏览路径栈和临时标题不持久化；重新启动、切换来源模式或删除 Card 时回到来源根。历史归档复用 TodoItem 的现有字段，不新增 schema 字段。归档导出路径与跨度是一次性操作参数，不写入设置。

`workspace.placements` 保存 Placement 的稳定 ID、Card ID、Display Target、相对工作区矩形、层级、横纵锚点和参考工作区。配置当前只接受 schema version 27；旧版本必须通过显式迁移后才能写回，未来版本直接拒绝，不能让旧版本猜测新结构。默认水平锚点是 Left；只有屏幕右边缘或其他 Card 右边缘对齐写入 Right。默认 Target 是创建时的主显示器稳定 ID；离线 Target 不产生 Projection，设置界面将按在线/离线显示器分组，并允许用户明确转移 Target。

`settings.timeZoneOffsetMinutes` 为 `null` 时跟随 Windows，否则保存 UTC 分钟偏移；`settings.language` 使用 `system`、`zh-CN` 或 `en-US`。语言变化会同步刷新设置页、桌面卡片、待办控件、托盘、确认对话框和未命名 Card 的类型默认标题；用户明确修改过的 Card 名称属于内容数据，不自动翻译。`storage.root` 可以从设置界面选择；迁移要求目标为空，文件移动与配置发布作为可回滚事务执行，Card 内部仍只记录相对路径。

设置窗口和原生菜单不保存独立的应用主题开关，默认跟随 Windows `AppsUseLightTheme`。系统主题变化时更新标题栏、背景、中性控件、文字与图标；Card 实例外观色块保持其真实预览颜色，不随设置窗口主题反转。

主配置损坏、无法读取或在写入中断后缺失时，`JsonConfigStore::inspect()` 会分别检查主文件、`.bak1` 和 `.bak2`，不会自动提升或覆盖任何文件。自有恢复界面向用户展示可用备份；用户确认后调用显式的 `load(ConfigSource)` 和 `promoteBackup(ConfigSource)`，先将损坏主文件保留为 `.corrupt-*`，再以原子方式提升所选备份。未来版本错误不会回退到旧备份，以避免静默降级或覆盖新格式。

## Physical Layout

最终文件格式将在存储原型后确定。逻辑上至少区分：

```text
Config/
  settings
  layouts
  todos/
    <card-id>.json
Cache/
Logs/
Updates/
```

物理格式可以借鉴 PixPin 的扁平键和值记录方式，但必须以 Desto 的配置继承、实例身份和迁移需求为准。
