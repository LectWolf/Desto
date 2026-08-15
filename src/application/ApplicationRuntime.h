#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "Card.h"
#include "WorkspaceLayout.h"

namespace desto::application {

struct CreateApplicationCard {
    domain::CardId cardId;
    std::filesystem::path relativeStoragePath;
};

struct CreateMappingCard {
    domain::CardId cardId;
};

struct CreateTodoCard {
    domain::CardId cardId;
};

struct SetCardVisibility {
    domain::CardId cardId;
    bool visible;
};

struct SetCardExpanded {
    domain::CardId cardId;
    bool expanded;
};

struct SetPlacement {
    domain::CardPlacement placement;
};

struct RemovePlacement {
    domain::PlacementId placementId;
};

struct UpdateDisplayTopology {
    std::vector<domain::DisplaySnapshot> displays;
};

struct RequestCardDeletion {
    domain::CardId cardId;
};

struct CancelCardDeletion {
    domain::CardId cardId;
    std::uint64_t token;
};

struct CommitCardDeletion {
    domain::CardId cardId;
    std::uint64_t token;
};

using ApplicationCommand = std::variant<
    CreateApplicationCard,
    CreateMappingCard,
    CreateTodoCard,
    SetCardVisibility,
    SetCardExpanded,
    SetPlacement,
    RemovePlacement,
    UpdateDisplayTopology,
    RequestCardDeletion,
    CancelCardDeletion,
    CommitCardDeletion>;

enum class CommandStatus {
    Applied,
    NoChange,
    Rejected,
};

enum class CommandError {
    None,
    DuplicateCardId,
    CardNotFound,
    PlacementNotFound,
    DeletionAlreadyPending,
    DeletionNotPending,
    DeletionTokenMismatch,
    InvalidCommand,
};

enum class PersistenceUrgency {
    None,
    Deferred,
    Immediate,
};

struct DeletionRequest {
    std::uint64_t token;
    domain::CardDeletionPreview preview;
};

struct ApplicationChangeSet {
    std::vector<domain::CardId> createdCards;
    std::vector<domain::CardId> changedCards;
    std::vector<domain::CardId> removedCards;
    bool displayTopologyChanged = false;
    bool layoutChanged = false;
    bool projectionsChanged = false;
    std::optional<DeletionRequest> deletionRequest;
    PersistenceUrgency persistence = PersistenceUrgency::None;
};

struct CommandResult {
    CommandStatus status = CommandStatus::Rejected;
    CommandError error = CommandError::None;
    std::uint64_t revision = 0;
    ApplicationChangeSet changes;
    std::string diagnostic;
};

class ApplicationRuntime {
public:
    // Commands are serialized by the application loop; this type is not thread-safe.
    [[nodiscard]] CommandResult execute(const ApplicationCommand& command);

    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] const domain::Card* findCard(const domain::CardId& cardId) const noexcept;
    [[nodiscard]] std::vector<const domain::Card*> cards() const;
    [[nodiscard]] const domain::WorkspaceLayout& workspace() const noexcept { return workspace_; }
    [[nodiscard]] const std::vector<domain::DisplaySnapshot>& displays() const noexcept {
        return displays_;
    }
    [[nodiscard]] const std::vector<domain::PlacementProjection>& projections() const noexcept {
        return projections_;
    }
    [[nodiscard]] std::optional<DeletionRequest> pendingDeletion(
        const domain::CardId& cardId) const;

private:
    struct PendingDeletion {
        std::uint64_t token;
        domain::CardDeletionPreview preview;
    };

    [[nodiscard]] CommandResult handle(const CreateApplicationCard& command);
    [[nodiscard]] CommandResult handle(const CreateMappingCard& command);
    [[nodiscard]] CommandResult handle(const CreateTodoCard& command);
    [[nodiscard]] CommandResult handle(const SetCardVisibility& command);
    [[nodiscard]] CommandResult handle(const SetCardExpanded& command);
    [[nodiscard]] CommandResult handle(const SetPlacement& command);
    [[nodiscard]] CommandResult handle(const RemovePlacement& command);
    [[nodiscard]] CommandResult handle(const UpdateDisplayTopology& command);
    [[nodiscard]] CommandResult handle(const RequestCardDeletion& command);
    [[nodiscard]] CommandResult handle(const CancelCardDeletion& command);
    [[nodiscard]] CommandResult handle(const CommitCardDeletion& command);

    [[nodiscard]] CommandResult applied(ApplicationChangeSet changes);
    [[nodiscard]] CommandResult noChange() const;
    [[nodiscard]] CommandResult rejected(CommandError error, std::string diagnostic = {}) const;
    [[nodiscard]] bool refreshProjections();

    std::unordered_map<domain::CardId, std::unique_ptr<domain::Card>> cards_;
    domain::WorkspaceLayout workspace_;
    std::vector<domain::DisplaySnapshot> displays_;
    std::vector<domain::PlacementProjection> projections_;
    std::unordered_map<domain::CardId, PendingDeletion> pendingDeletions_;
    std::uint64_t revision_ = 0;
    std::uint64_t nextDeletionToken_ = 1;
};

} // namespace desto::application
