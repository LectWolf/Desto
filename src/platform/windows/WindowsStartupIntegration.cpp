#include "WindowsStartupIntegration.h"

#include <Windows.h>
#include <sddl.h>
#include <taskschd.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace desto::platform::windows {
namespace {

using Microsoft::WRL::ComPtr;

class ScopedBstr final {
public:
    explicit ScopedBstr(std::wstring_view value)
        : value_(SysAllocStringLen(value.data(), static_cast<UINT>(value.size()))) {}
    ~ScopedBstr() { SysFreeString(value_); }
    ScopedBstr(const ScopedBstr&) = delete;
    ScopedBstr& operator=(const ScopedBstr&) = delete;
    [[nodiscard]] BSTR get() const noexcept { return value_; }

private:
    BSTR value_ = nullptr;
};

class ComApartment final {
public:
    ComApartment() noexcept : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComApartment() {
        if (SUCCEEDED(result_)) CoUninitialize();
    }
    [[nodiscard]] HRESULT result() const noexcept { return result_; }

private:
    HRESULT result_ = E_FAIL;
};

struct RegistryMutationResult {
    bool succeeded = false;
    bool changed = false;
    LSTATUS error = ERROR_SUCCESS;
};

RegistryMutationResult RemoveLegacyRunValue(
    std::wstring_view registryPath,
    std::wstring_view valueName) noexcept {
    HKEY key = nullptr;
    const auto opened = RegOpenKeyExW(
        HKEY_CURRENT_USER,
        std::wstring(registryPath).c_str(),
        0,
        KEY_SET_VALUE,
        &key);
    if (opened == ERROR_FILE_NOT_FOUND) return {.succeeded = true};
    if (opened != ERROR_SUCCESS) return {.error = opened};
    const auto removed = RegDeleteValueW(key, std::wstring(valueName).c_str());
    RegCloseKey(key);
    if (removed != ERROR_SUCCESS && removed != ERROR_FILE_NOT_FOUND) {
        return {.error = removed};
    }
    return {.succeeded = true, .changed = removed == ERROR_SUCCESS};
}

RegistryMutationResult EnsureLegacyRunValue(
    std::wstring_view registryPath,
    std::wstring_view valueName,
    std::wstring_view command) noexcept {
    HKEY key = nullptr;
    const auto opened = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        std::wstring(registryPath).c_str(),
        0,
        nullptr,
        0,
        KEY_QUERY_VALUE | KEY_SET_VALUE,
        nullptr,
        &key,
        nullptr);
    if (opened != ERROR_SUCCESS) return {.error = opened};

    DWORD type = 0;
    DWORD bytes = 0;
    auto queried = RegQueryValueExW(
        key, std::wstring(valueName).c_str(), nullptr, &type, nullptr, &bytes);
    std::wstring existing;
    if (queried == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) {
        std::vector<wchar_t> buffer(
            (std::max<std::size_t>)(1, bytes / sizeof(wchar_t) + 1), L'\0');
        queried = RegQueryValueExW(
            key,
            std::wstring(valueName).c_str(),
            nullptr,
            &type,
            reinterpret_cast<BYTE*>(buffer.data()),
            &bytes);
        if (queried == ERROR_SUCCESS) existing.assign(buffer.data());
    }
    if (queried == ERROR_SUCCESS && existing == command) {
        RegCloseKey(key);
        return {.succeeded = true};
    }
    const auto commandBytes = static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
    const auto wrote = RegSetValueExW(
        key,
        std::wstring(valueName).c_str(),
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(command.data()),
        commandBytes);
    RegCloseKey(key);
    return wrote == ERROR_SUCCESS
        ? RegistryMutationResult{.succeeded = true, .changed = true}
        : RegistryMutationResult{.error = wrote};
}

std::wstring TakeBstr(BSTR value) {
    if (value == nullptr) return {};
    std::wstring result(value, SysStringLen(value));
    SysFreeString(value);
    return result;
}

