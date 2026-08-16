#include "WindowsDesktopHost.h"
#include "PlacementInteraction.h"

#include <Windows.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
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
    bool collapsePressed = false;
};

LRESULT CALLBACK GuideWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window, &paint);
        RECT bounds{};
        GetClientRect(window, &bounds);
        FillRect(dc, &bounds, reinterpret_cast<HBRUSH>(GetClassLongPtrW(window, GCLP_HBRBACKGROUND)));
        EndPaint(window, &paint);
        return 0;
    }
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

} // namespace

struct WindowsDesktopHost::Impl {
    explicit Impl(std::wstring titleValue)
        : title(std::move(titleValue)) {
        if (title.empty()) {
            throw std::invalid_argument("Desktop host title must not be empty.");
        }
    }

    ~Impl() {
        destroyGuides();
        destroySurfaces();
        if (guideWindowClassRegistered) {
            UnregisterClassW(guideClassName.c_str(), module);
        }
        if (windowClassRegistered) {
            UnregisterClassW(className.c_str(), module);
        }
        if (guideBrush != nullptr) {
            DeleteObject(guideBrush);
        }
    }

    std::wstring title;
    std::wstring className = L"DestoDesktopHostSurface";
    std::wstring guideClassName = L"DestoAlignmentGuide";
    HINSTANCE module = nullptr;
    bool windowClassRegistered = false;
    bool guideWindowClassRegistered = false;
    bool closeRequested = false;
    HWND verticalGuide = nullptr;
    HWND horizontalGuide = nullptr;
    HBRUSH guideBrush = nullptr;
    WindowsDesktopHost::PlacementChangedCallback placementChanged;
    WindowsDesktopHost::CardExpandedChangedCallback cardExpandedChanged;
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
        case WM_ENTERSIZEMOVE:
            if (instance != nullptr) {
                instance->hideGuides();
                return 0;
            }
            break;
        case WM_MOVING:
        case WM_SIZING:
            if (instance != nullptr) {
                try {
                    instance->updateInteractionGuides(
                        window,
                        *reinterpret_cast<const RECT*>(lParam));
                } catch (...) {
                    instance->hideGuides();
                }
                return TRUE;
            }
            break;
        case WM_EXITSIZEMOVE:
            if (instance != nullptr) {
                instance->hideGuides();
                try {
                    instance->commitInteraction(window);
                } catch (...) {
                    // Native window procedures must not allow exceptions to cross Win32.
                }
                return 0;
            }
            break;
        case WM_LBUTTONDOWN:
            if (instance != nullptr
                && instance->beginCollapsePress(window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) {
                return 0;
            }
            break;
        case WM_LBUTTONUP:
            if (instance != nullptr
                && instance->endCollapsePress(window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) {
                return 0;
            }
            break;
        case WM_CAPTURECHANGED:
            if (instance != nullptr) {
                instance->cancelCollapsePress(window);
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

        guideBrush = CreateSolidBrush(RGB(74, 132, 255));
        if (guideBrush == nullptr) {
            throw std::runtime_error("CreateSolidBrush failed for alignment guides.");
        }
        WNDCLASSW guideClass{};
        guideClass.lpfnWndProc = &GuideWindowProcedure;
        guideClass.hInstance = module;
        guideClass.lpszClassName = guideClassName.c_str();
        guideClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(IDC_ARROW));
        guideClass.hbrBackground = guideBrush;
        if (RegisterClassW(&guideClass) == 0) {
            throw std::runtime_error("RegisterClassW failed for alignment guides.");
        }
        guideWindowClassRegistered = true;
    }

    Surface* findSurface(HWND window) noexcept {
        const auto found = std::find_if(
            surfaces.begin(), surfaces.end(), [&](const Surface& surface) {
                return surface.window == window;
            });
        return found == surfaces.end() ? nullptr : &*found;
    }

    static int dipToPixels(double value, const Surface& surface) noexcept {
        return std::max(1, static_cast<int>(std::lround(
            value * surface.display.effectiveDpi / 96.0)));
    }

    static RECT collapseControlRect(const Surface& surface) noexcept {
        const auto size = dipToPixels(36.0, surface);
        const auto inset = dipToPixels(6.0, surface);
        return {
            surface.width - inset - size,
            inset,
            surface.width - inset,
            inset + size,
        };
    }

    static bool pointInside(const RECT& rect, int x, int y) noexcept {
        return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
    }

    bool isCollapseControlHit(const Surface& surface, int x, int y) const noexcept {
        return surface.card.showCollapseControl
            && pointInside(collapseControlRect(surface), x, y);
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
        const auto resizeBorder = dipToPixels(6.0, *surface);
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
        if (y < dipToPixels(48.0, *surface) && !isCollapseControlHit(*surface, x, y)) {
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

    void destroyGuides() noexcept {
        if (verticalGuide != nullptr) {
            DestroyWindow(verticalGuide);
            verticalGuide = nullptr;
        }
        if (horizontalGuide != nullptr) {
            DestroyWindow(horizontalGuide);
            horizontalGuide = nullptr;
        }
    }

    void hideGuides() noexcept {
        if (verticalGuide != nullptr) {
            ShowWindow(verticalGuide, SW_HIDE);
        }
        if (horizontalGuide != nullptr) {
            ShowWindow(horizontalGuide, SW_HIDE);
        }
    }

    void ensureGuides() {
        const auto createGuide = [&]() {
            const auto window = CreateWindowExW(
                WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                guideClassName.c_str(),
                L"",
                WS_POPUP,
                0,
                0,
                1,
                1,
                nullptr,
                nullptr,
                module,
                nullptr);
            if (window == nullptr) {
                throw std::runtime_error("CreateWindowExW failed for alignment guide.");
            }
            SetLayeredWindowAttributes(window, 0, 210, LWA_ALPHA);
            return window;
        };
        if (verticalGuide == nullptr) {
            verticalGuide = createGuide();
        }
        if (horizontalGuide == nullptr) {
            horizontalGuide = createGuide();
        }
    }

    void updateInteractionGuides(HWND window, const RECT& windowRect) {
        auto* moved = findSurface(window);
        if (moved == nullptr || (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
            hideGuides();
            return;
        }
        const auto scale = moved->display.effectiveDpi / 96.0;
        const domain::PlacementRect proposed{
            .left = windowRect.left / scale - moved->display.workAreaLeft,
            .top = windowRect.top / scale - moved->display.workAreaTop,
            .width = (windowRect.right - windowRect.left) / scale,
            .height = (windowRect.bottom - windowRect.top) / scale,
        };
        std::vector<domain::PlacementRect> otherCards;
        for (const auto& surface : surfaces) {
            if (surface.window != window
                && surface.projection.displayId == moved->projection.displayId
                && surface.projection.placementId != moved->projection.placementId) {
                otherCards.push_back(surface.projection.rect);
            }
        }
        const auto result = presentation::ResolvePlacementInteractionDetailed(
            proposed,
            moved->display.workAreaWidth,
            moved->display.workAreaHeight,
            otherCards,
            false);
        ensureGuides();
        constexpr int guideThickness = 2;
        const auto workLeft = static_cast<int>(std::lround(moved->display.workAreaLeft * scale));
        const auto workTop = static_cast<int>(std::lround(moved->display.workAreaTop * scale));
        const auto workWidth = std::max(1, static_cast<int>(std::lround(
            moved->display.workAreaWidth * scale)));
        const auto workHeight = std::max(1, static_cast<int>(std::lround(
            moved->display.workAreaHeight * scale)));
        if (result.verticalGuide.has_value()) {
            const auto x = static_cast<int>(std::lround(
                (moved->display.workAreaLeft + *result.verticalGuide) * scale));
            SetWindowPos(
                verticalGuide,
                HWND_TOPMOST,
                x - guideThickness / 2,
                workTop,
                guideThickness,
                workHeight,
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
        } else {
            ShowWindow(verticalGuide, SW_HIDE);
        }
        if (result.horizontalGuide.has_value()) {
            const auto y = static_cast<int>(std::lround(
                (moved->display.workAreaTop + *result.horizontalGuide) * scale));
            SetWindowPos(
                horizontalGuide,
                HWND_TOPMOST,
                workLeft,
                y - guideThickness / 2,
                workWidth,
                guideThickness,
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
        } else {
            ShowWindow(horizontalGuide, SW_HIDE);
        }
    }

    bool beginCollapsePress(HWND window, int x, int y) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr || !isCollapseControlHit(*surface, x, y)) {
            return false;
        }
        surface->collapsePressed = true;
        SetCapture(window);
        try {
            render(*surface, surface->display, surface->card, surface->ordinal);
        } catch (...) {
            surface->collapsePressed = false;
            ReleaseCapture();
        }
        return true;
    }

    bool endCollapsePress(HWND window, int x, int y) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr || !surface->collapsePressed) {
            return false;
        }
        const auto commit = isCollapseControlHit(*surface, x, y);
        surface->collapsePressed = false;
        if (GetCapture() == window) {
            ReleaseCapture();
        }
        if (commit) {
            surface->card.expanded = !surface->card.expanded;
        }
        try {
            render(*surface, surface->display, surface->card, surface->ordinal);
        } catch (...) {
            return true;
        }
        if (commit && cardExpandedChanged) {
            try {
                cardExpandedChanged(surface->card.id, surface->card.expanded);
            } catch (...) {
                // Native window procedures must not allow callback exceptions to escape.
            }
        }
        return true;
    }

    void cancelCollapsePress(HWND window) noexcept {
        auto* surface = findSurface(window);
        if (surface == nullptr || !surface->collapsePressed) {
            return;
        }
        surface->collapsePressed = false;
        try {
            render(*surface, surface->display, surface->card, surface->ordinal);
        } catch (...) {
        }
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
        (void)ordinal;
        const auto darkSurface = card.appearancePreset == "black"
            || card.appearancePreset == "dark";
        const auto pearlSurface = card.appearancePreset == "pearl-pink"
            || card.appearancePreset == "jewel";
        const auto visibleBottom = card.expanded
            ? surface.height
            : std::min(surface.height, dipToPixels(48.0, surface));
        surface.interactiveHeight = visibleBottom;
        const auto radius = std::min(
            static_cast<int>(std::lround(card.cornerRadius * display.effectiveDpi / 96.0)),
            std::min(surface.width, visibleBottom) / 2);

        for (int y = 0; y < surface.height; ++y) {
            for (int x = 0; x < surface.width; ++x) {
                std::uint32_t red = darkSurface ? 31u : 248u;
                std::uint32_t green = darkSurface ? 33u : 250u;
                std::uint32_t blue = darkSurface ? 38u : 252u;
                if (pearlSurface) {
                    const auto horizontal = surface.width <= 1
                        ? 0.0
                        : static_cast<double>(x) / (surface.width - 1);
                    const auto vertical = visibleBottom <= 1
                        ? 0.0
                        : static_cast<double>(std::min(y, visibleBottom - 1))
                            / (visibleBottom - 1);
                    const auto amethyst = std::exp(-(
                        std::pow(horizontal - 0.12, 2.0) / 0.11
                        + std::pow(vertical - 0.15, 2.0) / 0.32));
                    const auto aquamarine = std::exp(-(
                        std::pow(horizontal - 0.88, 2.0) / 0.16
                        + std::pow(vertical - 0.28, 2.0) / 0.24));
                    const auto tourmaline = std::exp(-(
                        std::pow(horizontal - 0.22, 2.0) / 0.18
                        + std::pow(vertical - 0.88, 2.0) / 0.20));
                    const auto amber = std::exp(-(
                        std::pow(horizontal - 0.78, 2.0) / 0.22
                        + std::pow(vertical - 0.82, 2.0) / 0.18));
                    const auto diagonalSheen = std::exp(-std::pow(
                        horizontal * 0.82 + vertical * 0.58 - 0.72,
                        2.0) / 0.012);
                    const auto channel = [&](double base, double a, double q, double t, double g) {
                        return static_cast<std::uint32_t>(std::lround(std::clamp(
                            base + a * amethyst + q * aquamarine
                                + t * tourmaline + g * amber
                                + 13.0 * diagonalSheen,
                            0.0,
                            255.0)));
                    };
                    red = channel(226.0, 7.0, -28.0, 23.0, 24.0);
                    green = channel(225.0, -28.0, 21.0, -22.0, 12.0);
                    blue = channel(236.0, 17.0, 14.0, 10.0, -29.0);
                }
                surface.pixels[y * surface.width + x] = (red << 16) | (green << 8) | blue;
            }
        }

        for (int y = 0; y < surface.height; ++y) {
            if (y >= visibleBottom) {
                std::fill(
                    surface.pixels + y * surface.width,
                    surface.pixels + (y + 1) * surface.width,
                    0u);
            }
        }

        const auto text = card.showTitle ? card.title : L"";
        SetBkMode(surface.memoryDc, TRANSPARENT);
        const auto foreground = darkSurface ? RGB(244, 246, 249) : RGB(38, 40, 45);
        SetTextColor(surface.memoryDc, foreground);
        const auto font = CreateFontW(
            -dipToPixels(14.0, surface),
            0,
            0,
            0,
            FW_SEMIBOLD,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI Variable Text");
        const auto previousFont = font == nullptr
            ? nullptr
            : SelectObject(surface.memoryDc, font);
        const auto textLeft = dipToPixels(14.0, surface);
        const auto control = collapseControlRect(surface);
        RECT textRect{
            textLeft,
            0,
            card.showCollapseControl ? control.left - dipToPixels(4.0, surface)
                                     : surface.width - textLeft,
            std::min(visibleBottom, dipToPixels(48.0, surface)),
        };
        DrawTextW(
            surface.memoryDc,
            text.c_str(),
            -1,
            &textRect,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (previousFont != nullptr) {
            SelectObject(surface.memoryDc, previousFont);
        }
        if (font != nullptr) {
            DeleteObject(font);
        }

        const auto blendRgb = [&](int x, int y, std::uint32_t color, double coverage) {
            if (x < 0 || y < 0 || x >= surface.width || y >= visibleBottom || coverage <= 0.0) {
                return;
            }
            auto& pixel = surface.pixels[y * surface.width + x];
            const auto amount = std::clamp(coverage, 0.0, 1.0);
            const auto mix = [&](int shift) {
                const auto background = static_cast<double>((pixel >> shift) & 0xFFu);
                const auto foregroundChannel = static_cast<double>((color >> shift) & 0xFFu);
                return static_cast<std::uint32_t>(std::lround(
                    background + (foregroundChannel - background) * amount));
            };
            pixel = (mix(16) << 16) | (mix(8) << 8) | mix(0);
        };
        if (card.showCollapseControl) {
            const auto centerX = (control.left + control.right) / 2.0;
            const auto centerY = (control.top + control.bottom) / 2.0;
            const auto scale = display.effectiveDpi / 96.0;
            if (surface.collapsePressed) {
                const auto pressRadius = 14.0 * scale;
                for (int y = control.top; y < control.bottom; ++y) {
                    for (int x = control.left; x < control.right; ++x) {
                        const auto dx = x + 0.5 - centerX;
                        const auto dy = y + 0.5 - centerY;
                        const auto coverage = std::clamp(
                            pressRadius + 0.5 - std::sqrt(dx * dx + dy * dy),
                            0.0,
                            1.0);
                        blendRgb(x, y, darkSurface ? 0x00FFFFFFu : 0x00000000u,
                                 coverage * 0.08);
                    }
                }
            }
            const auto chevronColor = darkSurface ? 0x00E5E8EDu : 0x005B6069u;
            const auto halfWidth = 5.0 * scale;
            const auto halfHeight = 2.75 * scale;
            const auto direction = card.expanded ? -1.0 : 1.0;
            const auto middleY = centerY + direction * halfHeight;
            const auto sideY = centerY - direction * halfHeight;
            const auto strokeRadius = std::max(0.75, 0.8 * scale);
            const auto drawSegment = [&](double x1, double y1, double x2, double y2) {
                const auto vx = x2 - x1;
                const auto vy = y2 - y1;
                const auto lengthSquared = vx * vx + vy * vy;
                const auto minX = static_cast<int>(std::floor(std::min(x1, x2) - strokeRadius - 1));
                const auto maxX = static_cast<int>(std::ceil(std::max(x1, x2) + strokeRadius + 1));
                const auto minY = static_cast<int>(std::floor(std::min(y1, y2) - strokeRadius - 1));
                const auto maxY = static_cast<int>(std::ceil(std::max(y1, y2) + strokeRadius + 1));
                for (int y = minY; y <= maxY; ++y) {
                    for (int x = minX; x <= maxX; ++x) {
                        const auto px = x + 0.5;
                        const auto py = y + 0.5;
                        const auto projection = std::clamp(
                            ((px - x1) * vx + (py - y1) * vy) / lengthSquared,
                            0.0,
                            1.0);
                        const auto dx = px - (x1 + projection * vx);
                        const auto dy = py - (y1 + projection * vy);
                        const auto coverage = std::clamp(
                            strokeRadius + 0.5 - std::sqrt(dx * dx + dy * dy),
                            0.0,
                            1.0);
                        blendRgb(x, y, chevronColor, coverage);
                    }
                }
            };
            drawSegment(centerX - halfWidth, sideY, centerX, middleY);
            drawSegment(centerX, middleY, centerX + halfWidth, sideY);
        }

        const auto surfaceAlpha = std::clamp(card.opacity, 0.0, 1.0) * 255.0;
        const auto halfWidth = surface.width / 2.0;
        const auto halfHeight = visibleBottom / 2.0;
        for (int y = 0; y < visibleBottom; ++y) {
            for (int x = 0; x < surface.width; ++x) {
                double coverage = 1.0;
                if (radius > 0) {
                    const auto qx = std::abs(x + 0.5 - halfWidth) - (halfWidth - radius);
                    const auto qy = std::abs(y + 0.5 - halfHeight) - (halfHeight - radius);
                    const auto outsideX = std::max(qx, 0.0);
                    const auto outsideY = std::max(qy, 0.0);
                    const auto signedDistance = std::sqrt(
                        outsideX * outsideX + outsideY * outsideY)
                        + std::min(std::max(qx, qy), 0.0) - radius;
                    coverage = std::clamp(0.5 - signedDistance, 0.0, 1.0);
                }
                const auto alpha = static_cast<std::uint32_t>(std::lround(
                    surfaceAlpha * coverage));
                auto& pixel = surface.pixels[y * surface.width + x];
                const auto red = ((pixel >> 16) & 0xFFu) * alpha / 255u;
                const auto green = ((pixel >> 8) & 0xFFu) * alpha / 255u;
                const auto blue = (pixel & 0xFFu) * alpha / 255u;
                pixel = (alpha << 24) | (red << 16) | (green << 8) | blue;
            }
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

void WindowsDesktopHost::setCardExpandedChangedCallback(CardExpandedChangedCallback callback) {
    impl_->cardExpandedChanged = std::move(callback);
}

} // namespace desto::platform::windows
