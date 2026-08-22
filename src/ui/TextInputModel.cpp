#include "TextInputModel.h"

#include <algorithm>
#include <cwctype>
#include <utility>

namespace desto::ui {
namespace {

std::size_t PreviousCodePoint(std::wstring_view text, std::size_t position) noexcept {
    position = (std::min)(position, text.size());
    if (position == 0) return 0;
    auto result = position - 1;
    if (result > 0 && text[result] >= 0xDC00 && text[result] <= 0xDFFF
        && text[result - 1] >= 0xD800 && text[result - 1] <= 0xDBFF) {
        --result;
    }
    return result;
}

std::size_t NextCodePoint(std::wstring_view text, std::size_t position) noexcept {
    position = (std::min)(position, text.size());
    if (position >= text.size()) return text.size();
    auto result = position + 1;
    if (result < text.size() && text[position] >= 0xD800 && text[position] <= 0xDBFF
        && text[result] >= 0xDC00 && text[result] <= 0xDFFF) {
        ++result;
    }
    return result;
}

} // namespace

TextInputModel::TextInputModel(std::size_t maximumLength, std::wstring text)
    : maximumLength_(maximumLength), text_(std::move(text)) {
    if (text_.size() > maximumLength_) text_.resize(maximumLength_);
    selection_ = {text_.size(), text_.size()};
}

TextChange TextInputModel::setText(std::wstring text) {
    const auto oldSize = text_.size();
    if (text.size() > maximumLength_) text.resize(maximumLength_);
    text_ = std::move(text);
    selection_ = {text_.size(), text_.size()};
    return {0, oldSize, text_.size()};
}

void TextInputModel::setSelection(std::size_t anchor, std::size_t caret) noexcept {
    selection_.anchor = (std::min)(anchor, text_.size());
    selection_.caret = (std::min)(caret, text_.size());
}

void TextInputModel::move(TextMovement movement, bool extend) noexcept {
    auto next = target(movement);
    if (!extend && selection_.anchor != selection_.caret) {
        if (movement == TextMovement::PreviousCharacter
            || movement == TextMovement::PreviousWord) {
            next = (std::min)(selection_.anchor, selection_.caret);
        } else if (movement == TextMovement::NextCharacter
                   || movement == TextMovement::NextWord) {
            next = (std::max)(selection_.anchor, selection_.caret);
        }
    }
    selection_.caret = next;
    if (!extend) selection_.anchor = next;
}

TextChange TextInputModel::replace(
    std::size_t start,
    std::size_t end,
    std::wstring_view text,
    bool rememberUndo) {
    start = (std::min)(start, text_.size());
    end = std::clamp(end, start, text_.size());
    const auto available = maximumLength_ - (text_.size() - (end - start));
    text = text.substr(0, (std::min)(available, text.size()));
    if (rememberUndo) {
        if (undoStates_.size() >= 100) undoStates_.erase(undoStates_.begin());
        undoStates_.push_back({text_, selection_});
    }
    text_.replace(start, end - start, text);
    selection_ = {start + text.size(), start + text.size()};
    return {start, end, selection_.caret};
}

TextChange TextInputModel::replaceSelection(
    std::wstring_view text, bool rememberUndo) {
    return replace(
        (std::min)(selection_.anchor, selection_.caret),
        (std::max)(selection_.anchor, selection_.caret),
        text,
        rememberUndo);
}

std::optional<TextChange> TextInputModel::undo() {
    if (undoStates_.empty()) return std::nullopt;
    const auto oldSize = text_.size();
    auto state = std::move(undoStates_.back());
    undoStates_.pop_back();
    text_ = std::move(state.text);
    selection_ = state.selection;
    return TextChange{0, oldSize, text_.size()};
}

std::size_t TextInputModel::target(TextMovement movement) const noexcept {
    switch (movement) {
    case TextMovement::Start:
        return 0;
    case TextMovement::End:
        return text_.size();
    case TextMovement::PreviousCharacter:
        return PreviousCodePoint(text_, selection_.caret);
    case TextMovement::NextCharacter:
        return NextCodePoint(text_, selection_.caret);
    case TextMovement::PreviousWord: {
        auto result = (std::min)(selection_.caret, text_.size());
        while (result > 0) {
            const auto previous = PreviousCodePoint(text_, result);
            if (std::iswspace(text_[previous]) == 0) break;
            result = previous;
        }
        while (result > 0) {
            const auto previous = PreviousCodePoint(text_, result);
            if (std::iswspace(text_[previous]) != 0) break;
            result = previous;
        }
        return result;
    }
    case TextMovement::NextWord: {
        auto result = (std::min)(selection_.caret, text_.size());
        while (result < text_.size() && std::iswspace(text_[result]) == 0) {
            result = NextCodePoint(text_, result);
        }
        while (result < text_.size() && std::iswspace(text_[result]) != 0) {
            result = NextCodePoint(text_, result);
        }
        return result;
    }
    }
    return selection_.caret;
}

} // namespace desto::ui
