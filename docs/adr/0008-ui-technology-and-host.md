# ADR 0008: Production UI technology and desktop host

- 状态：Accepted
- 日期：2026-08-16

## 背景

Desto 的核心约束是低常驻内存、快速启动、同步显隐、正确的桌面层级和多显示器 DPI。事项 14 的 Win32 layered/GDI 原型私有工作集约 1.35 MB；事项 15 的 DirectComposition + Direct2D + DirectWrite 原型约 59.85 MB。两种实现均能完成 Projection 展示，但常驻成本差异明显。当前构建环境没有 Qt/qmake，无法将 Qt 作为可重复的本地候选纳入基准。

## 决策

正式产品采用以下组合：

1. **窗口宿主：Win32**。每个可见 Projection 使用 `WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE` 的无边框窗口；输入、焦点、任务栏和显示器定位由宿主统一管理。
2. **窗口合成：分层窗口 + `UpdateLayeredWindow`**。所有卡片的位置和显示状态通过批量窗口提交完成；默认使用桌面层级，不把卡片强制放在其他应用之上。
3. **卡片渲染：Direct2D 离屏渲染**。渲染器写入预乘 Alpha 的 32 位位图，再交给分层窗口提交。该路径可以提供圆角、文字、主题和高 DPI 绘制能力，同时不需要每个 Card 持有 DirectComposition Surface 或 D3D 合成树。
4. **线程模型：单一 UI 线程**。窗口消息、Projection 提交和可见卡片绘制在 UI 线程串行完成；文件监视、Shell 元数据和配置 I/O 留在外围任务，由结果消息合并回 UI 状态。
5. **依赖策略：系统组件优先**。正式运行时不引入 Qt、WinUI、CEF、浏览器内核或大型 UI 运行时；Direct2D、DirectWrite、D3D/Windowing API 使用 Windows 系统组件。第三方库必须单独证明其资源收益和发布必要性。

## 取舍

- 相比 GDI，Direct2D 离屏渲染能降低正式 UI 的绘制复杂度并提高视觉质量，但渲染器仍需控制位图尺寸、缓存和重绘范围。
- 相比 DirectComposition，该方案放弃硬件合成树和复杂动画能力，换取更低的常驻内存、更容易控制的桌面层级和更简单的故障恢复。
- 这不是“永远不能使用 DirectComposition”的限制；若后续动画需求和性能数据证明其收益超过约 60 MB 的成本，可以通过新的 ADR 重新评估。

## 约束

正式 UI 不得把 HWND、D2D 资源或显示器句柄传入 Domain/Application。渲染器只接收不可变的 Card/Projection 视图，宿主负责资源生命周期、批量提交和异常清理。事项 17 起按此决策实现产品窗口宿主。
