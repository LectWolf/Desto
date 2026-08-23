#include "WindowsTrayHost.h"
#include "WindowsPopupMenu.h"

#include <Windows.h>
#include <shellapi.h>

#include <stdexcept>
#include <array>
#include <utility>

namespace desto::platform::windows {

std::uint32_t ResolveDestoTrayIconFlags() noexcept {
    return NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
}

namespace {

constexpr UINT TrayMessage = WM_APP + 0x71;
constexpr UINT OpenSettingsCommand = 1;
constexpr UINT ExitCommand = 2;
constexpr UINT ToggleDesktopCommand = 3;
constexpr int DestoIconResourceId = 101;

} // namespace

struct WindowsTrayHost::Impl {
    explicit Impl()
        : module(GetModuleHandleW(nullptr)) {
        WNDCLASSEXW windowClass{
            .cbSize = sizeof(WNDCLASSEXW),
            .style = 0,
            .lpfnWndProc = &WindowProcedure,
            .hInstance = module,
            .hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(IDC_ARROW)),
            .hbrBackground = nullptr,
            .lpszClassName = className,
        };
        if (RegisterClassExW(&windowClass) == 0
            && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            throw std::runtime_error("Unable to register tray window class.");
        }
        window = CreateWindowExW(
            WS_EX_TOOLWINDOW,
            className,
            L"Desto",
            WS_POPUP,
            0, 0, 1, 1,
            nullptr,
            nullptr,
            module,
            this);
        if (window == nullptr) throw std::runtime_error("Unable to create tray window.");

        icon.cbSize = sizeof(NOTIFYICONDATAW);
        icon.hWnd = window;
        icon.uID = 1;
        icon.uFlags = ResolveDestoTrayIconFlags();
        icon.uCallbackMessage = TrayMessage;
        icon.hIcon = static_cast<HICON>(LoadImageW(
            module,
            MAKEINTRESOURCEW(DestoIconResourceId),
            IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON),
            GetSystemMetrics(SM_CYSMICON),
            LR_DEFAULTCOLOR | LR_SHARED));
        if (icon.hIcon == nullptr) {
            icon.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(IDI_APPLICATION));
        }
        wcsncpy_s(icon.szTip, L"Desto", _TRUNCATE);
        if (!Shell_NotifyIconW(NIM_ADD, &icon)) {
            DestroyWindow(window);
            window = nullptr;
            throw std::runtime_error("Unable to create the Desto tray icon.");
        }
        icon.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &icon);
    }

    ~Impl() {
        if (window != nullptr) {
            Shell_NotifyIconW(NIM_DELETE, &icon);
            DestroyWindow(window);
        }
    }

    static LRESULT CALLBACK WindowProcedure(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept {
        auto* instance = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            instance = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(instance));
        }
        if (instance == nullptr) return DefWindowProcW(window, message, wParam, lParam);
        if (message == TrayMessage) {
            const auto event = static_cast<UINT>(LOWORD(lParam));
            if (event == WM_LBUTTONDBLCLK) {
                if (instance->openSettings) instance->openSettings();
                return 0;
            }
            if (event == WM_RBUTTONUP) {
                instance->showMenu();
                return 0;
            }
            return 0;
        }
        if (message == WM_COMMAND) {
            switch (LOWORD(wParam)) {
            case OpenSettingsCommand:
                if (instance->openSettings) instance->openSettings();
                return 0;
            case ExitCommand:
                if (instance->exitApplication) instance->exitApplication();
                return 0;
            case ToggleDesktopCommand:
                if (instance->toggleDesktop) instance->toggleDesktop();
                return 0;
            default:
                break;
            }
        }
        if (message == WM_DESTROY) {
            instance->window = nullptr;
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    void showMenu() noexcept {
        POINT cursor{};
        GetCursorPos(&cursor);
        const auto english = language == "en-US";
        const std::array items{
            WindowsPopupMenuItem{
                ToggleDesktopCommand,
                english
                    ? (desktopVisible ? L"Hide desktop cards" : L"Show desktop cards")
                    : (desktopVisible ? L"隐藏桌面卡片" : L"显示桌面卡片"),
                desktopVisible ? L"\uE8EE" : L"\uE8F4"},
            WindowsPopupMenuItem{
                OpenSettingsCommand,
                english ? L"Open settings" : L"打开设置",
                L"\uE713"},
            WindowsPopupMenuItem{
                ExitCommand,
                english ? L"Exit" : L"退出",
                L"\uE8BB",
                true,
                false},
        };
        const auto selected = ShowWindowsPopupMenu(window, cursor, items);
        if (selected == OpenSettingsCommand && openSettings) openSettings();
        if (selected == ToggleDesktopCommand && toggleDesktop) toggleDesktop();
        if (selected == ExitCommand && exitApplication) exitApplication();
    }

    HINSTANCE module = nullptr;
    HWND window = nullptr;
    NOTIFYICONDATAW icon{};
    Callback openSettings;
    Callback toggleDesktop;
    Callback exitApplication;
    std::string language = "zh-CN";
    bool desktopVisible = true;
    static constexpr const wchar_t* className = L"DestoTrayWindow";
};

WindowsTrayHost::WindowsTrayHost()
    : impl_(std::make_unique<Impl>()) {}

WindowsTrayHost::~WindowsTrayHost() = default;

void WindowsTrayHost::setOpenSettingsCallback(Callback callback) {
    impl_->openSettings = std::move(callback);
}

void WindowsTrayHost::setToggleDesktopCallback(Callback callback) {
    impl_->toggleDesktop = std::move(callback);
}

void WindowsTrayHost::setExitCallback(Callback callback) {
    impl_->exitApplication = std::move(callback);
}

void WindowsTrayHost::setLanguage(std::string language) {
    if (language != "zh-CN" && language != "en-US") {
        throw std::invalid_argument("Tray language must be resolved.");
    }
    impl_->language = std::move(language);
}

void WindowsTrayHost::setDesktopVisible(bool visible) noexcept {
    impl_->desktopVisible = visible;
}

} // namespace desto::platform::windows
