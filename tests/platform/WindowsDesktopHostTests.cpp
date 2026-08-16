#include "TestSupport.h"
#include "WindowsDesktopHost.h"

#include <Windows.h>

#include <vector>

using namespace desto::domain;
using namespace desto::platform::windows;
using namespace desto::presentation;

namespace {

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
    bool callbackCalled = false;
    host.setPlacementChangedCallback(
        [&](const PlacementId& placementId, const CardId& cardId, const PlacementRect& rect) {
            DESTO_CHECK(placementId == "placement-test");
            DESTO_CHECK(cardId == "card-test");
            changed = rect;
            callbackCalled = true;
        });
    host.present(projections, displays, cards);

    const auto window = FindWindowW(L"DestoDesktopHostSurface", L"Desto Host Test");
    DESTO_CHECK(window != nullptr);
    const auto styles = GetWindowLongPtrW(window, GWL_EXSTYLE);
    DESTO_CHECK((styles & WS_EX_LAYERED) != 0);
    DESTO_CHECK((styles & WS_EX_TOOLWINDOW) != 0);
    DESTO_CHECK((styles & WS_EX_NOACTIVATE) != 0);

    RECT windowRect{};
    DESTO_CHECK(GetWindowRect(window, &windowRect));
    const auto captionHit = SendMessageW(
        window,
        WM_NCHITTEST,
        0,
        MAKELPARAM(windowRect.left + 50, windowRect.top + 20));
    DESTO_CHECK(captionHit == HTCAPTION);

    MINMAXINFO minimums{};
    SendMessageW(window, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&minimums));
    DESTO_CHECK(minimums.ptMinTrackSize.x == 160);
    DESTO_CHECK(minimums.ptMinTrackSize.y == 80);

    DESTO_CHECK(SetWindowPos(window, nullptr, 7, 9, 300, 200, SWP_NOACTIVATE | SWP_NOZORDER));
    SendMessageW(window, WM_EXITSIZEMOVE, 0, 0);
    DESTO_CHECK(callbackCalled);
    DESTO_CHECK(changed.left == 0);
    DESTO_CHECK(changed.top == 0);
    DESTO_CHECK(changed.width == 300);
    DESTO_CHECK(changed.height == 200);
}

} // namespace

int main() {
    return desto::test::Run(RunTests);
}
