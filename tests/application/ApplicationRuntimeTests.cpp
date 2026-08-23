#include "ApplicationRuntime.h"
#include "ApplicationCardOrdering.h"
#include "TestSupport.h"

#include <array>
#include <filesystem>

using namespace desto::application;
using namespace desto::domain;

namespace {

void RunTests() {
    ApplicationRuntime runtime;

    const auto applicationCreated = runtime.execute(CreateApplicationCard{
        .cardId = "application-1",
        .relativeStoragePath = "cards/application-1",
    });
    DESTO_CHECK(applicationCreated.status == CommandStatus::Applied);
    DESTO_CHECK(applicationCreated.revision == 1);
    DESTO_CHECK(applicationCreated.changes.createdCards == std::vector<CardId>{"application-1"});
    DESTO_CHECK(applicationCreated.changes.persistence == PersistenceUrgency::Deferred);

    DESTO_CHECK(runtime.execute(CreateMappingCard{"mapping-1"}).status == CommandStatus::Applied);
    DESTO_CHECK(runtime.execute(CreateTodoCard{"todo-1"}).status == CommandStatus::Applied);
    DESTO_CHECK(runtime.cards().size() == 3);
    DESTO_CHECK(runtime.cards()[0]->id() == "application-1");
    DESTO_CHECK(runtime.cards()[1]->id() == "mapping-1");
    DESTO_CHECK(runtime.cards()[2]->id() == "todo-1");
    DESTO_CHECK(runtime.execute(RenameCard{"todo-1", "今天要做"}).status
                == CommandStatus::Applied);
    DESTO_CHECK(runtime.findCard("todo-1")->name() == "今天要做");
    DESTO_CHECK(runtime.execute(RenameCard{"todo-1", ""}).status
                == CommandStatus::Rejected);

    DESTO_CHECK(runtime.execute(SetCardChromePreferences{
        "application-1",
        {.showCollapseControl = false, .showCloseControl = false,
         .showPinControl = false, .showTitle = true},
    }).status == CommandStatus::Applied);
    DESTO_CHECK(!runtime.findCard("application-1")->chrome().showCollapseControl);
    DESTO_CHECK(runtime.execute(SetCardChromePreferences{
        "application-1", {.showCollapseControl = false, .showCloseControl = false,
         .showPinControl = true, .pinOnTop = true, .showTitle = true,
         .positionLocked = true},
    }).status == CommandStatus::Applied);
    DESTO_CHECK(runtime.findCard("application-1")->chrome().pinOnTop);
    DESTO_CHECK(runtime.findCard("application-1")->chrome().positionLocked);
    DESTO_CHECK(runtime.execute(SetCardAppearancePreferences{
        "application-1", {.preset = "jewel", .opacity = 0.92, .cornerRadius = 20.0},
    }).status == CommandStatus::Applied);
    DESTO_CHECK(runtime.findCard("application-1")->appearance().preset == "jewel");
    DESTO_CHECK(runtime.execute(SetCardAppearancePreferences{
        "application-1", {.preset = "jewel", .opacity = 2.0, .cornerRadius = 20.0},
    }).status == CommandStatus::Rejected);

    auto todoResult = runtime.execute(AddTodoItem{"todo-1", "todo-a", "First task"});
    DESTO_CHECK(todoResult.status == CommandStatus::Applied);
    DESTO_CHECK(todoResult.changes.changedCards == std::vector<CardId>{"todo-1"});
    DESTO_CHECK(todoResult.changes.persistence == PersistenceUrgency::Deferred);
    DESTO_CHECK(runtime.execute(AddTodoItem{"todo-1", "todo-b", "Second task"}).status
                == CommandStatus::Applied);
    const auto* todo = static_cast<const TodoCard*>(runtime.findCard("todo-1"));
    DESTO_CHECK(todo->items().size() == 2);
    DESTO_CHECK(todo->items()[0].title == "First task");
    DESTO_CHECK(todo->items()[0].createdAtUnixMilliseconds > 0);
    DESTO_CHECK(todo->items()[0].scheduledDate.has_value());

    const auto duplicateTodo = runtime.execute(AddTodoItem{"todo-1", "todo-a", "Duplicate"});
    DESTO_CHECK(duplicateTodo.status == CommandStatus::Rejected);
    DESTO_CHECK(duplicateTodo.error == CommandError::DuplicateTodoItemId);
    DESTO_CHECK(todo->items().size() == 2);
    DESTO_CHECK(runtime.execute(AddTodoItem{"todo-1", "blank", "  "}).status
                == CommandStatus::Rejected);
    DESTO_CHECK(todo->items().size() == 2);

    DESTO_CHECK(runtime.execute(RenameTodoItem{"todo-1", "todo-a", "Renamed"}).status
                == CommandStatus::Applied);
    DESTO_CHECK(todo->items()[0].title == "Renamed");
    DESTO_CHECK(runtime.execute(RenameTodoItem{"todo-1", "missing", "Nope"}).error
                == CommandError::TodoItemNotFound);
    DESTO_CHECK(runtime.execute(SetTodoItemCompleted{"todo-1", "todo-a", true}).status
                == CommandStatus::Applied);
    DESTO_CHECK(todo->items()[0].completed);
    DESTO_CHECK(todo->items()[0].completedAtUnixMilliseconds > 0);
    DESTO_CHECK(runtime.execute(SetTodoItemCompleted{"todo-1", "todo-a", true}).status
                == CommandStatus::NoChange);
    DESTO_CHECK(todo->items()[1].id == "todo-b");

    DESTO_CHECK(runtime.execute(ArchiveCompletedTodoItems{"todo-1"}).status
                == CommandStatus::Applied);
    DESTO_CHECK(todo->items()[0].archived == true);
    DESTO_CHECK(todo->items()[1].archived == false);
    DESTO_CHECK(runtime.execute(RestoreArchivedTodoItems{"todo-1", 2'000}).status
                == CommandStatus::Applied);
    DESTO_CHECK(!todo->items()[1].archived);
    DESTO_CHECK(todo->items()[0].completedAtUnixMilliseconds == 2'000);
    DESTO_CHECK(runtime.execute(ArchiveCompletedTodoItems{"todo-1"}).status
                == CommandStatus::Applied);
    DESTO_CHECK(runtime.execute(RestoreArchivedTodoItem{"todo-1", "todo-a", 3'000}).status
                == CommandStatus::Applied);
    DESTO_CHECK(!todo->items()[0].archived);
    DESTO_CHECK(todo->items()[0].completedAtUnixMilliseconds == 3'000);
    DESTO_CHECK(runtime.execute(ArchiveTodoItem{"todo-1", "todo-a"}).status
                == CommandStatus::Applied);
    DESTO_CHECK(todo->items()[0].archived);
    DESTO_CHECK(runtime.execute(ArchiveTodoItem{"todo-1", "todo-b"}).status
                == CommandStatus::NoChange);
    DESTO_CHECK(runtime.execute(SetTodoCardPreferences{
        "todo-1", {.showCreatedTime = true}}).status == CommandStatus::Applied);
    DESTO_CHECK(todo->preferences().showCreatedTime);
    DESTO_CHECK(runtime.execute(SetTodoItemCompleted{"todo-1", "todo-a", false}).status
                == CommandStatus::Applied);
    DESTO_CHECK(!todo->items()[0].completed);
    DESTO_CHECK(todo->items()[0].completedAtUnixMilliseconds == 0);

    DESTO_CHECK(runtime.execute(ReorderTodoItems{
        "todo-1", {"todo-b", "todo-a"}}).status == CommandStatus::Applied);
    DESTO_CHECK(todo->items()[0].id == "todo-b");
    const auto invalidOrder = runtime.execute(ReorderTodoItems{
        "todo-1", {"todo-b", "todo-b"}});
    DESTO_CHECK(invalidOrder.status == CommandStatus::Rejected);
    DESTO_CHECK(todo->items()[0].id == "todo-b");
    DESTO_CHECK(runtime.execute(RemoveTodoItem{"todo-1", "todo-a"}).status
                == CommandStatus::Applied);
    DESTO_CHECK(todo->items().size() == 1);
    DESTO_CHECK(todo->items().front().id == "todo-b");
    DESTO_CHECK(todo->items().front().title == "Second task");
    DESTO_CHECK(!todo->items().front().completed);

    ApplicationRuntime todoRestore;
    todoRestore.restore(runtime.cardSnapshots(), runtime.workspace());
    const auto* restoredTodo = static_cast<const TodoCard*>(todoRestore.findCard("todo-1"));
    DESTO_CHECK(restoredTodo->items() == todo->items());

    ApplicationRuntime historyRuntime;
    DESTO_CHECK(historyRuntime.execute(CreateTodoCard{"history-card"}).status
                == CommandStatus::Applied);
    DESTO_CHECK(historyRuntime.execute(AddHistoricalArchivedTodoItem{
        "history-card", "history-1", "Past task", 1'722'513'600'000,
        {2024, 8, 1}}).status == CommandStatus::Applied);
    const auto* historyCard = static_cast<const TodoCard*>(
        historyRuntime.findCard("history-card"));
    DESTO_CHECK(historyCard->items().size() == 1);
    DESTO_CHECK(historyCard->items()[0].completed);
    DESTO_CHECK(historyCard->items()[0].archived);
    DESTO_CHECK(historyCard->items()[0].completedAtUnixMilliseconds
                == 1'722'513'600'000);
    DESTO_CHECK(historyCard->items()[0].scheduledDate == TodoDate(2024, 8, 1));
    DESTO_CHECK(historyRuntime.execute(AddHistoricalArchivedTodoItem{
        "history-card", "history-2", "Invalid", 0, {2024, 8, 1}}).status
                == CommandStatus::Rejected);

    const auto mappingRoot =
        (std::filesystem::temp_directory_path() / "DestoRuntimeMappingSource").lexically_normal();
    const auto mappingFolder = runtime.execute(SetMappingFolderSource{
        .cardId = "mapping-1",
        .sourceRoot = mappingRoot,
    });
    DESTO_CHECK(mappingFolder.status == CommandStatus::Applied);
    const auto* mapping = static_cast<const MappingCard*>(runtime.findCard("mapping-1"));
    DESTO_CHECK(mapping->mode() == MappingMode::Folder);
    DESTO_CHECK(mapping->sourceRoot() == mappingRoot);
    DESTO_CHECK(mapping->allowsSourceMutation());
    DESTO_CHECK(runtime.execute(SetMappingFolderSource{"mapping-1", mappingRoot / "."}).status
                == CommandStatus::NoChange);

    DESTO_CHECK(runtime.execute(CreateMappingCard{"mapping-2"}).status
                == CommandStatus::Applied);
    const auto duplicateMapping = runtime.execute(SetMappingFolderSource{
        .cardId = "mapping-2",
        .sourceRoot = mappingRoot / ".",
    });
    DESTO_CHECK(duplicateMapping.status == CommandStatus::Rejected);
    DESTO_CHECK(duplicateMapping.error == CommandError::MappingSourceAlreadyMapped);

    const std::vector<FileReference> references{
        {"editor", mappingRoot / "Editor.exe"},
        {"notes", mappingRoot / "Notes.txt"},
    };
    DESTO_CHECK(runtime.execute(SetMappingReferences{"mapping-1", references}).status
                == CommandStatus::Applied);
    DESTO_CHECK(mapping->mode() == MappingMode::References);
    DESTO_CHECK(mapping->references() == references);
    DESTO_CHECK(!mapping->allowsSourceMutation());
    DESTO_CHECK(runtime.execute(SetMappingFolderSource{"mapping-2", mappingRoot}).status
                == CommandStatus::Applied);
    DESTO_CHECK(static_cast<const MappingCard*>(runtime.findCard("mapping-2"))
                ->allowsSourceMutation());
    DESTO_CHECK(runtime.execute(SetMappingSourceMutation{"mapping-2", false}).status
                == CommandStatus::Applied);
    const auto* secondMapping = static_cast<const MappingCard*>(runtime.findCard("mapping-2"));
    DESTO_CHECK(!secondMapping->allowsSourceMutation());
    DESTO_CHECK(runtime.execute(SetMappingSourceMutation{"mapping-2", false}).status
                == CommandStatus::NoChange);

    const auto invalidReferences = runtime.execute(SetMappingReferences{
        .cardId = "mapping-1",
        .references = {{"", mappingRoot / "Invalid.txt"}},
    });
    DESTO_CHECK(invalidReferences.status == CommandStatus::Rejected);
    DESTO_CHECK(invalidReferences.error == CommandError::InvalidCommand);
    DESTO_CHECK(mapping->references() == references);
    DESTO_CHECK(runtime.execute(ClearMappingSource{"mapping-2"}).status
                == CommandStatus::Applied);
    DESTO_CHECK(secondMapping->mode() == MappingMode::Folder);
    DESTO_CHECK(runtime.execute(SetMappingMode{"mapping-2", MappingMode::References}).status
        == CommandStatus::Applied);
    DESTO_CHECK(secondMapping->mode() == MappingMode::References);
    DESTO_CHECK(secondMapping->references().empty());
    DESTO_CHECK(runtime.execute(SetMappingPresentationMode{
        "mapping-2", MappingPresentationMode::List}).status == CommandStatus::Applied);
    DESTO_CHECK(secondMapping->presentationMode() == MappingPresentationMode::List);
    DESTO_CHECK(runtime.execute(SetMappingCardLayout{
        "mapping-2", ApplicationItemSortMode::ModifiedDate,
        {{mappingRoot / "Second.txt", 1, 0}}}).status
                == CommandStatus::Applied);
    DESTO_CHECK(secondMapping->sortMode() == ApplicationItemSortMode::ModifiedDate);
    DESTO_CHECK(secondMapping->itemPlacements().size() == 1);
    const auto snapshotsAfterMappingPreferences = runtime.cardSnapshots();
    const auto mappingSnapshot = std::ranges::find(
        snapshotsAfterMappingPreferences, "mapping-2", &CardSnapshot::id);
    DESTO_CHECK(mappingSnapshot != snapshotsAfterMappingPreferences.end());
    DESTO_CHECK(mappingSnapshot->mappingMode == MappingMode::References);
    DESTO_CHECK(mappingSnapshot->mappingPresentationMode == MappingPresentationMode::List);
    DESTO_CHECK(mappingSnapshot->mappingSortMode == ApplicationItemSortMode::ModifiedDate);
    DESTO_CHECK(mappingSnapshot->mappingItemPlacements.size() == 1);
    DESTO_CHECK(mappingSnapshot->mappingItemPlacements.front().column == 1);
    DESTO_CHECK(runtime.execute(ClearMappingSource{"mapping-2"}).status
                == CommandStatus::NoChange);

    ApplicationRuntime duplicateRestoreRuntime;
    const std::vector<CardSnapshot> duplicateMappingSnapshots{
        {.id = "restored-mapping-1", .type = CardType::Mapping,
         .mappingSourceRoot = mappingRoot},
        {.id = "restored-mapping-2", .type = CardType::Mapping,
         .mappingSourceRoot = mappingRoot / "."},
    };
    bool duplicateRestoreRejected = false;
    try {
        duplicateRestoreRuntime.restore(duplicateMappingSnapshots, {});
    } catch (const std::invalid_argument&) {
        duplicateRestoreRejected = true;
    }
    DESTO_CHECK(duplicateRestoreRejected);
    DESTO_CHECK(duplicateRestoreRuntime.cards().empty());

    const auto contentChanged = runtime.execute(SetCardContentPreferences{
        .cardId = "application-1",
        .preferences = {.itemSize = CardItemSize::ExtraLarge, .showItemNames = false},
    });
    DESTO_CHECK(contentChanged.status == CommandStatus::Applied);
    DESTO_CHECK(runtime.findCard("application-1")->content().itemSize
                == CardItemSize::ExtraLarge);
    DESTO_CHECK(!runtime.findCard("application-1")->content().showItemNames);
    DESTO_CHECK(runtime.execute(SetApplicationCardLayout{
        "application-1",
        ApplicationItemSortMode::Custom,
        {{"Editor.lnk", 0, 0}, {"Browser.lnk", 2, 0}}}).status == CommandStatus::Applied);
    const auto* application = static_cast<const ApplicationCard*>(
        runtime.findCard("application-1"));
    DESTO_CHECK(application->itemPlacements().front().fileName == "Editor.lnk");
    DESTO_CHECK(application->itemPlacements()[1].column == 2);
    DESTO_CHECK(runtime.execute(SetApplicationPresentationMode{
        "application-1", MappingPresentationMode::List}).status
        == CommandStatus::Applied);
    DESTO_CHECK(application->presentationMode() == MappingPresentationMode::List);
    const auto tooSmallGrid = runtime.execute(SetCardContentPreferences{
        .cardId = "application-1",
        .preferences = {
            .itemSize = CardItemSize::ExtraLarge,
            .showItemNames = true,
            .sizeMode = CardSizeMode::Fixed,
            .widthSpan = 2,
            .fixedColumns = 2,
            .fixedRows = 1,
        },
    });
    DESTO_CHECK(tooSmallGrid.status == CommandStatus::Rejected);
    DESTO_CHECK(application->content().sizeMode == CardSizeMode::Adaptive);
    const auto invalidLayout = runtime.execute(SetApplicationCardLayout{
        "application-1",
        ApplicationItemSortMode::Name,
        {{"Editor.lnk", 0, 0}, {"Browser.lnk", 0, 0}},
    });
    DESTO_CHECK(invalidLayout.status == CommandStatus::Rejected);
    DESTO_CHECK(application->sortMode() == ApplicationItemSortMode::Custom);
    DESTO_CHECK(application->itemPlacements()[1].column == 2);

    const auto revisionBeforeDuplicate = runtime.revision();
    const auto duplicate = runtime.execute(CreateTodoCard{"todo-1"});
    DESTO_CHECK(duplicate.status == CommandStatus::Rejected);
    DESTO_CHECK(duplicate.error == CommandError::DuplicateCardId);
    DESTO_CHECK(duplicate.revision == revisionBeforeDuplicate);

    const auto invalid = runtime.execute(CreateApplicationCard{
        .cardId = "invalid",
        .relativeStoragePath = "C:/absolute",
    });
    DESTO_CHECK(invalid.status == CommandStatus::Rejected);
    DESTO_CHECK(invalid.error == CommandError::InvalidCommand);
    DESTO_CHECK(runtime.findCard("invalid") == nullptr);

    const std::vector<DisplaySnapshot> twoDisplays{
        {.id = "display-a", .workAreaWidth = 1280, .workAreaHeight = 720},
        {.id = "display-b", .workAreaWidth = 1920, .workAreaHeight = 1040, .primary = true},
    };
    const auto topology = runtime.execute(UpdateDisplayTopology{twoDisplays});
    DESTO_CHECK(topology.status == CommandStatus::Applied);
    DESTO_CHECK(topology.changes.displayTopologyChanged);
    DESTO_CHECK(!topology.changes.projectionsChanged);
    DESTO_CHECK(topology.changes.persistence == PersistenceUrgency::None);

    const CardPlacement placement{
        .id = "placement-1",
        .cardId = "application-1",
        .target = DisplayTarget::specific("display-a"),
        .rect = {40, 50, 320, 220},
    };
    const auto placed = runtime.execute(SetPlacement{placement});
    DESTO_CHECK(placed.status == CommandStatus::Applied);
    DESTO_CHECK(placed.changes.layoutChanged);
    DESTO_CHECK(placed.changes.projectionsChanged);
    DESTO_CHECK(placed.changes.persistence == PersistenceUrgency::Deferred);
    DESTO_CHECK(runtime.projections().size() == 1);
    DESTO_CHECK(runtime.projections().front().displayId == "display-a");

    const auto placementNoChange = runtime.execute(SetPlacement{placement});
    DESTO_CHECK(placementNoChange.status == CommandStatus::NoChange);
    const auto revisionBeforeVisibility = runtime.revision();
    const auto hidden = runtime.execute(SetCardVisibility{"application-1", false});
    DESTO_CHECK(hidden.status == CommandStatus::Applied);
    DESTO_CHECK(hidden.changes.changedCards == std::vector<CardId>{"application-1"});
    DESTO_CHECK(!runtime.findCard("application-1")->isVisible());
    DESTO_CHECK(runtime.revision() == revisionBeforeVisibility + 1);
    DESTO_CHECK(runtime.execute(SetCardVisibility{"application-1", false}).status
                == CommandStatus::NoChange);

    const auto collapsed = runtime.execute(SetCardExpanded{"todo-1", false});
    DESTO_CHECK(collapsed.status == CommandStatus::Applied);
    DESTO_CHECK(!runtime.findCard("todo-1")->isExpanded());
    DESTO_CHECK(runtime.execute(SetCardExpanded{"todo-1", false}).status
                == CommandStatus::NoChange);

    const auto revisionBeforeInvalidPlacement = runtime.revision();
    const auto invalidPlacement = runtime.execute(SetPlacement{{
        .id = "invalid-placement",
        .cardId = "todo-1",
        .target = DisplayTarget::all(),
        .rect = {0, 0, 0, 220},
    }});
    DESTO_CHECK(invalidPlacement.status == CommandStatus::Rejected);
    DESTO_CHECK(invalidPlacement.error == CommandError::InvalidCommand);
    DESTO_CHECK(runtime.revision() == revisionBeforeInvalidPlacement);
    DESTO_CHECK(runtime.workspace().placements().size() == 1);

    const auto missingCardPlacement = runtime.execute(SetPlacement{{
        .id = "missing-card-placement",
        .cardId = "missing-card",
        .target = DisplayTarget::all(),
    }});
    DESTO_CHECK(missingCardPlacement.status == CommandStatus::Rejected);
    DESTO_CHECK(missingCardPlacement.error == CommandError::CardNotFound);

    const auto disconnectedTopology = runtime.execute(UpdateDisplayTopology{{
        {.id = "display-b", .workAreaWidth = 1920, .workAreaHeight = 1040, .primary = true},
    }});
    DESTO_CHECK(disconnectedTopology.status == CommandStatus::Applied);
    DESTO_CHECK(disconnectedTopology.changes.projectionsChanged);
    DESTO_CHECK(runtime.projections().empty());
    DESTO_CHECK(runtime.workspace().unavailablePlacements(runtime.displays()).size() == 1);
    DESTO_CHECK(runtime.workspace().placements().front().target.displayId() == "display-a");

    const auto revisionBeforeRepeatedTopology = runtime.revision();
    const auto repeatedTopology = runtime.execute(UpdateDisplayTopology{{
        {.id = "display-b", .workAreaWidth = 1920, .workAreaHeight = 1040, .primary = true},
    }});
    DESTO_CHECK(repeatedTopology.status == CommandStatus::NoChange);
    DESTO_CHECK(runtime.revision() == revisionBeforeRepeatedTopology);

    const auto revisionBeforeInvalidTopology = runtime.revision();
    const auto invalidTopology = runtime.execute(UpdateDisplayTopology{{
        {.id = "duplicate", .workAreaWidth = 100, .workAreaHeight = 100},
        {.id = "duplicate", .workAreaWidth = 100, .workAreaHeight = 100},
    }});
    DESTO_CHECK(invalidTopology.status == CommandStatus::Rejected);
    DESTO_CHECK(invalidTopology.error == CommandError::InvalidCommand);
    DESTO_CHECK(runtime.revision() == revisionBeforeInvalidTopology);
    DESTO_CHECK(runtime.displays().size() == 1);

    const auto deletion = runtime.execute(RequestCardDeletion{"application-1"});
    DESTO_CHECK(deletion.status == CommandStatus::Applied);
    DESTO_CHECK(deletion.changes.deletionRequest.has_value());
    DESTO_CHECK(deletion.changes.deletionRequest->preview.effect
                == CardDeletionEffect::ReturnManagedItemsToDesktop);
    DESTO_CHECK(runtime.findCard("application-1") != nullptr);

    const auto token = deletion.changes.deletionRequest->token;
    const auto duplicateDeletion = runtime.execute(RequestCardDeletion{"application-1"});
    DESTO_CHECK(duplicateDeletion.error == CommandError::DeletionAlreadyPending);
    const auto wrongToken = runtime.execute(CommitCardDeletion{"application-1", token + 1});
    DESTO_CHECK(wrongToken.status == CommandStatus::Rejected);
    DESTO_CHECK(wrongToken.error == CommandError::DeletionTokenMismatch);
    DESTO_CHECK(runtime.findCard("application-1") != nullptr);

    const auto committed = runtime.execute(CommitCardDeletion{"application-1", token});
    DESTO_CHECK(committed.status == CommandStatus::Applied);
    DESTO_CHECK(committed.changes.removedCards == std::vector<CardId>{"application-1"});
    DESTO_CHECK(committed.changes.layoutChanged);
    DESTO_CHECK(!committed.changes.projectionsChanged);
    DESTO_CHECK(committed.changes.persistence == PersistenceUrgency::Immediate);
    DESTO_CHECK(runtime.findCard("application-1") == nullptr);
    DESTO_CHECK(runtime.workspace().placements().empty());
    DESTO_CHECK(runtime.projections().empty());

    const auto mappingDeletion = runtime.execute(RequestCardDeletion{"mapping-1"});
    DESTO_CHECK(mappingDeletion.changes.deletionRequest->preview.effect
                == CardDeletionEffect::RemoveCardOnly);
    const auto mappingToken = mappingDeletion.changes.deletionRequest->token;
    const auto wrongCancel = runtime.execute(CancelCardDeletion{"mapping-1", mappingToken + 1});
    DESTO_CHECK(wrongCancel.status == CommandStatus::Rejected);
    DESTO_CHECK(wrongCancel.error == CommandError::DeletionTokenMismatch);
    DESTO_CHECK(runtime.pendingDeletion("mapping-1").has_value());
    DESTO_CHECK(runtime.execute(CancelCardDeletion{"mapping-1", mappingToken}).status
                == CommandStatus::Applied);
    DESTO_CHECK(!runtime.pendingDeletion("mapping-1").has_value());
    DESTO_CHECK(runtime.findCard("mapping-1") != nullptr);

    DESTO_CHECK(runtime.execute(SetMappingFolderSource{"mapping-2", mappingRoot}).status
                == CommandStatus::Applied);
    const auto secondMappingDeletion = runtime.execute(RequestCardDeletion{"mapping-2"});
    DESTO_CHECK(secondMappingDeletion.status == CommandStatus::Applied);
    DESTO_CHECK(runtime.execute(CommitCardDeletion{
        "mapping-2", secondMappingDeletion.changes.deletionRequest->token}).status
                == CommandStatus::Applied);
    DESTO_CHECK(runtime.execute(CreateMappingCard{"mapping-3"}).status
                == CommandStatus::Applied);
    DESTO_CHECK(runtime.execute(SetMappingFolderSource{"mapping-3", mappingRoot}).status
                == CommandStatus::Applied);

    const auto missingPlacement = runtime.execute(RemovePlacement{"missing"});
    DESTO_CHECK(missingPlacement.status == CommandStatus::Rejected);
    DESTO_CHECK(missingPlacement.error == CommandError::PlacementNotFound);

    ApplicationRuntime anchoredRuntime;
    DESTO_CHECK(anchoredRuntime.execute(CreateApplicationCard{
        .cardId = "anchored",
        .relativeStoragePath = "cards/anchored",
    }).status == CommandStatus::Applied);
    DESTO_CHECK(anchoredRuntime.execute(UpdateDisplayTopology{{
        {.id = "display-a", .workAreaWidth = 1280, .workAreaHeight = 720, .primary = true},
    }}).status == CommandStatus::Applied);
    const CardPlacement edgePlacement{
        .id = "placement-anchored",
        .cardId = "anchored",
        .target = DisplayTarget::specific("display-a"),
        .rect = {960, 200, 320, 220},
        .horizontalAnchor = PlacementHorizontalAnchor::Left,
        .verticalAnchor = PlacementVerticalAnchor::Free,
        .referenceWorkAreaWidth = 1280,
        .referenceWorkAreaHeight = 720,
    };
    DESTO_CHECK(anchoredRuntime.execute(SetPlacement{edgePlacement}).status
                == CommandStatus::Applied);
    auto snappedPlacement = edgePlacement;
    snappedPlacement.horizontalAnchor = PlacementHorizontalAnchor::Right;
    snappedPlacement.verticalAnchor = PlacementVerticalAnchor::Bottom;
    const auto snapped = anchoredRuntime.execute(SetPlacement{snappedPlacement});
    DESTO_CHECK(snapped.status == CommandStatus::Applied);
    DESTO_CHECK(snapped.changes.layoutChanged);
    DESTO_CHECK(anchoredRuntime.workspace().placements().front().horizontalAnchor
                == PlacementHorizontalAnchor::Right);
    DESTO_CHECK(anchoredRuntime.workspace().placements().front().verticalAnchor
                == PlacementVerticalAnchor::Bottom);
    DESTO_CHECK(anchoredRuntime.execute(SetPlacement{snappedPlacement}).status
                == CommandStatus::NoChange);
    DESTO_CHECK(anchoredRuntime.execute(UpdateDisplayTopology{{
        {.id = "display-a", .workAreaWidth = 1920, .workAreaHeight = 1080, .primary = true},
    }}).status == CommandStatus::Applied);
    DESTO_CHECK(anchoredRuntime.projections().size() == 1);
    DESTO_CHECK(anchoredRuntime.projections().front().rect.left == 1600);
    DESTO_CHECK(anchoredRuntime.projections().front().rect.top == 560);

    const auto exportText = FormatTodoArchiveExport(std::array{
        TodoArchiveExportEntry{{2026, 3, 2}, 20, "later"},
        TodoArchiveExportEntry{{2026, 3, 1}, 30, "second"},
        TodoArchiveExportEntry{{2026, 3, 1}, 10, "first"},
    });
    DESTO_CHECK(exportText.find(
        "2026-03-01\r\nfirst\r\nsecond\r\n\r\n2026-03-02\r\nlater\r\n")
        != std::string::npos);
}

} // namespace

int main() {
    return desto::test::Run(RunTests);
}
