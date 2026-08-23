#include "ApplicationRuntime.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace desto::application {
namespace {

template <typename CardType, typename... Arguments>
std::unique_ptr<domain::Card> CreateCard(Arguments&&... arguments) {
    return std::make_unique<CardType>(std::forward<Arguments>(arguments)...);
}

std::int64_t UnixMillisecondsNow() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool SameDisplays(
    const std::vector<domain::DisplaySnapshot>& left,
    const std::vector<domain::DisplaySnapshot>& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto& a = left[index];
        const auto& b = right[index];
        if (a.id != b.id || a.workAreaLeft != b.workAreaLeft || a.workAreaTop != b.workAreaTop
            || a.workAreaWidth != b.workAreaWidth || a.workAreaHeight != b.workAreaHeight
            || a.effectiveDpi != b.effectiveDpi || a.primary != b.primary) {
            return false;
        }
    }
    return true;
}

bool SameProjections(
    const std::vector<domain::PlacementProjection>& left,
    const std::vector<domain::PlacementProjection>& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto& a = left[index];
        const auto& b = right[index];
        if (a.placementId != b.placementId || a.cardId != b.cardId
            || a.requestedDisplayId != b.requestedDisplayId || a.displayId != b.displayId
            || a.rect.left != b.rect.left || a.rect.top != b.rect.top
            || a.rect.width != b.rect.width || a.rect.height != b.rect.height
            || a.zIndex != b.zIndex
            || a.horizontalAnchor != b.horizontalAnchor
            || a.verticalAnchor != b.verticalAnchor
            || a.fallback != b.fallback) {
            return false;
        }
    }
    return true;
}

std::unique_ptr<domain::Card> RestoreCard(const domain::CardSnapshot& snapshot) {
    if (snapshot.id.empty()) {
        throw std::invalid_argument("Card snapshot id must not be empty.");
    }

    std::unique_ptr<domain::Card> card;
    switch (snapshot.type) {
    case domain::CardType::Application:
        card = std::make_unique<domain::ApplicationCard>(
            snapshot.id,
            snapshot.applicationStoragePath);
        static_cast<domain::ApplicationCard*>(card.get())->setLayout(
            snapshot.applicationSortMode,
            snapshot.applicationItemPlacements);
        static_cast<domain::ApplicationCard*>(card.get())->setPresentationMode(
            snapshot.applicationPresentationMode);
        break;
    case domain::CardType::Mapping: {
        if (!snapshot.mappingSourceRoot.empty() && !snapshot.mappingReferences.empty()) {
            throw std::invalid_argument("Mapping snapshot cannot contain a folder and references.");
        }
        for (const auto& reference : snapshot.mappingReferences) {
            if (reference.id.empty() || reference.path.empty()) {
                throw std::invalid_argument("Mapping reference must have an id and path.");
            }
        }
        auto mapping = std::make_unique<domain::MappingCard>(snapshot.id);
        if (!snapshot.mappingSourceRoot.empty()) {
            mapping->setFolderSource(snapshot.mappingSourceRoot);
        } else if (!snapshot.mappingReferences.empty()) {
            mapping->setReferences(snapshot.mappingReferences);
        } else {
            mapping->setMode(snapshot.mappingMode);
        }
        mapping->setAllowsSourceMutation(snapshot.mappingAllowsSourceMutation);
        mapping->setPresentationMode(snapshot.mappingPresentationMode);
        mapping->setLayout(
            snapshot.mappingSortMode,
            snapshot.mappingItemPlacements);
        card = std::move(mapping);
        break;
    }
    case domain::CardType::Todo:
        for (const auto& item : snapshot.todoItems) {
            if (item.id.empty() || item.title.empty()) {
                throw std::invalid_argument("Todo item must have an id and title.");
            }
        }
        {
            auto todo = std::make_unique<domain::TodoCard>(snapshot.id);
            todo->setItems(snapshot.todoItems);
            todo->setPreferences(snapshot.todoPreferences);
            card = std::move(todo);
        }
        break;
    }

    if (card->type() == domain::CardType::Application) {
        static_cast<const domain::ApplicationCard&>(*card)
            .validateContentPreferences(snapshot.content);
    }
    card->setVisible(snapshot.visible);
    if (!snapshot.name.empty()) card->setName(snapshot.name);
    card->setExpanded(snapshot.expanded);
    card->setChrome(snapshot.chrome);
    card->setAppearance(snapshot.appearance);
    card->setContent(snapshot.content);
    return card;
}

} // namespace

