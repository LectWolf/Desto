#include "MappingSourceRegistry.h"

#include <algorithm>
#include <cwctype>
#include <stdexcept>

namespace desto::domain {

std::wstring MappingSourceRegistry::comparisonKey(
    std::filesystem::path sourceRoot) {
    if (sourceRoot.empty() || !sourceRoot.is_absolute()) {
        throw std::invalid_argument("Mapping source root must be an absolute path.");
    }
    auto value = sourceRoot.lexically_normal().generic_wstring();
    std::ranges::transform(value, value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    while (value.size() > 1 && value.back() == L'/') {
        value.pop_back();
    }
    return value;
}

bool MappingSourceRegistry::tryRegister(
    const CardId& cardId,
    std::filesystem::path sourceRoot) {
    if (cardId.empty()) {
        throw std::invalid_argument("Mapping card id must not be empty.");
    }

    auto key = comparisonKey(std::move(sourceRoot));
    const auto owner = owners_.find(key);
    if (owner != owners_.end() && owner->second != cardId) {
        return false;
    }

    const auto previous = sourcesByCard_.find(cardId);
    if (previous != sourcesByCard_.end() && previous->second != key) {
        owners_.erase(previous->second);
    }
    owners_[key] = cardId;
    sourcesByCard_[cardId] = std::move(key);
    return true;
}

void MappingSourceRegistry::unregister(const CardId& cardId) noexcept {
    const auto source = sourcesByCard_.find(cardId);
    if (source == sourcesByCard_.end()) {
        return;
    }
    owners_.erase(source->second);
    sourcesByCard_.erase(source);
}

std::optional<CardId> MappingSourceRegistry::ownerOf(
    const std::filesystem::path& sourceRoot) const {
    const auto owner = owners_.find(comparisonKey(sourceRoot));
    return owner == owners_.end() ? std::nullopt : std::optional<CardId>(owner->second);
}

} // namespace desto::domain
