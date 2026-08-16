#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "Card.h"
#include "FileMoveTransaction.h"
#include "StorageRoot.h"

namespace desto::storage {

struct ApplicationCardImportPlan {
    domain::CardId cardId;
    std::filesystem::path cardDirectory;
    std::vector<FileMove> moves;
};

struct ApplicationCardImportResult {
    bool succeeded = false;
    std::vector<FileMove> completedMoves;
    std::vector<std::string> failures;
};

class ApplicationCardImportService {
public:
    explicit ApplicationCardImportService(StorageRoot storageRoot);

    [[nodiscard]] ApplicationCardImportPlan plan(
        const domain::ApplicationCard& card,
        std::span<const std::filesystem::path> sources) const;

    [[nodiscard]] ApplicationCardImportResult execute(
        const ApplicationCardImportPlan& plan) const;

private:
    StorageRoot storageRoot_;
};

} // namespace desto::storage
