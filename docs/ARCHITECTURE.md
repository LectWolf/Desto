# Architecture

## Goals

架构需要同时支持低资源占用、多显示器、实例级配置和不同 Card 类型，并且让这些能力能够独立演进。

技术决策已在事项 16 固化。本文件描述稳定模块和数据流；正式桌面宿主使用 Win32，卡片内容使用轻量 GDI DIB 渲染后提交到分层窗口，不引入 Qt、WinUI 或 WebView 常驻运行时。高质量渲染后端可以在后续以显式可选方式加入，但不能改变默认资源预算。

## Module Map

```text
Application
  |-- Domain
  |    |-- Card
  |    |-- Workspace
  |    `-- Layout
  |-- Features
  |    `-- concrete card capabilities
  |-- Presentation
  |    |-- desktop surfaces
  |    |-- card chrome
  |    `-- settings UI
  |-- DestoUi
  |    |-- control state and layout
  |    |-- focus and hit testing
  |    `-- immutable draw snapshots and commands
  |-- Platform.Windows
  |    |-- displays and DPI
  |    |-- desktop shell
  |    |-- input and drag/drop
  |    `-- native item metadata
  `-- Storage
       |-- settings
       |-- layouts
       `-- migrations
```

## Card Model

`Card` 是稳定基类。它拥有通用状态和生命周期，但不包含具体内容类型的实现细节。

概念模型：

```text
Card
  |-- identity
  |-- type identity
  |-- lifecycle state
  |-- capabilities
  |-- chrome preferences
  |-- appearance preferences
  `-- content preferences

Concrete Card
  |-- content model
  |-- type-specific commands
  `-- type-specific persistence

WorkspaceLayout 独立保存 Card 的 Placement 和几何位置；Card 不持有 Windows 窗口或显示器对象。
```

继承只表达真正的类型关系。可选行为、外观和入口使用组合模型，避免为每种配置组合创建子类。

### Deferred card extensions

当前版本不提供 Extension Card、包加载器、SDK、C ABI、第三方 DLL 执行或时间驱动 Card 刷新。
这些能力曾用于原型验证，但在交互、资源预算、安全隔离和发布边界完整规划前已从产品骨架撤销。
schema 23 只保留一次性迁移：读取 schema 22 时删除旧扩展 Card、对应 Placement、排序项和扩展设置，
避免旧配置阻止主程序启动。
当前 schema 27 在此基础上增加文件展示入口可见性、归一水平锚点、Todo 宽度档位和实例级 Card 位置锁定，并精确迁移两代旧水晶预设 Alpha；它不恢复任何扩展运行时。

水晶 Card 仍由 Win32 分层窗口提交，但材质与可读内容不共用一张未预乘 RGB 草稿：底板/图标框 coverage、文字与控件的预乘内容、Shell 图标分别合成。圆角填充和实线框共享 Presentation 层的 `RoundedRectSpec` 像素采样，宿主只负责选材质、生成覆盖率并提交最终 DIB。

若未来重新评估插件化，仍应以原子能力注册为边界，而不是让插件直接持有 HWND 或修改宿主状态：

```text
CardDescriptor (id, display name, icon, schema version, capabilities)
        |
        +-- CardFactory      -> domain instance + migration
        +-- CardController   -> commands, validation, content snapshot
        +-- CardRenderer     -> immutable CardView / draw commands
        `-- CardSettings     -> settings sections and hit targets
```

该结构仅是未来设计约束，不是当前稳定接口。重新引入前必须通过独立原型、性能测量、安全模型和 ADR；
不得恢复旧 ABI 或让历史 schema 字段自动演化为新接口。

## Configuration Resolution

Card 的有效配置按以下顺序解析：

```text
application default
        -> card-type default
        -> card instance override
```

实例覆盖拥有最高优先级。更改全局默认只影响没有明确覆盖该属性的实例。

入口可见性是实例配置的一部分。Presentation 根据 Card 的能力与实例偏好共同决定显示内容，不通过具体类型判断堆叠条件分支。

## Stable Seams

第一阶段只保留确定存在变化的接缝：

- Domain 与 Windows 平台能力之间。
- Domain 与持久化之间。
- Presentation 与渲染实现之间。

