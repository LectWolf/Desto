#include "ApplicationCardImport.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace desto::storage {
namespace {

std::string ComparisonKey(const std::filesystem::path& path) {
    auto value = path.lexically_normal().generic_string();
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

bool IsWithin(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    const auto rootKey = ComparisonKey(root);
    const auto candidateKey = ComparisonKey(candidate);
    return candidateKey == rootKey
        || (candidateKey.size() > rootKey.size()
            && candidateKey.starts_with(rootKey)
            && candidateKey[rootKey.size()] == '/');
}

std::filesystem::path FindAvailableDestination(
    const std::filesystem::path& directory,
    const std::filesystem::path& fileName,
    const std::vector<FileMove>& plannedMoves) {
    const auto occupied = [&](const std::filesystem::path& candidate) {
        return std::filesystem::exists(candidate)
            || std::any_of(
                plannedMoves.begin(),
                plannedMoves.end(),
                [&](const FileMove& move) { return move.destination == candidate; });
    };

    auto candidate = directory / fileName;
    if (!occupied(candidate)) {
        return candidate;
    }
    for (std::size_t suffix = 1; suffix < 1'000'000; ++suffix) {
        auto candidateName = fileName.stem();
        candidateName += " (";
        candidateName += std::to_string(suffix);
        candidateName += ")";
        candidateName += fileName.extension();
        candidate = directory / candidateName;
        if (!occupied(candidate)) {
            return candidate;
        }
    }
    throw std::runtime_error("Unable to find a conflict-free Card item name.");
}

} // namespace

ApplicationCardImportService::ApplicationCardImportService(StorageRoot storageRoot)
    : storageRoot_(std::move(storageRoot)) {
}

ApplicationCardImportPlan ApplicationCardImportService::plan(
    const domain::ApplicationCard& card,
    std::span<const std::filesystem::path> sources) const {
    ApplicationCardImportPlan result{
        .cardId = card.id(),
        .cardDirectory = storageRoot_.resolveCardPath(card.relativeStoragePath()),
    };
    for (const auto& source : sources) {
        if (source.empty() || !source.is_absolute() || !std::filesystem::exists(source)) {
            throw std::invalid_argument("Application Card import sources must exist and be absolute.");
        }
        const auto normalized = source.lexically_normal();
        if (IsWithin(result.cardDirectory, normalized)) {
            continue;
        }
        if (std::filesystem::is_directory(normalized)
            && IsWithin(normalized, result.cardDirectory)) {
            throw std::invalid_argument(
                "Application Card storage cannot be nested inside an imported directory.");
        }
        if (std::any_of(
                result.moves.begin(),
                result.moves.end(),
                [&](const FileMove& move) { return move.source == normalized; })) {
            continue;
        }
        result.moves.push_back({
            .source = normalized,
            .destination = FindAvailableDestination(
                result.cardDirectory,
                normalized.filename(),
                result.moves),
        });
    }
    return result;
}

ApplicationCardImportResult ApplicationCardImportService::execute(
    const ApplicationCardImportPlan& plan) const {
    if (plan.cardId.empty() || plan.cardDirectory.empty()) {
        throw std::invalid_argument("Application Card import plan is incomplete.");
    }
    std::filesystem::create_directories(plan.cardDirectory);
    const auto transaction = FileMoveTransaction::execute(plan.moves);
    return {
        .succeeded = transaction.succeeded,
        .completedMoves = transaction.completedMoves,
        .failures = transaction.failures,
    };
}

} // namespace desto::storage
