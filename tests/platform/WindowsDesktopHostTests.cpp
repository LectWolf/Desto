#include "TestSupport.h"
#include "WindowsDesktopHost.h"
#include "WindowsFileDragDrop.h"
#include "WindowsPopupMenu.h"
#include "WindowsTextInput.h"
#include "WindowsTrayHost.h"

#include <Windows.h>
#include <CommCtrl.h>
#include <shellapi.h>
#include <WtsApi32.h>

#include <thread>
#include <algorithm>
#include <chrono>
#include <vector>

using namespace desto::domain;
using namespace desto::platform::windows;
using namespace desto::presentation;

namespace {

HWND FindOwnedTooltip(HWND owner) {
    struct Search {
        HWND owner;
        HWND result = nullptr;
    } search{owner};
    EnumWindows([](HWND candidate, LPARAM parameter) {
        auto& value = *reinterpret_cast<Search*>(parameter);
        wchar_t className[64]{};
        if (GetWindow(candidate, GW_OWNER) == value.owner
            && GetClassNameW(candidate, className, 64) > 0
            && _wcsicmp(className, L"DestoItemTooltip") == 0) {
            value.result = candidate;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.result;
}

HWND FindTodoEditor(HWND owner) {
    struct Search {
        HWND owner;
        HWND result = nullptr;
    } search{owner};
    EnumWindows([](HWND candidate, LPARAM parameter) {
        auto& value = *reinterpret_cast<Search*>(parameter);
        wchar_t className[64]{};
        if (GetPropW(candidate, L"DestoTodoEditorSurface") == value.owner) {
            if (GetClassNameW(candidate, className, 64) > 0
                && _wcsicmp(className, L"DestoWindowsTextInput") == 0) {
                value.result = candidate;
                return FALSE;
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.result;
}

std::vector<HWND> FindHostWindows(const wchar_t* title) {
    struct Search {
        const wchar_t* title;
        std::vector<HWND> windows;
    } search{title};
    EnumWindows([](HWND candidate, LPARAM parameter) {
        auto& value = *reinterpret_cast<Search*>(parameter);
        wchar_t className[64]{};
        wchar_t windowTitle[128]{};
        if (GetClassNameW(candidate, className, 64) > 0
            && GetWindowTextW(candidate, windowTitle, 128) > 0
            && _wcsicmp(className, L"DestoDesktopHostSurface") == 0
            && _wcsicmp(windowTitle, value.title) == 0) {
            value.windows.push_back(candidate);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    std::ranges::sort(search.windows, [](HWND left, HWND right) {
        RECT leftRect{};
        RECT rightRect{};
        GetWindowRect(left, &leftRect);
        GetWindowRect(right, &rightRect);
        return leftRect.top < rightRect.top;
    });
    return search.windows;
}

bool IsWindowAbove(HWND expectedAbove, HWND expectedBelow) {
    struct Search {
        HWND above;
        HWND below;
        bool foundAbove = false;
        bool result = false;
    } search{expectedAbove, expectedBelow};
    EnumWindows([](HWND candidate, LPARAM parameter) {
        auto& value = *reinterpret_cast<Search*>(parameter);
        if (candidate == value.above) {
            value.foundAbove = true;
        } else if (candidate == value.below) {
            value.result = value.foundAbove;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.result;
}

bool WaitForWindowRect(
    HWND window,
    const RECT& expected,
    std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        MSG message{};
        while (PeekMessageW(&message, window, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        RECT actual{};
        if (GetWindowRect(window, &actual)
            && EqualRect(&actual, &expected)) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) return false;
        MsgWaitForMultipleObjectsEx(
            0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}

COLORREF RenderedClientPixel(HWND window, int x, int y) {
    RedrawWindow(window, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    const auto dc = GetDC(window);
    DESTO_CHECK(dc != nullptr);
    const auto color = GetPixel(dc, x, y);
    ReleaseDC(window, dc);
    return color;
}

IDropTarget* RegisteredDropTarget(HWND window) {
    return reinterpret_cast<IDropTarget*>(GetPropW(window, L"OleDropTargetInterface"));
}

void RunTests() {
    DESTO_CHECK(ResolveWindowsFileContextMenuMode(22621, false)
        == WindowsFileContextMenuMode::Compact);
    DESTO_CHECK(ResolveWindowsFileContextMenuMode(22621, true)
        == WindowsFileContextMenuMode::Classic);
    DESTO_CHECK(ResolveWindowsFileContextMenuMode(19045, false)
        == WindowsFileContextMenuMode::Classic);
    DESTO_CHECK((ResolveDestoTrayIconFlags() & NIF_SHOWTIP) != 0);
    DESTO_CHECK(ResolveTodoTextFontFamily(L"Plain task")
        == L"Segoe UI Variable Text");
    DESTO_CHECK(ResolveTodoTextFontFamily(L"Plan \U0001F680")
        == L"Segoe UI Variable Text");
    DESTO_CHECK(ResolveLayeredSurfaceTextQuality() == ANTIALIASED_QUALITY);
    DESTO_CHECK(ResolveLayeredSurfaceTextQuality() != CLEARTYPE_QUALITY);
    const auto crystalStyle = ResolveCrystalMaterialStyle();
    DESTO_CHECK(crystalStyle.surfaceOpacity == 0.32);
    DESTO_CHECK(crystalStyle.itemFillOpacity == 0.16);
    DESTO_CHECK(crystalStyle.itemOutlineOpacity == 0.54);
    DESTO_CHECK(crystalStyle.surfaceOutlineOpacity == 0.52);
    DESTO_CHECK(CompositeCrystalLayerPixel(
        0x00F8FAFCu, 82u, 0u, 1.0) == 0x52505051u);
    DESTO_CHECK(CompositeCrystalLayerPixel(
        0x00F8FAFCu, 82u, 0xFF26282Du, 1.0) == 0xFF26282Du);
    const auto halfTextPixel = CompositeCrystalLayerPixel(
        0x00F8FAFCu, 82u, 0x80131417u, 1.0);
    DESTO_CHECK(((halfTextPixel >> 24) & 0xFFu) >= 167u);
    DESTO_CHECK(((halfTextPixel >> 24) & 0xFFu) <= 169u);
    DESTO_CHECK(((halfTextPixel >> 16) & 0xFFu) < 64u);
    {
        BITMAPINFO bitmapInfo{};
        bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmapInfo.bmiHeader.biWidth = 160;
        bitmapInfo.bmiHeader.biHeight = -48;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        const auto bitmap = CreateDIBSection(
            nullptr, &bitmapInfo, DIB_RGB_COLORS, &pixels, nullptr, 0);
        const auto dc = CreateCompatibleDC(nullptr);
        DESTO_CHECK(bitmap != nullptr && dc != nullptr && pixels != nullptr);
        const auto previousBitmap = SelectObject(dc, bitmap);
        std::fill_n(static_cast<std::uint32_t*>(pixels), 160 * 48, 0x00FFFFFFu);
        const auto font = CreateFontW(
            -14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ResolveLayeredSurfaceTextQuality(), DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI Variable Text");
        DESTO_CHECK(font != nullptr);
        const auto previousFont = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(24, 24, 24));
        RECT bounds{4, 2, 156, 46};
        DrawTextW(dc, L"开源网络待办", -1, &bounds,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        std::uint32_t maximumChannelSpread = 0;
        std::size_t antialiasedPixels = 0;
        for (std::size_t index = 0; index < 160 * 48; ++index) {
            const auto pixel = static_cast<std::uint32_t*>(pixels)[index];
            const auto blue = pixel & 0xFFu;
            const auto green = (pixel >> 8) & 0xFFu;
            const auto red = (pixel >> 16) & 0xFFu;
            const auto maximum = (std::max)(red, (std::max)(green, blue));
            const auto minimum = (std::min)(red, (std::min)(green, blue));
            maximumChannelSpread = (std::max)(maximumChannelSpread, maximum - minimum);
            if (red > 24 && red < 255) ++antialiasedPixels;
        }
        // Font rasterization can vary slightly between Windows runner images;
        // require evidence of antialiasing without depending on a fixed count.
        DESTO_CHECK(antialiasedPixels > 0);
        DESTO_CHECK(maximumChannelSpread <= 1);
        SelectObject(dc, previousFont);
        DeleteObject(font);
        SelectObject(dc, previousBitmap);
        DeleteDC(dc);
        DeleteObject(bitmap);
    }
    DESTO_CHECK(ResolveFileCardDropEffect(
        CardType::Mapping, MappingMode::Folder, true, true, true,
        DROPEFFECT_MOVE) == DROPEFFECT_MOVE);
    DESTO_CHECK(ResolveFileCardDropEffect(
        CardType::Mapping, MappingMode::Folder, true, true, false,
        DROPEFFECT_MOVE) == DROPEFFECT_MOVE);
    DESTO_CHECK(ResolveFileCardDropEffect(
        CardType::Mapping, MappingMode::Folder, true, false, false,
        DROPEFFECT_MOVE) == DROPEFFECT_MOVE);
    DESTO_CHECK(ResolveFileCardDropEffect(
        CardType::Mapping, MappingMode::Folder, false, true, false,
        DROPEFFECT_COPY) == DROPEFFECT_COPY);
    DESTO_CHECK(ResolveFileCardDropEffect(
        CardType::Mapping, MappingMode::References, true, true, false,
        DROPEFFECT_COPY) == DROPEFFECT_COPY);
    DESTO_CHECK(ResolveFileCardDropEffect(
        CardType::Mapping, MappingMode::References, true, false, false,
        DROPEFFECT_MOVE, true) == DROPEFFECT_MOVE);
    DESTO_CHECK(ResolveFileCardDropEffect(
        CardType::Application, MappingMode::Empty, false, false, false,
        DROPEFFECT_MOVE) == DROPEFFECT_MOVE);
    const std::vector<DisplaySnapshot> displays{
        {
            .id = "display-test",
            .workAreaWidth = 1920,
            .workAreaHeight = 1040,
            .effectiveDpi = 96,
            .primary = true,
        },
        {
            .id = "display-high-dpi",
            .workAreaLeft = 1280,
            .workAreaWidth = 1280,
            .workAreaHeight = 720,
            .effectiveDpi = 144,
        },
    };
    {
    WindowsDesktopHost host(L"Desto Host Test");
    const std::vector<PlacementProjection> projections{{
        .placementId = "placement-test",
        .cardId = "card-test",
        .displayId = "display-test",
        .rect = {40, 48, 320, 220},
    }};
    const auto overlay = CreateWindowExW(
        0, L"STATIC", L"Desto Overlay Test", WS_OVERLAPPED | WS_VISIBLE,
        20, 20, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    DESTO_CHECK(overlay != nullptr);
    host.setOverlayWindow(overlay);
    const std::vector<CardView> cards{{
        .id = "card-test",
        .type = CardType::Application,
        .title = L"Application",
        .typeLabel = L"Application",
    }};
    PlacementRect changed{};
    DisplayId changedDisplayId;
    auto horizontalAnchor = PlacementHorizontalAnchor::Free;
    auto verticalAnchor = PlacementVerticalAnchor::Free;
    double referenceWorkAreaWidth = 0;
    double referenceWorkAreaHeight = 0;
    bool callbackCalled = false;
    bool expansionCallbackCalled = false;
    bool expanded = true;
    bool itemActivated = false;
    std::optional<std::wstring> contextMenuItem;
    host.setPlacementChangedCallback(
        [&](const PlacementId& placementId,
            const CardId& cardId,
            const DisplayId& displayId,
            const PlacementRect& rect,
            PlacementHorizontalAnchor horizontal,
            PlacementVerticalAnchor vertical,
            double referenceWidth,
            double referenceHeight) {
            DESTO_CHECK(placementId == "placement-test");
            DESTO_CHECK(cardId == "card-test");
            changedDisplayId = displayId;
            changed = rect;
            horizontalAnchor = horizontal;
            verticalAnchor = vertical;
            referenceWorkAreaWidth = referenceWidth;
            referenceWorkAreaHeight = referenceHeight;
            callbackCalled = true;
        });
    host.setCardExpandedChangedCallback([&](const CardId& cardId, bool value) {
        DESTO_CHECK(cardId == "card-test");
        expansionCallbackCalled = true;
        expanded = value;
    });
    host.setCardItemActivatedCallback(
        [&](const CardId& cardId, const CardItemView& item) {
            DESTO_CHECK(cardId == "card-test");
            DESTO_CHECK(item.displayName == L"Example.txt");
            itemActivated = true;
        });
    host.setCardItemContextMenuCallback(
        [&](const CardId& cardId, const CardItemView& item, int, int) {
            DESTO_CHECK(cardId == "card-test");
            contextMenuItem = item.displayName;
            return true;
        });
    host.present(projections, displays, cards);

    const auto window = FindWindowW(L"DestoDesktopHostSurface", L"Desto Host Test");
    DESTO_CHECK(window != nullptr);
    const auto styles = GetWindowLongPtrW(window, GWL_EXSTYLE);
    DESTO_CHECK((styles & WS_EX_LAYERED) != 0);
    DESTO_CHECK((styles & WS_EX_TOOLWINDOW) != 0);
    DESTO_CHECK((styles & WS_EX_NOACTIVATE) != 0);
    DESTO_CHECK((styles & WS_EX_ACCEPTFILES) == 0);
    DESTO_CHECK(RevokeDragDrop(window) == S_OK);
    DESTO_CHECK(host.cardsVisible());
    host.setCardsVisible(false);
    DESTO_CHECK(!host.cardsVisible());
    DESTO_CHECK(!IsWindowVisible(window));
    host.setCardsVisible(true);
    DESTO_CHECK(host.cardsVisible());
    DESTO_CHECK(IsWindowVisible(window));
    DESTO_CHECK(SetWindowPos(
        overlay, HWND_TOP, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));
    DESTO_CHECK(SetWindowPos(
        window, HWND_TOP, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));
    SendMessageW(window, WM_MOUSEACTIVATE, reinterpret_cast<WPARAM>(window),
        MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN));
    DESTO_CHECK(IsWindowAbove(overlay, window));

    RECT windowRect{};
    DESTO_CHECK(GetWindowRect(window, &windowRect));
    DESTO_CHECK(windowRect.right - windowRect.left == 244);
    DESTO_CHECK(windowRect.bottom - windowRect.top == 120);
    const auto captionHit = SendMessageW(
        window,
        WM_NCHITTEST,
        0,
        MAKELPARAM(windowRect.left + 50, windowRect.top + 20));
    DESTO_CHECK(captionHit == HTCAPTION);
    const auto cornerHit = SendMessageW(
        window,
        WM_NCHITTEST,
        0,
        MAKELPARAM(windowRect.left + 2, windowRect.top + 2));
    DESTO_CHECK(cornerHit == HTCAPTION);

    RECT movingRect{7, 9, 251, 136};
    DESTO_CHECK(SetWindowPos(
        window, HWND_TOP, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));
    // Settings stays above cards even when a card receives a native z-order
    // request between focus transitions.
    DESTO_CHECK(IsWindowAbove(overlay, window));
    SendMessageW(window, WM_MOVING, 0, reinterpret_cast<LPARAM>(&movingRect));
    DESTO_CHECK(IsWindowAbove(overlay, window));
    const auto guide = FindWindowW(L"DestoAlignmentGuide", nullptr);
    DESTO_CHECK(guide != nullptr);
    DESTO_CHECK(IsWindowVisible(guide));
    DESTO_CHECK((GetWindowLongPtrW(guide, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0);

    DESTO_CHECK(SetWindowPos(window, nullptr, 7, 9, 244, 120, SWP_NOACTIVATE | SWP_NOZORDER));
    SendMessageW(window, WM_EXITSIZEMOVE, 0, 0);
    DESTO_CHECK(!IsWindowVisible(guide));
    DESTO_CHECK(callbackCalled);
    DESTO_CHECK(changed.left == 8);
    DESTO_CHECK(changed.top == 8);
    DESTO_CHECK(changed.width == 244);
    DESTO_CHECK(changed.height == 120);
    DESTO_CHECK(horizontalAnchor == PlacementHorizontalAnchor::Left);
    DESTO_CHECK(verticalAnchor == PlacementVerticalAnchor::Top);
    DESTO_CHECK(referenceWorkAreaWidth == 1920);
    DESTO_CHECK(referenceWorkAreaHeight == 1040);
    DESTO_CHECK(changedDisplayId == "display-test");

    host.updateCardItems("card-test", {{
        .id = L"example",
        .displayName = L"Example.txt",
        .sourcePath = L"C:\\Example.exe",
        .state = CardItemState::IconUnavailable,
    }});
    callbackCalled = false;
    host.updateCardContentPreferences(
        "card-test",
        {.itemSize = CardItemSize::Large, .showItemNames = false});
    DESTO_CHECK(GetWindowRect(window, &windowRect));
    DESTO_CHECK(windowRect.right - windowRect.left == 244);
    DESTO_CHECK(windowRect.bottom - windowRect.top == 120);

    const auto tooltip = FindOwnedTooltip(window);
    DESTO_CHECK(tooltip != nullptr);
    wchar_t tooltipClass[64]{};
    DESTO_CHECK(GetClassNameW(
        tooltip, tooltipClass, static_cast<int>(std::size(tooltipClass))) > 0);
    DESTO_CHECK(std::wstring_view(tooltipClass) == L"DestoItemTooltip");
    SendMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(56, 82));
    DESTO_CHECK(!IsWindowVisible(tooltip));
    SendMessageW(window, WM_TIMER, 2, 0);
    DESTO_CHECK(IsWindowVisible(tooltip));
    wchar_t tooltipText[128]{};
    GetWindowTextW(tooltip, tooltipText, static_cast<int>(std::size(tooltipText)));
    DESTO_CHECK(std::wstring_view(tooltipText) == L"Example.txt");
    SendMessageW(window, WM_MOUSELEAVE, 0, 0);
    DESTO_CHECK(!IsWindowVisible(tooltip));
    SendMessageW(window, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(56, 82));
    DESTO_CHECK(itemActivated);
    SendMessageW(window, WM_RBUTTONUP, MK_RBUTTON, MAKELPARAM(56, 82));
    DESTO_CHECK(contextMenuItem == L"Example.txt");
    contextMenuItem.reset();
    SendMessageW(window, WM_RBUTTONUP, MK_RBUTTON, MAKELPARAM(220, 110));
    DESTO_CHECK(!contextMenuItem.has_value());

    host.updateCardContentPreferences(
        "card-test",
        {.itemSize = CardItemSize::Large, .showItemNames = true});
    host.updateCardContentPreferences(
        "card-test",
        {.itemSize = CardItemSize::Large, .showItemNames = false});

    std::vector<CardItemView> adaptiveItems;
    for (int index = 0; index < 5; ++index) {
        adaptiveItems.push_back({
            .id = std::to_wstring(index),
            .displayName = std::to_wstring(index),
            .sourcePath = L"C:\\Item" + std::to_wstring(index) + L".lnk",
            .state = CardItemState::IconUnavailable,
        });
    }
    host.updateCardItems("card-test", adaptiveItems);
    DESTO_CHECK(GetWindowRect(window, &windowRect));
    DESTO_CHECK(windowRect.right - windowRect.left == 244);
    host.updateCardItemsBatch({{
        "card-test",
        adaptiveItems,
        ApplicationItemSortMode::Custom,
        {
            {"Item0.lnk", 0, 0},
            {"Item1.lnk", 1, 0},
            {"Item2.lnk", 2, 0},
            {"Item3.lnk", 3, 0},
            {"Item4.lnk", 4, 0},
        },
    }});
    DESTO_CHECK(GetWindowRect(window, &windowRect));
    DESTO_CHECK(windowRect.right - windowRect.left == 299);
    host.updateCardItemsBatch({{
        "card-test",
        adaptiveItems,
        ApplicationItemSortMode::Name,
        {
            {"Item0.lnk", 0, 0},
            {"Item1.lnk", 1, 0},
            {"Item2.lnk", 2, 0},
            {"Item3.lnk", 3, 0},
            {"Item4.lnk", 4, 0},
        },
    }});
    DESTO_CHECK(GetWindowRect(window, &windowRect));
    DESTO_CHECK(windowRect.right - windowRect.left == 299);
    host.updateCardItemsBatch({{
        "card-test",
        adaptiveItems,
        ApplicationItemSortMode::Name,
        {},
    }});
    DESTO_CHECK(GetWindowRect(window, &windowRect));
    DESTO_CHECK(windowRect.right - windowRect.left == 244);

    host.updateCardContentPreferences(
        "card-test",
        {.itemSize = CardItemSize::Large,
         .showItemNames = false,
         .sizeMode = CardSizeMode::Adaptive,
         .widthSpan = 5});
    host.updateCardItemsBatch({{
        "card-test",
        adaptiveItems,
        ApplicationItemSortMode::Custom,
        {
            {"Item0.lnk", 0, 0},
            {"Item1.lnk", 1, 0},
            {"Item2.lnk", 2, 0},
            {"Item3.lnk", 3, 0},
        },
    }});
    DESTO_CHECK(GetWindowRect(window, &windowRect));
    DESTO_CHECK(windowRect.right - windowRect.left == 244);
    adaptiveItems.pop_back();
    host.updateCardItemsBatch({{
        "card-test",
        adaptiveItems,
        ApplicationItemSortMode::Custom,
        {
            {"Item0.lnk", 0, 0},
            {"Item1.lnk", 1, 0},
            {"Item2.lnk", 2, 0},
            {"Item3.lnk", 3, 0},
        },
    }});
    DESTO_CHECK(GetWindowRect(window, &windowRect));
    DESTO_CHECK(windowRect.right - windowRect.left == 244);

    int refreshCount = 0;
    host.setCardItemsRefreshCallback(
        [&](const CardId& cardId, CardItemSize itemSize) {
            DESTO_CHECK(cardId == "card-test");
            DESTO_CHECK(itemSize == CardItemSize::Small
                || itemSize == CardItemSize::Medium
                || itemSize == CardItemSize::Large
                || itemSize == CardItemSize::ExtraLarge);
            ++refreshCount;
            return adaptiveItems;
        });

    host.updateCardContentPreferences(
        "card-test",
        {
            .itemSize = CardItemSize::Medium,
            .showItemNames = false,
            .sizeMode = CardSizeMode::Fixed,
            .widthSpan = 3,
            .fixedColumns = 4,
            .fixedRows = 3,
        });
    DESTO_CHECK(GetWindowRect(window, &windowRect));
    // The original medium preset uses four 44-DIP item slots at three spans.
    DESTO_CHECK(windowRect.right - windowRect.left == 200);
    DESTO_CHECK(windowRect.bottom - windowRect.top == 196);
    DESTO_CHECK(refreshCount == 1);
    DESTO_CHECK(callbackCalled);
    DESTO_CHECK(changed.width == 200);
    DESTO_CHECK(changed.height == 196.0);

    host.updateCardContentPreferences(
        "card-test",
        {.itemSize = CardItemSize::Large, .showItemNames = false});
    DESTO_CHECK(GetWindowRect(window, &windowRect));
    DESTO_CHECK(windowRect.right - windowRect.left == 244);
    DESTO_CHECK(windowRect.bottom - windowRect.top == 120);
    DESTO_CHECK(refreshCount == 2);

    host.updateCardContentPreferences(
        "card-test",
        {.itemSize = CardItemSize::ExtraLarge,
         .showItemNames = false,
         .sizeMode = CardSizeMode::Adaptive,
         .widthSpan = 4});
    DESTO_CHECK(GetWindowRect(window, &windowRect));
    DESTO_CHECK(windowRect.right - windowRect.left == 320);
    host.updateCardContentPreferences(
        "card-test",
        {.itemSize = CardItemSize::Small,
         .showItemNames = false,
         .sizeMode = CardSizeMode::Adaptive,
         .widthSpan = 4});
    DESTO_CHECK(GetWindowRect(window, &windowRect));
    DESTO_CHECK(windowRect.right - windowRect.left == 246);
    host.updateCardContentPreferences(
        "card-test",
        {.itemSize = CardItemSize::Large,
         .showItemNames = false,
         .sizeMode = CardSizeMode::Adaptive,
         .widthSpan = 4});
    DESTO_CHECK(GetWindowRect(window, &windowRect));
    DESTO_CHECK(windowRect.right - windowRect.left == 244);

    SendMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(232, 24));
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(232, 24));
    DESTO_CHECK(!expansionCallbackCalled);
    DESTO_CHECK(expanded);
    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(232, 24));
    DESTO_CHECK(expansionCallbackCalled);
    DESTO_CHECK(!expanded);
    expansionCallbackCalled = false;
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(232, 24));
    SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(120, 24));
    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(120, 24));
    DESTO_CHECK(!expansionCallbackCalled);
    DESTO_CHECK(!expanded);
    DESTO_CHECK(SendMessageW(
        window,
        WM_NCHITTEST,
        0,
        MAKELPARAM(windowRect.left + 100, windowRect.top + 100)) == HTTRANSPARENT);
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(232, 24));
    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(232, 24));
    DESTO_CHECK(expansionCallbackCalled);
    DESTO_CHECK(expanded);

    SendMessageW(window, WM_ENTERSIZEMOVE, 0, 0);
    RECT suggestedDpiRect{2050, 120, 2416, 300};
    SendMessageW(
        window,
        WM_DPICHANGED,
        MAKELPARAM(144, 144),
        reinterpret_cast<LPARAM>(&suggestedDpiRect));
    DESTO_CHECK(GetWindowRect(window, &windowRect));
    DESTO_CHECK(windowRect.left == suggestedDpiRect.left);
    DESTO_CHECK(windowRect.top == suggestedDpiRect.top);
    DESTO_CHECK(windowRect.right == suggestedDpiRect.right);
    DESTO_CHECK(windowRect.bottom == suggestedDpiRect.bottom);
    SendMessageW(window, WM_EXITSIZEMOVE, 0, 0);
    DESTO_CHECK(changedDisplayId == "display-high-dpi");
    DESTO_CHECK(changed.width == 244);
    DESTO_CHECK(GetWindowRect(window, &windowRect));
    DESTO_CHECK(windowRect.right - windowRect.left == 366);

    const auto refreshesBeforeQueuedRequest = refreshCount;
    host.setCardItemsRefreshCallback(
        [&](const CardId& cardId, CardItemSize itemSize) {
            DESTO_CHECK(cardId == "card-test");
            DESTO_CHECK(itemSize == CardItemSize::Large);
            ++refreshCount;
            host.requestClose();
            return adaptiveItems;
        });
    std::thread refreshRequester([&] {
        host.requestCardItemsRefresh("card-test");
        host.requestCardItemsRefresh("card-test");
    });
    refreshRequester.join();
    host.run();
    DESTO_CHECK(refreshCount == refreshesBeforeQueuedRequest + 1);
    DestroyWindow(overlay);
    }

    {
        WindowsDesktopHost movingOverlayHost(L"Desto Moving Overlay Regression Test");
        const std::vector<PlacementProjection> overlayProjections{
            {.placementId = "overlay-regression-placement",
             .cardId = "overlay-regression-card",
             .displayId = "display-test",
             .rect = {900, 100, 320, 220}},
            {.placementId = "overlay-sibling-placement",
             .cardId = "overlay-sibling-card",
             .displayId = "display-test",
             .rect = {900, 260, 320, 220}},
        };
        const std::vector<CardView> overlayCards{
            {.id = "overlay-regression-card",
             .type = CardType::Application,
             .title = L"Overlay regression"},
            {.id = "overlay-sibling-card",
             .type = CardType::Application,
             .title = L"Overlay sibling"},
        };
        const auto overlay = CreateWindowExW(
            0, L"STATIC", L"Desto Moving Overlay", WS_OVERLAPPED | WS_VISIBLE,
            880, 80, 360, 280, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        DESTO_CHECK(overlay != nullptr);
        movingOverlayHost.setOverlayWindow(overlay);
        movingOverlayHost.present(overlayProjections, displays, overlayCards);
        const auto cards = FindHostWindows(L"Desto Moving Overlay Regression Test");
        DESTO_CHECK(cards.size() == 2);
        const auto card = cards[0];
        const auto sibling = cards[1];
        SendMessageW(card, WM_ENTERSIZEMOVE, 0, 0);
        RECT moving{920, 100, 1164, 227};
        SendMessageW(card, WM_MOVING, 0, reinterpret_cast<LPARAM>(&moving));
        DESTO_CHECK(SetWindowPos(
            card, HWND_TOP, moving.left, moving.top,
            moving.right - moving.left, moving.bottom - moving.top,
            SWP_NOACTIVATE));
        DESTO_CHECK(IsWindowAbove(overlay, card));
        DESTO_CHECK(IsWindowAbove(card, sibling));
        for (int iteration = 0; iteration < 32; ++iteration) {
            moving.left += iteration % 2 == 0 ? 1 : -1;
            moving.right += iteration % 2 == 0 ? 1 : -1;
            SendMessageW(card, WM_MOVING, 0, reinterpret_cast<LPARAM>(&moving));
            SetWindowPos(card, overlay, moving.left, moving.top,
                moving.right - moving.left, moving.bottom - moving.top,
                SWP_NOACTIVATE);
            DESTO_CHECK(IsWindowAbove(overlay, card));
            DESTO_CHECK(IsWindowAbove(card, sibling));
        }
        SendMessageW(card, WM_EXITSIZEMOVE, 0, 0);
        DESTO_CHECK(IsWindowAbove(overlay, card));
        DESTO_CHECK(IsWindowAbove(sibling, card));
        DestroyWindow(overlay);
    }

    {
        POINT dragPointer{};
        DESTO_CHECK(GetCursorPos(&dragPointer));
        const auto highLeft = dragPointer.x - 64;
        const auto highTop = dragPointer.y - 64;
        const std::vector<DisplaySnapshot> adjacentDisplays{
            {.id = "adjacent-low", .workAreaLeft = static_cast<double>(highLeft - 640),
             .workAreaTop = static_cast<double>(highTop),
             .workAreaWidth = 640, .workAreaHeight = 600,
             .effectiveDpi = 96, .primary = true},
            {.id = "adjacent-high", .workAreaLeft = highLeft / 1.5,
             .workAreaTop = highTop / 1.5,
             .workAreaWidth = 800.0 / 1.5,
             .workAreaHeight = 600.0 / 1.5, .effectiveDpi = 144},
        };
        const std::vector<PlacementProjection> adjacentProjections{{
            .placementId = "adjacent-placement",
            .cardId = "adjacent-card",
            .displayId = "adjacent-low",
            .rect = {396, 60, 244, 120},
        }};
        const std::vector<CardView> adjacentCards{{
            .id = "adjacent-card",
            .type = CardType::Application,
            .title = L"Adjacent DPI",
        }};
        WindowsDesktopHost adjacentHost(L"Desto Adjacent DPI Regression Test");
        adjacentHost.present(adjacentProjections, adjacentDisplays, adjacentCards);
        const auto card = FindWindowW(
            L"DestoDesktopHostSurface", L"Desto Adjacent DPI Regression Test");
        DESTO_CHECK(card != nullptr);
        SendMessageW(card, WM_ENTERSIZEMOVE, 0, 0);
        RECT moving{
            highLeft - 20, highTop + 40,
            highLeft + 224, highTop + 167};
        SendMessageW(card, WM_MOVING, 0, reinterpret_cast<LPARAM>(&moving));
        DESTO_CHECK(moving.right - moving.left == 244);
        DESTO_CHECK(moving.bottom - moving.top == 127);
        RECT dpiSuggested{
            highLeft + 24, highTop + 40,
            highLeft + 390, highTop + 220};
        SendMessageW(
            card,
            WM_DPICHANGED,
            MAKELPARAM(144, 144),
            reinterpret_cast<LPARAM>(&dpiSuggested));
        RECT changedRect{};
        DESTO_CHECK(GetWindowRect(card, &changedRect));
        DESTO_CHECK(changedRect.left == dpiSuggested.left);
        DESTO_CHECK(changedRect.top == dpiSuggested.top);
        DESTO_CHECK(changedRect.right == dpiSuggested.right);
        DESTO_CHECK(changedRect.bottom == dpiSuggested.bottom);
        SendMessageW(card, WM_EXITSIZEMOVE, 0, 0);
    }

    const std::vector<PlacementProjection> mappingProjections{1, {
        .placementId = "mapping-placement",
        .cardId = "mapping-card",
        .displayId = "display-test",
        .rect = {400, 48, 320, 220},
    }};
    const std::vector<CardView> mappingCards{1, {
        .id = "mapping-card",
        .type = CardType::Mapping,
        .title = L"Mapping",
        .typeLabel = L"Mapping",
        .mappingMode = MappingMode::References,
    }};
    {
        WindowsDesktopHost emptyMappingHost(L"Desto Empty Mapping Host Test");
        std::optional<MappingPresentationMode> mappingPresentationChanged;
        emptyMappingHost.setMappingPresentationChangedCallback(
            [&](const CardId& cardId, MappingPresentationMode mode) {
                DESTO_CHECK(cardId == "mapping-card");
                mappingPresentationChanged = mode;
                return true;
            });
        emptyMappingHost.present(mappingProjections, displays, mappingCards);
        const auto mappingWindow = FindWindowW(
            L"DestoDesktopHostSurface", L"Desto Empty Mapping Host Test");
        DESTO_CHECK(mappingWindow != nullptr);
        RECT mappingWindowRect{};
        DESTO_CHECK(GetWindowRect(mappingWindow, &mappingWindowRect));
        DESTO_CHECK(mappingWindowRect.bottom - mappingWindowRect.top == 120);
        const auto mappingWidth = mappingWindowRect.right - mappingWindowRect.left;
        const POINT mappingViewControlPoint{
            mappingWindowRect.left + mappingWidth - 62,
            mappingWindowRect.top + 24,
        };
        DESTO_CHECK(SendMessageW(
            mappingWindow,
            WM_NCHITTEST,
            0,
            MAKELPARAM(mappingViewControlPoint.x, mappingViewControlPoint.y)) == HTCLIENT);
        SendMessageW(mappingWindow, WM_LBUTTONDOWN, MK_LBUTTON,
            MAKELPARAM(mappingWidth - 62, 24));
        SendMessageW(mappingWindow, WM_LBUTTONUP, 0,
            MAKELPARAM(mappingWidth - 62, 24));
        DESTO_CHECK(mappingPresentationChanged == MappingPresentationMode::List);
        emptyMappingHost.updateMappingCard(
            "mapping-card", MappingMode::Folder, false, {{
                .id = L"mapped-item",
                .displayName = L"Mapped.txt",
                .sourcePath = L"C:\\Mapped.txt",
                .state = CardItemState::IconUnavailable,
            }, {
                .id = L"mapped-item-2",
                .displayName = L"Mapped 2.txt",
                .sourcePath = L"C:\\Mapped 2.txt",
                .state = CardItemState::IconUnavailable,
            }, {
                .id = L"mapped-item-3",
                .displayName = L"Mapped 3.txt",
                .sourcePath = L"C:\\Mapped 3.txt",
                .state = CardItemState::IconUnavailable,
            }});
        DESTO_CHECK(GetWindowRect(mappingWindow, &mappingWindowRect));
        DESTO_CHECK(mappingWindowRect.bottom - mappingWindowRect.top == 190);

        // A configured folder source may legitimately be empty. Its source
        // state must remain active so external drops are moves into that
        // directory, even though there are no items to render yet.
        emptyMappingHost.updateMappingCard(
            "mapping-card", MappingMode::Folder, true, {},
            ApplicationItemSortMode::Custom, {}, true);
        auto* emptySourceData = CreateFileDataObject(
            {L"C:\\Desto-empty-folder-source.txt"});
        DESTO_CHECK(emptySourceData != nullptr);
        auto* emptySourceTarget = RegisteredDropTarget(mappingWindow);
        DESTO_CHECK(emptySourceTarget != nullptr);
        DWORD emptySourceEffect = DROPEFFECT_MOVE | DROPEFFECT_COPY;
        DESTO_CHECK(emptySourceTarget->DragEnter(
            emptySourceData, MK_LBUTTON, {420, 120}, &emptySourceEffect) == S_OK);
        DESTO_CHECK(emptySourceEffect == DROPEFFECT_MOVE);
        DESTO_CHECK(emptySourceTarget->DragLeave() == S_OK);
        emptySourceData->Release();
        DESTO_CHECK(RevokeDragDrop(mappingWindow) == S_OK);
    }

    {
        const std::vector<PlacementProjection> scrollProjections{1, {
            .placementId = "scroll-placement",
            .cardId = "scroll-card",
            .displayId = "display-test",
            .rect = {760, 48, 180, 220},
        }};
        CardView scrollCard{
            .id = "scroll-card",
            .type = CardType::Application,
            .title = L"Scrollable",
            .content = {
                .itemSize = CardItemSize::Large,
                .showItemNames = false,
                .sizeMode = CardSizeMode::Fixed,
                .widthSpan = 2,
                .fixedColumns = 2,
                .fixedRows = 3,
                .maximumVisibleRows = 1,
            },
            .applicationSortMode = ApplicationItemSortMode::Name,
        };
        for (int index = 0; index < 6; ++index) {
            scrollCard.items.push_back({
                .id = L"item-" + std::to_wstring(index),
                .displayName = L"Item" + std::to_wstring(index),
                .sourcePath = L"C:\\Item" + std::to_wstring(index) + L".lnk",
                .state = CardItemState::IconUnavailable,
            });
        }
        WindowsDesktopHost scrollHost(L"Desto Scroll Host Test");
        std::wstring activated;
        scrollHost.setCardItemActivatedCallback(
            [&](const CardId&, const CardItemView& item) { activated = item.displayName; });
        const std::vector<CardView> scrollCards{scrollCard};
        scrollHost.present(scrollProjections, displays, scrollCards);
        const auto scrollWindow = FindWindowW(
            L"DestoDesktopHostSurface", L"Desto Scroll Host Test");
        DESTO_CHECK(scrollWindow != nullptr);
        RECT scrollRect{};
        DESTO_CHECK(GetWindowRect(scrollWindow, &scrollRect));
        DESTO_CHECK(scrollRect.bottom - scrollRect.top == 120);
        SendMessageW(scrollWindow, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(40, 76));
        DESTO_CHECK(activated == L"Item0");
        SendMessageW(scrollWindow, WM_MOUSEWHEEL,
            MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0);
        activated.clear();
        SendMessageW(scrollWindow, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(40, 76));
        DESTO_CHECK(activated == L"Item2");
    }

    auto immutableCards = mappingCards;
    immutableCards.front().mappingMode = MappingMode::Folder;
    immutableCards.front().mappingHasSource = true;
    immutableCards.front().mappingAllowsSourceMutation = false;
    {
        WindowsDesktopHost immutableMappingHost(L"Desto Immutable Mapping Host Test");
        immutableMappingHost.present(mappingProjections, displays, immutableCards);
        const auto immutableWindow = FindWindowW(
            L"DestoDesktopHostSurface", L"Desto Immutable Mapping Host Test");
        DESTO_CHECK(immutableWindow != nullptr);
        auto* immutableData = CreateFileDataObject({L"C:\\Desto-immutable-folder-source.txt"});
        DESTO_CHECK(immutableData != nullptr);
        auto* immutableTarget = RegisteredDropTarget(immutableWindow);
        DESTO_CHECK(immutableTarget != nullptr);
        DWORD immutableEffect = DROPEFFECT_MOVE | DROPEFFECT_COPY;
        DESTO_CHECK(immutableTarget->DragEnter(
            immutableData, MK_LBUTTON, {420, 120}, &immutableEffect) == S_OK);
        DESTO_CHECK(immutableEffect == DROPEFFECT_MOVE);
        DESTO_CHECK(immutableTarget->DragLeave() == S_OK);
        immutableData->Release();
        DESTO_CHECK(RevokeDragDrop(immutableWindow) == S_OK);
    }

    const std::vector<PlacementProjection> todoProjections{1, {
        .placementId = "todo-placement",
        .cardId = "todo-card",
        .displayId = "display-test",
        .rect = {760, 48, 320, 220},
    }};
    std::vector<CardView> todoCards{1, {
        .id = "todo-card",
        .type = CardType::Todo,
        .title = L"Todo",
        .typeLabel = L"Todo",
        .appearancePreset = "transparent-black",
        .todoItems = {
            {.id = "todo-first", .title = "First task", .completed = false},
            {.id = "todo-second", .title = "Second task", .completed = false},
        },
    }};
    {
        WindowsDesktopHost todoHost(L"Desto Todo Host Test");
        const auto todoOverlay = CreateWindowExW(
            0, L"STATIC", L"Desto Todo Overlay Test", WS_OVERLAPPED | WS_VISIBLE,
            20, 20, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        DESTO_CHECK(todoOverlay != nullptr);
        todoHost.setOverlayWindow(todoOverlay);
        bool completedChanged = false;
        bool archived = false;
        std::optional<TodoDate> addedDate;
        std::vector<std::string> reordered;
        todoHost.setTodoItemCompletedChangedCallback(
            [&](const CardId& cardId, const std::string& itemId, bool completed) {
                DESTO_CHECK(cardId == "todo-card");
                DESTO_CHECK(itemId == "todo-first");
                completedChanged = completed;
                return true;
            });
        todoHost.setTodoItemsReorderedCallback(
            [&](const CardId& cardId, const std::vector<std::string>& order) {
                DESTO_CHECK(cardId == "todo-card");
                reordered = order;
                return true;
            });
        todoHost.setTodoItemsArchivedCallback([&](const CardId& cardId) {
            DESTO_CHECK(cardId == "todo-card");
            archived = true;
            return true;
        });
        todoHost.setTodoItemAddedScheduledCallback(
            [&](const CardId& cardId, const std::string& title, TodoDate date)
                -> std::optional<TodoItem> {
                DESTO_CHECK(cardId == "todo-card");
                DESTO_CHECK(title == "Plan \xF0\x9F\x9A\x80");
                addedDate = date;
                return TodoItem{.id = "todo-scheduled", .title = title, .scheduledDate = date};
            });
        todoHost.present(todoProjections, displays, todoCards);
        const auto todoWindow = FindWindowW(L"DestoDesktopHostSurface", L"Desto Todo Host Test");
        DESTO_CHECK(todoWindow != nullptr);
        RECT todoRect{};
        DESTO_CHECK(GetWindowRect(todoWindow, &todoRect));
        DESTO_CHECK(todoRect.right - todoRect.left == 244);
        DESTO_CHECK(todoRect.bottom - todoRect.top == 216);
        todoHost.updateCardContentPreferences(
            "todo-card",
            {.itemSize = CardItemSize::Large, .widthSpan = 5});
        DESTO_CHECK(GetWindowRect(todoWindow, &todoRect));
        DESTO_CHECK(todoRect.right - todoRect.left == 299);
        todoHost.updateCardContentPreferences(
            "todo-card",
            {.itemSize = CardItemSize::Large, .widthSpan = 6});
        DESTO_CHECK(GetWindowRect(todoWindow, &todoRect));
        DESTO_CHECK(todoRect.right - todoRect.left == 354);
        todoHost.updateCardContentPreferences(
            "todo-card",
            {.itemSize = CardItemSize::Large, .widthSpan = 4});

        todoHost.resetRenderStatistics();
        for (int index = 0; index < 100; ++index) {
            SendMessageW(todoWindow, WM_MOUSEMOVE, 0,
                MAKELPARAM(100, index % 2 == 0 ? 149 : 191));
        }
        DESTO_CHECK(todoHost.renderStatistics().fullSurfaceRenders == 0);
        DESTO_CHECK(todoHost.renderStatistics().fullSurfaceCommits == 0);

        SendMessageW(todoWindow, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(26, 149));
        DESTO_CHECK(!completedChanged);
        SendMessageW(todoWindow, WM_LBUTTONUP, 0, MAKELPARAM(26, 149));
        DESTO_CHECK(completedChanged);

        SendMessageW(todoWindow, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(100, 149));
        SendMessageW(todoWindow, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(100, 191));
        SendMessageW(todoWindow, WM_LBUTTONUP, 0, MAKELPARAM(100, 191));
        DESTO_CHECK(reordered == std::vector<std::string>({"todo-second", "todo-first"}));

        SendMessageW(todoWindow, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(100, 149));
        SendMessageW(todoWindow, WM_LBUTTONUP, 0, MAKELPARAM(100, 149));
        const auto existingTodoEditor = FindTodoEditor(todoWindow);
        DESTO_CHECK(existingTodoEditor == nullptr);

        todoCards.front().todoItems.front().completed = true;
        todoCards.front().todoItems.push_back({"todo-third", "Third task", false});
        todoHost.updateTodoItems("todo-card", todoCards.front().todoItems);
        DESTO_CHECK(GetWindowRect(todoWindow, &todoRect));
        DESTO_CHECK(todoRect.bottom - todoRect.top == 258);

        SendMessageW(todoWindow, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(200, 60));
        DESTO_CHECK(GetWindowRect(todoWindow, &todoRect));
        DESTO_CHECK(todoRect.bottom - todoRect.top == 258);
        SendMessageW(todoWindow, WM_LBUTTONUP, 0, MAKELPARAM(200, 60));
        DESTO_CHECK(GetWindowRect(todoWindow, &todoRect));
        DESTO_CHECK(todoRect.bottom - todoRect.top == 258);
        SendMessageW(todoWindow, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(200, 60));
        SendMessageW(todoWindow, WM_LBUTTONUP, 0, MAKELPARAM(200, 60));
        DESTO_CHECK(GetWindowRect(todoWindow, &todoRect));
        DESTO_CHECK(todoRect.bottom - todoRect.top == 258);

        SendMessageW(todoWindow, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(30, 60));
        DESTO_CHECK(GetWindowRect(todoWindow, &todoRect));
        DESTO_CHECK(todoRect.bottom - todoRect.top == 258);
        SendMessageW(todoWindow, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(30, 60));
        SendMessageW(todoWindow, WM_TIMER, 5, 0);
        DESTO_CHECK(GetWindowRect(todoWindow, &todoRect));
        DESTO_CHECK(todoRect.bottom - todoRect.top >= 380);
        SendMessageW(todoWindow, WM_LBUTTONUP, 0, MAKELPARAM(30, 60));
        SendMessageW(todoWindow, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(240, 200));
        SendMessageW(todoWindow, WM_LBUTTONUP, 0, MAKELPARAM(240, 200));
        DESTO_CHECK(GetWindowRect(todoWindow, &todoRect));
        DESTO_CHECK(todoRect.bottom - todoRect.top == 258);

        SendMessageW(todoWindow, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(140, 60));
        DESTO_CHECK(!archived);
        SendMessageW(todoWindow, WM_LBUTTONUP, 0, MAKELPARAM(140, 60));
        DESTO_CHECK(archived);
        SendMessageW(todoWindow, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(30, 60));
        SendMessageW(todoWindow, WM_LBUTTONUP, 0, MAKELPARAM(30, 60));
        SendMessageW(todoWindow, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(40, 100));
        SendMessageW(todoWindow, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(40, 132));
        SendMessageW(todoWindow, WM_LBUTTONUP, 0, MAKELPARAM(40, 132));
        const auto ownedEditor = FindTodoEditor(todoWindow);
        DESTO_CHECK(ownedEditor == nullptr);
        SetActiveWindow(todoOverlay);
        SendMessageW(todoWindow, WM_MOUSEACTIVATE, reinterpret_cast<WPARAM>(todoWindow),
            MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN));
        SendMessageW(todoWindow, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(40, 100));
        SendMessageW(todoWindow, WM_LBUTTONUP, 0, MAKELPARAM(40, 100));
        const auto addEditor = FindTodoEditor(todoWindow);
        DESTO_CHECK(addEditor != nullptr);
        DESTO_CHECK(IsWindowsTextInput(addEditor));
        DESTO_CHECK((GetWindowLongPtrW(addEditor, GWL_STYLE) & WS_POPUP) != 0);
        DESTO_CHECK(GetParent(addEditor) == nullptr);
        DESTO_CHECK(FindWindowExW(addEditor, nullptr, L"EDIT", nullptr) == nullptr);
        RECT editorRect{};
        DESTO_CHECK(GetWindowRect(addEditor, &editorRect));
        RECT todoWindowRect{};
        DESTO_CHECK(GetWindowRect(todoWindow, &todoWindowRect));
        DESTO_CHECK(editorRect.right - editorRect.left
            == todoWindowRect.right - todoWindowRect.left - 20);
        DESTO_CHECK(editorRect.bottom - editorRect.top == 44);
        DESTO_CHECK(GetFocus() == addEditor);
        DESTO_CHECK(IsWindowAbove(todoOverlay, todoWindow));
        DESTO_CHECK(IsWindowAbove(todoOverlay, addEditor));
        SendMessageW(addEditor, WM_CHAR, L'A', 0);
        wchar_t editorText[8]{};
        GetWindowTextW(addEditor, editorText, static_cast<int>(std::size(editorText)));
        DESTO_CHECK(std::wstring_view(editorText) == L"A");
        SetWindowTextW(addEditor, L"");
        SendMessageW(addEditor, WM_CHAR, L'你', 0);
        SendMessageW(addEditor, WM_CHAR, L'好', 0);
        DWORD todoCaretStart = 0;
        DWORD todoCaretEnd = 0;
        SendMessageW(addEditor, EM_GETSEL,
            reinterpret_cast<WPARAM>(&todoCaretStart),
            reinterpret_cast<LPARAM>(&todoCaretEnd));
        DESTO_CHECK(todoCaretStart == 2 && todoCaretEnd == 2);
        SetWindowTextW(addEditor, L"Plan \U0001F680");
        DESTO_CHECK(WindowsTextInputText(addEditor) == L"Plan \U0001F680");
        SendMessageW(addEditor, WM_KEYDOWN, VK_RETURN, 0);
        DESTO_CHECK(addedDate == AddTodoDays(CurrentSystemTodoDate(), 1));
        SendMessageW(todoWindow, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(40, 100));
        SendMessageW(todoWindow, WM_LBUTTONUP, 0, MAKELPARAM(40, 100));
        const auto collapsingTodoEditor = FindTodoEditor(todoWindow);
        DESTO_CHECK(collapsingTodoEditor != nullptr);
        RECT todoClient{};
        DESTO_CHECK(GetClientRect(todoWindow, &todoClient));
        const auto collapseX = todoClient.right - 24;
        SendMessageW(todoWindow, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(collapseX, 24));
        SendMessageW(todoWindow, WM_LBUTTONUP, 0, MAKELPARAM(collapseX, 24));
        DESTO_CHECK(FindTodoEditor(todoWindow) == nullptr);
        DestroyWindow(todoOverlay);
    }

    {
        auto crystalCards = todoCards;
        crystalCards.front().appearancePreset = "transparent-white";
        WindowsDesktopHost crystalHost(L"Desto Crystal Todo Editor Test");
        const auto overlay = CreateWindowExW(
            0, L"STATIC", L"Desto Crystal Todo Overlay", WS_OVERLAPPED | WS_VISIBLE,
            20, 20, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        DESTO_CHECK(overlay != nullptr);
        crystalHost.setOverlayWindow(overlay);
        crystalHost.present(todoProjections, displays, crystalCards);
        const auto window = FindWindowW(
            L"DestoDesktopHostSurface", L"Desto Crystal Todo Editor Test");
        DESTO_CHECK(window != nullptr);
        SetActiveWindow(overlay);
        SendMessageW(window, WM_MOUSEACTIVATE, reinterpret_cast<WPARAM>(window),
            MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN));
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(40, 100));
        SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(40, 100));
        const auto editor = FindTodoEditor(window);
        DESTO_CHECK(editor != nullptr);
        DESTO_CHECK(IsWindowsTextInput(editor));
        DESTO_CHECK(GetParent(editor) == nullptr);
        DESTO_CHECK((GetWindowLongPtrW(editor, GWL_EXSTYLE) & WS_EX_LAYERED) != 0);
        DESTO_CHECK((GetWindowLongPtrW(editor, GWL_EXSTYLE) & WS_EX_TRANSPARENT) == 0);
        DESTO_CHECK(FindWindowExW(editor, nullptr, L"EDIT", nullptr) == nullptr);
        SendMessageW(editor, WM_KEYDOWN, VK_ESCAPE, 0);
        DestroyWindow(overlay);
    }

    {
        auto overdueCards = todoCards;
        overdueCards.front().todoItems = {{
            .id = "overdue-item",
            .title = "Overdue",
            .scheduledDate = AddTodoDays(CurrentSystemTodoDate(), -1),
        }};
        WindowsDesktopHost overdueHost(L"Desto Overdue Todo Host Test");
        overdueHost.present(todoProjections, displays, overdueCards);
        const auto window = FindWindowW(
            L"DestoDesktopHostSurface", L"Desto Overdue Todo Host Test");
        DESTO_CHECK(window != nullptr);
        RECT rect{};
        DESTO_CHECK(GetWindowRect(window, &rect));
        DESTO_CHECK(rect.bottom - rect.top >= 174);
        DESTO_CHECK(rect.bottom - rect.top < 200);
    }

    {
        auto scrollTodoCards = todoCards;
        scrollTodoCards.front().content.maximumVisibleRows = 2;
        scrollTodoCards.front().todoItems.clear();
        for (int index = 0; index < 5; ++index) {
            scrollTodoCards.front().todoItems.push_back({
                .id = "scroll-todo-" + std::to_string(index),
                .title = "Task " + std::to_string(index),
                .scheduledDate = CurrentSystemTodoDate(),
            });
        }
        WindowsDesktopHost scrollTodoHost(L"Desto Scroll Todo Host Test");
        std::string completedItem;
        scrollTodoHost.setTodoItemCompletedChangedCallback(
            [&](const CardId&, const std::string& itemId, bool) {
                completedItem = itemId;
                return true;
            });
        scrollTodoHost.present(todoProjections, displays, scrollTodoCards);
        const auto window = FindWindowW(
            L"DestoDesktopHostSurface", L"Desto Scroll Todo Host Test");
        DESTO_CHECK(window != nullptr);
        RECT limitedTodoClient{};
        DESTO_CHECK(GetClientRect(window, &limitedTodoClient));
        const auto hiddenCheckboxPixel = RenderedClientPixel(
            window, 28, limitedTodoClient.bottom - 2);
        const auto adjacentBottomPixel = RenderedClientPixel(
            window, 48, limitedTodoClient.bottom - 2);
        DESTO_CHECK(hiddenCheckboxPixel == adjacentBottomPixel);
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(28, 149));
        SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(28, 149));
        DESTO_CHECK(completedItem == "scroll-todo-0");
        SendMessageW(window, WM_MOUSEWHEEL,
            MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0);
        completedItem.clear();
        SendMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(28, 149));
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(28, 149));
        SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(28, 149));
        DESTO_CHECK(completedItem == "scroll-todo-1");
    }

    {
        const auto today = CurrentSystemTodoDate();
        auto datedCards = todoCards;
        datedCards.front().todoItems = {
            {.id = "today-item", .title = "Today", .scheduledDate = today},
            {.id = "tomorrow-item", .title = "Tomorrow",
             .scheduledDate = AddTodoDays(today, 1)},
        };
        WindowsDesktopHost datedHost(L"Desto Dated Todo Host Test");
        datedHost.present(todoProjections, displays, datedCards);
        const auto window = FindWindowW(
            L"DestoDesktopHostSurface", L"Desto Dated Todo Host Test");
        DESTO_CHECK(window != nullptr);
        RECT rect{};
        DESTO_CHECK(GetWindowRect(window, &rect));
        DESTO_CHECK(rect.bottom - rect.top == 174);
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(30, 60));
        SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(30, 60));
        DESTO_CHECK(GetWindowRect(window, &rect));
        DESTO_CHECK(rect.bottom - rect.top == 174);
    }

    {
        auto longTitleCards = todoCards;
        longTitleCards.front().todoItems = {{
            .id = "long-item",
            .title = "这是一条用于验证待办内容完整换行的长标题这是一条用于验证待办内容完整换行的长标题这是一条用于验证待办内容完整换行的长标题",
            .scheduledDate = CurrentSystemTodoDate(),
        }};
        WindowsDesktopHost longTitleHost(L"Desto Long Todo Host Test");
        longTitleHost.present(todoProjections, displays, longTitleCards);
        const auto window = FindWindowW(
            L"DestoDesktopHostSurface", L"Desto Long Todo Host Test");
        DESTO_CHECK(window != nullptr);
        RECT rect{};
        DESTO_CHECK(GetWindowRect(window, &rect));
        DESTO_CHECK(rect.bottom - rect.top >= 198);
    }

    {
        const std::vector<PlacementProjection> stackedProjections{
            {.placementId = "stack-top", .cardId = "stack-top-card",
             .displayId = "display-test", .rect = {80, 48, 320, 220}},
            {.placementId = "stack-bottom", .cardId = "stack-bottom-card",
             .displayId = "display-test", .rect = {80, 262, 320, 220}},
        };
        const std::vector<CardView> stackedCards{
            {.id = "stack-top-card", .type = CardType::Todo, .title = L"Top"},
            {.id = "stack-bottom-card", .type = CardType::Todo, .title = L"Bottom"},
         };
         WindowsDesktopHost stackedHost(L"Desto Stacked Host Test");
         int stackedItemRefreshes = 0;
         stackedHost.setCardItemsRefreshCallback(
             [&](const CardId&, CardItemSize) {
                 ++stackedItemRefreshes;
                 return std::vector<CardItemView>{};
             });
         stackedHost.present(stackedProjections, displays, stackedCards);
        const auto windows = FindHostWindows(L"Desto Stacked Host Test");
        DESTO_CHECK(windows.size() == 2);
        RECT topRect{};
        RECT bottomRect{};
        DESTO_CHECK(GetWindowRect(windows[0], &topRect));
         DESTO_CHECK(GetWindowRect(windows[1], &bottomRect));
         DESTO_CHECK(bottomRect.top - topRect.bottom == 8);
         stackedHost.resetRenderStatistics();
         SendMessageW(windows[0], WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(220, 24));
         SendMessageW(windows[0], WM_LBUTTONUP, 0, MAKELPARAM(220, 24));
         DESTO_CHECK(GetWindowRect(windows[1], &bottomRect));
         DESTO_CHECK(bottomRect.top == topRect.top + 48 + 8);
         DESTO_CHECK(stackedItemRefreshes == 0);
         DESTO_CHECK(stackedHost.renderStatistics().fullSurfaceCommits == 2);
     }

    {
        const std::vector<PlacementProjection> stackedProjections{
            {.placementId = "names-top", .cardId = "names-top-card",
             .displayId = "display-test", .rect = {80, 48, 320, 220}},
            {.placementId = "names-bottom", .cardId = "names-bottom-card",
             .displayId = "display-test", .rect = {80, 176, 320, 220}},
        };
        const std::vector<CardView> stackedCards{
            {.id = "names-top-card", .type = CardType::Application, .title = L"Top",
             .content = {.itemSize = CardItemSize::Large, .showItemNames = false},
             .items = {{.id = L"long-name", .displayName = L"A very long file name.txt",
                        .state = CardItemState::IconUnavailable}}},
            {.id = "names-bottom-card", .type = CardType::Application, .title = L"Bottom"},
        };
        WindowsDesktopHost namesHost(L"Desto Item Names Stack Test");
        namesHost.present(stackedProjections, displays, stackedCards);
        const auto windows = FindHostWindows(L"Desto Item Names Stack Test");
        DESTO_CHECK(windows.size() == 2);
        RECT topBefore{};
        RECT bottomBefore{};
        DESTO_CHECK(GetWindowRect(windows[0], &topBefore));
        DESTO_CHECK(GetWindowRect(windows[1], &bottomBefore));
        DESTO_CHECK(bottomBefore.top - topBefore.bottom == 8);
        namesHost.updateCardContentPreferences(
            "names-top-card",
            {.itemSize = CardItemSize::Large, .showItemNames = true});
        RECT topAfter{};
        RECT bottomAfter{};
        DESTO_CHECK(GetWindowRect(windows[0], &topAfter));
        DESTO_CHECK(GetWindowRect(windows[1], &bottomAfter));
        DESTO_CHECK(topAfter.bottom > topBefore.bottom);
        DESTO_CHECK(bottomAfter.top - topAfter.bottom == 8);
    }

    {
        const std::vector<PlacementProjection> stackedProjections{
            {.placementId = "move-top", .cardId = "move-top-card",
             .displayId = "display-test", .rect = {80, 48, 320, 220}},
            {.placementId = "move-bottom", .cardId = "move-bottom-card",
             .displayId = "display-test", .rect = {80, 276, 320, 220}},
        };
        const std::vector<CardView> stackedCards{
            {.id = "move-top-card", .type = CardType::Todo, .title = L"Top"},
            {.id = "move-bottom-card", .type = CardType::Todo, .title = L"Bottom"},
        };
        WindowsDesktopHost movingHost(L"Desto Moving Stack Test");
        movingHost.present(stackedProjections, displays, stackedCards);
        const auto windows = FindHostWindows(L"Desto Moving Stack Test");
        DESTO_CHECK(windows.size() == 2);
        RECT topRect{};
        RECT bottomBefore{};
        DESTO_CHECK(GetWindowRect(windows[0], &topRect));
        DESTO_CHECK(GetWindowRect(windows[1], &bottomBefore));
        DESTO_CHECK(SetWindowPos(
            windows[0], nullptr, topRect.left + 120, topRect.top + 96,
            topRect.right - topRect.left, topRect.bottom - topRect.top,
            SWP_NOACTIVATE | SWP_NOZORDER));
        SendMessageW(windows[0], WM_EXITSIZEMOVE, 0, 0);
        RECT bottomAfter{};
        DESTO_CHECK(GetWindowRect(windows[1], &bottomAfter));
        DESTO_CHECK(bottomAfter.left == bottomBefore.left);
        DESTO_CHECK(bottomAfter.top == bottomBefore.top);
    }

    {
        const std::vector<PlacementProjection> listProjections{{
            .placementId = "custom-list-placement",
            .cardId = "custom-list-card",
            .displayId = "display-test",
            .rect = {520, 48, 244, 190},
        }};
        CardView listCard{
            .id = "custom-list-card",
             .type = CardType::Application,
             .title = L"Custom List",
             .content = {.itemSize = CardItemSize::Large, .widthSpan = 4},
             .applicationSortMode = ApplicationItemSortMode::Custom,
             .applicationItemPlacements = {
                 {L"One.lnk", 2, 0},
                 {L"Two.lnk", 4, 0},
                 {L"Three.lnk", 1, 2},
             },
             .mappingPresentationMode = MappingPresentationMode::List,
             .items = {
                 {.id = L"one", .displayName = L"One", .sourcePath = L"C:\\One.lnk",
                  .state = CardItemState::IconUnavailable},
                {.id = L"two", .displayName = L"Two", .sourcePath = L"C:\\Two.lnk",
                 .state = CardItemState::IconUnavailable},
                 {.id = L"three", .displayName = L"Three", .sourcePath = L"C:\\Three.lnk",
                  .state = CardItemState::IconUnavailable},
             },
         };
        WindowsDesktopHost listHost(L"Desto Custom List Test");
        std::vector<std::wstring> activated;
        std::optional<std::wstring> contextMenuItem;
        listHost.setCardItemActivatedCallback(
            [&](const CardId&, const CardItemView& item) {
                activated.push_back(item.displayName);
            });
        listHost.setCardItemContextMenuCallback(
            [&](const CardId& cardId, const CardItemView& item, int, int) {
                DESTO_CHECK(cardId == "custom-list-card");
                contextMenuItem = item.displayName;
                return true;
            });
        listHost.present(listProjections, displays, std::span<const CardView>(&listCard, 1));
        const auto listWindow = FindWindowW(
            L"DestoDesktopHostSurface", L"Desto Custom List Test");
        DESTO_CHECK(listWindow != nullptr);
        for (int row = 0; row < 3; ++row) {
            SendMessageW(listWindow, WM_LBUTTONDBLCLK, MK_LBUTTON,
                MAKELPARAM(80, 76 + row * 42));
        }
         const std::vector<std::wstring> expectedActivated{L"One", L"Two", L"Three"};
         DESTO_CHECK(activated == expectedActivated);
        SendMessageW(listWindow, WM_RBUTTONUP, MK_RBUTTON, MAKELPARAM(80, 118));
        DESTO_CHECK(contextMenuItem == L"Two");
        DESTO_CHECK(RevokeDragDrop(listWindow) == S_OK);
    }

    {
        const std::vector<PlacementProjection> presentationStackProjections{
            {.placementId = "presentation-leader", .cardId = "presentation-leader-card",
             .displayId = "display-test", .rect = {80, 48, 244, 120}},
            {.placementId = "presentation-follower", .cardId = "presentation-follower-card",
             .displayId = "display-test", .rect = {80, 176, 244, 174}},
        };
        CardView presentationLeader{
             .id = "presentation-leader-card",
             .type = CardType::Mapping,
             .title = L"Leader",
             .applicationSortMode = ApplicationItemSortMode::Name,
             .items = {
                {.id = L"a", .displayName = L"A", .sourcePath = L"C:\\A.txt",
                 .state = CardItemState::IconUnavailable},
                {.id = L"b", .displayName = L"B", .sourcePath = L"C:\\B.txt",
                 .state = CardItemState::IconUnavailable},
                 {.id = L"c", .displayName = L"C", .sourcePath = L"C:\\C.txt",
                  .state = CardItemState::IconUnavailable},
             },
         };
        const std::vector<CardView> presentationStackCards{
            presentationLeader,
            {.id = "presentation-follower-card", .type = CardType::Todo,
             .title = L"Follower"},
        };
        WindowsDesktopHost presentationStackHost(L"Desto Presentation Stack Test");
        int itemRefreshes = 0;
        int placementChanges = 0;
        bool changedToList = false;
        presentationStackHost.setCardItemsRefreshCallback(
            [&](const CardId&, CardItemSize) {
                ++itemRefreshes;
                return std::vector<CardItemView>{};
            });
        presentationStackHost.setPlacementChangedCallback(
            [&](const PlacementId&, const CardId&, const DisplayId&, const PlacementRect&,
                PlacementHorizontalAnchor, PlacementVerticalAnchor, double, double) {
                ++placementChanges;
            });
        presentationStackHost.setMappingPresentationChangedCallback(
            [&](const CardId&, MappingPresentationMode mode) {
                changedToList = mode == MappingPresentationMode::List;
                return true;
            });
        presentationStackHost.present(
            presentationStackProjections, displays, presentationStackCards);
        const auto windows = FindHostWindows(L"Desto Presentation Stack Test");
        DESTO_CHECK(windows.size() == 2);
        presentationStackHost.resetRenderStatistics();
        SendMessageW(windows[0], WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(182, 24));
        SendMessageW(windows[0], WM_LBUTTONUP, 0, MAKELPARAM(182, 24));
        RECT leaderRect{};
        RECT followerRect{};
        DESTO_CHECK(GetWindowRect(windows[0], &leaderRect));
        DESTO_CHECK(GetWindowRect(windows[1], &followerRect));
        DESTO_CHECK(changedToList);
        DESTO_CHECK(leaderRect.bottom - leaderRect.top == 190);
        DESTO_CHECK(followerRect.top - leaderRect.bottom == 8);
        DESTO_CHECK(itemRefreshes == 0);
        DESTO_CHECK(placementChanges == 2);
        DESTO_CHECK(presentationStackHost.renderStatistics().fullSurfaceCommits == 2);
        DESTO_CHECK(RevokeDragDrop(windows[0]) == S_OK);
    }

    {
        const std::vector<DisplaySnapshot> constrainedDisplays{{
            .id = "display-constrained",
            .workAreaWidth = 1000,
            .workAreaHeight = 600,
            .effectiveDpi = 96,
            .primary = true,
        }};
        const std::vector<PlacementProjection> constrainedProjections{
            {.placementId = "constrained-leader", .cardId = "constrained-leader-card",
             .displayId = "display-constrained", .rect = {40, 20, 244, 120}},
            {.placementId = "constrained-middle", .cardId = "constrained-middle-card",
             .displayId = "display-constrained", .rect = {40, 148, 244, 174}},
            {.placementId = "constrained-bottom", .cardId = "constrained-bottom-card",
             .displayId = "display-constrained", .rect = {40, 330, 244, 174}},
        };
        CardView constrainedLeader{
            .id = "constrained-leader-card",
            .type = CardType::Application,
            .title = L"Constrained",
            .content = {.itemSize = CardItemSize::Large, .widthSpan = 4},
            .applicationSortMode = ApplicationItemSortMode::Name,
        };
        for (int index = 0; index < 40; ++index) {
            wchar_t name[16]{};
            swprintf_s(name, L"Item%02d", index);
            constrainedLeader.items.push_back({
                .id = name,
                .displayName = name,
                .sourcePath = std::filesystem::path(L"C:\\") / (std::wstring(name) + L".lnk"),
                .state = CardItemState::IconUnavailable,
            });
        }
        const std::vector<CardView> constrainedCards{
            constrainedLeader,
            {.id = "constrained-middle-card", .type = CardType::Todo, .title = L"Middle"},
            {.id = "constrained-bottom-card", .type = CardType::Todo, .title = L"Bottom"},
        };
        WindowsDesktopHost constrainedHost(L"Desto Constrained Stack Test");
        std::wstring activated;
        constrainedHost.setCardItemActivatedCallback(
            [&](const CardId&, const CardItemView& item) { activated = item.displayName; });
        constrainedHost.setMappingPresentationChangedCallback(
            [&](const CardId&, MappingPresentationMode) { return true; });
        constrainedHost.present(constrainedProjections, constrainedDisplays, constrainedCards);
        const auto windows = FindHostWindows(L"Desto Constrained Stack Test");
        DESTO_CHECK(windows.size() == 3);
        RECT gridLeader{};
        RECT middle{};
        RECT bottom{};
        DESTO_CHECK(GetWindowRect(windows[0], &gridLeader));
        DESTO_CHECK(GetWindowRect(windows[1], &middle));
        DESTO_CHECK(GetWindowRect(windows[2], &bottom));
        DESTO_CHECK(gridLeader.bottom - gridLeader.top == 174);
        DESTO_CHECK(middle.top - gridLeader.bottom == 8);
        DESTO_CHECK(bottom.top - middle.bottom == 8);
        DESTO_CHECK(bottom.bottom <= 600);

        SendMessageW(windows[0], WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(182, 24));
        SendMessageW(windows[0], WM_LBUTTONUP, 0, MAKELPARAM(182, 24));
        RECT listLeader{};
        DESTO_CHECK(GetWindowRect(windows[0], &listLeader));
        DESTO_CHECK(GetWindowRect(windows[1], &middle));
        DESTO_CHECK(GetWindowRect(windows[2], &bottom));
        DESTO_CHECK(listLeader.bottom - listLeader.top == 190);
        DESTO_CHECK(middle.top - listLeader.bottom == 8);
        DESTO_CHECK(bottom.top - middle.bottom == 8);
        DESTO_CHECK(bottom.bottom <= 600);

        SendMessageW(windows[0], WM_MOUSEWHEEL,
            MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0);
        SendMessageW(windows[0], WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(80, 76));
        DESTO_CHECK(activated == L"Item01");
        DESTO_CHECK(RevokeDragDrop(windows[0]) == S_OK);
    }

    {
        const std::vector<PlacementProjection> chromeProjections{{
            .placementId = "chrome-placement",
            .cardId = "chrome-card",
            .displayId = "display-test",
            .rect = {840, 48, 244, 120},
        }};
        const std::vector<CardView> chromeCards{{
            .id = "chrome-card",
            .type = CardType::Mapping,
            .title = L"Chrome",
            .showCollapseControl = false,
            .showPinControl = true,
        }};
        WindowsDesktopHost chromeHost(L"Desto Chrome Slot Test");
        bool pinChanged = false;
        bool mappingChanged = false;
        chromeHost.setCardPinChangedCallback([&](const CardId& cardId, bool pinned) {
            DESTO_CHECK(cardId == "chrome-card");
            pinChanged = pinned;
            return true;
        });
        chromeHost.setMappingPresentationChangedCallback(
            [&](const CardId& cardId, MappingPresentationMode mode) {
                DESTO_CHECK(cardId == "chrome-card");
                mappingChanged = mode == MappingPresentationMode::List;
                return true;
            });
        chromeHost.present(chromeProjections, displays, chromeCards);
        const auto chromeWindow = FindWindowW(
            L"DestoDesktopHostSurface", L"Desto Chrome Slot Test");
        DESTO_CHECK(chromeWindow != nullptr);
        SendMessageW(chromeWindow, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(220, 24));
        SendMessageW(chromeWindow, WM_LBUTTONUP, 0, MAKELPARAM(220, 24));
        DESTO_CHECK(pinChanged);
        SendMessageW(chromeWindow, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(182, 24));
        SendMessageW(chromeWindow, WM_LBUTTONUP, 0, MAKELPARAM(182, 24));
        DESTO_CHECK(mappingChanged);
        mappingChanged = false;
        chromeHost.updateCardChromePreferences(
            "chrome-card",
            {.showCollapseControl = false,
             .showPinControl = true,
             .showPresentationControl = false});
        RECT hiddenControlWindowRect{};
        DESTO_CHECK(GetWindowRect(chromeWindow, &hiddenControlWindowRect));
        DESTO_CHECK(SendMessageW(
            chromeWindow,
            WM_NCHITTEST,
            0,
            MAKELPARAM(
                hiddenControlWindowRect.left + 182,
                hiddenControlWindowRect.top + 24)) == HTCAPTION);
        chromeHost.updateCardChromePreferences(
            "chrome-card",
            {.showCollapseControl = false,
             .showPinControl = true,
             .showPresentationControl = false,
             .positionLocked = true});
        DESTO_CHECK(SendMessageW(
            chromeWindow,
            WM_NCHITTEST,
            0,
            MAKELPARAM(
                hiddenControlWindowRect.left + 80,
                hiddenControlWindowRect.top + 24)) == HTCLIENT);
        DESTO_CHECK(!mappingChanged);
    }

    {
        const std::vector<PlacementProjection> zOrderProjections{
            {.placementId = "pinned-placement", .cardId = "pinned-card",
             .displayId = "display-test", .rect = {112, 48, 320, 220}},
            {.placementId = "desktop-placement", .cardId = "desktop-card",
             .displayId = "display-test", .rect = {112, 300, 320, 220}},
        };
        const std::vector<CardView> zOrderCards{
            {.id = "pinned-card", .type = CardType::Application, .title = L"Pinned",
             .showPinControl = true, .pinOnTop = true,
             .items = {{.id = L"first", .displayName = L"First.txt"}}},
            {.id = "desktop-card", .type = CardType::Application, .title = L"Desktop"},
        };
        const auto overlay = CreateWindowExW(
            0, L"STATIC", L"Desto Z Order Overlay", WS_OVERLAPPED | WS_VISIBLE,
            40, 40, 360, 260, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        DESTO_CHECK(overlay != nullptr);
        WindowsDesktopHost zOrderHost(L"Desto Pin Persistence Test");
        zOrderHost.setOverlayWindow(overlay);
        zOrderHost.present(zOrderProjections, displays, zOrderCards);
        const auto windows = FindHostWindows(L"Desto Pin Persistence Test");
        DESTO_CHECK(windows.size() == 2);
        const auto pinned = windows[0];
        const auto desktop = windows[1];
        const auto isTopmost = [](HWND window) {
            return (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        };
        DESTO_CHECK(isTopmost(pinned));
        DESTO_CHECK(!isTopmost(desktop));
        DESTO_CHECK(IsWindowAbove(pinned, overlay));
        DESTO_CHECK(IsWindowAbove(overlay, desktop));

        zOrderHost.updateCardItems("pinned-card", {
            {.id = L"first", .displayName = L"First.txt"},
            {.id = L"second", .displayName = L"Second.txt"},
        });
        DESTO_CHECK(isTopmost(pinned));
        DESTO_CHECK(IsWindowAbove(pinned, overlay));

        RECT pinnedRect{};
        DESTO_CHECK(GetWindowRect(pinned, &pinnedRect));
        DESTO_CHECK(SetWindowPos(
            pinned, nullptr, pinnedRect.left + 24, pinnedRect.top + 16,
            pinnedRect.right - pinnedRect.left, pinnedRect.bottom - pinnedRect.top,
            SWP_NOACTIVATE | SWP_NOZORDER));
        SendMessageW(pinned, WM_EXITSIZEMOVE, 0, 0);
        DESTO_CHECK(isTopmost(pinned));
        DESTO_CHECK(IsWindowAbove(pinned, overlay));

        DESTO_CHECK(SetWindowPos(
            overlay, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));
        zOrderHost.setPinnedCardsYieldToFullscreen(false);
        DESTO_CHECK(isTopmost(pinned));
        DESTO_CHECK(IsWindowVisible(pinned));
        DESTO_CHECK(IsWindowAbove(pinned, overlay));
        DESTO_CHECK(SetWindowPos(
            overlay, HWND_NOTOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));

        zOrderHost.setCardsVisible(false);
        zOrderHost.setCardsVisible(true);
        DESTO_CHECK(isTopmost(pinned));
        DESTO_CHECK(!isTopmost(desktop));
        DESTO_CHECK(IsWindowAbove(pinned, overlay));
        DESTO_CHECK(IsWindowAbove(overlay, desktop));
        DestroyWindow(overlay);
    }

    {
        const std::vector<PlacementProjection> initialProjections{{
            .placementId = "incremental-a-placement",
            .cardId = "incremental-a",
            .displayId = "display-test",
            .rect = {40, 680, 244, 120},
        }};
        const std::vector<CardView> initialCards{{
            .id = "incremental-a",
            .type = CardType::Application,
            .title = L"A",
        }};
        WindowsDesktopHost incrementalHost(L"Desto Incremental Card Test");
        incrementalHost.present(initialProjections, displays, initialCards);
        const auto initialWindows = FindHostWindows(L"Desto Incremental Card Test");
        DESTO_CHECK(initialWindows.size() == 1);
        const auto preservedWindow = initialWindows.front();

        const std::vector<PlacementProjection> insertedProjections{{
            .placementId = "incremental-b-placement",
            .cardId = "incremental-b",
            .displayId = "display-test",
            .rect = {320, 680, 244, 120},
        }};
        const CardView insertedCard{
            .id = "incremental-b",
            .type = CardType::Todo,
            .title = L"B",
        };
        incrementalHost.insertCard(insertedProjections, insertedCard);
        const auto afterInsert = FindHostWindows(L"Desto Incremental Card Test");
        DESTO_CHECK(afterInsert.size() == 2);
        DESTO_CHECK(IsWindow(preservedWindow));
        DESTO_CHECK(std::ranges::find(afterInsert, preservedWindow) != afterInsert.end());

        incrementalHost.removeCard("incremental-b");
        const auto afterRemove = FindHostWindows(L"Desto Incremental Card Test");
        DESTO_CHECK(afterRemove.size() == 1);
        DESTO_CHECK(afterRemove.front() == preservedWindow);
    }

    {
        const PlacementRect original{100, 60, 244, 120};
        const std::vector<PlacementProjection> dropProjections{{
            .placementId = "drop-placement",
            .cardId = "drop-card",
            .displayId = "display-test",
            .rect = original,
            .horizontalAnchor = PlacementHorizontalAnchor::Right,
            .verticalAnchor = PlacementVerticalAnchor::Bottom,
        }};
        CardView dropCard{
            .id = "drop-card",
            .type = CardType::Application,
            .title = L"Drop placement",
            .content = {
                .itemSize = CardItemSize::Large,
                .showItemNames = false,
                .sizeMode = CardSizeMode::Adaptive,
            },
        };
        for (int index = 0; index < 4; ++index) {
            dropCard.items.push_back({
                .id = L"drop-item-" + std::to_wstring(index),
                .displayName = L"Item " + std::to_wstring(index),
            });
            dropCard.applicationItemPlacements.push_back({
                .fileName = L"Item" + std::to_wstring(index) + L".txt",
                .column = static_cast<std::uint32_t>(index),
                .row = 0,
            });
        }

        WindowsDesktopHost dropHost(L"Desto Drop Placement Test");
        bool placementChanged = false;
        FileDropOperation dropOperation = FileDropOperation::Copy;
        PlacementRect committed{};
        dropHost.setPlacementChangedCallback(
            [&](const PlacementId&, const CardId&, const DisplayId&,
                const PlacementRect& rect, PlacementHorizontalAnchor,
                PlacementVerticalAnchor, double, double) {
                placementChanged = true;
                committed = rect;
            });
        dropHost.setApplicationItemsDroppedCallback(
            [&](const CardId&, const std::vector<std::filesystem::path>&,
                const std::optional<CardId>&, FileDropOperation operation,
                std::size_t, std::size_t) {
                dropOperation = operation;
                auto expandedItems = dropCard.items;
                expandedItems.push_back({.id = L"dropped", .displayName = L"Dropped"});
                auto expandedPlacements = dropCard.applicationItemPlacements;
                expandedPlacements.push_back({
                    .fileName = L"Dropped.txt",
                    .column = 4,
                    .row = 0,
                });
                dropHost.updateCardItemsBatch({{
                    .cardId = "drop-card",
                    .items = std::move(expandedItems),
                    .sortMode = ApplicationItemSortMode::Custom,
                    .itemPlacements = std::move(expandedPlacements),
                }});
                return true;
            });
        dropHost.present(dropProjections, displays, std::span{&dropCard, std::size_t{1}});
        const auto window = FindWindowW(
            L"DestoDesktopHostSurface", L"Desto Drop Placement Test");
        DESTO_CHECK(window != nullptr);
        auto* target = RegisteredDropTarget(window);
        DESTO_CHECK(target != nullptr);
        auto* data = CreateFileDataObject({L"C:\\Desto-drop-placement-test.txt"});
        DESTO_CHECK(data != nullptr);
        RECT before{};
        DESTO_CHECK(GetWindowRect(window, &before));
        POINTL edge{before.right - 2, before.top + 80};
        DWORD effect = DROPEFFECT_MOVE;
        DESTO_CHECK(target->DragEnter(data, MK_LBUTTON, edge, &effect) == S_OK);
        std::this_thread::sleep_for(std::chrono::milliseconds(210));
        effect = DROPEFFECT_MOVE;
        DESTO_CHECK(target->DragOver(MK_LBUTTON, edge, &effect) == S_OK);
        RECT preview{};
        DESTO_CHECK(GetWindowRect(window, &preview));
        DESTO_CHECK(preview.left < before.left);
        DESTO_CHECK(preview.right == before.right);
        DESTO_CHECK(preview.top == before.top);
        effect = DROPEFFECT_MOVE;
        DESTO_CHECK(target->Drop(data, MK_LBUTTON, edge, &effect) == S_OK);
        data->Release();
        RECT after{};
        DESTO_CHECK(GetWindowRect(window, &after));
        DESTO_CHECK(after.left < before.left);
        DESTO_CHECK(after.right == before.right);
        DESTO_CHECK(after.top == before.top);
        DESTO_CHECK(after.right - after.left > before.right - before.left);
        DESTO_CHECK(placementChanged);
        DESTO_CHECK(dropOperation == FileDropOperation::Move);
        DESTO_CHECK(static_cast<LONG>(committed.left) == after.left);
        DESTO_CHECK(static_cast<LONG>(committed.top) == after.top);
        DESTO_CHECK(static_cast<LONG>(committed.width) == after.right - after.left);
        DESTO_CHECK(static_cast<LONG>(committed.height) == after.bottom - after.top);

        auto* copyData = CreateFileDataObject({L"C:\\Desto-drop-copy-test.txt"});
        DESTO_CHECK(copyData != nullptr);
        effect = DROPEFFECT_MOVE | DROPEFFECT_COPY;
        DESTO_CHECK(target->DragEnter(
            copyData, MK_LBUTTON | MK_CONTROL, edge, &effect) == S_OK);
        DESTO_CHECK(effect == DROPEFFECT_COPY);
        DESTO_CHECK(target->DragLeave() == S_OK);
        copyData->Release();
    }

    {
        const std::vector<PlacementProjection> cancelProjections{{
            .placementId = "drop-cancel-placement",
            .cardId = "drop-cancel-card",
            .displayId = "display-test",
            .rect = {480, 60, 244, 120},
            .horizontalAnchor = PlacementHorizontalAnchor::Center,
            .verticalAnchor = PlacementVerticalAnchor::Bottom,
        }};
        CardView cancelCard{
            .id = "drop-cancel-card",
            .type = CardType::Application,
            .title = L"Drop cancel",
            .content = {
                .itemSize = CardItemSize::Large,
                .showItemNames = false,
                .sizeMode = CardSizeMode::Adaptive,
            },
        };
        for (int index = 0; index < 4; ++index) {
            cancelCard.items.push_back({
                .id = L"cancel-item-" + std::to_wstring(index),
                .displayName = L"Item " + std::to_wstring(index),
            });
        }
        WindowsDesktopHost cancelHost(L"Desto Drop Cancel Placement Test");
        cancelHost.present(
            cancelProjections, displays, std::span{&cancelCard, std::size_t{1}});
        const auto window = FindWindowW(
            L"DestoDesktopHostSurface", L"Desto Drop Cancel Placement Test");
        DESTO_CHECK(window != nullptr);
        auto* target = RegisteredDropTarget(window);
        DESTO_CHECK(target != nullptr);
        auto* data = CreateFileDataObject({L"C:\\Desto-drop-cancel-test.txt"});
        DESTO_CHECK(data != nullptr);
        RECT before{};
        DESTO_CHECK(GetWindowRect(window, &before));
        POINTL edge{before.right - 2, before.top + 80};
        DWORD effect = DROPEFFECT_MOVE;
        DESTO_CHECK(target->DragEnter(data, MK_LBUTTON, edge, &effect) == S_OK);
        std::this_thread::sleep_for(std::chrono::milliseconds(210));
        effect = DROPEFFECT_MOVE;
        DESTO_CHECK(target->DragOver(MK_LBUTTON, edge, &effect) == S_OK);
        RECT preview{};
        DESTO_CHECK(GetWindowRect(window, &preview));
        DESTO_CHECK(preview.left == before.left);
        DESTO_CHECK(preview.right > before.right);
        DESTO_CHECK(preview.top == before.top);
        DESTO_CHECK(target->DragLeave() == S_OK);
        DESTO_CHECK(WaitForWindowRect(
            window, before, std::chrono::milliseconds(500)));
        data->Release();
        RECT after{};
        DESTO_CHECK(GetWindowRect(window, &after));
        DESTO_CHECK(after.left == before.left);
        DESTO_CHECK(after.top == before.top);
        DESTO_CHECK(after.right == before.right);
        DESTO_CHECK(after.bottom == before.bottom);
    }

    {
        const std::vector<PlacementProjection> edgeProjections{{
            .placementId = "drop-edge-placement",
            .cardId = "drop-edge-card",
            .displayId = "display-test",
            .rect = {100, 260, 244, 120},
            .horizontalAnchor = PlacementHorizontalAnchor::Left,
            .verticalAnchor = PlacementVerticalAnchor::Top,
        }};
        CardView edgeCard{
            .id = "drop-edge-card",
            .type = CardType::Application,
            .title = L"Drop edge progression",
            .content = {
                .itemSize = CardItemSize::Large,
                .showItemNames = false,
                .sizeMode = CardSizeMode::Adaptive,
            },
        };
        for (int index = 0; index < 4; ++index) {
            edgeCard.items.push_back({
                .id = L"edge-item-" + std::to_wstring(index),
                .displayName = L"Item " + std::to_wstring(index),
            });
        }
        WindowsDesktopHost edgeHost(L"Desto Drop Edge Progression Test");
        edgeHost.present(edgeProjections, displays, std::span{&edgeCard, std::size_t{1}});
        const auto window = FindWindowW(
            L"DestoDesktopHostSurface", L"Desto Drop Edge Progression Test");
        DESTO_CHECK(window != nullptr);
        auto* target = RegisteredDropTarget(window);
        DESTO_CHECK(target != nullptr);
        auto* data = CreateFileDataObject({L"C:\\Desto-drop-edge-test.txt"});
        DESTO_CHECK(data != nullptr);
        RECT initial{};
        DESTO_CHECK(GetWindowRect(window, &initial));
        POINTL firstEdge{initial.right - 2, initial.top + 80};
        DWORD effect = DROPEFFECT_MOVE;
        DESTO_CHECK(target->DragEnter(data, MK_LBUTTON, firstEdge, &effect) == S_OK);
        std::this_thread::sleep_for(std::chrono::milliseconds(210));
        effect = DROPEFFECT_MOVE;
        DESTO_CHECK(target->DragOver(MK_LBUTTON, firstEdge, &effect) == S_OK);
        RECT firstExpansion{};
        DESTO_CHECK(GetWindowRect(window, &firstExpansion));
        DESTO_CHECK(firstExpansion.left == initial.left);
        DESTO_CHECK(firstExpansion.right > initial.right);

        POINTL secondEdge{firstExpansion.right - 2, firstExpansion.top + 80};
        effect = DROPEFFECT_MOVE;
        DESTO_CHECK(target->DragOver(MK_LBUTTON, secondEdge, &effect) == S_OK);
        std::this_thread::sleep_for(std::chrono::milliseconds(210));
        effect = DROPEFFECT_MOVE;
        DESTO_CHECK(target->DragOver(MK_LBUTTON, secondEdge, &effect) == S_OK);
        RECT secondExpansion{};
        DESTO_CHECK(GetWindowRect(window, &secondExpansion));
        DESTO_CHECK(secondExpansion.left == initial.left);
        DESTO_CHECK(secondExpansion.right > firstExpansion.right);

        std::this_thread::sleep_for(std::chrono::milliseconds(210));
        effect = DROPEFFECT_MOVE;
        DESTO_CHECK(target->DragOver(MK_LBUTTON, secondEdge, &effect) == S_OK);
        RECT stationary{};
        DESTO_CHECK(GetWindowRect(window, &stationary));
        DESTO_CHECK(stationary.left == secondExpansion.left);
        DESTO_CHECK(stationary.right == secondExpansion.right);
        std::this_thread::sleep_for(std::chrono::milliseconds(210));
        effect = DROPEFFECT_MOVE;
        DESTO_CHECK(target->DragOver(MK_LBUTTON, secondEdge, &effect) == S_OK);
        DESTO_CHECK(GetWindowRect(window, &stationary));
        DESTO_CHECK(stationary.left == secondExpansion.left);
        DESTO_CHECK(stationary.right == secondExpansion.right);
        DESTO_CHECK(target->DragLeave() == S_OK);
        DESTO_CHECK(WaitForWindowRect(
            window, initial, std::chrono::milliseconds(500)));
        data->Release();
    }

    {
        const std::vector<PlacementProjection> shrinkProjections{{
            .placementId = "drop-shrink-placement",
            .cardId = "drop-shrink-card",
            .displayId = "display-test",
            .rect = {800, 260, 244, 120},
            .horizontalAnchor = PlacementHorizontalAnchor::Right,
            .verticalAnchor = PlacementVerticalAnchor::Top,
        }};
        CardView shrinkCard{
            .id = "drop-shrink-card",
            .type = CardType::Application,
            .title = L"Drop right shrink",
            .content = {
                .itemSize = CardItemSize::Large,
                .showItemNames = false,
                .sizeMode = CardSizeMode::Adaptive,
            },
        };
        for (int index = 0; index < 4; ++index) {
            shrinkCard.items.push_back({
                .id = L"shrink-item-" + std::to_wstring(index),
                .displayName = L"Item " + std::to_wstring(index),
            });
        }
        WindowsDesktopHost shrinkHost(L"Desto Drop Right Shrink Test");
        shrinkHost.present(
            shrinkProjections, displays, std::span{&shrinkCard, std::size_t{1}});
        const auto window = FindWindowW(
            L"DestoDesktopHostSurface", L"Desto Drop Right Shrink Test");
        DESTO_CHECK(window != nullptr);
        auto* target = RegisteredDropTarget(window);
        DESTO_CHECK(target != nullptr);
        auto* data = CreateFileDataObject({L"C:\\Desto-drop-shrink-test.txt"});
        DESTO_CHECK(data != nullptr);
        RECT initial{};
        DESTO_CHECK(GetWindowRect(window, &initial));
        POINTL edge{initial.right - 2, initial.top + 80};
        DWORD effect = DROPEFFECT_MOVE;
        DESTO_CHECK(target->DragEnter(data, MK_LBUTTON, edge, &effect) == S_OK);
        std::this_thread::sleep_for(std::chrono::milliseconds(210));
        effect = DROPEFFECT_MOVE;
        DESTO_CHECK(target->DragOver(MK_LBUTTON, edge, &effect) == S_OK);
        RECT expanded{};
        DESTO_CHECK(GetWindowRect(window, &expanded));
        DESTO_CHECK(expanded.left < initial.left);
        DESTO_CHECK(expanded.right == initial.right);

        POINTL inside{expanded.left + 190, expanded.top + 80};
        effect = DROPEFFECT_MOVE;
        DESTO_CHECK(target->DragOver(MK_LBUTTON, inside, &effect) == S_OK);
        RECT pendingShrink{};
        DESTO_CHECK(GetWindowRect(window, &pendingShrink));
        DESTO_CHECK(pendingShrink.left == expanded.left);
        DESTO_CHECK(pendingShrink.right == initial.right);

        POINTL recaptured{expanded.right - 40, expanded.top + 80};
        effect = DROPEFFECT_MOVE;
        DESTO_CHECK(target->DragOver(MK_LBUTTON, recaptured, &effect) == S_OK);
        std::this_thread::sleep_for(std::chrono::milliseconds(210));
        effect = DROPEFFECT_MOVE;
        DESTO_CHECK(target->DragOver(MK_LBUTTON, recaptured, &effect) == S_OK);
        RECT recapturedRect{};
        DESTO_CHECK(GetWindowRect(window, &recapturedRect));
        DESTO_CHECK(recapturedRect.left == expanded.left);
        DESTO_CHECK(recapturedRect.right == initial.right);

        effect = DROPEFFECT_MOVE;
        DESTO_CHECK(target->DragOver(MK_LBUTTON, inside, &effect) == S_OK);
        std::this_thread::sleep_for(std::chrono::milliseconds(210));
        effect = DROPEFFECT_MOVE;
        DESTO_CHECK(target->DragOver(MK_LBUTTON, inside, &effect) == S_OK);
        RECT shrunk{};
        DESTO_CHECK(GetWindowRect(window, &shrunk));
        DESTO_CHECK(shrunk.left == initial.left);
        DESTO_CHECK(shrunk.right == initial.right);
        DESTO_CHECK(target->DragLeave() == S_OK);
        DESTO_CHECK(WaitForWindowRect(
            window, initial, std::chrono::milliseconds(500)));
        data->Release();
    }

    {
        // Shell lifecycle messages must repair a stale desktop owner and keep
        // cards hidden while the session or display shell is unavailable.
        const std::vector<PlacementProjection> lifecycleProjections{{
            .placementId = "lifecycle-placement",
            .cardId = "lifecycle-card",
            .displayId = "display-test",
            .rect = {520, 48, 320, 220},
        }};
        const std::vector<CardView> lifecycleCards{{
            .id = "lifecycle-card",
            .type = CardType::Application,
            .title = L"Lifecycle",
        }};
        WindowsDesktopHost lifecycleHost(L"Desto Shell Lifecycle Test");
        lifecycleHost.present(lifecycleProjections, displays, lifecycleCards);
        const auto card = FindWindowW(
            L"DestoDesktopHostSurface", L"Desto Shell Lifecycle Test");
        const auto lifecycle = FindWindowW(L"DestoShellLifecycleHost", nullptr);
        DESTO_CHECK(card != nullptr);
        DESTO_CHECK(lifecycle != nullptr);
        DESTO_CHECK(IsWindowVisible(card));

        const auto fakeOwner = CreateWindowExW(
            0, L"STATIC", L"Desto Fake Desktop Owner", WS_POPUP,
            0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        DESTO_CHECK(fakeOwner != nullptr);
        SetWindowLongPtrW(card, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(fakeOwner));
        DESTO_CHECK(GetWindow(card, GW_OWNER) == fakeOwner);
        const auto taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        DESTO_CHECK(taskbarCreated != 0);
        SendMessageW(lifecycle, taskbarCreated, 0, 0);
        const auto desktopOwner = FindWindowExW(
            FindWindowW(L"Progman", nullptr), nullptr, L"SHELLDLL_DefView", nullptr);
        if (desktopOwner != nullptr) {
            DESTO_CHECK(GetWindow(card, GW_OWNER) == GetAncestor(desktopOwner, GA_ROOT));
        }

        SendMessageW(lifecycle, WM_WTSSESSION_CHANGE, WTS_SESSION_LOCK, 0);
        DESTO_CHECK(!IsWindowVisible(card));
        SendMessageW(lifecycle, WM_WTSSESSION_CHANGE, WTS_SESSION_UNLOCK, 0);
        DESTO_CHECK(IsWindowVisible(card));
        SendMessageW(lifecycle, WM_POWERBROADCAST, PBT_APMSUSPEND, 0);
        DESTO_CHECK(!IsWindowVisible(card));
        SendMessageW(lifecycle, WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0);
        DESTO_CHECK(IsWindowVisible(card));
        DestroyWindow(fakeOwner);
    }
}

} // namespace

int main() {
    return desto::test::Run(RunTests);
}
