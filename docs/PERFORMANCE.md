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

事项 34 为 Shell 项目增加进程内 LRU，默认最多 256 个“规范化路径 + 源尺寸”条目，缓存持有的图标像素总量最多 8 MiB。命中前比较文件存在性、类型、大小和修改时间；移动事务主动失效源与目标路径。上限是容量边界，不是预分配，空布局不会为缓存保留 8 MiB。

文件夹 Mapping 的 `ReadDirectoryChangesW` 通知现在保留具体相对路径；60 ms 合并窗口内只对新增、删除、修改和重命名涉及的项目重新检查。单卡片一次合并超过 512 个路径、通知缓冲区溢出或路径无法安全归入来源根时，才回退整目录枚举。所有 Card 隐藏时停止目录监视并清除 LRU 引用，重新显示后先做一次全量对账，再恢复增量监听。

Qt/qmake 未安装在当前构建环境，因此事项 15 采用 Windows SDK 自带 DirectComposition 作为可构建候选。事项 16 需要结合这组数据和输入、DPI、桌面层级、发布依赖等维度做技术选择，不能只按渲染效果判断。

### 2026-08-18 Desktop Performance Gate

`desto_desktop_performance_gate` 在独立 Release 进程中驱动正式 `WindowsDesktopHost`。Typical 使用 2 个显示器快照、4 个 Card、100 个可见项目和 90 份独立 32x32 图标像素；每个场景重复 5 次。下表记录范围：

| Scenario | Startup ms | Visibility P95 ms | Private MB | Threads | Idle CPU |
| --- | ---: | ---: | ---: | ---: | ---: |
| Empty | 19.66-29.24 | 0.05-0.37 | 2.07-2.10 | 7 | 0% |
| Typical | 88.38-133.99 | 6.22-9.93 | 5.06-5.16 | 7 | 0% |

schema 23 撤销扩展原型后的单次 Release smoke：Empty 为 30.13 ms 启动、0.13 ms 显隐 P95、2,174,976 私有字节、7 线程、0% 空闲 CPU；Typical 为 158.42 ms、13.92 ms、5,292,032 私有字节、7 线程、0%。两项均通过当前门禁，但单次 smoke 不替代上表的多轮范围；发布候选仍需在固定环境重复采样。

schema 24 完成锚点、拖放扩缩、Todo 宽度和展示入口修订后的 Release 测量：Empty 为 25.87 ms 启动、0.077 ms 显隐 P95、2,138,112 私有字节、7 线程、0% 空闲 CPU；Typical 为 153.44 ms、13.27 ms、5,300,224 私有字节、7 线程、0% 空闲 CPU。两项 `passed=1`，Typical 显隐仍低于 16 ms 目标。环境为 Windows 11 Pro 10.0.26200、Intel Core i7-11800H、16 logical processors；测量时间为 2026-08-18。

完整应用使用当前本机 6 Card/schema 22 配置从外部启动 5 次，从创建进程到 GUI 资源稳定为 210.51-358.61 ms，低于 500 ms 门槛。稳定 2 秒后实际进程为 42.97 MB 工作集、8.70 MB 私有内存、12 个线程，随后 1 秒空闲 CPU 为 0%。该工作集包含 Shell、COM、托盘、设置宿主、文件监视和本机图标，不能与只测渲染宿主的 Private MB 横向混用。

schema 24 真实 5 Card 配置完成迁移并重新启动后，稳定进程为 43,737,088 bytes 工作集、8,646,656 bytes 私有内存、4 线程，随后 1 秒空闲 CPU 为 0%。该样本确认撤销的扩展目录删除后没有对应常驻线程；本次未执行多轮冷启动计时，因此不能用它替换上一段的启动范围。

2026-08-19 完成归档日期、Folder Mapping MOVE、彩色 Emoji、Win11 菜单策略和启动注册修订后，Release Empty 为 20.8039 ms 启动、0.0649 ms 显隐 P95、2,277,376 私有字节、7 线程、0% 空闲 CPU；Typical 为 94.4816 ms、5.1364 ms、5,451,776 私有字节、7 线程、0%。两项 `passed=1`。基准场景不含 Emoji，DirectWrite/Direct2D 工厂未初始化；彩色 Emoji 的按需首次绘制成本由像素正确性测试覆盖，尚未作为独立性能场景固化。

2026-08-19 发布候选最终二进制（包含首次空 Workspace、文件原位重命名、自绘归档月历和 0.36 水晶透明预设）再次测量：Empty 为 24.6836 ms 启动、0.0792 ms 显隐 P95、2,277,376 私有字节、7 线程、0% 空闲 CPU；Typical 为 115.024 ms、7.8074 ms、5,423,104 私有字节、7 线程、0%。两项均为 `passed=1`，Typical 显隐仍低于 16 ms 目标。原位编辑器和月历都是交互时创建、关闭后销毁；水晶预设只改变既有分层 DIB 合成参数，因此固定场景没有新增常驻线程或空闲唤醒。

