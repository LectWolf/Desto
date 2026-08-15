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

当前处于 `（18/34）Card chrome and rendering`。事项 1-17 已完成；正式桌面宿主已可运行，使用 Win32 分层窗口、批量定位和轻量 GDI DIB 渲染，Release 采样约 3.6 MB 私有工作集。下一步实现 Card 标题、图标、展开/收起、关闭/固定入口和实例级外观配置。
