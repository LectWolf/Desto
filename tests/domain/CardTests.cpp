#include "Card.h"
#include "TestSupport.h"

#include <stdexcept>

using namespace desto::domain;

namespace {

void RunTests() {
    DESTO_CHECK(!CardContentPreferences{}.showItemNames);
    DESTO_CHECK(CardContentPreferences{}.itemSize == CardItemSize::Large);
    DESTO_CHECK(CardContentPreferences{}.widthSpan == 4);
    DESTO_CHECK(CardChromePreferences{}.showPresentationControl);
    DESTO_CHECK(!CardChromePreferences{}.positionLocked);
    DESTO_CHECK(!CardContentPreferences{}.maximumVisibleRows.has_value());
    ApplicationCard application("application-1", "cards/application-1");
    MappingCard mapping("mapping-1");
    TodoCard todos("todos-1");

    DESTO_CHECK(application.type() == CardType::Application);
    DESTO_CHECK(mapping.type() == CardType::Mapping);
    DESTO_CHECK(todos.type() == CardType::Todo);
    DESTO_CHECK(application.appearance().preset == "system");
    DESTO_CHECK(mapping.appearance().preset == "system");
    DESTO_CHECK(todos.appearance().preset == "system");
    DESTO_CHECK(application.relativeStoragePath() == "cards/application-1");
    DESTO_CHECK(mapping.requiresDeletionConfirmation());
    DESTO_CHECK(application.deletionEffect() == CardDeletionEffect::ReturnManagedItemsToDesktop);
    DESTO_CHECK(mapping.deletionEffect() == CardDeletionEffect::RemoveCardOnly);
    DESTO_CHECK(todos.deletionEffect() == CardDeletionEffect::RemoveCardOnly);
    DESTO_CHECK(application.deletionPreview().requiresConfirmation);
    DESTO_CHECK(mapping.deletionPreview().requiresConfirmation);
    DESTO_CHECK(todos.deletionPreview().requiresConfirmation);
    DESTO_CHECK(mapping.mode() == MappingMode::References);
    DESTO_CHECK(!mapping.presentsAsFolderMapping());
    DESTO_CHECK(mapping.presentationMode() == MappingPresentationMode::Grid);
    mapping.setPresentationMode(MappingPresentationMode::List);
    DESTO_CHECK(mapping.presentationMode() == MappingPresentationMode::List);
    DESTO_CHECK(IsValidTodoDate({2024, 2, 29}));
    DESTO_CHECK(!IsValidTodoDate({2023, 2, 29}));
    DESTO_CHECK(AddTodoDays({2024, 2, 28}, 1) == TodoDate(2024, 2, 29));
    DESTO_CHECK(ToString({2026, 8, 16}) == "2026-08-16");
    DESTO_CHECK(TodoDateAtUnixMilliseconds(0, 0) == TodoDate(1970, 1, 1));
    DESTO_CHECK(TodoDateAtUnixMilliseconds(0, -60) == TodoDate(1969, 12, 31));
    const TodoItem completedToday{
        .id = "completed", .title = "Completed", .completed = true,
        .completedAtUnixMilliseconds = 3'600'000,
    };
    DESTO_CHECK(!IsTodoItemArchived(completedToday, {1970, 1, 1}, 0));
    DESTO_CHECK(IsTodoItemArchived(completedToday, {1970, 1, 2}, 0));
    auto explicitlyArchived = completedToday;
    explicitlyArchived.archived = true;
    DESTO_CHECK(IsTodoItemArchived(explicitlyArchived, {1970, 1, 1}, 0));
    const std::vector<TodoItem> datedTodos{
        {.id = "active-today", .title = "Today", .scheduledDate = TodoDate{1970, 1, 2}},
        {.id = "overdue", .title = "Overdue", .scheduledDate = TodoDate{1970, 1, 1}},
        {.id = "archived-yesterday", .title = "Old",
         .completed = true, .completedAtUnixMilliseconds = 0,
         .scheduledDate = TodoDate{1970, 1, 1}, .archived = true},
        {.id = "tomorrow", .title = "Tomorrow", .scheduledDate = TodoDate{1970, 1, 3}},
    };
    const auto todayView = ResolveTodoDateView(datedTodos, {1970, 1, 2}, 0, 0);
    DESTO_CHECK(todayView.size() == 2);
    DESTO_CHECK(todayView[0].index == 1 && todayView[0].overdue);
    DESTO_CHECK(todayView[1].index == 0 && !todayView[1].archived);
    const auto tomorrowView = ResolveTodoDateView(datedTodos, {1970, 1, 2}, 1, 0);
    DESTO_CHECK(tomorrowView.size() == 1);
    DESTO_CHECK(tomorrowView[0].index == 3);
    const auto historyView = ResolveTodoDateView(datedTodos, {1970, 1, 2}, -1, 0);
    DESTO_CHECK(historyView.size() == 2);
    DESTO_CHECK(historyView[0].index == 1 && !historyView[0].archived);
    DESTO_CHECK(historyView[1].index == 2 && historyView[1].archived);

    todos.setItems({
        {.id = "todo-1", .title = "First", .completed = false,
         .createdAtUnixMilliseconds = 100, .scheduledDate = TodoDate{2026, 8, 16}},
        {.id = "todo-2", .title = "Second", .completed = true, .archived = true},
    });
    DESTO_CHECK(todos.items().size() == 2);
    DESTO_CHECK(todos.items()[0].scheduledDate == TodoDate(2026, 8, 16));
    const auto validTodos = todos.items();

    bool invalidTodosRejected = false;
    try {
        todos.setItems({
            {.id = "duplicate", .title = "First"},
            {.id = "duplicate", .title = "Second"},
        });
    } catch (const std::invalid_argument&) {
        invalidTodosRejected = true;
    }
    DESTO_CHECK(invalidTodosRejected);
    DESTO_CHECK(todos.items() == validTodos);

    invalidTodosRejected = false;
    try {
        todos.setItems({{
            .id = "incomplete-with-time", .title = "Invalid",
            .completedAtUnixMilliseconds = 100,
        }});
    } catch (const std::invalid_argument&) {
        invalidTodosRejected = true;
    }
    DESTO_CHECK(invalidTodosRejected);
    DESTO_CHECK(todos.items() == validTodos);

    invalidTodosRejected = false;
    try {
        todos.setItems({{.id = "blank", .title = "  \t"}});
    } catch (const std::invalid_argument&) {
        invalidTodosRejected = true;
    }
    DESTO_CHECK(invalidTodosRejected);
    DESTO_CHECK(todos.items() == validTodos);

    auto chrome = application.chrome();
    chrome.showCollapseControl = false;
    chrome.positionLocked = true;
    application.setChrome(chrome);
    DESTO_CHECK(!application.chrome().showCollapseControl);
    DESTO_CHECK(application.chrome().positionLocked);
    DESTO_CHECK(mapping.chrome().showCollapseControl);

    application.setAppearance({"compact", 0.8});
    DESTO_CHECK(application.appearance().preset == "compact");
    DESTO_CHECK(application.appearance().opacity == 0.8);
    application.setContent({.itemSize = CardItemSize::Large, .showItemNames = false});
    application.setSortMode(ApplicationItemSortMode::ModifiedDate);
    application.setItemPlacements({{"Browser.lnk", 0, 0}, {"Editor.exe", 2, 0}});
    DESTO_CHECK(application.content().itemSize == CardItemSize::Large);
    DESTO_CHECK(!application.content().showItemNames);
    DESTO_CHECK(application.sortMode() == ApplicationItemSortMode::ModifiedDate);
    DESTO_CHECK(application.itemPlacements().size() == 2);
    DESTO_CHECK(application.itemPlacements()[1].column == 2);
    application.setPresentationMode(MappingPresentationMode::List);
    DESTO_CHECK(application.presentationMode() == MappingPresentationMode::List);

    mapping.setFolderSource("C:/Projects");
    DESTO_CHECK(mapping.mode() == MappingMode::Folder);
    DESTO_CHECK(mapping.presentsAsFolderMapping());
    DESTO_CHECK(mapping.allowsSourceMutation());

    mapping.setReferences({{"item-1", "C:/Projects/App.exe"}, {"item-2", "C:/Projects/Tool.exe"}});
    DESTO_CHECK(mapping.mode() == MappingMode::References);
    DESTO_CHECK(!mapping.presentsAsFolderMapping());
    DESTO_CHECK(mapping.sourceRoot().empty());
    mapping.setLayout(ApplicationItemSortMode::Custom, {
        {"C:/Projects/Tool.exe", 0, 0},
        {"C:/Projects/App.exe", 2, 0},
    });
    DESTO_CHECK(mapping.itemPlacements().size() == 2);
    DESTO_CHECK(mapping.itemPlacements()[1].column == 2);

    mapping.clearSource();
    DESTO_CHECK(mapping.mode() == MappingMode::References);
    DESTO_CHECK(mapping.itemPlacements().empty());
    DESTO_CHECK(!mapping.presentsAsFolderMapping());
    mapping.setMode(MappingMode::Folder);
    DESTO_CHECK(mapping.mode() == MappingMode::Folder);
    mapping.clearSource();
    DESTO_CHECK(mapping.mode() == MappingMode::Folder);

    bool rejected = false;
    try {
        ApplicationCard invalid("invalid", "C:/absolute");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    DESTO_CHECK(rejected);

    rejected = false;
    try {
        mapping.setItemPlacements({{"relative.txt", 0, 0}});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    DESTO_CHECK(rejected);

    rejected = false;
    try {
        application.setContent({
            .itemSize = CardItemSize::Medium,
            .showItemNames = true,
            .sizeMode = CardSizeMode::Fixed,
            .fixedColumns = 0,
            .fixedRows = 3,
        });
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    DESTO_CHECK(rejected);

    rejected = false;
    try {
        application.setContent({
            .itemSize = CardItemSize::Small,
            .widthSpan = 2,
        });
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    DESTO_CHECK(rejected);

    DESTO_CHECK(MinimumCardWidthSpan(CardItemSize::Small) == 3);
    DESTO_CHECK(MinimumCardWidthSpan(CardItemSize::ExtraLarge) == 2);
    DESTO_CHECK(ProjectCardColumns(4, CardItemSize::Small) == 6);
    DESTO_CHECK(ProjectCardColumns(4, CardItemSize::ExtraLarge) == 3);
    DESTO_CHECK(InferCardWidthSpan(5, CardItemSize::Medium) == 4);

    rejected = false;
    try {
        application.setItemPlacements({{"nested/Editor.exe", 0, 0}});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    DESTO_CHECK(rejected);

    DESTO_CHECK(ToString(CardType::Mapping) == "mapping");
    DESTO_CHECK(ToString(CardItemSize::ExtraLarge) == "extraLarge");
    DESTO_CHECK(ToString(CardSizeMode::Fixed) == "fixed");
    DESTO_CHECK(ToString(ApplicationItemSortMode::ItemType) == "itemType");
}

} // namespace

int main() {
    return desto::test::Run(RunTests);
}
