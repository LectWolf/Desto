#include "WindowsDesktopTriggerHost.h"

#include <Windows.h>
#include <CommCtrl.h>
#include <dwmapi.h>
#include <oleacc.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

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

    IAccessible* accessible = nullptr;
    VARIANT child{};
    VariantInit(&child);
    if (AccessibleObjectFromPoint(point, &accessible, &child) != S_OK
        || accessible == nullptr) {
        return IsBlankTaskbarAccessibilityTarget(false, 0, clicked == taskbar);
    }
    VARIANT role{};
    VariantInit(&role);
    const auto roleResult = accessible->get_accRole(child, &role);
    accessible->Release();
    if (roleResult != S_OK || role.vt != VT_I4) {
        VariantClear(&role);
        VariantClear(&child);
        return IsBlankTaskbarAccessibilityTarget(false, 0, clicked == taskbar);
    }
    const auto value = role.lVal;
    VariantClear(&role);
    VariantClear(&child);
    return IsBlankTaskbarAccessibilityTarget(true, value, clicked == taskbar);
}

bool TryToggleDesktopWithExplorer() noexcept {
    constexpr UINT ButtonClickMessage = 0x00F5;
    const auto taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    const auto showDesktopButton = taskbar == nullptr ? nullptr : FindWindowExW(
        taskbar, nullptr, L"TrayShowDesktopButtonWClass", nullptr);
    if (showDesktopButton != nullptr) {
        DWORD_PTR ignored = 0;
        if (SendMessageTimeoutW(
                showDesktopButton, ButtonClickMessage, 0, 0,
                SMTO_ABORTIFHUNG, 250, &ignored) != 0) {
            return true;
        }
    }

    std::array<INPUT, 4> input{};
    input[0].type = INPUT_KEYBOARD;
    input[0].ki.wVk = VK_LWIN;
    input[0].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
    input[1].type = INPUT_KEYBOARD;
    input[1].ki.wVk = 'D';
    input[2] = input[1];
    input[2].ki.dwFlags = KEYEVENTF_KEYUP;
    input[3] = input[0];
    input[3].ki.dwFlags = KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
    return SendInput(static_cast<UINT>(input.size()), input.data(), sizeof(INPUT))
        == input.size();
}

} // namespace

