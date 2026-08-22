#include "WindowsDesktopHost.h"

#include <Windows.h>
#include <Psapi.h>
#include <TlHelp32.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace desto::domain;
using namespace desto::platform::windows;
using namespace desto::presentation;

namespace {

struct ScenarioData {
    std::string name;
    std::vector<DisplaySnapshot> displays;
    std::vector<PlacementProjection> projections;
    std::vector<CardView> cards;
    std::size_t itemCount = 0;
};

std::shared_ptr<const std::vector<std::uint32_t>> MakeIcon(int seed) {
    auto pixels = std::make_shared<std::vector<std::uint32_t>>(32 * 32);
    const auto red = static_cast<std::uint32_t>(48 + seed * 17 % 160);
    const auto green = static_cast<std::uint32_t>(72 + seed * 29 % 144);
    const auto blue = static_cast<std::uint32_t>(96 + seed * 41 % 128);
    const auto color = 0xff000000u | red << 16 | green << 8 | blue;
    std::fill(pixels->begin(), pixels->end(), color);
    return pixels;
}

std::vector<CardItemView> MakeFileItems(int count, int seed) {
    std::vector<CardItemView> result;
    result.reserve(count);
    for (int index = 0; index < count; ++index) {
        result.push_back({
            .id = L"item-" + std::to_wstring(seed) + L"-" + std::to_wstring(index),
            .displayName = L"Performance item " + std::to_wstring(index) + L".lnk",
            .sourcePath = L"C:\\DestoPerformance\\item-" + std::to_wstring(index) + L".lnk",
            .fileSize = static_cast<std::uintmax_t>(index + 1) * 4096,
            .itemType = L"Shortcut",
            .modifiedTime = 1'723'803'600'000 + index,
            .state = CardItemState::Ready,
            .icon = {.width = 32, .height = 32, .premultipliedPixels = MakeIcon(seed + index)},
        });
    }
    return result;
}

ScenarioData MakeScenario(std::string_view name) {
    ScenarioData result;
    result.name = std::string(name);
    result.displays = {{
        .id = "display-primary", .workAreaWidth = 1920, .workAreaHeight = 1040,
        .effectiveDpi = 96, .primary = true,
    }};
    if (name == "empty") return result;
    if (name != "typical") {
        throw std::invalid_argument("scenario must be empty or typical");
    }

    result.displays.push_back({
        .id = "display-secondary", .workAreaLeft = 1920, .workAreaWidth = 2560,
        .workAreaHeight = 1400, .effectiveDpi = 144,
    });
    for (int index = 0; index < 3; ++index) {
        CardView card{
            .id = "file-card-" + std::to_string(index),
            .type = index == 2 ? CardType::Mapping : CardType::Application,
            .title = L"Performance files " + std::to_wstring(index + 1),
            .typeLabel = index == 2 ? L"Mapping" : L"Application",
            .content = {
                .itemSize = CardItemSize::Large,
                .showItemNames = false,
                .sizeMode = CardSizeMode::Fixed,
                .widthSpan = 4,
                .fixedColumns = 4,
                .fixedRows = 4,
                .maximumVisibleRows = 4,
            },
            .items = MakeFileItems(30, index * 31),
        };
        if (index == 2) card.mappingHasSource = true;
        result.cards.push_back(std::move(card));
        result.projections.push_back({
            .placementId = "file-placement-" + std::to_string(index),
            .cardId = "file-card-" + std::to_string(index),
            .displayId = index == 1 ? "display-secondary" : "display-primary",
            .rect = {48.0 + index * 360.0, 56.0, 320, 420},
        });
    }

    CardView todo{
        .id = "todo-card",
        .type = CardType::Todo,
        .title = L"Performance todo",
        .typeLabel = L"Todo",
        .content = {.maximumVisibleRows = 5},
    };
    for (int index = 0; index < 10; ++index) {
        todo.todoItems.push_back({
            .id = "todo-" + std::to_string(index),
            .title = "Performance todo item " + std::to_string(index),
            .createdAtUnixMilliseconds = 1'723'803'600'000 + index,
            .scheduledDate = TodoDate{2026, 8, 18},
        });
    }
    result.cards.push_back(std::move(todo));
    result.projections.push_back({
        .placementId = "todo-placement",
        .cardId = "todo-card",
        .displayId = "display-secondary",
        .rect = {440, 520, 320, 420},
    });
    result.itemCount = 100;
    return result;
}

void PumpFor(std::chrono::milliseconds duration) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    MSG message{};
    do {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
}

std::uint64_t FileTimeValue(const FILETIME& time) {
    ULARGE_INTEGER value{};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return value.QuadPart;
}

double SampleIdleCpuPercent() {
    FILETIME creation{}, exit{}, kernelBefore{}, userBefore{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernelBefore, &userBefore)) {
        throw std::runtime_error("GetProcessTimes failed.");
    }
    const auto wallStart = std::chrono::steady_clock::now();
    PumpFor(std::chrono::seconds(1));
    FILETIME kernelAfter{}, userAfter{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernelAfter, &userAfter)) {
        throw std::runtime_error("GetProcessTimes failed.");
    }
    const auto wallTicks = std::chrono::duration_cast<std::chrono::duration<double>>(
        std::chrono::steady_clock::now() - wallStart).count() * 10'000'000.0;
    const auto processTicks = static_cast<double>(
        FileTimeValue(kernelAfter) - FileTimeValue(kernelBefore)
        + FileTimeValue(userAfter) - FileTimeValue(userBefore));
    return wallTicks <= 0 ? 0 : processTicks / wallTicks * 100.0;
}

