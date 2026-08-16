#include "ApplicationCardTransfer.h"

#include <algorithm>
#include <unordered_set>

namespace desto::application {

namespace {

bool IsPathInsideDirectory(
    const std::filesystem::path& path,
    const std::filesystem::path& directory) {
    const auto normalizedPath = path.lexically_normal();
    const auto normalizedDirectory = directory.lexically_normal();
    auto pathPart = normalizedPath.begin();
    auto directoryPart = normalizedDirectory.begin();
    for (; directoryPart != normalizedDirectory.end(); ++directoryPart, ++pathPart) {
        if (pathPart == normalizedPath.end()) {
            return false;
        }
#ifdef _WIN32
        if (_wcsicmp(pathPart->c_str(), directoryPart->c_str()) != 0) return false;
#else
        if (*pathPart != *directoryPart) return false;
#endif
    }
    return pathPart != normalizedPath.end();
}

} // namespace

std::vector<domain::CardId> ResolveApplicationCardRefreshBatch(
    const domain::CardId& targetCardId,
    std::span<const ApplicationCardLocation> cards,
    std::span<const std::filesystem::path> sourcePaths) {
    const auto target = std::find_if(
        cards.begin(), cards.end(), [&](const ApplicationCardLocation& card) {
            return card.cardId == targetCardId;
        });
    if (target == cards.end()) {
        return {};
    }

    std::vector<domain::CardId> result;
    std::unordered_set<domain::CardId> seen;
    for (const auto& card : cards) {
        const auto containsSource = std::ranges::any_of(
            sourcePaths, [&](const std::filesystem::path& sourcePath) {
                return IsPathInsideDirectory(sourcePath, card.directory);
            });
        if (containsSource && seen.insert(card.cardId).second) {
            result.push_back(card.cardId);
        }
    }
    if (seen.insert(targetCardId).second) {
        result.push_back(targetCardId);
    }
    return result;
}

} // namespace desto::application
