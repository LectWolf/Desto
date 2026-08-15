# ADR-0005: Explicit process lifecycle with a Windows named mutex

- Status: Accepted
- Date: 2026-08-16

## Context

Desto 是长期运行的桌面进程。重复启动、配置加载未完成就创建窗口、托盘退出未释放实例锁，都会造成错误的用户体验或残留进程。生命周期逻辑还需要在没有 UI 的测试中验证。

## Decision

使用 `ApplicationLifecycle` 强制 `begin -> configurationLoaded -> runtimeReady` 的启动顺序，并通过 `requestShutdown -> completeShutdown` 完成正常退出。异常路径立即释放单实例 gate 并进入 Failed 状态；析构函数再次执行幂等释放。

Windows 使用命名 Mutex 作为单实例 Adapter；测试使用内存 Adapter。诊断记录使用固定事件码和等级，采用有容量上限的内存环形记录，不写入用户路径、文件名或文件内容。

## Consequences

- 第二个进程在获取 Mutex 时可确定收到 AlreadyRunning，不会创建第二套运行时。
- 生命周期模块不依赖 UI、文件系统或线程，托盘和窗口宿主只负责调用状态接口。
- 当前诊断记录由外围宿主决定是否写入日志文件；记录器本身不会引入常驻 I/O。

## Validation

- 内存 Adapter 测试重复实例、启动顺序、托盘退出和异常退出。
- Windows Adapter 测试同名 Mutex 的互斥、释放和重新获取。
- Debug/Release 测试套件通过。
