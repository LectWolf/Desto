#include "TestSupport.h"
#include "WindowsDesktopHost.h"

#include <Windows.h>
#include <CommCtrl.h>

#include <thread>
#include <vector>

using namespace desto::domain;
using namespace desto::platform::windows;
using namespace desto::presentation;

namespace {

HWND FindOwnedTooltip(HWND owner) {
    struct Search {
        HWND owner;
        HWND result = nullptr;
    } search{owner};
    EnumWindows([](HWND candidate, LPARAM parameter) {
        auto& value = *reinterpret_cast<Search*>(parameter);
        wchar_t className[64]{};
        if (GetWindow(candidate, GW_OWNER) == value.owner
            && GetClassNameW(candidate, className, 64) > 0
            && _wcsicmp(className, L"DestoItemTooltip") == 0) {
            value.result = candidate;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.result;
}

void RunTests() {
    const std::vector<DisplaySnapshot> displays{
        {
            .id = "display-test",
            .workAreaWidth = 1920,
            .workAreaHeight = 1040,
            .effectiveDpi = 96,
            .primary = true,
        },
        {
            .id = "display-high-dpi",
            .workAreaLeft = 1280,
            .workAreaWidth = 1280,
            .workAreaHeight = 720,
            .effectiveDpi = 144,
        },
    };
    {
    WindowsDesktopHost host(L"Desto Host Test");
    const std::vector<PlacementProjection> projections{{
        .placementId = "placement-test",
        .cardId = "card-test",
        .displayId = "display-test",
        .rect = {40, 48, 320, 220},
    }};
    const std::vector<CardView> cards{{
        .id = "card-test",
        .type = CardType::Application,
        .title = L"Application",
        .typeLabel = L"Application",
    }};
    PlacementRect changed{};
    DisplayId changedDisplayId;
    auto horizontalAnchor = PlacementHorizontalAnchor::Free;
    auto verticalAnchor = PlacementVerticalAnchor::Free;
    double referenceWorkAreaWidth = 0;
    double referenceWorkAreaHeight = 0;
    bool callbackCalled = false;
    bool expansionCallbackCalled = false;
    bool expanded = true;
    bool itemActivated = false;
    host.setPlacementChangedCallback(
        [&](const PlacementId& placementId,
            const CardId& cardId,
            const DisplayId& displayId,
            const PlacementRect& rect,
            PlacementHorizontalAnchor horizontal,
            PlacementVerticalAnchor vertical,
            double referenceWidth,
            double referenceHeight) {
            DESTO_CHECK(placementId == "placement-test");
            DESTO_CHECK(cardId == "card-test");
            changedDisplayId = displayId;
            changed = rect;
            horizontalAnchor = horizontal;
            verticalAnchor = vertical;
            referenceWorkAreaWidth = referenceWidth;
            referenceWorkAreaHeight = referenceHeight;
            callbackCalled = true;
        });
    host.setCardExpandedChangedCallback([&](const CardId& cardId, bool value) {
        DESTO_CHECK(cardId == "card-test");
        expansionCallbackCalled = true;
        expanded = value;
    });
    host.setCardItemActivatedCallback(
        [&](const CardId& cardId, const CardItemView& item) {
            DESTO_CHECK(cardId == "card-test");
            DESTO_CHECK(item.displayName == L"Example.txt");
            itemActivated = true;
        });
    host.present(projections, displays, cards);

    const auto window = FindWindowW(L"DestoDesktopHostSurface", L"Desto Host Test");
    DESTO_CHECK(window != nullptr);
    const auto styles = GetWindowLongPtrW(window, GWL_EXSTYLE);
    DESTO_CHECK((styles & WS_EX_LAYERED) != 0);
    DESTO_CHECK((styles & WS_EX_TOOLWINDOW) != 0);
    DESTO_CHECK((styles & WS_EX_NOACTIVATE) != 0);
    DESTO_CHECK((styles & WS_EX_ACCEPTFILES) == 0);
    DESTO_CHECK(RevokeDragDrop(window) == S_OK);

    RECT windowRect{};
    DESTO_CHECK(GetWindowRect(window, &windowRect));
    DESTO_CHECK(windowRect.right - windowRect.left == 244);
    DESTO_CHECK(windowRect.bottom - windowRect.top == 127);
    const auto captionHit = SendMessageW(
        window,
        WM_NCHITTEST,
        0,
        MAKELPARAM(windowRect.left + 50, windowRect.top + 20));
    DESTO_CHECK(captionHit == HTCAPTION);
    const auto cornerHit = SendMessageW(
        window,
        WM_NCHITTEST,
        0,
        MAKELPARAM(windowRect.left + 2, windowRect.top + 2));
    DESTO_CHECK(cornerHit == HTCAPTION);

    RECT movingRect{7, 9, 251, 136};
    SendMessageW(window, WM_MOVING, 0, reinterpret_cast<LPARAM>(&movingRect));
    const auto guide = FindWindowW(L"DestoAlignmentGuide", nullptr);
    DESTO_CHECK(guide != nullptr);
    DESTO_CHECK(IsWindowVisible(guide));
    DESTO_CHECK((GetWindowLongPtrW(guide, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0);

    DESTO_CHECK(SetWindowPos(window, nullptr, 7, 9, 244, 127, SWP_NOACTIVATE | SWP_NOZORDER));
    SendMessageW(window, WM_EXITSIZEMOVE, 0, 0);
    DESTO_CHECK(!IsWindowVisible(guide));
    DESTO_CHECK(callbackCalled);
    DESTO_CHECK(changed.left == 8);
    DESTO_CHECK(changed.top == 8);
    DESTO_CHECK(changed.width == 244);
    DESTO_CHECK(changed.height == 127);
    DESTO_CHECK(horizontalAnchor == PlacementHorizontalAnchor::Left);
    DESTO_CHECK(verticalAnchor == PlacementVerticalAnchor::Top);
    DESTO_CHECK(referenceWorkAreaWidth == 1920);
    DESTO_CHECK(referenceWorkAreaHeight == 1040);
    DESTO_CHECK(changedDisplayId == "display-test");

    host.updateCardItems("card-test", {{
        .id = L"example",
        .displayName = L"Example.txt",
        .sourcePath = L"C:\\Example.exe",
        .state = CardItemState::IconUnavailable,
    }});
    host.updateCardContentPreferences(
        "card-test",
        {.itemSize = CardItemSize::Large, .showItemNames = false});
    DESTO_CHECK(GetWindowRect(window, &windowRect));
    DESTO_CHECK(windowRect.right - windowRect.left == 244);
    DESTO_CHECK(windowRect.bottom - windowRect.top == 127);

    const auto tooltip = FindOwnedTooltip(window);
    DESTO_CHECK(tooltip != nullptr);
    wchar_t tooltipClass[64]{};
    DESTO_CHECK(GetClassNameW(
        tooltip, tooltipClass, static_cast<int>(std::size(tooltipClass))) > 0);
    DESTO_CHECK(std::wstring_view(tooltipClass) == L"DestoItemTooltip");
    SendMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(56, 82));
    DESTO_CHECK(!IsWindowVisible(tooltip));
    SendMessageW(window, WM_TIMER, 2, 0);
    DESTO_CHECK(IsWindowVisible(tooltip));
    wchar_t tooltipText[128]{};
    GetWindowTextW(tooltip, tooltipText, static_cast<int>(std::size(tooltipText)));
    DESTO_CHECK(std::wstring_view(tooltipText) == L"Example.txt");
    SendMessageW(window, WM_MOUSELEAVE, 0, 0);
    DESTO_CHECK(!IsWindowVisible(tooltip));
    SendMessageW(window, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(56, 82));
    DESTO_CHECK(itemActivated);

    std::vector<CardItemView> adaptiveItems;
    for (int index = 0; index < 5; ++index) {
        adaptiveItems.push_back({
            .id = std::to_wstring(index),
            .displayName = std::to_wstring(index),
            .sourcePath = L"C:\\Item" + std::to_wstring(index) + L".lnk",
            .state = CardItemState::IconUnavailable,
        });
    }
    host.updateCardItems("card-test", adaptiveItems);
    DESTO_CHECK(GetWindowRect(window, &windowRect));
    DESTO_CHECK(windowRect.right - windowRect.left == 244);
    host.updateCardItemsBatch({{
        "card-test",
        adaptiveItems,
        ApplicationItemSortMode::Custom,
        {
            {"Item0.lnk", 0, 0},
            {"Item1.lnk", 1, 0},
            {"Item2.lnk", 2, 0},
            {"Item3.lnk", 3, 0},
            {"Item4.lnk", 4, 0},
        },
    }});
    DESTO_CHECK(GetWindowRect(window, &windowRect));
    DESTO_CHECK(windowRect.right - windowRect.left == 299);
    adaptiveItems.pop_back();
    host.updateCardItemsBatch({{
        "card-test",
        adaptiveItems,
        ApplicationItemSortMode::Custom,
        {
            {"Item0.lnk", 0, 0},
            {"Item1.lnk", 1, 0},
            {"Item2.lnk", 2, 0},
            {"Item3.lnk", 3, 0},
        },
    }});
    DESTO_CHECK(GetWindowRect(window, &windowRect));
    DESTO_CHECK(windowRect.right - windowRect.left == 244);

    int refreshCount = 0;
    host.setCardItemsRefreshCallback(
        [&](const CardId& cardId, CardItemSize itemSize) {
            DESTO_CHECK(cardId == "card-test");
            DESTO_CHECK(itemSize == CardItemSize::Medium || itemSize == CardItemSize::Large);
            ++refreshCount;
            return adaptiveItems;
        });

    host.updateCardContentPreferences(
        "card-test",
        {
            .itemSize = CardItemSize::Medium,
            .showItemNames = false,
            .sizeMode = CardSizeMode::Fixed,
            .fixedColumns = 2,
            .fixedRows = 3,
        });
    DESTO_CHECK(GetWindowRect(window, &windowRect));
    DESTO_CHECK(windowRect.right - windowRect.left == 180);
    DESTO_CHECK(windowRect.bottom - windowRect.top == 204);
    DESTO_CHECK(refreshCount == 1);

    host.updateCardContentPreferences(
        "card-test",
        {.itemSize = CardItemSize::Large, .showItemNames = false});
    DESTO_CHECK(GetWindowRect(window, &windowRect));
    DESTO_CHECK(windowRect.right - windowRect.left == 244);
    DESTO_CHECK(windowRect.bottom - windowRect.top == 127);
    DESTO_CHECK(refreshCount == 2);

    SendMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(232, 24));
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(232, 24));
    DESTO_CHECK(expansionCallbackCalled);
    DESTO_CHECK(!expanded);
    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(232, 24));
    expansionCallbackCalled = false;
    SendMessageW(window, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(232, 24));
    DESTO_CHECK(expansionCallbackCalled);
    DESTO_CHECK(expanded);
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(232, 24));
    DESTO_CHECK(!expanded);
    DESTO_CHECK(SendMessageW(
        window,
        WM_NCHITTEST,
        0,
        MAKELPARAM(windowRect.left + 100, windowRect.top + 100)) == HTTRANSPARENT);

    DESTO_CHECK(SetWindowPos(
        window, nullptr, 2050, 120, 244, 127, SWP_NOACTIVATE | SWP_NOZORDER));
    RECT crossDisplayMovingRect{2050, 120, 2294, 247};
    SendMessageW(
        window,
        WM_MOVING,
        0,
        reinterpret_cast<LPARAM>(&crossDisplayMovingRect));
    DESTO_CHECK(crossDisplayMovingRect.right - crossDisplayMovingRect.left == 366);
    SendMessageW(window, WM_EXITSIZEMOVE, 0, 0);
    DESTO_CHECK(changedDisplayId == "display-high-dpi");
    DESTO_CHECK(changed.width == 244);
    DESTO_CHECK(GetWindowRect(window, &windowRect));
    DESTO_CHECK(windowRect.right - windowRect.left == 366);

    const auto refreshesBeforeQueuedRequest = refreshCount;
    host.setCardItemsRefreshCallback(
        [&](const CardId& cardId, CardItemSize itemSize) {
            DESTO_CHECK(cardId == "card-test");
            DESTO_CHECK(itemSize == CardItemSize::Large);
            ++refreshCount;
            host.requestClose();
            return adaptiveItems;
        });
    std::thread refreshRequester([&] {
        host.requestCardItemsRefresh("card-test");
        host.requestCardItemsRefresh("card-test");
    });
    refreshRequester.join();
    host.run();
    DESTO_CHECK(refreshCount == refreshesBeforeQueuedRequest + 1);
    }

    const std::vector<PlacementProjection> mappingProjections{1, {
        .placementId = "mapping-placement",
        .cardId = "mapping-card",
        .displayId = "display-test",
        .rect = {400, 48, 320, 220},
    }};
    const std::vector<CardView> mappingCards{1, {
        .id = "mapping-card",
        .type = CardType::Mapping,
        .title = L"Mapping",
        .typeLabel = L"Mapping",
        .mappingMode = MappingMode::Empty,
    }};
    {
        WindowsDesktopHost emptyMappingHost(L"Desto Empty Mapping Host Test");
        emptyMappingHost.present(mappingProjections, displays, mappingCards);
        const auto mappingWindow = FindWindowW(
            L"DestoDesktopHostSurface", L"Desto Empty Mapping Host Test");
        DESTO_CHECK(mappingWindow != nullptr);
        RECT mappingWindowRect{};
        DESTO_CHECK(GetWindowRect(mappingWindow, &mappingWindowRect));
        DESTO_CHECK(mappingWindowRect.bottom - mappingWindowRect.top == 127);
        DESTO_CHECK(RevokeDragDrop(mappingWindow) == S_OK);
    }

    auto immutableCards = mappingCards;
    immutableCards.front().mappingMode = MappingMode::Folder;
    immutableCards.front().mappingAllowsSourceMutation = false;
    {
        WindowsDesktopHost immutableMappingHost(L"Desto Immutable Mapping Host Test");
        immutableMappingHost.present(mappingProjections, displays, immutableCards);
        const auto immutableWindow = FindWindowW(
            L"DestoDesktopHostSurface", L"Desto Immutable Mapping Host Test");
        DESTO_CHECK(immutableWindow != nullptr);
        DESTO_CHECK(RevokeDragDrop(immutableWindow) == DRAGDROP_E_NOTREGISTERED);
    }

    const std::vector<PlacementProjection> todoProjections{1, {
        .placementId = "todo-placement",
        .cardId = "todo-card",
        .displayId = "display-test",
        .rect = {760, 48, 320, 220},
    }};
    std::vector<CardView> todoCards{1, {
        .id = "todo-card",
        .type = CardType::Todo,
        .title = L"Todo",
        .typeLabel = L"Todo",
        .todoItems = {
            {.id = "todo-first", .title = "First task", .completed = false},
            {.id = "todo-second", .title = "Second task", .completed = false},
        },
    }};
    {
        WindowsDesktopHost todoHost(L"Desto Todo Host Test");
        bool completedChanged = false;
        bool renamed = false;
        bool archived = false;
        std::optional<TodoDate> addedDate;
        std::vector<std::string> reordered;
        todoHost.setTodoItemCompletedChangedCallback(
            [&](const CardId& cardId, const std::string& itemId, bool completed) {
                DESTO_CHECK(cardId == "todo-card");
                DESTO_CHECK(itemId == "todo-first");
                completedChanged = completed;
                return true;
            });
        todoHost.setTodoItemRenamedCallback(
            [&](const CardId& cardId, const std::string& itemId, const std::string& title) {
                DESTO_CHECK(cardId == "todo-card");
                DESTO_CHECK(itemId == "todo-second");
                renamed = title == "Renamed task";
                return renamed;
            });
        todoHost.setTodoItemsReorderedCallback(
            [&](const CardId& cardId, const std::vector<std::string>& order) {
                DESTO_CHECK(cardId == "todo-card");
                reordered = order;
                return true;
            });
        todoHost.setTodoItemsArchivedCallback([&](const CardId& cardId) {
            DESTO_CHECK(cardId == "todo-card");
            archived = true;
            return true;
        });
        todoHost.setTodoItemAddedScheduledCallback(
            [&](const CardId& cardId, const std::string& title, TodoDate date)
                -> std::optional<TodoItem> {
                DESTO_CHECK(cardId == "todo-card");
                DESTO_CHECK(title == "Scheduled task");
                addedDate = date;
                return TodoItem{.id = "todo-scheduled", .title = title, .scheduledDate = date};
            });
        todoHost.present(todoProjections, displays, todoCards);
        const auto todoWindow = FindWindowW(L"DestoDesktopHostSurface", L"Desto Todo Host Test");
        DESTO_CHECK(todoWindow != nullptr);
        RECT todoRect{};
        DESTO_CHECK(GetWindowRect(todoWindow, &todoRect));
        DESTO_CHECK(todoRect.bottom - todoRect.top == 200);

        SendMessageW(todoWindow, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(26, 131));
        DESTO_CHECK(completedChanged);

        SendMessageW(todoWindow, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(100, 131));
        SendMessageW(todoWindow, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(100, 173));
        SendMessageW(todoWindow, WM_LBUTTONUP, 0, MAKELPARAM(100, 173));
        DESTO_CHECK(reordered == std::vector<std::string>({"todo-second", "todo-first"}));

        SendMessageW(todoWindow, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(100, 131));
        SendMessageW(todoWindow, WM_LBUTTONUP, 0, MAKELPARAM(100, 131));
        const auto editor = FindWindowW(L"Edit", L"Second task");
        DESTO_CHECK(editor != nullptr);
        SetWindowTextW(editor, L"Renamed task");
        SendMessageW(editor, WM_KEYDOWN, VK_RETURN, 0);
        DESTO_CHECK(renamed);

        todoCards.front().todoItems.front().completed = true;
        todoCards.front().todoItems.push_back({"todo-third", "Third task", false});
        todoHost.updateTodoItems("todo-card", todoCards.front().todoItems);
        DESTO_CHECK(GetWindowRect(todoWindow, &todoRect));
        DESTO_CHECK(todoRect.bottom - todoRect.top == 242);

        SendMessageW(todoWindow, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(259, 24));
        DESTO_CHECK(archived);
        SendMessageW(todoWindow, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(250, 70));
        SendMessageW(todoWindow, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(40, 70));
        const auto addEditor = FindWindowW(L"Edit", L"");
        DESTO_CHECK(addEditor != nullptr);
        SetWindowTextW(addEditor, L"Scheduled task");
        SendMessageW(addEditor, WM_KEYDOWN, VK_RETURN, 0);
        DESTO_CHECK(addedDate == AddTodoDays(CurrentSystemTodoDate(), 1));
    }
}

} // namespace

int main() {
    return desto::test::Run(RunTests);
}
