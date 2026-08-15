#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace desto::domain {

using CardId = std::string;

enum class CardType {
    Application,
    FolderMapping,
    Reference,
    Todo,
};

struct CardRect {
    double left = 0;
    double top = 0;
    double width = 320;
    double height = 220;
};

struct CardChromePreferences {
    bool showCollapseControl = true;
    bool showCloseControl = true;
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

class Card {
public:
    virtual ~Card() = default;

    [[nodiscard]] const CardId& id() const noexcept { return id_; }
    [[nodiscard]] CardType type() const noexcept { return type_; }
    [[nodiscard]] const CardRect& rect() const noexcept { return rect_; }
    [[nodiscard]] bool isVisible() const noexcept { return visible_; }
    [[nodiscard]] bool isExpanded() const noexcept { return expanded_; }
    [[nodiscard]] const CardChromePreferences& chrome() const noexcept { return chrome_; }
    [[nodiscard]] const CardAppearancePreferences& appearance() const noexcept { return appearance_; }

    void setRect(CardRect rect);
    void setVisible(bool visible) noexcept { visible_ = visible; }
    void setExpanded(bool expanded) noexcept { expanded_ = expanded; }
    void setChrome(CardChromePreferences preferences);
    void setAppearance(CardAppearancePreferences preferences);

protected:
    Card(CardId id, CardType type);

private:
    CardId id_;
    CardType type_;
    CardRect rect_;
    bool visible_ = true;
    bool expanded_ = true;
    CardChromePreferences chrome_;
    CardAppearancePreferences appearance_;
};

class ApplicationCard final : public Card {
public:
    ApplicationCard(CardId id, std::filesystem::path managedRoot);

    [[nodiscard]] const std::filesystem::path& managedRoot() const noexcept { return managedRoot_; }
    void setManagedRoot(std::filesystem::path managedRoot);

private:
    std::filesystem::path managedRoot_;
};

class FolderMappingCard final : public Card {
public:
    FolderMappingCard(CardId id, std::filesystem::path sourceRoot);

    [[nodiscard]] const std::filesystem::path& sourceRoot() const noexcept { return sourceRoot_; }
    [[nodiscard]] bool allowsSourceMutation() const noexcept { return allowsSourceMutation_; }
    void setSourceRoot(std::filesystem::path sourceRoot);
    void setAllowsSourceMutation(bool allowed) noexcept { allowsSourceMutation_ = allowed; }

private:
    std::filesystem::path sourceRoot_;
    bool allowsSourceMutation_ = false;
};

class ReferenceCard final : public Card {
public:
    explicit ReferenceCard(CardId id);

    [[nodiscard]] const std::vector<FileReference>& references() const noexcept { return references_; }
    void setReferences(std::vector<FileReference> references);

private:
    std::vector<FileReference> references_;
};

class TodoCard final : public Card {
public:
    explicit TodoCard(CardId id);

    [[nodiscard]] const std::vector<TodoItem>& items() const noexcept { return items_; }
    void setItems(std::vector<TodoItem> items);

private:
    std::vector<TodoItem> items_;
};

[[nodiscard]] std::string_view ToString(CardType type) noexcept;

} // namespace desto::domain
