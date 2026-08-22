#include "PlacementInteraction.h"
#include "TestSupport.h"

#include <vector>

using namespace desto::domain;
using namespace desto::presentation;

namespace {

void RunTests() {
    const PlacementRect expansionBase{100.0, 60.0, 244.0, 120.0};
    const auto rightExpansion = ResolveAdaptiveDropExpansionRect(
        expansionBase, 299.0, 174.0, PlacementHorizontalAnchor::Right);
    DESTO_CHECK(rightExpansion.left == 45.0);
    DESTO_CHECK(rightExpansion.top == 60.0);
    DESTO_CHECK(rightExpansion.width == 299.0);
    DESTO_CHECK(rightExpansion.height == 174.0);
    DESTO_CHECK(rightExpansion.left + rightExpansion.width
                == expansionBase.left + expansionBase.width);
    const auto rightShrink = ResolveAdaptiveDropExpansionRect(
        rightExpansion, expansionBase.width, expansionBase.height,
        PlacementHorizontalAnchor::Right);
    DESTO_CHECK(rightShrink.left == expansionBase.left);
    DESTO_CHECK(rightShrink.top == expansionBase.top);
    DESTO_CHECK(rightShrink.width == expansionBase.width);
    DESTO_CHECK(rightShrink.left + rightShrink.width
                == rightExpansion.left + rightExpansion.width);
    for (const auto anchor : {
             PlacementHorizontalAnchor::Free,
             PlacementHorizontalAnchor::Left,
             PlacementHorizontalAnchor::Center}) {
        const auto expansion = ResolveAdaptiveDropExpansionRect(
            expansionBase, 299.0, 174.0, anchor);
        DESTO_CHECK(expansion.left == expansionBase.left);
        DESTO_CHECK(expansion.top == expansionBase.top);
        DESTO_CHECK(expansion.width == 299.0);
        DESTO_CHECK(expansion.height == 174.0);
    }

    auto rect = ResolvePlacementInteraction({7, 9, 300, 200}, 1920, 1040, {}, false);
    DESTO_CHECK(rect.left == 8);
    DESTO_CHECK(rect.top == 8);

    rect = ResolvePlacementInteraction({807, 419, 300, 200}, 1920, 1040, {}, false);
    DESTO_CHECK(rect.left == 810);
    DESTO_CHECK(rect.top == 420);

    const std::vector<PlacementRect> others{{500, 300, 240, 180}};
    rect = ResolvePlacementInteraction({193, 302, 300, 200}, 1920, 1040, others, false);
    DESTO_CHECK(rect.left == 192);
    DESTO_CHECK(rect.top == 300);

    rect = ResolvePlacementInteraction({7, 9, 300, 200}, 1920, 1040, {}, true);
    DESTO_CHECK(rect.left == 8);
    DESTO_CHECK(rect.top == 9);

    rect = ResolvePlacementInteraction({1900, 1000, 20, 20}, 1920, 1040, {}, true);
    DESTO_CHECK(rect.width == 160);
    DESTO_CHECK(rect.height == 80);
    DESTO_CHECK(rect.left == 1752);
    DESTO_CHECK(rect.top == 952);

    rect = ResolvePlacementInteraction({1620, 840, 300, 200}, 1920, 1040, {}, false);
    DESTO_CHECK(rect.left == 1612);
    DESTO_CHECK(rect.top == 832);

    const auto detailed = ResolvePlacementInteractionDetailed(
        {193, 302, 300, 200}, 1920, 1040, others, false);
    DESTO_CHECK(detailed.verticalGuide == 496);
    DESTO_CHECK(detailed.horizontalGuide == 300);
    DESTO_CHECK(detailed.horizontalAnchor == PlacementHorizontalAnchor::Left);
    const auto ordinary = ResolvePlacementInteractionDetailed(
        {300, 100, 300, 200}, 1920, 1040, {}, false);
    DESTO_CHECK(ordinary.horizontalAnchor == PlacementHorizontalAnchor::Left);
    const auto centered = ResolvePlacementInteractionDetailed(
        {807, 100, 300, 200}, 1920, 1040, {}, false);
    DESTO_CHECK(centered.horizontalAnchor == PlacementHorizontalAnchor::Left);
    const auto screenRight = ResolvePlacementInteractionDetailed(
        {1611, 100, 300, 200}, 1920, 1040, {}, false);
    DESTO_CHECK(screenRight.horizontalAnchor == PlacementHorizontalAnchor::Right);
    const auto cardRight = ResolvePlacementInteractionDetailed(
        {440, 100, 300, 200}, 1920, 1040, others, false);
    DESTO_CHECK(cardRight.horizontalAnchor == PlacementHorizontalAnchor::Right);
    const auto spaced = ResolvePlacementInteractionDetailed(
        {749, 488, 240, 180}, 1920, 1040, others, false);
    DESTO_CHECK(spaced.rect.left == 748);
    DESTO_CHECK(spaced.rect.top == 488);
    DESTO_CHECK(spaced.verticalGuide == 744);
    DESTO_CHECK(spaced.horizontalGuide == 484);
    const auto formerlyFlush = ResolvePlacementInteractionDetailed(
        {200, 300, 300, 180}, 1920, 1040, others, false);
    DESTO_CHECK(formerlyFlush.rect.left == 192);
    DESTO_CHECK(formerlyFlush.rect.top == 300);
    const auto bypassed = ResolvePlacementInteractionDetailed(
        {193, 302, 300, 200}, 1920, 1040, others, true);
    DESTO_CHECK(!bypassed.verticalGuide.has_value());
    DESTO_CHECK(!bypassed.horizontalGuide.has_value());
    DESTO_CHECK(bypassed.horizontalAnchor == PlacementHorizontalAnchor::Left);
}

} // namespace

int main() {
    return desto::test::Run(RunTests);
}
