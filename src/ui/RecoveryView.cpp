#include "RecoveryView.h"

namespace desto::ui {

DialogSpec RecoveryView::backupAvailable(std::wstring_view backupName) {
    return {
        .title = L"恢复 Desto 配置",
        .message = L"主配置无法读取。是否使用备用配置 " + std::wstring(backupName)
            + L" 恢复？\n损坏的主配置会保留为 .corrupt 文件。",
        .confirmLabel = L"使用备用配置",
        .cancelLabel = L"退出",
    };
}

DialogSpec RecoveryView::noUsableConfiguration() {
    return {
        .title = L"配置文件损坏",
        .message = L"主配置和备用配置都无法读取。是否创建空 Workspace？\n原文件不会被删除。",
        .confirmLabel = L"创建空 Workspace",
        .cancelLabel = L"退出",
    };
}

} // namespace desto::ui
