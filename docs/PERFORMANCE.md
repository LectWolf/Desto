# Performance Strategy

## Principle

性能是 Desto 的产品能力，不是发布前的清理工作。架构和功能设计需要持续回答：常驻什么、按需加载什么、何时释放、如何测量。

## Metrics

基础基准至少记录：

- 冷启动到首个稳定画面的时间。
- 空布局的私有工作集和提交大小。
- 典型布局的私有工作集和提交大小。
- 全部 Card 显示和隐藏的提交延迟。
- 拖动、缩放和吸附的输入到画面延迟。
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
