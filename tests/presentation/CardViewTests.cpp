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
    DESTO_CHECK(applicationView.title == L"Application");
    DESTO_CHECK(applicationView.showTitle);
    DESTO_CHECK(applicationView.showCollapseControl);
    DESTO_CHECK(applicationView.showCloseControl);
    DESTO_CHECK(applicationView.showPinControl);
    DESTO_CHECK(applicationView.opacity == 1.0);

    auto chrome = application.chrome();
    chrome.showTitle = false;
    chrome.showCollapseControl = false;
    chrome.showCloseControl = false;
    chrome.showPinControl = false;
    application.setChrome(chrome);
    application.setExpanded(false);
    application.setAppearance({"dark", 0.75, 28.0});
    applicationView = MakeCardView(application);
    DESTO_CHECK(!applicationView.showTitle);
    DESTO_CHECK(!applicationView.showCollapseControl);
    DESTO_CHECK(!applicationView.showCloseControl);
    DESTO_CHECK(!applicationView.showPinControl);
    DESTO_CHECK(!applicationView.expanded);
    DESTO_CHECK(applicationView.appearancePreset == "dark");
    DESTO_CHECK(applicationView.opacity == 0.75);
    DESTO_CHECK(applicationView.cornerRadius == 28.0);
}

} // namespace

int main() {
    return desto::test::Run(RunTests);
}
