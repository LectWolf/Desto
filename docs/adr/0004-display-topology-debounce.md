# Display topology changes are debounced before projection refresh

- Status: Accepted
- Date: 2026-08-16

Windows 在显示器连接、DPI、设备和系统设置变化时可能连续发送多条消息。Desto 将这些消息交给独立的 `DisplayTopologyMonitor`，在最后一条消息后的安静窗口内只查询一次 Provider，并按稳定 DisplayId 生成 Added/Removed/Modified 差异；Provider 失败会保留待刷新状态并按相同策略重试。Windows 消息源只负责触发请求，回调运行在隐藏窗口线程上，必须快速、非阻塞且不能调用其生命周期方法。

该防抖只合并显示器拓扑查询，不决定正在拖动窗口何时跨入另一 DPI 区域。交互式跨屏缩放由 Windows Per-Monitor V2 的 `WM_DPICHANGED` 驱动，宿主采用系统建议矩形；`WM_MOVING` 不按窗口交叠比例主动改 DPI 或尺寸。两条路径必须保持分离，避免自定义阈值与窗口缩放互相反馈。
