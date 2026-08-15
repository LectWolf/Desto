#pragma once

#include <filesystem>

#include "WorkspaceLayout.h"

namespace desto::storage {

struct ApplicationConfig {
    static constexpr int CurrentSchemaVersion = 1;

    int schemaVersion = CurrentSchemaVersion;
    std::filesystem::path storageRoot;
    domain::WorkspaceLayout workspace;
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