CommandResult ApplicationRuntime::execute(const ApplicationCommand& command) {
    try {
        return std::visit(
            [this](const auto& value) { return handle(value); },
            command);
    } catch (const std::invalid_argument& error) {
        return rejected(CommandError::InvalidCommand, error.what());
    }
}

const domain::Card* ApplicationRuntime::findCard(
    const domain::CardId& cardId) const noexcept {
    const auto card = cards_.find(cardId);
    return card == cards_.end() ? nullptr : card->second.get();
}

std::vector<const domain::Card*> ApplicationRuntime::cards() const {
    std::vector<const domain::Card*> result;
    result.reserve(cards_.size());
    for (const auto& [id, card] : cards_) {
        result.push_back(card.get());
    }
    std::sort(
        result.begin(),
        result.end(),
        [](const domain::Card* left, const domain::Card* right) {
            return left->id() < right->id();
        });
    return result;
}

std::vector<domain::CardSnapshot> ApplicationRuntime::cardSnapshots() const {
    const auto orderedCards = cards();
    std::vector<domain::CardSnapshot> result;
    result.reserve(orderedCards.size());
    for (const auto* card : orderedCards) {
        domain::CardSnapshot snapshot{
            .id = card->id(),
            .name = card->name(),
            .type = card->type(),
            .visible = card->isVisible(),
            .expanded = card->isExpanded(),
            .chrome = card->chrome(),
            .appearance = card->appearance(),
            .content = card->content(),
        };
        switch (card->type()) {
        case domain::CardType::Application:
            snapshot.applicationStoragePath =
                static_cast<const domain::ApplicationCard*>(card)->relativeStoragePath();
            snapshot.applicationSortMode =
                static_cast<const domain::ApplicationCard*>(card)->sortMode();
            snapshot.applicationItemPlacements =
                static_cast<const domain::ApplicationCard*>(card)->itemPlacements();
            snapshot.applicationPresentationMode =
                static_cast<const domain::ApplicationCard*>(card)->presentationMode();
            break;
        case domain::CardType::Mapping: {
            const auto* mapping = static_cast<const domain::MappingCard*>(card);
            snapshot.mappingSourceRoot = mapping->sourceRoot();
            snapshot.mappingReferences = mapping->references();
            snapshot.mappingAllowsSourceMutation = mapping->allowsSourceMutation();
            snapshot.mappingMode = mapping->mode();
            snapshot.mappingPresentationMode = mapping->presentationMode();
            snapshot.mappingSortMode = mapping->sortMode();
            snapshot.mappingItemPlacements = mapping->itemPlacements();
            break;
        }
        case domain::CardType::Todo:
            snapshot.todoPreferences = static_cast<const domain::TodoCard*>(card)->preferences();
            snapshot.todoItems = static_cast<const domain::TodoCard*>(card)->items();
            break;
        }
        result.push_back(std::move(snapshot));
    }
    return result;
}

void ApplicationRuntime::restore(
    const std::vector<domain::CardSnapshot>& cards,
    const domain::WorkspaceLayout& workspace) {
    std::unordered_map<domain::CardId, std::unique_ptr<domain::Card>> restoredCards;
    restoredCards.reserve(cards.size());
    for (const auto& snapshot : cards) {
        auto card = RestoreCard(snapshot);
        const auto [iterator, inserted] = restoredCards.emplace(snapshot.id, std::move(card));
        if (!inserted) {
            throw std::invalid_argument("Card snapshot ids must be unique.");
        }
    }
    for (const auto& placement : workspace.placements()) {
        if (!restoredCards.contains(placement.cardId)) {
            throw std::invalid_argument("Placement references a missing card.");
        }
    }

    domain::MappingSourceRegistry restoredMappingSources;
    for (const auto& [cardId, card] : restoredCards) {
        if (card->type() != domain::CardType::Mapping) {
            continue;
        }
        const auto* mapping = static_cast<const domain::MappingCard*>(card.get());
        if (mapping->mode() == domain::MappingMode::Folder
            && !mapping->sourceRoot().empty()
            && !restoredMappingSources.tryRegister(cardId, mapping->sourceRoot())) {
            throw std::invalid_argument(
                "Mapping folder sources must be unique across Mapping Cards.");
        }
    }

    auto nextProjections = workspace.project(displays_);
    cards_ = std::move(restoredCards);
    mappingSources_ = std::move(restoredMappingSources);
    workspace_ = workspace;
    projections_ = std::move(nextProjections);
    pendingDeletions_.clear();
    revision_ = 0;
    nextDeletionToken_ = 1;
}

