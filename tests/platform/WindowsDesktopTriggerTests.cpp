#include "TestSupport.h"
#include "WindowsDesktopTriggerHost.h"

#include <Windows.h>
#include <Oleacc.h>
#include <UIAutomation.h>

#include <algorithm>
#include <array>
#include <vector>

using namespace desto::platform::windows;

namespace {

bool IsWindowAbove(HWND upper, HWND lower) {
    for (auto window = upper; window != nullptr; window = GetWindow(window, GW_HWNDNEXT)) {
        if (window == lower) return true;
    }
    return false;
}

std::vector<HWND> CaptureOrder(const std::vector<HWND>& targets) {
    struct Context {
        const std::vector<HWND>& targets;
        std::vector<HWND> result;
    } context{targets};
    EnumWindows(+[](HWND window, LPARAM parameter) -> BOOL {
        auto& value = *reinterpret_cast<Context*>(parameter);
        if (std::ranges::find(value.targets, window) != value.targets.end()) {
            value.result.push_back(window);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
    return context.result;
}

void RunTests() {
    {
        WindowsDesktopTriggerHost triggerHost;
        DESTO_CHECK(triggerHost.hookThreadId() != 0);
        DESTO_CHECK(triggerHost.hookThreadId() != GetCurrentThreadId());
    }
    const auto module = GetModuleHandleW(nullptr);
    const auto owner = CreateWindowExW(
        0, L"STATIC", L"Desto session owner", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        0, 0, 240, 180, nullptr, nullptr, module, nullptr);
    const auto ownedAppWindow = CreateWindowExW(
        WS_EX_APPWINDOW, L"STATIC", L"Desto independent owned window",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        20, 20, 240, 180, owner, nullptr, module, nullptr);
    const auto ownedDialog = CreateWindowExW(
        0, L"STATIC", L"Desto owned dialog", WS_POPUP | WS_VISIBLE,
        40, 40, 180, 120, owner, nullptr, module, nullptr);
    DESTO_CHECK(owner != nullptr && ownedAppWindow != nullptr && ownedDialog != nullptr);
    DESTO_CHECK(ShouldCaptureDesktopSessionWindow(owner));
    const auto ownerMonitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
    DESTO_CHECK(ShouldCaptureDesktopSessionWindow(owner, ownerMonitor));
    DESTO_CHECK(!ShouldCaptureDesktopSessionWindow(
        owner, reinterpret_cast<HMONITOR>(static_cast<std::uintptr_t>(1))));
    DESTO_CHECK(!ShouldCaptureDesktopSessionWindow(ownedAppWindow));
    DESTO_CHECK(!ShouldCaptureDesktopSessionWindow(ownedDialog));

    WNDCLASSW destoClass{
        .lpfnWndProc = &DefWindowProcW,
        .hInstance = module,
        .lpszClassName = L"DestoSettingsWindow",
    };
    DESTO_CHECK(RegisterClassW(&destoClass) != 0
        || GetLastError() == ERROR_CLASS_ALREADY_EXISTS);
    const auto destoWindow = CreateWindowExW(
        0, destoClass.lpszClassName, L"Desto settings", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        60, 60, 240, 180, nullptr, nullptr, module, nullptr);
    DESTO_CHECK(destoWindow != nullptr);
    DESTO_CHECK(ShouldCaptureDesktopSessionWindow(destoWindow));
    DestroyWindow(destoWindow);
    UnregisterClassW(destoClass.lpszClassName, module);

    DESTO_CHECK(ShouldRestoreDesktopSessionOnForeground(
        true, true, false, true, false, false));
    DESTO_CHECK(!ShouldRestoreDesktopSessionOnForeground(
        true, true, false, true, true, true));
    DESTO_CHECK(ShouldRestoreDesktopSessionOnForeground(
        true, true, false, true, true, false));
    DESTO_CHECK(!ShouldRestoreDesktopSessionOnForeground(
        true, true, false, false, false, false));
    DESTO_CHECK(!ShouldRestoreDesktopSessionOnForeground(
        true, true, true, true, false, false));
    DESTO_CHECK(ShouldRestoreDesktopSessionOnForeground(
        false, true, false, true, false, false));
    DESTO_CHECK(ResolveDesktopSessionToggleAction(false, false)
        == DesktopSessionToggleAction::BeginSession);
    DESTO_CHECK(ResolveDesktopSessionToggleAction(true, true)
        == DesktopSessionToggleAction::RestoreSession);
    DESTO_CHECK(ResolveDesktopSessionToggleAction(true, false)
        == DesktopSessionToggleAction::HideExposedWindows);
    DestroyWindow(ownedDialog);
    DestroyWindow(ownedAppWindow);
    DestroyWindow(owner);

    const auto first = CreateWindowExW(
        0, L"STATIC", L"Desto Z first", WS_POPUP | WS_VISIBLE,
        0, 0, 40, 40, nullptr, nullptr, module, nullptr);
    const auto second = CreateWindowExW(
        0, L"STATIC", L"Desto Z second", WS_POPUP | WS_VISIBLE,
        0, 0, 40, 40, nullptr, nullptr, module, nullptr);
    const auto third = CreateWindowExW(
        0, L"STATIC", L"Desto Z third", WS_POPUP | WS_VISIBLE,
        0, 0, 40, 40, nullptr, nullptr, module, nullptr);
    DESTO_CHECK(first != nullptr && second != nullptr && third != nullptr);
    const auto order = CaptureOrder({first, second, third});
    DESTO_CHECK(order.size() == 3);
    std::vector<std::pair<HWND, WINDOWPLACEMENT>> placements;
    for (const auto window : order) {
        WINDOWPLACEMENT placement{.length = sizeof(WINDOWPLACEMENT)};
        DESTO_CHECK(GetWindowPlacement(window, &placement));
        placements.emplace_back(window, placement);
        ShowWindow(window, SW_MINIMIZE);
    }
    for (auto iterator = placements.rbegin(); iterator != placements.rend(); ++iterator) {
        DESTO_CHECK(RestoreCapturedWindowPlacement(iterator->first, iterator->second));
    }
    for (std::size_t index = 1; index < order.size(); ++index) {
        DESTO_CHECK(IsWindowAbove(order[index - 1], order[index]));
    }
    DestroyWindow(third);
    DestroyWindow(second);
    DestroyWindow(first);

    const auto maximized = CreateWindowExW(
        0, L"STATIC", L"Desto maximized restore", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        80, 80, 640, 480, nullptr, nullptr, module, nullptr);
    DESTO_CHECK(maximized != nullptr);
    ShowWindow(maximized, SW_SHOWMAXIMIZED);
    WINDOWPLACEMENT maximizedPlacement{.length = sizeof(WINDOWPLACEMENT)};
    DESTO_CHECK(GetWindowPlacement(maximized, &maximizedPlacement));
    DESTO_CHECK(maximizedPlacement.showCmd == SW_SHOWMAXIMIZED);
    const auto overlay = CreateWindowExW(
        0, L"STATIC", L"Desto restore overlay", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        160, 160, 360, 240, nullptr, nullptr, module, nullptr);
    DESTO_CHECK(overlay != nullptr);
    WINDOWPLACEMENT overlayPlacement{.length = sizeof(WINDOWPLACEMENT)};
    DESTO_CHECK(GetWindowPlacement(overlay, &overlayPlacement));
    DESTO_CHECK(IsWindowAbove(overlay, maximized));
    ShowWindow(overlay, SW_MINIMIZE);
    ShowWindow(maximized, SW_MINIMIZE);
    DESTO_CHECK(RestoreCapturedWindowPlacement(maximized, maximizedPlacement));
    DESTO_CHECK(RestoreCapturedWindowPlacement(overlay, overlayPlacement));
    DESTO_CHECK(IsZoomed(maximized));
    DESTO_CHECK(IsWindowAbove(overlay, maximized));
    DestroyWindow(overlay);
    DestroyWindow(maximized);

    DesktopDoubleClickDetector detector(500, 8, 8);
    DESTO_CHECK(!detector.registerClick(100, 100, 1000, true));
    DESTO_CHECK(detector.registerClick(103, 103, 1300, true));

    DESTO_CHECK(!detector.registerClick(100, 100, 2000, true));
    DESTO_CHECK(!detector.registerClick(106, 100, 2200, true));
    DESTO_CHECK(!detector.registerClick(106, 100, 2250, false));
    DESTO_CHECK(!detector.registerClick(106, 100, 2300, true));
    DESTO_CHECK(detector.registerClick(106, 100, 2400, true));

    DESTO_CHECK(IsBlankTaskbarAccessibilityTarget(
        true, ROLE_SYSTEM_CLIENT, true));
    DESTO_CHECK(IsBlankTaskbarAccessibilityTarget(
        true, ROLE_SYSTEM_PANE, true));
    DESTO_CHECK(!IsBlankTaskbarAccessibilityTarget(
        true, ROLE_SYSTEM_PUSHBUTTON, true));
    DESTO_CHECK(!IsBlankTaskbarAccessibilityTarget(
        true, ROLE_SYSTEM_TOOLBAR, true));
    DESTO_CHECK(!IsBlankTaskbarAccessibilityTarget(
        false, 0, true));

    DESTO_CHECK(!IsBlankTaskbarAutomationTarget(
        true, UIA_ButtonControlTypeId, true));
    DESTO_CHECK(!IsBlankTaskbarAutomationTarget(
        true, UIA_ListItemControlTypeId, true));
    DESTO_CHECK(IsBlankTaskbarAutomationTarget(
        true, UIA_PaneControlTypeId, true));
    DESTO_CHECK(IsBlankTaskbarAutomationTarget(
        true, UIA_CustomControlTypeId, false));
    DESTO_CHECK(!IsBlankTaskbarAutomationTarget(
        false, UIA_PaneControlTypeId, false));
}

} // namespace

int main() {
    return desto::test::Run(RunTests);
}
