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
}

void ValidateDisplays(std::span<const DisplaySnapshot> displays) {
    std::vector<DisplayId> ids;
    std::size_t primaryCount = 0;
    ids.reserve(displays.size());
    for (const auto& display : displays) {
        if (display.id.empty() || !std::isfinite(display.workAreaWidth)
            || !std::isfinite(display.workAreaHeight)
            || display.workAreaWidth <= 0 || display.workAreaHeight <= 0) {
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

PlacementRect FitToDisplay(const PlacementRect& source, const DisplaySnapshot& display) {
    PlacementRect result = source;
    result.width = std::min(result.width, display.workAreaWidth);
    result.height = std::min(result.height, display.workAreaHeight);
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

    const auto primary = std::find_if(
        orderedDisplays.begin(),
        orderedDisplays.end(),
        [](const DisplaySnapshot* display) { return display->primary; });
    const auto* fallbackDisplay = primary == orderedDisplays.end()
        ? orderedDisplays.front()
        : *primary;

    std::map<std::pair<CardId, DisplayId>, PlacementProjection> projections;
    const auto addProjection = [&](const CardPlacement& placement,
                                   const DisplaySnapshot& display,
                                   bool fallback) {
        PlacementProjection candidate{
            .placementId = placement.id,
            .cardId = placement.cardId,
            .requestedDisplayId = placement.target.kind() == DisplayTargetKind::SpecificDisplay
                ? std::optional<DisplayId>(placement.target.displayId())
                : std::nullopt,
            .displayId = display.id,
            .rect = FitToDisplay(placement.rect, display),
            .zIndex = placement.zIndex,
            .fallback = fallback,
        };
        const auto key = std::pair(candidate.cardId, candidate.displayId);
        const auto existing = projections.find(key);
        if (existing == projections.end()
            || (existing->second.fallback && !candidate.fallback)
            || (existing->second.fallback == candidate.fallback
                && candidate.placementId < existing->second.placementId)) {
            projections.insert_or_assign(std::move(key), std::move(candidate));
        }
    };

    for (const auto& placement : placements_) {
        if (placement.target.kind() == DisplayTargetKind::AllDisplays) {
            for (const auto* display : orderedDisplays) {
                addProjection(placement, *display, false);
            }
            continue;
        }

        const auto requested = std::find_if(
            orderedDisplays.begin(),
            orderedDisplays.end(),
            [&](const DisplaySnapshot* display) {
                return display->id == placement.target.displayId();
            });
        addProjection(
            placement,
            requested == orderedDisplays.end() ? *fallbackDisplay : **requested,
            requested == orderedDisplays.end());
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

} // namespace desto::domain
