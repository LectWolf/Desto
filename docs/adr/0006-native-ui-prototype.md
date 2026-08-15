# ADR 0006: Native layered-window prototype

- 状态：Accepted for prototype
- 日期：2026-08-16

## 背景

Desto 需要低常驻资源、同步显隐、多显示器和正确的桌面层级。正式 UI 技术尚未在事项 15/16 决定，因此先用最小原生宿主验证 Projection 到窗口的边界。

## 决策

事项 14 使用 Win32 layered window + GDI DIB 作为技术原型：

- 每个 `PlacementProjection` 对应一个 `WS_POPUP` 工具窗口。
- 窗口使用 `WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT`，不抢焦点并允许桌面输入穿透。
- 通过 `UpdateLayeredWindow` 更新像素，通过 `BeginDeferWindowPos`/`EndDeferWindowPos` 一次提交所有窗口的位置和可见性。
- 进程启用 `DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2`；显示器工作区原点、尺寸和 Placement 坐标统一按 DIP 计算，再转换为物理像素。
- 原型直接消费 `ApplicationRuntime` 的临时 Projection，不改变持久化 Placement。

## 结果与边界

该方案可以在真实 Windows 显示器拓扑上验证透明窗口、批量显隐、多 DPI 和桌面层级，且不把 HWND、RECT 或 GDI 类型带入 Domain/Application。它只用于技术比较，不代表正式产品必须采用 GDI；事项 15 将用候选 UI 技术复现同一场景，事项 16 再根据内存、启动、输入和发布成本选择最终宿主。
