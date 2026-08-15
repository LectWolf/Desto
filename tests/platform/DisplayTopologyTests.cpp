#include "DisplayTopology.h"
#include "TestSupport.h"

using namespace desto::domain;
using namespace desto::platform;

namespace {

void RunTests() {
    MemoryDisplayTopologyProvider provider({
        {.id = "display-b", .workAreaWidth = 1920, .workAreaHeight = 1040, .primary = true},
    });
    auto first = provider.snapshot();
    DESTO_CHECK(first.size() == 1);
    DESTO_CHECK(first.front().id == "display-b");

    first.front().id = "mutated-copy";
    DESTO_CHECK(provider.snapshot().front().id == "display-b");

    provider.setSnapshot({
        {.id = "display-a", .workAreaWidth = 1280, .workAreaHeight = 720},
        {.id = "display-b", .workAreaWidth = 1920, .workAreaHeight = 1040, .primary = true},
    });
    DESTO_CHECK(provider.snapshot().size() == 2);
}

} // namespace

int main() {
    return desto::test::Run(RunTests);
}
