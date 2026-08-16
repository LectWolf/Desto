#include "WorkspaceLayout.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <tuple>

namespace desto::domain {
namespace {

void ValidateRect(const PlacementRect& rect) {
    if (!std::isfinite(rect.left) || !std::isfinite(rect.top)
        || !std::isfinite(rect.width) || !std::isfinite(rect.height)
        || rect.width <= 0 || rect.height <= 0) {
        throw std::invalid_argument("Placement rectangle must be finite and have positive dimensions.");
    }
}

void ValidatePlacement(const CardPlacement& placement) {
    if (placement.id.empty() || placement.cardId.empty()) {
        throw std::invalid_argument("Placement and card ids must not be empty.");
    }
    ValidateRect(placement.rect);
    const auto hasReferenceWidth = placement.referenceWorkAreaWidth > 0;
    const auto hasReferenceHeight = placement.referenceWorkAreaHeight > 0;
    if (!std::isfinite(placement.referenceWorkAreaWidth)
        || !std::isfinite(placement.referenceWorkAreaHeight)
        || hasReferenceWidth != hasReferenceHeight
        || placement.referenceWorkAreaWidth < 0
        || placement.referenceWorkAreaHeight < 0) {
        throw std::invalid_argument(
            "Placement reference work area must be absent or have positive finite dimensions.");
    }
}

void ValidateDisplays(std::span<const DisplaySnapshot> displays) {
    std::vector<DisplayId> ids;
    std::size_t primaryCount = 0;
    ids.reserve(displays.size());
    for (const auto& display : displays) {
        if (display.id.empty() || !std::isfinite(display.workAreaLeft)
            || !std::isfinite(display.workAreaTop) || !std::isfinite(display.workAreaWidth)
            || !std::isfinite(display.workAreaHeight) || !std::isfinite(display.effectiveDpi)
            || display.workAreaWidth <= 0 || display.workAreaHeight <= 0
            || display.effectiveDpi <= 0) {
            throw std::invalid_argument("Display snapshot must have an id and positive finite work area.");
        }
        ids.push_back(display.id);
        primaryCount += display.primary ? 1 : 0;
    }
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
        throw std::invalid_argument("Display snapshot ids must be unique.");
    }
    if (primaryCount > 1) {
        throw std::invalid_argument("Display snapshot must not contain multiple primary displays.");
    }
}

double ReflowAxis(
    double position,
    double size,
    double referenceExtent,
    double currentExtent,
    bool startAnchored,
    bool centerAnchored,
    bool endAnchored) {
    if (referenceExtent <= 0 || referenceExtent == currentExtent) return position;
    if (startAnchored) return position;
    if (endAnchored) return currentExtent - size - (referenceExtent - position - size);
    if (centerAnchored) {
        return currentExtent / 2.0 - size / 2.0
            + (position + size / 2.0 - referenceExtent / 2.0);
    }
    const auto oldTravel = std::max(0.0, referenceExtent - size);
    const auto newTravel = std::max(0.0, currentExtent - size);
    return oldTravel <= 0 ? 0.0 : std::clamp(position / oldTravel, 0.0, 1.0) * newTravel;
}

PlacementRect FitToDisplay(const CardPlacement& placement, const DisplaySnapshot& display) {
    PlacementRect result = placement.rect;
    result.width = std::min(result.width, display.workAreaWidth);
    result.height = std::min(result.height, display.workAreaHeight);
    result.left = ReflowAxis(
        result.left,
        result.width,
        placement.referenceWorkAreaWidth,
        display.workAreaWidth,
        placement.horizontalAnchor == PlacementHorizontalAnchor::Left,
        placement.horizontalAnchor == PlacementHorizontalAnchor::Center,
        placement.horizontalAnchor == PlacementHorizontalAnchor::Right);
    result.top = ReflowAxis(
        result.top,
        result.height,
        placement.referenceWorkAreaHeight,
        display.workAreaHeight,
        placement.verticalAnchor == PlacementVerticalAnchor::Top,
        placement.verticalAnchor == PlacementVerticalAnchor::Center,
        placement.verticalAnchor == PlacementVerticalAnchor::Bottom);
    result.left = std::clamp(result.left, 0.0, display.workAreaWidth - result.width);
    result.top = std::clamp(result.top, 0.0, display.workAreaHeight - result.height);
    return result;
}

} // namespace

DisplayTarget::DisplayTarget(DisplayTargetKind kind, DisplayId displayId) noexcept
    : kind_(kind), displayId_(std::move(displayId)) {
}

DisplayTarget DisplayTarget::specific(DisplayId displayId) {
    if (displayId.empty()) {
        throw std::invalid_argument("Specific display target id must not be empty.");
    }
    return DisplayTarget(DisplayTargetKind::SpecificDisplay, std::move(displayId));
}

DisplayTarget DisplayTarget::all() noexcept {
    return DisplayTarget(DisplayTargetKind::AllDisplays, {});
}

std::string_view ToString(PlacementHorizontalAnchor anchor) noexcept {
    switch (anchor) {
    case PlacementHorizontalAnchor::Free: return "free";
    case PlacementHorizontalAnchor::Left: return "left";
    case PlacementHorizontalAnchor::Center: return "center";
    case PlacementHorizontalAnchor::Right: return "right";
    }
    return "free";
}

