#include "Card.h"
#include "TestSupport.h"

#include <stdexcept>

using namespace desto::domain;

namespace {

void RunTests() {
    ApplicationCard application("application-1", "cards/application-1");
    MappingCard mapping("mapping-1");
    TodoCard todos("todos-1");

    DESTO_CHECK(application.type() == CardType::Application);
    DESTO_CHECK(mapping.type() == CardType::Mapping);
    DESTO_CHECK(todos.type() == CardType::Todo);
    DESTO_CHECK(application.relativeStoragePath() == "cards/application-1");
    DESTO_CHECK(mapping.requiresDeletionConfirmation());
    DESTO_CHECK(application.deletionEffect() == CardDeletionEffect::ReturnManagedItemsToDesktop);
    DESTO_CHECK(mapping.deletionEffect() == CardDeletionEffect::RemoveCardOnly);
    DESTO_CHECK(todos.deletionEffect() == CardDeletionEffect::RemoveCardOnly);
    DESTO_CHECK(application.deletionPreview().requiresConfirmation);
    DESTO_CHECK(mapping.deletionPreview().requiresConfirmation);
    DESTO_CHECK(todos.deletionPreview().requiresConfirmation);
    DESTO_CHECK(mapping.mode() == MappingMode::Empty);
    DESTO_CHECK(mapping.presentsAsFolderMapping());

    auto chrome = application.chrome();
    chrome.showCollapseControl = false;
    application.setChrome(chrome);
    DESTO_CHECK(!application.chrome().showCollapseControl);
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

    mapping.setFolderSource("C:/Projects");
    DESTO_CHECK(mapping.mode() == MappingMode::Folder);
    DESTO_CHECK(mapping.presentsAsFolderMapping());
    DESTO_CHECK(mapping.allowsSourceMutation());

    mapping.setReferences({{"item-1", "C:/Projects/App.exe"}, {"item-2", "C:/Projects/Tool.exe"}});
    DESTO_CHECK(mapping.mode() == MappingMode::References);
    DESTO_CHECK(!mapping.presentsAsFolderMapping());
    DESTO_CHECK(mapping.sourceRoot().empty());

    mapping.clearSource();
    DESTO_CHECK(mapping.mode() == MappingMode::Empty);
    DESTO_CHECK(mapping.presentsAsFolderMapping());

    bool rejected = false;
    try {
        ApplicationCard invalid("invalid", "C:/absolute");
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
