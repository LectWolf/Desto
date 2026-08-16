#include "ApplicationCardOrdering.h"
#include "TestSupport.h"

using namespace desto::application;
using namespace desto::domain;

namespace {

void RunTests() {
    const std::vector<ApplicationItemPlacement> sparse{
        {"One.lnk", 0, 0},
        {"Three.lnk", 2, 0},
    };
    const std::vector<std::filesystem::path> actual{"One.lnk", "Three.lnk"};
    const auto reconciled = ReconcileApplicationItemPlacements(sparse, actual, 5);
    DESTO_CHECK(reconciled.fits);
    DESTO_CHECK(reconciled.placements == sparse);

    const std::vector<ApplicationItemPlacement> wideCustom{{"One.lnk", 4, 0}};
    const auto preservedWhenNarrow = ReconcileApplicationItemPlacements(
        wideCustom, std::vector<std::filesystem::path>{"One.lnk"}, 3);
    DESTO_CHECK(preservedWhenNarrow.placements == wideCustom);

    const std::vector<std::filesystem::path> withNew{"One.lnk", "Two.lnk", "Three.lnk"};
    const auto appended = ReconcileApplicationItemPlacements(sparse, withNew, 5);
    const ApplicationItemPlacement expectedNew{"Two.lnk", 1, 0};
    DESTO_CHECK(appended.placements[2] == expectedNew);

    const auto moved = MoveApplicationItemsToSlot(
        appended.placements,
        std::vector<std::filesystem::path>{"One.lnk"},
        4,
        1,
        5);
    DESTO_CHECK(moved.fits);
    const ApplicationItemPlacement expectedMoved{"One.lnk", 4, 1};
    DESTO_CHECK(moved.placements.back() == expectedMoved);

    const auto insertedOnOccupied = MoveApplicationItemsToSlot(
        sparse,
        std::vector<std::filesystem::path>{"New.lnk"},
        0,
        0,
        5);
    DESTO_CHECK(insertedOnOccupied.fits);
    const auto inserted = std::ranges::find(
        insertedOnOccupied.placements, std::filesystem::path{"New.lnk"},
        &ApplicationItemPlacement::fileName);
    const auto displaced = std::ranges::find(
        insertedOnOccupied.placements, std::filesystem::path{"One.lnk"},
        &ApplicationItemPlacement::fileName);
    DESTO_CHECK(inserted->column == 0 && inserted->row == 0);
    DESTO_CHECK(displaced->column == 1 && displaced->row == 0);

    const auto full = ReconcileApplicationItemPlacements(
        {}, withNew, 2, 1);
    DESTO_CHECK(!full.fits);
    DESTO_CHECK(full.placements.size() == 2);

    const std::vector<ApplicationItemSortData> metadata{
        {"Large.txt", L"Large", 20, L"Text", 100},
        {"App.lnk", L"App", 4, L"Shortcut", 300},
        {"Small.txt", L"Small", 4, L"Text", 200},
    };
    const auto byName = ProjectApplicationItems(metadata, sparse, ApplicationItemSortMode::Name, 2);
    DESTO_CHECK(byName[0].fileName == "App.lnk");
    DESTO_CHECK(byName[2].column == 0 && byName[2].row == 1);
    const auto bySize = ProjectApplicationItems(metadata, sparse, ApplicationItemSortMode::Size, 5);
    DESTO_CHECK(bySize[0].fileName == "App.lnk");
    DESTO_CHECK(bySize[1].fileName == "Small.txt");
    const auto byType = ProjectApplicationItems(metadata, sparse, ApplicationItemSortMode::ItemType, 5);
    DESTO_CHECK(byType[0].fileName == "App.lnk");
    const auto byModified = ProjectApplicationItems(
        metadata, sparse, ApplicationItemSortMode::ModifiedDate, 5);
    DESTO_CHECK(byModified[0].fileName == "App.lnk");

    const auto customAgain = ProjectApplicationItems(
        metadata,
        std::vector<ApplicationItemPlacement>{{"Large.txt", 0, 0}, {"App.lnk", 2, 0}, {"Small.txt", 4, 1}},
        ApplicationItemSortMode::Custom,
        5);
    DESTO_CHECK(customAgain[1].column == 2 && customAgain[1].row == 0);
    DESTO_CHECK(customAgain[2].column == 4 && customAgain[2].row == 1);

    const std::vector<ApplicationItemSortData> sixItems{
        {"6", L"6"}, {"5", L"5"}, {"4", L"4"},
        {"3", L"3"}, {"2", L"2"}, {"1", L"1"},
    };
    const auto fiveColumns = ProjectApplicationItems(
        sixItems, {}, ApplicationItemSortMode::Name, 5);
    DESTO_CHECK(fiveColumns[5].column == 0 && fiveColumns[5].row == 1);
}

} // namespace

int main() { return desto::test::Run(RunTests); }
