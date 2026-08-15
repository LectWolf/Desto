# Desto

Desto 是一个面向 Windows 的轻量桌面整理工具，目前处于架构与技术验证阶段。

项目目标：

- 低常驻内存和接近零的空闲 CPU 占用。
- 快速启动、同步显隐和稳定的多屏体验。
- 简约但精致的界面。
- 每个 Card 拥有独立的功能入口、外观和交互配置。
- 稳定的核心结构，以及可独立开发和验证的功能模块。

## Documents

- [产品边界](docs/PRODUCT.md)
- [领域模型](docs/DOMAIN.md)
- [存储与迁移](docs/STORAGE.md)
- [架构原则](docs/ARCHITECTURE.md)
- [性能策略](docs/PERFORMANCE.md)
- [交付路线](docs/ROADMAP.md)
- [配置模型](docs/CONFIGURATION.md)
- [架构决策记录](docs/adr/README.md)

## Status

当前处于 `（17/34）Production desktop host`。事项 1-16 已完成；已确定采用 Win32 原生窗口宿主、分层窗口、批量定位和 Direct2D 离屏渲染，不引入 Qt/WinUI/CEF 常驻运行时。下一步开始正式产品窗口宿主，第 17 项进入产品界面实现。
