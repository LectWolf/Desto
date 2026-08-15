#include "ApplicationCardReturn.h"
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
  "futureFeature": {"enabled": true}
})";
        JsonConfigStore configStore(configPath);
        const auto loadedConfig = configStore.load();
        DESTO_CHECK(loadedConfig.schemaVersion == 1);
        DESTO_CHECK(loadedConfig.storageRoot == std::filesystem::path("C:\\OldDesto"));
        configStore.save({.schemaVersion = 1, .storageRoot = testRoot / "new-storage"});
        std::ifstream savedConfig(configPath);
        const std::string savedText{
            std::istreambuf_iterator<char>(savedConfig),
            std::istreambuf_iterator<char>()};
        DESTO_CHECK(savedText.find("futureFeature") != std::string::npos);
        DESTO_CHECK(savedText.find("new-storage") != std::string::npos);
        for (const auto& entry : std::filesystem::directory_iterator(configPath.parent_path())) {
            DESTO_CHECK(entry.path().filename().wstring().find(L"settings.json.tmp-") == std::wstring::npos);
        }

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
