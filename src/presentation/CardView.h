#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Card.h"

namespace desto::presentation {

enum class CardItemState {
    Ready,
    Missing,
    UnresolvedShortcut,
    IconUnavailable,
};

struct CardItemIcon {
    int width = 0;
    int height = 0;
    std::shared_ptr<const std::vector<std::uint32_t>> premultipliedPixels;

    [[nodiscard]] bool empty() const noexcept {
        return width <= 0 || height <= 0 || premultipliedPixels == nullptr
            || premultipliedPixels->size()
                != static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    }
};

struct CardItemView {
    std::wstring id;
    std::wstring displayName;
    std::filesystem::path sourcePath;
    std::filesystem::path resolvedTargetPath;
    std::wstring appUserModelId;
    CardItemState state = CardItemState::Ready;
    CardItemIcon icon;
};

struct CardView {
    domain::CardId id;
    domain::CardType type = domain::CardType::Todo;
    std::wstring title;
    std::wstring typeLabel;
    bool visible = true;
    bool expanded = true;
    bool showTitle = true;
    bool showCollapseControl = true;
    bool showCloseControl = false;
    bool showPinControl = false;
    std::string appearancePreset = "default";
    double opacity = 1.0;
    double cornerRadius = 16.0;
    domain::CardContentPreferences content;
    std::vector<CardItemView> items;
};

[[nodiscard]] CardView MakeCardView(const domain::Card& card);

} // namespace desto::presentation
