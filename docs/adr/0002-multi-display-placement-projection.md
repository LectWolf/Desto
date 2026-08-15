# Multi-display layout uses persistent placements and temporary projections

- Status: Accepted
- Date: 2026-08-16

Desto 将位置从 Card 移到 WorkspaceLayout 的 Placement：一个 Card 可以分别放置到多个指定显示器，或使用单个“所有显示器”目标。Placement 保存相对显示器工作区的 DIP 坐标，当前显示器连接状态由 DisplaySnapshot 单独提供；目标显示器断开时只生成 Fallback Projection，不改写原始 Placement，重连后恢复。这样避免依赖不稳定的显示器枚举序号，也避免分辨率、DPI 或断屏事件永久改变用户布局。
