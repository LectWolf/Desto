#include "JsonConfigStore.h"

#include <chrono>
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
    ApplicationConfig result;
    if (document.contains("schemaVersion")) {
        result.schemaVersion = document.at("schemaVersion").get<int>();
    }
    if (document.contains("storage") && document.at("storage").is_object()
        && document.at("storage").contains("root")) {
        result.storageRoot = FromUtf8(document.at("storage").at("root").get<std::string>());
    }
    if (result.schemaVersion < 1) {
        throw std::runtime_error("Unsupported configuration schema version.");
    }
    if (!result.storageRoot.empty() && !result.storageRoot.is_absolute()) {
        throw std::runtime_error("Configuration storage root must be absolute.");
    }
    return result;
}

void JsonConfigStore::save(const ApplicationConfig& config) const {
    if (config.schemaVersion < 1) {
        throw std::invalid_argument("Configuration schema version must be positive.");
    }
    if (config.storageRoot.empty() || !config.storageRoot.is_absolute()) {
        throw std::invalid_argument("Configuration storage root must be absolute and non-empty.");
    }

    auto document = ReadDocument(configPath_);
    document["schemaVersion"] = config.schemaVersion;
    document["storage"]["root"] = ToUtf8(config.storageRoot);
    WriteAtomically(configPath_, document.dump(2) + "\n");
}

} // namespace desto::storage
