#pragma once

#include <cstddef>
#include <optional>

#include "Card.h"

namespace desto::presentation {

struct CardContentLayoutSettings {
    double headerHeight = 48.0;
    double horizontalPadding = 12.0;
    double verticalPadding = 12.0;
    double itemWidth = 55.0;
    double itemHeight = 55.0;
    double iconSize = 34.0;
    double horizontalGap = 4.0;
    double verticalGap = 4.0;
    std::size_t minimumColumns = 1;
    std::size_t maximumColumns = 8;
    std::size_t preferredColumns = 4;
};

[[nodiscard]] CardContentLayoutSettings ResolveCardContentLayoutSettings(
    const domain::CardContentPreferences& preferences) noexcept;

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

[[nodiscard]] std::optional<std::size_t> ResolveCardSlotIndex(
    double availableWidth,
    double pointerX,
    double pointerY,
    CardContentLayoutSettings settings = {},
    std::optional<std::size_t> maximumRows = std::nullopt);

} // namespace desto::presentation
