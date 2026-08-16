# Performance Strategy

## Principle

性能是 Desto 的产品能力，不是发布前的清理工作。架构和功能设计需要持续回答：常驻什么、按需加载什么、何时释放、如何测量。

## Metrics

基础基准至少记录：

- 冷启动到首个稳定画面的时间。
- 空布局的私有工作集和提交大小。
- 典型布局的私有工作集和提交大小。
- 全部 Card 显示和隐藏的提交延迟。
- 拖动、吸附和内容驱动尺寸更新的输入到画面延迟。
- 空闲 CPU、线程数和周期性唤醒。
- 每增加一个显示器、Card 和可见项目的边际成本。

## Benchmark Scenarios

```text
Empty
  1 display, 0 cards

Typical
  2 displays, 4 cards, 100 visible items

Heavy
  3 displays, 20 cards, 1000 total items

Hidden
  Typical layout with every card hidden
```

候选 UI 技术必须在相同场景、相同素材和相同构建配置下比较。

## Headless Baseline

基准程序为 `desto_headless_baseline`，每个场景单独启动一个进程，避免不同场景共享堆状态。它先写入并读取同一份配置快照，再执行生命周期启动、Runtime 恢复、显示拓扑提交、逐 Card 显隐提交和拓扑变化提交。空闲 CPU 使用 1 秒窗口采样，私有工作集和线程数通过 Windows 进程 API 读取。

```powershell
cmake --build build --config Release
build\benchmarks\Release\desto_headless_baseline.exe --scenario empty
build\benchmarks\Release\desto_headless_baseline.exe --scenario typical
build\benchmarks\Release\desto_headless_baseline.exe --scenario heavy
build\benchmarks\Release\desto_headless_baseline.exe --scenario hidden
```

输出为 CSV，字段依次为场景、显示器数、Card 数、项目数、隐藏标记、启动毫秒、显隐提交毫秒、拓扑提交毫秒、私有工作集字节、线程数和空闲 CPU 百分比。该基线不包含 UI 窗口、Shell 图标或文件监视成本，不能直接当作最终产品内存预算。

### 2026-08-16 Baseline

环境：Windows 11 Pro 10.0.26200，Intel Core i7-11800H，16 logical processors。Release 结果如下；每个场景为一次独立进程测量。

| Scenario | Startup ms | Visibility ms | Topology ms | Private MB | Threads | Idle CPU |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Empty | 0.317 | 0.000 | 0.001 | 0.68 | 4 | 0% |
| Typical | 0.768 | 0.001 | 0.005 | 0.75 | 4 | 0% |
| Heavy | 2.623 | 0.003 | 0.031 | 1.30 | 4 | 0% |
| Hidden | 0.701 | 0.003 | 0.009 | 0.73 | 4 | 0% |

Debug 同一场景的私有工作集为 0.69/0.95/2.79/0.93 MB，启动为 0.828/5.584/42.060/11.186 ms（Empty/Typical/Heavy/Hidden）。这些数据用于验证基线程序可重复，不用于发布性能门槛。

### UI Prototype Comparison

2026-08-16 在同一 Windows 11 环境、同一 4 Card/当前显示器拓扑和 Release 配置下，各启动一次原型并在约 700 ms 后采样私有工作集。采样包含窗口和渲染初始化，不是最终产品预算：

| Prototype | Rendering/host | Private MB | Exit | Residual process |
| --- | --- | ---: | ---: | --- |
| Native | Win32 layered window + GDI DIB | 1.35 | 0 | none |
| Alternative | DirectComposition + Direct2D + DWrite | 59.85 | 0 | none |

事项 17 的正式宿主使用 Win32 layered/GDI DIB，当前 3 个 Card 在每个活动显示器上投影时 Release 私有工作集采样约 3.6 MB，退出码为 0 且无残留进程。该数值仍会随 Card 内容、Shell 图标和文件监视功能增加，后续事项必须继续用相同工具复测。

事项 19 加入按需对齐线、抗锯齿圆角、Chevron 按下反馈和自适应排版计算后，同一 3 Card Release 宿主稳定采样为 3.56 MB 私有内存、4 个线程；10 秒定时退出后无残留进程。对齐线仅在拖动期间显示，空闲时不运行计时器或渲染循环。

事项 20 加入 Shell 元数据 Adapter、Explorer 拖入、项目 Tooltip 和空 Application Card 后，同一 3 Card Release 宿主稳定采样为 4.05 MB 私有内存、16.10 MB 工作集、4 个线程；8 秒定时退出码为 0 且无残留进程。每个成功解析的 64x64 Shell 图标增加约 16 KiB 像素数据，多屏 Projection 通过共享所有权复用该数据；目录实时监视和长期图标缓存尚未启用。

事项 20 完成自适应扩缩、跨 DPI 目标迁移、16/32 px Shell 源档位刷新和双线性显示采样后，当前本地配置的 Release 预览稳定采样为 4.99 MB 私有内存、21.38 MB 工作集、9 个线程。每个项目只保留当前 Card 的一份 16x16 或 32x32 预乘 Alpha 源图；目标尺寸采样不建立常驻缩放缓存。该数据包含本机 Shell/COM 按需线程，需在事项 31 的固定机器和固定素材场景复测后才作为发布门槛结论。

Qt/qmake 未安装在当前构建环境，因此事项 15 采用 Windows SDK 自带 DirectComposition 作为可构建候选。事项 16 需要结合这组数据和输入、DPI、桌面层级、发布依赖等维度做技术选择，不能只按渲染效果判断。

## Initial Targets

以下数字是原型阶段的目标，不是未经验证的承诺：

| Metric | Initial target |
| --- | ---: |
| Empty private working set | <= 30 MB |
| Typical private working set | <= 50 MB |
| Idle CPU after settling | approximately 0% |
| Cold start to stable presentation | <= 500 ms |
| Synchronized show/hide commit | <= 16 ms |

原型结果必须记录设备、系统版本、显示器拓扑和测量工具。若候选实现未达标，应先解释成本来源，再决定优化、调整预算或更换方案。

## Feature Cost

每项功能需要区分：

- 基础常驻成本。
- 启用后的实例级成本。
- 可见时的渲染成本。
- 隐藏或关闭后的剩余成本。

实例关闭某项功能入口或视觉内容后，与其专属的工作应停止；共享基础设施是否保留由实际成本和复用情况决定。
