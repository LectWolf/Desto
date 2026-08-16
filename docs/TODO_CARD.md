# TodoCard

TodoCard 保存稳定 ID、UTF-8 标题、完成状态和显式用户顺序。内容不依赖文件系统；切换完成状态和编辑标题不会隐式重排，只有用户拖动行才改变保存顺序。

## Commands And Validation

`ApplicationRuntime` 提供新增、重命名、切换完成、删除和完整顺序重排命令。每个命令先在副本上完成校验，再整体替换 TodoCard 内容：

- ID 必须非空且在 Card 内唯一。
- 标题必须包含可见字符，UTF-8 长度不超过 512 字节。
- 重排必须恰好包含当前全部 ID 且每个 ID 只出现一次。
- 缺失项目、重复 ID、空标题和不完整顺序会被拒绝，原状态与 Revision 不变。

成功变更只标记当前 Card，使用 Deferred 持久化；Snapshot 与 schema 5 配置按数组顺序保存标题和完成状态，恢复时再次执行相同领域校验。

## Native Interaction

展开的 TodoCard 保留至少一行内容高度。标题栏的加号和空状态行都可新增项目；单击圆形勾选控件切换完成状态，双击文字打开编辑，拖动行改变顺序，右键菜单删除项目。悬停行与标题栏入口均提供热点，停留后 Tooltip 展示完整标题。

文本输入使用按需创建的原生 Edit 弹层：Enter 或失焦提交，Escape 取消。弹层关闭后立即销毁，Card 窗口继续使用 `WS_EX_NOACTIVATE`，因此非编辑状态没有额外窗口、焦点和常驻控件成本。同一 Card 的多屏 Projection 在一次领域变更后同步更新内容与高度。

## Rendering And Cost

待办行直接绘制到 Card 现有离屏位图，不为每一行创建 HWND。常驻增量仅为 TodoItem 文本和少量交互状态；编辑控件只在用户输入期间存在。内容高度按 36 DIP 行步长计算，空 Card 保留一行，收起状态只提交标题区域。
