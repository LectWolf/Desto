#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

namespace desto::application {

[[nodiscard]] std::vector<std::filesystem::path> ReconcileApplicationItemOrder(
    std::span<const std::filesystem::path> preferredOrder,
    std::span<const std::filesystem::path> actualFileNames);

[[nodiscard]] std::vector<std::filesystem::path> MoveApplicationItemsToIndex(
    std::span<const std::filesystem::path> currentOrder,
    std::span<const std::filesystem::path> movedFileNames,
    std::size_t insertionIndex);

} // namespace desto::application