std::optional<std::wstring> CurrentUserSid() noexcept {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return std::nullopt;
    }
    DWORD bytes = 0;
    (void)GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    if (bytes == 0) {
        CloseHandle(token);
        return std::nullopt;
    }
    std::vector<std::byte> buffer(bytes);
    if (!GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes)) {
        CloseHandle(token);
        return std::nullopt;
    }
    CloseHandle(token);
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    wchar_t* sid = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &sid) || sid == nullptr) {
        return std::nullopt;
    }
    std::wstring result(sid);
    LocalFree(sid);
    return result;
}

bool ExistingTaskMatches(
    IRegisteredTask* registered,
    const DestoStartupTaskDescriptor& expected) noexcept {
    if (registered == nullptr) return false;
    try {
        VARIANT_BOOL enabled = VARIANT_FALSE;
        if (FAILED(registered->get_Enabled(&enabled)) || enabled != VARIANT_TRUE) return false;
        ComPtr<ITaskDefinition> definition;
        if (FAILED(registered->get_Definition(definition.GetAddressOf()))
            || definition == nullptr) return false;

        ComPtr<IPrincipal> principal;
        TASK_LOGON_TYPE logonType = TASK_LOGON_NONE;
        TASK_RUNLEVEL_TYPE runLevel = TASK_RUNLEVEL_LUA;
        if (FAILED(definition->get_Principal(principal.GetAddressOf()))
            || principal == nullptr
            || FAILED(principal->get_LogonType(&logonType))
            || FAILED(principal->get_RunLevel(&runLevel))
            || logonType != TASK_LOGON_INTERACTIVE_TOKEN
            || runLevel != TASK_RUNLEVEL_LUA) {
            return false;
        }

        ComPtr<ITriggerCollection> triggers;
        LONG triggerCount = 0;
        ComPtr<ITrigger> trigger;
        TASK_TRIGGER_TYPE2 triggerType = TASK_TRIGGER_EVENT;
        if (FAILED(definition->get_Triggers(triggers.GetAddressOf()))
            || triggers == nullptr
            || FAILED(triggers->get_Count(&triggerCount))
            || triggerCount != 1
            || FAILED(triggers->get_Item(1, trigger.GetAddressOf()))
            || trigger == nullptr
            || FAILED(trigger->get_Type(&triggerType))
            || triggerType != TASK_TRIGGER_LOGON) {
            return false;
        }
        ComPtr<ILogonTrigger> logonTrigger;
        if (FAILED(trigger.As(&logonTrigger)) || logonTrigger == nullptr) return false;
        BSTR delayValue = nullptr;
        if (FAILED(logonTrigger->get_Delay(&delayValue))) return false;
        const auto delay = TakeBstr(delayValue);
        if (!delay.empty()) return false;

        ComPtr<IActionCollection> actions;
        LONG actionCount = 0;
        ComPtr<IAction> action;
        if (FAILED(definition->get_Actions(actions.GetAddressOf()))
            || actions == nullptr
            || FAILED(actions->get_Count(&actionCount))
            || actionCount != 1
            || FAILED(actions->get_Item(1, action.GetAddressOf()))
            || action == nullptr) {
            return false;
        }
        ComPtr<IExecAction> exec;
        if (FAILED(action.As(&exec)) || exec == nullptr) return false;
        BSTR pathValue = nullptr;
        BSTR argumentsValue = nullptr;
        BSTR directoryValue = nullptr;
        if (FAILED(exec->get_Path(&pathValue))
            || FAILED(exec->get_Arguments(&argumentsValue))
            || FAILED(exec->get_WorkingDirectory(&directoryValue))) {
            SysFreeString(pathValue);
            SysFreeString(argumentsValue);
            SysFreeString(directoryValue);
            return false;
        }
        const auto path = TakeBstr(pathValue);
        const auto arguments = TakeBstr(argumentsValue);
        const auto directory = TakeBstr(directoryValue);
        return std::filesystem::path(path).lexically_normal() == expected.executable
            && arguments == expected.arguments
            && std::filesystem::path(directory).lexically_normal()
                == expected.workingDirectory;
    } catch (...) {
        return false;
    }
}

