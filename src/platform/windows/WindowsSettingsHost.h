#pragma once

#include <functional>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "CardView.h"

namespace desto::platform::windows {

[[nodiscard]] std::uint32_t ResolveSettingsThemeColor(
    std::uint32_t color,
    bool darkMode) noexcept;

struct FileCardSettingsLayout {
    int appearanceTop = 166;
    int appearanceBottom = 204;
    int toolbarLabelTop = 220;
    int toolbarTop = 250;
    int toolbarBottom = 292;
    int sourceLabelTop = 0;
    int sourceTop = 0;
    int sourceBottom = 0;
    int optionsLabelTop = 308;
    int optionsTop = 338;
    int optionsBottom = 380;
    int extraTop = 396;
};

[[nodiscard]] FileCardSettingsLayout ResolveFileCardSettingsLayout(
    bool mappingCard) noexcept;

struct WindowsSystemSettings {
    std::optional<std::int32_t> timeZoneOffsetMinutes;
    std::string language = "system";
    std::filesystem::path storageRoot;
    double globalCornerRadius = 16.0;
    bool runAtStartup = false;
    std::string desktopDoubleClickAction = "none";
    std::string taskbarDoubleClickAction = "none";
    bool pinnedCardsYieldToFullscreen = true;
    bool showIconBackgroundFrame = false;
    bool confirmFileDeletion = true;
    std::string updateChannel = "stable";
};

class WindowsSettingsHost final {
public:
    using AppearanceChangedCallback = std::function<bool(
        const domain::CardId&, const domain::CardAppearancePreferences&)>;
    using ContentChangedCallback = std::function<bool(
        const domain::CardId&, const domain::CardContentPreferences&)>;
    using ApplicationSortChangedCallback = std::function<bool(
        const domain::CardId&, domain::ApplicationItemSortMode)>;
    using MappingSortChangedCallback = std::function<bool(
        const domain::CardId&, domain::ApplicationItemSortMode)>;
    using MappingModeChangedCallback = std::function<bool(
        const domain::CardId&, domain::MappingMode)>;
    using ChromeChangedCallback = std::function<bool(
        const domain::CardId&, const domain::CardChromePreferences&)>;
    using TodoPreferencesChangedCallback = std::function<bool(
        const domain::CardId&, const domain::TodoCardPreferences&)>;
    using RestoreArchivedCallback = std::function<bool(const domain::CardId&)>;
    using RestoreArchivedItemCallback = std::function<bool(
        const domain::CardId&, const std::string&)>;
    using DeleteArchivedItemCallback = std::function<bool(
        const domain::CardId&, const std::string&)>;
    using ArchiveTodoItemCallback = std::function<bool(
        const domain::CardId&, const std::string&)>;
    using HistoricalArchiveAddedCallback = std::function<std::optional<domain::TodoItem>(
        const domain::CardId&, const std::wstring&, domain::TodoDate)>;
    using ArchiveExportCallback = std::function<bool(
        domain::TodoDate, domain::TodoDate, const std::filesystem::path&)>;
    using CardAddedCallback = std::function<std::optional<presentation::CardView>(
        domain::CardType)>;
    using CardDeletedCallback = std::function<bool(const domain::CardId&)>;
    using CardRenamedCallback = std::function<bool(
        const domain::CardId&, const std::wstring&)>;
    using CardVisibilityChangedCallback = std::function<bool(
        const domain::CardId&, bool)>;
    using CardOrderChangedCallback = std::function<bool(
        const std::vector<domain::CardId>&)>;
    using GlobalCornerRadiusChangedCallback = std::function<bool(double, bool)>;
    using TimeZoneChangedCallback = std::function<bool(std::optional<std::int32_t>)>;
    using LanguageChangedCallback = std::function<bool(const std::string&)>;
    using StorageRootChangedCallback = std::function<bool(const std::filesystem::path&)>;
    using RunAtStartupChangedCallback = std::function<bool(bool)>;
    using DesktopDoubleClickActionChangedCallback = std::function<bool(const std::string&)>;
    using TaskbarDoubleClickActionChangedCallback = std::function<bool(const std::string&)>;
    using RestoreWindowsOnNewWindowChangedCallback = std::function<bool(bool)>;
    using PinnedCardsYieldToFullscreenChangedCallback = std::function<bool(bool)>;
    using IconBackgroundFrameChangedCallback = std::function<bool(bool)>;
    using FileDeletionConfirmationChangedCallback = std::function<bool(bool)>;
    using UpdateChannelChangedCallback = std::function<bool(const std::string&)>;
    using UpdateRequestedCallback = std::function<void()>;

