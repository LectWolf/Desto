#include "TestSupport.h"
#include "WindowsDirectoryChangeSource.h"

#include <Windows.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>

using namespace desto::platform::windows;

namespace {

void RunTests() {
    const auto root = std::filesystem::temp_directory_path()
        / ("DestoDirectoryWatch-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    std::mutex mutex;
    std::condition_variable changedCondition;
    std::vector<std::vector<desto::domain::CardId>> batches;
    WindowsDirectoryChangeSource source(
        {{"mapping-1", root}},
        [&](std::vector<desto::domain::CardId> changed) {
            std::lock_guard lock(mutex);
            batches.push_back(std::move(changed));
            changedCondition.notify_all();
        });
    source.start();
    DESTO_CHECK(source.running());
    std::ofstream(root / "One.txt") << "one";
    std::ofstream(root / "Two.txt") << "two";
    {
        std::unique_lock lock(mutex);
        DESTO_CHECK(changedCondition.wait_for(
            lock,
            std::chrono::seconds(3),
            [&] { return !batches.empty(); }));
        DESTO_CHECK(batches.front() == std::vector<desto::domain::CardId>{"mapping-1"});
    }
    source.stop();
    DESTO_CHECK(!source.running());

    bool missingRejected = false;
    try {
        WindowsDirectoryChangeSource missing(
            {{"missing", root / "missing"}},
            [](std::vector<desto::domain::CardId>) {});
        missing.start();
    } catch (const std::runtime_error&) {
        missingRejected = true;
    }
    DESTO_CHECK(missingRejected);
    std::filesystem::remove_all(root);
}

} // namespace

int main() {
    return desto::test::Run(RunTests);
}
