#include "FileMoveTransaction.h"

#include <stdexcept>

namespace desto::storage {

void FileMoveTransaction::moveEntry(
    const std::filesystem::path& source,
    const std::filesystem::path& destination) {
    if (!std::filesystem::exists(source)) {
        throw std::runtime_error("Move source does not exist.");
    }
    if (std::filesystem::exists(destination)) {
        throw std::runtime_error("Move destination already exists.");
    }

    std::filesystem::create_directories(destination.parent_path());
    try {
        std::filesystem::rename(source, destination);
    } catch (const std::filesystem::filesystem_error&) {
        try {
            std::filesystem::copy(
                source,
                destination,
                std::filesystem::copy_options::recursive
                    | std::filesystem::copy_options::copy_symlinks);
            std::filesystem::remove_all(source);
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove_all(destination, ignored);
            throw;
        }
    }
}

FileMoveTransactionResult FileMoveTransaction::execute(std::span<const FileMove> moves) {
    FileMoveTransactionResult result;
    try {
        for (const auto& move : moves) {
            moveEntry(move.source, move.destination);
            result.completedMoves.push_back(move);
        }
        result.succeeded = true;
        return result;
    } catch (const std::exception& exception) {
        result.failures.push_back(exception.what());
    }

    const auto rollbackResult = rollback(result.completedMoves);
    result.failures.insert(
        result.failures.end(),
        rollbackResult.failures.begin(),
        rollbackResult.failures.end());
    result.completedMoves.clear();
    return result;
}

FileMoveTransactionResult FileMoveTransaction::rollback(
    std::span<const FileMove> completedMoves) {
    FileMoveTransactionResult result;
    for (auto iterator = completedMoves.rbegin(); iterator != completedMoves.rend(); ++iterator) {
        try {
            moveEntry(iterator->destination, iterator->source);
        } catch (const std::exception& exception) {
            result.failures.push_back(exception.what());
        }
    }
    result.succeeded = result.failures.empty();
    return result;
}

} // namespace desto::storage
