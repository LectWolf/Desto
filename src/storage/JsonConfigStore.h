#pragma once

#include <filesystem>

namespace desto::storage {

struct ApplicationConfig {
    int schemaVersion = 1;
    std::filesystem::path storageRoot;
};

class JsonConfigStore {
public:
    explicit JsonConfigStore(std::filesystem::path configPath);

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return configPath_; }
    [[nodiscard]] ApplicationConfig load() const;
    void save(const ApplicationConfig& config) const;

private:
    std::filesystem::path configPath_;
};

} // namespace desto::storage
