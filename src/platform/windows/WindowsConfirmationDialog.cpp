#include "WindowsConfirmationDialog.h"

#include <windowsx.h>

#include <memory>
#include <string>
#include <utility>

namespace desto::platform::windows {
namespace {

constexpr wchar_t kClassName[] = L"Desto.CustomConfirmation";
constexpr int kWidth = 470;
constexpr int kHeight = 250;

struct DialogState {
    HWND owner = nullptr;
    HWND window = nullptr;
    desto::ui::Dialog dialog;
    bool result = false;
    bool done = false;
};

RECT NativeRect(const desto::ui::Rect& rect) noexcept {
    return {rect.x, rect.y, rect.x + rect.width, rect.y + rect.height};
}

void DrawTextBlock(HDC dc, const std::wstring& text, RECT rect, COLORREF color,
    int size, UINT format, int weight = FW_NORMAL) noexcept {
    const auto font = CreateFontW(
        -size, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    const auto previous = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, format);
    SelectObject(dc, previous);
    DeleteObject(font);
}

void FillRoundRect(HDC dc, RECT rect, COLORREF fill, COLORREF outline) noexcept {
    const auto brush = CreateSolidBrush(fill);
    const auto pen = CreatePen(PS_SOLID, 1, outline);
    const auto oldBrush = SelectObject(dc, brush);
    const auto oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, 14, 14);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void CloseDialog(DialogState& state, bool result) noexcept {
    state.result = result;
    state.done = true;
    if (state.window != nullptr) {
        DestroyWindow(state.window);
        state.window = nullptr;
    }
}

void ApplyAction(DialogState& state, desto::ui::DialogAction action) noexcept {
    CloseDialog(state, action == desto::ui::DialogAction::Confirm);
}

LRESULT CALLBACK DialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept {
    auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(
        window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        state = static_cast<DialogState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        state->window = window;
    }
    if (state == nullptr) return DefWindowProcW(window, message, wParam, lParam);

    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        state->dialog.layout({client.right, client.bottom});
        const auto view = state->dialog.snapshot();
        const auto& spec = state->dialog.spec();
        const auto background = CreateSolidBrush(RGB(21, 23, 28));
        FillRect(dc, &client, background);
        DeleteObject(background);
        FillRoundRect(dc, {1, 1, client.right - 1, client.bottom - 1},
            RGB(38, 41, 48), RGB(82, 86, 98));
        auto titleRect = NativeRect(view.titleBounds);
        DrawTextBlock(dc, spec.title,
            titleRect, RGB(245, 247, 250), 18,
            DT_SINGLELINE | DT_VCENTER, FW_SEMIBOLD);
        auto messageRect = NativeRect(view.messageBounds);
        DrawTextBlock(dc, spec.message, messageRect,
            RGB(190, 194, 203), 13, DT_WORDBREAK | DT_TOP);
        auto confirmRect = NativeRect(view.confirmButton.bounds);
        if (view.cancelButton.has_value()) {
            auto cancelRect = NativeRect(view.cancelButton->bounds);
            FillRoundRect(dc, cancelRect, RGB(49, 52, 60),
                view.cancelButton->focused ? RGB(164, 175, 196) : RGB(86, 90, 101));
            DrawTextBlock(dc, view.cancelButton->label, cancelRect,
                RGB(232, 235, 240), 12, DT_SINGLELINE | DT_CENTER | DT_VCENTER, FW_SEMIBOLD);
        }
        FillRoundRect(dc, confirmRect, RGB(56, 126, 238),
            view.confirmButton.focused ? RGB(176, 207, 255) : RGB(95, 157, 255));
        DrawTextBlock(dc, view.confirmButton.label, confirmRect,
            RGB(255, 255, 255), 12, DT_SINGLELINE | DT_CENTER | DT_VCENTER, FW_SEMIBOLD);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_LBUTTONUP: {
        if (const auto action = state->dialog.pointerReleased(
                {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)})) {
            ApplyAction(*state, *action);
        }
        return 0;
    }
    case WM_KEYDOWN: {
        std::optional<desto::ui::DialogKey> key;
        if (wParam == VK_ESCAPE) key = desto::ui::DialogKey::Escape;
        if (wParam == VK_RETURN) key = desto::ui::DialogKey::Enter;
        if (wParam == VK_TAB) key = desto::ui::DialogKey::Tab;
        if (key.has_value()) {
            const auto action = state->dialog.keyPressed(
                *key, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            if (action.has_value()) ApplyAction(*state, *action);
            else InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        CloseDialog(*state, false);
        return 0;
    case WM_NCDESTROY:
        state->window = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool EnsureDialogClass() noexcept {
    static ATOM atom = [] {
        WNDCLASSW klass{};
        klass.lpfnWndProc = DialogProc;
        klass.hInstance = GetModuleHandleW(nullptr);
        klass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(IDC_ARROW));
        klass.hbrBackground = nullptr;
        klass.lpszClassName = kClassName;
        return RegisterClassW(&klass);
    }();
    return atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

} // namespace

bool ShowWindowsConfirmation(
    HWND owner,
    std::wstring_view title,
    std::wstring_view message,
    std::wstring_view confirmLabel,
    std::wstring_view cancelLabel) noexcept {
    return ShowWindowsDialog(owner, {
        .title = std::wstring(title),
        .message = std::wstring(message),
        .confirmLabel = std::wstring(confirmLabel),
        .cancelLabel = std::wstring(cancelLabel),
    });
}

bool ShowWindowsDialog(HWND owner, desto::ui::DialogSpec spec) noexcept {
    try {
        if (!EnsureDialogClass()) return false;
        DialogState state{
            .owner = owner,
            .dialog = desto::ui::Dialog(std::move(spec)),
        };
        RECT workArea{};
        const auto monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{.cbSize = sizeof(info)};
        if (monitor != nullptr && GetMonitorInfoW(monitor, &info)) {
            workArea = info.rcWork;
        } else {
            workArea = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        }
        const auto left = workArea.left + ((workArea.right - workArea.left) - kWidth) / 2;
        const auto top = workArea.top + ((workArea.bottom - workArea.top) - kHeight) / 2;
        state.window = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            kClassName, L"", WS_POPUP,
            left, top, kWidth, kHeight, owner, nullptr,
            GetModuleHandleW(nullptr), &state);
        if (state.window == nullptr) return false;
        // The popup is painted with a rounded surface; clip the native window
        // as well so the dark backing rectangle cannot show at the corners.
        const auto region = CreateRoundRectRgn(0, 0, kWidth + 1, kHeight + 1, 18, 18);
        if (region != nullptr) SetWindowRgn(state.window, region, TRUE);
        const auto ownerWasEnabled = owner != nullptr && IsWindowEnabled(owner) != FALSE;
        if (ownerWasEnabled) EnableWindow(owner, FALSE);
        ShowWindow(state.window, SW_SHOWNOACTIVATE);
        SetForegroundWindow(state.window);
        UpdateWindow(state.window);
        MSG message{};
        while (!state.done) {
            const auto result = GetMessageW(&message, nullptr, 0, 0);
            if (result <= 0) {
                state.done = true;
                break;
            }
            if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
                CloseDialog(state, false);
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (state.window != nullptr) {
            DestroyWindow(state.window);
            state.window = nullptr;
        }
        if (ownerWasEnabled) {
            EnableWindow(owner, TRUE);
            SetForegroundWindow(owner);
        }
        return state.result;
    } catch (...) {
        return false;
    }
}

bool ShowWindowsAlert(
    HWND owner,
    std::wstring_view title,
    std::wstring_view message,
    std::wstring_view buttonLabel) noexcept {
    return ShowWindowsConfirmation(owner, title, message, buttonLabel, {});
}

} // namespace desto::platform::windows
