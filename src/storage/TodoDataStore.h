#pragma once

#include <filesystem>

#include "JsonConfigStore.h"

namespace desto::storage {

class TodoDataStore final {
public:
    explicit TodoDataStore(std::filesystem::path configDirectory);

    void loadInto(ApplicationConfig& config) const;
    void save(const ApplicationConfig& config) const;

private:
    std::filesystem::path directory_;
};

} // namespace desto::storage