std::optional<DeletionRequest> ApplicationRuntime::pendingDeletion(
    const domain::CardId& cardId) const {
    const auto pending = pendingDeletions_.find(cardId);
    if (pending == pendingDeletions_.end()) {
        return std::nullopt;
    }
    return DeletionRequest{
        .token = pending->second.token,
        .preview = pending->second.preview,
    };
}

CommandResult ApplicationRuntime::handle(const CreateApplicationCard& command) {
    if (cards_.contains(command.cardId)) {
        return rejected(CommandError::DuplicateCardId);
    }
    auto card = CreateCard<domain::ApplicationCard>(
        command.cardId,
        command.relativeStoragePath);
    cards_.emplace(command.cardId, std::move(card));
    return applied({
        .createdCards = {command.cardId},
        .persistence = PersistenceUrgency::Deferred,
    });
}

CommandResult ApplicationRuntime::handle(const CreateMappingCard& command) {
    if (cards_.contains(command.cardId)) {
        return rejected(CommandError::DuplicateCardId);
    }
    auto card = CreateCard<domain::MappingCard>(command.cardId);
    cards_.emplace(command.cardId, std::move(card));
    return applied({
        .createdCards = {command.cardId},
        .persistence = PersistenceUrgency::Deferred,
    });
}

CommandResult ApplicationRuntime::handle(const CreateTodoCard& command) {
    if (cards_.contains(command.cardId)) {
        return rejected(CommandError::DuplicateCardId);
    }
    auto card = CreateCard<domain::TodoCard>(command.cardId);
    cards_.emplace(command.cardId, std::move(card));
    return applied({
        .createdCards = {command.cardId},
        .persistence = PersistenceUrgency::Deferred,
    });
}

CommandResult ApplicationRuntime::handle(const AddTodoItem& command) {
    auto card = cards_.find(command.cardId);
    if (card == cards_.end() || card->second->type() != domain::CardType::Todo) {
        return rejected(CommandError::CardNotFound);
    }
    auto* todo = static_cast<domain::TodoCard*>(card->second.get());
    if (std::ranges::any_of(todo->items(), [&](const domain::TodoItem& item) {
            return item.id == command.itemId;
        })) {
        return rejected(CommandError::DuplicateTodoItemId);
    }
    auto items = todo->items();
    items.push_back({
        command.itemId,
        command.title,
        false,
        command.createdAtUnixMilliseconds == 0
            ? UnixMillisecondsNow()
            : command.createdAtUnixMilliseconds,
        0,
        command.scheduledDate.value_or(domain::CurrentSystemTodoDate()),
        false,
    });
    todo->setItems(std::move(items));
    return applied({
        .changedCards = {command.cardId},
        .persistence = PersistenceUrgency::Deferred,
    });
}

CommandResult ApplicationRuntime::handle(const AddHistoricalArchivedTodoItem& command) {
    auto card = cards_.find(command.cardId);
    if (card == cards_.end() || card->second->type() != domain::CardType::Todo) {
        return rejected(CommandError::CardNotFound);
    }
    if (command.archivedAtUnixMilliseconds <= 0
        || !domain::IsValidTodoDate(command.scheduledDate)) {
        return rejected(CommandError::InvalidCommand);
    }
    auto* todo = static_cast<domain::TodoCard*>(card->second.get());
    if (std::ranges::any_of(todo->items(), [&](const domain::TodoItem& item) {
            return item.id == command.itemId;
        })) {
        return rejected(CommandError::DuplicateTodoItemId);
    }
    auto items = todo->items();
    items.push_back({
        command.itemId,
        command.title,
        true,
        command.archivedAtUnixMilliseconds,
        command.archivedAtUnixMilliseconds,
        command.scheduledDate,
        true,
    });
    todo->setItems(std::move(items));
    return applied({
        .changedCards = {command.cardId},
        .persistence = PersistenceUrgency::Deferred,
    });
}

CommandResult ApplicationRuntime::handle(const ArchiveCompletedTodoItems& command) {
    auto card = cards_.find(command.cardId);
    if (card == cards_.end() || card->second->type() != domain::CardType::Todo) {
        return rejected(CommandError::CardNotFound);
    }
    auto* todo = static_cast<domain::TodoCard*>(card->second.get());
    auto items = todo->items();
    bool changed = false;
    for (auto& item : items) {
        if (item.completed && !item.archived) {
            item.archived = true;
            changed = true;
        }
    }
    if (!changed) return noChange();
    todo->setItems(std::move(items));
    return applied({
        .changedCards = {command.cardId},
        .persistence = PersistenceUrgency::Deferred,
    });
}

