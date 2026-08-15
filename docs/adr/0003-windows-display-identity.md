# Windows display identity comes from DisplayConfig target paths

- Status: Accepted
- Date: 2026-08-16

WindowsDisplayTopology 使用 `QueryDisplayConfig` 的 `DISPLAYCONFIG_TARGET_DEVICE_NAME.monitorDevicePath` 作为 DisplayId。`EnumDisplayMonitors` 返回的 GDI 名称只用于关联工作区矩形和主屏标志，不能作为持久化身份，因为 `DISPLAY1` 这类枚举名称会随拓扑变化。若无法把当前监视器关联到稳定 target path，适配器直接失败并报告错误，不产生可能污染布局的伪身份。
