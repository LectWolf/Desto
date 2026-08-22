#pragma once

#include <Windows.h>
#include <objidl.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace desto::platform::windows {

struct FileDropTargetCallbacks {
    std::function<DWORD(
        POINTL,
        DWORD,
        DWORD,
        const std::optional<std::string>&)> dragOver;
    std::function<void()> dragLeave;
    std::function<DWORD(
        std::vector<std::filesystem::path>,
        std::optional<std::string>,
        POINTL,
        DWORD,
        DWORD)> drop;
};

struct FileDragResult {
    HRESULT status = E_FAIL;
    DWORD effect = DROPEFFECT_NONE;
    bool completedInsideDesto = false;
};

[[nodiscard]] IDropTarget* CreateFileDropTarget(FileDropTargetCallbacks callbacks);
[[nodiscard]] IDataObject* CreateFileDataObject(
    const std::vector<std::filesystem::path>& paths,
    std::optional<std::string> sourceCardId = std::nullopt,
    bool allowMove = true,
    bool exposeToShell = true);
[[nodiscard]] FileDragResult BeginFileDrag(
    const std::vector<std::filesystem::path>& paths,
    std::optional<std::string> sourceCardId = std::nullopt,
    bool allowMove = true,
    bool exposeToShell = true);
[[nodiscard]] bool WasFileDropHandledByDesto(IDataObject* data) noexcept;
[[nodiscard]] std::optional<DWORD> PerformedFileDropEffect(IDataObject* data) noexcept;

} // namespace desto::platform::windows
