#include "ApplicationRuntime.h"
#include "WindowsDisplayTopology.h"

#include <Windows.h>

#undef max
#undef min

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace desto::application;
using namespace desto::domain;
using namespace desto::platform::windows;

namespace {

struct Surface {
    HWND window = nullptr;
    PlacementProjection projection;
    int width = 0;
    int height = 0;
};

class NativeProjectionHost {
public:
    NativeProjectionHost() = default;
    ~NativeProjectionHost() {
        for (const auto& surface : surfaces_) {
            if (surface.window != nullptr) {
                DestroyWindow(surface.window);
            }
        }
        if (module_ != nullptr) {
            UnregisterClassW(className_.c_str(), module_);
        }
    }

    NativeProjectionHost(const NativeProjectionHost&) = delete;
    NativeProjectionHost& operator=(const NativeProjectionHost&) = delete;

    void show(
        const std::vector<PlacementProjection>& projections,
        const std::vector<DisplaySnapshot>& displays) {
        module_ = GetModuleHandleW(nullptr);
        className_ = L"DestoNativePrototypeSurface";
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = &WindowProcedure;
        windowClass.hInstance = module_;
        windowClass.lpszClassName = className_.c_str();
        windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(IDC_ARROW));
        if (RegisterClassW(&windowClass) == 0) {
            throw std::runtime_error("RegisterClassW failed for native prototype.");
        }

        surfaces_.reserve(projections.size());
        for (const auto& projection : projections) {
            const auto display = std::find_if(
                displays.begin(),
                displays.end(),
                [&](const DisplaySnapshot& candidate) {
                    return candidate.id == projection.displayId;
                });
            if (display == displays.end()) {
                throw std::runtime_error("Projection references an unknown display.");
            }
            const auto scale = display->effectiveDpi / 96.0;
            Surface surface{
                .projection = projection,
                .width = std::max(1, static_cast<int>(std::lround(projection.rect.width * scale))),
                .height = std::max(1, static_cast<int>(std::lround(projection.rect.height * scale))),
            };
            surfaces_.push_back(std::move(surface));
            auto& stored = surfaces_.back();
            stored.window = CreateWindowExW(
                WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                className_.c_str(),
                L"Desto Native Prototype",
                WS_POPUP,
                0,
                0,
                stored.width,
                stored.height,
                nullptr,
                nullptr,
                module_,
                &stored);
            if (stored.window == nullptr) {
                throw std::runtime_error("CreateWindowExW failed for native prototype surface.");
            }
            Render(stored, *display, surfaces_.size());
        }

        auto deferred = BeginDeferWindowPos(static_cast<int>(surfaces_.size()));
        if (deferred == nullptr) {
            throw std::runtime_error("BeginDeferWindowPos failed for native prototype.");
        }
        for (const auto& surface : surfaces_) {
            const auto display = std::find_if(
                displays.begin(),
                displays.end(),
                [&](const DisplaySnapshot& candidate) {
                    return candidate.id == surface.projection.displayId;
                });
            const auto scale = display->effectiveDpi / 96.0;
            const auto left = static_cast<int>(std::lround(
                (display->workAreaLeft + surface.projection.rect.left) * scale));
            const auto top = static_cast<int>(std::lround(
                (display->workAreaTop + surface.projection.rect.top) * scale));
            deferred = DeferWindowPos(
                deferred,
                surface.window,
                HWND_BOTTOM,
                left,
                top,
                surface.width,
                surface.height,
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
            if (deferred == nullptr) {
                throw std::runtime_error("DeferWindowPos failed for native prototype.");
            }
        }
        if (EndDeferWindowPos(deferred) == FALSE) {
            throw std::runtime_error("EndDeferWindowPos failed for native prototype.");
        }
    }

private:
    static LRESULT CALLBACK WindowProcedure(
        HWND window,
        UINT message,
        WPARAM wParam,
        LPARAM lParam) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(
                window,
                GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        switch (message) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_ERASEBKGND:
            return 1;
        default:
            return DefWindowProcW(window, message, wParam, lParam);
        }
    }

