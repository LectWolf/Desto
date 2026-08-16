#pragma once

#include <Windows.h>
#include <objidl.h>

#include <filesystem>
#include <functional>
#include <vector>

namespace desto::platform::windows {

struct FileDropTargetCallbacks {
    std::function<DWORD(POINTL, DWORD)> dragOver;
    std::function<void()> dragLeave;
    std::function<DWORD(std::vector<std::filesystem::path>, POINTL, DWORD)> drop;
};

struct FileDragResult {
    HRESULT status = E_FAIL;
    DWORD effect = DROPEFFECT_NONE;
};

[[nodiscard]] IDropTarget* CreateFileDropTarget(FileDropTargetCallbacks callbacks);
[[nodiscard]] IDataObject* CreateFileDataObject(
    const std::vector<std::filesystem::path>& paths);
[[nodiscard]] FileDragResult BeginFileDrag(
    const std::vector<std::filesystem::path>& paths);

} // namespace desto::platform::windows