    explicit WindowsSettingsHost(std::wstring title = L"Desto");
    ~WindowsSettingsHost();

    WindowsSettingsHost(const WindowsSettingsHost&) = delete;
    WindowsSettingsHost& operator=(const WindowsSettingsHost&) = delete;

    void present(
        std::span<const presentation::CardView> cards,
        const WindowsSystemSettings& settings);
    void insertCard(presentation::CardView card);
    void updateCard(presentation::CardView card);
    void show();
    void hide() noexcept;
    [[nodiscard]] void* nativeHandle() const noexcept;

    void setAppearanceChangedCallback(AppearanceChangedCallback callback);
    void setContentChangedCallback(ContentChangedCallback callback);
    void setApplicationSortChangedCallback(ApplicationSortChangedCallback callback);
    void setMappingSortChangedCallback(MappingSortChangedCallback callback);
    void setMappingModeChangedCallback(MappingModeChangedCallback callback);
    void setChromeChangedCallback(ChromeChangedCallback callback);
    void setTodoPreferencesChangedCallback(TodoPreferencesChangedCallback callback);
    void setRestoreArchivedCallback(RestoreArchivedCallback callback);
    void setRestoreArchivedItemCallback(RestoreArchivedItemCallback callback);
    void setDeleteArchivedItemCallback(DeleteArchivedItemCallback callback);
    void setArchiveTodoItemCallback(ArchiveTodoItemCallback callback);
    void setHistoricalArchiveAddedCallback(HistoricalArchiveAddedCallback callback);
    void setArchiveExportCallback(ArchiveExportCallback callback);
    void setCardAddedCallback(CardAddedCallback callback);
    void setCardDeletedCallback(CardDeletedCallback callback);
    void setCardRenamedCallback(CardRenamedCallback callback);
    void setCardVisibilityChangedCallback(CardVisibilityChangedCallback callback);
    void setCardOrderChangedCallback(CardOrderChangedCallback callback);
    void setGlobalCornerRadiusChangedCallback(GlobalCornerRadiusChangedCallback callback);
    void setTimeZoneChangedCallback(TimeZoneChangedCallback callback);
    void setLanguageChangedCallback(LanguageChangedCallback callback);
    void setStorageRootChangedCallback(StorageRootChangedCallback callback);
    void setRunAtStartupChangedCallback(RunAtStartupChangedCallback callback);
    void setDesktopDoubleClickActionChangedCallback(
        DesktopDoubleClickActionChangedCallback callback);
    void setTaskbarDoubleClickActionChangedCallback(
        TaskbarDoubleClickActionChangedCallback callback);
    // Kept as a source-compatible no-op for older integrations; the
    // experimental foreground-restoration feature is removed.
    void setRestoreWindowsOnNewWindowChangedCallback(
        RestoreWindowsOnNewWindowChangedCallback callback);
    void setPinnedCardsYieldToFullscreenChangedCallback(
        PinnedCardsYieldToFullscreenChangedCallback callback);
    void setIconBackgroundFrameChangedCallback(IconBackgroundFrameChangedCallback callback);
    void setFileDeletionConfirmationChangedCallback(
        FileDeletionConfirmationChangedCallback callback);
    void setUpdateChannelChangedCallback(UpdateChannelChangedCallback callback);
    void setUpdateRequestedCallback(UpdateRequestedCallback callback);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace desto::platform::windows
