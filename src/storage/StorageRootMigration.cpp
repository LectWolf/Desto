#include "StorageRootMigration.h"

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
    while (value.size() > 1 && value.back() == '/') {
        value.pop_back();
    }
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

} // namespace

StorageRootMigrationPlan StorageRootMigrationService::plan(
    const StorageRoot& source,
    std::filesystem::path target) const {
    target = target.lexically_normal();
    if (target.empty() || !target.is_absolute()) {
        throw std::invalid_argument("Migration target must be an absolute path.");
    }

    const auto sourcePath = source.path();
    if (ComparisonKey(sourcePath) == ComparisonKey(target)) {
        throw std::invalid_argument("Migration source and target must differ.");
    }
    if (IsWithin(sourcePath, target) || IsWithin(target, sourcePath)) {
        throw std::invalid_argument("Migration source and target must not contain one another.");
    }
    if (!std::filesystem::exists(sourcePath) || !std::filesystem::is_directory(sourcePath)) {
        throw std::invalid_argument("Migration source directory does not exist.");
    }
    if (std::filesystem::exists(target) && !std::filesystem::is_directory(target)) {
        throw std::invalid_argument("Migration target exists and is not a directory.");
    }
    if (std::filesystem::exists(target)
        && std::filesystem::directory_iterator(target) != std::filesystem::directory_iterator{}) {
        throw std::invalid_argument("Migration target directory must be empty.");
    }

    StorageRootMigrationPlan result{
        .sourceRoot = sourcePath,
        .targetRoot = target,
    };
    for (const auto& entry : std::filesystem::directory_iterator(sourcePath)) {
        result.moves.push_back({
            .source = entry.path(),
            .destination = target / entry.path().filename(),
        });
    }
    return result;
}

StorageRootMigrationResult StorageRootMigrationService::execute(
    const StorageRootMigrationPlan& migrationPlan) const {
    StorageRootMigrationResult result;
    try {
        std::filesystem::create_directories(migrationPlan.targetRoot);
    } catch (const std::exception& exception) {
        result.failures.push_back(exception.what());
        return result;
    }

    const auto transaction = FileMoveTransaction::execute(migrationPlan.moves);
    if (!transaction.succeeded) {
        result.failures = transaction.failures;
        return result;
    }
    result.completedMoves = transaction.completedMoves;

    try {
        if (std::filesystem::exists(migrationPlan.sourceRoot)) {
            std::filesystem::remove(migrationPlan.sourceRoot);
        }
        result.succeeded = true;
        return result;
    } catch (const std::exception& exception) {
        result.failures.push_back(exception.what());
        const auto rollbackResult = FileMoveTransaction::rollback(result.completedMoves);
        result.failures.insert(
            result.failures.end(),
            rollbackResult.failures.begin(),
            rollbackResult.failures.end());
        if (rollbackResult.succeeded) {
            result.completedMoves.clear();
        }
        return result;
    }
}

StorageRootMigrationResult StorageRootMigrationService::migrate(
    const StorageRoot& source,
    std::filesystem::path target,
    const JsonConfigStore& configStore,
    int schemaVersion) const {
    const auto migrationPlan = plan(source, std::move(target));
    auto result = execute(migrationPlan);
    if (!result.succeeded) {
        return result;
    }

    try {
        auto config = configStore.load();
        config.schemaVersion = schemaVersion;
        config.storageRoot = migrationPlan.targetRoot;
        configStore.save(config);
        return result;
    } catch (const std::exception& exception) {
        result.succeeded = false;
        result.failures.push_back(exception.what());
        const auto rollbackResult = FileMoveTransaction::rollback(result.completedMoves);
        result.failures.insert(
            result.failures.end(),
            rollbackResult.failures.begin(),
            rollbackResult.failures.end());
        if (rollbackResult.succeeded) {
            std::error_code ignored;
            std::filesystem::create_directories(migrationPlan.sourceRoot, ignored);
            result.completedMoves.clear();
        }
        return result;
    }
}

} // namespace desto::storage
