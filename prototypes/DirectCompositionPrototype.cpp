#include "ApplicationRuntime.h"
#include "WindowsDisplayTopology.h"

#include <Windows.h>
#include <d2d1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#undef max
#undef min

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwrite.lib")

using Microsoft::WRL::ComPtr;
using namespace desto::application;
using namespace desto::domain;
using namespace desto::platform::windows;

namespace {

struct Surface {
    HWND window = nullptr;
    PlacementProjection projection;
    int width = 0;
    int height = 0;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> visual;
    ComPtr<IDCompositionSurface> content;
};

class DirectCompositionHost {
public:
    DirectCompositionHost() = default;
    ~DirectCompositionHost() {
        for (const auto& surface : surfaces_) {
            if (surface.window != nullptr) {
                DestroyWindow(surface.window);
            }
        }
        if (module_ != nullptr) {
            UnregisterClassW(className_.c_str(), module_);
        }
    }

    DirectCompositionHost(const DirectCompositionHost&) = delete;
    DirectCompositionHost& operator=(const DirectCompositionHost&) = delete;

    void show(
        const std::vector<PlacementProjection>& projections,
        const std::vector<DisplaySnapshot>& displays) {
        InitializeGraphics();
        module_ = GetModuleHandleW(nullptr);
        className_ = L"DestoDirectCompositionPrototypeSurface";
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = &WindowProcedure;
        windowClass.hInstance = module_;
        windowClass.lpszClassName = className_.c_str();
        windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(IDC_ARROW));
        if (RegisterClassW(&windowClass) == 0) {
            throw std::runtime_error("RegisterClassW failed for DirectComposition prototype.");
        }

        textFormat_.Reset();
        Check(dwriteFactory_->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            16.0f,
            L"",
            &textFormat_),
            "CreateTextFormat failed.");

        surfaces_.reserve(projections.size());
        for (const auto& projection : projections) {
            const auto display = std::find_if(
                displays.begin(), displays.end(), [&](const DisplaySnapshot& candidate) {
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
                WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                className_.c_str(),
                L"Desto DirectComposition Prototype",
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
                throw std::runtime_error("CreateWindowExW failed for DirectComposition surface.");
            }
            Check(compositionDevice_->CreateTargetForHwnd(
                      stored.window, TRUE, &stored.target),
                "CreateTargetForHwnd failed.");
            Check(compositionDevice_->CreateVisual(&stored.visual), "CreateVisual failed.");
            Check(compositionDevice_->CreateSurface(
                      stored.width,
                      stored.height,
                      DXGI_FORMAT_B8G8R8A8_UNORM,
                      DXGI_ALPHA_MODE_PREMULTIPLIED,
                      &stored.content),
                "CreateSurface failed.");
            Check(stored.visual->SetContent(stored.content.Get()), "SetContent failed.");
            Check(stored.target->SetRoot(stored.visual.Get()), "SetRoot failed.");
            Render(stored, *display, surfaces_.size());
        }
        Check(compositionDevice_->Commit(), "Initial composition commit failed.");

        auto deferred = BeginDeferWindowPos(static_cast<int>(surfaces_.size()));
        if (deferred == nullptr) {
            throw std::runtime_error("BeginDeferWindowPos failed for DirectComposition prototype.");
        }
        for (const auto& surface : surfaces_) {
            const auto display = std::find_if(
                displays.begin(), displays.end(), [&](const DisplaySnapshot& candidate) {
                    return candidate.id == surface.projection.displayId;
                });
            const auto scale = display->effectiveDpi / 96.0;
            const auto left = static_cast<int>(std::lround(display->workAreaLeft * scale
                + surface.projection.rect.left * scale));
            const auto top = static_cast<int>(std::lround(display->workAreaTop * scale
                + surface.projection.rect.top * scale));
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
                throw std::runtime_error("DeferWindowPos failed for DirectComposition prototype.");
            }
        }
        if (!EndDeferWindowPos(deferred)) {
            throw std::runtime_error("EndDeferWindowPos failed for DirectComposition prototype.");
        }
    }

