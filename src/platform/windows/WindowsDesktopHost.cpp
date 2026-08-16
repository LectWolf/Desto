#include "WindowsDesktopHost.h"
#include "PlacementInteraction.h"

#include <Windows.h>
#include <windowsx.h>

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
    domain::DisplaySnapshot display;
    presentation::CardView card;
    std::size_t ordinal = 0;
    int width = 0;
    int height = 0;
    int interactiveHeight = 0;
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
    WindowsDesktopHost::PlacementChangedCallback placementChanged;
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
        auto* instance = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        switch (message) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_NCHITTEST:
            return instance == nullptr ? HTCLIENT : instance->hitTest(window, lParam);
        case WM_GETMINMAXINFO:
            if (instance != nullptr) {
                instance->setMinimumTrackSize(window, lParam);
                return 0;
            }
            break;
        case WM_EXITSIZEMOVE:
            if (instance != nullptr) {
                try {
                    instance->commitInteraction(window);
                } catch (...) {
                    // Native window procedures must not allow exceptions to cross Win32.
                }
                return 0;
            }
            break;
        case WM_ERASEBKGND:
            return 1;
        default:
            return DefWindowProcW(window, message, wParam, lParam);
        }
        return DefWindowProcW(window, message, wParam, lParam);
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

    Surface* findSurface(HWND window) noexcept {
        const auto found = std::find_if(
            surfaces.begin(), surfaces.end(), [&](const Surface& surface) {
                return surface.window == window;
            });
        return found == surfaces.end() ? nullptr : &*found;
    }

    LRESULT hitTest(HWND window, LPARAM lParam) noexcept {
        const auto* surface = findSurface(window);
        if (surface == nullptr) {
            return HTCLIENT;
        }
        RECT windowRect{};
        GetWindowRect(window, &windowRect);
        const auto x = GET_X_LPARAM(lParam) - windowRect.left;
        const auto y = GET_Y_LPARAM(lParam) - windowRect.top;
        if (y < 0 || y >= surface->interactiveHeight) {
            return HTTRANSPARENT;
        }
        constexpr int resizeBorder = 6;
        const auto left = x < resizeBorder;
        const auto right = x >= surface->width - resizeBorder;
        const auto top = y < resizeBorder;
        const auto bottom = y >= surface->interactiveHeight - resizeBorder;
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (bottom) return HTBOTTOM;
        if (top) return HTTOP;
        if (y < 48 && x < surface->width - 96) {
            return HTCAPTION;
        }
        return HTCLIENT;
    }

    void setMinimumTrackSize(HWND window, LPARAM lParam) noexcept {
        const auto* surface = findSurface(window);
        if (surface == nullptr) {
            return;
        }
        auto* bounds = reinterpret_cast<MINMAXINFO*>(lParam);
        const auto scale = surface->display.effectiveDpi / 96.0;
        bounds->ptMinTrackSize.x = static_cast<LONG>(std::lround(160.0 * scale));
        bounds->ptMinTrackSize.y = static_cast<LONG>(std::lround(80.0 * scale));
    }

    void destroyBitmap(Surface& surface) noexcept {
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

    void destroySurface(Surface& surface) noexcept {
        if (surface.window != nullptr) {
            DestroyWindow(surface.window);
            surface.window = nullptr;
        }
        destroyBitmap(surface);
    }

    void destroySurfaces() noexcept {
        for (auto& surface : surfaces) {
            destroySurface(surface);
        }
        surfaces.clear();
    }

    void createBitmap(Surface& surface) {
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
    }

    void createSurface(Surface& surface) {
        createBitmap(surface);
        surface.window = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
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

    void render(
        Surface& surface,
        const domain::DisplaySnapshot& display,
        const presentation::CardView& card,
        std::size_t ordinal) {
        std::uint32_t red = 0x20;
        std::uint32_t green = 0x80;
        std::uint32_t blue = 0xD0;
        const auto lightSurface = card.appearancePreset == "white"
            || card.appearancePreset == "pearl-pink"
            || card.appearancePreset == "default";
        if (lightSurface) {
            red = 0xF6;
            green = 0xF7;
            blue = 0xFA;
        } else if (card.appearancePreset == "pearl-pink") {
            red = 0xF4;
            green = 0xB0;
            blue = 0xD8;
        } else if (card.appearancePreset == "compact") {
            red = 0x30;
            green = 0xA0;
            blue = 0x90;
        } else if (card.appearancePreset == "dark") {
            red = 0x28;
            green = 0x34;
            blue = 0x48;
        }
        if (card.appearancePreset == "pearl-pink") {
            red = 0xF2;
            green = 0xAE;
            blue = 0xD6;
        }
        red = (red + (ordinal % 3) * 8) & 0xFF;
        const auto alpha = static_cast<std::uint32_t>(
            std::lround(std::clamp(card.opacity, 0.0, 1.0) * 255.0));
        const auto accent = (alpha << 24) | (red << 16) | (green << 8) | blue;
        const auto visibleBottom = card.expanded ? surface.height : std::min(surface.height, 52);
        surface.interactiveHeight = visibleBottom;
        const auto radius = std::min(
            static_cast<int>(std::lround(card.cornerRadius * display.effectiveDpi / 96.0)),
            std::min(surface.width, visibleBottom) / 2);
        const auto isInside = [&](int x, int y) {
            if (radius <= 0) {
                return true;
            }
            const auto cornerX = x < radius ? radius : x >= surface.width - radius
                ? surface.width - radius - 1 : -1;
            const auto cornerY = y < radius ? radius : y >= visibleBottom - radius
                ? visibleBottom - radius - 1 : -1;
            if (cornerX < 0 || cornerY < 0) {
                return true;
            }
            const auto dx = static_cast<double>(x - cornerX);
            const auto dy = static_cast<double>(y - cornerY);
            return dx * dx + dy * dy <= static_cast<double>(radius * radius);
        };
        std::fill(surface.pixels, surface.pixels + surface.width * surface.height, accent);
        if (visibleBottom < surface.height) {
            std::fill(surface.pixels + visibleBottom * surface.width,
                      surface.pixels + surface.width * surface.height,
                      0u);
        }
        for (int y = 0; y < surface.height; ++y) {
            for (int x = 0; x < surface.width; ++x) {
                if (y >= visibleBottom || !isInside(x, y)) {
                    surface.pixels[y * surface.width + x] = 0u;
                } else if (x < 2 || y < 2 || x >= surface.width - 2
                           || y >= visibleBottom - 2) {
                    const auto borderColor = lightSurface ? 0x002B3948u : 0x00FFFFFFu;
                    surface.pixels[y * surface.width + x] = (alpha << 24) | borderColor;
                }
            }
        }
        const auto text = card.showTitle ? card.title : L"";
        SetBkMode(surface.memoryDc, TRANSPARENT);
        const auto foreground = lightSurface ? RGB(32, 42, 52) : RGB(255, 255, 255);
        SetTextColor(surface.memoryDc, foreground);
        wchar_t iconGlyph = L'C';
        if (card.type == domain::CardType::Application) {
            iconGlyph = L'A';
        } else if (card.type == domain::CardType::Mapping) {
            iconGlyph = L'M';
        } else if (card.type == domain::CardType::Todo) {
            iconGlyph = L'T';
        }
        RECT iconRect{12, 12, 34, 34};
        HBRUSH iconBrush = CreateSolidBrush(lightSurface ? RGB(45, 55, 65) : RGB(255, 255, 255));
        FillRect(surface.memoryDc, &iconRect, iconBrush);
        DeleteObject(iconBrush);
        SetTextColor(surface.memoryDc, lightSurface ? RGB(245, 247, 250) : RGB(20, 40, 60));
        DrawTextW(surface.memoryDc, &iconGlyph, 1, &iconRect,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        SetTextColor(surface.memoryDc, foreground);
        RECT textRect{42, 12, surface.width - 120, 42};
        DrawTextW(surface.memoryDc, text.c_str(), -1, &textRect, DT_LEFT | DT_SINGLELINE);
        int controlRight = surface.width - 12;
        const auto drawControl = [&](wchar_t glyph, bool enabled) {
            if (!enabled) {
                return;
            }
            RECT controlRect{controlRight - 20, 12, controlRight, 42};
            const std::wstring label(1, glyph);
            DrawTextW(surface.memoryDc, label.c_str(), -1, &controlRect,
                      DT_CENTER | DT_SINGLELINE);
            controlRight -= 24;
        };
        drawControl(L'X', card.showCloseControl);
        drawControl(L'P', card.showPinControl);
        drawControl(card.expanded ? L'-' : L'+', card.showCollapseControl);

        if (card.expanded) {
            const auto typeText = card.typeLabel + L"  "
                + std::to_wstring(static_cast<int>(display.effectiveDpi)) + L" DPI";
            RECT typeRect{14, 58, surface.width - 14, 86};
            SetTextColor(surface.memoryDc, lightSurface ? RGB(70, 80, 90) : RGB(230, 240, 250));
            DrawTextW(surface.memoryDc, typeText.c_str(), -1, &typeRect,
                      DT_LEFT | DT_SINGLELINE);
        }

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

    void commitInteraction(HWND window) {
        auto* moved = findSurface(window);
        if (moved == nullptr) {
            return;
        }
        RECT windowRect{};
        if (!GetWindowRect(window, &windowRect)) {
            return;
        }
        const auto scale = moved->display.effectiveDpi / 96.0;
        domain::PlacementRect proposed{
            .left = windowRect.left / scale - moved->display.workAreaLeft,
            .top = windowRect.top / scale - moved->display.workAreaTop,
            .width = (windowRect.right - windowRect.left) / scale,
            .height = (windowRect.bottom - windowRect.top) / scale,
        };
        std::vector<domain::PlacementRect> otherCards;
        for (const auto& surface : surfaces) {
            if (surface.window != window && surface.projection.displayId == moved->projection.displayId
                && surface.projection.placementId != moved->projection.placementId) {
                otherCards.push_back(surface.projection.rect);
            }
        }
        const auto resolved = presentation::ResolvePlacementInteraction(
            proposed,
            moved->display.workAreaWidth,
            moved->display.workAreaHeight,
            otherCards,
            (GetKeyState(VK_CONTROL) & 0x8000) != 0);

        std::vector<Surface*> affected;
        for (auto& surface : surfaces) {
            if (surface.projection.placementId == moved->projection.placementId) {
                surface.projection.rect = resolved;
                affected.push_back(&surface);
            }
        }
        auto deferred = BeginDeferWindowPos(static_cast<int>(affected.size()));
        if (deferred == nullptr) {
            throw std::runtime_error("BeginDeferWindowPos failed after Card interaction.");
        }
        for (auto* surface : affected) {
            const auto surfaceScale = surface->display.effectiveDpi / 96.0;
            const auto width = std::max(1, static_cast<int>(std::lround(resolved.width * surfaceScale)));
            const auto height = std::max(1, static_cast<int>(std::lround(resolved.height * surfaceScale)));
            if (width != surface->width || height != surface->height) {
                destroyBitmap(*surface);
                surface->width = width;
                surface->height = height;
                createBitmap(*surface);
                render(*surface, surface->display, surface->card, surface->ordinal);
            }
            const auto left = static_cast<int>(std::lround(
                surface->display.workAreaLeft * surfaceScale + resolved.left * surfaceScale));
            const auto top = static_cast<int>(std::lround(
                surface->display.workAreaTop * surfaceScale + resolved.top * surfaceScale));
            deferred = DeferWindowPos(
                deferred,
                surface->window,
                HWND_BOTTOM,
                left,
                top,
                width,
                height,
                SWP_NOACTIVATE);
            if (deferred == nullptr) {
                throw std::runtime_error("DeferWindowPos failed after Card interaction.");
            }
        }
        if (!EndDeferWindowPos(deferred)) {
            throw std::runtime_error("EndDeferWindowPos failed after Card interaction.");
        }
        if (placementChanged) {
            placementChanged(moved->projection.placementId, moved->projection.cardId, resolved);
        }
    }

    void present(
        std::span<const domain::PlacementProjection> projections,
        std::span<const domain::DisplaySnapshot> displays,
        std::span<const presentation::CardView> cards) {
        if (!windowClassRegistered) {
            initialize();
        }
        destroySurfaces();
        surfaces.reserve(projections.size());
        for (const auto& projection : projections) {
            const auto card = std::find_if(
                cards.begin(), cards.end(), [&](const presentation::CardView& candidate) {
                    return candidate.id == projection.cardId;
                });
            if (card == cards.end()) {
                throw std::invalid_argument("Projection references an unknown Card view.");
            }
            if (!card->visible) {
                continue;
            }
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
                .display = *display,
                .card = *card,
                .ordinal = surfaces.size() + 1,
                .width = std::max(1, static_cast<int>(std::lround(projection.rect.width * scale))),
                .height = std::max(1, static_cast<int>(std::lround(projection.rect.height * scale))),
            };
            try {
                createSurface(surface);
                render(surface, *display, *card, surface.ordinal);
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
    std::span<const domain::DisplaySnapshot> displays,
    std::span<const presentation::CardView> cards) {
    impl_->present(projections, displays, cards);
}

int WindowsDesktopHost::run(int durationMilliseconds) {
    return impl_->run(durationMilliseconds);
}

void WindowsDesktopHost::requestClose() noexcept {
    impl_->closeRequested = true;
    PostQuitMessage(0);
}

void WindowsDesktopHost::setPlacementChangedCallback(PlacementChangedCallback callback) {
    impl_->placementChanged = std::move(callback);
}

} // namespace desto::platform::windows
