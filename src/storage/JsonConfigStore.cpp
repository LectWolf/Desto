#include "JsonConfigStore.h"

#include <chrono>
#include <algorithm>
#include <fstream>
#include <format>
#include <stdexcept>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace desto::storage {
namespace {

using Json = nlohmann::json;

std::string ToUtf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::filesystem::path FromUtf8(const std::string& value) {
    return std::filesystem::u8path(value);
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

void WriteAtomically(const std::filesystem::path& target, const std::string& content) {
    std::filesystem::create_directories(target.parent_path());
    const auto temporary = target.parent_path()
        / (target.filename().wstring() + L".tmp-" + std::to_wstring(
            std::chrono::high_resolution_clock::now().time_since_epoch().count()));

    try {
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

ApplicationConfig JsonConfigStore::load() const {
    const auto document = ReadDocument(configPath_);
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

void JsonConfigStore::save(const ApplicationConfig& config) const {
    if (config.schemaVersion != ApplicationConfig::CurrentSchemaVersion) {
        throw std::invalid_argument("Configuration schema version is not supported.");
    }
    if (config.storageRoot.empty() || !config.storageRoot.is_absolute()) {
        throw std::invalid_argument("Configuration storage root must be absolute and non-empty.");
    }

    auto document = ReadDocument(configPath_);
    if (!document.is_object()) {
        throw std::runtime_error("Configuration root must be a JSON object.");
    }
    document["schemaVersion"] = config.schemaVersion;
    document["storage"]["root"] = ToUtf8(config.storageRoot);

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
    WriteAtomically(configPath_, document.dump(2) + "\n");
}

} // namespace desto::storage
