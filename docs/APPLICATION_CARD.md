# Application Card

## Ownership

Application Card 管理存储根下自己的相对目录。Explorer 拖入的文件或目录会被移动到该目录，而不是只保存外部路径。导入使用文件事务；同批任一移动失败时回滚已经完成的移动。

重名项目保留原扩展名并依次使用 `Name (1)`、`Name (2)`。已经位于 Card 目录内的项目不会重复移动，包含 Card 存储目录的上级目录不能被拖入，避免递归复制或移动自身。

## Shell Presentation

Windows Adapter 枚举 Card 目录并生成不可变项目视图：

- 普通文件和目录使用 Shell 显示名称与真实 Shell 图标。
- `.lnk` 标题忽略扩展名，并解析目标路径。
- 商店应用快捷方式读取 `PKEY_AppUserModel_ID`；图标优先从 `shell:AppsFolder` 的真实应用项提取。
- 缺失项目、不可解析快捷方式和图标不可用是不同状态。
- 图标不可用时不绘制 `FILE`、字母块或其他伪造图标；名称仍保留，项目仍可在可启动时打开。

悬停提示只显示项目名称；失效项目追加简短状态，不显示完整文件路径。双击通过 Windows Shell 打开原始项目。

## Drag And Ordering

宿主使用 OLE `IDropTarget` 接收 Explorer 的 `CF_HDROP`，在 `DragOver` 阶段把指针转换为内容网格插入索引。预选槽位占据真实布局位置并让后续项目即时让位；只有插入索引变化时才重绘。Drop 成功后先完成文件事务，再把相对文件名顺序提交给 `ApplicationRuntime`。

项目按下后必须超过 Windows 系统拖动阈值才进入 `DoDragDrop`，因此单击和双击启动不会被误判为拖出。数据源提供 `CF_HDROP` 和首选移动效果；拖到 Explorer 后按返回效果重新枚举源 Card，同一卡片内 Drop 则复用同一路径完成排序。取消拖动不改变文件和顺序。

图标规格和名称显隐属于每个 Card 的内容偏好，不属于全局渲染常量。设置入口由事项 23 的主界面提供；当前宿主与快照已经消费并持久化这两个值。

## Rendering And Cost

Shell 图标在目录枚举时按需解析为 64x64 预乘 Alpha 像素，Card 的多屏 Projection 共享同一像素所有权，不重复保存图标位图。当前版本在启动和拖入后刷新；目录实时监听、增量失效和大目录缓存属于事项 27。
