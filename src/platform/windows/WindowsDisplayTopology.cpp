#include "WindowsDisplayTopology.h"

#include <Windows.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <stdexcept>
#include <string>
#include <vector>

namespace desto::platform::windows {
namespace {

struct ActiveDisplayTarget {
    std::wstring devicePath;
    std::wstring gdiName;
};

struct MonitorEntry {
    std::wstring gdiName;
    RECT workArea{};
    bool primary = false;
    HMONITOR handle = nullptr;
};

struct MonitorEnumerationContext {
    std::vector<MonitorEntry> monitors;
    std::string failure;
};

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const auto required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0) {
        throw std::runtime_error("Unable to convert display identity to UTF-8.");
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            required,
            nullptr,
            nullptr)
        != required) {
        throw std::runtime_error("Unable to convert display identity to UTF-8.");
    }
    return result;
}

BOOL CALLBACK EnumerateMonitors(
    HMONITOR monitor,
    HDC,
    LPRECT,
    LPARAM parameter) {
    auto& context = *reinterpret_cast<MonitorEnumerationContext*>(parameter);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        context.failure = "GetMonitorInfoW failed.";
        return FALSE;
    }
    context.monitors.push_back({
        .gdiName = info.szDevice,
        .workArea = info.rcWork,
        .primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0,
        .handle = monitor,
    });
    return TRUE;
}

std::vector<ActiveDisplayTarget> QueryActiveDisplayTargets() {
    for (int attempt = 0; attempt < 3; ++attempt) {
        UINT32 pathCount = 0;
        UINT32 modeCount = 0;
        auto status = GetDisplayConfigBufferSizes(
            QDC_ONLY_ACTIVE_PATHS,
            &pathCount,
            &modeCount);
        if (status != ERROR_SUCCESS) {
            throw std::runtime_error("GetDisplayConfigBufferSizes failed.");
        }

        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        status = QueryDisplayConfig(
            QDC_ONLY_ACTIVE_PATHS,
            &pathCount,
            paths.data(),
            &modeCount,
            modes.data(),
            nullptr);
        if (status == ERROR_INSUFFICIENT_BUFFER) {
            continue;
        }
        if (status != ERROR_SUCCESS) {
            throw std::runtime_error("QueryDisplayConfig failed.");
        }

        std::vector<ActiveDisplayTarget> result;
        result.reserve(pathCount);
        for (UINT32 index = 0; index < pathCount; ++index) {
            DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
            target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            target.header.size = sizeof(target);
            target.header.adapterId = paths[index].targetInfo.adapterId;
            target.header.id = paths[index].targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS) {
                throw std::runtime_error("DisplayConfigGetDeviceInfo failed.");
            }
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
            source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            source.header.size = sizeof(source);
            source.header.adapterId = paths[index].sourceInfo.adapterId;
            source.header.id = paths[index].sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) {
                throw std::runtime_error("DisplayConfigGetDeviceInfo source failed.");
            }
            if (target.monitorDevicePath[0] == L'\0' || source.viewGdiDeviceName[0] == L'\0') {
                throw std::runtime_error("Active display has no stable device identity.");
            }
            result.push_back({target.monitorDevicePath, source.viewGdiDeviceName});
        }
        return result;
    }
    throw std::runtime_error("Display topology changed while it was being queried.");
}

} // namespace

std::vector<domain::DisplaySnapshot> WindowsDisplayTopology::snapshot() const {
    MonitorEnumerationContext context;
    if (!EnumDisplayMonitors(nullptr, nullptr, EnumerateMonitors, reinterpret_cast<LPARAM>(&context))) {
        throw std::runtime_error(
            context.failure.empty() ? "EnumDisplayMonitors failed." : context.failure);
    }

    const auto targets = QueryActiveDisplayTargets();
    std::vector<domain::DisplaySnapshot> result;
    result.reserve(context.monitors.size());
    for (const auto& monitor : context.monitors) {
        const auto target = std::find_if(
            targets.begin(),
            targets.end(),
            [&](const ActiveDisplayTarget& candidate) {
                return _wcsicmp(candidate.gdiName.c_str(), monitor.gdiName.c_str()) == 0;
            });
        if (target == targets.end()) {
            throw std::runtime_error("Unable to match monitor to a stable display identity.");
        }

        UINT xDpi = 96;
        UINT yDpi = 96;
        if (GetDpiForMonitor(monitor.handle, MDT_EFFECTIVE_DPI, &xDpi, &yDpi) != S_OK
            || xDpi == 0 || yDpi == 0) {
            throw std::runtime_error("GetDpiForMonitor failed.");
        }
        const auto widthPixels = monitor.workArea.right - monitor.workArea.left;
        const auto heightPixels = monitor.workArea.bottom - monitor.workArea.top;
        result.push_back({
            .id = WideToUtf8(target->devicePath),
            .workAreaWidth = static_cast<double>(widthPixels) * 96.0 / xDpi,
            .workAreaHeight = static_cast<double>(heightPixels) * 96.0 / yDpi,
            .primary = monitor.primary,
        });
    }

    std::sort(
        result.begin(),
        result.end(),
        [](const domain::DisplaySnapshot& left, const domain::DisplaySnapshot& right) {
            return left.id < right.id;
        });
    if (std::adjacent_find(
            result.begin(),
            result.end(),
            [](const domain::DisplaySnapshot& left, const domain::DisplaySnapshot& right) {
                return left.id == right.id;
            }) != result.end()) {
        throw std::runtime_error("Windows display topology returned duplicate identities.");
    }
    if (std::count_if(
            result.begin(),
            result.end(),
            [](const domain::DisplaySnapshot& display) { return display.primary; }) > 1) {
        throw std::runtime_error("Windows display topology returned multiple primary displays.");
    }
    return result;
}

} // namespace desto::platform::windows
