#include "MappingCardImport.h"

#include "DirectoryImportPlanner.h"

#include <stdexcept>

namespace desto::storage {

MappingCardImportPlan MappingCardImportService::plan(
    const domain::MappingCard& card,
    std::span<const std::filesystem::path> sources) const {
    return plan(card, card.sourceRoot(), sources);
}

MappingCardImportPlan MappingCardImportService::plan(
    const domain::MappingCard& card,
    const std::filesystem::path& destinationDirectory,
    std::span<const std::filesystem::path> sources) const {
    if (card.mode() != domain::MappingMode::Folder) {
        throw std::invalid_argument("Mapping Card must use a folder source.");
    }
    auto planned = DirectoryImportPlanner::plan(destinationDirectory, sources);
    return {
        .cardId = card.id(),
        .sourceRoot = std::move(planned.destinationDirectory),
        .moves = std::move(planned.moves),
    };
}

MappingCardImportResult MappingCardImportService::execute(
    const MappingCardImportPlan& plan) const {
    if (plan.cardId.empty() || plan.sourceRoot.empty()) {
        throw std::invalid_argument("Mapping Card import plan is incomplete.");
    }
    const auto result = FileMoveTransaction::execute(plan.moves);
    return {
        .succeeded = result.succeeded,
        .completedMoves = result.completedMoves,
        .failures = result.failures,
    };
}

} // namespace desto::storage
