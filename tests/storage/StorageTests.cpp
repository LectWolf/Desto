#include "ApplicationCardReturn.h"
#include "MappingRegistry.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
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

} // namespace

int main() {
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
        assert(storageRoot.resolveCardPath("cards/application-1") == cardPath);

        bool rejected = false;
        try {
            (void)storageRoot.resolveCardPath("../outside");
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        assert(rejected);

        ApplicationCard application("application-1", "cards/application-1");
        ApplicationCardReturnService returnService(storageRoot);
        const auto plan = returnService.plan(application, desktopPath);
        assert(plan.preview.requiresConfirmation);
        assert(plan.moves.size() == 1);
        assert(plan.moves.front().destination.filename() == "Example (1).lnk");

        rejected = false;
        try {
            (void)returnService.execute(plan, {"other-card"});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        assert(rejected);

        const auto result = returnService.execute(plan, {"application-1"});
        assert(result.succeeded);
        assert(std::filesystem::exists(desktopPath / "Example (1).lnk"));
        assert(!std::filesystem::exists(cardPath / "Example.lnk"));

        MappingRegistry registry;
        assert(registry.tryRegister("mapping-1", testRoot / "Projects"));
        assert(!registry.tryRegister("mapping-2", testRoot / "Projects" / "."));
        assert(registry.ownerOf(testRoot / "Projects").value() == "mapping-1");
        assert(registry.tryRegister("mapping-1", testRoot / "OtherProjects"));
        assert(!registry.ownerOf(testRoot / "Projects").has_value());
        assert(registry.ownerOf(testRoot / "OtherProjects").value() == "mapping-1");
        assert(registry.size() == 1);
        registry.unregister("mapping-1");
        assert(!registry.ownerOf(testRoot / "OtherProjects").has_value());
    } catch (...) {
        std::filesystem::remove_all(testRoot);
        throw;
    }

    std::filesystem::remove_all(testRoot);
    return 0;
}
