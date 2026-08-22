#include "CardView.h"

#include <codecvt>
#include <locale>

namespace desto::presentation {
namespace {

std::wstring TypeLabel(domain::CardType type, bool english) {
    switch (type) {
    case domain::CardType::Application:
        return english ? L"Applications" : L"应用";
    case domain::CardType::Mapping:
        return english ? L"Mapping" : L"映射";
    case domain::CardType::Todo:
        return english ? L"Tasks" : L"待办";
    }
    return english ? L"Card" : L"卡片";
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    try {
        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
        return converter.from_bytes(value);
    } catch (...) {
        return {};
    }
}

} // namespace

CardView MakeCardView(const domain::Card& card, std::string_view resolvedLanguage) {
    const auto& chrome = card.chrome();
    const auto& appearance = card.appearance();
    const auto english = resolvedLanguage == "en-US";
    CardView result{
        .id = card.id(),
        .type = card.type(),
        .title = card.name().empty()
            ? TypeLabel(card.type(), english) : Utf8ToWide(card.name()),
        .typeLabel = TypeLabel(card.type(), english),
        .visible = card.isVisible(),
        .expanded = card.isExpanded(),
        .showTitle = chrome.showTitle,
        .showCollapseControl = chrome.showCollapseControl,
        .showCloseControl = chrome.showCloseControl,
        .showPinControl = chrome.showPinControl,
        .showPresentationControl = chrome.showPresentationControl,
        .pinOnTop = chrome.pinOnTop,
        .positionLocked = chrome.positionLocked,
        .appearancePreset = appearance.preset,
        .opacity = appearance.opacity,
        .cornerRadius = appearance.cornerRadius,
        .content = card.content(),
    };
    if (card.type() == domain::CardType::Application) {
        const auto& application = static_cast<const domain::ApplicationCard&>(card);
        result.applicationSortMode = application.sortMode();
        result.applicationItemPlacements = application.itemPlacements();
        result.mappingPresentationMode = application.presentationMode();
    } else if (card.type() == domain::CardType::Mapping) {
        const auto& mapping = static_cast<const domain::MappingCard&>(card);
        result.mappingMode = mapping.mode();
        result.mappingPresentationMode = mapping.presentationMode();
        result.mappingSortMode = mapping.sortMode();
        result.applicationSortMode = mapping.sortMode();
        result.applicationItemPlacements = mapping.itemPlacements();
        result.mappingHasSource = !mapping.sourceRoot().empty()
            || !mapping.references().empty();
        result.mappingAllowsSourceMutation = mapping.allowsSourceMutation();
    } else if (card.type() == domain::CardType::Todo) {
        const auto& todo = static_cast<const domain::TodoCard&>(card);
        result.todoPreferences = todo.preferences();
        result.todoItems = todo.items();
    }
    return result;
}

} // namespace desto::presentation
