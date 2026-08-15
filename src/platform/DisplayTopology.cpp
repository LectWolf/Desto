#include "DisplayTopology.h"

namespace desto::platform {

MemoryDisplayTopologyProvider::MemoryDisplayTopologyProvider(
    std::vector<domain::DisplaySnapshot> displays)
    : displays_(std::move(displays)) {
}

std::vector<domain::DisplaySnapshot> MemoryDisplayTopologyProvider::snapshot() const {
    return displays_;
}

void MemoryDisplayTopologyProvider::setSnapshot(
    std::vector<domain::DisplaySnapshot> displays) {
    displays_ = std::move(displays);
}

} // namespace desto::platform
