#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "Card.h"
#include "FileMoveTransaction.h"
#include "StorageRoot.h"

namespace desto::storage {

using ReturnMove = FileMove;

struct ApplicationCardDeletionPlan {
    domain::CardDeletionPreview preview;
    std::filesystem::path cardDirectory;
    std::filesystem::path desktopDirectory;
    std::vector<ReturnMove> moves;
};

struct DeletionConfirmation {
    domain::CardId cardId;
};

struct ApplicationCardDeletionResult {
    bool succeeded = false;
    std::vector<ReturnMove> completedMoves;
    std::vector<std::string> failures;
};

class ApplicationCardReturnService {
public:
    explicit ApplicationCardReturnService(StorageRoot storageRoot);

    [[nodiscard]] ApplicationCardDeletionPlan plan(
        const domain::ApplicationCard& card,
        std::filesystem::path desktopDirectory) const;

    [[nodiscard]] ApplicationCardDeletionResult execute(
        const ApplicationCardDeletionPlan& plan,
        const DeletionConfirmation& confirmation) const;

private:
    StorageRoot storageRoot_;
};

} // namespace desto::storage
