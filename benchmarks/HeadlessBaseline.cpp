#include "ApplicationLifecycle.h"
#include "ApplicationRuntime.h"
#include "JsonConfigStore.h"
#include "SingleInstance.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#include <Psapi.h>
#include <TlHelp32.h>
#endif

using namespace desto::application;
using namespace desto::domain;
using namespace desto::storage;

namespace {

struct Scenario {
    std::string name;
    int displayCount;
    int cardCount;
    int itemCount;
    bool hidden;
};

Scenario SelectScenario(std::string_view name) {
    if (name == "empty") {
        return {"empty", 1, 0, 0, false};
    }
    if (name == "typical") {
        return {"typical", 2, 4, 100, false};
    }
    if (name == "heavy") {
        return {"heavy", 3, 20, 1000, false};
    }
    if (name == "hidden") {
        return {"hidden", 2, 4, 100, true};
    }
    throw std::invalid_argument("Scenario must be empty, typical, heavy, or hidden.");
}

std::vector<DisplaySnapshot> MakeDisplays(int count) {
    std::vector<DisplaySnapshot> displays;
    displays.reserve(count);
    for (int index = 0; index < count; ++index) {
        displays.push_back({
            .id = "display-" + std::to_string(index),
            .workAreaWidth = 1920.0 + index * 160.0,
            .workAreaHeight = 1080.0,
            .primary = index == 0,
        });
    }
    return displays;
}

ApplicationConfig MakeConfig(const Scenario& scenario, const std::filesystem::path& root) {
    ApplicationConfig config;
    config.storageRoot = root / "storage";
    config.cards.reserve(scenario.cardCount);
    for (int index = 0; index < scenario.cardCount; ++index) {
        CardSnapshot card{
            .id = "card-" + std::to_string(index),
            .type = index % 3 == 0
                ? CardType::Application
                : index % 3 == 1 ? CardType::Mapping : CardType::Todo,
            .visible = !scenario.hidden,
            .expanded = true,
        };
        if (card.type == CardType::Application) {
            card.applicationStoragePath = "cards/" + card.id;
        } else if (card.type == CardType::Mapping) {
            card.mappingSourceRoot = "C:/DestoBenchmark/" + card.id;
            card.mappingAllowsSourceMutation = false;
        }
        config.cards.push_back(std::move(card));
        config.workspace.setPlacement({
            .id = "placement-" + std::to_string(index),
            .cardId = "card-" + std::to_string(index),
            .target = DisplayTarget::all(),
            .rect = {40.0 + index * 8.0, 48.0 + index * 6.0, 320, 220},
            .zIndex = index,
        });
    }

    int todoCards = 0;
    for (const auto& card : config.cards) {
        todoCards += card.type == CardType::Todo ? 1 : 0;
    }
    int remainingTodoCards = todoCards;
    int remainingItems = scenario.itemCount;
    for (auto& card : config.cards) {
        if (card.type != CardType::Todo) {
            continue;
        }
        const auto cardsLeft = remainingTodoCards--;
        const auto itemCount = remainingItems > 0
            ? (remainingItems + cardsLeft - 1) / cardsLeft
            : 0;
        for (int item = 0; item < itemCount && remainingItems > 0; ++item) {
            card.todoItems.push_back({
                .id = card.id + "-item-" + std::to_string(item),
                .title = "Benchmark item " + std::to_string(item),
                .completed = item % 2 == 0,
            });
            --remainingItems;
        }
    }
    return config;
}

std::uint64_t ProcessId() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

struct ProcessMetrics {
    std::uint64_t privateWorkingSetBytes = 0;
    std::uint32_t threads = 0;
    double cpuPercent = 0;
};

#ifdef _WIN32
std::uint64_t FileTimeValue(const FILETIME& time) {
    ULARGE_INTEGER value{};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return value.QuadPart;
}
#endif

ProcessMetrics ReadProcessMetrics() {
    ProcessMetrics result;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX memory{};
    if (GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
            sizeof(memory))) {
        result.privateWorkingSetBytes = memory.PrivateUsage;
    }

    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        THREADENTRY32 entry{.dwSize = sizeof(entry)};
        const auto processId = GetCurrentProcessId();
        if (Thread32First(snapshot, &entry)) {
            do {
                if (entry.th32OwnerProcessID == processId) {
                    ++result.threads;
                }
            } while (Thread32Next(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }

    FILETIME creation{}, exit{}, kernelBefore{}, userBefore{};
    GetProcessTimes(
        GetCurrentProcess(),
        &creation,
        &exit,
        &kernelBefore,
        &userBefore);
    const auto wallStart = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    FILETIME kernelAfter{}, userAfter{};
    GetProcessTimes(
        GetCurrentProcess(),
        &creation,
        &exit,
        &kernelAfter,
        &userAfter);
    const auto wallTicks = std::chrono::duration_cast<std::chrono::duration<double>>(
        std::chrono::steady_clock::now() - wallStart).count() * 10000000.0;
    const auto processTicks = static_cast<double>(
        FileTimeValue(kernelAfter) - FileTimeValue(kernelBefore)
        + FileTimeValue(userAfter) - FileTimeValue(userBefore));
    result.cpuPercent = wallTicks <= 0 ? 0 : processTicks / wallTicks * 100.0;
#endif
    return result;
}

double Milliseconds(const std::chrono::steady_clock::duration& duration) {
    return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(duration).count();
}

int Run(const Scenario& scenario) {
    const auto root = std::filesystem::temp_directory_path()
        / ("DestoHeadlessBaseline-" + std::to_string(ProcessId()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto configPath = root / "settings.json";

    try {
        const auto config = MakeConfig(scenario, root);
        JsonConfigStore store(configPath);
        store.save(config);

        const auto displays = MakeDisplays(scenario.displayCount);
        MemorySingleInstanceDomain instanceDomain;
        MemorySingleInstanceGate instanceGate(instanceDomain);
        DiagnosticRecorder diagnostics(DiagnosticLevel::Warning);
        ApplicationLifecycle lifecycle(instanceGate, diagnostics);
        ApplicationRuntime runtime;

        const auto startupBegin = std::chrono::steady_clock::now();
        const auto loaded = store.load();
        if (!lifecycle.begin().applied
            || !lifecycle.configurationLoaded().applied) {
            throw std::runtime_error("Headless lifecycle startup failed.");
        }
        runtime.restore(loaded.cards, loaded.workspace);
        if (!lifecycle.runtimeReady().applied
            || runtime.execute(UpdateDisplayTopology{displays}).status == CommandStatus::Rejected) {
            throw std::runtime_error("Headless runtime startup failed.");
        }
        const auto startupDuration = std::chrono::steady_clock::now() - startupBegin;

        const auto visibilityBegin = std::chrono::steady_clock::now();
        for (const auto* card : runtime.cards()) {
            (void)runtime.execute(SetCardVisibility{card->id(), !card->isVisible()});
        }
        const auto visibilityDuration = std::chrono::steady_clock::now() - visibilityBegin;

        auto changedDisplays = displays;
        for (auto& display : changedDisplays) {
            display.workAreaWidth += 1;
        }
        const auto topologyBegin = std::chrono::steady_clock::now();
        (void)runtime.execute(UpdateDisplayTopology{std::move(changedDisplays)});
        const auto topologyDuration = std::chrono::steady_clock::now() - topologyBegin;

        const auto metrics = ReadProcessMetrics();
        (void)lifecycle.requestShutdown(ShutdownReason::User);
        (void)lifecycle.completeShutdown();
        std::filesystem::remove_all(root);

        std::cout << "scenario,displays,cards,items,hidden,startup_ms,visibility_ms,topology_ms,"
                     "private_working_set_bytes,threads,idle_cpu_percent\n";
        std::cout << scenario.name << ',' << scenario.displayCount << ',' << scenario.cardCount << ','
                  << scenario.itemCount << ',' << (scenario.hidden ? 1 : 0) << ','
                  << Milliseconds(startupDuration) << ',' << Milliseconds(visibilityDuration) << ','
                  << Milliseconds(topologyDuration) << ',' << metrics.privateWorkingSetBytes << ','
                  << metrics.threads << ',' << metrics.cpuPercent << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(root);
        std::cerr << "baseline failed: " << error.what() << '\n';
        return 1;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3 || std::string_view(argv[1]) != "--scenario") {
        std::cerr << "usage: desto_headless_baseline --scenario <empty|typical|heavy|hidden>\n";
        return 2;
    }
    try {
        return Run(SelectScenario(argv[2]));
    } catch (const std::exception& error) {
        std::cerr << "baseline failed: " << error.what() << '\n';
        return 2;
    }
}
