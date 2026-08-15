# Desto Delivery Roadmap

本路线把工作拆成一次可以完整验收的交付事项。事项编号是当前进度，不代表日历百分比；一项通常在一次工作回合内完成，也允许因为新增需求、验收失败或必须等待外部验证而在同一编号继续处理。

## Progress Rules

- 所有面向用户的阶段更新和最终回复使用 `（N/34）事项名`。
- 默认一次完成当前事项；只有验收未通过、出现新的底层缺口、用户新增范围或需要真实 Windows/硬件验证时，才继续使用同一个编号。
- 完成当前事项的退出条件后，下一次工作进入下一个编号；不按回复次数、提交数量或代码行数跳号。
- 用户临时新增功能先插入最早的依赖位置，重新计算总数并说明进度变化。
- 事项之间存在依赖时，不提前实现会掩盖底层问题的上层功能。

## Foundation

### 01. Project skeleton and rules [x]

初始化 CMake、Git、目录职责、Agent 规则、领域术语和文档入口。退出条件是新模块可以独立加入、构建和测试。

### 02. Card domain model [x]

建立 Card 基类、ApplicationCard、MappingCard、TodoCard、能力和实例配置。退出条件是类型不依赖 UI 或 Windows，非法状态有明确错误。

### 03. Ownership and deletion semantics [x]

完成删除预览、所有 Card 二次确认、应用文件退回桌面、Mapping 唯一映射和引用/目录归属规则。退出条件是副作用可解释且失败不会静默丢文件。

### 04. File transactions and storage-root migration [x]

完成同盘重命名、跨盘复制回滚、存储根迁移和配置失败反向恢复。退出条件是文件移动、目标冲突、嵌套路径和迁移失败均有可测试结果。

### 05. Versioned atomic configuration [x]

完成 JSON schema version、未知字段保留、Windows 原子替换和临时文件清理。退出条件是写入中断不破坏最后有效配置。

### 06. Workspace placement model [x]

完成 DisplayTarget、Placement、WorkspaceLayout、断屏 Fallback Projection、重连恢复和工作区夹取。退出条件是布局不依赖显示器枚举序号，临时投影不改写原始位置。

### 07. Windows display identity adapter [x]

完成 `DisplayTopologyProvider`、内存 Adapter 和 Windows `monitorDevicePath` 适配器。退出条件是稳定身份、工作区、DPI、主屏状态和异常拓扑可明确表示。

### 08. Topology change listener [x]

监听 `WM_DISPLAYCHANGE`、DPI 和显示器相关系统变化，加入防抖、重复事件合并和快照差异计算。退出条件是拓扑抖动不会产生重复布局写入，变化能触发新的 Projection 计算。

### 09. Application state and command runtime [x]

建立应用状态、命令、状态变更结果和事件分发；把 UI、文件系统和平台副作用放到外围 Adapter。退出条件是无 UI 测试可以驱动 Card/Workspace 的完整状态变更。

### 10. Complete Card/Workspace persistence [x]

把 Card 内容、实例偏好、Placement、默认值和实例覆盖写入同一可恢复快照。退出条件是创建、修改、删除、重启恢复均不丢失身份和用户明确设置。

### 11. Schema migration and recovery [x]

实现显式 schema 迁移链、未来版本拒绝策略、损坏配置恢复、备份/回滚和迁移日志。退出条件是旧版本升级、未知字段、损坏文件和中断迁移都有确定结果。

### 12. Process lifecycle and diagnostics [x]

完成单实例、启动顺序、正常退出、托盘退出、异常退出、日志等级和诊断信息。退出条件是退出后无残留进程，失败可以定位且不泄露用户文件内容。

### 13. Headless integration and performance baseline [x]

建立无 UI 端到端场景和 Empty/Typical/Heavy/Hidden 基线，记录启动、内存、线程、CPU、显隐提交和拓扑变化成本。退出条件是 Debug/Release 均有可重复数据，后续 UI 比较使用同一基线。

## UI And Desktop Host

### 14. Native UI prototype [x]

用 Win32/DirectComposition 或同等原生路线做最小可见原型，消费真实 Projection，验证透明窗口、同步显隐、多 DPI 和桌面层级。退出条件是原型可重复展示多个 Card，且有性能数据。

这是第一次开始制作可见界面，但仍是技术原型，不是正式产品 UI。

### 15. Alternative UI prototype

使用仍具现实可行性的候选框架做同等原型，复用相同素材、Card 数量、显示器拓扑和交互场景。退出条件是两种路线可以公平比较，而不是凭感觉选型。

