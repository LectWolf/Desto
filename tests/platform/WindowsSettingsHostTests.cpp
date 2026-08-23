#include "TestSupport.h"
#include "WindowsSettingsHost.h"
#include "WindowsTextInput.h"

#include <Windows.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <vector>

using namespace desto::domain;
using namespace desto::platform::windows;
using namespace desto::presentation;

namespace {

void Click(HWND window, int x, int y) {
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
}

void RunTests() {
    const auto applicationLayout = ResolveFileCardSettingsLayout(false);
    DESTO_CHECK(applicationLayout.appearanceBottom < applicationLayout.toolbarLabelTop);
    DESTO_CHECK(applicationLayout.toolbarLabelTop + 24 <= applicationLayout.toolbarTop);
    DESTO_CHECK(applicationLayout.toolbarBottom < applicationLayout.optionsLabelTop);
    DESTO_CHECK(applicationLayout.optionsLabelTop + 24 <= applicationLayout.optionsTop);
    DESTO_CHECK(applicationLayout.optionsBottom < applicationLayout.extraTop);
    const auto mappingLayout = ResolveFileCardSettingsLayout(true);
    DESTO_CHECK(mappingLayout.appearanceBottom < mappingLayout.toolbarLabelTop);
    DESTO_CHECK(mappingLayout.toolbarBottom < mappingLayout.sourceLabelTop);
    DESTO_CHECK(mappingLayout.sourceLabelTop + 24 <= mappingLayout.sourceTop);
    DESTO_CHECK(mappingLayout.sourceBottom < mappingLayout.optionsLabelTop);
    DESTO_CHECK(mappingLayout.optionsLabelTop + 24 <= mappingLayout.optionsTop);
    DESTO_CHECK(mappingLayout.optionsBottom < mappingLayout.extraTop);
    DESTO_CHECK(ResolveSettingsThemeColor(RGB(18, 19, 21), true) == RGB(18, 19, 21));
    const auto lightBackground = ResolveSettingsThemeColor(RGB(18, 19, 21), false);
    DESTO_CHECK(lightBackground == RGB(243, 243, 243));
    const auto lightNeutral = ResolveSettingsThemeColor(RGB(25, 26, 29), false);
    DESTO_CHECK(lightNeutral == RGB(255, 255, 255));
    DESTO_CHECK(ResolveSettingsThemeColor(RGB(49, 52, 58), false)
        == RGB(246, 246, 246));
    DESTO_CHECK(ResolveSettingsThemeColor(RGB(47, 113, 220), false)
        == RGB(47, 113, 220));
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto yesterday = now - std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::hours(24)).count();
    WindowsSettingsHost host(L"Desto Settings Test");
    std::vector<CardView> cards{
        {.id = "application-card", .type = CardType::Application,
         .appearancePreset = "white",
         .applicationItemPlacements = {
             {"One.lnk", 0, 0}, {"Two.lnk", 1, 0},
             {"Three.lnk", 2, 1}, {"Four.lnk", 3, 1},
             // Keep the fixture shrinkable at the ExtraLarge density.  A
             // fifth-column placement would correctly block the first
             // decrement because that density projects only four columns.
             {"Five.lnk", 3, 2},
         }},
        {.id = "todo-card", .type = CardType::Todo,
         .appearancePreset = "transparent-black",
         .todoItems = {
             {.id = "archived", .title = "Archived", .completed = true,
              .completedAtUnixMilliseconds = yesterday, .archived = true},
             {.id = "automatic", .title = "Automatic", .completed = true,
              .completedAtUnixMilliseconds = yesterday},
             {.id = "today-archived", .title = "Today archived", .completed = true,
              .completedAtUnixMilliseconds = now, .archived = true},
             {.id = "today-search", .title = "Today cross-date search", .completed = true,
              .completedAtUnixMilliseconds = now, .archived = true},
         }},
        {.id = "mapping-card", .type = CardType::Mapping,
         .mappingMode = MappingMode::References,
         .mappingHasSource = true},
    };
    bool appearanceChanged = false;
    std::string appearancePresetChanged;
    double appearanceOpacityChanged = 1.0;
    std::vector<CardContentPreferences> contentChanges;
    std::optional<CardContentPreferences> todoContentChange;
    std::optional<std::pair<CardId, CardChromePreferences>> chromeChange;
    std::optional<ApplicationItemSortMode> sortModeChanged;
    bool todoPreferencesChanged = false;
    bool mappingModeChanged = false;
    std::optional<ApplicationItemSortMode> mappingSortModeChanged;
    bool restored = false;
    std::string restoredItemId;
    bool timeZoneChanged = false;
    bool languageChanged = false;
    bool radiusCommitted = false;
    bool cardDeleted = false;
    bool cardRenamed = false;
    bool runAtStartupChanged = false;
    std::string desktopDoubleClickChanged;
    std::string taskbarDoubleClickChanged;
    bool fullscreenYieldChanged = false;
    host.setAppearanceChangedCallback(
        [&](const CardId& id, const CardAppearancePreferences& preferences) {
            DESTO_CHECK(id == "application-card");
            appearancePresetChanged = preferences.preset;
            appearanceOpacityChanged = preferences.opacity;
            appearanceChanged = preferences.preset == "brand";
            return true;
        });
    host.setContentChangedCallback(
        [&](const CardId& id, const CardContentPreferences& preferences) {
            if (id == "application-card") {
                contentChanges.push_back(preferences);
            } else {
                DESTO_CHECK(id == "todo-card");
                todoContentChange = preferences;
            }
            return true;
        });
    host.setChromeChangedCallback(
        [&](const CardId& id, const CardChromePreferences& preferences) {
            chromeChange = std::pair{id, preferences};
            return true;
        });
    host.setApplicationSortChangedCallback(
        [&](const CardId& id, ApplicationItemSortMode mode) {
            DESTO_CHECK(id == "application-card");
            sortModeChanged = mode;
            return true;
        });
    host.setTodoPreferencesChangedCallback(
        [&](const CardId& id, const TodoCardPreferences& preferences) {
            DESTO_CHECK(id == "todo-card");
            todoPreferencesChanged = preferences.showCreatedTime;
            return todoPreferencesChanged;
        });
    host.setMappingModeChangedCallback([&](const CardId& id, MappingMode mode) {
        DESTO_CHECK(id == "mapping-card");
        mappingModeChanged = mode == MappingMode::Folder;
        return mappingModeChanged;
    });
    host.setMappingSortChangedCallback(
        [&](const CardId& id, ApplicationItemSortMode mode) {
            DESTO_CHECK(id == "mapping-card");
            mappingSortModeChanged = mode;
            return true;
        });
    host.setRestoreArchivedItemCallback([&](const CardId& id, const std::string& itemId) {
        DESTO_CHECK(id == "todo-card");
        restoredItemId = itemId;
        restored = true;
        return true;
    });
    host.setTimeZoneChangedCallback([&](std::optional<std::int32_t> offset) {
        timeZoneChanged = offset == 0;
        return true;
    });
    host.setGlobalCornerRadiusChangedCallback([&](double radius, bool commit) {
        DESTO_CHECK(radius == 24.0);
        radiusCommitted = commit;
        return true;
    });
    host.setLanguageChangedCallback([&](const std::string& language) {
        languageChanged = language == "en-US";
        return languageChanged;
    });
    host.setCardDeletedCallback([&](const CardId& id) {
        DESTO_CHECK(id == "application-card");
        cardDeleted = true;
        return true;
    });
    host.setCardRenamedCallback([&](const CardId& id, const std::wstring& title) {
        DESTO_CHECK(id == "application-card");
        cardRenamed = title == L"Renamed application";
        return cardRenamed;
    });
    host.setRunAtStartupChangedCallback([&](bool enabled) {
        runAtStartupChanged = enabled;
        return true;
    });
    host.setDesktopDoubleClickActionChangedCallback([&](const std::string& action) {
        desktopDoubleClickChanged = action;
        return true;
    });
    host.setTaskbarDoubleClickActionChangedCallback([&](const std::string& action) {
        taskbarDoubleClickChanged = action;
        return true;
    });
    host.setPinnedCardsYieldToFullscreenChangedCallback([&](bool enabled) {
        fullscreenYieldChanged = !enabled;
        return true;
    });
    host.present(cards, {
        .timeZoneOffsetMinutes = 0,
        .storageRoot = L"C:\\DestoData",
        .globalCornerRadius = 16.0,
    });
    host.show();
    const auto window = static_cast<HWND>(host.nativeHandle());
    DESTO_CHECK(window != nullptr);
    DESTO_CHECK(IsWindowVisible(window));
    RECT settingsRect{};
    DESTO_CHECK(GetWindowRect(window, &settingsRect));
    DESTO_CHECK(settingsRect.right - settingsRect.left == 780);
    DESTO_CHECK(settingsRect.bottom - settingsRect.top == 640);
    RECT settingsClientRect{};
    DESTO_CHECK(GetClientRect(window, &settingsClientRect));

