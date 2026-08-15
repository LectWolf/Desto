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
