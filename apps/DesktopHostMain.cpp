#include "ApplicationLifecycle.h"
#include "ApplicationCardImport.h"
#include "ApplicationCardOrdering.h"
#include "ApplicationCardReturn.h"
#include "ApplicationCardTransfer.h"
#include "ApplicationRuntime.h"
#include "Diagnostics.h"
#include "CardContentLayout.h"
#include "FileCopyTransaction.h"
#include "FileMoveTransaction.h"
#include "JsonConfigStore.h"
#include "TodoDataStore.h"
#include "MappingCardImport.h"
#include "DirectoryImportPlanner.h"
#include "CardView.h"
#include "StorageRoot.h"
#include "StorageRootMigration.h"
#include "WindowsDesktopHost.h"
#include "WindowsConfirmationDialog.h"
#include "WindowsDesktopTriggerHost.h"
#include "WindowsDirectoryChangeSource.h"
#include "WindowsDisplayTopology.h"
#include "WindowsShellItemCatalog.h"
#include "WindowsSettingsHost.h"
#include "WindowsSingleInstanceGate.h"
#include "WindowsStartupIntegration.h"
#include "WindowsTrayHost.h"
#include "RecoveryView.h"

#include <Windows.h>
#include <shlobj.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace desto::application;
using namespace desto::domain;
using namespace desto::platform::windows;
using namespace desto::storage;

namespace {

DestoLaunchOptions ReadLaunchOptions() noexcept {
    int argumentCount = 0;
    auto** arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments == nullptr) return {};
    std::vector<std::wstring_view> views;
    views.reserve(static_cast<std::size_t>(argumentCount > 1 ? argumentCount - 1 : 0));
    for (int index = 1; index < argumentCount; ++index) views.emplace_back(arguments[index]);
    const auto options = ParseDestoLaunchOptions(views);
    LocalFree(arguments);
    return options;
}

std::filesystem::path CurrentExecutablePath() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const auto length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) throw std::runtime_error("Unable to resolve the Desto executable path.");
        if (length + 1 < buffer.size()) {
            return std::filesystem::path(
                std::wstring_view(buffer.data(), static_cast<std::size_t>(length)));
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::filesystem::path DefaultStorageRoot() {
    wchar_t* localApplicationData = nullptr;
    if (FAILED(SHGetKnownFolderPath(
            FOLDERID_LocalAppData,
            KF_FLAG_DEFAULT,
            nullptr,
            &localApplicationData))
        || localApplicationData == nullptr) {
        throw std::runtime_error("Unable to resolve the local application data directory.");
    }
    const auto result = std::filesystem::path(localApplicationData) / "Desto" / "Data";
    CoTaskMemFree(localApplicationData);
    return result;
}

std::filesystem::path DesktopDirectory() {
    wchar_t* desktop = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, KF_FLAG_DEFAULT, nullptr, &desktop))
        || desktop == nullptr) {
        throw std::runtime_error("Unable to resolve the desktop directory.");
    }
    const auto result = std::filesystem::path(desktop);
    CoTaskMemFree(desktop);
    return result;
}

std::string WideToUtf8(std::wstring_view value) {
    if (value.empty()) return {};
    const auto required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), required, nullptr, nullptr) != required) {
        return {};
    }
    return result;
}

std::wstring Utf8ToWide(std::string_view value) {
    if (value.empty()) return {};
    const auto length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        result.data(), length);
    return result;
}

std::string ResolveUiLanguage(std::string_view preference) {
    if (preference == "zh-CN" || preference == "en-US") {
        return std::string(preference);
    }
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE
        ? "zh-CN" : "en-US";
}

std::int64_t TodoDateNoonUnixMilliseconds(
    TodoDate date,
    std::optional<std::int32_t> offsetMinutes) noexcept {
    using namespace std::chrono;
    const auto archiveDay = sys_days{
        year{date.year} / month{date.month} / std::chrono::day{date.day}};
    if (offsetMinutes.has_value()) {
        return duration_cast<milliseconds>(
            (archiveDay + hours{12} - minutes{*offsetMinutes}).time_since_epoch()).count();
    }
    std::tm local{};
    local.tm_year = date.year - 1900;
    local.tm_mon = static_cast<int>(date.month) - 1;
    local.tm_mday = date.day;
    local.tm_hour = 12;
    local.tm_isdst = -1;
    const auto seconds = std::mktime(&local);
    return seconds < 0 ? 0 : static_cast<std::int64_t>(seconds) * 1000;
}

std::wstring UiText(
    std::string_view resolvedLanguage,
    std::wstring_view chinese,
    std::wstring_view english) {
    return std::wstring(resolvedLanguage == "en-US" ? english : chinese);
}

const ApplicationCard* FindApplicationCard(
    const ApplicationRuntime& runtime,
    const CardId& cardId) {
    const auto cards = runtime.cards();
    const auto found = std::find_if(
        cards.begin(),
        cards.end(),
        [&](const Card* card) {
            return card->id() == cardId && card->type() == CardType::Application;
        });
    return found == cards.end() ? nullptr : static_cast<const ApplicationCard*>(*found);
}

const MappingCard* FindMappingCard(
    const ApplicationRuntime& runtime,
    const CardId& cardId) {
    const auto* card = runtime.findCard(cardId);
    return card != nullptr && card->type() == CardType::Mapping
        ? static_cast<const MappingCard*>(card) : nullptr;
}

struct MappingNavigationState {
    std::vector<std::filesystem::path> directoryStack;
    std::unordered_map<std::wstring, std::vector<ApplicationItemPlacement>> placementsByDirectory;
};

std::vector<desto::presentation::CardItemView> MappingItems(
    const MappingCard& card,
    WindowsShellItemCatalog& catalog,
    CardItemSize itemSize) {
    const auto iconSize = ResolveShellIconSourceSize(itemSize);
    if (card.mode() == MappingMode::Folder) {
        if (card.sourceRoot().empty()) return {};
        return catalog.enumerate(card.sourceRoot(), {}, iconSize);
    }
    std::vector<desto::presentation::CardItemView> result;
    result.reserve(card.references().size());
    for (const auto& reference : card.references()) {
        result.push_back(catalog.inspect(reference.path, iconSize));
    }
    return result;
}

bool MappingHasSource(const MappingCard& card) noexcept {
    return !card.sourceRoot().empty() || !card.references().empty();
}

std::vector<std::filesystem::path> ItemFileNames(
    const std::vector<desto::presentation::CardItemView>& items) {
    std::vector<std::filesystem::path> result;
    result.reserve(items.size());
    for (const auto& item : items) {
        result.push_back(item.sourcePath.filename());
    }
    return result;
}

std::vector<std::filesystem::path> ItemSourcePaths(
    const std::vector<desto::presentation::CardItemView>& items) {
    std::vector<std::filesystem::path> result;
    result.reserve(items.size());
    for (const auto& item : items) result.push_back(item.sourcePath.lexically_normal());
    return result;
}

std::wstring PathKey(const std::filesystem::path& path) {
    auto result = path.lexically_normal().wstring();
    std::ranges::transform(result, result.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return result;
}

std::string MappingReferenceId(
    const std::filesystem::path& path,
    const std::vector<FileReference>& existing) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const auto character : PathKey(path)) {
        hash ^= static_cast<std::uint64_t>(character);
        hash *= 1099511628211ull;
    }
    const auto base = "reference-" + std::to_string(hash);
    auto candidate = base;
    for (std::size_t suffix = 1;
         std::ranges::any_of(existing, [&](const FileReference& reference) {
             return reference.id == candidate;
         });
         ++suffix) {
        candidate = base + "-" + std::to_string(suffix);
    }
    return candidate;
}

const desto::presentation::CardItemView* FindCachedItem(
    const std::unordered_map<
        CardId,
        std::vector<desto::presentation::CardItemView>>& itemsByCard,
    const std::filesystem::path& path) {
    const auto key = PathKey(path);
    for (const auto& [cardId, items] : itemsByCard) {
        (void)cardId;
        const auto found = std::ranges::find_if(items, [&](const auto& item) {
            return PathKey(item.sourcePath) == key;
        });
        if (found != items.end()) return &*found;
    }
    return nullptr;
}

std::vector<std::filesystem::path> PlacementOrder(const ApplicationCard& card) {
    auto placements = card.itemPlacements();
    std::ranges::sort(placements, {}, [](const ApplicationItemPlacement& placement) {
        return std::pair{placement.row, placement.column};
    });
    std::vector<std::filesystem::path> result;
    result.reserve(placements.size());
    for (const auto& placement : placements) result.push_back(placement.fileName);
    return result;
}

std::uint32_t CardColumns(
    const ApplicationRuntime&,
    const ApplicationCard& card,
    std::optional<std::size_t> projectedItemCount = std::nullopt) {
    (void)projectedItemCount;
    if (card.content().sizeMode == CardSizeMode::Fixed) {
        return static_cast<std::uint32_t>(desto::domain::ProjectCardColumns(
            card.content().widthSpan, card.content().itemSize));
    }
    const auto settings = desto::presentation::ResolveCardContentLayoutSettings(card.content());
    std::size_t preservedColumns = 0;
    for (const auto& placement : card.itemPlacements()) {
        preservedColumns = (std::max)(
            preservedColumns, static_cast<std::size_t>(placement.column) + 1);
    }
    if (card.sortMode() != ApplicationItemSortMode::Custom) {
        return static_cast<std::uint32_t>(
            desto::presentation::ResolveSortedAdaptiveCardColumns(
                preservedColumns, settings));
    }
    return static_cast<std::uint32_t>(
        desto::presentation::ResolveCustomAdaptiveCardColumns(
            preservedColumns, card.content().itemSize, settings));
}

std::optional<std::uint32_t> CardMaximumRows(const ApplicationCard& card) {
    return card.content().sizeMode == CardSizeMode::Fixed
        ? std::optional<std::uint32_t>(card.content().fixedRows)
        : std::nullopt;
}

std::uint32_t MappingCardColumns(const MappingCard& card) {
    if (card.content().sizeMode == CardSizeMode::Fixed) {
        return static_cast<std::uint32_t>(desto::domain::ProjectCardColumns(
            card.content().widthSpan, card.content().itemSize));
    }
    const auto settings = desto::presentation::ResolveCardContentLayoutSettings(card.content());
    std::size_t preservedColumns = 0;
    for (const auto& placement : card.itemPlacements()) {
        preservedColumns = (std::max)(
            preservedColumns, static_cast<std::size_t>(placement.column) + 1);
    }
    return static_cast<std::uint32_t>(card.sortMode() == ApplicationItemSortMode::Custom
        ? desto::presentation::ResolveCustomAdaptiveCardColumns(
            preservedColumns, card.content().itemSize, settings)
        : desto::presentation::ResolveSortedAdaptiveCardColumns(
            preservedColumns, settings));
}

std::optional<std::uint32_t> MappingCardMaximumRows(const MappingCard& card) {
    return card.content().sizeMode == CardSizeMode::Fixed
        ? std::optional<std::uint32_t>(card.content().fixedRows)
        : std::nullopt;
}

ShellIconSourceSize CardShellIconSource(const ApplicationCard& card) noexcept {
    return ResolveShellIconSourceSize(card.content().itemSize);
}

class FileTransactionRollbackGuard final {
public:
    FileTransactionRollbackGuard(
        std::span<const FileMove> moves,
        FileDropOperation operation,
        DiagnosticRecorder& diagnostics)
        : moves_(moves.begin(), moves.end()),
          operation_(operation),
          diagnostics_(diagnostics) {
    }

    ~FileTransactionRollbackGuard() noexcept {
        (void)rollbackNow();
    }

    [[nodiscard]] bool rollbackNow() noexcept {
        if (!active_) return true;
        active_ = false;
        if (moves_.empty()) return true;
        try {
            const auto succeeded = operation_ == FileDropOperation::Copy
                ? FileCopyTransaction::rollback(moves_).succeeded
                : FileMoveTransaction::rollback(moves_).succeeded;
            if (!succeeded) {
                diagnostics_.record(
                    DiagnosticLevel::Error, "desktop.import_rollback_failed");
            }
            return succeeded;
        } catch (...) {
            try {
                diagnostics_.record(
                    DiagnosticLevel::Error, "desktop.import_rollback_failed");
            } catch (...) {
            }
            return false;
        }
    }

    void release() noexcept { active_ = false; }

private:
    std::vector<FileMove> moves_;
    FileDropOperation operation_;
    DiagnosticRecorder& diagnostics_;
    bool active_ = true;
};

