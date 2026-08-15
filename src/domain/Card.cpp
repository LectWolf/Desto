#include "Card.h"

#include <algorithm>
#include <stdexcept>

namespace desto::domain {
namespace {

void ValidateAppearance(const CardAppearancePreferences& appearance) {
    if (appearance.opacity < 0 || appearance.opacity > 1) {
        throw std::invalid_argument("Card opacity must be between 0 and 1.");
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

} // namespace desto::domain
