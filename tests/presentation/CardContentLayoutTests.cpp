#include "CardContentLayout.h"
#include "TestSupport.h"

#include <stdexcept>

using namespace desto::presentation;
using namespace desto::domain;

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

    const auto small = ResolveCardContentLayoutSettings({
        .itemSize = CardItemSize::Small,
        .showItemNames = false,
    });
    const auto extraLarge = ResolveCardContentLayoutSettings({
        .itemSize = CardItemSize::ExtraLarge,
        .showItemNames = true,
    });
    DESTO_CHECK(small.iconSize == 28.0);
    DESTO_CHECK(small.itemHeight == 40.0);
    DESTO_CHECK(extraLarge.iconSize == 68.0);
    DESTO_CHECK(extraLarge.itemHeight == 100.0);

    DESTO_CHECK(ResolveCardInsertionIndex(7, 320.0, 36.0, 64.0) == 0);
    DESTO_CHECK(ResolveCardInsertionIndex(7, 320.0, 110.0, 64.0) == 1);
    DESTO_CHECK(ResolveCardInsertionIndex(7, 320.0, 1000.0, 1000.0) == 7);

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
