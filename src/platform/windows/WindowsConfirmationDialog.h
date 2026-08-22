#pragma once

#include <Windows.h>

#include <string_view>

#include "Dialog.h"

namespace desto::platform::windows {

// Shows a small owner-modal confirmation surface whose content and controls
// are drawn by Desto. The return value is true only for the confirm action.
[[nodiscard]] bool ShowWindowsConfirmation(
    HWND owner,
    std::wstring_view title,
    std::wstring_view message,
    std::wstring_view confirmLabel = L"确定",
    std::wstring_view cancelLabel = L"取消") noexcept;

[[nodiscard]] bool ShowWindowsDialog(
    HWND owner,
    desto::ui::DialogSpec spec) noexcept;

[[nodiscard]] bool ShowWindowsAlert(
    HWND owner,
    std::wstring_view title,
    std::wstring_view message,
    std::wstring_view buttonLabel = L"确定") noexcept;

} // namespace desto::platform::windows
