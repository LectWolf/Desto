#include "JsonConfigStore.h"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <format>
#include <regex>
#include <stdexcept>
#include <unordered_set>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace desto::storage {
namespace {

using Json = nlohmann::json;

class UnsupportedSchemaError final : public std::runtime_error {
public:
    explicit UnsupportedSchemaError(const std::string& message)
        : std::runtime_error(message) {
    }
};

std::string ToUtf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::filesystem::path FromUtf8(const std::string& value) {
    return std::filesystem::u8path(value);
}

domain::CardType ParseCardType(const std::string& value) {
    if (value == "application") {
        return domain::CardType::Application;
    }
    if (value == "mapping") {
        return domain::CardType::Mapping;
    }
    if (value == "todo") {
        return domain::CardType::Todo;
    }
    throw std::runtime_error("Configuration card type is invalid.");
}

domain::CardItemSize ParseCardItemSize(const std::string& value) {
    if (value == "small") {
        return domain::CardItemSize::Small;
    }
    if (value == "medium") {
        return domain::CardItemSize::Medium;
    }
    if (value == "large") {
        return domain::CardItemSize::Large;
    }
    if (value == "extraLarge") {
        return domain::CardItemSize::ExtraLarge;
    }
    throw std::runtime_error("Configuration card item size is invalid.");
}

domain::CardSizeMode ParseCardSizeMode(const std::string& value) {
    if (value == "adaptive") return domain::CardSizeMode::Adaptive;
    if (value == "fixed") return domain::CardSizeMode::Fixed;
    throw std::runtime_error("Configuration card size mode is invalid.");
}

domain::PlacementHorizontalAnchor ParseHorizontalAnchor(const std::string& value) {
    if (value == "free") return domain::PlacementHorizontalAnchor::Free;
    if (value == "left") return domain::PlacementHorizontalAnchor::Left;
    if (value == "center") return domain::PlacementHorizontalAnchor::Center;
    if (value == "right") return domain::PlacementHorizontalAnchor::Right;
    throw std::runtime_error("Configuration horizontal placement anchor is invalid.");
}

domain::PlacementVerticalAnchor ParseVerticalAnchor(const std::string& value) {
    if (value == "free") return domain::PlacementVerticalAnchor::Free;
    if (value == "top") return domain::PlacementVerticalAnchor::Top;
    if (value == "center") return domain::PlacementVerticalAnchor::Center;
    if (value == "bottom") return domain::PlacementVerticalAnchor::Bottom;
    throw std::runtime_error("Configuration vertical placement anchor is invalid.");
}

domain::ApplicationItemSortMode ParseApplicationItemSortMode(const std::string& value) {
    if (value == "custom") return domain::ApplicationItemSortMode::Custom;
    if (value == "name") return domain::ApplicationItemSortMode::Name;
    if (value == "size") return domain::ApplicationItemSortMode::Size;
    if (value == "itemType") return domain::ApplicationItemSortMode::ItemType;
    if (value == "modifiedDate") return domain::ApplicationItemSortMode::ModifiedDate;
    throw std::runtime_error("Configuration application sort mode is invalid.");
}

domain::MappingMode ParseMappingMode(const std::string& value) {
    if (value == "folder") return domain::MappingMode::Folder;
    if (value == "references") return domain::MappingMode::References;
    throw std::runtime_error("Configuration mapping source mode is invalid.");
}

domain::MappingPresentationMode ParseMappingPresentationMode(const std::string& value) {
    if (value == "grid") return domain::MappingPresentationMode::Grid;
    if (value == "list") return domain::MappingPresentationMode::List;
    throw std::runtime_error("Configuration mapping presentation mode is invalid.");
}

domain::TodoDate ParseTodoDate(const std::string& value) {
    std::smatch match;
    static const std::regex pattern(R"(^([0-9]{4})-([0-9]{2})-([0-9]{2})$)");
    if (!std::regex_match(value, match, pattern)) {
        throw std::runtime_error("Configuration Todo date is invalid.");
    }
    const domain::TodoDate date{
        static_cast<std::int32_t>(std::stoi(match[1].str())),
        static_cast<std::uint8_t>(std::stoi(match[2].str())),
        static_cast<std::uint8_t>(std::stoi(match[3].str())),
    };
    if (!domain::IsValidTodoDate(date)) {
        throw std::runtime_error("Configuration Todo date is invalid.");
    }
    return date;
}

Json ReadDocument(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        return Json::object();
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Unable to open configuration file.");
    }
    try {
        return Json::parse(input);
    } catch (const Json::parse_error& error) {
        throw std::runtime_error(std::format("Invalid configuration JSON: {}", error.what()));
    }
}

