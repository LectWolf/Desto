#include "FileMoveTransaction.h"
#include "JsonConfigStore.h"
#include "TestSupport.h"
#include "WindowsDesktopHost.h"
#include "WindowsSingleInstanceGate.h"
#include "WorkspaceLayout.h"

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using namespace desto::application;
using namespace desto::domain;
using namespace desto::platform::windows;
using namespace desto::presentation;
using namespace desto::storage;

namespace {

class ScopedHandle final {
public:
    explicit ScopedHandle(HANDLE handle = nullptr) noexcept : handle_(handle) {}
    ~ScopedHandle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] HANDLE release() noexcept {
        const auto result = handle_;
        handle_ = nullptr;
        return result;
    }

private:
    HANDLE handle_ = nullptr;
};

std::filesystem::path NewTestRoot() {
    return std::filesystem::temp_directory_path()
        / (L"DestoFaultInjection-" + std::to_wstring(GetCurrentProcessId()) + L"-"
           + std::to_wstring(
               std::chrono::steady_clock::now().time_since_epoch().count()));
}

std::string ReadAll(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>(),
    };
}

std::size_t TemporaryConfigCount(const std::filesystem::path& directory) {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().filename().wstring().starts_with(L"settings.json.tmp-")) ++count;
    }
    return count;
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

void VerifyLockedMoveRollsBack(const std::filesystem::path& root) {
    const auto sourceDirectory = root / "move-source";
    const auto destinationDirectory = root / "move-destination";
    std::filesystem::create_directories(sourceDirectory);
    std::filesystem::create_directories(destinationDirectory);
    const auto firstSource = sourceDirectory / "first.txt";
    const auto secondSource = sourceDirectory / "second.txt";
    const auto firstDestination = destinationDirectory / "first.txt";
    const auto secondDestination = destinationDirectory / "second.txt";
    std::ofstream(firstSource) << "first";
    std::ofstream(secondSource) << "second";

    ScopedHandle lock(CreateFileW(
        secondSource.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr));
    DESTO_CHECK(lock.get() != INVALID_HANDLE_VALUE);

    const std::vector<FileMove> moves{
        {firstSource, firstDestination},
        {secondSource, secondDestination},
    };
    const auto result = FileMoveTransaction::execute(moves);
    DESTO_CHECK(!result.succeeded);
    DESTO_CHECK(result.completedMoves.empty());
    DESTO_CHECK(!result.failures.empty());
    DESTO_CHECK(ReadAll(firstSource) == "first");
    DESTO_CHECK(ReadAll(secondSource) == "second");
    DESTO_CHECK(!std::filesystem::exists(firstDestination));
    DESTO_CHECK(!std::filesystem::exists(secondDestination));
}

void VerifyInterruptedConfigPublication(const std::filesystem::path& root) {
    const auto configPath = root / "config" / "settings.json";
    JsonConfigStore store(configPath);
    ApplicationConfig config;
    config.storageRoot = root / "storage-before";
    store.save(config);
    const auto validBefore = ReadAll(configPath);

    ScopedHandle lock(CreateFileW(
        configPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr));
    DESTO_CHECK(lock.get() != INVALID_HANDLE_VALUE);
    config.storageRoot = root / "storage-after";
    bool publicationFailed = false;
    try {
        store.save(config);
    } catch (const std::exception&) {
        publicationFailed = true;
    }
    DESTO_CHECK(publicationFailed);
    DESTO_CHECK(ReadAll(configPath) == validBefore);
    DESTO_CHECK(TemporaryConfigCount(configPath.parent_path()) == 0);
    DESTO_CHECK(store.load().storageRoot == root / "storage-before");

    CloseHandle(lock.release());
    store.save(config);
    DESTO_CHECK(store.load().storageRoot == root / "storage-after");
}

void VerifyTopologyProjectionDoesNotDrift() {
    WorkspaceLayout layout;
    const CardPlacement original{
        .id = "stable-placement",
        .cardId = "stable-card",
        .target = DisplayTarget::specific("display-a"),
        .rect = {1572, 812, 320, 200},
        .zIndex = 7,
        .horizontalAnchor = PlacementHorizontalAnchor::Right,
        .verticalAnchor = PlacementVerticalAnchor::Bottom,
        .referenceWorkAreaWidth = 1920,
        .referenceWorkAreaHeight = 1040,
    };
    layout.setPlacement(original);
    const std::vector<DisplaySnapshot> large{{
        .id = "display-a", .workAreaWidth = 1920, .workAreaHeight = 1040,
        .effectiveDpi = 96, .primary = true,
    }};
    const std::vector<DisplaySnapshot> compactDisplay{{
        .id = "display-a", .workAreaWidth = 1280, .workAreaHeight = 680,
        .effectiveDpi = 144, .primary = true,
    }};
    const std::vector<DisplaySnapshot> offline{{
        .id = "display-b", .workAreaWidth = 2560, .workAreaHeight = 1400,
        .effectiveDpi = 120, .primary = true,
    }};

    for (int iteration = 0; iteration < 1000; ++iteration) {
        DESTO_CHECK(layout.project(iteration % 2 == 0 ? compactDisplay : large).size() == 1);
        DESTO_CHECK(layout.project(offline).empty());
        DESTO_CHECK(layout.unavailablePlacements(offline).size() == 1);
    }

    DESTO_CHECK(layout.placements().size() == 1);
    const auto& persisted = layout.placements().front();
    DESTO_CHECK(persisted.id == original.id);
    DESTO_CHECK(persisted.cardId == original.cardId);
    DESTO_CHECK(persisted.target == original.target);
    DESTO_CHECK(persisted.rect.left == original.rect.left);
    DESTO_CHECK(persisted.rect.top == original.rect.top);
    DESTO_CHECK(persisted.rect.width == original.rect.width);
    DESTO_CHECK(persisted.rect.height == original.rect.height);
    DESTO_CHECK(persisted.zIndex == original.zIndex);
    DESTO_CHECK(persisted.horizontalAnchor == original.horizontalAnchor);
    DESTO_CHECK(persisted.verticalAnchor == original.verticalAnchor);
    DESTO_CHECK(persisted.referenceWorkAreaWidth == original.referenceWorkAreaWidth);
    DESTO_CHECK(persisted.referenceWorkAreaHeight == original.referenceWorkAreaHeight);
}

