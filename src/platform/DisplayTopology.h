#pragma once

#include <vector>

#include "WorkspaceLayout.h"

namespace desto::platform {

class DisplayTopologyProvider {
public:
    virtual ~DisplayTopologyProvider() = default;

    [[nodiscard]] virtual std::vector<domain::DisplaySnapshot> snapshot() const = 0;
};

class MemoryDisplayTopologyProvider final : public DisplayTopologyProvider {
public:
    explicit MemoryDisplayTopologyProvider(
        std::vector<domain::DisplaySnapshot> displays = {});

    [[nodiscard]] std::vector<domain::DisplaySnapshot> snapshot() const override;
    void setSnapshot(std::vector<domain::DisplaySnapshot> displays);

private:
    std::vector<domain::DisplaySnapshot> displays_;
};

} // namespace desto::platform
