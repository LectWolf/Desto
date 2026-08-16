#include "PlacementInteraction.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace desto::presentation {
namespace {

template <typename Anchor>
void Consider(
    double candidate,
    double current,
    double guide,
    Anchor candidateAnchor,
    double threshold,
    double& bestDelta,
    std::optional<double>& bestGuide,
    Anchor& bestAnchor) {
    const auto delta = candidate - current;
    if (std::abs(delta) <= threshold && std::abs(delta) < std::abs(bestDelta)) {
        bestDelta = delta;
        bestGuide = guide;
        bestAnchor = candidateAnchor;
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

    const auto horizontalInset = workAreaWidth > settings.visualGap * 2.0
        ? settings.visualGap
        : 0.0;
    const auto verticalInset = workAreaHeight > settings.visualGap * 2.0
        ? settings.visualGap
        : 0.0;
    const auto maximumWidth = workAreaWidth - horizontalInset * 2.0;
    const auto maximumHeight = workAreaHeight - verticalInset * 2.0;
    proposed.width = std::clamp(
        proposed.width, std::min(settings.minimumWidth, maximumWidth), maximumWidth);
    proposed.height = std::clamp(
        proposed.height, std::min(settings.minimumHeight, maximumHeight), maximumHeight);
    proposed.left = std::clamp(
        proposed.left, horizontalInset, workAreaWidth - horizontalInset - proposed.width);
    proposed.top = std::clamp(
        proposed.top, verticalInset, workAreaHeight - verticalInset - proposed.height);
    if (bypassSnapping) {
        return {.rect = proposed};
    }

    auto horizontalDelta = settings.threshold + 1.0;
    auto verticalDelta = settings.threshold + 1.0;
    std::optional<double> verticalGuide;
    std::optional<double> horizontalGuide;
    auto horizontalAnchor = domain::PlacementHorizontalAnchor::Free;
    auto verticalAnchor = domain::PlacementVerticalAnchor::Free;
    Consider(horizontalInset, proposed.left, horizontalInset,
             domain::PlacementHorizontalAnchor::Left,
             settings.threshold, horizontalDelta, verticalGuide, horizontalAnchor);
    Consider(workAreaWidth - horizontalInset, proposed.left + proposed.width,
             workAreaWidth - horizontalInset,
             domain::PlacementHorizontalAnchor::Right,
             settings.threshold, horizontalDelta, verticalGuide, horizontalAnchor);
    Consider(workAreaWidth / 2.0, proposed.left + proposed.width / 2.0,
             workAreaWidth / 2.0, domain::PlacementHorizontalAnchor::Center,
             settings.threshold, horizontalDelta, verticalGuide, horizontalAnchor);
    Consider(verticalInset, proposed.top, verticalInset,
             domain::PlacementVerticalAnchor::Top,
             settings.threshold, verticalDelta, horizontalGuide, verticalAnchor);
    Consider(workAreaHeight - verticalInset, proposed.top + proposed.height,
             workAreaHeight - verticalInset,
             domain::PlacementVerticalAnchor::Bottom,
             settings.threshold, verticalDelta, horizontalGuide, verticalAnchor);
    Consider(workAreaHeight / 2.0, proposed.top + proposed.height / 2.0,
             workAreaHeight / 2.0, domain::PlacementVerticalAnchor::Center,
             settings.threshold, verticalDelta, horizontalGuide, verticalAnchor);

    for (const auto& other : otherCards) {
        Consider(other.left, proposed.left, other.left,
                 domain::PlacementHorizontalAnchor::Free,
                 settings.threshold, horizontalDelta, verticalGuide, horizontalAnchor);
        Consider(other.left + other.width,
                 proposed.left + proposed.width,
                 other.left + other.width,
                 domain::PlacementHorizontalAnchor::Free,
                 settings.threshold,
                 horizontalDelta,
                 verticalGuide,
                 horizontalAnchor);
        Consider(other.left + other.width + settings.visualGap,
                 proposed.left,
                 other.left + other.width + settings.visualGap / 2.0,
                 domain::PlacementHorizontalAnchor::Free,
                 settings.threshold,
                 horizontalDelta,
                 verticalGuide,
                 horizontalAnchor);
        Consider(other.left - settings.visualGap,
                 proposed.left + proposed.width,
                 other.left - settings.visualGap / 2.0,
                 domain::PlacementHorizontalAnchor::Free,
                 settings.threshold,
                 horizontalDelta,
                 verticalGuide,
                 horizontalAnchor);
        Consider(other.top, proposed.top, other.top,
                 domain::PlacementVerticalAnchor::Free,
                 settings.threshold, verticalDelta, horizontalGuide, verticalAnchor);
        Consider(other.top + other.height,
                 proposed.top + proposed.height,
                 other.top + other.height,
                 domain::PlacementVerticalAnchor::Free,
                 settings.threshold,
                 verticalDelta,
                 horizontalGuide,
                 verticalAnchor);
        Consider(other.top + other.height + settings.visualGap,
                 proposed.top,
                 other.top + other.height + settings.visualGap / 2.0,
                 domain::PlacementVerticalAnchor::Free,
                 settings.threshold,
                 verticalDelta,
                 horizontalGuide,
                 verticalAnchor);
        Consider(other.top - settings.visualGap,
                 proposed.top + proposed.height,
                 other.top - settings.visualGap / 2.0,
                 domain::PlacementVerticalAnchor::Free,
                 settings.threshold,
                 verticalDelta,
                 horizontalGuide,
                 verticalAnchor);
    }

    if (std::abs(horizontalDelta) <= settings.threshold) {
        proposed.left += horizontalDelta;
    }
    if (std::abs(verticalDelta) <= settings.threshold) {
        proposed.top += verticalDelta;
    }
    proposed.left = std::clamp(
        proposed.left, horizontalInset, workAreaWidth - horizontalInset - proposed.width);
    proposed.top = std::clamp(
        proposed.top, verticalInset, workAreaHeight - verticalInset - proposed.height);
    return {
        .rect = proposed,
        .verticalGuide = verticalGuide,
        .horizontalGuide = horizontalGuide,
        .horizontalAnchor = horizontalAnchor,
        .verticalAnchor = verticalAnchor,
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