正式 Presentation 宿主的边界已确定：Win32 窗口生命周期、输入和桌面层级属于 Platform/Presentation 宿主；Card 不持有 HWND。卡片渲染器输出预乘 Alpha 位图，由宿主通过 `UpdateLayeredWindow` 批量提交。DirectComposition 和 Direct2D 原型仅用于比较，不作为常驻卡片窗口的默认合成路径。

`DestoUi` 位于 Presentation 与 Windows Adapter 之间，统一承载控件状态、布局、焦点、命中测试和命令，不暴露 `HWND`。首个稳定切片是 `Dialog` 与 `RecoveryView`：恢复场景生成平台无关规格，`Dialog` 由尺寸和输入事件生成不可变绘制快照及确认/取消命令，`WindowsConfirmationDialog` 只转换窗口消息并以 GDI 绘制。`Surface`、`TextInput`、`List` 和 `Menu` 仍按纵向切片逐步迁入，不预先暴露一组浅转发接口。

文本输入的文本、方向选择区、UTF-16 光标移动、长度限制、替换和撤销由平台无关的 `DestoUi::TextInputModel` 承载。Windows 平台模块 `WindowsTextInput` 是其 Adapter，外部接口只包含创建参数、文本/选区访问、样式、焦点和诊断统计；实现内部封装 TSF `ITextStoreACP`、Windows 输入法 Context、Uniscribe 布局与命中测试、组合范围、剪贴板和 GDI DIB 绘制。设置页使用 Child Adapter，桌面 Card 使用 Popup Adapter；两者不创建原生 Edit，也不把文字、选区或光标状态泄漏给父窗口。Popup 的 DIB/DC 属于输入实例并跨帧复用，尺寸变化或销毁时集中释放。TSF 组合范围留在 Windows Adapter；中文输入、RTL、UTF-16 代理对和高 DPI 必须保持可重复验证。

`presentation::CardView` 是 Domain Card 到宿主渲染之间的不可变视图。它包含标题、类型标识、可见/展开状态、入口可见性、外观值和平台 Adapter 已解析的内容项目；宿主不读取 Card 子类或自行枚举持久化路径。内容图标以共享的预乘 Alpha 像素视图跨 Projection 复用。关闭某个入口只改变 Card 实例偏好，不会改变 Card 的能力或生命周期。

Application Card 的目录所有权、Shell 解析和失败语义记录在 [APPLICATION_CARD.md](APPLICATION_CARD.md)。Explorer 文件交换由独立 OLE Adapter 处理 `CF_HDROP`、`IDataObject` 与 `IDropTarget`；宿主只计算插入槽位和发出命令。Storage Adapter 先完成可回滚移动，再以源/目标内存快照生成同一批项目视图；窗口消息过程不直接修改领域状态或执行零散文件操作。

拖动由 Win32 标题区命中测试发起，窗口边缘始终返回内容命中，不提供鼠标自由缩放。Card 尺寸由实例内容偏好计算：自适应 Custom 使用 4-span 密度基线和稀疏占用边界，拖放扩缩每次跨一列；固定模式使用明确格数。尺寸变化先在离屏位图完成，再以一次 Layered Window 提交更新。`presentation::ResolvePlacementInteraction` 在不依赖 HWND 的情况下完成屏幕边缘、中心和 Card 等距吸附；水平默认 Left，只有屏幕或其他 Card 的右边缘对齐保存 Right。Ctrl 只绕过吸附，8 DIP 安全间隔和工作区夹取仍保留。吸附意图只在拖动期间创建按需的 3 DIP 透明间隔虚线，不保留空闲渲染循环。拖动跨屏时 `WM_MOVING` 不主动选择 DPI；Windows Per-Monitor V2 通过 `WM_DPICHANGED` 提供切换时机和建议矩形，操作结束后再把 Display ID、逻辑矩形、横纵锚点和参考工作区提交给 `ApplicationRuntime::SetPlacement`。

