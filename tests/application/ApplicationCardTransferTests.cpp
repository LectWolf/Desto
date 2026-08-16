#include "ApplicationCardTransfer.h"
#include "TestSupport.h"

using namespace desto::application;
using namespace desto::domain;

namespace {

void RunTests() {
    const std::vector<ApplicationCardLocation> cards{
        {"application-1", "C:/Desto/cards/application-1"},
        {"application-2", "C:/Desto/cards/application-2"},
    };
    const std::vector<std::filesystem::path> sources{
        "C:/Desto/cards/application-1/Editor.lnk",
    };
    const auto refreshBatch = ResolveApplicationCardRefreshBatch(
        "application-2", cards, sources);
    DESTO_CHECK(refreshBatch == std::vector<CardId>({
        "application-1",
        "application-2",
    }));

    const std::vector<std::filesystem::path> externalSources{
        "C:/Users/LectWolf/Desktop/Editor.lnk",
    };
    DESTO_CHECK(ResolveApplicationCardRefreshBatch(
        "application-2", cards, externalSources) ==
        std::vector<CardId>{"application-2"});

    const std::vector<std::filesystem::path> sameCardSources{
        "C:/Desto/cards/application-1/Editor.lnk",
    };
    DESTO_CHECK(ResolveApplicationCardRefreshBatch(
        "application-1", cards, sameCardSources) ==
        std::vector<CardId>{"application-1"});

    const std::vector<std::filesystem::path> multipleSources{
        "C:/Desto/cards/application-2/Browser.lnk",
        "C:/Desto/cards/application-1/Editor.lnk",
        "C:/Desto/cards/application-1/Terminal.lnk",
    };
    DESTO_CHECK(ResolveApplicationCardRefreshBatch(
        "application-2", cards, multipleSources) ==
        std::vector<CardId>({"application-1", "application-2"}));

    const std::vector<std::filesystem::path> prefixCollision{
        "C:/Desto/cards/application-10/Editor.lnk",
    };
    DESTO_CHECK(ResolveApplicationCardRefreshBatch(
        "application-2", cards, prefixCollision) ==
        std::vector<CardId>{"application-2"});

    DESTO_CHECK(ResolveApplicationCardRefreshBatch(
        "missing", cards, externalSources).empty());
}

} // namespace

int main() {
    return desto::test::Run(RunTests);
}
