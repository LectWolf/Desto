#pragma once

#include <filesystem>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace desto::domain {

using CardId = std::string;

enum class CardType {
    Application,
    Mapping,
    Todo,
};

enum class CardDeletionEffect {
    ReturnManagedItemsToDesktop,
    RemoveCardOnly,
};

enum class MappingMode {
    Empty,
    Folder,
    References,
};

enum class MappingPresentationMode {
    Grid,
    List,
};

enum class CardItemSize {
    Small,
    Medium,
    Large,
    ExtraLarge,
};

enum class CardSizeMode {
    Adaptive,
    Fixed,
};

enum class ApplicationItemSortMode {
    Custom,
    Name,
    Size,
    ItemType,
    ModifiedDate,
};

struct ApplicationItemPlacement {
    std::filesystem::path fileName;
    std::uint32_t column = 0;
    std::uint32_t row = 0;

    bool operator==(const ApplicationItemPlacement&) const = default;
};

struct CardDeletionPreview {
    CardId cardId;
    CardType cardType;
    CardDeletionEffect effect;
    bool requiresConfirmation = true;
};

struct CardChromePreferences {
    bool showCollapseControl = true;
    bool showCloseControl = false;
    bool showPinControl = false;
    bool showPresentationControl = true;
    bool pinOnTop = false;
    bool showTitle = true;
    bool positionLocked = false;

    bool operator==(const CardChromePreferences&) const = default;
};

struct CardAppearancePreferences {
    std::string preset = "system";
    double opacity = 1.0;
    double cornerRadius = 16.0;

    bool operator==(const CardAppearancePreferences&) const = default;
};

struct CardContentPreferences {
    CardItemSize itemSize = CardItemSize::Large;
    bool showItemNames = false;
    CardSizeMode sizeMode = CardSizeMode::Adaptive;
    std::uint32_t widthSpan = 4;
    std::uint32_t fixedColumns = 4;
    std::uint32_t fixedRows = 3;
    std::optional<std::uint32_t> maximumVisibleRows;

    bool operator==(const CardContentPreferences&) const = default;
};

struct FileReference {
    std::string id;
    std::filesystem::path path;

    bool operator==(const FileReference&) const = default;
};

struct TodoDate {
    std::int32_t year = 1970;
    std::uint8_t month = 1;
    std::uint8_t day = 1;

    bool operator==(const TodoDate&) const = default;
    bool operator<(const TodoDate& other) const noexcept {
        return std::tie(year, month, day)
            < std::tie(other.year, other.month, other.day);
    }
};

[[nodiscard]] bool IsValidTodoDate(TodoDate date) noexcept;
[[nodiscard]] TodoDate CurrentSystemTodoDate() noexcept;
[[nodiscard]] TodoDate CurrentTodoDate(
    std::optional<std::int32_t> offsetMinutes = std::nullopt) noexcept;
[[nodiscard]] TodoDate TodoDateAtUnixMilliseconds(
    std::int64_t unixMilliseconds,
    std::optional<std::int32_t> offsetMinutes = std::nullopt) noexcept;
[[nodiscard]] TodoDate AddTodoDays(TodoDate date, std::int32_t days) noexcept;
[[nodiscard]] std::int32_t CompareTodoDates(TodoDate left, TodoDate right) noexcept;
[[nodiscard]] std::string ToString(TodoDate date);

struct TodoItem {
    std::string id;
    std::string title;
    bool completed = false;
    std::int64_t createdAtUnixMilliseconds = 0;
    std::int64_t completedAtUnixMilliseconds = 0;
    std::optional<TodoDate> scheduledDate;
    bool archived = false;

    bool operator==(const TodoItem&) const = default;
};

[[nodiscard]] bool IsTodoItemArchived(
    const TodoItem& item,
    TodoDate currentDate,
    std::optional<std::int32_t> offsetMinutes = std::nullopt) noexcept;

struct TodoCardPreferences {
    bool showCreatedTime = false;

    bool operator==(const TodoCardPreferences&) const = default;
};

