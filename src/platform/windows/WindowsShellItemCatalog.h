#pragma once

#include <filesystem>
#include <vector>

#include "CardView.h"

namespace desto::platform::windows {

class WindowsShellItemCatalog {
public:
    [[nodiscard]] presentation::CardItemView inspect(
        const std::filesystem::path& sourcePath) const;

    [[nodiscard]] std::vector<presentation::CardItemView> enumerate(
        const std::filesystem::path& directory) const;

    [[nodiscard]] bool launch(const presentation::CardItemView& item) const noexcept;
};

} // namespace desto::platform::windows
