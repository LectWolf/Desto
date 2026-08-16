#include "WindowsDesktopHost.h"
#include "ApplicationCardOrdering.h"
#include "CardContentLayout.h"
#include "PlacementInteraction.h"
#include "PremultipliedImageResampler.h"
#include "RoundedDashGeometry.h"
#include "WindowsFileDragDrop.h"

#include <Windows.h>
#include <CommCtrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <unordered_set>
#include <vector>

#undef max
#undef min

namespace desto::platform::windows {
namespace {

constexpr UINT_PTR ItemTooltipTimerId = 2;
constexpr UINT ItemTooltipDelayMilliseconds = 600;
constexpr UINT_PTR DropPreviewResetTimerId = 3;
constexpr UINT DropPreviewResetDelayMilliseconds = 90;
constexpr UINT CardItemsRefreshMessage = WM_APP + 0x41;

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const auto length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) throw std::invalid_argument("Todo title is not valid UTF-8.");
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), length) != length) {
        throw std::runtime_error("Unable to decode Todo title.");
    }
    return result;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const auto length = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (length <= 0) throw std::invalid_argument("Todo title contains invalid text.");
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), length, nullptr, nullptr) != length) {
        throw std::runtime_error("Unable to encode Todo title.");
    }
    return result;
}

struct Surface {
    HWND window = nullptr;
    HDC memoryDc = nullptr;
    HBITMAP bitmap = nullptr;
    HBITMAP previousBitmap = nullptr;
    std::uint32_t* pixels = nullptr;
    domain::PlacementProjection projection;
    domain::DisplaySnapshot display;
    presentation::CardView card;
    std::optional<domain::PlacementId> verticalLeaderPlacementId;
    std::size_t ordinal = 0;
    int width = 0;
    int height = 0;
    int interactiveHeight = 0;
    bool collapseHovered = false;
    bool collapsePressed = false;
    bool todoAddHovered = false;
    bool todoAddPressed = false;
    bool todoViewHovered = false;
    bool todoViewPressed = false;
    bool todoArchiveHovered = false;
    bool todoArchivePressed = false;
    bool todoRemainingHovered = false;
    bool todoRemainingPressed = false;
    bool todoRemainingOnly = false;
    int todoAddDateOffset = 0;
    HWND tooltip = nullptr;
    std::optional<std::size_t> hoveredItem;
    std::optional<std::size_t> hoveredTodoRow;
    std::wstring tooltipText;
    IDropTarget* dropTarget = nullptr;
    std::optional<std::size_t> dropInsertionIndex;
    std::optional<std::size_t> dropPreviewColumns;
    std::optional<presentation::CardDropPreview> pendingDropExpansion;
    std::uint64_t dropExpansionStartedAt = 0;
    std::optional<std::size_t> pressedItem;
    POINT itemDragStart{};
    bool itemDragActive = false;
    std::optional<std::size_t> pressedTodoRow;
    std::optional<std::size_t> pressedTodoCheckbox;
    std::optional<std::size_t> todoDragTarget;
    POINT todoDragStart{};
};

struct TodoDisplayEntry {
    bool header = false;
    std::size_t itemIndex = 0;
    domain::TodoDate date{};
};

std::wstring TodoDateLabel(domain::TodoDate date) {
    const auto today = domain::CurrentSystemTodoDate();
    const auto delta = domain::CompareTodoDates(date, today);
    if (delta == 0) return L"今天";
    if (date == domain::AddTodoDays(today, 1)) return L"明天";
    if (date == domain::AddTodoDays(today, -1)) return L"昨日";
    if (date == domain::AddTodoDays(today, -2)) return L"前日";
    wchar_t buffer[32]{};
    if (date.year == today.year) {
        swprintf_s(buffer, L"%u月%u日",
            static_cast<unsigned>(date.month), static_cast<unsigned>(date.day));
    } else {
        swprintf_s(buffer, L"%u年%u月%u日", static_cast<unsigned>(date.year),
            static_cast<unsigned>(date.month), static_cast<unsigned>(date.day));
    }
    return buffer;
}

LRESULT CALLBACK GuideWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window, &paint);
        RECT bounds{};
        GetClientRect(window, &bounds);
        FillRect(dc, &bounds, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        const auto brush = CreateSolidBrush(RGB(74, 132, 255));
        if (brush != nullptr) {
            const auto vertical = (bounds.bottom - bounds.top) > (bounds.right - bounds.left);
            constexpr int dash = 11;
            constexpr int gap = 7;
            const auto length = vertical ? bounds.bottom : bounds.right;
            for (int position = 0; position < length; position += dash + gap) {
                RECT segment = vertical
                    ? RECT{bounds.left, position, bounds.right,
                        std::min<LONG>(position + dash, bounds.bottom)}
                    : RECT{position, bounds.top,
                        std::min<LONG>(position + dash, bounds.right), bounds.bottom};
                FillRect(dc, &segment, brush);
            }
            DeleteObject(brush);
        }
        EndPaint(window, &paint);
        return 0;
    }
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

LRESULT CALLBACK TooltipWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window, &paint);
        RECT bounds{};
        GetClientRect(window, &bounds);
        const auto background = CreateSolidBrush(RGB(39, 41, 47));
        const auto border = CreatePen(PS_SOLID, 1, RGB(72, 76, 86));
        const auto previousBrush = background == nullptr ? nullptr : SelectObject(dc, background);
        const auto previousPen = border == nullptr ? nullptr : SelectObject(dc, border);
        RoundRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom, 10, 10);
        if (previousPen != nullptr) SelectObject(dc, previousPen);
        if (previousBrush != nullptr) SelectObject(dc, previousBrush);
        if (border != nullptr) DeleteObject(border);
        if (background != nullptr) DeleteObject(background);

        const auto length = GetWindowTextLengthW(window);
        std::wstring text(static_cast<std::size_t>(std::max(0, length)), L'\0');
        if (length > 0) GetWindowTextW(window, text.data(), length + 1);
        const auto font = CreateFontW(
            -13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
        const auto previousFont = font == nullptr ? nullptr : SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(244, 246, 249));
        RECT textRect{10, 7, bounds.right - 10, bounds.bottom - 7};
        DrawTextW(dc, text.c_str(), -1, &textRect,
            DT_LEFT | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
        if (previousFont != nullptr) SelectObject(dc, previousFont);
        if (font != nullptr) DeleteObject(font);
        EndPaint(window, &paint);
        return 0;
    }
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

} // namespace

struct WindowsDesktopHost::Impl {
    explicit Impl(std::wstring titleValue)
        : title(std::move(titleValue)), ownerThreadId(GetCurrentThreadId()) {
        if (title.empty()) {
            throw std::invalid_argument("Desktop host title must not be empty.");
        }
        MSG message{};
        PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    }

    ~Impl() {
        if (todoEditor != nullptr) finishTodoEdit(todoEditor, false);
        destroyGuides();
        destroySurfaces();
        if (guideWindowClassRegistered) {
            UnregisterClassW(guideClassName.c_str(), module);
        }
        if (tooltipWindowClassRegistered) {
            UnregisterClassW(tooltipClassName.c_str(), module);
        }
        if (windowClassRegistered) {
            UnregisterClassW(className.c_str(), module);
        }
        if (oleInitialized) {
            OleUninitialize();
        }
    }

    std::wstring title;
    std::wstring className = L"DestoDesktopHostSurface";
    std::wstring guideClassName = L"DestoAlignmentGuide";
    std::wstring tooltipClassName = L"DestoItemTooltip";
    HINSTANCE module = nullptr;
    bool windowClassRegistered = false;
    bool guideWindowClassRegistered = false;
    bool tooltipWindowClassRegistered = false;
    bool closeRequested = false;
    bool oleInitialized = false;
    HWND verticalGuide = nullptr;
    HWND horizontalGuide = nullptr;
    WindowsDesktopHost::PlacementChangedCallback placementChanged;
    WindowsDesktopHost::CardExpandedChangedCallback cardExpandedChanged;
    WindowsDesktopHost::ApplicationItemsDroppedCallback applicationItemsDropped;
    WindowsDesktopHost::ApplicationItemDragCompletedCallback applicationItemDragCompleted;
    WindowsDesktopHost::CardItemActivatedCallback cardItemActivated;
    WindowsDesktopHost::CardItemsRefreshCallback cardItemsRefresh;
    WindowsDesktopHost::TodoItemAddedCallback todoItemAdded;
    WindowsDesktopHost::TodoItemAddedScheduledCallback todoItemAddedScheduled;
    WindowsDesktopHost::TodoItemRenamedCallback todoItemRenamed;
    WindowsDesktopHost::TodoItemCompletedChangedCallback todoItemCompletedChanged;
    WindowsDesktopHost::TodoItemRemovedCallback todoItemRemoved;
    WindowsDesktopHost::TodoItemsReorderedCallback todoItemsReordered;
    WindowsDesktopHost::TodoItemsArchivedCallback todoItemsArchived;
    std::vector<Surface> surfaces;
    std::vector<domain::DisplaySnapshot> displays;
    DWORD ownerThreadId = 0;
    std::mutex pendingRefreshMutex;
    std::unordered_set<domain::CardId> pendingRefreshCards;
    HWND todoEditor = nullptr;
    HWND todoEditorSurface = nullptr;
    std::optional<std::string> todoEditorItemId;