bool ShouldCaptureDesktopSessionWindow(HWND window, HMONITOR monitor) noexcept {
    if (window == nullptr || window == GetShellWindow() || !IsWindowVisible(window)
        || IsIconic(window) || IsTransientShellOrSwitcherWindow(window)) return false;
    if (GetWindow(window, GW_OWNER) != nullptr) return false;
    wchar_t className[64]{};
    GetClassNameW(window, className, 64);
    const auto isDestoSurface = wcscmp(className, L"DestoDesktopHostSurface") == 0
        || wcscmp(className, L"DestoDesktopTriggerWindow") == 0
        || wcscmp(className, L"DestoTrayWindow") == 0
        || wcscmp(className, L"DestoItemTooltip") == 0;
    if (isDestoSurface
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

bool ShouldRestoreDesktopSessionOnForeground(
    bool allDisplaysSession,
    bool sessionActive,
    bool transitioning,
    bool restoreOnForeground,
    bool candidateWasCaptured,
    bool candidateIsIconic) noexcept {
    (void)allDisplaysSession;
    if (!sessionActive || transitioning || !restoreOnForeground) return false;
    // Explorer emits foreground events while Show Desktop is still minimizing
    // the captured stack.  Those events describe the transition itself, not a
    // user opening or selecting a window.  Ignore only that observable state;
    // a captured window restored from the taskbar is no longer iconic and must
    // restore the rest of the session immediately.
    return !candidateWasCaptured || !candidateIsIconic;
}

DesktopSessionToggleAction ResolveDesktopSessionToggleAction(
    bool sessionActive,
    bool desktopVisible) noexcept {
    if (!sessionActive) return DesktopSessionToggleAction::BeginSession;
    return desktopVisible
        ? DesktopSessionToggleAction::RestoreSession
        : DesktopSessionToggleAction::HideExposedWindows;
}

bool RestoreCapturedWindowPlacement(
    HWND window, const WINDOWPLACEMENT& captured) noexcept {
    if (window == nullptr || !IsWindow(window)) return false;
    auto placement = captured;
    placement.length = sizeof(WINDOWPLACEMENT);
    // Preserve the original placement exactly. An asynchronous placement can
    // finish after the Z-order pass and promote a maximized window too late.
    const auto placed = SetWindowPlacement(window, &placement) != FALSE;
    return placed || ShowWindowAsync(window, captured.showCmd) != FALSE;
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

struct WindowsTaskbarWindowToggle::Impl {
    struct WindowState {
        HWND window = nullptr;
        WINDOWPLACEMENT placement{.length = sizeof(WINDOWPLACEMENT)};
    };

    struct Session {
        HMONITOR monitor = nullptr;
        bool allDisplays = false;
        bool nativeShell = false;
        bool desktopVisible = true;
        std::vector<WindowState> windows;
    };

    static bool isDestoWindow(HWND window) noexcept {
        wchar_t className[128]{};
        if (window == nullptr || GetClassNameW(window, className, 128) <= 0) return false;
        return wcsncmp(className, L"Desto", 5) == 0;
    }

    static bool eligibleRestoreTrigger(HWND window) noexcept {
        if (window == nullptr || !IsWindow(window) || !IsWindowVisible(window)
            || IsTransientShellOrSwitcherWindow(window)
            || isDestoWindow(window)) {
            return false;
        }
        BOOL cloaked = FALSE;
        return FAILED(DwmGetWindowAttribute(
                   window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))
            || !cloaked;
    }

    Impl() {
        if (foregroundObserver != nullptr) {
            throw std::runtime_error("Only one taskbar window toggle may be active.");
        }
        foregroundObserver = this;
        foregroundHook = SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND,
            EVENT_SYSTEM_FOREGROUND,
            nullptr,
            &ForegroundEventProcedure,
            0,
            0,
            WINEVENT_OUTOFCONTEXT);
        if (foregroundHook == nullptr) foregroundObserver = nullptr;
    }

    ~Impl() {
        if (foregroundHook != nullptr) UnhookWinEvent(foregroundHook);
        if (foregroundObserver == this) foregroundObserver = nullptr;
    }

    static void CALLBACK ForegroundEventProcedure(
        HWINEVENTHOOK, DWORD event, HWND window, LONG, LONG, DWORD, DWORD) noexcept {
        if (foregroundObserver == nullptr || window == nullptr) return;
        if (event == EVENT_SYSTEM_FOREGROUND) foregroundObserver->onForegroundChanged(window);
    }

    static void captureWindows(Session& session) noexcept {
        struct Enumeration {
            HMONITOR monitor = nullptr;
            std::vector<WindowState>* windows = nullptr;
        } enumeration{session.monitor, &session.windows};
        EnumWindows(+[](HWND window, LPARAM parameter) -> BOOL {
            auto& value = *reinterpret_cast<Enumeration*>(parameter);
            if (!ShouldCaptureDesktopSessionWindow(window, value.monitor)) return TRUE;
            WindowState state{.window = window};
            if (GetWindowPlacement(window, &state.placement)) {
                value.windows->push_back(state);
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&enumeration));
    }

    static void reassertForeground(HWND candidate) noexcept {
        if (!eligibleRestoreTrigger(candidate)) return;
        if (IsIconic(candidate) || !IsWindowVisible(candidate)) {
            ShowWindowAsync(candidate, SW_RESTORE);
        }
        SetForegroundWindow(candidate);
    }

    static void restoreCapturedWindows(Session& session) noexcept {
        for (auto iterator = session.windows.rbegin();
             iterator != session.windows.rend(); ++iterator) {
            if (IsWindow(iterator->window)) {
                (void)RestoreCapturedWindowPlacement(iterator->window, iterator->placement);
            }
        }
        HWND insertAfter = HWND_TOP;
        for (const auto& state : session.windows) {
            if (!IsWindow(state.window)) continue;
            SetWindowPos(state.window, insertAfter, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER
                    | SWP_NOSENDCHANGING | SWP_SHOWWINDOW);
            insertAfter = state.window;
        }
        session.windows.clear();
    }

    static void hideExposedWindows(Session& session) noexcept {
        std::vector<WindowState> exposed;
        struct Enumeration {
            HMONITOR monitor = nullptr;
            std::vector<WindowState>* exposed = nullptr;
            std::vector<WindowState>* captured = nullptr;
        } enumeration{session.monitor, &exposed, &session.windows};
        EnumWindows(+[](HWND window, LPARAM parameter) -> BOOL {
            auto& value = *reinterpret_cast<Enumeration*>(parameter);
            if (!ShouldCaptureDesktopSessionWindow(window, value.monitor)) return TRUE;
            const auto existing = std::ranges::find(*value.captured, window, &WindowState::window);
            if (existing != value.captured->end()) {
                value.exposed->push_back(*existing);
                value.captured->erase(existing);
                return TRUE;
            }
            WindowState state{.window = window};
            if (GetWindowPlacement(window, &state.placement)) {
                value.exposed->push_back(state);
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&enumeration));

        for (auto iterator = exposed.rbegin(); iterator != exposed.rend(); ++iterator) {
            session.windows.insert(session.windows.begin(), *iterator);
        }
        for (const auto& state : exposed) {
            if (IsWindow(state.window)) ShowWindowAsync(state.window, SW_MINIMIZE);
        }
        session.desktopVisible = true;
    }

    static bool containsWindow(const Session& session, HWND window) noexcept {
        return std::ranges::any_of(session.windows, [&](const auto& state) {
            return state.window == window;
        });
    }

    auto findSession(HMONITOR monitor) noexcept {
        return std::ranges::find(sessions, monitor, &Session::monitor);
    }

    void restoreSession(std::size_t index, HWND foregroundAfter) noexcept {
        if (index >= sessions.size() || transitioning) return;
        transitioning = true;
        auto session = std::move(sessions[index]);
        sessions.erase(sessions.begin() + static_cast<std::ptrdiff_t>(index));

        if (session.nativeShell) {
            // Consume Explorer's own Show Desktop state before restoring the
            // captured windows. Otherwise the next taskbar gesture can be
            // interpreted as "restore windows" instead of "show desktop".
            (void)TryToggleDesktopWithExplorer();
            if (!session.windows.empty()) restoreCapturedWindows(session);
        } else {
            restoreCapturedWindows(session);
        }

        transitioning = false;
        reassertForeground(foregroundAfter);
    }

    void onForegroundChanged(HWND candidate) noexcept {
        if (sessions.empty() || transitioning || !eligibleRestoreTrigger(candidate)) return;
        auto session = std::ranges::find_if(sessions, [](const auto& value) {
            return value.allDisplays;
        });
        if (session == sessions.end()) {
            const auto monitor = MonitorFromWindow(candidate, MONITOR_DEFAULTTONULL);
            session = findSession(monitor);
        }
        if (session == sessions.end()) return;

        // Foreground restoration was intentionally removed. A taskbar
        // double-click only toggles the selected desktop session.
        session->desktopVisible = false;
        return;
        const auto captured = containsWindow(*session, candidate);
        if (!ShouldRestoreDesktopSessionOnForeground(
                session->allDisplays, true, transitioning, restoreOnNewWindow,
                captured, IsIconic(candidate))) {
            return;
        }
        restoreSession(
            static_cast<std::size_t>(std::distance(sessions.begin(), session)), candidate);
    }

    void toggle(POINT point, bool currentDisplayOnly) noexcept {
        if (transitioning) return;
        const auto monitor = currentDisplayOnly
            ? MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST) : nullptr;

        if (currentDisplayOnly) {
            const auto native = std::ranges::find_if(sessions, [](const auto& session) {
                return session.allDisplays;
            });
            if (native != sessions.end()) {
                restoreSession(
                    static_cast<std::size_t>(std::distance(sessions.begin(), native)), nullptr);
                return;
            }
            const auto existing = findSession(monitor);
            if (existing != sessions.end()) {
                if (ResolveDesktopSessionToggleAction(true, existing->desktopVisible)
                    == DesktopSessionToggleAction::HideExposedWindows) {
                    transitioning = true;
                    hideExposedWindows(*existing);
                    transitioning = false;
                    return;
                }
                restoreSession(
                    static_cast<std::size_t>(std::distance(sessions.begin(), existing)), nullptr);
                return;
            }
        } else {
            const auto existing = findSession(nullptr);
            if (existing != sessions.end()) {
                if (ResolveDesktopSessionToggleAction(true, existing->desktopVisible)
                    == DesktopSessionToggleAction::HideExposedWindows) {
                    transitioning = true;
                    hideExposedWindows(*existing);
                    transitioning = false;
                    return;
                }
                restoreSession(
                    static_cast<std::size_t>(std::distance(sessions.begin(), existing)), nullptr);
                return;
            }
            if (!sessions.empty()) {
                while (!sessions.empty()) restoreSession(sessions.size() - 1, nullptr);
                return;
            }
        }

        Session session{.monitor = monitor, .allDisplays = !currentDisplayOnly};
        captureWindows(session);
        if (currentDisplayOnly && session.windows.empty()) return;

        transitioning = true;
        if (!currentDisplayOnly) {
            const auto foregroundBefore = GetForegroundWindow();
            const auto desktopWasForeground = foregroundBefore != nullptr
                && IsDesktopHostWindow(GetAncestor(foregroundBefore, GA_ROOT));
            session.nativeShell = TryToggleDesktopWithExplorer();
            if (session.windows.empty() && desktopWasForeground) {
                transitioning = false;
                return;
            }
        }
        if (!session.nativeShell) {
            for (const auto& state : session.windows) {
                if (IsWindow(state.window)) ShowWindowAsync(state.window, SW_MINIMIZE);
            }
        }
        if (!session.nativeShell && session.windows.empty()) {
            transitioning = false;
            return;
        }
        sessions.push_back(std::move(session));
        transitioning = false;
    }

    std::vector<Session> sessions;
    bool restoreOnNewWindow = true;
    bool transitioning = false;
    HWINEVENTHOOK foregroundHook = nullptr;
    static inline Impl* foregroundObserver = nullptr;
};

WindowsTaskbarWindowToggle::WindowsTaskbarWindowToggle()
    : impl_(std::make_unique<Impl>()) {}

WindowsTaskbarWindowToggle::~WindowsTaskbarWindowToggle() = default;

void WindowsTaskbarWindowToggle::setRestoreOnNewWindow(bool enabled) noexcept {
    impl_->restoreOnNewWindow = enabled;
}

void WindowsTaskbarWindowToggle::toggle(
    int screenX, int screenY, bool currentDisplayOnly) {
    impl_->toggle({screenX, screenY}, currentDisplayOnly);
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