同一显示器内保持 8 DIP 垂直间隔且左边、右边或中心对齐的 Card，由宿主识别为纵向堆叠。宿主在唯一的 Layered Surface 提交边界比较本次与上次可见高度；发生变化就从该 Card 开始移动下方完整 follower 链并保存 Placement。收缩、展开、日历、内容行数、Grid/List 和输入导致的尺寸变化不再由各调用方分别订阅或补事件。启动时根据持久化 Placement 与完整展开尺寸重建该关系，因此不把 HWND 或临时窗口引用写入配置。

`WindowsShellItemCatalog` 根据 Card 的四档内容偏好选择 32、48、64、96 px Shell 原生源帧。CardItem 只携带当前所需的一份预乘 Alpha 位图；跨 Card 移动或实例偏好切换档位时重新解析目标帧，避免长期放大低分辨率图标，也不为尚未选择的档位增加常驻内存。宿主使用预乘 Alpha 双线性采样生成目标 DPI 下的显示像素。

托盘、文本编辑和 Win11 文件紧凑上下文菜单使用 Win32 `HMENU` 与 `TrackPopupMenuEx`，不使用分层窗口或 owner-draw。菜单文本、快捷键列、禁用状态、系统字体、DPI、圆角和深浅色由 Windows 菜单主题负责；Desto 只在菜单弹出前同步系统应用主题。文件命令仍来自 Shell `IContextMenu`；完整菜单转发 `IContextMenu2/3` owner-draw 消息。Explorer 的 XAML 紧凑菜单没有公开宿主 API，平台层只复现动词层级并明确保留“显示更多选项”。设置页、桌面 Card 和输入框的图标字体通过 `WindowsIconFont` 集中解析：存在 `Segoe Fluent Icons` 时优先使用，不存在时回退 Windows 10 自带的 `Segoe MDL2 Assets`。字体能力只在进程内检测一次，图标继续使用灰度抗锯齿，避免 ClearType 子像素在小图标上形成彩色边缘。

默认 Card 与 Todo 文字使用 GDI 离屏 DIB 绘制。所有最终提交到 `WS_EX_LAYERED` 的普通文字和图标 glyph 统一使用 `ANTIALIASED_QUALITY` 灰阶覆盖，禁止 ClearType 的 RGB 子像素值进入预乘 Alpha；不透明的设置窗口和 Tooltip 可以继续使用 ClearType。正式运行时不提供彩色 Emoji 渲染路径，输入框可见内容也由 GDI DIB 提交。

任务栏双击最小化前按 `EnumWindows` 的 top-to-bottom 顺序保存窗口，恢复时以 `SW_SHOWNOACTIVATE` 批量还原、按相同链逐个插入并最后激活原前台窗口，避免异步恢复改变应用之间的相对层级和逐窗触发任务栏注意力闪烁。Desto 创建的展示桌面会话可监听新的前台应用窗口，在恢复旧窗口后把新窗口重新放到最上层。置顶 Card 在前台窗口事件后无条件重新确认 `HWND_TOPMOST`；全屏让位开启且检测到全屏应用时仍保持隐藏。

桌面宿主另有一个隐藏的 `DestoShellLifecycleHost` 原生窗口接收 `TaskbarCreated`、锁屏/解锁和电源恢复消息。Explorer 重启后重新解析 `SHELLDLL_DefView` 所属宿主并修复普通 Card 的 owner；锁屏、休眠和虚拟桌面锚点不允许 Card 显示。卡片在生命周期可见性恢复时只批量切换显示状态，不重写 Z 顺序，避免置顶 Card 被覆盖层压回。

Windows 平台当前提供 `DisplayTopologyProvider` 的两个 Adapter：生产环境的 `WindowsDisplayTopology` 和测试使用的 `MemoryDisplayTopologyProvider`。`DisplayTopologyMonitor` 负责快照差异和防抖，`WindowsDisplayChangeSource` 负责系统消息输入。接口只暴露 `DisplaySnapshot`，不暴露 Win32 句柄、RECT 或 DPI API。

事项 14 的原生原型位于 `prototypes/`，仅作为 Presentation/Platform 宿主验证，不属于正式产品 UI。它消费 `ApplicationRuntime::projections()`，为每个 `PlacementProjection` 创建一个 `WS_EX_LAYERED`、`WS_EX_NOACTIVATE`、点击穿透的 Win32 工具窗口；所有窗口通过 `BeginDeferWindowPos`/`EndDeferWindowPos` 批量定位和显示。进程使用 Per-Monitor-V2 DPI 感知，显示器工作区原点和 Placement 坐标在 DIP 与像素之间集中转换。原型不持有 Card 业务状态，也不把 HWND 泄漏到 Domain/Application 接口。