HRESULT ConnectTaskScheduler(
    ComPtr<ITaskService>& service,
    ComPtr<ITaskFolder>& root) noexcept {
    auto result = CoCreateInstance(
        CLSID_TaskScheduler,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(service.GetAddressOf()));
    if (FAILED(result)) return result;
    VARIANT empty{};
    VariantInit(&empty);
    result = service->Connect(empty, empty, empty, empty);
    if (FAILED(result)) return result;
    ScopedBstr rootPath(L"\\");
    if (rootPath.get() == nullptr) return E_OUTOFMEMORY;
    return service->GetFolder(rootPath.get(), root.GetAddressOf());
}

HRESULT RegisterStartupTask(
    ITaskService* service,
    ITaskFolder* root,
    std::wstring_view taskName,
    const DestoStartupTaskDescriptor& descriptor) noexcept {
    try {
        const auto currentUserSid = CurrentUserSid();
        if (!currentUserSid.has_value()) return HRESULT_FROM_WIN32(GetLastError());
        ComPtr<ITaskDefinition> definition;
        auto result = service->NewTask(0, definition.GetAddressOf());
        if (FAILED(result)) return result;

        ComPtr<IRegistrationInfo> registration;
        result = definition->get_RegistrationInfo(registration.GetAddressOf());
        if (FAILED(result)) return result;
        ScopedBstr description(L"Starts Desto immediately when the current user signs in.");
        if (description.get() == nullptr) return E_OUTOFMEMORY;
        result = registration->put_Description(description.get());
        if (FAILED(result)) return result;

        ComPtr<IPrincipal> principal;
        result = definition->get_Principal(principal.GetAddressOf());
        if (FAILED(result)) return result;
        if (FAILED(result = principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN))
            || FAILED(result = principal->put_RunLevel(TASK_RUNLEVEL_LUA))) {
            return result;
        }
        ScopedBstr principalUser(*currentUserSid);
        if (principalUser.get() == nullptr) return E_OUTOFMEMORY;
        if (FAILED(result = principal->put_UserId(principalUser.get()))) return result;

        ComPtr<ITaskSettings> settings;
        result = definition->get_Settings(settings.GetAddressOf());
        if (FAILED(result)) return result;
        if (FAILED(result = settings->put_StartWhenAvailable(VARIANT_TRUE))
            || FAILED(result = settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE))
            || FAILED(result = settings->put_StopIfGoingOnBatteries(VARIANT_FALSE))
            || FAILED(result = settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW))) {
            return result;
        }
        ScopedBstr noExecutionLimit(L"PT0S");
        if (noExecutionLimit.get() == nullptr) return E_OUTOFMEMORY;
        if (FAILED(result = settings->put_ExecutionTimeLimit(noExecutionLimit.get()))) {
            return result;
        }

        ComPtr<ITriggerCollection> triggers;
        ComPtr<ITrigger> trigger;
        result = definition->get_Triggers(triggers.GetAddressOf());
        if (FAILED(result)
            || FAILED(result = triggers->Create(TASK_TRIGGER_LOGON, trigger.GetAddressOf()))) {
            return result;
        }
        ComPtr<ILogonTrigger> logonTrigger;
        if (FAILED(result = trigger.As(&logonTrigger))) return result;
        ScopedBstr triggerId(L"DestoLogon");
        if (triggerId.get() == nullptr) return E_OUTOFMEMORY;
        if (FAILED(result = logonTrigger->put_Id(triggerId.get()))) return result;

        ComPtr<IActionCollection> actions;
        ComPtr<IAction> action;
        result = definition->get_Actions(actions.GetAddressOf());
        if (FAILED(result)
            || FAILED(result = actions->Create(TASK_ACTION_EXEC, action.GetAddressOf()))) {
            return result;
        }
        ComPtr<IExecAction> exec;
        if (FAILED(result = action.As(&exec))) return result;
        ScopedBstr path(descriptor.executable.wstring());
        ScopedBstr arguments(descriptor.arguments);
        ScopedBstr directory(descriptor.workingDirectory.wstring());
        if (path.get() == nullptr || arguments.get() == nullptr || directory.get() == nullptr) {
            return E_OUTOFMEMORY;
        }
        if (FAILED(result = exec->put_Path(path.get()))
            || FAILED(result = exec->put_Arguments(arguments.get()))
            || FAILED(result = exec->put_WorkingDirectory(directory.get()))) {
            return result;
        }

        ScopedBstr name(taskName);
        if (name.get() == nullptr) return E_OUTOFMEMORY;
        VARIANT empty{};
        VariantInit(&empty);
        VARIANT user{};
        VariantInit(&user);
        user.vt = VT_BSTR;
        user.bstrVal = SysAllocString(principalUser.get());
        if (user.bstrVal == nullptr) return E_OUTOFMEMORY;
        ComPtr<IRegisteredTask> registered;
        result = root->RegisterTaskDefinition(
            name.get(),
            definition.Get(),
            TASK_CREATE_OR_UPDATE,
            user,
            empty,
            TASK_LOGON_INTERACTIVE_TOKEN,
            empty,
            registered.GetAddressOf());
        VariantClear(&user);
        return result;
    } catch (...) {
        return E_UNEXPECTED;
    }
}

} // namespace