void VerifyVisibilityAndShellRepairDoNotLeakResources() {
    const std::vector<DisplaySnapshot> displays{{
        .id = "display-test", .workAreaWidth = 1920, .workAreaHeight = 1040,
        .effectiveDpi = 96, .primary = true,
    }};
    const std::vector<PlacementProjection> projections{{
        .placementId = "stability-placement",
        .cardId = "stability-card",
        .displayId = "display-test",
        .rect = {40, 48, 320, 220},
    }};
    const std::vector<CardView> cards{{
        .id = "stability-card",
        .type = CardType::Application,
        .title = L"Stability",
    }};

    WindowsDesktopHost host(L"Desto Fault Injection Visibility");
    host.present(projections, displays, cards);
    const auto cardWindow = FindWindowW(
        L"DestoDesktopHostSurface", L"Desto Fault Injection Visibility");
    const auto lifecycleWindow = FindCurrentProcessWindowByClass(L"DestoShellLifecycleHost");
    DESTO_CHECK(cardWindow != nullptr);
    DESTO_CHECK(lifecycleWindow != nullptr);
    const auto gdiBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    const auto userBefore = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    DWORD handlesBefore = 0;
    DESTO_CHECK(GetProcessHandleCount(GetCurrentProcess(), &handlesBefore));
    const auto taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    DESTO_CHECK(taskbarCreated != 0);

    for (int iteration = 0; iteration < 1000; ++iteration) {
        host.setCardsVisible(false);
        DESTO_CHECK(!IsWindowVisible(cardWindow));
        host.setCardsVisible(true);
        DESTO_CHECK(IsWindowVisible(cardWindow));
        if (iteration % 20 == 0) {
            SendMessageW(lifecycleWindow, taskbarCreated, 0, 0);
            DESTO_CHECK(IsWindow(cardWindow));
        }
    }

    DWORD handlesAfter = 0;
    DESTO_CHECK(GetProcessHandleCount(GetCurrentProcess(), &handlesAfter));
    DESTO_CHECK(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == gdiBefore);
    DESTO_CHECK(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS) == userBefore);
    DESTO_CHECK(handlesAfter == handlesBefore);
}

std::wstring Quote(const std::wstring& value) {
    return L"\"" + value + L"\"";
}

void VerifyAbnormalExitReleasesProcessState(
    const std::filesystem::path& root,
    const std::filesystem::path& executable) {
    const auto configPath = root / "abnormal" / "settings.json";
    ApplicationConfig config;
    config.storageRoot = root / "abnormal-storage";
    JsonConfigStore(configPath).save(config);
    const auto mutexName = std::wstring(L"Local\\DestoFaultInjection-")
        + std::to_wstring(GetCurrentProcessId());
    auto commandLine = Quote(executable.wstring()) + L" --abnormal-child "
        + Quote(mutexName) + L" " + Quote(configPath.wstring());
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{.cb = sizeof(STARTUPINFOW)};
    PROCESS_INFORMATION process{};
    DESTO_CHECK(CreateProcessW(
        executable.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process));
    ScopedHandle processHandle(process.hProcess);
    ScopedHandle threadHandle(process.hThread);
    DESTO_CHECK(WaitForSingleObject(processHandle.get(), 5000) == WAIT_OBJECT_0);
    DWORD exitCode = 0;
    DESTO_CHECK(GetExitCodeProcess(processHandle.get(), &exitCode));
    DESTO_CHECK(exitCode == 77);

    WindowsSingleInstanceGate restarted(mutexName);
    DESTO_CHECK(restarted.acquire() == InstanceAcquireResult::Acquired);
    DESTO_CHECK(JsonConfigStore(configPath).load().storageRoot == root / "abnormal-storage");
    restarted.release();
}

int RunAbnormalChild(std::wstring_view mutexName, const std::filesystem::path& configPath) {
    WindowsSingleInstanceGate gate{std::wstring(mutexName)};
    if (gate.acquire() != InstanceAcquireResult::Acquired) return 71;
    if (JsonConfigStore(configPath).load().storageRoot.empty()) return 72;
    TerminateProcess(GetCurrentProcess(), 77);
    return 73;
}

void RunTests(const std::filesystem::path& executable) {
    const auto root = NewTestRoot();
    std::filesystem::create_directories(root);
    try {
        VerifyLockedMoveRollsBack(root);
        VerifyInterruptedConfigPublication(root);
        VerifyTopologyProjectionDoesNotDrift();
        VerifyVisibilityAndShellRepairDoNotLeakResources();
        VerifyAbnormalExitReleasesProcessState(root, executable);
        std::filesystem::remove_all(root);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        throw;
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && std::wstring_view(argv[1]) == L"--abnormal-child") {
        try {
            return RunAbnormalChild(argv[2], argv[3]);
        } catch (...) {
            return 74;
        }
    }
    return desto::test::Run([&] { RunTests(argv[0]); });
}
