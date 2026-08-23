#include "WindowsDesktopHost.h"
#include "ApplicationCardOrdering.h"
#include "CardContentLayout.h"
#include "PlacementInteraction.h"
#include "PremultipliedImageResampler.h"
#include "RoundedDashGeometry.h"
#include "WindowsFileDragDrop.h"
#include "WindowsIconFont.h"
#include "WindowsTextInput.h"
#include "WindowsPopupMenu.h"

#include <Windows.h>
#include <CommCtrl.h>
#include <ShlObj.h>
#include <ShObjIdl.h>
#include <Uxtheme.h>
#include <WtsApi32.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
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

#pragma comment(lib, "uxtheme.lib")

namespace desto::platform::windows {

std::uint32_t ResolveFileCardDropEffect(
    domain::CardType type,
    domain::MappingMode mappingMode,
    bool mappingHasSource,
    bool mappingAllowsSourceMutation,
    bool sameCardSource,
    std::uint32_t allowedEffects,
    bool mappingCanNavigateUp) noexcept {
    if (type == domain::CardType::Application) {
        return (allowedEffects & DROPEFFECT_MOVE) != 0
            ? DROPEFFECT_MOVE : DROPEFFECT_NONE;
    }
    if (type == domain::CardType::Mapping) {
        if (sameCardSource && (allowedEffects & DROPEFFECT_MOVE) != 0) {
            return DROPEFFECT_MOVE;
        }
        if (mappingMode == domain::MappingMode::Folder) {
            if (!mappingHasSource) {
                return (allowedEffects & DROPEFFECT_COPY) != 0
                    ? DROPEFFECT_COPY : DROPEFFECT_NONE;
            }
            // Folder source cards are live projections of one real directory.
            // Their file operations are always enabled; Card locking is the
            // separate, user-visible switch that disables all mutations.
            return (allowedEffects & DROPEFFECT_MOVE) != 0
                ? DROPEFFECT_MOVE : DROPEFFECT_NONE;
        }
        if (mappingCanNavigateUp) {
            return (allowedEffects & DROPEFFECT_MOVE) != 0
                ? DROPEFFECT_MOVE
                : (allowedEffects & DROPEFFECT_COPY) != 0
                ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        }
        // A reference collection never mutates the source. Some shell drag
        // providers advertise MOVE only, so still request a copy effect here;
        // the application callback treats this branch as reference creation.
        return (allowedEffects & DROPEFFECT_COPY) != 0
            ? DROPEFFECT_COPY
            : (allowedEffects & DROPEFFECT_MOVE) != 0
            ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    }
    return DROPEFFECT_NONE;
}

std::wstring_view ResolveTodoTextFontFamily(std::wstring_view) noexcept {
    return L"Segoe UI Variable Text";
}

CrystalMaterialStyle ResolveCrystalMaterialStyle() noexcept {
    return {
        .surfaceOpacity = 0.20,
        .itemFillOpacity = 0.14,
        .itemOutlineOpacity = 0.62,
        .surfaceOutlineOpacity = 0.60,
    };
}

std::uint32_t ResolveLayeredSurfaceTextQuality() noexcept {
    // ClearType stores separate red, green and blue coverage values. Those
    // subpixels become cyan/orange fringes when a layered DIB is subsequently
    // alpha-composited over the desktop. Grayscale antialiasing is stable on
    // every wallpaper and display orientation.
    return ANTIALIASED_QUALITY;
}

// Small type on a translucent layered card needs more ink than the same size
// on an opaque ClearType surface. Keep the size ladder; raise weight instead.
constexpr int kCardTitleWeight = FW_SEMIBOLD;

std::uint32_t CompositeCrystalLayerPixel(
    std::uint32_t materialRgb,
    std::uint32_t materialAlpha,
    std::uint32_t premultipliedContent,
    double shapeCoverage) noexcept {
    const auto multiply = [](std::uint32_t value, std::uint32_t alpha) {
        return (value * alpha + 127u) / 255u;
    };
    const auto baseAlpha = std::min(255u, materialAlpha);
    const auto contentAlpha = (premultipliedContent >> 24) & 0xFFu;
    const auto inverse = 255u - contentAlpha;
    const auto compositeChannel = [&](int shift) {
        const auto base = multiply((materialRgb >> shift) & 0xFFu, baseAlpha);
        const auto content = (premultipliedContent >> shift) & 0xFFu;
        return std::min(255u, content + multiply(base, inverse));
    };
    const auto compositeAlpha = std::min(
        255u, contentAlpha + multiply(baseAlpha, inverse));
    const auto clipAlpha = static_cast<std::uint32_t>(std::lround(
        std::clamp(shapeCoverage, 0.0, 1.0) * 255.0));
    const auto alpha = multiply(compositeAlpha, clipAlpha);
    const auto red = multiply(compositeChannel(16), clipAlpha);
    const auto green = multiply(compositeChannel(8), clipAlpha);
    const auto blue = multiply(compositeChannel(0), clipAlpha);
    return (alpha << 24) | (red << 16) | (green << 8) | blue;
}

namespace {

using Microsoft::WRL::ComPtr;

struct TextMaskSurface {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previousBitmap = nullptr;
    std::uint32_t* pixels = nullptr;
    int width = 0;
    int height = 0;

    TextMaskSurface(int requestedWidth, int requestedHeight)
        : width(requestedWidth), height(requestedHeight) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        bitmap = CreateDIBSection(
            nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        dc = CreateCompatibleDC(nullptr);
        if (bitmap == nullptr || dc == nullptr || bits == nullptr) {
            if (dc != nullptr) DeleteDC(dc);
            if (bitmap != nullptr) DeleteObject(bitmap);
            throw std::runtime_error("Unable to create layered text mask.");
        }
        pixels = static_cast<std::uint32_t*>(bits);
        previousBitmap = SelectObject(dc, bitmap);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
    }

    ~TextMaskSurface() {
        if (dc != nullptr && previousBitmap != nullptr) {
            SelectObject(dc, previousBitmap);
        }
        if (dc != nullptr) DeleteDC(dc);
        if (bitmap != nullptr) DeleteObject(bitmap);
    }

    TextMaskSurface(const TextMaskSurface&) = delete;
    TextMaskSurface& operator=(const TextMaskSurface&) = delete;