    Click(window, 620, 176);
    Click(window, 620, 253);
    DESTO_CHECK(timeZoneChanged);
    Click(window, 620, 230);
    Click(window, 620, 343);
    DESTO_CHECK(languageChanged);
    Click(window, 520, 398);
    DESTO_CHECK(radiusCommitted);
    Click(window, 680, 118);
    DESTO_CHECK(runAtStartupChanged);

    Click(window, 70, 70);
    Click(window, 620, 122);
    Click(window, 620, 230);
    DESTO_CHECK(desktopDoubleClickChanged == "cards");
    Click(window, 710, 180);
    Click(window, 620, 290);
    DESTO_CHECK(taskbarDoubleClickChanged == "current-display");
    Click(window, 680, 238);
    DESTO_CHECK(fullscreenYieldChanged);

    Click(window, 70, 120);
    Click(window, 678, 100);
    const auto renameEdit = GetDlgItem(window, 1001);
    DESTO_CHECK(renameEdit != nullptr);
    DESTO_CHECK(IsWindowsTextInput(renameEdit));
    DESTO_CHECK(FindWindowExW(renameEdit, nullptr, L"EDIT", nullptr) == nullptr);
    DWORD renameSelectionStart = 0;
    DWORD renameSelectionEnd = 0;
    SendMessageW(renameEdit, EM_GETSEL,
        reinterpret_cast<WPARAM>(&renameSelectionStart),
        reinterpret_cast<LPARAM>(&renameSelectionEnd));
    const auto renameTextLength = static_cast<DWORD>(GetWindowTextLengthW(renameEdit));
    DESTO_CHECK(renameSelectionStart == renameTextLength);
    DESTO_CHECK(renameSelectionEnd == renameTextLength);
    SetWindowTextW(renameEdit, L"Renamed application");
    SendMessageW(renameEdit, WM_KEYDOWN, VK_RETURN, 0);
    DESTO_CHECK(cardRenamed);
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(380, 185));
    DESTO_CHECK(!appearanceChanged);
    SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(330, 185));
    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(330, 185));
    DESTO_CHECK(!appearanceChanged);
    Click(window, 428, 185);
    DESTO_CHECK(appearanceChanged);
    Click(window, 476, 185);
    DESTO_CHECK(appearancePresetChanged == "transparent-white");
    DESTO_CHECK(appearanceOpacityChanged == 0.20);
    Click(window, 280, 185);
    DESTO_CHECK(appearancePresetChanged == "system");
    Click(window, 279, 271);
    DESTO_CHECK(chromeChange.has_value());
    DESTO_CHECK(chromeChange->first == "application-card");
    DESTO_CHECK(!chromeChange->second.showPresentationControl);

    Click(window, 735, 185);
    DESTO_CHECK(!contentChanges.empty());
    DESTO_CHECK(contentChanges.back().itemSize == CardItemSize::ExtraLarge);
    DESTO_CHECK(contentChanges.back().fixedColumns ==
        ProjectCardColumns(contentChanges.back().widthSpan,
            CardItemSize::ExtraLarge));
    DESTO_CHECK(contentChanges.back().widthSpan == 4);
    Click(window, 720, 100);
    // More -> Sort -> Modified date (cascade opens to the left).
    Click(window, 650, 142);
    Click(window, 500, 290);
    DESTO_CHECK(sortModeChanged == ApplicationItemSortMode::ModifiedDate);
    Click(window, 330, 360);
    DESTO_CHECK(contentChanges.back().sizeMode == CardSizeMode::Fixed);
    DESTO_CHECK(contentChanges.back().fixedColumns == 4);
    DESTO_CHECK(contentChanges.back().widthSpan == 5);
    DESTO_CHECK(contentChanges.back().fixedRows == 3);
    const auto changesBeforeDensityChange = contentChanges.size();
    Click(window, 674, 185);
    DESTO_CHECK(contentChanges.size() == changesBeforeDensityChange + 1);
    DESTO_CHECK(contentChanges.back().itemSize == CardItemSize::Large);
    DESTO_CHECK(contentChanges.back().fixedColumns == 5);
    DESTO_CHECK(contentChanges.back().widthSpan == 5);
    Click(window, 720, 185);
    DESTO_CHECK(contentChanges.size() == changesBeforeDensityChange + 2);
    DESTO_CHECK(contentChanges.back().itemSize == CardItemSize::ExtraLarge);
    DESTO_CHECK(contentChanges.back().fixedColumns == 4);
    DESTO_CHECK(contentChanges.back().widthSpan == 5);
    const auto changesAtMinimum = contentChanges.size();
    // The editor is split into a fixed left rail and a detail pane.  Derive
    // stepper hit points from the shared layout instead of relying on the
    // pre-rail coordinates that used to be valid here.
    constexpr int detailLeft = 258;
    constexpr int stepperGap = 8;
    constexpr int stepperTop = 396;
    constexpr int stepperHeight = 36;
    const auto firstStepperWidth =
        (settingsClientRect.right - 26 - detailLeft - 16) / 3;
    Click(window, detailLeft + firstStepperWidth - 15,
        stepperTop + stepperHeight / 2);
    DESTO_CHECK(contentChanges.size() == changesAtMinimum + 1);
    DESTO_CHECK(contentChanges.back().widthSpan == 6);
    Click(window, detailLeft + 15, stepperTop + stepperHeight / 2);
    DESTO_CHECK(contentChanges.size() == changesAtMinimum + 2);
    DESTO_CHECK(contentChanges.back().widthSpan == 5);
    Click(window, detailLeft + firstStepperWidth - 15, stepperTop + stepperHeight / 2);
    DESTO_CHECK(contentChanges.size() == changesAtMinimum + 3);
    DESTO_CHECK(contentChanges.back().widthSpan == 6);
    Click(window, detailLeft + firstStepperWidth - 15,
        stepperTop + stepperHeight / 2);
    DESTO_CHECK(contentChanges.size() == changesAtMinimum + 4);
    DESTO_CHECK(contentChanges.back().widthSpan == 7);
    Click(window, detailLeft + firstStepperWidth + stepperGap
            + firstStepperWidth - 15,
        stepperTop + stepperHeight / 2);
    DESTO_CHECK(contentChanges.back().fixedRows == 4);
    Click(window, 380, 360);
    DESTO_CHECK(contentChanges.back().maximumVisibleRows == 3);
    Click(window, 435, 360);
    DESTO_CHECK(chromeChange.has_value());
    DESTO_CHECK(chromeChange->first == "application-card");
    DESTO_CHECK(chromeChange->second.positionLocked);
    Click(window, detailLeft + 2 * (firstStepperWidth + stepperGap)
            + firstStepperWidth - 15,
        stepperTop + stepperHeight / 2);
    DESTO_CHECK(contentChanges.back().maximumVisibleRows == 4);

    Click(window, 210, 214);
    Click(window, 300, 355);
    DESTO_CHECK(!mappingModeChanged);
    Click(window, 420, 355);
    Click(window, 540, 395);
    DESTO_CHECK(mappingModeChanged);
    Click(window, 279, 271);
    DESTO_CHECK(chromeChange.has_value());
    DESTO_CHECK(chromeChange->first == "mapping-card");
    DESTO_CHECK(!chromeChange->second.showPresentationControl);
    Click(window, 720, 100);
    Click(window, 650, 142);
    Click(window, 500, 180);
    DESTO_CHECK(mappingSortModeChanged == ApplicationItemSortMode::Name);

    Click(window, 210, 160);
    Click(window, 725, 185);
    DESTO_CHECK(todoContentChange.has_value());
    DESTO_CHECK(todoContentChange->widthSpan == 6);
    Click(window, 279, 271);
    DESTO_CHECK(chromeChange.has_value());
    DESTO_CHECK(chromeChange->first == "todo-card");
    DESTO_CHECK(!chromeChange->second.showCollapseControl);
    Click(window, 279, 359);
    DESTO_CHECK(todoPreferencesChanged);
    Click(window, 70, 164);
    const auto archiveSearch = GetDlgItem(window, 1002);
    DESTO_CHECK(archiveSearch != nullptr);
    DESTO_CHECK(IsWindowsTextInput(archiveSearch));
    DESTO_CHECK(FindWindowExW(archiveSearch, nullptr, L"EDIT", nullptr) == nullptr);
    const auto archiveAddInput = GetDlgItem(window, 1003);
    DESTO_CHECK(archiveAddInput != nullptr);
    DESTO_CHECK(IsWindowsTextInput(archiveAddInput));
    DESTO_CHECK(IsWindowVisible(archiveSearch));
    SetWindowTextW(archiveSearch, L"Today cross-date search");
    Click(window, 660, 220);
    DESTO_CHECK(restored);
    DESTO_CHECK(restoredItemId == "today-search");
    restored = false;
    SetWindowTextW(archiveSearch, L"Today archived");
    RECT archiveClient{};
    DESTO_CHECK(GetClientRect(window, &archiveClient));
    constexpr int archiveContentLeft = 184;
    constexpr int calendarWidth = 336;
    const auto dateLabelLeft = archiveContentLeft + 48;
    const auto dateLabelRight = archiveClient.right - 74;
    Click(window, (dateLabelLeft + dateLabelRight) / 2, 104);
    DESTO_CHECK(!IsWindowVisible(archiveSearch));
    const auto today = CurrentTodoDate(0);
    const auto firstDay = std::chrono::sys_days{
        std::chrono::year{today.year}
        / std::chrono::month{today.month}
        / std::chrono::day{1}};
    const auto mondayOffset = static_cast<int>(
        (std::chrono::weekday{firstDay}.c_encoding() + 6) % 7);
    const auto todayCell = mondayOffset + static_cast<int>(today.day) - 1;
    const auto calendarLeft = std::clamp(
        static_cast<int>((dateLabelLeft + dateLabelRight - calendarWidth) / 2),
        archiveContentLeft,
        static_cast<int>(archiveClient.right - 26 - calendarWidth));
    Click(window,
        calendarLeft + (todayCell % 7) * 48 + 24,
        204 + (todayCell / 7) * 34 + 17);
    DESTO_CHECK(IsWindowVisible(archiveSearch));
    Click(window, 660, 220);
    DESTO_CHECK(restored);
    DESTO_CHECK(restoredItemId == "today-archived");
    restored = false;
    Click(window, 204, 104);
    SetWindowTextW(archiveSearch, L"not-present");
    Click(window, 620, 180);
    DESTO_CHECK(!restored);
    SetWindowTextW(archiveSearch, L"Archived");
    Click(window, 660, 220);
    DESTO_CHECK(restored);
    DESTO_CHECK(restoredItemId == "archived");
    restored = false;
    SetWindowTextW(archiveSearch, L"Automatic");
    Click(window, 660, 220);
    DESTO_CHECK(restored);
    DESTO_CHECK(restoredItemId == "automatic");

    auto liveArchiveCard = cards[1];
    liveArchiveCard.todoItems.push_back({
        .id = "live-archive",
        .title = "Live archive update",
        .completed = true,
        .completedAtUnixMilliseconds = yesterday,
        .archived = true,
    });
    host.updateCard(std::move(liveArchiveCard));
    restored = false;
    SetWindowTextW(archiveSearch, L"Live archive update");
    Click(window, 660, 220);
    DESTO_CHECK(restored);
    DESTO_CHECK(restoredItemId == "live-archive");

    Click(window, 70, 120);
    Click(window, 210, 100);
    Click(window, 720, 100);
    Click(window, 650, 195);
    DESTO_CHECK(!cardDeleted);
    // Confirmation panel is centered in the current client area; keep the
    // click inside the confirm button after the settings layout changes.
    Click(window, 540, 384);
    DESTO_CHECK(cardDeleted);

    SendMessageW(window, WM_CLOSE, 0, 0);
    DESTO_CHECK(!IsWindowVisible(window));

    {
        WindowsSettingsHost deletionHost(L"Desto Archive Deletion Reentrancy Test");
        CardView deletionCard{
            .id = "deletion-todo", .type = CardType::Todo,
            .todoItems = {{
                .id = "delete-me", .title = "Delete me", .completed = true,
                .completedAtUnixMilliseconds = yesterday, .archived = true,
            }},
        };
        std::size_t deleteCalls = 0;
        deletionHost.setDeleteArchivedItemCallback(
            [&](const CardId& cardId, const std::string& itemId) {
                DESTO_CHECK(cardId == deletionCard.id);
                DESTO_CHECK(itemId == "delete-me");
                ++deleteCalls;
                deletionCard.todoItems.clear();
                deletionHost.updateCard(deletionCard);
                return true;
            });
        deletionHost.present({&deletionCard, 1}, {
            .timeZoneOffsetMinutes = 0,
            .storageRoot = L"C:\\DestoData",
            .globalCornerRadius = 16.0,
        });
        deletionHost.show();
        const auto deletionWindow = static_cast<HWND>(deletionHost.nativeHandle());
        DESTO_CHECK(deletionWindow != nullptr);
        Click(deletionWindow, 70, 164);
        const auto deletionSearch = GetDlgItem(deletionWindow, 1002);
        DESTO_CHECK(deletionSearch != nullptr && IsWindowVisible(deletionSearch));
        SetWindowTextW(deletionSearch, L"Delete me");
        Click(deletionWindow, 710, 220);
        Click(deletionWindow, 540, 380);
        DESTO_CHECK(deleteCalls == 1);
        SendMessageW(deletionWindow, WM_CLOSE, 0, 0);
    }

    {
        WindowsSettingsHost contentHost(L"Desto Settings Content Test");
        std::string archivedItem;
        contentHost.setArchiveTodoItemCallback(
            [&](const CardId& cardId, const std::string& itemId) {
                DESTO_CHECK(cardId == "preview-todo");
                archivedItem = itemId;
                return true;
            });
        CardView previewCard{
            .id = "preview-todo", .type = CardType::Todo, .title = L"待办",
            .todoItems = {
                {.id = "archived-item", .title = "Archived", .archived = true},
                {.id = "completed-0", .title = "Completed 0", .completed = true},
                {.id = "completed-1", .title = "Completed 1", .completed = true},
                {.id = "completed-2", .title = "Completed 2", .completed = true},
                {.id = "completed-3", .title = "Completed 3", .completed = true},
                {.id = "completed-4", .title = "Completed 4", .completed = true},
                {.id = "completed-5", .title = "Completed 5", .completed = true},
            },
        };
        contentHost.present({&previewCard, 1}, {
            .timeZoneOffsetMinutes = 0,
            .storageRoot = L"C:\\DestoData",
            .globalCornerRadius = 16.0,
        });
        contentHost.show();
        const auto contentWindow = static_cast<HWND>(contentHost.nativeHandle());
        DESTO_CHECK(contentWindow != nullptr);
        Click(contentWindow, 70, 120);
        SendMessageW(contentWindow, WM_MOUSEMOVE, 0, MAKELPARAM(420, 434));
        Click(contentWindow, 420, 434);
        DESTO_CHECK(archivedItem == "completed-0");

        archivedItem.clear();
        SendMessageW(contentWindow, WM_MOUSEWHEEL,
            MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)),
            MAKELPARAM(420, 448));
        SendMessageW(contentWindow, WM_MOUSEMOVE, 0, MAKELPARAM(420, 434));
        Click(contentWindow, 420, 434);
        DESTO_CHECK(archivedItem == "completed-1");
        SendMessageW(contentWindow, WM_CLOSE, 0, 0);
        DESTO_CHECK(!IsWindowVisible(contentWindow));
    }

    {
        WindowsSettingsHost scrollHost(L"Desto Settings Editor Scroll Test");
        bool namesChanged = false;
        scrollHost.setContentChangedCallback(
            [&](const CardId& cardId, const CardContentPreferences& preferences) {
                DESTO_CHECK(cardId == "scroll-application");
                namesChanged = preferences.showItemNames;
                return true;
            });
        CardView scrollCard{
            .id = "scroll-application",
            .type = CardType::Application,
            .title = L"Scrollable settings",
        };
        for (int index = 0; index < 14; ++index) {
            scrollCard.items.push_back({
                .id = L"item-" + std::to_wstring(index),
                .displayName = L"Item " + std::to_wstring(index),
            });
        }
        scrollHost.present({&scrollCard, 1}, {
            .timeZoneOffsetMinutes = 0,
            .storageRoot = L"C:\\DestoData",
            .globalCornerRadius = 16.0,
        });
        scrollHost.show();
        const auto scrollWindow = static_cast<HWND>(scrollHost.nativeHandle());
        Click(scrollWindow, 70, 120);
        SendMessageW(scrollWindow, WM_MOUSEWHEEL,
            MAKEWPARAM(0, static_cast<WORD>(-4 * WHEEL_DELTA)), 0);
        // The file preview is now a six-column icon grid, so its editor is
        // shorter and the same wheel delta reaches a smaller offset.
        Click(scrollWindow, 278, 300);
        DESTO_CHECK(namesChanged);
        SendMessageW(scrollWindow, WM_CLOSE, 0, 0);
    }

    {
        WindowsSettingsHost railHost(L"Desto Card Rail Test");
        std::optional<std::pair<CardId, bool>> visibilityChange;
        std::vector<CardId> reordered;
        railHost.setCardVisibilityChangedCallback([&](const CardId& id, bool visible) {
            visibilityChange = std::pair{id, visible};
            return true;
        });
        railHost.setCardOrderChangedCallback([&](const std::vector<CardId>& order) {
            reordered = order;
            return true;
        });
        const std::vector<CardView> railCards{
            {.id = "rail-a", .type = CardType::Application, .title = L"A"},
            {.id = "rail-b", .type = CardType::Todo, .title = L"B"},
            {.id = "rail-c", .type = CardType::Mapping, .title = L"C"},
        };
        railHost.present(railCards, {
            .storageRoot = L"C:\\DestoData",
        });
        railHost.show();
        const auto railWindow = static_cast<HWND>(railHost.nativeHandle());
        Click(railWindow, 70, 120);
        Click(railWindow, 225, 122);
        DESTO_CHECK(visibilityChange.has_value());
        DESTO_CHECK(visibilityChange->first == "rail-a");
        DESTO_CHECK(!visibilityChange->second);
        SendMessageW(railWindow, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(210, 100));
        Sleep(200);
        SendMessageW(railWindow, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(210, 214));
        SendMessageW(railWindow, WM_LBUTTONUP, 0, MAKELPARAM(210, 214));
        DESTO_CHECK((reordered
            == std::vector<CardId>({"rail-b", "rail-c", "rail-a"})));
        SendMessageW(railWindow, WM_CLOSE, 0, 0);
    }

    {
        WindowsSettingsHost keyboardHost(L"Desto Settings Keyboard Test");
        keyboardHost.present({}, {
            .timeZoneOffsetMinutes = 0,
            .storageRoot = L"C:\\DestoData",
            .globalCornerRadius = 16.0,
        });
        keyboardHost.show();
        const auto keyboardWindow = static_cast<HWND>(keyboardHost.nativeHandle());
        const auto search = GetDlgItem(keyboardWindow, 1002);
        DESTO_CHECK(keyboardWindow != nullptr && search != nullptr);
        DESTO_CHECK(!IsWindowVisible(search));
        // Navigate to Archive explicitly. The sidebar now includes the
        // dedicated About actions and no longer has the old tab-count shape.
        Click(keyboardWindow, 70, 14 + 3 * 44 + 18);
        DESTO_CHECK(IsWindowVisible(search));

        SetFocus(search);
        DESTO_CHECK(GetFocus() == search);
        SetWindowTextW(search, L"keyboard selection");
        SetFocus(search);
        BYTE keyboardState[256]{};
        DESTO_CHECK(GetKeyboardState(keyboardState));
        const auto previousControlState = keyboardState[VK_CONTROL];
        keyboardState[VK_CONTROL] = static_cast<BYTE>(previousControlState | 0x80);
        DESTO_CHECK(SetKeyboardState(keyboardState));
        SendMessageW(search, WM_KEYDOWN, 'A', 0);
        keyboardState[VK_CONTROL] = previousControlState;
        DESTO_CHECK(SetKeyboardState(keyboardState));
        DWORD selectionStart = 0;
        DWORD selectionEnd = 0;
        SendMessageW(search, EM_GETSEL,
            reinterpret_cast<WPARAM>(&selectionStart),
            reinterpret_cast<LPARAM>(&selectionEnd));
        DESTO_CHECK(selectionStart == 0 && selectionEnd == 18);

        Click(keyboardWindow, 70, 14 + 0 * 44 + 18);
        DESTO_CHECK(!IsWindowVisible(search));
        SendMessageW(keyboardWindow, WM_KEYDOWN, VK_ESCAPE, 0);
        DESTO_CHECK(!IsWindowVisible(keyboardWindow));
    }
}

} // namespace

int main() {
    return desto::test::Run(RunTests);
}
