#include "ApplicationCardReturn.h"
#include "ApplicationRuntime.h"
#include "FileCopyTransaction.h"
#include "JsonConfigStore.h"
#include "TodoDataStore.h"
#include "MappingRegistry.h"
#include "StorageRootMigration.h"
#include "TestSupport.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

using namespace desto::domain;
using namespace desto::application;
using namespace desto::storage;

namespace {

std::filesystem::path NewTestRoot() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path()
        / ("DestoStorageTests-" + std::to_string(suffix));
    std::filesystem::create_directories(root);
    return root;
}

void RunTests() {
    const auto testRoot = NewTestRoot();
    try {
        const auto storagePath = testRoot / "storage";
        const auto desktopPath = testRoot / "desktop";
        const auto cardPath = storagePath / "cards" / "application-1";
        std::filesystem::create_directories(cardPath);
        std::filesystem::create_directories(desktopPath);

        std::ofstream(cardPath / "Example.lnk") << "managed";
        std::ofstream(desktopPath / "Example.lnk") << "existing";

        StorageRoot storageRoot(storagePath);
        DESTO_CHECK(storageRoot.resolveCardPath("cards/application-1") == cardPath);

        bool rejected = false;
        try {
            (void)storageRoot.resolveCardPath("../outside");
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        DESTO_CHECK(rejected);

        ApplicationCard application("application-1", "cards/application-1");
        ApplicationCardReturnService returnService(storageRoot);
        const auto plan = returnService.plan(application, desktopPath);
        DESTO_CHECK(plan.preview.requiresConfirmation);
        DESTO_CHECK(plan.moves.size() == 1);
        DESTO_CHECK(plan.moves.front().destination.filename() == "Example (1).lnk");

        rejected = false;
        try {
            (void)returnService.execute(plan, {"other-card"});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        DESTO_CHECK(rejected);

        const auto result = returnService.execute(plan, {"application-1"});
        DESTO_CHECK(result.succeeded);
        DESTO_CHECK(std::filesystem::exists(desktopPath / "Example (1).lnk"));
        DESTO_CHECK(!std::filesystem::exists(cardPath / "Example.lnk"));

        const auto copySource = testRoot / "copy-source";
        const auto copyDestination = testRoot / "copy-destination";
        std::filesystem::create_directories(copySource / "Folder");
        std::filesystem::create_directories(copyDestination);
        std::ofstream(copySource / "File.txt") << "file";
        std::ofstream(copySource / "Folder" / "Nested.txt") << "nested";
        const std::vector<FileMove> copies{
            {copySource / "File.txt", copyDestination / "File.txt"},
            {copySource / "Folder", copyDestination / "Folder"},
        };
        const auto copied = FileCopyTransaction::execute(copies);
        DESTO_CHECK(copied.succeeded);
        DESTO_CHECK(std::filesystem::exists(copySource / "File.txt"));
        DESTO_CHECK(std::filesystem::exists(copySource / "Folder" / "Nested.txt"));
        DESTO_CHECK(std::filesystem::exists(copyDestination / "File.txt"));
        DESTO_CHECK(std::filesystem::exists(copyDestination / "Folder" / "Nested.txt"));

        const auto rollbackSource = testRoot / "copy-rollback-source";
        const auto rollbackDestination = testRoot / "copy-rollback-destination";
        std::filesystem::create_directories(rollbackSource);
        std::filesystem::create_directories(rollbackDestination);
        std::ofstream(rollbackSource / "First.txt") << "first";
        std::ofstream(rollbackSource / "Second.txt") << "second";
        std::ofstream(rollbackDestination / "Second.txt") << "occupied";
        const std::vector<FileMove> failingCopies{
            {rollbackSource / "First.txt", rollbackDestination / "First.txt"},
            {rollbackSource / "Second.txt", rollbackDestination / "Second.txt"},
        };
        const auto failedCopy = FileCopyTransaction::execute(failingCopies);
        DESTO_CHECK(!failedCopy.succeeded);
        DESTO_CHECK(std::filesystem::exists(rollbackSource / "First.txt"));
        DESTO_CHECK(std::filesystem::exists(rollbackSource / "Second.txt"));
        DESTO_CHECK(!std::filesystem::exists(rollbackDestination / "First.txt"));
        DESTO_CHECK(std::filesystem::exists(rollbackDestination / "Second.txt"));

        MappingRegistry registry;
        DESTO_CHECK(registry.tryRegister("mapping-1", testRoot / "Projects"));
        DESTO_CHECK(!registry.tryRegister("mapping-2", testRoot / "Projects" / "."));
        DESTO_CHECK(registry.ownerOf(testRoot / "Projects").value() == "mapping-1");
        DESTO_CHECK(registry.tryRegister("mapping-1", testRoot / "OtherProjects"));
        DESTO_CHECK(!registry.ownerOf(testRoot / "Projects").has_value());
        DESTO_CHECK(registry.ownerOf(testRoot / "OtherProjects").value() == "mapping-1");
        DESTO_CHECK(registry.size() == 1);
        registry.unregister("mapping-1");
        DESTO_CHECK(!registry.ownerOf(testRoot / "OtherProjects").has_value());

        const auto migrationSourcePath = testRoot / "migration-source";
        const auto migrationTargetPath = testRoot / "migration-target";
        std::filesystem::create_directories(migrationSourcePath / "nested");
        std::ofstream(migrationSourcePath / "nested" / "data.txt") << "data";
        StorageRootMigrationService migrationService;
        const auto migrationPlan = migrationService.plan(
            StorageRoot(migrationSourcePath),
            migrationTargetPath);
        const auto migration = migrationService.execute(migrationPlan);
        DESTO_CHECK(migration.succeeded);
        DESTO_CHECK(!std::filesystem::exists(migrationSourcePath));
        DESTO_CHECK(std::filesystem::exists(migrationTargetPath / "nested" / "data.txt"));
        const auto migrationRollback = FileMoveTransaction::rollback(migration.completedMoves);
        DESTO_CHECK(migrationRollback.succeeded);
        DESTO_CHECK(std::filesystem::exists(migrationSourcePath / "nested" / "data.txt"));

        const auto configPath = testRoot / "config" / "settings.json";
        std::filesystem::create_directories(configPath.parent_path());
        std::ofstream(configPath) << R"({
  "schemaVersion": 1,
  "storage": {"root": "C:\\OldDesto"},
  "futureFeature": {"enabled": true},
  "workspace": {
    "futureWorkspaceField": "keep-me",
    "placements": [{
      "id": "placement-1",
      "cardId": "card-1",
      "target": {"kind": "specific", "displayId": "display-a"},
      "rect": {"left": 10, "top": 20, "width": 300, "height": 200},
      "futurePlacementField": {"value": 42}
    }]
  }
})";
        JsonConfigStore configStore(configPath);
        auto loadedConfig = configStore.load();
        DESTO_CHECK(loadedConfig.schemaVersion == ApplicationConfig::CurrentSchemaVersion);
        DESTO_CHECK(loadedConfig.storageRoot == std::filesystem::path("C:\\OldDesto"));
        DESTO_CHECK(loadedConfig.workspace.placements().size() == 1);
        DESTO_CHECK(!loadedConfig.preferences.runAtStartup);
        DESTO_CHECK(loadedConfig.preferences.restoreWindowsOnNewWindow);

        const auto schemaThreePath = testRoot / "schema-3" / "settings.json";
        std::filesystem::create_directories(schemaThreePath.parent_path());
        std::ofstream(schemaThreePath) << R"({
  "schemaVersion": 3,
  "storage": {"root": "C:\\Desto"},
  "cards": [{
    "id": "legacy-application",
    "type": "application",
    "application": {
      "storagePath": "cards/legacy-application",
      "itemOrder": ["One.lnk", "Two.lnk", "Five.lnk"]
    }
  }]
})";
        const auto migratedThree = JsonConfigStore(schemaThreePath).load();
        DESTO_CHECK(migratedThree.schemaVersion == ApplicationConfig::CurrentSchemaVersion);
        DESTO_CHECK(migratedThree.cards.front().applicationSortMode
                    == ApplicationItemSortMode::Custom);
        DESTO_CHECK(migratedThree.cards.front().applicationItemPlacements.size() == 3);
        DESTO_CHECK(migratedThree.cards.front().applicationItemPlacements[2].column == 2);
        DESTO_CHECK(migratedThree.cards.front().applicationItemPlacements[2].row == 0);
        DESTO_CHECK(migratedThree.cards.front().content.itemSize == CardItemSize::Large);
        DESTO_CHECK(!migratedThree.cards.front().content.showItemNames);

        const auto schemaEightPath = testRoot / "schema-8" / "settings.json";
        std::filesystem::create_directories(schemaEightPath.parent_path());
        std::ofstream(schemaEightPath) << R"({
  "schemaVersion": 8,
  "storage": {"root": "C:\\Desto"},
  "cards": [{
    "id": "legacy-glass",
    "type": "todo",
    "appearance": {"preset": "apple-glass-black", "opacity": 0.72},
    "todo": {"items": []}
  }]
})";
        const auto migratedEight = JsonConfigStore(schemaEightPath).load();
        DESTO_CHECK(migratedEight.schemaVersion == ApplicationConfig::CurrentSchemaVersion);
        DESTO_CHECK(migratedEight.cards.front().appearance.preset == "mica-dark");
        DESTO_CHECK(migratedEight.cards.front().appearance.opacity == 0.92);
        const auto schemaNinePath = testRoot / "schema-9" / "settings.json";
        std::filesystem::create_directories(schemaNinePath.parent_path());
        std::ofstream(schemaNinePath) << R"({
  "schemaVersion": 9,
  "storage": {"root": "C:\\Desto"},
  "cards": [{
    "id": "legacy-todo",
    "type": "todo",
    "todo": {"items": [
      {"id": "visible", "title": "Visible", "completed": true},
      {"id": "archived", "title": "Archived", "completed": true, "archived": true}
    ]}
  }]
})";
        const auto migratedNine = JsonConfigStore(schemaNinePath).load();
        DESTO_CHECK(migratedNine.cards.front().todoItems[0].completedAtUnixMilliseconds > 0);
        DESTO_CHECK(!migratedNine.cards.front().todoItems[0].archived);
        DESTO_CHECK(migratedNine.cards.front().todoItems[1].completedAtUnixMilliseconds == 0);
        DESTO_CHECK(migratedNine.cards.front().todoItems[1].archived);
        const auto schemaEighteenPath = testRoot / "schema-18" / "settings.json";
        std::filesystem::create_directories(schemaEighteenPath.parent_path());
        std::ofstream(schemaEighteenPath) << R"({
  "schemaVersion": 18,
  "storage": {"root": "C:\\Desto"},
  "cards": [{
    "id": "legacy-small",
    "type": "application",
    "content": {"itemSize": "small", "fixedColumns": 6},
    "application": {"storagePath": "cards/legacy-small", "itemPlacements": []}
  }, {
    "id": "legacy-extension",
    "type": "extension",
    "content": {"itemSize": "medium", "fixedColumns": 5},
    "extension": {
      "extensionId": "desto.example",
      "cardId": "desto.example.sample",
      "schemaVersion": 1,
      "state": "{}"
    }
  }]
})";
        const auto migratedEighteen = JsonConfigStore(schemaEighteenPath).load();
        DESTO_CHECK(migratedEighteen.schemaVersion == ApplicationConfig::CurrentSchemaVersion);
        DESTO_CHECK(migratedEighteen.cards.size() == 1);
        DESTO_CHECK(migratedEighteen.cards[0].content.widthSpan == 4);
        DESTO_CHECK(migratedEighteen.preferences.cardOrder
                    == std::vector<CardId>({"legacy-small"}));
        const auto schemaNineteenPath = testRoot / "schema-19" / "settings.json";
        std::filesystem::create_directories(schemaNineteenPath.parent_path());
        std::ofstream(schemaNineteenPath) << R"({
  "schemaVersion": 19,
  "storage": {"root": "C:\\Desto"},
  "cards": [{
    "id": "legacy-transparent",
    "type": "todo",
    "appearance": {"preset": "transparent-black", "opacity": 0.56},
    "todo": {"items": []}
  }]
})";
        const auto migratedNineteen = JsonConfigStore(schemaNineteenPath).load();
        DESTO_CHECK(migratedNineteen.schemaVersion == ApplicationConfig::CurrentSchemaVersion);
        DESTO_CHECK(migratedNineteen.cards.front().appearance.preset == "transparent-white");
        DESTO_CHECK(migratedNineteen.cards.front().appearance.opacity == 0.56);
        const auto schemaTwentyTwoPath = testRoot / "schema-22" / "settings.json";
        std::filesystem::create_directories(schemaTwentyTwoPath.parent_path());
        std::ofstream(schemaTwentyTwoPath) << R"({
  "schemaVersion": 22,
  "storage": {"root": "C:\\Desto"},
  "cards": [{
    "id": "built-in",
    "type": "application",
    "content": {"itemSize": "large", "valueAnimation": "jump"},
    "application": {"storagePath": "cards/built-in", "itemPlacements": []},
    "futureCardField": "keep-me"
  }, {
    "id": "legacy-extension",
    "type": "extension",
    "extension": {
      "extensionId": "desto.example",
      "cardId": "desto.example.sample",
      "schemaVersion": 1,
      "state": "{}"
    }
  }],
  "workspace": {"placements": [{
    "id": "placement-built-in",
    "cardId": "built-in",
    "target": {"kind": "specific", "displayId": "display-a"},
    "rect": {"left": 10, "top": 20, "width": 300, "height": 200}
  }, {
    "id": "placement-extension",
    "cardId": "legacy-extension",
    "target": {"kind": "specific", "displayId": "display-a"},
    "rect": {"left": 30, "top": 40, "width": 320, "height": 220}
  }]},
  "settings": {
    "disabledExtensions": ["desto.example"],
    "cardOrder": ["legacy-extension", "built-in"]
  }
})";
        JsonConfigStore schemaTwentyTwoStore(schemaTwentyTwoPath);
        const auto migratedTwentyTwo = schemaTwentyTwoStore.load();
        DESTO_CHECK(migratedTwentyTwo.schemaVersion == ApplicationConfig::CurrentSchemaVersion);
        DESTO_CHECK(migratedTwentyTwo.cards.size() == 1);
        DESTO_CHECK(migratedTwentyTwo.cards.front().id == "built-in");
        DESTO_CHECK(migratedTwentyTwo.workspace.placements().size() == 1);
        DESTO_CHECK(migratedTwentyTwo.workspace.placements().front().cardId == "built-in");
        DESTO_CHECK(migratedTwentyTwo.preferences.cardOrder
                    == std::vector<CardId>({"built-in"}));
        schemaTwentyTwoStore.save(migratedTwentyTwo);
        std::ifstream migratedTwentyTwoFile(schemaTwentyTwoPath);
        const std::string migratedTwentyTwoText{
            std::istreambuf_iterator<char>(migratedTwentyTwoFile),
            std::istreambuf_iterator<char>()};
        DESTO_CHECK(migratedTwentyTwoText.find("legacy-extension") == std::string::npos);
        DESTO_CHECK(migratedTwentyTwoText.find("disabledExtensions") == std::string::npos);
        DESTO_CHECK(migratedTwentyTwoText.find("valueAnimation") == std::string::npos);
        DESTO_CHECK(migratedTwentyTwoText.find("futureCardField") != std::string::npos);
        const auto schemaTwentyThreePath = testRoot / "schema-23" / "settings.json";
        std::filesystem::create_directories(schemaTwentyThreePath.parent_path());
        std::ofstream(schemaTwentyThreePath) << R"({
  "schemaVersion": 23,
  "storage": {"root": "C:\\Desto"},
  "cards": [{
    "id": "file-card",
    "type": "application",
    "chrome": {},
    "application": {"storagePath": "cards/file-card", "itemPlacements": []}
  }, {
    "id": "todo-small",
    "type": "todo",
    "content": {"widthSpan": 3},
    "todo": {"items": []}
  }, {
    "id": "todo-medium",
    "type": "todo",
    "content": {"widthSpan": 4},
    "todo": {"items": []}
  }, {
    "id": "todo-large",
    "type": "todo",
    "content": {"widthSpan": 5},
    "todo": {"items": []}
  }],
  "workspace": {"placements": [{
    "id": "placement-left",
    "cardId": "file-card",
    "target": {"kind": "specific", "displayId": "display-a"},
    "rect": {"left": 10, "top": 20, "width": 244, "height": 120},
    "horizontalAnchor": "free"
  }, {
    "id": "placement-right",
    "cardId": "todo-large",
    "target": {"kind": "specific", "displayId": "display-a"},
    "rect": {"left": 1000, "top": 20, "width": 354, "height": 174},
    "horizontalAnchor": "right"
  }]}
})";
        const auto migratedTwentyThree = JsonConfigStore(schemaTwentyThreePath).load();
        DESTO_CHECK(migratedTwentyThree.schemaVersion
                    == ApplicationConfig::CurrentSchemaVersion);
        DESTO_CHECK(migratedTwentyThree.cards[0].chrome.showPresentationControl);
        DESTO_CHECK(migratedTwentyThree.cards[1].content.widthSpan == 4);
        DESTO_CHECK(migratedTwentyThree.cards[2].content.widthSpan == 5);
        DESTO_CHECK(migratedTwentyThree.cards[3].content.widthSpan == 6);
        DESTO_CHECK(migratedTwentyThree.workspace.placements()[0].horizontalAnchor
                    == PlacementHorizontalAnchor::Left);
        DESTO_CHECK(migratedTwentyThree.workspace.placements()[1].horizontalAnchor
                    == PlacementHorizontalAnchor::Right);
        const auto schemaTwentyFourPath = testRoot / "schema-24" / "settings.json";
        std::filesystem::create_directories(schemaTwentyFourPath.parent_path());
        std::ofstream(schemaTwentyFourPath) << R"({
  "schemaVersion": 24,
  "storage": {"root": "C:\\Desto"},
  "cards": [{
    "id": "old-crystal-preset",
    "type": "application",
    "appearance": {"preset": "transparent-white", "opacity": 0.36},
    "application": {"storagePath": "cards/old-crystal-preset", "itemPlacements": []}
  }, {
    "id": "custom-crystal-opacity",
    "type": "application",
    "appearance": {"preset": "transparent-white", "opacity": 0.5},
    "application": {"storagePath": "cards/custom-crystal-opacity", "itemPlacements": []}
  }]
})";
        const auto migratedTwentyFour = JsonConfigStore(schemaTwentyFourPath).load();
        DESTO_CHECK(migratedTwentyFour.cards[0].appearance.opacity == 0.32);
        DESTO_CHECK(migratedTwentyFour.cards[1].appearance.opacity == 0.5);
        const auto schemaTwentyFivePath = testRoot / "schema-25" / "settings.json";
        std::filesystem::create_directories(schemaTwentyFivePath.parent_path());
        std::ofstream(schemaTwentyFivePath) << R"({
  "schemaVersion": 25,
  "storage": {"root": "C:\\Desto"},
  "cards": [{
    "id": "first-crystal-preset",
    "type": "application",
    "appearance": {"preset": "transparent-white", "opacity": 0.62},
    "application": {"storagePath": "cards/first-crystal-preset", "itemPlacements": []}
  }, {
    "id": "custom-crystal-opacity",
    "type": "application",
    "appearance": {"preset": "transparent-white", "opacity": 0.5},
    "application": {"storagePath": "cards/custom-crystal-opacity", "itemPlacements": []}
  }]
})";
        const auto migratedTwentyFive = JsonConfigStore(schemaTwentyFivePath).load();
        DESTO_CHECK(migratedTwentyFive.cards[0].appearance.opacity == 0.32);
        DESTO_CHECK(migratedTwentyFive.cards[1].appearance.opacity == 0.5);
        const auto schemaTwentySixPath = testRoot / "schema-26" / "settings.json";
        std::filesystem::create_directories(schemaTwentySixPath.parent_path());
        std::ofstream(schemaTwentySixPath) << R"({
  "schemaVersion": 26,
  "storage": {"root": "C:\\Desto"},
  "cards": [{
    "id": "unlocked-card",
    "type": "todo",
    "chrome": {"showTitle": true},
    "todo": {"items": []}
  }]
})";
        const auto migratedTwentySix = JsonConfigStore(schemaTwentySixPath).load();
        DESTO_CHECK(migratedTwentySix.schemaVersion
                    == ApplicationConfig::CurrentSchemaVersion);
        DESTO_CHECK(!migratedTwentySix.cards[0].chrome.positionLocked);
        loadedConfig.cards = {
            {
                .id = "card-1",
                .type = CardType::Application,
                .visible = false,
                .expanded = true,
                .chrome = {.showCollapseControl = false, .showCloseControl = true,
                    .showPresentationControl = false, .showTitle = false,
                    .positionLocked = true},
                .appearance = {.preset = "compact", .opacity = 0.8},
                .content = {.itemSize = CardItemSize::Large, .showItemNames = false,
                    .maximumVisibleRows = 2},
                .applicationStoragePath = "cards/application-1",
                .applicationSortMode = ApplicationItemSortMode::Size,
                .applicationItemPlacements = {{"Editor.lnk", 0, 0}, {"Browser.lnk", 2, 0}},
                .applicationPresentationMode = MappingPresentationMode::List,
            },
            {
                .id = "mapping-1",
                .type = CardType::Mapping,
                .mappingSourceRoot = testRoot / "external-projects",
                .mappingAllowsSourceMutation = false,
                .mappingMode = MappingMode::Folder,
                .mappingPresentationMode = MappingPresentationMode::List,
                .mappingSortMode = ApplicationItemSortMode::Custom,
                .mappingItemPlacements = {
                    {testRoot / "external-projects" / "B.txt", 0, 0},
                    {testRoot / "external-projects" / "A.txt", 2, 0},
                },
            },
            {
                .id = "todo-1",
                .type = CardType::Todo,
                .todoItems = {
                    {.id = "todo-item-1", .title = "Ship persistence", .completed = true,
                     .createdAtUnixMilliseconds = 1723800000000,
                     .completedAtUnixMilliseconds = 1723803600000,
                     .scheduledDate = TodoDate{2024, 8, 16}},
                },
            },
        };
        loadedConfig.storageRoot = testRoot / "new-storage";
        loadedConfig.preferences.timeZoneOffsetMinutes = 8 * 60;
        loadedConfig.preferences.language = "zh-CN";
        loadedConfig.preferences.globalCardCornerRadius = 24.0;
        loadedConfig.preferences.runAtStartup = true;
        loadedConfig.preferences.desktopDoubleClickAction = "cards";
        loadedConfig.preferences.taskbarDoubleClickAction = "current-display";
        loadedConfig.preferences.restoreWindowsOnNewWindow = false;
        loadedConfig.preferences.pinnedCardsYieldToFullscreen = false;
        loadedConfig.preferences.cardOrder = {"todo-1", "card-1", "mapping-1"};
        auto anchoredPlacement = loadedConfig.workspace.placements().front();
        anchoredPlacement.horizontalAnchor = PlacementHorizontalAnchor::Right;
        anchoredPlacement.verticalAnchor = PlacementVerticalAnchor::Bottom;
        anchoredPlacement.referenceWorkAreaWidth = 1920;
        anchoredPlacement.referenceWorkAreaHeight = 1040;
        loadedConfig.workspace.setPlacement(std::move(anchoredPlacement));
        configStore.save(loadedConfig);
        std::ifstream savedConfig(configPath);
        const std::string savedText{
            std::istreambuf_iterator<char>(savedConfig),
            std::istreambuf_iterator<char>()};
        savedConfig.close();
        DESTO_CHECK(savedText.find("futureFeature") != std::string::npos);
        DESTO_CHECK(savedText.find("futureWorkspaceField") != std::string::npos);
        DESTO_CHECK(savedText.find("futurePlacementField") != std::string::npos);
        DESTO_CHECK(savedText.find("horizontalAnchor") != std::string::npos);
        DESTO_CHECK(savedText.find("referenceWorkArea") != std::string::npos);
        DESTO_CHECK(savedText.find("new-storage") != std::string::npos);
        for (const auto& entry : std::filesystem::directory_iterator(configPath.parent_path())) {
            DESTO_CHECK(entry.path().filename().wstring().find(L"settings.json.tmp-") == std::wstring::npos);
        }

        // The runtime migration writes Todo items beside settings.json. Once
        // that file exists, the main document must stop growing with the list.
        TodoDataStore todoDataStore(configPath.parent_path());
        todoDataStore.save(loadedConfig);
        auto compactConfig = loadedConfig;
        compactConfig.cards[2].todoItems.clear();
        configStore.save(compactConfig);
        std::ifstream compactSavedConfig(configPath);
        const std::string compactText{
            std::istreambuf_iterator<char>(compactSavedConfig),
            std::istreambuf_iterator<char>()};
        compactSavedConfig.close();
        DESTO_CHECK(compactText.find("\"items\"") == std::string::npos);
        auto restoredTodoConfig = configStore.load();
        todoDataStore.loadInto(restoredTodoConfig);
        DESTO_CHECK(restoredTodoConfig.cards[2].todoItems.size() == 1);
        DESTO_CHECK(restoredTodoConfig.cards[2].todoItems.front().title == "Ship persistence");

        auto reloadedConfig = configStore.load();
        todoDataStore.loadInto(reloadedConfig);
        DESTO_CHECK(reloadedConfig.cards.size() == 3);
        DESTO_CHECK(reloadedConfig.preferences.timeZoneOffsetMinutes == 8 * 60);
        DESTO_CHECK(reloadedConfig.preferences.language == "zh-CN");
        DESTO_CHECK(reloadedConfig.preferences.globalCardCornerRadius == 24.0);
        DESTO_CHECK(reloadedConfig.preferences.runAtStartup);
        DESTO_CHECK(reloadedConfig.preferences.desktopDoubleClickAction == "cards");
        DESTO_CHECK(reloadedConfig.preferences.taskbarDoubleClickAction == "current-display");
        DESTO_CHECK(!reloadedConfig.preferences.restoreWindowsOnNewWindow);
        DESTO_CHECK(!reloadedConfig.preferences.pinnedCardsYieldToFullscreen);
        DESTO_CHECK(reloadedConfig.preferences.cardOrder == loadedConfig.preferences.cardOrder);
        DESTO_CHECK(reloadedConfig.cards[0].id == "card-1");
        DESTO_CHECK(!reloadedConfig.cards[0].visible);
        DESTO_CHECK(!reloadedConfig.cards[0].chrome.showPresentationControl);
        DESTO_CHECK(reloadedConfig.cards[0].chrome.positionLocked);
        DESTO_CHECK(reloadedConfig.cards[0].appearance.preset == "compact");
        DESTO_CHECK(reloadedConfig.cards[0].content.itemSize == CardItemSize::Large);
        DESTO_CHECK(reloadedConfig.cards[0].content.widthSpan == 4);
        DESTO_CHECK(!reloadedConfig.cards[0].content.showItemNames);
        DESTO_CHECK(reloadedConfig.cards[0].content.maximumVisibleRows == 2);
        DESTO_CHECK(reloadedConfig.cards[0].applicationStoragePath
                    == std::filesystem::path("cards/application-1"));
        DESTO_CHECK(reloadedConfig.cards[0].applicationSortMode
                    == ApplicationItemSortMode::Size);
        DESTO_CHECK(reloadedConfig.cards[0].applicationItemPlacements.size() == 2);
        DESTO_CHECK(reloadedConfig.cards[0].applicationItemPlacements[1].column == 2);
        DESTO_CHECK(reloadedConfig.cards[0].applicationPresentationMode
                    == MappingPresentationMode::List);
        DESTO_CHECK(reloadedConfig.cards[1].mappingSourceRoot
                    == testRoot / "external-projects");
        DESTO_CHECK(!reloadedConfig.cards[1].mappingAllowsSourceMutation);
        DESTO_CHECK(reloadedConfig.cards[1].mappingMode == MappingMode::Folder);
        DESTO_CHECK(reloadedConfig.cards[1].mappingPresentationMode
                    == MappingPresentationMode::List);
        DESTO_CHECK(reloadedConfig.cards[1].mappingItemPlacements.size() == 2);
        DESTO_CHECK(reloadedConfig.cards[1].mappingItemPlacements[1].column == 2);
        DESTO_CHECK(reloadedConfig.cards[2].todoItems.size() == 1);
        DESTO_CHECK(reloadedConfig.cards[2].todoItems.front().completed);
        DESTO_CHECK(reloadedConfig.cards[2].todoItems.front().createdAtUnixMilliseconds > 0);
        DESTO_CHECK(reloadedConfig.cards[2].todoItems.front().completedAtUnixMilliseconds
                    == 1723803600000);
        DESTO_CHECK(reloadedConfig.workspace.placements().front().horizontalAnchor
                    == PlacementHorizontalAnchor::Right);
        DESTO_CHECK(reloadedConfig.workspace.placements().front().verticalAnchor
                    == PlacementVerticalAnchor::Bottom);
        DESTO_CHECK(reloadedConfig.workspace.placements().front().referenceWorkAreaWidth == 1920);
        DESTO_CHECK(reloadedConfig.workspace.placements().front().referenceWorkAreaHeight == 1040);

        ApplicationRuntime restoredRuntime;
        restoredRuntime.restore(reloadedConfig.cards, reloadedConfig.workspace);
        DESTO_CHECK(restoredRuntime.cards().size() == 3);
        DESTO_CHECK(!restoredRuntime.findCard("card-1")->isVisible());
        DESTO_CHECK(restoredRuntime.findCard("card-1")->appearance().preset == "compact");
        DESTO_CHECK(restoredRuntime.findCard("card-1")->content().itemSize == CardItemSize::Large);
        DESTO_CHECK(restoredRuntime.workspace().placements().size() == 1);

        auto invalidCardConfig = reloadedConfig;
        invalidCardConfig.cards[0].applicationStoragePath = "C:/absolute-storage";
        bool invalidCardRejected = false;
        try {
            configStore.save(invalidCardConfig);
        } catch (const std::invalid_argument&) {
            invalidCardRejected = true;
        }
        DESTO_CHECK(invalidCardRejected);
        DESTO_CHECK(configStore.load().cards[0].applicationStoragePath
                    == std::filesystem::path("cards/application-1"));

        bool invalidRestoreRejected = false;
        try {
            restoredRuntime.restore(invalidCardConfig.cards, invalidCardConfig.workspace);
        } catch (const std::invalid_argument&) {
            invalidRestoreRejected = true;
        }
        DESTO_CHECK(invalidRestoreRejected);
        DESTO_CHECK(restoredRuntime.findCard("card-1") != nullptr);
        DESTO_CHECK(!restoredRuntime.findCard("card-1")->isVisible());

        const auto recoveryConfigPath = testRoot / "recovery" / "settings.json";
        JsonConfigStore recoveryStore(recoveryConfigPath);
        auto recoveryConfig = reloadedConfig;
        recoveryConfig.storageRoot = testRoot / "recovery-storage-1";
        recoveryStore.save(recoveryConfig);
        recoveryConfig.storageRoot = testRoot / "recovery-storage-2";
        recoveryStore.save(recoveryConfig);
        const auto inspectedRecovery = recoveryStore.inspect();
        DESTO_CHECK(inspectedRecovery.primary.state == ConfigFileState::Valid);
        DESTO_CHECK(inspectedRecovery.backup1.state == ConfigFileState::Valid);
        DESTO_CHECK(inspectedRecovery.backup2.state == ConfigFileState::Missing);

        const auto explicitRecoveryPath = testRoot / "explicit-recovery" / "settings.json";
        JsonConfigStore explicitRecoveryStore(explicitRecoveryPath);
        auto explicitConfig = recoveryConfig;
        explicitConfig.storageRoot = testRoot / "explicit-storage-1";
        explicitRecoveryStore.save(explicitConfig);
        explicitConfig.storageRoot = testRoot / "explicit-storage-2";
        explicitRecoveryStore.save(explicitConfig);
        std::ofstream(explicitRecoveryPath, std::ios::binary | std::ios::trunc) << "{broken";
        const auto explicitInspection = explicitRecoveryStore.inspect();
        DESTO_CHECK(explicitInspection.primary.state == ConfigFileState::Invalid);
        DESTO_CHECK(explicitInspection.backup1.state == ConfigFileState::Valid);
        const auto backupConfig = explicitRecoveryStore.load(ConfigSource::Backup1);
        DESTO_CHECK(backupConfig.storageRoot == testRoot / "explicit-storage-1");
        explicitRecoveryStore.promoteBackup(ConfigSource::Backup1);
        DESTO_CHECK(explicitRecoveryStore.load().storageRoot
                    == testRoot / "explicit-storage-1");
        bool corruptCopyFound = false;
        for (const auto& entry : std::filesystem::directory_iterator(
                 explicitRecoveryPath.parent_path())) {
            if (entry.path().filename().wstring().find(L"settings.json.corrupt-") == 0) {
                corruptCopyFound = true;
                break;
            }
        }
        DESTO_CHECK(corruptCopyFound);

        std::ofstream(recoveryConfigPath, std::ios::binary | std::ios::trunc) << "{broken";
        const auto recoveredConfig = recoveryStore.load();
        DESTO_CHECK(recoveredConfig.recoveredFromBackup);
        DESTO_CHECK(recoveredConfig.storageRoot == testRoot / "recovery-storage-1");
        DESTO_CHECK(recoveredConfig.cards.size() == 3);
        recoveryStore.save(recoveredConfig);
        DESTO_CHECK(!recoveryStore.load().recoveredFromBackup);
        std::filesystem::remove(recoveryConfigPath);
        DESTO_CHECK(recoveryStore.load().recoveredFromBackup);
        recoveryStore.save(recoveredConfig);

        std::ofstream(recoveryConfigPath, std::ios::binary | std::ios::trunc)
            << R"({"schemaVersion": 99})";
        bool futureVersionRejected = false;
        try {
            (void)recoveryStore.load();
        } catch (const std::runtime_error&) {
            futureVersionRejected = true;
        }
        DESTO_CHECK(futureVersionRejected);

        const auto configuredMigrationSource = testRoot / "configured-migration-source";
        const auto configuredMigrationTarget = testRoot / "configured-migration-target";
        std::filesystem::create_directories(configuredMigrationSource);
        std::ofstream(configuredMigrationSource / "owned.dat") << "owned";
        const auto configuredMigration = migrationService.migrate(
            StorageRoot(configuredMigrationSource),
            configuredMigrationTarget,
            configStore);
        DESTO_CHECK(configuredMigration.succeeded);
        const auto afterMigrationConfig = configStore.load();
        DESTO_CHECK(afterMigrationConfig.storageRoot == configuredMigrationTarget);
        DESTO_CHECK(afterMigrationConfig.cards.size() == 3);
        DESTO_CHECK(afterMigrationConfig.workspace.placements().size() == 1);
        DESTO_CHECK(afterMigrationConfig.preferences.timeZoneOffsetMinutes == 8 * 60);
        DESTO_CHECK(afterMigrationConfig.preferences.language == "zh-CN");

        const auto failedMigrationSource = testRoot / "failed-migration-source";
        const auto failedMigrationTarget = testRoot / "failed-migration-target";
        std::filesystem::create_directories(failedMigrationSource);
        std::ofstream(failedMigrationSource / "data.txt") << "data";
        const auto invalidConfigPath = testRoot / "config-directory";
        std::filesystem::create_directories(invalidConfigPath);
        JsonConfigStore invalidConfigStore(invalidConfigPath);
        const auto failedMigration = migrationService.migrate(
            StorageRoot(failedMigrationSource),
            failedMigrationTarget,
            invalidConfigStore);
        DESTO_CHECK(!failedMigration.succeeded);
        DESTO_CHECK(std::filesystem::exists(failedMigrationSource / "data.txt"));
        DESTO_CHECK(!std::filesystem::exists(failedMigrationTarget / "data.txt"));
    } catch (...) {
        std::filesystem::remove_all(testRoot);
        throw;
    }

    std::filesystem::remove_all(testRoot);
}

} // namespace

int main() {
    return desto::test::Run(RunTests);
}
