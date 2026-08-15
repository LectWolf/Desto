#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "FileMoveTransaction.h"
#include "JsonConfigStore.h"
#include "StorageRoot.h"

namespace desto::storage {

struct StorageRootMigrationPlan {
    std::filesystem::path sourceRoot;
    std::filesystem::path targetRoot;
    std::vector<FileMove> moves;
};

struct StorageRootMigrationResult {
    bool succeeded = false;
    std::vector<FileMove> completedMoves;
    std::vector<std::string> failures;
};

class StorageRootMigrationService {
public:
    [[nodiscard]] StorageRootMigrationPlan plan(
        const StorageRoot& source,
        std::filesystem::path target) const;

    [[nodiscard]] StorageRootMigrationResult execute(
        const StorageRootMigrationPlan& plan) const;

    [[nodiscard]] StorageRootMigrationResult migrate(
        const StorageRoot& source,
        std::filesystem::path target,
        const JsonConfigStore& configStore,
        int schemaVersion = 1) const;
};

} // namespace desto::storage