std::uint64_t PrivateBytes() {
    PROCESS_MEMORY_COUNTERS_EX memory{};
    if (!GetProcessMemoryInfo(
            GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
            sizeof(memory))) {
        throw std::runtime_error("GetProcessMemoryInfo failed.");
    }
    return memory.PrivateUsage;
}

std::uint32_t ThreadCount() {
    std::uint32_t result = 0;
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) throw std::runtime_error("Thread snapshot failed.");
    THREADENTRY32 entry{.dwSize = sizeof(THREADENTRY32)};
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID == GetCurrentProcessId()) ++result;
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

double Milliseconds(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(duration).count();
}

double Percentile95(std::vector<double> samples) {
    std::ranges::sort(samples);
    const auto index = static_cast<std::size_t>(
        std::ceil(static_cast<double>(samples.size()) * 0.95)) - 1;
    return samples[(std::min)(index, samples.size() - 1)];
}

int Run(std::string_view scenarioName) {
    const auto startupBegin = std::chrono::steady_clock::now();
    auto scenario = MakeScenario(scenarioName);
    WindowsDesktopHost host(L"Desto Desktop Performance Gate");
    host.present(scenario.projections, scenario.displays, scenario.cards);
    const auto startupMilliseconds = Milliseconds(std::chrono::steady_clock::now() - startupBegin);
    PumpFor(std::chrono::milliseconds(250));

    host.setCardsVisible(false);
    host.setCardsVisible(true);
    std::vector<double> visibilitySamples;
    visibilitySamples.reserve(40);
    for (int iteration = 0; iteration < 20; ++iteration) {
        auto begin = std::chrono::steady_clock::now();
        host.setCardsVisible(false);
        visibilitySamples.push_back(Milliseconds(std::chrono::steady_clock::now() - begin));
        begin = std::chrono::steady_clock::now();
        host.setCardsVisible(true);
        visibilitySamples.push_back(Milliseconds(std::chrono::steady_clock::now() - begin));
    }
    PumpFor(std::chrono::milliseconds(100));
    const auto privateBytes = PrivateBytes();
    const auto threads = ThreadCount();
    const auto idleCpu = SampleIdleCpuPercent();
    const auto visibilityP95 = Percentile95(visibilitySamples);
    const auto memoryBudget = scenarioName == "empty" ? 30ull : 50ull;
    const auto privateMegabytes = static_cast<double>(privateBytes) / 1024.0 / 1024.0;
    const auto passed = privateMegabytes <= static_cast<double>(memoryBudget)
        && startupMilliseconds <= 500.0
        && visibilityP95 <= 16.0
        && idleCpu <= 0.5;

    std::cout << "scenario,displays,cards,items,startup_ms,visibility_p95_ms,"
                 "private_bytes,threads,idle_cpu_percent,passed\n";
    std::cout << scenario.name << ',' << scenario.displays.size() << ','
              << scenario.cards.size() << ',' << scenario.itemCount << ','
              << startupMilliseconds << ',' << visibilityP95 << ',' << privateBytes << ','
              << threads << ',' << idleCpu << ',' << (passed ? 1 : 0) << '\n';
    return passed ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc != 3 || std::string_view(argv[1]) != "--scenario") {
        std::cerr << "usage: desto_desktop_performance_gate --scenario <empty|typical>\n";
        return 2;
    }
    try {
        return Run(argv[2]);
    } catch (const std::exception& error) {
        std::cerr << "desktop performance gate failed: " << error.what() << '\n';
        return 2;
    }
}
