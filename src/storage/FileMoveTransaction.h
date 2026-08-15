#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace desto::storage {

struct FileMove {
    std::filesystem::path source;
    std::filesystem::path destination;
};

struct FileMoveTransactionResult {
    bool succeeded = false;
    std::vector<FileMove> completedMoves;
    std::vector<std::string> failures;
};

class FileMoveTransaction {
public:
    [[nodiscard]] static FileMoveTransactionResult execute(std::span<const FileMove> moves);
    [[nodiscard]] static FileMoveTransactionResult rollback(std::span<const FileMove> completedMoves);

private:
    static void moveEntry(
        const std::filesystem::path& source,
        const std::filesystem::path& destination);
};

} // namespace desto::storage