    static LRESULT CALLBACK WindowProcedure(
        HWND window,
        UINT message,
        WPARAM wParam,
        LPARAM lParam) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(
                window,
                GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* instance = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        switch (message) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_NCHITTEST:
            return instance == nullptr ? HTCLIENT : instance->hitTest(window, lParam);
        case WM_ENTERSIZEMOVE:
            if (instance != nullptr) {
                instance->hideGuides();
                return 0;
            }
            break;
        case WM_MOVING:
            if (instance != nullptr) {
                try {
                    instance->updateInteractionGuides(
                        window,
                        *reinterpret_cast<RECT*>(lParam));
                } catch (...) {
                    instance->hideGuides();
                }
                return TRUE;
            }
            break;
        case WM_EXITSIZEMOVE:
            if (instance != nullptr) {
                instance->hideGuides();
                try {
                    instance->commitInteraction(window);
                } catch (...) {
                    // Native window procedures must not allow exceptions to cross Win32.
                }
                return 0;
            }
            break;
        case WM_LBUTTONDOWN:
            if (instance != nullptr
                && instance->beginTodoPress(window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) {
                return 0;
            }
            if (instance != nullptr
                && instance->beginCollapsePress(window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) {
                return 0;
            }
            if (instance != nullptr
                && instance->beginItemPress(window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) {
                return 0;
            }
            break;
        case WM_LBUTTONDBLCLK:
            if (instance != nullptr
                && instance->beginCollapsePress(
                    window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) {
                return 0;
            }
            if (instance != nullptr
                && instance->activateCardItem(
                    window,
                    GET_X_LPARAM(lParam),
                    GET_Y_LPARAM(lParam))) {
                return 0;
            }
            break;
        case WM_LBUTTONUP:
            if (instance != nullptr
                && instance->endTodoPress(window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) {
                return 0;
            }
            if (instance != nullptr
                && instance->endCollapsePress(window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) {
                return 0;
            }
            if (instance != nullptr && instance->endItemPress(window)) {
                return 0;
            }
            break;
        case WM_MOUSEMOVE:
            if (instance != nullptr) {
                if (instance->updateTodoDrag(
                        window,
                        GET_X_LPARAM(lParam),
                        GET_Y_LPARAM(lParam),
                        wParam)) {
                    return 0;
                }
                if (instance->updateItemDrag(
                        window,
                        GET_X_LPARAM(lParam),
                        GET_Y_LPARAM(lParam),
                        wParam)) {
                    return 0;
                }
                instance->updatePointerHover(
                    window,
                    GET_X_LPARAM(lParam),
                    GET_Y_LPARAM(lParam));
            }
            break;
        case WM_RBUTTONUP:
            if (instance != nullptr
                && instance->removeTodoAt(window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) {
                return 0;
            }
            break;
        case WM_MOUSELEAVE:
            if (instance != nullptr) {
                instance->clearPointerHover(window);
                return 0;
            }
            break;
        case WM_TIMER:
            if (instance != nullptr && wParam == ItemTooltipTimerId) {
                instance->showItemTooltip(window);
                return 0;
            }
            if (instance != nullptr && wParam == DropPreviewResetTimerId) {
                instance->clearDropPreview(window);
                return 0;
            }
            break;
        case WM_CAPTURECHANGED:
            if (instance != nullptr) {
                instance->cancelTodoPress(window);
                instance->cancelCollapsePress(window);
                instance->cancelItemPress(window);
                return 0;
            }
            break;
        case WM_ERASEBKGND:
            return 1;
        default:
            return DefWindowProcW(window, message, wParam, lParam);
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    static LRESULT CALLBACK TodoEditorProcedure(
        HWND window,
        UINT message,
        WPARAM wParam,
        LPARAM lParam,
        UINT_PTR,
        DWORD_PTR reference) {
        auto* instance = reinterpret_cast<Impl*>(reference);
        if (instance != nullptr && message == WM_KEYDOWN) {
            if (wParam == VK_RETURN) {
                instance->finishTodoEdit(window, true);
                return 0;
            }
            if (wParam == VK_ESCAPE) {
                instance->finishTodoEdit(window, false);
                return 0;
            }
        }
        if (instance != nullptr && message == WM_KILLFOCUS) {
            instance->finishTodoEdit(window, true);
            return 0;
        }
        if (message == WM_NCDESTROY) {
            RemoveWindowSubclass(window, &TodoEditorProcedure, 1);
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }

    void initialize() {
        module = GetModuleHandleW(nullptr);
        const auto oleResult = OleInitialize(nullptr);
        if (FAILED(oleResult)) {
            throw std::runtime_error("OleInitialize failed for desktop host drag and drop.");
        }
        oleInitialized = true;
        INITCOMMONCONTROLSEX controls{
            .dwSize = sizeof(INITCOMMONCONTROLSEX),
            .dwICC = ICC_WIN95_CLASSES,
        };
        if (!InitCommonControlsEx(&controls)) {
            throw std::runtime_error("InitCommonControlsEx failed for desktop host.");
        }
        WNDCLASSW windowClass{};
        windowClass.style = CS_DBLCLKS;
        windowClass.lpfnWndProc = &WindowProcedure;
        windowClass.hInstance = module;
        windowClass.lpszClassName = className.c_str();
        windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(IDC_ARROW));
        if (RegisterClassW(&windowClass) == 0) {
            throw std::runtime_error("RegisterClassW failed for desktop host.");
        }
        windowClassRegistered = true;

        WNDCLASSW guideClass{};
        guideClass.lpfnWndProc = &GuideWindowProcedure;
        guideClass.hInstance = module;
        guideClass.lpszClassName = guideClassName.c_str();
        guideClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(IDC_ARROW));
        guideClass.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        if (RegisterClassW(&guideClass) == 0) {
            throw std::runtime_error("RegisterClassW failed for alignment guides.");
        }
        guideWindowClassRegistered = true;

        WNDCLASSW tooltipClass{};
        tooltipClass.lpfnWndProc = &TooltipWindowProcedure;
        tooltipClass.hInstance = module;
        tooltipClass.lpszClassName = tooltipClassName.c_str();
        tooltipClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(IDC_ARROW));
        if (RegisterClassW(&tooltipClass) == 0) {
            throw std::runtime_error("RegisterClassW failed for item Tooltip.");
        }
        tooltipWindowClassRegistered = true;
    }

    Surface* findSurface(HWND window) noexcept {
        const auto found = std::find_if(
            surfaces.begin(), surfaces.end(), [&](const Surface& surface) {
                return surface.window == window;
            });
        return found == surfaces.end() ? nullptr : &*found;
    }

    static RECT displayPixelRect(const domain::DisplaySnapshot& display) noexcept {
        const auto scale = display.effectiveDpi / 96.0;
        const auto left = static_cast<LONG>(std::lround(display.workAreaLeft * scale));
        const auto top = static_cast<LONG>(std::lround(display.workAreaTop * scale));
        return {
            left,
            top,
            left + static_cast<LONG>(std::lround(display.workAreaWidth * scale)),
            top + static_cast<LONG>(std::lround(display.workAreaHeight * scale)),
        };
    }

    const domain::DisplaySnapshot* displayForWindowRect(
        const RECT& windowRect,
        const domain::DisplayId& preferredDisplayId) const noexcept {
        const domain::DisplaySnapshot* result = nullptr;
        long long bestOverlap = -1;
        double bestDistance = 0.0;
        const auto windowCenterX = (windowRect.left + windowRect.right) / 2.0;
        const auto windowCenterY = (windowRect.top + windowRect.bottom) / 2.0;
        for (const auto& display : displays) {
            const auto bounds = displayPixelRect(display);
            const auto intersectionWidth = std::max<LONG>(
                0, std::min(windowRect.right, bounds.right) - std::max(windowRect.left, bounds.left));
            const auto intersectionHeight = std::max<LONG>(
                0, std::min(windowRect.bottom, bounds.bottom) - std::max(windowRect.top, bounds.top));
            const auto overlap = static_cast<long long>(intersectionWidth) * intersectionHeight;
            const auto centerX = (bounds.left + bounds.right) / 2.0;
            const auto centerY = (bounds.top + bounds.bottom) / 2.0;
            const auto distance = std::pow(windowCenterX - centerX, 2.0)
                + std::pow(windowCenterY - centerY, 2.0);
            const auto preferredTie = result != nullptr
                && display.id == preferredDisplayId && result->id != preferredDisplayId;
            if (result == nullptr || overlap > bestOverlap
                || (overlap == bestOverlap && (distance < bestDistance || preferredTie))) {
                result = &display;
                bestOverlap = overlap;
                bestDistance = distance;
            }
        }
        return result;
    }

    static int dipToPixels(double value, const Surface& surface) noexcept {
        return std::max(1, static_cast<int>(std::lround(
            value * surface.display.effectiveDpi / 96.0)));
    }

    static RECT collapseControlRect(const Surface& surface) noexcept {
        const auto size = dipToPixels(36.0, surface);
        const auto inset = dipToPixels(6.0, surface);
        return {
            surface.width - inset - size,
            inset,
            surface.width - inset,
            inset + size,
        };
    }

    static RECT todoAddControlRect(const Surface& surface) noexcept {
        const auto inset = dipToPixels(10.0, surface);
        const auto top = dipToPixels(80.0, surface);
        return {inset, top, surface.width - inset, top + dipToPixels(44.0, surface)};
    }

    static RECT todoArchiveControlRect(const Surface& surface) noexcept {
        const auto right = surface.width - dipToPixels(82.0, surface);
        return {right - dipToPixels(50.0, surface), dipToPixels(44.0, surface),
            right, dipToPixels(76.0, surface)};
    }

    static RECT todoViewControlRect(const Surface& surface) noexcept {
        return {dipToPixels(10.0, surface), dipToPixels(44.0, surface),
            dipToPixels(78.0, surface), dipToPixels(76.0, surface)};
    }

    static RECT todoRemainingRect(const Surface& surface) noexcept {
        return {surface.width - dipToPixels(76.0, surface), dipToPixels(44.0, surface),
            surface.width - dipToPixels(10.0, surface), dipToPixels(76.0, surface)};
    }

    static std::vector<TodoDisplayEntry> todoDisplayEntries(const Surface& surface) {
        const auto today = domain::CurrentSystemTodoDate();
        const auto selectedDate = domain::AddTodoDays(today, surface.todoAddDateOffset);
        std::vector<std::size_t> overdue;
        std::vector<std::size_t> selected;
        for (std::size_t index = 0; index < surface.card.todoItems.size(); ++index) {
            const auto& item = surface.card.todoItems[index];
            if (item.archived) continue;
            if (surface.todoRemainingOnly && item.completed) continue;
            const auto date = item.scheduledDate.value_or(today);
            if (surface.todoAddDateOffset == 0
                && !item.completed && domain::CompareTodoDates(date, today) < 0) {
                overdue.push_back(index);
            } else if (date == selectedDate) {
                selected.push_back(index);
            }
        }
        std::ranges::stable_sort(overdue, [&](std::size_t left, std::size_t right) {
            const auto leftDate = surface.card.todoItems[left].scheduledDate.value_or(today);
            const auto rightDate = surface.card.todoItems[right].scheduledDate.value_or(today);
            return leftDate == rightDate ? left < right : leftDate < rightDate;
        });
        std::vector<TodoDisplayEntry> result;
        std::optional<domain::TodoDate> lastDate;
        for (const auto itemIndex : overdue) {
            const auto date = surface.card.todoItems[itemIndex].scheduledDate.value_or(today);
            if (!lastDate.has_value() || *lastDate != date) {
                result.push_back({true, 0, date});
                lastDate = date;
            }
            result.push_back({false, itemIndex, date});
        }
        for (const auto itemIndex : selected) {
            result.push_back({false, itemIndex, selectedDate});
        }
        return result;
    }

    static double todoRowHeightDip(const Surface& surface, std::size_t itemIndex) noexcept {
        if (itemIndex >= surface.card.todoItems.size()) return 42.0;
        try {
            const auto title = Utf8ToWide(surface.card.todoItems[itemIndex].title);
            const auto scale = surface.display.effectiveDpi / 96.0;
            const auto dc = GetDC(nullptr);
            if (dc == nullptr) return 42.0;
            const auto font = CreateFontW(
                -std::max(1, static_cast<int>(std::lround(13.0 * scale))), 0, 0, 0,
                FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                L"Segoe UI Variable Text");
            const auto previous = font == nullptr ? nullptr : SelectObject(dc, font);
            RECT measured{0, 0, std::max(1, static_cast<int>(std::lround(
                std::max(88.0, surface.projection.rect.width - 76.0) * scale))), 0};
            DrawTextW(dc, title.c_str(), -1, &measured,
                DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
            if (previous != nullptr) SelectObject(dc, previous);
            if (font != nullptr) DeleteObject(font);
            ReleaseDC(nullptr, dc);
            return std::max(42.0, measured.bottom / scale + 12.0
                + (surface.card.todoPreferences.showCreatedTime ? 14.0 : 0.0));
        } catch (...) {
            return 42.0;
        }
    }

    static RECT todoEntryRect(const Surface& surface, std::size_t entryIndex) noexcept {
        const auto inset = dipToPixels(10.0, surface);
        LONG top = dipToPixels(128.0, surface);
        const auto entries = todoDisplayEntries(surface);
        for (std::size_t index = 0; index < entryIndex && index < entries.size(); ++index) {
            top += dipToPixels(entries[index].header
                ? 22.0 : todoRowHeightDip(surface, entries[index].itemIndex), surface);
        }
        const auto height = entries.size() > entryIndex && entries[entryIndex].header
            ? dipToPixels(22.0, surface)
            : dipToPixels(entries.size() > entryIndex
                ? todoRowHeightDip(surface, entries[entryIndex].itemIndex) : 42.0, surface);
        return {inset, top, surface.width - inset, top + height};
    }

    static std::vector<RECT> todoEntryRects(
        const Surface& surface,
        std::span<const TodoDisplayEntry> entries) {
        std::vector<RECT> result;
        result.reserve(entries.size());
        const auto inset = dipToPixels(10.0, surface);
        LONG top = dipToPixels(128.0, surface);
        for (const auto& entry : entries) {
            const auto height = dipToPixels(entry.header
                ? 22.0 : todoRowHeightDip(surface, entry.itemIndex), surface);
            result.push_back({inset, top, surface.width - inset, top + height});
            top += height;
        }
        return result;
    }

    static RECT todoRowRect(const Surface& surface, std::size_t itemIndex) noexcept {
        const auto entries = todoDisplayEntries(surface);
        for (std::size_t index = 0; index < entries.size(); ++index) {
            if (!entries[index].header && entries[index].itemIndex == itemIndex) {
                return todoEntryRect(surface, index);
            }
        }
        const auto empty = todoAddControlRect(surface);
        return {empty.left, empty.bottom, empty.right, empty.bottom + dipToPixels(42.0, surface)};
    }

    static RECT todoCheckboxRect(const Surface& surface, std::size_t index) noexcept {
        const auto row = todoRowRect(surface, index);
        const auto size = dipToPixels(18.0, surface);
        const auto left = row.left + dipToPixels(8.0, surface);
        const auto top = row.top + ((row.bottom - row.top) - size) / 2;
        return {left, top, left + size, top + size};
    }


    static RECT todoCheckboxRect(const Surface& surface, const RECT& row) noexcept {
        const auto size = dipToPixels(18.0, surface);
        const auto left = row.left + dipToPixels(8.0, surface);
        const auto top = row.top + ((row.bottom - row.top) - size) / 2;
        return {left, top, left + size, top + size};
    }

    static bool pointInside(const RECT& rect, int x, int y) noexcept {
        return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
    }

    static presentation::CardContentLayoutSettings baseItemLayoutSettings(
        const Surface& surface) noexcept {
        auto settings = presentation::ResolveCardContentLayoutSettings(surface.card.content);
        if (surface.card.content.sizeMode == domain::CardSizeMode::Fixed) {
            settings.minimumColumns = surface.card.content.fixedColumns;
            settings.maximumColumns = surface.card.content.fixedColumns;
            settings.preferredColumns = surface.card.content.fixedColumns;
        } else {
            std::size_t requiredColumns = 1;
            if (surface.card.applicationSortMode == domain::ApplicationItemSortMode::Custom) {
                for (const auto& placement : surface.card.applicationItemPlacements) {
                    requiredColumns = std::max<std::size_t>(
                        requiredColumns, placement.column + 1);
                }
            }
            settings.preferredColumns = presentation::ResolveAdaptiveCardColumns(
                surface.card.applicationSortMode == domain::ApplicationItemSortMode::Custom
                    ? std::size_t{0}
                    : surface.card.items.size(),
                requiredColumns,
                settings);
        }
        return settings;
    }

    static presentation::CardContentLayoutSettings itemLayoutSettings(
        const Surface& surface) noexcept {
        auto settings = baseItemLayoutSettings(surface);
        if (surface.card.content.sizeMode != domain::CardSizeMode::Fixed) {
            if (surface.dropPreviewColumns.has_value()) {
                settings.preferredColumns = std::max(
                    settings.preferredColumns, *surface.dropPreviewColumns);
            } else if (surface.dropInsertionIndex.has_value()) {
                settings.preferredColumns = presentation::ResolveAdaptiveCardColumns(
                    std::max(surface.card.items.size(), *surface.dropInsertionIndex + 1),
                    settings.preferredColumns,
                    settings);
            }
            settings.minimumColumns = settings.preferredColumns;
            settings.maximumColumns = settings.preferredColumns;
        }
        return settings;
    }

    static std::size_t contentSlotCount(
        const Surface& surface,
        bool includeDropPreview = true) noexcept {
        if (surface.card.content.sizeMode == domain::CardSizeMode::Fixed) {
            return static_cast<std::size_t>(surface.card.content.fixedColumns)
                * surface.card.content.fixedRows;
        }
        auto result = surface.card.items.size();
        if (surface.card.applicationSortMode == domain::ApplicationItemSortMode::Custom) {
            const auto columns = itemLayoutSettings(surface).preferredColumns;
            for (const auto& placement : surface.card.applicationItemPlacements) {
                result = std::max(
                    result,
                    static_cast<std::size_t>(placement.row) * columns
                        + placement.column + 1);
            }
        }
        if (includeDropPreview && surface.dropInsertionIndex.has_value()) {
            result = std::max(result, *surface.dropInsertionIndex + 1);
        }
        return result;
    }

    static DWORD acceptedDropEffect(
        const Surface& surface,
        DWORD allowedEffect) noexcept {
        if (surface.card.type == domain::CardType::Application) {
            return (allowedEffect & DROPEFFECT_MOVE) != 0
                ? DROPEFFECT_MOVE : DROPEFFECT_NONE;
        }
        if (surface.card.type != domain::CardType::Mapping) {
            return DROPEFFECT_NONE;
        }
        if (surface.card.mappingMode == domain::MappingMode::Folder) {
            return surface.card.mappingAllowsSourceMutation
                    && (allowedEffect & DROPEFFECT_MOVE) != 0
                ? DROPEFFECT_MOVE : DROPEFFECT_NONE;
        }
        return (allowedEffect & DROPEFFECT_COPY) != 0
            ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    }

    static domain::PlacementRect contentDrivenRect(const Surface& surface) {
        if (surface.card.type == domain::CardType::Todo) {
            auto rect = surface.projection.rect;
            const auto right = rect.left + rect.width;
            const auto horizontalCenter = rect.left + rect.width / 2.0;
            const auto bottom = rect.top + rect.height;
            const auto verticalCenter = rect.top + rect.height / 2.0;
            rect.width = std::min(std::max(240.0, rect.width), surface.display.workAreaWidth);
            const auto entries = todoDisplayEntries(surface);
            double contentHeight = 136.0;
            for (const auto& entry : entries) contentHeight += entry.header
                ? 22.0 : todoRowHeightDip(surface, entry.itemIndex);
            if (entries.empty()) contentHeight = 206.0;
            rect.height = std::min(std::max(178.0, contentHeight), surface.display.workAreaHeight);
            if (surface.projection.horizontalAnchor == domain::PlacementHorizontalAnchor::Right) {
                rect.left = right - rect.width;
            } else if (surface.projection.horizontalAnchor
                       == domain::PlacementHorizontalAnchor::Center) {
                rect.left = horizontalCenter - rect.width / 2.0;
            }
            if (surface.projection.verticalAnchor == domain::PlacementVerticalAnchor::Bottom) {
                rect.top = bottom - rect.height;
            } else if (surface.projection.verticalAnchor
                       == domain::PlacementVerticalAnchor::Center) {
                rect.top = verticalCenter - rect.height / 2.0;
            }
            return presentation::ResolvePlacementInteraction(
                rect, surface.display.workAreaWidth, surface.display.workAreaHeight, {}, true);
        }
        const auto settings = itemLayoutSettings(surface);
        const auto columns = settings.preferredColumns;
        const auto width = std::max(
            180.0,
            settings.horizontalPadding * 2.0
                + columns * settings.itemWidth
                + (columns - 1) * settings.horizontalGap);
        const auto slotCount = contentSlotCount(surface);
        const auto layoutItemCount = (surface.card.type == domain::CardType::Application
                || surface.card.type == domain::CardType::Mapping)
                && slotCount == 0
            ? std::size_t{1}
            : slotCount;
        const auto layout = presentation::ResolveCardContentLayout(
            layoutItemCount, width, settings);
        auto rect = surface.projection.rect;
        const auto right = rect.left + rect.width;
        const auto horizontalCenter = rect.left + rect.width / 2.0;
        const auto bottom = rect.top + rect.height;
        const auto verticalCenter = rect.top + rect.height / 2.0;
        rect.width = std::min(width, surface.display.workAreaWidth);
        rect.height = std::min(
            std::max(120.0, layout.idealHeight), surface.display.workAreaHeight);
        if (surface.projection.horizontalAnchor == domain::PlacementHorizontalAnchor::Right) {
            rect.left = right - rect.width;
        } else if (surface.projection.horizontalAnchor
                   == domain::PlacementHorizontalAnchor::Center) {
            rect.left = horizontalCenter - rect.width / 2.0;
        }
        if (surface.projection.verticalAnchor == domain::PlacementVerticalAnchor::Bottom) {
            rect.top = bottom - rect.height;
        } else if (surface.projection.verticalAnchor
                   == domain::PlacementVerticalAnchor::Center) {
            rect.top = verticalCenter - rect.height / 2.0;
        }
        return presentation::ResolvePlacementInteraction(
            rect,
            surface.display.workAreaWidth,
            surface.display.workAreaHeight,
            {},
            true);
    }

    static presentation::CardContentLayout itemLayout(
        const Surface& surface,
        std::size_t itemCount) {
        const auto scale = surface.display.effectiveDpi / 96.0;
        return presentation::ResolveCardContentLayout(
            itemCount,
            surface.width / scale,
            itemLayoutSettings(surface));
    }

    static RECT itemRect(
        const Surface& surface,
        std::size_t column,
        std::size_t row,
        std::size_t slotCount) {
        const auto settings = itemLayoutSettings(surface);
        const auto layout = itemLayout(surface, slotCount);
        const auto scale = surface.display.effectiveDpi / 96.0;
        const auto widthDip = surface.width / scale;
        const auto contentLeft = (widthDip - layout.contentWidth) / 2.0;
        const auto left = contentLeft + column * (settings.itemWidth + settings.horizontalGap);
        const auto top = settings.headerHeight + settings.verticalPadding
            + row * (settings.itemHeight + settings.verticalGap);
        return {
            static_cast<LONG>(std::lround(left * scale)),
            static_cast<LONG>(std::lround(top * scale)),
            static_cast<LONG>(std::lround((left + settings.itemWidth) * scale)),
            static_cast<LONG>(std::lround((top + settings.itemHeight) * scale)),
        };
    }

    struct ProjectedItem {
        std::size_t itemIndex;
        std::uint32_t column;
        std::uint32_t row;
    };

    static std::vector<ProjectedItem> projectedItems(const Surface& surface) {
        const auto columns = static_cast<std::uint32_t>(itemLayout(surface, 0).columns);
        std::vector<application::ApplicationItemSortData> metadata;
        metadata.reserve(surface.card.items.size());
        for (const auto& item : surface.card.items) {
            metadata.push_back({
                item.sourcePath.filename(), item.displayName, item.fileSize,
                item.itemType, item.modifiedTime,
            });
        }
        const auto projection = application::ProjectApplicationItems(
            metadata,
            surface.card.applicationItemPlacements,
            surface.card.applicationSortMode,
            columns);
        std::vector<ProjectedItem> result;
        result.reserve(projection.size());
        for (const auto& projected : projection) {
            const auto found = std::find_if(
                surface.card.items.begin(), surface.card.items.end(), [&](const auto& item) {
                    return _wcsicmp(
                        item.sourcePath.filename().c_str(), projected.fileName.c_str()) == 0;
                });
            if (found != surface.card.items.end()) {
                result.push_back({
                    static_cast<std::size_t>(std::distance(surface.card.items.begin(), found)),
                    projected.column,
                    projected.row,
                });
            }
        }
        return result;
    }

    static std::size_t projectedSlotCount(
        const Surface& surface,
        std::span<const ProjectedItem> projection) {
        const auto columns = itemLayout(surface, 0).columns;
        if (surface.card.content.sizeMode == domain::CardSizeMode::Fixed) {
            return static_cast<std::size_t>(surface.card.content.fixedColumns)
                * surface.card.content.fixedRows;
        }
        std::size_t result = 0;
        for (const auto& item : projection) {
            result = std::max(
                result,
                static_cast<std::size_t>(item.row) * columns + item.column + 1);
        }
        return result;
    }

    static std::optional<std::size_t> itemAt(const Surface& surface, int x, int y) {
        if (!surface.card.expanded || surface.card.items.empty()
            || y < dipToPixels(48.0, surface) || y >= surface.interactiveHeight) {
            return std::nullopt;
        }
        const auto projection = projectedItems(surface);
        const auto slotCount = projectedSlotCount(surface, projection);
        for (const auto& projected : projection) {
            if (pointInside(itemRect(
                    surface, projected.column, projected.row, slotCount), x, y)) {
                return projected.itemIndex;
            }
        }
        return std::nullopt;
    }

    static std::optional<std::size_t> todoRowAt(const Surface& surface, int x, int y) noexcept {
        if (!surface.card.expanded || surface.card.type != domain::CardType::Todo) {
            return std::nullopt;
        }
        const auto entries = todoDisplayEntries(surface);
        const auto rects = todoEntryRects(surface, entries);
        for (std::size_t index = 0; index < entries.size(); ++index) {
            if (!entries[index].header && pointInside(rects[index], x, y)) {
                return entries[index].itemIndex;
            }
        }
        return std::nullopt;
    }

    static bool isTodoAddControlHit(const Surface& surface, int x, int y) noexcept {
        return surface.card.type == domain::CardType::Todo
            && pointInside(todoAddControlRect(surface), x, y);
    }

    static bool isTodoArchiveControlHit(const Surface& surface, int x, int y) noexcept {
        return surface.card.type == domain::CardType::Todo
            && pointInside(todoArchiveControlRect(surface), x, y);
    }

    static bool isTodoViewControlHit(const Surface& surface, int x, int y) noexcept {
        return surface.card.type == domain::CardType::Todo
            && pointInside(todoViewControlRect(surface), x, y);
    }

    static bool isTodoRemainingControlHit(const Surface& surface, int x, int y) noexcept {
        return surface.card.type == domain::CardType::Todo
            && pointInside(todoRemainingRect(surface), x, y);
    }

    bool isCollapseControlHit(const Surface& surface, int x, int y) const noexcept {
        return surface.card.showCollapseControl
            && pointInside(collapseControlRect(surface), x, y);
    }

    LRESULT hitTest(HWND window, LPARAM lParam) noexcept {
        const auto* surface = findSurface(window);
        if (surface == nullptr) {
            return HTCLIENT;
        }
        RECT windowRect{};
        GetWindowRect(window, &windowRect);
        const auto x = GET_X_LPARAM(lParam) - windowRect.left;
        const auto y = GET_Y_LPARAM(lParam) - windowRect.top;
        if (y < 0 || y >= surface->interactiveHeight) {
            return HTTRANSPARENT;
        }
        if (y < dipToPixels(48.0, *surface)
            && !isCollapseControlHit(*surface, x, y)
            && !isTodoArchiveControlHit(*surface, x, y)
            && !isTodoViewControlHit(*surface, x, y)
            && !isTodoAddControlHit(*surface, x, y)) {
            return HTCAPTION;
        }
        return HTCLIENT;
    }

    void destroyGuides() noexcept {
        if (verticalGuide != nullptr) {
            DestroyWindow(verticalGuide);
            verticalGuide = nullptr;
        }
        if (horizontalGuide != nullptr) {
            DestroyWindow(horizontalGuide);
            horizontalGuide = nullptr;
        }
    }

    void hideGuides() noexcept {
        if (verticalGuide != nullptr) {
            ShowWindow(verticalGuide, SW_HIDE);
        }
        if (horizontalGuide != nullptr) {
            ShowWindow(horizontalGuide, SW_HIDE);
        }
    }

    void ensureGuides() {
        const auto createGuide = [&]() {
            const auto window = CreateWindowExW(
                WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                guideClassName.c_str(),
                L"",
                WS_POPUP,
                0,
                0,
                1,
                1,
                nullptr,
                nullptr,
                module,
                nullptr);
            if (window == nullptr) {
                throw std::runtime_error("CreateWindowExW failed for alignment guide.");
            }
            SetLayeredWindowAttributes(window, RGB(0, 0, 0), 220, LWA_ALPHA | LWA_COLORKEY);
            return window;
        };
        if (verticalGuide == nullptr) {
            verticalGuide = createGuide();
        }
        if (horizontalGuide == nullptr) {
            horizontalGuide = createGuide();
        }
    }

    void updateInteractionGuides(HWND window, RECT& windowRect) {
        auto* moved = findSurface(window);
        if (moved == nullptr) {
            hideGuides();
            return;
        }
        const auto* targetDisplay = displayForWindowRect(windowRect, moved->display.id);
        if (targetDisplay == nullptr) {
            hideGuides();
            return;
        }
        const auto target = *targetDisplay;
        const auto scale = target.effectiveDpi / 96.0;
        if (moved->display.id != target.id) {
            moved->display = target;
            moved->projection.displayId = target.id;
            const auto width = std::max(1, static_cast<int>(std::lround(
                moved->projection.rect.width * scale)));
            const auto height = std::max(1, static_cast<int>(std::lround(
                moved->projection.rect.height * scale)));
            replaceBitmap(*moved, width, height);
            render(*moved, moved->display, moved->card, moved->ordinal, false);
            windowRect.right = windowRect.left + width;
            windowRect.bottom = windowRect.top + height;
            commitSurfaceAt(*moved, {windowRect.left, windowRect.top});
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
            hideGuides();
            return;
        }
        const domain::PlacementRect proposed{
            .left = windowRect.left / scale - target.workAreaLeft,
            .top = windowRect.top / scale - target.workAreaTop,
            .width = moved->projection.rect.width,
            .height = moved->projection.rect.height,
        };
        std::vector<domain::PlacementRect> otherCards;
        for (const auto& surface : surfaces) {
            if (surface.window != window
                && surface.projection.displayId == target.id
                && surface.projection.placementId != moved->projection.placementId) {
                otherCards.push_back(surface.projection.rect);
            }
        }
        const auto result = presentation::ResolvePlacementInteractionDetailed(
            proposed,
            target.workAreaWidth,
            target.workAreaHeight,
            otherCards,
            false);
        ensureGuides();
        const auto guideThickness = std::max(3, static_cast<int>(std::lround(3.0 * scale)));
        const auto workLeft = static_cast<int>(std::lround(target.workAreaLeft * scale));
        const auto workTop = static_cast<int>(std::lround(target.workAreaTop * scale));
        const auto workWidth = std::max(1, static_cast<int>(std::lround(
            target.workAreaWidth * scale)));
        const auto workHeight = std::max(1, static_cast<int>(std::lround(
            target.workAreaHeight * scale)));
        if (result.verticalGuide.has_value()) {
            const auto x = static_cast<int>(std::lround(
                (target.workAreaLeft + *result.verticalGuide) * scale));
            SetWindowPos(
                verticalGuide,
                HWND_TOPMOST,
                x - guideThickness / 2,
                workTop,
                guideThickness,
                workHeight,
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
        } else {
            ShowWindow(verticalGuide, SW_HIDE);
        }
        if (result.horizontalGuide.has_value()) {
            const auto y = static_cast<int>(std::lround(
                (target.workAreaTop + *result.horizontalGuide) * scale));
            SetWindowPos(
                horizontalGuide,
                HWND_TOPMOST,
                workLeft,
                y - guideThickness / 2,
                workWidth,
                guideThickness,
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
        } else {
            ShowWindow(horizontalGuide, SW_HIDE);
        }
    }

    Surface* findSurfaceByCard(const domain::CardId& cardId) noexcept {
        const auto found = std::ranges::find_if(surfaces, [&](const Surface& surface) {
            return surface.card.id == cardId;
        });
        return found == surfaces.end() ? nullptr : &*found;
    }

    static double visibleHeightDip(const Surface& surface) noexcept {
        return surface.card.expanded ? surface.projection.rect.height : 48.0;
    }

    static bool horizontallyAligned(
        const domain::PlacementRect& left,
        const domain::PlacementRect& right) noexcept {
        constexpr double tolerance = 1.5;
        return std::abs(left.left - right.left) <= tolerance
            || std::abs((left.left + left.width) - (right.left + right.width)) <= tolerance
            || std::abs((left.left + left.width / 2.0)
                - (right.left + right.width / 2.0)) <= tolerance;
    }

    void inferVerticalLeader(Surface& follower) noexcept {
        follower.verticalLeaderPlacementId.reset();
        constexpr double visualGap = 8.0;
        constexpr double tolerance = 1.5;
        for (const auto& candidate : surfaces) {
            if (&candidate == &follower
                || candidate.projection.displayId != follower.projection.displayId
                || !horizontallyAligned(candidate.projection.rect, follower.projection.rect)) {
                continue;
            }
            const auto expandedTop = candidate.projection.rect.top
                + candidate.projection.rect.height + visualGap;
            const auto visibleTop = candidate.projection.rect.top
                + visibleHeightDip(candidate) + visualGap;
            if (std::abs(follower.projection.rect.top - expandedTop) <= tolerance
                || std::abs(follower.projection.rect.top - visibleTop) <= tolerance) {
                follower.verticalLeaderPlacementId = candidate.projection.placementId;
                return;
            }
        }
    }

    void inferVerticalLeaders() noexcept {
        for (auto& surface : surfaces) inferVerticalLeader(surface);
    }

    void reflowVerticalFollowers(Surface& leader, bool commit) noexcept {
        std::unordered_set<domain::PlacementId> visited;
        const auto reflow = [&](auto&& self, Surface& current) -> void {
            if (!visited.insert(current.projection.placementId).second) return;
            for (auto& follower : surfaces) {
                if (!follower.verticalLeaderPlacementId.has_value()
                    || *follower.verticalLeaderPlacementId != current.projection.placementId
                    || follower.projection.displayId != current.projection.displayId) {
                    continue;
                }
                follower.projection.rect.top = current.projection.rect.top
                    + visibleHeightDip(current) + 8.0;
                if (commit && follower.window != nullptr) {
                    try {
                        commitSurface(follower);
                        if (placementChanged) {
                            placementChanged(
                                follower.projection.placementId,
                                follower.projection.cardId,
                                follower.projection.displayId,
                                follower.projection.rect,
                                follower.projection.horizontalAnchor,
                                follower.projection.verticalAnchor,
                                follower.display.workAreaWidth,
                                follower.display.workAreaHeight);
                        }
                    } catch (...) {}
                }
                self(self, follower);
            }
        };
        reflow(reflow, leader);
    }

    void repaintTodoCard(const domain::CardId& cardId) noexcept {
        const auto* source = findSurfaceByCard(cardId);
        if (source == nullptr) return;
        const auto items = source->card.todoItems;
        for (auto& surface : surfaces) {
            if (surface.card.id != cardId) continue;
            try {
                surface.card.todoItems = items;
                resizeSurfaceForContent(surface, true);
                render(surface, surface.display, surface.card, surface.ordinal);
            } catch (...) {
            }
        }
        for (auto& surface : surfaces) {
            if (surface.card.id == cardId) reflowVerticalFollowers(surface, true);
        }
    }

    void finishTodoEdit(HWND editor, bool commit) noexcept {
        if (editor == nullptr || editor != todoEditor) return;
        const auto surfaceWindow = todoEditorSurface;
        const auto itemId = todoEditorItemId;
        todoEditor = nullptr;
        todoEditorSurface = nullptr;
        todoEditorItemId.reset();
        std::wstring text;
        if (commit) {
            const auto length = GetWindowTextLengthW(editor);
            text.resize(static_cast<std::size_t>(std::max(0, length)));
            if (length > 0) GetWindowTextW(editor, text.data(), length + 1);
        }
        RemoveWindowSubclass(editor, &TodoEditorProcedure, 1);
        DestroyWindow(editor);
        if (!commit || text.empty() || surfaceWindow == nullptr) return;
        auto* surface = findSurface(surfaceWindow);
        if (surface == nullptr) return;
        try {
            const auto utf8 = WideToUtf8(text);
            bool accepted = false;
            if (itemId.has_value()) {
                accepted = todoItemRenamed
                    && todoItemRenamed(surface->card.id, *itemId, utf8);
                if (accepted) {
                    for (auto& item : surface->card.todoItems) {
                        if (item.id == *itemId) item.title = utf8;
                    }
                }
            } else if (todoItemAddedScheduled) {
                const auto added = todoItemAddedScheduled(
                    surface->card.id,
                    utf8,
                    domain::AddTodoDays(domain::CurrentSystemTodoDate(), surface->todoAddDateOffset));
                if (added.has_value()) {
                    surface->card.todoItems.push_back(*added);
                    accepted = true;
                }
            } else if (todoItemAdded) {
                const auto added = todoItemAdded(surface->card.id, utf8);
                if (added.has_value()) {
                    surface->card.todoItems.push_back(*added);
                    accepted = true;
                }
            }
            if (accepted) repaintTodoCard(surface->card.id);
        } catch (...) {
        }
    }

    void beginTodoEdit(HWND window, std::optional<std::size_t> row) noexcept {
        try {
        auto* surface = findSurface(window);
        if (surface == nullptr || surface->card.type != domain::CardType::Todo
            || todoEditor != nullptr) return;
        std::wstring initial;
        std::optional<std::string> itemId;
        if (row.has_value() && *row < surface->card.todoItems.size()) {
            itemId = surface->card.todoItems[*row].id;
            initial = Utf8ToWide(surface->card.todoItems[*row].title);
        }
        RECT anchor = row.has_value() ? todoRowRect(*surface, *row) : todoAddControlRect(*surface);
        POINT topLeft{anchor.left, anchor.top};
        ClientToScreen(window, &topLeft);
        const auto width = static_cast<int>(row.has_value()
            ? std::max<LONG>(dipToPixels(120.0, *surface), anchor.right - anchor.left)
            : std::max<LONG>(dipToPixels(100.0, *surface), anchor.right - anchor.left - dipToPixels(108.0, *surface)));
        const auto height = static_cast<int>(
            std::max<LONG>(dipToPixels(32.0, *surface), anchor.bottom - anchor.top));
        auto editor = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            L"EDIT",
            initial.c_str(),
            WS_POPUP | ES_AUTOHSCROLL,
            topLeft.x,
            topLeft.y,
            width,
            height,
            window,
            nullptr,
            module,
            nullptr);
        if (editor == nullptr) return;
        SendMessageW(editor, EM_SETLIMITTEXT, 512, 0);
        SendMessageW(editor, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        todoEditor = editor;
        todoEditorSurface = window;
        todoEditorItemId = std::move(itemId);
        SetWindowSubclass(editor, &TodoEditorProcedure, 1, reinterpret_cast<DWORD_PTR>(this));
        ShowWindow(editor, SW_SHOWNOACTIVATE);
        SetWindowPos(editor, HWND_TOPMOST, topLeft.x, topLeft.y, width, height, SWP_SHOWWINDOW);
        SetForegroundWindow(editor);
        SetFocus(editor);
        SendMessageW(editor, EM_SETSEL, 0, -1);
        } catch (...) {
        }
    }

    bool beginTodoPress(HWND window, int x, int y) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr || surface->card.type != domain::CardType::Todo) return false;
        if (isTodoViewControlHit(*surface, x, y)) {
            surface->todoViewPressed = true;
            surface->todoViewHovered = true;
            SetCapture(window);
            try { render(*surface, surface->display, surface->card, surface->ordinal); } catch (...) {}
            return true;
        }
        if (isTodoArchiveControlHit(*surface, x, y)) {
            surface->todoArchivePressed = true;
            surface->todoArchiveHovered = true;
            SetCapture(window);
            try { render(*surface, surface->display, surface->card, surface->ordinal); } catch (...) {}
            return true;
        }
        if (isTodoRemainingControlHit(*surface, x, y)) {
            surface->todoRemainingPressed = true;
            surface->todoRemainingHovered = true;
            SetCapture(window);
            try { render(*surface, surface->display, surface->card, surface->ordinal); } catch (...) {}
            return true;
        }
        if (isTodoAddControlHit(*surface, x, y)) {
            surface->todoAddPressed = true;
            surface->todoAddHovered = true;
            SetCapture(window);
            try { render(*surface, surface->display, surface->card, surface->ordinal); } catch (...) {}
            return true;
        }
        if (!surface->card.expanded) return false;
        if (surface->card.todoItems.empty()
            && pointInside(todoRowRect(*surface, 0), x, y)) {
            beginTodoEdit(window, std::nullopt);
            return true;
        }
        const auto row = todoRowAt(*surface, x, y);
        if (!row.has_value()) return false;
        if (pointInside(todoCheckboxRect(*surface, *row), x, y)) {
            surface->pressedTodoCheckbox = row;
            SetCapture(window);
            try { render(*surface, surface->display, surface->card, surface->ordinal); } catch (...) {}
            return true;
        }
        surface->pressedTodoRow = row;
        surface->todoDragStart = {x, y};
        SetCapture(window);
        return true;
    }

    bool editTodoAt(HWND window, int x, int y) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr || surface->card.type != domain::CardType::Todo) return false;
        const auto row = todoRowAt(*surface, x, y);
        if (!row.has_value()) return false;
        beginTodoEdit(window, row);
        return true;
    }

    bool updateTodoDrag(HWND window, int x, int y, WPARAM keyState) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr) return false;
        if (surface->todoViewPressed || surface->todoArchivePressed
            || surface->todoRemainingPressed || surface->todoAddPressed
            || surface->pressedTodoCheckbox.has_value()) {
            if ((keyState & MK_LBUTTON) == 0) return endTodoPress(window, x, y);
            const auto viewHovered = surface->todoViewPressed
                && isTodoViewControlHit(*surface, x, y);
            const auto archiveHovered = surface->todoArchivePressed
                && isTodoArchiveControlHit(*surface, x, y);
            const auto remainingHovered = surface->todoRemainingPressed
                && isTodoRemainingControlHit(*surface, x, y);
            const auto addHovered = surface->todoAddPressed
                && isTodoAddControlHit(*surface, x, y);
            const auto checkboxHovered = surface->pressedTodoCheckbox.has_value()
                && pointInside(todoCheckboxRect(*surface, *surface->pressedTodoCheckbox), x, y);
            if (viewHovered != surface->todoViewHovered
                || archiveHovered != surface->todoArchiveHovered
                || remainingHovered != surface->todoRemainingHovered
                || addHovered != surface->todoAddHovered
                || (surface->pressedTodoCheckbox.has_value()
                    && checkboxHovered != (surface->hoveredTodoRow == surface->pressedTodoCheckbox))) {
                surface->todoViewHovered = viewHovered;
                surface->todoArchiveHovered = archiveHovered;
                surface->todoRemainingHovered = remainingHovered;
                surface->todoAddHovered = addHovered;
                surface->hoveredTodoRow = checkboxHovered
                    ? surface->pressedTodoCheckbox : std::optional<std::size_t>{};
                try { render(*surface, surface->display, surface->card, surface->ordinal); } catch (...) {}
            }
            return true;
        }
        if (!surface->pressedTodoRow.has_value()) return false;
        if ((keyState & MK_LBUTTON) == 0) return endTodoPress(window, x, y);
        if (!surface->todoDragTarget.has_value()) {
            const auto thresholdX = GetSystemMetrics(SM_CXDRAG);
            const auto thresholdY = GetSystemMetrics(SM_CYDRAG);
            if (std::abs(x - surface->todoDragStart.x) < thresholdX
                && std::abs(y - surface->todoDragStart.y) < thresholdY) return true;
        }
        auto target = todoRowAt(*surface, x, y);
        if (!target.has_value()) {
            const auto entries = todoDisplayEntries(*surface);
            std::vector<std::size_t> visibleItems;
            for (const auto& entry : entries) {
                if (!entry.header) visibleItems.push_back(entry.itemIndex);
            }
            if (!visibleItems.empty()) {
                target = y < todoRowRect(*surface, visibleItems.front()).top
                    ? visibleItems.front() : visibleItems.back();
            }
        }
        if (target != surface->todoDragTarget) {
            surface->todoDragTarget = target;
            try { render(*surface, surface->display, surface->card, surface->ordinal); } catch (...) {}
        }
        return true;
    }

    bool endTodoPress(HWND window, int x, int y) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr) return false;
        if (surface->todoViewPressed || surface->todoArchivePressed
            || surface->todoRemainingPressed || surface->todoAddPressed
            || surface->pressedTodoCheckbox.has_value()) {
            const auto commitView = surface->todoViewPressed
                && isTodoViewControlHit(*surface, x, y);
            const auto commitArchive = surface->todoArchivePressed
                && isTodoArchiveControlHit(*surface, x, y);
            const auto commitRemaining = surface->todoRemainingPressed
                && isTodoRemainingControlHit(*surface, x, y);
            const auto commitAdd = surface->todoAddPressed
                && isTodoAddControlHit(*surface, x, y);
            const auto checkbox = surface->pressedTodoCheckbox;
            const auto commitCheckbox = checkbox.has_value()
                && pointInside(todoCheckboxRect(*surface, *checkbox), x, y);
            surface->todoViewPressed = false;
            surface->todoArchivePressed = false;
            surface->todoRemainingPressed = false;
            surface->todoAddPressed = false;
            surface->pressedTodoCheckbox.reset();
            if (GetCapture() == window) ReleaseCapture();
            if (commitView) {
                surface->todoAddDateOffset = surface->todoAddDateOffset == 0 ? 1 : 0;
                resizeSurfaceForContent(*surface, true);
            } else if (commitArchive) {
                if (todoItemsArchived && todoItemsArchived(surface->card.id)) {
                    for (auto& item : surface->card.todoItems) {
                        if (item.completed) item.archived = true;
                    }
                    resizeSurfaceForContent(*surface, true);
                }
            } else if (commitRemaining) {
                surface->todoRemainingOnly = !surface->todoRemainingOnly;
                resizeSurfaceForContent(*surface, true);
            } else if (commitCheckbox && *checkbox < surface->card.todoItems.size()) {
                const auto& item = surface->card.todoItems[*checkbox];
                if (todoItemCompletedChanged
                    && todoItemCompletedChanged(surface->card.id, item.id, !item.completed)) {
                    surface->card.todoItems[*checkbox].completed = !item.completed;
                    resizeSurfaceForContent(*surface, true);
                }
            }
            try { render(*surface, surface->display, surface->card, surface->ordinal); } catch (...) {}
            if (commitAdd) beginTodoEdit(window, std::nullopt);
            return true;
        }
        if (!surface->pressedTodoRow.has_value()) return false;
        const auto source = *surface->pressedTodoRow;
        auto target = surface->todoDragTarget;
        surface->pressedTodoRow.reset();
        surface->todoDragTarget.reset();
        if (GetCapture() == window) ReleaseCapture();
        if (target.has_value() && source != *target && *target < surface->card.todoItems.size()) {
            auto reordered = surface->card.todoItems;
            const auto moved = reordered[source];
            reordered.erase(reordered.begin() + static_cast<std::ptrdiff_t>(source));
            const auto destination = std::min(*target, reordered.size());
            reordered.insert(reordered.begin() + static_cast<std::ptrdiff_t>(destination), moved);
            std::vector<std::string> order;
            order.reserve(reordered.size());
            for (const auto& item : reordered) order.push_back(item.id);
            if (todoItemsReordered && todoItemsReordered(surface->card.id, order)) {
                surface->card.todoItems = std::move(reordered);
            }
        }
        else if (!target.has_value()) {
            beginTodoEdit(window, source);
        }
        try { render(*surface, surface->display, surface->card, surface->ordinal); } catch (...) {}
        return true;
    }

    void cancelTodoPress(HWND window) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr) return;
        const auto changed = surface->todoViewPressed || surface->todoArchivePressed
            || surface->todoRemainingPressed || surface->todoAddPressed
            || surface->pressedTodoCheckbox.has_value()
            || surface->pressedTodoRow.has_value();
        surface->todoViewPressed = false;
        surface->todoArchivePressed = false;
        surface->todoRemainingPressed = false;
        surface->todoAddPressed = false;
        surface->pressedTodoCheckbox.reset();
        surface->pressedTodoRow.reset();
        surface->todoDragTarget.reset();
        if (changed) {
            try { render(*surface, surface->display, surface->card, surface->ordinal); } catch (...) {}
        }
    }

    bool removeTodoAt(HWND window, int x, int y) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr || surface->card.type != domain::CardType::Todo) return false;
        const auto row = todoRowAt(*surface, x, y);
        if (!row.has_value() || !todoItemRemoved) return false;
        const auto itemId = surface->card.todoItems[*row].id;
        HMENU menu = CreatePopupMenu();
        if (menu == nullptr) return true;
        AppendMenuW(menu, MF_STRING, 1, L"删除");
        POINT point{x, y};
        ClientToScreen(window, &point);
        SetForegroundWindow(window);
        const auto command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, point.x, point.y, 0, window, nullptr);
        DestroyMenu(menu);
        if (command == 1 && todoItemRemoved(surface->card.id, itemId)) {
            surface->card.todoItems.erase(surface->card.todoItems.begin() + static_cast<std::ptrdiff_t>(*row));
            repaintTodoCard(surface->card.id);
        }
        return true;
    }

    bool beginCollapsePress(HWND window, int x, int y) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr || !isCollapseControlHit(*surface, x, y)) {
            return false;
        }
        surface->collapseHovered = true;
        surface->collapsePressed = true;
        SetCapture(window);
        try {
            render(*surface, surface->display, surface->card, surface->ordinal);
        } catch (...) {
        }
        return true;
    }

    bool endCollapsePress(HWND window, int x, int y) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr || !surface->collapsePressed) {
            return false;
        }
        const auto commit = isCollapseControlHit(*surface, x, y);
        surface->collapseHovered = commit;
        surface->collapsePressed = false;
        if (GetCapture() == window) {
            ReleaseCapture();
        }
        const auto cardId = surface->card.id;
        const auto expanded = commit ? !surface->card.expanded : surface->card.expanded;
        if (commit) {
            for (auto& candidate : surfaces) {
                if (candidate.card.id != cardId) continue;
                candidate.card.expanded = expanded;
                try {
                    render(candidate, candidate.display, candidate.card, candidate.ordinal);
                } catch (...) {
                }
            }
            for (auto& candidate : surfaces) {
                if (candidate.card.id == cardId) reflowVerticalFollowers(candidate, true);
            }
        } else {
            try {
                render(*surface, surface->display, surface->card, surface->ordinal);
            } catch (...) {
            }
        }
        if (commit && cardExpandedChanged) {
            try {
                cardExpandedChanged(cardId, expanded);
            } catch (...) {
                // Native window procedures must not allow callback exceptions to escape.
            }
        }
        return true;
    }

    void cancelCollapsePress(HWND window) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr || !surface->collapsePressed) {
            return;
        }
        surface->collapsePressed = false;
        try {
            render(*surface, surface->display, surface->card, surface->ordinal);
        } catch (...) {
        }
    }

    bool beginItemPress(HWND window, int x, int y) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr || surface->itemDragActive) {
            return false;
        }
        const auto item = itemAt(*surface, x, y);
        if (!item.has_value()) {
            return false;
        }
        surface->pressedItem = item;
        surface->itemDragStart = {x, y};
        SetCapture(window);
        return true;
    }

    bool endItemPress(HWND window) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr || !surface->pressedItem.has_value()) {
            return false;
        }
        surface->pressedItem.reset();
        if (GetCapture() == window) {
            ReleaseCapture();
        }
        return true;
    }

    void cancelItemPress(HWND window) noexcept {
        auto* surface = findSurface(window);
        if (surface != nullptr && !surface->itemDragActive) {
            surface->pressedItem.reset();
        }
    }

    bool updateItemDrag(HWND window, int x, int y, WPARAM keyState) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr || surface->itemDragActive
            || !surface->pressedItem.has_value()) {
            return false;
        }
        if (surface->card.type == domain::CardType::Mapping
            && (surface->card.mappingMode != domain::MappingMode::Folder
                || !surface->card.mappingAllowsSourceMutation)) {
            return false;
        }
        if ((keyState & MK_LBUTTON) == 0) {
            return endItemPress(window);
        }
        const auto thresholdX = GetSystemMetrics(SM_CXDRAG);
        const auto thresholdY = GetSystemMetrics(SM_CYDRAG);
        if (std::abs(x - surface->itemDragStart.x) < thresholdX
            && std::abs(y - surface->itemDragStart.y) < thresholdY) {
            return false;
        }

        const auto item = surface->card.items[*surface->pressedItem];
        const auto cardId = surface->card.id;
        surface->pressedItem.reset();
        surface->itemDragActive = true;
        clearPointerHover(window);
        if (GetCapture() == window) {
            ReleaseCapture();
        }
        const auto result = BeginFileDrag({item.sourcePath}, cardId);
        if (auto* current = findSurface(window); current != nullptr) {
            current->itemDragActive = false;
        }
        if (result.status == DRAGDROP_S_DROP && result.effect != DROPEFFECT_NONE
            && !result.completedInsideDesto
            && applicationItemDragCompleted) {
            try {
                applicationItemDragCompleted(cardId);
            } catch (...) {
            }
        }
        return true;
    }

    bool activateCardItem(HWND window, int x, int y) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr) {
            return false;
        }
        const auto index = itemAt(*surface, x, y);
        if (!index.has_value()) {
            return false;
        }
        const auto& item = surface->card.items[*index];
        if (item.state == presentation::CardItemState::Missing
            || item.state == presentation::CardItemState::UnresolvedShortcut) {
            return true;
        }
        if (cardItemActivated) {
            try {
                cardItemActivated(surface->card.id, item);
            } catch (...) {
            }
        }
        return true;
    }

    void clearPointerHover(HWND window, bool repaint = true) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr) {
            return;
        }
        KillTimer(window, ItemTooltipTimerId);
        if (surface->tooltip != nullptr) {
            ShowWindow(surface->tooltip, SW_HIDE);
        }
        const auto changed = surface->hoveredItem.has_value()
            || surface->hoveredTodoRow.has_value() || surface->collapseHovered
            || surface->todoAddHovered || surface->todoViewHovered
            || surface->todoArchiveHovered || surface->todoRemainingHovered;
        surface->hoveredItem.reset();
        surface->hoveredTodoRow.reset();
        surface->collapseHovered = false;
        surface->todoAddHovered = false;
        surface->todoViewHovered = false;
        surface->todoArchiveHovered = false;
        surface->todoRemainingHovered = false;
        if (changed && repaint) {
            try {
                render(*surface, surface->display, surface->card, surface->ordinal);
            } catch (...) {
            }
        }
    }

    void showItemTooltip(HWND window) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr || surface->tooltip == nullptr
            || (!surface->hoveredItem.has_value() && !surface->hoveredTodoRow.has_value())) {
            return;
        }
        KillTimer(window, ItemTooltipTimerId);
        SetWindowTextW(surface->tooltip, surface->tooltipText.c_str());
        const auto dc = GetDC(surface->tooltip);
        if (dc == nullptr) return;
        const auto font = CreateFontW(
            -dipToPixels(13.0, *surface), 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI Variable Text");
        const auto previousFont = font == nullptr ? nullptr : SelectObject(dc, font);
        RECT measured{0, 0, dipToPixels(260.0, *surface), 0};
        DrawTextW(dc, surface->tooltipText.c_str(), -1, &measured,
            DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        if (previousFont != nullptr) SelectObject(dc, previousFont);
        if (font != nullptr) DeleteObject(font);
        ReleaseDC(surface->tooltip, dc);
        const auto width = std::max<LONG>(
            dipToPixels(48.0, *surface), measured.right + dipToPixels(20.0, *surface));
        const auto height = std::max<LONG>(
            dipToPixels(30.0, *surface), measured.bottom + dipToPixels(14.0, *surface));
        POINT cursor{};
        GetCursorPos(&cursor);
        auto left = cursor.x + dipToPixels(12.0, *surface);
        auto top = cursor.y + dipToPixels(18.0, *surface);
        MONITORINFO monitorInfo{.cbSize = sizeof(MONITORINFO)};
        if (GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitorInfo)) {
            left = std::clamp(left, monitorInfo.rcWork.left, monitorInfo.rcWork.right - width);
            top = std::clamp(top, monitorInfo.rcWork.top, monitorInfo.rcWork.bottom - height);
        }
        SetWindowRgn(
            surface->tooltip,
            CreateRoundRectRgn(0, 0, width + 1, height + 1,
                dipToPixels(10.0, *surface), dipToPixels(10.0, *surface)),
            FALSE);
        SetWindowPos(
            surface->tooltip, HWND_TOPMOST, left, top, width, height,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(surface->tooltip, nullptr, FALSE);
    }

    void updatePointerHover(HWND window, int x, int y) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr) {
            return;
        }
        TRACKMOUSEEVENT tracking{
            .cbSize = sizeof(TRACKMOUSEEVENT),
            .dwFlags = TME_LEAVE,
            .hwndTrack = window,
        };
        TrackMouseEvent(&tracking);
        const auto collapseHovered = isCollapseControlHit(*surface, x, y);
        const auto todoViewHovered = !collapseHovered && isTodoViewControlHit(*surface, x, y);
        const auto todoArchiveHovered = !collapseHovered && !todoViewHovered
            && isTodoArchiveControlHit(*surface, x, y);
        const auto todoRemainingHovered = !collapseHovered && !todoViewHovered
            && !todoArchiveHovered && isTodoRemainingControlHit(*surface, x, y);
        const auto todoAddHovered = !collapseHovered && !todoViewHovered
            && !todoArchiveHovered && !todoRemainingHovered
            && isTodoAddControlHit(*surface, x, y);
        const auto todoRow = collapseHovered || todoViewHovered
            || todoArchiveHovered || todoRemainingHovered || todoAddHovered
            ? std::optional<std::size_t>{}
            : todoRowAt(*surface, x, y);
        const auto index = collapseHovered || todoViewHovered
            || todoArchiveHovered || todoRemainingHovered || todoAddHovered
            || todoRow.has_value()
            ? std::optional<std::size_t>{}
            : itemAt(*surface, x, y);
        if (index == surface->hoveredItem && todoRow == surface->hoveredTodoRow
            && collapseHovered == surface->collapseHovered
            && todoAddHovered == surface->todoAddHovered
            && todoViewHovered == surface->todoViewHovered
            && todoArchiveHovered == surface->todoArchiveHovered
            && todoRemainingHovered == surface->todoRemainingHovered) {
            return;
        }
        clearPointerHover(window, false);
        surface->collapseHovered = collapseHovered;
        surface->todoViewHovered = todoViewHovered;
        surface->todoArchiveHovered = todoArchiveHovered;
        surface->todoRemainingHovered = todoRemainingHovered;
        surface->todoAddHovered = todoAddHovered;
        surface->hoveredTodoRow = todoRow;
        if (todoRow.has_value()) {
            try { render(*surface, surface->display, surface->card, surface->ordinal); } catch (...) {}
            return;
        }
        if (!index.has_value() || surface->tooltip == nullptr) {
            try {
                render(*surface, surface->display, surface->card, surface->ordinal);
            } catch (...) {
            }
            return;
        }
        surface->hoveredItem = index;
        const auto& item = surface->card.items[*index];
        surface->tooltipText = item.displayName;
        if (item.state == presentation::CardItemState::Missing) {
            surface->tooltipText += L"\n(Item is missing)";
        } else if (item.state == presentation::CardItemState::UnresolvedShortcut) {
            surface->tooltipText += L"\n(Shortcut is unavailable)";
        }
        SetTimer(window, ItemTooltipTimerId, ItemTooltipDelayMilliseconds, nullptr);
        try {
            render(*surface, surface->display, surface->card, surface->ordinal);
        } catch (...) {
        }
    }

    DWORD updateDropPreview(
        HWND window,
        POINTL screenPoint,
        DWORD allowedEffect,
        const std::optional<std::string>& sourceCardId = std::nullopt) noexcept {
        auto* surface = findSurface(window);
        KillTimer(window, DropPreviewResetTimerId);
        if (surface == nullptr || !surface->card.expanded
            || acceptedDropEffect(*surface, allowedEffect) == DROPEFFECT_NONE) {
            clearDropPreview(window);
            return DROPEFFECT_NONE;
        }
        POINT point{screenPoint.x, screenPoint.y};
        if (!ScreenToClient(window, &point)
            || point.y < dipToPixels(48.0, *surface)
            || point.y >= surface->interactiveHeight) {
            clearDropPreview(window);
            return DROPEFFECT_NONE;
        }
        const auto scale = surface->display.effectiveDpi / 96.0;
        const auto widthDip = surface->width / scale;
        const auto pointerXDip = point.x / scale;
        const auto pointerYDip = point.y / scale;
        std::optional<std::size_t> insertion;
        std::optional<std::size_t> previewColumns;
        if (surface->card.content.sizeMode == domain::CardSizeMode::Fixed) {
            insertion = presentation::ResolveCardSlotIndex(
                widthDip,
                pointerXDip,
                pointerYDip,
                baseItemLayoutSettings(*surface),
                surface->card.content.fixedRows);
        } else {
            const auto settings = baseItemLayoutSettings(*surface);
            const auto previousPreview = surface->dropInsertionIndex.has_value()
                ? std::optional<presentation::CardDropPreview>({
                    *surface->dropInsertionIndex,
                    surface->dropPreviewColumns.value_or(settings.preferredColumns),
                })
                : std::nullopt;
            const auto expandedPreview = presentation::ResolveAdaptiveCardDropPreview(
                contentSlotCount(*surface, false),
                widthDip,
                pointerXDip,
                pointerYDip,
                settings,
                previousPreview);
            const auto conservativePreview = presentation::ResolveAdaptiveCardDropPreview(
                contentSlotCount(*surface, false),
                widthDip,
                pointerXDip,
                pointerYDip,
                settings,
                previousPreview,
                false);
            const auto origin = !sourceCardId.has_value()
                ? presentation::CardDropOrigin::External
                : *sourceCardId == surface->card.id
                    ? presentation::CardDropOrigin::SameCard
                    : presentation::CardDropOrigin::OtherCard;
            const auto expansionRequested = expandedPreview.insertionIndex
                    != conservativePreview.insertionIndex
                || expandedPreview.columns != conservativePreview.columns;
            auto preview = expandedPreview;
            if (expansionRequested && origin == presentation::CardDropOrigin::SameCard) {
                if (!surface->pendingDropExpansion.has_value()
                    || surface->pendingDropExpansion->insertionIndex
                        != expandedPreview.insertionIndex
                    || surface->pendingDropExpansion->columns != expandedPreview.columns) {
                    surface->pendingDropExpansion = expandedPreview;
                    surface->dropExpansionStartedAt = GetTickCount64();
                }
                const auto elapsed = GetTickCount64() - surface->dropExpansionStartedAt;
                if (!presentation::IsAdaptiveDropExpansionReady(origin, elapsed)) {
                    preview = conservativePreview;
                }
            } else {
                surface->pendingDropExpansion.reset();
                surface->dropExpansionStartedAt = 0;
            }
            insertion = preview.insertionIndex;
            previewColumns = preview.columns;
        }
        if (!insertion.has_value()) {
            clearDropPreview(window);
            return DROPEFFECT_NONE;
        }
        if (surface->dropInsertionIndex != *insertion
            || surface->dropPreviewColumns != previewColumns) {
            surface->dropInsertionIndex = *insertion;
            surface->dropPreviewColumns = previewColumns;
            try {
                resizeSurfaceForContent(*surface, true);
                render(*surface, surface->display, surface->card, surface->ordinal);
            } catch (...) {
                surface->dropInsertionIndex.reset();
                surface->dropPreviewColumns.reset();
                return DROPEFFECT_NONE;
            }
        }
        return acceptedDropEffect(*surface, allowedEffect);
    }

    void clearDropPreview(HWND window) noexcept {
        auto* surface = findSurface(window);
        KillTimer(window, DropPreviewResetTimerId);
        if (surface == nullptr || !surface->dropInsertionIndex.has_value()) {
            return;
        }
        surface->dropInsertionIndex.reset();
        surface->dropPreviewColumns.reset();
        surface->pendingDropExpansion.reset();
        surface->dropExpansionStartedAt = 0;
        try {
            resizeSurfaceForContent(*surface, true);
            render(*surface, surface->display, surface->card, surface->ordinal);
        } catch (...) {
        }
    }

    void scheduleDropPreviewClear(HWND window) noexcept {
        if (findSurface(window) != nullptr) {
            SetTimer(
                window,
                DropPreviewResetTimerId,
                DropPreviewResetDelayMilliseconds,
                nullptr);
        }
    }

    DWORD completeFileDrop(
        HWND window,
        std::vector<std::filesystem::path> paths,
        std::optional<std::string> sourceCardId,
        POINTL screenPoint,
        DWORD allowedEffect) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr || paths.empty()
            || updateDropPreview(window, screenPoint, allowedEffect, sourceCardId)
                == DROPEFFECT_NONE) {
            clearDropPreview(window);
            return DROPEFFECT_NONE;
        }
        const auto cardId = surface->card.id;
        const auto insertion = surface->dropInsertionIndex.value_or(surface->card.items.size());
        const auto layoutColumns = itemLayout(*surface, 0).columns;
        KillTimer(window, DropPreviewResetTimerId);
        surface->dropInsertionIndex.reset();
        surface->dropPreviewColumns.reset();
        if (!applicationItemsDropped) {
            try {
                resizeSurfaceForContent(*surface, true);
                render(*surface, surface->display, surface->card, surface->ordinal);
            } catch (...) {
            }
            return DROPEFFECT_NONE;
        }
        try {
            if (applicationItemsDropped(
                    cardId, paths, sourceCardId, insertion, layoutColumns)) {
                return acceptedDropEffect(*surface, allowedEffect);
            }
        } catch (...) {
        }
        try {
            resizeSurfaceForContent(*surface, true);
            render(*surface, surface->display, surface->card, surface->ordinal);
        } catch (...) {
        }
        return DROPEFFECT_NONE;
    }

    void destroyBitmap(Surface& surface) noexcept {
        if (surface.previousBitmap != nullptr && surface.memoryDc != nullptr) {
            SelectObject(surface.memoryDc, surface.previousBitmap);
            surface.previousBitmap = nullptr;
        }
        if (surface.bitmap != nullptr) {
            DeleteObject(surface.bitmap);
            surface.bitmap = nullptr;
        }
        surface.pixels = nullptr;
        if (surface.memoryDc != nullptr) {
            DeleteDC(surface.memoryDc);
            surface.memoryDc = nullptr;
        }
    }

    void destroySurface(Surface& surface) noexcept {
        if (surface.dropTarget != nullptr) {
            if (surface.window != nullptr) {
                RevokeDragDrop(surface.window);
            }
            surface.dropTarget->Release();
            surface.dropTarget = nullptr;
        }
        if (surface.tooltip != nullptr) {
            DestroyWindow(surface.tooltip);
            surface.tooltip = nullptr;
        }
        if (surface.window != nullptr) {
            DestroyWindow(surface.window);
            surface.window = nullptr;
        }
        destroyBitmap(surface);
    }

    void destroySurfaces() noexcept {
        for (auto& surface : surfaces) {
            destroySurface(surface);
        }
        surfaces.clear();
    }

    void replaceBitmap(Surface& surface, int width, int height) {
        auto memoryDc = CreateCompatibleDC(nullptr);
        if (memoryDc == nullptr) {
            throw std::runtime_error("CreateCompatibleDC failed for desktop host.");
        }
        BITMAPINFO bitmapInfo{};
        bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmapInfo.bmiHeader.biWidth = width;
        bitmapInfo.bmiHeader.biHeight = -height;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        auto bitmap = CreateDIBSection(
            memoryDc, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bitmap == nullptr) {
            DeleteDC(memoryDc);
            throw std::runtime_error("CreateDIBSection failed for desktop host.");
        }
        const auto previousBitmap = static_cast<HBITMAP>(SelectObject(memoryDc, bitmap));
        if (previousBitmap == nullptr || previousBitmap == HGDI_ERROR) {
            DeleteObject(bitmap);
            DeleteDC(memoryDc);
            throw std::runtime_error("SelectObject failed for desktop host bitmap.");
        }

        destroyBitmap(surface);
        surface.memoryDc = memoryDc;
        surface.bitmap = bitmap;
        surface.previousBitmap = previousBitmap;
        surface.pixels = static_cast<std::uint32_t*>(bits);
        surface.width = width;
        surface.height = height;
    }

    void createBitmap(Surface& surface) {
        replaceBitmap(surface, surface.width, surface.height);
    }

    void resizeSurfaceForContent(Surface& surface, bool allocateBitmap) {
        const auto rect = contentDrivenRect(surface);
        const auto scale = surface.display.effectiveDpi / 96.0;
        const auto width = std::max(1, static_cast<int>(std::lround(rect.width * scale)));
        const auto height = std::max(1, static_cast<int>(std::lround(rect.height * scale)));
        surface.projection.rect = rect;
        if (width == surface.width && height == surface.height) return;
        if (allocateBitmap) {
            replaceBitmap(surface, width, height);
        } else {
            surface.width = width;
            surface.height = height;
        }
    }

    void createSurface(Surface& surface) {
        createBitmap(surface);
        surface.window = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            className.c_str(),
            title.c_str(),
            WS_POPUP,
            0,
            0,
            surface.width,
            surface.height,
            nullptr,
            nullptr,
            module,
            this);
        if (surface.window == nullptr) {
            throw std::runtime_error("CreateWindowExW failed for desktop host surface.");
        }
        if (surface.card.type == domain::CardType::Application
            || (surface.card.type == domain::CardType::Mapping
                && (surface.card.mappingMode != domain::MappingMode::Folder
                    || surface.card.mappingAllowsSourceMutation))) {
            surface.dropTarget = CreateFileDropTarget({
                .dragOver = [this, window = surface.window](
                                POINTL point,
                                DWORD allowed,
                                const std::optional<std::string>& sourceCardId) {
                    return updateDropPreview(window, point, allowed, sourceCardId);
                },
                .dragLeave = [this, window = surface.window]() {
                    scheduleDropPreviewClear(window);
                },
                .drop = [this, window = surface.window](
                            std::vector<std::filesystem::path> paths,
                            std::optional<std::string> sourceCardId,
                            POINTL point,
                            DWORD allowed) {
                    return completeFileDrop(
                        window, std::move(paths), std::move(sourceCardId), point, allowed);
                },
            });
            if (surface.dropTarget == nullptr
                || FAILED(RegisterDragDrop(surface.window, surface.dropTarget))) {
                throw std::runtime_error("RegisterDragDrop failed for file Card.");
            }
        }
        surface.tooltip = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            tooltipClassName.c_str(),
            L"",
            WS_POPUP,
            0,
            0,
            1,
            1,
            surface.window,
            nullptr,
            module,
            nullptr);
        if (surface.tooltip == nullptr) {
            throw std::runtime_error("CreateWindowExW failed for Card item tooltip.");
        }
    }

    void render(
        Surface& surface,
        const domain::DisplaySnapshot& display,
        const presentation::CardView& card,
        std::size_t ordinal,
        bool commit = true) {
        (void)ordinal;
        const auto darkSurface = card.appearancePreset == "black"
            || card.appearancePreset == "dark";
        const auto pearlSurface = card.appearancePreset == "pearl-pink"
            || card.appearancePreset == "jewel";
        const auto visibleBottom = card.expanded
            ? surface.height
            : std::min(surface.height, dipToPixels(48.0, surface));
        surface.interactiveHeight = visibleBottom;
        const auto radius = std::min(
            static_cast<int>(std::lround(card.cornerRadius * display.effectiveDpi / 96.0)),
            std::min(surface.width, visibleBottom) / 2);

        for (int y = 0; y < surface.height; ++y) {
            for (int x = 0; x < surface.width; ++x) {
                std::uint32_t red = darkSurface ? 31u : 248u;
                std::uint32_t green = darkSurface ? 33u : 250u;
                std::uint32_t blue = darkSurface ? 38u : 252u;
                if (pearlSurface) {
                    const auto horizontal = surface.width <= 1
                        ? 0.0
                        : static_cast<double>(x) / (surface.width - 1);
                    const auto vertical = visibleBottom <= 1
                        ? 0.0
                        : static_cast<double>(std::min(y, visibleBottom - 1))
                            / (visibleBottom - 1);
                    const auto amethyst = std::exp(-(
                        std::pow(horizontal - 0.12, 2.0) / 0.11
                        + std::pow(vertical - 0.15, 2.0) / 0.32));
                    const auto aquamarine = std::exp(-(
                        std::pow(horizontal - 0.88, 2.0) / 0.16
                        + std::pow(vertical - 0.28, 2.0) / 0.24));
                    const auto tourmaline = std::exp(-(
                        std::pow(horizontal - 0.22, 2.0) / 0.18
                        + std::pow(vertical - 0.88, 2.0) / 0.20));
                    const auto amber = std::exp(-(
                        std::pow(horizontal - 0.78, 2.0) / 0.22
                        + std::pow(vertical - 0.82, 2.0) / 0.18));
                    const auto diagonalSheen = std::exp(-std::pow(
                        horizontal * 0.82 + vertical * 0.58 - 0.72,
                        2.0) / 0.012);
                    const auto channel = [&](double base, double a, double q, double t, double g) {
                        return static_cast<std::uint32_t>(std::lround(std::clamp(
                            base + a * amethyst + q * aquamarine
                                + t * tourmaline + g * amber
                                + 13.0 * diagonalSheen,
                            0.0,
                            255.0)));
                    };
                    red = channel(226.0, 7.0, -28.0, 23.0, 24.0);
                    green = channel(225.0, -28.0, 21.0, -22.0, 12.0);
                    blue = channel(236.0, 17.0, 14.0, 10.0, -29.0);
                }
                surface.pixels[y * surface.width + x] = (red << 16) | (green << 8) | blue;
            }
        }

        for (int y = 0; y < surface.height; ++y) {
            if (y >= visibleBottom) {
                std::fill(
                    surface.pixels + y * surface.width,
                    surface.pixels + (y + 1) * surface.width,
                    0u);
            }
        }

        std::wstring titleText = card.title;
        if (card.type == domain::CardType::Todo) {
            titleText = L"待办";
        }
        const auto text = card.showTitle ? titleText : L"";
        SetBkMode(surface.memoryDc, TRANSPARENT);
        const auto foreground = darkSurface ? RGB(244, 246, 249) : RGB(38, 40, 45);
        SetTextColor(surface.memoryDc, foreground);
        const auto font = CreateFontW(
            -dipToPixels(14.0, surface),
            0,
            0,
            0,
            FW_SEMIBOLD,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI Variable Text");
        const auto previousFont = font == nullptr
            ? nullptr
            : SelectObject(surface.memoryDc, font);
        const auto textLeft = dipToPixels(14.0, surface);
        const auto control = collapseControlRect(surface);
        RECT textRect{
            textLeft,
            0,
            card.showCollapseControl ? control.left - dipToPixels(4.0, surface)
                                     : surface.width - textLeft,
            std::min(visibleBottom, dipToPixels(48.0, surface)),
        };
        DrawTextW(
            surface.memoryDc,
            text.c_str(),
            -1,
            &textRect,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (previousFont != nullptr) {
            SelectObject(surface.memoryDc, previousFont);
        }
        if (font != nullptr) {
            DeleteObject(font);
        }

        const auto blendRgb = [&](int x, int y, std::uint32_t color, double coverage) {
            if (x < 0 || y < 0 || x >= surface.width || y >= visibleBottom || coverage <= 0.0) {
                return;
            }
            auto& pixel = surface.pixels[y * surface.width + x];
            const auto amount = std::clamp(coverage, 0.0, 1.0);
            const auto mix = [&](int shift) {
                const auto background = static_cast<double>((pixel >> shift) & 0xFFu);
                const auto foregroundChannel = static_cast<double>((color >> shift) & 0xFFu);
                return static_cast<std::uint32_t>(std::lround(
                    background + (foregroundChannel - background) * amount));
            };
            pixel = (mix(16) << 16) | (mix(8) << 8) | mix(0);
        };
        const auto drawDashedRoundedBorder = [&] (
            const RECT& rect,
            double radius,
            std::uint32_t color,
            double opacity = 1.0) {
            const presentation::RoundedDashSpec spec{
                .width = static_cast<double>(rect.right - rect.left),
                .height = static_cast<double>(rect.bottom - rect.top),
                .radius = radius,
                .strokeWidth = static_cast<double>(dipToPixels(2.0, surface)),
                .nominalPeriod = static_cast<double>(dipToPixels(9.0, surface)),
            };
            for (int y = rect.top; y < rect.bottom; ++y) {
                for (int x = rect.left; x < rect.right; ++x) {
                    const auto coverage = presentation::SampleRoundedDashCoverage(
                        spec,
                        x - rect.left + 0.5,
                        y - rect.top + 0.5);
                    blendRgb(x, y, color, coverage * opacity);
                }
            }
        };
        const auto drawRoundedFill = [&] (
            const RECT& rect,
            double radius,
            std::uint32_t color,
            double opacity) {
            const auto centerX = (rect.left + rect.right) / 2.0;
            const auto centerY = (rect.top + rect.bottom) / 2.0;
            const auto halfWidth = (rect.right - rect.left) / 2.0;
            const auto halfHeight = (rect.bottom - rect.top) / 2.0;
            const auto clampedRadius = std::min(radius, std::min(halfWidth, halfHeight));
            for (int y = rect.top; y < rect.bottom; ++y) {
                for (int x = rect.left; x < rect.right; ++x) {
                    const auto qx = std::abs(x + 0.5 - centerX)
                        - (halfWidth - clampedRadius);
                    const auto qy = std::abs(y + 0.5 - centerY)
                        - (halfHeight - clampedRadius);
                    const auto outsideX = std::max(qx, 0.0);
                    const auto outsideY = std::max(qy, 0.0);
                    const auto distance = std::sqrt(
                        outsideX * outsideX + outsideY * outsideY)
                        + std::min(std::max(qx, qy), 0.0) - clampedRadius;
                    blendRgb(x, y, color, std::clamp(0.5 - distance, 0.0, 1.0) * opacity);
                }
            }
        };
        if (card.expanded && card.type == domain::CardType::Todo) {
            const auto entries = todoDisplayEntries(surface);
            const auto remaining = std::ranges::count_if(entries, [&](const auto& entry) {
                return !entry.header && !card.todoItems[entry.itemIndex].completed;
            });
            const auto actionFont = CreateFontW(
                -dipToPixels(12.0, surface), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
            const auto previousActionFont = actionFont == nullptr
                ? nullptr : SelectObject(surface.memoryDc, actionFont);
            auto viewRect = todoViewControlRect(surface);
            if (surface.todoViewHovered || surface.todoViewPressed) {
                drawRoundedFill(
                    viewRect,
                    dipToPixels(9.0, surface),
                    darkSurface ? 0x00FFFFFFu : 0x0018212Fu,
                    surface.todoViewPressed ? (darkSurface ? 0.14 : 0.10)
                                            : (darkSurface ? 0.08 : 0.055));
            }
            SetTextColor(surface.memoryDc, darkSurface ? RGB(232, 234, 239) : RGB(45, 49, 57));
            DrawTextW(surface.memoryDc,
                surface.todoAddDateOffset == 0 ? L"今天" : L"明天", -1, &viewRect,
                DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            auto archiveRect = todoArchiveControlRect(surface);
            const auto hasCompleted = std::ranges::any_of(entries, [&](const auto& entry) {
                return !entry.header && card.todoItems[entry.itemIndex].completed;
            });
            if ((surface.todoArchiveHovered || surface.todoArchivePressed) && hasCompleted) {
                drawRoundedFill(
                    archiveRect,
                    dipToPixels(9.0, surface),
                    darkSurface ? 0x00FFFFFFu : 0x0018212Fu,
                    surface.todoArchivePressed ? (darkSurface ? 0.14 : 0.10)
                                               : (darkSurface ? 0.08 : 0.055));
            }
            SetTextColor(surface.memoryDc, hasCompleted
                ? (darkSurface ? RGB(180, 186, 198) : RGB(126, 132, 143))
                : (darkSurface ? RGB(112, 118, 129) : RGB(181, 185, 192)));
            DrawTextW(surface.memoryDc, L"归档", -1, &archiveRect,
                DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            auto remainingRect = todoRemainingRect(surface);
            if (surface.todoRemainingOnly || surface.todoRemainingHovered
                || surface.todoRemainingPressed) {
                drawRoundedFill(
                    remainingRect,
                    dipToPixels(9.0, surface),
                    darkSurface ? 0x00FFFFFFu : 0x0018212Fu,
                    surface.todoRemainingPressed ? (darkSurface ? 0.16 : 0.11)
                        : surface.todoRemainingOnly ? (darkSurface ? 0.11 : 0.075)
                        : (darkSurface ? 0.08 : 0.05));
            }
            SetTextColor(surface.memoryDc,
                darkSurface ? RGB(182, 188, 199) : RGB(104, 110, 121));
            const auto remainingText = L"剩余 " + std::to_wstring(remaining);
            DrawTextW(surface.memoryDc, remainingText.c_str(), -1, &remainingRect,
                DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            if (previousActionFont != nullptr) SelectObject(surface.memoryDc, previousActionFont);
            if (actionFont != nullptr) DeleteObject(actionFont);

            const auto rowFill = darkSurface ? 0x00FFFFFFu : 0x00111827u;
            if (surface.hoveredTodoRow.has_value()
                && *surface.hoveredTodoRow < card.todoItems.size()) {
                const auto row = todoRowRect(surface, *surface.hoveredTodoRow);
                for (int y = row.top; y < row.bottom; ++y) {
                    for (int x = row.left; x < row.right; ++x) {
                        blendRgb(x, y, rowFill, darkSurface ? 0.08 : 0.05);
                    }
                }
            }
            const auto todoFont = CreateFontW(
                -dipToPixels(13.0, surface), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
            const auto previousTodoFont = todoFont == nullptr
                ? nullptr : SelectObject(surface.memoryDc, todoFont);
            const auto entryRects = todoEntryRects(surface, entries);
            for (std::size_t entryIndex = 0; entryIndex < entries.size(); ++entryIndex) {
                const auto& entry = entries[entryIndex];
                auto row = entryRects[entryIndex];
                if (entry.header) {
                    SetTextColor(surface.memoryDc, darkSurface ? RGB(170, 177, 190) : RGB(103, 109, 121));
                    DrawTextW(surface.memoryDc, TodoDateLabel(entry.date).c_str(), -1, &row,
                        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
                    continue;
                }
                const auto& item = card.todoItems[entry.itemIndex];
                const auto checkbox = todoCheckboxRect(surface, row);
                if (surface.pressedTodoCheckbox == entry.itemIndex
                    && surface.hoveredTodoRow == entry.itemIndex) {
                    auto checkboxHotspot = checkbox;
                    InflateRect(&checkboxHotspot,
                        dipToPixels(5.0, surface), dipToPixels(5.0, surface));
                    drawRoundedFill(
                        checkboxHotspot,
                        dipToPixels(9.0, surface),
                        darkSurface ? 0x00FFFFFFu : 0x0018212Fu,
                        darkSurface ? 0.13 : 0.08);
                }
                const auto checkboxColor = item.completed
                    ? (darkSurface ? 0x0087BEFFu : 0x003774DCu)
                    : (darkSurface ? 0x00BEC4CFu : 0x008E95A2u);
                const auto centerX = (checkbox.left + checkbox.right) / 2.0;
                const auto centerY = (checkbox.top + checkbox.bottom) / 2.0;
                const auto circleRadius = (checkbox.right - checkbox.left) / 2.0 - 1.0;
                const auto strokeWidth = std::max(1.0, 1.35 * display.effectiveDpi / 96.0);
                for (int y = checkbox.top; y < checkbox.bottom; ++y) {
                    for (int x = checkbox.left; x < checkbox.right; ++x) {
                        const auto dx = x + 0.5 - centerX;
                        const auto dy = y + 0.5 - centerY;
                        const auto distance = std::sqrt(dx * dx + dy * dy);
                        const auto coverage = item.completed
                            ? std::clamp(circleRadius + 0.5 - distance, 0.0, 1.0)
                            : std::clamp(strokeWidth / 2.0 + 0.5
                                - std::abs(distance - circleRadius), 0.0, 1.0);
                        blendRgb(x, y, checkboxColor, coverage);
                    }
                }
                if (item.completed) {
                    const auto pointDistance = [](double px, double py,
                                                  double x1, double y1,
                                                  double x2, double y2) {
                        const auto vx = x2 - x1;
                        const auto vy = y2 - y1;
                        const auto lengthSquared = vx * vx + vy * vy;
                        const auto position = lengthSquared <= 0.0 ? 0.0 : std::clamp(
                            ((px - x1) * vx + (py - y1) * vy) / lengthSquared, 0.0, 1.0);
                        const auto dx = px - (x1 + position * vx);
                        const auto dy = py - (y1 + position * vy);
                        return std::sqrt(dx * dx + dy * dy);
                    };
                    const auto x1 = checkbox.left + dipToPixels(4.5, surface);
                    const auto y1 = centerY;
                    const auto x2 = checkbox.left + dipToPixels(7.5, surface);
                    const auto y2 = checkbox.bottom - dipToPixels(4.5, surface);
                    const auto x3 = checkbox.right - dipToPixels(3.5, surface);
                    const auto y3 = checkbox.top + dipToPixels(4.5, surface);
                    for (int y = checkbox.top; y < checkbox.bottom; ++y) {
                        for (int x = checkbox.left; x < checkbox.right; ++x) {
                            const auto distance = std::min(
                                pointDistance(x + 0.5, y + 0.5, x1, y1, x2, y2),
                                pointDistance(x + 0.5, y + 0.5, x2, y2, x3, y3));
                            blendRgb(x, y, 0x00FFFFFFu,
                                std::clamp(strokeWidth + 0.5 - distance, 0.0, 1.0));
                        }
                    }
                }
                SetTextColor(surface.memoryDc, item.completed
                    ? (darkSurface ? RGB(156, 161, 172) : RGB(126, 132, 142)) : foreground);
                RECT label{
                    checkbox.right + dipToPixels(9.0, surface),
                    row.top + (card.todoPreferences.showCreatedTime ? dipToPixels(3.0, surface) : 0),
                    row.right - dipToPixels(8.0, surface),
                    row.bottom - (card.todoPreferences.showCreatedTime ? dipToPixels(14.0, surface) : 0),
                };
                const auto itemTitle = Utf8ToWide(item.title);
                RECT measured{0, 0, std::max<LONG>(1, label.right - label.left), 0};
                DrawTextW(
                    surface.memoryDc,
                    itemTitle.c_str(),
                    -1,
                    &measured,
                    DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
                const auto singleLine = measured.bottom <= dipToPixels(18.0, surface);
                if (!singleLine && !card.todoPreferences.showCreatedTime) {
                    const auto available = label.bottom - label.top;
                    const auto measuredHeight = std::min<LONG>(measured.bottom, available);
                    label.top += std::max<LONG>(0, (available - measuredHeight) / 2);
                }
                DrawTextW(
                    surface.memoryDc,
                    itemTitle.c_str(),
                    -1,
                    &label,
                    DT_LEFT | DT_NOPREFIX | (singleLine
                        ? DT_SINGLELINE | DT_VCENTER
                        : DT_WORDBREAK | DT_EDITCONTROL));
                if (card.todoPreferences.showCreatedTime && item.createdAtUnixMilliseconds > 0) {
                    const auto timestamp = static_cast<std::time_t>(item.createdAtUnixMilliseconds / 1000);
                    tm localTime{};
                    localtime_s(&localTime, &timestamp);
                    wchar_t timeText[16]{};
                    swprintf_s(timeText, L"%02d:%02d", localTime.tm_hour, localTime.tm_min);
                    SetTextColor(surface.memoryDc, darkSurface ? RGB(145, 151, 163) : RGB(128, 134, 145));
                    RECT timeRect{label.left, label.bottom, label.right, row.bottom};
                    DrawTextW(surface.memoryDc, timeText, -1, &timeRect,
                        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
                }
                if (item.completed && singleLine) {
                    SIZE textExtent{};
                    GetTextExtentPoint32W(
                        surface.memoryDc,
                        itemTitle.c_str(),
                        static_cast<int>(itemTitle.size()),
                        &textExtent);
                    const auto textWidth = std::min<LONG>(
                        textExtent.cx, label.right - label.left);
                    const auto lineY = (label.top + label.bottom) / 2;
                    const auto linePen = CreatePen(PS_SOLID, std::max(1, dipToPixels(1.0, surface)),
                        darkSurface ? RGB(156, 161, 172) : RGB(126, 132, 142));
                    const auto previousLinePen = linePen == nullptr ? nullptr : SelectObject(surface.memoryDc, linePen);
                    MoveToEx(surface.memoryDc, label.left, lineY, nullptr);
                    LineTo(surface.memoryDc, label.left + textWidth, lineY);
                    if (previousLinePen != nullptr) SelectObject(surface.memoryDc, previousLinePen);
                    if (linePen != nullptr) DeleteObject(linePen);
                }
            }
            if (previousTodoFont != nullptr) SelectObject(surface.memoryDc, previousTodoFont);
            if (todoFont != nullptr) DeleteObject(todoFont);
            const auto addRect = todoAddControlRect(surface);
            drawRoundedFill(
                addRect,
                dipToPixels(11.0, surface),
                darkSurface ? 0x00FFFFFFu : 0x0018212Fu,
                surface.todoAddPressed ? (darkSurface ? 0.13 : 0.085)
                    : surface.todoAddHovered ? (darkSurface ? 0.085 : 0.052)
                    : (darkSurface ? 0.045 : 0.028));
            const auto addFont = CreateFontW(
                -dipToPixels(12.0, surface), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
            const auto previousAddFont = addFont == nullptr
                ? nullptr : SelectObject(surface.memoryDc, addFont);
            SetTextColor(surface.memoryDc,
                darkSurface ? RGB(185, 190, 200) : RGB(111, 117, 128));
            RECT addText{addRect.left + dipToPixels(12.0, surface), addRect.top,
                addRect.right - dipToPixels(42.0, surface), addRect.bottom};
            DrawTextW(surface.memoryDc, L"添加待办", -1, &addText,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            const auto plusX = addRect.right - dipToPixels(20.0, surface);
            const auto plusY = (addRect.top + addRect.bottom) / 2;
            const auto plusPen = CreatePen(PS_SOLID, std::max(1, dipToPixels(1.0, surface)),
                darkSurface ? RGB(177, 182, 191) : RGB(113, 118, 128));
            const auto previousPlusPen = plusPen == nullptr
                ? nullptr : SelectObject(surface.memoryDc, plusPen);
            MoveToEx(surface.memoryDc, plusX - dipToPixels(4.0, surface), plusY, nullptr);
            LineTo(surface.memoryDc, plusX + dipToPixels(4.0, surface), plusY);
            MoveToEx(surface.memoryDc, plusX, plusY - dipToPixels(4.0, surface), nullptr);
            LineTo(surface.memoryDc, plusX, plusY + dipToPixels(4.0, surface));
            if (previousPlusPen != nullptr) SelectObject(surface.memoryDc, previousPlusPen);
            if (plusPen != nullptr) DeleteObject(plusPen);
            if (previousAddFont != nullptr) SelectObject(surface.memoryDc, previousAddFont);
            if (addFont != nullptr) DeleteObject(addFont);
            if (entries.empty()) {
                RECT emptyRect{dipToPixels(16.0, surface), dipToPixels(138.0, surface),
                    surface.width - dipToPixels(16.0, surface), visibleBottom - dipToPixels(12.0, surface)};
                const auto emptyFont = CreateFontW(
                    -dipToPixels(11.0, surface), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                    DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
                const auto previousEmptyFont = emptyFont == nullptr
                    ? nullptr : SelectObject(surface.memoryDc, emptyFont);
                SetTextColor(surface.memoryDc,
                    darkSurface ? RGB(159, 165, 176) : RGB(112, 117, 127));
                DrawTextW(surface.memoryDc, L"暂无待办", -1, &emptyRect,
                    DT_CENTER | DT_SINGLELINE | DT_BOTTOM | DT_NOPREFIX);
                if (previousEmptyFont != nullptr) SelectObject(surface.memoryDc, previousEmptyFont);
                if (emptyFont != nullptr) DeleteObject(emptyFont);
            }
        }
        if (card.expanded
            && (card.type == domain::CardType::Application
                || card.type == domain::CardType::Mapping)
            && card.items.empty() && !surface.dropInsertionIndex.has_value()) {
            const auto inset = dipToPixels(12.0, surface);
            const auto top = dipToPixels(60.0, surface);
            RECT emptyRect{
                inset,
                top,
                surface.width - inset,
                visibleBottom - inset,
            };
            if (emptyRect.right > emptyRect.left && emptyRect.bottom > emptyRect.top) {
                const auto emptyColor = darkSurface ? RGB(154, 160, 170) : RGB(132, 138, 148);
                drawDashedRoundedBorder(
                    emptyRect,
                    static_cast<double>(dipToPixels(12.0, surface)),
                    darkSurface ? 0x009AA0AAu : 0x00848A94u,
                    0.92);

                const auto emptyFont = CreateFontW(
                    -dipToPixels(12.0, surface), 0, 0, 0, FW_NORMAL,
                    FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                    L"Segoe UI Variable Text");
                const auto previousEmptyFont = emptyFont == nullptr
                    ? nullptr : SelectObject(surface.memoryDc, emptyFont);
                SetTextColor(surface.memoryDc, emptyColor);
                DrawTextW(
                    surface.memoryDc,
                    L"拖放文件至此",
                    -1,
                    &emptyRect,
                    DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
                if (previousEmptyFont != nullptr) {
                    SelectObject(surface.memoryDc, previousEmptyFont);
                }
                if (emptyFont != nullptr) DeleteObject(emptyFont);
                SetTextColor(surface.memoryDc, foreground);
            }
        }
        if (card.expanded && (!card.items.empty() || surface.dropInsertionIndex.has_value())) {
            const auto scale = display.effectiveDpi / 96.0;
            const auto settings = itemLayoutSettings(surface);
            const auto iconSize = std::max(
                1,
                static_cast<int>(std::lround(settings.iconSize * scale)));
            const auto iconRegionSize = std::max(
                1,
                static_cast<int>(std::lround(settings.itemWidth * scale)));
            const auto projection = projectedItems(surface);
            auto visualSlotCount = projectedSlotCount(surface, projection);
            if (surface.dropInsertionIndex.has_value()) {
                visualSlotCount = std::max(visualSlotCount, *surface.dropInsertionIndex + 1);
            }
            const auto blendPremultiplied = [&](int x, int y, std::uint32_t foregroundPixel) {
                if (x < 0 || y < 0 || x >= surface.width || y >= visibleBottom) {
                    return;
                }
                const auto alpha = (foregroundPixel >> 24) & 0xFFu;
                if (alpha == 0) {
                    return;
                }
                auto& backgroundPixel = surface.pixels[y * surface.width + x];
                const auto inverse = 255u - alpha;
                const auto composite = [&](int shift) {
                    const auto foreground = (foregroundPixel >> shift) & 0xFFu;
                    const auto background = (backgroundPixel >> shift) & 0xFFu;
                    return std::min(255u, foreground + background * inverse / 255u);
                };
                backgroundPixel = (composite(16) << 16)
                    | (composite(8) << 8)
                    | composite(0);
            };

            if (surface.hoveredItem.has_value()) {
                const auto hovered = std::ranges::find(
                    projection, *surface.hoveredItem, &ProjectedItem::itemIndex);
                if (hovered != projection.end()) {
                    auto hotspot = itemRect(
                        surface, hovered->column, hovered->row, visualSlotCount);
                    const auto inset = dipToPixels(2.0, surface);
                    hotspot.left += inset;
                    hotspot.top += inset;
                    hotspot.right -= inset;
                    hotspot.bottom -= inset;
                    const auto hotspotRadius = static_cast<double>(dipToPixels(8.0, surface));
                    const auto centerX = (hotspot.left + hotspot.right) / 2.0;
                    const auto centerY = (hotspot.top + hotspot.bottom) / 2.0;
                    const auto halfWidth = (hotspot.right - hotspot.left) / 2.0;
                    const auto halfHeight = (hotspot.bottom - hotspot.top) / 2.0;
                    for (int y = hotspot.top; y < hotspot.bottom; ++y) {
                        for (int x = hotspot.left; x < hotspot.right; ++x) {
                            const auto qx = std::abs(x + 0.5 - centerX)
                                - (halfWidth - hotspotRadius);
                            const auto qy = std::abs(y + 0.5 - centerY)
                                - (halfHeight - hotspotRadius);
                            const auto outsideX = std::max(qx, 0.0);
                            const auto outsideY = std::max(qy, 0.0);
                            const auto distance = std::sqrt(
                                outsideX * outsideX + outsideY * outsideY)
                                + std::min(std::max(qx, qy), 0.0) - hotspotRadius;
                            const auto coverage = std::clamp(0.5 - distance, 0.0, 1.0);
                            blendRgb(
                                x,
                                y,
                                darkSurface ? 0x00FFFFFFu : 0x001F2937u,
                                coverage * (darkSurface ? 0.10 : 0.06));
                        }
                    }
                }
            }

            if (surface.dropInsertionIndex.has_value()) {
                auto preview = itemRect(
                    surface,
                    *surface.dropInsertionIndex % itemLayout(surface, 0).columns,
                    *surface.dropInsertionIndex / itemLayout(surface, 0).columns,
                    visualSlotCount);
                const auto inset = dipToPixels(3.0, surface);
                preview.left += inset;
                preview.top += inset;
                preview.right -= inset;
                preview.bottom -= inset;
                const auto previewRadius = static_cast<double>(dipToPixels(9.0, surface));
                const auto centerX = (preview.left + preview.right) / 2.0;
                const auto centerY = (preview.top + preview.bottom) / 2.0;
                const auto halfPreviewWidth = (preview.right - preview.left) / 2.0;
                const auto halfPreviewHeight = (preview.bottom - preview.top) / 2.0;
                for (int y = preview.top; y < preview.bottom; ++y) {
                    for (int x = preview.left; x < preview.right; ++x) {
                        const auto qx = std::abs(x + 0.5 - centerX)
                            - (halfPreviewWidth - previewRadius);
                        const auto qy = std::abs(y + 0.5 - centerY)
                            - (halfPreviewHeight - previewRadius);
                        const auto outsideX = std::max(qx, 0.0);
                        const auto outsideY = std::max(qy, 0.0);
                        const auto distance = std::sqrt(outsideX * outsideX + outsideY * outsideY)
                            + std::min(std::max(qx, qy), 0.0) - previewRadius;
                        const auto fillCoverage = std::clamp(0.5 - distance, 0.0, 1.0);
                        blendRgb(
                            x,
                            y,
                            0x004A84FFu,
                            fillCoverage * (darkSurface ? 0.12 : 0.08));
                    }
                }
                drawDashedRoundedBorder(
                    preview,
                    previewRadius,
                    0x004A84FFu,
                    darkSurface ? 0.96 : 0.86);
            }

            for (const auto& projected : projection) {
                const auto slot = itemRect(
                    surface, projected.column, projected.row, visualSlotCount);
                if (slot.top >= visibleBottom) {
                    continue;
                }
                const auto& item = card.items[projected.itemIndex];
                if (!item.icon.empty()) {
                    const auto iconLeft = slot.left + ((slot.right - slot.left) - iconSize) / 2;
                    const auto iconTop = slot.top + (iconRegionSize - iconSize) / 2;
                    for (int targetY = 0; targetY < iconSize; ++targetY) {
                        for (int targetX = 0; targetX < iconSize; ++targetX) {
                            blendPremultiplied(
                                iconLeft + targetX,
                                iconTop + targetY,
                                presentation::SamplePremultipliedBilinear(
                                    *item.icon.premultipliedPixels,
                                    item.icon.width,
                                    item.icon.height,
                                    iconSize,
                                    iconSize,
                                    targetX,
                                    targetY));
                        }
                    }
                }
            }

            const auto itemFont = !card.content.showItemNames
                ? nullptr
                : CreateFontW(
                    -dipToPixels(settings.itemFontSize, surface),
                0,
                0,
                0,
                FW_NORMAL,
                FALSE,
                FALSE,
                FALSE,
                DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE,
                L"Segoe UI Variable Text");
            const auto previousItemFont = itemFont == nullptr
                ? nullptr
                : SelectObject(surface.memoryDc, itemFont);
            if (card.content.showItemNames) {
                for (const auto& projected : projection) {
                    const auto slot = itemRect(
                        surface, projected.column, projected.row, visualSlotCount);
                    if (slot.top >= visibleBottom) {
                        continue;
                    }
                    const auto& item = card.items[projected.itemIndex];
                    const auto ready = item.state == presentation::CardItemState::Ready
                        || item.state == presentation::CardItemState::IconUnavailable;
                    SetTextColor(
                        surface.memoryDc,
                        ready
                            ? foreground
                            : (darkSurface ? RGB(148, 153, 162) : RGB(121, 126, 135)));
                    RECT labelRect{
                        slot.left,
                        slot.top + iconRegionSize,
                        slot.right,
                        std::min<LONG>(slot.bottom, visibleBottom),
                    };
                    DrawTextW(
                        surface.memoryDc,
                        item.displayName.c_str(),
                        -1,
                        &labelRect,
                        DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
                }
            }
            if (previousItemFont != nullptr) {
                SelectObject(surface.memoryDc, previousItemFont);
            }
            if (itemFont != nullptr) {
                DeleteObject(itemFont);
            }
            SetTextColor(surface.memoryDc, foreground);
        }
        if (card.showCollapseControl) {
            const auto centerX = (control.left + control.right) / 2.0;
            const auto centerY = (control.top + control.bottom) / 2.0;
            const auto scale = display.effectiveDpi / 96.0;
            if (surface.collapseHovered || surface.collapsePressed) {
                const auto pressRadius = 14.0 * scale;
                for (int y = control.top; y < control.bottom; ++y) {
                    for (int x = control.left; x < control.right; ++x) {
                        const auto dx = x + 0.5 - centerX;
                        const auto dy = y + 0.5 - centerY;
                        const auto coverage = std::clamp(
                            pressRadius + 0.5 - std::sqrt(dx * dx + dy * dy),
                            0.0,
                            1.0);
                        const auto opacity = surface.collapsePressed ? 0.10 : 0.06;
                        blendRgb(
                            x,
                            y,
                            darkSurface ? 0x00FFFFFFu : 0x00000000u,
                            coverage * opacity);
                    }
                }
            }
            const auto chevronColor = darkSurface ? 0x00E5E8EDu : 0x005B6069u;
            const auto halfWidth = 5.0 * scale;
            const auto halfHeight = 2.75 * scale;
            const auto direction = card.expanded ? -1.0 : 1.0;
            const auto middleY = centerY + direction * halfHeight;
            const auto sideY = centerY - direction * halfHeight;
            const auto strokeRadius = std::max(0.75, 0.8 * scale);
            const auto drawSegment = [&](double x1, double y1, double x2, double y2) {
                const auto vx = x2 - x1;
                const auto vy = y2 - y1;
                const auto lengthSquared = vx * vx + vy * vy;
                const auto minX = static_cast<int>(std::floor(std::min(x1, x2) - strokeRadius - 1));
                const auto maxX = static_cast<int>(std::ceil(std::max(x1, x2) + strokeRadius + 1));
                const auto minY = static_cast<int>(std::floor(std::min(y1, y2) - strokeRadius - 1));
                const auto maxY = static_cast<int>(std::ceil(std::max(y1, y2) + strokeRadius + 1));
                for (int y = minY; y <= maxY; ++y) {
                    for (int x = minX; x <= maxX; ++x) {
                        const auto px = x + 0.5;
                        const auto py = y + 0.5;
                        const auto projection = std::clamp(
                            ((px - x1) * vx + (py - y1) * vy) / lengthSquared,
                            0.0,
                            1.0);
                        const auto dx = px - (x1 + projection * vx);
                        const auto dy = py - (y1 + projection * vy);
                        const auto coverage = std::clamp(
                            strokeRadius + 0.5 - std::sqrt(dx * dx + dy * dy),
                            0.0,
                            1.0);
                        blendRgb(x, y, chevronColor, coverage);
                    }
                }
            };
            drawSegment(centerX - halfWidth, sideY, centerX, middleY);
            drawSegment(centerX, middleY, centerX + halfWidth, sideY);
        }

        const auto surfaceAlpha = std::clamp(card.opacity, 0.0, 1.0) * 255.0;
        const auto halfWidth = surface.width / 2.0;
        const auto halfHeight = visibleBottom / 2.0;
        for (int y = 0; y < visibleBottom; ++y) {
            for (int x = 0; x < surface.width; ++x) {
                double coverage = 1.0;
                if (radius > 0) {
                    const auto qx = std::abs(x + 0.5 - halfWidth) - (halfWidth - radius);
                    const auto qy = std::abs(y + 0.5 - halfHeight) - (halfHeight - radius);
                    const auto outsideX = std::max(qx, 0.0);
                    const auto outsideY = std::max(qy, 0.0);
                    const auto signedDistance = std::sqrt(
                        outsideX * outsideX + outsideY * outsideY)
                        + std::min(std::max(qx, qy), 0.0) - radius;
                    coverage = std::clamp(0.5 - signedDistance, 0.0, 1.0);
                }
                const auto alpha = static_cast<std::uint32_t>(std::lround(
                    surfaceAlpha * coverage));
                auto& pixel = surface.pixels[y * surface.width + x];
                const auto red = ((pixel >> 16) & 0xFFu) * alpha / 255u;
                const auto green = ((pixel >> 8) & 0xFFu) * alpha / 255u;
                const auto blue = (pixel & 0xFFu) * alpha / 255u;
                pixel = (alpha << 24) | (red << 16) | (green << 8) | blue;
            }
        }

        if (commit) commitSurface(surface);
    }

    void commitSurfaceAt(Surface& surface, POINT destination) {
        HDC screen = GetDC(nullptr);
        if (screen == nullptr) {
            throw std::runtime_error("GetDC failed for desktop host.");
        }
        POINT source{0, 0};
        SIZE size{surface.width, surface.height};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        const auto updated = UpdateLayeredWindow(
            surface.window,
            screen,
            &destination,
            &size,
            surface.memoryDc,
            &source,
            0,
            &blend,
            ULW_ALPHA);
        ReleaseDC(nullptr, screen);
        if (!updated) {
            throw std::runtime_error("UpdateLayeredWindow failed for desktop host.");
        }
    }

    void commitSurface(Surface& surface) {
        const auto scale = surface.display.effectiveDpi / 96.0;
        commitSurfaceAt(surface, {
            static_cast<LONG>(std::lround(
                (surface.display.workAreaLeft + surface.projection.rect.left) * scale)),
            static_cast<LONG>(std::lround(
                (surface.display.workAreaTop + surface.projection.rect.top) * scale)),
        });
    }

    void commitInteraction(HWND window) {
        auto* moved = findSurface(window);
        if (moved == nullptr) {
            return;
        }
        RECT windowRect{};
        if (!GetWindowRect(window, &windowRect)) {
            return;
        }
        const auto* selectedDisplay = displayForWindowRect(windowRect, moved->display.id);
        if (selectedDisplay == nullptr) {
            return;
        }
        const auto targetDisplay = *selectedDisplay;
        const auto scale = targetDisplay.effectiveDpi / 96.0;
        domain::PlacementRect proposed{
            .left = windowRect.left / scale - targetDisplay.workAreaLeft,
            .top = windowRect.top / scale - targetDisplay.workAreaTop,
            .width = moved->projection.rect.width,
            .height = moved->projection.rect.height,
        };
        std::vector<domain::PlacementRect> otherCards;
        for (const auto& surface : surfaces) {
            if (surface.window != window && surface.projection.displayId == targetDisplay.id
                && surface.projection.placementId != moved->projection.placementId) {
                otherCards.push_back(surface.projection.rect);
            }
        }
        const auto resolved = presentation::ResolvePlacementInteractionDetailed(
            proposed,
            targetDisplay.workAreaWidth,
            targetDisplay.workAreaHeight,
            otherCards,
            (GetKeyState(VK_CONTROL) & 0x8000) != 0);

        moved->display = targetDisplay;
        moved->projection.displayId = targetDisplay.id;
        moved->projection.requestedDisplayId = targetDisplay.id;
        moved->projection.rect = resolved.rect;
        std::vector<Surface*> affected{moved};
        auto deferred = BeginDeferWindowPos(static_cast<int>(affected.size()));
        if (deferred == nullptr) {
            throw std::runtime_error("BeginDeferWindowPos failed after Card interaction.");
        }
        for (auto* surface : affected) {
            const auto surfaceScale = surface->display.effectiveDpi / 96.0;
            const auto width = std::max(1, static_cast<int>(
                std::lround(resolved.rect.width * surfaceScale)));
            const auto height = std::max(1, static_cast<int>(
                std::lround(resolved.rect.height * surfaceScale)));
            if (width != surface->width || height != surface->height) {
                destroyBitmap(*surface);
                surface->width = width;
                surface->height = height;
                createBitmap(*surface);
                render(*surface, surface->display, surface->card, surface->ordinal);
            }
            const auto left = static_cast<int>(std::lround(
                surface->display.workAreaLeft * surfaceScale
                + resolved.rect.left * surfaceScale));
            const auto top = static_cast<int>(std::lround(
                surface->display.workAreaTop * surfaceScale
                + resolved.rect.top * surfaceScale));
            deferred = DeferWindowPos(
                deferred,
                surface->window,
                HWND_BOTTOM,
                left,
                top,
                width,
                height,
                SWP_NOACTIVATE);
            if (deferred == nullptr) {
                throw std::runtime_error("DeferWindowPos failed after Card interaction.");
            }
        }
        if (!EndDeferWindowPos(deferred)) {
            throw std::runtime_error("EndDeferWindowPos failed after Card interaction.");
        }
        inferVerticalLeader(*moved);
        reflowVerticalFollowers(*moved, true);
        if (placementChanged) {
            placementChanged(
                moved->projection.placementId,
                moved->projection.cardId,
                moved->projection.displayId,
                resolved.rect,
                resolved.horizontalAnchor,
                resolved.verticalAnchor,
                targetDisplay.workAreaWidth,
                targetDisplay.workAreaHeight);
        }
    }

    void present(
        std::span<const domain::PlacementProjection> projections,
        std::span<const domain::DisplaySnapshot> displays,
        std::span<const presentation::CardView> cards) {
        if (!windowClassRegistered) {
            initialize();
        }
        destroySurfaces();
        this->displays.assign(displays.begin(), displays.end());
        surfaces.reserve(projections.size());
        for (const auto& projection : projections) {
            const auto card = std::find_if(
                cards.begin(), cards.end(), [&](const presentation::CardView& candidate) {
                    return candidate.id == projection.cardId;
                });
            if (card == cards.end()) {
                throw std::invalid_argument("Projection references an unknown Card view.");
            }
            if (!card->visible) {
                continue;
            }
            const auto display = std::find_if(
                displays.begin(), displays.end(), [&](const domain::DisplaySnapshot& candidate) {
                    return candidate.id == projection.displayId;
                });
            if (display == displays.end()) {
                throw std::invalid_argument("Projection references an unknown display.");
            }
            const auto scale = display->effectiveDpi / 96.0;
            Surface surface{
                .projection = projection,
                .display = *display,
                .card = *card,
                .ordinal = surfaces.size() + 1,
                .width = std::max(1, static_cast<int>(std::lround(projection.rect.width * scale))),
                .height = std::max(1, static_cast<int>(std::lround(projection.rect.height * scale))),
            };
            try {
                resizeSurfaceForContent(surface, false);
                createSurface(surface);
                render(surface, *display, *card, surface.ordinal);
                surfaces.push_back(std::move(surface));
            } catch (...) {
                destroySurface(surface);
                throw;
            }
        }

        inferVerticalLeaders();
        for (auto& surface : surfaces) {
            if (!surface.verticalLeaderPlacementId.has_value()) {
                reflowVerticalFollowers(surface, false);
            }
        }

        auto deferred = BeginDeferWindowPos(static_cast<int>(surfaces.size()));
        if (deferred == nullptr && !surfaces.empty()) {
            throw std::runtime_error("BeginDeferWindowPos failed for desktop host.");
        }
        for (const auto& surface : surfaces) {
            const auto display = std::find_if(
                displays.begin(), displays.end(), [&](const domain::DisplaySnapshot& candidate) {
                    return candidate.id == surface.projection.displayId;
                });
            const auto scale = display->effectiveDpi / 96.0;
            const auto left = static_cast<int>(std::lround(
                display->workAreaLeft * scale + surface.projection.rect.left * scale));
            const auto top = static_cast<int>(std::lround(
                display->workAreaTop * scale + surface.projection.rect.top * scale));
            deferred = DeferWindowPos(
                deferred,
                surface.window,
                HWND_BOTTOM,
                left,
                top,
                surface.width,
                surface.height,
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
            if (deferred == nullptr) {
                throw std::runtime_error("DeferWindowPos failed for desktop host.");
            }
        }
        if (!surfaces.empty() && !EndDeferWindowPos(deferred)) {
            throw std::runtime_error("EndDeferWindowPos failed for desktop host.");
        }
    }

    void updateCardItems(
        const domain::CardId& cardId,
        const std::vector<presentation::CardItemView>& items) {
        std::vector<Surface*> affected;
        for (auto& surface : surfaces) {
            if (surface.card.id != cardId) {
                continue;
            }
            clearPointerHover(surface.window, false);
            surface.card.items = items;
            if (surface.dropInsertionIndex.has_value()) {
                surface.dropInsertionIndex = std::min(
                    *surface.dropInsertionIndex,
                    surface.card.items.size());
            }
            resizeSurfaceForContent(surface, true);
            render(surface, surface.display, surface.card, surface.ordinal, false);
            affected.push_back(&surface);
        }
        for (auto* surface : affected) {
            commitSurface(*surface);
            reflowVerticalFollowers(*surface, true);
        }
    }

    void updateCardItemsBatch(
        const std::vector<WindowsDesktopHost::CardItemsUpdate>& updates) {
        std::vector<Surface*> affected;
        for (auto& surface : surfaces) {
            const auto update = std::find_if(
                updates.begin(), updates.end(), [&](const auto& candidate) {
                    return candidate.cardId == surface.card.id;
                });
            if (update == updates.end()) continue;
            clearPointerHover(surface.window, false);
            surface.card.items = update->items;
            surface.card.applicationSortMode = update->sortMode;
            surface.card.applicationItemPlacements = update->itemPlacements;
            if (surface.dropInsertionIndex.has_value()) {
                surface.dropInsertionIndex = std::min(
                    *surface.dropInsertionIndex, surface.card.items.size());
            }
            resizeSurfaceForContent(surface, true);
            render(surface, surface.display, surface.card, surface.ordinal, false);
            affected.push_back(&surface);
        }
        for (auto* surface : affected) {
            commitSurface(*surface);
            reflowVerticalFollowers(*surface, true);
        }
    }

    void updateTodoItems(
        const domain::CardId& cardId,
        const std::vector<domain::TodoItem>& items) {
        std::vector<Surface*> affected;
        for (auto& surface : surfaces) {
            if (surface.card.id != cardId) continue;
            clearPointerHover(surface.window, false);
            surface.card.todoItems = items;
            resizeSurfaceForContent(surface, true);
            render(surface, surface.display, surface.card, surface.ordinal, false);
            affected.push_back(&surface);
        }
        for (auto* surface : affected) {
            commitSurface(*surface);
            reflowVerticalFollowers(*surface, true);
        }
    }

    void queueCardItemsRefresh(const domain::CardId& cardId) noexcept {
        if (cardId.empty()) {
            return;
        }
        bool shouldPost = false;
        {
            std::lock_guard lock(pendingRefreshMutex);
            shouldPost = pendingRefreshCards.insert(cardId).second;
        }
        if (shouldPost
            && !PostThreadMessageW(ownerThreadId, CardItemsRefreshMessage, 0, 0)) {
            std::lock_guard lock(pendingRefreshMutex);
            pendingRefreshCards.erase(cardId);
        }
    }

    void refreshQueuedCardItems() noexcept {
        std::unordered_set<domain::CardId> pending;
        {
            std::lock_guard lock(pendingRefreshMutex);
            pending.swap(pendingRefreshCards);
        }
        if (!cardItemsRefresh || pending.empty()) {
            return;
        }
        std::vector<domain::CardId> ordered(pending.begin(), pending.end());
        std::ranges::sort(ordered);
        std::vector<WindowsDesktopHost::CardItemsUpdate> updates;
        for (const auto& cardId : ordered) {
            const auto surface = std::find_if(
                surfaces.begin(), surfaces.end(), [&](const Surface& candidate) {
                    return candidate.card.id == cardId;
                });
            if (surface == surfaces.end()) {
                continue;
            }
            try {
                updates.push_back({
                    cardId,
                    cardItemsRefresh(cardId, surface->card.content.itemSize),
                    surface->card.applicationSortMode,
                    surface->card.applicationItemPlacements,
                });
            } catch (...) {
                // One failed Card refresh must not discard other queued Cards.
            }
        }
        try {
            updateCardItemsBatch(updates);
        } catch (...) {
            // Refresh requests cannot throw through the Win32 message loop.
        }
    }

    void updateCardContentPreferences(
        const domain::CardId& cardId,
        domain::CardContentPreferences preferences) {
        std::vector<presentation::CardItemView> refreshedItems;
        bool itemsRefreshed = false;
        const auto sourcePixels = [](domain::CardItemSize size) {
            return size == domain::CardItemSize::Small || size == domain::CardItemSize::Medium
                ? 16 : 32;
        };
        const auto first = std::find_if(
            surfaces.begin(), surfaces.end(), [&](const auto& surface) {
                return surface.card.id == cardId;
            });
        if (first != surfaces.end()
            && sourcePixels(first->card.content.itemSize) != sourcePixels(preferences.itemSize)
            && cardItemsRefresh) {
            refreshedItems = cardItemsRefresh(cardId, preferences.itemSize);
            itemsRefreshed = true;
        }
        std::vector<Surface*> affected;
        for (auto& surface : surfaces) {
            if (surface.card.id != cardId) {
                continue;
            }
            clearPointerHover(surface.window, false);
            surface.card.content = preferences;
            if (itemsRefreshed) {
                surface.card.items = refreshedItems;
            }
            resizeSurfaceForContent(surface, true);
            render(surface, surface.display, surface.card, surface.ordinal, false);
            affected.push_back(&surface);
        }
        for (auto* surface : affected) commitSurface(*surface);
    }

    void updateCardChromePreferences(
        const domain::CardId& cardId,
        domain::CardChromePreferences preferences) {
        for (auto& surface : surfaces) {
            if (surface.card.id != cardId) continue;
            surface.card.showCollapseControl = preferences.showCollapseControl;
            surface.card.showCloseControl = preferences.showCloseControl;
            surface.card.showPinControl = preferences.showPinControl;
            surface.card.showTitle = preferences.showTitle;
            render(surface, surface.display, surface.card, surface.ordinal);
        }
    }

    void updateCardAppearancePreferences(
        const domain::CardId& cardId,
        domain::CardAppearancePreferences preferences) {
        for (auto& surface : surfaces) {
            if (surface.card.id != cardId) continue;
            surface.card.appearancePreset = preferences.preset;
            surface.card.opacity = preferences.opacity;
            surface.card.cornerRadius = preferences.cornerRadius;
            render(surface, surface.display, surface.card, surface.ordinal);
        }
    }

    void updateTodoPreferences(
        const domain::CardId& cardId,
        domain::TodoCardPreferences preferences) {
        std::vector<Surface*> affected;
        for (auto& surface : surfaces) {
            if (surface.card.id != cardId || surface.card.type != domain::CardType::Todo) continue;
            surface.card.todoPreferences = preferences;
            resizeSurfaceForContent(surface, true);
            render(surface, surface.display, surface.card, surface.ordinal, false);
            affected.push_back(&surface);
        }
        for (auto* surface : affected) {
            commitSurface(*surface);
            reflowVerticalFollowers(*surface, true);
        }
    }

    int run(int durationMilliseconds) {
        UINT_PTR timer = 0;
        if (durationMilliseconds > 0) {
            timer = SetTimer(nullptr, 1, static_cast<UINT>(durationMilliseconds), nullptr);
            if (timer == 0) {
                throw std::runtime_error("SetTimer failed for desktop host.");
            }
        }
        MSG message{};
        while (!closeRequested && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (message.message == CardItemsRefreshMessage) {
                refreshQueuedCardItems();
                continue;
            }
            if (timer != 0 && message.message == WM_TIMER && message.wParam == timer) {
                closeRequested = true;
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (timer != 0) {
            KillTimer(nullptr, timer);
        }
        return 0;
    }
};

WindowsDesktopHost::WindowsDesktopHost(std::wstring title)
    : impl_(std::make_unique<Impl>(std::move(title))) {
}

WindowsDesktopHost::~WindowsDesktopHost() = default;

void WindowsDesktopHost::present(
    std::span<const domain::PlacementProjection> projections,
    std::span<const domain::DisplaySnapshot> displays,
    std::span<const presentation::CardView> cards) {
    impl_->present(projections, displays, cards);
}

int WindowsDesktopHost::run(int durationMilliseconds) {
    return impl_->run(durationMilliseconds);
}

void WindowsDesktopHost::requestClose() noexcept {
    impl_->closeRequested = true;
    PostQuitMessage(0);
}

void WindowsDesktopHost::setPlacementChangedCallback(PlacementChangedCallback callback) {
    impl_->placementChanged = std::move(callback);
}

void WindowsDesktopHost::setCardExpandedChangedCallback(CardExpandedChangedCallback callback) {
    impl_->cardExpandedChanged = std::move(callback);
}

void WindowsDesktopHost::setApplicationItemsDroppedCallback(
    ApplicationItemsDroppedCallback callback) {
    impl_->applicationItemsDropped = std::move(callback);
}

void WindowsDesktopHost::setApplicationItemDragCompletedCallback(
    ApplicationItemDragCompletedCallback callback) {
    impl_->applicationItemDragCompleted = std::move(callback);
}

void WindowsDesktopHost::setCardItemActivatedCallback(CardItemActivatedCallback callback) {
    impl_->cardItemActivated = std::move(callback);
}

void WindowsDesktopHost::setCardItemsRefreshCallback(CardItemsRefreshCallback callback) {
    impl_->cardItemsRefresh = std::move(callback);
}

void WindowsDesktopHost::setTodoItemAddedCallback(TodoItemAddedCallback callback) {
    impl_->todoItemAdded = std::move(callback);
}

void WindowsDesktopHost::setTodoItemAddedScheduledCallback(
    TodoItemAddedScheduledCallback callback) {
    impl_->todoItemAddedScheduled = std::move(callback);
}

void WindowsDesktopHost::setTodoItemRenamedCallback(TodoItemRenamedCallback callback) {
    impl_->todoItemRenamed = std::move(callback);
}

void WindowsDesktopHost::setTodoItemCompletedChangedCallback(
    TodoItemCompletedChangedCallback callback) {
    impl_->todoItemCompletedChanged = std::move(callback);
}

void WindowsDesktopHost::setTodoItemRemovedCallback(TodoItemRemovedCallback callback) {
    impl_->todoItemRemoved = std::move(callback);
}

void WindowsDesktopHost::setTodoItemsReorderedCallback(TodoItemsReorderedCallback callback) {
    impl_->todoItemsReordered = std::move(callback);
}

void WindowsDesktopHost::setTodoItemsArchivedCallback(TodoItemsArchivedCallback callback) {
    impl_->todoItemsArchived = std::move(callback);
}

void WindowsDesktopHost::updateCardItems(
    const domain::CardId& cardId,
    std::vector<presentation::CardItemView> items) {
    impl_->updateCardItems(cardId, items);
}

void WindowsDesktopHost::updateCardItemsBatch(std::vector<CardItemsUpdate> updates) {
    impl_->updateCardItemsBatch(updates);
}

void WindowsDesktopHost::updateTodoItems(
    const domain::CardId& cardId,
    std::vector<domain::TodoItem> items) {
    impl_->updateTodoItems(cardId, items);
}

void WindowsDesktopHost::requestCardItemsRefresh(const domain::CardId& cardId) noexcept {
    impl_->queueCardItemsRefresh(cardId);
}

void WindowsDesktopHost::updateCardContentPreferences(
    const domain::CardId& cardId,
    domain::CardContentPreferences preferences) {
    impl_->updateCardContentPreferences(cardId, preferences);
}

void WindowsDesktopHost::updateCardChromePreferences(
    const domain::CardId& cardId,
    domain::CardChromePreferences preferences) {
    impl_->updateCardChromePreferences(cardId, std::move(preferences));
}

void WindowsDesktopHost::updateCardAppearancePreferences(
    const domain::CardId& cardId,
    domain::CardAppearancePreferences preferences) {
    impl_->updateCardAppearancePreferences(cardId, std::move(preferences));
}

void WindowsDesktopHost::updateTodoPreferences(
    const domain::CardId& cardId,
    domain::TodoCardPreferences preferences) {
    impl_->updateTodoPreferences(cardId, std::move(preferences));
}

} // namespace desto::platform::windows
