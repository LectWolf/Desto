#include "WindowsDesktopHost.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#undef max
#undef min

namespace desto::platform::windows {
namespace {

struct Surface {
    HWND window = nullptr;
    HDC memoryDc = nullptr;
    HBITMAP bitmap = nullptr;
    HBITMAP previousBitmap = nullptr;
    std::uint32_t* pixels = nullptr;
    domain::PlacementProjection projection;
    int width = 0;
    int height = 0;
};

} // namespace

struct WindowsDesktopHost::Impl {
    explicit Impl(std::wstring titleValue)
        : title(std::move(titleValue)) {
        if (title.empty()) {
            throw std::invalid_argument("Desktop host title must not be empty.");
        }
    }

    ~Impl() {
        destroySurfaces();
        if (windowClassRegistered) {
            UnregisterClassW(className.c_str(), module);
        }
    }

    std::wstring title;
    std::wstring className = L"DestoDesktopHostSurface";
    HINSTANCE module = nullptr;
    bool windowClassRegistered = false;
    bool closeRequested = false;
    std::vector<Surface> surfaces;

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

    void initialize() {
        module = GetModuleHandleW(nullptr);
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = &WindowProcedure;
        windowClass.hInstance = module;
        windowClass.lpszClassName = className.c_str();
        windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(IDC_ARROW));
        if (RegisterClassW(&windowClass) == 0) {
            throw std::runtime_error("RegisterClassW failed for desktop host.");
        }
        windowClassRegistered = true;

    }

    void destroySurface(Surface& surface) noexcept {
        if (surface.window != nullptr) {
            DestroyWindow(surface.window);
            surface.window = nullptr;
        }
        if (surface.previousBitmap != nullptr && surface.memoryDc != nullptr) {
            SelectObject(surface.memoryDc, surface.previousBitmap);
            surface.previousBitmap = nullptr;
        }
        if (surface.bitmap != nullptr) {
            DeleteObject(surface.bitmap);
            surface.bitmap = nullptr;
        }
        surface.pixels = nullptr;
        if (surface.memoryDc != nullptr) {
            DeleteDC(surface.memoryDc);
            surface.memoryDc = nullptr;
        }
    }

    void destroySurfaces() noexcept {
        for (auto& surface : surfaces) {
            destroySurface(surface);
        }
        surfaces.clear();
    }

    void createSurface(Surface& surface) {
        surface.memoryDc = CreateCompatibleDC(nullptr);
        if (surface.memoryDc == nullptr) {
            throw std::runtime_error("CreateCompatibleDC failed for desktop host.");
        }
        BITMAPINFO bitmapInfo{};
        bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmapInfo.bmiHeader.biWidth = surface.width;
        bitmapInfo.bmiHeader.biHeight = -surface.height;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        surface.bitmap = CreateDIBSection(
            surface.memoryDc, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (surface.bitmap == nullptr) {
            throw std::runtime_error("CreateDIBSection failed for desktop host.");
        }
        surface.pixels = static_cast<std::uint32_t*>(bits);
        surface.previousBitmap = static_cast<HBITMAP>(
            SelectObject(surface.memoryDc, surface.bitmap));
        surface.window = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
            className.c_str(),
            title.c_str(),
            WS_POPUP,
            0,
            0,
            surface.width,
            surface.height,
            nullptr,
            nullptr,
            module,
            this);
        if (surface.window == nullptr) {
            throw std::runtime_error("CreateWindowExW failed for desktop host surface.");
        }
    }