DestoLaunchOptions ParseDestoLaunchOptions(
    std::span<const std::wstring_view> arguments) noexcept {
    DestoLaunchOptions options;
    try {
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            const auto argument = arguments[index];
            if (argument == L"--settings") {
                options.showSettings = true;
            } else if (argument == L"--autostart") {
                options.startedAutomatically = true;
            } else if (argument == L"--duration-ms" && index + 1 < arguments.size()) {
                options.durationMilliseconds = std::clamp(
                    std::stoi(std::wstring(arguments[++index])), 1000, 120000);
            } else if (argument.starts_with(L"--duration-ms=")) {
                options.durationMilliseconds = std::clamp(
                    std::stoi(std::wstring(argument.substr(14))), 1000, 120000);
            }
        }
    } catch (...) {
        options.durationMilliseconds = 0;
    }
    return options;
}

std::wstring BuildDestoStartupCommand(const std::filesystem::path& executable) {
    const auto descriptor = BuildDestoStartupTaskDescriptor(executable);
    return L"\"" + descriptor.executable.wstring() + L"\" " + descriptor.arguments;
}

DestoStartupTaskDescriptor BuildDestoStartupTaskDescriptor(
    const std::filesystem::path& executable) {
    if (executable.empty() || !executable.is_absolute()) {
        throw std::invalid_argument("Desto startup executable must be an absolute path.");
    }
    const auto normalized = executable.lexically_normal();
    if (normalized.wstring().find(L'\"') != std::wstring::npos) {
        throw std::invalid_argument("Desto startup executable contains an invalid quote.");
    }
    return {
        .executable = normalized,
        .workingDirectory = normalized.parent_path(),
        .arguments = L"--autostart",
        .triggerDelayMilliseconds = 0,
    };
}

WindowsStartupIntegration::WindowsStartupIntegration(
    std::filesystem::path executable,
    std::wstring registryPath,
    std::wstring valueName)
    : executable_(std::move(executable).lexically_normal()),
      registryPath_(std::move(registryPath)),
      valueName_(std::move(valueName)) {
    if (registryPath_.empty() || valueName_.empty()) {
        throw std::invalid_argument("Desto startup identity must not be empty.");
    }
    (void)BuildDestoStartupTaskDescriptor(executable_);
}

