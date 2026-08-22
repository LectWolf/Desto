# Tests

测试优先覆盖领域状态转换、配置解析、布局和吸附等稳定模块接口。平台集成测试与纯算法测试分开运行。

`desto_fault_injection_tests` 额外覆盖文件锁、配置发布中断、拓扑往返、高频显隐、Shell 重绑定和异常进程退出。覆盖矩阵及 soak 命令见 [稳定性验证](../docs/STABILITY.md)。

`desto_windows_text_input_tests` 覆盖统一自绘输入窗口的单 HWND 结构、无原生 Edit 子窗口、UTF-16 中文/Emoji、选区、撤销、提交/取消以及逐像素 Alpha 绘制；设置与桌面宿主测试再覆盖搜索、归档补录、Card 重命名和 Todo 添加的实际调用。文件项重命名入口已移除，文件夹/文件改名由资源管理器完成。
