#include "PlacementInteraction.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace desto::presentation {
namespace {

void Consider(
    double candidate,
    double current,
    double guide,
    double threshold,
    double& bestDelta,
    std::optional<double>& bestGuide) {
    const auto delta = candidate - current;
    if (std::abs(delta) <= threshold && std::abs(delta) < std::abs(bestDelta)) {
        bestDelta = delta;
        bestGuide = guide;
    }
}

} // namespace

PlacementInteractionResult ResolvePlacementInteractionDetailed(
    domain::PlacementRect proposed,
    double workAreaWidth,
    double workAreaHeight,
    std::span<const domain::PlacementRect> otherCards,
    bool bypassSnapping,
    SnapSettings settings) {
    if (!std::isfinite(workAreaWidth) || !std::isfinite(workAreaHeight)
        || !std::isfinite(settings.threshold) || !std::isfinite(settings.visualGap)
        || !std::isfinite(settings.minimumWidth)
        || !std::isfinite(settings.minimumHeight) || workAreaWidth <= 0 || workAreaHeight <= 0
        || settings.threshold < 0 || settings.visualGap < 0
        || settings.minimumWidth <= 0 || settings.minimumHeight <= 0) {
        throw std::invalid_argument("Placement interaction settings must be positive and finite.");
    }

    proposed.width = std::clamp(proposed.width, settings.minimumWidth, workAreaWidth);
    proposed.height = std::clamp(proposed.height, settings.minimumHeight, workAreaHeight);
    proposed.left = std::clamp(proposed.left, 0.0, workAreaWidth - proposed.width);
    proposed.top = std::clamp(proposed.top, 0.0, workAreaHeight - proposed.height);
    if (bypassSnapping) {
        return {.rect = proposed};
    }

    auto horizontalDelta = settings.threshold + 1.0;
    auto verticalDelta = settings.threshold + 1.0;
    std::optional<double> verticalGuide;
    std::optional<double> horizontalGuide;
    Consider(0.0, proposed.left, 0.0, settings.threshold, horizontalDelta, verticalGuide);
    Consider(workAreaWidth, proposed.left + proposed.width, workAreaWidth,
             settings.threshold, horizontalDelta, verticalGuide);
    Consider(workAreaWidth / 2.0, proposed.left + proposed.width / 2.0,
             workAreaWidth / 2.0, settings.threshold, horizontalDelta, verticalGuide);
    Consider(0.0, proposed.top, 0.0, settings.threshold, verticalDelta, horizontalGuide);
    Consider(workAreaHeight, proposed.top + proposed.height, workAreaHeight,
             settings.threshold, verticalDelta, horizontalGuide);
    Consider(workAreaHeight / 2.0, proposed.top + proposed.height / 2.0,
             workAreaHeight / 2.0, settings.threshold, verticalDelta, horizontalGuide);

    for (const auto& other : otherCards) {
        Consider(other.left, proposed.left, other.left, settings.threshold,
                 horizontalDelta, verticalGuide);
        Consider(other.left + other.width,
                 proposed.left + proposed.width,
                 other.left + other.width,
                 settings.threshold,
                 horizontalDelta,
                 verticalGuide);
        Consider(other.left, proposed.left + proposed.width, other.left,
                 settings.threshold, horizontalDelta, verticalGuide);
        Consider(other.left + other.width, proposed.left, other.left + other.width,
                 settings.threshold, horizontalDelta, verticalGuide);
        Consider(other.left + other.width + settings.visualGap,
                 proposed.left,
                 other.left + other.width + settings.visualGap / 2.0,
                 settings.threshold,
                 horizontalDelta,
                 verticalGuide);
        Consider(other.left - settings.visualGap,
                 proposed.left + proposed.width,
                 other.left - settings.visualGap / 2.0,
                 settings.threshold,
                 horizontalDelta,
                 verticalGuide);
        Consider(other.top, proposed.top, other.top, settings.threshold,
                 verticalDelta, horizontalGuide);
        Consider(other.top + other.height,
                 proposed.top + proposed.height,
                 other.top + other.height,
                 settings.threshold,
                 verticalDelta,
                 horizontalGuide);
        Consider(other.top, proposed.top + proposed.height, other.top,
                 settings.threshold, verticalDelta, horizontalGuide);
        Consider(other.top + other.height, proposed.top, other.top + other.height,
                 settings.threshold, verticalDelta, horizontalGuide);
        Consider(other.top + other.height + settings.visualGap,
                 proposed.top,
                 other.top + other.height + settings.visualGap / 2.0,
                 settings.threshold,
                 verticalDelta,
                 horizontalGuide);
        Consider(other.top - settings.visualGap,
                 proposed.top + proposed.height,
                 other.top - settings.visualGap / 2.0,
                 settings.threshold,
                 verticalDelta,
                 horizontalGuide);
    }

    if (std::abs(horizontalDelta) <= settings.threshold) {
        proposed.left += horizontalDelta;
    }
    if (std::abs(verticalDelta) <= settings.threshold) {
        proposed.top += verticalDelta;
    }
    proposed.left = std::clamp(proposed.left, 0.0, workAreaWidth - proposed.width);
    proposed.top = std::clamp(proposed.top, 0.0, workAreaHeight - proposed.height);
    return {
        .rect = proposed,
        .verticalGuide = verticalGuide,
        .horizontalGuide = horizontalGuide,
    };
}

domain::PlacementRect ResolvePlacementInteraction(
    domain::PlacementRect proposed,
    double workAreaWidth,
    double workAreaHeight,
    std::span<const domain::PlacementRect> otherCards,
    bool bypassSnapping,
    SnapSettings settings) {
    return ResolvePlacementInteractionDetailed(
        proposed,
        workAreaWidth,
        workAreaHeight,
        otherCards,
        bypassSnapping,
        settings).rect;
}

} // namespace desto::presentation
