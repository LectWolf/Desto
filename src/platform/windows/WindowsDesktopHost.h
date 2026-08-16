#pragma once

#include <memory>
#include <span>
#include <string>

#include "CardView.h"
#include "WorkspaceLayout.h"

namespace desto::platform::windows {

class WindowsDesktopHost final {
public:
    explicit WindowsDesktopHost(std::wstring title = L"Desto");
    ~WindowsDesktopHost();

    WindowsDesktopHost(const WindowsDesktopHost&) = delete;
    WindowsDesktopHost& operator=(const WindowsDesktopHost&) = delete;

    // Creates or updates all Projection windows in one position/visibility commit.
    void present(
        std::span<const domain::PlacementProjection> projections,
        std::span<const domain::DisplaySnapshot> displays,
        std::span<const presentation::CardView> cards);

    // Runs the host message loop until requestClose() or the optional timeout.
    int run(int durationMilliseconds = 0);
    void requestClose() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace desto::platform::windows
