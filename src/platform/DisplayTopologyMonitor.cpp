#include "DisplayTopologyMonitor.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <tuple>

namespace desto::platform {
namespace {

constexpr double kDisplaySizeEpsilon = 0.01;

void ValidateSnapshots(std::span<const domain::DisplaySnapshot> displays) {
    std::vector<domain::DisplayId> ids;
    std::size_t primaryCount = 0;
    ids.reserve(displays.size());
    for (const auto& display : displays) {
        if (display.id.empty() || !std::isfinite(display.workAreaWidth)
            || !std::isfinite(display.workAreaHeight)
            || display.workAreaWidth <= 0 || display.workAreaHeight <= 0) {
            throw std::invalid_argument("Display snapshot must have a valid work area.");
        }
        ids.push_back(display.id);
        primaryCount += display.primary ? 1 : 0;
    }
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
        throw std::invalid_argument("Display snapshot ids must be unique.");
    }
    if (primaryCount > 1) {
        throw std::invalid_argument("Display snapshot must have at most one primary display.");
    }
}

bool Equivalent(
    const domain::DisplaySnapshot& left,
    const domain::DisplaySnapshot& right) {
    return left.id == right.id
        && left.primary == right.primary
        && std::abs(left.workAreaWidth - right.workAreaWidth) <= kDisplaySizeEpsilon
        && std::abs(left.workAreaHeight - right.workAreaHeight) <= kDisplaySizeEpsilon;
}

} // namespace

DisplayTopologyDiff DiffDisplaySnapshots(
    std::span<const domain::DisplaySnapshot> previous,
    std::span<const domain::DisplaySnapshot> current) {
    ValidateSnapshots(previous);
    ValidateSnapshots(current);

    std::vector<const domain::DisplaySnapshot*> oldDisplays;
    std::vector<const domain::DisplaySnapshot*> newDisplays;
    oldDisplays.reserve(previous.size());
    newDisplays.reserve(current.size());
    for (const auto& display : previous) {
        oldDisplays.push_back(&display);
    }
    for (const auto& display : current) {
        newDisplays.push_back(&display);
    }
    const auto byId = [](const domain::DisplaySnapshot* left,
                         const domain::DisplaySnapshot* right) {
        return left->id < right->id;
    };
    std::sort(oldDisplays.begin(), oldDisplays.end(), byId);
    std::sort(newDisplays.begin(), newDisplays.end(), byId);

    DisplayTopologyDiff result;
    std::size_t oldIndex = 0;
    std::size_t newIndex = 0;
    while (oldIndex < oldDisplays.size() || newIndex < newDisplays.size()) {
        if (oldIndex == oldDisplays.size()) {
            result.changes.push_back({
                .kind = DisplayChangeKind::Added,
                .id = newDisplays[newIndex]->id,
                .previous = std::nullopt,
                .current = *newDisplays[newIndex],
            });
            ++newIndex;
            continue;
        }
        if (newIndex == newDisplays.size()) {
            result.changes.push_back({
                .kind = DisplayChangeKind::Removed,
                .id = oldDisplays[oldIndex]->id,
                .previous = *oldDisplays[oldIndex],
                .current = std::nullopt,
            });
            ++oldIndex;
            continue;
        }

        if (oldDisplays[oldIndex]->id < newDisplays[newIndex]->id) {
            result.changes.push_back({
                .kind = DisplayChangeKind::Removed,
                .id = oldDisplays[oldIndex]->id,
                .previous = *oldDisplays[oldIndex],
                .current = std::nullopt,
            });
            ++oldIndex;
        } else if (newDisplays[newIndex]->id < oldDisplays[oldIndex]->id) {
            result.changes.push_back({
                .kind = DisplayChangeKind::Added,
                .id = newDisplays[newIndex]->id,
                .previous = std::nullopt,
                .current = *newDisplays[newIndex],
            });
            ++newIndex;
        } else {
            if (!Equivalent(*oldDisplays[oldIndex], *newDisplays[newIndex])) {
                result.changes.push_back({
                    .kind = DisplayChangeKind::Modified,
                    .id = oldDisplays[oldIndex]->id,
                    .previous = *oldDisplays[oldIndex],
                    .current = *newDisplays[newIndex],
                });
            }
            ++oldIndex;
            ++newIndex;
        }
    }
    return result;
}

DisplayTopologyMonitor::DisplayTopologyMonitor(
    const DisplayTopologyProvider& provider,
    std::chrono::milliseconds debounce)
    : provider_(provider), debounce_(debounce) {
    if (debounce_ < std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("Display topology debounce must not be negative.");
    }
}

void DisplayTopologyMonitor::initialize() {
    current_ = provider_.snapshot();
    ValidateSnapshots(current_);
    refreshDeadline_.reset();
    initialized_ = true;
}

void DisplayTopologyMonitor::requestRefresh(TimePoint now) {
    if (!initialized_) {
        throw std::logic_error("Display topology monitor must be initialized first.");
    }
    refreshDeadline_ = now + debounce_;
}

std::optional<DisplayTopologyDiff> DisplayTopologyMonitor::poll(TimePoint now) {
    if (!refreshDeadline_.has_value() || now < *refreshDeadline_) {
        return std::nullopt;
    }

    try {
        const auto next = provider_.snapshot();
        ValidateSnapshots(next);
        const auto diff = DiffDisplaySnapshots(current_, next);
        current_ = next;
        refreshDeadline_.reset();
        if (diff.empty()) {
            return std::nullopt;
        }
        return diff;
    } catch (...) {
        refreshDeadline_ = now + debounce_;
        throw;
    }
}

} // namespace desto::platform
