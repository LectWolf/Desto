# Desto v2 重构边界

## 目标

v2 的目标不是增加功能，而是删掉无意义复杂度，减少生产 Bug，并让每个稳定模块通过小接口承载更多行为。用户可观察行为保持兼容；实现细节、测试辅助代码和旧渲染路径可以替换。

## 模块依赖

```text
desto-domain
    -> desto-application
        -> desto-storage
        -> desto-presentation
            -> desto-ui
                -> desto-platform-windows
                    -> desto-app
```

依赖方向只能向下。Domain 不依赖 Windows、JSON、GDI 或 HWND；Application 不创建窗口和线程；Storage 不弹对话框；UI 不直接修改配置文件或 Card；平台 Adapter 不承载业务规则。

## DestoUi

`DestoUi` 统一提供状态、布局、焦点、命中测试、命令和 GDI DIB 提交。第一批组件为 `Surface`、`Dialog`、`Button`、`TextInput`、`List`、`Menu` 和 `RecoveryView`。组件只暴露输入事件和绘制结果，不暴露 HWND 给业务层。

首个纵向切片由 `desto_ui` 的 `Dialog` 和 `RecoveryView` 提供。`Dialog` 接收与平台无关的尺寸、指针和按键事件，输出不可变绘制快照与 `Confirm`/`Cancel` 命令；`RecoveryView` 只负责把恢复场景投影为对话框规格。`WindowsConfirmationDialog` 是该 seam 的首个 Adapter，只转换 Win32 消息并使用 GDI 绘制快照，不再拥有按钮布局、焦点或命中规则。后续组件沿用同一方向迁入，不为尚未存在的第二种绘制实现建立可替换体系。

输入法适配仍是 Windows 平台能力，但文本状态、选择区、组合串、撤销和光标属于 `TextInput`。绘制后端只接受不可变文本快照和布局结果，使用 GDI DIB 完成提交。

`TextInputModel` 已作为第二个纵向切片迁入 `desto_ui`，统一承载文本、方向选择区、UTF-16 代理对光标移动、按词移动、长度限制、替换和有界撤销历史。`WindowsTextInput` 继续作为 Adapter，把 Win32/TSF 输入转换为模型操作并向 TSF 报告变化；组合范围、文本测量与命中布局暂留 Adapter，等 GDI/Uniscribe 反馈信号覆盖后再迁移，避免一次改动同时改变输入状态和字形布局。

## 配置恢复接口

```text
ConfigRepository::inspect()
ConfigRepository::loadPrimary()
ConfigRepository::loadBackup(id)
ConfigRepository::promoteBackup(id)
ConfigRepository::save(config)
```

`inspect` 只返回主文件和备份的有效性、版本、代数和诊断信息；恢复界面根据结果请求用户选择。Repository 不依赖 UI，也不自动做提升操作。
