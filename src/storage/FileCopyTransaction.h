#pragma once

#include <span>
#include <string>
#include <vector>

#include "FileMoveTransaction.h"

namespace desto::storage {

class FileCopyTransaction {
public:
    [[nodiscard]] static FileMoveTransactionResult execute(
        std::span<const FileMove> copies);
    [[nodiscard]] static FileMoveTransactionResult rollback(
        std::span<const FileMove> completedCopies);
};

} // namespace desto::storage
