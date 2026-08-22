#pragma once

#include <functional>
#include <filesystem>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "CardView.h"
#include "WorkspaceLayout.h"

namespace desto::platform::windows {

enum class FileDropOperation {
    Move,
    Copy,
};

// Returns the OLE effect accepted by a file Card. Folder mappings move external
// files into their source directory, while reference mappings only add links.
[[nodiscard]] std::uint32_t ResolveFileCardDropEffect(
    domain::CardType type,
    domain::MappingMode mappingMode,
    bool mappingHasSource,
    bool mappingAllowsSourceMutation,
    bool sameCardSource,
    std::uint32_t allowedEffects,
    bool mappingCanNavigateUp = false) noexcept;

[[nodiscard]] std::wstring_view ResolveTodoTextFontFamily(
    std::wstring_view text) noexcept;

struct CrystalMaterialStyle {
    double surfaceOpacity;
    double itemFillOpacity;
    double itemOutlineOpacity;
    double surfaceOutlineOpacity;
};

[[nodiscard]] CrystalMaterialStyle ResolveCrystalMaterialStyle() noexcept;
[[nodiscard]] std::uint32_t ResolveLayeredSurfaceTextQuality() noexcept;
[[nodiscard]] std::uint32_t CompositeCrystalLayerPixel(
    std::uint32_t materialRgb,
    std::uint32_t materialAlpha,
    std::uint32_t premultipliedContent,
    double shapeCoverage) noexcept;

class WindowsDesktopHost final {
public:
    struct RenderStatistics {
        std::uint64_t fullSurfaceRenders = 0;
        std::uint64_t fullSurfaceCommits = 0;
        std::uint64_t fullSurfaceCommitNanoseconds = 0;
    };
    using PlacementChangedCallback = std::function<void(
        const domain::PlacementId&,
        const domain::CardId&,
        const domain::DisplayId&,
        const domain::PlacementRect&,
        domain::PlacementHorizontalAnchor,
        domain::PlacementVerticalAnchor,
        double referenceWorkAreaWidth,
        double referenceWorkAreaHeight)>;
    using CardExpandedChangedCallback = std::function<void(const domain::CardId&, bool)>;
    using CardPinChangedCallback = std::function<bool(const domain::CardId&, bool)>;
    using MappingPresentationChangedCallback = std::function<bool(
        const domain::CardId&, domain::MappingPresentationMode)>;
    using ApplicationItemsDroppedCallback = std::function<bool(
        const domain::CardId&,
        const std::vector<std::filesystem::path>&,
        const std::optional<domain::CardId>& sourceCardId,
        FileDropOperation operation,
        std::size_t insertionIndex,
        std::size_t layoutColumns)>;
    using ApplicationItemDragCompletedCallback = std::function<void(
        const domain::CardId&)>;
    using CardItemActivatedCallback = std::function<void(
        const domain::CardId&,
        const presentation::CardItemView&)>;
    using CardItemContextMenuCallback = std::function<bool(
        const domain::CardId&,
        const presentation::CardItemView&,
        int screenX,
        int screenY)>;
    using MappingNavigateUpCallback = std::function<void(const domain::CardId&)>;
    using MappingReferenceRemovedCallback = std::function<bool(
        const domain::CardId&,
        const presentation::CardItemView&)>;
    using FileDeleteConfirmationCallback = std::function<bool(
        const domain::CardId&,
        const presentation::CardItemView&)>;
    using CardItemsRefreshCallback = std::function<std::vector<presentation::CardItemView>(
        const domain::CardId&,
        domain::CardItemSize)>;
    using TodoItemAddedCallback = std::function<std::optional<domain::TodoItem>(
        const domain::CardId&,
        const std::string&)>;
    using TodoItemAddedScheduledCallback = std::function<std::optional<domain::TodoItem>(
        const domain::CardId&,
        const std::string&,
        domain::TodoDate)>;
    using TodoItemCompletedChangedCallback = std::function<bool(
        const domain::CardId&,
        const std::string&,
        bool)>;
    using TodoItemRemovedCallback = std::function<bool(
        const domain::CardId&,
        const std::string&)>;
    using TodoItemsReorderedCallback = std::function<bool(
        const domain::CardId&,
        const std::vector<std::string>&)>;
    using TodoItemsArchivedCallback = std::function<bool(const domain::CardId&)>;

    explicit WindowsDesktopHost(std::wstring title = L"Desto");
    ~WindowsDesktopHost();

    WindowsDesktopHost(const WindowsDesktopHost&) = delete;
    WindowsDesktopHost& operator=(const WindowsDesktopHost&) = delete;