Json MigrateDocument(Json document) {
    if (!document.is_object()) {
        throw std::runtime_error("Configuration root must be a JSON object.");
    }

    const auto sourceVersion = document.value("schemaVersion", 1);
    if (sourceVersion > ApplicationConfig::CurrentSchemaVersion) {
        throw UnsupportedSchemaError("Configuration schema version is newer than this build.");
    }
    if (sourceVersion < 1) {
        throw std::runtime_error("Configuration schema version is invalid.");
    }

    auto version = sourceVersion;
    while (version < ApplicationConfig::CurrentSchemaVersion) {
        if (version == 1) {
            // Schema 2 makes the Card collection explicit; old files may omit it.
            if (!document.contains("cards")) {
                document["cards"] = Json::array();
            }
            document["schemaVersion"] = 2;
            version = 2;
            continue;
        }
        if (version == 2) {
            // Schema 3 adds optional per-Card content preferences and Application item order.
            document["schemaVersion"] = 3;
            version = 3;
            continue;
        }
        if (version == 3) {
            // Schema 4 preserves sparse custom slots separately from transient sorted projections.
            if (document.contains("cards") && document["cards"].is_array()) {
                for (auto& card : document["cards"]) {
                    if (!card.is_object() || card.value("type", std::string{}) != "application"
                        || !card.contains("application") || !card["application"].is_object()) {
                        continue;
                    }
                    auto& application = card["application"];
                    application["sortMode"] = "custom";
                    application["itemPlacements"] = Json::array();
                    if (application.contains("itemOrder") && application["itemOrder"].is_array()) {
                        std::uint32_t index = 0;
                        for (const auto& item : application["itemOrder"]) {
                            if (item.is_string()) {
                                application["itemPlacements"].push_back({
                                    {"fileName", item},
                                    {"column", index % 4},
                                    {"row", index / 4},
                                });
                                ++index;
                            }
                        }
                    }
                    application.erase("itemOrder");
                }
            }
            document["schemaVersion"] = 4;
            version = 4;
            continue;
        }
        if (version == 4) {
            // Schema 5 preserves display reflow intent and never projects offline displays elsewhere.
            if (document.contains("workspace") && document["workspace"].is_object()
                && document["workspace"].contains("placements")
                && document["workspace"]["placements"].is_array()) {
                for (auto& placement : document["workspace"]["placements"]) {
                    if (!placement.is_object()) continue;
                    placement["horizontalAnchor"] = "free";
                    placement["verticalAnchor"] = "free";
                    placement["referenceWorkArea"] = {{"width", 0}, {"height", 0}};
                }
            }
            document["schemaVersion"] = 5;
            version = 5;
            continue;
        }
        if (version == 5) {
            // Schema 6 adds optional Todo dates, creation timestamps and archive state.
            document["schemaVersion"] = 6;
            version = 6;
            continue;
        }
        if (version == 6) {
            // Schema 7 adds an optional user-facing Card name.
            if (document.contains("cards") && document["cards"].is_array()) {
                for (auto& card : document["cards"]) {
                    if (card.is_object() && !card.contains("name")) card["name"] = "";
                }
            }
            document["schemaVersion"] = 7;
            version = 7;
            continue;
        }
        if (version == 7) {
            // Schema 8 persists application-wide behavior independently of Card instances.
            double inheritedRadius = 16.0;
            if (document.contains("cards") && document["cards"].is_array()
                && !document["cards"].empty()) {
                const auto& firstCard = document["cards"].front();
                if (firstCard.is_object() && firstCard.contains("appearance")
                    && firstCard["appearance"].is_object()) {
                    inheritedRadius = firstCard["appearance"].value(
                        "cornerRadius", inheritedRadius);
                }
                for (auto& card : document["cards"]) {
                    if (!card.is_object() || !card.contains("appearance")
                        || !card["appearance"].is_object()) continue;
                    auto& appearance = card["appearance"];
                    const auto preset = appearance.value("preset", std::string{});
                    const auto oldOpacity = appearance.value("opacity", 1.0);
                    const auto replaceOldDefault = [&](double previous, double next) {
                        if (std::abs(oldOpacity - previous) < 0.0001) {
                            appearance["opacity"] = next;
                        }
                    };
                    if (preset == "mica-dark" || preset == "black" || preset == "dark") {
                        replaceOldDefault(0.85, 0.92);
                    } else if (preset == "mica-white" || preset == "white"
                               || preset == "default") {
                        replaceOldDefault(0.75, 0.88);
                    } else if (preset == "brand" || preset == "jewel"
                               || preset == "pearl-pink") {
                        replaceOldDefault(0.94, 0.97);
                    } else if (preset == "apple-glass-white") {
                        replaceOldDefault(0.42, 0.68);
                    } else if (preset == "apple-glass-black") {
                        replaceOldDefault(0.48, 0.72);
                    }
                }
            }
            document["settings"] = {
                {"language", "system"},
                {"timeZoneOffsetMinutes", nullptr},
                {"globalCardCornerRadius", inheritedRadius},
            };
            document["schemaVersion"] = 8;
            version = 8;
            continue;
        }
        if (version == 8) {
            // Schema 9 removes the two redundant glass presets. Existing
            // instances retain their light/dark intent using the closest
            // supported material.
            if (document.contains("cards") && document["cards"].is_array()) {
                for (auto& card : document["cards"]) {
                    if (!card.is_object() || !card.contains("appearance")
                        || !card["appearance"].is_object()) continue;
                    auto& appearance = card["appearance"];
                    const auto preset = appearance.value("preset", std::string{});
                    if (preset == "apple-glass-white") {
                        appearance["preset"] = "mica-white";
                        appearance["opacity"] = 0.88;
                    } else if (preset == "apple-glass-black") {
                        appearance["preset"] = "mica-dark";
                        appearance["opacity"] = 0.92;
                    }
                }
            }
            document["schemaVersion"] = 9;
            version = 9;
            continue;
        }
        if (version == 9) {
            // Completed legacy items that are still visible begin their
            // archive clock at migration, so an upgrade never hides them.
            const auto migratedAt = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (document.contains("cards") && document["cards"].is_array()) {
                for (auto& card : document["cards"]) {
                    if (!card.is_object() || card.value("type", std::string{}) != "todo"
                        || !card.contains("todo") || !card["todo"].is_object()
                        || !card["todo"].contains("items")
                        || !card["todo"]["items"].is_array()) continue;
                    for (auto& item : card["todo"]["items"]) {
                        if (!item.is_object()) continue;
                        const auto completed = item.value("completed", false);
                        const auto archived = item.value("archived", false);
                        item["completedAt"] = completed && !archived
                            ? Json(migratedAt) : Json(std::int64_t{0});
                    }
                }
            }
            document["schemaVersion"] = 10;
            version = 10;
            continue;
        }
        if (version == 10) {
            // Schema 11 persists an empty Mapping Card's source intent and
            // keeps source selection independent from grid/list presentation.
            if (document.contains("cards") && document["cards"].is_array()) {
                for (auto& card : document["cards"]) {
                    if (!card.is_object() || card.value("type", std::string{}) != "mapping"
                        || !card.contains("mapping") || !card["mapping"].is_object()) continue;
                    auto& mapping = card["mapping"];
                    const auto hasFolder = mapping.contains("sourceRoot")
                        && mapping["sourceRoot"].is_string()
                        && !mapping["sourceRoot"].get<std::string>().empty();
                    mapping["sourceMode"] = hasFolder ? "folder" : "references";
                    mapping["presentationMode"] = "grid";
                }
            }
            document["schemaVersion"] = 11;
            version = 11;
            continue;
        }
        if (version == 11) {
            // Schema 12 adds an optional per-Card visible row limit. Existing
            // cards remain unlimited until the user explicitly enables it.
            if (document.contains("cards") && document["cards"].is_array()) {
                for (auto& card : document["cards"]) {
                    if (!card.is_object()) continue;
                    if (!card.contains("content") || !card["content"].is_object()) {
                        card["content"] = Json::object();
                    }
                    card["content"]["maximumVisibleRows"] = nullptr;
                }
            }
            document["schemaVersion"] = 12;
            version = 12;
            continue;
        }
        if (version == 12) {
            // Schema 13 persists the user's startup intent. The actual Run
            // entry remains a Windows adapter concern and is reconciled on launch.
            if (!document.contains("settings") || !document["settings"].is_object()) {
                document["settings"] = Json::object();
            }
            document["settings"]["runAtStartup"] = false;
            document["schemaVersion"] = 13;
            version = 13;
            continue;
        }
        if (version == 13) {
            // Schema 14 separates desktop and taskbar gestures from startup
            // behavior and records the global fullscreen policy for pinned cards.
            if (!document.contains("settings") || !document["settings"].is_object()) {
                document["settings"] = Json::object();
            }
            document["settings"]["desktopDoubleClickAction"] = "all";
            document["settings"]["taskbarDoubleClickAction"] = "none";
            document["settings"]["pinnedCardsYieldToFullscreen"] = true;
            document["schemaVersion"] = 14;
            version = 14;
            continue;
        }
        if (version == 14) {
            // Schema 15 records whether a Desto-created show-desktop session
            // is restored when the user opens another application window.
            if (!document.contains("settings") || !document["settings"].is_object()) {
                document["settings"] = Json::object();
            }
            document["settings"]["restoreWindowsOnNewWindow"] = true;
            document["schemaVersion"] = 15;
            version = 15;
            continue;
        }
        if (version == 15) {
            // Schema 16 adds opaque, namespaced state for extension Cards.
            document["schemaVersion"] = 16;
            version = 16;
            continue;
        }
        if (version == 16) {
            // Schema 17 persists disabled extension packages independently
            // from their installed files and existing Card instances.
            if (!document.contains("settings") || !document["settings"].is_object()) {
                document["settings"] = Json::object();
            }
            document["settings"]["disabledExtensions"] = Json::array();
            document["schemaVersion"] = 17;
            version = 17;
            continue;
        }
        if (version == 17) {
            // Schema 18 adds the opt-in rounded background behind desktop
            // icons. The default remains off to preserve the existing card
            // appearance.
            if (!document.contains("settings") || !document["settings"].is_object()) {
                document["settings"] = Json::object();
            }
            document["settings"]["showIconBackgroundFrame"] = false;
            document["schemaVersion"] = 18;
            version = 18;
            continue;
        }
        if (version == 18) {
            // Schema 19 makes the density-independent width span authoritative.
            // File cards infer it from their legacy density columns; extension
            // cards already used 3/4/5 as their user-facing size projection.
            if (document.contains("cards") && document["cards"].is_array()) {
                for (auto& card : document["cards"]) {
                    if (!card.is_object()) continue;
                    if (!card.contains("content") || !card["content"].is_object()) {
                        card["content"] = Json::object();
                    }
                    auto& content = card["content"];
                    const auto legacyColumns = content.value("fixedColumns", 4u);
                    const auto type = card.value("type", std::string{});
                    if (type == "extension") {
                        content["widthSpan"] = std::clamp(legacyColumns, 2u, 64u);
                    } else {
                        const auto itemSize = ParseCardItemSize(
                            content.value("itemSize", std::string{"large"}));
                        content["widthSpan"] = domain::InferCardWidthSpan(
                            legacyColumns, itemSize);
                    }
                }
            }
            document["schemaVersion"] = 19;
            version = 19;
            continue;
        }
        if (version == 19) {
            // Schema 20 replaces the legacy black transparent material with a
            // white transparent material while preserving per-card opacity.
            if (document.contains("cards") && document["cards"].is_array()) {
                for (auto& card : document["cards"]) {
                    if (!card.is_object() || !card.contains("appearance")
                        || !card["appearance"].is_object()) continue;
                    auto& appearance = card["appearance"];
                    if (appearance.value("preset", std::string{}) == "transparent-black") {
                        appearance["preset"] = "transparent-white";
                    }
                }
            }
            document["schemaVersion"] = 20;
            version = 20;
            continue;
        }
        if (version == 20) {
            // Schema 21 persists the Settings card-rail order independently
            // from Card identity and desktop Placement order.
            if (!document.contains("settings") || !document["settings"].is_object()) {
                document["settings"] = Json::object();
            }
            auto order = Json::array();
            if (document.contains("cards") && document["cards"].is_array()) {
                for (const auto& card : document["cards"]) {
                    if (card.is_object() && card.contains("id")
                        && card["id"].is_string()) {
                        order.push_back(card["id"]);
                    }
                }
            }
            document["settings"]["cardOrder"] = std::move(order);
            document["schemaVersion"] = 21;
            version = 21;
            continue;
        }
        if (version == 21) {
            // Schema 22 persists sparse custom positions for Mapping Cards
            // and promotes grid/list presentation to every file Card.
            // Older Mapping Cards are reconciled into their current visual
            // order on first load, so migration starts with no stale paths.
            if (document.contains("cards") && document["cards"].is_array()) {
                for (auto& card : document["cards"]) {
                    if (!card.is_object() || card.value("type", std::string{}) != "mapping"
                        || !card.contains("mapping") || !card["mapping"].is_object()) {
                        if (card.is_object()
                            && card.value("type", std::string{}) == "application"
                            && card.contains("application")
                            && card["application"].is_object()) {
                            card["application"]["presentationMode"] = "grid";
                        }
                    } else {
                        card["mapping"]["itemPlacements"] = Json::array();
                    }
                }
            }
            document["schemaVersion"] = 22;
            version = 22;
            continue;
        }
        if (version == 22) {
            // Schema 23 removes the postponed extension system. Existing
            // extension Cards and their placements are discarded atomically;
            // all built-in Cards and unknown fields remain intact.
            std::unordered_set<std::string> removedCardIds;
            if (document.contains("cards") && document["cards"].is_array()) {
                auto retainedCards = Json::array();
                for (auto& card : document["cards"]) {
                    if (card.is_object()
                        && card.value("type", std::string{}) == "extension") {
                        if (card.contains("id") && card["id"].is_string()) {
                            removedCardIds.insert(card["id"].get<std::string>());
                        }
                        continue;
                    }
                    if (card.is_object() && card.contains("content")
                        && card["content"].is_object()) {
                        card["content"].erase("valueAnimation");
                    }
                    retainedCards.push_back(std::move(card));
                }
                document["cards"] = std::move(retainedCards);
            }
            if (document.contains("workspace") && document["workspace"].is_object()
                && document["workspace"].contains("placements")
                && document["workspace"]["placements"].is_array()) {
                auto retainedPlacements = Json::array();
                for (auto& placement : document["workspace"]["placements"]) {
                    const auto cardId = placement.is_object()
                            && placement.contains("cardId")
                            && placement["cardId"].is_string()
                        ? placement["cardId"].get<std::string>() : std::string{};
                    if (!removedCardIds.contains(cardId)) {
                        retainedPlacements.push_back(std::move(placement));
                    }
                }
                document["workspace"]["placements"] = std::move(retainedPlacements);
            }
            if (document.contains("settings") && document["settings"].is_object()) {
                auto& settings = document["settings"];
                settings.erase("disabledExtensions");
                if (settings.contains("cardOrder") && settings["cardOrder"].is_array()) {
                    auto retainedOrder = Json::array();
                    for (auto& cardId : settings["cardOrder"]) {
                        if (!cardId.is_string()
                            || !removedCardIds.contains(cardId.get<std::string>())) {
                            retainedOrder.push_back(std::move(cardId));
                        }
                    }
                    settings["cardOrder"] = std::move(retainedOrder);
                }
            }
            document["schemaVersion"] = 23;
            version = 23;
            continue;
        }
        if (version == 23) {
            // Schema 24 separates the visibility of the file presentation
            // button from the presentation mode itself. It also makes the
            // new default horizontal intent explicit and remaps Todo's
            // user-facing width presets from 3/4/5 to 4/5/6 large-icon slots.
            if (document.contains("cards") && document["cards"].is_array()) {
                for (auto& card : document["cards"]) {
                    if (!card.is_object()) continue;
                    auto& chrome = card["chrome"];
                    if (!chrome.is_object()) chrome = Json::object();
                    chrome["showPresentationControl"] =
                        chrome.value("showPresentationControl", true);
                    if (card.value("type", std::string{}) == "todo") {
                        auto& content = card["content"];
                        if (!content.is_object()) content = Json::object();
                        const auto oldSpan = content.value("widthSpan", 4u);
                        content["widthSpan"] = oldSpan >= 3u && oldSpan <= 5u
                            ? oldSpan + 1u : oldSpan;
                    }
                }
            }
            if (document.contains("workspace") && document["workspace"].is_object()
                && document["workspace"].contains("placements")
                && document["workspace"]["placements"].is_array()) {
                for (auto& placement : document["workspace"]["placements"]) {
                    if (!placement.is_object()) continue;
                    if (placement.value("horizontalAnchor", std::string{"left"})
                        != "right") {
                        placement["horizontalAnchor"] = "left";
                    }
                }
            }
            document["schemaVersion"] = 24;
            version = 24;
            continue;
        }
        if (version == 24) {
            // Schema 25 refines the built-in crystal preset. Only the exact
            // opacity written by schema 24's preset is migrated; any other
            // value is an instance-level choice and remains untouched.
            if (document.contains("cards") && document["cards"].is_array()) {
                for (auto& card : document["cards"]) {
                    if (!card.is_object() || !card.contains("appearance")
                        || !card["appearance"].is_object()) {
                        continue;
                    }
                    auto& appearance = card["appearance"];
                    if (appearance.value("preset", std::string{}) != "transparent-white"
                        || !appearance.contains("opacity")
                        || !appearance["opacity"].is_number()) {
                        continue;
                    }
                    if (std::abs(appearance["opacity"].get<double>() - 0.36) < 1.0e-9) {
                        appearance["opacity"] = 0.32;
                    }
                }
            }
            document["schemaVersion"] = 25;
            version = 25;
            continue;
        }
        if (version == 25) {
            // Schema 26 also recognizes the first transparent-white preset
            // value shipped before 0.36. The UI has no free opacity editor,
            // so this exact 0.62 value is another built-in preset revision;
            // unrelated instance values remain authoritative.
            if (document.contains("cards") && document["cards"].is_array()) {
                for (auto& card : document["cards"]) {
                    if (!card.is_object() || !card.contains("appearance")
                        || !card["appearance"].is_object()) {
                        continue;
                    }
                    auto& appearance = card["appearance"];
                    if (appearance.value("preset", std::string{}) != "transparent-white"
                        || !appearance.contains("opacity")
                        || !appearance["opacity"].is_number()) {
                        continue;
                    }
                    if (std::abs(appearance["opacity"].get<double>() - 0.62) < 1.0e-9) {
                        appearance["opacity"] = 0.32;
                    }
                }
            }
            document["schemaVersion"] = 26;
            version = 26;
            continue;
        }
        if (version == 26) {
            if (document.contains("cards") && document["cards"].is_array()) {
                for (auto& card : document["cards"]) {
                    if (!card.is_object()) continue;
                    if (!card.contains("chrome") || !card["chrome"].is_object()) {
                        card["chrome"] = Json::object();
                    }
                    card["chrome"]["positionLocked"] = false;
                }
            }
            document["schemaVersion"] = 27;
            version = 27;
            continue;
        }
        if (version == 27) {
            if (!document.contains("settings") || !document["settings"].is_object()) {
                document["settings"] = Json::object();
            }
            document["settings"]["updateChannel"] = "stable";
            document["schemaVersion"] = 28;
            version = 28;
            continue;
        }
        throw std::runtime_error("Configuration schema migration is unavailable.");
    }
    return document;
}

