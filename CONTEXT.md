# Desto

Desto 组织桌面内容，并在显示器连接状态变化时保留用户安排的原始意图。

## Language

**Card**:
可独立配置外观、行为和内容来源的桌面信息单元。
_Avoid_: Box, Drawer, Container

**Workspace**:
一组 Card 及其跨显示器安排所组成的用户桌面环境。
_Avoid_: Desktop, Scene

**Placement**:
一个 Card 在指定显示目标中的持久化位置和尺寸；同一 Card 可以拥有多个 Placement。
_Avoid_: Window position, CardRect

**Display Target**:
Placement 希望出现的单个显示器或全部显示器范围。
_Avoid_: Monitor index, Screen number

**Display Identity**:
跨重启和枚举顺序变化仍可恢复同一显示器的稳定身份。
_Avoid_: Display handle, Enumeration index

**Display Snapshot**:
当前已连接显示器及其可用工作区的瞬时描述。
_Avoid_: Saved monitor, Display configuration

**Projection**:
根据持久化 Placement 和当前 Display Snapshot 得到的临时可显示结果。
_Avoid_: Migrated placement, Rewritten layout

**Fallback Projection**:
目标显示器不可用时，在另一显示器上临时呈现的 Projection；它不改变原始 Placement。
_Avoid_: Moved card, Reassigned placement

**Content Density**:
文件内容在 Card 中的视觉密度预设，决定图标资源档位、图标尺寸、文字区节拍和建议列数。当前稳定档位为小、中、大、特大；它不是 Card 的物理宽度。
_Avoid_: Card size, Grid width

**Card Width Policy**:
Card 内容区域的布局宽度规则，分为自适应和固定格数；固定格数是布局覆盖值，不改变 Content Density 的图标来源。
_Avoid_: Icon size, Card density

**Card Visibility**:
单个 Card 是否参与当前 Workspace 的正常 Projection，是实例配置。它与桌面双击或任务栏双击造成的全局显隐会话无关。
_Avoid_: Global visibility, Hidden session

**Card Chrome**:
所有 Card 共有的标题栏入口和交互状态，包括收缩、置顶及其可见性；具体内容类型只扩展自己的内容操作，不重复实现 Chrome。
_Avoid_: Card-specific toolbar, Type controls
