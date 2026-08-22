#include "Dialog.h"

#include <utility>

namespace desto::ui {
namespace {

constexpr int kHorizontalInset = 24;
constexpr int kTitleTop = 20;
constexpr int kTitleHeight = 34;
constexpr int kMessageTop = 70;
constexpr int kButtonWidth = 96;
constexpr int kButtonHeight = 36;
constexpr int kButtonRightInset = 22;
constexpr int kButtonBottomInset = 20;
constexpr int kButtonGap = 10;

} // namespace

bool Rect::contains(Point point) const noexcept {
    return point.x >= x && point.y >= y
        && point.x < x + width && point.y < y + height;
}

Dialog::Dialog(DialogSpec spec) : spec_(std::move(spec)) {}

void Dialog::layout(Size surfaceSize) noexcept {
    surfaceSize_.width = surfaceSize.width > 0 ? surfaceSize.width : 0;
    surfaceSize_.height = surfaceSize.height > 0 ? surfaceSize.height : 0;
}

DialogSnapshot Dialog::snapshot() const {
    const auto buttonTop = surfaceSize_.height - kButtonBottomInset - kButtonHeight;
    const auto confirmLeft = surfaceSize_.width - kButtonRightInset - kButtonWidth;
    const Rect confirmBounds{confirmLeft, buttonTop, kButtonWidth, kButtonHeight};
    DialogSnapshot result{
        .titleBounds = {
            kHorizontalInset,
            kTitleTop,
            surfaceSize_.width - kHorizontalInset * 2,
            kTitleHeight,
        },
        .messageBounds = {
            kHorizontalInset,
            kMessageTop,
            surfaceSize_.width - kHorizontalInset * 2,
            buttonTop - kMessageTop - 20,
        },
        .confirmButton = {
            .bounds = confirmBounds,
            .label = spec_.confirmLabel,
            .focused = focus_ == Focus::Confirm,
            .primary = true,
        },
    };
    if (!spec_.cancelLabel.empty()) {
        result.cancelButton = ButtonSnapshot{
            .bounds = {
                confirmLeft - kButtonGap - kButtonWidth,
                buttonTop,
                kButtonWidth,
                kButtonHeight,
            },
            .label = spec_.cancelLabel,
            .focused = focus_ == Focus::Cancel,
            .primary = false,
        };
    }
    return result;
}

std::optional<DialogAction> Dialog::pointerReleased(Point point) const noexcept {
    const auto buttonTop = surfaceSize_.height - kButtonBottomInset - kButtonHeight;
    const auto confirmLeft = surfaceSize_.width - kButtonRightInset - kButtonWidth;
    if (Rect{confirmLeft, buttonTop, kButtonWidth, kButtonHeight}.contains(point)) {
        return DialogAction::Confirm;
    }
    if (!spec_.cancelLabel.empty()
        && Rect{confirmLeft - kButtonGap - kButtonWidth,
                buttonTop,
                kButtonWidth,
                kButtonHeight}.contains(point)) {
        return DialogAction::Cancel;
    }
    return std::nullopt;
}

std::optional<DialogAction> Dialog::keyPressed(
    DialogKey key, bool reverseFocus) noexcept {
    if (key == DialogKey::Escape) return DialogAction::Cancel;
    if (key == DialogKey::Enter) {
        return focus_ == Focus::Confirm ? DialogAction::Confirm : DialogAction::Cancel;
    }
    if (key == DialogKey::Tab && !spec_.cancelLabel.empty()) {
        (void)reverseFocus;
        focus_ = focus_ == Focus::Confirm ? Focus::Cancel : Focus::Confirm;
    }
    return std::nullopt;
}

} // namespace desto::ui
