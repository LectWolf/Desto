#include "Card.h"

#include <cassert>
#include <stdexcept>

using namespace desto::domain;

int main() {
    ApplicationCard application("application-1", "cards/application-1");
    FolderMappingCard folder("folder-1", "C:/Projects");
    ReferenceCard references("references-1");
    TodoCard todos("todos-1");

    assert(application.type() == CardType::Application);
    assert(folder.type() == CardType::FolderMapping);
    assert(references.type() == CardType::Reference);
    assert(todos.type() == CardType::Todo);
    assert(application.relativeStoragePath() == "cards/application-1");
    assert(folder.allowsSourceMutation());

    auto chrome = application.chrome();
    chrome.showCollapseControl = false;
    application.setChrome(chrome);
    assert(!application.chrome().showCollapseControl);
    assert(folder.chrome().showCollapseControl);

    application.setAppearance({"compact", 0.8});
    assert(application.appearance().preset == "compact");
    assert(application.appearance().opacity == 0.8);

    folder.setAllowsSourceMutation(false);
    assert(!folder.allowsSourceMutation());
    folder.setAllowsSourceMutation(true);
    assert(folder.allowsSourceMutation());

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

    assert(ToString(CardType::Reference) == "reference");
    return 0;
}