// Persistence-neutral value representation used at the storage seam.
struct CardSnapshot {
    CardId id;
    std::string name;
    CardType type = CardType::Todo;
    bool visible = true;
    bool expanded = true;
    CardChromePreferences chrome;
    CardAppearancePreferences appearance;
    CardContentPreferences content;
    std::filesystem::path applicationStoragePath;
    ApplicationItemSortMode applicationSortMode = ApplicationItemSortMode::Custom;
    std::vector<ApplicationItemPlacement> applicationItemPlacements;
    MappingPresentationMode applicationPresentationMode = MappingPresentationMode::Grid;
    std::filesystem::path mappingSourceRoot;
    std::vector<FileReference> mappingReferences;
    bool mappingAllowsSourceMutation = true;
    MappingMode mappingMode = MappingMode::References;
    MappingPresentationMode mappingPresentationMode = MappingPresentationMode::Grid;
    ApplicationItemSortMode mappingSortMode = ApplicationItemSortMode::Custom;
    std::vector<ApplicationItemPlacement> mappingItemPlacements;
    TodoCardPreferences todoPreferences;
    std::vector<TodoItem> todoItems;
};

class Card {
public:
    virtual ~Card() = default;

    [[nodiscard]] const CardId& id() const noexcept { return id_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] CardType type() const noexcept { return type_; }
    [[nodiscard]] bool isVisible() const noexcept { return visible_; }
    [[nodiscard]] bool isExpanded() const noexcept { return expanded_; }
    [[nodiscard]] const CardChromePreferences& chrome() const noexcept { return chrome_; }
    [[nodiscard]] const CardAppearancePreferences& appearance() const noexcept { return appearance_; }
    [[nodiscard]] const CardContentPreferences& content() const noexcept { return content_; }
    [[nodiscard]] bool requiresDeletionConfirmation() const noexcept { return true; }
    [[nodiscard]] CardDeletionPreview deletionPreview() const noexcept;
    [[nodiscard]] virtual CardDeletionEffect deletionEffect() const noexcept = 0;

    void setVisible(bool visible) noexcept { visible_ = visible; }
    void setName(std::string name);
    void setExpanded(bool expanded) noexcept { expanded_ = expanded; }
    void setChrome(CardChromePreferences preferences);
    void setAppearance(CardAppearancePreferences preferences);
    void setContent(CardContentPreferences preferences);

protected:
    Card(CardId id, CardType type);

private:
    CardId id_;
    std::string name_;
    CardType type_;
    bool visible_ = true;
    bool expanded_ = true;
    CardChromePreferences chrome_;
    CardAppearancePreferences appearance_;
    CardContentPreferences content_;
};

class ApplicationCard final : public Card {
public:
    ApplicationCard(CardId id, std::filesystem::path relativeStoragePath);

    [[nodiscard]] CardDeletionEffect deletionEffect() const noexcept override {
        return CardDeletionEffect::ReturnManagedItemsToDesktop;
    }
    [[nodiscard]] const std::filesystem::path& relativeStoragePath() const noexcept { return relativeStoragePath_; }
    [[nodiscard]] ApplicationItemSortMode sortMode() const noexcept { return sortMode_; }
    [[nodiscard]] const std::vector<ApplicationItemPlacement>& itemPlacements() const noexcept {
        return itemPlacements_;
    }
    [[nodiscard]] MappingPresentationMode presentationMode() const noexcept {
        return presentationMode_;
    }
    void setRelativeStoragePath(std::filesystem::path relativeStoragePath);
    void setSortMode(ApplicationItemSortMode sortMode);
    void setItemPlacements(std::vector<ApplicationItemPlacement> placements);
    void setLayout(
        ApplicationItemSortMode sortMode,
        std::vector<ApplicationItemPlacement> placements);
    void validateContentPreferences(const CardContentPreferences& preferences) const;
    void setPresentationMode(MappingPresentationMode mode) noexcept {
        presentationMode_ = mode;
    }

private:
    std::filesystem::path relativeStoragePath_;
    ApplicationItemSortMode sortMode_ = ApplicationItemSortMode::Custom;
    std::vector<ApplicationItemPlacement> itemPlacements_;
    MappingPresentationMode presentationMode_ = MappingPresentationMode::Grid;
};