2026-08-19 schema 25 水晶材质和分层字体修订后的最终 Release：Empty 为 19.1477 ms 启动、0.1879 ms 显隐 P95、2,195,456 私有字节、7 线程、0% 空闲 CPU；Typical 为 131.095 ms、8.3312 ms、5,390,336 私有字节、7 线程、0%。两项 `passed=1`，Typical 显隐低于 16 ms。水晶 Card 重绘期间为图标框创建每像素 1 byte 的瞬时 coverage 缓冲，提交后立即释放；固定基准没有水晶 Card，因此这组结果证明默认路径没有新增线程和空闲唤醒，水晶实例的单次重绘成本由宿主测试覆盖但尚未建立独立性能场景。

2026-08-19 schema 26 最终候选 Release：Empty 为 38.3339 ms 启动、0.2048 ms 显隐 P95、2,265,088 私有字节、7 线程、0% 空闲 CPU；Typical 为 229.196 ms、8.6008 ms、5,427,200 私有字节、7 线程、0%。两项 `passed=1`，Typical 显隐低于 16 ms，启动低于 500 ms。schema 26 只增加已知 0.62 水晶预设值的配置迁移，运行期材质路径与 schema 25 相同；本轮单次启动时间高于上一轮，应视为本机采样波动，不据此声称启动性能改善或退化。

2026-08-19 水晶内容层与统一内描边修复后的 Release：Empty 为 39.9765 ms 启动、0.0967 ms 显隐 P95、2,281,472 私有字节、7 线程、0% 空闲 CPU；Typical 为 151.918 ms、12.7129 ms、5,414,912 私有字节、7 线程、0%。两项 `passed=1`。真实 6 Card 配置稳定后为 8,790,016 私有字节、40,321,024 工作集、6 线程，连续 3 秒 CPU 增量为 0。水晶重绘期间新增每像素 4 byte 的预乘内容缓冲和同尺寸临时 mask DIB，提交后释放；固定性能场景无水晶 Card，因此数据只证明默认路径和空闲状态没有新增常驻成本，水晶重绘正确性由宿主像素测试与真实桌面截图覆盖。

2026-08-20 归档范围、输入缓存和桌面交互性能修复后的最终 Release：Empty 为 15.7921 ms 启动、0.1026 ms 显隐 P95、2,260,992 私有字节、7 线程、0% 空闲 CPU；Typical 为 158.319 ms、8.0367 ms、5,554,176 私有字节、7 线程、0%。两项均为 `passed=1`。诊断期间发现纯文本 Todo 因“统一使用 Segoe UI Emoji 字体族”被错误当成“包含彩色 Emoji”，会无条件初始化 Direct2D，Typical 达到 57,470,976 私有字节和 19 线程。修正后字体族不变，只有实际 Emoji 码点、VS16 或 ZWJ 才进入彩色渲染，恢复到 5,554,176 字节和 7 线程。Todo 两行交替 100 次的宿主回归统计为 0 次完整 Render、0 次完整 Commit；同尺寸 Popup 输入的 DIB 和 D2D RenderTarget 各只创建一次。

2026-08-20 Todo 长按日期、月历内容层和输入合帧修复后的 Release：Empty 稳定复测为 18.0746 ms 启动、0.1142 ms 显隐 P95、2,269,184 私有字节、7 线程、0% 空闲 CPU；Typical 为 111.09 ms、7.0531 ms、5,537,792 私有字节、7 线程、0%，两项均为 `passed=1`。Empty 首次采样出现 1.53899% 瞬时 CPU 并失败，连续两次复测均为 0%，视为本机调度噪声，不据此宣称退化。Popup 的 TSF 文字、选区、组合串和光标重绘请求现在由私有窗口消息合并，同一消息周期只执行一次 `UpdateLayeredWindow`；回归以 20 次光标请求和一次输入只增加一次 paint 固化。

```powershell
build\benchmarks\Release\desto_desktop_performance_gate.exe --scenario empty
build\benchmarks\Release\desto_desktop_performance_gate.exe --scenario typical
```

## Initial Targets

### 2026-08-22 Uniscribe input and visibility commit

Release `desto_desktop_performance_gate` 在同一工作区运行。将 Card 显隐提交从带 `HWND_BOTTOM`/`HWND_TOPMOST` 的 Z 序重排改为保留现有层级的 `SWP_NOZORDER` 后，Typical 的显隐 P95 从本轮修复前的 `20.08-25.55 ms` 降至 `4.22-5.87 ms`；Empty 为 `0.11-1.39 ms`。三轮复测中空闲 CPU 通常为 `0%`，偶发采样噪声不超过门禁所记录的系统调度波动；成功轮次均满足 `30/50 MB` 私有内存、`500 ms` 启动和 `16 ms` 显隐目标。文本输入测试同时覆盖 Uniscribe 的中文、Emoji 代理对和 RTL 命中路径。

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

时间驱动 Extension Card、数字转场帧、局部金额提交统计和工资性能门禁已撤销。当前宿主没有为该原型保留 timer、deadline、字体缓存或空闲唤醒成本。若未来引入周期刷新能力，必须重新定义可重复场景、常驻成本、释放时机和失败预算，历史工资门禁数据不能作为新实现基线。