    void clear(const RECT& requestedRect) noexcept {
        const auto left = std::clamp<LONG>(requestedRect.left, 0, width);
        const auto top = std::clamp<LONG>(requestedRect.top, 0, height);
        const auto right = std::clamp<LONG>(requestedRect.right, left, width);
        const auto bottom = std::clamp<LONG>(requestedRect.bottom, top, height);
        for (auto y = top; y < bottom; ++y) {
            std::fill(
                pixels + static_cast<std::size_t>(y) * width + left,
                pixels + static_cast<std::size_t>(y) * width + right,
                0u);
        }
    }
};

bool SystemAppsUseDarkTheme() noexcept {
    DWORD lightTheme = 1;
    DWORD size = sizeof(lightTheme);
    return RegGetValueW(
               HKEY_CURRENT_USER,
               L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
               L"AppsUseLightTheme",
               RRF_RT_REG_DWORD,
               nullptr,
               &lightTheme,
               &size) == ERROR_SUCCESS
        && lightTheme == 0;
}

HWND FindDesktopOwnerWindow() noexcept {
    auto shellView = FindWindowExW(
        FindWindowW(L"Progman", nullptr), nullptr, L"SHELLDLL_DefView", nullptr);
    if (shellView == nullptr) {
        EnumWindows(+[](HWND window, LPARAM parameter) -> BOOL {
            auto& result = *reinterpret_cast<HWND*>(parameter);
            result = FindWindowExW(window, nullptr, L"SHELLDLL_DefView", nullptr);
            return result == nullptr ? TRUE : FALSE;
        }, reinterpret_cast<LPARAM>(&shellView));
    }
    return shellView == nullptr ? nullptr : GetAncestor(shellView, GA_ROOT);
}

constexpr UINT_PTR ItemTooltipTimerId = 2;
constexpr UINT ItemTooltipDelayMilliseconds = 600;
constexpr UINT_PTR DropPreviewResetTimerId = 3;
constexpr UINT DropPreviewResetDelayMilliseconds = 90;
constexpr UINT_PTR TodoViewLongPressTimerId = 5;
constexpr UINT TodoViewLongPressDelayMilliseconds = 500;
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
    std::optional<std::int32_t> timeZoneOffsetMinutes;
    std::optional<domain::PlacementId> verticalLeaderPlacementId;
    std::optional<double> lastCommittedVisibleHeightDip;
    std::size_t ordinal = 0;
    int width = 0;
    int height = 0;
    int interactiveHeight = 0;
    bool nativeMoveActive = false;
    bool alwaysOnTop = false;
    bool collapseHovered = false;
    bool collapsePressed = false;
    bool pinHovered = false;
    bool pinPressed = false;
    bool mappingViewHovered = false;
    bool mappingViewPressed = false;
    bool mappingUpHovered = false;
    bool mappingUpPressed = false;
    bool todoAddHovered = false;
    bool todoAddPressed = false;
    bool todoViewHovered = false;
    bool todoViewPressed = false;
    bool todoViewLongPressTriggered = false;
    std::optional<int> todoViewPressOriginalDateOffset;
    bool todoArchiveHovered = false;
    bool todoArchivePressed = false;
    int todoAddDateOffset = 0;
    bool todoCalendarOpen = false;
    domain::TodoDate todoCalendarMonth{1970, 1, 1};
    std::optional<int> todoCalendarPressed;
    HWND tooltip = nullptr;
    std::optional<std::size_t> hoveredItem;
    std::optional<std::size_t> hoveredTodoRow;
    std::wstring tooltipText;
    IDropTarget* dropTarget = nullptr;
    std::optional<std::size_t> dropInsertionIndex;
    std::optional<std::size_t> dropPreviewColumns;
    std::optional<domain::PlacementRect> dropPreviewOriginRect;
    bool dropDragActive = false;
    bool dropCommitInProgress = false;
    bool dropCommitContentApplied = false;
    DWORD dropAllowedEffect = DROPEFFECT_NONE;
    DWORD dropKeyState = 0;
    std::optional<std::string> dropSourceCardId;
    std::optional<presentation::CardDropPreview> pendingDropExpansion;
    bool pendingDropShrink = false;
    std::uint64_t dropExpansionStartedAt = 0;
    std::size_t dropExpansionStep = 0;
    std::optional<std::size_t> pressedItem;
    POINT itemDragStart{};
    bool itemDragActive = false;
    std::size_t scrollRowOffset = 0;
    std::optional<std::size_t> automaticVisibleRows;
    std::optional<double> automaticMaximumHeightDip;
    std::optional<std::size_t> pressedTodoRow;
    std::optional<std::size_t> pressedTodoCheckbox;
    std::optional<std::size_t> todoDragTarget;
    POINT todoDragStart{};
};

struct TodoDisplayEntry {
    bool showDateLabel = false;
    bool archived = false;
    std::size_t itemIndex = 0;
    domain::TodoDate date{};
};

domain::TodoDate CurrentTodoDate(std::optional<std::int32_t> offsetMinutes) noexcept {
    return domain::CurrentTodoDate(offsetMinutes);
}

std::int64_t UnixMillisecondsNow() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::wstring TodoDateLabel(
    domain::TodoDate date,
    std::optional<std::int32_t> offsetMinutes,
    bool english) {
    const auto today = CurrentTodoDate(offsetMinutes);
    const auto delta = domain::CompareTodoDates(date, today);
    if (delta == 0) return english ? L"Today" : L"今天";
    if (date == domain::AddTodoDays(today, 1)) return english ? L"Tomorrow" : L"明天";
    if (date == domain::AddTodoDays(today, -1)) return english ? L"Yesterday" : L"昨日";
    if (date == domain::AddTodoDays(today, -2)) return english ? L"2 days ago" : L"前日";
    wchar_t buffer[32]{};
    if (english) {
        if (date.year == today.year) {
            swprintf_s(buffer, L"%02u/%02u",
                static_cast<unsigned>(date.month), static_cast<unsigned>(date.day));
        } else {
            swprintf_s(buffer, L"%u/%02u/%02u", static_cast<unsigned>(date.year),
                static_cast<unsigned>(date.month), static_cast<unsigned>(date.day));
        }
    } else if (date.year == today.year) {
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
        if (foregroundHook != nullptr) UnhookWinEvent(foregroundHook);
        if (foregroundObserver == this) foregroundObserver = nullptr;
        if (todoEditor != nullptr) finishTodoEdit(todoEditor, false);
        destroyGuides();
        destroySurfaces();
        if (lifecycleWindow != nullptr) {
            KillTimer(lifecycleWindow, ShellBindingRetryTimerId);
            if (sessionNotificationsRegistered) {
                WTSUnRegisterSessionNotification(lifecycleWindow);
            }
            DestroyWindow(lifecycleWindow);
            lifecycleWindow = nullptr;
        }
        if (virtualDesktopManager != nullptr) {
            virtualDesktopManager->Release();
            virtualDesktopManager = nullptr;
        }
        if (guideWindowClassRegistered) {
            UnregisterClassW(guideClassName.c_str(), module);
        }
        if (tooltipWindowClassRegistered) {
            UnregisterClassW(tooltipClassName.c_str(), module);
        }
        if (windowClassRegistered) {
            UnregisterClassW(className.c_str(), module);
        }
        if (lifecycleWindowClassRegistered) {
            UnregisterClassW(lifecycleClassName.c_str(), module);
        }
        if (oleInitialized) {
            OleUninitialize();
        }
    }

    std::wstring title;
    std::wstring className = L"DestoDesktopHostSurface";
    std::wstring guideClassName = L"DestoAlignmentGuide";
    std::wstring tooltipClassName = L"DestoItemTooltip";
    std::wstring lifecycleClassName = L"DestoShellLifecycleHost";
    HINSTANCE module = nullptr;
    bool windowClassRegistered = false;
    bool guideWindowClassRegistered = false;
    bool tooltipWindowClassRegistered = false;
    bool lifecycleWindowClassRegistered = false;
    bool closeRequested = false;
    bool cardsGloballyVisible = true;
    bool pinnedCardsYieldToFullscreen = true;
    bool showIconBackgroundFrame = false;
    bool pinnedCardsSuppressedForFullscreen = false;
    bool oleInitialized = false;
    bool sessionLocked = false;
    bool systemSuspended = false;
    bool onOriginVirtualDesktop = true;
    bool desktopShellAvailable = true;
    bool sessionNotificationsRegistered = false;
    HWND verticalGuide = nullptr;
    HWND horizontalGuide = nullptr;
    WindowsDesktopHost::PlacementChangedCallback placementChanged;
    WindowsDesktopHost::CardExpandedChangedCallback cardExpandedChanged;
    WindowsDesktopHost::CardPinChangedCallback cardPinChanged;
    WindowsDesktopHost::MappingPresentationChangedCallback mappingPresentationChanged;
    WindowsDesktopHost::ApplicationItemsDroppedCallback applicationItemsDropped;
    WindowsDesktopHost::ApplicationItemDragCompletedCallback applicationItemDragCompleted;
    WindowsDesktopHost::CardItemActivatedCallback cardItemActivated;
    WindowsDesktopHost::CardItemContextMenuCallback cardItemContextMenu;
    WindowsDesktopHost::MappingNavigateUpCallback mappingNavigateUp;
    WindowsDesktopHost::MappingReferenceRemovedCallback mappingReferenceRemoved;
    WindowsDesktopHost::FileDeleteConfirmationCallback fileDeleteConfirmation;
    WindowsDesktopHost::CardItemsRefreshCallback cardItemsRefresh;
    WindowsDesktopHost::TodoItemAddedCallback todoItemAdded;
    WindowsDesktopHost::TodoItemAddedScheduledCallback todoItemAddedScheduled;
    WindowsDesktopHost::TodoItemCompletedChangedCallback todoItemCompletedChanged;
    WindowsDesktopHost::TodoItemRemovedCallback todoItemRemoved;
    WindowsDesktopHost::TodoItemsReorderedCallback todoItemsReordered;
    WindowsDesktopHost::TodoItemsArchivedCallback todoItemsArchived;
    std::vector<Surface> surfaces;
    std::vector<domain::DisplaySnapshot> displays;
    std::optional<std::int32_t> timeZoneOffsetMinutes;
    std::string language = "zh-CN";
    HWND overlayWindow = nullptr;
    HWND desktopOwnerWindow = nullptr;
    HWND lifecycleWindow = nullptr;
    IVirtualDesktopManager* virtualDesktopManager = nullptr;
    UINT taskbarCreatedMessage = 0;
    bool repairingZOrder = false;
    bool systemAppsUseDarkTheme = SystemAppsUseDarkTheme();
    DWORD ownerThreadId = 0;
    HWINEVENTHOOK foregroundHook = nullptr;
    static inline Impl* foregroundObserver = nullptr;
    static constexpr UINT ForegroundChangedMessage = WM_APP + 0x73;
    static constexpr UINT_PTR ShellBindingRetryTimerId = 4;
    static constexpr UINT ShellBindingRetryMilliseconds = 250;
    std::mutex pendingRefreshMutex;
    std::unordered_set<domain::CardId> pendingRefreshCards;
    HWND todoEditor = nullptr;
    HWND todoEditorSurface = nullptr;
    IContextMenu2* activeShellContextMenu2 = nullptr;
    IContextMenu3* activeShellContextMenu3 = nullptr;
    RenderStatistics renderStatistics;

    static LRESULT CALLBACK LifecycleWindowProcedure(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(
                window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* instance = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (instance == nullptr) return DefWindowProcW(window, message, wParam, lParam);

        if (message == instance->taskbarCreatedMessage) {
            instance->desktopShellAvailable = false;
            instance->rebindDesktopShell();
            return 0;
        }
        switch (message) {
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            instance->refreshSystemCardTheme();
            return 0;
        case WM_WTSSESSION_CHANGE:
            if (wParam == WTS_SESSION_LOCK) instance->sessionLocked = true;
            if (wParam == WTS_SESSION_UNLOCK) instance->sessionLocked = false;
            instance->applyLifecycleVisibility();
            return 0;
        case WM_POWERBROADCAST:
            if (wParam == PBT_APMSUSPEND || wParam == PBT_APMSTANDBY) {
                instance->systemSuspended = true;
                instance->applyLifecycleVisibility();
                return TRUE;
            }
            if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND
                || wParam == PBT_APMRESUMECRITICAL) {
                instance->systemSuspended = false;
                instance->desktopShellAvailable = false;
                instance->rebindDesktopShell();
                return TRUE;
            }
            break;
        case WM_TIMER:
            if (wParam == ShellBindingRetryTimerId) {
                instance->rebindDesktopShell();
                return 0;
            }
            break;
        case WM_CLOSE:
            // Inno Setup/Restart Manager sends WM_CLOSE to applications that
            // hold files being replaced. Request the normal host shutdown so
            // configuration is persisted before the installer continues.
            instance->closeRequested = true;
            PostQuitMessage(0);
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        default:
            break;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    bool forwardShellContextMenuMessage(
        UINT message,
        WPARAM wParam,
        LPARAM lParam,
        LRESULT& result) noexcept {
        if (activeShellContextMenu3 != nullptr) {
            if (message == WM_INITMENUPOPUP || message == WM_DRAWITEM
                || message == WM_MEASUREITEM || message == WM_MENUCHAR) {
                return SUCCEEDED(activeShellContextMenu3->HandleMenuMsg2(
                    message, wParam, lParam, &result));
            }
        } else if (activeShellContextMenu2 != nullptr) {
            if (message == WM_INITMENUPOPUP || message == WM_DRAWITEM
                || message == WM_MEASUREITEM) {
                result = 0;
                return SUCCEEDED(activeShellContextMenu2->HandleMenuMsg(
                    message, wParam, lParam));
            }
        }
        return false;
    }

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
        LRESULT shellMenuResult = 0;
        if (instance != nullptr && instance->forwardShellContextMenuMessage(
                message, wParam, lParam, shellMenuResult)) {
            return shellMenuResult;
        }
        switch (message) {
        case WM_MOUSEACTIVATE:
            if (instance != nullptr) instance->keepOverlayAbove(window);
            return MA_NOACTIVATE;
        case WM_NCHITTEST:
            return instance == nullptr ? HTCLIENT : instance->hitTest(window, lParam);
        case WM_ENTERSIZEMOVE:
            if (instance != nullptr) {
                instance->finishEditorsForSurface(window);
                if (auto* surface = instance->findSurface(window); surface != nullptr) {
                    surface->nativeMoveActive = true;
                    const auto insertAfter = instance->overlayWindow != nullptr
                            && IsWindowVisible(instance->overlayWindow)
                        ? instance->overlayWindow : HWND_TOP;
                    SetWindowPos(window, insertAfter, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
                }
                instance->hideGuides();
                instance->keepOverlayAbove(window);
                return 0;
            }
            break;
        case WM_WINDOWPOSCHANGING:
            if (instance != nullptr) {
                instance->constrainCardZOrderDuringMove(
                    window, *reinterpret_cast<WINDOWPOS*>(lParam));
            }
            break;
        case WM_WINDOWPOSCHANGED:
            if (instance != nullptr) {
                const auto* surface = instance->findSurface(window);
                if (surface == nullptr || !surface->nativeMoveActive) {
                    instance->keepOverlayAbove(window);
                }
            }
            break;
        case WM_MOVING:
            if (instance != nullptr) {
                try {
                    instance->updateInteractionGuides(
                        window,
                        *reinterpret_cast<RECT*>(lParam));
                    // Reassert the relationship on every native move tick;
                    // one WM_WINDOWPOSCHANGING correction can be displaced
                    // when the pointer crosses another top-level window.
                    instance->keepOverlayAbove(window);
                } catch (...) {
                    instance->hideGuides();
                }
                return TRUE;
            }
            break;
        case WM_DPICHANGED:
            if (instance != nullptr && lParam != 0) {
                try {
                    instance->applyDpiChange(
                        window,
                        LOWORD(wParam),
                        *reinterpret_cast<const RECT*>(lParam));
                } catch (...) {
                    // Keep the last valid surface if Windows supplies an
                    // unusable target or the backing bitmap cannot be rebuilt.
                }
                return 0;
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
                if (auto* surface = instance->findSurface(window); surface != nullptr) {
                    surface->nativeMoveActive = false;
                    instance->applySurfaceLayering(*surface);
                    instance->keepOverlayAbove(window);
                }
                return 0;
            }
            break;
        case WM_LBUTTONDOWN:
            if (instance != nullptr
                && instance->beginTodoCalendarPress(
                    window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) {
                return 0;
            }
            if (instance != nullptr
                && instance->beginMappingUpPress(
                    window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) {
                return 0;
            }
            if (instance != nullptr
                && instance->beginPinPress(window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) {
                return 0;
            }
            if (instance != nullptr
                && instance->beginTodoPress(window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) {
                return 0;
            }
            if (instance != nullptr
                && instance->beginMappingViewPress(
                    window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) {
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
                && instance->beginMappingUpPress(
                    window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) {
                return 0;
            }
            if (instance != nullptr
                && instance->beginPinPress(window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) {
                return 0;
            }
            if (instance != nullptr
                && instance->beginMappingViewPress(
                    window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) {
                return 0;
            }
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
                && instance->endTodoCalendarPress(
                    window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) {
                return 0;
            }
            if (instance != nullptr
                && instance->endMappingUpPress(
                    window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) {
                return 0;
            }
            if (instance != nullptr
                && instance->endPinPress(window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) {
                return 0;
            }
            if (instance != nullptr
                && instance->endTodoPress(window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) {
                return 0;
            }
            if (instance != nullptr
                && instance->endMappingViewPress(
                    window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) {
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
        case WM_MOUSEWHEEL:
            if (instance != nullptr
                && instance->scrollCard(window, GET_WHEEL_DELTA_WPARAM(wParam))) {
                return 0;
            }
            break;
        case WM_RBUTTONUP:
            if (instance != nullptr
                && instance->removeTodoAt(window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) {
                return 0;
            }
            if (instance != nullptr
                && instance->showCardItemContextMenu(
                    window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) {
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
            if (instance != nullptr && wParam == TodoViewLongPressTimerId) {
                instance->triggerTodoViewLongPress(window);
                return 0;
            }
            break;
        case WM_CAPTURECHANGED:
            if (instance != nullptr) {
                instance->cancelTodoPress(window);
                instance->cancelTodoCalendarPress(window);
                instance->cancelMappingUpPress(window);
                instance->cancelMappingViewPress(window);
                instance->cancelPinPress(window);
                instance->cancelCollapsePress(window);
                instance->cancelItemPress(window);
                return 0;
            }
            break;
        case WM_COMMAND:
            if (instance != nullptr
                && (HIWORD(wParam) == WindowsTextInputCommitNotification
                    || HIWORD(wParam) == WindowsTextInputCancelNotification)) {
                const auto editor = reinterpret_cast<HWND>(lParam);
                const auto commit = HIWORD(wParam) == WindowsTextInputCommitNotification;
                if (editor == instance->todoEditor) {
                    instance->finishTodoEdit(editor, commit);
                    return 0;
                }
            }
            break;
        case WM_ERASEBKGND:
            return 1;
        default:
            return DefWindowProcW(window, message, wParam, lParam);
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    static void CALLBACK ForegroundEventProcedure(
        HWINEVENTHOOK, DWORD event, HWND, LONG, LONG, DWORD, DWORD) noexcept {
        if (event == EVENT_SYSTEM_FOREGROUND && foregroundObserver != nullptr) {
            PostThreadMessageW(
                foregroundObserver->ownerThreadId, ForegroundChangedMessage, 0, 0);
        }
    }

    void initialize() {
        module = GetModuleHandleW(nullptr);
        desktopOwnerWindow = FindDesktopOwnerWindow();
        if (foregroundObserver == nullptr) {
            foregroundObserver = this;
            foregroundHook = SetWinEventHook(
                EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                nullptr, &ForegroundEventProcedure, 0, 0,
                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
            if (foregroundHook == nullptr) foregroundObserver = nullptr;
        }
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

        WNDCLASSW lifecycleClass{};
        lifecycleClass.lpfnWndProc = &LifecycleWindowProcedure;
        lifecycleClass.hInstance = module;
        lifecycleClass.lpszClassName = lifecycleClassName.c_str();
        if (RegisterClassW(&lifecycleClass) == 0) {
            throw std::runtime_error("RegisterClassW failed for shell lifecycle host.");
        }
        lifecycleWindowClassRegistered = true;
        taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
        if (taskbarCreatedMessage == 0) {
            throw std::runtime_error("RegisterWindowMessageW failed for TaskbarCreated.");
        }
        lifecycleWindow = CreateWindowExW(
            WS_EX_TOOLWINDOW,
            lifecycleClassName.c_str(),
            L"DestoShellLifecycleHost",
            WS_POPUP,
            0, 0, 0, 0,
            nullptr,
            nullptr,
            module,
            this);
        if (lifecycleWindow == nullptr) {
            throw std::runtime_error("CreateWindowExW failed for shell lifecycle host.");
        }
        sessionNotificationsRegistered = WTSRegisterSessionNotification(
            lifecycleWindow, NOTIFY_FOR_THIS_SESSION) != FALSE;
        (void)CoCreateInstance(
            CLSID_VirtualDesktopManager,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&virtualDesktopManager));
        rebindDesktopShell();
    }

    bool virtualDesktopIsCurrent() noexcept {
        if (virtualDesktopManager == nullptr || lifecycleWindow == nullptr) return true;
        BOOL current = TRUE;
        if (FAILED(virtualDesktopManager->IsWindowOnCurrentVirtualDesktop(
                lifecycleWindow, &current))) {
            return true;
        }
        return current != FALSE;
    }

    bool lifecycleAllowsCards() const noexcept {
        return !sessionLocked && !systemSuspended && desktopShellAvailable
            && onOriginVirtualDesktop;
    }

    void refreshSystemCardTheme() noexcept {
        const auto nextDarkTheme = SystemAppsUseDarkTheme();
        if (nextDarkTheme == systemAppsUseDarkTheme) return;
        systemAppsUseDarkTheme = nextDarkTheme;
        for (auto& surface : surfaces) {
            if (surface.card.appearancePreset != "system") continue;
            render(surface, surface.display, surface.card, surface.ordinal);
        }
    }

    bool surfaceShouldBeVisible(const Surface& surface) noexcept {
        return cardsGloballyVisible && lifecycleAllowsCards()
            && !(surface.alwaysOnTop && pinnedCardsSuppressedForFullscreen);
    }

    void applyLifecycleVisibility() noexcept {
        onOriginVirtualDesktop = virtualDesktopIsCurrent();
        const auto allowed = lifecycleAllowsCards();
        if (!allowed) {
            hideGuides();
            if (todoEditor != nullptr) finishTodoEdit(todoEditor, false);
            for (auto& surface : surfaces) {
                clearPointerHover(surface.window, false);
            }
        }
        auto deferred = BeginDeferWindowPos(static_cast<int>(surfaces.size()));
        if (deferred == nullptr && !surfaces.empty()) return;
        for (const auto& surface : surfaces) {
            deferred = DeferWindowPos(
                deferred,
                surface.window,
                nullptr,
                0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER
                    | (surfaceShouldBeVisible(surface) ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
            if (deferred == nullptr) return;
        }
        if (!surfaces.empty()) EndDeferWindowPos(deferred);
    }

    void rebindDesktopShell() noexcept {
        const auto owner = FindDesktopOwnerWindow();
        if (owner == nullptr) {
            desktopShellAvailable = false;
            applyLifecycleVisibility();
            if (lifecycleWindow != nullptr) {
                SetTimer(lifecycleWindow, ShellBindingRetryTimerId,
                    ShellBindingRetryMilliseconds, nullptr);
            }
            return;
        }
        desktopOwnerWindow = owner;
        desktopShellAvailable = true;
        if (lifecycleWindow != nullptr) KillTimer(lifecycleWindow, ShellBindingRetryTimerId);
        for (auto& surface : surfaces) {
            if (surface.window == nullptr || !IsWindow(surface.window)) {
                if (surface.dropTarget != nullptr) {
                    surface.dropTarget->Release();
                    surface.dropTarget = nullptr;
                }
                surface.window = nullptr;
                surface.tooltip = nullptr;
                try {
                    createSurface(surface);
                    render(surface, surface.display, surface.card, surface.ordinal);
                } catch (...) {
                    surface.window = nullptr;
                    surface.tooltip = nullptr;
                }
            } else {
                applySurfaceLayering(surface);
                if (!surface.alwaysOnTop && surface.window != nullptr) {
                    const auto owner = GetWindow(surface.window, GW_OWNER);
                    if (owner != desktopOwnerWindow) {
                        SetWindowLongPtrW(surface.window, GWLP_HWNDPARENT,
                            reinterpret_cast<LONG_PTR>(desktopOwnerWindow));
                    }
                }
            }
        }
        refreshPinnedFullscreenState();
        applyLifecycleVisibility();
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

    const domain::DisplaySnapshot* displayForPointer(POINT point) const noexcept {
        for (const auto& display : displays) {
            const auto bounds = displayPixelRect(display);
            if (point.x >= bounds.left && point.x < bounds.right
                && point.y >= bounds.top && point.y < bounds.bottom) {
                return &display;
            }
        }
        return nullptr;
    }

    bool foregroundWindowIsFullscreen() const noexcept {
        const auto foreground = GetForegroundWindow();
        if (foreground == nullptr || foreground == overlayWindow || IsIconic(foreground)
            || !IsWindowVisible(foreground)) return false;
        if (foreground == todoEditor) return false;
        if (std::ranges::any_of(surfaces, [&](const Surface& surface) {
                return surface.window == foreground;
            })) return false;
        wchar_t className[64]{};
        GetClassNameW(foreground, className, 64);
        if (wcscmp(className, L"Progman") == 0 || wcscmp(className, L"WorkerW") == 0
            || wcscmp(className, L"Shell_TrayWnd") == 0
            || wcscmp(className, L"Shell_SecondaryTrayWnd") == 0) return false;
        RECT bounds{};
        if (!GetWindowRect(foreground, &bounds)) return false;
        MONITORINFO monitorInfo{.cbSize = sizeof(MONITORINFO)};
        const auto monitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTONULL);
        if (monitor == nullptr || !GetMonitorInfoW(monitor, &monitorInfo)) return false;
        constexpr LONG tolerance = 2;
        return bounds.left <= monitorInfo.rcMonitor.left + tolerance
            && bounds.top <= monitorInfo.rcMonitor.top + tolerance
            && bounds.right >= monitorInfo.rcMonitor.right - tolerance
            && bounds.bottom >= monitorInfo.rcMonitor.bottom - tolerance;
    }

    void refreshPinnedFullscreenState() noexcept {
        onOriginVirtualDesktop = virtualDesktopIsCurrent();
        const auto lifecycleVisible = lifecycleAllowsCards();
        const auto suppressed = pinnedCardsYieldToFullscreen
            && foregroundWindowIsFullscreen();
        pinnedCardsSuppressedForFullscreen = suppressed;
        for (auto& surface : surfaces) {
            if (!surface.alwaysOnTop) continue;
            if (suppressed || !cardsGloballyVisible || !lifecycleVisible) {
                ShowWindow(surface.window, SW_HIDE);
            } else {
                SetWindowPos(surface.window, HWND_TOPMOST, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW
                        | SWP_NOSENDCHANGING);
                SetWindowPos(surface.window, HWND_TOP, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW
                        | SWP_NOSENDCHANGING);
            }
        }
        applyLifecycleVisibility();
    }

    void setPinnedCardsYieldToFullscreen(bool enabled) noexcept {
        pinnedCardsYieldToFullscreen = enabled;
        if (!enabled) pinnedCardsSuppressedForFullscreen = true;
        refreshPinnedFullscreenState();
    }

    void setIconBackgroundFrameVisible(bool enabled) noexcept {
        if (showIconBackgroundFrame == enabled) return;
        showIconBackgroundFrame = enabled;
        for (auto& surface : surfaces) {
            try {
                render(surface, surface.display, surface.card, surface.ordinal, false);
                commitSurface(surface);
            } catch (...) {
            }
        }
    }

    static int dipToPixels(double value, const Surface& surface) noexcept {
        return std::max(1, static_cast<int>(std::lround(
            value * surface.display.effectiveDpi / 96.0)));
    }

    bool usesEnglish() const noexcept { return language == "en-US"; }

    std::wstring tr(std::wstring_view chinese, std::wstring_view english) const {
        return std::wstring(usesEnglish() ? english : chinese);
    }

    static HWND zOrderTarget(const Surface& surface) noexcept {
        return surface.alwaysOnTop ? HWND_TOPMOST : HWND_BOTTOM;
    }

    void applySurfaceLayering(Surface& surface) noexcept {
        SetWindowLongPtrW(surface.window, GWLP_HWNDPARENT,
            reinterpret_cast<LONG_PTR>(surface.alwaysOnTop ? nullptr : desktopOwnerWindow));
        SetWindowPos(surface.window, zOrderTarget(surface),
            0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
    }

    static bool isWindowAbove(HWND above, HWND below) noexcept {
        if (above == nullptr || below == nullptr || above == below) return false;
        for (auto current = GetWindow(below, GW_HWNDPREV);
             current != nullptr;
             current = GetWindow(current, GW_HWNDPREV)) {
            if (current == above) return true;
        }
        return false;
    }

    void keepOverlayAbove(HWND cardWindow) noexcept {
        const auto* surface = findSurface(cardWindow);
        if (surface != nullptr && surface->alwaysOnTop) return;
        if (overlayWindow == nullptr || !IsWindowVisible(overlayWindow)
            || overlayWindow == cardWindow || repairingZOrder) {
            return;
        }
        // WM_MOVING can arrive at high frequency.  Avoid issuing a new
        // deferred z-order transaction when the relationship is already
        // correct; repeatedly rewriting the stack is what caused a card to
        // flash while crossing the settings window.
        if (isWindowAbove(overlayWindow, cardWindow)) return;
        repairingZOrder = true;
        auto deferred = BeginDeferWindowPos(2);
        if (deferred != nullptr) {
            deferred = DeferWindowPos(deferred, overlayWindow, HWND_TOP,
                0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        if (deferred != nullptr) {
            deferred = DeferWindowPos(deferred, cardWindow,
                surface != nullptr && surface->nativeMoveActive
                    ? overlayWindow : HWND_BOTTOM,
                0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        if (deferred != nullptr) EndDeferWindowPos(deferred);
        repairingZOrder = false;
    }

    void constrainCardZOrderDuringMove(HWND cardWindow, WINDOWPOS& position) noexcept {
        const auto* surface = findSurface(cardWindow);
        if (surface == nullptr || (position.flags & SWP_NOZORDER) != 0) return;
        position.hwndInsertAfter = surface->nativeMoveActive
                && overlayWindow != nullptr && IsWindowVisible(overlayWindow)
            ? overlayWindow : surface->nativeMoveActive ? HWND_TOP : zOrderTarget(*surface);
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

    static RECT pinControlRect(const Surface& surface) noexcept {
        const auto size = dipToPixels(36.0, surface);
        const auto gap = dipToPixels(2.0, surface);
        const auto collapse = collapseControlRect(surface);
        if (!surface.card.showCollapseControl) return collapse;
        return {collapse.left - gap - size, collapse.top,
            collapse.left - gap, collapse.bottom};
    }

    static RECT mappingViewControlRect(const Surface& surface) noexcept {
        const auto size = dipToPixels(36.0, surface);
        const auto gap = dipToPixels(2.0, surface);
        const auto collapse = collapseControlRect(surface);
        const auto pin = surface.card.showPinControl ? pinControlRect(surface) : RECT{};
        const auto right = surface.card.showPinControl
            ? pin.left - gap
            : surface.card.showCollapseControl
            ? collapse.left - gap
            : surface.width - dipToPixels(6.0, surface);
        return {right - size, collapse.top, right, collapse.bottom};
    }

    static RECT mappingUpControlRect(const Surface& surface) noexcept {
        const auto inset = dipToPixels(6.0, surface);
        const auto size = dipToPixels(36.0, surface);
        return {inset, inset, inset + size, inset + size};
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

    static RECT todoCalendarRect(const Surface& surface) noexcept {
        const auto inset = dipToPixels(10.0, surface);
        return {inset, dipToPixels(80.0, surface),
            surface.width - inset, surface.height - inset};
    }

    static RECT todoCalendarPreviousRect(const Surface& surface) noexcept {
        const auto panel = todoCalendarRect(surface);
        return {panel.left + dipToPixels(8.0, surface),
            panel.top + dipToPixels(7.0, surface),
            panel.left + dipToPixels(40.0, surface),
            panel.top + dipToPixels(39.0, surface)};
    }

    static RECT todoCalendarNextRect(const Surface& surface) noexcept {
        const auto panel = todoCalendarRect(surface);
        return {panel.right - dipToPixels(40.0, surface),
            panel.top + dipToPixels(7.0, surface),
            panel.right - dipToPixels(8.0, surface),
            panel.top + dipToPixels(39.0, surface)};
    }

    static RECT todoCalendarDayRect(const Surface& surface, std::size_t index) noexcept {
        const auto panel = todoCalendarRect(surface);
        const auto width = std::max<LONG>(1, panel.right - panel.left);
        const auto header = dipToPixels(68.0, surface);
        const auto gridTop = panel.top + header;
        const auto gridHeight = std::max<LONG>(1, panel.bottom - gridTop - dipToPixels(8.0, surface));
        const auto column = static_cast<LONG>(index % 7);
        const auto row = static_cast<LONG>(index / 7);
        return {
            panel.left + column * width / 7,
            gridTop + row * gridHeight / 6,
            panel.left + (column + 1) * width / 7,
            gridTop + (row + 1) * gridHeight / 6,
        };
    }

    static std::chrono::sys_days todoSysDays(domain::TodoDate date) noexcept {
        return std::chrono::sys_days{
            std::chrono::year{date.year} / std::chrono::month{date.month}
                / std::chrono::day{date.day}};
    }

    static domain::TodoDate todoDateFromSysDays(std::chrono::sys_days value) noexcept {
        const std::chrono::year_month_day date{value};
        return {static_cast<int>(date.year()),
            static_cast<std::uint8_t>(static_cast<unsigned>(date.month())),
            static_cast<std::uint8_t>(static_cast<unsigned>(date.day()))};
    }

    static domain::TodoDate todoCalendarCellDate(
        const Surface& surface, std::size_t index) noexcept {
        const auto first = todoSysDays(surface.todoCalendarMonth);
        const auto weekday = std::chrono::weekday{first}.iso_encoding();
        return todoDateFromSysDays(
            first - std::chrono::days{weekday - 1} + std::chrono::days{index});
    }

    static int todoCalendarHit(const Surface& surface, int x, int y) noexcept {
        if (!surface.todoCalendarOpen) return -100;
        if (pointInside(todoCalendarPreviousRect(surface), x, y)) return -2;
        if (pointInside(todoCalendarNextRect(surface), x, y)) return -1;
        for (std::size_t index = 0; index < 42; ++index) {
            if (pointInside(todoCalendarDayRect(surface, index), x, y)) {
                return static_cast<int>(index);
            }
        }
        return pointInside(todoCalendarRect(surface), x, y) ? -3 : -4;
    }

    static std::vector<TodoDisplayEntry> todoDisplayEntries(const Surface& surface) {
        const auto today = CurrentTodoDate(surface.timeZoneOffsetMinutes);
        std::vector<TodoDisplayEntry> result;
        for (const auto& item : domain::ResolveTodoDateView(
                surface.card.todoItems, today, surface.todoAddDateOffset,
                surface.timeZoneOffsetMinutes)) {
            result.push_back({
                .showDateLabel = item.overdue || item.archived,
                .archived = item.archived,
                .itemIndex = item.index,
                .date = item.date,
            });
        }
        return result;
    }

    static std::size_t todoDateActivity(
        const Surface& surface, domain::TodoDate date) noexcept {
        const auto today = CurrentTodoDate(surface.timeZoneOffsetMinutes);
        std::size_t count = 0;
        for (const auto& item : surface.card.todoItems) {
            const auto scheduled = item.scheduledDate.value_or(today);
            if (!domain::IsTodoItemArchived(
                    item, today, surface.timeZoneOffsetMinutes)) {
                if (scheduled == date) ++count;
                continue;
            }
            const auto timestamp = item.completedAtUnixMilliseconds > 0
                ? item.completedAtUnixMilliseconds : item.createdAtUnixMilliseconds;
            const auto archiveDate = timestamp > 0
                ? domain::TodoDateAtUnixMilliseconds(
                    timestamp, surface.timeZoneOffsetMinutes)
                : scheduled;
            if (archiveDate == date || scheduled == date) ++count;
        }
        return count;
    }

    static double todoRowHeightDip(const Surface& surface, std::size_t itemIndex) noexcept {
        if (itemIndex >= surface.card.todoItems.size()) return 42.0;
        try {
            const auto title = Utf8ToWide(surface.card.todoItems[itemIndex].title);
            const auto scale = surface.display.effectiveDpi / 96.0;
            const auto dc = GetDC(nullptr);
            if (dc == nullptr) return 42.0;
            const auto maximumWidth = std::max(1, static_cast<int>(std::lround(
                std::max(88.0, surface.projection.rect.width - 76.0) * scale)));
            const auto font = CreateFontW(
                -std::max(1, static_cast<int>(std::lround(13.0 * scale))), 0, 0, 0,
                FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                ResolveTodoTextFontFamily(title).data());
            const auto previous = font == nullptr ? nullptr : SelectObject(dc, font);
            RECT measured{0, 0, maximumWidth, 0};
            DrawTextW(dc, title.c_str(), -1, &measured,
                DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
            if (previous != nullptr) SelectObject(dc, previous);
            if (font != nullptr) DeleteObject(font);
            ReleaseDC(nullptr, dc);
            const auto today = CurrentTodoDate(surface.timeZoneOffsetMinutes);
            const auto& item = surface.card.todoItems[itemIndex];
            const auto historical = surface.todoAddDateOffset != 0
                && surface.todoAddDateOffset != 1;
            const auto showDateLabel = (surface.todoAddDateOffset == 0
                    && domain::CompareTodoDates(item.scheduledDate.value_or(today), today) < 0)
                || (historical && domain::IsTodoItemArchived(
                    item, today, surface.timeZoneOffsetMinutes));
            const auto showMetadata = showDateLabel
                || surface.card.todoPreferences.showCreatedTime;
            return std::max(42.0, measured.bottom / scale + 12.0
                + (showMetadata ? 16.0 : 0.0));
        } catch (...) {
            return 42.0;
        }
    }

    static RECT todoEntryRect(const Surface& surface, std::size_t entryIndex) noexcept {
        if (entryIndex < surface.scrollRowOffset) return {0, -1, 0, -1};
        const auto rowLimit = visibleRowLimit(surface);
        if (rowLimit.has_value()
            && entryIndex >= surface.scrollRowOffset + *rowLimit) {
            return {0, surface.height, 0, surface.height};
        }
        const auto inset = dipToPixels(10.0, surface);
        LONG top = dipToPixels(128.0, surface);
        const auto entries = todoDisplayEntries(surface);
        for (std::size_t index = 0; index < entryIndex && index < entries.size(); ++index) {
            top += dipToPixels(todoRowHeightDip(surface, entries[index].itemIndex), surface);
        }
        for (std::size_t index = 0;
             index < surface.scrollRowOffset && index < entries.size(); ++index) {
            top -= dipToPixels(todoRowHeightDip(surface, entries[index].itemIndex), surface);
        }
        const auto height = dipToPixels(entries.size() > entryIndex
            ? todoRowHeightDip(surface, entries[entryIndex].itemIndex) : 42.0, surface);
        return {inset, top, surface.width - inset, top + height};
    }

    static std::vector<RECT> todoEntryRects(
        const Surface& surface,
        std::span<const TodoDisplayEntry> entries) {
        std::vector<RECT> result;
        result.reserve(entries.size());
        for (std::size_t index = 0; index < entries.size(); ++index) {
            result.push_back(todoEntryRect(surface, index));
        }
        return result;
    }

    static RECT todoRowRect(const Surface& surface, std::size_t itemIndex) noexcept {
        const auto entries = todoDisplayEntries(surface);
        for (std::size_t index = 0; index < entries.size(); ++index) {
            if (entries[index].itemIndex == itemIndex) {
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

    static void fitItemLayoutToWidthSpan(
        const Surface& surface,
        presentation::CardContentLayoutSettings& settings) noexcept {
        if (surface.card.content.sizeMode == domain::CardSizeMode::Adaptive) {
            const auto fittedWidthSpan = presentation::ResolveCardWidthSpanForColumns(
                settings.preferredColumns, surface.card.content.itemSize);
            settings.widthSpan = surface.card.applicationSortMode
                    == domain::ApplicationItemSortMode::Custom
                ? fittedWidthSpan
                : std::max(surface.card.content.widthSpan, fittedWidthSpan);
        } else {
            settings.widthSpan = surface.card.content.widthSpan;
        }
    }

    static presentation::CardContentLayoutSettings baseItemLayoutSettings(
        const Surface& surface) noexcept {
        auto settings = presentation::ResolveCardContentLayoutSettings(surface.card.content);
        if (surface.card.content.sizeMode == domain::CardSizeMode::Fixed) {
            settings.preferredColumns = presentation::ResolveCardColumnsForWidthSpan(
                surface.card.content.widthSpan, surface.card.content.itemSize);
            settings.minimumColumns = settings.preferredColumns;
            settings.maximumColumns = settings.preferredColumns;
        } else {
            std::size_t preservedColumns = 0;
            for (const auto& placement : surface.card.applicationItemPlacements) {
                preservedColumns = std::max<std::size_t>(
                    preservedColumns, placement.column + 1);
            }
            if (surface.card.applicationSortMode == domain::ApplicationItemSortMode::Custom) {
                settings.preferredColumns = presentation::ResolveCustomAdaptiveCardColumns(
                    preservedColumns, surface.card.content.itemSize, settings);
            } else {
                settings.preferredColumns = presentation::ResolveSortedAdaptiveCardColumns(
                    preservedColumns, settings);
            }
        }
        fitItemLayoutToWidthSpan(surface, settings);
        return settings;
    }

    static presentation::CardContentLayoutSettings itemLayoutSettings(
        const Surface& surface) noexcept {
        auto settings = baseItemLayoutSettings(surface);
        if ((surface.card.type == domain::CardType::Application
                || surface.card.type == domain::CardType::Mapping)
            && surface.card.mappingPresentationMode
                == domain::MappingPresentationMode::List) {
            const auto scale = surface.display.effectiveDpi / 96.0;
            const auto widthDip = scale <= 0.0
                ? surface.projection.rect.width : surface.width / scale;
            settings.itemWidth = std::max(156.0, widthDip - settings.horizontalPadding * 2.0);
            settings.itemHeight = 42.0;
            settings.iconSize = 26.0;
            settings.itemFontSize = 11.0;
            settings.preferredColumns = 1;
            settings.minimumColumns = 1;
            settings.maximumColumns = 1;
        }
        if (surface.card.content.sizeMode != domain::CardSizeMode::Fixed
            && !((surface.card.type == domain::CardType::Application
                    || surface.card.type == domain::CardType::Mapping)
                && surface.card.mappingPresentationMode
                    == domain::MappingPresentationMode::List)) {
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
            fitItemLayoutToWidthSpan(surface, settings);
        }
        return settings;
    }

    static std::size_t contentSlotCount(
        const Surface& surface,
        bool includeDropPreview = true) noexcept {
        const auto listPresentation = (surface.card.type == domain::CardType::Application
                || surface.card.type == domain::CardType::Mapping)
            && surface.card.mappingPresentationMode
                == domain::MappingPresentationMode::List;
        if (listPresentation) {
            auto result = surface.card.items.size();
            if (includeDropPreview && surface.dropInsertionIndex.has_value()) {
                result = std::max(result, *surface.dropInsertionIndex + 1);
            }
            return result;
        }
        if (surface.card.content.sizeMode == domain::CardSizeMode::Fixed) {
            return itemLayoutSettings(surface).preferredColumns
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

    static std::optional<std::size_t> visibleRowLimit(
        const Surface& surface) noexcept {
        auto result = surface.automaticVisibleRows;
        if (surface.card.content.maximumVisibleRows.has_value()) {
            const auto configured = static_cast<std::size_t>(
                *surface.card.content.maximumVisibleRows);
            result = result.has_value()
                ? std::optional<std::size_t>{std::min(*result, configured)}
                : std::optional<std::size_t>{configured};
        }
        return result;
    }

    double reservedFollowerHeightDip(
        const Surface& leader,
        std::unordered_set<domain::PlacementId>& visited) const noexcept {
        if (!visited.insert(leader.projection.placementId).second) return 0.0;
        double maximumBranchHeight = 0.0;
        for (const auto& follower : surfaces) {
            if (!follower.verticalLeaderPlacementId.has_value()
                || *follower.verticalLeaderPlacementId != leader.projection.placementId
                || follower.projection.displayId != leader.projection.displayId) {
                continue;
            }
            const auto branchHeight = 8.0 + visibleHeightDip(follower)
                + reservedFollowerHeightDip(follower, visited);
            maximumBranchHeight = std::max(maximumBranchHeight, branchHeight);
        }
        return maximumBranchHeight;
    }

    void updateAutomaticContentConstraint(Surface& surface) noexcept {
        surface.automaticVisibleRows.reset();
        surface.automaticMaximumHeightDip.reset();

        std::unordered_set<domain::PlacementId> visited;
        const auto followerHeight = reservedFollowerHeightDip(surface, visited);
        const auto availableHeight = std::clamp(
            surface.display.workAreaHeight - surface.projection.rect.top - followerHeight,
            48.0,
            surface.display.workAreaHeight);
        surface.automaticMaximumHeightDip = availableHeight;

        if (surface.card.type != domain::CardType::Application
            && surface.card.type != domain::CardType::Mapping) {
            return;
        }

        const auto settings = itemLayoutSettings(surface);
        const auto columns = std::max<std::size_t>(1, settings.preferredColumns);
        const auto slotCount = contentSlotCount(surface);
        const auto layoutItemCount = slotCount == 0 ? std::size_t{1} : slotCount;
        const auto totalRows = std::max<std::size_t>(
            1, (layoutItemCount + columns - 1) / columns);
        const auto configuredRows = surface.card.content.maximumVisibleRows.has_value()
            ? std::min<std::size_t>(
                totalRows, *surface.card.content.maximumVisibleRows)
            : totalRows;

        std::size_t fittingRows = 0;
        const auto width = surface.card.mappingPresentationMode
                == domain::MappingPresentationMode::List
            ? presentation::ResolveCardOuterWidth(settings.widthSpan)
            : presentation::ResolveFileCardOuterWidth(columns, settings);
        for (std::size_t rows = 1; rows <= configuredRows; ++rows) {
            const auto visibleItems = std::min(layoutItemCount, rows * columns);
            const auto layout = presentation::ResolveCardContentLayout(
                visibleItems, width, settings);
            if (std::max(120.0, layout.idealHeight) > availableHeight + 0.01) break;
            fittingRows = rows;
        }
        surface.automaticVisibleRows = std::max<std::size_t>(1, fittingRows);
    }

    static DWORD acceptedDropEffect(
        const Surface& surface,
        DWORD allowedEffect,
        DWORD keyState = 0,
        const std::optional<std::string>& sourceCardId = std::nullopt) noexcept {
        if (surface.card.positionLocked) return DROPEFFECT_NONE;
        const auto sameCardSource = sourceCardId.has_value()
            && *sourceCardId == surface.card.id;
        const auto defaultEffect = ResolveFileCardDropEffect(
            surface.card.type,
            surface.card.mappingMode,
            surface.card.mappingHasSource,
            surface.card.mappingAllowsSourceMutation,
            sameCardSource,
            allowedEffect,
            surface.card.mappingCanNavigateUp);
        if (defaultEffect == DROPEFFECT_MOVE
            && !sameCardSource
            && (keyState & MK_CONTROL) != 0
            && (allowedEffect & DROPEFFECT_COPY) != 0) {
            return DROPEFFECT_COPY;
        }
        return defaultEffect;
    }

    static std::size_t maximumScrollOffset(const Surface& surface) noexcept {
        const auto rowLimit = visibleRowLimit(surface);
        if (!rowLimit.has_value()) return 0;
        const auto visibleRows = *rowLimit;
        if (surface.card.type == domain::CardType::Todo) {
            const auto count = todoDisplayEntries(surface).size();
            return count > visibleRows ? count - visibleRows : 0;
        }
        const auto columns = std::max<std::size_t>(
            1, itemLayoutSettings(surface).preferredColumns);
        const auto slots = contentSlotCount(surface, false);
        const auto rows = (slots + columns - 1) / columns;
        return rows > visibleRows ? rows - visibleRows : 0;
    }

    bool scrollCard(HWND window, int wheelDelta) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr || !surface->card.expanded
            || !visibleRowLimit(*surface).has_value()) return false;
        const auto maximum = maximumScrollOffset(*surface);
        const auto previous = surface->scrollRowOffset;
        if (wheelDelta > 0) {
            if (surface->scrollRowOffset > 0) --surface->scrollRowOffset;
        } else if (wheelDelta < 0) {
            surface->scrollRowOffset = std::min(maximum, surface->scrollRowOffset + 1);
        }
        if (surface->scrollRowOffset != previous) {
            if (surface->dropDragActive) {
                POINT cursor{};
                if (GetCursorPos(&cursor)) {
                    (void)updateDropPreview(
                        window,
                        {cursor.x, cursor.y},
                        surface->dropAllowedEffect,
                        surface->dropKeyState,
                        surface->dropSourceCardId);
                }
            } else {
                try { render(*surface, surface->display, surface->card, surface->ordinal); } catch (...) {}
            }
        }
        return maximum > 0;
    }

    domain::PlacementRect contentDrivenRect(const Surface& surface) const {
        if (surface.card.type == domain::CardType::Todo) {
            auto rect = surface.projection.rect;
            const auto right = rect.left + rect.width;
            const auto horizontalCenter = rect.left + rect.width / 2.0;
            const auto bottom = rect.top + rect.height;
            const auto verticalCenter = rect.top + rect.height / 2.0;
            rect.width = std::min(
                presentation::ResolveTodoCardOuterWidth(surface.card.content.widthSpan),
                surface.display.workAreaWidth);
            const auto entries = todoDisplayEntries(surface);
            constexpr double twoLargeIconRowsHeight = 174.0;
            double contentHeight = 132.0;
            const auto rowLimit = visibleRowLimit(surface);
            if (rowLimit.has_value() && !entries.empty()) {
                const auto visibleRows = std::min<std::size_t>(
                    *rowLimit, entries.size());
                double maximumWindowHeight = 0.0;
                for (std::size_t start = 0; start + visibleRows <= entries.size(); ++start) {
                    double windowHeight = 0.0;
                    for (std::size_t index = start; index < start + visibleRows; ++index) {
                        windowHeight += todoRowHeightDip(surface, entries[index].itemIndex);
                    }
                    maximumWindowHeight = std::max(maximumWindowHeight, windowHeight);
                }
                contentHeight += maximumWindowHeight;
            } else {
                for (const auto& entry : entries) {
                    contentHeight += todoRowHeightDip(surface, entry.itemIndex);
                }
            }
            if (entries.empty()) contentHeight = twoLargeIconRowsHeight;
            if (surface.todoCalendarOpen) contentHeight = std::max(contentHeight, 380.0);
            rect.height = std::min(
                std::max(twoLargeIconRowsHeight, contentHeight),
                surface.automaticMaximumHeightDip.value_or(
                    surface.display.workAreaHeight));
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
        const auto listPresentation = (surface.card.type == domain::CardType::Application
                || surface.card.type == domain::CardType::Mapping)
            && surface.card.mappingPresentationMode
                == domain::MappingPresentationMode::List;
        const auto width = listPresentation
            ? presentation::ResolveCardOuterWidth(settings.widthSpan)
            : presentation::ResolveFileCardOuterWidth(columns, settings);
        const auto slotCount = contentSlotCount(surface);
        const auto layoutItemCount = (surface.card.type == domain::CardType::Application
                || surface.card.type == domain::CardType::Mapping)
                && slotCount == 0
            ? std::size_t{1}
            : slotCount;
        const auto rowLimit = visibleRowLimit(surface);
        const auto visibleItemCount = rowLimit.has_value()
            ? std::min(
                layoutItemCount,
                *rowLimit * columns)
            : layoutItemCount;
        const auto layout = presentation::ResolveCardContentLayout(
            visibleItemCount, width, settings);
        auto rect = surface.projection.rect;
        const auto right = rect.left + rect.width;
        const auto horizontalCenter = rect.left + rect.width / 2.0;
        const auto bottom = rect.top + rect.height;
        const auto verticalCenter = rect.top + rect.height / 2.0;
        rect.width = std::min(width, surface.display.workAreaWidth);
        rect.height = std::min(
            std::max(120.0, layout.idealHeight),
            surface.automaticMaximumHeightDip.value_or(
                surface.display.workAreaHeight));
        if (surface.dropPreviewColumns.has_value() || surface.dropCommitInProgress) {
            rect = presentation::ResolveAdaptiveDropExpansionRect(
                surface.dropPreviewOriginRect.value_or(surface.projection.rect),
                rect.width,
                rect.height,
                surface.projection.horizontalAnchor);
        } else {
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
        }
        auto placementSettings = presentation::SnapSettings{};
        if (surface.card.content.sizeMode == domain::CardSizeMode::Fixed) {
            placementSettings.minimumWidth = width;
        }
        return presentation::ResolvePlacementInteraction(
            rect,
            surface.display.workAreaWidth,
            surface.display.workAreaHeight,
            {},
            true,
            placementSettings);
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
        if (row < surface.scrollRowOffset) return {0, -1, 0, -1};
        const auto rowLimit = visibleRowLimit(surface);
        if (rowLimit.has_value()
            && row >= surface.scrollRowOffset + *rowLimit) {
            return {0, surface.height, 0, surface.height};
        }
        const auto settings = itemLayoutSettings(surface);
        const auto layout = itemLayout(surface, slotCount);
        const auto scale = surface.display.effectiveDpi / 96.0;
        const auto widthDip = surface.width / scale;
        const auto contentLeft = (widthDip - layout.contentWidth) / 2.0;
        const auto left = contentLeft + column * (settings.itemWidth + settings.horizontalGap);
        const auto top = settings.headerHeight + settings.verticalPadding
            + (static_cast<double>(row) - static_cast<double>(surface.scrollRowOffset))
                * (settings.itemHeight + settings.verticalGap);
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
                surface.card.type == domain::CardType::Mapping
                    ? item.sourcePath : item.sourcePath.filename(),
                item.displayName, item.fileSize,
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
                    const auto key = surface.card.type == domain::CardType::Mapping
                        ? item.sourcePath : item.sourcePath.filename();
                    return _wcsicmp(key.c_str(), projected.fileName.c_str()) == 0;
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
            return itemLayoutSettings(surface).preferredColumns
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

    struct ItemNameLayout {
        int iconLeft = 0;
        int iconTop = 0;
        RECT label{};
    };

    static ItemNameLayout resolveItemNameLayout(
        const Surface& surface,
        std::size_t column,
        std::size_t row,
        std::size_t slotCount,
        int visibleBottom,
        bool iconFrame) {
        const auto slot = itemRect(surface, column, row, slotCount);
        const auto settings = itemLayoutSettings(surface);
        const auto scale = surface.display.effectiveDpi / 96.0;
        const auto iconSize = std::max(
            1, static_cast<int>(std::lround(settings.iconSize * scale)));
        const auto listPresentation = (surface.card.type == domain::CardType::Application
                || surface.card.type == domain::CardType::Mapping)
            && surface.card.mappingPresentationMode
                == domain::MappingPresentationMode::List;
        const auto iconRegionSize = std::max(
            1, static_cast<int>(std::lround(
                (listPresentation ? settings.itemHeight : settings.itemWidth) * scale)));
        const auto framedNames = iconFrame && !listPresentation
            && surface.card.content.showItemNames;
        const auto framePad = dipToPixels(framedNames ? 6.0 : 0.0, surface);
        const auto labelGap = dipToPixels(
            listPresentation ? 10.0 : framedNames ? 4.0 : 3.0, surface);
        const auto iconLeft = listPresentation
            ? slot.left + dipToPixels(8.0, surface)
            : slot.left + ((slot.right - slot.left) - iconSize) / 2;
        int iconTop = slot.top + (iconRegionSize - iconSize) / 2;
        if (framedNames) {
            const auto line = dipToPixels(settings.itemFontSize + 4.0, surface);
            const auto block = iconSize + labelGap + line;
            const auto innerTop = slot.top + framePad;
            const auto innerBottom = slot.bottom - framePad;
            iconTop = innerTop + std::max(0,
                static_cast<int>(innerBottom - innerTop - block) / 2);
            return {
                .iconLeft = iconLeft,
                .iconTop = iconTop,
                .label = RECT{
                    slot.left + framePad,
                    iconTop + iconSize + labelGap,
                    slot.right - framePad,
                    std::min<LONG>(iconTop + iconSize + labelGap + line, visibleBottom),
                },
            };
        }
        return {
            .iconLeft = iconLeft,
            .iconTop = iconTop,
            .label = listPresentation
                ? RECT{iconLeft + iconSize + labelGap, slot.top,
                    slot.right - dipToPixels(8.0, surface),
                    std::min<LONG>(slot.bottom, visibleBottom)}
                : RECT{slot.left, iconTop + iconSize + labelGap, slot.right,
                    std::min<LONG>(slot.bottom, visibleBottom)},
        };
    }

    static RECT itemLabelRect(
        const Surface& surface,
        std::size_t column,
        std::size_t row,
        std::size_t slotCount,
        int visibleBottom,
        bool iconFrame) {
        return resolveItemNameLayout(
            surface, column, row, slotCount, visibleBottom, iconFrame).label;
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
            if (pointInside(rects[index], x, y)) {
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

    bool isCollapseControlHit(const Surface& surface, int x, int y) const noexcept {
        return surface.card.showCollapseControl
            && pointInside(collapseControlRect(surface), x, y);
    }

    static bool isPinControlHit(const Surface& surface, int x, int y) noexcept {
        return surface.card.showPinControl
            && pointInside(pinControlRect(surface), x, y);
    }

    static bool isMappingViewControlHit(const Surface& surface, int x, int y) noexcept {
        return (surface.card.type == domain::CardType::Application
                || surface.card.type == domain::CardType::Mapping)
            && surface.card.showPresentationControl
            && pointInside(mappingViewControlRect(surface), x, y);
    }

    static bool isMappingUpControlHit(const Surface& surface, int x, int y) noexcept {
        return surface.card.type == domain::CardType::Mapping
            && surface.card.mappingCanNavigateUp
            && pointInside(mappingUpControlRect(surface), x, y);
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
            && !isPinControlHit(*surface, x, y)
            && !isMappingViewControlHit(*surface, x, y)
            && !isMappingUpControlHit(*surface, x, y)
            && !isTodoArchiveControlHit(*surface, x, y)
            && !isTodoViewControlHit(*surface, x, y)
            && !isTodoAddControlHit(*surface, x, y)) {
            return surface->card.positionLocked ? HTCLIENT : HTCAPTION;
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
        // Windows owns cross-monitor DPI switching. WM_DPICHANGED updates the
        // active display and applies its suggested pointer-preserving rect.
        // WM_MOVING only computes guides in that already-selected coordinate
        // system and never initiates another scale transition.
        const auto target = moved->display;
        const auto scale = target.effectiveDpi / 96.0;
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

    void applyDpiChange(HWND window, UINT dpi, const RECT& suggestedRect) {
        auto* moved = findSurface(window);
        if (moved == nullptr || dpi == 0
            || suggestedRect.right <= suggestedRect.left
            || suggestedRect.bottom <= suggestedRect.top) {
            return;
        }
        const auto* target = displayForWindowRect(suggestedRect, moved->display.id);
        if (target == nullptr) return;

        moved->display = *target;
        moved->projection.displayId = target->id;
        const auto width = suggestedRect.right - suggestedRect.left;
        const auto height = suggestedRect.bottom - suggestedRect.top;
        replaceBitmap(*moved, width, height);
        render(*moved, moved->display, moved->card, moved->ordinal, false);
        commitSurfaceAt(*moved, {suggestedRect.left, suggestedRect.top});
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

    static double legacyExpandedHeightDip(const Surface& surface) noexcept {
        if (surface.card.type != domain::CardType::Todo) {
            return surface.projection.rect.height;
        }
        const auto entries = todoDisplayEntries(surface);
        if (entries.empty()) return 206.0;
        double height = 136.0;
        for (const auto& entry : entries) {
            height += todoRowHeightDip(surface, entry.itemIndex);
        }
        return std::max(178.0, height);
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
            const auto contentExpandedTop = candidate.projection.rect.top
                + contentDrivenRect(candidate).height + visualGap;
            const auto visibleTop = candidate.projection.rect.top
                + visibleHeightDip(candidate) + visualGap;
            const auto legacyExpandedTop = candidate.projection.rect.top
                + legacyExpandedHeightDip(candidate) + visualGap;
            if (std::abs(follower.projection.rect.top - expandedTop) <= tolerance
                || std::abs(follower.projection.rect.top - contentExpandedTop) <= tolerance
                || std::abs(follower.projection.rect.top - visibleTop) <= tolerance
                || std::abs(follower.projection.rect.top - legacyExpandedTop) <= tolerance) {
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
                        const auto scale = follower.display.effectiveDpi / 96.0;
                        const auto left = static_cast<int>(std::lround(
                            follower.display.workAreaLeft * scale
                            + follower.projection.rect.left * scale));
                        const auto top = static_cast<int>(std::lround(
                            follower.display.workAreaTop * scale
                            + follower.projection.rect.top * scale));
                        SetWindowPos(
                            follower.window,
                            nullptr,
                            left,
                            top,
                            0,
                            0,
                            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
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

    static bool samePlacementRect(
        const domain::PlacementRect& left,
        const domain::PlacementRect& right) noexcept {
        constexpr double tolerance = 0.01;
        return std::abs(left.left - right.left) <= tolerance
            && std::abs(left.top - right.top) <= tolerance
            && std::abs(left.width - right.width) <= tolerance
            && std::abs(left.height - right.height) <= tolerance;
    }

    static domain::PlacementRect contentUpdatePreviousRect(
        const Surface& surface) noexcept {
        if (surface.dropCommitInProgress && surface.dropPreviewOriginRect.has_value()) {
            return *surface.dropPreviewOriginRect;
        }
        return surface.projection.rect;
    }

    void notifyPlacementChanged(Surface& surface) noexcept {
        if (!placementChanged) return;
        try {
            placementChanged(
                surface.projection.placementId,
                surface.projection.cardId,
                surface.projection.displayId,
                surface.projection.rect,
                surface.projection.horizontalAnchor,
                surface.projection.verticalAnchor,
                surface.display.workAreaWidth,
                surface.display.workAreaHeight);
        } catch (...) {
        }
    }

    void commitContentUpdate(
        Surface& surface,
        const domain::PlacementRect& previousRect) {
        commitSurface(surface);
        if (!samePlacementRect(previousRect, surface.projection.rect)) {
            notifyPlacementChanged(surface);
        }
        if (surface.dropCommitInProgress) {
            surface.dropCommitContentApplied = true;
        }
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
    }

    void finishTodoEdit(HWND editor, bool commit) noexcept {
        if (editor == nullptr || editor != todoEditor) return;
        const auto surfaceWindow = todoEditorSurface;
        todoEditor = nullptr;
        todoEditorSurface = nullptr;
        auto text = commit ? WindowsTextInputText(editor) : std::wstring{};
        RemovePropW(editor, L"DestoTodoEditorSurface");
        DestroyWindow(editor);
        if (auto* surface = findSurface(surfaceWindow); surface != nullptr) {
            try {
                render(*surface, surface->display, surface->card, surface->ordinal);
            } catch (...) {
            }
        }
        if (!commit || text.empty() || surfaceWindow == nullptr) return;
        auto* surface = findSurface(surfaceWindow);
        if (surface == nullptr) return;
        try {
            const auto utf8 = WideToUtf8(text);
            bool accepted = false;
            if (todoItemAddedScheduled) {
                const auto added = todoItemAddedScheduled(
                    surface->card.id,
                    utf8,
                    domain::AddTodoDays(
                        CurrentTodoDate(surface->timeZoneOffsetMinutes),
                        surface->todoAddDateOffset));
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

    void beginTodoEdit(HWND window) noexcept {
        try {
        auto* surface = findSurface(window);
        if (surface == nullptr || surface->card.type != domain::CardType::Todo
            || todoEditor != nullptr) return;
        const auto anchor = todoAddControlRect(*surface);
        const auto width = static_cast<int>(std::max<LONG>(
            dipToPixels(100.0, *surface), anchor.right - anchor.left));
        const auto height = static_cast<int>(anchor.bottom - anchor.top);
        POINT editorOrigin{
            anchor.left,
            anchor.top,
        };
        if (!ClientToScreen(window, &editorOrigin)) return;
        const auto dark = surface->card.appearancePreset == "mica-dark"
            || surface->card.appearancePreset == "transparent-black"
            || surface->card.appearancePreset == "black"
            || surface->card.appearancePreset == "dark"
            || (surface->card.appearancePreset == "system" && systemAppsUseDarkTheme);
        const auto textColor = dark ? RGB(236, 238, 242) : RGB(45, 49, 57);
        const auto brand = surface->card.appearancePreset == "brand"
            || surface->card.appearancePreset == "jewel"
            || surface->card.appearancePreset == "pearl-pink";
        const auto transparent = surface->card.appearancePreset == "transparent-white";
        WindowsTextInputStyle style;
        style.background = transparent ? RGB(0, 0, 0) : dark ? RGB(32, 33, 36)
            : brand ? RGB(237, 242, 255) : RGB(243, 243, 243);
        style.outline = transparent ? RGB(150, 174, 201)
            : dark ? RGB(67, 70, 77) : brand ? RGB(188, 199, 229) : RGB(207, 210, 216);
        style.focusedOutline = transparent ? RGB(112, 160, 216) : RGB(90, 153, 235);
        style.text = textColor;
        style.placeholder = dark ? RGB(166, 171, 180)
            : transparent ? RGB(83, 93, 107) : RGB(112, 117, 126);
        style.selection = RGB(70, 118, 196);
        style.compositionUnderline = RGB(45, 110, 205);
        style.backgroundAlpha = transparent ? 0 : 255;
        style.outlineAlpha = transparent ? 210 : 255;
        style.cornerRadius = 10.0F;
        style.paddingLeft = 8.0F;
        style.paddingRight = 8.0F;
        style.fontSize = static_cast<float>(dipToPixels(13.0, *surface));
        style.fontFamily = L"Segoe UI Emoji";
        auto editor = CreateWindowsTextInput({
            .notificationWindow = window,
            .bounds = RECT{editorOrigin.x, editorOrigin.y,
                editorOrigin.x + width, editorOrigin.y + height},
            .popup = true,
            .commitOnFocusLoss = true,
            .maximumLength = 512,
            .placeholder = tr(L"待办内容", L"Task"),
            .style = std::move(style),
        });
        if (editor == nullptr) return;
        SetPropW(editor, L"DestoTodoEditorSurface", window);
        todoEditor = editor;
        todoEditorSurface = window;
        render(*surface, surface->display, surface->card, surface->ordinal);
        FocusWindowsTextInput(editor);
        SetWindowPos(editor, HWND_BOTTOM, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetWindowPos(window, editor, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        } catch (...) {
        }
    }

    void finishEditorsForSurface(HWND surfaceWindow) noexcept {
        if (surfaceWindow == nullptr) return;
        if (todoEditorSurface == surfaceWindow && todoEditor != nullptr) {
            finishTodoEdit(todoEditor, false);
        }
    }

    void finishEditorsForCard(const domain::CardId& cardId) noexcept {
        if (cardId.empty()) return;
        if (todoEditorSurface != nullptr) {
            const auto* surface = findSurface(todoEditorSurface);
            if (surface != nullptr && surface->card.id == cardId) {
                finishTodoEdit(todoEditor, false);
            }
        }
    }


    bool beginTodoPress(HWND window, int x, int y) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr || surface->card.type != domain::CardType::Todo) return false;
        if (isTodoViewControlHit(*surface, x, y)) {
            surface->todoViewPressOriginalDateOffset = surface->todoAddDateOffset;
            surface->todoViewPressed = true;
            surface->todoViewLongPressTriggered = false;
            surface->todoViewHovered = true;
            SetCapture(window);
            SetTimer(window, TodoViewLongPressTimerId,
                TodoViewLongPressDelayMilliseconds, nullptr);
            try {
                render(*surface, surface->display, surface->card, surface->ordinal);
            } catch (...) {}
            return true;
        }
        if (isTodoArchiveControlHit(*surface, x, y)) {
            surface->todoArchivePressed = true;
            surface->todoArchiveHovered = true;
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
            beginTodoEdit(window);
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

    void triggerTodoViewLongPress(HWND window) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr) return;
        KillTimer(window, TodoViewLongPressTimerId);
        if (!surface->todoViewPressed
            || surface->todoViewLongPressTriggered
            || !surface->todoViewHovered || GetCapture() != window) {
            return;
        }
        surface->todoViewLongPressTriggered = true;
        surface->todoViewPressed = false;
        surface->todoViewHovered = false;
        if (GetCapture() == window) ReleaseCapture();
        const auto control = todoViewControlRect(*surface);
        (void)openTodoCalendar(window,
            (control.left + control.right) / 2,
            (control.top + control.bottom) / 2);
    }

    bool openTodoCalendar(HWND window, int x, int y) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr || !isTodoViewControlHit(*surface, x, y)) return false;
        if (todoEditor != nullptr) finishTodoEdit(todoEditor, false);
        surface->todoCalendarOpen = true;
        const auto selected = domain::AddTodoDays(
            CurrentTodoDate(surface->timeZoneOffsetMinutes), surface->todoAddDateOffset);
        surface->todoCalendarMonth = {selected.year, selected.month, 1};
        const auto previousRect = contentUpdatePreviousRect(*surface);
        try {
            resizeSurfaceForContent(*surface, true);
            render(*surface, surface->display, surface->card, surface->ordinal, false);
            commitContentUpdate(*surface, previousRect);
        } catch (...) {}
        return true;
    }

    bool beginTodoCalendarPress(HWND window, int x, int y) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr || !surface->todoCalendarOpen) return false;
        surface->todoCalendarPressed = todoCalendarHit(*surface, x, y);
        SetCapture(window);
        try { render(*surface, surface->display, surface->card, surface->ordinal); } catch (...) {}
        return true;
    }

    bool endTodoCalendarPress(HWND window, int x, int y) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr || !surface->todoCalendarPressed.has_value()) return false;
        const auto pressedHit = *surface->todoCalendarPressed;
        const auto releasedHit = todoCalendarHit(*surface, x, y);
        surface->todoCalendarPressed.reset();
        if (GetCapture() == window) ReleaseCapture();
        if (pressedHit == releasedHit) {
            if (pressedHit == -2 || pressedHit == -1) {
                auto month = std::chrono::year{surface->todoCalendarMonth.year}
                    / std::chrono::month{surface->todoCalendarMonth.month};
                month += std::chrono::months{pressedHit == -2 ? -1 : 1};
                surface->todoCalendarMonth = {
                    static_cast<int>(month.year()),
                    static_cast<std::uint8_t>(static_cast<unsigned>(month.month())), 1};
            } else if (pressedHit >= 0 && pressedHit < 42) {
                const auto selected = todoCalendarCellDate(
                    *surface, static_cast<std::size_t>(pressedHit));
                const auto today = CurrentTodoDate(surface->timeZoneOffsetMinutes);
                surface->todoAddDateOffset = static_cast<int>(
                    (todoSysDays(selected) - todoSysDays(today)).count());
                surface->todoCalendarOpen = false;
                const auto previousRect = contentUpdatePreviousRect(*surface);
                try {
                    resizeSurfaceForContent(*surface, true);
                    render(*surface, surface->display, surface->card, surface->ordinal, false);
                    commitContentUpdate(*surface, previousRect);
                } catch (...) {}
                return true;
            } else if (pressedHit == -4) {
                surface->todoCalendarOpen = false;
                const auto previousRect = contentUpdatePreviousRect(*surface);
                try {
                    resizeSurfaceForContent(*surface, true);
                    render(*surface, surface->display, surface->card, surface->ordinal, false);
                    commitContentUpdate(*surface, previousRect);
                } catch (...) {}
                return true;
            }
        }
        try { render(*surface, surface->display, surface->card, surface->ordinal); } catch (...) {}
        return true;
    }

    void cancelTodoCalendarPress(HWND window) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr || !surface->todoCalendarPressed.has_value()) return;
        surface->todoCalendarPressed.reset();
        try { render(*surface, surface->display, surface->card, surface->ordinal); } catch (...) {}
    }

    bool updateTodoDrag(HWND window, int x, int y, WPARAM keyState) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr) return false;
        if (surface->todoViewPressed || surface->todoArchivePressed
            || surface->todoAddPressed
            || surface->pressedTodoCheckbox.has_value()) {
            if ((keyState & MK_LBUTTON) == 0) return endTodoPress(window, x, y);
            const auto viewHovered = surface->todoViewPressed
                && isTodoViewControlHit(*surface, x, y);
            const auto archiveHovered = surface->todoArchivePressed
                && isTodoArchiveControlHit(*surface, x, y);
            const auto addHovered = surface->todoAddPressed
                && isTodoAddControlHit(*surface, x, y);
            const auto checkboxHovered = surface->pressedTodoCheckbox.has_value()
                && pointInside(todoCheckboxRect(*surface, *surface->pressedTodoCheckbox), x, y);
            if (viewHovered != surface->todoViewHovered
                || archiveHovered != surface->todoArchiveHovered
                || addHovered != surface->todoAddHovered
                || (surface->pressedTodoCheckbox.has_value()
                    && checkboxHovered != (surface->hoveredTodoRow == surface->pressedTodoCheckbox))) {
                surface->todoViewHovered = viewHovered;
                surface->todoArchiveHovered = archiveHovered;
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
            for (const auto& entry : entries) visibleItems.push_back(entry.itemIndex);
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
            || surface->todoAddPressed
            || surface->pressedTodoCheckbox.has_value()) {
            const auto commitView = surface->todoViewPressed
                && isTodoViewControlHit(*surface, x, y);
            const auto longPressTriggered = surface->todoViewLongPressTriggered;
            const auto commitArchive = surface->todoArchivePressed
                && isTodoArchiveControlHit(*surface, x, y);
            const auto commitAdd = surface->todoAddPressed
                && isTodoAddControlHit(*surface, x, y);
            const auto checkbox = surface->pressedTodoCheckbox;
            const auto commitCheckbox = checkbox.has_value()
                && pointInside(todoCheckboxRect(*surface, *checkbox), x, y);
            KillTimer(window, TodoViewLongPressTimerId);
            surface->todoViewPressOriginalDateOffset.reset();
            surface->todoViewPressed = false;
            surface->todoViewLongPressTriggered = false;
            surface->todoArchivePressed = false;
            surface->todoAddPressed = false;
            surface->pressedTodoCheckbox.reset();
            if (GetCapture() == window) ReleaseCapture();
            if (commitView) {
                if (!longPressTriggered) {
                    surface->todoAddDateOffset = surface->todoAddDateOffset == 0 ? 1 : 0;
                    resizeSurfaceForContent(*surface, true);
                }
            } else if (commitArchive) {
                if (todoItemsArchived && todoItemsArchived(surface->card.id)) {
                    for (auto& item : surface->card.todoItems) {
                        if (item.completed) item.archived = true;
                    }
                    resizeSurfaceForContent(*surface, true);
                }
            } else if (commitCheckbox && *checkbox < surface->card.todoItems.size()) {
                const auto& item = surface->card.todoItems[*checkbox];
                if (todoItemCompletedChanged
                    && todoItemCompletedChanged(surface->card.id, item.id, !item.completed)) {
                    auto& updated = surface->card.todoItems[*checkbox];
                    updated.completed = !item.completed;
                    updated.completedAtUnixMilliseconds = updated.completed
                        ? UnixMillisecondsNow() : 0;
                    if (!updated.completed) updated.archived = false;
                    resizeSurfaceForContent(*surface, true);
                }
            }
            try { render(*surface, surface->display, surface->card, surface->ordinal); } catch (...) {}
            if (commitAdd) beginTodoEdit(window);
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
        try { render(*surface, surface->display, surface->card, surface->ordinal); } catch (...) {}
        return true;
    }

    void cancelTodoPress(HWND window) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr) return;
        auto changed = surface->todoViewPressed || surface->todoArchivePressed
            || surface->todoAddPressed
            || surface->pressedTodoCheckbox.has_value()
            || surface->pressedTodoRow.has_value();
        if (surface->todoViewPressOriginalDateOffset.has_value()) {
            surface->todoAddDateOffset = *surface->todoViewPressOriginalDateOffset;
            surface->todoViewPressOriginalDateOffset.reset();
            changed = true;
            try { resizeSurfaceForContent(*surface, true); } catch (...) {}
        }
        KillTimer(window, TodoViewLongPressTimerId);
        surface->todoViewPressed = false;
        surface->todoViewLongPressTriggered = false;
        surface->todoArchivePressed = false;
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
        const auto removeText = tr(L"删除", L"Delete");
        POINT point{x, y};
        ClientToScreen(window, &point);
        const std::array items{WindowsPopupMenuItem{
            1, removeText, L"", false, true, true}};
        const auto command = ShowWindowsPopupMenu(window, point, items);
        if (command == 1 && todoItemRemoved(surface->card.id, itemId)) {
            surface->card.todoItems.erase(surface->card.todoItems.begin() + static_cast<std::ptrdiff_t>(*row));
            repaintTodoCard(surface->card.id);
        }
        return true;
    }

    bool beginMappingViewPress(HWND window, int x, int y) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr || !isMappingViewControlHit(*surface, x, y)) return false;
        surface->mappingViewHovered = true;
        surface->mappingViewPressed = true;
        SetCapture(window);
        try { render(*surface, surface->display, surface->card, surface->ordinal); } catch (...) {}
        return true;
    }

    bool beginMappingUpPress(HWND window, int x, int y) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr || !isMappingUpControlHit(*surface, x, y)) return false;
        surface->mappingUpHovered = true;
        surface->mappingUpPressed = true;
        SetCapture(window);
        try { render(*surface, surface->display, surface->card, surface->ordinal); } catch (...) {}
        return true;
    }

    bool endMappingUpPress(HWND window, int x, int y) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr || !surface->mappingUpPressed) return false;
        const auto commit = isMappingUpControlHit(*surface, x, y);
        surface->mappingUpPressed = false;
        surface->mappingUpHovered = commit;
        if (GetCapture() == window) ReleaseCapture();
        if (commit && mappingNavigateUp) {
            try { mappingNavigateUp(surface->card.id); } catch (...) {}
        } else {
            try { render(*surface, surface->display, surface->card, surface->ordinal); } catch (...) {}
        }
        return true;
    }

    void cancelMappingUpPress(HWND window) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr || !surface->mappingUpPressed) return;
        surface->mappingUpPressed = false;
        try { render(*surface, surface->display, surface->card, surface->ordinal); } catch (...) {}
    }

    bool endMappingViewPress(HWND window, int x, int y) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr || !surface->mappingViewPressed) return false;
        const auto commit = isMappingViewControlHit(*surface, x, y);
        surface->mappingViewPressed = false;
        surface->mappingViewHovered = commit;
        if (GetCapture() == window) ReleaseCapture();
        if (!commit) {
            try { render(*surface, surface->display, surface->card, surface->ordinal); } catch (...) {}
            return true;
        }
        const auto next = surface->card.mappingPresentationMode
                == domain::MappingPresentationMode::Grid
            ? domain::MappingPresentationMode::List
            : domain::MappingPresentationMode::Grid;
        if (mappingPresentationChanged
            && !mappingPresentationChanged(surface->card.id, next)) {
            try { render(*surface, surface->display, surface->card, surface->ordinal); } catch (...) {}
            return true;
        }
        const auto cardId = surface->card.id;
        std::vector<std::pair<Surface*, domain::PlacementRect>> affected;
        for (auto& candidate : surfaces) {
            if (candidate.card.id != cardId) continue;
            candidate.card.mappingPresentationMode = next;
            try {
                const auto previousRect = contentUpdatePreviousRect(candidate);
                resizeSurfaceForContent(candidate, true);
                render(candidate, candidate.display, candidate.card, candidate.ordinal, false);
                affected.emplace_back(&candidate, previousRect);
            } catch (...) {}
        }
        for (auto& [candidate, previousRect] : affected) {
            try { commitContentUpdate(*candidate, previousRect); } catch (...) {}
        }
        return true;
    }

    void cancelMappingViewPress(HWND window) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr || !surface->mappingViewPressed) return;
        surface->mappingViewPressed = false;
        try { render(*surface, surface->display, surface->card, surface->ordinal); } catch (...) {}
    }

    bool beginPinPress(HWND window, int x, int y) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr || !isPinControlHit(*surface, x, y)) return false;
        surface->pinPressed = true;
        surface->pinHovered = true;
        SetCapture(window);
        try { render(*surface, surface->display, surface->card, surface->ordinal); } catch (...) {}
        return true;
    }

    bool endPinPress(HWND window, int x, int y) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr || !surface->pinPressed) return false;
        const auto commit = isPinControlHit(*surface, x, y);
        surface->pinPressed = false;
        surface->pinHovered = commit;
        if (GetCapture() == window) ReleaseCapture();
        if (commit) {
            const auto cardId = surface->card.id;
            const auto next = !surface->alwaysOnTop;
            if (!cardPinChanged || cardPinChanged(cardId, next)) {
                for (auto& candidate : surfaces) {
                    if (candidate.card.id != cardId) continue;
                    candidate.alwaysOnTop = next;
                    candidate.card.pinOnTop = next;
                    applySurfaceLayering(candidate);
                    if (!next) keepOverlayAbove(candidate.window);
                    try { render(candidate, candidate.display, candidate.card, candidate.ordinal); } catch (...) {}
                }
            }
            else {
                try { render(*surface, surface->display, surface->card, surface->ordinal); } catch (...) {}
            }
        } else {
            try { render(*surface, surface->display, surface->card, surface->ordinal); } catch (...) {}
        }
        return true;
    }

    void cancelPinPress(HWND window) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr || !surface->pinPressed) return;
        surface->pinPressed = false;
        try { render(*surface, surface->display, surface->card, surface->ordinal); } catch (...) {}
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
            if (!expanded) finishEditorsForCard(cardId);
            for (auto& candidate : surfaces) {
                if (candidate.card.id != cardId) continue;
                candidate.card.expanded = expanded;
                try {
                    render(candidate, candidate.display, candidate.card, candidate.ordinal);
                } catch (...) {
                }
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
        if (surface->card.positionLocked) {
            surface->pressedItem.reset();
            if (GetCapture() == window) ReleaseCapture();
            return true;
        }
        surface->pressedItem.reset();
        surface->itemDragActive = true;
        clearPointerHover(window);
        if (GetCapture() == window) {
            ReleaseCapture();
        }
        std::error_code rootItemError;
        const auto rootReferenceItem = surface->card.type == domain::CardType::Mapping
            && surface->card.mappingMode == domain::MappingMode::References
            && !surface->card.mappingCanNavigateUp
            && std::filesystem::exists(item.sourcePath, rootItemError)
            && !rootItemError;
        const auto allowMove = surface->card.type != domain::CardType::Mapping
            || surface->card.mappingMode != domain::MappingMode::References
            || surface->card.mappingCanNavigateUp;
        const auto result = BeginFileDrag(
            {item.sourcePath}, cardId, allowMove, !rootReferenceItem);
        if (auto* current = findSurface(window); current != nullptr) {
            current->itemDragActive = false;
        }
        if (rootReferenceItem
            && result.status == DRAGDROP_S_DROP
            && !result.completedInsideDesto
            && mappingReferenceRemoved) {
            // A root reference folder is only a mapping operation when the
            // OLE gesture actually leaves Desto. It is never exposed as a
            // shell file drop, so no external target can copy it first.
            try { (void)mappingReferenceRemoved(cardId, item); } catch (...) {}
        }
        if (result.status == DRAGDROP_S_DROP
            && !result.completedInsideDesto
            && applicationItemDragCompleted) {
            // The external target is authoritative about the final effect;
            // refresh the source projection even when it reports NONE so a
            // completed shell move cannot leave stale cached items visible.
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

    bool showCardItemContextMenu(HWND window, int x, int y) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr) return false;
        const auto index = itemAt(*surface, x, y);
        if (!index.has_value()) return false;

        const auto& item = surface->card.items[*index];
        const auto sourcePath = item.sourcePath;
        const auto removeReferenceOnly = surface->card.type == domain::CardType::Mapping
            && surface->card.mappingMode == domain::MappingMode::References
            && !surface->card.mappingCanNavigateUp;
        POINT screenPoint{x, y};
        if (!ClientToScreen(window, &screenPoint)) return true;
        if (cardItemContextMenu) {
            try {
                if (cardItemContextMenu(
                        surface->card.id, item, screenPoint.x, screenPoint.y)) {
                    return true;
                }
            } catch (...) {
            }
        }

        constexpr UINT FirstShellCommand = 1;
        constexpr UINT LastShellCommand = 0x7FFF;
        constexpr UINT CompactOpenCommand = 0x8001;
        constexpr UINT CompactCutCommand = 0x8002;
        constexpr UINT CompactCopyCommand = 0x8003;
        constexpr UINT CompactDeleteCommand = 0x8004;
        constexpr UINT CompactPropertiesCommand = 0x8005;
        constexpr UINT ShowMoreCommand = 0x80FF;

        const wchar_t* deferredVerb = nullptr;
        auto showClassicMenu = CurrentWindowsFileContextMenuMode()
            == WindowsFileContextMenuMode::Classic;
        if (!showClassicMenu || surface->card.positionLocked) {
            std::vector<WindowsPopupMenuItem> compactItems{
                {.command = CompactOpenCommand,
                 .label = usesEnglish() ? L"Open" : L"打开"},
                {.command = CompactCutCommand,
                 .label = usesEnglish() ? L"Cut" : L"剪切",
                 .separatorBefore = true},
                {.command = CompactCopyCommand,
                 .label = usesEnglish() ? L"Copy" : L"复制"},
            };
            compactItems.push_back({
                .command = CompactDeleteCommand,
                .label = removeReferenceOnly
                    ? (usesEnglish() ? L"Remove reference" : L"移除引用")
                    : (usesEnglish() ? L"Delete" : L"删除"),
                .danger = true,
            });
            compactItems.push_back({
                .command = CompactPropertiesCommand,
                .label = usesEnglish() ? L"Properties" : L"属性",
                .separatorBefore = true,
            });
            compactItems.push_back({
                .command = ShowMoreCommand,
                .label = usesEnglish() ? L"Show more options" : L"显示更多选项",
                .separatorBefore = true,
            });
            if (surface->card.positionLocked) {
                compactItems.erase(compactItems.begin() + 1, compactItems.end());
            }
            switch (ShowWindowsPopupMenu(window, screenPoint, compactItems)) {
            case 0:
                return true;
            case CompactOpenCommand:
                deferredVerb = L"open";
                break;
            case CompactCutCommand:
                deferredVerb = L"cut";
                break;
            case CompactCopyCommand:
                deferredVerb = L"copy";
                break;
            case CompactDeleteCommand:
                if (removeReferenceOnly) {
                    if (mappingReferenceRemoved) {
                        try { (void)mappingReferenceRemoved(surface->card.id, item); } catch (...) {}
                    }
                    return true;
                }
                if (fileDeleteConfirmation) {
                    try {
                        if (!fileDeleteConfirmation(surface->card.id, item)) return true;
                    } catch (...) {
                        return true;
                    }
                }
                deferredVerb = L"delete";
                break;
            case CompactPropertiesCommand:
                deferredVerb = L"properties";
                break;
            case ShowMoreCommand:
                showClassicMenu = true;
                break;
            default:
                return true;
            }
        }

        PIDLIST_ABSOLUTE absoluteItem = nullptr;
        SFGAOF attributes = 0;
        if (FAILED(SHParseDisplayName(
                sourcePath.c_str(), nullptr, &absoluteItem, 0, &attributes))
            || absoluteItem == nullptr) {
            return true;
        }

        ComPtr<IShellFolder> parentFolder;
        PCUITEMID_CHILD childItem = nullptr;
        const auto bindResult = SHBindToParent(
            absoluteItem,
            IID_PPV_ARGS(&parentFolder),
            &childItem);
        if (FAILED(bindResult) || parentFolder == nullptr || childItem == nullptr) {
            CoTaskMemFree(absoluteItem);
            return true;
        }

        ComPtr<IContextMenu> contextMenu;
        const auto menuResult = parentFolder->GetUIObjectOf(
            window,
            1,
            &childItem,
            IID_IContextMenu,
            nullptr,
            reinterpret_cast<void**>(contextMenu.GetAddressOf()));
        if (FAILED(menuResult) || contextMenu == nullptr) {
            CoTaskMemFree(absoluteItem);
            return true;
        }

        const auto menu = CreatePopupMenu();
        if (menu == nullptr) {
            CoTaskMemFree(absoluteItem);
            return true;
        }
        const auto queryResult = contextMenu->QueryContextMenu(
            menu, 0, FirstShellCommand, LastShellCommand, CMF_NORMAL);
        if (FAILED(queryResult)) {
            DestroyMenu(menu);
            CoTaskMemFree(absoluteItem);
            return true;
        }
        const auto assignedCommands = static_cast<UINT>(queryResult) & 0xFFFFu;
        const auto findShellCommand = [&](const wchar_t* verb) noexcept {
            for (UINT offset = 0; offset < assignedCommands; ++offset) {
                wchar_t canonical[128]{};
                if (SUCCEEDED(contextMenu->GetCommandString(
                        offset,
                        GCS_VERBW,
                        nullptr,
                        reinterpret_cast<LPSTR>(canonical),
                        static_cast<UINT>(std::size(canonical))))
                    && _wcsicmp(canonical, verb) == 0) {
                    return FirstShellCommand + offset;
                }
            }
            return 0u;
        };
        const auto deleteCommand = findShellCommand(L"delete");
        const auto cutCommand = findShellCommand(L"cut");
        // File-item rename is intentionally unavailable in Desto. Text editing
        // is handled only by Todo/Card settings editors; Explorer rename is
        // still available when the user opens the folder in Explorer.
        const auto renameCommand = findShellCommand(L"rename");
        if (renameCommand != 0) DeleteMenu(menu, renameCommand, MF_BYCOMMAND);
        if ((removeReferenceOnly || surface->card.positionLocked) && cutCommand != 0) {
            DeleteMenu(menu, cutCommand, MF_BYCOMMAND);
        }

        ComPtr<IContextMenu2> contextMenu2;
        ComPtr<IContextMenu3> contextMenu3;
        (void)contextMenu.As(&contextMenu2);
        (void)contextMenu.As(&contextMenu3);
        const auto trackClassicMenu = [&]() noexcept {
            activeShellContextMenu2 = contextMenu2.Get();
            activeShellContextMenu3 = contextMenu3.Get();
            const auto selected = TrackPopupMenuEx(
                menu,
                TPM_RETURNCMD | TPM_RIGHTBUTTON,
                screenPoint.x,
                screenPoint.y,
                window,
                nullptr);
            activeShellContextMenu3 = nullptr;
            activeShellContextMenu2 = nullptr;
            return selected;
        };

        const auto command = showClassicMenu
            ? trackClassicMenu() : findShellCommand(deferredVerb);

        const auto removeReference = removeReferenceOnly && deleteCommand != 0
            && command == deleteCommand;
        if (!removeReference && !surface->card.positionLocked
            && command >= FirstShellCommand && command <= LastShellCommand) {
            if (command == deleteCommand && fileDeleteConfirmation) {
                try {
                    if (!fileDeleteConfirmation(surface->card.id, item)) {
                        DestroyMenu(menu);
                        CoTaskMemFree(absoluteItem);
                        return true;
                    }
                } catch (...) {
                    DestroyMenu(menu);
                    CoTaskMemFree(absoluteItem);
                    return true;
                }
            }
            const auto workingDirectory = sourcePath.parent_path().wstring();
            CMINVOKECOMMANDINFOEX invocation{};
            invocation.cbSize = sizeof(invocation);
            invocation.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
            invocation.hwnd = window;
            invocation.lpVerb = MAKEINTRESOURCEA(command - FirstShellCommand);
            invocation.lpVerbW = MAKEINTRESOURCEW(command - FirstShellCommand);
            invocation.lpDirectoryW = workingDirectory.c_str();
            invocation.nShow = SW_SHOWNORMAL;
            invocation.ptInvoke = screenPoint;
            (void)contextMenu->InvokeCommand(
                reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invocation));
        }
        DestroyMenu(menu);
        CoTaskMemFree(absoluteItem);
        PostMessageW(window, WM_NULL, 0, 0);
        if (removeReference && mappingReferenceRemoved) {
            try { (void)mappingReferenceRemoved(surface->card.id, item); } catch (...) {}
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
        const auto changed = surface->hoveredItem.has_value() || surface->collapseHovered
            || surface->pinHovered
            || surface->mappingViewHovered
            || surface->mappingUpHovered
            || surface->todoAddHovered || surface->todoViewHovered
            || surface->todoArchiveHovered;
        surface->hoveredItem.reset();
        surface->hoveredTodoRow.reset();
        surface->collapseHovered = false;
        surface->pinHovered = false;
        surface->mappingViewHovered = false;
        surface->mappingUpHovered = false;
        surface->todoAddHovered = false;
        surface->todoViewHovered = false;
        surface->todoArchiveHovered = false;
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
        const auto pinHovered = !collapseHovered && isPinControlHit(*surface, x, y);
        const auto mappingViewHovered = !collapseHovered && !pinHovered
            && isMappingViewControlHit(*surface, x, y);
        const auto mappingUpHovered = !collapseHovered && !pinHovered && !mappingViewHovered
            && isMappingUpControlHit(*surface, x, y);
        const auto todoViewHovered = !collapseHovered && !pinHovered && !mappingViewHovered
            && !mappingUpHovered
            && isTodoViewControlHit(*surface, x, y);
        const auto todoArchiveHovered = !collapseHovered && !pinHovered && !todoViewHovered
            && isTodoArchiveControlHit(*surface, x, y);
        const auto todoAddHovered = !collapseHovered && !pinHovered && !todoViewHovered
            && !todoArchiveHovered
            && isTodoAddControlHit(*surface, x, y);
        const auto todoRow = collapseHovered || pinHovered || mappingViewHovered
            || mappingUpHovered || todoViewHovered
            || todoArchiveHovered || todoAddHovered
            ? std::optional<std::size_t>{}
            : todoRowAt(*surface, x, y);
        const auto index = collapseHovered || pinHovered || mappingViewHovered
            || mappingUpHovered || todoViewHovered
            || todoArchiveHovered || todoAddHovered
            || todoRow.has_value()
            ? std::optional<std::size_t>{}
            : itemAt(*surface, x, y);
        if (index == surface->hoveredItem && todoRow == surface->hoveredTodoRow
            && collapseHovered == surface->collapseHovered
            && pinHovered == surface->pinHovered
            && mappingViewHovered == surface->mappingViewHovered
            && mappingUpHovered == surface->mappingUpHovered
            && todoAddHovered == surface->todoAddHovered
            && todoViewHovered == surface->todoViewHovered
            && todoArchiveHovered == surface->todoArchiveHovered) {
            return;
        }
        const auto visualHoverChanged = index != surface->hoveredItem
            || todoRow != surface->hoveredTodoRow
            || collapseHovered != surface->collapseHovered
            || pinHovered != surface->pinHovered
            || mappingViewHovered != surface->mappingViewHovered
            || mappingUpHovered != surface->mappingUpHovered
            || todoAddHovered != surface->todoAddHovered
            || todoViewHovered != surface->todoViewHovered
            || todoArchiveHovered != surface->todoArchiveHovered;
        clearPointerHover(window, false);
        surface->collapseHovered = collapseHovered;
        surface->pinHovered = pinHovered;
        surface->mappingViewHovered = mappingViewHovered;
        surface->mappingUpHovered = mappingUpHovered;
        surface->todoViewHovered = todoViewHovered;
        surface->todoArchiveHovered = todoArchiveHovered;
        surface->todoAddHovered = todoAddHovered;
        surface->hoveredTodoRow = todoRow;
        if (todoRow.has_value()) {
            if (visualHoverChanged) {
                try {
                    render(*surface, surface->display, surface->card, surface->ordinal);
                } catch (...) {
                }
            }
            return;
        }
        if (!index.has_value() || surface->tooltip == nullptr) {
            if (visualHoverChanged) {
                try {
                    render(*surface, surface->display, surface->card, surface->ordinal);
                } catch (...) {
                }
            }
            return;
        }
        surface->hoveredItem = index;
        const auto& item = surface->card.items[*index];
        surface->tooltipText = item.displayName;
        if (item.state == presentation::CardItemState::Missing) {
            surface->tooltipText += tr(L"\n项目缺失", L"\nItem is missing");
        } else if (item.state == presentation::CardItemState::UnresolvedShortcut) {
            surface->tooltipText += tr(
                L"\n快捷方式不可用", L"\nShortcut is unavailable");
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
        DWORD keyState,
        const std::optional<std::string>& sourceCardId = std::nullopt) noexcept {
        auto* surface = findSurface(window);
        KillTimer(window, DropPreviewResetTimerId);
        if (surface == nullptr || !surface->card.expanded
            || acceptedDropEffect(*surface, allowedEffect, keyState, sourceCardId)
                == DROPEFFECT_NONE) {
            clearDropPreview(window);
            return DROPEFFECT_NONE;
        }
        surface->dropDragActive = true;
        surface->dropAllowedEffect = allowedEffect;
        surface->dropKeyState = keyState;
        surface->dropSourceCardId = sourceCardId;
        // OLE drag-over messages continue to arrive while the pointer crosses
        // an item. A drag preview owns the visual state, so stale hover
        // hotspots and tooltips must not repaint the item underneath it.
        clearPointerHover(window, false);
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
            const auto settings = baseItemLayoutSettings(*surface);
            insertion = presentation::ResolveCardSlotIndex(
                widthDip,
                pointerXDip,
                presentation::ResolveScrolledCardPointerY(
                    pointerYDip, surface->scrollRowOffset, settings),
                settings,
                surface->card.content.fixedRows);
        } else {
            const auto settings = baseItemLayoutSettings(*surface);
            const auto scrolledPointerYDip = presentation::ResolveScrolledCardPointerY(
                pointerYDip, surface->scrollRowOffset, settings);
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
                scrolledPointerYDip,
                settings,
                previousPreview);
            const auto conservativePreview = presentation::ResolveAdaptiveCardDropPreview(
                contentSlotCount(*surface, false),
                widthDip,
                pointerXDip,
                scrolledPointerYDip,
                settings,
                previousPreview,
                false);
            const auto expansionRequested = expandedPreview.insertionIndex
                    != conservativePreview.insertionIndex
                || expandedPreview.columns != conservativePreview.columns;
            const auto shrinkRequested = previousPreview.has_value()
                && conservativePreview.columns < previousPreview->columns;
            auto preview = expansionRequested ? expandedPreview : conservativePreview;
            if (expansionRequested || shrinkRequested) {
                const auto pendingPreview = expansionRequested
                    ? expandedPreview : conservativePreview;
                if (!surface->pendingDropExpansion.has_value()
                    || surface->pendingDropExpansion->insertionIndex
                        != pendingPreview.insertionIndex
                    || surface->pendingDropExpansion->columns != pendingPreview.columns
                    || surface->pendingDropShrink != shrinkRequested) {
                    if (expansionRequested && surface->pendingDropExpansion.has_value()
                        && !surface->pendingDropShrink) {
                        ++surface->dropExpansionStep;
                    }
                    surface->pendingDropExpansion = pendingPreview;
                    surface->pendingDropShrink = shrinkRequested;
                    surface->dropExpansionStartedAt = GetTickCount64();
                }
                const auto elapsed = GetTickCount64() - surface->dropExpansionStartedAt;
                const auto ready = shrinkRequested
                    ? presentation::IsAdaptiveDropExpansionReady(elapsed)
                    : presentation::IsAdaptiveDropExpansionReady(
                        elapsed,
                        surface->dropExpansionStep,
                        surface->projection.horizontalAnchor
                            == domain::PlacementHorizontalAnchor::Right);
                if (!ready) {
                    preview = previousPreview.value_or(conservativePreview);
                }
            } else {
                surface->pendingDropExpansion.reset();
                surface->pendingDropShrink = false;
                surface->dropExpansionStartedAt = 0;
                surface->dropExpansionStep = 0;
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
            if (!surface->dropPreviewOriginRect.has_value()) {
                surface->dropPreviewOriginRect = surface->projection.rect;
            }
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
        return acceptedDropEffect(*surface, allowedEffect, keyState, sourceCardId);
    }

    static void resetDropInteractionState(
        Surface& surface,
        bool clearOrigin = true) noexcept {
        surface.dropInsertionIndex.reset();
        surface.dropPreviewColumns.reset();
        surface.dropDragActive = false;
        surface.dropAllowedEffect = DROPEFFECT_NONE;
        surface.dropKeyState = 0;
        surface.dropSourceCardId.reset();
        surface.pendingDropExpansion.reset();
        surface.pendingDropShrink = false;
        surface.dropExpansionStartedAt = 0;
        surface.dropExpansionStep = 0;
        surface.dropCommitInProgress = false;
        surface.dropCommitContentApplied = false;
        if (clearOrigin) {
            surface.dropPreviewOriginRect.reset();
        }
    }

    void clearDropPreview(HWND window) noexcept {
        auto* surface = findSurface(window);
        KillTimer(window, DropPreviewResetTimerId);
        if (surface == nullptr) return;
        const auto hadPreview = surface->dropInsertionIndex.has_value()
            || surface->dropPreviewOriginRect.has_value();
        if (surface->dropPreviewOriginRect.has_value()) {
            surface->projection.rect = *surface->dropPreviewOriginRect;
        }
        resetDropInteractionState(*surface);
        if (!hadPreview) return;
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
        DWORD allowedEffect,
        DWORD keyState) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr || paths.empty()
            || updateDropPreview(
                window, screenPoint, allowedEffect, keyState, sourceCardId)
                == DROPEFFECT_NONE) {
            clearDropPreview(window);
            return DROPEFFECT_NONE;
        }
        const auto cardId = surface->card.id;
        const auto insertion = surface->dropInsertionIndex.value_or(surface->card.items.size());
        const auto layoutColumns = itemLayout(*surface, 0).columns;
        const auto acceptedEffect = acceptedDropEffect(
            *surface, allowedEffect, keyState, sourceCardId);
        const auto operation = acceptedEffect == DROPEFFECT_COPY
            ? FileDropOperation::Copy
            : FileDropOperation::Move;
        KillTimer(window, DropPreviewResetTimerId);
        resetDropInteractionState(*surface, false);
        surface->dropCommitInProgress = true;
        if (!applicationItemsDropped) {
            if (surface->dropPreviewOriginRect.has_value()) {
                surface->projection.rect = *surface->dropPreviewOriginRect;
            }
            resetDropInteractionState(*surface);
            try {
                resizeSurfaceForContent(*surface, true);
                render(*surface, surface->display, surface->card, surface->ordinal);
            } catch (...) {
            }
            return DROPEFFECT_NONE;
        }
        try {
            if (applicationItemsDropped(
                    cardId, paths, sourceCardId, operation, insertion, layoutColumns)) {
                const auto contentApplied = surface->dropCommitContentApplied;
                if (!contentApplied && surface->dropPreviewOriginRect.has_value()) {
                    surface->projection.rect = *surface->dropPreviewOriginRect;
                }
                resetDropInteractionState(*surface);
                if (!contentApplied) {
                    resizeSurfaceForContent(*surface, true);
                    render(*surface, surface->display, surface->card, surface->ordinal);
                }
                return acceptedEffect;
            }
        } catch (...) {
        }
        if (surface->dropPreviewOriginRect.has_value()) {
            surface->projection.rect = *surface->dropPreviewOriginRect;
        }
        resetDropInteractionState(*surface);
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
        updateAutomaticContentConstraint(surface);
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

    static bool acceptsFileDrops(const Surface& surface) noexcept {
        return surface.card.type == domain::CardType::Application
            || surface.card.type == domain::CardType::Mapping;
    }

    void configureDropTarget(Surface& surface) {
        const auto shouldAccept = acceptsFileDrops(surface);
        if (!shouldAccept && surface.dropTarget != nullptr) {
            if (surface.window != nullptr) RevokeDragDrop(surface.window);
            surface.dropTarget->Release();
            surface.dropTarget = nullptr;
            return;
        }
        if (!shouldAccept || surface.dropTarget != nullptr) return;

        auto* target = CreateFileDropTarget({
            .dragOver = [this, window = surface.window](
                            POINTL point,
                            DWORD allowed,
                            DWORD keyState,
                            const std::optional<std::string>& sourceCardId) {
                return updateDropPreview(
                    window, point, allowed, keyState, sourceCardId);
            },
            .dragLeave = [this, window = surface.window]() {
                scheduleDropPreviewClear(window);
            },
            .drop = [this, window = surface.window](
                        std::vector<std::filesystem::path> paths,
                        std::optional<std::string> sourceCardId,
                        POINTL point,
                        DWORD allowed,
                        DWORD keyState) {
                return completeFileDrop(
                    window, std::move(paths), std::move(sourceCardId),
                    point, allowed, keyState);
            },
        });
        if (target == nullptr || FAILED(RegisterDragDrop(surface.window, target))) {
            if (target != nullptr) target->Release();
            throw std::runtime_error("RegisterDragDrop failed for file Card.");
        }
        surface.dropTarget = target;
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
            surface.alwaysOnTop ? nullptr : desktopOwnerWindow,
            nullptr,
            module,
            this);
        if (surface.window == nullptr) {
            throw std::runtime_error("CreateWindowExW failed for desktop host surface.");
        }
        configureDropTarget(surface);
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
        ++renderStatistics.fullSurfaceRenders;
        (void)ordinal;
        const auto systemDarkSurface = card.appearancePreset == "system"
            && systemAppsUseDarkTheme;
        const auto systemLightSurface = card.appearancePreset == "system"
            && !systemAppsUseDarkTheme;
        const auto micaDarkSurface = card.appearancePreset == "mica-dark"
            || card.appearancePreset == "transparent-black"
            || systemDarkSurface
            || card.appearancePreset == "black" || card.appearancePreset == "dark";
        const auto micaWhiteSurface = card.appearancePreset == "mica-white"
            || systemLightSurface
            || card.appearancePreset == "white" || card.appearancePreset == "default";
        const auto brandSurface = card.appearancePreset == "brand"
            || card.appearancePreset == "jewel" || card.appearancePreset == "pearl-pink";
        const auto appleWhiteSurface = card.appearancePreset == "apple-glass-white";
        const auto appleBlackSurface = card.appearancePreset == "apple-glass-black";
        const auto crystalSurface = card.appearancePreset == "transparent-white";
        const auto crystalStyle = ResolveCrystalMaterialStyle();
        const auto darkSurface = micaDarkSurface || appleBlackSurface;
        const auto visibleBottom = card.expanded
            ? surface.height
            : std::min(surface.height, dipToPixels(48.0, surface));
        surface.scrollRowOffset = std::min(
            surface.scrollRowOffset, maximumScrollOffset(surface));
        surface.interactiveHeight = visibleBottom;
        const auto radius = std::min(
            static_cast<int>(std::lround(card.cornerRadius * display.effectiveDpi / 96.0)),
            std::min(surface.width, visibleBottom) / 2);
        std::vector<std::uint8_t> materialAlphaBoost(
            crystalSurface
                ? static_cast<std::size_t>(surface.width) * visibleBottom
                : std::size_t{0},
            0u);
        std::vector<std::uint32_t> contentOverlay(
            static_cast<std::size_t>(surface.width) * visibleBottom,
            0u);
        auto textMask = std::make_unique<TextMaskSurface>(surface.width, visibleBottom);
        const auto recordMaterialAlpha = [&] (int x, int y, double coverage) {
            if (materialAlphaBoost.empty() || x < 0 || y < 0
                || x >= surface.width || y >= visibleBottom || coverage <= 0.0) {
                return;
            }
            auto& value = materialAlphaBoost[
                static_cast<std::size_t>(y) * surface.width + x];
            value = std::max(value, static_cast<std::uint8_t>(std::lround(
                std::clamp(coverage, 0.0, 1.0) * 255.0)));
        };
        const auto blendPremultipliedContent = [&] (
            int x, int y, std::uint32_t sourcePixel) {
            if (contentOverlay.empty() || x < 0 || y < 0
                || x >= surface.width || y >= visibleBottom) {
                return;
            }
            const auto sourceAlpha = (sourcePixel >> 24) & 0xFFu;
            if (sourceAlpha == 0u) return;
            auto& destination = contentOverlay[
                static_cast<std::size_t>(y) * surface.width + x];
            const auto inverse = 255u - sourceAlpha;
            const auto composite = [&](int shift) {
                const auto source = (sourcePixel >> shift) & 0xFFu;
                const auto target = (destination >> shift) & 0xFFu;
                return std::min(255u, source + (target * inverse + 127u) / 255u);
            };
            const auto outputAlpha = std::min(
                255u, sourceAlpha
                    + (((destination >> 24) & 0xFFu) * inverse + 127u) / 255u);
            destination = (outputAlpha << 24) | (composite(16) << 16)
                | (composite(8) << 8) | composite(0);
        };
        const auto blendContent = [&] (
            int x, int y, std::uint32_t color, double coverage) {
            const auto alpha = static_cast<std::uint32_t>(std::lround(
                std::clamp(coverage, 0.0, 1.0) * 255.0));
            if (alpha == 0u) return;
            const auto premultiply = [&](int shift) {
                return (((color >> shift) & 0xFFu) * alpha + 127u) / 255u;
            };
            blendPremultipliedContent(
                x, y, (alpha << 24) | (premultiply(16) << 16)
                    | (premultiply(8) << 8) | premultiply(0));
        };
        const auto drawSurfaceText = [&] (
            const wchar_t* value,
            int count,
            RECT* bounds,
            UINT format) -> int {
            if (textMask == nullptr || (format & DT_CALCRECT) != 0u) {
                return DrawTextW(surface.memoryDc, value, count, bounds, format);
            }
            textMask->clear(*bounds);
            const auto selectedFont = GetCurrentObject(surface.memoryDc, OBJ_FONT);
            const auto previousMaskFont = selectedFont == nullptr
                ? nullptr : SelectObject(textMask->dc, selectedFont);
            const auto result = DrawTextW(textMask->dc, value, count, bounds, format);
            if (previousMaskFont != nullptr) {
                SelectObject(textMask->dc, previousMaskFont);
            }
            const auto color = static_cast<std::uint32_t>(GetTextColor(surface.memoryDc));
            const auto rgb = ((color & 0xFFu) << 16) | (color & 0xFF00u)
                | ((color >> 16) & 0xFFu);
            const auto left = std::clamp<LONG>(bounds->left, 0, surface.width);
            const auto top = std::clamp<LONG>(bounds->top, 0, visibleBottom);
            const auto right = std::clamp<LONG>(bounds->right, left, surface.width);
            const auto bottom = std::clamp<LONG>(bounds->bottom, top, visibleBottom);
            for (auto y = top; y < bottom; ++y) {
                for (auto x = left; x < right; ++x) {
                    const auto maskPixel = textMask->pixels[
                        static_cast<std::size_t>(y) * surface.width + x];
                    const auto mask = std::max({
                        (maskPixel >> 16) & 0xFFu,
                        (maskPixel >> 8) & 0xFFu,
                        maskPixel & 0xFFu,
                    });
                    blendContent(x, y, rgb, static_cast<double>(mask) / 255.0);
                }
            }
            return result;
        };
        for (int y = 0; y < surface.height; ++y) {
            for (int x = 0; x < surface.width; ++x) {
                std::uint32_t red = micaDarkSurface || appleBlackSurface ? 32u : 243u;
                std::uint32_t green = micaDarkSurface || appleBlackSurface ? 33u : 243u;
                std::uint32_t blue = micaDarkSurface || appleBlackSurface ? 36u : 243u;
                if (brandSurface) {
                    const auto horizontal = surface.width <= 1
                        ? 0.0
                        : static_cast<double>(x) / (surface.width - 1);
                    const auto vertical = visibleBottom <= 1
                        ? 0.0
                        : static_cast<double>(std::min(y, visibleBottom - 1))
                            / (visibleBottom - 1);
                    const auto diagonal = std::clamp(
                        horizontal * 0.58 + vertical * 0.42, 0.0, 1.0);
                    // Match the supplied Win11 brand material: #eae6ff -> #edf2ff.
                    red = static_cast<std::uint32_t>(std::lround(234.0 + 3.0 * diagonal));
                    green = static_cast<std::uint32_t>(std::lround(230.0 + 12.0 * diagonal));
                    blue = 255u;
                } else if (appleWhiteSurface) {
                    const auto horizontal = surface.width <= 1
                        ? 0.0 : static_cast<double>(x) / (surface.width - 1);
                    const auto vertical = visibleBottom <= 1
                        ? 0.0 : static_cast<double>(std::min(y, visibleBottom - 1))
                            / (visibleBottom - 1);
                    red = static_cast<std::uint32_t>(std::lround(249.0 + 4.0 * (1.0 - vertical)));
                    green = static_cast<std::uint32_t>(std::lround(250.0 + 3.0 * horizontal));
                    blue = static_cast<std::uint32_t>(std::lround(253.0 + 2.0 * (1.0 - horizontal)));
                } else if (appleBlackSurface) {
                    const auto diagonal = static_cast<double>(x + y)
                        / std::max(1, surface.width + visibleBottom - 2);
                    red = static_cast<std::uint32_t>(std::lround(15.0 + 12.0 * diagonal));
                    green = static_cast<std::uint32_t>(std::lround(16.0 + 13.0 * diagonal));
                    blue = static_cast<std::uint32_t>(std::lround(20.0 + 17.0 * diagonal));
                } else if (crystalSurface) {
                    const auto horizontal = surface.width <= 1
                        ? 0.0 : static_cast<double>(x) / (surface.width - 1);
                    const auto vertical = visibleBottom <= 1
                        ? 0.0 : static_cast<double>(std::min(y, visibleBottom - 1))
                            / (visibleBottom - 1);
                    red = static_cast<std::uint32_t>(std::lround(226.0 + 8.0 * (1.0 - vertical)));
                    green = static_cast<std::uint32_t>(std::lround(232.0 + 6.0 * horizontal));
                    blue = static_cast<std::uint32_t>(std::lround(240.0 + 6.0 * (1.0 - horizontal)));
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
        const auto text = card.showTitle ? titleText : L"";
        SetBkMode(surface.memoryDc, TRANSPARENT);
        const auto foreground = darkSurface ? RGB(244, 246, 249)
            : crystalSurface ? RGB(28, 32, 38) : RGB(38, 40, 45);
        SetTextColor(surface.memoryDc, foreground);
        const auto font = CreateFontW(
            -dipToPixels(14.0, surface),
            0,
            0,
            0,
            kCardTitleWeight,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            ResolveLayeredSurfaceTextQuality(),
            DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI Variable Text");
        const auto previousFont = font == nullptr
            ? nullptr
            : SelectObject(surface.memoryDc, font);
        const auto textLeft = card.mappingCanNavigateUp
            ? mappingUpControlRect(surface).right + dipToPixels(4.0, surface)
            : dipToPixels(18.0, surface);
        const auto control = collapseControlRect(surface);
        const auto mappingControl = mappingViewControlRect(surface);
        const auto headerControlLeft = (card.type == domain::CardType::Application
                || card.type == domain::CardType::Mapping)
            ? card.showPresentationControl ? mappingControl.left
                : card.showPinControl ? pinControlRect(surface).left
                : card.showCollapseControl ? control.left : surface.width - textLeft
            : card.showPinControl ? pinControlRect(surface).left : control.left;
        RECT textRect{
            textLeft,
            0,
            (card.showCollapseControl || card.showPinControl
                || ((card.type == domain::CardType::Application
                    || card.type == domain::CardType::Mapping)
                    && card.showPresentationControl))
                ? headerControlLeft - dipToPixels(4.0, surface)
                : surface.width - textLeft,
            std::min(visibleBottom, dipToPixels(48.0, surface)),
        };
        drawSurfaceText(
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
        const auto blendVisibleContent = [&] (
            int x, int y, std::uint32_t color, double coverage) {
            blendContent(x, y, color, coverage);
        };
        const auto drawContentSegment = [&] (
            double x1,
            double y1,
            double x2,
            double y2,
            double strokeRadius,
            std::uint32_t color) {
            const auto vx = x2 - x1;
            const auto vy = y2 - y1;
            const auto lengthSquared = vx * vx + vy * vy;
            if (lengthSquared <= 0.0) return;
            const auto minX = static_cast<int>(std::floor(
                std::min(x1, x2) - strokeRadius - 1.0));
            const auto maxX = static_cast<int>(std::ceil(
                std::max(x1, x2) + strokeRadius + 1.0));
            const auto minY = static_cast<int>(std::floor(
                std::min(y1, y2) - strokeRadius - 1.0));
            const auto maxY = static_cast<int>(std::ceil(
                std::max(y1, y2) + strokeRadius + 1.0));
            for (int y = minY; y <= maxY; ++y) {
                for (int x = minX; x <= maxX; ++x) {
                    const auto px = x + 0.5;
                    const auto py = y + 0.5;
                    const auto projection = std::clamp(
                        ((px - x1) * vx + (py - y1) * vy) / lengthSquared,
                        0.0,
                        1.0);
                    const auto distance = std::hypot(
                        px - (x1 + projection * vx),
                        py - (y1 + projection * vy));
                    const auto coverage = std::clamp(
                        strokeRadius + 0.5 - distance, 0.0, 1.0);
                    blendVisibleContent(x, y, color, coverage);
                }
            }
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
                    blendVisibleContent(x, y, color, coverage * opacity);
                }
            }
        };
        const auto drawRoundedFill = [&] (
            const RECT& rect,
            double radius,
            std::uint32_t color,
            double opacity,
            double materialOpacity = 0.0) {
            const presentation::RoundedRectSpec spec{
                .width = static_cast<double>(rect.right - rect.left),
                .height = static_cast<double>(rect.bottom - rect.top),
                .radius = radius,
            };
            for (int y = rect.top; y < rect.bottom; ++y) {
                for (int x = rect.left; x < rect.right; ++x) {
                    const auto coverage = presentation::SampleRoundedRectCoverage(
                        spec, x - rect.left + 0.5, y - rect.top + 0.5);
                    blendRgb(x, y, color, coverage * opacity);
                    recordMaterialAlpha(x, y, coverage * materialOpacity);
                }
            }
        };
        const auto drawRoundedOutline = [&] (
            const RECT& rect,
            double radius,
            double strokeWidth,
            std::uint32_t color,
            double opacity,
            double materialOpacity = 0.0) {
            const presentation::RoundedRectSpec spec{
                .width = static_cast<double>(rect.right - rect.left),
                .height = static_cast<double>(rect.bottom - rect.top),
                .radius = radius,
                .strokeWidth = strokeWidth,
            };
            for (int y = rect.top; y < rect.bottom; ++y) {
                for (int x = rect.left; x < rect.right; ++x) {
                    const auto coverage = presentation::SampleInnerRoundedOutlineCoverage(
                        spec, x - rect.left + 0.5, y - rect.top + 0.5);
                    blendRgb(x, y, color, coverage * opacity);
                    recordMaterialAlpha(x, y, coverage * materialOpacity);
                }
            }
        };
        const auto drawContentRoundedFill = [&] (
            const RECT& rect,
            double radius,
            std::uint32_t color,
            double opacity) {
            const presentation::RoundedRectSpec spec{
                .width = static_cast<double>(rect.right - rect.left),
                .height = static_cast<double>(rect.bottom - rect.top),
                .radius = radius,
            };
            for (int y = rect.top; y < rect.bottom; ++y) {
                for (int x = rect.left; x < rect.right; ++x) {
                    const auto coverage = presentation::SampleRoundedRectCoverage(
                        spec, x - rect.left + 0.5, y - rect.top + 0.5);
                    blendContent(x, y, color, coverage * opacity);
                }
            }
        };
        const auto drawContentRoundedOutline = [&] (
            const RECT& rect,
            double radius,
            double strokeWidth,
            std::uint32_t color,
            double opacity) {
            const presentation::RoundedRectSpec spec{
                .width = static_cast<double>(rect.right - rect.left),
                .height = static_cast<double>(rect.bottom - rect.top),
                .radius = radius,
                .strokeWidth = strokeWidth,
            };
            for (int y = rect.top; y < rect.bottom; ++y) {
                for (int x = rect.left; x < rect.right; ++x) {
                    const auto coverage = presentation::SampleInnerRoundedOutlineCoverage(
                        spec, x - rect.left + 0.5, y - rect.top + 0.5);
                    blendContent(x, y, color, coverage * opacity);
                }
            }
        };
        if (card.expanded && card.type == domain::CardType::Todo) {
            const auto entries = todoDisplayEntries(surface);
            const auto remaining = std::ranges::count_if(entries, [&](const auto& entry) {
                return !card.todoItems[entry.itemIndex].completed;
            });
            const auto actionFont = CreateFontW(
                -dipToPixels(12.0, surface), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                ResolveLayeredSurfaceTextQuality(),
                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
            const auto actionSecondaryFont = CreateFontW(
                -dipToPixels(11.0, surface), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                ResolveLayeredSurfaceTextQuality(),
                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
            const auto previousActionFont = actionFont == nullptr
                ? nullptr : SelectObject(surface.memoryDc, actionFont);
            const auto drawTodoAction = [&] (
                RECT bounds, const std::wstring& value, COLORREF color) {
                SetTextColor(surface.memoryDc, color);
                drawSurfaceText(value.c_str(), -1, &bounds,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            };
            auto viewRect = todoViewControlRect(surface);
            if (surface.todoViewHovered || surface.todoViewPressed) {
                drawRoundedFill(
                    viewRect,
                    dipToPixels(9.0, surface),
                    darkSurface ? 0x00FFFFFFu : 0x0018212Fu,
                    surface.todoViewPressed ? (darkSurface ? 0.14 : 0.10)
                                            : (darkSurface ? 0.08 : 0.055));
            }
            const auto actionPrimaryColor = darkSurface ? RGB(232, 234, 239)
                : crystalSurface ? RGB(25, 34, 46) : RGB(45, 49, 57);
            const auto viewText = TodoDateLabel(
                domain::AddTodoDays(CurrentTodoDate(surface.timeZoneOffsetMinutes),
                    surface.todoAddDateOffset),
                surface.timeZoneOffsetMinutes,
                usesEnglish());
            drawTodoAction(viewRect, viewText, actionPrimaryColor);
            auto archiveRect = todoArchiveControlRect(surface);
            const auto hasCompleted = std::ranges::any_of(entries, [&](const auto& entry) {
                return card.todoItems[entry.itemIndex].completed;
            });
            if ((surface.todoArchiveHovered || surface.todoArchivePressed) && hasCompleted) {
                drawRoundedFill(
                    archiveRect,
                    dipToPixels(9.0, surface),
                    darkSurface ? 0x00FFFFFFu : 0x0018212Fu,
                    surface.todoArchivePressed ? (darkSurface ? 0.14 : 0.10)
                                               : (darkSurface ? 0.08 : 0.055));
            }
            const auto archiveColor = hasCompleted
                ? (darkSurface ? RGB(180, 186, 198) : RGB(70, 75, 84))
                : (darkSurface ? RGB(112, 118, 129) : RGB(82, 87, 96));
            const auto archiveText = tr(L"归档", L"Archive");
            if (actionSecondaryFont != nullptr) {
                SelectObject(surface.memoryDc, actionSecondaryFont);
            }
            drawTodoAction(archiveRect, archiveText,
                crystalSurface && hasCompleted ? RGB(42, 52, 65) : archiveColor);
            auto remainingRect = todoRemainingRect(surface);
            if (!surface.todoCalendarOpen) {
                drawRoundedFill(
                    remainingRect,
                    dipToPixels(9.0, surface),
                    darkSurface ? 0x00FFFFFFu : 0x0018212Fu,
                    darkSurface ? 0.07 : 0.045);
            }
            const auto remainingColor = darkSurface ? RGB(182, 188, 199)
                : crystalSurface ? RGB(34, 43, 56) : RGB(55, 60, 68);
            const auto remainingText = tr(L"剩余 ", L"Left ") + std::to_wstring(remaining);
            drawTodoAction(remainingRect, remainingText, remainingColor);
            if (previousActionFont != nullptr) SelectObject(surface.memoryDc, previousActionFont);
            if (actionFont != nullptr) DeleteObject(actionFont);
            if (actionSecondaryFont != nullptr) DeleteObject(actionSecondaryFont);

            const auto todoFont = CreateFontW(
                -dipToPixels(13.0, surface), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                ResolveLayeredSurfaceTextQuality(),
                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
            const auto previousTodoFont = todoFont == nullptr
                ? nullptr : SelectObject(surface.memoryDc, todoFont);
            const auto metadataFont = CreateFontW(
                -dipToPixels(9.0, surface), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                ResolveLayeredSurfaceTextQuality(),
                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
            const auto entryRects = todoEntryRects(surface, entries);
            if (!surface.todoCalendarOpen) for (std::size_t entryIndex = 0; entryIndex < entries.size(); ++entryIndex) {
                const auto& entry = entries[entryIndex];
                auto row = entryRects[entryIndex];
                if (row.right <= row.left || row.bottom <= row.top
                    || row.top >= visibleBottom
                    || row.bottom <= dipToPixels(128.0, surface)) {
                    continue;
                }
                const auto& item = card.todoItems[entry.itemIndex];
                if (surface.hoveredTodoRow == entry.itemIndex) {
                    drawRoundedFill(
                        row,
                        dipToPixels(10.0, surface),
                        darkSurface ? 0x00FFFFFFu : 0x0018212Fu,
                        surface.pressedTodoCheckbox == entry.itemIndex
                            ? (darkSurface ? 0.12 : 0.08)
                            : (darkSurface ? 0.07 : 0.045));
                }
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
                    : (darkSurface ? 0x00BEC4CFu
                        : crystalSurface ? 0x00465468u : 0x008E95A2u);
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
                        blendVisibleContent(x, y, checkboxColor, coverage);
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
                            blendVisibleContent(x, y, 0x00FFFFFFu,
                                std::clamp(strokeWidth + 0.5 - distance, 0.0, 1.0));
                        }
                    }
                }
                const auto titleColor = item.completed
                    ? (darkSurface ? RGB(156, 161, 172) : RGB(78, 83, 92)) : foreground;
                SetTextColor(surface.memoryDc, titleColor);
                const auto showCreatedTime = card.todoPreferences.showCreatedTime
                    && item.createdAtUnixMilliseconds > 0;
                const auto showMetadata = entry.showDateLabel || showCreatedTime;
                RECT label{
                    checkbox.right + dipToPixels(9.0, surface),
                    row.top + (showMetadata ? dipToPixels(3.0, surface) : 0),
                    row.right - dipToPixels(8.0, surface),
                    row.bottom - (showMetadata ? dipToPixels(16.0, surface) : 0),
                };
                const auto itemTitle = Utf8ToWide(item.title);
                const auto previousTitleFont = todoFont == nullptr
                    ? nullptr : SelectObject(surface.memoryDc, todoFont);
                RECT measured{0, 0, std::max<LONG>(1, label.right - label.left), 0};
                drawSurfaceText(
                    itemTitle.c_str(),
                    -1,
                    &measured,
                    DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
                const auto singleLine = measured.bottom <= dipToPixels(18.0, surface);
                if (!singleLine && !showMetadata) {
                    const auto available = label.bottom - label.top;
                    const auto measuredHeight = std::min<LONG>(measured.bottom, available);
                    label.top += std::max<LONG>(0, (available - measuredHeight) / 2);
                }
                drawSurfaceText(
                    itemTitle.c_str(),
                    -1,
                    &label,
                    DT_LEFT | DT_NOPREFIX | (singleLine
                        ? DT_SINGLELINE | DT_VCENTER
                        : DT_WORDBREAK | DT_EDITCONTROL));
                auto metadataLeft = label.left;
                if (entry.showDateLabel) {
                    const auto dateText = entry.archived
                        ? tr(L"归档", L"Archived")
                        : TodoDateLabel(
                            entry.date, surface.timeZoneOffsetMinutes, usesEnglish());
                    const auto previousMetadataFont = metadataFont == nullptr
                        ? nullptr : SelectObject(surface.memoryDc, metadataFont);
                    SIZE dateExtent{};
                    GetTextExtentPoint32W(surface.memoryDc, dateText.c_str(),
                        static_cast<int>(dateText.size()), &dateExtent);
                    const auto badgePadX = dipToPixels(8.0, surface);
                    const auto badgePadY = dipToPixels(3.0, surface);
                    const auto badgeTop = label.bottom + dipToPixels(2.0, surface);
                    const auto badge = RECT{
                        metadataLeft,
                        badgeTop,
                        metadataLeft + dateExtent.cx + badgePadX,
                        badgeTop + dateExtent.cy + badgePadY,
                    };
                    drawRoundedFill(
                        badge,
                        dipToPixels(5.0, surface),
                        0x002F71DCu,
                        darkSurface ? 0.92 : 0.96);
                    SetTextColor(surface.memoryDc, RGB(255, 255, 255));
                    auto dateTextRect = badge;
                    drawSurfaceText(dateText.c_str(), -1, &dateTextRect,
                        DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
                    if (previousMetadataFont != nullptr) {
                        SelectObject(surface.memoryDc, previousMetadataFont);
                    }
                    metadataLeft = badge.right + dipToPixels(6.0, surface);
                }
                if (showCreatedTime) {
                    const auto timestamp = static_cast<std::time_t>(item.createdAtUnixMilliseconds / 1000);
                    tm localTime{};
                    if (surface.timeZoneOffsetMinutes.has_value()) {
                        const auto adjusted = timestamp
                            + static_cast<std::time_t>(*surface.timeZoneOffsetMinutes) * 60;
                        gmtime_s(&localTime, &adjusted);
                    } else {
                        localtime_s(&localTime, &timestamp);
                    }
                    wchar_t timeText[16]{};
                    swprintf_s(timeText, L"%02d:%02d", localTime.tm_hour, localTime.tm_min);
                    const auto previousMetadataFont = metadataFont == nullptr
                        ? nullptr : SelectObject(surface.memoryDc, metadataFont);
                    const auto timeColor = darkSurface ? RGB(161, 167, 179)
                        : crystalSurface ? RGB(50, 61, 76) : RGB(72, 77, 86);
                    RECT timeRect{metadataLeft, label.bottom, label.right, row.bottom};
                    SetTextColor(surface.memoryDc, timeColor);
                    drawSurfaceText(timeText, -1, &timeRect,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                    if (previousMetadataFont != nullptr) {
                        SelectObject(surface.memoryDc, previousMetadataFont);
                    }
                }
                if (item.completed && singleLine && !entry.archived) {
                    SIZE textExtent{};
                    GetTextExtentPoint32W(
                        surface.memoryDc,
                        itemTitle.c_str(),
                        static_cast<int>(itemTitle.size()),
                        &textExtent);
                    const auto textWidth = std::min<LONG>(
                        textExtent.cx, label.right - label.left);
                    const auto lineY = (label.top + label.bottom) / 2;
                    drawContentSegment(
                        label.left,
                        lineY,
                        label.left + textWidth,
                        lineY,
                        std::max(0.5, dipToPixels(1.0, surface) / 2.0),
                        darkSurface ? 0x009CA1ACu : 0x007E848Eu);
                }
                if (previousTitleFont != nullptr) {
                    SelectObject(surface.memoryDc, previousTitleFont);
                }
            }
            if (previousTodoFont != nullptr) SelectObject(surface.memoryDc, previousTodoFont);
            if (todoFont != nullptr) DeleteObject(todoFont);
            if (metadataFont != nullptr) DeleteObject(metadataFont);
            const auto addRect = todoAddControlRect(surface);
            const auto editingTodo = todoEditor != nullptr
                && todoEditorSurface == surface.window;
            if (!editingTodo && !surface.todoCalendarOpen) {
                drawRoundedFill(
                    addRect,
                    dipToPixels(11.0, surface),
                    darkSurface ? 0x00FFFFFFu : 0x0018212Fu,
                    surface.todoAddPressed ? (darkSurface ? 0.13 : 0.085)
                        : surface.todoAddHovered ? (darkSurface ? 0.085 : 0.052)
                        : (darkSurface ? 0.045 : 0.028));
                const auto addFont = CreateFontW(
                    -dipToPixels(12.0, surface), 0, 0, 0, FW_NORMAL,
                    FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, ResolveLayeredSurfaceTextQuality(),
                    DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
                const auto previousAddFont = addFont == nullptr
                    ? nullptr : SelectObject(surface.memoryDc, addFont);
                SetTextColor(surface.memoryDc, darkSurface
                    ? RGB(185, 190, 200)
                    : crystalSurface ? RGB(38, 48, 61) : RGB(68, 73, 82));
                const auto addTodoText = tr(L"添加待办", L"Add task");
                SIZE addTextSize{};
                GetTextExtentPoint32W(surface.memoryDc, addTodoText.c_str(),
                    static_cast<int>(addTodoText.size()), &addTextSize);
                const auto iconWidth = dipToPixels(10.0, surface);
                const auto gap = dipToPixels(6.0, surface);
                const auto groupWidth = iconWidth + gap + addTextSize.cx;
                const auto groupLeft = (addRect.left + addRect.right - groupWidth) / 2;
                RECT addText{groupLeft + iconWidth + gap, addRect.top,
                    groupLeft + groupWidth, addRect.bottom};
                drawSurfaceText(addTodoText.c_str(), -1, &addText,
                    DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
                const auto plusX = groupLeft + iconWidth / 2;
                const auto plusY = (addRect.top + addRect.bottom) / 2;
                const auto plusColor = darkSurface ? 0x00B1B6BFu
                    : crystalSurface ? 0x0026303Du : 0x00444A52u;
                const auto plusRadius = std::max(
                    0.5, dipToPixels(1.0, surface) / 2.0);
                drawContentSegment(plusX - dipToPixels(4.0, surface), plusY,
                    plusX + dipToPixels(4.0, surface), plusY,
                    plusRadius, plusColor);
                drawContentSegment(plusX, plusY - dipToPixels(4.0, surface),
                    plusX, plusY + dipToPixels(4.0, surface),
                    plusRadius, plusColor);
                if (previousAddFont != nullptr) {
                    SelectObject(surface.memoryDc, previousAddFont);
                }
                if (addFont != nullptr) DeleteObject(addFont);
            }
            if (entries.empty() && !surface.todoCalendarOpen) {
                RECT emptyRect{dipToPixels(16.0, surface), dipToPixels(138.0, surface),
                    surface.width - dipToPixels(16.0, surface), visibleBottom - dipToPixels(12.0, surface)};
                const auto emptyFont = CreateFontW(
                    -dipToPixels(11.0, surface), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    ResolveLayeredSurfaceTextQuality(),
                    DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
                const auto previousEmptyFont = emptyFont == nullptr
                    ? nullptr : SelectObject(surface.memoryDc, emptyFont);
                SetTextColor(surface.memoryDc,
                    darkSurface ? RGB(159, 165, 176) : RGB(76, 81, 90));
                const auto emptyTodoText = tr(L"暂无待办", L"No tasks");
                drawSurfaceText(emptyTodoText.c_str(), -1, &emptyRect,
                    DT_CENTER | DT_SINGLELINE | DT_BOTTOM | DT_NOPREFIX);
                if (previousEmptyFont != nullptr) SelectObject(surface.memoryDc, previousEmptyFont);
                if (emptyFont != nullptr) DeleteObject(emptyFont);
            }
            if (surface.todoCalendarOpen) {
                const auto panel = todoCalendarRect(surface);
                const auto panelLeft = std::clamp<LONG>(panel.left, 0, surface.width);
                const auto panelTop = std::clamp<LONG>(panel.top, 0, visibleBottom);
                const auto panelRight = std::clamp<LONG>(panel.right, panelLeft, surface.width);
                const auto panelBottom = std::clamp<LONG>(panel.bottom, panelTop, visibleBottom);
                for (auto y = panelTop; y < panelBottom; ++y) {
                    std::fill(
                        contentOverlay.begin() + static_cast<std::size_t>(y) * surface.width + panelLeft,
                        contentOverlay.begin() + static_cast<std::size_t>(y) * surface.width + panelRight,
                        0u);
                }
                const presentation::RoundedRectSpec panelSpec{
                    .width = static_cast<double>(panel.right - panel.left),
                    .height = static_cast<double>(panel.bottom - panel.top),
                    .radius = static_cast<double>(dipToPixels(12.0, surface)),
                };
                for (int y = panel.top; y < panel.bottom; ++y) {
                    for (int x = panel.left; x < panel.right; ++x) {
                        const auto coverage = presentation::SampleRoundedRectCoverage(
                            panelSpec, x - panel.left + 0.5, y - panel.top + 0.5);
                        const auto panelColor = crystalSurface
                            ? 0x00F4FAFFu
                            : darkSurface ? 0x0021242Au : 0x00F4F6F9u;
                        blendVisibleContent(x, y, panelColor,
                            coverage * (crystalSurface ? crystalStyle.itemFillOpacity + 0.08 : 1.0));
                    }
                }
                drawContentRoundedOutline(panel, dipToPixels(12.0, surface),
                    std::max(1.0, static_cast<double>(dipToPixels(1.0, surface))),
                    crystalSurface ? 0x00FFFFFFu
                        : darkSurface ? 0x00666C78u : 0x00AEB8C6u,
                    crystalSurface ? crystalStyle.surfaceOutlineOpacity : 0.72);
                const auto previousRect = todoCalendarPreviousRect(surface);
                const auto nextRect = todoCalendarNextRect(surface);
                if (surface.todoCalendarPressed == -2) {
                    drawContentRoundedFill(previousRect, dipToPixels(8.0, surface),
                        darkSurface || crystalSurface ? 0x00FFFFFFu : 0x0018212Fu, 0.14);
                }
                if (surface.todoCalendarPressed == -1) {
                    drawContentRoundedFill(nextRect, dipToPixels(8.0, surface),
                        darkSurface || crystalSurface ? 0x00FFFFFFu : 0x0018212Fu, 0.14);
                }
                const auto calendarFont = CreateFontW(
                    -dipToPixels(11.0, surface), 0, 0, 0, FW_NORMAL,
                    FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, ResolveLayeredSurfaceTextQuality(),
                    DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
                const auto calendarTitleFont = CreateFontW(
                    -dipToPixels(12.0, surface), 0, 0, 0, FW_SEMIBOLD,
                    FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, ResolveLayeredSurfaceTextQuality(),
                    DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
                const auto previousCalendarFont = calendarFont == nullptr
                    ? nullptr : SelectObject(surface.memoryDc, calendarFont);
                const auto calendarColor = crystalSurface ? RGB(25, 34, 46)
                    : darkSurface ? RGB(225, 228, 234) : RGB(42, 47, 56);
                SetTextColor(surface.memoryDc, calendarColor);
                wchar_t monthTitle[32]{};
                if (usesEnglish()) {
                    constexpr std::array months{L"January", L"February", L"March", L"April",
                        L"May", L"June", L"July", L"August", L"September", L"October",
                        L"November", L"December"};
                    swprintf_s(monthTitle, L"%s %d",
                        months[std::clamp<int>(surface.todoCalendarMonth.month, 1, 12) - 1],
                        surface.todoCalendarMonth.year);
                } else {
                    swprintf_s(monthTitle, L"%d年%u月", surface.todoCalendarMonth.year,
                        static_cast<unsigned>(surface.todoCalendarMonth.month));
                }
                const auto previousTitleFont = calendarTitleFont == nullptr
                    ? nullptr : SelectObject(surface.memoryDc, calendarTitleFont);
                auto titleRect = panel;
                titleRect.bottom = panel.top + dipToPixels(44.0, surface);
                drawSurfaceText(monthTitle, -1, &titleRect,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                if (previousTitleFont != nullptr) SelectObject(surface.memoryDc, previousTitleFont);
                const auto glyphFont = CreateFontW(
                    -dipToPixels(11.0, surface), 0, 0, 0, FW_NORMAL,
                    FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, ResolveLayeredSurfaceTextQuality(),
                    DEFAULT_PITCH | FF_DONTCARE, WindowsIconFontFamily().data());
                const auto oldGlyph = glyphFont == nullptr
                    ? nullptr : SelectObject(surface.memoryDc, glyphFont);
                auto previousGlyphRect = previousRect;
                auto nextGlyphRect = nextRect;
                drawSurfaceText(L"\uE76B", 1, &previousGlyphRect,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                drawSurfaceText(L"\uE76C", 1, &nextGlyphRect,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                if (oldGlyph != nullptr) SelectObject(surface.memoryDc, oldGlyph);
                if (glyphFont != nullptr) DeleteObject(glyphFont);
                const std::array weekdays = usesEnglish()
                    ? std::array<std::wstring_view, 7>{L"M", L"T", L"W", L"T", L"F", L"S", L"S"}
                    : std::array<std::wstring_view, 7>{L"一", L"二", L"三", L"四", L"五", L"六", L"日"};
                const auto weekdayTop = panel.top + dipToPixels(43.0, surface);
                for (std::size_t column = 0; column < 7; ++column) {
                    RECT weekdayRect{
                        panel.left + static_cast<LONG>(column) * (panel.right - panel.left) / 7,
                        weekdayTop,
                        panel.left + static_cast<LONG>(column + 1) * (panel.right - panel.left) / 7,
                        panel.top + dipToPixels(68.0, surface)};
                    drawSurfaceText(weekdays[column].data(), 1, &weekdayRect,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                }
                const auto today = CurrentTodoDate(surface.timeZoneOffsetMinutes);
                const auto selected = domain::AddTodoDays(today, surface.todoAddDateOffset);
                for (std::size_t index = 0; index < 42; ++index) {
                    const auto date = todoCalendarCellDate(surface, index);
                    auto dayRect = todoCalendarDayRect(surface, index);
                    const auto selectedDay = date == selected;
                    const auto currentMonth = date.year == surface.todoCalendarMonth.year
                        && date.month == surface.todoCalendarMonth.month;
                    const auto todayDay = date == today;
                    const auto heat = todoDateActivity(surface, date);
                    auto fill = dayRect;
                    InflateRect(&fill, -dipToPixels(3.0, surface), -dipToPixels(2.0, surface));
                    if (selectedDay || surface.todoCalendarPressed == static_cast<int>(index)) {
                        drawContentRoundedFill(fill, dipToPixels(7.0, surface),
                            selectedDay ? 0x002B76D6u
                                : darkSurface || crystalSurface ? 0x00FFFFFFu : 0x0018212Fu,
                            selectedDay ? 0.96 : 0.12);
                    } else if (heat > 0) {
                        const auto heatColor = heat >= 5 ? 0x00589CFFu
                            : heat >= 3 ? 0x002F71DCu
                            : heat == 2 ? 0x00244E8Au : 0x001C3456u;
                        const auto heatOpacity = crystalSurface
                            ? (heat >= 5 ? 0.62 : heat >= 3 ? 0.48 : heat == 2 ? 0.36 : 0.24)
                            : (heat >= 5 ? 0.92 : heat >= 3 ? 0.78 : heat == 2 ? 0.58 : 0.38);
                        drawContentRoundedFill(fill, dipToPixels(7.0, surface),
                            heatColor, heatOpacity);
                    }
                    if (todayDay && !selectedDay) {
                        drawContentRoundedOutline(fill, dipToPixels(7.0, surface),
                            std::max(1.0, static_cast<double>(dipToPixels(1.0, surface))),
                            crystalSurface ? 0x00D8EBFFu : 0x003388FFu, 0.95);
                    }
                    wchar_t dayText[4]{};
                    swprintf_s(dayText, L"%u", static_cast<unsigned>(date.day));
                    SetTextColor(surface.memoryDc,
                        selectedDay || heat >= 3 ? RGB(255, 255, 255)
                            : todayDay && crystalSurface ? RGB(29, 94, 184)
                            : heat > 0 && crystalSurface ? RGB(236, 242, 250)
                            : currentMonth ? calendarColor
                            : crystalSurface ? RGB(92, 104, 118)
                            : darkSurface ? RGB(174, 187, 202) : RGB(147, 153, 163));
                    drawSurfaceText(dayText, -1, &dayRect,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                }
                if (previousCalendarFont != nullptr) {
                    SelectObject(surface.memoryDc, previousCalendarFont);
                }
                if (calendarFont != nullptr) DeleteObject(calendarFont);
                if (calendarTitleFont != nullptr) DeleteObject(calendarTitleFont);
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
                    CLIP_DEFAULT_PRECIS, ResolveLayeredSurfaceTextQuality(),
                    DEFAULT_PITCH | FF_DONTCARE,
                    L"Segoe UI Variable Text");
                const auto previousEmptyFont = emptyFont == nullptr
                    ? nullptr : SelectObject(surface.memoryDc, emptyFont);
                SetTextColor(surface.memoryDc, emptyColor);
                const auto emptyCardText = tr(L"拖放文件至此", L"Drop files here");
                drawSurfaceText(
                    emptyCardText.c_str(),
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
                static_cast<int>(std::lround(
                    ((card.type == domain::CardType::Application
                            || card.type == domain::CardType::Mapping)
                        && card.mappingPresentationMode
                            == domain::MappingPresentationMode::List
                        ? settings.itemHeight : settings.itemWidth) * scale)));
            const auto projection = projectedItems(surface);
            auto visualSlotCount = projectedSlotCount(surface, projection);
            if (surface.dropInsertionIndex.has_value()) {
                visualSlotCount = std::max(visualSlotCount, *surface.dropInsertionIndex + 1);
            }
            const auto persistentIconBackground = showIconBackgroundFrame;
            if (persistentIconBackground || surface.hoveredItem.has_value()) {
                const auto backgroundHover = surface.hoveredItem.has_value();
                const auto hovered = backgroundHover
                    ? std::ranges::find(
                        projection, *surface.hoveredItem, &ProjectedItem::itemIndex)
                    : projection.end();
                if (persistentIconBackground) {
                    // Crystal tiles use a lighter fill and a separately
                    // composited bright rim, matching the material hierarchy
                    // of the outer Card. The global frame option applies the
                    // same geometry to the other appearance presets.
                    for (const auto& projectedItem : projection) {
                        auto hotspot = itemRect(
                            surface, projectedItem.column, projectedItem.row, visualSlotCount);
                        const auto inset = dipToPixels(3.0, surface);
                        hotspot.left += inset;
                        hotspot.top += inset;
                        hotspot.right -= inset;
                        hotspot.bottom -= inset;
                        const auto tileRadius = static_cast<double>(
                            dipToPixels(9.0, surface));
                        const auto fillOpacity = crystalSurface
                            ? crystalStyle.itemFillOpacity : 0.07;
                        const auto outlineOpacity = crystalSurface
                            ? crystalStyle.itemOutlineOpacity
                            : darkSurface ? 0.18 : 0.14;
                        const auto fillColor = crystalSurface
                            ? 0x00F4FAFFu
                            : darkSurface ? 0x00FFFFFFu : 0x001F2937u;
                        const auto outlineColor = crystalSurface || darkSurface
                            ? 0x00FFFFFFu : 0x006D7683u;
                        drawRoundedFill(
                            hotspot,
                            tileRadius,
                            fillColor,
                            fillOpacity,
                            crystalSurface ? fillOpacity : 0.0);
                        drawRoundedOutline(
                            hotspot,
                            tileRadius,
                            static_cast<double>(dipToPixels(1.0, surface)),
                            outlineColor,
                            outlineOpacity,
                            crystalSurface ? outlineOpacity : 0.0);
                    }
                }
                if (backgroundHover) {
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
                                    coverage * (darkSurface ? 0.10 : 0.11));
                            }
                        }
                    }
                }
            }

            if (surface.dropInsertionIndex.has_value()) {
                const auto layoutColumns = itemLayout(surface, 0).columns;
                const auto previewColumn = *surface.dropInsertionIndex % layoutColumns;
                const auto previewRow = *surface.dropInsertionIndex / layoutColumns;
                const auto preview = itemRect(
                    surface, previewColumn, previewRow, visualSlotCount);
                const auto occupied = std::ranges::any_of(projection, [&](const auto& item) {
                    return item.column == previewColumn && item.row == previewRow;
                });
                if (!occupied) {
                    auto emptyPreview = preview;
                    const auto inset = dipToPixels(3.0, surface);
                    emptyPreview.left += inset;
                    emptyPreview.top += inset;
                    emptyPreview.right -= inset;
                    emptyPreview.bottom -= inset;
                    const auto previewRadius = static_cast<double>(dipToPixels(9.0, surface));
                    const auto centerX = (emptyPreview.left + emptyPreview.right) / 2.0;
                    const auto centerY = (emptyPreview.top + emptyPreview.bottom) / 2.0;
                    const auto halfPreviewWidth = (emptyPreview.right - emptyPreview.left) / 2.0;
                    const auto halfPreviewHeight = (emptyPreview.bottom - emptyPreview.top) / 2.0;
                    for (int y = emptyPreview.top; y < emptyPreview.bottom; ++y) {
                        for (int x = emptyPreview.left; x < emptyPreview.right; ++x) {
                            const auto qx = std::abs(x + 0.5 - centerX)
                                - (halfPreviewWidth - previewRadius);
                            const auto qy = std::abs(y + 0.5 - centerY)
                                - (halfPreviewHeight - previewRadius);
                            const auto outsideX = std::max(qx, 0.0);
                            const auto outsideY = std::max(qy, 0.0);
                            const auto distance = std::sqrt(
                                outsideX * outsideX + outsideY * outsideY)
                                + std::min(std::max(qx, qy), 0.0) - previewRadius;
                            const auto fillCoverage = std::clamp(0.5 - distance, 0.0, 1.0);
                            blendRgb(
                                x, y, 0x004A84FFu,
                                fillCoverage * (darkSurface ? 0.12 : 0.08));
                        }
                    }
                    drawDashedRoundedBorder(
                        emptyPreview, previewRadius, 0x004A84FFu,
                        darkSurface ? 0.96 : 0.86);
                } else {
                    // An occupied slot is represented by an insertion rule,
                    // never by tinting the file's own hotspot blue.
                    const auto lineThickness = std::max(2, dipToPixels(2.0, surface));
                    RECT line{};
                    if (previewColumn > 0) {
                        const auto x = preview.left;
                        line = {x - lineThickness / 2, preview.top + dipToPixels(6.0, surface),
                            x + (lineThickness + 1) / 2,
                            preview.bottom - dipToPixels(6.0, surface)};
                    } else {
                        const auto y = preview.top;
                        line = {preview.left + dipToPixels(6.0, surface),
                            y - lineThickness / 2,
                            preview.right - dipToPixels(6.0, surface),
                            y + (lineThickness + 1) / 2};
                    }
                    drawDashedRoundedBorder(
                        line, static_cast<double>(lineThickness), 0x004A84FFu,
                        darkSurface ? 0.96 : 0.86);
                }
            }

            for (const auto& projected : projection) {
                const auto slot = itemRect(
                    surface, projected.column, projected.row, visualSlotCount);
                if (slot.bottom <= slot.top || slot.right <= slot.left
                    || slot.top >= visibleBottom) {
                    continue;
                }
            }

            const auto listPresentation = (card.type == domain::CardType::Application
                    || card.type == domain::CardType::Mapping)
                && card.mappingPresentationMode == domain::MappingPresentationMode::List;
            const auto itemFont = !card.content.showItemNames && !listPresentation
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
                ResolveLayeredSurfaceTextQuality(),
                DEFAULT_PITCH | FF_DONTCARE,
                L"Segoe UI Variable Text");
            const auto previousItemFont = itemFont == nullptr
                ? nullptr
                : SelectObject(surface.memoryDc, itemFont);
            if (card.content.showItemNames || listPresentation) {
                for (const auto& projected : projection) {
                    const auto slot = itemRect(
                        surface, projected.column, projected.row, visualSlotCount);
                    if (slot.bottom <= slot.top || slot.right <= slot.left
                        || slot.top >= visibleBottom) {
                        continue;
                    }
                    const auto& item = card.items[projected.itemIndex];
                    const auto ready = item.state == presentation::CardItemState::Ready
                        || item.state == presentation::CardItemState::IconUnavailable;
                    const auto labelColor = ready
                        ? foreground
                        : (darkSurface ? RGB(148, 153, 162) : RGB(121, 126, 135));
                    SetTextColor(surface.memoryDc, labelColor);
                    auto labelRect = itemLabelRect(
                        surface,
                        projected.column,
                        projected.row,
                        visualSlotCount,
                        visibleBottom,
                        showIconBackgroundFrame);
                    drawSurfaceText(
                        item.displayName.c_str(),
                        -1,
                        &labelRect,
                        (listPresentation ? DT_LEFT : DT_CENTER)
                            | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
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
        if (card.mappingCanNavigateUp) {
            const auto up = mappingUpControlRect(surface);
            if (surface.mappingUpHovered || surface.mappingUpPressed) {
                drawRoundedFill(
                    up,
                    dipToPixels(10.0, surface),
                    darkSurface ? 0x00FFFFFFu : 0x0018212Fu,
                    surface.mappingUpPressed ? (darkSurface ? 0.14 : 0.10)
                        : (darkSurface ? 0.08 : 0.055));
            }
            const auto iconColor = darkSurface ? 0x00D3D7DFu
                : crystalSurface ? 0x00293342u : 0x004C515Au;
            const auto stroke = std::max(0.5, dipToPixels(1.5, surface) / 2.0);
            const auto centerX = (up.left + up.right) / 2.0;
            const auto centerY = (up.top + up.bottom) / 2.0;
            const auto half = dipToPixels(5.0, surface);
            drawContentSegment(centerX - half, centerY + half / 2.0,
                centerX, centerY - half / 2.0, stroke, iconColor);
            drawContentSegment(centerX, centerY - half / 2.0,
                centerX + half, centerY + half / 2.0, stroke, iconColor);
        }
        if (card.showPresentationControl
            && (card.type == domain::CardType::Application
                || card.type == domain::CardType::Mapping)) {
            if (surface.mappingViewHovered || surface.mappingViewPressed) {
                drawRoundedFill(
                    mappingControl,
                    dipToPixels(10.0, surface),
                    darkSurface ? 0x00FFFFFFu : 0x0018212Fu,
                    surface.mappingViewPressed ? (darkSurface ? 0.14 : 0.10)
                        : (darkSurface ? 0.08 : 0.055));
            }
            const auto iconColor = darkSurface ? 0x00D3D7DFu
                : crystalSurface ? 0x00293342u : 0x004C515Au;
            const auto iconStrokeRadius = std::max(
                0.5, dipToPixels(1.25, surface) / 2.0);
            const auto centerX = (mappingControl.left + mappingControl.right) / 2;
            const auto centerY = (mappingControl.top + mappingControl.bottom) / 2;
            if (card.mappingPresentationMode == domain::MappingPresentationMode::Grid) {
                const auto cell = dipToPixels(4.0, surface);
                const auto gap = dipToPixels(3.0, surface);
                for (int row = 0; row < 2; ++row) {
                    for (int column = 0; column < 2; ++column) {
                        const auto left = centerX - cell - gap / 2 + column * (cell + gap);
                        const auto top = centerY - cell - gap / 2 + row * (cell + gap);
                        drawContentSegment(
                            left, top, left + cell, top,
                            iconStrokeRadius, iconColor);
                        drawContentSegment(
                            left + cell, top, left + cell, top + cell,
                            iconStrokeRadius, iconColor);
                        drawContentSegment(
                            left + cell, top + cell, left, top + cell,
                            iconStrokeRadius, iconColor);
                        drawContentSegment(
                            left, top + cell, left, top,
                            iconStrokeRadius, iconColor);
                    }
                }
            } else {
                for (int row = -1; row <= 1; ++row) {
                    const auto y = centerY + row * dipToPixels(5.0, surface);
                    drawContentSegment(
                        centerX - dipToPixels(7.0, surface), y,
                        centerX + dipToPixels(7.0, surface), y,
                        iconStrokeRadius, iconColor);
                }
            }
        }
        if (card.showPinControl) {
            const auto pin = pinControlRect(surface);
            if (surface.pinHovered || surface.pinPressed) {
                const auto centerX = (pin.left + pin.right) / 2.0;
                const auto centerY = (pin.top + pin.bottom) / 2.0;
                const auto radius = static_cast<double>(dipToPixels(14.0, surface));
                for (int y = pin.top; y < pin.bottom; ++y) {
                    for (int x = pin.left; x < pin.right; ++x) {
                        const auto distance = std::hypot(x + 0.5 - centerX, y + 0.5 - centerY);
                        blendRgb(x, y, darkSurface ? 0x00FFFFFFu : 0x00000000u,
                            std::clamp(radius + 0.5 - distance, 0.0, 1.0)
                                * (surface.pinPressed ? 0.12 : 0.06));
                    }
                }
            }
            const auto pinColor = surface.alwaysOnTop
                ? (darkSurface ? RGB(127, 179, 255)
                    : crystalSurface ? RGB(29, 94, 184) : RGB(45, 115, 213))
                : (darkSurface ? RGB(229, 232, 237)
                    : crystalSurface ? RGB(33, 43, 56) : RGB(91, 96, 105));
            const auto pinFont = CreateFontW(
                -dipToPixels(17.0, surface), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                ResolveLayeredSurfaceTextQuality(), DEFAULT_PITCH | FF_DONTCARE,
                WindowsIconFontFamily().data());
            const auto oldPinFont = pinFont == nullptr
                ? nullptr : SelectObject(surface.memoryDc, pinFont);
            SetTextColor(surface.memoryDc, pinColor);
            SetBkMode(surface.memoryDc, TRANSPARENT);
            const auto glyph = surface.alwaysOnTop ? L"\uE840" : L"\uE718";
            auto pinGlyphRect = pin;
            drawSurfaceText(glyph, 1, &pinGlyphRect,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            if (oldPinFont != nullptr) SelectObject(surface.memoryDc, oldPinFont);
            if (pinFont != nullptr) DeleteObject(pinFont);
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
            const auto chevronColor = darkSurface ? 0x00E5E8EDu
                : crystalSurface ? 0x00212B38u : 0x005B6069u;
            const auto halfWidth = 5.0 * scale;
            const auto halfHeight = 2.75 * scale;
            const auto direction = card.expanded ? -1.0 : 1.0;
            const auto middleY = centerY + direction * halfHeight;
            const auto sideY = centerY - direction * halfHeight;
            const auto strokeRadius = std::max(0.75, 0.8 * scale);
            drawContentSegment(
                centerX - halfWidth, sideY, centerX, middleY,
                strokeRadius, chevronColor);
            drawContentSegment(
                centerX, middleY, centerX + halfWidth, sideY,
                strokeRadius, chevronColor);
        }

        const auto maximumScroll = maximumScrollOffset(surface);
        const auto rowLimit = visibleRowLimit(surface);
        if (card.expanded && maximumScroll > 0 && rowLimit.has_value()) {
            const auto trackTop = card.type == domain::CardType::Todo
                ? dipToPixels(128.0, surface)
                : dipToPixels(56.0, surface);
            const auto trackBottom = visibleBottom - dipToPixels(10.0, surface);
            const auto trackHeight = std::max<LONG>(1, trackBottom - trackTop);
            const auto visibleRows = static_cast<double>(*rowLimit);
            const auto totalRows = visibleRows + static_cast<double>(maximumScroll);
            const auto thumbHeight = std::clamp(
                static_cast<LONG>(std::lround(trackHeight * visibleRows / totalRows)),
                static_cast<LONG>(dipToPixels(24.0, surface)), trackHeight);
            const auto travel = trackHeight - thumbHeight;
            const auto thumbTop = trackTop + static_cast<LONG>(std::lround(
                travel * static_cast<double>(surface.scrollRowOffset)
                    / static_cast<double>(maximumScroll)));
            const auto right = surface.width - dipToPixels(5.0, surface);
            const auto track = RECT{
                right - dipToPixels(3.0, surface), trackTop,
                right, trackBottom};
            drawRoundedFill(
                track,
                dipToPixels(2.0, surface),
                darkSurface ? 0x00FFFFFFu : 0x0018212Fu,
                darkSurface ? 0.08 : 0.06);
            const auto thumb = RECT{
                right - dipToPixels(3.0, surface), thumbTop,
                right, thumbTop + thumbHeight};
            drawRoundedFill(
                thumb,
                dipToPixels(2.0, surface),
                darkSurface ? 0x00FFFFFFu : 0x0018212Fu,
                darkSurface ? 0.46 : 0.34);
        }

        const auto surfaceAlpha = (crystalSurface
            ? crystalStyle.surfaceOpacity
            : std::clamp(card.opacity, 0.0, 1.0)) * 255.0;
        const presentation::RoundedRectSpec surfaceShape{
            .width = static_cast<double>(surface.width),
            .height = static_cast<double>(visibleBottom),
            .radius = static_cast<double>(radius),
            .strokeWidth = static_cast<double>(std::max(1, dipToPixels(1.0, surface))),
        };
        for (int y = 0; y < visibleBottom; ++y) {
            for (int x = 0; x < surface.width; ++x) {
                const auto coverage = presentation::SampleRoundedRectCoverage(
                    surfaceShape, x + 0.5, y + 0.5);
                auto& pixel = surface.pixels[y * surface.width + x];
                auto pixelAlpha = surfaceAlpha;
                if (!materialAlphaBoost.empty()) {
                    const auto materialCoverage = static_cast<double>(
                        materialAlphaBoost[
                            static_cast<std::size_t>(y) * surface.width + x]) / 255.0;
                    pixelAlpha += (255.0 - pixelAlpha) * materialCoverage;
                }
                pixel = CompositeCrystalLayerPixel(
                    pixel,
                    static_cast<std::uint32_t>(std::lround(pixelAlpha)),
                    contentOverlay[
                        static_cast<std::size_t>(y) * surface.width + x],
                    coverage);

                const auto edgeCoverage = presentation::SampleInnerRoundedOutlineCoverage(
                    surfaceShape, x + 0.5, y + 0.5);
                if (edgeCoverage > 0.0) {
                    const auto edgeColor = darkSurface || brandSurface || crystalSurface
                        ? 0x00FFFFFFu : 0x00636B78u;
                    const auto edgeOpacity = crystalSurface
                        ? crystalStyle.surfaceOutlineOpacity
                        : darkSurface || brandSurface ? 0.16 : 0.20;
                    const auto sourceAlpha = static_cast<std::uint32_t>(std::lround(
                        (crystalSurface ? 255.0 : surfaceAlpha)
                            * edgeCoverage * edgeOpacity));
                    const auto inverse = 255u - sourceAlpha;
                    const auto composite = [&](int shift) {
                        const auto source = ((edgeColor >> shift) & 0xFFu)
                            * sourceAlpha / 255u;
                        const auto destination = (pixel >> shift) & 0xFFu;
                        return std::min(255u, source + destination * inverse / 255u);
                    };
                    const auto outputAlpha = std::min(
                        255u, sourceAlpha + ((pixel >> 24) & 0xFFu) * inverse / 255u);
                    pixel = (outputAlpha << 24) | (composite(16) << 16)
                        | (composite(8) << 8) | composite(0);
                }
            }
        }

        if (card.expanded && (!card.items.empty() || surface.dropInsertionIndex.has_value())) {
            const auto scale = display.effectiveDpi / 96.0;
            const auto settings = itemLayoutSettings(surface);
            const auto iconSize = std::max(
                1, static_cast<int>(std::lround(settings.iconSize * scale)));
            const auto listPresentation = (card.type == domain::CardType::Application
                    || card.type == domain::CardType::Mapping)
                && card.mappingPresentationMode == domain::MappingPresentationMode::List;
            const auto projection = projectedItems(surface);
            auto visualSlotCount = projectedSlotCount(surface, projection);
            if (surface.dropInsertionIndex.has_value()) {
                visualSlotCount = std::max(visualSlotCount, *surface.dropInsertionIndex + 1);
            }
            const auto compositeIconPixel = [&](int x, int y, std::uint32_t sourcePixel) {
                if (x < 0 || y < 0 || x >= surface.width || y >= visibleBottom) return;
                // Card material opacity applies only to the background. Shell
                // icons retain their own premultiplied alpha and are composed
                // over that material at full source opacity.
                constexpr double iconOpacity = 1.0;
                const auto sourceAlpha = static_cast<std::uint32_t>(std::lround(
                    static_cast<double>((sourcePixel >> 24) & 0xFFu) * iconOpacity));
                if (sourceAlpha == 0) return;
                auto& destination = surface.pixels[y * surface.width + x];
                const auto inverse = 255u - sourceAlpha;
                const auto composite = [&](int shift) {
                    const auto source = static_cast<std::uint32_t>(std::lround(
                        static_cast<double>((sourcePixel >> shift) & 0xFFu) * iconOpacity));
                    const auto target = (destination >> shift) & 0xFFu;
                    return std::min(255u, source + target * inverse / 255u);
                };
                const auto outputAlpha = std::min(
                    255u, sourceAlpha + ((destination >> 24) & 0xFFu) * inverse / 255u);
                destination = (outputAlpha << 24) | (composite(16) << 16)
                    | (composite(8) << 8) | composite(0);
            };
            for (const auto& projected : projection) {
                const auto& item = card.items[projected.itemIndex];
                if (item.icon.empty()) continue;
                const auto slot = itemRect(
                    surface, projected.column, projected.row, visualSlotCount);
                if (slot.bottom <= slot.top || slot.right <= slot.left
                    || slot.top >= visibleBottom) continue;
                const auto chrome = resolveItemNameLayout(
                    surface, projected.column, projected.row, visualSlotCount,
                    visibleBottom, showIconBackgroundFrame);
                const auto iconLeft = chrome.iconLeft;
                const auto iconTop = chrome.iconTop;
                for (int targetY = 0; targetY < iconSize; ++targetY) {
                    for (int targetX = 0; targetX < iconSize; ++targetX) {
                        const auto sourcePixel = iconSize == item.icon.width
                                && iconSize == item.icon.height
                            ? (*item.icon.premultipliedPixels)[
                                static_cast<std::size_t>(targetY) * item.icon.width
                                    + static_cast<std::size_t>(targetX)]
                            : presentation::SamplePremultipliedBilinear(
                                *item.icon.premultipliedPixels,
                                item.icon.width,
                                item.icon.height,
                                iconSize,
                                iconSize,
                                targetX,
                                targetY);
                        compositeIconPixel(
                            iconLeft + targetX, iconTop + targetY, sourcePixel);
                    }
                }
            }
        }

        if (commit) commitSurface(surface);
    }

    void commitSurfaceAt(Surface& surface, POINT destination) {
        const auto commitStartedAt = std::chrono::steady_clock::now();
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
        ++renderStatistics.fullSurfaceCommits;
        renderStatistics.fullSurfaceCommitNanoseconds += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - commitStartedAt).count());
        const auto visibleHeight = visibleHeightDip(surface);
        const auto heightChanged = surface.lastCommittedVisibleHeightDip.has_value()
            && std::abs(*surface.lastCommittedVisibleHeightDip - visibleHeight) > 0.01;
        surface.lastCommittedVisibleHeightDip = visibleHeight;
        if (heightChanged) reflowVerticalFollowers(surface, true);
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
        moved->projection.horizontalAnchor = resolved.horizontalAnchor;
        moved->projection.verticalAnchor = resolved.verticalAnchor;
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
                zOrderTarget(*surface),
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
        // Moving a Card changes only its own placement. Re-infer stack links
        // from the current positions without translating any follower.
        inferVerticalLeaders();
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
                .timeZoneOffsetMinutes = timeZoneOffsetMinutes,
                .ordinal = surfaces.size() + 1,
                .width = std::max(1, static_cast<int>(std::lround(projection.rect.width * scale))),
                .height = std::max(1, static_cast<int>(std::lround(projection.rect.height * scale))),
                .alwaysOnTop = card->pinOnTop,
            };
            surfaces.push_back(std::move(surface));
        }

        // Placement relationships belong to the persisted layout. Infer them
        // before content-driven sizing changes the projected rectangles.
        inferVerticalLeaders();
        try {
            for (auto& surface : surfaces) {
                resizeSurfaceForContent(surface, false);
                createSurface(surface);
                render(surface, surface.display, surface.card, surface.ordinal);
            }
        } catch (...) {
            destroySurfaces();
            throw;
        }
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
                zOrderTarget(surface),
                left,
                top,
                surface.width,
                surface.height,
                SWP_NOACTIVATE
                    | (surfaceShouldBeVisible(surface) ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
            if (deferred == nullptr) {
                throw std::runtime_error("DeferWindowPos failed for desktop host.");
            }
        }
        if (!surfaces.empty() && !EndDeferWindowPos(deferred)) {
            throw std::runtime_error("EndDeferWindowPos failed for desktop host.");
        }
    }

    void insertCard(
        std::span<const domain::PlacementProjection> projections,
        const presentation::CardView& card) {
        if (!windowClassRegistered) initialize();
        if (std::ranges::any_of(surfaces, [&](const Surface& surface) {
                return surface.card.id == card.id;
            })) {
            throw std::invalid_argument("Card already has a desktop surface.");
        }

        const auto insertionStart = surfaces.size();
        const auto projectionCount = static_cast<std::size_t>(std::ranges::count_if(
            projections, [&](const domain::PlacementProjection& projection) {
                return projection.cardId == card.id;
            }));
        surfaces.reserve(insertionStart + projectionCount);
        try {
            for (const auto& projection : projections) {
                if (projection.cardId != card.id || !card.visible) continue;
                const auto display = std::ranges::find(
                    displays, projection.displayId, &domain::DisplaySnapshot::id);
                if (display == displays.end()) {
                    throw std::invalid_argument(
                        "Card Projection references an unknown display.");
                }
                const auto scale = display->effectiveDpi / 96.0;
                surfaces.push_back({
                    .projection = projection,
                    .display = *display,
                    .card = card,
                    .timeZoneOffsetMinutes = timeZoneOffsetMinutes,
                    .ordinal = surfaces.size() + 1,
                    .width = std::max(1, static_cast<int>(std::lround(
                        projection.rect.width * scale))),
                    .height = std::max(1, static_cast<int>(std::lround(
                        projection.rect.height * scale))),
                    .alwaysOnTop = card.pinOnTop,
                });
                auto& surface = surfaces.back();
                resizeSurfaceForContent(surface, false);
                createSurface(surface);
                render(surface, surface.display, surface.card, surface.ordinal);
            }

            const auto insertedCount = surfaces.size() - insertionStart;
            auto deferred = BeginDeferWindowPos(static_cast<int>(insertedCount));
            if (deferred == nullptr && insertedCount != 0) {
                throw std::runtime_error("BeginDeferWindowPos failed for inserted Card.");
            }
            for (auto index = insertionStart; index < surfaces.size(); ++index) {
                const auto& surface = surfaces[index];
                const auto scale = surface.display.effectiveDpi / 96.0;
                const auto left = static_cast<int>(std::lround(
                    surface.display.workAreaLeft * scale
                    + surface.projection.rect.left * scale));
                const auto top = static_cast<int>(std::lround(
                    surface.display.workAreaTop * scale
                    + surface.projection.rect.top * scale));
                deferred = DeferWindowPos(
                    deferred,
                    surface.window,
                    zOrderTarget(surface),
                    left,
                    top,
                    surface.width,
                    surface.height,
                    SWP_NOACTIVATE
                        | (surfaceShouldBeVisible(surface) ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
                if (deferred == nullptr) {
                    throw std::runtime_error("DeferWindowPos failed for inserted Card.");
                }
            }
            if (insertedCount != 0 && !EndDeferWindowPos(deferred)) {
                throw std::runtime_error("EndDeferWindowPos failed for inserted Card.");
            }
            inferVerticalLeaders();
        } catch (...) {
            for (auto index = insertionStart; index < surfaces.size(); ++index) {
                destroySurface(surfaces[index]);
            }
            surfaces.erase(
                surfaces.begin() + static_cast<std::ptrdiff_t>(insertionStart),
                surfaces.end());
            inferVerticalLeaders();
            throw;
        }
    }

    void removeCard(const domain::CardId& cardId) noexcept {
        if (cardId.empty()) return;
        finishEditorsForCard(cardId);
        for (auto& surface : surfaces) {
            if (surface.card.id == cardId) destroySurface(surface);
        }
        surfaces.erase(
            std::remove_if(surfaces.begin(), surfaces.end(), [&](const Surface& surface) {
                return surface.card.id == cardId;
            }),
            surfaces.end());
        for (std::size_t index = 0; index < surfaces.size(); ++index) {
            surfaces[index].ordinal = index + 1;
        }
        inferVerticalLeaders();
    }

    void setCardsVisible(bool visible) {
        if (cardsGloballyVisible == visible) return;
        if (!visible) {
            hideGuides();
            if (todoEditor != nullptr) finishTodoEdit(todoEditor, false);
            for (auto& surface : surfaces) clearPointerHover(surface.window, false);
        }
        cardsGloballyVisible = visible;
        onOriginVirtualDesktop = virtualDesktopIsCurrent();
        auto deferred = BeginDeferWindowPos(static_cast<int>(surfaces.size()));
        if (deferred == nullptr && !surfaces.empty()) {
            throw std::runtime_error("BeginDeferWindowPos failed for Card visibility.");
        }
        for (const auto& surface : surfaces) {
            deferred = DeferWindowPos(
                deferred, surface.window, nullptr, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER
                    | (surfaceShouldBeVisible(surface) ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
            if (deferred == nullptr) {
                throw std::runtime_error("DeferWindowPos failed for Card visibility.");
            }
        }
        if (!surfaces.empty() && !EndDeferWindowPos(deferred)) {
            throw std::runtime_error("EndDeferWindowPos failed for Card visibility.");
        }
    }

    void updateCardItems(
        const domain::CardId& cardId,
        const std::vector<presentation::CardItemView>& items) {
        std::vector<std::pair<Surface*, domain::PlacementRect>> affected;
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
            const auto previousRect = contentUpdatePreviousRect(surface);
            resizeSurfaceForContent(surface, true);
            render(surface, surface.display, surface.card, surface.ordinal, false);
            affected.emplace_back(&surface, previousRect);
        }
        for (auto& [surface, previousRect] : affected) {
            commitContentUpdate(*surface, previousRect);
        }
    }

    void updateMappingCard(
        const domain::CardId& cardId,
        domain::MappingMode mode,
        bool allowsSourceMutation,
        const std::vector<presentation::CardItemView>& items,
        domain::ApplicationItemSortMode sortMode,
        const std::vector<domain::ApplicationItemPlacement>& itemPlacements,
        bool mappingHasSource) {
        std::vector<std::pair<Surface*, domain::PlacementRect>> affected;
        for (auto& surface : surfaces) {
            if (surface.card.id != cardId
                || surface.card.type != domain::CardType::Mapping) {
                continue;
            }
            clearPointerHover(surface.window, false);
            surface.card.mappingMode = mode;
            surface.card.mappingHasSource = mappingHasSource;
            surface.card.mappingAllowsSourceMutation = allowsSourceMutation;
            surface.card.applicationSortMode = sortMode;
            surface.card.mappingSortMode = sortMode;
            surface.card.applicationItemPlacements = itemPlacements;
            surface.card.items = items;
            configureDropTarget(surface);
            const auto previousRect = contentUpdatePreviousRect(surface);
            resizeSurfaceForContent(surface, true);
            render(surface, surface.display, surface.card, surface.ordinal, false);
            affected.emplace_back(&surface, previousRect);
        }
        for (auto& [surface, previousRect] : affected) {
            commitContentUpdate(*surface, previousRect);
        }
    }

    void updateMappingNavigation(
        const domain::CardId& cardId,
        std::wstring title,
        bool canNavigateUp,
        const std::vector<presentation::CardItemView>& items,
        const std::vector<domain::ApplicationItemPlacement>& itemPlacements) {
        std::vector<std::pair<Surface*, domain::PlacementRect>> affected;
        for (auto& surface : surfaces) {
            if (surface.card.id != cardId
                || surface.card.type != domain::CardType::Mapping) continue;
            clearPointerHover(surface.window, false);
            surface.card.title = title;
            surface.card.mappingCanNavigateUp = canNavigateUp;
            surface.card.items = items;
            surface.card.applicationItemPlacements = itemPlacements;
            surface.scrollRowOffset = 0;
            const auto previousRect = contentUpdatePreviousRect(surface);
            resizeSurfaceForContent(surface, true);
            render(surface, surface.display, surface.card, surface.ordinal, false);
            affected.emplace_back(&surface, previousRect);
        }
        for (auto& [surface, previousRect] : affected) {
            commitContentUpdate(*surface, previousRect);
        }
    }

    void updateCardItemsBatch(
        const std::vector<WindowsDesktopHost::CardItemsUpdate>& updates) {
        std::vector<std::pair<Surface*, domain::PlacementRect>> affected;
        for (auto& surface : surfaces) {
            const auto update = std::find_if(
                updates.begin(), updates.end(), [&](const auto& candidate) {
                    return candidate.cardId == surface.card.id;
                });
            if (update == updates.end()) continue;
            clearPointerHover(surface.window, false);
            surface.card.items = update->items;
            surface.card.applicationSortMode = update->sortMode;
            if (surface.card.type == domain::CardType::Mapping) {
                surface.card.mappingSortMode = update->sortMode;
            }
            surface.card.applicationItemPlacements = update->itemPlacements;
            if (surface.dropInsertionIndex.has_value()) {
                surface.dropInsertionIndex = std::min(
                    *surface.dropInsertionIndex, surface.card.items.size());
            }
            const auto previousRect = contentUpdatePreviousRect(surface);
            resizeSurfaceForContent(surface, true);
            render(surface, surface.display, surface.card, surface.ordinal, false);
            affected.emplace_back(&surface, previousRect);
        }
        for (auto& [surface, previousRect] : affected) {
            commitContentUpdate(*surface, previousRect);
        }
    }

    void updateTodoItems(
        const domain::CardId& cardId,
        const std::vector<domain::TodoItem>& items) {
        std::vector<std::pair<Surface*, domain::PlacementRect>> affected;
        for (auto& surface : surfaces) {
            if (surface.card.id != cardId) continue;
            clearPointerHover(surface.window, false);
            surface.card.todoItems = items;
            const auto previousRect = contentUpdatePreviousRect(surface);
            resizeSurfaceForContent(surface, true);
            render(surface, surface.display, surface.card, surface.ordinal, false);
            affected.emplace_back(&surface, previousRect);
        }
        for (auto& [surface, previousRect] : affected) {
            commitContentUpdate(*surface, previousRect);
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
        domain::CardContentPreferences preferences,
        std::optional<std::vector<domain::ApplicationItemPlacement>> itemPlacements) {
        finishEditorsForCard(cardId);
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
        std::vector<std::pair<Surface*, domain::PlacementRect>> affected;
        for (auto& surface : surfaces) {
            if (surface.card.id != cardId) {
                continue;
            }
            clearPointerHover(surface.window, false);
            surface.card.content = preferences;
            if (itemPlacements.has_value()
                && (surface.card.type == domain::CardType::Application
                    || surface.card.type == domain::CardType::Mapping)) {
                surface.card.applicationItemPlacements = *itemPlacements;
            }
            if (itemsRefreshed) {
                surface.card.items = refreshedItems;
            }
            const auto previousRect = contentUpdatePreviousRect(surface);
            resizeSurfaceForContent(surface, true);
            render(surface, surface.display, surface.card, surface.ordinal, false);
            affected.emplace_back(&surface, previousRect);
        }
        for (auto& [surface, previousRect] : affected) {
            commitContentUpdate(*surface, previousRect);
        }
    }

    void updateCardChromePreferences(
        const domain::CardId& cardId,
        domain::CardChromePreferences preferences) {
        std::vector<std::pair<Surface*, domain::PlacementRect>> affected;
        for (auto& surface : surfaces) {
            if (surface.card.id != cardId) continue;
            surface.card.showCollapseControl = preferences.showCollapseControl;
            surface.card.showCloseControl = preferences.showCloseControl;
            surface.card.showPinControl = preferences.showPinControl;
            surface.card.showPresentationControl = preferences.showPresentationControl;
            surface.card.pinOnTop = preferences.pinOnTop;
            surface.card.positionLocked = preferences.positionLocked;
            surface.alwaysOnTop = preferences.pinOnTop;
            surface.card.showTitle = preferences.showTitle;
            applySurfaceLayering(surface);
            if (!surface.alwaysOnTop) keepOverlayAbove(surface.window);
            if (!preferences.showCollapseControl && !surface.card.expanded) {
                surface.card.expanded = true;
            }
            const auto previousRect = surface.projection.rect;
            resizeSurfaceForContent(surface, true);
            render(surface, surface.display, surface.card, surface.ordinal, false);
            affected.emplace_back(&surface, previousRect);
        }
        for (auto& [surface, previousRect] : affected) {
            commitContentUpdate(*surface, previousRect);
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
        std::vector<std::pair<Surface*, domain::PlacementRect>> affected;
        for (auto& surface : surfaces) {
            if (surface.card.id != cardId || surface.card.type != domain::CardType::Todo) continue;
            surface.card.todoPreferences = preferences;
            const auto previousRect = surface.projection.rect;
            resizeSurfaceForContent(surface, true);
            render(surface, surface.display, surface.card, surface.ordinal, false);
            affected.emplace_back(&surface, previousRect);
        }
        for (auto& [surface, previousRect] : affected) {
            commitContentUpdate(*surface, previousRect);
        }
    }

    void updateCardTitles(std::span<const presentation::CardView> cards) {
        std::vector<Surface*> affected;
        for (auto& surface : surfaces) {
            const auto view = std::find_if(cards.begin(), cards.end(), [&](const auto& candidate) {
                return candidate.id == surface.card.id;
            });
            if (view == cards.end()) continue;
            surface.card.title = view->title;
            surface.card.typeLabel = view->typeLabel;
            surface.card.showTitle = view->showTitle;
            render(surface, surface.display, surface.card, surface.ordinal, false);
            affected.push_back(&surface);
        }
        for (auto* surface : affected) commitSurface(*surface);
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
            if (message.message == ForegroundChangedMessage) {
                refreshPinnedFullscreenState();
                continue;
            }
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

void WindowsDesktopHost::insertCard(
    std::span<const domain::PlacementProjection> projections,
    const presentation::CardView& card) {
    impl_->insertCard(projections, card);
}

void WindowsDesktopHost::removeCard(const domain::CardId& cardId) noexcept {
    impl_->removeCard(cardId);
}

int WindowsDesktopHost::run(int durationMilliseconds) {
    return impl_->run(durationMilliseconds);
}

void WindowsDesktopHost::requestClose() noexcept {
    impl_->closeRequested = true;
    PostQuitMessage(0);
}

void WindowsDesktopHost::setCardsVisible(bool visible) {
    impl_->setCardsVisible(visible);
}

bool WindowsDesktopHost::cardsVisible() const noexcept {
    return impl_->cardsGloballyVisible;
}

void WindowsDesktopHost::setPinnedCardsYieldToFullscreen(bool enabled) noexcept {
    impl_->setPinnedCardsYieldToFullscreen(enabled);
}

void WindowsDesktopHost::setIconBackgroundFrameVisible(bool enabled) noexcept {
    impl_->setIconBackgroundFrameVisible(enabled);
}

void WindowsDesktopHost::setPlacementChangedCallback(PlacementChangedCallback callback) {
    impl_->placementChanged = std::move(callback);
}

void WindowsDesktopHost::setCardExpandedChangedCallback(CardExpandedChangedCallback callback) {
    impl_->cardExpandedChanged = std::move(callback);
}

void WindowsDesktopHost::setCardPinChangedCallback(CardPinChangedCallback callback) {
    impl_->cardPinChanged = std::move(callback);
}

void WindowsDesktopHost::setMappingPresentationChangedCallback(
    MappingPresentationChangedCallback callback) {
    impl_->mappingPresentationChanged = std::move(callback);
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

void WindowsDesktopHost::setCardItemContextMenuCallback(
    CardItemContextMenuCallback callback) {
    impl_->cardItemContextMenu = std::move(callback);
}

void WindowsDesktopHost::setMappingNavigateUpCallback(MappingNavigateUpCallback callback) {
    impl_->mappingNavigateUp = std::move(callback);
}

void WindowsDesktopHost::setMappingReferenceRemovedCallback(
    MappingReferenceRemovedCallback callback) {
    impl_->mappingReferenceRemoved = std::move(callback);
}

void WindowsDesktopHost::setFileDeleteConfirmationCallback(
    FileDeleteConfirmationCallback callback) {
    impl_->fileDeleteConfirmation = std::move(callback);
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

void WindowsDesktopHost::updateMappingCard(
    const domain::CardId& cardId,
    domain::MappingMode mode,
    bool allowsSourceMutation,
    std::vector<presentation::CardItemView> items,
    domain::ApplicationItemSortMode sortMode,
    std::vector<domain::ApplicationItemPlacement> itemPlacements,
    bool mappingHasSource) {
    impl_->updateMappingCard(
        cardId, mode, allowsSourceMutation, items, sortMode, itemPlacements,
        mappingHasSource);
}

void WindowsDesktopHost::updateMappingNavigation(
    const domain::CardId& cardId,
    std::wstring title,
    bool canNavigateUp,
    std::vector<presentation::CardItemView> items,
    std::vector<domain::ApplicationItemPlacement> itemPlacements) {
    impl_->updateMappingNavigation(
        cardId, std::move(title), canNavigateUp, items, itemPlacements);
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
    domain::CardContentPreferences preferences,
    std::optional<std::vector<domain::ApplicationItemPlacement>> itemPlacements) {
    impl_->updateCardContentPreferences(
        cardId, std::move(preferences), std::move(itemPlacements));
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

void WindowsDesktopHost::updateCardTitles(
    std::span<const presentation::CardView> cards) {
    impl_->updateCardTitles(cards);
}

void WindowsDesktopHost::setTimeZoneOffsetMinutes(
    std::optional<std::int32_t> offsetMinutes) {
    impl_->timeZoneOffsetMinutes = offsetMinutes;
    for (auto& surface : impl_->surfaces) {
        surface.timeZoneOffsetMinutes = offsetMinutes;
        impl_->resizeSurfaceForContent(surface, true);
        impl_->render(surface, surface.display, surface.card, surface.ordinal);
    }
}

void WindowsDesktopHost::setLanguage(std::string language) {
    if (language != "zh-CN" && language != "en-US") {
        throw std::invalid_argument("Desktop host language must be resolved.");
    }
    impl_->language = std::move(language);
}

void WindowsDesktopHost::setOverlayWindow(void* window) noexcept {
    impl_->overlayWindow = static_cast<HWND>(window);
}

WindowsDesktopHost::RenderStatistics WindowsDesktopHost::renderStatistics() const noexcept {
    return impl_->renderStatistics;
}

void WindowsDesktopHost::resetRenderStatistics() noexcept {
    impl_->renderStatistics = {};
}

} // namespace desto::platform::windows