    void render(Surface& surface, const domain::DisplaySnapshot& display, std::size_t ordinal) {
        const auto accent = 0xFF2080D0u + static_cast<std::uint32_t>(ordinal % 4) * 0x000C0C00u;
        std::fill(surface.pixels, surface.pixels + surface.width * surface.height, accent);
        for (int y = 0; y < surface.height; ++y) {
            for (int x = 0; x < surface.width; ++x) {
                if (x < 2 || y < 2 || x >= surface.width - 2 || y >= surface.height - 2) {
                    surface.pixels[y * surface.width + x] = 0xEEFFFFFFu;
                }
            }
        }
        const auto text = L"Card " + std::to_wstring(ordinal)
            + L"  " + std::to_wstring(static_cast<int>(display.effectiveDpi)) + L" DPI";
        SetBkMode(surface.memoryDc, TRANSPARENT);
        SetTextColor(surface.memoryDc, RGB(255, 255, 255));
        RECT textRect{14, 12, surface.width - 14, 42};
        DrawTextW(surface.memoryDc, text.c_str(), -1, &textRect, DT_LEFT | DT_SINGLELINE);

        HDC screen = GetDC(nullptr);
        if (screen == nullptr) {
            throw std::runtime_error("GetDC failed for desktop host.");
        }
        POINT source{0, 0};
        SIZE size{surface.width, surface.height};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        const auto updated = UpdateLayeredWindow(
            surface.window,
            screen,
            nullptr,
            &size,
            surface.memoryDc,
            &source,
            0,
            &blend,
            ULW_ALPHA);
        ReleaseDC(nullptr, screen);
        if (!updated) {
            throw std::runtime_error("UpdateLayeredWindow failed for desktop host.");
        }
    }

    void present(
        std::span<const domain::PlacementProjection> projections,
        std::span<const domain::DisplaySnapshot> displays) {
        if (!windowClassRegistered) {
            initialize();
        }
        destroySurfaces();
        surfaces.reserve(projections.size());
        for (const auto& projection : projections) {
            const auto display = std::find_if(
                displays.begin(), displays.end(), [&](const domain::DisplaySnapshot& candidate) {
                    return candidate.id == projection.displayId;
                });
            if (display == displays.end()) {
                throw std::invalid_argument("Projection references an unknown display.");
            }
            const auto scale = display->effectiveDpi / 96.0;
            Surface surface{
                .projection = projection,
                .width = std::max(1, static_cast<int>(std::lround(projection.rect.width * scale))),
                .height = std::max(1, static_cast<int>(std::lround(projection.rect.height * scale))),
            };
            try {
                createSurface(surface);
                render(surface, *display, surfaces.size() + 1);
                surfaces.push_back(std::move(surface));
            } catch (...) {
                destroySurface(surface);
                throw;
            }
        }

        auto deferred = BeginDeferWindowPos(static_cast<int>(surfaces.size()));
        if (deferred == nullptr && !surfaces.empty()) {
            throw std::runtime_error("BeginDeferWindowPos failed for desktop host.");
        }
        for (const auto& surface : surfaces) {
            const auto display = std::find_if(
                displays.begin(), displays.end(), [&](const domain::DisplaySnapshot& candidate) {
                    return candidate.id == surface.projection.displayId;
                });
            const auto scale = display->effectiveDpi / 96.0;
            const auto left = static_cast<int>(std::lround(
                display->workAreaLeft * scale + surface.projection.rect.left * scale));
            const auto top = static_cast<int>(std::lround(
                display->workAreaTop * scale + surface.projection.rect.top * scale));
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
                throw std::runtime_error("DeferWindowPos failed for desktop host.");
            }
        }
        if (!surfaces.empty() && !EndDeferWindowPos(deferred)) {
            throw std::runtime_error("EndDeferWindowPos failed for desktop host.");
        }
    }

    int run(int durationMilliseconds) {
        UINT_PTR timer = 0;
        if (durationMilliseconds > 0) {
            timer = SetTimer(nullptr, 1, static_cast<UINT>(durationMilliseconds), nullptr);
            if (timer == 0) {
                throw std::runtime_error("SetTimer failed for desktop host.");
            }
        }
        MSG message{};
        while (!closeRequested && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (timer != 0 && message.message == WM_TIMER && message.wParam == timer) {
                closeRequested = true;
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (timer != 0) {
            KillTimer(nullptr, timer);
        }
        return 0;
    }
};

WindowsDesktopHost::WindowsDesktopHost(std::wstring title)
    : impl_(std::make_unique<Impl>(std::move(title))) {
}

WindowsDesktopHost::~WindowsDesktopHost() = default;

void WindowsDesktopHost::present(
    std::span<const domain::PlacementProjection> projections,
    std::span<const domain::DisplaySnapshot> displays) {
    impl_->present(projections, displays);
}

int WindowsDesktopHost::run(int durationMilliseconds) {
    return impl_->run(durationMilliseconds);
}

void WindowsDesktopHost::requestClose() noexcept {
    impl_->closeRequested = true;
    PostQuitMessage(0);
}

} // namespace desto::platform::windows