CommandResult ApplicationRuntime::handle(const ArchiveTodoItem& command) {
    auto card = cards_.find(command.cardId);
    if (card == cards_.end() || card->second->type() != domain::CardType::Todo) {
        return rejected(CommandError::CardNotFound);
    }
    auto* todo = static_cast<domain::TodoCard*>(card->second.get());
    auto items = todo->items();
    const auto item = std::ranges::find(items, command.itemId, &domain::TodoItem::id);
    if (item == items.end()) return rejected(CommandError::TodoItemNotFound);
    if (!item->completed || item->archived) return noChange();
    item->archived = true;
    todo->setItems(std::move(items));
    return applied({
        .changedCards = {command.cardId},
        .persistence = PersistenceUrgency::Deferred,
    });
}

CommandResult ApplicationRuntime::handle(const RestoreArchivedTodoItems& command) {
    auto card = cards_.find(command.cardId);
    if (card == cards_.end() || card->second->type() != domain::CardType::Todo) {
        return rejected(CommandError::CardNotFound);
    }
    auto* todo = static_cast<domain::TodoCard*>(card->second.get());
    auto items = todo->items();
    const auto restoredAt = command.restoredAtUnixMilliseconds == 0
        ? UnixMillisecondsNow() : command.restoredAtUnixMilliseconds;
    if (restoredAt < 0) return rejected(CommandError::InvalidCommand);
    bool changed = false;
    for (auto& item : items) {
        if (item.archived || (item.completed && item.completedAtUnixMilliseconds > 0)) {
            item.archived = false;
            if (item.completed) item.completedAtUnixMilliseconds = restoredAt;
            changed = true;
        }
    }
    if (!changed) return noChange();
    todo->setItems(std::move(items));
    return applied({
        .changedCards = {command.cardId},
        .persistence = PersistenceUrgency::Deferred,
    });
}

CommandResult ApplicationRuntime::handle(const RestoreArchivedTodoItem& command) {
    auto card = cards_.find(command.cardId);
    if (card == cards_.end() || card->second->type() != domain::CardType::Todo) {
        return rejected(CommandError::CardNotFound);
    }
    auto* todo = static_cast<domain::TodoCard*>(card->second.get());
    auto items = todo->items();
    const auto item = std::ranges::find(items, command.itemId, &domain::TodoItem::id);
    if (item == items.end()) return rejected(CommandError::TodoItemNotFound);
    if (!item->archived && !item->completed) return noChange();
    const auto restoredAt = command.restoredAtUnixMilliseconds == 0
        ? UnixMillisecondsNow() : command.restoredAtUnixMilliseconds;
    if (restoredAt < 0) return rejected(CommandError::InvalidCommand);
    item->archived = false;
    if (item->completed) item->completedAtUnixMilliseconds = restoredAt;
    todo->setItems(std::move(items));
    return applied({
        .changedCards = {command.cardId},
        .persistence = PersistenceUrgency::Deferred,
    });
}

CommandResult ApplicationRuntime::handle(const SetTodoCardPreferences& command) {
    auto card = cards_.find(command.cardId);
    if (card == cards_.end() || card->second->type() != domain::CardType::Todo) {
        return rejected(CommandError::CardNotFound);
    }
    auto* todo = static_cast<domain::TodoCard*>(card->second.get());
    if (todo->preferences() == command.preferences) return noChange();
    todo->setPreferences(command.preferences);
    return applied({
        .changedCards = {command.cardId},
        .persistence = PersistenceUrgency::Deferred,
    });
}

CommandResult ApplicationRuntime::handle(const RenameTodoItem& command) {
    auto card = cards_.find(command.cardId);
    if (card == cards_.end() || card->second->type() != domain::CardType::Todo) {
        return rejected(CommandError::CardNotFound);
    }
    auto* todo = static_cast<domain::TodoCard*>(card->second.get());
    auto items = todo->items();
    const auto item = std::ranges::find(items, command.itemId, &domain::TodoItem::id);
    if (item == items.end()) {
        return rejected(CommandError::TodoItemNotFound);
    }
    if (item->title == command.title) {
        return noChange();
    }
    item->title = command.title;
    todo->setItems(std::move(items));
    return applied({
        .changedCards = {command.cardId},
        .persistence = PersistenceUrgency::Deferred,
    });
}

