#include "CardContentLayout.h"
#include "TestSupport.h"

#include <stdexcept>

using namespace desto::presentation;

namespace {

void RunTests() {
    const auto empty = ResolveCardContentLayout(0, 320.0);
    DESTO_CHECK(empty.columns == 4);
    DESTO_CHECK(empty.rows == 0);
    DESTO_CHECK(empty.idealHeight == 48.0);

    const auto compact = ResolveCardContentLayout(7, 320.0);
    DESTO_CHECK(compact.columns == 4);
    DESTO_CHECK(compact.rows == 2);
    DESTO_CHECK(compact.contentWidth == 280.0);
    DESTO_CHECK(compact.contentHeight == 160.0);
    DESTO_CHECK(compact.idealHeight == 232.0);

    const auto narrow = ResolveCardContentLayout(3, 150.0);
    DESTO_CHECK(narrow.columns == 1);
    DESTO_CHECK(narrow.rows == 3);

    const auto capped = ResolveCardContentLayout(20, 2000.0);
    DESTO_CHECK(capped.columns == 8);
    DESTO_CHECK(capped.rows == 3);

    bool rejected = false;
    try {
        (void)ResolveCardContentLayout(1, 0.0);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    DESTO_CHECK(rejected);
}

} // namespace

int main() {
    return desto::test::Run(RunTests);
}