std::filesystem::path LegacyBackupPath(const std::filesystem::path& target) {
    return target.parent_path() / (target.filename().wstring() + L".bak");
}

std::filesystem::path BackupPath(const std::filesystem::path& target) {
    return LegacyBackupPath(target);
}

std::filesystem::path BackupPath(const std::filesystem::path& target, int generation) {
    return target.parent_path() / (target.filename().wstring()
        + L".bak" + std::to_wstring(generation));
}

std::filesystem::path ConfigPathForSource(
    const std::filesystem::path& target,
    ConfigSource source) {
    switch (source) {
    case ConfigSource::Primary:
        return target;
    case ConfigSource::Backup1:
        if (std::filesystem::exists(BackupPath(target, 1))) {
            return BackupPath(target, 1);
        }
        return LegacyBackupPath(target);
    case ConfigSource::Backup2:
        return BackupPath(target, 2);
    }
    return target;
}

std::string ReadRawFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Unable to open configuration file.");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void WriteAtomically(
    const std::filesystem::path& target,
    const std::string& content,
    bool preserveBackup) {
    std::filesystem::create_directories(target.parent_path());
    const auto temporary = target.parent_path()
        / (target.filename().wstring() + L".tmp-" + std::to_wstring(
            std::chrono::high_resolution_clock::now().time_since_epoch().count()));

    try {
        if (std::filesystem::exists(target) && !preserveBackup) {
            const auto backup1 = BackupPath(target, 1);
            const auto backup2 = BackupPath(target, 2);
            if (std::filesystem::exists(backup1)) {
                std::filesystem::copy_file(
                    backup1, backup2, std::filesystem::copy_options::overwrite_existing);
            } else if (std::filesystem::exists(LegacyBackupPath(target))) {
                std::filesystem::copy_file(
                    LegacyBackupPath(target), backup2,
                    std::filesystem::copy_options::overwrite_existing);
            }
            std::filesystem::copy_file(
                target, backup1, std::filesystem::copy_options::overwrite_existing);
            // Keep the v1 .bak name readable by older builds and tools.
            std::filesystem::copy_file(
                target, LegacyBackupPath(target),
                std::filesystem::copy_options::overwrite_existing);
        }
#ifdef _WIN32
        const auto handle = CreateFileW(
            temporary.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("Unable to create temporary configuration file.");
        }
        DWORD written = 0;
        const auto bytes = static_cast<DWORD>(content.size());
        const auto writeSucceeded = WriteFile(handle, content.data(), bytes, &written, nullptr);
        const auto flushSucceeded = writeSucceeded && written == bytes && FlushFileBuffers(handle);
        CloseHandle(handle);
        if (!flushSucceeded) {
            throw std::runtime_error("Unable to flush temporary configuration file.");
        }

        if (std::filesystem::exists(target)) {
            if (!ReplaceFileW(target.c_str(), temporary.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) {
                throw std::runtime_error("Unable to atomically replace configuration file.");
            }
        } else if (!MoveFileExW(
                       temporary.c_str(),
                       target.c_str(),
                       MOVEFILE_WRITE_THROUGH | MOVEFILE_REPLACE_EXISTING)) {
            throw std::runtime_error("Unable to publish configuration file.");
        }
#else
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("Unable to create temporary configuration file.");
        }
        output << content;
        output.flush();
        if (!output) {
            throw std::runtime_error("Unable to write temporary configuration file.");
        }
        output.close();
        std::filesystem::rename(temporary, target);
#endif
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

} // namespace

JsonConfigStore::JsonConfigStore(std::filesystem::path configPath)
    : configPath_(std::move(configPath)) {
    if (configPath_.empty() || !configPath_.is_absolute()) {
        throw std::invalid_argument("Configuration path must be an absolute path.");
    }
    configPath_ = configPath_.lexically_normal();
}

ApplicationConfig ParseDocument(const Json& document) {
    if (!document.is_object()) {
        throw std::runtime_error("Configuration root must be a JSON object.");
    }
    ApplicationConfig result;
    if (document.contains("schemaVersion")) {
        result.schemaVersion = document.at("schemaVersion").get<int>();
    }
    if (document.contains("storage")) {
        const auto& storage = document.at("storage");
        if (!storage.is_object() || !storage.contains("root") || !storage.at("root").is_string()) {
            throw std::runtime_error("Configuration storage.root must be a string.");
        }
        result.storageRoot = FromUtf8(storage.at("root").get<std::string>());
    }
    if (result.schemaVersion != ApplicationConfig::CurrentSchemaVersion) {
        throw std::runtime_error("Unsupported configuration schema version.");
    }
    if (!result.storageRoot.empty() && !result.storageRoot.is_absolute()) {
        throw std::runtime_error("Configuration storage root must be absolute.");
    }
    if (document.contains("settings")) {
        const auto& settings = document.at("settings");
        if (!settings.is_object()) {
            throw std::runtime_error("Configuration settings must be an object.");
        }
        result.preferences.language = settings.value("language", std::string{"system"});
        if (result.preferences.language != "system"
            && result.preferences.language != "zh-CN"
            && result.preferences.language != "en-US") {
            throw std::runtime_error("Configuration language is invalid.");
        }
        if (settings.contains("timeZoneOffsetMinutes")
            && !settings.at("timeZoneOffsetMinutes").is_null()) {
            if (!settings.at("timeZoneOffsetMinutes").is_number_integer()) {
                throw std::runtime_error("Configuration time zone offset is invalid.");
            }
            result.preferences.timeZoneOffsetMinutes =
                settings.at("timeZoneOffsetMinutes").get<std::int32_t>();
            if (*result.preferences.timeZoneOffsetMinutes < -12 * 60
                || *result.preferences.timeZoneOffsetMinutes > 14 * 60) {
                throw std::runtime_error("Configuration time zone offset is outside the valid range.");
            }
        }
        result.preferences.globalCardCornerRadius =
            settings.value("globalCardCornerRadius", 16.0);
        if (!std::isfinite(result.preferences.globalCardCornerRadius)
            || result.preferences.globalCardCornerRadius < 0.0
            || result.preferences.globalCardCornerRadius > 32.0) {
            throw std::runtime_error("Configuration global Card corner radius is invalid.");
        }
        result.preferences.runAtStartup = settings.value("runAtStartup", false);
        result.preferences.desktopDoubleClickAction =
            settings.value("desktopDoubleClickAction", std::string{"none"});
        if (result.preferences.desktopDoubleClickAction != "none"
            && result.preferences.desktopDoubleClickAction != "icons"
            && result.preferences.desktopDoubleClickAction != "cards"
            && result.preferences.desktopDoubleClickAction != "all") {
            throw std::runtime_error("Configuration desktop double-click action is invalid.");
        }
        result.preferences.taskbarDoubleClickAction =
            settings.value("taskbarDoubleClickAction", std::string{"none"});
        if (result.preferences.taskbarDoubleClickAction != "none"
            && result.preferences.taskbarDoubleClickAction != "all-displays"
            && result.preferences.taskbarDoubleClickAction != "current-display") {
            throw std::runtime_error("Configuration taskbar double-click action is invalid.");
        }
        result.preferences.pinnedCardsYieldToFullscreen =
            settings.value("pinnedCardsYieldToFullscreen", true);
        result.preferences.restoreWindowsOnNewWindow =
            settings.value("restoreWindowsOnNewWindow", true);
        result.preferences.showIconBackgroundFrame =
            settings.value("showIconBackgroundFrame", false);
        result.preferences.confirmFileDeletion =
            settings.value("confirmFileDeletion", true);
        result.preferences.updateChannel =
            settings.value("updateChannel", std::string{"stable"});
        if (result.preferences.updateChannel != "stable"
            && result.preferences.updateChannel != "development") {
            throw std::runtime_error("Configuration update channel is invalid.");
        }
        if (settings.contains("cardOrder")) {
            const auto& order = settings.at("cardOrder");
            if (!order.is_array()) {
                throw std::runtime_error("Configuration Card order must be an array.");
            }
            std::unordered_set<std::string> unique;
            for (const auto& value : order) {
                if (!value.is_string()) {
                    throw std::runtime_error("Configuration Card order id is invalid.");
                }
                auto id = value.get<std::string>();
                if (!id.empty() && unique.insert(id).second) {
                    result.preferences.cardOrder.push_back(std::move(id));
                }
            }
        }
    }
    if (document.contains("cards")) {
        const auto& cards = document.at("cards");
        if (!cards.is_array()) {
            throw std::runtime_error("Configuration cards must be an array.");
        }
        std::unordered_set<domain::CardId> cardIds;
        for (const auto& value : cards) {
            if (!value.is_object() || !value.contains("id") || !value.at("id").is_string()
                || !value.contains("type") || !value.at("type").is_string()) {
                throw std::runtime_error("Configuration card is missing identity fields.");
            }
            domain::CardSnapshot card{
                .id = value.at("id").get<domain::CardId>(),
                .name = value.value("name", std::string{}),
                .type = ParseCardType(value.at("type").get<std::string>()),
                .visible = value.value("visible", true),
                .expanded = value.value("expanded", true),
            };
            if (card.id.empty()) {
                throw std::runtime_error("Configuration card id must not be empty.");
            }
            if (!cardIds.insert(card.id).second) {
                throw std::runtime_error("Configuration card ids must be unique.");
            }
            if (value.contains("chrome")) {
                const auto& chrome = value.at("chrome");
                if (!chrome.is_object()) {
                    throw std::runtime_error("Configuration card chrome is invalid.");
                }
                card.chrome.showCollapseControl = chrome.value("showCollapseControl", true);
                card.chrome.showCloseControl = chrome.value("showCloseControl", false);
                card.chrome.showPinControl = chrome.value("showPinControl", false);
                card.chrome.showPresentationControl = chrome.value("showPresentationControl", true);
                card.chrome.pinOnTop = chrome.value("pinOnTop", false);
                card.chrome.showTitle = chrome.value("showTitle", true);
                card.chrome.positionLocked = chrome.value("positionLocked", false);
            }
            if (value.contains("appearance")) {
                const auto& appearance = value.at("appearance");
                if (!appearance.is_object()) {
                    throw std::runtime_error("Configuration card appearance is invalid.");
                }
                card.appearance.preset = appearance.value("preset", std::string{"system"});
                card.appearance.opacity = appearance.value("opacity", 1.0);
                card.appearance.cornerRadius = appearance.value("cornerRadius", 16.0);
            }
            if (value.contains("content")) {
                const auto& content = value.at("content");
                if (!content.is_object()) {
                    throw std::runtime_error("Configuration card content preferences are invalid.");
                }
                card.content.itemSize = ParseCardItemSize(
                    content.value("itemSize", std::string{"large"}));
                card.content.showItemNames = content.value("showItemNames", false);
                card.content.sizeMode = ParseCardSizeMode(
                    content.value("sizeMode", std::string{"adaptive"}));
                card.content.widthSpan = content.value("widthSpan", 4u);
                card.content.fixedColumns = content.value("fixedColumns", 4u);
                card.content.fixedRows = content.value("fixedRows", 3u);
                if (content.contains("maximumVisibleRows")
                    && !content.at("maximumVisibleRows").is_null()) {
                    card.content.maximumVisibleRows =
                        content.at("maximumVisibleRows").get<std::uint32_t>();
                }
            }
            switch (card.type) {
            case domain::CardType::Application: {
                const auto& application = value.at("application");
                if (!application.is_object() || !application.contains("storagePath")
                    || !application.at("storagePath").is_string()) {
                    throw std::runtime_error("Configuration application card is invalid.");
                }
                card.applicationStoragePath = FromUtf8(application.at("storagePath").get<std::string>());
                card.applicationSortMode = ParseApplicationItemSortMode(
                    application.value("sortMode", std::string{"custom"}));
                card.applicationPresentationMode = ParseMappingPresentationMode(
                    application.value("presentationMode", std::string{"grid"}));
                if (application.contains("itemPlacements")) {
                    if (!application.at("itemPlacements").is_array()) {
                        throw std::runtime_error("Configuration application item placements are invalid.");
                    }
                    for (const auto& item : application.at("itemPlacements")) {
                        if (!item.is_object() || !item.contains("fileName")
                            || !item.at("fileName").is_string()
                            || !item.contains("column") || !item.at("column").is_number_unsigned()
                            || !item.contains("row") || !item.at("row").is_number_unsigned()) {
                            throw std::runtime_error("Configuration application item placement is invalid.");
                        }
                        card.applicationItemPlacements.push_back({
                            .fileName = FromUtf8(item.at("fileName").get<std::string>()),
                            .column = item.at("column").get<std::uint32_t>(),
                            .row = item.at("row").get<std::uint32_t>(),
                        });
                    }
                }
                break;
            }
            case domain::CardType::Mapping: {
                const auto& mapping = value.at("mapping");
                if (!mapping.is_object()) {
                    throw std::runtime_error("Configuration mapping card is invalid.");
                }
                card.mappingMode = ParseMappingMode(
                    mapping.value("sourceMode", std::string{"references"}));
                card.mappingAllowsSourceMutation = mapping.value(
                    "allowsSourceMutation",
                    card.mappingMode == domain::MappingMode::Folder);
                card.mappingPresentationMode = ParseMappingPresentationMode(
                    mapping.value("presentationMode", std::string{"grid"}));
                card.mappingSortMode = ParseApplicationItemSortMode(
                    mapping.value("sortMode", std::string{"custom"}));
                if (mapping.contains("itemPlacements")) {
                    if (!mapping.at("itemPlacements").is_array()) {
                        throw std::runtime_error(
                            "Configuration mapping item placements are invalid.");
                    }
                    for (const auto& item : mapping.at("itemPlacements")) {
                        if (!item.is_object() || !item.contains("path")
                            || !item.at("path").is_string()
                            || !item.contains("column") || !item.at("column").is_number_unsigned()
                            || !item.contains("row") || !item.at("row").is_number_unsigned()) {
                            throw std::runtime_error(
                                "Configuration mapping item placement is invalid.");
                        }
                        card.mappingItemPlacements.push_back({
                            .fileName = FromUtf8(item.at("path").get<std::string>()),
                            .column = item.at("column").get<std::uint32_t>(),
                            .row = item.at("row").get<std::uint32_t>(),
                        });
                    }
                }
                if (mapping.contains("sourceRoot")) {
                    if (!mapping.at("sourceRoot").is_string()) {
                        throw std::runtime_error("Configuration mapping sourceRoot is invalid.");
                    }
                    card.mappingSourceRoot = FromUtf8(mapping.at("sourceRoot").get<std::string>());
                }
                if (mapping.contains("references")) {
                    if (!mapping.at("references").is_array()) {
                        throw std::runtime_error("Configuration mapping references are invalid.");
                    }
                    for (const auto& reference : mapping.at("references")) {
                        if (!reference.is_object() || !reference.contains("id")
                            || !reference.at("id").is_string() || !reference.contains("path")
                            || !reference.at("path").is_string()) {
                            throw std::runtime_error("Configuration mapping reference is invalid.");
                        }
                        card.mappingReferences.push_back({
                            .id = reference.at("id").get<std::string>(),
                            .path = FromUtf8(reference.at("path").get<std::string>()),
                        });
                    }
                }
                if (!card.mappingSourceRoot.empty() && !card.mappingReferences.empty()) {
                    throw std::runtime_error("Configuration mapping cannot contain a folder and references.");
                }
                if (!card.mappingSourceRoot.empty()) card.mappingMode = domain::MappingMode::Folder;
                if (!card.mappingReferences.empty()) card.mappingMode = domain::MappingMode::References;
                break;
            }
            case domain::CardType::Todo: {
                const auto& todo = value.at("todo");
                if (!todo.is_object()
                    || (todo.contains("items") && !todo.at("items").is_array())) {
                    throw std::runtime_error("Configuration todo card is invalid.");
                }
                const auto items = todo.contains("items") ? todo.at("items") : Json::array();
                for (const auto& item : items) {
                    if (!item.is_object() || !item.contains("id") || !item.at("id").is_string()
                        || !item.contains("title") || !item.at("title").is_string()) {
                        throw std::runtime_error("Configuration todo item is invalid.");
                    }
                    std::optional<domain::TodoDate> scheduledDate;
                    if (item.contains("scheduledDate") && !item.at("scheduledDate").is_null()) {
                        if (!item.at("scheduledDate").is_string()) {
                            throw std::runtime_error("Configuration Todo date is invalid.");
                        }
                        scheduledDate = ParseTodoDate(item.at("scheduledDate").get<std::string>());
                    }
                    card.todoItems.push_back({
                        .id = item.at("id").get<std::string>(),
                        .title = item.at("title").get<std::string>(),
                        .completed = item.value("completed", false),
                        .createdAtUnixMilliseconds = item.value("createdAt", std::int64_t{0}),
                        .completedAtUnixMilliseconds = item.value(
                            "completedAt", std::int64_t{0}),
                        .scheduledDate = scheduledDate,
                        .archived = item.value("archived", false),
                    });
                }
                if (todo.contains("showCreatedTime")) {
                    card.todoPreferences.showCreatedTime = todo.at("showCreatedTime").get<bool>();
                }
                break;
            }
            }
            if (card.type == domain::CardType::Application) {
                domain::ApplicationCard validated(card.id, card.applicationStoragePath);
                validated.setLayout(
                    card.applicationSortMode, card.applicationItemPlacements);
                validated.validateContentPreferences(card.content);
            }
            result.cards.push_back(std::move(card));
        }
    }
    if (document.contains("workspace")) {
        const auto& workspace = document.at("workspace");
        if (!workspace.is_object() || !workspace.contains("placements")
            || !workspace.at("placements").is_array()) {
            throw std::runtime_error("Configuration workspace.placements must be an array.");
        }
        for (const auto& value : workspace.at("placements")) {
            if (!value.is_object() || !value.contains("id") || !value.contains("cardId")
                || !value.contains("target") || !value.contains("rect")) {
                throw std::runtime_error("Configuration placement is missing required fields.");
            }
            const auto& target = value.at("target");
            if (!target.is_object() || !target.contains("kind")) {
                throw std::runtime_error("Configuration placement target is invalid.");
            }
            const auto kind = target.at("kind").get<std::string>();
            if (kind != "all" && kind != "specific") {
                throw std::runtime_error("Configuration placement target kind is invalid.");
            }
            if (kind == "specific" && (!target.contains("displayId")
                                         || !target.at("displayId").is_string())) {
                throw std::runtime_error("Configuration specific target is missing displayId.");
            }
            const auto displayTarget = kind == "all"
                ? domain::DisplayTarget::all()
                : domain::DisplayTarget::specific(target.at("displayId").get<std::string>());
            const auto& rect = value.at("rect");
            if (!rect.is_object()) {
                throw std::runtime_error("Configuration placement rect is invalid.");
            }
            double referenceWidth = 0;
            double referenceHeight = 0;
            if (value.contains("referenceWorkArea")) {
                const auto& reference = value.at("referenceWorkArea");
                if (!reference.is_object()) {
                    throw std::runtime_error("Configuration placement reference work area is invalid.");
                }
                referenceWidth = reference.value("width", 0.0);
                referenceHeight = reference.value("height", 0.0);
            }
            result.workspace.setPlacement({
                .id = value.at("id").get<domain::PlacementId>(),
                .cardId = value.at("cardId").get<domain::CardId>(),
                .target = displayTarget,
                .rect = {
                    .left = rect.at("left").get<double>(),
                    .top = rect.at("top").get<double>(),
                    .width = rect.at("width").get<double>(),
                    .height = rect.at("height").get<double>(),
                },
                .zIndex = value.value("zIndex", 0),
                .horizontalAnchor = ParseHorizontalAnchor(
                    value.value("horizontalAnchor", std::string{"left"})),
                .verticalAnchor = ParseVerticalAnchor(
                    value.value("verticalAnchor", std::string{"free"})),
                .referenceWorkAreaWidth = referenceWidth,
                .referenceWorkAreaHeight = referenceHeight,
            });
        }
    }
    return result;
}

ApplicationConfig JsonConfigStore::load() const {
    const auto backup = ConfigPathForSource(configPath_, ConfigSource::Backup1);
    if (!std::filesystem::exists(configPath_) && std::filesystem::exists(backup)) {
        auto recovered = ParseDocument(MigrateDocument(ReadDocument(backup)));
        recovered.recoveredFromBackup = true;
        return recovered;
    }
    try {
        return ParseDocument(MigrateDocument(ReadDocument(configPath_)));
    } catch (const UnsupportedSchemaError&) {
        throw;
    } catch (const std::exception& primaryError) {
        if (!std::filesystem::exists(backup)) {
            throw;
        }
        try {
            auto recovered = ParseDocument(MigrateDocument(ReadDocument(backup)));
            recovered.recoveredFromBackup = true;
            return recovered;
        } catch (const UnsupportedSchemaError&) {
            throw;
        } catch (const std::exception& backupError) {
            throw std::runtime_error(
                std::format(
                    "Configuration and backup are invalid. Primary: {} Backup: {}",
                    primaryError.what(),
                    backupError.what()));
        }
    }
}

namespace {

ConfigFileInspection InspectConfigFile(const std::filesystem::path& path) noexcept {
    ConfigFileInspection result{.path = path};
    if (!std::filesystem::exists(path)) {
        result.state = ConfigFileState::Missing;
        return result;
    }
    try {
        (void)ParseDocument(MigrateDocument(ReadDocument(path)));
        result.state = ConfigFileState::Valid;
    } catch (const UnsupportedSchemaError& error) {
        result.state = ConfigFileState::UnsupportedSchema;
        result.diagnostic = error.what();
    } catch (const std::exception& error) {
        result.state = ConfigFileState::Invalid;
        result.diagnostic = error.what();
    } catch (...) {
        result.state = ConfigFileState::Invalid;
        result.diagnostic = "Unknown configuration error.";
    }
    return result;
}

} // namespace

ConfigInspection JsonConfigStore::inspect() const noexcept {
    return {
        .primary = InspectConfigFile(configPath_),
        .backup1 = InspectConfigFile(ConfigPathForSource(configPath_, ConfigSource::Backup1)),
        .backup2 = InspectConfigFile(ConfigPathForSource(configPath_, ConfigSource::Backup2)),
    };
}

ApplicationConfig JsonConfigStore::load(ConfigSource source) const {
    const auto path = ConfigPathForSource(configPath_, source);
    auto config = ParseDocument(MigrateDocument(ReadDocument(path)));
    config.recoveredFromBackup = source != ConfigSource::Primary;
    return config;
}

void JsonConfigStore::promoteBackup(ConfigSource source) const {
    if (source == ConfigSource::Primary) {
        throw std::invalid_argument("The primary configuration is not a backup source.");
    }
    const auto inspection = inspect();
    const auto selected = source == ConfigSource::Backup1
        ? inspection.backup1 : inspection.backup2;
    if (selected.state != ConfigFileState::Valid) {
        throw std::runtime_error("The selected configuration backup is not valid.");
    }

    const auto sourcePath = selected.path;
    const auto corrupt = configPath_.parent_path()
        / (configPath_.filename().wstring() + L".corrupt-"
            + std::to_wstring(std::chrono::high_resolution_clock::now()
                .time_since_epoch().count()));
    const auto content = ReadRawFile(sourcePath);
    if (std::filesystem::exists(configPath_)) {
        std::filesystem::rename(configPath_, corrupt);
    }
    try {
        WriteAtomically(configPath_, content, true);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(configPath_, ignored);
        if (std::filesystem::exists(corrupt)) {
            std::filesystem::rename(corrupt, configPath_, ignored);
        }
        throw;
    }
}

void JsonConfigStore::save(const ApplicationConfig& config) const {
    if (config.schemaVersion != ApplicationConfig::CurrentSchemaVersion) {
        throw std::invalid_argument("Configuration schema version is not supported.");
    }
    if (config.storageRoot.empty() || !config.storageRoot.is_absolute()) {
        throw std::invalid_argument("Configuration storage root must be absolute and non-empty.");
    }
    if (config.preferences.language != "system"
        && config.preferences.language != "zh-CN"
        && config.preferences.language != "en-US") {
        throw std::invalid_argument("Configuration language is invalid.");
    }
    if (config.preferences.timeZoneOffsetMinutes.has_value()
        && (*config.preferences.timeZoneOffsetMinutes < -12 * 60
            || *config.preferences.timeZoneOffsetMinutes > 14 * 60)) {
        throw std::invalid_argument("Configuration time zone offset is outside the valid range.");
    }
    if (!std::isfinite(config.preferences.globalCardCornerRadius)
        || config.preferences.globalCardCornerRadius < 0.0
        || config.preferences.globalCardCornerRadius > 32.0) {
        throw std::invalid_argument("Configuration global Card corner radius is invalid.");
    }
    if (config.preferences.desktopDoubleClickAction != "none"
        && config.preferences.desktopDoubleClickAction != "icons"
        && config.preferences.desktopDoubleClickAction != "cards"
        && config.preferences.desktopDoubleClickAction != "all") {
        throw std::invalid_argument("Configuration desktop double-click action is invalid.");
    }
    if (config.preferences.taskbarDoubleClickAction != "none"
        && config.preferences.taskbarDoubleClickAction != "all-displays"
        && config.preferences.taskbarDoubleClickAction != "current-display") {
        throw std::invalid_argument("Configuration taskbar double-click action is invalid.");
    }
    if (config.preferences.updateChannel != "stable"
        && config.preferences.updateChannel != "development") {
        throw std::invalid_argument("Configuration update channel is invalid.");
    }
    std::unordered_set<domain::CardId> cardIds;
    for (const auto& card : config.cards) {
        if (card.id.empty() || !cardIds.insert(card.id).second) {
            throw std::invalid_argument("Configuration card ids must be unique and non-empty.");
        }
        if (!std::isfinite(card.appearance.opacity)
            || card.appearance.opacity < 0 || card.appearance.opacity > 1
            || !std::isfinite(card.appearance.cornerRadius)
            || card.appearance.cornerRadius < 0 || card.appearance.cornerRadius > 128) {
            throw std::invalid_argument(
                "Configuration card opacity or corner radius is outside the allowed range.");
        }
        domain::TodoCard contentValidator("configuration-content-validator");
        contentValidator.setContent(card.content);
        if (card.content.fixedColumns == 0 || card.content.fixedRows == 0
            || card.content.fixedColumns > 64 || card.content.fixedRows > 64) {
            throw std::invalid_argument("Configuration fixed Card grid must be between 1 and 64.");
        }
        switch (card.type) {
        case domain::CardType::Application:
            if (card.applicationStoragePath.empty() || card.applicationStoragePath.is_absolute()) {
                throw std::invalid_argument("Application card storage path must be relative and non-empty.");
            }
            {
                domain::ApplicationCard validated(card.id, card.applicationStoragePath);
                validated.setLayout(
                    card.applicationSortMode,
                    card.applicationItemPlacements);
                validated.validateContentPreferences(card.content);
            }
            break;
        case domain::CardType::Mapping:
            if (!card.mappingSourceRoot.empty() && !card.mappingReferences.empty()) {
                throw std::invalid_argument("Mapping card cannot contain a folder and references.");
            }
            if ((!card.mappingSourceRoot.empty()
                    && card.mappingMode != domain::MappingMode::Folder)
                || (!card.mappingReferences.empty()
                    && card.mappingMode != domain::MappingMode::References)
                || card.mappingMode == domain::MappingMode::Empty) {
                throw std::invalid_argument("Mapping card source does not match its source mode.");
            }
            for (const auto& reference : card.mappingReferences) {
                if (reference.id.empty() || reference.path.empty()) {
                    throw std::invalid_argument("Mapping reference must have an id and path.");
                }
            }
            break;
        case domain::CardType::Todo:
            {
                domain::TodoCard validated(card.id);
                validated.setItems(card.todoItems);
            }
            break;
        }
    }

    Json document;
    bool usedBackup = false;
    try {
        const auto backup = ConfigPathForSource(configPath_, ConfigSource::Backup1);
        if (!std::filesystem::exists(configPath_)
            && std::filesystem::exists(backup)) {
            document = MigrateDocument(ReadDocument(backup));
            usedBackup = true;
        } else {
            document = MigrateDocument(ReadDocument(configPath_));
        }
    } catch (const UnsupportedSchemaError&) {
        throw;
    } catch (const std::exception&) {
        const auto backup = ConfigPathForSource(configPath_, ConfigSource::Backup1);
        if (!std::filesystem::exists(backup)) throw;
        document = MigrateDocument(ReadDocument(backup));
        usedBackup = true;
    }
    document["schemaVersion"] = config.schemaVersion;
    document["storage"]["root"] = ToUtf8(config.storageRoot);
    document["settings"] = {
        {"language", config.preferences.language},
        {"timeZoneOffsetMinutes", config.preferences.timeZoneOffsetMinutes.has_value()
            ? Json(*config.preferences.timeZoneOffsetMinutes) : Json(nullptr)},
        {"globalCardCornerRadius", config.preferences.globalCardCornerRadius},
        {"runAtStartup", config.preferences.runAtStartup},
        {"desktopDoubleClickAction", config.preferences.desktopDoubleClickAction},
        {"taskbarDoubleClickAction", config.preferences.taskbarDoubleClickAction},
        // Retained only for round-trip compatibility with existing settings;
        // it no longer affects runtime behavior or has a settings UI.
        {"restoreWindowsOnNewWindow", config.preferences.restoreWindowsOnNewWindow},
        {"pinnedCardsYieldToFullscreen", config.preferences.pinnedCardsYieldToFullscreen},
        {"showIconBackgroundFrame", config.preferences.showIconBackgroundFrame},
        {"confirmFileDeletion", config.preferences.confirmFileDeletion},
        {"updateChannel", config.preferences.updateChannel},
        {"cardOrder", config.preferences.cardOrder},
    };

    auto& cards = document["cards"];
    if (!cards.is_array()) {
        cards = Json::array();
    }
    Json existingCardsById = Json::object();
    for (const auto& value : cards) {
        if (value.is_object() && value.contains("id") && value.at("id").is_string()) {
            existingCardsById[value.at("id").get<std::string>()] = value;
        }
    }
    cards = Json::array();
    auto cardSnapshots = config.cards;
    std::sort(
        cardSnapshots.begin(),
        cardSnapshots.end(),
        [](const domain::CardSnapshot& left, const domain::CardSnapshot& right) {
            return left.id < right.id;
        });
    for (const auto& card : cardSnapshots) {
        if (card.id.empty()) {
            throw std::invalid_argument("Configuration card id must not be empty.");
        }
        auto value = existingCardsById.contains(card.id)
            ? existingCardsById.at(card.id)
            : Json::object();
        value["id"] = card.id;
        value["name"] = card.name;
        value["type"] = domain::ToString(card.type);
        value["visible"] = card.visible;
        value["expanded"] = card.expanded;
        value["chrome"] = {
            {"showCollapseControl", card.chrome.showCollapseControl},
            {"showCloseControl", card.chrome.showCloseControl},
            {"showPinControl", card.chrome.showPinControl},
            {"showPresentationControl", card.chrome.showPresentationControl},
            {"pinOnTop", card.chrome.pinOnTop},
            {"showTitle", card.chrome.showTitle},
            {"positionLocked", card.chrome.positionLocked},
        };
        value["appearance"] = {
            {"preset", card.appearance.preset},
            {"opacity", card.appearance.opacity},
            {"cornerRadius", card.appearance.cornerRadius},
        };
        value["content"] = {
            {"itemSize", domain::ToString(card.content.itemSize)},
            {"showItemNames", card.content.showItemNames},
            {"sizeMode", domain::ToString(card.content.sizeMode)},
            {"widthSpan", card.content.widthSpan},
            {"fixedColumns", card.content.fixedColumns},
            {"fixedRows", card.content.fixedRows},
            {"maximumVisibleRows", card.content.maximumVisibleRows.has_value()
                ? Json(*card.content.maximumVisibleRows) : Json(nullptr)},
        };
        switch (card.type) {
        case domain::CardType::Application:
            value.erase("mapping");
            value.erase("todo");
            value.erase("extension");
            value["application"]["storagePath"] = ToUtf8(card.applicationStoragePath);
            value["application"]["sortMode"] = domain::ToString(card.applicationSortMode);
            value["application"]["presentationMode"] =
                card.applicationPresentationMode == domain::MappingPresentationMode::List
                ? "list" : "grid";
            value["application"]["itemPlacements"] = Json::array();
            value["application"].erase("itemOrder");
            for (const auto& placement : card.applicationItemPlacements) {
                value["application"]["itemPlacements"].push_back({
                    {"fileName", ToUtf8(placement.fileName)},
                    {"column", placement.column},
                    {"row", placement.row},
                });
            }
            break;
        case domain::CardType::Mapping: {
            value.erase("application");
            value.erase("todo");
            value.erase("extension");
            auto& mapping = value["mapping"];
            mapping["allowsSourceMutation"] = card.mappingAllowsSourceMutation;
            mapping["sourceMode"] = card.mappingMode == domain::MappingMode::Folder
                ? "folder" : "references";
            mapping["presentationMode"] =
                card.mappingPresentationMode == domain::MappingPresentationMode::List
                ? "list" : "grid";
            mapping["sortMode"] = domain::ToString(card.mappingSortMode);
            mapping["itemPlacements"] = Json::array();
            for (const auto& placement : card.mappingItemPlacements) {
                mapping["itemPlacements"].push_back({
                    {"path", ToUtf8(placement.fileName)},
                    {"column", placement.column},
                    {"row", placement.row},
                });
            }
            if (!card.mappingSourceRoot.empty()) {
                mapping["sourceRoot"] = ToUtf8(card.mappingSourceRoot);
                mapping.erase("references");
            } else {
                mapping.erase("sourceRoot");
                mapping["references"] = Json::array();
                for (const auto& reference : card.mappingReferences) {
                    mapping["references"].push_back({
                        {"id", reference.id},
                        {"path", ToUtf8(reference.path)},
                    });
                }
            }
            break;
        }
        case domain::CardType::Todo:
            value.erase("application");
            value.erase("mapping");
            value.erase("extension");
            value["todo"]["showCreatedTime"] = card.todoPreferences.showCreatedTime;
            // Once TodoDataStore has published the per-card file, the main
            // settings document no longer owns the potentially large item list.
            // Keep writing the legacy field only when no external file exists,
            // so direct config users and old installations remain compatible.
            const auto todoPath = configPath_.parent_path() / "todos" / (card.id + ".json");
            if (std::filesystem::exists(todoPath)) {
                value["todo"].erase("items");
            } else {
                value["todo"]["items"] = Json::array();
                for (const auto& item : card.todoItems) {
                    Json serializedItem{
                        {"id", item.id},
                        {"title", item.title},
                        {"completed", item.completed},
                        {"createdAt", item.createdAtUnixMilliseconds},
                        {"completedAt", item.completedAtUnixMilliseconds},
                        {"archived", item.archived},
                    };
                    if (item.scheduledDate.has_value()) {
                        serializedItem["scheduledDate"] = domain::ToString(*item.scheduledDate);
                    }
                    value["todo"]["items"].push_back(std::move(serializedItem));
                }
            }
            break;
        }
        cards.push_back(std::move(value));
    }

    auto& placements = document["workspace"]["placements"];
    if (!placements.is_array()) {
        placements = Json::array();
    }
    Json existingById = Json::object();
    for (const auto& value : placements) {
        if (value.is_object() && value.contains("id") && value.at("id").is_string()) {
            existingById[value.at("id").get<std::string>()] = value;
        }
    }
    placements = Json::array();
    auto layoutPlacements = config.workspace.placements();
    std::sort(
        layoutPlacements.begin(),
        layoutPlacements.end(),
        [](const domain::CardPlacement& left, const domain::CardPlacement& right) {
            return left.id < right.id;
        });
    for (const auto& placement : layoutPlacements) {
        auto value = existingById.contains(placement.id)
            ? existingById.at(placement.id)
            : Json::object();
        value["id"] = placement.id;
        value["cardId"] = placement.cardId;
        value["target"]["kind"] = placement.target.kind() == domain::DisplayTargetKind::AllDisplays
            ? "all"
            : "specific";
        if (placement.target.kind() == domain::DisplayTargetKind::SpecificDisplay) {
            value["target"]["displayId"] = placement.target.displayId();
        } else {
            value["target"].erase("displayId");
        }
        value["rect"] = {
            {"left", placement.rect.left},
            {"top", placement.rect.top},
            {"width", placement.rect.width},
            {"height", placement.rect.height},
        };
        value["zIndex"] = placement.zIndex;
        value["horizontalAnchor"] = domain::ToString(placement.horizontalAnchor);
        value["verticalAnchor"] = domain::ToString(placement.verticalAnchor);
        value["referenceWorkArea"] = {
            {"width", placement.referenceWorkAreaWidth},
            {"height", placement.referenceWorkAreaHeight},
        };
        placements.push_back(std::move(value));
    }
    WriteAtomically(configPath_, document.dump(2) + "\n", usedBackup);
}

} // namespace desto::storage