CommandResult ApplicationRuntime::handle(const SetTodoItemCompleted& command) {
    auto card = cards_.find(command.cardId);
    if (card == cards_.end() || card->second->type() != domain::CardType::Todo) {
        return rejected(CommandError::CardNotFound);
    }
    auto* todo = static_cast<domain::TodoCard*>(card->second.get());
    auto items = todo->items();
    const auto item = std::ranges::find(items, command.itemId, &domain::TodoItem::id);
    if (item == items.end()) {
        return rejected(CommandError::TodoItemNotFound);
    }
    if (item->completed == command.completed
        && command.completedAtUnixMilliseconds == 0
        && (!command.completed || item->completedAtUnixMilliseconds > 0)
        && (command.completed || !item->archived)) {
        return noChange();
    }
    const auto completedAt = command.completed
        ? (command.completedAtUnixMilliseconds == 0
            ? UnixMillisecondsNow() : command.completedAtUnixMilliseconds)
        : 0;
    if (completedAt < 0) return rejected(CommandError::InvalidCommand);
    if (item->completed == command.completed
        && item->completedAtUnixMilliseconds == completedAt
        && (command.completed || !item->archived)) {
        return noChange();
    }
    item->completed = command.completed;
    item->completedAtUnixMilliseconds = completedAt;
    if (!command.completed) item->archived = false;
    todo->setItems(std::move(items));
    return applied({
        .changedCards = {command.cardId},
        .persistence = PersistenceUrgency::Deferred,
    });
}

CommandResult ApplicationRuntime::handle(const RemoveTodoItem& command) {
    auto card = cards_.find(command.cardId);
    if (card == cards_.end() || card->second->type() != domain::CardType::Todo) {
        return rejected(CommandError::CardNotFound);
    }
    auto* todo = static_cast<domain::TodoCard*>(card->second.get());
    auto items = todo->items();
    const auto item = std::ranges::find(items, command.itemId, &domain::TodoItem::id);
    if (item == items.end()) {
        return rejected(CommandError::TodoItemNotFound);
    }
    items.erase(item);
    todo->setItems(std::move(items));
    return applied({
        .changedCards = {command.cardId},
        .persistence = PersistenceUrgency::Deferred,
    });
}

CommandResult ApplicationRuntime::handle(const ReorderTodoItems& command) {
    auto card = cards_.find(command.cardId);
    if (card == cards_.end() || card->second->type() != domain::CardType::Todo) {
        return rejected(CommandError::CardNotFound);
    }
    auto* todo = static_cast<domain::TodoCard*>(card->second.get());
    const auto& existing = todo->items();
    if (command.orderedItemIds.size() != existing.size()) {
        return rejected(CommandError::InvalidCommand, "Todo order must contain every item once.");
    }
    std::vector<domain::TodoItem> reordered;
    reordered.reserve(existing.size());
    for (const auto& id : command.orderedItemIds) {
        const auto item = std::ranges::find(existing, id, &domain::TodoItem::id);
        if (item == existing.end()
            || std::ranges::any_of(reordered, [&](const domain::TodoItem& value) {
                return value.id == id;
            })) {
            return rejected(
                CommandError::InvalidCommand, "Todo order must contain every item once.");
        }
        reordered.push_back(*item);
    }
    if (reordered == existing) {
        return noChange();
    }
    todo->setItems(std::move(reordered));
    return applied({
        .changedCards = {command.cardId},
        .persistence = PersistenceUrgency::Deferred,
    });
}

CommandResult ApplicationRuntime::handle(const SetCardVisibility& command) {
    auto card = cards_.find(command.cardId);
    if (card == cards_.end()) {
        return rejected(CommandError::CardNotFound);
    }
    if (card->second->isVisible() == command.visible) {
        return noChange();
    }
    card->second->setVisible(command.visible);
    return applied({
        .changedCards = {command.cardId},
        .persistence = PersistenceUrgency::Deferred,
    });
}

CommandResult ApplicationRuntime::handle(const SetCardExpanded& command) {
    auto card = cards_.find(command.cardId);
    if (card == cards_.end()) {
        return rejected(CommandError::CardNotFound);
    }
    if (card->second->isExpanded() == command.expanded) {
        return noChange();
    }
    card->second->setExpanded(command.expanded);
    return applied({
        .changedCards = {command.cardId},
        .persistence = PersistenceUrgency::Deferred,
    });
}

CommandResult ApplicationRuntime::handle(const RenameCard& command) {
    auto card = cards_.find(command.cardId);
    if (card == cards_.end()) return rejected(CommandError::CardNotFound);
    if (card->second->name() == command.name) return noChange();
    card->second->setName(command.name);
    return applied({
        .changedCards = {command.cardId},
        .persistence = PersistenceUrgency::Deferred,
    });
}

