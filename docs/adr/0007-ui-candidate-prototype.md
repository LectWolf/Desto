# ADR 0007: DirectComposition candidate prototype

- 状态：Proposed for comparison
- 日期：2026-08-16

## 背景

事项 14 的 Win32 layered/GDI 原型已经验证了 Projection、透明窗口和批量定位。为了避免只根据单一实现决定 UI 技术，事项 15 需要一个具备真实合成能力的对照实现。当前机器没有 Qt/qmake，无法在本地对 Qt 做可重复构建和测量。

## 对照实现

`prototypes/desto_directcomposition_prototype` 使用 Windows SDK 的 DirectComposition、Direct2D、DirectWrite 和 D3D11：

- 每个 Projection 仍对应一个不抢焦点、点击穿透的工具窗口。
- 窗口内容改由 DirectComposition Surface 和 Direct2D 绘制。
- 硬件 D3D11 设备不可用时使用 Windows 自带 WARP，确保原型在无硬件加速环境也能启动。
- 显示拓扑、DPI 转换、Card 数量、Projection 和批量窗口定位与事项 14 完全一致。

## 观测结果

在 2026-08-16 的本机 Release 采样中，DirectComposition 原型私有工作集约 59.85 MB，Win32 layered/GDI 原型约 1.35 MB；两者均正常退出且没有残留进程。该数据只用于候选比较，采样时机和驱动状态会影响绝对值。

## 后续

本记录不直接接受 DirectComposition 为正式技术。事项 16 将比较内存、启动、显隐提交、输入延迟、DPI、桌面层级、依赖体积和维护成本，并在新的 ADR 中接受或拒绝该候选。
