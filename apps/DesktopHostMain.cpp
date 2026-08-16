#include "ApplicationLifecycle.h"
#include "ApplicationCardImport.h"
#include "ApplicationCardOrdering.h"
#include "ApplicationCardTransfer.h"
#include "ApplicationRuntime.h"
#include "Diagnostics.h"
#include "CardContentLayout.h"
#include "FileMoveTransaction.h"
#include "MappingCardImport.h"
#include "CardView.h"
#include "StorageRoot.h"
#include "WindowsDesktopHost.h"
#include "WindowsDirectoryChangeSource.h"
#include "WindowsDisplayTopology.h"
#include "WindowsShellItemCatalog.h"
#include "WindowsSingleInstanceGate.h"

#include <Windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <stdexcept>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_map>
#include <vector>

using namespace desto::application;
using namespace desto::domain;
using namespace desto::platform::windows;
using namespace desto::storage;

namespace {

int DurationMilliseconds(std::wstring_view commandLine) {
    constexpr auto marker = std::wstring_view(L"--duration-ms ");
    const auto position = commandLine.find(marker);
    if (position == std::wstring_view::npos) {
        return 0;
    }
    try {
        return std::clamp(
            std::stoi(std::wstring(commandLine.substr(position + marker.size()))), 1000, 120000);
    } catch (...) {
        return 0;
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

std::vector<desto::presentation::CardItemView> MappingItems(
    const MappingCard& card,
    WindowsShellItemCatalog& catalog,
    CardItemSize itemSize) {
    const auto iconSize = ResolveShellIconSourceSize(itemSize);
    if (card.mode() == MappingMode::Folder) {
        return catalog.enumerate(card.sourceRoot(), {}, iconSize);
    }
    std::vector<desto::presentation::CardItemView> result;
    result.reserve(card.references().size());
    for (const auto& reference : card.references()) {
        result.push_back(catalog.inspect(reference.path, iconSize));
    }
    return result;
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
    if (card.content().sizeMode == CardSizeMode::Fixed) {
        return card.content().fixedColumns;
    }
    const auto settings = desto::presentation::ResolveCardContentLayoutSettings(card.content());
    std::size_t requiredColumns = 1;
    if (card.sortMode() == ApplicationItemSortMode::Custom) {
        for (const auto& placement : card.itemPlacements()) {
            requiredColumns = (std::max)(
                requiredColumns, static_cast<std::size_t>(placement.column) + 1);
        }
    }
    return static_cast<std::uint32_t>(desto::presentation::ResolveAdaptiveCardColumns(
        card.sortMode() == ApplicationItemSortMode::Custom
            ? std::size_t{0}
            : projectedItemCount.value_or(card.itemPlacements().size()),
        requiredColumns,
        settings));
}

std::optional<std::uint32_t> CardMaximumRows(const ApplicationCard& card) {
    return card.content().sizeMode == CardSizeMode::Fixed
        ? std::optional<std::uint32_t>(card.content().fixedRows)
        : std::nullopt;
}

ShellIconSourceSize CardShellIconSource(const ApplicationCard& card) noexcept {
    return ResolveShellIconSourceSize(card.content().itemSize);
}

class FileMoveRollbackGuard final {
public:
    FileMoveRollbackGuard(
        std::span<const FileMove> moves,
        DiagnosticRecorder& diagnostics)
        : moves_(moves.begin(), moves.end()), diagnostics_(diagnostics) {
    }

    ~FileMoveRollbackGuard() noexcept {
        (void)rollbackNow();
    }

    [[nodiscard]] bool rollbackNow() noexcept {
        if (!active_) return true;
        active_ = false;
        if (moves_.empty()) return true;
        try {
            const auto succeeded = FileMoveTransaction::rollback(moves_).succeeded;
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
    DiagnosticRecorder& diagnostics_;
    bool active_ = true;
};

void SeedPreview(
    ApplicationRuntime& runtime,
    const std::vector<DisplaySnapshot>& displays,
    const std::filesystem::path& mappingSourceRoot) {
    if (displays.empty()) {
        throw std::runtime_error("A display is required to seed preview Cards.");
    }
    const auto primary = std::ranges::find_if(displays, &DisplaySnapshot::primary);
    const auto& targetDisplay = primary == displays.end() ? displays.front() : *primary;
    const std::vector<CardId> ids{
        "preview-application-1",
        "preview-application-2",
        "preview-mapping",
        "preview-todo",
    };
    const auto firstApplication = runtime.execute(
        CreateApplicationCard{ids[0], std::filesystem::path("cards") / ids[0]});
    const auto secondApplication = runtime.execute(
        CreateApplicationCard{ids[1], std::filesystem::path("cards") / ids[1]});
    if (firstApplication.status == CommandStatus::Rejected
        || secondApplication.status == CommandStatus::Rejected) {
        throw std::runtime_error("Unable to create application preview Card.");
    }
    if (runtime.execute(CreateMappingCard{ids[2]}).status == CommandStatus::Rejected
        || runtime.execute(CreateTodoCard{ids[3]}).status == CommandStatus::Rejected) {
        throw std::runtime_error("Unable to create preview Card.");
    }
    if (runtime.execute(SetMappingFolderSource{ids[2], mappingSourceRoot}).status
        == CommandStatus::Rejected) {
        throw std::runtime_error("Unable to configure Mapping Card preview source.");
    }
    for (std::size_t index = 0; index < ids.size(); ++index) {
        const auto placement = runtime.execute(SetPlacement{{
            .id = "preview-placement-" + ids[index],
            .cardId = ids[index],
            .target = DisplayTarget::specific(targetDisplay.id),
            .rect = {48.0 + index * 360.0, 56.0 + index * 32.0, 320.0, 220.0},
            .zIndex = static_cast<std::int32_t>(index),
            .referenceWorkAreaWidth = targetDisplay.workAreaWidth,
            .referenceWorkAreaHeight = targetDisplay.workAreaHeight,
        }});
        if (placement.status == CommandStatus::Rejected) {
            throw std::runtime_error("Unable to create preview Placement.");
        }
    }
    auto cardSnapshots = runtime.cardSnapshots();
    for (auto& card : cardSnapshots) {
        switch (card.type) {
        case CardType::Application:
            card.appearance = {.preset = "white", .opacity = 0.96, .cornerRadius = 18.0};
            break;
        case CardType::Mapping:
            card.appearance = {.preset = "black", .opacity = 0.82, .cornerRadius = 24.0};
            break;
        case CardType::Todo:
            card.appearance = {.preset = "jewel", .opacity = 0.92, .cornerRadius = 30.0};
            break;
        }
    }
    const auto workspace = runtime.workspace();
    runtime.restore(cardSnapshots, workspace);
    if (runtime.execute(UpdateDisplayTopology{displays}).status == CommandStatus::Rejected) {
        throw std::runtime_error("Unable to apply display topology.");
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR commandLine, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
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
        StorageRoot storageRoot(DefaultStorageRoot());
        storageRoot.ensureExists();
        const auto mappingPreviewRoot = storageRoot.path() / "mapping-preview";
        std::filesystem::create_directories(mappingPreviewRoot);
        SeedPreview(runtime, displays, mappingPreviewRoot);
        ApplicationCardImportService importService(storageRoot);
        MappingCardImportService mappingImportService;
        WindowsShellItemCatalog shellItems;
        std::vector<desto::presentation::CardView> cardViews;
        std::unordered_map<
            CardId,
            std::vector<desto::presentation::CardItemView>> cardItemsById;
        for (const auto* card : runtime.cards()) {
            if (card->type() == CardType::Application) {
                const auto* applicationCard = static_cast<const ApplicationCard*>(card);
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
                    throw std::runtime_error("Application Card fixed grid is smaller than its content.");
                }
                if (reconciled.placements != applicationCard->itemPlacements()) {
                    (void)runtime.execute(SetApplicationCardLayout{
                        applicationCard->id(), applicationCard->sortMode(), reconciled.placements,
                    });
                }
                auto view = desto::presentation::MakeCardView(*applicationCard);
                cardItemsById[applicationCard->id()] = items;
                view.items = std::move(items);
                cardViews.push_back(std::move(view));
                continue;
            }
            if (card->type() == CardType::Mapping) {
                const auto* mappingCard = static_cast<const MappingCard*>(card);
                auto items = MappingItems(
                    *mappingCard, shellItems, mappingCard->content().itemSize);
                auto view = desto::presentation::MakeCardView(*mappingCard);
                cardItemsById[mappingCard->id()] = items;
                view.items = std::move(items);
                cardViews.push_back(std::move(view));
                continue;
            }
            cardViews.push_back(desto::presentation::MakeCardView(*card));
        }
        WindowsDesktopHost host;
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
                }
            });
        host.setCardExpandedChangedCallback(
            [&](const CardId& cardId, bool expanded) {
                if (runtime.execute(SetCardExpanded{cardId, expanded}).status
                    == CommandStatus::Rejected) {
                    diagnostics.record(DiagnosticLevel::Warning, "desktop.expansion_rejected");
                }
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
                const auto* updated = static_cast<const TodoCard*>(runtime.findCard(cardId));
                const auto item = std::ranges::find(updated->items(), itemId, &TodoItem::id);
                return item == updated->items().end() ? std::nullopt : std::optional<TodoItem>(*item);
            });
        host.setTodoItemRenamedCallback(
            [&](const CardId& cardId, const std::string& itemId, const std::string& title) {
                return runtime.execute(RenameTodoItem{cardId, itemId, title}).status
                    != CommandStatus::Rejected;
            });
        host.setTodoItemCompletedChangedCallback(
            [&](const CardId& cardId, const std::string& itemId, bool completed) {
                return runtime.execute(SetTodoItemCompleted{cardId, itemId, completed}).status
                    != CommandStatus::Rejected;
            });
        host.setTodoItemRemovedCallback(
            [&](const CardId& cardId, const std::string& itemId) {
                return runtime.execute(RemoveTodoItem{cardId, itemId}).status
                    != CommandStatus::Rejected;
            });
        host.setTodoItemsReorderedCallback(
            [&](const CardId& cardId, const std::vector<std::string>& order) {
                return runtime.execute(ReorderTodoItems{cardId, order}).status
                    != CommandStatus::Rejected;
            });
        host.setTodoItemsArchivedCallback([&](const CardId& cardId) {
            return runtime.execute(ArchiveCompletedTodoItems{cardId}).status
                != CommandStatus::Rejected;
        });
        host.setApplicationItemsDroppedCallback(
            [&](const CardId& cardId,
                const std::vector<std::filesystem::path>& paths,
                const std::optional<CardId>& sourceCardId,
                std::size_t insertionIndex,
                std::size_t layoutColumns) {
                if (const auto* mapping = FindMappingCard(runtime, cardId);
                    mapping != nullptr) {
                    try {
                        if (mapping->mode() != MappingMode::Folder) {
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
                            host.updateCardItems(cardId, items);
                            cardItemsById[cardId] = std::move(items);
                            return true;
                        }

                        const auto plan = mappingImportService.plan(*mapping, paths);
                        const auto result = mappingImportService.execute(plan);
                        if (!result.succeeded) {
                            diagnostics.record(
                                DiagnosticLevel::Warning, "desktop.mapping_import_failed");
                            return false;
                        }
                        FileMoveRollbackGuard rollbackGuard(
                            result.completedMoves, diagnostics);
                        std::vector<WindowsDesktopHost::CardItemsUpdate> updates;
                        auto targetItems = MappingItems(
                            *mapping, shellItems, mapping->content().itemSize);
                        updates.push_back({cardId, targetItems});

                        std::optional<std::vector<ApplicationItemPlacement>> previousPlacements;
                        std::optional<std::vector<ApplicationItemPlacement>> nextPlacements;
                        std::vector<desto::presentation::CardItemView> sourceItems;
                        if (sourceCardId.has_value() && *sourceCardId != cardId) {
                            if (const auto* sourceApplication =
                                    FindApplicationCard(runtime, *sourceCardId);
                                sourceApplication != nullptr) {
                                sourceItems = shellItems.enumerate(
                                    storageRoot.resolveCardPath(
                                        sourceApplication->relativeStoragePath()),
                                    PlacementOrder(*sourceApplication),
                                    CardShellIconSource(*sourceApplication));
                                const auto reconciled = ReconcileApplicationItemPlacements(
                                    sourceApplication->itemPlacements(),
                                    ItemFileNames(sourceItems),
                                    CardColumns(runtime, *sourceApplication, sourceItems.size()),
                                    CardMaximumRows(*sourceApplication));
                                if (!reconciled.fits) {
                                    return false;
                                }
                                previousPlacements = sourceApplication->itemPlacements();
                                nextPlacements = reconciled.placements;
                                if (runtime.execute(SetApplicationCardLayout{
                                        *sourceCardId,
                                        sourceApplication->sortMode(),
                                        *nextPlacements}).status == CommandStatus::Rejected) {
                                    return false;
                                }
                                updates.push_back({
                                    *sourceCardId,
                                    sourceItems,
                                    sourceApplication->sortMode(),
                                    *nextPlacements,
                                });
                            } else if (const auto* sourceMapping =
                                           FindMappingCard(runtime, *sourceCardId);
                                       sourceMapping != nullptr) {
                                sourceItems = MappingItems(
                                    *sourceMapping,
                                    shellItems,
                                    sourceMapping->content().itemSize);
                                updates.push_back({*sourceCardId, sourceItems});
                            }
                        }
                        try {
                            host.updateCardItemsBatch(std::move(updates));
                        } catch (...) {
                            if (previousPlacements.has_value()) {
                                const auto* sourceApplication =
                                    FindApplicationCard(runtime, *sourceCardId);
                                if (sourceApplication != nullptr) {
                                    (void)runtime.execute(SetApplicationCardLayout{
                                        *sourceCardId,
                                        sourceApplication->sortMode(),
                                        *previousPlacements,
                                    });
                                }
                            }
                            (void)rollbackGuard.rollbackNow();
                            return false;
                        }
                        cardItemsById[cardId] = std::move(targetItems);
                        if (sourceCardId.has_value() && *sourceCardId != cardId) {
                            cardItemsById[*sourceCardId] = std::move(sourceItems);
                        }
                        rollbackGuard.release();
                        for (const auto& move : result.completedMoves) {
                            shellItems.notifyMoved(move.source, move.destination);
                        }
                        return true;
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
                    auto refreshBatch = ResolveApplicationCardRefreshBatch(
                        cardId, cardLocations, paths);
                    if (sourceCardId.has_value()
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
                    const auto result = importService.execute(plan);
                    if (!result.succeeded) {
                        diagnostics.record(DiagnosticLevel::Warning, "desktop.import_failed");
                        return false;
                    }
                    FileMoveRollbackGuard rollbackGuard(result.completedMoves, diagnostics);
                    for (const auto& move : result.completedMoves) {
                        const auto sourceKey = PathKey(move.source);
                        for (auto& [affectedCardId, items] : nextItemsByCard) {
                            (void)affectedCardId;
                            std::erase_if(items, [&](const auto& item) {
                                return PathKey(item.sourcePath) == sourceKey;
                            });
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
                            placements = MoveApplicationItemsToSlot(
                                placements.placements,
                                movedNames,
                                static_cast<std::uint32_t>(insertionIndex % affectedColumns),
                                static_cast<std::uint32_t>(insertionIndex / affectedColumns),
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
                        }
                    }
                    rollbackGuard.release();
                    for (const auto& move : result.completedMoves) {
                        shellItems.notifyMoved(move.source, move.destination);
                    }
                    return true;
                } catch (...) {
                    diagnostics.record(DiagnosticLevel::Warning, "desktop.import_rejected");
                    return false;
                }
            });
        host.setApplicationItemDragCompletedCallback([&](const CardId& cardId) {
            const auto* card = FindApplicationCard(runtime, cardId);
            if (card == nullptr) {
                return;
            }
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
            cardItemsById[cardId] = std::move(items);
        });
        host.setCardItemActivatedCallback(
            [&](const CardId&, const desto::presentation::CardItemView& item) {
                if (!shellItems.launch(item)) {
                    diagnostics.record(DiagnosticLevel::Warning, "desktop.item_launch_failed");
                }
            });
        host.setCardItemsRefreshCallback(
            [&](const CardId& cardId, CardItemSize itemSize) {
                if (const auto* mapping = FindMappingCard(runtime, cardId);
                    mapping != nullptr) {
                    auto items = MappingItems(*mapping, shellItems, itemSize);
                    cardItemsById[cardId] = items;
                    return items;
                }
                const auto* card = FindApplicationCard(runtime, cardId);
                if (card == nullptr) {
                    return std::vector<desto::presentation::CardItemView>{};
                }
                return shellItems.enumerate(
                    storageRoot.resolveCardPath(card->relativeStoragePath()),
                    PlacementOrder(*card),
                    ResolveShellIconSourceSize(itemSize));
            });
        host.present(runtime.projections(), displays, cardViews);
        std::vector<DirectoryMappingWatch> mappingWatches;
        for (const auto* card : runtime.cards()) {
            if (card->type() != CardType::Mapping) {
                continue;
            }
            const auto* mapping = static_cast<const MappingCard*>(card);
            if (mapping->mode() == MappingMode::Folder) {
                mappingWatches.push_back({mapping->id(), mapping->sourceRoot()});
            }
        }
        WindowsDirectoryChangeSource directoryChanges(
            std::move(mappingWatches),
            [&](std::vector<CardId> changedCards) {
                for (const auto& cardId : changedCards) {
                    host.requestCardItemsRefresh(cardId);
                }
            });
        directoryChanges.start();
        if (!lifecycle.runtimeReady().applied) {
            throw std::runtime_error("Runtime lifecycle transition failed.");
        }
        host.run(DurationMilliseconds(commandLine));
        if (!lifecycle.requestShutdown(ShutdownReason::User).applied) {
            throw std::runtime_error("Shutdown request transition failed.");
        }
        if (!lifecycle.completeShutdown().applied) {
            throw std::runtime_error("Shutdown completion transition failed.");
        }
        return 0;
    } catch (...) {
        (void)lifecycle.fail();
        return 1;
    }
}