CommandResult ApplicationRuntime::handle(const SetCardChromePreferences& command) {
    auto card = cards_.find(command.cardId);
    if (card == cards_.end()) return rejected(CommandError::CardNotFound);
    if (card->second->chrome() == command.preferences) return noChange();
    card->second->setChrome(command.preferences);
    return applied({
        .changedCards = {command.cardId},
        .persistence = PersistenceUrgency::Deferred,
    });
}

CommandResult ApplicationRuntime::handle(const SetCardAppearancePreferences& command) {
    auto card = cards_.find(command.cardId);
    if (card == cards_.end()) return rejected(CommandError::CardNotFound);
    if (card->second->appearance() == command.preferences) return noChange();
    try {
        card->second->setAppearance(command.preferences);
    } catch (const std::invalid_argument& error) {
        return rejected(CommandError::InvalidCommand, error.what());
    }
    return applied({
        .changedCards = {command.cardId},
        .persistence = PersistenceUrgency::Deferred,
    });
}

CommandResult ApplicationRuntime::handle(const SetCardContentPreferences& command) {
    auto card = cards_.find(command.cardId);
    if (card == cards_.end()) {
        return rejected(CommandError::CardNotFound);
    }
    if (card->second->content() == command.preferences) {
        return noChange();
    }
    if (card->second->type() == domain::CardType::Application) {
        static_cast<const domain::ApplicationCard&>(*card->second)
            .validateContentPreferences(command.preferences);
    }
    card->second->setContent(command.preferences);
    return applied({
        .changedCards = {command.cardId},
        .persistence = PersistenceUrgency::Deferred,
    });
}

CommandResult ApplicationRuntime::handle(const SetApplicationCardLayout& command) {
    auto card = cards_.find(command.cardId);
    if (card == cards_.end() || card->second->type() != domain::CardType::Application) {
        return rejected(CommandError::CardNotFound);
    }
    auto* applicationCard = static_cast<domain::ApplicationCard*>(card->second.get());
    if (applicationCard->sortMode() == command.sortMode
        && applicationCard->itemPlacements() == command.itemPlacements) {
        return noChange();
    }
    applicationCard->setLayout(command.sortMode, command.itemPlacements);
    return applied({
        .changedCards = {command.cardId},
        .persistence = PersistenceUrgency::Deferred,
    });
}

CommandResult ApplicationRuntime::handle(
    const SetApplicationPresentationMode& command) {
    auto card = cards_.find(command.cardId);
    if (card == cards_.end() || card->second->type() != domain::CardType::Application) {
        return rejected(CommandError::CardNotFound);
    }
    auto* applicationCard = static_cast<domain::ApplicationCard*>(card->second.get());
    if (applicationCard->presentationMode() == command.mode) return noChange();
    applicationCard->setPresentationMode(command.mode);
    return applied({
        .changedCards = {command.cardId},
        .persistence = PersistenceUrgency::Deferred,
    });
}

CommandResult ApplicationRuntime::handle(const SetMappingCardLayout& command) {
    auto card = cards_.find(command.cardId);
    if (card == cards_.end() || card->second->type() != domain::CardType::Mapping) {
        return rejected(CommandError::CardNotFound);
    }
    auto* mapping = static_cast<domain::MappingCard*>(card->second.get());
    if (mapping->sortMode() == command.sortMode
        && mapping->itemPlacements() == command.itemPlacements) return noChange();
    mapping->setLayout(command.sortMode, command.itemPlacements);
    return applied({
        .changedCards = {command.cardId},
        .persistence = PersistenceUrgency::Deferred,
    });
}

CommandResult ApplicationRuntime::handle(const SetMappingFolderSource& command) {
    auto card = cards_.find(command.cardId);
    if (card == cards_.end() || card->second->type() != domain::CardType::Mapping) {
        return rejected(CommandError::CardNotFound);
    }
    auto* mapping = static_cast<domain::MappingCard*>(card->second.get());
    const auto owner = mappingSources_.ownerOf(command.sourceRoot);
    if (mapping->mode() == domain::MappingMode::Folder
        && owner.has_value() && *owner == command.cardId) {
        return noChange();
    }
    if (!mappingSources_.tryRegister(command.cardId, command.sourceRoot)) {
        return rejected(
            CommandError::MappingSourceAlreadyMapped,
            "The mapping folder source is already assigned to another Card.");
    }
    mapping->setFolderSource(command.sourceRoot);
    // A folder source is a live view of one real directory. Its file items
    // must support the same move-in/move-out semantics as that directory.
    mapping->setAllowsSourceMutation(true);
    return applied({
        .changedCards = {command.cardId},
        .persistence = PersistenceUrgency::Deferred,
    });
}

