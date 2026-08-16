#include "DirectoryImportPlanner.h"

#include <algorithm>
#include <cwctype>
#include <stdexcept>

namespace desto::storage {
namespace {

std::wstring ComparisonKey(const std::filesystem::path& path) {
    auto value = path.lexically_normal().generic_wstring();
    std::ranges::transform(value, value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

bool IsWithin(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    const auto rootKey = ComparisonKey(root);
    const auto candidateKey = ComparisonKey(candidate);
    return candidateKey == rootKey
        || (candidateKey.size() > rootKey.size()
            && candidateKey.starts_with(rootKey)
            && candidateKey[rootKey.size()] == L'/');
}

std::filesystem::path FindAvailableDestination(
    const std::filesystem::path& directory,
    const std::filesystem::path& fileName,
    const std::vector<FileMove>& plannedMoves) {
    const auto occupied = [&](const std::filesystem::path& candidate) {
        return std::filesystem::exists(candidate)
            || std::ranges::any_of(plannedMoves, [&](const FileMove& move) {
                return ComparisonKey(move.destination) == ComparisonKey(candidate);
            });
    };
    auto candidate = directory / fileName;
    if (!occupied(candidate)) {
        return candidate;
    }
    for (std::size_t suffix = 1; suffix < 1'000'000; ++suffix) {
        auto name = fileName.stem();
        name += " (";
        name += std::to_string(suffix);
        name += ")";
        name += fileName.extension();
        candidate = directory / name;
        if (!occupied(candidate)) {
            return candidate;
        }
    }
    throw std::runtime_error("Unable to find a conflict-free import name.");
}

} // namespace

DirectoryImportPlan DirectoryImportPlanner::plan(
    std::filesystem::path destinationDirectory,
    std::span<const std::filesystem::path> sources) {
    if (destinationDirectory.empty() || !destinationDirectory.is_absolute()) {
        throw std::invalid_argument("Import destination must be an absolute directory.");
    }
    destinationDirectory = destinationDirectory.lexically_normal();
    std::error_code error;
    if (!std::filesystem::is_directory(destinationDirectory, error) || error) {
        throw std::invalid_argument("Import destination must exist and be a directory.");
    }
    DirectoryImportPlan result{.destinationDirectory = destinationDirectory};
    for (const auto& sourceValue : sources) {
        if (sourceValue.empty() || !sourceValue.is_absolute()
            || !std::filesystem::exists(sourceValue)) {
            throw std::invalid_argument("Import sources must exist and be absolute.");
        }
        const auto source = sourceValue.lexically_normal();
        if (IsWithin(destinationDirectory, source)) {
            continue;
        }
        if (std::filesystem::is_directory(source) && IsWithin(source, destinationDirectory)) {
            throw std::invalid_argument("Import destination cannot be nested in a source.");
        }
        if (std::ranges::any_of(result.moves, [&](const FileMove& move) {
                return ComparisonKey(move.source) == ComparisonKey(source);
            })) {
            continue;
        }
        result.moves.push_back({
            source,
            FindAvailableDestination(destinationDirectory, source.filename(), result.moves),
        });
    }
    return result;
}

} // namespace desto::storage
