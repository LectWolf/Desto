# MappingCard

MappingCard 展示不归 Desto 所有的外部内容。删除 Card 只删除映射关系，不移动或删除外部文件。

## Source modes

- References：默认形态，保存一组稳定引用 ID 和绝对路径。文件和文件夹都只建立引用，不移动原项目；目标移动或删除后，项目保留并显示 Missing 状态。
- Folder：唯一映射一个绝对目录，目录内容变化会实时刷新 Card。已绑定的目录即使当前为空，仍是完整的 Folder 来源：外部文件/文件夹可真实移动进入，目录中的项目也可拖出；只有尚未绑定 `sourceRoot` 的 Folder Card 才把单个目录 Drop 作为选择来源。

Folder 和 References 互斥。来源形态是显式实例配置，即使 Card 为空也会持久化；清空来源不会把形态改回不确定状态。切换来源由 `ApplicationRuntime` 串行执行，只有新状态完整校验成功后才替换旧状态。

## Presentation modes

- Grid：使用 Card 实例的图标档位和网格布局。
- List：每个引用占一行，始终显示经过省略处理的名称。

展示形态与来源形态独立保存。切换 Grid/List 不改变来源目录、引用集合、排序数据或当前图标快照，也不触发目录枚举。卡片标题栏中的视图按钮提供即时切换；设置页只显示或隐藏该按钮，不直接切换展示形态。

List 把 Custom 稀疏 Grid 槽位按 `(row,column)` 顺序临时展平为连续单列，每项严格占一行；切回 Grid 后原槽位恢复。切换造成的高度变化会保存当前 Card Placement 并移动下方堆叠链，follower 不重绘。若完整内容会侵占下方 Card 或越过工作区，宿主按完整行限制当前 Card 高度并启用内部滚动。

## Folder ownership

同一规范化目录只能被一个 MappingCard 占用。`domain::MappingSourceRegistry` 与 Runtime 状态一起恢复、更新和删除，界面层不参与维护该约束。恢复配置遇到重复目录时会整体拒绝，不留下半恢复状态。

MappingCard 的拖放语义由来源形态决定。References 根内容的外部或其他 Card 拖入宣告 `COPY`，只增加引用，不移动原项目；根文件拖出同样只允许 `COPY`。根引用文件夹不向 Explorer 暴露 `CF_HDROP`，只携带 Desto 内部拖放格式，因此外部目标不能先复制文件夹；手势在 Desto 外释放后仅弹出自绘“移除引用”确认。根内容在卡片内拖动只改变槽位，不触发移除确认，取消后拖动状态也会清理并可再次操作。进入引用文件夹后，文件和文件夹使用真实 Windows 文件拖放语义，目标目录由当前导航路径决定并通过 `DirectoryImportPlanner` 与 `FileMoveTransaction` 直接移动到源文件夹；同名目标按 `Name (N).ext` 安全改名，事务失败不留下半移动状态。锁定 Card 时所有拖入、拖出和重排均禁用。Folder 尚未设置来源时，拖入单个目录以 `COPY` 效果选择来源，绝不让 Explorer 误删未移动的目录；设置或切换到文件夹来源后，已有来源（包括内容为空的目录）外部拖入宣告 `MOVE`，并通过 `DirectoryImportPlanner` 与 `FileMoveTransaction` 把文件或目录真实移动到 `sourceRoot`。按住 Ctrl 时目标效果为 `COPY`，使用 `FileCopyTransaction`，源文件保留。文件夹来源卡片拖出继续使用同一源路径，外部目标按 Windows Shell 的 MOVE/COPY 结果执行；切换到 References 时领域命令自动关闭 `allowsSourceMutation`，保持只读引用语义。`allowsSourceMutation` 保留为旧配置兼容字段，但不再阻止文件夹来源拖放；真正禁用修改应使用卡片锁定。

Folder 移动提交成功后立即从来源目录重建项目与自定义槽位，再提交 Card 画面和配置；从 MappingCard 拖到 Explorer 或其他外部目标后，宿主在 OLE 手势结束时无条件重建源 Card（即使目标回报的最终 effect 为 NONE），避免移动已完成而缓存项目仍可见。目录监听随后收到的系统事件只会收敛到相同快照。从其他 Desto Card 移入时，标准 OLE `Performed DropEffect=MOVE` 让源 Card 在拖动结束后刷新，避免源目标各自执行第二次文件事务。删除 MappingCard 始终只删除映射关系，不删除来源目录或其中内容。

## Live refresh

`WindowsDirectoryChangeSource` 使用一个 IO completion port 工作线程监听全部 Folder MappingCard。每个目录持有一个文件句柄和 4 KiB 缓冲；60 ms 静默窗口合并重复事件，并按 Card ID 去重。运行时切换或选择来源后会替换监听集合，无需重启应用。

工作线程只提交 Card ID。`WindowsDesktopHost::requestCardItemsRefresh` 将请求合并后投递到宿主线程，目录枚举、Shell 元数据/图标解析、内容尺寸计算和 Layered Window 提交均在宿主线程完成。

Folder 投影不展示带 `FILE_ATTRIBUTE_HIDDEN` 的直接子项。过滤同时位于首次 `enumerate` 和 `refreshDirectoryEntries` 两条路径；文件从可见改为隐藏时，增量更新会先删除旧项目再拒绝重新加入。Grid/List 只改变展示，不绕过该过滤。可见项目在两种展示中均使用该文件原有的 Explorer 右键菜单。

文件项不提供 Desto 内部重命名入口。需要改名时由资源管理器完成，Folder 目录监听或 References 手动刷新随后重新枚举源项目。

References 不持有目录监听资源；每次刷新逐项检查目标，Missing 项目不会从引用集合中自动删除。

## In-card folder navigation

Grid 与 List 中的目录都可双击进入，文件仍使用原 Shell 激活。导航状态是桌面宿主进程中的会话数据，不写入 MappingCard、配置或用户标题：进入后 Card 项目替换为当前目录的直接子项，标题临时显示文件夹名，标题左侧出现向上按钮；继续双击子目录可任意深入。

Folder 模式以 `sourceRoot` 内容为导航根，返回按钮最多回到来源根内容。References 模式以原引用集合为导航根；进入任意被引用目录后按路径栈返回，栈清空时恢复原引用图标集合、原标题、排序和自定义 Placement。导航目录不继承引用根的稀疏 Placement，滚动从第一行开始。目录监听仍只持有 Folder 来源根；浏览子目录期间用户刷新会全量枚举当前目录。

References 根模式的右键“删除”表示移除引用，且先显示确认；Windows 11 紧凑菜单直接显示“移除引用”；经典菜单若选中 Shell canonical `delete`，宿主会拦截并调用 `SetMappingReferences`，随后协调绝对路径 Placement 并保存。进入引用文件夹后，删除是对真实源文件的操作，删除先显示全局确认（可在功能设置中关闭）；重命名需在资源管理器中完成。源文件夹根映射本身不会因移除引用而删除。
