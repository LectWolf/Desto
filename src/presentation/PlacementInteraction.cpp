#include "PlacementInteraction.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace desto::presentation {
namespace {

void Consider(double candidate, double current, double threshold, double& bestDelta) {
    const auto delta = candidate - current;
    if (std::abs(delta) <= threshold && std::abs(delta) < std::abs(bestDelta)) {
        bestDelta = delta;
    }
}

} // namespace

domain::PlacementRect ResolvePlacementInteraction(
    domain::PlacementRect proposed,
    double workAreaWidth,
    double workAreaHeight,
    std::span<const domain::PlacementRect> otherCards,
    bool bypassSnapping,
    SnapSettings settings) {
    if (!std::isfinite(workAreaWidth) || !std::isfinite(workAreaHeight)
        || !std::isfinite(settings.threshold) || !std::isfinite(settings.minimumWidth)
        || !std::isfinite(settings.minimumHeight) || workAreaWidth <= 0 || workAreaHeight <= 0
        || settings.threshold < 0 || settings.minimumWidth <= 0 || settings.minimumHeight <= 0) {
        throw std::invalid_argument("Placement interaction settings must be positive and finite.");
    }

    proposed.width = std::clamp(proposed.width, settings.minimumWidth, workAreaWidth);
    proposed.height = std::clamp(proposed.height, settings.minimumHeight, workAreaHeight);
    proposed.left = std::clamp(proposed.left, 0.0, workAreaWidth - proposed.width);
    proposed.top = std::clamp(proposed.top, 0.0, workAreaHeight - proposed.height);
    if (bypassSnapping) {
        return proposed;
    }

    auto horizontalDelta = settings.threshold + 1.0;
    auto verticalDelta = settings.threshold + 1.0;
    Consider(0.0, proposed.left, settings.threshold, horizontalDelta);
    Consider(workAreaWidth, proposed.left + proposed.width, settings.threshold, horizontalDelta);
    Consider(workAreaWidth / 2.0,
             proposed.left + proposed.width / 2.0,
             settings.threshold,
             horizontalDelta);
    Consider(0.0, proposed.top, settings.threshold, verticalDelta);
    Consider(workAreaHeight, proposed.top + proposed.height, settings.threshold, verticalDelta);
    Consider(workAreaHeight / 2.0,
             proposed.top + proposed.height / 2.0,
             settings.threshold,
             verticalDelta);

    for (const auto& other : otherCards) {
        Consider(other.left, proposed.left, settings.threshold, horizontalDelta);
        Consider(other.left + other.width,
                 proposed.left + proposed.width,
                 settings.threshold,
                 horizontalDelta);
        Consider(other.left, proposed.left + proposed.width, settings.threshold, horizontalDelta);
        Consider(other.left + other.width, proposed.left, settings.threshold, horizontalDelta);
        Consider(other.top, proposed.top, settings.threshold, verticalDelta);
        Consider(other.top + other.height,
                 proposed.top + proposed.height,
                 settings.threshold,
                 verticalDelta);
        Consider(other.top, proposed.top + proposed.height, settings.threshold, verticalDelta);
        Consider(other.top + other.height, proposed.top, settings.threshold, verticalDelta);
    }

    if (std::abs(horizontalDelta) <= settings.threshold) {
        proposed.left += horizontalDelta;
    }
    if (std::abs(verticalDelta) <= settings.threshold) {
        proposed.top += verticalDelta;
    }
    proposed.left = std::clamp(proposed.left, 0.0, workAreaWidth - proposed.width);
    proposed.top = std::clamp(proposed.top, 0.0, workAreaHeight - proposed.height);
    return proposed;
}

} // namespace desto::presentation
