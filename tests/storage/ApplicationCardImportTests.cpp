#include "ApplicationCardImport.h"
#include "MappingCardImport.h"
#include "TestSupport.h"

#include <chrono>
#include <fstream>

using namespace desto::domain;
using namespace desto::storage;

namespace {

void RunTests() {
    const auto token = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root = std::filesystem::temp_directory_path() / ("desto-import-" + token);
    const auto sourceDirectory = root / "incoming";
    const auto storageDirectory = root / "storage";
    std::filesystem::create_directories(sourceDirectory);
    std::ofstream(sourceDirectory / "App.lnk") << "first";
    std::ofstream(sourceDirectory / "Second.lnk") << "second";
    std::filesystem::create_directories(storageDirectory / "cards" / "application-1");
    std::ofstream(storageDirectory / "cards" / "application-1" / "App.lnk") << "existing";

    ApplicationCard card("application-1", "cards/application-1");
    ApplicationCardImportService service{StorageRoot(storageDirectory)};
    const std::vector<std::filesystem::path> sources{
        sourceDirectory / "App.lnk",
        sourceDirectory / "Second.lnk",
        sourceDirectory / "Second.lnk",
    };
    const auto plan = service.plan(card, sources);
    DESTO_CHECK(plan.moves.size() == 2);
    DESTO_CHECK(plan.moves[0].destination.filename() == "App (1).lnk");
    const auto result = service.execute(plan);
    DESTO_CHECK(result.succeeded);
    DESTO_CHECK(result.completedMoves.size() == 2);
    DESTO_CHECK(std::filesystem::exists(plan.cardDirectory / "App (1).lnk"));
    DESTO_CHECK(std::filesystem::exists(plan.cardDirectory / "Second.lnk"));
    DESTO_CHECK(!std::filesystem::exists(sources[0]));

    const std::vector<std::filesystem::path> alreadyStored{
        plan.cardDirectory / "Second.lnk",
    };
    DESTO_CHECK(service.plan(card, alreadyStored).moves.empty());

    bool rejectedAncestor = false;
    try {
        const std::vector<std::filesystem::path> ancestor{storageDirectory};
        (void)service.plan(card, ancestor);
    } catch (const std::invalid_argument&) {
        rejectedAncestor = true;
    }
    DESTO_CHECK(rejectedAncestor);

    const auto mappingRoot = root / "mapped";
    std::filesystem::create_directories(mappingRoot);
    std::ofstream(sourceDirectory / "Mapped.txt") << "mapped";
    std::ofstream(mappingRoot / "Mapped.txt") << "existing";
    MappingCard mapping("mapping-1");
    mapping.setFolderSource(mappingRoot);
    MappingCardImportService mappingService;
    const std::vector<std::filesystem::path> alreadyMapped{mappingRoot / "Mapped.txt"};
    const auto mappingPlan = mappingService.plan(mapping, alreadyMapped);
    DESTO_CHECK(mappingPlan.moves.empty());
    const std::vector<std::filesystem::path> mappingSources{
        sourceDirectory / "Mapped.txt",
    };
    const auto mappingMovePlan = mappingService.plan(mapping, mappingSources);
    DESTO_CHECK(mappingMovePlan.moves.size() == 1);
    DESTO_CHECK(mappingMovePlan.moves.front().destination.filename() == "Mapped (1).txt");
    DESTO_CHECK(mappingService.execute(mappingMovePlan).succeeded);
    DESTO_CHECK(std::filesystem::exists(mappingRoot / "Mapped (1).txt"));
    DESTO_CHECK(!std::filesystem::exists(mappingSources.front()));

    const auto mappingChild = mappingRoot / "Child";
    std::filesystem::create_directories(mappingChild);
    std::ofstream(sourceDirectory / "Child import.txt") << "child";
    const std::vector<std::filesystem::path> childSources{
        sourceDirectory / "Child import.txt",
    };
    const auto childPlan = mappingService.plan(mapping, mappingChild, childSources);
    DESTO_CHECK(childPlan.moves.size() == 1);
    DESTO_CHECK(childPlan.moves.front().destination.parent_path() == mappingChild);
    DESTO_CHECK(mappingService.execute(childPlan).succeeded);
    DESTO_CHECK(std::filesystem::exists(mappingChild / "Child import.txt"));
    DESTO_CHECK(!std::filesystem::exists(mappingRoot / "Child import.txt"));

    mapping.setAllowsSourceMutation(false);
    bool mutationRejected = false;
    try {
        (void)mappingService.plan(mapping, mappingSources);
    } catch (const std::invalid_argument&) {
        mutationRejected = true;
    }
    DESTO_CHECK(mutationRejected);

    std::filesystem::remove_all(root);
}

} // namespace

int main() {
    return desto::test::Run(RunTests);
}
