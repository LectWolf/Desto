#include "StorageRoot.h"

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
    if (candidateKey == rootKey) {
        return true;
    }

    return candidateKey.size() > rootKey.size()
        && candidateKey.starts_with(rootKey)
        && candidateKey[rootKey.size()] == '/';
}

} // namespace

StorageRoot::StorageRoot(std::filesystem::path root)
    : root_(std::move(root)) {
    if (root_.empty() || !root_.is_absolute()) {
        throw std::invalid_argument("Storage root must be an absolute path.");
    }
    root_ = root_.lexically_normal();
}

std::filesystem::path StorageRoot::resolveCardPath(const std::filesystem::path& relativePath) const {
    if (relativePath.empty() || relativePath.is_absolute()) {
        throw std::invalid_argument("Card storage path must be relative and non-empty.");
    }

    const auto candidate = (root_ / relativePath).lexically_normal();
    if (!IsWithin(root_, candidate)) {
        throw std::invalid_argument("Card storage path escapes the storage root.");
    }
    return candidate;
}

void StorageRoot::ensureExists() const {
    std::filesystem::create_directories(root_);
}

} // namespace desto::storage
