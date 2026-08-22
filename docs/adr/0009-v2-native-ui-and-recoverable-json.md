# ADR 0009: v2 native UI and recoverable JSON configuration

- 状态：Accepted
- 日期：2026-08-22

## Context

Desto 的核心窗口需要直接参与 Windows 桌面 owner、分层窗口、任务栏、OLE 拖放和多显示器 DPI。现有代码中 Card、设置页、输入框和平台窗口的职责逐渐混合，部分生产接口是为了满足内部测试而存在。当前文本输入和彩色 Emoji 路径还引入了 Direct2D/DirectWrite；单体 JSON 配置虽然可读，但主文件损坏时必须有明确、可验证且由用户确认的恢复流程。

## Decision

### UI and rendering

v2 保留单进程 C++20/Win32 宿主。可见 UI 组件由 Desto 自有组件模块绘制，不使用 Windows MessageBox、TaskDialog、原生 Edit、原生 List 或其他系统可见对话框作为产品组件。

底层窗口、消息循环、TSF、剪贴板、文件拖放、DPI 和 `UpdateLayeredWindow` 仍由 Windows Adapter 提供。Card、设置页、输入框、菜单和恢复界面共享 `DestoUi` 的状态、布局、焦点、命中测试、主题和 GDI DIB 渲染接口。

正式产品删除彩色 Emoji 能力。Card 和输入框统一使用 GDI/自有 DIB 渲染；Direct2D/DirectWrite 不再作为正式运行时依赖。Unicode 文本通过 GDI 字体回退和现有 TSF 文本状态处理；Emoji 以单色字形或系统回退字形显示，不承诺彩色字体。

### Configuration

v2 继续使用 JSON 作为权威配置格式，不引入 SQLite。配置规模不需要数据库查询，JSON 便于诊断、导入、导出和 v1 迁移；可靠性通过存储协议而非更换格式解决。

主文件和至少两代备份采用轮换写入：

```text
settings.json
settings.json.bak1
settings.json.bak2
settings.json.corrupt-<timestamp>
```

每个文件必须通过大小上限、JSON 结构、schema、语义约束和内容校验后才算有效。写入使用临时文件、刷新、备份轮换和原子替换；失败不得破坏最后一个有效文件。

启动时独立检查主文件和备份：

```text
主文件有效 -> 正常启动
主文件无效 + 备份有效 -> 自有恢复界面，等待用户确认
主文件和备份均无效 -> 自有损坏界面，用户选择退出或创建空 Workspace
```

程序不得静默将备份提升为主文件。用户确认后，损坏主文件先改名为 `.corrupt-*`，再次校验所选备份，再以原子方式提升为主文件；原备份保留，不直接删除。

### Migration

现有 schema 1..27 视为 v1 JSON。`V1ConfigMigrator` 只读旧文件，将其转为规范化 v2 模型，再由新的 `ConfigRepository` 写回。迁移失败不覆盖 v1 文件；成功迁移保留 `settings.json.v1-migrated` 副本，并记录迁移来源和目标版本。

## Consequences

正面影响：默认运行时不再加载彩色 Emoji 和 Direct2D；UI 行为统一；配置损坏可由用户控制恢复；不增加 SQLite 依赖和数据库恢复复杂度。

代价：GDI 文本需要重新验证字体回退、组合输入、RTL 和高 DPI；自有恢复界面必须在配置不可用时使用内置默认状态启动；多代备份会增加少量磁盘占用和写入流程复杂度。

## Rejected alternatives

- Qt、WinUI 3、WPF、Avalonia、Electron：增加运行时和发布成本，不能消除桌面 Shell 的 Win32 适配。
- SQLite：当前数据量和查询需求不足以抵消新依赖、数据库迁移和恢复复杂度。
- 自动恢复或静默创建空 Workspace：可能掩盖用户数据损坏，不符合可控恢复要求。
