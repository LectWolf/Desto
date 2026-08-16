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
    CardView result{
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
    if (card.type() == domain::CardType::Application) {
        const auto& application = static_cast<const domain::ApplicationCard&>(card);
        result.applicationSortMode = application.sortMode();
        result.applicationItemPlacements = application.itemPlacements();
    } else if (card.type() == domain::CardType::Mapping) {
        const auto& mapping = static_cast<const domain::MappingCard&>(card);
        result.mappingMode = mapping.mode();
        result.mappingAllowsSourceMutation = mapping.allowsSourceMutation();
    } else if (card.type() == domain::CardType::Todo) {
        const auto& todo = static_cast<const domain::TodoCard&>(card);
        result.todoPreferences = todo.preferences();
        result.todoItems = todo.items();
    }
    return result;
}

} // namespace desto::presentation
