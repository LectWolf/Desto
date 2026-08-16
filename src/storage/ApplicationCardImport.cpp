#include "ApplicationCardImport.h"

#include "DirectoryImportPlanner.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace desto::storage {
ApplicationCardImportService::ApplicationCardImportService(StorageRoot storageRoot)
    : storageRoot_(std::move(storageRoot)) {
}

ApplicationCardImportPlan ApplicationCardImportService::plan(
    const domain::ApplicationCard& card,
    std::span<const std::filesystem::path> sources) const {
    ApplicationCardImportPlan result{
        .cardId = card.id(),
        .cardDirectory = storageRoot_.resolveCardPath(card.relativeStoragePath()),
    };
    result.moves = DirectoryImportPlanner::plan(result.cardDirectory, sources).moves;
    return result;
}

ApplicationCardImportResult ApplicationCardImportService::execute(
    const ApplicationCardImportPlan& plan) const {
    if (plan.cardId.empty() || plan.cardDirectory.empty()) {
        throw std::invalid_argument("Application Card import plan is incomplete.");
    }
    std::filesystem::create_directories(plan.cardDirectory);
    const auto transaction = FileMoveTransaction::execute(plan.moves);
    return {
        .succeeded = transaction.succeeded,
        .completedMoves = transaction.completedMoves,
        .failures = transaction.failures,
    };
}

} // namespace desto::storage
