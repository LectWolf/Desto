#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include "CardView.h"

namespace desto::platform::windows {

enum class ShellIconSourceSize : int {
    Small = 32,
    Medium = 48,
    Large = 64,
    ExtraLarge = 96,
};

[[nodiscard]] ShellIconSourceSize ResolveShellIconSourceSize(
    domain::CardItemSize itemSize) noexcept;

struct ShellItemCacheStats {
    std::size_t entries = 0;
    std::size_t iconBytes = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
};

class WindowsShellItemCatalog {
public:
    explicit WindowsShellItemCatalog(
        std::size_t maximumEntries = 256,
        std::size_t maximumIconBytes = 8 * 1024 * 1024);
    ~WindowsShellItemCatalog();

    WindowsShellItemCatalog(const WindowsShellItemCatalog&) = delete;
    WindowsShellItemCatalog& operator=(const WindowsShellItemCatalog&) = delete;

    [[nodiscard]] presentation::CardItemView inspect(
        const std::filesystem::path& sourcePath,
        ShellIconSourceSize iconSize = ShellIconSourceSize::Medium) const;

    [[nodiscard]] std::vector<presentation::CardItemView> enumerate(
        const std::filesystem::path& directory,
        std::span<const std::filesystem::path> preferredOrder = {},
        ShellIconSourceSize iconSize = ShellIconSourceSize::Medium) const;

    [[nodiscard]] std::vector<presentation::CardItemView> refreshIcons(
        std::span<const presentation::CardItemView> items,
        ShellIconSourceSize iconSize) const;

    [[nodiscard]] std::vector<presentation::CardItemView> refreshDirectoryEntries(
        const std::filesystem::path& directory,
        std::span<const presentation::CardItemView> currentItems,
        std::span<const std::filesystem::path> changedRelativePaths,
        ShellIconSourceSize iconSize = ShellIconSourceSize::Medium) const;

    [[nodiscard]] presentation::CardItemView retarget(
        presentation::CardItemView preparedItem,
        const std::filesystem::path& destinationPath) const;

    void notifyMoved(
        const std::filesystem::path& sourcePath,
        const std::filesystem::path& destinationPath) const noexcept;

    void invalidate(const std::filesystem::path& sourcePath) const noexcept;
    void clearCache() const noexcept;
    [[nodiscard]] ShellItemCacheStats cacheStats() const noexcept;

    [[nodiscard]] bool launch(const presentation::CardItemView& item) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace desto::platform::windows
