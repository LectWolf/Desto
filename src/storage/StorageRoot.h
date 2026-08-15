#pragma once

#include <filesystem>

namespace desto::storage {

class StorageRoot {
public:
    explicit StorageRoot(std::filesystem::path root);

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return root_; }
    [[nodiscard]] std::filesystem::path resolveCardPath(const std::filesystem::path& relativePath) const;
    void ensureExists() const;

private:
    std::filesystem::path root_;
};

} // namespace desto::storage

