#pragma once

#include <functional>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "CardView.h"
#include "WorkspaceLayout.h"

namespace desto::platform::windows {

class WindowsDesktopHost final {
public:
    using PlacementChangedCallback = std::function<void(
        const domain::PlacementId&,
        const domain::CardId&,
        const domain::PlacementRect&)>;
    using CardExpandedChangedCallback = std::function<void(const domain::CardId&, bool)>;
    using ApplicationItemsDroppedCallback = std::function<bool(
        const domain::CardId&,
        const std::vector<std::filesystem::path>&,
        std::size_t insertionIndex)>;
    using ApplicationItemDragCompletedCallback = std::function<void(
        const domain::CardId&)>;
    using CardItemActivatedCallback = std::function<void(
        const domain::CardId&,
        const presentation::CardItemView&)>;

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
    void setPlacementChangedCallback(PlacementChangedCallback callback);
    void setCardExpandedChangedCallback(CardExpandedChangedCallback callback);
    void setApplicationItemsDroppedCallback(ApplicationItemsDroppedCallback callback);
    void setApplicationItemDragCompletedCallback(ApplicationItemDragCompletedCallback callback);
    void setCardItemActivatedCallback(CardItemActivatedCallback callback);
    void updateCardItems(
        const domain::CardId& cardId,
        std::vector<presentation::CardItemView> items);
    void updateCardContentPreferences(
        const domain::CardId& cardId,
        domain::CardContentPreferences preferences);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace desto::platform::windows
