#include "TestSupport.h"
#include "WindowsTextInput.h"

#include <Windows.h>
#include <windowsx.h>

#include <array>
#include <string_view>

using namespace desto::platform::windows;

namespace {

struct Notifications {
    int changes = 0;
    int commits = 0;
    int cancels = 0;
};

LRESULT CALLBACK OwnerProcedure(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* notifications = reinterpret_cast<Notifications*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        notifications = static_cast<Notifications*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(notifications));
    }
    if (message == WM_COMMAND && notifications != nullptr) {
        if (HIWORD(wParam) == EN_CHANGE) ++notifications->changes;
        if (HIWORD(wParam) == WindowsTextInputCommitNotification) {
            ++notifications->commits;
        }
        if (HIWORD(wParam) == WindowsTextInputCancelNotification) {
            ++notifications->cancels;
        }
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void RunTests() {
    WNDCLASSW descriptor{};
    descriptor.lpfnWndProc = &OwnerProcedure;
    descriptor.hInstance = GetModuleHandleW(nullptr);
    descriptor.lpszClassName = L"DestoTextInputTestOwner";
    DESTO_CHECK(RegisterClassW(&descriptor) != 0
        || GetLastError() == ERROR_CLASS_ALREADY_EXISTS);

    Notifications notifications;
    const auto owner = CreateWindowExW(
        0, descriptor.lpszClassName, L"Text input owner",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        40, 40, 420, 180,
        nullptr, nullptr, descriptor.hInstance, &notifications);
    DESTO_CHECK(owner != nullptr);

    WindowsTextInputStyle style;
    style.background = RGB(24, 26, 30);
    style.outline = RGB(70, 74, 82);
    style.focusedOutline = RGB(51, 136, 255);
    style.text = RGB(240, 242, 246);
    style.placeholder = RGB(140, 145, 154);
    style.cornerRadius = 9.0F;
    style.leadingGlyph = L"\uE721";

    const auto input = CreateWindowsTextInput({
        .notificationWindow = owner,
        .controlId = 77,
        .bounds = RECT{20, 20, 380, 64},
        .maximumLength = 16,
        .placeholder = L"搜索",
        .style = style,
    });
    DESTO_CHECK(input != nullptr);
    DESTO_CHECK(IsWindowsTextInput(input));
    DESTO_CHECK(FindWindowExW(input, nullptr, L"EDIT", nullptr) == nullptr);
    DESTO_CHECK(GetParent(input) == owner);

    SetWindowTextW(input, L"");
    SendMessageW(input, WM_CHAR, L'你', 0);
    SendMessageW(input, WM_CHAR, L'好', 0);
    DESTO_CHECK(WindowsTextInputText(input) == L"你好");
    DWORD start = 0;
    DWORD end = 0;
    SendMessageW(input, EM_GETSEL,
        reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
    DESTO_CHECK(start == 2 && end == 2);

    SendMessageW(input, WM_UNDO, 0, 0);
    DESTO_CHECK(WindowsTextInputText(input) == L"你");
    SendMessageW(input, WM_CHAR, L'好', 0);
    DESTO_CHECK(WindowsTextInputText(input) == L"你好");

    SetWindowsTextInputText(input, L"Plan \U0001F680");
    DESTO_CHECK(GetWindowTextLengthW(input) == 7);
    std::array<wchar_t, 32> text{};
    GetWindowTextW(input, text.data(), static_cast<int>(text.size()));
    DESTO_CHECK(std::wstring_view(text.data()) == L"Plan \U0001F680");
    SetWindowsTextInputSelection(input, 0, 4);
    const auto selection = WindowsTextInputSelection(input);
    DESTO_CHECK(selection.first == 0 && selection.second == 4);

    SetWindowsTextInputText(input, L"中文 \U0001F680 abc");
    for (const auto position : {0, 1, 2, 3, 5, 6, 9}) {
        const auto packed = SendMessageW(input, EM_POSFROMCHAR, position, 0);
        const POINT point{GET_X_LPARAM(packed), GET_Y_LPARAM(packed)};
        const auto resolved = static_cast<std::size_t>(SendMessageW(
            input, EM_CHARFROMPOS, 0, MAKELPARAM(point.x, point.y)));
        DESTO_CHECK(resolved == static_cast<std::size_t>(position));
    }
    const auto emojiStart = SendMessageW(input, EM_POSFROMCHAR, 3, 0);
    const auto emojiEnd = SendMessageW(input, EM_POSFROMCHAR, 5, 0);
    const auto emojiX = GET_X_LPARAM(emojiStart)
        + (GET_X_LPARAM(emojiEnd) - GET_X_LPARAM(emojiStart)) * 3 / 4;
    DESTO_CHECK(SendMessageW(input, EM_CHARFROMPOS, 0, MAKELPARAM(emojiX, 4)) == 5);

    SetWindowsTextInputText(input, L"مرحبا");
    const auto rtlStart = SendMessageW(input, EM_POSFROMCHAR, 0, 0);
    const auto rtlEnd = SendMessageW(input, EM_POSFROMCHAR, 5, 0);
    DESTO_CHECK(GET_X_LPARAM(rtlStart) > GET_X_LPARAM(rtlEnd));
    for (const auto position : {0, 1, 3, 5}) {
        const auto packed = SendMessageW(input, EM_POSFROMCHAR, position, 0);
        const POINT point{GET_X_LPARAM(packed), GET_Y_LPARAM(packed)};
        const auto resolved = static_cast<std::size_t>(SendMessageW(
            input, EM_CHARFROMPOS, 0, MAKELPARAM(point.x, point.y)));
        DESTO_CHECK(resolved == static_cast<std::size_t>(position));
    }

    SetWindowsTextInputText(input, L"Plan \U0001F680");
    SetWindowsTextInputSelection(input, 7, 7);
    SendMessageW(input, WM_KEYDOWN, VK_BACK, 0);
    DESTO_CHECK(WindowsTextInputText(input) == L"Plan ");
    SendMessageW(input, WM_KEYDOWN, VK_RETURN, 0);
    DESTO_CHECK(notifications.commits == 1);
    SendMessageW(input, WM_KEYDOWN, VK_ESCAPE, 0);
    DESTO_CHECK(notifications.cancels == 1);
    DESTO_CHECK(notifications.changes >= 5);

    style.backgroundAlpha = 80;
    style.outlineAlpha = 180;
    style.background = RGB(20, 20, 20);
    style.outline = RGB(180, 180, 180);
    style.focusedOutline = RGB(180, 180, 180);
    style.text = RGB(245, 245, 245);
    style.leadingGlyph.clear();
    const auto popup = CreateWindowsTextInput({
        .notificationWindow = owner,
        .controlId = 78,
        .bounds = RECT{80, 80, 360, 124},
        .popup = true,
        .maximumLength = 32,
        .text = L"Plan \U0001F680",
        .style = style,
    });
    DESTO_CHECK(popup != nullptr);
    DESTO_CHECK(IsWindowsTextInput(popup));
    DESTO_CHECK(GetParent(popup) == nullptr);
    DESTO_CHECK((GetWindowLongPtrW(popup, GWL_EXSTYLE) & WS_EX_LAYERED) != 0);
    DESTO_CHECK((GetWindowLongPtrW(popup, GWL_EXSTYLE) & WS_EX_TRANSPARENT) == 0);
    DESTO_CHECK(FindWindowExW(popup, nullptr, L"EDIT", nullptr) == nullptr);
    const auto renderStats = WindowsTextInputRenderStats(popup);
    DESTO_CHECK(renderStats.nonTransparentPixels > 0);
    DESTO_CHECK(renderStats.coloredPixels == 0);
    DESTO_CHECK(renderStats.backingStoreCreations == 1);
    DESTO_CHECK(renderStats.renderTargetCreations == 0);
    for (int index = 0; index < 20; ++index) {
        SendMessageW(popup, WM_TIMER, 1, 0);
    }
    SendMessageW(popup, WM_CHAR, L'!', 0);
    const auto pendingStats = WindowsTextInputRenderStats(popup);
    DESTO_CHECK(pendingStats.paints == renderStats.paints);
    MSG redrawMessage{};
    while (PeekMessageW(&redrawMessage, popup, WM_APP + 0x31, WM_APP + 0x31, PM_REMOVE)) {
        DispatchMessageW(&redrawMessage);
    }
    const auto reusedStats = WindowsTextInputRenderStats(popup);
    DESTO_CHECK(reusedStats.paints == renderStats.paints + 1);
    DESTO_CHECK(reusedStats.backingStoreCreations == 1);
    DESTO_CHECK(reusedStats.renderTargetCreations == 0);

    DestroyWindow(popup);
    DestroyWindow(input);
    DestroyWindow(owner);
    UnregisterClassW(descriptor.lpszClassName, descriptor.hInstance);
}

} // namespace

int main() {
    return desto::test::Run(RunTests);
}