void SaveRuntimeConfiguration(
    const JsonConfigStore& store,
    const StorageRoot& storageRoot,
    const ApplicationRuntime& runtime,
    const ApplicationPreferences* preferences = nullptr) {
    auto config = store.load();
    config.schemaVersion = ApplicationConfig::CurrentSchemaVersion;
    config.storageRoot = storageRoot.path();
    if (preferences != nullptr) config.preferences = *preferences;
    config.cards = runtime.cardSnapshots();
    config.workspace = runtime.workspace();
    TodoDataStore(store.path().parent_path()).save(config);
    for (auto& card : config.cards) {
        if (card.type == CardType::Todo) card.todoItems.clear();
    }
    store.save(config);
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto launchOptions = ReadLaunchOptions();
    DiagnosticRecorder diagnostics(DiagnosticLevel::Info, 64);
    WindowsSingleInstanceGate instance(L"Local\\Desto.DesktopHost");
    ApplicationLifecycle lifecycle(instance, diagnostics);
    try {
        const auto begin = lifecycle.begin();
        if (!begin.applied) {
            return begin.error == LifecycleError::AlreadyRunning ? 0 : 1;
        }
        if (!lifecycle.configurationLoaded().applied) {
            throw std::runtime_error("Configuration lifecycle transition failed.");
        }
        WindowsDisplayTopology topology;
        const auto displays = topology.snapshot();
        ApplicationRuntime runtime;
        const auto defaultStorageRoot = DefaultStorageRoot();
        const auto configPath = defaultStorageRoot.parent_path() / "settings.json";
        JsonConfigStore configStore(configPath);
        TodoDataStore todoDataStore(configPath.parent_path());
        const auto configInspection = configStore.inspect();
        const auto hasSavedConfiguration = configInspection.primary.state != ConfigFileState::Missing
            || configInspection.backup1.state != ConfigFileState::Missing
            || configInspection.backup2.state != ConfigFileState::Missing;
        std::optional<ApplicationConfig> restoredConfiguration;
        if (configInspection.primaryUsable()) {
            restoredConfiguration = configStore.load(ConfigSource::Primary);
        } else if (configInspection.hasUsableBackup()) {
            const auto source = configInspection.backup1.state == ConfigFileState::Valid
                ? ConfigSource::Backup1 : ConfigSource::Backup2;
            const auto sourceName = source == ConfigSource::Backup1 ? L"settings.json.bak1" : L"settings.json.bak2";
            const auto accepted = ShowWindowsDialog(
                nullptr, desto::ui::RecoveryView::backupAvailable(sourceName));
            if (!accepted) return 1;
            configStore.promoteBackup(source);
            restoredConfiguration = configStore.load(ConfigSource::Primary);
        } else if (hasSavedConfiguration) {
            const auto accepted = ShowWindowsDialog(
                nullptr, desto::ui::RecoveryView::noUsableConfiguration());
            if (!accepted) return 1;
        }
        if (restoredConfiguration.has_value()) {
            todoDataStore.loadInto(*restoredConfiguration);
        }
        ApplicationPreferences applicationPreferences = restoredConfiguration.has_value()
            ? restoredConfiguration->preferences : ApplicationPreferences{};
        if (applicationPreferences.taskbarDoubleClickAction != "none") {
            applicationPreferences.taskbarDoubleClickAction = "current-display";
        }
        WindowsStartupIntegration startupIntegration(CurrentExecutablePath());
        const auto startupReconciliation = startupIntegration.ensure(
            applicationPreferences.runAtStartup);
        if (!startupReconciliation.succeeded) {
            diagnostics.record(DiagnosticLevel::Warning,
                "startup.registration_reconcile_failed."
                    + std::to_string(startupReconciliation.error));
        } else if (startupReconciliation.fallbackUsed) {
            diagnostics.record(
                DiagnosticLevel::Warning, "startup.zero_delay_task_unavailable.run_fallback");
        }
        if (launchOptions.startedAutomatically) {
            diagnostics.record(DiagnosticLevel::Info, "startup.automatic_launch");
        }
        auto uiLanguage = ResolveUiLanguage(applicationPreferences.language);
        StorageRoot storageRoot(
            restoredConfiguration.has_value() && !restoredConfiguration->storageRoot.empty()
                ? restoredConfiguration->storageRoot : defaultStorageRoot);
        storageRoot.ensureExists();
        if (restoredConfiguration.has_value()) {
            runtime.restore(restoredConfiguration->cards, restoredConfiguration->workspace);
            for (const auto* card : runtime.cards()) {
                auto appearance = card->appearance();
                appearance.cornerRadius = applicationPreferences.globalCardCornerRadius;
                (void)runtime.execute(SetCardAppearancePreferences{card->id(), appearance});
            }
            if (runtime.execute(UpdateDisplayTopology{displays}).status
                == CommandStatus::Rejected) {
                throw std::runtime_error("Unable to restore display topology.");
            }
        } else {
            if (runtime.execute(UpdateDisplayTopology{displays}).status
                == CommandStatus::Rejected) {
                throw std::runtime_error("Unable to initialize display topology.");
            }
            SaveRuntimeConfiguration(
                configStore, storageRoot, runtime, &applicationPreferences);
        }
        const auto normalizeCardOrder = [&] {
            const auto cards = runtime.cards();
            std::unordered_set<CardId> existing;
            for (const auto* card : cards) existing.insert(card->id());
            std::vector<CardId> normalized;
            normalized.reserve(cards.size());
            std::unordered_set<CardId> seen;
            for (const auto& id : applicationPreferences.cardOrder) {
                if (existing.contains(id) && seen.insert(id).second) normalized.push_back(id);
            }
            for (const auto* card : cards) {
                if (seen.insert(card->id()).second) normalized.push_back(card->id());
            }
            applicationPreferences.cardOrder = std::move(normalized);
        };
        normalizeCardOrder();
        const auto orderedRuntimeCards = [&]() {
            auto cards = runtime.cards();
            std::unordered_map<CardId, std::size_t> rank;
            rank.reserve(applicationPreferences.cardOrder.size());
            for (std::size_t index = 0; index < applicationPreferences.cardOrder.size(); ++index) {
                rank.emplace(applicationPreferences.cardOrder[index], index);
            }
            std::ranges::stable_sort(cards, [&](const auto* left, const auto* right) {
                const auto leftRank = rank.contains(left->id())
                    ? rank.at(left->id()) : applicationPreferences.cardOrder.size();
                const auto rightRank = rank.contains(right->id())
                    ? rank.at(right->id()) : applicationPreferences.cardOrder.size();
                return leftRank == rightRank ? left->id() < right->id() : leftRank < rightRank;
            });
            return cards;
        };
        ApplicationCardImportService importService(storageRoot);
        WindowsShellItemCatalog shellItems;
        std::vector<desto::presentation::CardView> cardViews;
        std::unordered_map<
            CardId,
            std::vector<desto::presentation::CardItemView>> cardItemsById;
        std::unordered_map<CardId, MappingNavigationState> mappingNavigation;
        const auto makeCardView = [&](const Card& card) {
                if (card.type() == CardType::Application) {
                    const auto* applicationCard = static_cast<const ApplicationCard*>(&card);
                    const auto directory = storageRoot.resolveCardPath(
                        applicationCard->relativeStoragePath());
                    std::filesystem::create_directories(directory);
                    auto items = shellItems.enumerate(
                        directory,
                        PlacementOrder(*applicationCard),
                        CardShellIconSource(*applicationCard));
                    const auto reconciled = ReconcileApplicationItemPlacements(
                        applicationCard->itemPlacements(),
                        ItemFileNames(items),
                        CardColumns(runtime, *applicationCard, items.size()),
                        CardMaximumRows(*applicationCard));
                    if (!reconciled.fits) {
                        throw std::runtime_error(
                            "Application Card fixed grid is smaller than its content.");
                    }
                    if (reconciled.placements != applicationCard->itemPlacements()) {
                        (void)runtime.execute(SetApplicationCardLayout{
                            applicationCard->id(), applicationCard->sortMode(),
                            reconciled.placements,
                        });
                    }
                    auto view = desto::presentation::MakeCardView(*applicationCard, uiLanguage);
                    cardItemsById[applicationCard->id()] = items;
                    view.items = std::move(items);
                    return view;
                }
                if (card.type() == CardType::Mapping) {
                    const auto* mappingCard = static_cast<const MappingCard*>(&card);
                    auto items = MappingItems(
                        *mappingCard, shellItems, mappingCard->content().itemSize);
                    auto reconciled = ReconcileApplicationItemPlacements(
                        mappingCard->itemPlacements(),
                        ItemSourcePaths(items),
                        MappingCardColumns(*mappingCard),
                        MappingCardMaximumRows(*mappingCard));
                    if (!reconciled.fits) {
                        throw std::runtime_error(
                            "Mapping Card fixed grid is smaller than its content.");
                    }
                    if (reconciled.placements != mappingCard->itemPlacements()) {
                        (void)runtime.execute(SetMappingCardLayout{
                            mappingCard->id(), mappingCard->sortMode(), reconciled.placements});
                    }
                    auto view = desto::presentation::MakeCardView(*mappingCard, uiLanguage);
                    cardItemsById[mappingCard->id()] = items;
                    view.items = std::move(items);
                    return view;
                }
                return desto::presentation::MakeCardView(card, uiLanguage);
        };
        const auto rebuildCardViews = [&] {
            cardViews.clear();
            cardItemsById.clear();
            for (const auto* card : orderedRuntimeCards()) {
                cardViews.push_back(makeCardView(*card));
            }
        };
        rebuildCardViews();
        double globalCardCornerRadius = applicationPreferences.globalCardCornerRadius;
        WindowsDesktopHost host;
        host.setTimeZoneOffsetMinutes(applicationPreferences.timeZoneOffsetMinutes);
        host.setLanguage(uiLanguage);
        host.setPinnedCardsYieldToFullscreen(
            applicationPreferences.pinnedCardsYieldToFullscreen);
        host.setIconBackgroundFrameVisible(
            applicationPreferences.showIconBackgroundFrame);
        const auto insertDesktopCard = [&](const CardId& cardId)
            -> std::optional<desto::presentation::CardView> {
            const auto* card = runtime.findCard(cardId);
            if (card == nullptr) return std::nullopt;
            auto view = makeCardView(*card);
            std::vector<PlacementProjection> cardProjections;
            for (const auto& projection : runtime.projections()) {
                if (projection.cardId == cardId) cardProjections.push_back(projection);
            }
            try {
                host.insertCard(cardProjections, view);
                cardViews.push_back(view);
                return view;
            } catch (...) {
                cardItemsById.erase(cardId);
                diagnostics.record(
                    DiagnosticLevel::Error, "desktop.incremental_card_insert_failed");
                return std::nullopt;
            }
        };
        WindowsSettingsHost settingsHost;
        host.setOverlayWindow(settingsHost.nativeHandle());
        WindowsTrayHost tray;
        tray.setLanguage(uiLanguage);
        tray.setOpenSettingsCallback([&] { settingsHost.show(); });
        tray.setExitCallback([&] { host.requestClose(); });
        WindowsDesktopTriggerHost desktopTrigger;
        WindowsTaskbarWindowToggle taskbarWindowToggle;
        std::unique_ptr<WindowsDirectoryChangeSource> directoryChanges;
        std::mutex pendingMappingChangesMutex;
        std::unordered_map<CardId, DirectoryMappingChange> pendingMappingChanges;
        const auto setDirectoryMonitoringActive = [&](bool active) {
            if (directoryChanges == nullptr) return;
            if (active) {
                if (!directoryChanges->running()) {
                    directoryChanges->start();
                    for (const auto* card : runtime.cards()) {
                        if (card->type() != CardType::Mapping) continue;
                        const auto* mapping = static_cast<const MappingCard*>(card);
                        if (mapping->mode() != MappingMode::Folder) continue;
                        {
                            std::lock_guard lock(pendingMappingChangesMutex);
                            pendingMappingChanges[mapping->id()] = {
                                .cardId = mapping->id(),
                                .requiresFullRefresh = true,
                            };
                        }
                        host.requestCardItemsRefresh(mapping->id());
                    }
                }
            } else {
                directoryChanges->stop();
                shellItems.clearCache();
            }
        };
        const auto toggleDesktopItems = [&] {
            try {
                const auto visible = !host.cardsVisible();
                desktopTrigger.setDesktopIconsVisible(visible);
                host.setCardsVisible(visible);
                tray.setDesktopVisible(visible);
                setDirectoryMonitoringActive(visible);
            } catch (...) {
                diagnostics.record(
                    DiagnosticLevel::Warning, "desktop.visibility_toggle_failed");
            }
        };
        tray.setToggleDesktopCallback(toggleDesktopItems);
        desktopTrigger.setDoubleClickCallback([&] {
            try {
                const auto& action = applicationPreferences.desktopDoubleClickAction;
                if (action == "none") return;
                if (action == "icons") {
                    desktopTrigger.setDesktopIconsVisible(
                        !desktopTrigger.desktopIconsVisible());
                    return;
                }
                if (action == "cards") {
                    const auto visible = !host.cardsVisible();
                    host.setCardsVisible(visible);
                    tray.setDesktopVisible(visible);
                    setDirectoryMonitoringActive(visible);
                    return;
                }
                toggleDesktopItems();
            } catch (...) {
                diagnostics.record(
                    DiagnosticLevel::Warning, "desktop.double_click_action_failed");
            }
        });
        desktopTrigger.setTaskbarDoubleClickCallback([&](int screenX, int screenY) {
            try {
                if (applicationPreferences.taskbarDoubleClickAction == "none") return;
                taskbarWindowToggle.showDesktop(
                    screenX,
                    screenY,
                    applicationPreferences.taskbarDoubleClickAction == "current-display");
            } catch (...) {
                diagnostics.record(
                    DiagnosticLevel::Warning, "taskbar.double_click_action_failed");
            }
        });
        const auto syncCardView = [&](
                                      const CardId& cardId,
                                      const std::vector<desto::presentation::CardItemView>& items) {
            const auto* card = runtime.findCard(cardId);
            if (card == nullptr) return;
            auto view = desto::presentation::MakeCardView(*card, uiLanguage);
            view.items = items;
            const auto cached = std::ranges::find(
                cardViews, cardId, &desto::presentation::CardView::id);
            if (cached != cardViews.end()) *cached = view;
            settingsHost.updateCard(std::move(view));
        };
        const auto restartMappingWatches = [&] {
            std::vector<DirectoryMappingWatch> watches;
            for (const auto* card : runtime.cards()) {
                if (card->type() != CardType::Mapping) continue;
                const auto* mapping = static_cast<const MappingCard*>(card);
                if (mapping->mode() == MappingMode::Folder
                    && !mapping->sourceRoot().empty()) {
                    watches.push_back({mapping->id(), mapping->sourceRoot()});
                }
            }
            if (directoryChanges == nullptr) {
                directoryChanges = std::make_unique<WindowsDirectoryChangeSource>(
                    std::move(watches),
                    [&](std::vector<DirectoryMappingChange> changes) {
                        for (auto& change : changes) {
                            const auto cardId = change.cardId;
                            {
                                std::lock_guard lock(pendingMappingChangesMutex);
                                auto [entry, inserted] = pendingMappingChanges.try_emplace(
                                    cardId,
                                    DirectoryMappingChange{.cardId = cardId});
                                (void)inserted;
                                auto& pending = entry->second;
                                pending.requiresFullRefresh = pending.requiresFullRefresh
                                    || change.requiresFullRefresh;
                                if (pending.requiresFullRefresh) {
                                    pending.relativePaths.clear();
                                } else {
                                    for (auto& path : change.relativePaths) {
                                        const auto duplicate = std::ranges::any_of(
                                            pending.relativePaths,
                                            [&](const auto& existing) {
                                                return PathKey(existing) == PathKey(path);
                                            });
                                        if (!duplicate) {
                                            pending.relativePaths.push_back(std::move(path));
                                        }
                                    }
                                }
                            }
                            host.requestCardItemsRefresh(cardId);
                        }
                    });
                directoryChanges->start();
            } else {
                directoryChanges->replaceWatches(std::move(watches));
            }
        };
        settingsHost.setAppearanceChangedCallback(
            [&](const CardId& cardId, const CardAppearancePreferences& preferences) {
                const auto* card = runtime.findCard(cardId);
                if (card == nullptr) return false;
                const auto previous = card->appearance();
                const auto result = runtime.execute(SetCardAppearancePreferences{cardId, preferences});
                if (result.status == CommandStatus::Rejected) return false;
                try {
                    host.updateCardAppearancePreferences(cardId, preferences);
                    SaveRuntimeConfiguration(configStore, storageRoot, runtime);
                    return true;
                } catch (...) {
                    (void)runtime.execute(SetCardAppearancePreferences{cardId, previous});
                    return false;
                }
            });
        settingsHost.setContentChangedCallback(
            [&](const CardId& cardId, const CardContentPreferences& preferences) {
                const auto* card = runtime.findCard(cardId);
                if (card == nullptr) return false;
                const auto previous = card->content();
                const auto* applicationCard = card->type() == CardType::Application
                    ? static_cast<const ApplicationCard*>(card) : nullptr;
                const auto* mappingCard = card->type() == CardType::Mapping
                    ? static_cast<const MappingCard*>(card) : nullptr;
                const auto isFileCard = applicationCard != nullptr || mappingCard != nullptr;
                const auto currentSortMode = applicationCard != nullptr
                    ? applicationCard->sortMode() : mappingCard != nullptr
                    ? mappingCard->sortMode() : ApplicationItemSortMode::Custom;
                const auto* currentPlacements = applicationCard != nullptr
                    ? &applicationCard->itemPlacements()
                    : mappingCard != nullptr ? &mappingCard->itemPlacements() : nullptr;
                std::optional<std::vector<ApplicationItemPlacement>> adjustedPlacements;
                std::optional<std::vector<ApplicationItemPlacement>> previousPlacements;
                bool layoutChanged = false;
                if (isFileCard
                    && currentSortMode == ApplicationItemSortMode::Custom
                    && card->content().itemSize != preferences.itemSize) {
                    const auto items = cardItemsById.find(cardId);
                    if (items != cardItemsById.end() && !items->second.empty()) {
                        const auto columns = static_cast<std::uint32_t>(
                            desto::domain::ProjectCardColumns(
                                preferences.widthSpan, preferences.itemSize));
                        const auto maximumRows = preferences.sizeMode == CardSizeMode::Fixed
                            ? std::optional<std::uint32_t>(preferences.fixedRows)
                            : std::nullopt;
                        const auto actualPaths = card->type() == CardType::Application
                            ? ItemFileNames(items->second)
                            : ItemSourcePaths(items->second);
                        const auto reconciled = ReflowApplicationItemPlacementsForGrid(
                            *currentPlacements, actualPaths, columns, maximumRows);
                        if (!reconciled.fits) return false;
                        if (reconciled.placements != *currentPlacements) {
                            previousPlacements = *currentPlacements;
                            const auto layoutResult = card->type() == CardType::Application
                                ? runtime.execute(SetApplicationCardLayout{
                                    cardId, currentSortMode, reconciled.placements})
                                : runtime.execute(SetMappingCardLayout{
                                    cardId, currentSortMode, reconciled.placements});
                            if (layoutResult.status == CommandStatus::Rejected) return false;
                            adjustedPlacements = reconciled.placements;
                            layoutChanged = true;
                        }
                    }
                }
                const auto result = runtime.execute(SetCardContentPreferences{cardId, preferences});
                if (result.status == CommandStatus::Rejected) {
                    if (layoutChanged && previousPlacements.has_value()) {
                        if (card->type() == CardType::Application) {
                            (void)runtime.execute(SetApplicationCardLayout{
                                cardId, currentSortMode, *previousPlacements});
                        } else {
                            (void)runtime.execute(SetMappingCardLayout{
                                cardId, currentSortMode, *previousPlacements});
                        }
                    }
                    return false;
                }
                try {
                    host.updateCardContentPreferences(
                        cardId, preferences, std::move(adjustedPlacements));
                    const auto cached = std::ranges::find(
                        cardViews, cardId, &desto::presentation::CardView::id);
                    if (cached != cardViews.end()) {
                        cached->content = preferences;
                        if (layoutChanged && previousPlacements.has_value()) {
                            cached->applicationItemPlacements =
                                *currentPlacements;
                        }
                        if (const auto items = cardItemsById.find(cardId);
                            items != cardItemsById.end()) {
                            cached->items = items->second;
                        }
                    }
                    SaveRuntimeConfiguration(configStore, storageRoot, runtime);
                    return true;
                } catch (...) {
                    (void)runtime.execute(SetCardContentPreferences{cardId, previous});
                    if (layoutChanged && previousPlacements.has_value()) {
                        if (card->type() == CardType::Application) {
                            (void)runtime.execute(SetApplicationCardLayout{
                                cardId, currentSortMode, *previousPlacements});
                        } else {
                            (void)runtime.execute(SetMappingCardLayout{
                                cardId, currentSortMode, *previousPlacements});
                        }
                    }
                    return false;
                }
            });
        settingsHost.setApplicationSortChangedCallback(
            [&](const CardId& cardId, ApplicationItemSortMode sortMode) {
                const auto* card = FindApplicationCard(runtime, cardId);
                if (card == nullptr) return false;
                const auto previousSortMode = card->sortMode();
                const auto placements = card->itemPlacements();
                if (previousSortMode == sortMode) return true;
                if (runtime.execute(SetApplicationCardLayout{
                        cardId, sortMode, placements}).status == CommandStatus::Rejected) {
                    return false;
                }
                const auto items = cardItemsById.contains(cardId)
                    ? cardItemsById.at(cardId)
                    : std::vector<desto::presentation::CardItemView>{};
                try {
                    host.updateCardItemsBatch({{
                        cardId, items, sortMode, placements,
                    }});
                    const auto cached = std::ranges::find(
                        cardViews, cardId, &desto::presentation::CardView::id);
                    if (cached != cardViews.end()) {
                        cached->applicationSortMode = sortMode;
                    }
                    SaveRuntimeConfiguration(configStore, storageRoot, runtime);
                    return true;
                } catch (...) {
                    (void)runtime.execute(SetApplicationCardLayout{
                        cardId, previousSortMode, placements});
                    try {
                        host.updateCardItemsBatch({{
                            cardId, items, previousSortMode, placements,
                        }});
                    } catch (...) {
                    }
                    return false;
                }
            });
        settingsHost.setMappingSortChangedCallback(
            [&](const CardId& cardId, ApplicationItemSortMode sortMode) {
                const auto* mapping = FindMappingCard(runtime, cardId);
                if (mapping == nullptr) return false;
                const auto previous = mapping->sortMode();
                const auto placements = mapping->itemPlacements();
                if (previous == sortMode) return true;
                if (runtime.execute(SetMappingCardLayout{cardId, sortMode, placements}).status
                    == CommandStatus::Rejected) {
                    return false;
                }
                try {
                    const auto items = cardItemsById.contains(cardId)
                        ? cardItemsById.at(cardId)
                        : std::vector<desto::presentation::CardItemView>{};
                    host.updateMappingCard(
                        cardId, mapping->mode(), mapping->allowsSourceMutation(), items,
                        sortMode, placements, MappingHasSource(*mapping));
                    const auto cached = std::ranges::find(
                        cardViews, cardId, &desto::presentation::CardView::id);
                    if (cached != cardViews.end()) {
                        cached->applicationSortMode = sortMode;
                        cached->mappingSortMode = sortMode;
                    }
                    SaveRuntimeConfiguration(configStore, storageRoot, runtime);
                    return true;
                } catch (...) {
                    (void)runtime.execute(SetMappingCardLayout{
                        cardId, previous, placements});
                    return false;
                }
            });
        settingsHost.setMappingModeChangedCallback(
            [&](const CardId& cardId, MappingMode mode) {
                const auto* mapping = FindMappingCard(runtime, cardId);
                if (mapping == nullptr) return false;
                const auto previousMode = mapping->mode();
                const auto previousRoot = mapping->sourceRoot();
                const auto previousReferences = mapping->references();
                if (runtime.execute(SetMappingMode{cardId, mode}).status
                    == CommandStatus::Rejected) return false;
                try {
                    mappingNavigation.erase(cardId);
                    cardItemsById[cardId].clear();
                    host.updateMappingCard(
                        cardId, mode, mapping->allowsSourceMutation(), {},
                        mapping->sortMode(), {}, MappingHasSource(*mapping));
                    syncCardView(cardId, {});
                    restartMappingWatches();
                    SaveRuntimeConfiguration(configStore, storageRoot, runtime);
                    return true;
                } catch (...) {
                    if (previousMode == MappingMode::Folder && !previousRoot.empty()) {
                        (void)runtime.execute(SetMappingFolderSource{cardId, previousRoot});
                    } else if (previousMode == MappingMode::References
                               && !previousReferences.empty()) {
                        (void)runtime.execute(SetMappingReferences{cardId, previousReferences});
                    } else {
                        (void)runtime.execute(SetMappingMode{cardId, previousMode});
                    }
                    return false;
                }
            });
        settingsHost.setChromeChangedCallback(
            [&](const CardId& cardId, const CardChromePreferences& preferences) {
                const auto* card = runtime.findCard(cardId);
                if (card == nullptr) return false;
                const auto previous = card->chrome();
                const auto previousExpanded = card->isExpanded();
                const auto result = runtime.execute(SetCardChromePreferences{cardId, preferences});
                if (result.status == CommandStatus::Rejected) return false;
                try {
                    if (!preferences.showCollapseControl && !previousExpanded) {
                        const auto expanded = runtime.execute(SetCardExpanded{cardId, true});
                        if (expanded.status == CommandStatus::Rejected) {
                            (void)runtime.execute(SetCardChromePreferences{cardId, previous});
                            return false;
                        }
                    }
                    host.updateCardChromePreferences(cardId, preferences);
                    SaveRuntimeConfiguration(configStore, storageRoot, runtime);
                    return true;
                } catch (...) {
                    (void)runtime.execute(SetCardChromePreferences{cardId, previous});
                    if (!previousExpanded) (void)runtime.execute(SetCardExpanded{cardId, false});
                    return false;
                }
            });
        settingsHost.setTodoPreferencesChangedCallback(
            [&](const CardId& cardId, const TodoCardPreferences& preferences) {
                const auto* card = runtime.findCard(cardId);
                if (card == nullptr || card->type() != CardType::Todo) return false;
                const auto previous = static_cast<const TodoCard*>(card)->preferences();
                const auto result = runtime.execute(SetTodoCardPreferences{cardId, preferences});
                if (result.status == CommandStatus::Rejected) return false;
                try {
                    host.updateTodoPreferences(cardId, preferences);
                    SaveRuntimeConfiguration(configStore, storageRoot, runtime);
                    return true;
                } catch (...) {
                    (void)runtime.execute(SetTodoCardPreferences{cardId, previous});
                    return false;
                }
            });
        settingsHost.setRestoreArchivedCallback([&](const CardId& cardId) {
            const auto result = runtime.execute(RestoreArchivedTodoItems{cardId});
            if (result.status == CommandStatus::Rejected) return false;
            const auto* card = runtime.findCard(cardId);
            if (card == nullptr || card->type() != CardType::Todo) return false;
            host.updateTodoItems(cardId, static_cast<const TodoCard*>(card)->items());
            syncCardView(cardId, {});
            SaveRuntimeConfiguration(configStore, storageRoot, runtime);
            return true;
        });
        settingsHost.setArchiveTodoItemCallback(
            [&](const CardId& cardId, const std::string& itemId) {
                const auto result = runtime.execute(ArchiveTodoItem{cardId, itemId});
                if (result.status == CommandStatus::Rejected) return false;
                const auto* card = runtime.findCard(cardId);
                if (card == nullptr || card->type() != CardType::Todo) return false;
                host.updateTodoItems(cardId, static_cast<const TodoCard*>(card)->items());
                syncCardView(cardId, {});
                SaveRuntimeConfiguration(configStore, storageRoot, runtime);
                return true;
            });
        settingsHost.setRestoreArchivedItemCallback(
            [&](const CardId& cardId, const std::string& itemId) {
                const auto result = runtime.execute(RestoreArchivedTodoItem{cardId, itemId});
                if (result.status == CommandStatus::Rejected) return false;
                const auto* card = runtime.findCard(cardId);
                if (card == nullptr || card->type() != CardType::Todo) return false;
                host.updateTodoItems(cardId, static_cast<const TodoCard*>(card)->items());
                syncCardView(cardId, {});
                SaveRuntimeConfiguration(configStore, storageRoot, runtime);
                return true;
            });
        settingsHost.setDeleteArchivedItemCallback(
            [&](const CardId& cardId, const std::string& itemId) {
                const auto* card = runtime.findCard(cardId);
                if (card == nullptr || card->type() != CardType::Todo) return false;
                const auto result = runtime.execute(RemoveTodoItem{cardId, itemId});
                if (result.status != CommandStatus::Applied) return false;
                const auto* updated = runtime.findCard(cardId);
                if (updated == nullptr || updated->type() != CardType::Todo) return false;
                host.updateTodoItems(cardId, static_cast<const TodoCard*>(updated)->items());
                syncCardView(cardId, {});
                SaveRuntimeConfiguration(configStore, storageRoot, runtime);
                return true;
            });
        std::uint64_t nextHistoricalArchiveSequence = 1;
        settingsHost.setHistoricalArchiveAddedCallback(
            [&](const CardId& cardId, const std::wstring& title, TodoDate date)
                -> std::optional<TodoItem> {
                const auto* card = runtime.findCard(cardId);
                if (card == nullptr || card->type() != CardType::Todo) return std::nullopt;
                const auto utf8Title = WideToUtf8(title);
                if (utf8Title.empty()) return std::nullopt;
                const auto* todo = static_cast<const TodoCard*>(card);
                std::string itemId;
                do {
                    itemId = cardId + "-history-"
                        + std::to_string(nextHistoricalArchiveSequence++);
                } while (std::ranges::any_of(todo->items(), [&](const TodoItem& item) {
                    return item.id == itemId;
                }));
                const auto timestamp = TodoDateNoonUnixMilliseconds(
                    date, applicationPreferences.timeZoneOffsetMinutes);
                const auto result = runtime.execute(AddHistoricalArchivedTodoItem{
                    cardId, itemId, utf8Title, timestamp, date});
                if (result.status != CommandStatus::Applied) return std::nullopt;
                const auto* updated = static_cast<const TodoCard*>(runtime.findCard(cardId));
                host.updateTodoItems(cardId, updated->items());
                auto view = desto::presentation::MakeCardView(*updated, uiLanguage);
                const auto cached = std::ranges::find(
                    cardViews, cardId, &desto::presentation::CardView::id);
                if (cached != cardViews.end()) *cached = view;
                SaveRuntimeConfiguration(configStore, storageRoot, runtime);
                const auto item = std::ranges::find(updated->items(), itemId, &TodoItem::id);
                return item == updated->items().end()
                    ? std::nullopt : std::optional<TodoItem>(*item);
            });
        settingsHost.setArchiveExportCallback(
            [&](TodoDate begin, TodoDate end, const std::filesystem::path& destination) {
                try {
                    std::string text = "\xEF\xBB\xBF";
                    text += uiLanguage == "en-US"
                        ? "Desto archived tasks\r\n"
                        : "Desto 待办归档\r\n";
                    text += ToString(begin) + " - " + ToString(end) + "\r\n\r\n";
                    for (const auto* card : orderedRuntimeCards()) {
                        if (card->type() != CardType::Todo) continue;
                        const auto* todo = static_cast<const TodoCard*>(card);
                        bool wroteCard = false;
                        for (const auto& item : todo->items()) {
                            if (!IsTodoItemArchived(
                                    item,
                                    CurrentTodoDate(
                                        applicationPreferences.timeZoneOffsetMinutes),
                                    applicationPreferences.timeZoneOffsetMinutes)) {
                                continue;
                            }
                            const auto timestamp = item.completedAtUnixMilliseconds > 0
                                ? item.completedAtUnixMilliseconds
                                : item.createdAtUnixMilliseconds;
                            const auto date = TodoDateAtUnixMilliseconds(
                                timestamp, applicationPreferences.timeZoneOffsetMinutes);
                            if (CompareTodoDates(date, begin) < 0
                                || CompareTodoDates(date, end) > 0) continue;
                            if (!wroteCard) {
                                auto cardTitle = card->name();
                                if (cardTitle.empty()) {
                                    cardTitle = uiLanguage == "en-US"
                                        ? "Task card" : "待办卡片";
                                }
                                text += "[" + cardTitle + "]\r\n";
                                wroteCard = true;
                            }
                            text += ToString(date) + "  " + item.title + "\r\n";
                        }
                        if (wroteCard) text += "\r\n";
                    }
                    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
                    if (!output) throw std::runtime_error("Unable to open archive export file.");
                    output.write(text.data(), static_cast<std::streamsize>(text.size()));
                    if (!output) throw std::runtime_error("Unable to write archive export file.");
                    return true;
                } catch (...) {
                    diagnostics.record(DiagnosticLevel::Warning,
                        "settings.archive_export_failed");
                    return false;
                }
            });
        settingsHost.setCardRenamedCallback(
            [&](const CardId& cardId, const std::wstring& name) {
                const auto utf8Name = WideToUtf8(name);
                if (utf8Name.empty()) return false;
                const auto result = runtime.execute(RenameCard{cardId, utf8Name});
                if (result.status == CommandStatus::Rejected) return false;
                const auto updated = desto::presentation::MakeCardView(
                    *runtime.findCard(cardId), uiLanguage);
                for (auto& view : cardViews) {
                    if (view.id == cardId) {
                        view.title = updated.title;
                        view.typeLabel = updated.typeLabel;
                    }
                }
                host.updateCardTitles(std::span<const desto::presentation::CardView>(&updated, 1));
                SaveRuntimeConfiguration(configStore, storageRoot, runtime);
                return true;
            });
        settingsHost.setCardVisibilityChangedCallback(
            [&](const CardId& cardId, bool visible) {
                const auto* card = runtime.findCard(cardId);
                if (card == nullptr || card->isVisible() == visible) return card != nullptr;
                if (runtime.execute(SetCardVisibility{cardId, visible}).status
                    == CommandStatus::Rejected) return false;
                try {
                    auto view = makeCardView(*runtime.findCard(cardId));
                    if (visible) {
                        std::vector<PlacementProjection> projections;
                        for (const auto& projection : runtime.projections()) {
                            if (projection.cardId == cardId) projections.push_back(projection);
                        }
                        host.insertCard(projections, view);
                    } else {
                        host.removeCard(cardId);
                    }
                    const auto cached = std::ranges::find(
                        cardViews, cardId, &desto::presentation::CardView::id);
                    if (cached != cardViews.end()) *cached = std::move(view);
                    SaveRuntimeConfiguration(
                        configStore, storageRoot, runtime, &applicationPreferences);
                    return true;
                } catch (...) {
                    (void)runtime.execute(SetCardVisibility{cardId, !visible});
                    if (visible) host.removeCard(cardId);
                    return false;
                }
            });
        settingsHost.setCardOrderChangedCallback(
            [&](const std::vector<CardId>& order) {
                if (order.size() != cardViews.size()) return false;
                std::unordered_set<CardId> unique(order.begin(), order.end());
                if (unique.size() != order.size()) return false;
                for (const auto& view : cardViews) {
                    if (!unique.contains(view.id)) return false;
                }
                const auto previousOrder = applicationPreferences.cardOrder;
                const auto previousViews = cardViews;
                applicationPreferences.cardOrder = order;
                std::unordered_map<CardId, std::size_t> rank;
                for (std::size_t index = 0; index < order.size(); ++index) {
                    rank.emplace(order[index], index);
                }
                std::ranges::stable_sort(cardViews, [&](const auto& left, const auto& right) {
                    return rank.at(left.id) < rank.at(right.id);
                });
                try {
                    SaveRuntimeConfiguration(
                        configStore, storageRoot, runtime, &applicationPreferences);
                    return true;
                } catch (...) {
                    applicationPreferences.cardOrder = previousOrder;
                    cardViews = previousViews;
                    return false;
                }
            });
        settingsHost.setGlobalCornerRadiusChangedCallback([&](double radius, bool commit) {
            std::vector<std::pair<CardId, CardAppearancePreferences>> previous;
            previous.reserve(runtime.cards().size());
            for (const auto* card : runtime.cards()) {
                previous.emplace_back(card->id(), card->appearance());
                auto appearance = card->appearance();
                appearance.cornerRadius = radius;
                if (runtime.execute(SetCardAppearancePreferences{card->id(), appearance}).status
                    == CommandStatus::Rejected) {
                    for (const auto& [id, oldAppearance] : previous) {
                        (void)runtime.execute(SetCardAppearancePreferences{id, oldAppearance});
                    }
                    return false;
                }
            }
            for (const auto* card : runtime.cards()) {
                host.updateCardAppearancePreferences(card->id(), card->appearance());
            }
            for (auto& view : cardViews) view.cornerRadius = radius;
            globalCardCornerRadius = radius;
            applicationPreferences.globalCardCornerRadius = radius;
            if (commit) {
                SaveRuntimeConfiguration(
                    configStore, storageRoot, runtime, &applicationPreferences);
            }
            return true;
        });
        settingsHost.setTimeZoneChangedCallback(
            [&](std::optional<std::int32_t> offsetMinutes) {
                applicationPreferences.timeZoneOffsetMinutes = offsetMinutes;
                host.setTimeZoneOffsetMinutes(offsetMinutes);
                SaveRuntimeConfiguration(
                    configStore, storageRoot, runtime, &applicationPreferences);
                return true;
            });
        settingsHost.setLanguageChangedCallback([&](const std::string& language) {
            applicationPreferences.language = language;
            uiLanguage = ResolveUiLanguage(language);
            host.setLanguage(uiLanguage);
            tray.setLanguage(uiLanguage);
            for (auto& view : cardViews) {
                const auto* card = runtime.findCard(view.id);
                if (card == nullptr) continue;
                const auto localized = desto::presentation::MakeCardView(*card, uiLanguage);
                view.title = localized.title;
                view.typeLabel = localized.typeLabel;
            }
            host.updateCardTitles(cardViews);
            settingsHost.present(cardViews, {
                .timeZoneOffsetMinutes = applicationPreferences.timeZoneOffsetMinutes,
                .language = applicationPreferences.language,
                .storageRoot = storageRoot.path(),
                .globalCornerRadius = applicationPreferences.globalCardCornerRadius,
                .runAtStartup = applicationPreferences.runAtStartup,
                .desktopDoubleClickAction = applicationPreferences.desktopDoubleClickAction,
                .taskbarDoubleClickAction = applicationPreferences.taskbarDoubleClickAction,
                .pinnedCardsYieldToFullscreen =
                    applicationPreferences.pinnedCardsYieldToFullscreen,
                .showIconBackgroundFrame =
                    applicationPreferences.showIconBackgroundFrame,
                .confirmFileDeletion = applicationPreferences.confirmFileDeletion,
                .updateChannel = applicationPreferences.updateChannel,
            });
            SaveRuntimeConfiguration(
                configStore, storageRoot, runtime, &applicationPreferences);
            return true;
        });
        settingsHost.setRunAtStartupChangedCallback([&](bool enabled) {
            const auto previous = applicationPreferences.runAtStartup;
            const auto updated = startupIntegration.ensure(enabled);
            if (!updated.succeeded) {
                diagnostics.record(DiagnosticLevel::Warning,
                    "startup.registration_update_failed." + std::to_string(updated.error));
                return false;
            }
            if (updated.fallbackUsed) {
                diagnostics.record(
                    DiagnosticLevel::Warning, "startup.zero_delay_task_unavailable.run_fallback");
            }
            applicationPreferences.runAtStartup = enabled;
            try {
                SaveRuntimeConfiguration(
                    configStore, storageRoot, runtime, &applicationPreferences);
                return true;
            } catch (...) {
                applicationPreferences.runAtStartup = previous;
                const auto rollback = startupIntegration.ensure(previous);
                diagnostics.record(DiagnosticLevel::Error,
                    rollback.succeeded ? "startup.preference_save_failed"
                                       : "startup.preference_rollback_failed."
                                           + std::to_string(rollback.error));
                return false;
            }
        });
        settingsHost.setDesktopDoubleClickActionChangedCallback(
            [&](const std::string& action) {
                const auto previous = applicationPreferences.desktopDoubleClickAction;
                applicationPreferences.desktopDoubleClickAction = action;
                try {
                    SaveRuntimeConfiguration(
                        configStore, storageRoot, runtime, &applicationPreferences);
                    return true;
                } catch (...) {
                    applicationPreferences.desktopDoubleClickAction = previous;
                    diagnostics.record(DiagnosticLevel::Error,
                        "desktop.double_click_preference_save_failed");
                    return false;
                }
            });
        settingsHost.setTaskbarDoubleClickActionChangedCallback(
            [&](const std::string& action) {
                const auto previous = applicationPreferences.taskbarDoubleClickAction;
                applicationPreferences.taskbarDoubleClickAction = action;
                try {
                    SaveRuntimeConfiguration(
                        configStore, storageRoot, runtime, &applicationPreferences);
                    return true;
                } catch (...) {
                    applicationPreferences.taskbarDoubleClickAction = previous;
                    diagnostics.record(DiagnosticLevel::Error,
                        "taskbar.double_click_preference_save_failed");
                    return false;
                }
            });
        settingsHost.setPinnedCardsYieldToFullscreenChangedCallback([&](bool enabled) {
            const auto previous = applicationPreferences.pinnedCardsYieldToFullscreen;
            applicationPreferences.pinnedCardsYieldToFullscreen = enabled;
            host.setPinnedCardsYieldToFullscreen(enabled);
            try {
                SaveRuntimeConfiguration(
                    configStore, storageRoot, runtime, &applicationPreferences);
                return true;
            } catch (...) {
                applicationPreferences.pinnedCardsYieldToFullscreen = previous;
                host.setPinnedCardsYieldToFullscreen(previous);
                diagnostics.record(DiagnosticLevel::Error,
                    "desktop.fullscreen_pin_preference_save_failed");
                return false;
            }
        });
        settingsHost.setUpdateChannelChangedCallback([&](const std::string& channel) {
            if (channel != "stable" && channel != "development") return false;
            const auto previous = applicationPreferences.updateChannel;
            applicationPreferences.updateChannel = channel;
            try {
                SaveRuntimeConfiguration(
                    configStore, storageRoot, runtime, &applicationPreferences);
                return true;
            } catch (...) {
                applicationPreferences.updateChannel = previous;
                diagnostics.record(DiagnosticLevel::Error,
                    "update.channel_preference_save_failed");
                return false;
            }
        });
        settingsHost.setIconBackgroundFrameChangedCallback([&](bool enabled) {
            const auto previous = applicationPreferences.showIconBackgroundFrame;
            applicationPreferences.showIconBackgroundFrame = enabled;
            host.setIconBackgroundFrameVisible(enabled);
            try {
                SaveRuntimeConfiguration(
                    configStore, storageRoot, runtime, &applicationPreferences);
                return true;
            } catch (...) {
                applicationPreferences.showIconBackgroundFrame = previous;
                host.setIconBackgroundFrameVisible(previous);
                diagnostics.record(DiagnosticLevel::Error,
                    "desktop.icon_background_preference_save_failed");
                return false;
            }
        });
        settingsHost.setFileDeletionConfirmationChangedCallback([&](bool enabled) {
            const auto previous = applicationPreferences.confirmFileDeletion;
            applicationPreferences.confirmFileDeletion = enabled;
            try {
                SaveRuntimeConfiguration(
                    configStore, storageRoot, runtime, &applicationPreferences);
                return true;
            } catch (...) {
                applicationPreferences.confirmFileDeletion = previous;
                diagnostics.record(DiagnosticLevel::Error,
                    "desktop.file_delete_confirmation_preference_save_failed");
                return false;
            }
        });
        settingsHost.setStorageRootChangedCallback(
            [&](const std::filesystem::path& requestedRoot) {
                std::error_code comparisonError;
                if (std::filesystem::equivalent(
                        storageRoot.path(), requestedRoot, comparisonError)
                    && !comparisonError) {
                    return true;
                }
                StorageRootMigrationService migration;
                StorageRootMigrationResult result;
                try {
                    result = migration.migrate(storageRoot, requestedRoot, configStore);
                } catch (const std::exception& error) {
                    (void)ShowWindowsAlert(
                        static_cast<HWND>(settingsHost.nativeHandle()),
                        L"Desto data migration", Utf8ToWide(error.what()));
                    return false;
                }
                if (!result.succeeded) {
                    const auto message = result.failures.empty()
                        ? std::string{"Unable to migrate the Desto data directory."}
                        : result.failures.front();
                    (void)ShowWindowsAlert(
                        static_cast<HWND>(settingsHost.nativeHandle()),
                        L"Desto data migration", Utf8ToWide(message));
                    return false;
                }
                storageRoot = StorageRoot(requestedRoot);
                importService = ApplicationCardImportService(storageRoot);
                shellItems.clearCache();
                std::vector<WindowsDesktopHost::CardItemsUpdate> updates;
                for (const auto* card : runtime.cards()) {
                    if (card->type() != CardType::Application) continue;
                    const auto* applicationCard = static_cast<const ApplicationCard*>(card);
                    auto view = makeCardView(*applicationCard);
                    updates.push_back({
                        applicationCard->id(),
                        view.items,
                        applicationCard->sortMode(),
                        applicationCard->itemPlacements(),
                    });
                    const auto cached = std::ranges::find(
                        cardViews, applicationCard->id(),
                        &desto::presentation::CardView::id);
                    if (cached != cardViews.end()) *cached = view;
                    settingsHost.updateCard(std::move(view));
                }
                host.updateCardItemsBatch(std::move(updates));
                SaveRuntimeConfiguration(
                    configStore, storageRoot, runtime, &applicationPreferences);
                return true;
            });
        std::uint64_t nextCardSequence = 1;
        settingsHost.setCardAddedCallback(
            [&](CardType type) -> std::optional<desto::presentation::CardView> {
                const auto& currentDisplays = runtime.displays();
                if (currentDisplays.empty()) return std::nullopt;
                CardId cardId;
                do {
                    cardId = "card-" + std::to_string(GetTickCount64()) + "-"
                        + std::to_string(nextCardSequence++);
                } while (runtime.findCard(cardId) != nullptr);

                CommandResult created;
                if (type == CardType::Application) {
                    created = runtime.execute(CreateApplicationCard{
                        cardId, std::filesystem::path("cards") / cardId});
                } else if (type == CardType::Mapping) {
                    created = runtime.execute(CreateMappingCard{cardId});
                } else {
                    created = runtime.execute(CreateTodoCard{cardId});
                }
                if (created.status == CommandStatus::Rejected) return std::nullopt;

                if (type == CardType::Mapping) {
                    (void)runtime.execute(SetMappingSourceMutation{cardId, false});
                }
                if (const auto* createdCard = runtime.findCard(cardId); createdCard != nullptr) {
                    auto appearance = createdCard->appearance();
                    appearance.cornerRadius = globalCardCornerRadius;
                    (void)runtime.execute(SetCardAppearancePreferences{cardId, appearance});
                }
                const auto primary = std::ranges::find_if(currentDisplays, &DisplaySnapshot::primary);
                const auto& display = primary == currentDisplays.end()
                    ? currentDisplays.front() : *primary;
                const auto cascade = static_cast<double>((runtime.cards().size() - 1) % 7) * 24.0;
                const auto placement = runtime.execute(SetPlacement{{
                    .id = "placement-" + cardId,
                    .cardId = cardId,
                    .target = DisplayTarget::specific(display.id),
                    .rect = {48.0 + cascade, 48.0 + cascade, 320.0, 220.0},
                    .zIndex = static_cast<std::int32_t>(runtime.cards().size()),
                    .referenceWorkAreaWidth = display.workAreaWidth,
                    .referenceWorkAreaHeight = display.workAreaHeight,
                }});
                if (placement.status == CommandStatus::Rejected) {
                    const auto deletion = runtime.execute(RequestCardDeletion{cardId});
                    if (deletion.changes.deletionRequest.has_value()) {
                        (void)runtime.execute(CommitCardDeletion{
                            cardId, deletion.changes.deletionRequest->token});
                    }
                    return std::nullopt;
                }
                const auto view = insertDesktopCard(cardId);
                if (!view.has_value()) {
                    const auto deletion = runtime.execute(RequestCardDeletion{cardId});
                    if (deletion.changes.deletionRequest.has_value()) {
                        (void)runtime.execute(CommitCardDeletion{
                            cardId, deletion.changes.deletionRequest->token});
                    }
                    return std::nullopt;
                }
                applicationPreferences.cardOrder.push_back(cardId);
                SaveRuntimeConfiguration(
                    configStore, storageRoot, runtime, &applicationPreferences);
                return view;
            });
        settingsHost.setCardDeletedCallback([&](const CardId& cardId) {
            const auto* card = runtime.findCard(cardId);
            if (card == nullptr) return false;
            const auto deletion = runtime.execute(RequestCardDeletion{cardId});
            if (!deletion.changes.deletionRequest.has_value()) return false;
            const auto token = deletion.changes.deletionRequest->token;
            std::vector<FileMove> returnedMoves;
            if (card->type() == CardType::Application) {
                ApplicationCardReturnService returnService(storageRoot);
                const auto plan = returnService.plan(
                    *static_cast<const ApplicationCard*>(card), DesktopDirectory());
                const auto returned = returnService.execute(plan, DeletionConfirmation{cardId});
                if (!returned.succeeded) {
                    (void)runtime.execute(CancelCardDeletion{cardId, token});
                    return false;
                }
                returnedMoves = returned.completedMoves;
            }
            const auto committed = runtime.execute(CommitCardDeletion{cardId, token});
            if (committed.status == CommandStatus::Rejected) {
                if (!returnedMoves.empty()) (void)FileMoveTransaction::rollback(returnedMoves);
                return false;
            }
            cardItemsById.erase(cardId);
            mappingNavigation.erase(cardId);
            host.removeCard(cardId);
            cardViews.erase(
                std::remove_if(cardViews.begin(), cardViews.end(), [&](const auto& view) {
                    return view.id == cardId;
                }),
                cardViews.end());
            applicationPreferences.cardOrder.erase(
                std::remove(applicationPreferences.cardOrder.begin(),
                    applicationPreferences.cardOrder.end(), cardId),
                applicationPreferences.cardOrder.end());
            SaveRuntimeConfiguration(
                configStore, storageRoot, runtime, &applicationPreferences);
            return true;
        });
        host.setPlacementChangedCallback(
            [&](const PlacementId& placementId,
                const CardId& cardId,
                const DisplayId& displayId,
                const PlacementRect& rect,
                PlacementHorizontalAnchor horizontalAnchor,
                PlacementVerticalAnchor verticalAnchor,
                double referenceWorkAreaWidth,
                double referenceWorkAreaHeight) {
                const auto& placements = runtime.workspace().placements();
                const auto found = std::find_if(
                    placements.begin(), placements.end(), [&](const CardPlacement& placement) {
                        return placement.id == placementId && placement.cardId == cardId;
                    });
                if (found == placements.end()) {
                    diagnostics.record(DiagnosticLevel::Warning, "desktop.placement_missing");
                    return;
                }
                auto updated = *found;
                updated.target = DisplayTarget::specific(displayId);
                updated.rect = rect;
                updated.horizontalAnchor = horizontalAnchor;
                updated.verticalAnchor = verticalAnchor;
                updated.referenceWorkAreaWidth = referenceWorkAreaWidth;
                updated.referenceWorkAreaHeight = referenceWorkAreaHeight;
                if (runtime.execute(SetPlacement{std::move(updated)}).status
                    == CommandStatus::Rejected) {
                    diagnostics.record(DiagnosticLevel::Warning, "desktop.placement_rejected");
                } else {
                    SaveRuntimeConfiguration(configStore, storageRoot, runtime);
                }
            });
        host.setCardExpandedChangedCallback(
            [&](const CardId& cardId, bool expanded) {
                if (runtime.execute(SetCardExpanded{cardId, expanded}).status
                    == CommandStatus::Rejected) {
                    diagnostics.record(DiagnosticLevel::Warning, "desktop.expansion_rejected");
                } else {
                    SaveRuntimeConfiguration(configStore, storageRoot, runtime);
                }
            });
        host.setCardPinChangedCallback(
            [&](const CardId& cardId, bool pinned) {
                const auto* card = runtime.findCard(cardId);
                if (card == nullptr) return false;
                auto chrome = card->chrome();
                chrome.pinOnTop = pinned;
                if (runtime.execute(SetCardChromePreferences{cardId, chrome}).status
                    == CommandStatus::Rejected) {
                    return false;
                }
                for (auto& view : cardViews) {
                    if (view.id == cardId) view.pinOnTop = pinned;
                }
                SaveRuntimeConfiguration(configStore, storageRoot, runtime);
                return true;
            });
        host.setMappingPresentationChangedCallback(
            [&](const CardId& cardId, MappingPresentationMode mode) {
                const auto* card = runtime.findCard(cardId);
                if (card == nullptr) return false;
                const auto result = card->type() == CardType::Application
                    ? runtime.execute(SetApplicationPresentationMode{cardId, mode})
                    : runtime.execute(SetMappingPresentationMode{cardId, mode});
                if (result.status == CommandStatus::Rejected) return false;
                const auto items = cardItemsById.contains(cardId)
                    ? cardItemsById.at(cardId)
                    : std::vector<desto::presentation::CardItemView>{};
                syncCardView(cardId, items);
                SaveRuntimeConfiguration(configStore, storageRoot, runtime);
                return true;
            });
        std::uint64_t nextTodoItemSequence = 1;
        host.setTodoItemAddedScheduledCallback(
            [&](const CardId& cardId, const std::string& title, TodoDate scheduledDate)
                -> std::optional<TodoItem> {
                const auto* card = runtime.findCard(cardId);
                if (card == nullptr || card->type() != CardType::Todo) return std::nullopt;
                const auto* todo = static_cast<const TodoCard*>(card);
                std::string itemId;
                do {
                    itemId = cardId + "-item-" + std::to_string(nextTodoItemSequence++);
                } while (std::ranges::any_of(todo->items(), [&](const TodoItem& item) {
                    return item.id == itemId;
                }));
                const auto result = runtime.execute(
                    AddTodoItem{cardId, itemId, title, 0, scheduledDate});
                if (result.status != CommandStatus::Applied) return std::nullopt;
                syncCardView(cardId, {});
                SaveRuntimeConfiguration(configStore, storageRoot, runtime);
                const auto* updated = static_cast<const TodoCard*>(runtime.findCard(cardId));
                const auto item = std::ranges::find(updated->items(), itemId, &TodoItem::id);
                return item == updated->items().end() ? std::nullopt : std::optional<TodoItem>(*item);
            });
        host.setTodoItemCompletedChangedCallback(
            [&](const CardId& cardId, const std::string& itemId, bool completed) {
                const auto result = runtime.execute(
                    SetTodoItemCompleted{cardId, itemId, completed});
                if (result.status == CommandStatus::Applied) {
                    syncCardView(cardId, {});
                    SaveRuntimeConfiguration(configStore, storageRoot, runtime);
                }
                return result.status != CommandStatus::Rejected;
            });
        host.setTodoItemRemovedCallback(
            [&](const CardId& cardId, const std::string& itemId) {
                const auto result = runtime.execute(RemoveTodoItem{cardId, itemId});
                if (result.status == CommandStatus::Applied) {
                    syncCardView(cardId, {});
                    SaveRuntimeConfiguration(configStore, storageRoot, runtime);
                }
                return result.status != CommandStatus::Rejected;
            });
        host.setTodoItemsReorderedCallback(
            [&](const CardId& cardId, const std::vector<std::string>& order) {
                const auto result = runtime.execute(ReorderTodoItems{cardId, order});
                if (result.status == CommandStatus::Applied) {
                    syncCardView(cardId, {});
                    SaveRuntimeConfiguration(configStore, storageRoot, runtime);
                }
                return result.status != CommandStatus::Rejected;
            });
        host.setTodoItemsArchivedCallback([&](const CardId& cardId) {
            const auto result = runtime.execute(ArchiveCompletedTodoItems{cardId});
            if (result.status == CommandStatus::Applied) {
                syncCardView(cardId, {});
                SaveRuntimeConfiguration(configStore, storageRoot, runtime);
            }
            return result.status != CommandStatus::Rejected;
        });
        host.setApplicationItemsDroppedCallback(
            [&](const CardId& cardId,
                const std::vector<std::filesystem::path>& paths,
                const std::optional<CardId>& sourceCardId,
                FileDropOperation operation,
                std::size_t insertionIndex,
                std::size_t layoutColumns) {
                if (sourceCardId.has_value() && *sourceCardId != cardId) {
                    const auto* sourceMapping = FindMappingCard(runtime, *sourceCardId);
                    const auto sourceNavigation = mappingNavigation.find(*sourceCardId);
                    const auto sourceIsReferenceRoot = sourceMapping != nullptr
                        && sourceMapping->mode() == MappingMode::References
                        && (sourceNavigation == mappingNavigation.end()
                            || sourceNavigation->second.directoryStack.empty())
                        && paths.size() == 1
                        && std::filesystem::is_directory(paths.front());
                    if (sourceIsReferenceRoot) {
                        diagnostics.record(DiagnosticLevel::Warning,
                            "desktop.reference_root_cross_card_drop_rejected");
                        return false;
                    }
                }
                if (const auto* mapping = FindMappingCard(runtime, cardId);
                    mapping != nullptr) {
                    try {
                        if (mapping->mode() == MappingMode::Folder
                            && !mapping->sourceRoot().empty()
                            && std::ranges::any_of(paths, [&](const auto& path) {
                                return PathKey(path) == PathKey(mapping->sourceRoot());
                            })) {
                            // Dropping the source root onto itself must never
                            // be forwarded to Explorer as a MOVE.
                            return false;
                        }
                        const auto syncMapping = [&](
                            std::vector<desto::presentation::CardItemView> items,
                            std::vector<ApplicationItemPlacement> placements) {
                            host.updateMappingCard(
                                cardId, mapping->mode(), mapping->allowsSourceMutation(), items,
                                mapping->sortMode(), placements, MappingHasSource(*mapping));
                            cardItemsById[cardId] = items;
                            syncCardView(cardId, items);
                        };
                        const auto moveMappingItems = [&] (
                            const std::vector<desto::presentation::CardItemView>& items,
                            std::span<const ApplicationItemPlacement> preferred,
                            std::span<const std::filesystem::path> movedPaths) {
                            const auto columns = static_cast<std::uint32_t>((std::max)(
                                layoutColumns,
                                static_cast<std::size_t>(MappingCardColumns(*mapping))));
                            auto reconciled = ReconcileApplicationItemPlacements(
                                preferred,
                                ItemSourcePaths(items),
                                columns,
                                MappingCardMaximumRows(*mapping));
                            if (!reconciled.fits) return reconciled;
                            if (mapping->sortMode() == ApplicationItemSortMode::Custom) {
                                const auto effectiveInsertionIndex = items.empty()
                                    ? std::size_t{0} : insertionIndex;
                                reconciled = MoveApplicationItemsToSlot(
                                    reconciled.placements,
                                    movedPaths,
                                    static_cast<std::uint32_t>(effectiveInsertionIndex % columns),
                                    static_cast<std::uint32_t>(effectiveInsertionIndex / columns),
                                    columns,
                                    MappingCardMaximumRows(*mapping));
                            }
                            return reconciled;
                        };
                        if (sourceCardId == cardId) {
                            const auto navigation = mappingNavigation.find(cardId);
                            if (navigation != mappingNavigation.end()
                                && !navigation->second.directoryStack.empty()) {
                                const auto& destination = navigation->second.directoryStack.back();
                                const auto directoryKey = PathKey(destination);
                                const auto isInternalReorder = std::ranges::all_of(paths,
                                    [&](const auto& path) {
                                        return PathKey(path.parent_path()) == directoryKey;
                                    });
                                if (isInternalReorder) {
                                    auto items = shellItems.enumerate(
                                        destination, {},
                                        ResolveShellIconSourceSize(mapping->content().itemSize));
                                    auto& localPlacements = navigation->second.placementsByDirectory[directoryKey];
                                    auto placements = moveMappingItems(
                                        items, localPlacements, paths);
                                    if (!placements.fits) return false;
                                    localPlacements = placements.placements;
                                    auto title = destination.filename().wstring();
                                    if (title.empty()) title = destination.wstring();
                                    cardItemsById[cardId] = items;
                                    host.updateMappingNavigation(
                                        cardId, std::move(title), true, items, localPlacements);
                                    return true;
                                }
                                DirectoryImportPlan plan;
                                try {
                                    plan = DirectoryImportPlanner::plan(destination, paths);
                                } catch (...) {
                                    return false;
                                }
                                const auto transaction = operation == FileDropOperation::Copy
                                    ? FileCopyTransaction::execute(plan.moves)
                                    : FileMoveTransaction::execute(plan.moves);
                                if (!transaction.succeeded) return false;
                                // The moved source can already be present in
                                // the shell catalog and in Explorer's icon
                                // cache. Invalidate both sides before reading
                                // the destination directory again.
                                for (const auto& move : transaction.completedMoves) {
                                    if (operation == FileDropOperation::Move) {
                                        shellItems.notifyMoved(move.source, move.destination);
                                    } else {
                                        shellItems.invalidate(move.destination);
                                    }
                                }
                                shellItems.clearCache();
                                auto refreshedItems = shellItems.enumerate(
                                    destination, {},
                                    ResolveShellIconSourceSize(mapping->content().itemSize));
                                auto& localPlacements = navigation->second.placementsByDirectory[directoryKey];
                                auto reconciled = ReconcileApplicationItemPlacements(
                                    localPlacements, ItemSourcePaths(refreshedItems),
                                    MappingCardColumns(*mapping), MappingCardMaximumRows(*mapping));
                                if (!reconciled.fits) return false;
                                localPlacements = reconciled.placements;
                                cardItemsById[cardId] = refreshedItems;
                                auto title = destination.filename().wstring();
                                if (title.empty()) title = destination.wstring();
                                host.updateMappingNavigation(
                                    cardId, std::move(title), true, refreshedItems, localPlacements);
                                return true;
                            }
                            const auto items = cardItemsById.contains(cardId)
                                ? cardItemsById.at(cardId)
                                : MappingItems(*mapping, shellItems, mapping->content().itemSize);
                            if (mapping->mode() == MappingMode::Folder
                                && !mapping->sourceRoot().empty()
                                && std::ranges::all_of(paths, [&](const auto& path) {
                                    return PathKey(path.parent_path())
                                        == PathKey(mapping->sourceRoot());
                                })) {
                                auto placements = moveMappingItems(
                                    items, mapping->itemPlacements(), paths);
                                if (!placements.fits
                                    || runtime.execute(SetMappingCardLayout{
                                        cardId, mapping->sortMode(), placements.placements}).status
                                        == CommandStatus::Rejected) {
                                    return false;
                                }
                                syncMapping(items, std::move(placements.placements));
                                SaveRuntimeConfiguration(configStore, storageRoot, runtime);
                                return true;
                            }
                            auto placements = moveMappingItems(
                                items, mapping->itemPlacements(), paths);
                            if (!placements.fits
                                || runtime.execute(SetMappingCardLayout{
                                    cardId, mapping->sortMode(), placements.placements}).status
                                    == CommandStatus::Rejected) {
                                return false;
                            }
                            syncMapping(items, std::move(placements.placements));
                            SaveRuntimeConfiguration(configStore, storageRoot, runtime);
                            return true;
                        }
                        const auto navigation = mappingNavigation.find(cardId);
                        if (navigation != mappingNavigation.end()
                            && !navigation->second.directoryStack.empty()) {
                            const auto& destination = navigation->second.directoryStack.back();
                            const auto directoryKey = PathKey(destination);
                            DirectoryImportPlan plan;
                            try {
                                plan = DirectoryImportPlanner::plan(destination, paths);
                            } catch (...) {
                                diagnostics.record(DiagnosticLevel::Warning,
                                    "desktop.referenced_directory_import_rejected");
                                return false;
                            }
                            const auto transaction = operation == FileDropOperation::Copy
                                ? FileCopyTransaction::execute(plan.moves)
                                : FileMoveTransaction::execute(plan.moves);
                            if (!transaction.succeeded) {
                                diagnostics.record(DiagnosticLevel::Warning,
                                    "desktop.referenced_directory_import_failed");
                                return false;
                            }
                            for (const auto& move : transaction.completedMoves) {
                                if (operation == FileDropOperation::Move) {
                                    shellItems.notifyMoved(move.source, move.destination);
                                } else {
                                    shellItems.invalidate(move.destination);
                                }
                            }
                            shellItems.clearCache();
                            auto refreshedItems = shellItems.enumerate(
                                destination, {},
                                ResolveShellIconSourceSize(mapping->content().itemSize));
                            auto& localPlacements = navigation->second.placementsByDirectory[directoryKey];
                            auto reconciled = ReconcileApplicationItemPlacements(
                                localPlacements, ItemSourcePaths(refreshedItems),
                                MappingCardColumns(*mapping), MappingCardMaximumRows(*mapping));
                            if (!reconciled.fits) return false;
                            localPlacements = reconciled.placements;
                            cardItemsById[cardId] = refreshedItems;
                            auto title = destination.filename().wstring();
                            if (title.empty()) title = destination.wstring();
                            host.updateMappingNavigation(
                                cardId, std::move(title), true, refreshedItems, localPlacements);
                            return true;
                        }
                        if (mapping->mode() == MappingMode::Folder
                            && mapping->sourceRoot().empty()
                            && paths.size() == 1
                            && std::filesystem::is_directory(paths.front())) {
                            // An empty Mapping Card is intentionally a folder
                            // selector. It never imports or moves the dropped
                            // files into Desto storage.
                            if (runtime.execute(SetMappingFolderSource{
                                    cardId, paths.front().lexically_normal()}).status
                                == CommandStatus::Rejected) {
                                return false;
                            }
                            const auto* updatedMapping = FindMappingCard(runtime, cardId);
                            if (updatedMapping == nullptr) return false;
                            auto items = MappingItems(
                                *updatedMapping, shellItems, updatedMapping->content().itemSize);
                            auto placements = ReconcileApplicationItemPlacements(
                                {}, ItemSourcePaths(items),
                                MappingCardColumns(*updatedMapping),
                                MappingCardMaximumRows(*updatedMapping));
                            if (!placements.fits
                                || runtime.execute(SetMappingCardLayout{
                                    cardId, updatedMapping->sortMode(), placements.placements}).status
                                    == CommandStatus::Rejected) return false;
                            syncMapping(std::move(items), std::move(placements.placements));
                            restartMappingWatches();
                            SaveRuntimeConfiguration(configStore, storageRoot, runtime);
                            return true;
                        }
                        if (mapping->mode() == MappingMode::References) {
                            const auto previousPlacements = mapping->itemPlacements();
                            auto references = mapping->references();
                            for (const auto& pathValue : paths) {
                                if (pathValue.empty() || !pathValue.is_absolute()
                                    || !std::filesystem::exists(pathValue)) {
                                    return false;
                                }
                                const auto path = pathValue.lexically_normal();
                                if (std::ranges::any_of(references, [&](const FileReference& value) {
                                        return PathKey(value.path) == PathKey(path);
                                    })) {
                                    continue;
                                }
                                references.push_back({MappingReferenceId(path, references), path});
                            }
                            if (runtime.execute(SetMappingReferences{cardId, references}).status
                                == CommandStatus::Rejected) {
                                return false;
                            }
                            auto items = MappingItems(
                                *mapping, shellItems, mapping->content().itemSize);
                            auto placements = moveMappingItems(
                                items, previousPlacements, paths);
                            if (!placements.fits
                                || runtime.execute(SetMappingCardLayout{
                                    cardId, mapping->sortMode(), placements.placements}).status
                                    == CommandStatus::Rejected) return false;
                            syncMapping(std::move(items), std::move(placements.placements));
                            SaveRuntimeConfiguration(configStore, storageRoot, runtime);
                            return true;
                        }
                        if (mapping->mode() == MappingMode::Folder
                            && !mapping->sourceRoot().empty()) {
                            MappingCardImportService mappingImportService;
                            const auto plan = mappingImportService.plan(*mapping, paths);
                            const auto currentItems = cardItemsById.contains(cardId)
                                ? cardItemsById.at(cardId)
                                : MappingItems(
                                    *mapping, shellItems, mapping->content().itemSize);
                            if (const auto maximumRows = MappingCardMaximumRows(*mapping);
                                maximumRows.has_value()) {
                                const auto capacity = static_cast<std::size_t>(
                                    MappingCardColumns(*mapping)) * *maximumRows;
                                if (currentItems.size() + plan.moves.size() > capacity) {
                                    diagnostics.record(
                                        DiagnosticLevel::Warning, "desktop.fixed_grid_full");
                                    return false;
                                }
                            }

                            const auto result = operation == FileDropOperation::Copy
                                ? FileCopyTransaction::execute(plan.moves)
                                : FileMoveTransaction::execute(plan.moves);
                            if (!result.succeeded) {
                                diagnostics.record(
                                    DiagnosticLevel::Warning, "desktop.mapping_import_failed");
                                return false;
                            }
                            FileTransactionRollbackGuard rollbackGuard(
                                result.completedMoves, operation, diagnostics);
                            const auto previousPlacements = mapping->itemPlacements();
                            auto items = MappingItems(
                                *mapping, shellItems, mapping->content().itemSize);
                            std::vector<std::filesystem::path> importedPaths;
                            importedPaths.reserve(result.completedMoves.size());
                            for (const auto& move : result.completedMoves) {
                                importedPaths.push_back(move.destination);
                            }
                            auto placements = moveMappingItems(
                                items, previousPlacements, importedPaths);
                            if (!placements.fits
                                || runtime.execute(SetMappingCardLayout{
                                    cardId, mapping->sortMode(), placements.placements}).status
                                    == CommandStatus::Rejected) {
                                return false;
                            }
                            try {
                                syncMapping(items, placements.placements);
                            } catch (...) {
                                (void)runtime.execute(SetMappingCardLayout{
                                    cardId, mapping->sortMode(), previousPlacements});
                                throw;
                            }
                            try {
                                SaveRuntimeConfiguration(configStore, storageRoot, runtime);
                            } catch (...) {
                                (void)runtime.execute(SetMappingCardLayout{
                                    cardId, mapping->sortMode(), previousPlacements});
                                return false;
                            }
                            rollbackGuard.release();
                            if (operation == FileDropOperation::Move) {
                                for (const auto& move : result.completedMoves) {
                                    shellItems.notifyMoved(move.source, move.destination);
                                }
                            }
                            return true;
                        }
                        return false;

                    } catch (...) {
                        diagnostics.record(
                            DiagnosticLevel::Warning, "desktop.mapping_import_rejected");
                        return false;
                    }
                }
                const auto* card = FindApplicationCard(runtime, cardId);
                if (card == nullptr) {
                    diagnostics.record(DiagnosticLevel::Warning, "desktop.import_card_missing");
                    return false;
                }
                try {
                    const auto directory = storageRoot.resolveCardPath(card->relativeStoragePath());
                    auto beforeEntry = cardItemsById.find(cardId);
                    if (beforeEntry == cardItemsById.end()) {
                        beforeEntry = cardItemsById.emplace(
                            cardId,
                            shellItems.enumerate(
                                directory,
                                PlacementOrder(*card),
                                CardShellIconSource(*card))).first;
                    }
                    const auto& before = beforeEntry->second;
                    const auto columns = CardColumns(runtime, *card);
                    const auto maximumRows = CardMaximumRows(*card);
                    if (maximumRows.has_value()) {
                        std::size_t incoming = 0;
                        for (const auto& path : paths) {
                            if (path.lexically_normal().parent_path() != directory.lexically_normal()) {
                                ++incoming;
                            }
                        }
                        const auto capacity = static_cast<std::size_t>(columns) * *maximumRows;
                        if (before.size() + incoming > capacity) {
                            diagnostics.record(DiagnosticLevel::Warning, "desktop.fixed_grid_full");
                            return false;
                        }
                    }
                    std::vector<ApplicationCardLocation> cardLocations;
                    for (const auto* candidate : runtime.cards()) {
                        if (candidate->type() != CardType::Application) continue;
                        const auto* application = static_cast<const ApplicationCard*>(candidate);
                        cardLocations.push_back({
                            application->id(),
                            storageRoot.resolveCardPath(application->relativeStoragePath()),
                        });
                    }
                    auto refreshBatch = operation == FileDropOperation::Copy
                        ? std::vector<CardId>{cardId}
                        : ResolveApplicationCardRefreshBatch(cardId, cardLocations, paths);
                    if (operation == FileDropOperation::Move
                        && sourceCardId.has_value()
                        && FindApplicationCard(runtime, *sourceCardId) != nullptr
                        && std::ranges::find(refreshBatch, *sourceCardId) == refreshBatch.end()) {
                        refreshBatch.insert(refreshBatch.begin(), *sourceCardId);
                    }

                    std::unordered_map<
                        std::wstring,
                        desto::presentation::CardItemView> preparedItems;
                    for (const auto& path : paths) {
                        if (const auto* cached = FindCachedItem(cardItemsById, path);
                            cached != nullptr
                            && (cached->icon.empty()
                                || cached->icon.width
                                    == static_cast<int>(CardShellIconSource(*card)))) {
                            preparedItems.emplace(PathKey(path), *cached);
                        } else {
                            preparedItems.emplace(
                                PathKey(path),
                                shellItems.inspect(path, CardShellIconSource(*card)));
                        }
                    }
                    std::unordered_map<
                        CardId,
                        std::vector<desto::presentation::CardItemView>> nextItemsByCard;
                    for (const auto& affectedCardId : refreshBatch) {
                        const auto* affected = FindApplicationCard(runtime, affectedCardId);
                        if (affected == nullptr) continue;
                        const auto cached = cardItemsById.find(affectedCardId);
                        nextItemsByCard[affectedCardId] = cached != cardItemsById.end()
                            ? cached->second
                            : shellItems.enumerate(
                                storageRoot.resolveCardPath(affected->relativeStoragePath()),
                                PlacementOrder(*affected),
                                CardShellIconSource(*affected));
                    }
                    const auto plan = importService.plan(*card, paths);
                    const auto result = operation == FileDropOperation::Copy
                        ? FileCopyTransaction::execute(plan.moves)
                        : FileMoveTransaction::execute(plan.moves);
                    if (!result.succeeded) {
                        diagnostics.record(DiagnosticLevel::Warning, "desktop.import_failed");
                        return false;
                    }
                    FileTransactionRollbackGuard rollbackGuard(
                        result.completedMoves, operation, diagnostics);
                    for (const auto& move : result.completedMoves) {
                        const auto sourceKey = PathKey(move.source);
                        if (operation == FileDropOperation::Move) {
                            for (auto& [affectedCardId, items] : nextItemsByCard) {
                                (void)affectedCardId;
                                std::erase_if(items, [&](const auto& item) {
                                    return PathKey(item.sourcePath) == sourceKey;
                                });
                            }
                        }
                        const auto prepared = preparedItems.find(sourceKey);
                        if (prepared == preparedItems.end()) {
                            diagnostics.record(
                                DiagnosticLevel::Warning, "desktop.prepared_item_missing");
                            return false;
                        }
                        auto movedItem = shellItems.retarget(
                            prepared->second, move.destination);
                        auto& targetItems = nextItemsByCard[cardId];
                        const auto destinationKey = PathKey(move.destination);
                        std::erase_if(targetItems, [&](const auto& item) {
                            return PathKey(item.sourcePath) == destinationKey;
                        });
                        targetItems.push_back(std::move(movedItem));
                    }
                    std::vector<std::filesystem::path> movedNames;
                    for (const auto& source : paths) {
                        const auto normalized = source.lexically_normal();
                        const auto completed = std::find_if(
                            result.completedMoves.begin(),
                            result.completedMoves.end(),
                            [&](const FileMove& move) { return move.source == normalized; });
                        if (completed != result.completedMoves.end()) {
                            movedNames.push_back(completed->destination.filename());
                        } else if (normalized.parent_path() == directory.lexically_normal()) {
                            movedNames.push_back(normalized.filename());
                        }
                    }
                    struct PendingUpdate {
                        CardId cardId;
                        ApplicationItemSortMode sortMode;
                        std::vector<ApplicationItemPlacement> previousPlacements;
                        std::vector<ApplicationItemPlacement> placements;
                        std::vector<desto::presentation::CardItemView> items;
                    };
                    std::vector<PendingUpdate> pendingUpdates;
                    for (const auto& affectedCardId : refreshBatch) {
                        const auto* affected = FindApplicationCard(runtime, affectedCardId);
                        if (affected == nullptr) continue;
                        auto items = nextItemsByCard[affectedCardId];
                        const auto affectedColumns = static_cast<std::uint32_t>((std::max)(
                            static_cast<std::size_t>(CardColumns(
                                runtime, *affected, items.size())),
                            affectedCardId == cardId ? layoutColumns : std::size_t{1}));
                        auto placements = ReconcileApplicationItemPlacements(
                            affected->itemPlacements(),
                            ItemFileNames(items),
                            affectedColumns,
                            CardMaximumRows(*affected));
                        if (!placements.fits) return false;
                        if (affectedCardId == cardId
                            && affected->sortMode() == ApplicationItemSortMode::Custom) {
                            const auto effectiveInsertionIndex = before.empty()
                                ? std::size_t{0} : insertionIndex;
                            placements = MoveApplicationItemsToSlot(
                                placements.placements,
                                movedNames,
                                static_cast<std::uint32_t>(effectiveInsertionIndex % affectedColumns),
                                static_cast<std::uint32_t>(effectiveInsertionIndex / affectedColumns),
                                affectedColumns,
                                CardMaximumRows(*affected));
                            if (!placements.fits) return false;
                        }
                        ApplicationCard validation(
                            affectedCardId, affected->relativeStoragePath());
                        validation.setLayout(affected->sortMode(), placements.placements);
                        pendingUpdates.push_back({
                            affectedCardId,
                            affected->sortMode(),
                            affected->itemPlacements(),
                            std::move(placements.placements),
                            std::move(items),
                        });
                    }
                    std::vector<WindowsDesktopHost::CardItemsUpdate> updates;
                    updates.reserve(pendingUpdates.size());
                    for (const auto& update : pendingUpdates) {
                        updates.push_back({
                            update.cardId,
                            update.items,
                            update.sortMode,
                            update.placements,
                        });
                    }

                    std::size_t appliedCount = 0;
                    for (; appliedCount < pendingUpdates.size(); ++appliedCount) {
                        const auto& update = pendingUpdates[appliedCount];
                        if (runtime.execute(SetApplicationCardLayout{
                                update.cardId, update.sortMode, update.placements}).status
                            == CommandStatus::Rejected) {
                            break;
                        }
                    }
                    if (appliedCount != pendingUpdates.size()) {
                        while (appliedCount > 0) {
                            --appliedCount;
                            const auto& update = pendingUpdates[appliedCount];
                            (void)runtime.execute(SetApplicationCardLayout{
                                update.cardId, update.sortMode, update.previousPlacements,
                            });
                        }
                        diagnostics.record(
                            DiagnosticLevel::Warning, "desktop.item_layout_rejected");
                        return false;
                    }
                    try {
                        host.updateCardItemsBatch(std::move(updates));
                    } catch (...) {
                        for (const auto& update : pendingUpdates) {
                            (void)runtime.execute(SetApplicationCardLayout{
                                update.cardId, update.sortMode, update.previousPlacements,
                            });
                        }
                        if (rollbackGuard.rollbackNow()) {
                            std::vector<WindowsDesktopHost::CardItemsUpdate> restoredUpdates;
                            for (const auto& update : pendingUpdates) {
                                const auto* restored = FindApplicationCard(runtime, update.cardId);
                                if (restored == nullptr) continue;
                                restoredUpdates.push_back({
                                    update.cardId,
                                    shellItems.enumerate(
                                        storageRoot.resolveCardPath(restored->relativeStoragePath()),
                                        PlacementOrder(*restored),
                                        CardShellIconSource(*restored)),
                                    restored->sortMode(),
                                    restored->itemPlacements(),
                                });
                            }
                            try {
                                host.updateCardItemsBatch(std::move(restoredUpdates));
                            } catch (...) {
                            }
                        }
                        diagnostics.record(
                            DiagnosticLevel::Warning, "desktop.item_batch_update_failed");
                        return false;
                    }
                    for (auto& update : pendingUpdates) {
                        const auto cached = cardItemsById.find(update.cardId);
                        if (cached != cardItemsById.end()) {
                            cached->second.swap(update.items);
                            syncCardView(update.cardId, cached->second);
                        }
                    }
                    rollbackGuard.release();
                    if (operation == FileDropOperation::Move) {
                        for (const auto& move : result.completedMoves) {
                            shellItems.notifyMoved(move.source, move.destination);
                        }
                    }
                    return true;
                } catch (...) {
                    diagnostics.record(DiagnosticLevel::Warning, "desktop.import_rejected");
                    return false;
                }
            });
        host.setApplicationItemDragCompletedCallback([&](const CardId& cardId) {
            const auto* card = FindApplicationCard(runtime, cardId);
            if (card != nullptr) {
                const auto directory = storageRoot.resolveCardPath(card->relativeStoragePath());
                auto items = shellItems.enumerate(
                    directory, PlacementOrder(*card), CardShellIconSource(*card));
                auto placements = ReconcileApplicationItemPlacements(
                    card->itemPlacements(),
                    ItemFileNames(items),
                    CardColumns(runtime, *card, items.size()),
                    CardMaximumRows(*card));
                if (!placements.fits
                    || runtime.execute(SetApplicationCardLayout{
                        cardId, card->sortMode(), placements.placements}).status
                    == CommandStatus::Rejected) {
                    diagnostics.record(DiagnosticLevel::Warning, "desktop.item_layout_rejected");
                    return;
                }
                host.updateCardItemsBatch({{
                    cardId, items, card->sortMode(), std::move(placements.placements),
                }});
                cardItemsById[cardId] = items;
                syncCardView(cardId, items);
                return;
            }

            const auto* mapping = FindMappingCard(runtime, cardId);
            if (mapping == nullptr) return;
            const auto navigation = mappingNavigation.find(cardId);
            if (navigation != mappingNavigation.end()
                && !navigation->second.directoryStack.empty()) {
                const auto& directory = navigation->second.directoryStack.back();
                auto items = shellItems.enumerate(
                    directory, {}, ResolveShellIconSourceSize(mapping->content().itemSize));
                const auto directoryKey = PathKey(directory);
                auto& localPlacements = navigation->second.placementsByDirectory[directoryKey];
                const auto reconciled = ReconcileApplicationItemPlacements(
                    localPlacements, ItemSourcePaths(items), MappingCardColumns(*mapping),
                    MappingCardMaximumRows(*mapping));
                if (!reconciled.fits) return;
                localPlacements = reconciled.placements;
                cardItemsById[cardId] = items;
                auto title = directory.filename().wstring();
                if (title.empty()) title = directory.wstring();
                host.updateMappingNavigation(
                    cardId, std::move(title), true, items, localPlacements);
                return;
            }

            auto items = MappingItems(*mapping, shellItems, mapping->content().itemSize);
            const auto placements = ReconcileApplicationItemPlacements(
                mapping->itemPlacements(),
                ItemSourcePaths(items),
                MappingCardColumns(*mapping),
                MappingCardMaximumRows(*mapping));
            if (!placements.fits
                || runtime.execute(SetMappingCardLayout{
                    cardId, mapping->sortMode(), placements.placements}).status
                == CommandStatus::Rejected) {
                diagnostics.record(DiagnosticLevel::Warning,
                    "desktop.mapping_item_layout_rejected");
                return;
            }
            host.updateMappingCard(
                cardId,
                mapping->mode(),
                mapping->allowsSourceMutation(),
                items,
                mapping->sortMode(),
                placements.placements,
                MappingHasSource(*mapping));
            cardItemsById[cardId] = items;
            syncCardView(cardId, items);
            SaveRuntimeConfiguration(configStore, storageRoot, runtime);
        });
        const auto showMappingNavigation = [&](const CardId& cardId) {
            const auto* mapping = FindMappingCard(runtime, cardId);
            if (mapping == nullptr) return false;
            const auto navigation = mappingNavigation.find(cardId);
            const auto hasDirectory = navigation != mappingNavigation.end()
                && !navigation->second.directoryStack.empty();
            std::vector<desto::presentation::CardItemView> items;
            std::vector<ApplicationItemPlacement> placements;
            std::wstring title;
            if (hasDirectory) {
                const auto& directory = navigation->second.directoryStack.back();
                items = shellItems.enumerate(
                    directory, {}, ResolveShellIconSourceSize(mapping->content().itemSize));
                const auto directoryKey = PathKey(directory);
                auto& localPlacements = navigation->second.placementsByDirectory[directoryKey];
                const auto reconciled = ReconcileApplicationItemPlacements(
                    localPlacements, ItemSourcePaths(items), MappingCardColumns(*mapping),
                    MappingCardMaximumRows(*mapping));
                if (!reconciled.fits) return false;
                localPlacements = reconciled.placements;
                placements = localPlacements;
                title = directory.filename().wstring();
                if (title.empty()) title = directory.wstring();
            } else {
                items = MappingItems(*mapping, shellItems, mapping->content().itemSize);
                title = desto::presentation::MakeCardView(*mapping, uiLanguage).title;
            }
            cardItemsById[cardId] = items;
            host.updateMappingNavigation(
                cardId, std::move(title), hasDirectory, items, std::move(placements));
            if (!hasDirectory) {
                host.updateMappingCard(
                    cardId,
                    mapping->mode(),
                    mapping->allowsSourceMutation(),
                    items,
                    mapping->sortMode(),
                    mapping->itemPlacements(),
                    MappingHasSource(*mapping));
            }
            return true;
        };
        host.setMappingNavigateUpCallback([&](const CardId& cardId) {
            const auto navigation = mappingNavigation.find(cardId);
            if (navigation == mappingNavigation.end()
                || navigation->second.directoryStack.empty()) return;
            navigation->second.directoryStack.pop_back();
            if (navigation->second.directoryStack.empty()) {
                mappingNavigation.erase(navigation);
            }
            if (!showMappingNavigation(cardId)) {
                diagnostics.record(DiagnosticLevel::Warning,
                    "desktop.mapping_navigation_up_failed");
            }
        });
        host.setCardItemActivatedCallback(
            [&](const CardId& cardId, const desto::presentation::CardItemView& item) {
                std::error_code statusError;
                if (FindMappingCard(runtime, cardId) != nullptr
                    && std::filesystem::is_directory(item.sourcePath, statusError)
                    && !statusError) {
                    mappingNavigation[cardId].directoryStack.push_back(item.sourcePath);
                    if (!showMappingNavigation(cardId)) {
                        mappingNavigation[cardId].directoryStack.pop_back();
                        diagnostics.record(DiagnosticLevel::Warning,
                            "desktop.mapping_navigation_open_failed");
                    }
                    return;
                }
                if (!shellItems.launch(item)) {
                    diagnostics.record(DiagnosticLevel::Warning, "desktop.item_launch_failed");
                }
            });
        host.setMappingReferenceRemovedCallback(
            [&](const CardId& cardId, const desto::presentation::CardItemView& item) {
                const auto* mapping = FindMappingCard(runtime, cardId);
                if (mapping == nullptr || mapping->mode() != MappingMode::References) {
                    return false;
                }
                const auto navigation = mappingNavigation.find(cardId);
                if (navigation != mappingNavigation.end()
                    && !navigation->second.directoryStack.empty()) {
                    return true;
                }
                if (!ShowWindowsConfirmation(
                        static_cast<HWND>(settingsHost.nativeHandle()),
                        L"移除引用映射",
                        L"是否移除该项目的引用映射？\n源文件不会被删除或移动。",
                        L"移除",
                        L"取消")) {
                    return true;
                }
                auto references = mapping->references();
                const auto key = PathKey(item.sourcePath);
                const auto removed = std::erase_if(references, [&](const auto& reference) {
                    return PathKey(reference.path) == key;
                });
                if (removed == 0) return true;
                if (runtime.execute(SetMappingReferences{
                        cardId, std::move(references)}).status == CommandStatus::Rejected) {
                    diagnostics.record(DiagnosticLevel::Warning,
                        "desktop.mapping_reference_remove_rejected");
                    return false;
                }
                const auto* updated = FindMappingCard(runtime, cardId);
                if (updated == nullptr) return false;
                auto items = MappingItems(
                    *updated, shellItems, updated->content().itemSize);
                const auto reconciled = ReconcileApplicationItemPlacements(
                    updated->itemPlacements(),
                    ItemSourcePaths(items),
                    MappingCardColumns(*updated),
                    MappingCardMaximumRows(*updated));
                if (!reconciled.fits
                    || runtime.execute(SetMappingCardLayout{
                        cardId, updated->sortMode(), reconciled.placements}).status
                        == CommandStatus::Rejected) {
                    diagnostics.record(DiagnosticLevel::Warning,
                        "desktop.mapping_reference_layout_rejected");
                    return false;
                }
                const auto* saved = FindMappingCard(runtime, cardId);
                host.updateMappingCard(
                    cardId,
                    saved->mode(),
                    saved->allowsSourceMutation(),
                    items,
                    saved->sortMode(),
                    reconciled.placements,
                    MappingHasSource(*saved));
                cardItemsById[cardId] = items;
                syncCardView(cardId, items);
                SaveRuntimeConfiguration(configStore, storageRoot, runtime);
                return true;
            });
        host.setFileDeleteConfirmationCallback(
            [&](const CardId&, const desto::presentation::CardItemView& item) {
                if (!applicationPreferences.confirmFileDeletion) return true;
                return ShowWindowsConfirmation(
                    static_cast<HWND>(settingsHost.nativeHandle()),
                    L"确认删除文件",
                    L"是否删除“" + item.displayName
                        + L"”？此操作将直接修改源文件。",
                    L"删除",
                    L"取消");
            });
        host.setCardItemsRefreshCallback(
            [&](const CardId& cardId, CardItemSize itemSize) {
                if (const auto* mapping = FindMappingCard(runtime, cardId);
                    mapping != nullptr) {
                    std::optional<DirectoryMappingChange> change;
                    {
                        std::lock_guard lock(pendingMappingChangesMutex);
                        const auto pending = pendingMappingChanges.find(cardId);
                        if (pending != pendingMappingChanges.end()) {
                            change = std::move(pending->second);
                            pendingMappingChanges.erase(pending);
                        }
                    }
                    const auto iconSize = ResolveShellIconSourceSize(itemSize);
                    const auto navigation = mappingNavigation.find(cardId);
                    if (navigation != mappingNavigation.end()
                        && !navigation->second.directoryStack.empty()) {
                        auto items = shellItems.enumerate(
                            navigation->second.directoryStack.back(), {}, iconSize);
                        cardItemsById[cardId] = items;
                        return items;
                    }
                    auto existing = cardItemsById.find(cardId);
                    if (mapping->mode() == MappingMode::Folder
                        && mapping->sourceRoot().empty()) {
                        std::vector<desto::presentation::CardItemView> emptyItems;
                        cardItemsById[cardId] = emptyItems;
                        return emptyItems;
                    }
                    const auto canRefreshIncrementally = mapping->mode() == MappingMode::Folder
                        && change.has_value()
                        && !change->requiresFullRefresh
                        && existing != cardItemsById.end()
                        && std::ranges::all_of(existing->second, [&](const auto& item) {
                            return item.icon.empty()
                                || (item.icon.width == static_cast<int>(iconSize)
                                    && item.icon.height == static_cast<int>(iconSize));
                        });
                    auto items = canRefreshIncrementally
                        ? shellItems.refreshDirectoryEntries(
                            mapping->sourceRoot(),
                            existing->second,
                            change->relativePaths,
                            iconSize)
                        : MappingItems(*mapping, shellItems, itemSize);
                    cardItemsById[cardId] = items;
                    return items;
                }
                 const auto* card = FindApplicationCard(runtime, cardId);
                 if (card == nullptr) {
                     return std::vector<desto::presentation::CardItemView>{};
                 }
                 auto items = shellItems.enumerate(
                     storageRoot.resolveCardPath(card->relativeStoragePath()),
                     PlacementOrder(*card),
                     ResolveShellIconSourceSize(itemSize));
                 cardItemsById[cardId] = items;
                 return items;
             });
        host.present(runtime.projections(), displays, cardViews);
        settingsHost.present(cardViews, {
            .timeZoneOffsetMinutes = applicationPreferences.timeZoneOffsetMinutes,
            .language = applicationPreferences.language,
            .storageRoot = storageRoot.path(),
            .globalCornerRadius = applicationPreferences.globalCardCornerRadius,
            .runAtStartup = applicationPreferences.runAtStartup,
            .desktopDoubleClickAction = applicationPreferences.desktopDoubleClickAction,
            .taskbarDoubleClickAction = applicationPreferences.taskbarDoubleClickAction,
            .pinnedCardsYieldToFullscreen =
                applicationPreferences.pinnedCardsYieldToFullscreen,
            .showIconBackgroundFrame =
                applicationPreferences.showIconBackgroundFrame,
            .updateChannel = applicationPreferences.updateChannel,
        });
        if (launchOptions.showSettings) {
            settingsHost.show();
        }
        restartMappingWatches();
        if (!lifecycle.runtimeReady().applied) {
            throw std::runtime_error("Runtime lifecycle transition failed.");
        }
        host.run(launchOptions.durationMilliseconds);
        SaveRuntimeConfiguration(
            configStore, storageRoot, runtime, &applicationPreferences);
        if (!lifecycle.requestShutdown(ShutdownReason::User).applied) {
            throw std::runtime_error("Shutdown request transition failed.");
        }
        if (!lifecycle.completeShutdown().applied) {
            throw std::runtime_error("Shutdown completion transition failed.");
        }
        return 0;
    } catch (const std::exception& error) {
        try {
            std::ofstream log(DefaultStorageRoot().parent_path() / "startup-error.log",
                std::ios::app);
            log << error.what() << "\n";
        } catch (...) {
        }
        (void)lifecycle.fail();
        return 1;
    } catch (...) {
        try {
            std::ofstream log(DefaultStorageRoot().parent_path() / "startup-error.log",
                std::ios::app);
            log << "unknown startup exception\n";
        } catch (...) {
        }
        (void)lifecycle.fail();
        return 1;
    }
}
