#include "MappingRegistry.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace desto::storage {
namespace {

std::string ComparisonKey(const std::filesystem::path& path) {
    auto value = path.lexically_normal().generic_string();
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

} // namespace

std::filesystem::path MappingRegistry::normalize(std::filesystem::path sourceRoot) {
    if (sourceRoot.empty() || !sourceRoot.is_absolute()) {
        throw std::invalid_argument("Mapping source root must be an absolute path.");
    }
    return sourceRoot.lexically_normal();
}

bool MappingRegistry::tryRegister(
    const domain::CardId& cardId,
    std::filesystem::path sourceRoot) {
    if (cardId.empty()) {
        throw std::invalid_argument("Mapping card id must not be empty.");
    }

    const auto key = ComparisonKey(normalize(std::move(sourceRoot)));
    const auto existing = owners_.find(key);
    if (existing != owners_.end() && existing->second != cardId) {
        return false;
    }

    const auto previousSource = sourcesByCard_.find(cardId);
    if (previousSource != sourcesByCard_.end() && previousSource->second != key) {
        owners_.erase(previousSource->second);
    }

    owners_[key] = cardId;
    sourcesByCard_[cardId] = key;
    return true;
}

void MappingRegistry::unregister(const domain::CardId& cardId) {
    const auto source = sourcesByCard_.find(cardId);
    if (source == sourcesByCard_.end()) {
        return;
    }
    owners_.erase(source->second);
    sourcesByCard_.erase(source);
}

std::optional<domain::CardId> MappingRegistry::ownerOf(
    const std::filesystem::path& sourceRoot) const {
    const auto key = ComparisonKey(normalize(sourceRoot));
    const auto iterator = owners_.find(key);
    if (iterator == owners_.end()) {
        return std::nullopt;
    }
    return iterator->second;
}

} // namespace desto::storage
