#pragma once

#include <chrono>
#include <optional>
#include <vector>

#include "DisplayTopology.h"

namespace desto::platform {

enum class DisplayChangeKind {
    Added,
    Removed,
    Modified,
};

struct DisplayChange {
    DisplayChangeKind kind;
    domain::DisplayId id;
    std::optional<domain::DisplaySnapshot> previous;
    std::optional<domain::DisplaySnapshot> current;
};

struct DisplayTopologyDiff {
    std::vector<DisplayChange> changes;

    [[nodiscard]] bool empty() const noexcept { return changes.empty(); }
};

[[nodiscard]] DisplayTopologyDiff DiffDisplaySnapshots(
    std::span<const domain::DisplaySnapshot> previous,
    std::span<const domain::DisplaySnapshot> current);

class DisplayTopologyMonitor {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    DisplayTopologyMonitor(
        const DisplayTopologyProvider& provider,
        std::chrono::milliseconds debounce);

    void initialize();
    void requestRefresh(TimePoint now);

    [[nodiscard]] std::optional<DisplayTopologyDiff> poll(TimePoint now);
    [[nodiscard]] const std::vector<domain::DisplaySnapshot>& current() const noexcept {
        return current_;
    }

private:
    const DisplayTopologyProvider& provider_;
    std::chrono::milliseconds debounce_;
    std::vector<domain::DisplaySnapshot> current_;
    std::optional<TimePoint> refreshDeadline_;
    bool initialized_ = false;
};

} // namespace desto::platform