class MappingCard final : public Card {
public:
    explicit MappingCard(CardId id);

    [[nodiscard]] CardDeletionEffect deletionEffect() const noexcept override {
        return CardDeletionEffect::RemoveCardOnly;
    }
    [[nodiscard]] MappingMode mode() const noexcept { return mode_; }
    [[nodiscard]] MappingPresentationMode presentationMode() const noexcept {
        return presentationMode_;
    }
    [[nodiscard]] ApplicationItemSortMode sortMode() const noexcept { return sortMode_; }
    [[nodiscard]] const std::vector<ApplicationItemPlacement>& itemPlacements() const noexcept {
        return itemPlacements_;
    }
    [[nodiscard]] bool presentsAsFolderMapping() const noexcept { return mode() != MappingMode::References; }
    [[nodiscard]] const std::filesystem::path& sourceRoot() const noexcept { return sourceRoot_; }
    [[nodiscard]] bool allowsSourceMutation() const noexcept { return allowsSourceMutation_; }
    [[nodiscard]] const std::vector<FileReference>& references() const noexcept { return references_; }
    void setFolderSource(std::filesystem::path sourceRoot);
    void setReferences(std::vector<FileReference> references);
    void setMode(MappingMode mode);
    void setPresentationMode(MappingPresentationMode mode) noexcept {
        presentationMode_ = mode;
    }
    void setSortMode(ApplicationItemSortMode mode);
    void setItemPlacements(std::vector<ApplicationItemPlacement> placements);
    void setLayout(
        ApplicationItemSortMode mode,
        std::vector<ApplicationItemPlacement> placements);
    void clearSource() noexcept;
    void setAllowsSourceMutation(bool allowed) noexcept { allowsSourceMutation_ = allowed; }

private:
    std::filesystem::path sourceRoot_;
    std::vector<FileReference> references_;
    bool allowsSourceMutation_ = true;
    MappingMode mode_ = MappingMode::References;
    MappingPresentationMode presentationMode_ = MappingPresentationMode::Grid;
    ApplicationItemSortMode sortMode_ = ApplicationItemSortMode::Custom;
    std::vector<ApplicationItemPlacement> itemPlacements_;
};

class TodoCard final : public Card {
public:
    explicit TodoCard(CardId id);

    [[nodiscard]] CardDeletionEffect deletionEffect() const noexcept override {
        return CardDeletionEffect::RemoveCardOnly;
    }
    [[nodiscard]] const std::vector<TodoItem>& items() const noexcept { return items_; }
    [[nodiscard]] const TodoCardPreferences& preferences() const noexcept { return preferences_; }
    void setItems(std::vector<TodoItem> items);
    void setPreferences(TodoCardPreferences preferences) noexcept { preferences_ = preferences; }

private:
    TodoCardPreferences preferences_;
    std::vector<TodoItem> items_;
};

[[nodiscard]] std::string_view ToString(CardType type) noexcept;
[[nodiscard]] std::string_view ToString(CardItemSize size) noexcept;
[[nodiscard]] std::uint32_t MinimumCardWidthSpan(CardItemSize size) noexcept;
[[nodiscard]] std::size_t ProjectCardColumns(
    std::uint32_t widthSpan,
    CardItemSize size) noexcept;
[[nodiscard]] std::uint32_t InferCardWidthSpan(
    std::size_t columns,
    CardItemSize size) noexcept;
[[nodiscard]] std::uint32_t FitCardWidthSpan(
    std::size_t minimumColumns,
    CardItemSize size) noexcept;
[[nodiscard]] std::string_view ToString(CardSizeMode mode) noexcept;
[[nodiscard]] std::string_view ToString(ApplicationItemSortMode mode) noexcept;

} // namespace desto::domain
