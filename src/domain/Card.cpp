#include "Card.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace desto::domain {
namespace {

void ValidateAppearance(const CardAppearancePreferences& appearance) {
    if (appearance.opacity < 0 || appearance.opacity > 1
        || appearance.cornerRadius < 0 || appearance.cornerRadius > 128) {
        throw std::invalid_argument(
            "Card opacity must be between 0 and 1 and corner radius must be between 0 and 128.");
    }
}

} // namespace

Card::Card(CardId id, CardType type)
    : id_(std::move(id)), type_(type) {
    if (id_.empty()) {
        throw std::invalid_argument("Card id must not be empty.");
    }
}

void Card::setChrome(CardChromePreferences preferences) {
    chrome_ = preferences;
}

void Card::setAppearance(CardAppearancePreferences preferences) {
    ValidateAppearance(preferences);
    appearance_ = std::move(preferences);
}

CardDeletionPreview Card::deletionPreview() const noexcept {
    return {
        .cardId = id_,
        .cardType = type_,
        .effect = deletionEffect(),
        .requiresConfirmation = true,
    };
}

ApplicationCard::ApplicationCard(CardId id, std::filesystem::path relativeStoragePath)
    : Card(std::move(id), CardType::Application),
      relativeStoragePath_(std::move(relativeStoragePath)) {
    if (relativeStoragePath_.empty() || relativeStoragePath_.is_absolute()) {
        throw std::invalid_argument("Application card storage path must be relative and non-empty.");
    }
}

void ApplicationCard::setRelativeStoragePath(std::filesystem::path relativeStoragePath) {
    if (relativeStoragePath.empty() || relativeStoragePath.is_absolute()) {
        throw std::invalid_argument("Application card storage path must be relative and non-empty.");
    }
    relativeStoragePath_ = std::move(relativeStoragePath);
}

void ApplicationCard::setItemOrder(std::vector<std::filesystem::path> itemOrder) {
    std::unordered_set<std::filesystem::path> unique;
    for (auto& item : itemOrder) {
        item = item.lexically_normal();
        if (item.empty() || item.is_absolute() || item != item.filename()
            || item == "." || item == ".." || !unique.insert(item).second) {
            throw std::invalid_argument(
                "Application card item order must contain unique relative file names.");
        }
    }
    itemOrder_ = std::move(itemOrder);
}

MappingCard::MappingCard(CardId id)
    : Card(std::move(id), CardType::Mapping) {
}

MappingMode MappingCard::mode() const noexcept {
    if (!sourceRoot_.empty()) {
        return MappingMode::Folder;
    }
    return references_.empty() ? MappingMode::Empty : MappingMode::References;
}

void MappingCard::setFolderSource(std::filesystem::path sourceRoot) {
    if (sourceRoot.empty()) {
        throw std::invalid_argument("Mapping folder source must not be empty.");
    }
    references_.clear();
    sourceRoot_ = std::move(sourceRoot);
}

void MappingCard::setReferences(std::vector<FileReference> references) {
    references.erase(
        std::remove_if(
            references.begin(),
            references.end(),
            [](const FileReference& reference) {
                return reference.id.empty() || reference.path.empty();
            }),
        references.end());
    sourceRoot_.clear();
    references_ = std::move(references);
}

void MappingCard::clearSource() noexcept {
    sourceRoot_.clear();
    references_.clear();
}

TodoCard::TodoCard(CardId id)
    : Card(std::move(id), CardType::Todo) {
}

void TodoCard::setItems(std::vector<TodoItem> items) {
    items.erase(
        std::remove_if(
            items.begin(),
            items.end(),
            [](const TodoItem& item) {
                return item.id.empty() || item.title.empty();
            }),
        items.end());
    items_ = std::move(items);
}

std::string_view ToString(CardType type) noexcept {
    switch (type) {
    case CardType::Application:
        return "application";
    case CardType::Mapping:
        return "mapping";
    case CardType::Todo:
        return "todo";
    }
    return "unknown";
}

std::string_view ToString(CardItemSize size) noexcept {
    switch (size) {
    case CardItemSize::Small:
        return "small";
    case CardItemSize::Medium:
        return "medium";
    case CardItemSize::Large:
        return "large";
    case CardItemSize::ExtraLarge:
        return "extraLarge";
    }
    return "medium";
}

} // namespace desto::domain
