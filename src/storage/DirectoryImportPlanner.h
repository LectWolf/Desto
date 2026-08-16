#pragma once

#include <filesystem>
#include <span>
#include <vector>

#include "FileMoveTransaction.h"

namespace desto::storage {

struct DirectoryImportPlan {
    std::filesystem::path destinationDirectory;
    std::vector<FileMove> moves;
};

class DirectoryImportPlanner {
public:
    [[nodiscard]] static DirectoryImportPlan plan(
        std::filesystem::path destinationDirectory,
        std::span<const std::filesystem::path> sources);
};

} // namespace desto::storage
