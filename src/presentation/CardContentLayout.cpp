#include "CardContentLayout.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace desto::presentation {

CardContentLayoutSettings ResolveCardContentLayoutSettings(
    const domain::CardContentPreferences& preferences) noexcept {
    CardContentLayoutSettings result;
    switch (preferences.itemSize) {
    case domain::CardItemSize::Small:
        result.itemWidth = 52.0;
        result.iconSize = 28.0;
        break;
    case domain::CardItemSize::Medium:
        result.itemWidth = 64.0;
        result.iconSize = 40.0;
        break;
    case domain::CardItemSize::Large:
        result.itemWidth = 76.0;
        result.iconSize = 52.0;
        break;
    case domain::CardItemSize::ExtraLarge:
        result.itemWidth = 96.0;
        result.iconSize = 68.0;
        break;
    }
    result.itemHeight = preferences.showItemNames
        ? result.iconSize + 32.0
        : result.iconSize + 12.0;
    return result;
}

CardContentLayout ResolveCardContentLayout(
    std::size_t itemCount,
    double availableWidth,
    CardContentLayoutSettings settings) {
    if (!std::isfinite(availableWidth) || availableWidth <= 0
        || !std::isfinite(settings.headerHeight) || settings.headerHeight < 0
        || !std::isfinite(settings.horizontalPadding) || settings.horizontalPadding < 0
        || !std::isfinite(settings.verticalPadding) || settings.verticalPadding < 0
        || !std::isfinite(settings.itemWidth) || settings.itemWidth <= 0
        || !std::isfinite(settings.itemHeight) || settings.itemHeight <= 0
        || !std::isfinite(settings.iconSize) || settings.iconSize <= 0
        || !std::isfinite(settings.horizontalGap) || settings.horizontalGap < 0
        || !std::isfinite(settings.verticalGap) || settings.verticalGap < 0
        || settings.minimumColumns == 0
        || settings.maximumColumns < settings.minimumColumns) {
        throw std::invalid_argument("Card content layout settings must be finite and valid.");
    }

    const auto usableWidth = std::max(0.0, availableWidth - settings.horizontalPadding * 2.0);
    const auto fittingColumns = static_cast<std::size_t>(std::max(
        1.0,
        std::floor((usableWidth + settings.horizontalGap)
                   / (settings.itemWidth + settings.horizontalGap))));
    const auto columns = std::clamp(
        fittingColumns,
        settings.minimumColumns,
        settings.maximumColumns);
    const auto rows = itemCount == 0 ? std::size_t{0} : (itemCount + columns - 1) / columns;
    const auto contentWidth = columns * settings.itemWidth
        + (columns - 1) * settings.horizontalGap;
    const auto contentHeight = rows == 0
        ? 0.0
        : rows * settings.itemHeight + (rows - 1) * settings.verticalGap;
    return {
        .columns = columns,
        .rows = rows,
        .contentWidth = contentWidth,
        .contentHeight = contentHeight,
        .idealHeight = settings.headerHeight
            + (rows == 0 ? 0.0 : settings.verticalPadding * 2.0 + contentHeight),
    };
}

std::size_t ResolveCardInsertionIndex(
    std::size_t itemCount,
    double availableWidth,
    double pointerX,
    double pointerY,
    CardContentLayoutSettings settings) {
    const auto layout = ResolveCardContentLayout(itemCount + 1, availableWidth, settings);
    const auto contentLeft = (availableWidth - layout.contentWidth) / 2.0;
    const auto localX = pointerX - contentLeft;
    const auto localY = pointerY - settings.headerHeight - settings.verticalPadding;
    if (localY <= 0) {
        return 0;
    }

    const auto pitchX = settings.itemWidth + settings.horizontalGap;
    const auto pitchY = settings.itemHeight + settings.verticalGap;
    const auto row = static_cast<std::size_t>(std::max(0.0, std::floor(localY / pitchY)));
    const auto column = static_cast<std::size_t>(std::clamp(
        std::floor((localX + settings.horizontalGap * 0.5) / pitchX),
        0.0,
        static_cast<double>(layout.columns - 1)));
    return std::min(itemCount, row * layout.columns + column);
}

std::optional<std::size_t> ResolveCardSlotIndex(
    double availableWidth,
    double pointerX,
    double pointerY,
    CardContentLayoutSettings settings,
    std::optional<std::size_t> maximumRows) {
    const auto layout = ResolveCardContentLayout(0, availableWidth, settings);
    const auto contentLeft = (availableWidth - layout.contentWidth) / 2.0;
    const auto localX = pointerX - contentLeft;
    const auto localY = pointerY - settings.headerHeight - settings.verticalPadding;
    if (localX < 0 || localY < 0) return std::nullopt;
    const auto pitchX = settings.itemWidth + settings.horizontalGap;
    const auto pitchY = settings.itemHeight + settings.verticalGap;
    const auto column = static_cast<std::size_t>(std::floor(localX / pitchX));
    const auto row = static_cast<std::size_t>(std::floor(localY / pitchY));
    if (column >= layout.columns
        || (maximumRows.has_value() && row >= *maximumRows)
        || std::fmod(localX, pitchX) >= settings.itemWidth
        || std::fmod(localY, pitchY) >= settings.itemHeight) {
        return std::nullopt;
    }
    return row * layout.columns + column;
}

} // namespace desto::presentation
