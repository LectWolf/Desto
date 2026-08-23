#include "TestSupport.h"
#include "WindowsDesktopTriggerHost.h"

#include <Windows.h>
#include <Oleacc.h>
#include <UIAutomation.h>

using namespace desto::platform::windows;

namespace {

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
    DESTO_CHECK(!ShouldCaptureDesktopSessionWindow(destoWindow));
    WindowsTaskbarWindowToggle toggle;
    (void)toggle;
    DestroyWindow(destoWindow);
    UnregisterClassW(destoClass.lpszClassName, module);
    DestroyWindow(ownedDialog);
    DestroyWindow(ownedAppWindow);
    DestroyWindow(owner);

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
