#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "Card.h"

namespace desto::application {

struct ApplicationItemSortData {
    std::filesystem::path fileName;
    std::wstring displayName;
    std::uintmax_t size = 0;
    std::wstring itemType;
    std::int64_t modifiedTime = 0;
};

struct ApplicationItemProjection {
    std::filesystem::path fileName;
    std::uint32_t column = 0;
    std::uint32_t row = 0;
};

struct ApplicationItemPlacementResult {
    bool fits = true;
    std::vector<domain::ApplicationItemPlacement> placements;
};

[[nodiscard]] ApplicationItemPlacementResult ReconcileApplicationItemPlacements(
    std::span<const domain::ApplicationItemPlacement> preferredPlacements,
    std::span<const std::filesystem::path> actualFileNames,
    std::uint32_t columns,
    std::optional<std::uint32_t> maximumRows = std::nullopt);

[[nodiscard]] ApplicationItemPlacementResult MoveApplicationItemsToSlot(
    std::span<const domain::ApplicationItemPlacement> currentPlacements,
    std::span<const std::filesystem::path> movedFileNames,
    std::uint32_t targetColumn,
    std::uint32_t targetRow,
    std::uint32_t columns,
    std::optional<std::uint32_t> maximumRows = std::nullopt);

[[nodiscard]] std::vector<ApplicationItemProjection> ProjectApplicationItems(
    std::span<const ApplicationItemSortData> items,
    std::span<const domain::ApplicationItemPlacement> customPlacements,
    domain::ApplicationItemSortMode sortMode,
    std::uint32_t columns);

} // namespace desto::application
