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
- [稳定性验证](docs/STABILITY.md)
- [发布流程](docs/RELEASING.md)
- [0.1.0 发布说明](docs/RELEASE_NOTES_0.1.0.md)
- [0.2.0 发布说明](docs/RELEASE_NOTES_0.2.0.md)
- [0.2.1 发布说明](docs/RELEASE_NOTES_0.2.1.md)
- [0.2.2 发布说明](docs/RELEASE_NOTES_0.2.2.md)
- [RC 验收清单](docs/RC_CHECKLIST.md)
- [交付路线](docs/ROADMAP.md)
- [配置模型](docs/CONFIGURATION.md)
- [架构决策记录](docs/adr/README.md)

## License

Desto is licensed under [GPL-3.0-or-later](LICENSE). Redistributions and
modified versions must preserve attribution and provide corresponding source
under the same license. Third-party components remain under their own licenses;
see [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

## Status

当前回到 `（48/49）Crystal transparency preset` 进行真实桌面视觉复验。事项 1-39、41-47 已完成，事项 40 的扩展原型已撤销；Desto 使用 Win32 原生宿主与原生系统菜单，首次启动保持空 Workspace，Card 支持多显示器、桌面层级、事务拖放、映射、文件原位重命名、自绘归档日期、彩色 Emoji、水晶透明外观、登录启动和桌面/任务栏触发。水晶材质与分层窗口字体通过目测后再恢复事项 49 的发布候选验收。