### 16. UI technology and host ADR

比较内存、启动、显隐、输入延迟、DPI、桌面层级、开发成本和发布体积，固化 UI 技术、渲染模型、窗口宿主和依赖策略。退出条件是 ADR 接受，后续不再反复更换基础 UI 技术。

### 17. Production desktop host

开始正式产品界面：实现窗口生命周期、Projection 消费、桌面层级、焦点、任务栏、透明材质和多屏定位。退出条件是窗口不会启动抢焦点、逐个闪烁或错误覆盖其他应用。

### 18. Card chrome and rendering

实现 Card 标题、图标、展开/收起、关闭/固定入口、实例级入口可见性和外观配置。退出条件是能力、入口和视觉表现分离，关闭入口后对应资源确实停止使用。

### 19. Interaction, resize and snapping

实现拖动、缩放、边缘/中心/Card 吸附、Ctrl 绕过吸附、键盘操作和高 DPI 输入转换。退出条件是位置保存准确，吸附不会改变用户主动绕过的操作。

## Card Features

### 20. ApplicationCard shell integration

接入 Explorer 拖放、原生文件名、快捷方式解析、Shell 图标、商店应用身份和失效状态。退出条件是解析失败可解释，不伪造错误类型或静默显示错误文件。

### 21. MappingCard live source

实现文件夹映射、引用集合、目录变化监听、反向修改开关和失效引用。退出条件是外部文件归属清晰，删除映射不会移动外部文件。

### 22. TodoCard experience

实现待办编辑、完成状态、排序、空状态和持久化。退出条件是卡片重启恢复，内容变化不会阻塞其他 Card。

### 23. Settings and configuration resolution

实现全局默认、类型默认、实例覆盖、配置迁移入口和每 Card 独立设置。退出条件是全局默认不会覆盖用户已有的实例明确选择。

### 24. Tray, desktop trigger and shortcut control

实现托盘、桌面触发显隐、快捷键配置、取消快捷键和退出流程。退出条件是显隐批量同步、退出无残留进程，关闭快捷键后不会留下隐藏呼出路径。

### 25. Storage and deletion UX

实现存储目录选择与迁移界面、删除二次确认、冲突重命名预览、失败恢复提示和诊断日志入口。退出条件是用户在任何破坏性操作前都能看到实际副作用。

## Robustness And Release Quality

### 26. Fullscreen, virtual desktop and shell lifecycle

处理全屏应用、桌面切换、Explorer 重启、任务栏变化、锁屏、休眠和虚拟桌面。退出条件是 Card 不会错误出现在其他应用或错误桌面上。

### 27. File/icon cache and large-directory scaling

实现 Shell 元数据缓存、文件监视增量更新、图标失效、批量目录和大数量项目处理。退出条件是隐藏状态停止不必要监视，规模增长不会线性拖慢显隐。

### 28. Startup and synchronized visibility performance

优化启动初始化、批量窗口提交、显隐并行、z-order、焦点和动画提交。退出条件是消除逐个出现、短暂闪烁和启动期间逐窗口抢前台。

### 29. Visual polish and accessibility

完成主题、缩放、Reduced Motion、键盘导航、提示、错误状态、空状态和视觉层级。退出条件是功能入口和美观表现都可按实例取舍，且不牺牲可访问性。

### 30. Fault injection and soak validation

覆盖频繁显隐、拓扑抖动、文件占用、配置中断、Explorer 重启、长时间运行和异常退出。退出条件是没有已知数据损坏、残留进程和无法恢复的布局状态。

### 31. Performance gate

用固定机器、系统版本、显示器拓扑和素材验证性能预算：Empty 私有工作集 <=30 MB、Typical <=50 MB、稳定后空闲 CPU 接近 0%、冷启动 <=500 ms、同步显隐提交目标 <=16 ms。未达标时先记录成本再调整实现或预算。

## Packaging And Release

### 32. Installer and data lifecycle

确定安装技术、按用户安装目录、数据目录、开机启动、升级保留配置、降级限制和卸载数据选择。退出条件是干净 Windows 环境可安装、启动和卸载。

### 33. Update, CI and supply chain

实现 GitHub Actions 的 Debug/Release 测试、构建、打包、版本标签、更新校验、失败回滚、许可证清单和构建可追溯性。代码签名证书等外部输入单独列出。

### 34. Release candidate acceptance

完成安装包、升级包、首次运行、日常使用、恢复、性能、已知限制和发布说明验收。退出条件是可以发布正式版本，并且后续开发不会依赖未记录的本地环境状态。
