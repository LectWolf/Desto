#include "PlacementInteraction.h"
#include "TestSupport.h"

#include <vector>

using namespace desto::domain;
using namespace desto::presentation;

namespace {

void RunTests() {
    auto rect = ResolvePlacementInteraction({7, 9, 300, 200}, 1920, 1040, {}, false);
    DESTO_CHECK(rect.left == 0);
    DESTO_CHECK(rect.top == 0);

    rect = ResolvePlacementInteraction({807, 419, 300, 200}, 1920, 1040, {}, false);
    DESTO_CHECK(rect.left == 810);
    DESTO_CHECK(rect.top == 420);

    const std::vector<PlacementRect> others{{500, 300, 240, 180}};
    rect = ResolvePlacementInteraction({193, 302, 300, 200}, 1920, 1040, others, false);
    DESTO_CHECK(rect.left == 192);
    DESTO_CHECK(rect.top == 300);

    rect = ResolvePlacementInteraction({7, 9, 300, 200}, 1920, 1040, {}, true);
    DESTO_CHECK(rect.left == 7);
    DESTO_CHECK(rect.top == 9);

    rect = ResolvePlacementInteraction({1900, 1000, 20, 20}, 1920, 1040, {}, true);
    DESTO_CHECK(rect.width == 160);
    DESTO_CHECK(rect.height == 80);
    DESTO_CHECK(rect.left == 1760);
    DESTO_CHECK(rect.top == 960);

    const auto detailed = ResolvePlacementInteractionDetailed(
        {193, 302, 300, 200}, 1920, 1040, others, false);
    DESTO_CHECK(detailed.verticalGuide == 496);
    DESTO_CHECK(detailed.horizontalGuide == 300);
    const auto spaced = ResolvePlacementInteractionDetailed(
        {749, 488, 240, 180}, 1920, 1040, others, false);
    DESTO_CHECK(spaced.rect.left == 748);
    DESTO_CHECK(spaced.rect.top == 488);
    DESTO_CHECK(spaced.verticalGuide == 744);
    DESTO_CHECK(spaced.horizontalGuide == 484);
    const auto bypassed = ResolvePlacementInteractionDetailed(
        {193, 302, 300, 200}, 1920, 1040, others, true);
    DESTO_CHECK(!bypassed.verticalGuide.has_value());
    DESTO_CHECK(!bypassed.horizontalGuide.has_value());
}

} // namespace

int main() {
    return desto::test::Run(RunTests);
}
