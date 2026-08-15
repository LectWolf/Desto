#pragma once

#include <filesystem>
#include <optional>
#include <unordered_map>

#include "Card.h"

namespace desto::storage {

class MappingRegistry {
public:
    bool tryRegister(const domain::CardId& cardId, std::filesystem::path sourceRoot);
    void unregister(const domain::CardId& cardId);

    [[nodiscard]] std::optional<domain::CardId> ownerOf(const std::filesystem::path& sourceRoot) const;
    [[nodiscard]] std::size_t size() const noexcept { return owners_.size(); }

private:
    static std::filesystem::path normalize(std::filesystem::path sourceRoot);

    std::unordered_map<std::string, domain::CardId> owners_;
    std::unordered_map<domain::CardId, std::string> sourcesByCard_;
};

} // namespace desto::storage