private:
    static void Check(HRESULT result, const char* message) {
        if (FAILED(result)) {
            throw std::runtime_error(message);
        }
    }

    void InitializeGraphics() {
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL featureLevel{};
        const auto hardwareResult = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            &d3dDevice_,
            &featureLevel,
            &d3dContext_);
        if (FAILED(hardwareResult)) {
            Check(D3D11CreateDevice(
                      nullptr,
                      D3D_DRIVER_TYPE_WARP,
                      nullptr,
                      flags,
                      nullptr,
                      0,
                      D3D11_SDK_VERSION,
                      &d3dDevice_,
                      &featureLevel,
                      &d3dContext_),
                "D3D11CreateDevice failed for hardware and WARP.");
        }
        ComPtr<IDXGIDevice> dxgiDevice;
        Check(d3dDevice_.As(&dxgiDevice), "Query IDXGIDevice failed.");
        Check(DCompositionCreateDevice(
                  dxgiDevice.Get(),
                  __uuidof(IDCompositionDevice),
                  reinterpret_cast<void**>(compositionDevice_.GetAddressOf())),
            "DCompositionCreateDevice failed.");
        Check(D2D1CreateFactory(
                  D2D1_FACTORY_TYPE_SINGLE_THREADED,
                  d2dFactory_.GetAddressOf()),
            "D2D1CreateFactory failed.");
        Check(DWriteCreateFactory(
                  DWRITE_FACTORY_TYPE_SHARED,
                  __uuidof(IDWriteFactory),
                  reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf())),
            "DWriteCreateFactory failed.");
    }

    void Render(const Surface& surface, const DisplaySnapshot& display, std::size_t ordinal) {
        ComPtr<IDXGISurface> dxgiSurface;
        POINT offset{};
        Check(surface.content->BeginDraw(
                  nullptr,
                  __uuidof(IDXGISurface),
                  reinterpret_cast<void**>(dxgiSurface.GetAddressOf()),
                  &offset),
            "IDCompositionSurface::BeginDraw failed.");
        D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        ComPtr<ID2D1RenderTarget> renderTarget;
        const auto renderResult = d2dFactory_->CreateDxgiSurfaceRenderTarget(
            dxgiSurface.Get(), &properties, &renderTarget);
        if (FAILED(renderResult)) {
            surface.content->EndDraw();
            throw std::runtime_error("CreateDxgiSurfaceRenderTarget failed.");
        }
        renderTarget->BeginDraw();
        renderTarget->Clear(D2D1::ColorF(0, 0.0f));
        ComPtr<ID2D1SolidColorBrush> fill;
        Check(renderTarget->CreateSolidColorBrush(
                  D2D1::ColorF(0.09f + static_cast<float>(ordinal % 4) * 0.08f,
                               0.48f,
                               0.85f,
                               0.94f),
                  fill.GetAddressOf()),
            "CreateSolidColorBrush failed.");
        renderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(2, 2, static_cast<float>(surface.width - 2),
                                          static_cast<float>(surface.height - 2)), 10, 10),
            fill.Get());
        ComPtr<ID2D1SolidColorBrush> border;
        Check(renderTarget->CreateSolidColorBrush(
                  D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.88f), border.GetAddressOf()),
            "CreateSolidColorBrush failed.");
        renderTarget->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(2, 2, static_cast<float>(surface.width - 2),
                                          static_cast<float>(surface.height - 2)), 10, 10),
            border.Get(), 2.0f);
        const auto title = L"Card " + std::to_wstring(ordinal)
            + L"  " + std::to_wstring(static_cast<int>(display.effectiveDpi)) + L" DPI";
        renderTarget->DrawText(
            title.c_str(),
            static_cast<UINT32>(title.size()),
            textFormat_.Get(),
            D2D1::RectF(14, 12, static_cast<float>(surface.width - 14), 42),
            border.Get());
        Check(renderTarget->EndDraw(), "ID2D1RenderTarget::EndDraw failed.");
        Check(surface.content->EndDraw(), "IDCompositionSurface::EndDraw failed.");
    }

    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(window, GWLP_USERDATA,
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

    HINSTANCE module_ = nullptr;
    std::wstring className_;
    ComPtr<ID3D11Device> d3dDevice_;
    ComPtr<ID3D11DeviceContext> d3dContext_;
    ComPtr<IDCompositionDevice> compositionDevice_;
    ComPtr<ID2D1Factory> d2dFactory_;
    ComPtr<IDWriteFactory> dwriteFactory_;
    ComPtr<IDWriteTextFormat> textFormat_;
    std::vector<Surface> surfaces_;
};

int DurationMilliseconds(std::wstring_view commandLine) {
    constexpr int defaultDuration = 15000;
    constexpr auto marker = std::wstring_view(L"--duration-ms ");
    const auto position = commandLine.find(marker);
    if (position == std::wstring_view::npos) {
        return defaultDuration;
    }
    try {
        return std::clamp(std::stoi(std::wstring(commandLine.substr(position + marker.size()))),
                          1000, 120000);
    } catch (...) {
        return defaultDuration;
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR commandLine, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comResult)) {
        MessageBoxA(nullptr, "CoInitializeEx failed.", "Desto DirectComposition Prototype", MB_OK);
        return 1;
    }
    try {
        WindowsDisplayTopology topology;
        const auto displays = topology.snapshot();
        ApplicationRuntime runtime;
        const std::vector<CardId> ids{"prototype-1", "prototype-2", "prototype-3", "prototype-4"};
        for (std::size_t index = 0; index < ids.size(); ++index) {
            const auto created = index % 3 == 0
                ? runtime.execute(CreateApplicationCard{ids[index], "cards/" + ids[index]})
                : index % 3 == 1 ? runtime.execute(CreateMappingCard{ids[index]})
                                 : runtime.execute(CreateTodoCard{ids[index]});
            if (created.status == CommandStatus::Rejected) {
                throw std::runtime_error("Unable to create prototype Card.");
            }
            const auto placement = runtime.execute(SetPlacement{{
                .id = "placement-" + ids[index],
                .cardId = ids[index],
                .target = DisplayTarget::all(),
                .rect = {40.0 + index * 40.0, 48.0 + index * 34.0, 320, 220},
                .zIndex = static_cast<std::int32_t>(index),
            }});
            if (placement.status == CommandStatus::Rejected) {
                throw std::runtime_error("Unable to create prototype Placement.");
            }
        }
        if (runtime.execute(UpdateDisplayTopology{displays}).status == CommandStatus::Rejected) {
            throw std::runtime_error("Unable to project prototype displays.");
        }
        DirectCompositionHost host;
        host.show(runtime.projections(), displays);
        const auto timer = SetTimer(nullptr, 1, DurationMilliseconds(commandLine), nullptr);
        if (timer == 0) {
            throw std::runtime_error("SetTimer failed for DirectComposition prototype.");
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
        CoUninitialize();
        return 0;
    } catch (const std::exception& error) {
        MessageBoxA(nullptr, error.what(), "Desto DirectComposition Prototype", MB_ICONERROR | MB_OK);
        CoUninitialize();
        return 1;
    }
}
