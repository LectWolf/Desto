#include "WindowsPopupMenu.h"

#include <string>

namespace desto::platform::windows {
namespace {

constexpr wchar_t kClassicContextMenuOverridePath[] =
    L"Software\\Classes\\CLSID\\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}\\InprocServer32";

std::uint32_t currentWindowsBuild() noexcept {
    using RtlGetVersion = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    const auto module = GetModuleHandleW(L"ntdll.dll");
    const auto rtlGetVersion = module == nullptr ? nullptr
        : reinterpret_cast<RtlGetVersion>(GetProcAddress(module, "RtlGetVersion"));
    if (rtlGetVersion == nullptr) return 0;
    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    return rtlGetVersion(&version) == 0 ? version.dwBuildNumber : 0;
}

bool classicContextMenuOverrideEnabled() noexcept {
    HKEY key = nullptr;
    const auto opened = RegOpenKeyExW(
        HKEY_CURRENT_USER,
        kClassicContextMenuOverridePath,
        0,
        KEY_QUERY_VALUE,
        &key);
    if (opened != ERROR_SUCCESS) return false;
    RegCloseKey(key);
    return true;
}

enum class PreferredAppMode : int {
    Default,
    AllowDark,
    ForceDark,
    ForceLight,
    Maximum,
};

bool systemAppsUseDarkTheme() noexcept {
    DWORD lightTheme = 1;
    DWORD size = sizeof(lightTheme);
    const auto result = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",
        RRF_RT_REG_DWORD,
        nullptr,
        &lightTheme,
        &size);
    return result == ERROR_SUCCESS && lightTheme == 0;
}

void applySystemMenuTheme() noexcept {
    const auto dark = systemAppsUseDarkTheme();
    const auto theme = LoadLibraryW(L"uxtheme.dll");
    if (theme != nullptr) {
        using SetPreferredAppMode = PreferredAppMode(WINAPI*)(PreferredAppMode);
        using FlushMenuThemes = void(WINAPI*)();
        const auto setPreferredAppMode = reinterpret_cast<SetPreferredAppMode>(
            GetProcAddress(theme, MAKEINTRESOURCEA(135)));
        const auto flushMenuThemes = reinterpret_cast<FlushMenuThemes>(
            GetProcAddress(theme, MAKEINTRESOURCEA(136)));
        if (setPreferredAppMode != nullptr) {
            setPreferredAppMode(dark
                ? PreferredAppMode::ForceDark
                : PreferredAppMode::ForceLight);
        }
        if (flushMenuThemes != nullptr) flushMenuThemes();
        FreeLibrary(theme);
    }
}

std::wstring nativeLabel(const WindowsPopupMenuItem& item) {
    std::wstring label(item.label);
    if (!item.shortcut.empty()) {
        label.push_back(L'\t');
        label.append(item.shortcut);
    }
    return label;
}

} // namespace

WindowsFileContextMenuMode ResolveWindowsFileContextMenuMode(
    std::uint32_t windowsBuild,
    bool classicMenuOverride) noexcept {
    return windowsBuild >= 22000 && !classicMenuOverride
        ? WindowsFileContextMenuMode::Compact
        : WindowsFileContextMenuMode::Classic;
}

WindowsFileContextMenuMode CurrentWindowsFileContextMenuMode() noexcept {
    return ResolveWindowsFileContextMenuMode(
        currentWindowsBuild(), classicContextMenuOverrideEnabled());
}

HMENU CreateNativeWindowsPopupMenu(
    std::span<const WindowsPopupMenuItem> items) noexcept {
    const auto menu = CreatePopupMenu();
    if (menu == nullptr) return nullptr;
    try {
        for (const auto& item : items) {
            if (item.separatorBefore
                && !AppendMenuW(menu, MF_SEPARATOR, 0, nullptr)) {
                DestroyMenu(menu);
                return nullptr;
            }
            const auto label = nativeLabel(item);
            const auto flags = MF_STRING | MF_BYCOMMAND
                | (item.enabled ? MF_ENABLED : MF_GRAYED);
            if (!AppendMenuW(menu, flags, item.command, label.c_str())) {
                DestroyMenu(menu);
                return nullptr;
            }
        }
    } catch (...) {
        DestroyMenu(menu);
        return nullptr;
    }
    return menu;
}

UINT ShowWindowsPopupMenu(
    HWND owner,
    POINT point,
    std::span<const WindowsPopupMenuItem> items) noexcept {
    if (owner == nullptr || items.empty()) return 0;
    const auto menu = CreateNativeWindowsPopupMenu(items);
    if (menu == nullptr) return 0;

    applySystemMenuTheme();
    SetForegroundWindow(owner);
    const auto command = TrackPopupMenuEx(
        menu,
        TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_WORKAREA,
        point.x,
        point.y,
        owner,
        nullptr);
    DestroyMenu(menu);
    PostMessageW(owner, WM_NULL, 0, 0);
    return command;
}

} // namespace desto::platform::windows
