#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
    std::uintmax_t fileSize = 0;
    std::wstring itemType;
    std::int64_t modifiedTime = 0;
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
    bool showPresentationControl = true;
    bool pinOnTop = false;
    bool positionLocked = false;
    std::string appearancePreset = "system";
    double opacity = 1.0;
    double cornerRadius = 16.0;
    domain::CardContentPreferences content;
    domain::ApplicationItemSortMode applicationSortMode =
        domain::ApplicationItemSortMode::Custom;
    std::vector<domain::ApplicationItemPlacement> applicationItemPlacements;
    domain::MappingMode mappingMode = domain::MappingMode::References;
    domain::MappingPresentationMode mappingPresentationMode =
        domain::MappingPresentationMode::Grid;
    domain::ApplicationItemSortMode mappingSortMode =
        domain::ApplicationItemSortMode::Custom;
    bool mappingHasSource = false;
    bool mappingAllowsSourceMutation = true;
    bool mappingCanNavigateUp = false;
    domain::TodoCardPreferences todoPreferences;
    std::vector<domain::TodoItem> todoItems;
    std::vector<CardItemView> items;
};

[[nodiscard]] CardView MakeCardView(
    const domain::Card& card,
    std::string_view resolvedLanguage = "zh-CN");

} // namespace desto::presentation
