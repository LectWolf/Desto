# Stability Validation

Desto 将故障注入和长时间运行验证分成两层：`desto_fault_injection_tests` 提供快速、确定性的回归信号，`desto_stability_soak` 重复驱动真实 Win32 Card 窗口、Shell 生命周期重绑定和原子配置写入。

## Fault Matrix

| 场景 | 自动验证 | 失败信号 |
| --- | --- | --- |
| 文件被占用 | 第二个移动源使用禁止删除共享的 Windows 句柄锁定 | 已完成的第一项未回滚、源内容改变或目标残留 |
| 配置发布中断 | 锁定当前配置，使临时文件写入成功但 `ReplaceFileW` 失败 | 最后有效配置改变、无法重新加载或 `.tmp-*` 残留 |
| 显示器拓扑抖动 | 1000 次在原尺寸、DPI 变化和目标离线之间往返投影 | 持久 Placement 任意字段漂移或离线 Card 被错误投影 |
| 高频显隐 | 1000 次批量隐藏/显示真实 Card HWND | 可见状态错误或进程 HANDLE/GDI/USER 对象增加 |
| Explorer 生命周期 | 重复发送 `TaskbarCreated`，并由平台测试注入失效桌面 owner | Card HWND 被重建、owner 未修复或资源增加 |
| 异常退出 | 子进程持有单实例互斥量后被强制终止 | 子进程残留、互斥量无法重新获取或最后有效配置无法加载 |
| 长时间运行 | 可调时长 soak 循环执行显隐、Shell 重绑定和配置往返 | 稳态 HANDLE/GDI/USER 对象持续增长或配置往返分叉 |

自动测试不会结束真实 `explorer.exe`。Explorer 重启由失效 owner 和 `TaskbarCreated` 在进程内模拟，这能验证 Desto 的重新发现路径，同时不会破坏用户当前桌面会话。真实 Explorer 崩溃、锁屏、休眠和物理显示器热插拔仍属于发布候选的人工验收项。

## Commands

```powershell
cmake --build build --config Debug --target desto_fault_injection_tests
build\tests\Debug\desto_fault_injection_tests.exe

cmake --build build --config Release --target desto_stability_soak
build\benchmarks\Release\desto_stability_soak.exe --duration-ms 10000
```

`--duration-ms` 接受正整数，可在无人值守环境扩大到数小时。soak 在首次走完显隐、Shell 重绑定和原子替换后建立稳态基线；输出同时保留 `handles_cold`，从而区分一次性 Windows 惰性初始化和随迭代累积的泄漏。

## 2026-08-18 Baseline

Windows 11 `10.0.26200`、Debug 和 Release 各运行 10 秒。两种配置的稳态句柄、GDI 和 USER 对象均无增长：

| Build | Iterations | Handles | GDI | USER | Private bytes start/end |
| --- | ---: | ---: | ---: | ---: | ---: |
| Debug | 261 | 213 / 213 | 13 / 13 | 15 / 15 | 3,469,312 / 3,608,576 |
| Release | 263 | 219 / 219 | 13 / 13 | 15 / 15 | 3,272,704 / 3,407,872 |

首次完整配置替换会一次性增加 8 个进程句柄；预热后的重复替换不再增长，因此当前证据指向 Windows/运行库惰性初始化，而不是按保存次数累积的泄漏。

同一 Release 在 30 秒加长运行中完成 1248 轮，稳态句柄 `213 -> 212`、GDI `13 -> 13`、USER `15 -> 15`，私有提交 `3,317,760 -> 3,518,464` 字节，峰值 `3,592,192` 字节。