CommandResult ApplicationRuntime::handle(const SetMappingReferences& command) {
    auto card = cards_.find(command.cardId);
    if (card == cards_.end() || card->second->type() != domain::CardType::Mapping) {
        return rejected(CommandError::CardNotFound);
    }
    auto* mapping = static_cast<domain::MappingCard*>(card->second.get());
    if (mapping->mode() != domain::MappingMode::Folder
        && mapping->references() == command.references) {
        return noChange();
    }
    mapping->setReferences(command.references);
    // References never own their targets; switching modes must not retain a
    // writable-folder flag from the previous source.
    mapping->setAllowsSourceMutation(false);
    mappingSources_.unregister(command.cardId);
    return applied({
        .changedCards = {command.cardId},
        .persistence = PersistenceUrgency::Deferred,
    });
}

CommandResult ApplicationRuntime::handle(const SetMappingMode& command) {
    auto card = cards_.find(command.cardId);
    if (card == cards_.end() || card->second->type() != domain::CardType::Mapping) {
        return rejected(CommandError::CardNotFound);
    }
    if (command.mode == domain::MappingMode::Empty) {
        return rejected(CommandError::InvalidCommand);
    }
    auto* mapping = static_cast<domain::MappingCard*>(card->second.get());
    if (mapping->mode() == command.mode) return noChange();
    mapping->setMode(command.mode);
    mapping->setAllowsSourceMutation(command.mode == domain::MappingMode::Folder);
    mappingSources_.unregister(command.cardId);
    return applied({
        .changedCards = {command.cardId},
        .persistence = PersistenceUrgency::Deferred,
    });
}

CommandResult ApplicationRuntime::handle(const SetMappingPresentationMode& command) {
    auto card = cards_.find(command.cardId);
    if (card == cards_.end() || card->second->type() != domain::CardType::Mapping) {
        return rejected(CommandError::CardNotFound);
    }
    auto* mapping = static_cast<domain::MappingCard*>(card->second.get());
    if (mapping->presentationMode() == command.mode) return noChange();
    mapping->setPresentationMode(command.mode);
    return applied({
        .changedCards = {command.cardId},
        .persistence = PersistenceUrgency::Deferred,
    });
}

CommandResult ApplicationRuntime::handle(const SetMappingSourceMutation& command) {
    auto card = cards_.find(command.cardId);
    if (card == cards_.end() || card->second->type() != domain::CardType::Mapping) {
        return rejected(CommandError::CardNotFound);
    }
    auto* mapping = static_cast<domain::MappingCard*>(card->second.get());
    if (mapping->allowsSourceMutation() == command.allowed) {
        return noChange();
    }
    mapping->setAllowsSourceMutation(command.allowed);
    return applied({
        .changedCards = {command.cardId},
        .persistence = PersistenceUrgency::Deferred,
    });
}

CommandResult ApplicationRuntime::handle(const ClearMappingSource& command) {
    auto card = cards_.find(command.cardId);
    if (card == cards_.end() || card->second->type() != domain::CardType::Mapping) {
        return rejected(CommandError::CardNotFound);
    }
    auto* mapping = static_cast<domain::MappingCard*>(card->second.get());
    if (mapping->sourceRoot().empty() && mapping->references().empty()) {
        return noChange();
    }
    mapping->clearSource();
    mappingSources_.unregister(command.cardId);
    return applied({
        .changedCards = {command.cardId},
        .persistence = PersistenceUrgency::Deferred,
    });
}

CommandResult ApplicationRuntime::handle(const SetPlacement& command) {
    if (!cards_.contains(command.placement.cardId)) {
        return rejected(CommandError::CardNotFound);
    }
    const auto existing = std::find_if(
        workspace_.placements().begin(),
        workspace_.placements().end(),
        [&](const domain::CardPlacement& placement) {
            return placement.id == command.placement.id;
        });
    if (existing != workspace_.placements().end()
        && existing->cardId == command.placement.cardId
        && existing->target == command.placement.target
        && existing->rect.left == command.placement.rect.left
        && existing->rect.top == command.placement.rect.top
        && existing->rect.width == command.placement.rect.width
        && existing->rect.height == command.placement.rect.height
        && existing->zIndex == command.placement.zIndex
        && existing->horizontalAnchor == command.placement.horizontalAnchor
        && existing->verticalAnchor == command.placement.verticalAnchor
        && existing->referenceWorkAreaWidth == command.placement.referenceWorkAreaWidth
        && existing->referenceWorkAreaHeight == command.placement.referenceWorkAreaHeight) {
        return noChange();
    }

    workspace_.setPlacement(command.placement);
    const auto projectionsChanged = refreshProjections();
    return applied({
        .layoutChanged = true,
        .projectionsChanged = projectionsChanged,
        .persistence = PersistenceUrgency::Deferred,
    });
}