    static void Render(
        const Surface& surface,
        const DisplaySnapshot& display,
        std::size_t ordinal) {
        BITMAPINFO bitmapInfo{};
        bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmapInfo.bmiHeader.biWidth = surface.width;
        bitmapInfo.bmiHeader.biHeight = -surface.height;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;

        HDC screen = GetDC(nullptr);
        HDC memory = CreateCompatibleDC(screen);
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(
            screen,
            &bitmapInfo,
            DIB_RGB_COLORS,
            &bits,
            nullptr,
            0);
        if (memory == nullptr || bitmap == nullptr || bits == nullptr) {
            if (bitmap != nullptr) DeleteObject(bitmap);
            if (memory != nullptr) DeleteDC(memory);
            ReleaseDC(nullptr, screen);
            throw std::runtime_error("CreateDIBSection failed for native prototype.");
        }
        SelectObject(memory, bitmap);
        auto* pixels = static_cast<std::uint32_t*>(bits);
        std::fill(pixels, pixels + surface.width * surface.height, 0u);
        const auto accent = 0xD02080E0u + static_cast<std::uint32_t>(ordinal % 4) * 0x000C0C00u;
        for (int y = 0; y < surface.height; ++y) {
            for (int x = 0; x < surface.width; ++x) {
                const auto border = x < 2 || y < 2 || x >= surface.width - 2 || y >= surface.height - 2;
                pixels[y * surface.width + x] = border ? 0xEEFFFFFFu : accent;
            }
        }
        const auto title = L"Card " + std::to_wstring(ordinal)
            + L"  " + std::to_wstring(static_cast<int>(display.effectiveDpi)) + L" DPI";
        SetBkMode(memory, TRANSPARENT);
        SetTextColor(memory, RGB(255, 255, 255));
        RECT textRect{12, 10, surface.width - 12, 34};
        DrawTextW(memory, title.c_str(), -1, &textRect, DT_LEFT | DT_SINGLELINE);

        POINT position{0, 0};
        SIZE size{surface.width, surface.height};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        if (!UpdateLayeredWindow(
                surface.window,
                screen,
                nullptr,
                &size,
                memory,
                &position,
                0,
                &blend,
                ULW_ALPHA)) {
            DeleteObject(bitmap);
            DeleteDC(memory);
            ReleaseDC(nullptr, screen);
            throw std::runtime_error("UpdateLayeredWindow failed for native prototype.");
        }
        DeleteObject(bitmap);
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
    }

    HINSTANCE module_ = nullptr;
    std::wstring className_;
    std::vector<Surface> surfaces_;
};

int DurationMilliseconds(std::wstring_view commandLine) {
    constexpr int defaultDuration = 15000;
    const auto marker = std::wstring_view(L"--duration-ms ");
    const auto position = commandLine.find(marker);
    if (position == std::wstring_view::npos) {
        return defaultDuration;
    }
    const auto value = commandLine.substr(position + marker.size());
    try {
        const auto duration = std::stoi(std::wstring(value));
        return std::clamp(duration, 1000, 120000);
    } catch (...) {
        return defaultDuration;
    }
}

} // namespace

int WINAPI wWinMain(
    HINSTANCE,
    HINSTANCE,
    PWSTR commandLine,
    int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try {
        WindowsDisplayTopology topology;
        const auto displays = topology.snapshot();
        if (displays.empty()) {
            throw std::runtime_error("A display is required to run the native prototype.");
        }
        const auto primary = std::ranges::find_if(displays, &DisplaySnapshot::primary);
        const auto& targetDisplay = primary == displays.end() ? displays.front() : *primary;
        ApplicationRuntime runtime;
        const std::vector<CardId> ids{"prototype-1", "prototype-2", "prototype-3", "prototype-4"};
        for (std::size_t index = 0; index < ids.size(); ++index) {
            const auto created = index % 3 == 0
                ? runtime.execute(CreateApplicationCard{ids[index], "cards/" + ids[index]})
                : index % 3 == 1
                    ? runtime.execute(CreateMappingCard{ids[index]})
                    : runtime.execute(CreateTodoCard{ids[index]});
            if (created.status == CommandStatus::Rejected) {
                throw std::runtime_error("Unable to create prototype Card.");
            }
            const auto placement = runtime.execute(SetPlacement{{
                .id = "placement-" + ids[index],
                .cardId = ids[index],
                .target = DisplayTarget::specific(targetDisplay.id),
                .rect = {40.0 + index * 40.0, 48.0 + index * 34.0, 320, 220},
                .zIndex = static_cast<std::int32_t>(index),
                .referenceWorkAreaWidth = targetDisplay.workAreaWidth,
                .referenceWorkAreaHeight = targetDisplay.workAreaHeight,
            }});
            if (placement.status == CommandStatus::Rejected) {
                throw std::runtime_error("Unable to create prototype Placement.");
            }
        }
        const auto topologyResult = runtime.execute(UpdateDisplayTopology{displays});
        if (topologyResult.status == CommandStatus::Rejected) {
            throw std::runtime_error("Unable to project prototype displays.");
        }

        NativeProjectionHost host;
        host.show(runtime.projections(), displays);
        const auto timer = SetTimer(nullptr, 1, DurationMilliseconds(commandLine), nullptr);
        if (timer == 0) {
            throw std::runtime_error("SetTimer failed for native prototype.");
        }
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (message.message == WM_TIMER && message.wParam == timer) {
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        KillTimer(nullptr, timer);
        return 0;
    } catch (const std::exception& error) {
        MessageBoxA(nullptr, error.what(), "Desto Native Prototype", MB_ICONERROR | MB_OK);
        return 1;
    }
}
