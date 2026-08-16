#include "TestSupport.h"
#include "WindowsDesktopHost.h"

#include <Windows.h>
#include <CommCtrl.h>

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
            && _wcsicmp(className, TOOLTIPS_CLASSW) == 0) {
            value.result = candidate;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.result;
}

void RunTests() {
    WindowsDesktopHost host(L"Desto Host Test");
    const std::vector<DisplaySnapshot> displays{{
        .id = "display-test",
        .workAreaWidth = 1920,
        .workAreaHeight = 1040,
        .effectiveDpi = 96,
        .primary = true,
    }};
    const std::vector<PlacementProjection> projections{{
        .placementId = "placement-test",
        .cardId = "card-test",
        .displayId = "display-test",
        .rect = {40, 48, 320, 220},
    }};
    const std::vector<CardView> cards{{
        .id = "card-test",
        .type = CardType::Application,
        .title = L"Application",
        .typeLabel = L"Application",
    }};
    PlacementRect changed{};
    auto horizontalAnchor = PlacementHorizontalAnchor::Free;
    auto verticalAnchor = PlacementVerticalAnchor::Free;
    double referenceWorkAreaWidth = 0;
    double referenceWorkAreaHeight = 0;
    bool callbackCalled = false;
    bool expansionCallbackCalled = false;
    bool expanded = true;
    bool itemActivated = false;
    host.setPlacementChangedCallback(
        [&](const PlacementId& placementId,
            const CardId& cardId,
            const PlacementRect& rect,
            PlacementHorizontalAnchor horizontal,
            PlacementVerticalAnchor vertical,
            double referenceWidth,
            double referenceHeight) {
            DESTO_CHECK(placementId == "placement-test");
            DESTO_CHECK(cardId == "card-test");
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
            DESTO_CHECK(item.displayName == L"Example");
            itemActivated = true;
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

    RECT windowRect{};
    DESTO_CHECK(GetWindowRect(window, &windowRect));
    DESTO_CHECK(windowRect.right - windowRect.left == 256);
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

    RECT movingRect{7, 9, 263, 129};
    SendMessageW(window, WM_MOVING, 0, reinterpret_cast<LPARAM>(&movingRect));
    const auto guide = FindWindowW(L"DestoAlignmentGuide", nullptr);
    DESTO_CHECK(guide != nullptr);
    DESTO_CHECK(IsWindowVisible(guide));
    DESTO_CHECK((GetWindowLongPtrW(guide, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0);

    DESTO_CHECK(SetWindowPos(window, nullptr, 7, 9, 256, 120, SWP_NOACTIVATE | SWP_NOZORDER));
    SendMessageW(window, WM_EXITSIZEMOVE, 0, 0);
    DESTO_CHECK(!IsWindowVisible(guide));
    DESTO_CHECK(callbackCalled);
    DESTO_CHECK(changed.left == 8);
    DESTO_CHECK(changed.top == 8);
    DESTO_CHECK(changed.width == 256);
    DESTO_CHECK(changed.height == 120);
    DESTO_CHECK(horizontalAnchor == PlacementHorizontalAnchor::Left);
    DESTO_CHECK(verticalAnchor == PlacementVerticalAnchor::Top);
    DESTO_CHECK(referenceWorkAreaWidth == 1920);
    DESTO_CHECK(referenceWorkAreaHeight == 1040);

    host.updateCardItems("card-test", {{
        .id = L"example",
        .displayName = L"Example",
        .sourcePath = L"C:\\Example.exe",
        .state = CardItemState::IconUnavailable,
    }});
    host.updateCardContentPreferences(
        "card-test",
        {.itemSize = CardItemSize::Large, .showItemNames = false});
    DESTO_CHECK(GetWindowRect(window, &windowRect));
    DESTO_CHECK(windowRect.right - windowRect.left == 256);
    DESTO_CHECK(windowRect.bottom - windowRect.top == 127);
    const auto tooltip = FindOwnedTooltip(window);
    DESTO_CHECK(tooltip != nullptr);
    SendMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(56, 82));
    DESTO_CHECK(!IsWindowVisible(tooltip));
    DESTO_CHECK(KillTimer(window, 2));
    SendMessageW(window, WM_MOUSELEAVE, 0, 0);
    DESTO_CHECK(!IsWindowVisible(tooltip));
    SendMessageW(window, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(56, 82));
    DESTO_CHECK(itemActivated);

    host.updateCardContentPreferences(
        "card-test",
        {
            .itemSize = CardItemSize::Medium,
            .showItemNames = false,
            .sizeMode = CardSizeMode::Fixed,
            .fixedColumns = 2,
            .fixedRows = 3,
        });
    DESTO_CHECK(GetWindowRect(window, &windowRect));
    DESTO_CHECK(windowRect.right - windowRect.left == 180);
    DESTO_CHECK(windowRect.bottom - windowRect.top == 212);

    host.updateCardContentPreferences(
        "card-test",
        {.itemSize = CardItemSize::Large, .showItemNames = false});
    DESTO_CHECK(GetWindowRect(window, &windowRect));
    DESTO_CHECK(windowRect.right - windowRect.left == 256);
    DESTO_CHECK(windowRect.bottom - windowRect.top == 127);

    SendMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(232, 24));
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(232, 24));
    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(232, 24));
    DESTO_CHECK(expansionCallbackCalled);
    DESTO_CHECK(!expanded);
    DESTO_CHECK(SendMessageW(
        window,
        WM_NCHITTEST,
        0,
        MAKELPARAM(windowRect.left + 100, windowRect.top + 100)) == HTTRANSPARENT);
}

} // namespace

int main() {
    return desto::test::Run(RunTests);
}