CommandResult ApplicationRuntime::handle(const RemovePlacement& command) {
    if (!workspace_.removePlacement(command.placementId)) {
        return rejected(CommandError::PlacementNotFound);
    }
    const auto projectionsChanged = refreshProjections();
    return applied({
        .layoutChanged = true,
        .projectionsChanged = projectionsChanged,
        .persistence = PersistenceUrgency::Deferred,
    });
}

CommandResult ApplicationRuntime::handle(const UpdateDisplayTopology& command) {
    auto displays = command.displays;
    std::sort(
        displays.begin(),
        displays.end(),
        [](const domain::DisplaySnapshot& left, const domain::DisplaySnapshot& right) {
            return left.id < right.id;
        });
    const auto nextProjections = workspace_.project(displays);
    if (SameDisplays(displays_, displays) && SameProjections(projections_, nextProjections)) {
        return noChange();
    }
    const auto projectionsChanged = !SameProjections(projections_, nextProjections);
    displays_ = std::move(displays);
    projections_ = nextProjections;
    return applied({
        .displayTopologyChanged = true,
        .projectionsChanged = projectionsChanged,
        .persistence = PersistenceUrgency::None,
    });
}

CommandResult ApplicationRuntime::handle(const RequestCardDeletion& command) {
    const auto card = cards_.find(command.cardId);
    if (card == cards_.end()) {
        return rejected(CommandError::CardNotFound);
    }
    if (pendingDeletions_.contains(command.cardId)) {
        return rejected(CommandError::DeletionAlreadyPending);
    }

    const auto token = nextDeletionToken_++;
    const auto preview = card->second->deletionPreview();
    pendingDeletions_.emplace(command.cardId, PendingDeletion{token, preview});
    return applied({
        .deletionRequest = DeletionRequest{token, preview},
        .persistence = PersistenceUrgency::None,
    });
}

CommandResult ApplicationRuntime::handle(const CancelCardDeletion& command) {
    const auto pending = pendingDeletions_.find(command.cardId);
    if (pending == pendingDeletions_.end()) {
        return rejected(CommandError::DeletionNotPending);
    }
    if (pending->second.token != command.token) {
        return rejected(CommandError::DeletionTokenMismatch);
    }
    pendingDeletions_.erase(pending);
    return applied({.persistence = PersistenceUrgency::None});
}

CommandResult ApplicationRuntime::handle(const CommitCardDeletion& command) {
    const auto pending = pendingDeletions_.find(command.cardId);
    if (pending == pendingDeletions_.end()) {
        return rejected(CommandError::DeletionNotPending);
    }
    if (pending->second.token != command.token) {
        return rejected(CommandError::DeletionTokenMismatch);
    }
    if (!cards_.contains(command.cardId)) {
        return rejected(CommandError::CardNotFound);
    }

    mappingSources_.unregister(command.cardId);
    cards_.erase(command.cardId);
    pendingDeletions_.erase(pending);
    const auto removedPlacements = workspace_.removeCard(command.cardId);
    const auto projectionsChanged = removedPlacements > 0 && refreshProjections();
    return applied({
        .removedCards = {command.cardId},
        .layoutChanged = removedPlacements > 0,
        .projectionsChanged = projectionsChanged,
        .persistence = PersistenceUrgency::Immediate,
    });
}

CommandResult ApplicationRuntime::applied(ApplicationChangeSet changes) {
    ++revision_;
    return {
        .status = CommandStatus::Applied,
        .error = CommandError::None,
        .revision = revision_,
        .changes = std::move(changes),
    };
}

CommandResult ApplicationRuntime::noChange() const {
    return {
        .status = CommandStatus::NoChange,
        .error = CommandError::None,
        .revision = revision_,
    };
}

CommandResult ApplicationRuntime::rejected(
    CommandError error,
    std::string diagnostic) const {
    return {
        .status = CommandStatus::Rejected,
        .error = error,
        .revision = revision_,
        .diagnostic = std::move(diagnostic),
    };
}

bool ApplicationRuntime::refreshProjections() {
    const auto next = workspace_.project(displays_);
    if (SameProjections(projections_, next)) {
        return false;
    }
    projections_ = next;
    return true;
}

} // namespace desto::application
