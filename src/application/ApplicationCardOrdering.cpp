#include "ApplicationCardOrdering.h"

#include <algorithm>
#include <cwctype>
#include <unordered_map>
#include <unordered_set>

namespace desto::application {
namespace {

std::wstring FoldCase(std::wstring value) {
    std::ranges::transform(value, value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

std::wstring PathKey(const std::filesystem::path& path) {
    return FoldCase(path.filename().wstring());
}

std::uint64_t Slot(std::uint32_t column, std::uint32_t row) {
    return (static_cast<std::uint64_t>(row) << 32) | column;
}

bool WithinCapacity(
    std::uint32_t column,
    std::uint32_t row,
    std::uint32_t columns,
    std::optional<std::uint32_t> maximumRows) {
    return columns > 0
        && (!maximumRows.has_value()
            || (column < columns && row < *maximumRows));
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> FirstFreeSlot(
    std::unordered_set<std::uint64_t>& occupied,
    std::uint64_t start,
    std::uint32_t columns,
    std::optional<std::uint32_t> maximumRows) {
    if (columns == 0) {
        return std::nullopt;
    }
    const auto limit = maximumRows.has_value()
        ? static_cast<std::uint64_t>(columns) * *maximumRows
        : std::numeric_limits<std::uint64_t>::max();
    for (auto index = start; index < limit; ++index) {
        const auto column = static_cast<std::uint32_t>(index % columns);
        const auto row = static_cast<std::uint32_t>(index / columns);
        if (!occupied.contains(Slot(column, row))) {
            occupied.insert(Slot(column, row));
            return std::pair{column, row};
        }
    }
    return std::nullopt;
}

} // namespace

ApplicationItemPlacementResult ReconcileApplicationItemPlacements(
    std::span<const domain::ApplicationItemPlacement> preferredPlacements,
    std::span<const std::filesystem::path> actualFileNames,
    std::uint32_t columns,
    std::optional<std::uint32_t> maximumRows) {
    ApplicationItemPlacementResult result;
    if (columns == 0) {
        result.fits = actualFileNames.empty();
        return result;
    }
    std::unordered_set<std::wstring> actual;
    for (const auto& fileName : actualFileNames) actual.insert(PathKey(fileName));
    std::unordered_set<std::wstring> placed;
    std::unordered_set<std::uint64_t> occupied;
    for (const auto& placement : preferredPlacements) {
        const auto key = PathKey(placement.fileName);
        if (!actual.contains(key) || placed.contains(key)
            || !WithinCapacity(placement.column, placement.row, columns, maximumRows)
            || occupied.contains(Slot(placement.column, placement.row))) {
            continue;
        }
        result.placements.push_back(placement);
        placed.insert(key);
        occupied.insert(Slot(placement.column, placement.row));
    }
    for (const auto& fileName : actualFileNames) {
        const auto key = PathKey(fileName);
        if (placed.contains(key)) continue;
        const auto slot = FirstFreeSlot(occupied, 0, columns, maximumRows);
        if (!slot.has_value()) {
            result.fits = false;
            continue;
        }
        result.placements.push_back({fileName.filename(), slot->first, slot->second});
        placed.insert(key);
    }
    return result;
}

ApplicationItemPlacementResult MoveApplicationItemsToSlot(
    std::span<const domain::ApplicationItemPlacement> currentPlacements,
    std::span<const std::filesystem::path> movedFileNames,
    std::uint32_t targetColumn,
    std::uint32_t targetRow,
    std::uint32_t columns,
    std::optional<std::uint32_t> maximumRows) {
    ApplicationItemPlacementResult result;
    if (!WithinCapacity(targetColumn, targetRow, columns, maximumRows)) {
        result.fits = false;
        result.placements.assign(currentPlacements.begin(), currentPlacements.end());
        return result;
    }
    std::unordered_set<std::wstring> moved;
    for (const auto& name : movedFileNames) moved.insert(PathKey(name));
    for (const auto& placement : currentPlacements) {
        if (!moved.contains(PathKey(placement.fileName))) {
            result.placements.push_back(placement);
        }
    }
    auto start = static_cast<std::uint64_t>(targetRow) * columns + targetColumn;
    for (const auto& name : movedFileNames) {
        const auto limit = maximumRows.has_value()
            ? static_cast<std::uint64_t>(columns) * *maximumRows
            : std::numeric_limits<std::uint64_t>::max();
        auto freeIndex = start;
        const auto atIndex = [&](std::uint64_t index) {
            return std::find_if(result.placements.begin(), result.placements.end(),
                [&](const domain::ApplicationItemPlacement& placement) {
                    return static_cast<std::uint64_t>(placement.row) * columns
                            + placement.column == index;
                });
        };
        while (freeIndex < limit && atIndex(freeIndex) != result.placements.end()) {
            ++freeIndex;
        }
        if (freeIndex >= limit) {
            result.fits = false;
            result.placements.assign(currentPlacements.begin(), currentPlacements.end());
            return result;
        }
        for (auto index = freeIndex; index > start; --index) {
            auto occupant = atIndex(index - 1);
            occupant->column = static_cast<std::uint32_t>(index % columns);
            occupant->row = static_cast<std::uint32_t>(index / columns);
        }
        result.placements.push_back({
            name.filename(),
            static_cast<std::uint32_t>(start % columns),
            static_cast<std::uint32_t>(start / columns),
        });
        ++start;
    }
    return result;
}

std::vector<ApplicationItemProjection> ProjectApplicationItems(
    std::span<const ApplicationItemSortData> items,
    std::span<const domain::ApplicationItemPlacement> customPlacements,
    domain::ApplicationItemSortMode sortMode,
    std::uint32_t columns) {
    if (columns == 0) return {};
    std::vector<const ApplicationItemSortData*> ordered;
    ordered.reserve(items.size());
    for (const auto& item : items) ordered.push_back(&item);
    if (sortMode == domain::ApplicationItemSortMode::Custom) {
        std::vector<std::filesystem::path> actualNames;
        actualNames.reserve(items.size());
        for (const auto& item : items) actualNames.push_back(item.fileName);
        const auto reconciled = ReconcileApplicationItemPlacements(
            customPlacements, actualNames, columns);
        std::unordered_map<std::wstring, const ApplicationItemSortData*> byName;
        for (const auto& item : items) byName.emplace(PathKey(item.fileName), &item);
        std::vector<ApplicationItemProjection> result;
        for (const auto& placement : reconciled.placements) {
            const auto found = byName.find(PathKey(placement.fileName));
            if (found != byName.end()) {
                result.push_back({found->second->fileName, placement.column, placement.row});
            }
        }
        return result;
    }
    const auto byName = [](const auto* left, const auto* right) {
        return FoldCase(left->displayName) < FoldCase(right->displayName);
    };
    std::stable_sort(ordered.begin(), ordered.end(), [&](const auto* left, const auto* right) {
        switch (sortMode) {
        case domain::ApplicationItemSortMode::Name:
            return byName(left, right);
        case domain::ApplicationItemSortMode::Size:
            return left->size != right->size ? left->size < right->size : byName(left, right);
        case domain::ApplicationItemSortMode::ItemType:
            return FoldCase(left->itemType) != FoldCase(right->itemType)
                ? FoldCase(left->itemType) < FoldCase(right->itemType) : byName(left, right);
        case domain::ApplicationItemSortMode::ModifiedDate:
            return left->modifiedTime != right->modifiedTime
                ? left->modifiedTime > right->modifiedTime : byName(left, right);
        case domain::ApplicationItemSortMode::Custom:
            return false;
        }
        return false;
    });
    std::vector<ApplicationItemProjection> result;
    result.reserve(ordered.size());
    for (std::size_t index = 0; index < ordered.size(); ++index) {
        result.push_back({
            ordered[index]->fileName,
            static_cast<std::uint32_t>(index % columns),
            static_cast<std::uint32_t>(index / columns),
        });
    }
    return result;
}

} // namespace desto::application
