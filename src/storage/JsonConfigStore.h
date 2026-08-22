#pragma once

#include <filesystem>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Card.h"
#include "WorkspaceLayout.h"

namespace desto::storage {

struct ApplicationPreferences {
    // null means that Todo dates follow the Windows time zone.
    std::optional<std::int32_t> timeZoneOffsetMinutes;
    std::string language = "system";
    double globalCardCornerRadius = 16.0;
    bool runAtStartup = false;
    std::string desktopDoubleClickAction = "none";
    std::string taskbarDoubleClickAction = "none";
    bool restoreWindowsOnNewWindow = true;
    bool pinnedCardsYieldToFullscreen = true;
    bool showIconBackgroundFrame = false;
    bool confirmFileDeletion = true;
    std::vector<domain::CardId> cardOrder;
};

struct ApplicationConfig {
    static constexpr int CurrentSchemaVersion = 27;

    int schemaVersion = CurrentSchemaVersion;
    std::filesystem::path storageRoot;
    ApplicationPreferences preferences;
    std::vector<domain::CardSnapshot> cards;
    domain::WorkspaceLayout workspace;
    bool recoveredFromBackup = false;
};

enum class ConfigSource {
    Primary,
    Backup1,
    Backup2,
};

enum class ConfigFileState {
    Missing,
    Valid,
    Invalid,
    UnsupportedSchema,
};

struct ConfigFileInspection {
    ConfigFileState state = ConfigFileState::Missing;
    std::filesystem::path path;
    std::string diagnostic;
};

struct ConfigInspection {
    ConfigFileInspection primary;
    ConfigFileInspection backup1;
    ConfigFileInspection backup2;

    [[nodiscard]] bool primaryUsable() const noexcept {
        return primary.state == ConfigFileState::Valid;
    }
    [[nodiscard]] bool hasUsableBackup() const noexcept {
        return backup1.state == ConfigFileState::Valid
            || backup2.state == ConfigFileState::Valid;
    }
};

class JsonConfigStore {
public:
    explicit JsonConfigStore(std::filesystem::path configPath);

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return configPath_; }
    [[nodiscard]] ConfigInspection inspect() const noexcept;
    [[nodiscard]] ApplicationConfig load() const;
    [[nodiscard]] ApplicationConfig load(ConfigSource source) const;
    void promoteBackup(ConfigSource source) const;
    void save(const ApplicationConfig& config) const;

private:
    std::filesystem::path configPath_;
};

} // namespace desto::storage
