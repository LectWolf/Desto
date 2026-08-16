#include "ApplicationCardReturn.h"
#include "ApplicationRuntime.h"
#include "JsonConfigStore.h"
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
        DESTO_CHECK(!migratedThree.cards.front().content.showItemNames);
        loadedConfig.cards = {
            {
                .id = "card-1",
                .type = CardType::Application,
                .visible = false,
                .expanded = true,
                .chrome = {.showCollapseControl = false, .showCloseControl = true, .showTitle = false},
                .appearance = {.preset = "compact", .opacity = 0.8},
                .content = {.itemSize = CardItemSize::Large, .showItemNames = false},
                .applicationStoragePath = "cards/application-1",
                .applicationSortMode = ApplicationItemSortMode::Size,
                .applicationItemPlacements = {{"Editor.lnk", 0, 0}, {"Browser.lnk", 2, 0}},
            },
            {
                .id = "mapping-1",
                .type = CardType::Mapping,
                .mappingSourceRoot = testRoot / "external-projects",
                .mappingAllowsSourceMutation = false,
            },
            {
                .id = "todo-1",
                .type = CardType::Todo,
                .todoItems = {
                    {.id = "todo-item-1", .title = "Ship persistence", .completed = true},
                },
            },
        };
        loadedConfig.storageRoot = testRoot / "new-storage";
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

        const auto reloadedConfig = configStore.load();
        DESTO_CHECK(reloadedConfig.cards.size() == 3);
        DESTO_CHECK(reloadedConfig.cards[0].id == "card-1");
        DESTO_CHECK(!reloadedConfig.cards[0].visible);
        DESTO_CHECK(reloadedConfig.cards[0].appearance.preset == "compact");
        DESTO_CHECK(reloadedConfig.cards[0].content.itemSize == CardItemSize::Large);
        DESTO_CHECK(!reloadedConfig.cards[0].content.showItemNames);
        DESTO_CHECK(reloadedConfig.cards[0].applicationStoragePath
                    == std::filesystem::path("cards/application-1"));
        DESTO_CHECK(reloadedConfig.cards[0].applicationSortMode
                    == ApplicationItemSortMode::Size);
        DESTO_CHECK(reloadedConfig.cards[0].applicationItemPlacements.size() == 2);
        DESTO_CHECK(reloadedConfig.cards[0].applicationItemPlacements[1].column == 2);
        DESTO_CHECK(reloadedConfig.cards[1].mappingSourceRoot
                    == testRoot / "external-projects");
        DESTO_CHECK(!reloadedConfig.cards[1].mappingAllowsSourceMutation);
        DESTO_CHECK(reloadedConfig.cards[2].todoItems.size() == 1);
        DESTO_CHECK(reloadedConfig.cards[2].todoItems.front().completed);
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
