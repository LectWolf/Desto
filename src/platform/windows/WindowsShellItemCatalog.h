#pragma once

#include <filesystem>
#include <span>
#include <vector>

#include "CardView.h"

namespace desto::platform::windows {

enum class ShellIconSourceSize : int {
    Small = 16,
    Medium = 32,
};

[[nodiscard]] ShellIconSourceSize ResolveShellIconSourceSize(
    domain::CardItemSize itemSize) noexcept;

class WindowsShellItemCatalog {
public:
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

    [[nodiscard]] presentation::CardItemView retarget(
        presentation::CardItemView preparedItem,
        const std::filesystem::path& destinationPath) const;

    void notifyMoved(
        const std::filesystem::path& sourcePath,
        const std::filesystem::path& destinationPath) const noexcept;

    [[nodiscard]] bool launch(const presentation::CardItemView& item) const noexcept;
};

} // namespace desto::platform::windows
