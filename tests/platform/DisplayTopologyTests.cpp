#include "DisplayTopology.h"
#include "DisplayTopologyMonitor.h"
#include "TestSupport.h"
#ifdef _WIN32
#include "WindowsDisplayChangeSource.h"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>

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

    DisplayTopologyMonitor monitor(provider, std::chrono::milliseconds(100));
    monitor.initialize();
    const auto start = DisplayTopologyMonitor::TimePoint{};

    provider.setSnapshot({
        {.id = "display-a", .workAreaWidth = 1400, .workAreaHeight = 720},
        {.id = "display-b", .workAreaWidth = 1920, .workAreaHeight = 1040, .primary = true},
    });
    monitor.requestRefresh(start);
    DESTO_CHECK(!monitor.poll(start + std::chrono::milliseconds(99)).has_value());
    const auto modified = monitor.poll(start + std::chrono::milliseconds(100));
    DESTO_CHECK(modified.has_value());
    DESTO_CHECK(modified->changes.size() == 1);
    DESTO_CHECK(modified->changes.front().kind == DisplayChangeKind::Modified);
    DESTO_CHECK(modified->changes.front().id == "display-a");

    provider.setSnapshot({
        {.id = "display-a", .workAreaWidth = 1400.005, .workAreaHeight = 720},
        {.id = "display-b", .workAreaWidth = 1920, .workAreaHeight = 1040, .primary = true},
    });
    monitor.requestRefresh(start + std::chrono::milliseconds(200));
    DESTO_CHECK(!monitor.poll(start + std::chrono::milliseconds(300)).has_value());
    const auto currentDisplayA = std::find_if(
        monitor.current().begin(),
        monitor.current().end(),
        [](const DisplaySnapshot& display) { return display.id == "display-a"; });
    DESTO_CHECK(currentDisplayA != monitor.current().end());
    DESTO_CHECK(std::abs(currentDisplayA->workAreaWidth - 1400.005) < 1e-9);

    provider.setSnapshot({
        {.id = "display-a", .workAreaWidth = 1400, .workAreaHeight = 720},
    });
    monitor.requestRefresh(start + std::chrono::milliseconds(400));
    monitor.requestRefresh(start + std::chrono::milliseconds(450));
    DESTO_CHECK(!monitor.poll(start + std::chrono::milliseconds(549)).has_value());
    const auto removed = monitor.poll(start + std::chrono::milliseconds(550));
    DESTO_CHECK(removed.has_value());
    DESTO_CHECK(removed->changes.size() == 1);
    DESTO_CHECK(removed->changes.front().kind == DisplayChangeKind::Removed);
    DESTO_CHECK(removed->changes.front().id == "display-b");

    provider.setSnapshot({
        {.id = "display-a", .workAreaWidth = 1400, .workAreaHeight = 720},
        {.id = "display-a", .workAreaWidth = 1400, .workAreaHeight = 720},
    });
    monitor.requestRefresh(start + std::chrono::milliseconds(600));
    bool rejected = false;
    try {
        (void)monitor.poll(start + std::chrono::milliseconds(700));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    DESTO_CHECK(rejected);
    provider.setSnapshot({
        {.id = "display-a", .workAreaWidth = 1400, .workAreaHeight = 720},
        {.id = "display-b", .workAreaWidth = 1920, .workAreaHeight = 1040, .primary = true},
    });
    const auto recovered = monitor.poll(start + std::chrono::milliseconds(800));
    DESTO_CHECK(recovered.has_value());
    DESTO_CHECK(recovered->changes.size() == 1);
    DESTO_CHECK(recovered->changes.front().kind == DisplayChangeKind::Added);

#ifdef _WIN32
    windows::WindowsDisplayChangeSource source([] {});
    source.start();
    DESTO_CHECK(source.running());
    source.stop();
    DESTO_CHECK(!source.running());
#endif
}

} // namespace

int main() {
    return desto::test::Run(RunTests);
}
