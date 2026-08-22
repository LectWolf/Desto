#include "CardView.h"
#include "TestSupport.h"

#include "Card.h"

using namespace desto::domain;
using namespace desto::presentation;

namespace {

void RunTests() {
    ApplicationCard application("application-1", "cards/application-1");
    auto applicationView = MakeCardView(application);
    DESTO_CHECK(applicationView.id == "application-1");
    DESTO_CHECK(applicationView.type == CardType::Application);
    DESTO_CHECK(applicationView.title == L"应用");
    DESTO_CHECK(applicationView.showTitle);
    DESTO_CHECK(applicationView.showCollapseControl);
    DESTO_CHECK(!applicationView.showCloseControl);
    DESTO_CHECK(!applicationView.showPinControl);
    DESTO_CHECK(applicationView.showPresentationControl);
    DESTO_CHECK(!applicationView.positionLocked);
    DESTO_CHECK(applicationView.opacity == 1.0);
    const auto englishApplicationView = MakeCardView(application, "en-US");
    DESTO_CHECK(englishApplicationView.title == L"Applications");
    DESTO_CHECK(englishApplicationView.typeLabel == L"Applications");

    application.setName("常用工具");
    applicationView = MakeCardView(application);
    DESTO_CHECK(applicationView.title == L"常用工具");

    auto chrome = application.chrome();
    chrome.showTitle = false;
    chrome.showCollapseControl = false;
    chrome.showCloseControl = false;
    chrome.showPinControl = false;
    chrome.showPresentationControl = false;
    chrome.positionLocked = true;
    application.setChrome(chrome);
    application.setExpanded(false);
    application.setAppearance({"dark", 0.75, 28.0});
    application.setContent({.itemSize = CardItemSize::Small, .showItemNames = false});
    applicationView = MakeCardView(application);
    DESTO_CHECK(!applicationView.showTitle);
    DESTO_CHECK(!applicationView.showCollapseControl);
    DESTO_CHECK(!applicationView.showCloseControl);
    DESTO_CHECK(!applicationView.showPinControl);
    DESTO_CHECK(!applicationView.showPresentationControl);
    DESTO_CHECK(applicationView.positionLocked);
    DESTO_CHECK(!applicationView.expanded);
    DESTO_CHECK(applicationView.appearancePreset == "dark");
    DESTO_CHECK(applicationView.opacity == 0.75);
    DESTO_CHECK(applicationView.cornerRadius == 28.0);
    DESTO_CHECK(applicationView.content.itemSize == CardItemSize::Small);
    DESTO_CHECK(!applicationView.content.showItemNames);

    MappingCard mapping("mapping-1");
    mapping.setFolderSource(std::filesystem::temp_directory_path() / "DestoMappingView");
    mapping.setAllowsSourceMutation(false);
    const auto mappingView = MakeCardView(mapping);
    DESTO_CHECK(mappingView.mappingMode == MappingMode::Folder);
    DESTO_CHECK(!mappingView.mappingAllowsSourceMutation);

    TodoCard todo("todo-1");
    todo.setItems({
        {.id = "first", .title = "First task", .completed = false},
        {.id = "second", .title = "Second task", .completed = true},
    });
    const auto todoView = MakeCardView(todo);
    DESTO_CHECK(todoView.todoItems == todo.items());
}

} // namespace

int main() {
    return desto::test::Run(RunTests);
}
