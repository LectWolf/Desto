#pragma once

#include <filesystem>
#include <span>
#include <vector>

#include "Card.h"

namespace desto::application {

struct ApplicationCardLocation {
    domain::CardId cardId;
    std::filesystem::path directory;
};

[[nodiscard]] std::vector<domain::CardId> ResolveApplicationCardRefreshBatch(
    const domain::CardId& targetCardId,
    std::span<const ApplicationCardLocation> cards,
    std::span<const std::filesystem::path> sourcePaths);

} // namespace desto::application
