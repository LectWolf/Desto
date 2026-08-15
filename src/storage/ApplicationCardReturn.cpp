#include "ApplicationCardReturn.h"

#include <algorithm>
#include <format>
#include <stdexcept>

namespace desto::storage {
namespace {

std::filesystem::path FindAvailableDestination(
    const std::filesystem::path& directory,
    const std::filesystem::path& fileName,
    const std::vector<ReturnMove>& plannedMoves) {
    auto candidate = directory / fileName;
    const auto isOccupied = [&](const std::filesystem::path& path) {
        if (std::filesystem::exists(path)) {
            return true;
        }
        return std::any_of(
            plannedMoves.begin(),
            plannedMoves.end(),
            [&](const ReturnMove& move) { return move.destination == path; });
    };

    if (!isOccupied(candidate)) {
        return candidate;
    }

    const auto stem = fileName.stem().string();
    const auto extension = fileName.extension().string();
    for (std::size_t suffix = 1; suffix < 1'000'000; ++suffix) {
        candidate = directory / std::format("{} ({}){}", stem, suffix, extension);
        if (!isOccupied(candidate)) {
            return candidate;
        }
    }

    throw std::runtime_error("Unable to find a conflict-free desktop name.");
}

void MoveEntry(const std::filesystem::path& source, const std::filesystem::path& destination) {
    std::filesystem::create_directories(destination.parent_path());
    try {
        std::filesystem::rename(source, destination);
    } catch (const std::filesystem::filesystem_error&) {
        std::filesystem::copy(
            source,
            destination,
            std::filesystem::copy_options::recursive
                | std::filesystem::copy_options::copy_symlinks);
        std::filesystem::remove_all(source);
    }
}

} // namespace

ApplicationCardReturnService::ApplicationCardReturnService(StorageRoot storageRoot)
    : storageRoot_(std::move(storageRoot)) {
}

ApplicationCardDeletionPlan ApplicationCardReturnService::plan(
    const domain::ApplicationCard& card,
    std::filesystem::path desktopDirectory) const {
    if (desktopDirectory.empty() || !desktopDirectory.is_absolute()) {
        throw std::invalid_argument("Desktop directory must be an absolute path.");
    }

    ApplicationCardDeletionPlan result{
        .preview = card.deletionPreview(),
        .cardDirectory = storageRoot_.resolveCardPath(card.relativeStoragePath()),
        .desktopDirectory = desktopDirectory.lexically_normal(),
    };

    if (!std::filesystem::exists(result.cardDirectory)) {
        return result;
    }

    for (const auto& entry : std::filesystem::directory_iterator(result.cardDirectory)) {
        result.moves.push_back({
            .source = entry.path(),
            .destination = FindAvailableDestination(
                result.desktopDirectory,
                entry.path().filename(),
                result.moves),
        });
    }

    std::sort(
        result.moves.begin(),
        result.moves.end(),
        [](const ReturnMove& left, const ReturnMove& right) {
            return left.source.filename() < right.source.filename();
        });
    return result;
}

ApplicationCardDeletionResult ApplicationCardReturnService::execute(
    const ApplicationCardDeletionPlan& plan,
    const DeletionConfirmation& confirmation) const {
    if (!plan.preview.requiresConfirmation
        || plan.preview.effect != domain::CardDeletionEffect::ReturnManagedItemsToDesktop
        || confirmation.cardId != plan.preview.cardId) {
        throw std::invalid_argument("A matching deletion confirmation is required.");
    }

    ApplicationCardDeletionResult result;
    try {
        for (const auto& move : plan.moves) {
            MoveEntry(move.source, move.destination);
            result.completedMoves.push_back(move);
        }
        if (std::filesystem::exists(plan.cardDirectory)) {
            std::filesystem::remove(plan.cardDirectory);
        }
        result.succeeded = true;
        return result;
    } catch (const std::exception& exception) {
        result.failures.push_back(exception.what());
        for (auto iterator = result.completedMoves.rbegin();
             iterator != result.completedMoves.rend();
             ++iterator) {
            try {
                MoveEntry(iterator->destination, iterator->source);
            } catch (const std::exception& rollbackException) {
                result.failures.push_back(rollbackException.what());
            }
        }
        result.completedMoves.clear();
        return result;
    }
}

} // namespace desto::storage
