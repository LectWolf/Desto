#include "Card.h"

#include <cassert>
#include <stdexcept>

using namespace desto::domain;

int main() {
    ApplicationCard application("application-1", "cards/application-1");
    MappingCard mapping("mapping-1");
    TodoCard todos("todos-1");

    assert(application.type() == CardType::Application);
    assert(mapping.type() == CardType::Mapping);
    assert(todos.type() == CardType::Todo);
    assert(application.relativeStoragePath() == "cards/application-1");
    assert(mapping.requiresDeletionConfirmation());
    assert(application.deletionEffect() == CardDeletionEffect::ReturnManagedItemsToDesktop);
    assert(mapping.deletionEffect() == CardDeletionEffect::RemoveCardOnly);
    assert(todos.deletionEffect() == CardDeletionEffect::RemoveCardOnly);
    assert(application.deletionPreview().requiresConfirmation);
    assert(mapping.deletionPreview().requiresConfirmation);
    assert(todos.deletionPreview().requiresConfirmation);
    assert(mapping.mode() == MappingMode::Empty);
    assert(mapping.presentsAsFolderMapping());

    auto chrome = application.chrome();
    chrome.showCollapseControl = false;
    application.setChrome(chrome);
    assert(!application.chrome().showCollapseControl);
    assert(mapping.chrome().showCollapseControl);

    application.setAppearance({"compact", 0.8});
    assert(application.appearance().preset == "compact");
    assert(application.appearance().opacity == 0.8);

    mapping.setFolderSource("C:/Projects");
    assert(mapping.mode() == MappingMode::Folder);
    assert(mapping.presentsAsFolderMapping());
    assert(mapping.allowsSourceMutation());

    mapping.setReferences({{"item-1", "C:/Projects/App.exe"}, {"item-2", "C:/Projects/Tool.exe"}});
    assert(mapping.mode() == MappingMode::References);
    assert(!mapping.presentsAsFolderMapping());
    assert(mapping.sourceRoot().empty());

    mapping.clearSource();
    assert(mapping.mode() == MappingMode::Empty);
    assert(mapping.presentsAsFolderMapping());

    bool rejected = false;
    try {
        application.setRect({0, 0, 0, 220});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);

    rejected = false;
    try {
        ApplicationCard invalid("invalid", "C:/absolute");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);

    assert(ToString(CardType::Mapping) == "mapping");
    return 0;
}
