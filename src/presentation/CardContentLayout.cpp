#include "CardContentLayout.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace desto::presentation {

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

} // namespace desto::presentation
