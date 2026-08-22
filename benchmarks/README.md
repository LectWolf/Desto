# Benchmarks

这里保存可重复运行的启动、内存、显隐、交互延迟和规模成本基准。结果必须记录环境，不提交无法复现的手工结论。

当前基准：`desto_headless_baseline --scenario empty|typical|heavy|hidden`。完整字段定义和 2026-08-16 的 Debug/Release 结果见 [性能策略](../docs/PERFORMANCE.md)。

稳定性压力入口为 `desto_stability_soak --duration-ms <毫秒>`。它重复驱动真实 Card HWND 的显隐、Shell 重绑定和原子配置往返，并输出冷态、稳态及峰值资源；判定和当前基线见 [稳定性验证](../docs/STABILITY.md)。

正式桌面宿主门禁为 `desto_desktop_performance_gate --scenario empty|typical`。已撤销的时间驱动扩展原型及其工资门禁不再参与构建或发布验收。
