#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

#include "Card.h"

namespace desto::domain {

class MappingSourceRegistry {
public:
    bool tryRegister(const CardId& cardId, std::filesystem::path sourceRoot);
    void unregister(const CardId& cardId) noexcept;

    [[nodiscard]] std::optional<CardId> ownerOf(
        const std::filesystem::path& sourceRoot) const;
    [[nodiscard]] std::size_t size() const noexcept { return owners_.size(); }

private:
    [[nodiscard]] static std::wstring comparisonKey(
        std::filesystem::path sourceRoot);

    std::unordered_map<std::wstring, CardId> owners_;
    std::unordered_map<CardId, std::wstring> sourcesByCard_;
};

} // namespace desto::domain
