#pragma once

#include <filesystem>
#include <string>
#include <string_view>
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

struct CardDeletionPreview {
    CardId cardId;
    CardType cardType;
    CardDeletionEffect effect;
    bool requiresConfirmation = true;
};

struct CardChromePreferences {
    bool showCollapseControl = true;
    bool showCloseControl = true;
    bool showPinControl = true;
    bool showTitle = true;
};

struct CardAppearancePreferences {
    std::string preset = "default";
    double opacity = 1.0;
};

struct FileReference {
    std::string id;
    std::filesystem::path path;
};

struct TodoItem {
    std::string id;
    std::string title;
    bool completed = false;
};

// Persistence-neutral value representation used at the storage seam.
struct CardSnapshot {
    CardId id;
    CardType type = CardType::Todo;
    bool visible = true;
    bool expanded = true;
    CardChromePreferences chrome;
    CardAppearancePreferences appearance;
    std::filesystem::path applicationStoragePath;
    std::filesystem::path mappingSourceRoot;
    std::vector<FileReference> mappingReferences;
    bool mappingAllowsSourceMutation = true;
    std::vector<TodoItem> todoItems;
};

class Card {
public:
    virtual ~Card() = default;

    [[nodiscard]] const CardId& id() const noexcept { return id_; }
    [[nodiscard]] CardType type() const noexcept { return type_; }
    [[nodiscard]] bool isVisible() const noexcept { return visible_; }
    [[nodiscard]] bool isExpanded() const noexcept { return expanded_; }
    [[nodiscard]] const CardChromePreferences& chrome() const noexcept { return chrome_; }
    [[nodiscard]] const CardAppearancePreferences& appearance() const noexcept { return appearance_; }
    [[nodiscard]] bool requiresDeletionConfirmation() const noexcept { return true; }
    [[nodiscard]] CardDeletionPreview deletionPreview() const noexcept;
    [[nodiscard]] virtual CardDeletionEffect deletionEffect() const noexcept = 0;

    void setVisible(bool visible) noexcept { visible_ = visible; }
    void setExpanded(bool expanded) noexcept { expanded_ = expanded; }
    void setChrome(CardChromePreferences preferences);
    void setAppearance(CardAppearancePreferences preferences);

protected:
    Card(CardId id, CardType type);

private:
    CardId id_;
    CardType type_;
    bool visible_ = true;
    bool expanded_ = true;
    CardChromePreferences chrome_;
    CardAppearancePreferences appearance_;
};

class ApplicationCard final : public Card {
public:
    ApplicationCard(CardId id, std::filesystem::path relativeStoragePath);

    [[nodiscard]] CardDeletionEffect deletionEffect() const noexcept override {
        return CardDeletionEffect::ReturnManagedItemsToDesktop;
    }
    [[nodiscard]] const std::filesystem::path& relativeStoragePath() const noexcept { return relativeStoragePath_; }
    void setRelativeStoragePath(std::filesystem::path relativeStoragePath);

private:
    std::filesystem::path relativeStoragePath_;
};

class MappingCard final : public Card {
public:
    explicit MappingCard(CardId id);

    [[nodiscard]] CardDeletionEffect deletionEffect() const noexcept override {
        return CardDeletionEffect::RemoveCardOnly;
    }
    [[nodiscard]] MappingMode mode() const noexcept;
    [[nodiscard]] bool presentsAsFolderMapping() const noexcept { return mode() != MappingMode::References; }
    [[nodiscard]] const std::filesystem::path& sourceRoot() const noexcept { return sourceRoot_; }
    [[nodiscard]] bool allowsSourceMutation() const noexcept { return allowsSourceMutation_; }
    [[nodiscard]] const std::vector<FileReference>& references() const noexcept { return references_; }
    void setFolderSource(std::filesystem::path sourceRoot);
    void setReferences(std::vector<FileReference> references);
    void clearSource() noexcept;
    void setAllowsSourceMutation(bool allowed) noexcept { allowsSourceMutation_ = allowed; }

private:
    std::filesystem::path sourceRoot_;
    std::vector<FileReference> references_;
    bool allowsSourceMutation_ = true;
};

class TodoCard final : public Card {
public:
    explicit TodoCard(CardId id);

    [[nodiscard]] CardDeletionEffect deletionEffect() const noexcept override {
        return CardDeletionEffect::RemoveCardOnly;
    }
    [[nodiscard]] const std::vector<TodoItem>& items() const noexcept { return items_; }
    void setItems(std::vector<TodoItem> items);

private:
    std::vector<TodoItem> items_;
};

[[nodiscard]] std::string_view ToString(CardType type) noexcept;

} // namespace desto::domain
