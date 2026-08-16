#include "CardView.h"

namespace desto::presentation {
namespace {

std::wstring TypeLabel(domain::CardType type) {
    switch (type) {
    case domain::CardType::Application:
        return L"Application";
    case domain::CardType::Mapping:
        return L"Mapping";
    case domain::CardType::Todo:
        return L"Todo";
    }
    return L"Card";
}

} // namespace

CardView MakeCardView(const domain::Card& card) {
    const auto& chrome = card.chrome();
    const auto& appearance = card.appearance();
    return {
        .id = card.id(),
        .type = card.type(),
        .title = TypeLabel(card.type()),
        .typeLabel = TypeLabel(card.type()),
        .visible = card.isVisible(),
        .expanded = card.isExpanded(),
        .showTitle = chrome.showTitle,
        .showCollapseControl = chrome.showCollapseControl,
        .showCloseControl = chrome.showCloseControl,
        .showPinControl = chrome.showPinControl,
        .appearancePreset = appearance.preset,
        .opacity = appearance.opacity,
        .cornerRadius = appearance.cornerRadius,
        .content = card.content(),
    };
}

} // namespace desto::presentation
