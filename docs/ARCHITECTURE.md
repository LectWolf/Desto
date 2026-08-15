# Architecture

## Goals

架构需要同时支持低资源占用、多显示器、实例级配置和不同 Card 类型，并且让这些能力能够独立演进。

技术框架尚未决定。本文件描述稳定模块和数据流，不绑定 Qt、Win32 或具体目录命名。

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
  `-- appearance preferences

Concrete Card
  |-- content model
  |-- type-specific commands
  `-- type-specific persistence

WorkspaceLayout 独立保存 Card 的 Placement 和几何位置；Card 不持有 Windows 窗口或显示器对象。
```

继承只表达真正的类型关系。可选行为、外观和入口使用组合模型，避免为每种配置组合创建子类。

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

Windows 平台当前提供 `DisplayTopologyProvider` 的两个 Adapter：生产环境的 `WindowsDisplayTopology` 和测试使用的 `MemoryDisplayTopologyProvider`。`DisplayTopologyMonitor` 负责快照差异和防抖，`WindowsDisplayChangeSource` 负责系统消息输入。接口只暴露 `DisplaySnapshot`，不暴露 Win32 句柄、RECT 或 DPI API。

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