模块内部可以拥有测试所需的内部接缝，但不得把实现细节扩散为公共接口。出现第二种真实实现前，不创建假想的可插拔体系。

## Event Flow

```text
native input
  -> application command
  -> domain state transition
  -> immutable change result
  -> persistence scheduling
  -> presentation update
```

领域状态变化应先形成明确结果，再由外围模块执行窗口、文件和配置副作用。

### Application Runtime

`ApplicationRuntime` 是 Card 与 Workspace 状态变更的应用层模块。它只公开一个命令入口，应用循环必须串行调用；模块自身不创建线程，也不直接访问窗口、文件系统或平台接口。

MappingCard 的来源切换也经过 `ApplicationRuntime`。领域级 `MappingSourceRegistry` 与 Card 集合一起原子恢复，统一保证外部目录唯一占用；Storage 和 UI 不复制这条规则。Windows 侧用单个 IO completion port 线程监听所有文件夹来源，事件合并后只把 Card ID 投递到宿主消息循环，Shell 枚举和窗口更新不跨线程执行。完整行为见 [MAPPING_CARD.md](MAPPING_CARD.md)。

每条命令返回 `CommandResult`，明确区分：

- 已应用、无变化和被拒绝。
- Card、布局、临时 Projection 和显示器拓扑的变化范围。
- 无需持久化、可延迟合并和必须立即落盘。
- 可供日志记录的诊断信息。

显示器拓扑属于瞬时运行状态。拓扑变化会重新计算 Projection，但不会改写持久化 Placement，也不会请求配置写入。

Card 删除使用两阶段流程：

```text
RequestCardDeletion
  -> 返回副作用预览和一次性 token
  -> 外围 Adapter 完成确认与文件事务
  -> CommitCardDeletion(token) 或 CancelCardDeletion(token)
```

提交前 Card、Placement 和 Projection 均保留；错误或过期 token 不能删除状态。应用 Card 的文件退回桌面由外围文件事务完成，运行时只在成功后提交内存状态。

持久化通过 `CardSnapshot` 值对象跨越 Storage seam。快照包含 Card 的通用状态、实例 Chrome/外观偏好和类型专属内容；`WorkspaceLayout` 作为同一配置快照的一部分保存 Placement。运行时恢复先在临时状态中构造并校验全部 Card 与 Placement，成功后一次替换，失败不会污染当前状态。

### Process Lifecycle

`ApplicationLifecycle` 统一管理进程启动和退出顺序：

```text
Created
  -> begin / acquire single-instance gate
  -> configurationLoaded
  -> runtimeReady / Running
  -> requestShutdown
  -> completeShutdown / Stopped
```

单实例接缝由 Windows 命名 Mutex Adapter 实现，测试使用内存 Adapter。生命周期析构时也会释放已持有的 gate，避免异常路径留下进程锁。诊断只记录固定事件码、等级和序号，并限制内存容量，不记录用户路径或文件内容。

## Multi-display Foundation

- 显示器使用可恢复的稳定身份，不以枚举顺序作为持久化主键。
- 布局模型与当前连接状态分离。
- `WorkspaceLayout` 保存 Placement；`DisplaySnapshot` 只描述当前拓扑；`PlacementProjection` 是临时解析结果。
- 坐标转换集中在显示模块，Card 不直接依赖系统显示器对象。
- Placement 使用相对显示器工作区的 DIP 坐标，DPI、工作区和虚拟桌面坐标在模块接口中具有明确语义。
- 显示器拓扑变化产生布局决策，不直接破坏持久化的原始布局。

## Atomic Feature Delivery

一个功能模块只有在以下内容同时成立时才算完成：

- 领域行为明确。
- 实例配置和默认继承明确。
- Presentation 能反映状态。
- 持久化和迁移明确。
- 失败路径可观察。
- 自动验证覆盖稳定接口。
- 资源成本已测量。
