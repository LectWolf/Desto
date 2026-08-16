#include "Card.h"

#include <algorithm>
#include <cwctype>
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

void ValidateContent(const CardContentPreferences& content) {
    switch (content.itemSize) {
    case CardItemSize::Small:
    case CardItemSize::Medium:
    case CardItemSize::Large:
    case CardItemSize::ExtraLarge:
        break;
    default:
        throw std::invalid_argument("Card item size is invalid.");
    }
    switch (content.sizeMode) {
    case CardSizeMode::Adaptive:
    case CardSizeMode::Fixed:
        break;
    default:
        throw std::invalid_argument("Card size mode is invalid.");
    }
    if (content.fixedColumns == 0 || content.fixedColumns > 64
        || content.fixedRows == 0 || content.fixedRows > 64) {
        throw std::invalid_argument("Card fixed grid dimensions must be between 1 and 64.");
    }
}

std::wstring FileNameKey(const std::filesystem::path& path) {
    auto result = path.filename().wstring();
    std::ranges::transform(result, result.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return result;
}

void ValidateSortMode(ApplicationItemSortMode sortMode) {
    switch (sortMode) {
    case ApplicationItemSortMode::Custom:
    case ApplicationItemSortMode::Name:
    case ApplicationItemSortMode::Size:
    case ApplicationItemSortMode::ItemType:
    case ApplicationItemSortMode::ModifiedDate:
        return;
    }
    throw std::invalid_argument("Application Card sort mode is invalid.");
}

void NormalizeAndValidatePlacements(std::vector<ApplicationItemPlacement>& placements) {
    std::unordered_set<std::wstring> unique;
    std::unordered_set<std::uint64_t> occupied;
    for (auto& placement : placements) {
        auto& item = placement.fileName;
        item = item.lexically_normal();
        const auto slot = (static_cast<std::uint64_t>(placement.row) << 32)
            | placement.column;
        if (item.empty() || item.is_absolute() || item != item.filename()
            || item == "." || item == ".." || !unique.insert(FileNameKey(item)).second
            || !occupied.insert(slot).second) {
            throw std::invalid_argument(
                "Application card placements must contain unique file names and slots.");
        }
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

void Card::setContent(CardContentPreferences preferences) {
    ValidateContent(preferences);
    content_ = preferences;
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

void ApplicationCard::setItemPlacements(std::vector<ApplicationItemPlacement> placements) {
    setLayout(sortMode_, std::move(placements));
}

void ApplicationCard::setSortMode(ApplicationItemSortMode sortMode) {
    setLayout(sortMode, itemPlacements_);
}

void ApplicationCard::setLayout(
    ApplicationItemSortMode sortMode,
    std::vector<ApplicationItemPlacement> placements) {
    ValidateSortMode(sortMode);
    NormalizeAndValidatePlacements(placements);
    sortMode_ = sortMode;
    itemPlacements_ = std::move(placements);
}

void ApplicationCard::validateContentPreferences(
    const CardContentPreferences& preferences) const {
    ValidateContent(preferences);
    if (preferences.sizeMode != CardSizeMode::Fixed) return;
    const auto outside = std::ranges::any_of(
        itemPlacements_, [&](const ApplicationItemPlacement& placement) {
            return placement.column >= preferences.fixedColumns
                || placement.row >= preferences.fixedRows;
        });
    if (outside) {
        throw std::invalid_argument(
            "Application Card custom positions do not fit the fixed grid.");
    }
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

std::string_view ToString(CardSizeMode mode) noexcept {
    switch (mode) {
    case CardSizeMode::Adaptive:
        return "adaptive";
    case CardSizeMode::Fixed:
        return "fixed";
    }
    return "adaptive";
}

std::string_view ToString(ApplicationItemSortMode mode) noexcept {
    switch (mode) {
    case ApplicationItemSortMode::Custom:
        return "custom";
    case ApplicationItemSortMode::Name:
        return "name";
    case ApplicationItemSortMode::Size:
        return "size";
    case ApplicationItemSortMode::ItemType:
        return "itemType";
    case ApplicationItemSortMode::ModifiedDate:
        return "modifiedDate";
    }
    return "custom";
}

} // namespace desto::domain
