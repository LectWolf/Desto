#pragma once

#include <cstddef>
#include <optional>

#include "Card.h"

namespace desto::presentation {

inline constexpr double CardWidthTrackDip = 55.0;
inline constexpr double CardWidthGapDip = 8.0;
inline constexpr std::uint32_t DefaultCardWidthSpan = 4;

struct CardContentLayoutSettings;

[[nodiscard]] double ResolveCardOuterWidth(
    std::uint32_t widthSpan) noexcept;

[[nodiscard]] double ResolveFileCardOuterWidth(
    std::size_t columns,
    const CardContentLayoutSettings& settings) noexcept;

[[nodiscard]] double ResolveTodoCardOuterWidth(
    std::uint32_t widthSpan) noexcept;

[[nodiscard]] std::size_t ResolveCardColumnsForWidthSpan(
    std::uint32_t widthSpan,
    domain::CardItemSize itemSize) noexcept;

[[nodiscard]] std::uint32_t ResolveLegacyCardWidthSpan(
    std::size_t columns,
    domain::CardItemSize itemSize) noexcept;

[[nodiscard]] std::uint32_t ResolveCardWidthSpanForColumns(
    std::size_t minimumColumns,
    domain::CardItemSize itemSize) noexcept;

struct CardContentLayoutSettings {
    std::uint32_t widthSpan = DefaultCardWidthSpan;
    double headerHeight = 48.0;
    double horizontalPadding = 12.0;
    double verticalPadding = 12.0;
    double itemWidth = 55.0;
    double itemHeight = 55.0;
    double iconSize = 34.0;
    double itemFontSize = 10.0;
    double horizontalGap = 0.0;
    double verticalGap = 0.0;
    std::size_t minimumColumns = 1;
    std::size_t maximumColumns = 8;
    std::size_t preferredColumns = 4;
};

[[nodiscard]] CardContentLayoutSettings ResolveCardContentLayoutSettings(
    const domain::CardContentPreferences& preferences) noexcept;

[[nodiscard]] std::size_t ResolveAdaptiveCardColumns(
    std::size_t itemCount,
    std::size_t requiredColumns,
    CardContentLayoutSettings settings = {}) noexcept;

[[nodiscard]] std::size_t ResolveSortedAdaptiveCardColumns(
    std::size_t preservedColumns,
    CardContentLayoutSettings settings = {}) noexcept;

[[nodiscard]] std::size_t ResolveCustomAdaptiveCardColumns(
    std::size_t preservedColumns,
    domain::CardItemSize itemSize,
    CardContentLayoutSettings settings = {}) noexcept;

struct CardContentLayout {
    std::size_t columns = 1;
    std::size_t rows = 0;
    double contentWidth = 0.0;
    double contentHeight = 0.0;
    double idealHeight = 0.0;
};

[[nodiscard]] CardContentLayout ResolveCardContentLayout(
    std::size_t itemCount,
    double availableWidth,
    CardContentLayoutSettings settings = {});

[[nodiscard]] std::size_t ResolveCardInsertionIndex(
    std::size_t itemCount,
    double availableWidth,
    double pointerX,
    double pointerY,
    CardContentLayoutSettings settings = {});

struct CardDropPreview {
    std::size_t insertionIndex = 0;
    std::size_t columns = 1;
};

[[nodiscard]] bool IsAdaptiveDropExpansionReady(
    std::uint64_t edgeHoverMilliseconds,
    std::size_t expansionStep = 0,
    bool damped = false) noexcept;

[[nodiscard]] std::uint64_t ResolveAdaptiveDropExpansionDelay(
    std::size_t expansionStep,
    bool damped = false) noexcept;

[[nodiscard]] CardDropPreview ResolveAdaptiveCardDropPreview(
    std::size_t occupiedSlotCount,
    double availableWidth,
    double pointerX,
    double pointerY,
    CardContentLayoutSettings settings = {},
    std::optional<CardDropPreview> previousPreview = std::nullopt,
    bool allowEdgeExpansion = true);

[[nodiscard]] std::optional<std::size_t> ResolveCardSlotIndex(
    double availableWidth,
    double pointerX,
    double pointerY,
    CardContentLayoutSettings settings = {},
    std::optional<std::size_t> maximumRows = std::nullopt);

[[nodiscard]] double ResolveScrolledCardPointerY(
    double pointerY,
    std::size_t scrollRowOffset,
    CardContentLayoutSettings settings = {}) noexcept;

} // namespace desto::presentation