StartupRegistrationResult WindowsStartupIntegration::ensure(bool enabled) const noexcept {
    ComApartment apartment;
    if (FAILED(apartment.result()) && apartment.result() != RPC_E_CHANGED_MODE) {
        if (!enabled) {
            const auto legacy = RemoveLegacyRunValue(registryPath_, valueName_);
            return legacy.succeeded
                ? StartupRegistrationResult{.succeeded = true, .changed = legacy.changed}
                : StartupRegistrationResult{
                    .error = static_cast<std::uint32_t>(legacy.error)};
        }
        try {
            const auto fallback = EnsureLegacyRunValue(
                registryPath_, valueName_, BuildDestoStartupCommand(executable_));
            return fallback.succeeded
                ? StartupRegistrationResult{
                    .succeeded = true, .changed = fallback.changed, .fallbackUsed = true}
                : StartupRegistrationResult{
                    .error = static_cast<std::uint32_t>(fallback.error)};
        } catch (...) {
            return {.error = ERROR_INVALID_DATA};
        }
    }
    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> root;
    auto result = ConnectTaskScheduler(service, root);
    if (FAILED(result) && enabled) {
        try {
            const auto fallback = EnsureLegacyRunValue(
                registryPath_, valueName_, BuildDestoStartupCommand(executable_));
            return fallback.succeeded
                ? StartupRegistrationResult{
                    .succeeded = true, .changed = fallback.changed, .fallbackUsed = true}
                : StartupRegistrationResult{
                    .error = static_cast<std::uint32_t>(fallback.error)};
        } catch (...) {
            return {.error = ERROR_INVALID_DATA};
        }
    }
    if (FAILED(result)) {
        const auto legacy = RemoveLegacyRunValue(registryPath_, valueName_);
        return legacy.succeeded
            ? StartupRegistrationResult{.succeeded = true, .changed = legacy.changed}
            : StartupRegistrationResult{
                .error = static_cast<std::uint32_t>(legacy.error)};
    }

    ScopedBstr taskName(valueName_);
    if (taskName.get() == nullptr) return {.error = ERROR_OUTOFMEMORY};
    ComPtr<IRegisteredTask> existing;
    const auto existingResult = root->GetTask(taskName.get(), existing.GetAddressOf());
    const auto exists = SUCCEEDED(existingResult) && existing != nullptr;

    if (!enabled) {
        const auto legacy = RemoveLegacyRunValue(registryPath_, valueName_);
        if (!legacy.succeeded) {
            return {.error = static_cast<std::uint32_t>(legacy.error)};
        }
        if (!exists) {
            return {.succeeded = true, .changed = legacy.changed};
        }
        result = root->DeleteTask(taskName.get(), 0);
        if (FAILED(result)) return {.error = static_cast<std::uint32_t>(result)};
        return {.succeeded = true, .changed = true};
    }

    try {
        const auto descriptor = BuildDestoStartupTaskDescriptor(executable_);
        if (exists && ExistingTaskMatches(existing.Get(), descriptor)) {
            const auto legacy = RemoveLegacyRunValue(registryPath_, valueName_);
            if (!legacy.succeeded) {
                return {.error = static_cast<std::uint32_t>(legacy.error)};
            }
            return {.succeeded = true, .changed = legacy.changed};
        }
        result = RegisterStartupTask(
            service.Get(), root.Get(), valueName_, descriptor);
        if (FAILED(result)) {
            const auto fallback = EnsureLegacyRunValue(
                registryPath_, valueName_, BuildDestoStartupCommand(executable_));
            return fallback.succeeded
                ? StartupRegistrationResult{
                    .succeeded = true, .changed = fallback.changed, .fallbackUsed = true}
                : StartupRegistrationResult{
                    .error = static_cast<std::uint32_t>(fallback.error)};
        }
        const auto legacy = RemoveLegacyRunValue(registryPath_, valueName_);
        if (!legacy.succeeded) {
            return {.error = static_cast<std::uint32_t>(legacy.error)};
        }
        return {.succeeded = true, .changed = true};
    } catch (...) {
        return {.error = ERROR_INVALID_DATA};
    }
}

} // namespace desto::platform::windows
