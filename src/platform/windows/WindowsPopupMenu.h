#pragma once

#include <Windows.h>

#include <cstdint>
#include <span>
#include <string_view>

namespace desto::platform::windows {

enum class WindowsFileContextMenuMode {
    Compact,
    Classic,
};

[[nodiscard]] WindowsFileContextMenuMode ResolveWindowsFileContextMenuMode(
    std::uint32_t windowsBuild,
    bool classicMenuOverride) noexcept;
[[nodiscard]] WindowsFileContextMenuMode CurrentWindowsFileContextMenuMode() noexcept;

struct WindowsPopupMenuItem {
    UINT command = 0;
    std::wstring_view label;
    std::wstring_view glyph;
    bool separatorBefore = false;
    bool danger = false;
    bool enabled = true;
    std::wstring_view shortcut;
    bool submenu = false;
};

// The caller owns the returned menu and must destroy it with DestroyMenu.
[[nodiscard]] HMENU CreateNativeWindowsPopupMenu(
    std::span<const WindowsPopupMenuItem> items) noexcept;
[[nodiscard]] UINT ShowWindowsPopupMenu(
    HWND owner,
    POINT point,
    std::span<const WindowsPopupMenuItem> items) noexcept;

} // namespace desto::platform::windows
