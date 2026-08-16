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

## Rendering And Cost

Shell 图标在目录枚举时按需解析为 64x64 预乘 Alpha 像素，Card 的多屏 Projection 共享同一像素所有权，不重复保存图标位图。当前版本在启动和拖入后刷新；目录实时监听、增量失效和大目录缓存属于事项 27。
