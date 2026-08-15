#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "Card.h"

namespace desto::domain {

using DisplayId = std::string;
using PlacementId = std::string;

enum class DisplayTargetKind {
    SpecificDisplay,
    AllDisplays,
};

class DisplayTarget {
public:
    [[nodiscard]] static DisplayTarget specific(DisplayId displayId);
    [[nodiscard]] static DisplayTarget all() noexcept;

    [[nodiscard]] DisplayTargetKind kind() const noexcept { return kind_; }
    [[nodiscard]] const DisplayId& displayId() const noexcept { return displayId_; }
    [[nodiscard]] bool operator==(const DisplayTarget&) const noexcept = default;

private:
    DisplayTarget(DisplayTargetKind kind, DisplayId displayId) noexcept;

    DisplayTargetKind kind_;
    DisplayId displayId_;
};

struct PlacementRect {
    double left = 0;
    double top = 0;
    double width = 320;
    double height = 220;
};

struct CardPlacement {
    PlacementId id;
    CardId cardId;
    DisplayTarget target = DisplayTarget::all();
    PlacementRect rect;
    std::int32_t zIndex = 0;
};

struct DisplaySnapshot {
    DisplayId id;
    double workAreaLeft = 0;
    double workAreaTop = 0;
    double workAreaWidth = 0;
    double workAreaHeight = 0;
    double effectiveDpi = 96;
    bool primary = false;
};

struct PlacementProjection {
    PlacementId placementId;
    CardId cardId;
    std::optional<DisplayId> requestedDisplayId;
    DisplayId displayId;
    PlacementRect rect;
    std::int32_t zIndex = 0;
    bool fallback = false;
};

class WorkspaceLayout {
public:
    [[nodiscard]] const std::vector<CardPlacement>& placements() const noexcept {
        return placements_;
    }

    void setPlacement(CardPlacement placement);
    [[nodiscard]] bool removePlacement(const PlacementId& placementId);
    [[nodiscard]] std::size_t removeCard(const CardId& cardId);
    [[nodiscard]] std::vector<PlacementProjection> project(
        std::span<const DisplaySnapshot> displays) const;

private:
    std::vector<CardPlacement> placements_;
};

} // namespace desto::domain
