#pragma once

#include <string>

#include "Card.h"

namespace desto::presentation {

struct CardView {
    domain::CardId id;
    domain::CardType type = domain::CardType::Todo;
    std::wstring title;
    std::wstring typeLabel;
    bool visible = true;
    bool expanded = true;
    bool showTitle = true;
    bool showCollapseControl = true;
    bool showCloseControl = true;
    bool showPinControl = true;
    std::string appearancePreset = "default";
    double opacity = 1.0;
    double cornerRadius = 16.0;
};

[[nodiscard]] CardView MakeCardView(const domain::Card& card);

} // namespace desto::presentation
