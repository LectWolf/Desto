#include "CardContentLayout.h"
#include "PremultipliedImageResampler.h"
#include "RoundedDashGeometry.h"
#include "TestSupport.h"

#include <stdexcept>
#include <array>

using namespace desto::presentation;
using namespace desto::domain;

namespace {

void RunTests() {
    DESTO_CHECK(ResolveCardOuterWidth(3) == 181.0);
    DESTO_CHECK(ResolveCardOuterWidth(4) == 244.0);
    DESTO_CHECK(ResolveCardOuterWidth(6)
                == ResolveCardOuterWidth(3) * 2.0 + CardWidthGapDip);
    DESTO_CHECK(ResolveCardOuterWidth(8)
                == ResolveCardOuterWidth(4) * 2.0 + CardWidthGapDip);
    constexpr std::array itemSizes{
        CardItemSize::Small,
        CardItemSize::Medium,
        CardItemSize::Large,
        CardItemSize::ExtraLarge,
    };
    constexpr std::array expectedColumns{
        std::array<std::size_t, 4>{5, 4, 3, 2},
        std::array<std::size_t, 4>{6, 5, 4, 3},
        std::array<std::size_t, 4>{8, 6, 5, 4},
        std::array<std::size_t, 4>{9, 8, 6, 5},
    };
    for (std::uint32_t span = 3; span <= 6; ++span) {
        for (std::size_t size = 0; size < itemSizes.size(); ++size) {
            DESTO_CHECK(ResolveCardColumnsForWidthSpan(span, itemSizes[size])
                        == expectedColumns[span - 3][size]);
        }
    }
    DESTO_CHECK(ResolveLegacyCardWidthSpan(6, CardItemSize::Small) == 4);
    DESTO_CHECK(ResolveLegacyCardWidthSpan(5, CardItemSize::Medium) == 4);
    DESTO_CHECK(ResolveLegacyCardWidthSpan(4, CardItemSize::Large) == 4);
    DESTO_CHECK(ResolveLegacyCardWidthSpan(3, CardItemSize::ExtraLarge) == 4);
    DESTO_CHECK(ResolveLegacyCardWidthSpan(5, CardItemSize::Large) == 5);
    DESTO_CHECK(ResolveCardWidthSpanForColumns(7, CardItemSize::Medium) == 6);

    const auto empty = ResolveCardContentLayout(0, 320.0);
    DESTO_CHECK(empty.columns == 5);
    DESTO_CHECK(empty.rows == 0);
    DESTO_CHECK(empty.idealHeight == 48.0);

    const auto compact = ResolveCardContentLayout(7, 320.0);
    DESTO_CHECK(compact.columns == 5);
    DESTO_CHECK(compact.rows == 2);
    DESTO_CHECK(compact.contentWidth == 275.0);
    DESTO_CHECK(compact.contentHeight == 110.0);
    DESTO_CHECK(compact.idealHeight == 182.0);

    const auto twoLargeRows = ResolveCardContentLayout(
        8,
        ResolveCardOuterWidth(4),
        ResolveCardContentLayoutSettings({}));
    DESTO_CHECK(twoLargeRows.columns == 4);
    DESTO_CHECK(twoLargeRows.rows == 2);
    DESTO_CHECK(twoLargeRows.idealHeight == 174.0);

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
    const auto smallWithName = ResolveCardContentLayoutSettings({
        .itemSize = CardItemSize::Small,
        .showItemNames = true,
    });
    const auto mediumWithName = ResolveCardContentLayoutSettings({
        .itemSize = CardItemSize::Medium,
        .showItemNames = true,
    });
    const auto largeWithName = ResolveCardContentLayoutSettings({
        .itemSize = CardItemSize::Large,
        .showItemNames = true,
    });
    DESTO_CHECK(small.iconSize == 20.0);
    DESTO_CHECK(small.itemFontSize == 9.0);
    DESTO_CHECK(small.itemWidth == 37.0);
    DESTO_CHECK(small.itemWidth == small.itemHeight);
    DESTO_CHECK(small.preferredColumns == 6);
    DESTO_CHECK(small.minimumColumns == 3);
    DESTO_CHECK(small.horizontalGap == 0.0);
    DESTO_CHECK(small.verticalGap == 0.0);
    DESTO_CHECK(smallWithName.itemHeight == 49.0);
    DESTO_CHECK(medium.iconSize == 26.0);
    DESTO_CHECK(medium.itemFontSize == 10.0);
    DESTO_CHECK(medium.itemWidth == 44.0);
    DESTO_CHECK(medium.itemWidth == medium.itemHeight);
    DESTO_CHECK(medium.preferredColumns == 5);
    DESTO_CHECK(medium.minimumColumns == 3);
    DESTO_CHECK(mediumWithName.itemHeight == 58.0);
    DESTO_CHECK(large.iconSize == 34.0);
    DESTO_CHECK(large.itemFontSize == 10.0);
    DESTO_CHECK(large.itemWidth == 55.0);
    DESTO_CHECK(large.verticalPadding == 8.0);
    DESTO_CHECK(large.preferredColumns == 4);
    DESTO_CHECK(large.minimumColumns == 2);
    DESTO_CHECK(largeWithName.itemHeight == 71.0);
    DESTO_CHECK(extraLarge.iconSize == 44.0);
    DESTO_CHECK(extraLarge.itemFontSize == 11.0);
    DESTO_CHECK(extraLarge.itemWidth == 74.0);
    DESTO_CHECK(extraLarge.itemHeight == extraLarge.itemWidth + 32.0);
    DESTO_CHECK(extraLarge.preferredColumns == 3);
    DESTO_CHECK(extraLarge.minimumColumns == 2);
    DESTO_CHECK(ResolveFileCardOuterWidth(6, small) == 246.0);
    DESTO_CHECK(ResolveFileCardOuterWidth(8, small) == 320.0);
    DESTO_CHECK(ResolveFileCardOuterWidth(5, medium) == 244.0);
    DESTO_CHECK(ResolveFileCardOuterWidth(6, medium) == 288.0);
    DESTO_CHECK(ResolveFileCardOuterWidth(4, large) == 244.0);
    DESTO_CHECK(ResolveFileCardOuterWidth(5, large) == 299.0);
    DESTO_CHECK(ResolveFileCardOuterWidth(3, extraLarge) == 246.0);
    DESTO_CHECK(ResolveFileCardOuterWidth(4, extraLarge) == 320.0);

    DESTO_CHECK(ResolveAdaptiveCardColumns(4, 1, large) == 4);
    DESTO_CHECK(ResolveAdaptiveCardColumns(5, 1, large) == 5);
    DESTO_CHECK(ResolveAdaptiveCardColumns(4, 1, large) == 4);
    DESTO_CHECK(ResolveAdaptiveCardColumns(2, 6, large) == 6);
    DESTO_CHECK(ResolveAdaptiveCardColumns(2, 9, large) == 9);
    DESTO_CHECK(ResolveAdaptiveCardColumns(20, 1, large) == 8);
    DESTO_CHECK(ResolveSortedAdaptiveCardColumns(0, large) == 4);
    DESTO_CHECK(ResolveSortedAdaptiveCardColumns(5, large) == 5);
    DESTO_CHECK(ResolveSortedAdaptiveCardColumns(2, extraLarge) == 2);
    DESTO_CHECK(ResolveCustomAdaptiveCardColumns(0, CardItemSize::Large, large) == 4);
    DESTO_CHECK(ResolveCustomAdaptiveCardColumns(4, CardItemSize::Large, large) == 4);
    DESTO_CHECK(ResolveCustomAdaptiveCardColumns(5, CardItemSize::Large, large) == 5);
    DESTO_CHECK(ResolveCustomAdaptiveCardColumns(0, CardItemSize::Small, large) == 6);
    auto historicalFiveSpan = large;
    historicalFiveSpan.preferredColumns = 5;
    DESTO_CHECK(ResolveCustomAdaptiveCardColumns(
        4, CardItemSize::Large, historicalFiveSpan) == 4);
    DESTO_CHECK(ResolveScrolledCardPointerY(76.0, 0, large) == 76.0);
    DESTO_CHECK(ResolveScrolledCardPointerY(76.0, 2, large) == 186.0);
    DESTO_CHECK(ResolveCardSlotIndex(
        244.0,
        34.0,
        ResolveScrolledCardPointerY(76.0, 2, large),
        large,
        4) == 8);

    constexpr std::array<std::uint32_t, 4> image{
        0xFFFF0000u,
        0x00000000u,
        0xFF00FF00u,
        0xFF0000FFu,
    };
    DESTO_CHECK(SamplePremultipliedBilinear(image, 2, 2, 3, 3, 0, 0) == image[0]);
    DESTO_CHECK(SamplePremultipliedBilinear(image, 2, 2, 3, 3, 1, 1) == 0xBF404040u);

    // Transparent RGB left by a shell bitmap must not become a visible halo
    // when the icon is resampled.
    constexpr std::array<std::uint32_t, 2> transparentEdge{
        0x00FFFFFFu,
        0xFFFF0000u,
    };
    DESTO_CHECK(SamplePremultipliedBilinear(
                    transparentEdge, 2, 1, 2, 1, 0, 0)
                == 0x00000000u);
    DESTO_CHECK(SamplePremultipliedBilinear(
                    transparentEdge, 2, 1, 2, 1, 1, 0)
                == 0xFFFF0000u);
    DESTO_CHECK(SamplePremultipliedBilinear(
                    transparentEdge, 2, 1, 3, 1, 1, 0)
                == 0x80800000u);

    const RoundedDashSpec dashSpec{
        .width = 160.0,
        .height = 80.0,
        .radius = 12.0,
        .strokeWidth = 2.5,
        .nominalPeriod = 9.0,
    };
    DESTO_CHECK(SampleRoundedDashCoverage(dashSpec, 80.0, 1.25) > 0.9);
    bool topHasDash = false;
    bool topHasGap = false;
    for (int x = 16; x < 144; ++x) {
        const auto coverage = SampleRoundedDashCoverage(dashSpec, x + 0.5, 1.25);
        topHasDash = topHasDash || coverage > 0.8;
        topHasGap = topHasGap || coverage < 0.1;
        DESTO_CHECK(std::abs(
            coverage - SampleRoundedDashCoverage(dashSpec, 159.5 - x, 1.25)) < 0.0001);
    }
    DESTO_CHECK(topHasDash);
    DESTO_CHECK(topHasGap);
    for (const auto size : {48.0, 49.0}) {
        const RoundedRectSpec outline{
            .width = size,
            .height = size,
            .radius = 10.0,
            .strokeWidth = 1.0,
        };
        const auto middle = size / 2.0;
        const auto top = SampleInnerRoundedOutlineCoverage(outline, middle, 0.5);
        const auto bottom = SampleInnerRoundedOutlineCoverage(
            outline, middle, size - 0.5);
        const auto left = SampleInnerRoundedOutlineCoverage(outline, 0.5, middle);
        const auto right = SampleInnerRoundedOutlineCoverage(
            outline, size - 0.5, middle);
        DESTO_CHECK(top > 0.99);
        DESTO_CHECK(std::abs(top - bottom) < 0.0001);
        DESTO_CHECK(std::abs(top - left) < 0.0001);
        DESTO_CHECK(std::abs(top - right) < 0.0001);
        for (int offset = 0; offset < 12; ++offset) {
            const auto coordinate = offset + 0.5;
            DESTO_CHECK(std::abs(
                SampleInnerRoundedOutlineCoverage(outline, coordinate, 0.5)
                - SampleInnerRoundedOutlineCoverage(
                    outline, size - coordinate, 0.5)) < 0.0001);
            DESTO_CHECK(std::abs(
                SampleInnerRoundedOutlineCoverage(outline, 0.5, coordinate)
                - SampleInnerRoundedOutlineCoverage(
                    outline, 0.5, size - coordinate)) < 0.0001);
        }
    }
    DESTO_CHECK(!IsAdaptiveDropExpansionReady(0));
    DESTO_CHECK(!IsAdaptiveDropExpansionReady(199));
    DESTO_CHECK(IsAdaptiveDropExpansionReady(200));
    DESTO_CHECK(ResolveAdaptiveDropExpansionDelay(0) == 200);
    DESTO_CHECK(ResolveAdaptiveDropExpansionDelay(1) == 200);
    DESTO_CHECK(ResolveAdaptiveDropExpansionDelay(1, true) == 500);
    DESTO_CHECK(ResolveAdaptiveDropExpansionDelay(2, true) == 800);
    DESTO_CHECK(!IsAdaptiveDropExpansionReady(499, 1, true));
    DESTO_CHECK(IsAdaptiveDropExpansionReady(500, 1, true));
    DESTO_CHECK(ResolveAdaptiveDropExpansionDelay(9, true) == 2900);

    DESTO_CHECK(ResolveCardInsertionIndex(7, 320.0, 36.0, 64.0) == 0);
    DESTO_CHECK(ResolveCardInsertionIndex(7, 320.0, 110.0, 64.0) == 1);
    DESTO_CHECK(ResolveCardInsertionIndex(4, 256.0, 250.0, 80.0) == 4);
    DESTO_CHECK(ResolveCardInsertionIndex(7, 320.0, 1000.0, 1000.0) == 7);
    const auto rightExpansion = ResolveAdaptiveCardDropPreview(
        4, 256.0, 250.0, 80.0, large);
    DESTO_CHECK(rightExpansion.insertionIndex == 4);
    DESTO_CHECK(rightExpansion.columns == 5);
    const auto delayedRightExpansion = ResolveAdaptiveCardDropPreview(
        4, 256.0, 250.0, 80.0, large, std::nullopt, false);
    DESTO_CHECK(delayedRightExpansion.columns == 4);
    DESTO_CHECK(delayedRightExpansion.insertionIndex == 3);
    const auto stationaryAfterFirstExpansion = ResolveAdaptiveCardDropPreview(
        4, 299.0, 242.0, 80.0, large, rightExpansion);
    DESTO_CHECK(stationaryAfterFirstExpansion.columns == 5);
    const auto secondHorizontalExpansion = ResolveAdaptiveCardDropPreview(
        4, 299.0, 297.0, 80.0, large, rightExpansion);
    DESTO_CHECK(secondHorizontalExpansion.columns == 6);
    const auto stationaryAfterSecondExpansion = ResolveAdaptiveCardDropPreview(
        4, 354.0, 297.0, 80.0, large, secondHorizontalExpansion);
    DESTO_CHECK(stationaryAfterSecondExpansion.columns == 6);
    const auto shrinkPreviousExpansion = ResolveAdaptiveCardDropPreview(
        4, 354.0, 190.0, 80.0, large, secondHorizontalExpansion);
    DESTO_CHECK(shrinkPreviousExpansion.columns == 5);
    const auto downwardExpansion = ResolveAdaptiveCardDropPreview(
        4, 256.0, 100.0, 120.0, large);
    DESTO_CHECK(downwardExpansion.insertionIndex == 5);
    DESTO_CHECK(downwardExpansion.columns == 4);
    const auto shrinkInside = ResolveAdaptiveCardDropPreview(
        4, 315.0, 190.0, 80.0, large);
    DESTO_CHECK(shrinkInside.columns == 4);
    const auto sparseRightExpansion = ResolveAdaptiveCardDropPreview(
        2, 256.0, 230.0, 80.0, large);
    DESTO_CHECK(sparseRightExpansion.insertionIndex == 4);
    DESTO_CHECK(sparseRightExpansion.columns == 5);
    const auto secondRowExpansion = ResolveAdaptiveCardDropPreview(
        2, 256.0, 40.0, 110.0, large);
    DESTO_CHECK(secondRowExpansion.insertionIndex == 4);
    DESTO_CHECK(secondRowExpansion.columns == 4);
    const auto thirdRowExpansion = ResolveAdaptiveCardDropPreview(
        2,
        256.0,
        40.0,
        169.0,
        large,
        secondRowExpansion);
    DESTO_CHECK(thirdRowExpansion.insertionIndex == 8);
    DESTO_CHECK(thirdRowExpansion.columns == 4);
    DESTO_CHECK(ResolveCardSlotIndex(320.0, 190.0, 148.0).value() == 8);
    DESTO_CHECK(ResolveCardSlotIndex(320.0, 190.0, 400.0, {}, 2).value() == 8);
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
