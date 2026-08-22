#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace desto::platform::windows {

struct DestoLaunchOptions {
    bool showSettings = false;
    bool startedAutomatically = false;
    int durationMilliseconds = 0;
};

[[nodiscard]] DestoLaunchOptions ParseDestoLaunchOptions(
    std::span<const std::wstring_view> arguments) noexcept;
[[nodiscard]] std::wstring BuildDestoStartupCommand(
    const std::filesystem::path& executable);

struct DestoStartupTaskDescriptor {
    std::filesystem::path executable;
    std::filesystem::path workingDirectory;
    std::wstring arguments = L"--autostart";
    std::uint32_t triggerDelayMilliseconds = 0;
};

[[nodiscard]] DestoStartupTaskDescriptor BuildDestoStartupTaskDescriptor(
    const std::filesystem::path& executable);

struct StartupRegistrationResult {
    bool succeeded = false;
    bool changed = false;
    bool fallbackUsed = false;
    std::uint32_t error = 0;
};

class WindowsStartupIntegration final {
public:
    explicit WindowsStartupIntegration(
        std::filesystem::path executable,
        std::wstring registryPath =
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        std::wstring valueName = L"Desto");

    [[nodiscard]] StartupRegistrationResult ensure(bool enabled) const noexcept;

private:
    std::filesystem::path executable_;
    std::wstring registryPath_;
    std::wstring valueName_;
};

} // namespace desto::platform::windows
