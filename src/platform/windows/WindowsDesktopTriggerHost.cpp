#include "WindowsDesktopTriggerHost.h"

#include <Windows.h>
#include <CommCtrl.h>
#include <dwmapi.h>
#include <oleacc.h>
#include <shlobj.h>
#include <UIAutomation.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#undef max
#undef min

namespace desto::platform::windows {

bool IsBlankTaskbarAccessibilityTarget(
    bool querySucceeded,
    long role,
    bool directTaskbarHit) noexcept {
    if (!querySucceeded) return false;
    // A taskbar button and tray icon are reported as CLIENT by MSAA too.  A
    // blank taskbar hit is either the taskbar root itself or an unlabelled
    // pane; never treat a child CLIENT as blank.
    // Any child window belongs to a task button, tray, Start/search or an
    // overflow surface. Only the taskbar root itself is considered blank.
    return role == ROLE_SYSTEM_PANE
        || (directTaskbarHit && role == ROLE_SYSTEM_CLIENT);
}

bool IsBlankTaskbarAutomationTarget(
    bool querySucceeded,
    long controlType,
    bool hasMeaningfulIdentity) noexcept {
    if (!querySucceeded) return false;

    // Pane/toolbar/window/custom are neutral taskbar containers. Buttons,
    // list items, menu items and edits represent an actual taskbar control.
    const auto neutralContainer = controlType == UIA_PaneControlTypeId
        || controlType == UIA_ToolBarControlTypeId
        || controlType == UIA_WindowControlTypeId
        || controlType == UIA_CustomControlTypeId;
    const auto interactiveType = controlType == UIA_ButtonControlTypeId
        || controlType == UIA_SplitButtonControlTypeId
        || controlType == UIA_EditControlTypeId
        || controlType == UIA_ListItemControlTypeId
        || controlType == UIA_MenuItemControlTypeId
        || controlType == UIA_TabItemControlTypeId;
    return !interactiveType && (!hasMeaningfulIdentity || neutralContainer);
}

namespace {

constexpr UINT DesktopDoubleClickMessage = WM_APP + 0x72;

HWND FindDesktopListView() noexcept {
    auto shellView = FindWindowExW(
        FindWindowW(L"Progman", nullptr), nullptr, L"SHELLDLL_DefView", nullptr);
    if (shellView == nullptr) {
        EnumWindows(+[](HWND window, LPARAM parameter) -> BOOL {
            auto& result = *reinterpret_cast<HWND*>(parameter);
            result = FindWindowExW(window, nullptr, L"SHELLDLL_DefView", nullptr);
            return result == nullptr ? TRUE : FALSE;
        }, reinterpret_cast<LPARAM>(&shellView));
    }
    return shellView == nullptr
        ? nullptr : FindWindowExW(shellView, nullptr, L"SysListView32", nullptr);
}

bool IsDesktopHostWindow(HWND window) noexcept {
    wchar_t className[64]{};
    if (window == nullptr || GetClassNameW(window, className, 64) <= 0) return false;
    return wcscmp(className, L"Progman") == 0
        || wcscmp(className, L"WorkerW") == 0
        || wcscmp(className, L"SHELLDLL_DefView") == 0;
}

bool IsBlankListViewPoint(HWND listView, POINT screenPoint) noexcept {
    POINT clientPoint = screenPoint;
    if (!ScreenToClient(listView, &clientPoint)) return false;
    DWORD processId = 0;
    GetWindowThreadProcessId(listView, &processId);
    const auto process = OpenProcess(
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE, FALSE, processId);
    if (process == nullptr) return false;

    struct RemoteHitTest {
        POINT point{};
        UINT flags = 0;
        int item = -1;
        int subItem = -1;
        int group = -1;
    } hitTest{clientPoint};
    const auto remote = VirtualAllocEx(
        process, nullptr, sizeof(hitTest), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote == nullptr) {
        CloseHandle(process);
        return false;
    }
    SIZE_T written = 0;
    const auto wrote = WriteProcessMemory(
        process, remote, &hitTest, sizeof(hitTest), &written)
        && written == sizeof(hitTest);
    DWORD_PTR result = 0;
    const auto sent = wrote && SendMessageTimeoutW(
        listView, LVM_HITTEST, 0, reinterpret_cast<LPARAM>(remote),
        SMTO_ABORTIFHUNG, 100, &result) != 0;
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
    return sent && static_cast<int>(result) == -1;
}

bool IsBlankDesktopPoint(POINT point) noexcept {
    const auto clicked = WindowFromPoint(point);
    if (clicked == nullptr) return false;
    const auto listView = FindDesktopListView();
    if (listView != nullptr && IsWindowVisible(listView) && clicked == listView) {
        return IsBlankListViewPoint(listView, point);
    }
    return IsDesktopHostWindow(clicked);
}

bool IsTaskbarWindow(HWND window) noexcept {
    wchar_t className[64]{};
    if (window == nullptr || GetClassNameW(window, className, 64) <= 0) return false;
    return wcscmp(className, L"Shell_TrayWnd") == 0
        || wcscmp(className, L"Shell_SecondaryTrayWnd") == 0;
}

bool TryClassifyTaskbarAutomationPoint(
    POINT point,
    bool& isInteractive) noexcept {
    isInteractive = true;
    HRESULT initResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool shouldUninitialize = SUCCEEDED(initResult);
    if (FAILED(initResult) && initResult != RPC_E_CHANGED_MODE) return false;

    IUIAutomation* automation = nullptr;
    IUIAutomationElement* element = nullptr;
    BSTR name = nullptr;
    BSTR className = nullptr;
    BSTR automationId = nullptr;
    bool result = false;
    do {
        if (FAILED(CoCreateInstance(
                CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&automation))) || automation == nullptr) break;
        if (FAILED(automation->ElementFromPoint(point, &element)) || element == nullptr) break;

        int controlType = 0;
        if (FAILED(element->get_CurrentControlType(&controlType))) break;
        (void)element->get_CurrentName(&name);
        (void)element->get_CurrentClassName(&className);
        (void)element->get_CurrentAutomationId(&automationId);
        const bool hasMeaningfulIdentity = (name != nullptr && name[0] != L'\0')
            || (className != nullptr && className[0] != L'\0')
            || (automationId != nullptr && automationId[0] != L'\0');
        isInteractive = !IsBlankTaskbarAutomationTarget(
            true, controlType, hasMeaningfulIdentity);
        result = true;
    } while (false);
    if (name != nullptr) SysFreeString(name);
    if (className != nullptr) SysFreeString(className);
    if (automationId != nullptr) SysFreeString(automationId);
    if (element != nullptr) element->Release();
    if (automation != nullptr) automation->Release();
    if (shouldUninitialize) CoUninitialize();
    return result;
}

bool IsTransientShellOrSwitcherWindow(HWND window) noexcept {
    if (window == nullptr) return true;
    wchar_t className[128]{};
    if (GetClassNameW(window, className, 128) <= 0) return true;
    if (wcscmp(className, L"Progman") == 0
        || wcscmp(className, L"WorkerW") == 0
        || wcscmp(className, L"SHELLDLL_DefView") == 0
        || wcscmp(className, L"Shell_TrayWnd") == 0
        || wcscmp(className, L"Shell_SecondaryTrayWnd") == 0
        || wcscmp(className, L"TopLevelWindowForOverflowXamlIsland") == 0
        || wcscmp(className, L"NotifyIconOverflowWindow") == 0
        || wcscmp(className, L"DV2ControlHost") == 0
        || wcscmp(className, L"#32768") == 0
        || wcscmp(className, L"tooltips_class32") == 0
        || wcscmp(className, L"MultitaskingViewFrame") == 0
        || wcscmp(className, L"TaskSwitcherWnd") == 0) {
        return true;
    }
    if (wcscmp(className, L"XamlExplorerHostIslandWindow") == 0) {
        wchar_t title[128]{};
        GetWindowTextW(window, title, 128);
        return wcsstr(title, L"Task Switching") != nullptr;
    }
    return false;
}

bool IsBlankTaskbarPoint(POINT point) noexcept {
    const auto clicked = WindowFromPoint(point);
    const auto taskbar = clicked == nullptr ? nullptr : GetAncestor(clicked, GA_ROOT);
    if (!IsTaskbarWindow(taskbar)) return false;
    POINT clientPoint = point;
    if (!ScreenToClient(taskbar, &clientPoint)) return false;
    auto childWindow = ChildWindowFromPointEx(
        taskbar, clientPoint,
        CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
    if (childWindow == nullptr || childWindow == taskbar) {
        childWindow = RealChildWindowFromPoint(taskbar, clientPoint);
    }
    // Descendant taskbar windows are not automatically controls: Win11 uses
    // ReBar/MSTaskSwWClass and composition bridges for both empty space and
    // task buttons. The UI Automation point probe below is the authoritative
    // distinction; only an actual interactive element is rejected.
    (void)childWindow;

    bool interactive = true;
    if (TryClassifyTaskbarAutomationPoint(point, interactive)) {
        // Classify only the actual pointer location. A nearby task button is
        // unrelated to a genuinely empty taskbar point and must not suppress
        // the empty-area double-click.
        return !interactive;
    }
    // UIA is deliberately fail-closed: treating an unknown taskbar surface as
    // blank would reintroduce the exact false positive this gate prevents.
    return false;
}

} // namespace

bool ShouldCaptureDesktopSessionWindow(HWND window, HMONITOR monitor) noexcept {
    if (window == nullptr || window == GetShellWindow() || !IsWindowVisible(window)
        || IsIconic(window) || IsTransientShellOrSwitcherWindow(window)) return false;
    if (GetWindow(window, GW_OWNER) != nullptr) return false;
    wchar_t className[64]{};
    GetClassNameW(window, className, 64);
    if (wcsncmp(className, L"Desto", 5) == 0
        || wcscmp(className, L"Progman") == 0 || wcscmp(className, L"WorkerW") == 0
        || wcscmp(className, L"Shell_TrayWnd") == 0
        || wcscmp(className, L"Shell_SecondaryTrayWnd") == 0) return false;
    const auto extendedStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((extendedStyle & WS_EX_TOOLWINDOW) != 0
        && (extendedStyle & WS_EX_APPWINDOW) == 0) return false;
    if ((extendedStyle & WS_EX_NOACTIVATE) != 0) return false;
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(
            window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return false;
    return monitor == nullptr
        || MonitorFromWindow(window, MONITOR_DEFAULTTONULL) == monitor;
}

DesktopDoubleClickDetector::DesktopDoubleClickDetector(
    std::uint32_t maximumDelay,
    int maximumWidth,
    int maximumHeight) noexcept
    : maximumDelay_(maximumDelay),
      maximumWidth_(std::max(0, maximumWidth)),
      maximumHeight_(std::max(0, maximumHeight)) {}

bool DesktopDoubleClickDetector::registerClick(
    int screenX,
    int screenY,
    std::uint32_t timestamp,
    bool desktopBackground) noexcept {
    if (!desktopBackground) {
        reset();
        return false;
    }
    const auto matched = hasFirstClick_
        && timestamp - lastTimestamp_ <= maximumDelay_
        && std::abs(static_cast<long long>(screenX) - lastScreenX_) * 2 <= maximumWidth_
        && std::abs(static_cast<long long>(screenY) - lastScreenY_) * 2 <= maximumHeight_;
    if (matched) {
        reset();
        return true;
    }
    hasFirstClick_ = true;
    lastScreenX_ = screenX;
    lastScreenY_ = screenY;
    lastTimestamp_ = timestamp;
    return false;
}

void DesktopDoubleClickDetector::reset() noexcept {
    hasFirstClick_ = false;
}

struct WindowsDesktopTriggerHost::Impl {
    Impl()
        : module(GetModuleHandleW(nullptr)), detector(
            GetDoubleClickTime(),
            GetSystemMetrics(SM_CXDOUBLECLK),
            GetSystemMetrics(SM_CYDOUBLECLK)) {
        if (active != nullptr) {
            throw std::runtime_error("Only one desktop trigger host may be active.");
        }
        WNDCLASSEXW windowClass{
            .cbSize = sizeof(WNDCLASSEXW),
            .lpfnWndProc = &WindowProcedure,
            .hInstance = module,
            .lpszClassName = className,
        };
        if (RegisterClassExW(&windowClass) == 0
            && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            throw std::runtime_error("Unable to register the desktop trigger window.");
        }
        classRegistered = true;
        window = CreateWindowExW(
            WS_EX_TOOLWINDOW, className, L"", WS_POPUP, 0, 0, 0, 0,
            nullptr, nullptr, module, this);
        if (window == nullptr) {
            UnregisterClassW(className, module);
            classRegistered = false;
            throw std::runtime_error("Unable to create the desktop trigger window.");
        }
        active.store(this, std::memory_order_release);
        hookThread = std::thread([this] { runHookThread(); });
        {
            std::unique_lock lock(hookMutex);
            hookReadyCondition.wait(lock, [this] { return hookReady; });
        }
        if (hookError != ERROR_SUCCESS) {
            if (hookThread.joinable()) hookThread.join();
            active.store(nullptr, std::memory_order_release);
            DestroyWindow(window);
            window = nullptr;
            UnregisterClassW(className, module);
            classRegistered = false;
            throw std::runtime_error("Unable to install the desktop mouse hook.");
        }
    }

    ~Impl() {
        if (hookThreadId != 0) PostThreadMessageW(hookThreadId, WM_QUIT, 0, 0);
        if (hookThread.joinable()) hookThread.join();
        auto* expected = this;
        active.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
        if (window != nullptr) DestroyWindow(window);
        if (classRegistered) UnregisterClassW(className, module);
    }

    void runHookThread() noexcept {
        MSG message{};
        PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        hookThreadId = GetCurrentThreadId();
        hook = SetWindowsHookExW(WH_MOUSE_LL, &HookProcedure, module, 0);
        {
            std::lock_guard lock(hookMutex);
            hookError = hook == nullptr ? GetLastError() : ERROR_SUCCESS;
            hookReady = true;
        }
        hookReadyCondition.notify_one();
        if (hook == nullptr) return;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        UnhookWindowsHookEx(hook);
        hook = nullptr;
    }

    static LRESULT CALLBACK WindowProcedure(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept {
        auto* instance = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            instance = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(instance));
        }
        if (message == DesktopDoubleClickMessage && instance != nullptr) {
            try {
                const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                const auto timestamp = static_cast<std::uint32_t>(wParam);
                const auto clicked = WindowFromPoint(point);
                const auto root = clicked == nullptr ? nullptr : GetAncestor(clicked, GA_ROOT);
                const auto desktopCandidate = IsDesktopHostWindow(clicked)
                    || IsDesktopHostWindow(root);
                // Only blank taskbar hits participate in the double-click
                // detector. A task button still has Shell_TrayWnd as its
                // ancestor, but must never poison the blank-area sequence.
                const auto taskbarCandidate = IsTaskbarWindow(root)
                    && IsBlankTaskbarPoint(point);
                if (instance->detector.registerClick(
                        point.x, point.y, timestamp, desktopCandidate)) {
                    if (instance->doubleClick && IsBlankDesktopPoint(point)) {
                        instance->doubleClick();
                    }
                }
                if (instance->taskbarDetector.registerClick(
                        point.x, point.y, timestamp, taskbarCandidate)) {
                    if (instance->taskbarDoubleClick) {
                        instance->taskbarDoubleClick(point.x, point.y);
                    }
                }
            } catch (...) {
            }
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    static LRESULT CALLBACK HookProcedure(
        int code, WPARAM wParam, LPARAM lParam) noexcept {
        auto* instance = active.load(std::memory_order_acquire);
        if (code >= 0 && instance != nullptr && wParam == WM_LBUTTONDOWN) {
            const auto& data = *reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
            try {
                if (instance->window != nullptr) {
                    PostMessageW(instance->window, DesktopDoubleClickMessage, data.time,
                        MAKELPARAM(data.pt.x, data.pt.y));
                }
            } catch (...) {
            }
        }
        return CallNextHookEx(instance == nullptr ? nullptr : instance->hook,
            code, wParam, lParam);
    }

    HHOOK hook = nullptr;
    std::thread hookThread;
    std::mutex hookMutex;
    std::condition_variable hookReadyCondition;
    DWORD hookThreadId = 0;
    DWORD hookError = ERROR_SUCCESS;
    bool hookReady = false;
    HINSTANCE module = nullptr;
    HWND window = nullptr;
    bool classRegistered = false;
    DesktopDoubleClickDetector detector;
    DesktopDoubleClickDetector taskbarDetector{
        GetDoubleClickTime(), GetSystemMetrics(SM_CXDOUBLECLK), GetSystemMetrics(SM_CYDOUBLECLK)};
    Callback doubleClick;
    TaskbarCallback taskbarDoubleClick;
    static inline std::atomic<Impl*> active = nullptr;
    static constexpr const wchar_t* className = L"DestoDesktopTriggerWindow";
};

WindowsDesktopTriggerHost::WindowsDesktopTriggerHost()
    : impl_(std::make_unique<Impl>()) {}

WindowsDesktopTriggerHost::~WindowsDesktopTriggerHost() = default;

void WindowsDesktopTriggerHost::setDoubleClickCallback(Callback callback) {
    impl_->doubleClick = std::move(callback);
}

void WindowsDesktopTriggerHost::setTaskbarDoubleClickCallback(TaskbarCallback callback) {
    impl_->taskbarDoubleClick = std::move(callback);
}

std::uint32_t WindowsDesktopTriggerHost::hookThreadId() const noexcept {
    return impl_->hookThreadId;
}

bool WindowsDesktopTriggerHost::desktopIconsVisible() const noexcept {
    DWORD hidden = 0;
    DWORD size = sizeof(hidden);
    if (RegGetValueW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
            L"HideIcons", RRF_RT_REG_DWORD, nullptr, &hidden, &size) == ERROR_SUCCESS) {
        return hidden == 0;
    }
    const auto listView = FindDesktopListView();
    return listView == nullptr || IsWindowVisible(listView);
}

void WindowsDesktopTriggerHost::setDesktopIconsVisible(bool visible) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
            0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        throw std::runtime_error("Unable to open the desktop icon setting.");
    }
    const DWORD hidden = visible ? 0u : 1u;
    const auto wrote = RegSetValueExW(
        key, L"HideIcons", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&hidden), sizeof(hidden));
    RegCloseKey(key);
    if (wrote != ERROR_SUCCESS) {
        throw std::runtime_error("Unable to save the desktop icon setting.");
    }

    if (const auto listView = FindDesktopListView(); listView != nullptr) {
        SetWindowPos(listView, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER
                | (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        if (visible) {
            RedrawWindow(listView, nullptr, nullptr,
                RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN);
        }
    }
}

void WindowsTaskbarWindowToggle::showDesktop(
    int screenX, int screenY, bool currentDisplayOnly) {
    const auto monitor = currentDisplayOnly
        ? MonitorFromPoint({screenX, screenY}, MONITOR_DEFAULTTONEAREST) : nullptr;
    EnumWindows(+[](HWND window, LPARAM parameter) -> BOOL {
        const auto monitor = reinterpret_cast<HMONITOR>(parameter);
        if (!ShouldCaptureDesktopSessionWindow(window, monitor)) return TRUE;
        ShowWindowAsync(window, SW_MINIMIZE);
        return TRUE;
    }, reinterpret_cast<LPARAM>(monitor));
}

} // namespace desto::platform::windows

