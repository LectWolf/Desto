#include "ApplicationCardOrdering.h"

#include <algorithm>

namespace desto::application {
namespace {

bool Contains(
    std::span<const std::filesystem::path> paths,
    const std::filesystem::path& candidate) {
    return std::find(paths.begin(), paths.end(), candidate) != paths.end();
}

void AppendUnique(
    std::vector<std::filesystem::path>& target,
    const std::filesystem::path& candidate) {
    if (!Contains(target, candidate)) {
        target.push_back(candidate);
    }
}

} // namespace

std::vector<std::filesystem::path> ReconcileApplicationItemOrder(
    std::span<const std::filesystem::path> preferredOrder,
    std::span<const std::filesystem::path> actualFileNames) {
    std::vector<std::filesystem::path> result;
    result.reserve(actualFileNames.size());
    for (const auto& preferred : preferredOrder) {
        if (Contains(actualFileNames, preferred)) {
            AppendUnique(result, preferred);
        }
    }
    for (const auto& actual : actualFileNames) {
        AppendUnique(result, actual);
    }
    return result;
}

std::vector<std::filesystem::path> MoveApplicationItemsToIndex(
    std::span<const std::filesystem::path> currentOrder,
    std::span<const std::filesystem::path> movedFileNames,
    std::size_t insertionIndex) {
    const auto originalIndex = std::min(insertionIndex, currentOrder.size());
    auto adjustedIndex = originalIndex;
    for (std::size_t index = 0; index < originalIndex; ++index) {
        if (Contains(movedFileNames, currentOrder[index])) {
            --adjustedIndex;
        }
    }

    std::vector<std::filesystem::path> result;
    result.reserve(currentOrder.size() + movedFileNames.size());
    for (const auto& current : currentOrder) {
        if (!Contains(movedFileNames, current)) {
            result.push_back(current);
        }
    }
    adjustedIndex = std::min(adjustedIndex, result.size());
    auto insertion = result.begin() + static_cast<std::ptrdiff_t>(adjustedIndex);
    for (const auto& moved : movedFileNames) {
        if (!Contains(result, moved)) {
            insertion = result.insert(insertion, moved) + 1;
        }
    }
    return result;
}

} // namespace desto::application
