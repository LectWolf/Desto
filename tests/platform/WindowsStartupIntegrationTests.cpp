#include "TestSupport.h"
#include "WindowsStartupIntegration.h"

#include <Windows.h>

#include <chrono>

using namespace desto::platform::windows;

namespace {

std::filesystem::path CurrentExecutablePath() {
    std::wstring buffer(32768, L'\0');
    const auto length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    DESTO_CHECK(length > 0 && length < buffer.size());
    buffer.resize(length);
    return buffer;
}

void RunTests() {
    const auto executable = CurrentExecutablePath();
    const auto descriptor = BuildDestoStartupTaskDescriptor(executable);
    DESTO_CHECK(descriptor.executable == executable.lexically_normal());
    DESTO_CHECK(descriptor.workingDirectory == executable.parent_path());
    DESTO_CHECK(descriptor.arguments == L"--autostart");
    DESTO_CHECK(descriptor.triggerDelayMilliseconds == 0);
    DESTO_CHECK(BuildDestoStartupCommand(executable)
        == L"\"" + executable.wstring() + L"\" --autostart");

    const auto uniqueName = L"Desto-Test-" + std::to_wstring(GetCurrentProcessId())
        + L"-" + std::to_wstring(
            std::chrono::steady_clock::now().time_since_epoch().count());
    WindowsStartupIntegration integration(
        executable,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        uniqueName);
    struct Cleanup {
        const WindowsStartupIntegration& integration;
        ~Cleanup() { (void)integration.ensure(false); }
    } cleanup{integration};

    DESTO_CHECK(integration.ensure(false).succeeded);
    const auto enabled = integration.ensure(true);
    DESTO_CHECK(enabled.succeeded && enabled.changed);
    const auto unchanged = integration.ensure(true);
    DESTO_CHECK(unchanged.succeeded && !unchanged.changed);
    const auto disabled = integration.ensure(false);
    DESTO_CHECK(disabled.succeeded && disabled.changed);
}

} // namespace

int main() {
    return desto::test::Run(RunTests);
}
