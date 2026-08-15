#pragma once

#include "../DisplayTopology.h"

namespace desto::platform::windows {

class WindowsDisplayTopology final : public DisplayTopologyProvider {
public:
    [[nodiscard]] std::vector<domain::DisplaySnapshot> snapshot() const override;
};

} // namespace desto::platform::windows
