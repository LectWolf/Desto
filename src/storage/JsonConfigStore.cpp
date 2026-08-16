#include "JsonConfigStore.h"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <format>
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
        throw std::runtime_error("Configuration schema migration is unavailable.");
    }
    return document;
}

std::filesystem::path BackupPath(const std::filesystem::path& target) {
    return target.parent_path() / (target.filename().wstring() + L".bak");
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
            std::filesystem::copy_file(
                target,
                BackupPath(target),
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
                card.chrome.showTitle = chrome.value("showTitle", true);
            }
            if (value.contains("appearance")) {
                const auto& appearance = value.at("appearance");
                if (!appearance.is_object()) {
                    throw std::runtime_error("Configuration card appearance is invalid.");
                }
                card.appearance.preset = appearance.value("preset", std::string{"default"});
                card.appearance.opacity = appearance.value("opacity", 1.0);
                card.appearance.cornerRadius = appearance.value("cornerRadius", 16.0);
            }
            if (value.contains("content")) {
                const auto& content = value.at("content");
                if (!content.is_object()) {
                    throw std::runtime_error("Configuration card content preferences are invalid.");
                }
                card.content.itemSize = ParseCardItemSize(
                    content.value("itemSize", std::string{"medium"}));
                card.content.showItemNames = content.value("showItemNames", true);
            }
            switch (card.type) {
            case domain::CardType::Application: {
                const auto& application = value.at("application");
                if (!application.is_object() || !application.contains("storagePath")
                    || !application.at("storagePath").is_string()) {
                    throw std::runtime_error("Configuration application card is invalid.");
                }
                card.applicationStoragePath = FromUtf8(application.at("storagePath").get<std::string>());
                if (application.contains("itemOrder")) {
                    if (!application.at("itemOrder").is_array()) {
                        throw std::runtime_error("Configuration application item order is invalid.");
                    }
                    for (const auto& item : application.at("itemOrder")) {
                        if (!item.is_string()) {
                            throw std::runtime_error("Configuration application item order is invalid.");
                        }
                        card.applicationItemOrder.push_back(FromUtf8(item.get<std::string>()));
                    }
                }
                break;
            }
            case domain::CardType::Mapping: {
                const auto& mapping = value.at("mapping");
                if (!mapping.is_object()) {
                    throw std::runtime_error("Configuration mapping card is invalid.");
                }
                card.mappingAllowsSourceMutation = mapping.value("allowsSourceMutation", true);
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
                break;
            }
            case domain::CardType::Todo: {
                const auto& todo = value.at("todo");
                if (!todo.is_object() || !todo.contains("items") || !todo.at("items").is_array()) {
                    throw std::runtime_error("Configuration todo card is invalid.");
                }
                for (const auto& item : todo.at("items")) {
                    if (!item.is_object() || !item.contains("id") || !item.at("id").is_string()
                        || !item.contains("title") || !item.at("title").is_string()) {
                        throw std::runtime_error("Configuration todo item is invalid.");
                    }
                    card.todoItems.push_back({
                        .id = item.at("id").get<std::string>(),
                        .title = item.at("title").get<std::string>(),
                        .completed = item.value("completed", false),
                    });
                }
                break;
            }
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
            });
        }
    }
    return result;
}

ApplicationConfig JsonConfigStore::load() const {
    const auto backup = BackupPath(configPath_);
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

void JsonConfigStore::save(const ApplicationConfig& config) const {
    if (config.schemaVersion != ApplicationConfig::CurrentSchemaVersion) {
        throw std::invalid_argument("Configuration schema version is not supported.");
    }
    if (config.storageRoot.empty() || !config.storageRoot.is_absolute()) {
        throw std::invalid_argument("Configuration storage root must be absolute and non-empty.");
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
        switch (card.type) {
        case domain::CardType::Application:
            if (card.applicationStoragePath.empty() || card.applicationStoragePath.is_absolute()) {
                throw std::invalid_argument("Application card storage path must be relative and non-empty.");
            }
            {
                domain::ApplicationCard validated(card.id, card.applicationStoragePath);
                validated.setItemOrder(card.applicationItemOrder);
            }
            break;
        case domain::CardType::Mapping:
            if (!card.mappingSourceRoot.empty() && !card.mappingReferences.empty()) {
                throw std::invalid_argument("Mapping card cannot contain a folder and references.");
            }
            for (const auto& reference : card.mappingReferences) {
                if (reference.id.empty() || reference.path.empty()) {
                    throw std::invalid_argument("Mapping reference must have an id and path.");
                }
            }
            break;
        case domain::CardType::Todo:
            for (const auto& item : card.todoItems) {
                if (item.id.empty() || item.title.empty()) {
                    throw std::invalid_argument("Todo item must have an id and title.");
                }
            }
            break;
        }
    }

    Json document;
    bool usedBackup = false;
    try {
        if (!std::filesystem::exists(configPath_)
            && std::filesystem::exists(BackupPath(configPath_))) {
            document = MigrateDocument(ReadDocument(BackupPath(configPath_)));
            usedBackup = true;
        } else {
            document = MigrateDocument(ReadDocument(configPath_));
        }
    } catch (const UnsupportedSchemaError&) {
        throw;
    } catch (const std::exception&) {
        document = MigrateDocument(ReadDocument(BackupPath(configPath_)));
        usedBackup = true;
    }
    document["schemaVersion"] = config.schemaVersion;
    document["storage"]["root"] = ToUtf8(config.storageRoot);

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
        value["type"] = domain::ToString(card.type);
        value["visible"] = card.visible;
        value["expanded"] = card.expanded;
        value["chrome"] = {
            {"showCollapseControl", card.chrome.showCollapseControl},
            {"showCloseControl", card.chrome.showCloseControl},
            {"showPinControl", card.chrome.showPinControl},
            {"showTitle", card.chrome.showTitle},
        };
        value["appearance"] = {
            {"preset", card.appearance.preset},
            {"opacity", card.appearance.opacity},
            {"cornerRadius", card.appearance.cornerRadius},
        };
        value["content"] = {
            {"itemSize", domain::ToString(card.content.itemSize)},
            {"showItemNames", card.content.showItemNames},
        };
        switch (card.type) {
        case domain::CardType::Application:
            value.erase("mapping");
            value.erase("todo");
            value["application"]["storagePath"] = ToUtf8(card.applicationStoragePath);
            value["application"]["itemOrder"] = Json::array();
            for (const auto& item : card.applicationItemOrder) {
                value["application"]["itemOrder"].push_back(ToUtf8(item));
            }
            break;
        case domain::CardType::Mapping: {
            value.erase("application");
            value.erase("todo");
            auto& mapping = value["mapping"];
            mapping["allowsSourceMutation"] = card.mappingAllowsSourceMutation;
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
            value["todo"]["items"] = Json::array();
            for (const auto& item : card.todoItems) {
                value["todo"]["items"].push_back({
                    {"id", item.id},
                    {"title", item.title},
                    {"completed", item.completed},
                });
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
        placements.push_back(std::move(value));
    }
    WriteAtomically(configPath_, document.dump(2) + "\n", usedBackup);
}

} // namespace desto::storage
