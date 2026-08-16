#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "Card.h"
#include "FileMoveTransaction.h"

namespace desto::storage {

struct MappingCardImportPlan {
    domain::CardId cardId;
    std::filesystem::path sourceRoot;
    std::vector<FileMove> moves;
};

struct MappingCardImportResult {
    bool succeeded = false;
    std::vector<FileMove> completedMoves;
    std::vector<std::string> failures;
};

class MappingCardImportService {
public:
    [[nodiscard]] MappingCardImportPlan plan(
        const domain::MappingCard& card,
        std::span<const std::filesystem::path> sources) const;
    [[nodiscard]] MappingCardImportResult execute(
        const MappingCardImportPlan& plan) const;
};

} // namespace desto::storage