std::string_view ToString(PlacementVerticalAnchor anchor) noexcept {
    switch (anchor) {
    case PlacementVerticalAnchor::Free: return "free";
    case PlacementVerticalAnchor::Top: return "top";
    case PlacementVerticalAnchor::Center: return "center";
    case PlacementVerticalAnchor::Bottom: return "bottom";
    }
    return "free";
}

void WorkspaceLayout::setPlacement(CardPlacement placement) {
    ValidatePlacement(placement);

    const auto sameId = std::find_if(
        placements_.begin(),
        placements_.end(),
        [&](const CardPlacement& existing) { return existing.id == placement.id; });
    if (sameId != placements_.end() && sameId->cardId != placement.cardId) {
        throw std::invalid_argument("A placement id cannot be rebound to another card.");
    }

    for (const auto& existing : placements_) {
        if (existing.id == placement.id || existing.cardId != placement.cardId) {
            continue;
        }
        if (existing.target.kind() == DisplayTargetKind::AllDisplays
            || placement.target.kind() == DisplayTargetKind::AllDisplays
            || existing.target.displayId() == placement.target.displayId()) {
            throw std::invalid_argument(
                "A card may target each display once or use a single all-displays placement.");
        }
    }

    if (sameId == placements_.end()) {
        placements_.push_back(std::move(placement));
    } else {
        *sameId = std::move(placement);
    }
}

bool WorkspaceLayout::removePlacement(const PlacementId& placementId) {
    const auto oldSize = placements_.size();
    std::erase_if(
        placements_,
        [&](const CardPlacement& placement) { return placement.id == placementId; });
    return placements_.size() != oldSize;
}

std::size_t WorkspaceLayout::removeCard(const CardId& cardId) {
    const auto oldSize = placements_.size();
    std::erase_if(
        placements_,
        [&](const CardPlacement& placement) { return placement.cardId == cardId; });
    return oldSize - placements_.size();
}

std::vector<PlacementProjection> WorkspaceLayout::project(
    std::span<const DisplaySnapshot> displays) const {
    ValidateDisplays(displays);
    if (displays.empty()) {
        return {};
    }

    std::vector<const DisplaySnapshot*> orderedDisplays;
    orderedDisplays.reserve(displays.size());
    for (const auto& display : displays) {
        orderedDisplays.push_back(&display);
    }
    std::sort(
        orderedDisplays.begin(),
        orderedDisplays.end(),
        [](const DisplaySnapshot* left, const DisplaySnapshot* right) {
            return left->id < right->id;
        });

    std::map<std::pair<CardId, DisplayId>, PlacementProjection> projections;
    const auto addProjection = [&](const CardPlacement& placement,
                                   const DisplaySnapshot& display) {
        PlacementProjection candidate{
            .placementId = placement.id,
            .cardId = placement.cardId,
            .requestedDisplayId = placement.target.kind() == DisplayTargetKind::SpecificDisplay
                ? std::optional<DisplayId>(placement.target.displayId())
                : std::nullopt,
            .displayId = display.id,
            .rect = FitToDisplay(placement, display),
            .zIndex = placement.zIndex,
            .horizontalAnchor = placement.horizontalAnchor,
            .verticalAnchor = placement.verticalAnchor,
            .fallback = false,
        };
        const auto key = std::pair(candidate.cardId, candidate.displayId);
        const auto existing = projections.find(key);
        if (existing == projections.end()
            || candidate.placementId < existing->second.placementId) {
            projections.insert_or_assign(std::move(key), std::move(candidate));
        }
    };

    for (const auto& placement : placements_) {
        if (placement.target.kind() == DisplayTargetKind::AllDisplays) {
            for (const auto* display : orderedDisplays) {
                addProjection(placement, *display);
            }
            continue;
        }

        const auto requested = std::find_if(
            orderedDisplays.begin(),
            orderedDisplays.end(),
            [&](const DisplaySnapshot* display) {
                return display->id == placement.target.displayId();
            });
        if (requested != orderedDisplays.end()) {
            addProjection(placement, **requested);
        }
    }

    std::vector<PlacementProjection> result;
    result.reserve(projections.size());
    for (auto& [key, projection] : projections) {
        result.push_back(std::move(projection));
    }
    std::sort(
        result.begin(),
        result.end(),
        [](const PlacementProjection& left, const PlacementProjection& right) {
            return std::tie(left.displayId, left.zIndex, left.placementId)
                < std::tie(right.displayId, right.zIndex, right.placementId);
        });
    return result;
}

std::vector<CardPlacement> WorkspaceLayout::unavailablePlacements(
    std::span<const DisplaySnapshot> displays) const {
    ValidateDisplays(displays);
    std::vector<CardPlacement> result;
    for (const auto& placement : placements_) {
        if (placement.target.kind() != DisplayTargetKind::SpecificDisplay) continue;
        const auto available = std::ranges::any_of(displays, [&](const auto& display) {
            return display.id == placement.target.displayId();
        });
        if (!available) result.push_back(placement);
    }
    std::ranges::sort(result, {}, &CardPlacement::id);
    return result;
}

} // namespace desto::domain
