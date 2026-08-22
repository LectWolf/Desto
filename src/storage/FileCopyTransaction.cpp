#include "FileCopyTransaction.h"

#include <filesystem>
#include <stdexcept>

namespace desto::storage {
namespace {

void copyEntry(
    const std::filesystem::path& source,
    const std::filesystem::path& destination) {
    if (!std::filesystem::exists(source)) {
        throw std::runtime_error("Copy source does not exist.");
    }
    if (std::filesystem::exists(destination)) {
        throw std::runtime_error("Copy destination already exists.");
    }
    std::filesystem::create_directories(destination.parent_path());
    try {
        std::filesystem::copy(
            source,
            destination,
            std::filesystem::copy_options::recursive
                | std::filesystem::copy_options::copy_symlinks);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(destination, ignored);
        throw;
    }
}

} // namespace

FileMoveTransactionResult FileCopyTransaction::execute(
    std::span<const FileMove> copies) {
    FileMoveTransactionResult result;
    try {
        for (const auto& copy : copies) {
            copyEntry(copy.source, copy.destination);
            result.completedMoves.push_back(copy);
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

FileMoveTransactionResult FileCopyTransaction::rollback(
    std::span<const FileMove> completedCopies) {
    FileMoveTransactionResult result;
    for (auto iterator = completedCopies.rbegin();
         iterator != completedCopies.rend(); ++iterator) {
        try {
            if (std::filesystem::is_directory(iterator->destination)) {
                std::filesystem::remove_all(iterator->destination);
            } else {
                std::filesystem::remove(iterator->destination);
            }
        } catch (const std::exception& exception) {
            result.failures.push_back(exception.what());
        }
    }
    result.succeeded = result.failures.empty();
    return result;
}

} // namespace desto::storage
