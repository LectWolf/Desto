#include "CardContentLayout.h"
#include "PremultipliedImageResampler.h"
#include "TestSupport.h"

#include <stdexcept>
#include <array>

using namespace desto::presentation;
using namespace desto::domain;

namespace {

void RunTests() {
    const auto empty = ResolveCardContentLayout(0, 320.0);
    DESTO_CHECK(empty.columns == 5);
    DESTO_CHECK(empty.rows == 0);
    DESTO_CHECK(empty.idealHeight == 48.0);

    const auto compact = ResolveCardContentLayout(7, 320.0);
    DESTO_CHECK(compact.columns == 5);
    DESTO_CHECK(compact.rows == 2);
    DESTO_CHECK(compact.contentWidth == 291.0);
    DESTO_CHECK(compact.contentHeight == 114.0);
    DESTO_CHECK(compact.idealHeight == 186.0);

    const auto narrow = ResolveCardContentLayout(3, 150.0);
    DESTO_CHECK(narrow.columns == 2);
    DESTO_CHECK(narrow.rows == 2);

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
    const auto medium = ResolveCardContentLayoutSettings({
        .itemSize = CardItemSize::Medium,
        .showItemNames = false,
    });
    const auto large = ResolveCardContentLayoutSettings({
        .itemSize = CardItemSize::Large,
        .showItemNames = false,
    });
    DESTO_CHECK(small.iconSize == 20.0);
    DESTO_CHECK(small.itemWidth == 37.0);
    DESTO_CHECK(small.itemWidth == small.itemHeight);
    DESTO_CHECK(small.preferredColumns == 6);
    DESTO_CHECK(medium.iconSize == 26.0);
    DESTO_CHECK(medium.itemWidth == 44.0);
    DESTO_CHECK(medium.itemWidth == medium.itemHeight);
    DESTO_CHECK(medium.preferredColumns == 5);
    DESTO_CHECK(large.iconSize == 34.0);
    DESTO_CHECK(large.itemWidth == 55.0);
    DESTO_CHECK(large.preferredColumns == 4);
    DESTO_CHECK(extraLarge.iconSize == 44.0);
    DESTO_CHECK(extraLarge.itemWidth == 74.0);
    DESTO_CHECK(extraLarge.itemHeight == extraLarge.itemWidth + 24.0);
    DESTO_CHECK(extraLarge.preferredColumns == 3);
    DESTO_CHECK(ResolveAdaptiveCardColumns(4, 1, large) == 4);
    DESTO_CHECK(ResolveAdaptiveCardColumns(5, 1, large) == 5);
    DESTO_CHECK(ResolveAdaptiveCardColumns(4, 1, large) == 4);
    DESTO_CHECK(ResolveAdaptiveCardColumns(2, 6, large) == 6);
    DESTO_CHECK(ResolveAdaptiveCardColumns(2, 9, large) == 9);
    DESTO_CHECK(ResolveAdaptiveCardColumns(20, 1, large) == 8);

    constexpr std::array<std::uint32_t, 4> image{
        0xFFFF0000u,
        0x00000000u,
        0xFF00FF00u,
        0xFF0000FFu,
    };
    DESTO_CHECK(SamplePremultipliedBilinear(image, 2, 2, 3, 3, 0, 0) == image[0]);
    DESTO_CHECK(SamplePremultipliedBilinear(image, 2, 2, 3, 3, 1, 1) == 0xBF404040u);

    DESTO_CHECK(ResolveCardInsertionIndex(7, 320.0, 36.0, 64.0) == 0);
    DESTO_CHECK(ResolveCardInsertionIndex(7, 320.0, 110.0, 64.0) == 1);
    DESTO_CHECK(ResolveCardInsertionIndex(4, 256.0, 250.0, 80.0) == 4);
    DESTO_CHECK(ResolveCardInsertionIndex(7, 320.0, 1000.0, 1000.0) == 7);
    const auto rightExpansion = ResolveAdaptiveCardDropPreview(
        4, 256.0, 250.0, 80.0, large);
    DESTO_CHECK(rightExpansion.insertionIndex == 4);
    DESTO_CHECK(rightExpansion.columns == 5);
    const auto downwardExpansion = ResolveAdaptiveCardDropPreview(
        4, 256.0, 100.0, 120.0, large);
    DESTO_CHECK(downwardExpansion.insertionIndex == 4);
    DESTO_CHECK(downwardExpansion.columns == 4);
    const auto shrinkInside = ResolveAdaptiveCardDropPreview(
        4, 315.0, 190.0, 80.0, large);
    DESTO_CHECK(shrinkInside.columns == 4);
    DESTO_CHECK(ResolveCardSlotIndex(320.0, 190.0, 148.0).value() == 7);
    DESTO_CHECK(ResolveCardSlotIndex(320.0, 190.0, 400.0, {}, 2).value() == 7);
    DESTO_CHECK(ResolveCardSlotIndex(320.0, 0.0, 64.0).value() == 0);
    DESTO_CHECK(ResolveCardSlotIndex(320.0, 88.0, 64.0).value() == 1);
    DESTO_CHECK(ResolveCardSlotIndex(320.0, 319.0, 64.0).value() == 4);
    DESTO_CHECK(ResolveCardSlotIndex(320.0, 36.0, 140.0).value() == 5);
    DESTO_CHECK(ResolveCardSlotIndex(320.0, 36.0, 219.0, {}, 2).value() == 5);

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
