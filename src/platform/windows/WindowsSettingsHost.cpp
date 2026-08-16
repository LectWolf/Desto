#include "WindowsSettingsHost.h"

#include <Windows.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#undef max
#undef min

namespace desto::platform::windows {
namespace {

enum class SettingsActionKind {
    None,
    SelectCard,
    WhiteAppearance,
    DarkAppearance,
    JewelAppearance,
    SmallItems,
    MediumItems,
    LargeItems,
    ExtraLargeItems,
    ToggleItemNames,
    ToggleCollapseControl,
    ToggleCreatedTime,
    RestoreArchived,
};

struct SettingsAction {
    SettingsActionKind kind = SettingsActionKind::None;
    std::size_t cardIndex = 0;

    bool operator==(const SettingsAction&) const = default;
};

RECT Rect(int left, int top, int right, int bottom) noexcept {
    return {left, top, right, bottom};
}

bool Contains(const RECT& rect, int x, int y) noexcept {
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

std::wstring CardTypeName(domain::CardType type) {
    switch (type) {
    case domain::CardType::Application: return L"应用卡片";
    case domain::CardType::Mapping: return L"映射卡片";
    case domain::CardType::Todo: return L"待办卡片";
    }
    return L"卡片";
}

domain::CardChromePreferences ChromePreferences(const presentation::CardView& card) {
    return {
        .showCollapseControl = card.showCollapseControl,
        .showCloseControl = card.showCloseControl,
        .showPinControl = card.showPinControl,
        .showTitle = card.showTitle,
    };
}

domain::CardAppearancePreferences AppearancePreferences(
    const presentation::CardView& card) {
    return {
        .preset = card.appearancePreset,
        .opacity = card.opacity,
        .cornerRadius = card.cornerRadius,
    };
}

HFONT CreateUiFont(int pixels, int weight = FW_NORMAL) {
    return CreateFontW(
        -pixels, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
}

void DrawLabel(HDC dc, std::wstring_view text, RECT rect, COLORREF color,
               int pixels, int weight = FW_NORMAL, UINT flags = DT_LEFT | DT_VCENTER) {
    const auto font = CreateUiFont(pixels, weight);
    const auto previous = font == nullptr ? nullptr : SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text.data(), static_cast<int>(text.size()), &rect,
        flags | DT_SINGLELINE | DT_NOPREFIX);
    if (previous != nullptr) SelectObject(dc, previous);
    if (font != nullptr) DeleteObject(font);
}

void FillRounded(HDC dc, RECT rect, COLORREF color, int radius) {
    const auto brush = CreateSolidBrush(color);
    const auto previousBrush = brush == nullptr ? nullptr : SelectObject(dc, brush);
    const auto previousPen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    if (previousPen != nullptr) SelectObject(dc, previousPen);
    if (previousBrush != nullptr) SelectObject(dc, previousBrush);
    if (brush != nullptr) DeleteObject(brush);
}

void DrawSwitch(HDC dc, RECT rect, bool enabled, bool hovered, bool pressed) {
    const auto background = enabled
        ? (pressed ? RGB(41, 96, 190) : hovered ? RGB(54, 112, 214) : RGB(62, 122, 226))
        : (pressed ? RGB(188, 192, 199) : hovered ? RGB(203, 207, 213) : RGB(216, 219, 224));
    FillRounded(dc, rect, background, rect.bottom - rect.top);
    const auto diameter = rect.bottom - rect.top - 6;
    const auto left = enabled ? rect.right - diameter - 3 : rect.left + 3;
    RECT knob{left, rect.top + 3, left + diameter, rect.bottom - 3};
    FillRounded(dc, knob, RGB(255, 255, 255), diameter);
}

} // namespace

struct WindowsSettingsHost::Impl {
    explicit Impl(std::wstring windowTitle)
        : title(std::move(windowTitle)), module(GetModuleHandleW(nullptr)) {
        WNDCLASSEXW windowClass{
            .cbSize = sizeof(WNDCLASSEXW),
            .style = CS_HREDRAW | CS_VREDRAW,
            .lpfnWndProc = &WindowProcedure,
            .hInstance = module,
            .hCursor = LoadCursorA(nullptr, IDC_ARROW),
            .hbrBackground = nullptr,
            .lpszClassName = className,
        };
        if (RegisterClassExW(&windowClass) == 0
            && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            throw std::runtime_error("Unable to register settings window class.");
        }
        window = CreateWindowExW(
            0, className, title.c_str(), WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 920, 640,
            nullptr, nullptr, module, this);
        if (window == nullptr) throw std::runtime_error("Unable to create settings window.");
    }

