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

    WorkspaceLayout fallbackLayout;
    fallbackLayout.setPlacement({
        .id = "missing-display",
        .cardId = "card-2",
        .target = DisplayTarget::specific("display-missing"),
        .rect = {1900, 1000, 500, 400},
    });
    const auto fallback = fallbackLayout.project(onlyDisplayB);
    DESTO_CHECK(fallback.size() == 1);
    DESTO_CHECK(fallback.front().fallback);
    DESTO_CHECK(fallback.front().requestedDisplayId == "display-missing");
    DESTO_CHECK(fallback.front().displayId == "display-b");
    DESTO_CHECK(fallback.front().rect.left == 1420);
    DESTO_CHECK(fallback.front().rect.top == 640);

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
