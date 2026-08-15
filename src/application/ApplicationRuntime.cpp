#include "ApplicationRuntime.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace desto::application {
namespace {

template <typename CardType, typename... Arguments>
std::unique_ptr<domain::Card> CreateCard(Arguments&&... arguments) {
    return std::make_unique<CardType>(std::forward<Arguments>(arguments)...);
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
        if (a.id != b.id || a.workAreaWidth != b.workAreaWidth
            || a.workAreaHeight != b.workAreaHeight || a.primary != b.primary) {
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
            || a.zIndex != b.zIndex || a.fallback != b.fallback) {
            return false;
        }
    }
    return true;
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
        && existing->zIndex == command.placement.zIndex) {
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