    ~Impl() {
        if (window != nullptr) DestroyWindow(window);
    }

    static LRESULT CALLBACK WindowProcedure(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept {
        auto* instance = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            instance = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(instance));
        }
        if (instance == nullptr) return DefWindowProcW(window, message, wParam, lParam);
        switch (message) {
        case WM_PAINT:
            instance->paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEMOVE:
            instance->updateHover(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_MOUSELEAVE:
            instance->hovered = {};
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            instance->beginPress(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_LBUTTONUP:
            instance->endPress(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_CAPTURECHANGED:
            instance->pressed = {};
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                POINT point{};
                GetCursorPos(&point);
                ScreenToClient(window, &point);
                if (instance->actionAt(point.x, point.y).kind != SettingsActionKind::None) {
                    SetCursor(LoadCursorA(nullptr, IDC_HAND));
                    return TRUE;
                }
            }
            break;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize = {760, 540};
            return 0;
        }
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                ShowWindow(window, SW_HIDE);
                return 0;
            }
            break;
        case WM_CLOSE:
            ShowWindow(window, SW_HIDE);
            return 0;
        case WM_DESTROY:
            instance->window = nullptr;
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    RECT cardRow(std::size_t index) const noexcept {
        return Rect(16, 104 + static_cast<int>(index) * 56, 216,
            154 + static_cast<int>(index) * 56);
    }

    std::array<RECT, 3> appearanceRects() const noexcept {
        return {Rect(270, 168, 318, 216), Rect(334, 168, 382, 216),
                Rect(398, 168, 446, 216)};
    }

    std::array<RECT, 4> itemSizeRects() const noexcept {
        return {Rect(270, 294, 334, 332), Rect(338, 294, 402, 332),
                Rect(406, 294, 470, 332), Rect(474, 294, 548, 332)};
    }

    RECT itemNamesRect() const noexcept { return Rect(270, 360, clientRight() - 32, 406); }
    RECT collapseRect() const noexcept { return Rect(270, 414, clientRight() - 32, 460); }
    RECT createdTimeRect() const noexcept { return Rect(270, 294, clientRight() - 32, 340); }
    RECT restoreRect() const noexcept { return Rect(270, 366, 412, 406); }

    int clientRight() const noexcept {
        RECT client{};
        if (window != nullptr) GetClientRect(window, &client);
        return std::max(760L, client.right);
    }

    SettingsAction actionAt(int x, int y) const noexcept {
        for (std::size_t index = 0; index < cards.size(); ++index) {
            if (Contains(cardRow(index), x, y)) {
                return {SettingsActionKind::SelectCard, index};
            }
        }
        if (cards.empty() || selectedCard >= cards.size()) return {};
        const auto appearance = appearanceRects();
        const std::array appearanceKinds{
            SettingsActionKind::WhiteAppearance,
            SettingsActionKind::DarkAppearance,
            SettingsActionKind::JewelAppearance,
        };
        for (std::size_t index = 0; index < appearance.size(); ++index) {
            if (Contains(appearance[index], x, y)) return {appearanceKinds[index], selectedCard};
        }
        const auto& card = cards[selectedCard];
        if (card.type == domain::CardType::Application
            || card.type == domain::CardType::Mapping) {
            const auto sizes = itemSizeRects();
            const std::array sizeKinds{
                SettingsActionKind::SmallItems,
                SettingsActionKind::MediumItems,
                SettingsActionKind::LargeItems,
                SettingsActionKind::ExtraLargeItems,
            };
            for (std::size_t index = 0; index < sizes.size(); ++index) {
                if (Contains(sizes[index], x, y)) return {sizeKinds[index], selectedCard};
            }
            if (Contains(itemNamesRect(), x, y)) {
                return {SettingsActionKind::ToggleItemNames, selectedCard};
            }
            if (Contains(collapseRect(), x, y)) {
                return {SettingsActionKind::ToggleCollapseControl, selectedCard};
            }
        } else if (card.type == domain::CardType::Todo) {
            if (Contains(createdTimeRect(), x, y)) {
                return {SettingsActionKind::ToggleCreatedTime, selectedCard};
            }
            if (Contains(restoreRect(), x, y)) {
                return {SettingsActionKind::RestoreArchived, selectedCard};
            }
        }
        return {};
    }

    void updateHover(int x, int y) noexcept {
        TRACKMOUSEEVENT tracking{
            .cbSize = sizeof(TRACKMOUSEEVENT),
            .dwFlags = TME_LEAVE,
            .hwndTrack = window,
        };
        TrackMouseEvent(&tracking);
        const auto next = actionAt(x, y);
        if (next == hovered) return;
        hovered = next;
        InvalidateRect(window, nullptr, FALSE);
    }

    void beginPress(int x, int y) noexcept {
        pressed = actionAt(x, y);
        hovered = pressed;
        if (pressed.kind != SettingsActionKind::None) SetCapture(window);
        InvalidateRect(window, nullptr, FALSE);
    }

    void endPress(int x, int y) noexcept {
        const auto action = actionAt(x, y);
        const auto commit = action == pressed ? action : SettingsAction{};
        pressed = {};
        if (GetCapture() == window) ReleaseCapture();
        if (commit.kind != SettingsActionKind::None) apply(commit);
        InvalidateRect(window, nullptr, FALSE);
    }

    void apply(const SettingsAction& action) noexcept {
        if (action.kind == SettingsActionKind::SelectCard) {
            selectedCard = action.cardIndex;
            return;
        }
        if (action.cardIndex >= cards.size()) return;
        auto& card = cards[action.cardIndex];
        try {
            if (action.kind == SettingsActionKind::WhiteAppearance
                || action.kind == SettingsActionKind::DarkAppearance
                || action.kind == SettingsActionKind::JewelAppearance) {
                auto preferences = AppearancePreferences(card);
                preferences.preset = action.kind == SettingsActionKind::WhiteAppearance
                    ? "white" : action.kind == SettingsActionKind::DarkAppearance
                    ? "black" : "jewel";
                if (appearanceChanged && appearanceChanged(card.id, preferences)) {
                    card.appearancePreset = preferences.preset;
                }
                return;
            }
            if (action.kind >= SettingsActionKind::SmallItems
                && action.kind <= SettingsActionKind::ExtraLargeItems) {
                auto preferences = card.content;
                preferences.itemSize = action.kind == SettingsActionKind::SmallItems
                    ? domain::CardItemSize::Small
                    : action.kind == SettingsActionKind::MediumItems
                    ? domain::CardItemSize::Medium
                    : action.kind == SettingsActionKind::LargeItems
                    ? domain::CardItemSize::Large
                    : domain::CardItemSize::ExtraLarge;
                if (contentChanged && contentChanged(card.id, preferences)) {
                    card.content = preferences;
                }
                return;
            }
            if (action.kind == SettingsActionKind::ToggleItemNames) {
                auto preferences = card.content;
                preferences.showItemNames = !preferences.showItemNames;
                if (contentChanged && contentChanged(card.id, preferences)) card.content = preferences;
                return;
            }
            if (action.kind == SettingsActionKind::ToggleCollapseControl) {
                auto preferences = ChromePreferences(card);
                preferences.showCollapseControl = !preferences.showCollapseControl;
                if (chromeChanged && chromeChanged(card.id, preferences)) {
                    card.showCollapseControl = preferences.showCollapseControl;
                }
                return;
            }
            if (action.kind == SettingsActionKind::ToggleCreatedTime) {
                auto preferences = card.todoPreferences;
                preferences.showCreatedTime = !preferences.showCreatedTime;
                if (todoPreferencesChanged
                    && todoPreferencesChanged(card.id, preferences)) {
                    card.todoPreferences = preferences;
                }
                return;
            }
            if (action.kind == SettingsActionKind::RestoreArchived
                && restoreArchived && restoreArchived(card.id)) {
                for (auto& item : card.todoItems) item.archived = false;
            }
        } catch (...) {
        }
    }

    void paint() noexcept {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        const auto memory = CreateCompatibleDC(dc);
        const auto bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
        const auto previousBitmap = bitmap == nullptr ? nullptr : SelectObject(memory, bitmap);
        const auto canvas = memory == nullptr || bitmap == nullptr ? dc : memory;
        FillRect(canvas, &client, GetSysColorBrush(COLOR_WINDOW));

        RECT sidebar{0, 0, 232, client.bottom};
        const auto sidebarBrush = CreateSolidBrush(RGB(246, 247, 249));
        FillRect(canvas, &sidebar, sidebarBrush);
        DeleteObject(sidebarBrush);
        RECT divider{231, 0, 232, client.bottom};
        const auto dividerBrush = CreateSolidBrush(RGB(226, 228, 232));
        FillRect(canvas, &divider, dividerBrush);
        DeleteObject(dividerBrush);

        DrawLabel(canvas, L"Desto", Rect(24, 20, 210, 52), RGB(28, 31, 37), 23, FW_SEMIBOLD);
        DrawLabel(canvas, L"卡片与偏好", Rect(24, 56, 210, 82), RGB(118, 123, 132), 12);
        for (std::size_t index = 0; index < cards.size(); ++index) {
            auto row = cardRow(index);
            const auto selected = index == selectedCard;
            const auto hot = hovered == SettingsAction{SettingsActionKind::SelectCard, index};
            const auto down = pressed == SettingsAction{SettingsActionKind::SelectCard, index};
            if (selected || hot || down) {
                FillRounded(canvas, row,
                    down ? RGB(220, 226, 236) : selected ? RGB(229, 234, 243) : RGB(236, 239, 244),
                    10);
            }
            auto dot = Rect(row.left + 12, row.top + 17, row.left + 28, row.top + 33);
            FillRounded(canvas, dot,
                cards[index].type == domain::CardType::Todo ? RGB(90, 127, 196)
                    : cards[index].type == domain::CardType::Mapping ? RGB(106, 139, 115)
                    : RGB(154, 112, 164), 16);
            DrawLabel(canvas, CardTypeName(cards[index].type),
                Rect(row.left + 38, row.top, row.right - 8, row.bottom),
                RGB(48, 52, 60), 13, selected ? FW_SEMIBOLD : FW_NORMAL);
        }

        if (!cards.empty() && selectedCard < cards.size()) paintSelected(canvas, client);

        if (canvas == memory) {
            BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
        }
        if (previousBitmap != nullptr) SelectObject(memory, previousBitmap);
        if (bitmap != nullptr) DeleteObject(bitmap);
        if (memory != nullptr) DeleteDC(memory);
        EndPaint(window, &paint);
    }

    void paintSelected(HDC dc, const RECT& client) noexcept {
        const auto& card = cards[selectedCard];
        DrawLabel(dc, CardTypeName(card.type), Rect(270, 24, client.right - 32, 58),
            RGB(29, 32, 38), 22, FW_SEMIBOLD);
        DrawLabel(dc, L"此处的更改只作用于当前卡片", Rect(270, 58, client.right - 32, 84),
            RGB(117, 122, 131), 12);
        DrawLabel(dc, L"外观", Rect(270, 116, 420, 146), RGB(53, 57, 65), 14, FW_SEMIBOLD);

        const auto swatches = appearanceRects();
        const std::array kinds{
            SettingsActionKind::WhiteAppearance,
            SettingsActionKind::DarkAppearance,
            SettingsActionKind::JewelAppearance,
        };
        const std::array labels{L"纯白", L"透明黑", L"珠宝"};
        for (std::size_t index = 0; index < swatches.size(); ++index) {
            auto outer = swatches[index];
            const auto action = SettingsAction{kinds[index], selectedCard};
            if (hovered == action || pressed == action) {
                auto hotspot = outer;
                InflateRect(&hotspot, 5, 5);
                FillRounded(dc, hotspot,
                    pressed == action ? RGB(222, 226, 233) : RGB(236, 238, 242), 12);
            }
            if (index == 0) {
                FillRounded(dc, outer, RGB(252, 252, 253), 10);
                const auto pen = CreatePen(PS_SOLID, 1, RGB(207, 211, 218));
                const auto previousPen = SelectObject(dc, pen);
                const auto previousBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
                RoundRect(dc, outer.left, outer.top, outer.right, outer.bottom, 10, 10);
                SelectObject(dc, previousBrush);
                SelectObject(dc, previousPen);
                DeleteObject(pen);
            } else if (index == 1) {
                FillRounded(dc, outer, RGB(42, 44, 50), 10);
            } else {
                const auto bandHeight = (outer.bottom - outer.top) / 4;
                const std::array colors{
                    RGB(246, 182, 218), RGB(167, 202, 242),
                    RGB(185, 225, 199), RGB(232, 205, 153),
                };
                const auto saved = SaveDC(dc);
                const auto region = CreateRoundRectRgn(
                    outer.left, outer.top, outer.right + 1, outer.bottom + 1, 10, 10);
                SelectClipRgn(dc, region);
                for (std::size_t band = 0; band < colors.size(); ++band) {
                    RECT strip{outer.left, outer.top + static_cast<int>(band) * bandHeight,
                        outer.right, band + 1 == colors.size() ? outer.bottom
                            : outer.top + static_cast<int>(band + 1) * bandHeight};
                    const auto brush = CreateSolidBrush(colors[band]);
                    FillRect(dc, &strip, brush);
                    DeleteObject(brush);
                }
                RestoreDC(dc, saved);
                DeleteObject(region);
            }
            const auto active = (index == 0 && (card.appearancePreset == "white"
                    || card.appearancePreset == "default"))
                || (index == 1 && (card.appearancePreset == "black"
                    || card.appearancePreset == "dark"))
                || (index == 2 && card.appearancePreset == "jewel");
            if (active) {
                const auto pen = CreatePen(PS_SOLID, 2, RGB(65, 116, 205));
                const auto previousPen = SelectObject(dc, pen);
                const auto previousBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
                auto ring = outer;
                InflateRect(&ring, 3, 3);
                RoundRect(dc, ring.left, ring.top, ring.right, ring.bottom, 13, 13);
                SelectObject(dc, previousBrush);
                SelectObject(dc, previousPen);
                DeleteObject(pen);
            }
            DrawLabel(dc, labels[index], Rect(outer.left - 4, outer.bottom + 7,
                outer.right + 4, outer.bottom + 28), RGB(112, 117, 126), 11,
                FW_NORMAL, DT_CENTER | DT_VCENTER);
        }

        if (card.type == domain::CardType::Application
            || card.type == domain::CardType::Mapping) {
            paintFileCardSettings(dc, card);
        } else if (card.type == domain::CardType::Todo) {
            paintTodoSettings(dc, card);
        }
    }

    void paintFileCardSettings(HDC dc, const presentation::CardView& card) noexcept {
        DrawLabel(dc, L"内容", Rect(270, 250, 420, 280), RGB(53, 57, 65), 14, FW_SEMIBOLD);
        DrawLabel(dc, L"图标大小", Rect(270, 270, 420, 292), RGB(119, 124, 133), 11);
        const auto rects = itemSizeRects();
        const std::array labels{L"小", L"中", L"大", L"特大"};
        const std::array values{
            domain::CardItemSize::Small, domain::CardItemSize::Medium,
            domain::CardItemSize::Large, domain::CardItemSize::ExtraLarge,
        };
        const std::array kinds{
            SettingsActionKind::SmallItems, SettingsActionKind::MediumItems,
            SettingsActionKind::LargeItems, SettingsActionKind::ExtraLargeItems,
        };
        for (std::size_t index = 0; index < rects.size(); ++index) {
            const auto action = SettingsAction{kinds[index], selectedCard};
            const auto active = card.content.itemSize == values[index];
            FillRounded(dc, rects[index],
                pressed == action ? RGB(214, 222, 235)
                    : active ? RGB(226, 232, 243)
                    : hovered == action ? RGB(239, 241, 245) : RGB(247, 248, 250), 9);
            DrawLabel(dc, labels[index], rects[index], active ? RGB(49, 91, 164) : RGB(83, 88, 98),
                12, active ? FW_SEMIBOLD : FW_NORMAL, DT_CENTER | DT_VCENTER);
        }
        paintToggleRow(dc, itemNamesRect(), L"显示文件名", card.content.showItemNames,
            SettingsActionKind::ToggleItemNames);
        paintToggleRow(dc, collapseRect(), L"显示收缩按钮", card.showCollapseControl,
            SettingsActionKind::ToggleCollapseControl);
    }

    void paintTodoSettings(HDC dc, const presentation::CardView& card) noexcept {
        DrawLabel(dc, L"待办", Rect(270, 250, 420, 280), RGB(53, 57, 65), 14, FW_SEMIBOLD);
        paintToggleRow(dc, createdTimeRect(), L"显示创建时间",
            card.todoPreferences.showCreatedTime, SettingsActionKind::ToggleCreatedTime);
        const auto restore = restoreRect();
        const auto action = SettingsAction{SettingsActionKind::RestoreArchived, selectedCard};
        FillRounded(dc, restore,
            pressed == action ? RGB(218, 223, 231)
                : hovered == action ? RGB(237, 239, 243) : RGB(247, 248, 250), 9);
        DrawLabel(dc, L"恢复已归档", restore, RGB(68, 73, 82), 12,
            FW_SEMIBOLD, DT_CENTER | DT_VCENTER);
    }

    void paintToggleRow(HDC dc, RECT rect, std::wstring_view label, bool enabled,
                        SettingsActionKind kind) noexcept {
        const auto action = SettingsAction{kind, selectedCard};
        if (hovered == action || pressed == action) {
            FillRounded(dc, rect,
                pressed == action ? RGB(237, 239, 243) : RGB(247, 248, 250), 9);
        }
        DrawLabel(dc, label, Rect(rect.left + 4, rect.top, rect.right - 76, rect.bottom),
            RGB(65, 69, 78), 13);
        RECT toggle{rect.right - 58, rect.top + 9, rect.right - 8, rect.bottom - 9};
        DrawSwitch(dc, toggle, enabled, hovered == action, pressed == action);
    }

    std::wstring title;
    HINSTANCE module = nullptr;
    HWND window = nullptr;
    std::vector<presentation::CardView> cards;
    std::size_t selectedCard = 0;
    SettingsAction hovered;
    SettingsAction pressed;
    AppearanceChangedCallback appearanceChanged;
    ContentChangedCallback contentChanged;
    ChromeChangedCallback chromeChanged;
    TodoPreferencesChangedCallback todoPreferencesChanged;
    RestoreArchivedCallback restoreArchived;
    static constexpr const wchar_t* className = L"DestoSettingsWindow";
};

WindowsSettingsHost::WindowsSettingsHost(std::wstring title)
    : impl_(std::make_unique<Impl>(std::move(title))) {}

WindowsSettingsHost::~WindowsSettingsHost() = default;

void WindowsSettingsHost::present(std::span<const presentation::CardView> cards) {
    impl_->cards.assign(cards.begin(), cards.end());
    if (impl_->selectedCard >= impl_->cards.size()) impl_->selectedCard = 0;
    InvalidateRect(impl_->window, nullptr, FALSE);
}

void WindowsSettingsHost::show() {
    ShowWindow(impl_->window, SW_SHOWNORMAL);
    SetForegroundWindow(impl_->window);
}

void WindowsSettingsHost::hide() noexcept {
    if (impl_->window != nullptr) ShowWindow(impl_->window, SW_HIDE);
}

void* WindowsSettingsHost::nativeHandle() const noexcept { return impl_->window; }

void WindowsSettingsHost::setAppearanceChangedCallback(AppearanceChangedCallback callback) {
    impl_->appearanceChanged = std::move(callback);
}

void WindowsSettingsHost::setContentChangedCallback(ContentChangedCallback callback) {
    impl_->contentChanged = std::move(callback);
}

void WindowsSettingsHost::setChromeChangedCallback(ChromeChangedCallback callback) {
    impl_->chromeChanged = std::move(callback);
}

void WindowsSettingsHost::setTodoPreferencesChangedCallback(
    TodoPreferencesChangedCallback callback) {
    impl_->todoPreferencesChanged = std::move(callback);
}

void WindowsSettingsHost::setRestoreArchivedCallback(RestoreArchivedCallback callback) {
    impl_->restoreArchived = std::move(callback);
}

} // namespace desto::platform::windows
