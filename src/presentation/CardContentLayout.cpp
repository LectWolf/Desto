#include "CardContentLayout.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace desto::presentation {

CardContentLayoutSettings ResolveCardContentLayoutSettings(
    const domain::CardContentPreferences& preferences) noexcept {
    CardContentLayoutSettings result;
    double fileNameHeight = 16.0;
    switch (preferences.itemSize) {
    case domain::CardItemSize::Small:
        result.itemWidth = 37.0;
        result.iconSize = 20.0;
        result.itemFontSize = 8.0;
        fileNameHeight = 12.0;
        result.preferredColumns = 6;
        break;
    case domain::CardItemSize::Medium:
        result.itemWidth = 44.0;
        result.iconSize = 26.0;
        result.itemFontSize = 9.0;
        fileNameHeight = 14.0;
        result.preferredColumns = 5;
        break;
    case domain::CardItemSize::Large:
        result.itemWidth = 55.0;
        result.iconSize = 34.0;
        result.itemFontSize = 10.0;
        fileNameHeight = 16.0;
        result.preferredColumns = 4;
        break;
    case domain::CardItemSize::ExtraLarge:
        result.itemWidth = 74.0;
        result.iconSize = 44.0;
        result.itemFontSize = 11.0;
        fileNameHeight = 32.0;
        result.preferredColumns = 3;
        break;
    }
    result.horizontalGap = 0.0;
    result.verticalGap = 0.0;
    result.itemHeight = preferences.showItemNames
        ? result.itemWidth + fileNameHeight
        : result.itemWidth;
    return result;
}

std::size_t ResolveAdaptiveCardColumns(
    std::size_t itemCount,
    std::size_t requiredColumns,
    CardContentLayoutSettings settings) noexcept {
    const auto contentColumns = std::max(settings.preferredColumns, itemCount);
    return std::max(
        requiredColumns,
        std::clamp(contentColumns, settings.minimumColumns, settings.maximumColumns));
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
        || !std::isfinite(settings.itemFontSize) || settings.itemFontSize <= 0
        || !std::isfinite(settings.horizontalGap) || settings.horizontalGap < 0
        || !std::isfinite(settings.verticalGap) || settings.verticalGap < 0
        || settings.minimumColumns == 0
        || settings.maximumColumns < settings.minimumColumns
        || settings.preferredColumns < settings.minimumColumns
        || settings.preferredColumns > settings.maximumColumns) {
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
    if (localX >= layout.contentWidth) {
        return itemCount;
    }
    if (localY >= layout.contentHeight + settings.verticalGap) {
        return itemCount;
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

CardDropPreview ResolveAdaptiveCardDropPreview(
    std::size_t occupiedSlotCount,
    double availableWidth,
    double pointerX,
    double pointerY,
    CardContentLayoutSettings settings,
    std::optional<CardDropPreview> previousPreview) {
    constexpr double edgeExpansionThreshold = 14.0;
    const auto baseColumns = std::clamp(
        settings.preferredColumns,
        settings.minimumColumns,
        settings.maximumColumns);
    const auto baseRows = std::max<std::size_t>(
        1,
        (std::max<std::size_t>(1, occupiedSlotCount) + baseColumns - 1)
            / baseColumns);
    const auto previousColumns = previousPreview.has_value()
        ? previousPreview->columns : baseColumns;
    const auto visibleColumns = std::clamp(
        std::max(baseColumns, previousColumns),
        settings.minimumColumns,
        settings.maximumColumns);
    const auto previousRows = previousPreview.has_value()
        ? previousPreview->insertionIndex / visibleColumns + 1
        : baseRows;
    const auto visibleRows = std::max(baseRows, previousRows);
    const auto contentWidth = visibleColumns * settings.itemWidth
        + (visibleColumns - 1) * settings.horizontalGap;
    const auto contentHeight = visibleRows * settings.itemHeight
        + (visibleRows - 1) * settings.verticalGap;
    const auto contentLeft = (availableWidth - contentWidth) / 2.0;
    const auto localX = pointerX - contentLeft;
    const auto localY = pointerY - settings.headerHeight - settings.verticalPadding;
    const auto pitchX = settings.itemWidth + settings.horizontalGap;
    const auto pitchY = settings.itemHeight + settings.verticalGap;
    auto column = static_cast<std::size_t>(std::clamp(
        std::floor(std::max(0.0, localX) / pitchX),
        0.0,
        static_cast<double>(visibleColumns - 1)));
    auto columns = std::max(baseColumns, column + 1);
    if (localX >= contentWidth - edgeExpansionThreshold
        && visibleColumns < settings.maximumColumns) {
        column = visibleColumns;
        columns = visibleColumns + 1;
    }

    auto row = static_cast<std::size_t>(std::clamp(
        std::floor(std::max(0.0, localY) / pitchY),
        0.0,
        static_cast<double>(visibleRows - 1)));
    if (localY >= contentHeight - edgeExpansionThreshold) {
        row = visibleRows;
    }
    return {row * columns + column, columns};
}

std::optional<std::size_t> ResolveCardSlotIndex(
    double availableWidth,
    double pointerX,
    double pointerY,
    CardContentLayoutSettings settings,
    std::optional<std::size_t> maximumRows) {
    const auto layout = ResolveCardContentLayout(0, availableWidth, settings);
    const auto contentLeft = (availableWidth - layout.contentWidth) / 2.0;
    const auto pitchX = settings.itemWidth + settings.horizontalGap;
    const auto pitchY = settings.itemHeight + settings.verticalGap;
    const auto localX = std::clamp(
        pointerX - contentLeft,
        0.0,
        std::max(0.0, layout.contentWidth - 1.0));
    const auto localY = std::max(
        0.0,
        pointerY - settings.headerHeight - settings.verticalPadding);
    const auto column = std::min(
        layout.columns - 1,
        static_cast<std::size_t>(std::floor(localX / pitchX)));
    auto row = static_cast<std::size_t>(std::floor(localY / pitchY));
    if (maximumRows.has_value()) {
        if (*maximumRows == 0) return std::nullopt;
        row = std::min(row, *maximumRows - 1);
    }
    return row * layout.columns + column;
}

} // namespace desto::presentation