    // Creates or updates all Projection windows in one position/visibility commit.
    void present(
        std::span<const domain::PlacementProjection> projections,
        std::span<const domain::DisplaySnapshot> displays,
        std::span<const presentation::CardView> cards);

    // Adds only the supplied Card surfaces. Existing Card HWNDs and backing
    // bitmaps are preserved.
    void insertCard(
        std::span<const domain::PlacementProjection> projections,
        const presentation::CardView& card);
    // Removes only surfaces belonging to the supplied Card.
    void removeCard(const domain::CardId& cardId) noexcept;

    // Runs the host message loop until requestClose() or the optional timeout.
    int run(int durationMilliseconds = 0);
    void requestClose() noexcept;
    void setCardsVisible(bool visible);
    [[nodiscard]] bool cardsVisible() const noexcept;
    void setPinnedCardsYieldToFullscreen(bool enabled) noexcept;
    void setIconBackgroundFrameVisible(bool enabled) noexcept;
    void setPlacementChangedCallback(PlacementChangedCallback callback);
    void setCardExpandedChangedCallback(CardExpandedChangedCallback callback);
    void setCardPinChangedCallback(CardPinChangedCallback callback);
    void setMappingPresentationChangedCallback(MappingPresentationChangedCallback callback);
    void setApplicationItemsDroppedCallback(ApplicationItemsDroppedCallback callback);
    void setApplicationItemDragCompletedCallback(ApplicationItemDragCompletedCallback callback);
    void setCardItemActivatedCallback(CardItemActivatedCallback callback);
    void setCardItemContextMenuCallback(CardItemContextMenuCallback callback);
    void setMappingNavigateUpCallback(MappingNavigateUpCallback callback);
    void setMappingReferenceRemovedCallback(MappingReferenceRemovedCallback callback);
    void setFileDeleteConfirmationCallback(FileDeleteConfirmationCallback callback);
    void setCardItemsRefreshCallback(CardItemsRefreshCallback callback);
    void setTodoItemAddedCallback(TodoItemAddedCallback callback);
    void setTodoItemAddedScheduledCallback(TodoItemAddedScheduledCallback callback);
    void setTodoItemCompletedChangedCallback(TodoItemCompletedChangedCallback callback);
    void setTodoItemRemovedCallback(TodoItemRemovedCallback callback);
    void setTodoItemsReorderedCallback(TodoItemsReorderedCallback callback);
    void setTodoItemsArchivedCallback(TodoItemsArchivedCallback callback);
    void updateCardItems(
        const domain::CardId& cardId,
        std::vector<presentation::CardItemView> items);
    void updateMappingCard(
        const domain::CardId& cardId,
        domain::MappingMode mode,
        bool allowsSourceMutation,
        std::vector<presentation::CardItemView> items,
        domain::ApplicationItemSortMode sortMode = domain::ApplicationItemSortMode::Custom,
        std::vector<domain::ApplicationItemPlacement> itemPlacements = {},
        bool mappingHasSource = false);
    void updateMappingNavigation(
        const domain::CardId& cardId,
        std::wstring title,
        bool canNavigateUp,
        std::vector<presentation::CardItemView> items,
        std::vector<domain::ApplicationItemPlacement> itemPlacements = {});
    struct CardItemsUpdate {
        domain::CardId cardId;
        std::vector<presentation::CardItemView> items;
        domain::ApplicationItemSortMode sortMode = domain::ApplicationItemSortMode::Custom;
        std::vector<domain::ApplicationItemPlacement> itemPlacements;
    };
    void updateCardItemsBatch(std::vector<CardItemsUpdate> updates);
    void updateTodoItems(
        const domain::CardId& cardId,
        std::vector<domain::TodoItem> items);
    // Thread-safe. The refresh callback and rendering run later on the host thread.
    void requestCardItemsRefresh(const domain::CardId& cardId) noexcept;
    void updateCardContentPreferences(
        const domain::CardId& cardId,
        domain::CardContentPreferences preferences,
        std::optional<std::vector<domain::ApplicationItemPlacement>> itemPlacements = std::nullopt);
    void updateCardChromePreferences(
        const domain::CardId& cardId,
        domain::CardChromePreferences preferences);
    void updateCardAppearancePreferences(
        const domain::CardId& cardId,
        domain::CardAppearancePreferences preferences);
    void updateTodoPreferences(
        const domain::CardId& cardId,
        domain::TodoCardPreferences preferences);
    void updateCardTitles(std::span<const presentation::CardView> cards);
    void setTimeZoneOffsetMinutes(std::optional<std::int32_t> offsetMinutes);
    void setLanguage(std::string language);
    void setOverlayWindow(void* window) noexcept;
    [[nodiscard]] RenderStatistics renderStatistics() const noexcept;
    void resetRenderStatistics() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace desto::platform::windows
