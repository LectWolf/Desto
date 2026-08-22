#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace desto::ui {

struct TextSelection {
    std::size_t anchor = 0;
    std::size_t caret = 0;

    bool operator==(const TextSelection&) const = default;
};

struct TextChange {
    std::size_t start = 0;
    std::size_t oldEnd = 0;
    std::size_t newEnd = 0;

    bool operator==(const TextChange&) const = default;
};

enum class TextMovement {
    Start,
    End,
    PreviousCharacter,
    NextCharacter,
    PreviousWord,
    NextWord,
};

class TextInputModel final {
public:
    explicit TextInputModel(std::size_t maximumLength, std::wstring text = {});

    [[nodiscard]] const std::wstring& text() const noexcept { return text_; }
    [[nodiscard]] TextSelection selection() const noexcept { return selection_; }
    [[nodiscard]] bool canUndo() const noexcept { return !undoStates_.empty(); }

    [[nodiscard]] TextChange setText(std::wstring text);
    void setSelection(std::size_t anchor, std::size_t caret) noexcept;
    void move(TextMovement movement, bool extend) noexcept;
    [[nodiscard]] TextChange replace(
        std::size_t start, std::size_t end, std::wstring_view text, bool rememberUndo);
    [[nodiscard]] TextChange replaceSelection(
        std::wstring_view text, bool rememberUndo);
    [[nodiscard]] std::optional<TextChange> undo();

private:
    struct State {
        std::wstring text;
        TextSelection selection;
    };

    std::size_t maximumLength_;
    std::wstring text_;
    TextSelection selection_;
    std::vector<State> undoStates_;

    [[nodiscard]] std::size_t target(TextMovement movement) const noexcept;
};

} // namespace desto::ui
