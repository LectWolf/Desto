# Multi-display layout uses persistent placements and temporary projections

- Status: Accepted
- Date: 2026-08-16

Desto 将位置从 Card 移到 WorkspaceLayout 的 Placement：Card 默认绑定一个由平台提供稳定身份的显示器；“所有显示器”是用户未来可显式选择的投影模式，不是默认值。Placement 保存相对显示器工作区的 DIP 坐标、边缘或中心锚点，以及记录位置时的参考工作区尺寸。

目标显示器断开时不生成替代 Projection，也不把 Card 投影到其他屏幕。原 Placement 留在离线显示器分组中，重连后按原身份恢复；未来设置界面允许用户把它转移到在线显示器，转移只更新 DisplayTarget，不复制 Card。分辨率、缩放或工作区变化时，边缘锚点保留边距，中心锚点保留中心偏移，自由位置按可移动范围等比映射，最后把溢出部分夹回工作区。

这样避免依赖不稳定的显示器枚举序号，也避免断屏事件让 Card 突然出现在当前工作屏幕或永久改写用户布局。
