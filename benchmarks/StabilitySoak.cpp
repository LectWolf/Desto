#include "JsonConfigStore.h"
#include "WindowsDesktopHost.h"
#include "WorkspaceLayout.h"

#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace desto::domain;
using namespace desto::platform::windows;
using namespace desto::presentation;
using namespace desto::storage;

namespace {

struct ResourceSnapshot {
    DWORD processHandles = 0;
    DWORD gdiObjects = 0;
    DWORD userObjects = 0;
    std::uint64_t privateBytes = 0;
};

ResourceSnapshot CaptureResources() {
    ResourceSnapshot result;
    if (!GetProcessHandleCount(GetCurrentProcess(), &result.processHandles)) {
        throw std::runtime_error("GetProcessHandleCount failed.");
    }
    result.gdiObjects = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    result.userObjects = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    PROCESS_MEMORY_COUNTERS_EX memory{};
    if (!GetProcessMemoryInfo(
            GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
            sizeof(memory))) {
        throw std::runtime_error("GetProcessMemoryInfo failed.");
    }
    result.privateBytes = memory.PrivateUsage;
    return result;
}

void PumpMessages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

HWND FindCurrentProcessWindowByClass(const wchar_t* className) {
    struct Search {
        DWORD processId = 0;
        const wchar_t* className = nullptr;
        HWND result = nullptr;
    } search{GetCurrentProcessId(), className};
    EnumWindows(+[](HWND candidate, LPARAM parameter) -> BOOL {
        auto& value = *reinterpret_cast<Search*>(parameter);
        DWORD processId = 0;
        wchar_t candidateClass[96]{};
        GetWindowThreadProcessId(candidate, &processId);
        if (processId == value.processId
            && GetClassNameW(candidate, candidateClass, static_cast<int>(std::size(candidateClass))) > 0
            && _wcsicmp(candidateClass, value.className) == 0) {
            value.result = candidate;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.result;
}

int ParseDuration(int argc, char** argv) {
    if (argc == 1) return 10000;
    if (argc != 3 || std::string_view(argv[1]) != "--duration-ms") {
        throw std::invalid_argument("usage: desto_stability_soak [--duration-ms <positive integer>]");
    }
    const auto value = std::stoi(argv[2]);
    if (value <= 0) throw std::invalid_argument("duration must be positive");
    return value;
}

int Run(int durationMilliseconds) {
    const auto root = std::filesystem::temp_directory_path()
        / ("DestoStabilitySoak-" + std::to_string(GetCurrentProcessId()));
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root);

    try {
        ApplicationConfig config;
        config.storageRoot = root / "storage";
        const auto configPath = root / "settings.json";
        JsonConfigStore store(configPath);
        store.save(config);

        const std::vector<DisplaySnapshot> displays{{
            .id = "display-soak", .workAreaWidth = 1920, .workAreaHeight = 1040,
            .effectiveDpi = 96, .primary = true,
        }};
        std::vector<PlacementProjection> projections;
        std::vector<CardView> cards;
        for (int index = 0; index < 4; ++index) {
            projections.push_back({
                .placementId = "placement-" + std::to_string(index),
                .cardId = "card-" + std::to_string(index),
                .displayId = "display-soak",
                .rect = {40.0 + index * 260.0, 48.0, 244, 220},
            });
            cards.push_back({
                .id = "card-" + std::to_string(index),
                .type = index == 3 ? CardType::Todo : CardType::Application,
                .title = L"Soak " + std::to_wstring(index + 1),
            });
        }

        WindowsDesktopHost host(L"Desto Stability Soak");
        host.present(projections, displays, cards);
        const auto lifecycle = FindCurrentProcessWindowByClass(L"DestoShellLifecycleHost");
        const auto taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        if (lifecycle == nullptr || taskbarCreated == 0) {
            throw std::runtime_error("Shell lifecycle window is unavailable.");
        }
        const auto cold = CaptureResources();
        host.setCardsVisible(false);
        host.setCardsVisible(true);
        SendMessageW(lifecycle, taskbarCreated, 0, 0);
        config.preferences.globalCardCornerRadius = 8.0;
        store.save(config);
        (void)store.load();
        PumpMessages();
        const auto baseline = CaptureResources();
        auto maximum = baseline;
        std::uint64_t iterations = 0;
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(durationMilliseconds);

        while (std::chrono::steady_clock::now() < deadline) {
            host.setCardsVisible(false);
            host.setCardsVisible(true);
            if (iterations % 10 == 0) {
                SendMessageW(lifecycle, taskbarCreated, 0, 0);
            }
            if (iterations % 25 == 0) {
                config.preferences.globalCardCornerRadius = iterations % 50 == 0 ? 8.0 : 16.0;
                store.save(config);
                if (store.load().preferences.globalCardCornerRadius
                    != config.preferences.globalCardCornerRadius) {
                    throw std::runtime_error("Configuration round trip diverged.");
                }
            }
            PumpMessages();
            const auto current = CaptureResources();
            maximum.processHandles = (std::max)(maximum.processHandles, current.processHandles);
            maximum.gdiObjects = (std::max)(maximum.gdiObjects, current.gdiObjects);
            maximum.userObjects = (std::max)(maximum.userObjects, current.userObjects);
            maximum.privateBytes = (std::max)(maximum.privateBytes, current.privateBytes);
            ++iterations;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        host.setCardsVisible(true);
        PumpMessages();
        const auto final = CaptureResources();
        std::cout << "duration_ms,iterations,handles_cold,handles_start,handles_peak,handles_end,"
                     "gdi_start,gdi_peak,gdi_end,user_start,user_peak,user_end,"
                     "private_start,private_peak,private_end\n";
        std::cout << durationMilliseconds << ',' << iterations << ','
                  << cold.processHandles << ',' << baseline.processHandles << ','
                  << maximum.processHandles << ','
                  << final.processHandles << ',' << baseline.gdiObjects << ','
                  << maximum.gdiObjects << ',' << final.gdiObjects << ','
                  << baseline.userObjects << ',' << maximum.userObjects << ','
                  << final.userObjects << ',' << baseline.privateBytes << ','
                  << maximum.privateBytes << ',' << final.privateBytes << '\n';

        const auto stable = final.processHandles <= baseline.processHandles + 2
            && final.gdiObjects <= baseline.gdiObjects + 2
            && final.userObjects <= baseline.userObjects + 2;
        std::filesystem::remove_all(root, ignored);
        return stable ? 0 : 1;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(root, ignored);
        std::cerr << "stability soak failed: " << error.what() << '\n';
        return 2;
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        return Run(ParseDuration(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
