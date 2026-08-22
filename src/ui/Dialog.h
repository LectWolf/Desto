#pragma once

#include <optional>
#include <string>

namespace desto::ui {

struct Point {
    int x = 0;
    int y = 0;
};

struct Size {
    int width = 0;
    int height = 0;
};

struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool operator==(const Rect&) const = default;

    [[nodiscard]] bool contains(Point point) const noexcept;
};

enum class DialogAction {
    Confirm,
    Cancel,
};

enum class DialogKey {
    Enter,
    Escape,
    Tab,
};

struct DialogSpec {
    std::wstring title;
    std::wstring message;
    std::wstring confirmLabel;
    std::wstring cancelLabel;
};

struct ButtonSnapshot {
    Rect bounds;
    std::wstring label;
    bool focused = false;
    bool primary = false;
};

struct DialogSnapshot {
    Rect titleBounds;
    Rect messageBounds;
    ButtonSnapshot confirmButton;
    std::optional<ButtonSnapshot> cancelButton;
};

class Dialog final {
public:
    explicit Dialog(DialogSpec spec);

    void layout(Size surfaceSize) noexcept;
    [[nodiscard]] std::optional<DialogAction> pointerReleased(Point point) const noexcept;
    [[nodiscard]] std::optional<DialogAction> keyPressed(
        DialogKey key, bool reverseFocus = false) noexcept;
    [[nodiscard]] DialogSnapshot snapshot() const;
    [[nodiscard]] const DialogSpec& spec() const noexcept { return spec_; }

private:
    enum class Focus {
        Confirm,
        Cancel,
    };

    DialogSpec spec_;
    Size surfaceSize_{};
    Focus focus_ = Focus::Confirm;
};

} // namespace desto::ui
