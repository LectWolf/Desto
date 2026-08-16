#pragma once

#include <span>
#include <optional>

#include "WorkspaceLayout.h"

namespace desto::presentation {

struct SnapSettings {
    double threshold = 12.0;
    double visualGap = 8.0;
    double minimumWidth = 160.0;
    double minimumHeight = 80.0;
};

struct PlacementInteractionResult {
    domain::PlacementRect rect;
    std::optional<double> verticalGuide;
    std::optional<double> horizontalGuide;
};

[[nodiscard]] PlacementInteractionResult ResolvePlacementInteractionDetailed(
    domain::PlacementRect proposed,
    double workAreaWidth,
    double workAreaHeight,
    std::span<const domain::PlacementRect> otherCards,
    bool bypassSnapping,
    SnapSettings settings = {});

[[nodiscard]] domain::PlacementRect ResolvePlacementInteraction(
    domain::PlacementRect proposed,
    double workAreaWidth,
    double workAreaHeight,
    std::span<const domain::PlacementRect> otherCards,
    bool bypassSnapping,
    SnapSettings settings = {});

} // namespace desto::presentation
