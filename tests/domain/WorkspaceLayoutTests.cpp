#include "TestSupport.h"
#include "WorkspaceLayout.h"

#include <stdexcept>

using namespace desto::domain;

namespace {

template <typename Operation>
bool Rejects(Operation&& operation) {
    try {
        operation();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

void RunTests() {
    DESTO_CHECK(Rejects([] { (void)DisplayTarget::specific({}); }));

    WorkspaceLayout layout;
    layout.setPlacement({
        .id = "placement-a",
        .cardId = "card-1",
        .target = DisplayTarget::specific("display-a"),
        .rect = {100, 80, 320, 220},
        .zIndex = 2,
    });
    layout.setPlacement({
        .id = "placement-b",
        .cardId = "card-1",
        .target = DisplayTarget::specific("display-b"),
        .rect = {200, 160, 400, 300},
        .zIndex = 1,
    });

    DESTO_CHECK(Rejects([&] {
        layout.setPlacement({
            .id = "duplicate-display",
            .cardId = "card-1",
            .target = DisplayTarget::specific("display-a"),
        });
    }));
    DESTO_CHECK(Rejects([&] {
        layout.setPlacement({
            .id = "all-conflict",
            .cardId = "card-1",
            .target = DisplayTarget::all(),
        });
    }));
    DESTO_CHECK(Rejects([&] {
        layout.setPlacement({
            .id = "invalid-rect",
            .cardId = "card-2",
            .target = DisplayTarget::specific("display-a"),
            .rect = {0, 0, 0, 100},
        });
    }));
    DESTO_CHECK(Rejects([&] {
        layout.setPlacement({
            .id = "placement-a",
            .cardId = "other-card",
            .target = DisplayTarget::specific("display-a"),
        });
    }));

    const std::vector<DisplaySnapshot> onlyDisplayB{
        {.id = "display-b", .workAreaWidth = 1920, .workAreaHeight = 1040, .primary = true},
    };
    const auto actualWins = layout.project(onlyDisplayB);
    DESTO_CHECK(actualWins.size() == 1);
    DESTO_CHECK(actualWins.front().placementId == "placement-b");
    DESTO_CHECK(!actualWins.front().fallback);

    const std::vector<DisplaySnapshot> reconnected{
        {.id = "display-a", .workAreaWidth = 1280, .workAreaHeight = 720},
        {.id = "display-b", .workAreaWidth = 1920, .workAreaHeight = 1040, .primary = true},
    };
    const auto restored = layout.project(reconnected);
    DESTO_CHECK(restored.size() == 2);
    DESTO_CHECK(restored.front().displayId == "display-a");
    DESTO_CHECK(!restored.front().fallback);
    DESTO_CHECK(layout.placements().front().target.displayId() == "display-a");

    WorkspaceLayout offlineLayout;
    offlineLayout.setPlacement({
        .id = "missing-display",
        .cardId = "card-2",
        .target = DisplayTarget::specific("display-missing"),
        .rect = {1900, 1000, 500, 400},
    });
    DESTO_CHECK(offlineLayout.project(onlyDisplayB).empty());
    const auto offline = offlineLayout.unavailablePlacements(onlyDisplayB);
    DESTO_CHECK(offline.size() == 1);
    DESTO_CHECK(offline.front().target.displayId() == "display-missing");
    auto transferred = offline.front();
    transferred.target = DisplayTarget::specific("display-b");
    transferred.rect = {80, 96, 500, 400};
    offlineLayout.setPlacement(transferred);
    const auto transferredProjection = offlineLayout.project(onlyDisplayB);
    DESTO_CHECK(transferredProjection.size() == 1);
    DESTO_CHECK(transferredProjection.front().displayId == "display-b");

    WorkspaceLayout anchoredLayout;
    anchoredLayout.setPlacement({
        .id = "anchored",
        .cardId = "card-anchored",
        .target = DisplayTarget::specific("display-a"),
        .rect = {1612, 832, 300, 200},
        .horizontalAnchor = PlacementHorizontalAnchor::Right,
        .verticalAnchor = PlacementVerticalAnchor::Bottom,
        .referenceWorkAreaWidth = 1920,
        .referenceWorkAreaHeight = 1040,
    });
    const std::vector<DisplaySnapshot> resizedDisplay{{
        .id = "display-a",
        .workAreaWidth = 1280,
        .workAreaHeight = 720,
        .primary = true,
    }};
    const auto reflowed = anchoredLayout.project(resizedDisplay);
    DESTO_CHECK(reflowed.size() == 1);
    DESTO_CHECK(reflowed.front().rect.left == 972);
    DESTO_CHECK(reflowed.front().rect.top == 512);
    DESTO_CHECK(reflowed.front().horizontalAnchor == PlacementHorizontalAnchor::Right);
    DESTO_CHECK(reflowed.front().verticalAnchor == PlacementVerticalAnchor::Bottom);

    WorkspaceLayout allDisplaysLayout;
    allDisplaysLayout.setPlacement({
        .id = "all-displays",
        .cardId = "card-3",
        .target = DisplayTarget::all(),
    });
    const auto everyDisplay = allDisplaysLayout.project(reconnected);
    DESTO_CHECK(everyDisplay.size() == 2);
    DESTO_CHECK(!everyDisplay.front().requestedDisplayId.has_value());
    DESTO_CHECK(!everyDisplay.front().fallback);
    DESTO_CHECK(allDisplaysLayout.removeCard("card-3") == 1);
    DESTO_CHECK(allDisplaysLayout.placements().empty());

    DESTO_CHECK(Rejects([&] {
        const std::vector<DisplaySnapshot> duplicates{
            {.id = "same", .workAreaWidth = 100, .workAreaHeight = 100, .primary = true},
            {.id = "same", .workAreaWidth = 100, .workAreaHeight = 100},
        };
        (void)layout.project(duplicates);
    }));
}

} // namespace

int main() {
    return desto::test::Run(RunTests);
}
