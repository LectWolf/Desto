#include "Card.h"

#include <algorithm>
#include <stdexcept>

namespace desto::domain {
namespace {

void ValidateRect(const CardRect& rect) {
    if (rect.width <= 0 || rect.height <= 0) {
        throw std::invalid_argument("Card dimensions must be positive.");
    }
}

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

void Card::setRect(CardRect rect) {
    ValidateRect(rect);
    rect_ = rect;
}

void Card::setChrome(CardChromePreferences preferences) {
    chrome_ = preferences;
}

void Card::setAppearance(CardAppearancePreferences preferences) {
    ValidateAppearance(preferences);
    appearance_ = std::move(preferences);
}

ApplicationCard::ApplicationCard(CardId id, std::filesystem::path managedRoot)
    : Card(std::move(id), CardType::Application),
      managedRoot_(std::move(managedRoot)) {
    if (managedRoot_.empty()) {
        throw std::invalid_argument("Application card managed root must not be empty.");
    }
}

void ApplicationCard::setManagedRoot(std::filesystem::path managedRoot) {
    if (managedRoot.empty()) {
        throw std::invalid_argument("Application card managed root must not be empty.");
    }
    managedRoot_ = std::move(managedRoot);
}

FolderMappingCard::FolderMappingCard(CardId id, std::filesystem::path sourceRoot)
    : Card(std::move(id), CardType::FolderMapping),
      sourceRoot_(std::move(sourceRoot)) {
    if (sourceRoot_.empty()) {
        throw std::invalid_argument("Folder mapping source root must not be empty.");
    }
}

void FolderMappingCard::setSourceRoot(std::filesystem::path sourceRoot) {
    if (sourceRoot.empty()) {
        throw std::invalid_argument("Folder mapping source root must not be empty.");
    }
    sourceRoot_ = std::move(sourceRoot);
}

ReferenceCard::ReferenceCard(CardId id)
    : Card(std::move(id), CardType::Reference) {
}

void ReferenceCard::setReferences(std::vector<FileReference> references) {
    references.erase(
        std::remove_if(
            references.begin(),
            references.end(),
            [](const FileReference& reference) {
                return reference.id.empty() || reference.path.empty();
            }),
        references.end());
    references_ = std::move(references);
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
    case CardType::FolderMapping:
        return "folder-mapping";
    case CardType::Reference:
        return "reference";
    case CardType::Todo:
        return "todo";
    }
    return "unknown";
}

} // namespace desto::domain

