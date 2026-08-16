#pragma once

#include <span>

#include "WorkspaceLayout.h"

namespace desto::presentation {

struct SnapSettings {
    double threshold = 12.0;
    double minimumWidth = 160.0;
    double minimumHeight = 80.0;
};

[[nodiscard]] domain::PlacementRect ResolvePlacementInteraction(
    domain::PlacementRect proposed,
    double workAreaWidth,
    double workAreaHeight,
    std::span<const domain::PlacementRect> otherCards,
    bool bypassSnapping,
    SnapSettings settings = {});

} // namespace desto::presentation
