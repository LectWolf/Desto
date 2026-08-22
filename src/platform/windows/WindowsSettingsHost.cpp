#include "WindowsSettingsHost.h"
#include "WindowsDesktopHost.h"
#include "WindowsIconFont.h"
#include "WindowsTextInput.h"
#include "CardContentLayout.h"

#include <Windows.h>
#include <winhttp.h>
#include <CommCtrl.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <urlmon.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>
#include <regex>

#ifndef DESTO_VERSION_MAJOR
#define DESTO_VERSION_MAJOR 0
#define DESTO_VERSION_MINOR 1
#define DESTO_VERSION_PATCH 0
#define DESTO_VERSION_BUILD 0
#endif

#undef max
#undef min

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "urlmon.lib")

namespace desto::platform::windows {

std::uint32_t ResolveSettingsThemeColor(
    std::uint32_t color,
    bool darkMode) noexcept {
    if (darkMode) return color;
    const auto red = GetRValue(color);
    const auto green = GetGValue(color);
    const auto blue = GetBValue(color);
    const auto maximum = std::max({red, green, blue});
    const auto minimum = std::min({red, green, blue});
    if (maximum - minimum > 18) return color;
    if (red == 18 && green == 19 && blue == 21) return RGB(243, 243, 243);
    const auto average = (static_cast<unsigned>(red)
        + static_cast<unsigned>(green) + static_cast<unsigned>(blue)) / 3u;
    if (average <= 40u) return RGB(255, 255, 255);
    if (average <= 55u) return RGB(246, 246, 246);
    if (average <= 80u) return RGB(218, 218, 218);
    const auto gray = static_cast<BYTE>(255u - average);
    return RGB(gray, gray, gray);
}

FileCardSettingsLayout ResolveFileCardSettingsLayout(bool mappingCard) noexcept {
    FileCardSettingsLayout result{};
    // Keep every section derived from the previous section. This is the
    // single source of truth for the editor's vertical rhythm; type-specific
    // rows may be inserted without reintroducing overlapping fixed offsets.
    constexpr int sectionGap = 16;
    constexpr int labelToControlGap = 30;
    constexpr int controlHeight = 42;
    result.appearanceTop = 166;
    result.appearanceBottom = 204;
    result.toolbarLabelTop = result.appearanceBottom + sectionGap;
    result.toolbarTop = result.toolbarLabelTop + labelToControlGap;
    result.toolbarBottom = result.toolbarTop + controlHeight;
    auto cursor = result.toolbarBottom + sectionGap;
    if (mappingCard) {
        result.sourceLabelTop = cursor;
        result.sourceTop = result.sourceLabelTop + labelToControlGap;
        result.sourceBottom = result.sourceTop + 38;
        cursor = result.sourceBottom + sectionGap + 2;
    }
    result.optionsLabelTop = cursor;
    result.optionsTop = result.optionsLabelTop + labelToControlGap;
    result.optionsBottom = result.optionsTop + controlHeight;
    result.extraTop = result.optionsBottom + sectionGap;
    return result;
}

namespace {

std::wstring CurrentDestoVersion(bool development) {
    wchar_t value[64]{};
    if (development) {
        swprintf_s(value, L"%d.%d.%d.%d", DESTO_VERSION_MAJOR,
            DESTO_VERSION_MINOR, DESTO_VERSION_PATCH, DESTO_VERSION_BUILD);
    } else {
        swprintf_s(value, L"%d.%d.%d", DESTO_VERSION_MAJOR,
            DESTO_VERSION_MINOR, DESTO_VERSION_PATCH);
    }
    return value;
}

int CompareDestoVersions(std::wstring left, std::wstring right) noexcept {
    auto next = [](std::wstring& value) {
        const auto dot = value.find(L'.');
        const auto part = value.substr(0, dot);
        value = dot == std::wstring::npos ? L"" : value.substr(dot + 1);
        return _wtoi(part.c_str());
    };
    for (int i = 0; i < 4; ++i) {
        const auto l = next(left);
        const auto r = next(right);
        if (l != r) return l < r ? -1 : 1;
    }
    return 0;
}

std::optional<std::string> DownloadUpdateMetadata(const wchar_t* host,
    const wchar_t* path) {
    HINTERNET session = WinHttpOpen(L"Desto/0.1 update-check",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return std::nullopt;
    HINTERNET connection = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connection) { WinHttpCloseHandle(session); return std::nullopt; }
    HINTERNET request = WinHttpOpenRequest(connection, L"GET", path, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request || !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request, nullptr)) {
        if (request) WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection); WinHttpCloseHandle(session); return std::nullopt;
    }
    std::string body;
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
        std::string chunk(available, '\0'); DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read)) break;
        body.append(chunk.data(), read);
    }
    WinHttpCloseHandle(request); WinHttpCloseHandle(connection); WinHttpCloseHandle(session);
    return body;
}

std::wstring JsonStringField(const std::string& json, const char* field) {
    const std::regex pattern(std::string("\\\"") + field + "\\\"\\s*:\\s*\\\"([^\\\"]+)\\\"");
    std::smatch match;
    if (!std::regex_search(json, match, pattern) || match.size() < 2) return {};
    const auto text = match[1].str();
    return std::wstring(text.begin(), text.end());
}

std::optional<std::wstring> DownloadInstaller(const std::wstring& url) {
    wchar_t tempPath[MAX_PATH]{};
    const auto length = GetTempPathW(static_cast<DWORD>(std::size(tempPath)), tempPath);
    if (length == 0 || length >= std::size(tempPath)) return std::nullopt;
    wchar_t tempFile[MAX_PATH]{};
    if (GetTempFileNameW(tempPath, L"Desto", 0, tempFile) == 0) return std::nullopt;
    const std::wstring installerPath = std::wstring(tempFile) + L".exe";
    DeleteFileW(tempFile);
    if (FAILED(URLDownloadToFileW(nullptr, url.c_str(), installerPath.c_str(),
            0, nullptr))) {
        DeleteFileW(installerPath.c_str());
        return std::nullopt;
    }
    return installerPath;
}

bool SystemAppsUseDarkTheme() noexcept {
    DWORD lightTheme = 1;
    DWORD size = sizeof(lightTheme);
    return RegGetValueW(
               HKEY_CURRENT_USER,
               L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
               L"AppsUseLightTheme",
               RRF_RT_REG_DWORD,
               nullptr,
               &lightTheme,
               &size) == ERROR_SUCCESS
        && lightTheme == 0;
}

bool gSettingsDarkMode = SystemAppsUseDarkTheme();

COLORREF ThemeColor(COLORREF color) noexcept {
    return ResolveSettingsThemeColor(color, gSettingsDarkMode);
}

COLORREF ThemeSurfaceColor(COLORREF darkColor, COLORREF lightColor) noexcept {
    return gSettingsDarkMode ? darkColor : lightColor;
}

constexpr int kSidebarWidth = 154;
constexpr int kContentLeft = 184;
constexpr int kCardRailWidth = 52;
constexpr int kRenameEditId = 1001;
constexpr int kArchiveSearchEditId = 1002;
constexpr int kArchiveAddEditId = 1003;
constexpr int kDestoIconResourceId = 101;
constexpr COLORREF kAccent = RGB(51, 136, 255);
constexpr COLORREF kAccentHover = RGB(68, 148, 255);
// Keep press feedback close to the accent color. A dark block reads as a
// destructive state in the compact native controls, especially in light mode.
constexpr COLORREF kAccentPressed = RGB(78, 153, 255);
constexpr COLORREF kAccentOutline = RGB(117, 178, 255);
constexpr DWORD kDwmCaptionColorAttribute = 35;
constexpr DWORD kDwmTextColorAttribute = 36;

enum class SettingsPage {
    System,
    Features,
    Cards,
    Archive,
    About,
};

enum class SettingsActionKind {
    None,
    Navigate,
    AddCard,
    AddApplication,
    AddMapping,
    AddTodo,
    SelectCard,
    OpenCardMenu,
    DismissCardMenu,
    RenameCard,
    DeleteCard,
    ToggleCardVisibility,
    SystemAppearance,
    MicaDarkAppearance,
    MicaWhiteAppearance,
    BrandAppearance,
    TransparentAppearance,
    SmallItems,
    MediumItems,
    LargeItems,
    ExtraLargeItems,
    SmallCardWidth,
    MediumCardWidth,
    LargeCardWidth,
    ToggleApplicationSortMenu,
    SelectApplicationSort,
    DismissApplicationSortMenu,
    SelectMappingReferences,
    SelectMappingFolder,
    TogglePresentationControl,
    ToggleItemNames,
    ToggleCollapseControl,
    TogglePinControl,
    TogglePositionLock,
    ToggleSizeMode,
    DecreaseFixedColumns,
    IncreaseFixedColumns,
    DecreaseFixedRows,
    IncreaseFixedRows,
    ToggleHeightLimit,
    DecreaseMaximumVisibleRows,
    IncreaseMaximumVisibleRows,
    ToggleCreatedTime,
    GlobalCornerRadius,
    SelectTimeZone,
    SelectLanguage,
    SelectTimeZoneOption,
    SelectLanguageOption,
    DismissSystemDropdown,
    ChangeStorageRoot,
    ConfirmStorageRootChange,
    CancelStorageRootChange,
    ToggleRunAtStartup,
    SelectDesktopDoubleClick,
    SelectDesktopDoubleClickOption,
    ToggleTaskbarDesktop,
    TogglePinnedCardsYieldToFullscreen,
    ToggleIconBackgroundFrame,
    ToggleFileDeletionConfirmation,
    ToggleUpdateChannel,
    OpenProject,
    CheckForUpdates,
    PreviousArchiveDate,
    NextArchiveDate,
    ToggleArchiveCalendar,
    PreviousArchiveMonth,
    NextArchiveMonth,
    SelectArchiveDate,
    DismissArchiveCalendar,
    FocusArchiveSearch,
    RestoreArchivedItem,
    DeleteArchivedItem,
    ArchiveTodoItem,
    AddHistoricalArchive,
    ExportArchive,
    CycleHistoricalArchiveCard,
    ConfirmHistoricalArchive,
    SelectArchiveExportBegin,
    SelectArchiveExportEnd,
    PreviousArchiveExportMonth,
    NextArchiveExportMonth,
    SelectArchiveExportDate,
    CancelArchiveOverlay,
    ConfirmDeletion,
    CancelDeletion,
};

enum class ArchiveOverlay {
    None,
    Add,
    Export,
};

enum class ArchiveExportDateField {
    None,
    Begin,
    End,
};

enum class SystemDropdown {
    None,
    TimeZone,
    Language,
    DesktopDoubleClick,
};

struct SettingsAction {
    SettingsActionKind kind = SettingsActionKind::None;
    std::size_t index = 0;
    std::size_t secondaryIndex = 0;

    bool operator==(const SettingsAction&) const = default;
};

struct ArchivedEntry {
    std::size_t cardIndex = 0;
    std::size_t itemIndex = 0;
};

RECT Rect(int left, int top, int right, int bottom) noexcept {
    return {left, top, right, bottom};
}

bool Contains(const RECT& rect, int x, int y) noexcept {
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

bool ContainsInsensitive(std::wstring value, std::wstring query) {
    std::ranges::transform(value, value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    std::ranges::transform(query, query.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value.find(query) != std::wstring::npos;
}

std::wstring CardTypeName(domain::CardType type, bool english) {
    switch (type) {
    case domain::CardType::Application: return english ? L"Application card" : L"应用卡片";
    case domain::CardType::Mapping: return english ? L"Mapping card" : L"映射卡片";
    case domain::CardType::Todo: return english ? L"Task card" : L"待办卡片";
    }
    return english ? L"Card" : L"卡片";
}

std::wstring DefaultCardTitle(const presentation::CardView& card, bool english) {
    return card.title.empty() ? CardTypeName(card.type, english) : card.title;
}

std::wstring CardTypeGlyph(domain::CardType type) {
    switch (type) {
    case domain::CardType::Application: return L"\uE8B7";
    case domain::CardType::Mapping: return L"\uE71B";
    case domain::CardType::Todo: return L"\uE73E";
    }
    return L"\uE8A9";
}

domain::CardChromePreferences ChromePreferences(const presentation::CardView& card) {
    return {
        .showCollapseControl = card.showCollapseControl,
        .showCloseControl = card.showCloseControl,
        .showPinControl = card.showPinControl,
        .showPresentationControl = card.showPresentationControl,
        .pinOnTop = card.pinOnTop,
        .showTitle = card.showTitle,
        .positionLocked = card.positionLocked,
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

HFONT CreateUiFont(int pixels, int weight = FW_NORMAL, const wchar_t* face = L"Segoe UI Variable Text") {
    return CreateFontW(
        -pixels, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, face);
}

HFONT CreateIconFont(int pixels) {
    const auto family = WindowsIconFontFamily();
    return CreateFontW(
        -pixels, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, family.data());
}

void DrawLabelRaw(HDC dc, std::wstring_view text, RECT rect, COLORREF color,
                  int pixels, int weight = FW_NORMAL, UINT flags = DT_LEFT | DT_VCENTER) {
    const auto font = CreateUiFont(pixels, weight);
    const auto previous = font == nullptr ? nullptr : SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text.data(), static_cast<int>(text.size()), &rect,
        flags | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    if (previous != nullptr) SelectObject(dc, previous);
    if (font != nullptr) DeleteObject(font);
}

void DrawLabel(HDC dc, std::wstring_view text, RECT rect, COLORREF color,
               int pixels, int weight = FW_NORMAL, UINT flags = DT_LEFT | DT_VCENTER) {
    DrawLabelRaw(dc, text, rect, ThemeColor(color), pixels, weight, flags);
}

void DrawMultilineLabel(HDC dc, std::wstring_view text, RECT rect, COLORREF color,
                        int pixels, int weight = FW_NORMAL) {
    const auto font = CreateUiFont(pixels, weight);
    const auto previous = font == nullptr ? nullptr : SelectObject(dc, font);
    SetTextColor(dc, ThemeColor(color));
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text.data(), static_cast<int>(text.size()), &rect,
        DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
    if (previous != nullptr) SelectObject(dc, previous);
    if (font != nullptr) DeleteObject(font);
}

void DrawGlyphRaw(HDC dc, std::wstring_view glyph, RECT rect, COLORREF color, int pixels) {
    const auto font = CreateIconFont(pixels);
    const auto previous = font == nullptr ? nullptr : SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, glyph.data(), static_cast<int>(glyph.size()), &rect,
        DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
    if (previous != nullptr) SelectObject(dc, previous);
    if (font != nullptr) DeleteObject(font);
}

void DrawGlyph(HDC dc, std::wstring_view glyph, RECT rect, COLORREF color, int pixels) {
    DrawGlyphRaw(dc, glyph, rect, ThemeColor(color), pixels);
}

bool DrawCardItemIcon(
    HDC dc, const presentation::CardItemIcon& icon, RECT rect) noexcept {
    if (icon.empty() || rect.right <= rect.left || rect.bottom <= rect.top) return false;
    auto* pixels = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(
        icon.premultipliedPixels->data()));
    constexpr auto pixelFormat = static_cast<Gdiplus::PixelFormat>(0x000E200Bu);
    Gdiplus::Bitmap bitmap(
        icon.width, icon.height, icon.width * static_cast<INT>(sizeof(std::uint32_t)),
        pixelFormat, pixels);
    if (bitmap.GetLastStatus() != Gdiplus::Ok) return false;
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    const auto targetWidth = rect.right - rect.left;
    const auto targetHeight = rect.bottom - rect.top;
    const auto scale = std::min(
        static_cast<double>(targetWidth) / static_cast<double>(icon.width),
        static_cast<double>(targetHeight) / static_cast<double>(icon.height));
    const auto drawWidth = std::max<LONG>(1, static_cast<LONG>(std::lround(icon.width * scale)));
    const auto drawHeight = std::max<LONG>(1, static_cast<LONG>(std::lround(icon.height * scale)));
    const auto drawLeft = rect.left + (targetWidth - drawWidth) / 2;
    const auto drawTop = rect.top + (targetHeight - drawHeight) / 2;
    return graphics.DrawImage(&bitmap,
        Gdiplus::Rect(drawLeft, drawTop, drawWidth, drawHeight)) == Gdiplus::Ok;
}

void FillRoundedRaw(HDC dc, RECT rect, COLORREF color, int radius) {
    if (radius <= 0) {
        const auto brush = CreateSolidBrush(color);
        if (brush != nullptr) {
            FillRect(dc, &rect, brush);
            DeleteObject(brush);
        }
        return;
    }
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    const auto width = static_cast<Gdiplus::REAL>(rect.right - rect.left);
    const auto height = static_cast<Gdiplus::REAL>(rect.bottom - rect.top);
    const auto corner = static_cast<Gdiplus::REAL>(std::clamp(
        radius, 0, static_cast<int>(std::min(width, height) / 2.0f)));
    Gdiplus::GraphicsPath path;
    path.AddArc(static_cast<Gdiplus::REAL>(rect.left), static_cast<Gdiplus::REAL>(rect.top),
        corner * 2.0f, corner * 2.0f, 180.0f, 90.0f);
    path.AddArc(static_cast<Gdiplus::REAL>(rect.right) - corner * 2.0f,
        static_cast<Gdiplus::REAL>(rect.top), corner * 2.0f, corner * 2.0f, 270.0f, 90.0f);
    path.AddArc(static_cast<Gdiplus::REAL>(rect.right) - corner * 2.0f,
        static_cast<Gdiplus::REAL>(rect.bottom) - corner * 2.0f,
        corner * 2.0f, corner * 2.0f, 0.0f, 90.0f);
    path.AddArc(static_cast<Gdiplus::REAL>(rect.left),
        static_cast<Gdiplus::REAL>(rect.bottom) - corner * 2.0f,
        corner * 2.0f, corner * 2.0f, 90.0f, 90.0f);
    path.CloseFigure();
    Gdiplus::SolidBrush brush(Gdiplus::Color(
        GetRValue(color), GetGValue(color), GetBValue(color)));
    graphics.FillPath(&brush, &path);
}

void FillRounded(HDC dc, RECT rect, COLORREF color, int radius) {
    FillRoundedRaw(dc, rect, ThemeColor(color), radius);
}

void FillRoundedGradientRaw(
    HDC dc,
    RECT rect,
    COLORREF startColor,
    COLORREF endColor,
    int radius) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    const auto width = static_cast<Gdiplus::REAL>(rect.right - rect.left);
    const auto height = static_cast<Gdiplus::REAL>(rect.bottom - rect.top);
    const auto corner = static_cast<Gdiplus::REAL>(std::clamp(
        radius, 0, static_cast<int>(std::min(width, height) / 2.0f)));
    Gdiplus::GraphicsPath path;
    path.AddArc(static_cast<Gdiplus::REAL>(rect.left), static_cast<Gdiplus::REAL>(rect.top),
        corner * 2.0f, corner * 2.0f, 180.0f, 90.0f);
    path.AddArc(static_cast<Gdiplus::REAL>(rect.right) - corner * 2.0f,
        static_cast<Gdiplus::REAL>(rect.top), corner * 2.0f, corner * 2.0f, 270.0f, 90.0f);
    path.AddArc(static_cast<Gdiplus::REAL>(rect.right) - corner * 2.0f,
        static_cast<Gdiplus::REAL>(rect.bottom) - corner * 2.0f,
        corner * 2.0f, corner * 2.0f, 0.0f, 90.0f);
    path.AddArc(static_cast<Gdiplus::REAL>(rect.left),
        static_cast<Gdiplus::REAL>(rect.bottom) - corner * 2.0f,
        corner * 2.0f, corner * 2.0f, 90.0f, 90.0f);
    path.CloseFigure();
    Gdiplus::LinearGradientBrush brush(
        Gdiplus::Point(rect.left, rect.top),
        Gdiplus::Point(rect.right, rect.bottom),
        Gdiplus::Color(GetRValue(startColor), GetGValue(startColor), GetBValue(startColor)),
        Gdiplus::Color(GetRValue(endColor), GetGValue(endColor), GetBValue(endColor)));
    graphics.FillPath(&brush, &path);
}

void FillRoundedGradient(
    HDC dc,
    RECT rect,
    COLORREF startColor,
    COLORREF endColor,
    int radius) {
    FillRoundedGradientRaw(
        dc, rect, ThemeColor(startColor), ThemeColor(endColor), radius);
}

void FillDiagonalSplitRoundedRaw(
    HDC dc, RECT rect, COLORREF lightColor, COLORREF darkColor, int radius) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    const auto width = static_cast<Gdiplus::REAL>(rect.right - rect.left);
    const auto height = static_cast<Gdiplus::REAL>(rect.bottom - rect.top);
    const auto corner = static_cast<Gdiplus::REAL>(std::clamp(
        radius, 0, static_cast<int>(std::min(width, height) / 2.0f)));
    Gdiplus::GraphicsPath path;
    path.AddArc(static_cast<Gdiplus::REAL>(rect.left),
        static_cast<Gdiplus::REAL>(rect.top), corner * 2.0f, corner * 2.0f,
        180.0f, 90.0f);
    path.AddArc(static_cast<Gdiplus::REAL>(rect.right) - corner * 2.0f,
        static_cast<Gdiplus::REAL>(rect.top), corner * 2.0f, corner * 2.0f,
        270.0f, 90.0f);
    path.AddArc(static_cast<Gdiplus::REAL>(rect.right) - corner * 2.0f,
        static_cast<Gdiplus::REAL>(rect.bottom) - corner * 2.0f,
        corner * 2.0f, corner * 2.0f, 0.0f, 90.0f);
    path.AddArc(static_cast<Gdiplus::REAL>(rect.left),
        static_cast<Gdiplus::REAL>(rect.bottom) - corner * 2.0f,
        corner * 2.0f, corner * 2.0f, 90.0f, 90.0f);
    path.CloseFigure();
    const auto state = graphics.Save();
    graphics.SetClip(&path);
    Gdiplus::SolidBrush lightBrush(Gdiplus::Color(
        GetRValue(lightColor), GetGValue(lightColor), GetBValue(lightColor)));
    graphics.FillPath(&lightBrush, &path);
    Gdiplus::SolidBrush darkBrush(Gdiplus::Color(
        GetRValue(darkColor), GetGValue(darkColor), GetBValue(darkColor)));
    const Gdiplus::Point points[] = {
        {rect.right, rect.top}, {rect.right, rect.bottom},
        {rect.left, rect.bottom},
    };
    graphics.FillPolygon(&darkBrush, points, 3);
    graphics.Restore(state);
}

void DrawRoundedOutlineRaw(
    HDC dc, RECT rect, COLORREF color, int radius, int width = 1) {
    if (radius <= 0) {
        const auto pen = CreatePen(PS_SOLID, std::max(1, width), color);
        const auto previousPen = pen == nullptr ? nullptr : SelectObject(dc, pen);
        const auto previousBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
        if (previousBrush != nullptr) SelectObject(dc, previousBrush);
        if (previousPen != nullptr) SelectObject(dc, previousPen);
        if (pen != nullptr) DeleteObject(pen);
        return;
    }
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    const auto inset = static_cast<Gdiplus::REAL>(width) / 2.0f;
    const auto corner = static_cast<Gdiplus::REAL>(std::max(0, radius)) - inset;
    Gdiplus::GraphicsPath path;
    const auto left = static_cast<Gdiplus::REAL>(rect.left) + inset;
    const auto top = static_cast<Gdiplus::REAL>(rect.top) + inset;
    const auto right = static_cast<Gdiplus::REAL>(rect.right) - inset;
    const auto bottom = static_cast<Gdiplus::REAL>(rect.bottom) - inset;
    const auto diameter = std::max(0.0f, corner * 2.0f);
    path.AddArc(left, top, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(right - diameter, top, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(right - diameter, bottom - diameter, diameter, diameter, 0.0f, 90.0f);
    path.AddArc(left, bottom - diameter, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
    Gdiplus::Pen pen(Gdiplus::Color(
        GetRValue(color), GetGValue(color), GetBValue(color)),
        static_cast<Gdiplus::REAL>(std::max(1, width)));
    graphics.DrawPath(&pen, &path);
}

void DrawRoundedOutline(
    HDC dc, RECT rect, COLORREF color, int radius, int width = 1) {
    DrawRoundedOutlineRaw(dc, rect, ThemeColor(color), radius, width);
}

void DrawHorizontalSeparator(HDC dc, int left, int right, int y) noexcept {
    const auto pen = CreatePen(PS_SOLID, 1, ThemeSurfaceColor(
        RGB(54, 56, 61), RGB(232, 233, 235)));
    const auto previous = pen == nullptr ? nullptr : SelectObject(dc, pen);
    MoveToEx(dc, left, y, nullptr);
    LineTo(dc, right, y);
    if (previous != nullptr) SelectObject(dc, previous);
    if (pen != nullptr) DeleteObject(pen);
}

void DrawSwitch(HDC dc, RECT rect, bool enabled, bool hovered, bool pressed) {
    const auto background = enabled
        ? (pressed ? kAccentPressed : hovered ? kAccentHover : kAccent)
        : (pressed ? RGB(74, 77, 83) : hovered ? RGB(82, 85, 92) : RGB(66, 69, 75));
    FillRounded(dc, rect, background, rect.bottom - rect.top);
    const auto diameter = rect.bottom - rect.top - 6;
    const auto left = enabled ? rect.right - diameter - 3 : rect.left + 3;
    FillRoundedRaw(dc, Rect(left, rect.top + 3, left + diameter, rect.bottom - 3),
        RGB(245, 246, 248), diameter);
}

void ApplySettingsWindowTheme(HWND window) noexcept {
    const BOOL darkMode = gSettingsDarkMode;
    DwmSetWindowAttribute(window, 20, &darkMode, sizeof(darkMode));
    const auto captionColor = gSettingsDarkMode
        ? RGB(25, 26, 29) : RGB(238, 245, 249);
    const auto textColor = gSettingsDarkMode
        ? RGB(245, 246, 248) : RGB(20, 24, 29);
    DwmSetWindowAttribute(
        window, kDwmCaptionColorAttribute, &captionColor, sizeof(captionColor));
    DwmSetWindowAttribute(
        window, kDwmTextColorAttribute, &textColor, sizeof(textColor));
}

std::wstring TodoDateText(const domain::TodoItem& item, bool english) {
    if (!item.scheduledDate.has_value()) return english ? L"No due date" : L"无计划日期";
    const auto& date = *item.scheduledDate;
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%04u-%02u-%02u", static_cast<unsigned>(date.year),
        static_cast<unsigned>(date.month), static_cast<unsigned>(date.day));
    return buffer;
}

std::wstring TodoTimeText(
    std::int64_t milliseconds,
    std::optional<std::int32_t> offsetMinutes,
    bool completed,
    bool english) {
    if (milliseconds <= 0) {
        return completed
            ? (english ? L"Completion time unavailable" : L"完成时间未知")
            : (english ? L"Creation time unavailable" : L"创建时间未知");
    }
    auto seconds = static_cast<time_t>(milliseconds / 1000);
    tm value{};
    if (offsetMinutes.has_value()) {
        seconds += static_cast<time_t>(*offsetMinutes) * 60;
        gmtime_s(&value, &seconds);
    } else {
        localtime_s(&value, &seconds);
    }
    wchar_t buffer[64]{};
    wcsftime(buffer, std::size(buffer), L"%Y-%m-%d %H:%M", &value);
    return buffer;
}

std::int64_t UnixMillisecondsNow() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const auto length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), length) != length) {
        return {};
    }
    return result;
}

std::string WideToUtf8(std::wstring_view value) {
    if (value.empty()) return {};
    const auto length = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), length, nullptr, nullptr) != length) return {};
    return result;
}

std::optional<std::filesystem::path> PickFolder(HWND owner, bool english) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(
            CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog))) || dialog == nullptr) {
        return std::nullopt;
    }
    FILEOPENDIALOGOPTIONS options{};
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(
        english ? L"Choose Desto data location" : L"选择 Desto 数据存储位置");
    if (dialog->Show(owner) != S_OK) {
        dialog->Release();
        return std::nullopt;
    }
    IShellItem* item = nullptr;
    if (FAILED(dialog->GetResult(&item)) || item == nullptr) {
        dialog->Release();
        return std::nullopt;
    }
    wchar_t* path = nullptr;
    const auto succeeded = SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path));
    std::optional<std::filesystem::path> result;
    if (succeeded && path != nullptr) result = std::filesystem::path(path);
    if (path != nullptr) CoTaskMemFree(path);
    item->Release();
    dialog->Release();
    return result;
}

std::optional<std::filesystem::path> PickArchiveExportFile(HWND owner, bool english) {
    IFileSaveDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(
            CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog))) || dialog == nullptr) return std::nullopt;
    const COMDLG_FILTERSPEC filter{
        english ? L"Text file" : L"文本文件", L"*.txt"};
    dialog->SetFileTypes(1, &filter);
    dialog->SetDefaultExtension(L"txt");
    dialog->SetFileName(L"Desto-archive.txt");
    dialog->SetTitle(english ? L"Export archived tasks" : L"导出待办归档");
    if (dialog->Show(owner) != S_OK) {
        dialog->Release();
        return std::nullopt;
    }
    IShellItem* item = nullptr;
    if (FAILED(dialog->GetResult(&item)) || item == nullptr) {
        dialog->Release();
        return std::nullopt;
    }
    wchar_t* path = nullptr;
    const auto succeeded = SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path));
    std::optional<std::filesystem::path> result;
    if (succeeded && path != nullptr) result = std::filesystem::path(path);
    if (path != nullptr) CoTaskMemFree(path);
    item->Release();
    dialog->Release();
    return result;
}

} // namespace

struct WindowsSettingsHost::Impl {
    explicit Impl(std::wstring windowTitle)
        : title(std::move(windowTitle)), module(GetModuleHandleW(nullptr)) {
        gSettingsDarkMode = SystemAppsUseDarkTheme();
        Gdiplus::GdiplusStartupInput gdiplusInput;
        if (Gdiplus::GdiplusStartup(
                &gdiplusToken, &gdiplusInput, nullptr) != Gdiplus::Ok) {
            throw std::runtime_error("Unable to initialize GDI+ for settings rendering.");
        }
        WNDCLASSEXW windowClass{
            .cbSize = sizeof(WNDCLASSEXW),
            .style = CS_HREDRAW | CS_VREDRAW,
            .lpfnWndProc = &WindowProcedure,
            .hInstance = module,
            .hIcon = static_cast<HICON>(LoadImageW(
                module, MAKEINTRESOURCEW(kDestoIconResourceId), IMAGE_ICON,
                GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
                LR_DEFAULTCOLOR | LR_SHARED)),
            .hCursor = LoadCursorA(nullptr, IDC_ARROW),
            .hbrBackground = nullptr,
            .lpszClassName = className,
            .hIconSm = static_cast<HICON>(LoadImageW(
                module, MAKEINTRESOURCEW(kDestoIconResourceId), IMAGE_ICON,
                GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                LR_DEFAULTCOLOR | LR_SHARED)),
        };
        if (RegisterClassExW(&windowClass) == 0
            && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            Gdiplus::GdiplusShutdown(gdiplusToken);
            gdiplusToken = 0;
            throw std::runtime_error("Unable to register settings window class.");
        }
        window = CreateWindowExW(
            0, className, title.c_str(), WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 780, 640,
            nullptr, nullptr, module, this);
        if (window == nullptr) {
            Gdiplus::GdiplusShutdown(gdiplusToken);
            gdiplusToken = 0;
            throw std::runtime_error("Unable to create settings window.");
        }
        archiveSearchEdit = CreateWindowsTextInput({
            .notificationWindow = window,
            .controlId = kArchiveSearchEditId,
            .bounds = RECT{0, 0, 1, 1},
            .maximumLength = 512,
            .placeholder = tr(L"按日期、内容或卡片名称搜索", L"Search date, task, or card"),
            .style = settingsInputStyle(true),
        });
        if (archiveSearchEdit == nullptr) {
            throw std::runtime_error("Unable to create archive search editor.");
        }
        archiveAddEdit = CreateWindowsTextInput({
            .notificationWindow = window,
            .controlId = kArchiveAddEditId,
            .bounds = RECT{0, 0, 1, 1},
            .maximumLength = 512,
            .placeholder = tr(L"待办内容", L"Task content"),
            .style = settingsInputStyle(false),
        });
        if (archiveAddEdit == nullptr) {
            throw std::runtime_error("Unable to create historical archive editor.");
        }
        ApplySettingsWindowTheme(window);
    }

    ~Impl() {
        closeRenameEditor(false);
        if (window != nullptr) DestroyWindow(window);
        if (gdiplusToken != 0) Gdiplus::GdiplusShutdown(gdiplusToken);
    }

    void refreshSystemTheme() noexcept {
        const auto nextDarkMode = SystemAppsUseDarkTheme();
        if (gSettingsDarkMode == nextDarkMode) return;
        gSettingsDarkMode = nextDarkMode;
        ApplySettingsWindowTheme(window);
        SetWindowsTextInputStyle(archiveSearchEdit, settingsInputStyle(true));
        SetWindowsTextInputStyle(archiveAddEdit, settingsInputStyle(false));
        if (renameEdit != nullptr) SetWindowsTextInputStyle(renameEdit, renameInputStyle());
        InvalidateRect(window, nullptr, FALSE);
    }

    WindowsTextInputStyle settingsInputStyle(bool search) const {
        WindowsTextInputStyle style;
        style.background = ThemeSurfaceColor(RGB(28, 30, 34), RGB(255, 255, 255));
        style.outline = ThemeColor(RGB(55, 58, 64));
        style.focusedOutline = kAccent;
        style.text = ThemeColor(RGB(241, 242, 245));
        style.placeholder = ThemeColor(RGB(139, 143, 151));
        style.selection = kAccent;
        style.compositionUnderline = kAccent;
        style.cornerRadius = 8.0F;
        style.paddingLeft = search ? 10.0F : 12.0F;
        style.paddingRight = search ? 14.0F : 12.0F;
        style.fontSize = 13.0F;
        if (search) {
            style.leadingGlyph = L"\uE721";
            style.leadingGlyphSize = 14.0F;
            style.leadingGlyphWidth = 30.0F;
        }
        return style;
    }

    WindowsTextInputStyle renameInputStyle() const {
        WindowsTextInputStyle style;
        style.background = ThemeSurfaceColor(RGB(18, 19, 21), RGB(243, 243, 243));
        style.outline = style.background;
        style.focusedOutline = style.background;
        style.text = ThemeColor(RGB(243, 244, 247));
        style.placeholder = ThemeColor(RGB(143, 146, 154));
        style.selection = kAccent;
        style.compositionUnderline = kAccent;
        style.outlineWidth = 0.0F;
        style.cornerRadius = 0.0F;
        style.paddingLeft = 0.0F;
        style.paddingRight = 0.0F;
        style.fontSize = 16.0F;
        style.fontWeight = FW_SEMIBOLD;
        return style;
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
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            instance->refreshSystemTheme();
            return 0;
        case WM_PAINT:
            instance->paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEMOVE:
            instance->updateHover(
                GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), wParam);
            return 0;
        case WM_MOUSELEAVE:
            instance->hovered = instance->keyboardFocus.value_or(SettingsAction{});
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
            instance->cardDragSource.reset();
            instance->cardDragTarget.reset();
            instance->cardDragActive = false;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_MOUSEWHEEL:
            if (instance->page == SettingsPage::Archive) {
                const auto delta = GET_WHEEL_DELTA_WPARAM(wParam);
                instance->archiveOffset = delta > 0
                    ? (instance->archiveOffset == 0 ? 0 : instance->archiveOffset - 1)
                    : instance->archiveOffset + 1;
                instance->clampArchiveOffset();
                InvalidateRect(window, nullptr, FALSE);
            } else if (instance->page == SettingsPage::Cards
                && !instance->cards.empty()
                && instance->selectedCard < instance->cards.size()) {
                instance->closeRenameEditor(true);
                const auto delta = GET_WHEEL_DELTA_WPARAM(wParam);
                auto& offset = instance->cardEditorOffsets[
                    instance->cards[instance->selectedCard].id];
                offset -= (delta / WHEEL_DELTA) * 56;
                instance->clampCardEditorOffset();
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        case WM_SIZE:
            instance->updateArchiveSearchEditor();
            instance->updateArchiveAddEditor();
            return 0;
        case WM_COMMAND:
            if (HIWORD(wParam) == WindowsTextInputCommitNotification) {
                if (reinterpret_cast<HWND>(lParam) == instance->renameEdit) {
                    instance->closeRenameEditor(true);
                } else if (reinterpret_cast<HWND>(lParam) == instance->archiveAddEdit
                    && instance->archiveOverlay == ArchiveOverlay::Add) {
                    instance->apply({SettingsActionKind::ConfirmHistoricalArchive});
                }
                return 0;
            }
            if (HIWORD(wParam) == WindowsTextInputCancelNotification) {
                if (reinterpret_cast<HWND>(lParam) == instance->renameEdit) {
                    instance->closeRenameEditor(false);
                } else if (reinterpret_cast<HWND>(lParam) == instance->archiveAddEdit) {
                    instance->archiveOverlay = ArchiveOverlay::None;
                    instance->updateArchiveAddEditor();
                    instance->updateArchiveSearchEditor();
                    InvalidateRect(window, nullptr, FALSE);
                }
                return 0;
            }
            if (HIWORD(wParam) == WindowsTextInputTabForwardNotification
                || HIWORD(wParam) == WindowsTextInputTabBackwardNotification) {
                if (reinterpret_cast<HWND>(lParam) == instance->renameEdit) {
                    instance->closeRenameEditor(true);
                } else {
                    SetFocus(instance->window);
                }
                instance->advanceKeyboardFocus(
                    HIWORD(wParam) == WindowsTextInputTabBackwardNotification);
                return 0;
            }
            if (LOWORD(wParam) == kArchiveSearchEditId && HIWORD(wParam) == EN_CHANGE) {
                instance->archiveOffset = 0;
                instance->clampArchiveOffset();
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            if (LOWORD(wParam) == kArchiveAddEditId && HIWORD(wParam) == EN_CHANGE) {
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            if (LOWORD(wParam) == kRenameEditId && HIWORD(wParam) == EN_CHANGE) {
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            break;
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
            info->ptMinTrackSize = {720, 600};
            return 0;
        }
        case WM_KEYDOWN:
            if (instance->handleKeyboardKey(wParam)) return 0;
            break;
        case WM_CLOSE:
            instance->closeRenameEditor(true);
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

    int clientRight() const noexcept {
        RECT client{};
        if (window != nullptr) GetClientRect(window, &client);
        return std::max(700L, client.right);
    }

    int clientBottom() const noexcept {
        RECT client{};
        if (window != nullptr) GetClientRect(window, &client);
        return std::max(520L, client.bottom);
    }

    RECT navigationRect(std::size_t index) const noexcept {
        return Rect(10, 14 + static_cast<int>(index) * 44, kSidebarWidth - 10,
            52 + static_cast<int>(index) * 44);
    }

    RECT addCardRect() const noexcept { return Rect(clientRight() - 126, 22, clientRight() - 26, 58); }
    RECT addTypeRect(std::size_t index) const noexcept {
        return Rect(clientRight() - 286, 96 + static_cast<int>(index) * 40,
            clientRight() - 26, 132 + static_cast<int>(index) * 40);
    }
    RECT addMenuPanelRect() const noexcept {
        return Rect(clientRight() - 294, 68, clientRight() - 18,
            addTypeRect(2).bottom + 8);
    }
    int cardListRight() const noexcept {
        return kContentLeft + kCardRailWidth;
    }
    int cardDetailLeft() const noexcept { return cardListRight() + 22; }
    RECT cardRow(std::size_t index) const noexcept {
        return Rect(kContentLeft + 2, 84 + static_cast<int>(index) * 54,
            cardListRight() - 2, 132 + static_cast<int>(index) * 54);
    }
    RECT cardTooltipRect(std::size_t index) const noexcept {
        const auto row = cardRow(index);
        const auto left = row.right + 10;
        return Rect(left, row.top - 5,
            std::min(static_cast<int>(left) + 216, clientRight() - 20), row.bottom + 9);
    }
    RECT cardVisibilityRect(std::size_t index) const noexcept {
        const auto row = cardRow(index);
        return Rect(row.right - 19, row.bottom - 19, row.right - 3, row.bottom - 3);
    }
    RECT cardMenuButtonRect() const noexcept {
        return Rect(clientRight() - 62, 82, clientRight() - 26, 118);
    }
    RECT renameButtonRect() const noexcept {
        return Rect(clientRight() - 102, 82, clientRight() - 66, 118);
    }
    RECT renameFieldRect(std::size_t index) const noexcept {
        (void)index;
        return Rect(cardDetailLeft(), 82, clientRight() - 108, 120);
    }
    RECT cardMenuPanelRect() const noexcept {
        const auto fileCard = !cards.empty() && selectedCard < cards.size()
            && (cards[selectedCard].type == domain::CardType::Application
                || cards[selectedCard].type == domain::CardType::Mapping);
        return Rect(clientRight() - 214, 124, clientRight() - 26,
            fileCard ? 224 : 172);
    }
    RECT cardMenuSortButtonRect() const noexcept {
        const auto panel = cardMenuPanelRect();
        return Rect(panel.left + 6, panel.top + 6, panel.right - 6, panel.top + 42);
    }
    RECT cardMenuSortRect(std::size_t index) const noexcept {
        const auto panel = cardMenuPanelRect();
        const auto top = panel.top + 6 + static_cast<int>(index) * 36;
        return Rect(panel.left - 194, top, panel.left - 6, top + 34);
    }
    RECT cardMenuDeleteRect() const noexcept {
        const auto panel = cardMenuPanelRect();
        const auto fileCard = !cards.empty() && selectedCard < cards.size()
            && (cards[selectedCard].type == domain::CardType::Application
                || cards[selectedCard].type == domain::CardType::Mapping);
        return fileCard
            ? Rect(panel.left + 6, panel.top + 52,
                panel.right - 6, panel.bottom - 6)
            : Rect(panel.left + 6, panel.top + 6, panel.right - 6, panel.bottom - 6);
    }
    RECT confirmationPanelRect() const noexcept {
        const auto width = std::min(440, clientRight() - 80);
        const auto height = 230;
        const auto left = (clientRight() - width) / 2;
        const auto top = (clientBottom() - height) / 2;
        return Rect(left, top, left + width, top + height);
    }
    RECT confirmationCancelRect() const noexcept {
        const auto panel = confirmationPanelRect();
        return Rect(panel.right - 210, panel.bottom - 54, panel.right - 116, panel.bottom - 18);
    }
    RECT confirmationConfirmRect() const noexcept {
        const auto panel = confirmationPanelRect();
        return Rect(panel.right - 108, panel.bottom - 54, panel.right - 18, panel.bottom - 18);
    }
    RECT appearanceRect(std::size_t index) const noexcept {
        const auto left = cardDetailLeft();
        const auto layout = ResolveFileCardSettingsLayout(false);
        return Rect(left + static_cast<int>(index) * 48, layout.appearanceTop,
            left + 42 + static_cast<int>(index) * 48, layout.appearanceBottom);
    }
    RECT itemSizeRect(std::size_t index) const noexcept {
        constexpr int size = 38;
        constexpr int gap = 7;
        const auto groupWidth = size * 4 + gap * 3;
        const auto left = clientRight() - 26 - groupWidth;
        const auto itemLeft = left + static_cast<int>(index) * (size + gap);
        const auto layout = ResolveFileCardSettingsLayout(false);
        return Rect(itemLeft, layout.appearanceTop, itemLeft + size, layout.appearanceBottom);
    }
    RECT compactCardSizeRect(std::size_t index) const noexcept {
        constexpr int width = 54;
        constexpr int height = 38;
        constexpr int gap = 7;
        const auto groupWidth = width * 3 + gap * 2;
        const auto left = clientRight() - 26 - groupWidth;
        const auto itemLeft = left + static_cast<int>(index) * (width + gap);
        return Rect(itemLeft, 166, itemLeft + width, 166 + height);
    }
    RECT itemNamesRect() const noexcept {
        return optionButtonRect(0);
    }
    RECT mappingModeRect(std::size_t index) const noexcept {
        const auto left = cardDetailLeft();
        const auto layout = ResolveFileCardSettingsLayout(true);
        constexpr int gap = 8;
        constexpr int totalWidth = 220;
        const auto width = (totalWidth - gap) / 2;
        const auto itemLeft = left + static_cast<int>(index) * (width + gap);
        return Rect(itemLeft, layout.sourceTop, itemLeft + width, layout.sourceBottom);
    }
    RECT collapseRect() const noexcept {
        const auto fileCard = !cards.empty() && selectedCard < cards.size()
            && (cards[selectedCard].type == domain::CardType::Application
                || cards[selectedCard].type == domain::CardType::Mapping);
        return fileToolbarButtonRect(fileCard ? 1 : 0);
    }
    RECT sizeModeRect() const noexcept {
        return optionButtonRect(1);
    }
    RECT fixedColumnsRect() const noexcept {
        return extraFileSettingRect(0);
    }
    RECT fixedRowsRect() const noexcept {
        return extraFileSettingRect(1);
    }
    bool selectedMappingCard() const noexcept {
        return !cards.empty() && selectedCard < cards.size()
            && cards[selectedCard].type == domain::CardType::Mapping;
    }
    RECT fileToolbarButtonRect(std::size_t index) const noexcept {
        constexpr int size = 42;
        constexpr int gap = 10;
        const auto layout = ResolveFileCardSettingsLayout(selectedMappingCard());
        const auto left = cardDetailLeft() + static_cast<int>(index) * (size + gap);
        return Rect(left, layout.toolbarTop, left + size, layout.toolbarBottom);
    }
    RECT filePresentationRect() const noexcept {
        return fileToolbarButtonRect(0);
    }
    RECT extraFileSettingRect(std::size_t slot) const noexcept {
        const auto left = cardDetailLeft();
        const auto right = clientRight() - 26;
        const auto width = (right - left - 16) / 3;
        const auto itemLeft = left + static_cast<int>(slot) * (width + 8);
        const auto top = ResolveFileCardSettingsLayout(selectedMappingCard()).extraTop;
        return Rect(itemLeft, top, itemLeft + width, top + 36);
    }
    RECT optionButtonRect(std::size_t index) const noexcept {
        constexpr int size = 42;
        constexpr int gap = 10;
        const auto left = cardDetailLeft() + static_cast<int>(index) * (size + gap);
        const auto top = ResolveFileCardSettingsLayout(selectedMappingCard()).optionsTop;
        return Rect(left, top, left + size, top + size);
    }
    RECT contentPreviewRect(const presentation::CardView& card) const {
        const auto total = contentItemIndices(card).size();
        const auto rows = (card.type == domain::CardType::Application
                || card.type == domain::CardType::Mapping)
            ? std::max<std::size_t>(1, (total + 5) / 6)
            : std::max<std::size_t>(1, total);
        const auto hasExtraSettings = card.content.sizeMode == domain::CardSizeMode::Fixed
            || card.content.maximumVisibleRows.has_value();
        const auto layout = ResolveFileCardSettingsLayout(
            card.type == domain::CardType::Mapping);
        const auto top = hasExtraSettings ? layout.extraTop + 52 : layout.extraTop;
        return Rect(cardDetailLeft(), top, clientRight() - 26,
            top + 44 + static_cast<int>(rows)
                * ((card.type == domain::CardType::Application
                    || card.type == domain::CardType::Mapping) ? 58 : 28) + 12);
    }

    RECT contentPreviewRowRect(const RECT& panel, std::size_t visibleIndex) const noexcept {
        return Rect(panel.left + 12,
            panel.top + 38 + static_cast<int>(visibleIndex) * 28,
            panel.right - 12,
            panel.top + 66 + static_cast<int>(visibleIndex) * 28);
    }
    RECT pinControlSettingRect() const noexcept {
        const auto fileCard = !cards.empty() && selectedCard < cards.size()
            && (cards[selectedCard].type == domain::CardType::Application
                || cards[selectedCard].type == domain::CardType::Mapping);
        return fileToolbarButtonRect(fileCard ? 2 : 1);
    }
    RECT heightLimitRect() const noexcept {
        return optionButtonRect(2);
    }
    RECT positionLockRect() const noexcept {
        const auto todoCard = !cards.empty() && selectedCard < cards.size()
            && cards[selectedCard].type == domain::CardType::Todo;
        return optionButtonRect(todoCard ? 2 : 3);
    }
    RECT maximumVisibleRowsRect() const noexcept {
        const auto fixed = !cards.empty() && selectedCard < cards.size()
            && cards[selectedCard].content.sizeMode == domain::CardSizeMode::Fixed;
        return extraFileSettingRect(fixed ? 2 : 0);
    }
    RECT todoHeightLimitRect() const noexcept {
        return optionButtonRect(1);
    }
    RECT todoMaximumVisibleRowsRect() const noexcept {
        return extraFileSettingRect(0);
    }
    static RECT decrementRect(RECT stepper) noexcept {
        stepper.right = stepper.left + 30;
        return stepper;
    }
    static RECT incrementRect(RECT stepper) noexcept {
        stepper.left = stepper.right - 30;
        return stepper;
    }
    RECT createdTimeRect() const noexcept {
        return optionButtonRect(0);
    }
    RECT todoContentPreviewRect(const presentation::CardView& card) const {
        const auto rows = std::max<std::size_t>(1, contentItemIndices(card).size());
        const auto layout = ResolveFileCardSettingsLayout(false);
        const auto top = card.content.maximumVisibleRows.has_value()
            ? layout.extraTop + 52 : layout.extraTop;
        return Rect(cardDetailLeft(), top, clientRight() - 26,
            top + 44 + static_cast<int>(rows) * 28 + 12);
    }

    RECT timeZoneRect() const noexcept {
        return Rect(clientRight() - 244, 158, clientRight() - 44, 194);
    }
    RECT languageRect() const noexcept {
        return Rect(clientRight() - 244, 212, clientRight() - 44, 248);
    }
    RECT storageRootRect() const noexcept {
        return Rect(clientRight() - 126, 266, clientRight() - 44, 302);
    }
    RECT runAtStartupRect() const noexcept {
        return Rect(kContentLeft + 18, 104, clientRight() - 44, 132);
    }
    RECT desktopDoubleClickRect() const noexcept {
        return Rect(clientRight() - 244, 104, clientRight() - 44, 140);
    }
    RECT taskbarDoubleClickRowRect() const noexcept {
        return Rect(kContentLeft + 18, 164, clientRight() - 44, 192);
    }
    RECT taskbarDoubleClickRect() const noexcept {
        return Rect(clientRight() - 84, 164, clientRight() - 44, 192);
    }
    RECT pinnedFullscreenRect() const noexcept {
        return Rect(kContentLeft + 18, 224, clientRight() - 44, 252);
    }
    RECT iconBackgroundFrameRect() const noexcept {
        return Rect(kContentLeft + 18, 260, clientRight() - 44, 288);
    }
    RECT fileDeletionConfirmationRect() const noexcept {
        return Rect(kContentLeft + 18, 296, clientRight() - 44, 324);
    }
    RECT openProjectRect() const noexcept {
        const auto center = (kContentLeft + clientRight() - 26) / 2;
        return Rect(center - 115, 300, center + 115, 334);
    }
    RECT checkForUpdatesRect() const noexcept {
        const auto center = (kContentLeft + clientRight() - 26) / 2;
        return Rect(center - 115, 342, center + 115, 376);
    }
    RECT updateChannelRect() const noexcept {
        const auto center = (kContentLeft + clientRight() - 26) / 2;
        return Rect(center - 115, 384, center + 115, 418);
    }
    RECT radiusOptionRect(std::size_t index) const noexcept {
        const auto left = kContentLeft + 18;
        const auto right = clientRight() - 44;
        constexpr int gap = 8;
        const auto width = std::max(1, (right - left - gap * 3) / 4);
        const auto itemLeft = left + static_cast<int>(index) * (width + gap);
        return Rect(itemLeft, 378, itemLeft + width, 418);
    }
    std::size_t systemDropdownOptionCount() const noexcept {
        if (activeSystemDropdown == SystemDropdown::TimeZone) return 4;
        if (activeSystemDropdown == SystemDropdown::Language) return 3;
        if (activeSystemDropdown == SystemDropdown::DesktopDoubleClick) return 4;
        return 0;
    }
    RECT systemDropdownOptionRect(std::size_t index) const noexcept {
        const auto anchor = activeSystemDropdown == SystemDropdown::TimeZone
            ? timeZoneRect()
            : activeSystemDropdown == SystemDropdown::Language
            ? languageRect()
            : desktopDoubleClickRect();
        const auto top = anchor.bottom + 6 + static_cast<int>(index) * 36;
        return Rect(anchor.left + 4, top, anchor.right - 4, top + 34);
    }
    RECT systemDropdownPanelRect() const noexcept {
        const auto anchor = activeSystemDropdown == SystemDropdown::TimeZone
            ? timeZoneRect()
            : activeSystemDropdown == SystemDropdown::Language
            ? languageRect()
            : desktopDoubleClickRect();
        return Rect(anchor.left, anchor.bottom + 2, anchor.right,
            anchor.bottom + 10 + static_cast<int>(systemDropdownOptionCount()) * 36);
    }
    RECT archiveSearchRect() const noexcept {
        return Rect(kContentLeft, 132, clientRight() - 26, 172);
    }
    RECT archiveAddButtonRect() const noexcept {
        return Rect(clientRight() - 246, 34, clientRight() - 142, 70);
    }
    RECT archiveExportButtonRect() const noexcept {
        return Rect(clientRight() - 134, 34, clientRight() - 26, 70);
    }
    RECT archiveOverlayRect() const noexcept {
        return Rect(kContentLeft + 28, 92, clientRight() - 54,
            std::min(clientBottom() - 18, 560));
    }
    RECT archiveOverlayCardRect() const noexcept {
        const auto panel = archiveOverlayRect();
        return Rect(panel.left + 20, panel.top + 58, panel.right - 20, panel.top + 98);
    }
    RECT archiveOverlayEditRect() const noexcept {
        const auto panel = archiveOverlayRect();
        return Rect(panel.left + 20, panel.top + 112, panel.right - 20, panel.top + 156);
    }
    RECT archiveOverlayConfirmRect() const noexcept {
        const auto panel = archiveOverlayRect();
        return Rect(panel.right - 130, panel.bottom - 54, panel.right - 20, panel.bottom - 16);
    }
    RECT archiveOverlayCancelRect() const noexcept {
        const auto panel = archiveOverlayRect();
        return Rect(panel.right - 248, panel.bottom - 54, panel.right - 138, panel.bottom - 16);
    }
    RECT archiveExportDateRect(std::size_t index) const noexcept {
        const auto panel = archiveOverlayRect();
        const auto width = (panel.right - panel.left - 48) / 2;
        const auto left = panel.left + 20 + static_cast<int>(index) * (width + 8);
        return Rect(left, panel.top + 70, left + width, panel.top + 112);
    }
    RECT archiveExportCalendarRect() const noexcept {
        const auto panel = archiveOverlayRect();
        return Rect(panel.left + 20, panel.top + 124,
            panel.right - 20, panel.bottom - 66);
    }
    RECT archiveExportCalendarPreviousRect() const noexcept {
        const auto panel = archiveExportCalendarRect();
        return Rect(panel.left + 8, panel.top + 7, panel.left + 40, panel.top + 39);
    }
    RECT archiveExportCalendarNextRect() const noexcept {
        const auto panel = archiveExportCalendarRect();
        return Rect(panel.right - 40, panel.top + 7, panel.right - 8, panel.top + 39);
    }
    RECT archiveExportCalendarDayRect(std::size_t index) const noexcept {
        const auto panel = archiveExportCalendarRect();
        const auto width = std::max<LONG>(1, panel.right - panel.left);
        const auto gridTop = panel.top + 66;
        const auto gridHeight = std::max<LONG>(1, panel.bottom - gridTop - 6);
        const auto column = static_cast<LONG>(index % 7);
        const auto row = static_cast<LONG>(index / 7);
        return Rect(
            panel.left + column * width / 7,
            gridTop + row * gridHeight / 6,
            panel.left + (column + 1) * width / 7,
            gridTop + (row + 1) * gridHeight / 6);
    }
    RECT archiveDatePreviousRect() const noexcept {
        return Rect(kContentLeft, 84, kContentLeft + 40, 124);
    }
    RECT archiveDateNextRect() const noexcept {
        return Rect(clientRight() - 66, 84, clientRight() - 26, 124);
    }
    RECT archiveDateLabelRect() const noexcept {
        return Rect(kContentLeft + 48, 84, clientRight() - 74, 124);
    }
    RECT archiveCalendarPanelRect() const noexcept {
        constexpr int width = 336;
        const auto label = archiveDateLabelRect();
        const auto left = std::clamp(
            static_cast<int>((label.left + label.right - width) / 2),
            kContentLeft,
            clientRight() - 26 - width);
        return Rect(left, 130, left + width, 414);
    }
    RECT archiveCalendarPreviousRect() const noexcept {
        const auto panel = archiveCalendarPanelRect();
        return Rect(panel.left + 10, panel.top + 8, panel.left + 44, panel.top + 42);
    }
    RECT archiveCalendarNextRect() const noexcept {
        const auto panel = archiveCalendarPanelRect();
        return Rect(panel.right - 44, panel.top + 8, panel.right - 10, panel.top + 42);
    }
    RECT archiveCalendarDayRect(std::size_t index) const noexcept {
        const auto panel = archiveCalendarPanelRect();
        const auto column = static_cast<int>(index % 7);
        const auto row = static_cast<int>(index / 7);
        return Rect(panel.left + column * 48, 204 + row * 34,
            panel.left + (column + 1) * 48, 204 + (row + 1) * 34);
    }
    RECT archiveRow(std::size_t visibleIndex) const noexcept {
        return Rect(kContentLeft, 186 + static_cast<int>(visibleIndex) * 76,
            clientRight() - 26, 254 + static_cast<int>(visibleIndex) * 76);
    }
    RECT archiveRestoreRect(std::size_t visibleIndex) const noexcept {
        const auto row = archiveRow(visibleIndex);
        return Rect(row.right - 94, row.top + 14, row.right - 54, row.bottom - 14);
    }
    RECT archiveDeleteRect(std::size_t visibleIndex) const noexcept {
        const auto row = archiveRow(visibleIndex);
        return Rect(row.right - 46, row.top + 14, row.right - 6, row.bottom - 14);
    }

    domain::TodoDate selectedArchiveDate() const noexcept {
        return domain::AddTodoDays(
            domain::CurrentTodoDate(timeZoneOffsetMinutes), archiveDateOffset);
    }

    static std::chrono::sys_days toSysDays(domain::TodoDate date) noexcept {
        return std::chrono::sys_days{
            std::chrono::year{date.year}
            / std::chrono::month{date.month}
            / std::chrono::day{date.day}};
    }

    static domain::TodoDate fromSysDays(std::chrono::sys_days days) noexcept {
        const std::chrono::year_month_day value{days};
        return {
            static_cast<std::int32_t>(static_cast<int>(value.year())),
            static_cast<std::uint8_t>(static_cast<unsigned>(value.month())),
            static_cast<std::uint8_t>(static_cast<unsigned>(value.day())),
        };
    }

    domain::TodoDate archiveCalendarCellDate(std::size_t index) const noexcept {
        const auto first = toSysDays(archiveCalendarMonth);
        const auto mondayOffset = static_cast<int>(
            (std::chrono::weekday{first}.c_encoding() + 6) % 7);
        return fromSysDays(first + std::chrono::days{
            static_cast<int>(index) - mondayOffset});
    }

    domain::TodoDate archiveExportCalendarCellDate(std::size_t index) const noexcept {
        const auto first = toSysDays(archiveExportCalendarMonth);
        const auto mondayOffset = static_cast<int>(
            (std::chrono::weekday{first}.c_encoding() + 6) % 7);
        return fromSysDays(first + std::chrono::days{
            static_cast<int>(index) - mondayOffset});
    }

    static std::wstring archiveDateValueText(domain::TodoDate date) {
        wchar_t buffer[32]{};
        swprintf_s(buffer, L"%04d-%02u-%02u", date.year,
            static_cast<unsigned>(date.month), static_cast<unsigned>(date.day));
        return buffer;
    }

    void shiftArchiveExportCalendarMonth(int delta) noexcept {
        auto month = std::chrono::year{archiveExportCalendarMonth.year}
            / std::chrono::month{archiveExportCalendarMonth.month};
        month += std::chrono::months{delta};
        archiveExportCalendarMonth = {
            static_cast<std::int32_t>(static_cast<int>(month.year())),
            static_cast<std::uint8_t>(static_cast<unsigned>(month.month())),
            1,
        };
    }

    bool archiveExportCalendarCanAdvance() const noexcept {
        const auto today = domain::CurrentTodoDate(timeZoneOffsetMinutes);
        return std::tie(archiveExportCalendarMonth.year, archiveExportCalendarMonth.month)
            < std::tie(today.year, today.month);
    }

    bool archiveCalendarCanAdvance() const noexcept {
        const auto today = domain::CurrentTodoDate(timeZoneOffsetMinutes);
        return std::tie(archiveCalendarMonth.year, archiveCalendarMonth.month)
            < std::tie(today.year, today.month);
    }

    void shiftArchiveCalendarMonth(int delta) noexcept {
        auto month = std::chrono::year{archiveCalendarMonth.year}
            / std::chrono::month{archiveCalendarMonth.month};
        month += std::chrono::months{delta};
        archiveCalendarMonth = {
            static_cast<std::int32_t>(static_cast<int>(month.year())),
            static_cast<std::uint8_t>(static_cast<unsigned>(month.month())),
            1,
        };
    }

    std::wstring archiveDateText() const {
        if (archiveDateOffset == 0) return tr(L"今天", L"Today");
        if (archiveDateOffset == -1) return tr(L"昨天", L"Yesterday");
        const auto date = selectedArchiveDate();
        wchar_t buffer[32]{};
        swprintf_s(buffer, L"%04d-%02u-%02u", date.year,
            static_cast<unsigned>(date.month), static_cast<unsigned>(date.day));
        return buffer;
    }

    std::vector<ArchivedEntry> archivedEntries() const {
        std::vector<ArchivedEntry> result;
        std::wstring query;
        if (archiveSearchEdit != nullptr) {
            const auto length = GetWindowTextLengthW(archiveSearchEdit);
            query.resize(static_cast<std::size_t>(std::max(0, length)) + 1);
            if (length > 0) GetWindowTextW(archiveSearchEdit, query.data(), length + 1);
            query.resize(static_cast<std::size_t>(std::max(0, length)));
        }
        const auto selectedDate = selectedArchiveDate();
        for (std::size_t cardIndex = 0; cardIndex < cards.size(); ++cardIndex) {
            if (cards[cardIndex].type != domain::CardType::Todo) continue;
            const auto today = domain::CurrentTodoDate(timeZoneOffsetMinutes);
            for (std::size_t itemIndex = 0;
                 itemIndex < cards[cardIndex].todoItems.size(); ++itemIndex) {
                if (domain::IsTodoItemArchived(
                        cards[cardIndex].todoItems[itemIndex], today,
                        timeZoneOffsetMinutes)) {
                    const auto& item = cards[cardIndex].todoItems[itemIndex];
                    const auto archiveTimestamp = item.completedAtUnixMilliseconds > 0
                        ? item.completedAtUnixMilliseconds
                        : item.createdAtUnixMilliseconds;
                    if (archiveTimestamp <= 0
                        || domain::TodoDateAtUnixMilliseconds(
                            archiveTimestamp, timeZoneOffsetMinutes) != selectedDate
                            && query.empty()) {
                        continue;
                    }
                    const auto completion = TodoTimeText(
                        item.completedAtUnixMilliseconds, timeZoneOffsetMinutes,
                        true, usesEnglish());
                    const auto searchable = Utf8ToWide(item.title) + L" "
                        + DefaultCardTitle(cards[cardIndex], usesEnglish()) + L" "
                        + completion + L" "
                        + TodoTimeText(item.createdAtUnixMilliseconds,
                            timeZoneOffsetMinutes, false, usesEnglish()) + L" "
                        + TodoDateText(item, usesEnglish());
                    if (!query.empty() && !ContainsInsensitive(searchable, query)) continue;
                    result.push_back({cardIndex, itemIndex});
                }
            }
        }
        return result;
    }

    std::size_t visibleArchiveRows() const noexcept {
        return static_cast<std::size_t>(std::max(1, (clientBottom() - 196) / 76));
    }

    static std::wstring currentEditText(HWND edit) noexcept {
        try {
            return WindowsTextInputText(edit);
        } catch (...) {
            return {};
        }
    }

    void updateArchiveSearchEditor() noexcept {
        if (archiveSearchEdit == nullptr) return;
        const auto search = archiveSearchRect();
        SetWindowsTextInputBounds(archiveSearchEdit, search);
        ShowWindow(archiveSearchEdit,
            page == SettingsPage::Archive && !pendingDeletion.has_value()
                && archiveOverlay == ArchiveOverlay::None && !archiveCalendarOpen
                ? SW_SHOW : SW_HIDE);
    }
    void updateArchiveAddEditor() noexcept {
        if (archiveAddEdit == nullptr) return;
        const auto edit = archiveOverlayEditRect();
        SetWindowsTextInputBounds(archiveAddEdit, edit);
        ShowWindow(archiveAddEdit,
            page == SettingsPage::Archive && archiveOverlay == ArchiveOverlay::Add
                ? SW_SHOWNA : SW_HIDE);
    }

    void clampArchiveOffset() noexcept {
        const auto entries = archivedEntries();
        const auto visible = visibleArchiveRows();
        archiveOffset = entries.size() > visible
            ? std::min(archiveOffset, entries.size() - visible) : 0;
    }

    std::vector<std::size_t> contentItemIndices(const presentation::CardView& card) const {
        std::vector<std::size_t> indices;
        if (card.type == domain::CardType::Todo) {
            const auto today = domain::CurrentTodoDate(timeZoneOffsetMinutes);
            for (std::size_t index = 0; index < card.todoItems.size(); ++index) {
                if (!domain::IsTodoItemArchived(
                        card.todoItems[index], today, timeZoneOffsetMinutes)) {
                    indices.push_back(index);
                }
            }
        } else {
            indices.resize(card.items.size());
            for (std::size_t index = 0; index < indices.size(); ++index) {
                indices[index] = index;
            }
        }
        return indices;
    }

    RECT cardEditorViewportRect() const noexcept {
        return Rect(cardDetailLeft() - 4, 76, clientRight() - 16, clientBottom() - 12);
    }

    int cardEditorOffset(const presentation::CardView& card) const noexcept {
        const auto found = cardEditorOffsets.find(card.id);
        return found == cardEditorOffsets.end() ? 0 : found->second;
    }

    int cardEditorContentBottom(const presentation::CardView& card) const {
        return (card.type == domain::CardType::Todo
            ? todoContentPreviewRect(card) : contentPreviewRect(card)).bottom + 18;
    }

    int maximumCardEditorOffset(const presentation::CardView& card) const {
        const auto viewport = cardEditorViewportRect();
        return std::max(0,
            cardEditorContentBottom(card) - static_cast<int>(viewport.bottom));
    }

    void clampCardEditorOffset() noexcept {
        if (cards.empty() || selectedCard >= cards.size()) return;
        const auto& card = cards[selectedCard];
        auto& offset = cardEditorOffsets[card.id];
        offset = std::clamp(offset, 0, maximumCardEditorOffset(card));
    }

    static bool fixedGridFits(
        const presentation::CardView& card,
        std::uint32_t widthSpan,
        std::uint32_t rows) noexcept {
        const auto columns = domain::ProjectCardColumns(widthSpan, card.content.itemSize);
        if (columns == 0 || rows == 0 || columns > 64 || rows > 64
            || card.items.size() > static_cast<std::size_t>(columns) * rows) {
            return false;
        }
        return std::ranges::none_of(
            card.applicationItemPlacements,
            [&](const domain::ApplicationItemPlacement& placement) {
                return placement.column >= columns || placement.row >= rows;
            });
    }

    static domain::CardContentPreferences fixedPreferencesFor(
        const presentation::CardView& card) noexcept {
        auto preferences = card.content;
        preferences.sizeMode = domain::CardSizeMode::Fixed;
        std::uint32_t requiredColumns = 1;
        for (const auto& placement : card.applicationItemPlacements) {
            requiredColumns = std::max(requiredColumns, placement.column + 1);
            preferences.fixedRows = std::max(
                preferences.fixedRows, placement.row + 1);
        }
        preferences.widthSpan = std::max(
            preferences.widthSpan,
            domain::FitCardWidthSpan(requiredColumns, preferences.itemSize));
        preferences.fixedColumns = static_cast<std::uint32_t>(
            domain::ProjectCardColumns(preferences.widthSpan, preferences.itemSize));
        const auto itemCount = static_cast<std::uint32_t>(std::min<std::size_t>(
            card.items.size(), std::numeric_limits<std::uint32_t>::max()));
        if (itemCount > 0) {
            preferences.fixedRows = std::max(
                preferences.fixedRows,
                (itemCount + preferences.fixedColumns - 1) / preferences.fixedColumns);
        }
        return preferences;
    }

    SettingsAction actionAt(int x, int y) const noexcept {
        if (pendingStorageRoot.has_value()) {
            if (Contains(confirmationCancelRect(), x, y)) {
                return {SettingsActionKind::CancelStorageRootChange};
            }
            if (Contains(confirmationConfirmRect(), x, y)) {
                return {SettingsActionKind::ConfirmStorageRootChange};
            }
            return {};
        }
        if (pendingDeletion.has_value()) {
            if (Contains(confirmationCancelRect(), x, y)) {
                return {SettingsActionKind::CancelDeletion};
            }
            if (Contains(confirmationConfirmRect(), x, y)) {
                return {SettingsActionKind::ConfirmDeletion};
            }
            return {};
        }
        if (page == SettingsPage::Archive && archiveOverlay != ArchiveOverlay::None) {
            if (Contains(archiveOverlayCancelRect(), x, y)) {
                return {SettingsActionKind::CancelArchiveOverlay};
            }
            if (archiveOverlay == ArchiveOverlay::Add) {
                if (Contains(archiveOverlayCardRect(), x, y)) {
                    return {SettingsActionKind::CycleHistoricalArchiveCard};
                }
                if (Contains(archiveOverlayConfirmRect(), x, y)) {
                    return {SettingsActionKind::ConfirmHistoricalArchive};
                }
            } else {
                if (Contains(archiveExportDateRect(0), x, y)) {
                    return {SettingsActionKind::SelectArchiveExportBegin};
                }
                if (Contains(archiveExportDateRect(1), x, y)) {
                    return {SettingsActionKind::SelectArchiveExportEnd};
                }
                if (archiveExportDateField != ArchiveExportDateField::None) {
                    if (Contains(archiveExportCalendarPreviousRect(), x, y)) {
                        return {SettingsActionKind::PreviousArchiveExportMonth};
                    }
                    if (archiveExportCalendarCanAdvance()
                        && Contains(archiveExportCalendarNextRect(), x, y)) {
                        return {SettingsActionKind::NextArchiveExportMonth};
                    }
                    for (std::size_t index = 0; index < 42; ++index) {
                        if (domain::CompareTodoDates(
                                archiveExportCalendarCellDate(index),
                                domain::CurrentTodoDate(timeZoneOffsetMinutes)) <= 0
                            && Contains(archiveExportCalendarDayRect(index), x, y)) {
                            return {SettingsActionKind::SelectArchiveExportDate, index};
                        }
                    }
                }
                if (Contains(archiveOverlayConfirmRect(), x, y)) {
                    return {SettingsActionKind::ExportArchive};
                }
            }
            return {};
        }
        if (page == SettingsPage::Archive && archiveCalendarOpen) {
            if (Contains(archiveDateLabelRect(), x, y)) {
                return {SettingsActionKind::ToggleArchiveCalendar};
            }
            if (Contains(archiveCalendarPreviousRect(), x, y)) {
                return {SettingsActionKind::PreviousArchiveMonth};
            }
            if (archiveCalendarCanAdvance()
                && Contains(archiveCalendarNextRect(), x, y)) {
                return {SettingsActionKind::NextArchiveMonth};
            }
            for (std::size_t index = 0; index < 42; ++index) {
                const auto date = archiveCalendarCellDate(index);
                if (domain::CompareTodoDates(
                        date, domain::CurrentTodoDate(timeZoneOffsetMinutes)) <= 0
                    && Contains(archiveCalendarDayRect(index), x, y)) {
                    return {SettingsActionKind::SelectArchiveDate, index};
                }
            }
            return {SettingsActionKind::DismissArchiveCalendar};
        }
        for (std::size_t index = 0;
            index <= static_cast<std::size_t>(SettingsPage::About); ++index) {
            if (Contains(navigationRect(index), x, y)) {
                return {SettingsActionKind::Navigate, index};
            }
        }
        if (page == SettingsPage::System) {
            if (activeSystemDropdown != SystemDropdown::None) {
                for (std::size_t index = 0; index < systemDropdownOptionCount(); ++index) {
                    if (Contains(systemDropdownOptionRect(index), x, y)) {
                        const auto kind = activeSystemDropdown == SystemDropdown::TimeZone
                            ? SettingsActionKind::SelectTimeZoneOption
                            : activeSystemDropdown == SystemDropdown::Language
                            ? SettingsActionKind::SelectLanguageOption
                            : SettingsActionKind::SelectDesktopDoubleClickOption;
                        return {kind, index};
                    }
                }
                if (Contains(timeZoneRect(), x, y)) return {SettingsActionKind::SelectTimeZone};
                if (Contains(languageRect(), x, y)) return {SettingsActionKind::SelectLanguage};
                return {SettingsActionKind::DismissSystemDropdown};
            }
            for (std::size_t index = 0; index < 4; ++index) {
                if (Contains(radiusOptionRect(index), x, y)) {
                    return {SettingsActionKind::GlobalCornerRadius, index};
                }
            }
            if (Contains(timeZoneRect(), x, y)) return {SettingsActionKind::SelectTimeZone};
            if (Contains(languageRect(), x, y)) return {SettingsActionKind::SelectLanguage};
            if (Contains(storageRootRect(), x, y)) return {SettingsActionKind::ChangeStorageRoot};
            if (Contains(runAtStartupRect(), x, y)) return {SettingsActionKind::ToggleRunAtStartup};
        }
        if (page == SettingsPage::Features) {
            if (activeSystemDropdown != SystemDropdown::None) {
                for (std::size_t index = 0; index < systemDropdownOptionCount(); ++index) {
                    if (Contains(systemDropdownOptionRect(index), x, y)) {
                        return {
                            SettingsActionKind::SelectDesktopDoubleClickOption,
                            index};
                    }
                }
                if (Contains(desktopDoubleClickRect(), x, y)) {
                    return {SettingsActionKind::SelectDesktopDoubleClick};
                }
                return {SettingsActionKind::DismissSystemDropdown};
            }
            if (Contains(desktopDoubleClickRect(), x, y)) {
                return {SettingsActionKind::SelectDesktopDoubleClick};
            }
            if (Contains(taskbarDoubleClickRowRect(), x, y)) {
                return {SettingsActionKind::ToggleTaskbarDesktop};
            }
            if (Contains(pinnedFullscreenRect(), x, y)) {
                return {SettingsActionKind::TogglePinnedCardsYieldToFullscreen};
            }
            if (Contains(iconBackgroundFrameRect(), x, y)) {
                return {SettingsActionKind::ToggleIconBackgroundFrame};
            }
            if (Contains(fileDeletionConfirmationRect(), x, y)) {
                return {SettingsActionKind::ToggleFileDeletionConfirmation};
            }
        }
        if (page == SettingsPage::About) {
            if (Contains(openProjectRect(), x, y)) return {SettingsActionKind::OpenProject};
            if (Contains(checkForUpdatesRect(), x, y)) return {SettingsActionKind::CheckForUpdates};
            if (Contains(updateChannelRect(), x, y)) return {SettingsActionKind::ToggleUpdateChannel};
        }
        if (page == SettingsPage::Cards) {
            if (Contains(addCardRect(), x, y)) return {SettingsActionKind::AddCard};
            if (addMenuOpen) {
                if (Contains(addTypeRect(0), x, y)) return {SettingsActionKind::AddApplication};
                if (Contains(addTypeRect(1), x, y)) return {SettingsActionKind::AddMapping};
                if (Contains(addTypeRect(2), x, y)) return {SettingsActionKind::AddTodo};
            }
            // The card rail owns its hit area. Resolve it before detail actions
            // so a row click cannot be captured by a scrolled editor surface.
            for (std::size_t index = 0; index < cards.size(); ++index) {
                if (Contains(cardVisibilityRect(index), x, y)) {
                    return {SettingsActionKind::ToggleCardVisibility, index};
                }
                if (Contains(cardRow(index), x, y)) return {SettingsActionKind::SelectCard, index};
            }
            const auto detailY = !cards.empty() && selectedCard < cards.size()
                ? y + cardEditorOffset(cards[selectedCard]) : y;
            if (!cards.empty() && selectedCard < cards.size()) {
                const auto fileCard = cards[selectedCard].type == domain::CardType::Application
                    || cards[selectedCard].type == domain::CardType::Mapping;
                if (fileCard && Contains(filePresentationRect(), x, detailY)) return {
                    SettingsActionKind::TogglePresentationControl, selectedCard};
                if (Contains(renameButtonRect(), x, detailY)) {
                    return {SettingsActionKind::RenameCard, selectedCard};
                }
                if (cardMenuOpen) {
                    const auto fileCard = cards[selectedCard].type
                            == domain::CardType::Application
                        || cards[selectedCard].type == domain::CardType::Mapping;
                    if (fileCard) {
                        if (applicationSortMenuOpen) {
                            for (std::size_t index = 0; index < 5; ++index) {
                                if (Contains(cardMenuSortRect(index), x, detailY)) {
                                    return {SettingsActionKind::SelectApplicationSort,
                                        selectedCard, index};
                                }
                            }
                        }
                    }
                    if (Contains(cardMenuDeleteRect(), x, detailY)) {
                        return {SettingsActionKind::DeleteCard, selectedCard};
                    }
                    if (fileCard && Contains(cardMenuSortButtonRect(), x, detailY)) {
                        return {SettingsActionKind::ToggleApplicationSortMenu, selectedCard};
                    }
                    if (!Contains(cardMenuButtonRect(), x, detailY)) {
                        return {SettingsActionKind::DismissCardMenu};
                    }
                }
                if (Contains(cardMenuButtonRect(), x, detailY)) {
                    return {SettingsActionKind::OpenCardMenu, selectedCard};
                }
            }
            if (cards.empty() || selectedCard >= cards.size()) return {};
            for (std::size_t index = 0; index < 5; ++index) {
                if (Contains(appearanceRect(index), x, detailY)) {
                    return {static_cast<SettingsActionKind>(
                        static_cast<int>(SettingsActionKind::SystemAppearance) + static_cast<int>(index)),
                        selectedCard};
                }
            }
            const auto& card = cards[selectedCard];
            if (card.type == domain::CardType::Todo) {
                for (std::size_t index = 0; index < 3; ++index) {
                    if (Contains(compactCardSizeRect(index), x, detailY)) {
                        return {static_cast<SettingsActionKind>(
                            static_cast<int>(SettingsActionKind::SmallCardWidth)
                                + static_cast<int>(index)),
                            selectedCard};
                    }
                }
            }
            const auto contentPanel = card.type == domain::CardType::Todo
                ? todoContentPreviewRect(card) : contentPreviewRect(card);
            const auto contentIndices = contentItemIndices(card);
            const auto contentVisible = contentIndices.size();
            for (std::size_t visible = 0; visible < contentVisible; ++visible) {
                if (!Contains(contentPreviewRowRect(contentPanel, visible), x, detailY)) continue;
                const auto sourceIndex = contentIndices[visible];
                if (card.type == domain::CardType::Todo
                    && card.todoItems[sourceIndex].completed
                    && !card.todoItems[sourceIndex].archived) {
                    return {SettingsActionKind::ArchiveTodoItem,
                        selectedCard, sourceIndex};
                }
                break;
            }
            if (card.type == domain::CardType::Application || card.type == domain::CardType::Mapping) {
                for (std::size_t index = 0; index < 4; ++index) {
                    if (Contains(itemSizeRect(index), x, detailY)) {
                        return {static_cast<SettingsActionKind>(
                            static_cast<int>(SettingsActionKind::SmallItems) + static_cast<int>(index)),
                            selectedCard};
                    }
                }
                if (card.type == domain::CardType::Mapping) {
                    for (std::size_t index = 0; index < 2; ++index) {
                        if (Contains(mappingModeRect(index), x, detailY)) {
                            return {index == 0
                                ? SettingsActionKind::SelectMappingReferences
                                : SettingsActionKind::SelectMappingFolder,
                                selectedCard};
                        }
                    }
                }
                if (Contains(itemNamesRect(), x, detailY)) return {SettingsActionKind::ToggleItemNames, selectedCard};
                if (Contains(sizeModeRect(), x, detailY)) return {SettingsActionKind::ToggleSizeMode, selectedCard};
                if (Contains(heightLimitRect(), x, detailY)) return {SettingsActionKind::ToggleHeightLimit, selectedCard};
                if (card.content.maximumVisibleRows.has_value()) {
                    if (*card.content.maximumVisibleRows > 1
                        && Contains(decrementRect(maximumVisibleRowsRect()), x, detailY)) {
                        return {SettingsActionKind::DecreaseMaximumVisibleRows, selectedCard};
                    }
                    if (*card.content.maximumVisibleRows < 64
                        && Contains(incrementRect(maximumVisibleRowsRect()), x, detailY)) {
                        return {SettingsActionKind::IncreaseMaximumVisibleRows, selectedCard};
                    }
                }
                if (card.content.sizeMode == domain::CardSizeMode::Fixed) {
                    if (card.content.widthSpan > domain::MinimumCardWidthSpan(card.content.itemSize)
                        && fixedGridFits(card, card.content.widthSpan - 1,
                            card.content.fixedRows)
                        && Contains(decrementRect(fixedColumnsRect()), x, detailY)) {
                        return {SettingsActionKind::DecreaseFixedColumns, selectedCard};
                    }
                    if (card.content.widthSpan < 64
                        && Contains(incrementRect(fixedColumnsRect()), x, detailY)) {
                        return {SettingsActionKind::IncreaseFixedColumns, selectedCard};
                    }
                    if (card.content.fixedRows > 1
                        && fixedGridFits(card, card.content.widthSpan,
                            card.content.fixedRows - 1)
                        && Contains(decrementRect(fixedRowsRect()), x, detailY)) {
                        return {SettingsActionKind::DecreaseFixedRows, selectedCard};
                    }
                    if (card.content.fixedRows < 64
                        && Contains(incrementRect(fixedRowsRect()), x, detailY)) {
                        return {SettingsActionKind::IncreaseFixedRows, selectedCard};
                    }
                }
            } else if (card.type == domain::CardType::Todo) {
                if (Contains(createdTimeRect(), x, detailY)) {
                    return {SettingsActionKind::ToggleCreatedTime, selectedCard};
                }
                if (Contains(todoHeightLimitRect(), x, detailY)) {
                    return {SettingsActionKind::ToggleHeightLimit, selectedCard};
                }
                if (card.content.maximumVisibleRows.has_value()) {
                    if (*card.content.maximumVisibleRows > 1
                        && Contains(decrementRect(todoMaximumVisibleRowsRect()), x, detailY)) {
                        return {SettingsActionKind::DecreaseMaximumVisibleRows, selectedCard};
                    }
                    if (*card.content.maximumVisibleRows < 64
                        && Contains(incrementRect(todoMaximumVisibleRowsRect()), x, detailY)) {
                        return {SettingsActionKind::IncreaseMaximumVisibleRows, selectedCard};
                    }
                }
            }
            if (Contains(collapseRect(), x, detailY)) {
                return {SettingsActionKind::ToggleCollapseControl, selectedCard};
            }
            if (Contains(pinControlSettingRect(), x, detailY)) {
                return {SettingsActionKind::TogglePinControl, selectedCard};
            }
            if (Contains(positionLockRect(), x, detailY)) {
                return {SettingsActionKind::TogglePositionLock, selectedCard};
            }
        }
        if (page == SettingsPage::Archive) {
            if (Contains(archiveAddButtonRect(), x, y)) {
                return {SettingsActionKind::AddHistoricalArchive};
            }
            if (Contains(archiveExportButtonRect(), x, y)) {
                return {SettingsActionKind::ExportArchive};
            }
            if (Contains(archiveDateLabelRect(), x, y)) {
                return {SettingsActionKind::ToggleArchiveCalendar};
            }
            if (Contains(archiveDatePreviousRect(), x, y)) {
                return {SettingsActionKind::PreviousArchiveDate};
            }
            if (archiveDateOffset < 0 && Contains(archiveDateNextRect(), x, y)) {
                return {SettingsActionKind::NextArchiveDate};
            }
            const auto entries = archivedEntries();
            const auto count = std::min(visibleArchiveRows(), entries.size() - std::min(entries.size(), archiveOffset));
            for (std::size_t index = 0; index < count; ++index) {
                if (Contains(archiveRestoreRect(index), x, y)) {
                    const auto& entry = entries[archiveOffset + index];
                    return {SettingsActionKind::RestoreArchivedItem, entry.cardIndex, entry.itemIndex};
                }
                if (Contains(archiveDeleteRect(index), x, y)) {
                    const auto& entry = entries[archiveOffset + index];
                    return {SettingsActionKind::DeleteArchivedItem, entry.cardIndex, entry.itemIndex};
                }
            }
        }
        if (page == SettingsPage::About) {
            if (Contains(openProjectRect(), x, y)) return {SettingsActionKind::OpenProject};
            if (Contains(checkForUpdatesRect(), x, y)) return {SettingsActionKind::CheckForUpdates};
        }
        return {};
    }

    std::vector<SettingsAction> keyboardActions() const {
        if (pendingStorageRoot.has_value()) {
            return {{SettingsActionKind::CancelStorageRootChange},
                {SettingsActionKind::ConfirmStorageRootChange}};
        }
        if (pendingDeletion.has_value()) {
            return {{SettingsActionKind::CancelDeletion},
                {SettingsActionKind::ConfirmDeletion}};
        }

        std::vector<SettingsAction> result;
        const auto append = [&](SettingsAction action) {
            if (action.kind != SettingsActionKind::None
                && std::ranges::find(result, action) == result.end()) {
                result.push_back(action);
            }
        };
        for (std::size_t index = 0;
            index <= static_cast<std::size_t>(SettingsPage::About); ++index) {
            append({SettingsActionKind::Navigate, index});
        }

        const auto appendSystemDropdown = [&] {
            if (activeSystemDropdown == SystemDropdown::None) return;
            const auto kind = activeSystemDropdown == SystemDropdown::TimeZone
                ? SettingsActionKind::SelectTimeZoneOption
                : activeSystemDropdown == SystemDropdown::Language
                ? SettingsActionKind::SelectLanguageOption
                : SettingsActionKind::SelectDesktopDoubleClickOption;
            for (std::size_t index = 0; index < systemDropdownOptionCount(); ++index) {
                append({kind, index});
            }
        };

        if (page == SettingsPage::System) {
            append({SettingsActionKind::SelectTimeZone});
            append({SettingsActionKind::SelectLanguage});
            append({SettingsActionKind::ChangeStorageRoot});
            append({SettingsActionKind::ToggleRunAtStartup});
            for (std::size_t index = 0; index < 4; ++index) {
                append({SettingsActionKind::GlobalCornerRadius, index});
            }
            appendSystemDropdown();
            return result;
        }
        if (page == SettingsPage::Features) {
            append({SettingsActionKind::SelectDesktopDoubleClick});
            append({SettingsActionKind::ToggleTaskbarDesktop});
            append({SettingsActionKind::TogglePinnedCardsYieldToFullscreen});
            append({SettingsActionKind::ToggleIconBackgroundFrame});
            appendSystemDropdown();
            return result;
        }
        if (page == SettingsPage::Cards) {
            append({SettingsActionKind::AddCard});
            if (addMenuOpen) {
                append({SettingsActionKind::AddApplication});
                append({SettingsActionKind::AddMapping});
                append({SettingsActionKind::AddTodo});
            }
            for (std::size_t index = 0; index < cards.size(); ++index) {
                append({SettingsActionKind::SelectCard, index});
                append({SettingsActionKind::ToggleCardVisibility, index});
            }
            if (cards.empty() || selectedCard >= cards.size()) return result;
            const auto& card = cards[selectedCard];
            append({SettingsActionKind::RenameCard, selectedCard});
            append({SettingsActionKind::OpenCardMenu, selectedCard});
            if (cardMenuOpen) {
                const auto fileCard = card.type == domain::CardType::Application
                    || card.type == domain::CardType::Mapping;
                if (fileCard) {
                    append({SettingsActionKind::ToggleApplicationSortMenu, selectedCard});
                    if (applicationSortMenuOpen) {
                        for (std::size_t index = 0; index < 5; ++index) {
                            append({SettingsActionKind::SelectApplicationSort,
                                selectedCard, index});
                        }
                    }
                }
                append({SettingsActionKind::DeleteCard, selectedCard});
            }
            for (auto kind = SettingsActionKind::SystemAppearance;
                 kind <= SettingsActionKind::TransparentAppearance;
                 kind = static_cast<SettingsActionKind>(static_cast<int>(kind) + 1)) {
                append({kind, selectedCard});
            }
                if (card.type == domain::CardType::Application
                || card.type == domain::CardType::Mapping) {
                append({SettingsActionKind::TogglePresentationControl, selectedCard});
                for (auto kind = SettingsActionKind::SmallItems;
                     kind <= SettingsActionKind::ExtraLargeItems;
                     kind = static_cast<SettingsActionKind>(static_cast<int>(kind) + 1)) {
                    append({kind, selectedCard});
                }
                if (card.type == domain::CardType::Mapping) {
                    append({SettingsActionKind::SelectMappingReferences, selectedCard});
                    append({SettingsActionKind::SelectMappingFolder, selectedCard});
                }
                append({SettingsActionKind::ToggleItemNames, selectedCard});
                append({SettingsActionKind::ToggleSizeMode, selectedCard});
                append({SettingsActionKind::ToggleHeightLimit, selectedCard});
                if (card.content.sizeMode == domain::CardSizeMode::Fixed) {
                    append({SettingsActionKind::DecreaseFixedColumns, selectedCard});
                    append({SettingsActionKind::IncreaseFixedColumns, selectedCard});
                    append({SettingsActionKind::DecreaseFixedRows, selectedCard});
                    append({SettingsActionKind::IncreaseFixedRows, selectedCard});
                }
                if (card.content.maximumVisibleRows.has_value()) {
                    append({SettingsActionKind::DecreaseMaximumVisibleRows, selectedCard});
                    append({SettingsActionKind::IncreaseMaximumVisibleRows, selectedCard});
                }
            } else if (card.type == domain::CardType::Todo) {
                append({SettingsActionKind::SmallCardWidth, selectedCard});
                append({SettingsActionKind::MediumCardWidth, selectedCard});
                append({SettingsActionKind::LargeCardWidth, selectedCard});
                append({SettingsActionKind::ToggleCreatedTime, selectedCard});
                append({SettingsActionKind::ToggleHeightLimit, selectedCard});
                if (card.content.maximumVisibleRows.has_value()) {
                    append({SettingsActionKind::DecreaseMaximumVisibleRows, selectedCard});
                    append({SettingsActionKind::IncreaseMaximumVisibleRows, selectedCard});
                }
                for (std::size_t index = 0; index < card.todoItems.size(); ++index) {
                    if (card.todoItems[index].completed && !card.todoItems[index].archived) {
                        append({SettingsActionKind::ArchiveTodoItem, selectedCard, index});
                    }
                }
            }
            append({SettingsActionKind::ToggleCollapseControl, selectedCard});
            append({SettingsActionKind::TogglePinControl, selectedCard});
            return result;
        }
        if (page == SettingsPage::Archive) {
            append({SettingsActionKind::PreviousArchiveDate});
            append({SettingsActionKind::ToggleArchiveCalendar});
            if (archiveDateOffset < 0) append({SettingsActionKind::NextArchiveDate});
            append({SettingsActionKind::FocusArchiveSearch});
            const auto entries = archivedEntries();
            const auto begin = std::min(archiveOffset, entries.size());
            const auto count = std::min(visibleArchiveRows(), entries.size() - begin);
            for (std::size_t index = 0; index < count; ++index) {
                const auto& entry = entries[begin + index];
                append({SettingsActionKind::RestoreArchivedItem,
                    entry.cardIndex, entry.itemIndex});
                append({SettingsActionKind::DeleteArchivedItem,
                    entry.cardIndex, entry.itemIndex});
            }
        }
        if (page == SettingsPage::About) {
            append({SettingsActionKind::OpenProject});
            append({SettingsActionKind::CheckForUpdates});
        }
        return result;
    }

    void advanceKeyboardFocus(bool reverse) noexcept {
        const auto actions = keyboardActions();
        if (actions.empty()) return;
        auto found = keyboardFocus.has_value()
            ? std::ranges::find(actions, *keyboardFocus) : actions.end();
        std::size_t index = 0;
        if (found == actions.end()) {
            index = reverse ? actions.size() - 1 : 0;
        } else {
            const auto current = static_cast<std::size_t>(found - actions.begin());
            index = reverse
                ? (current == 0 ? actions.size() - 1 : current - 1)
                : (current + 1) % actions.size();
        }
        keyboardFocus = actions[index];
        hovered = *keyboardFocus;
        if (GetFocus() != window) SetFocus(window);
        InvalidateRect(window, nullptr, FALSE);
    }

    void activateKeyboardFocus() noexcept {
        if (!keyboardFocus.has_value()) {
            advanceKeyboardFocus(false);
            return;
        }
        const auto action = *keyboardFocus;
        pressed = action;
        apply(action);
        pressed = {};
        const auto actions = keyboardActions();
        if (std::ranges::find(actions, action) == actions.end()) {
            keyboardFocus.reset();
        }
        hovered = keyboardFocus.value_or(SettingsAction{});
        InvalidateRect(window, nullptr, FALSE);
    }

    bool handleKeyboardKey(WPARAM key) noexcept {
        if (key == VK_TAB || key == VK_RIGHT || key == VK_DOWN
            || key == VK_LEFT || key == VK_UP) {
            const auto reverse = key == VK_LEFT || key == VK_UP
                || (key == VK_TAB && (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            advanceKeyboardFocus(reverse);
            return true;
        }
        if (key == VK_RETURN || key == VK_SPACE) {
            activateKeyboardFocus();
            return true;
        }
        if (key != VK_ESCAPE) return false;
        if (pendingStorageRoot.has_value()) {
            apply({SettingsActionKind::CancelStorageRootChange});
        } else if (pendingDeletion.has_value()) {
            apply({SettingsActionKind::CancelDeletion});
        } else if (archiveCalendarOpen) {
            archiveCalendarOpen = false;
        } else if (applicationSortMenuOpen) {
            applicationSortMenuOpen = false;
        } else if (cardMenuOpen) {
            cardMenuOpen = false;
        } else if (addMenuOpen) {
            addMenuOpen = false;
        } else if (activeSystemDropdown != SystemDropdown::None) {
            activeSystemDropdown = SystemDropdown::None;
        } else {
            ShowWindow(window, SW_HIDE);
        }
        hovered = keyboardFocus.value_or(SettingsAction{});
        InvalidateRect(window, nullptr, FALSE);
        return true;
    }

    void updateHover(int x, int y, WPARAM keyState) noexcept {
        TRACKMOUSEEVENT tracking{
            .cbSize = sizeof(TRACKMOUSEEVENT),
            .dwFlags = TME_LEAVE,
            .hwndTrack = window,
        };
        TrackMouseEvent(&tracking);
        if (cardDragSource.has_value() && GetCapture() == window
            && ((keyState & MK_LBUTTON) != 0
                || (GetKeyState(VK_LBUTTON) & 0x8000) != 0)) {
            if (!cardDragActive) {
                const auto held = GetTickCount64() - cardDragStartedAt >= 180;
                const auto moved = std::abs(x - cardDragStart.x) >= 3
                    || std::abs(y - cardDragStart.y) >= 3;
                cardDragActive = held && moved;
            }
            if (cardDragActive) {
                auto target = *cardDragSource;
                for (std::size_t index = 0; index < cards.size(); ++index) {
                    const auto row = cardRow(index);
                    if (y < (row.top + row.bottom) / 2) {
                        target = index;
                        break;
                    }
                    target = index;
                }
                if (cardDragTarget != target) {
                    cardDragTarget = target;
                    InvalidateRect(window, nullptr, FALSE);
                }
                return;
            }
        }
        const auto next = actionAt(x, y);
        if (next == hovered) return;
        hovered = next;
        InvalidateRect(window, nullptr, FALSE);
    }

    void beginPress(int x, int y) noexcept {
        // Parent-surface actions do not automatically steal focus from the
        // custom text input. Move focus back first so focus loss commits the
        // current value before another setting is applied.
        if (renameEdit != nullptr && GetFocus() == renameEdit) {
            SetFocus(window);
        }
        keyboardFocus.reset();
        pressed = actionAt(x, y);
        hovered = pressed;
        if (pressed.kind == SettingsActionKind::SelectCard) {
            cardDragSource = pressed.index;
            cardDragTarget = pressed.index;
            cardDragStartedAt = GetTickCount64();
            cardDragStart = {x, y};
            cardDragActive = false;
        } else {
            cardDragSource.reset();
            cardDragTarget.reset();
            cardDragActive = false;
        }
        if (pressed.kind != SettingsActionKind::None) SetCapture(window);
        InvalidateRect(window, nullptr, FALSE);
    }

    void endPress(int x, int y) noexcept {
        if (cardDragActive && cardDragSource.has_value() && cardDragTarget.has_value()
            && *cardDragSource < cards.size() && *cardDragTarget < cards.size()) {
            const auto original = cards;
            const auto selectedId = selectedCard < cards.size()
                ? std::optional<domain::CardId>(cards[selectedCard].id) : std::nullopt;
            auto moved = std::move(cards[*cardDragSource]);
            cards.erase(cards.begin() + static_cast<std::ptrdiff_t>(*cardDragSource));
            cards.insert(cards.begin() + static_cast<std::ptrdiff_t>(*cardDragTarget),
                std::move(moved));
            std::vector<domain::CardId> order;
            order.reserve(cards.size());
            for (const auto& card : cards) order.push_back(card.id);
            if (cardOrderChanged && !cardOrderChanged(order)) cards = original;
            if (selectedId.has_value()) {
                const auto selected = std::ranges::find(
                    cards, *selectedId, &presentation::CardView::id);
                selectedCard = selected == cards.end() ? 0
                    : static_cast<std::size_t>(selected - cards.begin());
            }
            pressed = {};
            cardDragSource.reset();
            cardDragTarget.reset();
            cardDragActive = false;
            if (GetCapture() == window) ReleaseCapture();
            InvalidateRect(window, nullptr, FALSE);
            return;
        }
        const auto action = actionAt(x, y);
        const auto commit = action == pressed ? action : SettingsAction{};
        pressed = {};
        cardDragSource.reset();
        cardDragTarget.reset();
        cardDragActive = false;
        if (GetCapture() == window) ReleaseCapture();
        if (commit.kind != SettingsActionKind::None) apply(commit);
        InvalidateRect(window, nullptr, FALSE);
    }

    bool usesEnglish() const noexcept {
        if (language == "en-US") return true;
        if (language == "zh-CN") return false;
        return PRIMARYLANGID(GetUserDefaultUILanguage()) != LANG_CHINESE;
    }

    std::wstring tr(std::wstring_view chinese, std::wstring_view english) const {
        return std::wstring(usesEnglish() ? english : chinese);
    }

    std::wstring timeZoneText() const {
        if (!timeZoneOffsetMinutes.has_value()) return tr(L"跟随系统", L"System");
        const auto offset = *timeZoneOffsetMinutes;
        const auto absolute = std::abs(offset);
        wchar_t value[32]{};
        swprintf_s(value, L"UTC%c%02d:%02d", offset >= 0 ? L'+' : L'-',
            absolute / 60, absolute % 60);
        return value;
    }

    std::wstring languageText() const {
        if (language == "zh-CN") return L"简体中文";
        if (language == "en-US") return L"English";
        return tr(L"跟随系统", L"System");
    }

    std::wstring desktopDoubleClickText() const {
        if (desktopDoubleClickAction == "none") return tr(L"不处理", L"Do nothing");
        if (desktopDoubleClickAction == "icons") return tr(L"桌面图标", L"Desktop icons");
        if (desktopDoubleClickAction == "cards") return tr(L"桌面卡片", L"Desktop cards");
        return tr(L"图标与卡片", L"Icons and cards");
    }

    std::wstring taskbarDoubleClickText() const {
        return taskbarDoubleClickAction == "none"
            ? tr(L"不处理", L"Do nothing")
            : tr(L"开启", L"On");
    }

    void apply(const SettingsAction& action) noexcept {
        try {
            if (action.kind == SettingsActionKind::OpenProject) {
                ShellExecuteW(window, L"open", L"https://github.com/LectWolf/Desto",
                    nullptr, nullptr, SW_SHOWNORMAL);
                return;
            }
            if (action.kind == SettingsActionKind::CheckForUpdates) {
                std::optional<std::string> metadata;
                const bool developmentChannel = updateChannel == "development";
                metadata = DownloadUpdateMetadata(L"api.github.com",
                    developmentChannel
                        ? L"/repos/LectWolf/Desto/releases?per_page=1"
                        : L"/repos/LectWolf/Desto/releases/latest");
                if (!metadata) {
                    metadata = DownloadUpdateMetadata(L"ghproxy.net",
                        developmentChannel
                            ? L"/https://api.github.com/repos/LectWolf/Desto/releases?per_page=1"
                            : L"/https://api.github.com/repos/LectWolf/Desto/releases/latest");
                }
                if (!metadata) {
                    MessageBoxW(window,
                        tr(L"暂时无法连接更新服务器，请稍后重试。",
                            L"Unable to reach the update servers. Please try again later.").c_str(),
                        L"Desto", MB_OK | MB_ICONWARNING);
                    return;
                }
                auto tag = JsonStringField(*metadata, "tag_name");
                auto download = JsonStringField(*metadata, "browser_download_url");
                if (!tag.empty() && tag.front() == L'v') tag.erase(tag.begin());
                const auto current = CurrentDestoVersion(developmentChannel);
                if (tag.empty() || CompareDestoVersions(tag, current) <= 0) {
                    MessageBoxW(window,
                        tr(L"当前已是最新版本。", L"You are already up to date.").c_str(),
                        L"Desto", MB_OK | MB_ICONINFORMATION);
                    return;
                }
                const auto prompt = tr(
                    (L"发现新版本 " + tag + L"，是否打开下载页面？").c_str(),
                    (L"Version " + tag + L" is available. Open the download page?").c_str());
                if (MessageBoxW(window, prompt.c_str(), L"Desto",
                        MB_YESNO | MB_ICONINFORMATION) == IDYES) {
                    const auto url = download.empty()
                        ? L"https://github.com/LectWolf/Desto/releases/latest"
                        : download;
                    if (download.empty()) {
                        ShellExecuteW(window, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    } else {
                        const auto installer = DownloadInstaller(url);
                        if (!installer) {
                            MessageBoxW(window,
                                tr(L"下载安装包失败，请稍后重试。",
                                    L"The installer could not be downloaded. Please try again later.").c_str(),
                                L"Desto", MB_OK | MB_ICONWARNING);
                            return;
                        }
                        ShellExecuteW(window, L"open", installer->c_str(), nullptr, nullptr,
                            SW_SHOWNORMAL);
                    }
                }
                return;
            }
            if (action.kind == SettingsActionKind::ToggleUpdateChannel) {
                const auto next = updateChannel == "development" ? "stable" : "development";
                if (updateChannelChanged && updateChannelChanged(next)) updateChannel = next;
                InvalidateRect(window, nullptr, FALSE);
                return;
            }
            if (action.kind == SettingsActionKind::Navigate) {
                closeRenameEditor(true);
                page = static_cast<SettingsPage>(action.index);
                addMenuOpen = false;
                cardMenuOpen = false;
                applicationSortMenuOpen = false;
                activeSystemDropdown = SystemDropdown::None;
                archiveCalendarOpen = false;
                archiveOverlay = ArchiveOverlay::None;
                updateArchiveSearchEditor();
                updateArchiveAddEditor();
                return;
            }
            if (action.kind == SettingsActionKind::AddHistoricalArchive) {
                const auto firstTodo = std::ranges::find(
                    cards, domain::CardType::Todo, &presentation::CardView::type);
                if (firstTodo == cards.end()) return;
                historicalArchiveCard = static_cast<std::size_t>(
                    std::distance(cards.begin(), firstTodo));
                archiveOverlay = ArchiveOverlay::Add;
                archiveCalendarOpen = false;
                SetWindowTextW(archiveAddEdit, L"");
                updateArchiveSearchEditor();
                updateArchiveAddEditor();
                SetFocus(archiveAddEdit);
                return;
            }
            if (action.kind == SettingsActionKind::CycleHistoricalArchiveCard) {
                if (cards.empty()) return;
                for (std::size_t offset = 1; offset <= cards.size(); ++offset) {
                    const auto index = (historicalArchiveCard + offset) % cards.size();
                    if (cards[index].type == domain::CardType::Todo) {
                        historicalArchiveCard = index;
                        break;
                    }
                }
                return;
            }
            if (action.kind == SettingsActionKind::ConfirmHistoricalArchive) {
                const auto text = currentEditText(archiveAddEdit);
                if (historicalArchiveCard >= cards.size() || text.empty()
                    || !historicalArchiveAdded) return;
                const auto cardId = cards[historicalArchiveCard].id;
                const auto added = historicalArchiveAdded(
                    cardId, text, selectedArchiveDate());
                if (!added.has_value()) return;
                cards[historicalArchiveCard].todoItems.push_back(*added);
                archiveOverlay = ArchiveOverlay::None;
                updateArchiveAddEditor();
                updateArchiveSearchEditor();
                clampArchiveOffset();
                return;
            }
            if (action.kind == SettingsActionKind::ExportArchive) {
                if (archiveOverlay != ArchiveOverlay::Export) {
                    archiveOverlay = ArchiveOverlay::Export;
                    archiveCalendarOpen = false;
                    archiveExportEnd = selectedArchiveDate();
                    archiveExportBegin = domain::AddTodoDays(archiveExportEnd, -6);
                    archiveExportCalendarMonth = {
                        archiveExportEnd.year, archiveExportEnd.month, 1};
                    archiveExportDateField = ArchiveExportDateField::None;
                    updateArchiveSearchEditor();
                    updateArchiveAddEditor();
                    return;
                }
                if (!archiveExport) return;
                const auto destination = PickArchiveExportFile(window, usesEnglish());
                if (destination.has_value()
                    && archiveExport(archiveExportBegin, archiveExportEnd, *destination)) {
                    archiveOverlay = ArchiveOverlay::None;
                    archiveExportDateField = ArchiveExportDateField::None;
                    updateArchiveSearchEditor();
                }
                return;
            }
            if (action.kind == SettingsActionKind::SelectArchiveExportBegin
                || action.kind == SettingsActionKind::SelectArchiveExportEnd) {
                archiveExportDateField = action.kind
                        == SettingsActionKind::SelectArchiveExportBegin
                    ? ArchiveExportDateField::Begin : ArchiveExportDateField::End;
                const auto value = archiveExportDateField == ArchiveExportDateField::Begin
                    ? archiveExportBegin : archiveExportEnd;
                archiveExportCalendarMonth = {value.year, value.month, 1};
                return;
            }
            if (action.kind == SettingsActionKind::PreviousArchiveExportMonth) {
                shiftArchiveExportCalendarMonth(-1);
                return;
            }
            if (action.kind == SettingsActionKind::NextArchiveExportMonth) {
                shiftArchiveExportCalendarMonth(1);
                return;
            }
            if (action.kind == SettingsActionKind::SelectArchiveExportDate) {
                const auto value = archiveExportCalendarCellDate(action.index);
                if (archiveExportDateField == ArchiveExportDateField::Begin) {
                    archiveExportBegin = value;
                    if (domain::CompareTodoDates(archiveExportBegin, archiveExportEnd) > 0) {
                        archiveExportEnd = archiveExportBegin;
                    }
                } else if (archiveExportDateField == ArchiveExportDateField::End) {
                    archiveExportEnd = value;
                    if (domain::CompareTodoDates(archiveExportEnd, archiveExportBegin) < 0) {
                        archiveExportBegin = archiveExportEnd;
                    }
                }
                archiveExportDateField = ArchiveExportDateField::None;
                return;
            }
            if (action.kind == SettingsActionKind::CancelArchiveOverlay) {
                archiveOverlay = ArchiveOverlay::None;
                archiveExportDateField = ArchiveExportDateField::None;
                updateArchiveAddEditor();
                updateArchiveSearchEditor();
                return;
            }
            if (action.kind == SettingsActionKind::SelectTimeZone) {
                activeSystemDropdown = activeSystemDropdown == SystemDropdown::TimeZone
                    ? SystemDropdown::None : SystemDropdown::TimeZone;
                return;
            }
            if (action.kind == SettingsActionKind::SelectLanguage) {
                activeSystemDropdown = activeSystemDropdown == SystemDropdown::Language
                    ? SystemDropdown::None : SystemDropdown::Language;
                return;
            }
            if (action.kind == SettingsActionKind::SelectDesktopDoubleClick) {
                activeSystemDropdown = activeSystemDropdown == SystemDropdown::DesktopDoubleClick
                    ? SystemDropdown::None : SystemDropdown::DesktopDoubleClick;
                return;
            }
            if (action.kind == SettingsActionKind::ToggleTaskbarDesktop) {
                const std::string next = taskbarDoubleClickAction == "none"
                    ? "current-display" : "none";
                if (taskbarDoubleClickActionChanged
                    && taskbarDoubleClickActionChanged(next)) {
                    taskbarDoubleClickAction = next;
                }
                return;
            }
            if (action.kind == SettingsActionKind::DismissSystemDropdown) {
                activeSystemDropdown = SystemDropdown::None;
                return;
            }
            if (action.kind == SettingsActionKind::SelectTimeZoneOption) {
                const std::array<std::optional<std::int32_t>, 4> values{
                    std::nullopt, 0, 8 * 60, -5 * 60};
                if (action.index < values.size() && timeZoneChanged
                    && timeZoneChanged(values[action.index])) {
                    timeZoneOffsetMinutes = values[action.index];
                }
                activeSystemDropdown = SystemDropdown::None;
                return;
            }
            if (action.kind == SettingsActionKind::SelectLanguageOption) {
                constexpr std::array<std::string_view, 3> values{
                    "system", "zh-CN", "en-US"};
                if (action.index < values.size()) {
                    const std::string next(values[action.index]);
                    if (languageChanged && languageChanged(next)) language = next;
                }
                activeSystemDropdown = SystemDropdown::None;
                return;
            }
            if (action.kind == SettingsActionKind::SelectDesktopDoubleClickOption) {
                constexpr std::array<std::string_view, 4> values{
                    "none", "icons", "cards", "all"};
                if (action.index < values.size()) {
                    const std::string next(values[action.index]);
                    if (desktopDoubleClickActionChanged
                        && desktopDoubleClickActionChanged(next)) {
                        desktopDoubleClickAction = next;
                    }
                }
                activeSystemDropdown = SystemDropdown::None;
                return;
            }
            if (action.kind == SettingsActionKind::TogglePinnedCardsYieldToFullscreen) {
                const auto next = !pinnedCardsYieldToFullscreen;
                if (pinnedCardsYieldToFullscreenChanged
                    && pinnedCardsYieldToFullscreenChanged(next)) {
                    pinnedCardsYieldToFullscreen = next;
                }
                return;
            }
            if (action.kind == SettingsActionKind::ToggleIconBackgroundFrame) {
                const auto next = !showIconBackgroundFrame;
                if (iconBackgroundFrameChanged
                    && iconBackgroundFrameChanged(next)) {
                    showIconBackgroundFrame = next;
                }
                return;
            }
            if (action.kind == SettingsActionKind::ToggleFileDeletionConfirmation) {
                const auto next = !confirmFileDeletion;
                if (fileDeletionConfirmationChanged
                    && fileDeletionConfirmationChanged(next)) {
                    confirmFileDeletion = next;
                }
                return;
            }
            if (action.kind == SettingsActionKind::GlobalCornerRadius) {
                constexpr std::array<double, 4> radii{0.0, 12.0, 24.0, 32.0};
                if (action.index < radii.size() && globalCornerRadiusChanged
                    && globalCornerRadiusChanged(radii[action.index], true)) {
                    globalCornerRadius = radii[action.index];
                    for (auto& card : cards) card.cornerRadius = radii[action.index];
                }
                return;
            }
            if (action.kind == SettingsActionKind::ChangeStorageRoot) {
                const auto selected = PickFolder(window, usesEnglish());
                if (selected.has_value()
                    && selected->lexically_normal() != storageRoot.lexically_normal()) {
                    pendingStorageRoot = selected->lexically_normal();
                    keyboardFocus = SettingsAction{SettingsActionKind::CancelStorageRootChange};
                    hovered = *keyboardFocus;
                }
                return;
            }
            if (action.kind == SettingsActionKind::CancelStorageRootChange) {
                pendingStorageRoot.reset();
                keyboardFocus.reset();
                return;
            }
            if (action.kind == SettingsActionKind::ConfirmStorageRootChange) {
                if (!pendingStorageRoot.has_value()) return;
                const auto selected = *pendingStorageRoot;
                if (storageRootChanged && storageRootChanged(selected)) {
                    storageRoot = selected;
                    pendingStorageRoot.reset();
                    keyboardFocus.reset();
                }
                return;
            }
            if (action.kind == SettingsActionKind::ToggleRunAtStartup) {
                const auto next = !runAtStartup;
                if (runAtStartupChanged && runAtStartupChanged(next)) {
                    runAtStartup = next;
                }
                return;
            }
            if (action.kind == SettingsActionKind::AddCard) {
                addMenuOpen = !addMenuOpen;
                return;
            }
            if (action.kind == SettingsActionKind::AddApplication
                || action.kind == SettingsActionKind::AddMapping
                || action.kind == SettingsActionKind::AddTodo) {
                const auto type = action.kind == SettingsActionKind::AddApplication
                    ? domain::CardType::Application
                    : action.kind == SettingsActionKind::AddMapping
                    ? domain::CardType::Mapping : domain::CardType::Todo;
                if (cardAdded) {
                    auto added = cardAdded(type);
                    if (added.has_value()) {
                        cards.push_back(std::move(*added));
                        selectedCard = cards.size() - 1;
                    }
                }
                addMenuOpen = false;
                return;
            }
            if (action.kind == SettingsActionKind::SelectCard) {
                closeRenameEditor(true);
                selectedCard = action.index;
                cardMenuOpen = false;
                applicationSortMenuOpen = false;
                clampCardEditorOffset();
                return;
            }
            if (action.kind == SettingsActionKind::ToggleCardVisibility) {
                if (action.index >= cards.size()) return;
                auto& card = cards[action.index];
                const auto next = !card.visible;
                if (cardVisibilityChanged && cardVisibilityChanged(card.id, next)) {
                    card.visible = next;
                }
                return;
            }
            if (action.kind == SettingsActionKind::OpenCardMenu) {
                cardMenuOpen = !cardMenuOpen;
                return;
            }
            if (action.kind == SettingsActionKind::DismissCardMenu) {
                cardMenuOpen = false;
                return;
            }
            if (action.kind == SettingsActionKind::RenameCard) {
                cardMenuOpen = false;
                beginRename(action.index);
                return;
            }
            if (action.kind == SettingsActionKind::DeleteCard) {
                if (action.index >= cards.size()) return;
                closeRenameEditor(false);
                cardMenuOpen = false;
                pendingDeletion = action;
                updateArchiveSearchEditor();
                return;
            }
            if (action.kind == SettingsActionKind::FocusArchiveSearch) {
                if (archiveSearchEdit != nullptr && IsWindowVisible(archiveSearchEdit)) {
                    SetFocus(archiveSearchEdit);
                }
                return;
            }
            if (action.kind == SettingsActionKind::ToggleArchiveCalendar) {
                archiveCalendarOpen = !archiveCalendarOpen;
                if (archiveCalendarOpen) {
                    archiveCalendarMonth = selectedArchiveDate();
                    archiveCalendarMonth.day = 1;
                }
                updateArchiveSearchEditor();
                return;
            }
            if (action.kind == SettingsActionKind::DismissArchiveCalendar) {
                archiveCalendarOpen = false;
                updateArchiveSearchEditor();
                return;
            }
            if (action.kind == SettingsActionKind::PreviousArchiveMonth) {
                shiftArchiveCalendarMonth(-1);
                return;
            }
            if (action.kind == SettingsActionKind::NextArchiveMonth) {
                if (archiveCalendarCanAdvance()) shiftArchiveCalendarMonth(1);
                return;
            }
            if (action.kind == SettingsActionKind::SelectArchiveDate) {
                if (action.index >= 42) return;
                const auto date = archiveCalendarCellDate(action.index);
                const auto today = domain::CurrentTodoDate(timeZoneOffsetMinutes);
                if (domain::CompareTodoDates(date, today) > 0) return;
                const auto difference = (toSysDays(date) - toSysDays(today)).count();
                archiveDateOffset = static_cast<std::int32_t>(std::clamp<std::int64_t>(
                    difference,
                    std::numeric_limits<std::int32_t>::min(),
                    0));
                archiveOffset = 0;
                archiveCalendarOpen = false;
                clampArchiveOffset();
                updateArchiveSearchEditor();
                return;
            }
            if (action.kind == SettingsActionKind::PreviousArchiveDate
                || action.kind == SettingsActionKind::NextArchiveDate) {
                archiveCalendarOpen = false;
                archiveDateOffset += action.kind == SettingsActionKind::PreviousArchiveDate
                    ? -1 : 1;
                archiveDateOffset = std::min<std::int32_t>(archiveDateOffset, 0);
                archiveOffset = 0;
                clampArchiveOffset();
                updateArchiveSearchEditor();
                return;
            }
            if (action.kind == SettingsActionKind::RestoreArchivedItem) {
                if (action.index >= cards.size()
                    || action.secondaryIndex >= cards[action.index].todoItems.size()) return;
                const auto cardId = cards[action.index].id;
                const auto itemId = cards[action.index].todoItems[action.secondaryIndex].id;
                if (restoreArchivedItem
                    && restoreArchivedItem(cardId, itemId)) {
                    const auto card = std::ranges::find(
                        cards, cardId, &presentation::CardView::id);
                    if (card != cards.end()) {
                        const auto item = std::ranges::find(
                            card->todoItems, itemId, &domain::TodoItem::id);
                        if (item != card->todoItems.end()) {
                            item->archived = false;
                            if (item->completed) {
                                item->completedAtUnixMilliseconds = UnixMillisecondsNow();
                            }
                        }
                    }
                    clampArchiveOffset();
                }
                return;
            }
            if (action.kind == SettingsActionKind::ArchiveTodoItem) {
                if (action.index >= cards.size()
                    || action.secondaryIndex >= cards[action.index].todoItems.size()) return;
                const auto cardId = cards[action.index].id;
                const auto itemId = cards[action.index].todoItems[action.secondaryIndex].id;
                if (!cards[action.index].todoItems[action.secondaryIndex].completed
                    || cards[action.index].todoItems[action.secondaryIndex].archived) return;
                if (archiveTodoItem
                    && archiveTodoItem(cardId, itemId)) {
                    const auto card = std::ranges::find(
                        cards, cardId, &presentation::CardView::id);
                    if (card != cards.end()) {
                        const auto item = std::ranges::find(
                            card->todoItems, itemId, &domain::TodoItem::id);
                        if (item != card->todoItems.end()) item->archived = true;
                    }
                    clampCardEditorOffset();
                }
                return;
            }
            if (action.kind == SettingsActionKind::DeleteArchivedItem) {
                if (action.index >= cards.size()
                    || action.secondaryIndex >= cards[action.index].todoItems.size()) return;
                pendingDeletion = action;
                updateArchiveSearchEditor();
                return;
            }
            if (action.kind == SettingsActionKind::CancelDeletion) {
                pendingDeletion.reset();
                updateArchiveSearchEditor();
                return;
            }
            if (action.kind == SettingsActionKind::ConfirmDeletion) {
                if (!pendingDeletion.has_value()) return;
                const auto deletion = *pendingDeletion;
                pendingDeletion.reset();
                if (deletion.kind == SettingsActionKind::DeleteCard) {
                    if (deletion.index < cards.size() && cardDeleted
                        && cardDeleted(cards[deletion.index].id)) {
                        cards.erase(cards.begin() + static_cast<std::ptrdiff_t>(deletion.index));
                        if (selectedCard >= cards.size()) {
                            selectedCard = cards.empty() ? 0 : cards.size() - 1;
                        }
                    }
                } else if (deletion.kind == SettingsActionKind::DeleteArchivedItem
                    && deletion.index < cards.size()
                    && deletion.secondaryIndex < cards[deletion.index].todoItems.size()) {
                    const auto cardId = cards[deletion.index].id;
                    const auto itemId = cards[deletion.index]
                        .todoItems[deletion.secondaryIndex].id;
                    if (deleteArchivedItem
                        && deleteArchivedItem(cardId, itemId)) {
                        const auto card = std::ranges::find(
                            cards, cardId, &presentation::CardView::id);
                        if (card != cards.end()) {
                            const auto item = std::ranges::find(
                                card->todoItems, itemId, &domain::TodoItem::id);
                            if (item != card->todoItems.end()) card->todoItems.erase(item);
                        }
                        clampArchiveOffset();
                    }
                } else if ((deletion.kind == SettingsActionKind::SelectMappingReferences
                            || deletion.kind == SettingsActionKind::SelectMappingFolder)
                           && deletion.index < cards.size()) {
                    auto& mappingCard = cards[deletion.index];
                    const auto mode = deletion.kind == SettingsActionKind::SelectMappingFolder
                        ? domain::MappingMode::Folder : domain::MappingMode::References;
                    if (mappingModeChanged && mappingModeChanged(mappingCard.id, mode)) {
                        mappingCard.mappingMode = mode;
                        mappingCard.mappingHasSource = false;
                        mappingCard.items.clear();
                    }
                }
                updateArchiveSearchEditor();
                return;
            }
            if (action.index >= cards.size()) return;
            auto& card = cards[action.index];
            if (action.kind >= SettingsActionKind::SystemAppearance
                && action.kind <= SettingsActionKind::TransparentAppearance) {
                auto preferences = AppearancePreferences(card);
                const std::array presets{
                    std::pair{"system", 0.90},
                    std::pair{"mica-dark", 0.92},
                    std::pair{"mica-white", 0.88},
                    std::pair{"brand", 0.97},
                std::pair{
                    "transparent-white",
                    ResolveCrystalMaterialStyle().surfaceOpacity},
                };
                const auto index = static_cast<std::size_t>(
                    static_cast<int>(action.kind)
                        - static_cast<int>(SettingsActionKind::SystemAppearance));
                if (index >= presets.size()) return;
                preferences.preset = presets[index].first;
                preferences.opacity = presets[index].second;
                if (appearanceChanged && appearanceChanged(card.id, preferences)) {
                    card.appearancePreset = preferences.preset;
                    card.opacity = preferences.opacity;
                }
                return;
            }
            if (action.kind == SettingsActionKind::SmallItems
                || action.kind == SettingsActionKind::MediumItems
                || action.kind == SettingsActionKind::LargeItems
                || action.kind == SettingsActionKind::ExtraLargeItems) {
                auto preferences = card.content;
                preferences.itemSize = action.kind == SettingsActionKind::SmallItems
                    ? domain::CardItemSize::Small
                    : action.kind == SettingsActionKind::MediumItems
                    ? domain::CardItemSize::Medium
                    : action.kind == SettingsActionKind::LargeItems
                    ? domain::CardItemSize::Large : domain::CardItemSize::ExtraLarge;
                // Density is independent from the card's configured width.
                // Do not resize or re-project the card when changing icon size.
                preferences.fixedColumns = static_cast<std::uint32_t>(
                    domain::ProjectCardColumns(preferences.widthSpan, preferences.itemSize));
                if (contentChanged && contentChanged(card.id, preferences)) card.content = preferences;
                return;
            }
            if (action.kind == SettingsActionKind::SmallCardWidth
                || action.kind == SettingsActionKind::MediumCardWidth
                || action.kind == SettingsActionKind::LargeCardWidth) {
                if (card.type != domain::CardType::Todo) return;
                auto preferences = card.content;
                preferences.widthSpan = action.kind == SettingsActionKind::SmallCardWidth
                    ? 4u : action.kind == SettingsActionKind::MediumCardWidth ? 5u : 6u;
                preferences.fixedColumns = static_cast<std::uint32_t>(
                    domain::ProjectCardColumns(preferences.widthSpan, preferences.itemSize));
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
            if (action.kind == SettingsActionKind::TogglePresentationControl) {
                if (card.type != domain::CardType::Application
                    && card.type != domain::CardType::Mapping) return;
                auto preferences = ChromePreferences(card);
                preferences.showPresentationControl =
                    !preferences.showPresentationControl;
                if (chromeChanged && chromeChanged(card.id, preferences)) {
                    card.showPresentationControl = preferences.showPresentationControl;
                }
                return;
            }
            if (action.kind == SettingsActionKind::ToggleApplicationSortMenu) {
                if (card.type != domain::CardType::Application
                    && card.type != domain::CardType::Mapping) return;
                applicationSortMenuOpen = !applicationSortMenuOpen;
                return;
            }
            if (action.kind == SettingsActionKind::SelectMappingReferences
                || action.kind == SettingsActionKind::SelectMappingFolder) {
                if (card.type != domain::CardType::Mapping) return;
                const auto mode = action.kind == SettingsActionKind::SelectMappingFolder
                    ? domain::MappingMode::Folder : domain::MappingMode::References;
                if (card.mappingMode == mode) return;
                if (card.mappingHasSource) {
                    pendingDeletion = action;
                    updateArchiveSearchEditor();
                    return;
                }
                if (mappingModeChanged && mappingModeChanged(card.id, mode)) {
                    card.mappingMode = mode;
                    card.mappingHasSource = false;
                    card.items.clear();
                }
                return;
            }
            if (action.kind == SettingsActionKind::DismissApplicationSortMenu) {
                applicationSortMenuOpen = false;
                return;
            }
            if (action.kind == SettingsActionKind::SelectApplicationSort) {
                if (card.type != domain::CardType::Application
                    && card.type != domain::CardType::Mapping) return;
                constexpr std::array modes{
                    domain::ApplicationItemSortMode::Custom,
                    domain::ApplicationItemSortMode::Name,
                    domain::ApplicationItemSortMode::Size,
                    domain::ApplicationItemSortMode::ItemType,
                    domain::ApplicationItemSortMode::ModifiedDate,
                };
                if (action.secondaryIndex >= modes.size()) return;
                const auto mode = modes[action.secondaryIndex];
                const auto changed = card.type == domain::CardType::Mapping
                    ? (mappingSortChanged && mappingSortChanged(card.id, mode))
                    : (applicationSortChanged && applicationSortChanged(card.id, mode));
                if (changed) {
                    card.applicationSortMode = mode;
                    if (card.type == domain::CardType::Mapping) {
                        card.mappingSortMode = mode;
                    }
                }
                applicationSortMenuOpen = false;
                cardMenuOpen = false;
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
            if (action.kind == SettingsActionKind::TogglePinControl) {
                auto preferences = ChromePreferences(card);
                preferences.showPinControl = !preferences.showPinControl;
                if (!preferences.showPinControl) preferences.pinOnTop = false;
                if (chromeChanged && chromeChanged(card.id, preferences)) {
                    card.showPinControl = preferences.showPinControl;
                    card.pinOnTop = preferences.pinOnTop;
                }
                return;
            }
            if (action.kind == SettingsActionKind::TogglePositionLock) {
                auto preferences = ChromePreferences(card);
                preferences.positionLocked = !preferences.positionLocked;
                if (chromeChanged && chromeChanged(card.id, preferences)) {
                    card.positionLocked = preferences.positionLocked;
                }
                return;
            }
            if (action.kind == SettingsActionKind::ToggleSizeMode) {
                auto preferences = card.content;
                preferences = preferences.sizeMode == domain::CardSizeMode::Adaptive
                    ? fixedPreferencesFor(card) : preferences;
                if (preferences.sizeMode == domain::CardSizeMode::Fixed
                    && card.content.sizeMode == domain::CardSizeMode::Fixed) {
                    preferences.sizeMode = domain::CardSizeMode::Adaptive;
                }
                if (contentChanged && contentChanged(card.id, preferences)) {
                    card.content = preferences;
                }
                return;
            }
            if (action.kind == SettingsActionKind::DecreaseFixedColumns
                || action.kind == SettingsActionKind::IncreaseFixedColumns
                || action.kind == SettingsActionKind::DecreaseFixedRows
                || action.kind == SettingsActionKind::IncreaseFixedRows) {
                auto preferences = card.content;
                if (preferences.sizeMode != domain::CardSizeMode::Fixed) return;
                if (action.kind == SettingsActionKind::DecreaseFixedColumns) {
                    if (preferences.widthSpan <= domain::MinimumCardWidthSpan(
                            preferences.itemSize)) return;
                    --preferences.widthSpan;
                } else if (action.kind == SettingsActionKind::IncreaseFixedColumns) {
                    if (preferences.widthSpan >= 64) return;
                    ++preferences.widthSpan;
                } else if (action.kind == SettingsActionKind::DecreaseFixedRows) {
                    if (preferences.fixedRows <= 1) return;
                    --preferences.fixedRows;
                } else {
                    if (preferences.fixedRows >= 64) return;
                    ++preferences.fixedRows;
                }
                preferences.fixedColumns = static_cast<std::uint32_t>(
                    domain::ProjectCardColumns(preferences.widthSpan, preferences.itemSize));
                if (!fixedGridFits(
                        card, preferences.widthSpan, preferences.fixedRows)) return;
                if (contentChanged && contentChanged(card.id, preferences)) {
                    card.content = preferences;
                }
                return;
            }
            if (action.kind == SettingsActionKind::ToggleHeightLimit
                || action.kind == SettingsActionKind::DecreaseMaximumVisibleRows
                || action.kind == SettingsActionKind::IncreaseMaximumVisibleRows) {
                auto preferences = card.content;
                if (action.kind == SettingsActionKind::ToggleHeightLimit) {
                    preferences.maximumVisibleRows = preferences.maximumVisibleRows.has_value()
                        ? std::optional<std::uint32_t>{}
                        : std::optional<std::uint32_t>{3};
                } else if (action.kind == SettingsActionKind::DecreaseMaximumVisibleRows) {
                    if (!preferences.maximumVisibleRows.has_value()
                        || *preferences.maximumVisibleRows <= 1) return;
                    --*preferences.maximumVisibleRows;
                } else {
                    if (!preferences.maximumVisibleRows.has_value()
                        || *preferences.maximumVisibleRows >= 64) return;
                    ++*preferences.maximumVisibleRows;
                }
                if (contentChanged && contentChanged(card.id, preferences)) {
                    card.content = preferences;
                }
                return;
            }
            if (action.kind == SettingsActionKind::ToggleCreatedTime) {
                auto preferences = card.todoPreferences;
                preferences.showCreatedTime = !preferences.showCreatedTime;
                if (todoPreferencesChanged && todoPreferencesChanged(card.id, preferences)) {
                    card.todoPreferences = preferences;
                }
            }
        } catch (...) {
        }
    }

    void beginRename(std::size_t index) noexcept {
        if (index >= cards.size()) return;
        closeRenameEditor(true);
        renameCardIndex = index;
        auto field = renameFieldRect(index);
        OffsetRect(&field, 0, -cardEditorOffset(cards[index]));
        const auto title = DefaultCardTitle(cards[index], usesEnglish());
        renameEdit = CreateWindowsTextInput({
            .notificationWindow = window,
            .controlId = kRenameEditId,
            .bounds = RECT{field.left, field.top + 4, field.right, field.top + 34},
            .maximumLength = 512,
            .text = title,
            .style = renameInputStyle(),
        });
        if (renameEdit == nullptr) {
            renameCardIndex.reset();
            return;
        }
        SetWindowsTextInputSelection(renameEdit, title.size(), title.size());
        FocusWindowsTextInput(renameEdit);
    }

    void closeRenameEditor(bool commit) noexcept {
        if (closingRename || renameEdit == nullptr) return;
        closingRename = true;
        const auto edit = renameEdit;
        const auto index = renameCardIndex;
        renameEdit = nullptr;
        renameCardIndex.reset();
        if (commit && index.has_value() && *index < cards.size()) {
            auto text = WindowsTextInputText(edit);
            if (!text.empty() && cardRenamed && cardRenamed(cards[*index].id, text)) {
                cards[*index].title = std::move(text);
            }
        }
        DestroyWindow(edit);
        closingRename = false;
        if (window != nullptr) InvalidateRect(window, nullptr, FALSE);
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
        const auto background = CreateSolidBrush(ThemeSurfaceColor(
            RGB(18, 19, 21), RGB(243, 243, 243)));
        FillRect(canvas, &client, background);
        DeleteObject(background);

        paintSidebar(canvas, client);
        switch (page) {
        case SettingsPage::System: paintSystem(canvas); break;
        case SettingsPage::Features: paintFeatures(canvas); break;
        case SettingsPage::Cards: paintCards(canvas); break;
        case SettingsPage::Archive: paintArchive(canvas); break;
        case SettingsPage::About: paintAbout(canvas); break;
        }
        if (pendingDeletion.has_value()) paintDeletionConfirmation(canvas);
        if (pendingStorageRoot.has_value()) paintStorageRootConfirmation(canvas);

        if (canvas == memory) BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
        if (previousBitmap != nullptr) SelectObject(memory, previousBitmap);
        if (bitmap != nullptr) DeleteObject(bitmap);
        if (memory != nullptr) DeleteDC(memory);
        EndPaint(window, &paint);
    }

    void paintSidebar(HDC dc, const RECT& client) noexcept {
        RECT sidebar{0, 0, kSidebarWidth, client.bottom};
        const auto brush = CreateSolidBrush(ThemeSurfaceColor(
            RGB(25, 26, 29), RGB(255, 255, 255)));
        FillRect(dc, &sidebar, brush);
        DeleteObject(brush);
        const std::array<std::wstring, 5> labels{
            tr(L"系统", L"System"), tr(L"功能", L"Features"),
            tr(L"卡片", L"Cards"), tr(L"归档", L"Archive"),
            tr(L"关于", L"About")};
        const std::array glyphs{
            L"\uE713", L"\uE713", L"\uE8B7", L"\uE7B8", L"\uE946"};
        for (std::size_t index = 0; index < labels.size(); ++index) {
            const auto action = SettingsAction{SettingsActionKind::Navigate, index};
            auto row = navigationRect(index);
            const auto selected = static_cast<std::size_t>(page) == index;
            if (selected || hovered == action || pressed == action) {
                FillRounded(dc, row,
                    pressed == action ? kAccentPressed
                        : selected ? kAccent : RGB(42, 43, 48), 7);
            }
            const auto color = selected ? RGB(255, 255, 255) : RGB(218, 220, 225);
            if (selected) {
                DrawGlyphRaw(dc, glyphs[index],
                    Rect(row.left + 8, row.top, row.left + 34, row.bottom), color, 16);
                DrawLabelRaw(dc, labels[index],
                    Rect(row.left + 40, row.top, row.right - 8, row.bottom), color, 12,
                    FW_SEMIBOLD);
            } else {
                DrawGlyph(dc, glyphs[index],
                    Rect(row.left + 8, row.top, row.left + 34, row.bottom), color, 15);
                DrawLabel(dc, labels[index],
                    Rect(row.left + 40, row.top, row.right - 8, row.bottom), color, 12);
            }
        }
    }

    void paintPageTitle(HDC dc, std::wstring_view titleText, std::wstring_view subtitle) noexcept {
        DrawLabel(dc, titleText, Rect(kContentLeft, 18, clientRight() - 24, 50),
            RGB(246, 247, 249), 20, FW_SEMIBOLD);
        if (!subtitle.empty()) {
            DrawLabel(dc, subtitle, Rect(kContentLeft, 48, clientRight() - 24, 70),
                RGB(150, 153, 161), 11);
        }
    }

    void paintSection(HDC dc, RECT rect) noexcept {
        if (gSettingsDarkMode) {
            FillRoundedRaw(dc, rect, RGB(38, 39, 43), 8);
        } else {
            FillRoundedRaw(dc, rect, RGB(255, 255, 255), 8);
        }
    }

    void paintSystem(HDC dc) noexcept {
        paintPageTitle(dc, tr(L"系统", L"System"),
            tr(L"Desto 的全局行为与视觉偏好", L"Global behavior and appearance"));
        const auto section = Rect(kContentLeft, 92, clientRight() - 26, 452);
        paintSection(dc, section);
        const auto paintValue = [&](RECT rect, const SettingsAction& action,
                                    std::wstring_view value, bool open) {
            FillRounded(dc, rect,
                pressed == action ? RGB(43, 70, 108)
                    : open ? RGB(37, 42, 49)
                    : hovered == action ? RGB(49, 52, 58) : RGB(32, 34, 38), 7);
            DrawRoundedOutline(dc, rect,
                open ? kAccent
                    : hovered == action ? RGB(83, 101, 128) : RGB(62, 65, 71), 7);
            DrawLabel(dc, value, Rect(rect.left + 10, rect.top, rect.right - 26, rect.bottom),
                RGB(225, 227, 231), 12, FW_NORMAL, DT_LEFT | DT_VCENTER);
            DrawGlyph(dc, open ? L"\uE70E" : L"\uE70D",
                Rect(rect.right - 25, rect.top, rect.right - 7, rect.bottom),
                RGB(157, 161, 169), 9);
        };
        DrawLabel(dc, tr(L"开机启动", L"Run at startup"),
            Rect(section.left + 18, 100, section.right - 66, 136),
            RGB(234, 235, 238), 12, FW_NORMAL);
        const auto startupAction = SettingsAction{SettingsActionKind::ToggleRunAtStartup};
        const auto startupRect = runAtStartupRect();
        const auto startupToggle = Rect(startupRect.right - 40, startupRect.top + 3,
            startupRect.right, startupRect.bottom - 5);
        FillRounded(dc, startupToggle,
            pressed == startupAction ? kAccentPressed
                : runAtStartup ? kAccent
                : hovered == startupAction ? RGB(64, 67, 74) : RGB(55, 57, 63), 10);
        DrawRoundedOutline(dc, startupToggle,
            runAtStartup ? kAccentOutline : RGB(88, 91, 99), 10);
        const auto startupKnob = runAtStartup
            ? Rect(startupToggle.right - 17, startupToggle.top + 3, startupToggle.right - 3, startupToggle.bottom - 3)
            : Rect(startupToggle.left + 3, startupToggle.top + 3, startupToggle.left + 17, startupToggle.bottom - 3);
        FillRoundedRaw(dc, startupKnob, RGB(241, 243, 246), 7);

        DrawLabel(dc, tr(L"时区", L"Time zone"),
            Rect(section.left + 18, 154, section.right - 230, 194),
            RGB(234, 235, 238), 12, FW_NORMAL);
        paintValue(timeZoneRect(), {SettingsActionKind::SelectTimeZone}, timeZoneText(),
            activeSystemDropdown == SystemDropdown::TimeZone);
        DrawLabel(dc, tr(L"语言", L"Language"),
            Rect(section.left + 18, 208, section.right - 230, 248),
            RGB(234, 235, 238), 12, FW_NORMAL);
        paintValue(languageRect(), {SettingsActionKind::SelectLanguage}, languageText(),
            activeSystemDropdown == SystemDropdown::Language);
        DrawLabel(dc, tr(L"数据存储位置", L"Data location"),
            Rect(section.left + 18, 262, section.right - 142, 286),
            RGB(234, 235, 238), 12, FW_NORMAL);
        DrawLabel(dc, storageRoot.wstring(),
            Rect(section.left + 18, 286, section.right - 142, 310),
            RGB(143, 147, 155), 10);
        const auto storageAction = SettingsAction{SettingsActionKind::ChangeStorageRoot};
        const auto storageButton = storageRootRect();
        FillRounded(dc, storageButton,
            pressed == storageAction ? RGB(43, 70, 108)
                : hovered == storageAction ? RGB(49, 52, 58) : RGB(32, 34, 38), 7);
        DrawRoundedOutline(dc, storageButton,
            hovered == storageAction ? RGB(83, 101, 128) : RGB(62, 65, 71), 7);
        DrawLabel(dc, tr(L"更改", L"Change"), storageButton,
            RGB(225, 227, 231), 11, FW_SEMIBOLD, DT_CENTER | DT_VCENTER);

        DrawLabel(dc, tr(L"卡片圆角", L"Card corners"),
            Rect(section.left + 18, 342, section.right - 18, 372),
            RGB(234, 235, 238), 12, FW_NORMAL);
        const std::array<std::wstring, 4> labels{
            tr(L"直角", L"Square"), L"Windows", L"macOS",
            tr(L"饱满圆角", L"Full")};
        constexpr std::array<double, 4> radii{0.0, 12.0, 24.0, 32.0};
        for (std::size_t index = 0; index < radii.size(); ++index) {
            const auto action = SettingsAction{SettingsActionKind::GlobalCornerRadius, index};
            const auto selected = std::abs(globalCornerRadius - radii[index]) < 0.5;
            const auto rect = radiusOptionRect(index);
            FillRounded(dc, rect,
                pressed == action ? kAccentPressed
                    : selected ? kAccent
                    : hovered == action ? RGB(49, 52, 58) : RGB(32, 34, 38), 7);
            DrawRoundedOutline(dc, rect,
                selected ? kAccentOutline
                    : hovered == action ? RGB(83, 101, 128) : RGB(62, 65, 71), 7);
            if (selected) {
                DrawLabelRaw(dc, labels[index], rect, RGB(255, 255, 255), 11,
                    FW_SEMIBOLD, DT_CENTER | DT_VCENTER);
            } else {
                DrawLabel(dc, labels[index], rect, RGB(232, 234, 238), 11,
                    FW_NORMAL, DT_CENTER | DT_VCENTER);
            }
        }
        paintSystemDropdown(dc);
    }

    void paintSystemDropdown(HDC dc) noexcept {
        if (activeSystemDropdown == SystemDropdown::None) return;
        const auto panel = systemDropdownPanelRect();
        FillRounded(dc, panel, RGB(31, 32, 36), 8);
        DrawRoundedOutline(dc, panel, RGB(67, 70, 77), 8);

        const std::array<std::wstring, 4> timeZoneLabels{
            tr(L"跟随系统", L"System"), L"UTC+00:00", L"UTC+08:00", L"UTC-05:00"};
        const std::array<std::wstring, 3> languageLabels{
            tr(L"跟随系统", L"System"), L"简体中文", L"English"};
        const std::array<std::wstring, 4> desktopLabels{
            tr(L"不处理", L"Do nothing"), tr(L"隐藏桌面图标", L"Desktop icons"),
            tr(L"隐藏桌面卡片", L"Desktop cards"), tr(L"同时隐藏", L"Icons and cards")};
        const std::array<std::wstring, 3> taskbarLabels{
            tr(L"不处理", L"Do nothing"), tr(L"全部屏幕", L"All displays"),
            tr(L"当前屏幕", L"Current display")};
        const auto selectedTimeZone = [&]() -> std::size_t {
            if (!timeZoneOffsetMinutes.has_value()) return 0;
            if (*timeZoneOffsetMinutes == 0) return 1;
            if (*timeZoneOffsetMinutes == 8 * 60) return 2;
            return 3;
        }();
        const auto selectedLanguage = language == "zh-CN" ? std::size_t{1}
            : language == "en-US" ? std::size_t{2} : std::size_t{0};
        const auto selectedDesktop = desktopDoubleClickAction == "icons" ? std::size_t{1}
            : desktopDoubleClickAction == "cards" ? std::size_t{2}
            : desktopDoubleClickAction == "all" ? std::size_t{3} : std::size_t{0};
        for (std::size_t index = 0; index < systemDropdownOptionCount(); ++index) {
            const auto kind = activeSystemDropdown == SystemDropdown::TimeZone
                ? SettingsActionKind::SelectTimeZoneOption
                : activeSystemDropdown == SystemDropdown::Language
                ? SettingsActionKind::SelectLanguageOption
                : SettingsActionKind::SelectDesktopDoubleClickOption;
            const auto action = SettingsAction{kind, index};
            const auto selected = activeSystemDropdown == SystemDropdown::TimeZone
                ? index == selectedTimeZone
                : activeSystemDropdown == SystemDropdown::Language
                ? index == selectedLanguage
                : activeSystemDropdown == SystemDropdown::DesktopDoubleClick
                ? index == selectedDesktop : false;
            const auto row = systemDropdownOptionRect(index);
            if (selected || hovered == action || pressed == action) {
                FillRounded(dc, row,
                    pressed == action ? kAccentPressed
                        : selected ? kAccent : RGB(46, 48, 53), 6);
            }
            const auto label = activeSystemDropdown == SystemDropdown::TimeZone
                ? timeZoneLabels[index]
                : activeSystemDropdown == SystemDropdown::Language
                ? languageLabels[index]
                : activeSystemDropdown == SystemDropdown::DesktopDoubleClick
                ? desktopLabels[index] : taskbarLabels[index];
            if (selected) {
                DrawLabelRaw(dc, label,
                    Rect(row.left + 10, row.top, row.right - 10, row.bottom),
                    RGB(255, 255, 255), 11, FW_SEMIBOLD, DT_LEFT | DT_VCENTER);
            } else {
                DrawLabel(dc, label,
                    Rect(row.left + 10, row.top, row.right - 10, row.bottom),
                    RGB(241, 242, 245), 11, FW_NORMAL, DT_LEFT | DT_VCENTER);
            }
        }
    }

    void paintFeatures(HDC dc) noexcept {
        paintPageTitle(dc, tr(L"功能", L"Features"),
            tr(L"桌面交互与窗口层级策略", L"Desktop gestures and window behavior"));
        const auto section = Rect(kContentLeft, 92, clientRight() - 26, 382);
        paintSection(dc, section);
        const auto paintValue = [&](RECT rect, const SettingsAction& action,
                                    std::wstring_view value, bool open) {
            FillRounded(dc, rect,
                pressed == action ? kAccentPressed
                    : open ? RGB(37, 42, 49)
                    : hovered == action ? RGB(49, 52, 58) : RGB(32, 34, 38), 7);
            DrawRoundedOutline(dc, rect,
                open ? kAccent
                    : hovered == action ? RGB(83, 101, 128) : RGB(62, 65, 71), 7);
            DrawLabel(dc, value, Rect(rect.left + 10, rect.top, rect.right - 26, rect.bottom),
                RGB(225, 227, 231), 11, FW_NORMAL, DT_LEFT | DT_VCENTER);
            DrawGlyph(dc, open ? L"\uE70E" : L"\uE70D",
                Rect(rect.right - 25, rect.top, rect.right - 7, rect.bottom),
                RGB(157, 161, 169), 9);
        };
        DrawLabel(dc, tr(L"双击桌面", L"Double-click desktop"),
            Rect(section.left + 18, 104, desktopDoubleClickRect().left - 12, 140),
            RGB(234, 235, 238), 11, FW_NORMAL, DT_LEFT | DT_VCENTER);
        paintValue(desktopDoubleClickRect(),
            {SettingsActionKind::SelectDesktopDoubleClick}, desktopDoubleClickText(),
            activeSystemDropdown == SystemDropdown::DesktopDoubleClick);
        const auto taskbarAction = SettingsAction{SettingsActionKind::ToggleTaskbarDesktop};
        const auto taskbarRow = taskbarDoubleClickRowRect();
        const auto taskbarToggle = Rect(
            taskbarDoubleClickRect().right - 36,
            taskbarDoubleClickRect().top + 4,
            taskbarDoubleClickRect().right,
            taskbarDoubleClickRect().bottom - 4);
        DrawLabel(dc, tr(L"双击任务栏回到桌面", L"Double-click taskbar to show desktop"),
            Rect(taskbarRow.left, taskbarRow.top - 4, taskbarToggle.left - 12, taskbarRow.bottom + 4),
            RGB(234, 235, 238), 11, FW_NORMAL, DT_LEFT | DT_VCENTER);
        const auto taskbarEnabled = taskbarDoubleClickAction != "none";
        FillRounded(dc, taskbarToggle,
            pressed == taskbarAction ? kAccentPressed
                : taskbarEnabled ? kAccent
                : hovered == taskbarAction ? RGB(64, 67, 74) : RGB(55, 57, 63), 10);
        DrawRoundedOutline(dc, taskbarToggle,
            taskbarEnabled ? kAccentOutline : RGB(88, 91, 99), 10);
        const auto taskbarKnob = taskbarEnabled
            ? Rect(taskbarToggle.right - 14, taskbarToggle.top + 3,
                taskbarToggle.right - 3, taskbarToggle.bottom - 3)
            : Rect(taskbarToggle.left + 3, taskbarToggle.top + 3,
                taskbarToggle.left + 14, taskbarToggle.bottom - 3);
        FillRoundedRaw(dc, taskbarKnob, RGB(241, 243, 246), 6);

        const auto action = SettingsAction{SettingsActionKind::TogglePinnedCardsYieldToFullscreen};
        const auto row = pinnedFullscreenRect();
        DrawLabel(dc, tr(L"全屏应用时隐藏置顶卡片", L"Hide pinned cards over fullscreen apps"),
            Rect(row.left, row.top - 4, row.right - 58, row.bottom + 4),
            RGB(234, 235, 238), 11, FW_NORMAL, DT_LEFT | DT_VCENTER);
        const auto toggle = Rect(row.right - 36, row.top + 4, row.right, row.bottom - 4);
        FillRounded(dc, toggle,
            pressed == action ? kAccentPressed
                : pinnedCardsYieldToFullscreen ? kAccent
                : hovered == action ? RGB(64, 67, 74) : RGB(55, 57, 63), 10);
        DrawRoundedOutline(dc, toggle,
            pinnedCardsYieldToFullscreen ? kAccentOutline : RGB(88, 91, 99), 10);
        const auto knob = pinnedCardsYieldToFullscreen
            ? Rect(toggle.right - 14, toggle.top + 3, toggle.right - 3, toggle.bottom - 3)
            : Rect(toggle.left + 3, toggle.top + 3, toggle.left + 14, toggle.bottom - 3);
        FillRoundedRaw(dc, knob, RGB(241, 243, 246), 6);
        const auto iconAction = SettingsAction{SettingsActionKind::ToggleIconBackgroundFrame};
        const auto iconRow = iconBackgroundFrameRect();
        DrawLabel(dc, tr(L"图标背景圆角框", L"Rounded icon background"),
            Rect(iconRow.left, iconRow.top - 4, iconRow.right - 58, iconRow.bottom + 4),
            RGB(234, 235, 238), 11, FW_NORMAL, DT_LEFT | DT_VCENTER);
        const auto iconToggle = Rect(iconRow.right - 36, iconRow.top + 4,
            iconRow.right, iconRow.bottom - 4);
        FillRounded(dc, iconToggle,
            pressed == iconAction ? kAccentPressed
                : showIconBackgroundFrame ? kAccent
                : hovered == iconAction ? RGB(64, 67, 74) : RGB(55, 57, 63), 10);
        DrawRoundedOutline(dc, iconToggle,
            showIconBackgroundFrame ? kAccentOutline : RGB(88, 91, 99), 10);
        const auto iconKnob = showIconBackgroundFrame
            ? Rect(iconToggle.right - 14, iconToggle.top + 3,
                iconToggle.right - 3, iconToggle.bottom - 3)
            : Rect(iconToggle.left + 3, iconToggle.top + 3,
                iconToggle.left + 14, iconToggle.bottom - 3);
        FillRoundedRaw(dc, iconKnob, RGB(241, 243, 246), 6);
        const auto deletionAction = SettingsAction{
            SettingsActionKind::ToggleFileDeletionConfirmation};
        const auto deletionRow = fileDeletionConfirmationRect();
        DrawLabel(dc, tr(L"删除文件前确认", L"Confirm before deleting files"),
            Rect(deletionRow.left, deletionRow.top - 4, deletionRow.right - 58,
                deletionRow.bottom + 4), RGB(234, 235, 238), 11, FW_NORMAL,
            DT_LEFT | DT_VCENTER);
        const auto deletionToggle = Rect(
            deletionRow.right - 36, deletionRow.top + 4,
            deletionRow.right, deletionRow.bottom - 4);
        FillRounded(dc, deletionToggle,
            pressed == deletionAction ? kAccentPressed
                : confirmFileDeletion ? kAccent
                : hovered == deletionAction ? RGB(64, 67, 74) : RGB(55, 57, 63), 10);
        DrawRoundedOutline(dc, deletionToggle,
            confirmFileDeletion ? kAccentOutline : RGB(88, 91, 99), 10);
        const auto deletionKnob = confirmFileDeletion
            ? Rect(deletionToggle.right - 14, deletionToggle.top + 3,
                deletionToggle.right - 3, deletionToggle.bottom - 3)
            : Rect(deletionToggle.left + 3, deletionToggle.top + 3,
                deletionToggle.left + 14, deletionToggle.bottom - 3);
        FillRoundedRaw(dc, deletionKnob, RGB(241, 243, 246), 6);
        paintSystemDropdown(dc);
    }

    void paintCards(HDC dc) noexcept {
        paintPageTitle(dc, tr(L"卡片", L"Cards"),
            tr(L"添加、重命名或调整每张卡片", L"Add, rename, and customize each card"));
        const auto addAction = SettingsAction{SettingsActionKind::AddCard};
        const auto add = addCardRect();
        FillRounded(dc, add,
            pressed == addAction ? kAccentPressed
                : hovered == addAction ? kAccentHover : kAccent, 7);
        DrawGlyphRaw(dc, L"\uE710", Rect(add.left + 8, add.top, add.left + 30, add.bottom), RGB(255, 255, 255), 14);
        DrawLabelRaw(dc, tr(L"添加", L"Add"), Rect(add.left + 32, add.top, add.right - 8, add.bottom), RGB(255, 255, 255), 12, FW_SEMIBOLD);

        if (cards.empty()) {
            const auto empty = Rect(kContentLeft, 94, clientRight() - 26, 214);
            paintSection(dc, empty);
            DrawLabel(dc, tr(L"还没有卡片", L"No cards yet"), Rect(empty.left + 18, 116, empty.right - 18, 148), RGB(235, 236, 239), 14, FW_SEMIBOLD);
            DrawLabel(dc, tr(L"使用右上角的添加按钮创建第一张卡片。",
                L"Use the Add button to create your first card."),
                Rect(empty.left + 18, 148, empty.right - 18, 178), RGB(148, 151, 159), 12);
        }
        for (std::size_t index = 0; index < cards.size(); ++index) paintCardRow(dc, index);
        if (cardDragActive && cardDragTarget.has_value()
            && *cardDragTarget < cards.size()) {
            auto target = cardRow(*cardDragTarget);
            InflateRect(&target, 1, 1);
            DrawRoundedOutline(dc, target, kAccent, 8, 2);
        }
        if (!cards.empty() && selectedCard < cards.size()) {
            const auto saved = SaveDC(dc);
            const auto viewport = cardEditorViewportRect();
            IntersectClipRect(dc, viewport.left, viewport.top, viewport.right, viewport.bottom);
            SetViewportOrgEx(dc, 0, -cardEditorOffset(cards[selectedCard]), nullptr);
            paintSelectedCard(dc);
            if (cardMenuOpen) paintCardMenu(dc);
            RestoreDC(dc, saved);
            paintCardEditorScrollbar(dc);
            paintOptionTooltip(dc);
        }
        paintCardRailTooltip(dc);
        if (addMenuOpen) paintAddMenu(dc);
    }

    void paintCardRow(HDC dc, std::size_t index) noexcept {
        const auto row = cardRow(index);
        const auto select = SettingsAction{SettingsActionKind::SelectCard, index};
        const auto selected = index == selectedCard;
        FillRounded(dc, row,
            pressed == select ? RGB(46, 48, 53)
                : selected ? RGB(43, 51, 63)
                : hovered == select ? RGB(41, 42, 47) : RGB(34, 35, 39), 7);
        DrawGlyph(dc, CardTypeGlyph(cards[index].type),
            row, selected ? RGB(118, 174, 255) : RGB(188, 191, 198), 17);
        const auto visibilityAction = SettingsAction{
            SettingsActionKind::ToggleCardVisibility, index};
        const auto visibility = cardVisibilityRect(index);
        if (hovered == visibilityAction || pressed == visibilityAction) {
            FillRounded(dc, visibility,
                pressed == visibilityAction ? RGB(48, 67, 94) : RGB(44, 48, 55), 5);
        }
        DrawGlyph(dc, cards[index].visible ? L"\uE890" : L"\uED1A", visibility,
            cards[index].visible ? RGB(136, 184, 255) : RGB(128, 132, 141), 8);
    }

    void paintCardRailTooltip(HDC dc) noexcept {
        if ((hovered.kind != SettingsActionKind::SelectCard
                && hovered.kind != SettingsActionKind::ToggleCardVisibility)
            || hovered.index >= cards.size()) return;
        const auto panel = cardTooltipRect(hovered.index);
        auto shadow = panel;
        OffsetRect(&shadow, 0, 3);
        FillRounded(dc, shadow, RGB(12, 13, 15), 9);
        FillRounded(dc, panel, RGB(44, 46, 51), 9);
        DrawRoundedOutline(dc, panel, RGB(72, 75, 82), 9);
        DrawLabel(dc, DefaultCardTitle(cards[hovered.index], usesEnglish()),
            Rect(panel.left + 14, panel.top + 8, panel.right - 12, panel.top + 31),
            RGB(244, 245, 247), 12, FW_SEMIBOLD);
        DrawLabel(dc, CardTypeName(cards[hovered.index].type, usesEnglish()),
            Rect(panel.left + 14, panel.top + 31, panel.right - 12, panel.bottom - 5),
            RGB(158, 162, 170), 10);
    }

    void paintCardEditorScrollbar(HDC dc) noexcept {
        if (cards.empty() || selectedCard >= cards.size()) return;
        const auto& card = cards[selectedCard];
        const auto maximum = maximumCardEditorOffset(card);
        if (maximum <= 0) return;
        const auto viewport = cardEditorViewportRect();
        const auto track = Rect(viewport.right - 4, viewport.top + 8,
            viewport.right - 1, viewport.bottom - 8);
        FillRounded(dc, track, RGB(48, 50, 56), 2);
        const auto viewportHeight = viewport.bottom - viewport.top;
        const auto contentHeight = viewportHeight + maximum;
        const auto thumbHeight = std::max<LONG>(28,
            (track.bottom - track.top) * viewportHeight
                / std::max<LONG>(1, contentHeight));
        const auto offset = cardEditorOffset(card);
        const auto thumbTop = track.top + (track.bottom - track.top - thumbHeight)
            * offset / maximum;
        FillRounded(dc, Rect(track.left, thumbTop, track.right, thumbTop + thumbHeight),
            RGB(126, 131, 141), 2);
    }

    void paintOptionTooltip(HDC dc) noexcept {
        if (cards.empty() || selectedCard >= cards.size()) return;
        RECT anchor{};
        std::wstring label;
        switch (hovered.kind) {
        case SettingsActionKind::TogglePresentationControl:
            anchor = filePresentationRect();
            label = tr(L"显示展示切换按钮", L"Show presentation switch");
            break;
        case SettingsActionKind::ToggleItemNames:
            anchor = itemNamesRect();
            label = tr(L"显示文件名", L"Show file names");
            break;
        case SettingsActionKind::ToggleCollapseControl:
            anchor = collapseRect();
            label = tr(L"显示收缩按钮", L"Show collapse button");
            break;
        case SettingsActionKind::TogglePinControl:
            anchor = pinControlSettingRect();
            label = tr(L"显示置顶按钮", L"Show pin button");
            break;
        case SettingsActionKind::TogglePositionLock:
            anchor = positionLockRect();
            label = tr(L"锁定卡片", L"Lock card");
            break;
        case SettingsActionKind::ToggleSizeMode:
            anchor = sizeModeRect();
            label = tr(L"自适应卡片尺寸", L"Adaptive card size");
            break;
        case SettingsActionKind::ToggleHeightLimit:
            anchor = cards[selectedCard].type == domain::CardType::Todo
                ? todoHeightLimitRect() : heightLimitRect();
            label = tr(L"限制卡片高度", L"Limit card height");
            break;
        case SettingsActionKind::ToggleCreatedTime:
            anchor = createdTimeRect();
            label = tr(L"显示创建时间", L"Show creation time");
            break;
        default:
            return;
        }
        OffsetRect(&anchor, 0, -cardEditorOffset(cards[selectedCard]));
        const auto viewport = cardEditorViewportRect();
        if (anchor.bottom <= viewport.top || anchor.top >= viewport.bottom) return;
        constexpr int width = 174;
        constexpr int height = 34;
        auto left = std::clamp(
            (anchor.left + anchor.right - width) / 2,
            viewport.left,
            viewport.right - width);
        auto top = anchor.bottom + 7;
        if (top + height > viewport.bottom) top = anchor.top - height - 7;
        const auto panel = Rect(left, top, left + width, top + height);
        FillRounded(dc, panel, RGB(45, 47, 52), 7);
        DrawRoundedOutline(dc, panel, RGB(77, 80, 87), 7);
        DrawLabel(dc, label, Rect(panel.left + 10, panel.top,
            panel.right - 10, panel.bottom), RGB(239, 240, 243), 10,
            FW_NORMAL, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    void paintAddMenu(HDC dc) noexcept {
        const std::array<std::wstring, 3> labels{
            tr(L"应用卡片", L"Application card"),
            tr(L"映射卡片", L"Mapping card"),
            tr(L"待办卡片", L"Task card")};
        const std::array glyphs{L"\uE8B7", L"\uE71B", L"\uE73E"};
        const std::array kinds{SettingsActionKind::AddApplication, SettingsActionKind::AddMapping, SettingsActionKind::AddTodo};
        const auto outer = addMenuPanelRect();
        FillRounded(dc, outer, RGB(45, 47, 52), 8);
        DrawRoundedOutline(dc, outer, RGB(68, 70, 77), 8);
        DrawLabel(dc, tr(L"内置卡片", L"Built-in cards"),
            Rect(outer.left + 14, outer.top + 8, outer.right - 14, outer.top + 30),
            RGB(151, 156, 166), 10, FW_SEMIBOLD);
        for (std::size_t index = 0; index < labels.size(); ++index) {
            const auto action = SettingsAction{kinds[index]};
            const auto row = addTypeRect(index);
            if (hovered == action || pressed == action) {
                FillRounded(dc, row, pressed == action ? RGB(52, 96, 160) : RGB(57, 59, 65), 6);
            }
            DrawGlyph(dc, glyphs[index], Rect(row.left + 8, row.top, row.left + 34, row.bottom), RGB(218, 220, 225), 15);
            DrawLabel(dc, labels[index], Rect(row.left + 42, row.top, row.right - 8, row.bottom), RGB(237, 238, 241), 12);
        }
    }

    void paintSelectedCard(HDC dc) noexcept {
        const auto& card = cards[selectedCard];
        const auto left = cardDetailLeft();
        const auto editing = renameCardIndex.has_value() && *renameCardIndex == selectedCard;
        if (!editing) {
            DrawLabel(dc, DefaultCardTitle(card, usesEnglish()),
                Rect(left, 84, renameButtonRect().left - 6, 112), RGB(243, 244, 247), 16, FW_SEMIBOLD,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        const auto renameAction = SettingsAction{SettingsActionKind::RenameCard, selectedCard};
        const auto rename = renameButtonRect();
        if (hovered == renameAction || pressed == renameAction || editing) {
            FillRounded(dc, rename,
                pressed == renameAction ? RGB(48, 67, 94) : RGB(38, 41, 46), 7);
        }
        DrawGlyph(dc, L"\uE70F", rename,
            editing ? RGB(111, 169, 255) : RGB(190, 193, 200), 13);
        DrawLabel(dc, CardTypeName(card.type, usesEnglish()), Rect(left, 112, clientRight() - 26, 136), RGB(143, 146, 154), 11);
        const auto menuAction = SettingsAction{SettingsActionKind::OpenCardMenu, selectedCard};
        const auto menu = cardMenuButtonRect();
        if (hovered == menuAction || pressed == menuAction || cardMenuOpen) {
            FillRounded(dc, menu,
                pressed == menuAction ? RGB(54, 56, 62) : RGB(43, 45, 50), 7);
        }
        DrawGlyph(dc, L"\uE712", menu, RGB(190, 193, 200), 15);
        // Appearance and the type-specific size selector share one row. Both
        // captions derive from the same selector boundary so translations or
        // future width changes cannot make them overlap again.
        const auto fileCard = card.type == domain::CardType::Application
            || card.type == domain::CardType::Mapping;
        const auto sizeCaptionLeft = fileCard
            ? itemSizeRect(0).left : compactCardSizeRect(0).left;
        DrawLabel(dc, tr(L"外观", L"Appearance"),
            Rect(left, 136, sizeCaptionLeft - 12, 160),
            RGB(216, 218, 223), 12, FW_SEMIBOLD,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        const std::array kinds{
            SettingsActionKind::SystemAppearance,
            SettingsActionKind::MicaDarkAppearance,
            SettingsActionKind::MicaWhiteAppearance,
            SettingsActionKind::BrandAppearance,
            SettingsActionKind::TransparentAppearance,
        };
        for (std::size_t index = 0; index < kinds.size(); ++index) {
            const auto action = SettingsAction{kinds[index], selectedCard};
            const auto swatch = appearanceRect(index);
            if (index == 0) {
                FillDiagonalSplitRoundedRaw(dc, swatch,
                    RGB(243, 243, 243), RGB(24, 25, 28), 8);
                DrawRoundedOutlineRaw(dc, swatch, RGB(151, 154, 160), 8);
            } else if (index == 1) {
                FillRoundedRaw(dc, swatch, RGB(32, 33, 36), 8);
                DrawRoundedOutlineRaw(dc, swatch, RGB(86, 89, 96), 8);
            } else if (index == 2) {
                FillRoundedRaw(dc, swatch, RGB(243, 243, 243), 8);
                DrawRoundedOutlineRaw(dc, swatch, RGB(195, 198, 204), 8);
            } else if (index == 3) {
                FillRoundedGradientRaw(
                    dc, swatch, RGB(234, 230, 255), RGB(237, 242, 255), 8);
                DrawRoundedOutlineRaw(dc, swatch, RGB(201, 205, 220), 8);
            } else {
                FillDiagonalSplitRoundedRaw(dc, swatch,
                    RGB(250, 253, 255), RGB(208, 220, 234), 8);
                DrawRoundedOutlineRaw(dc, swatch, RGB(151, 177, 207), 8);
                auto highlight = swatch;
                InflateRect(&highlight, -4, -4);
                DrawRoundedOutlineRaw(dc, highlight, RGB(255, 255, 255), 5);
            }
            const std::array presetNames{
                std::string_view{"system"}, std::string_view{"mica-dark"},
                std::string_view{"mica-white"}, std::string_view{"brand"},
                std::string_view{"transparent-white"},
            };
            const auto active = index < presetNames.size()
                && (card.appearancePreset == presetNames[index]
                    || (index == 1 && (card.appearancePreset == "black" || card.appearancePreset == "dark"))
                    || (index == 2 && (card.appearancePreset == "white" || card.appearancePreset == "default"))
                    || (index == 3 && card.appearancePreset == "jewel"));
            if (active || hovered == action || pressed == action) {
                auto ring = swatch;
                InflateRect(&ring, 3, 3);
                DrawRoundedOutline(dc, ring,
                    pressed == action ? RGB(111, 169, 255) : active ? RGB(76, 145, 244) : RGB(91, 94, 102), 10, active ? 2 : 1);
            }
        }
        if (!fileCard) {
            DrawLabel(dc, tr(L"卡片大小", L"Card size"),
                Rect(compactCardSizeRect(0).left, 136, clientRight() - 26, 160),
                RGB(216, 218, 223), 12, FW_SEMIBOLD,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            const std::array<std::wstring, 3> labels{
                tr(L"小", L"Small"), tr(L"中", L"Medium"), tr(L"大", L"Large")};
            const std::array kinds{
                SettingsActionKind::SmallCardWidth,
                SettingsActionKind::MediumCardWidth,
                SettingsActionKind::LargeCardWidth};
            constexpr std::array<std::uint32_t, 3> spans{4, 5, 6};
            for (std::size_t index = 0; index < labels.size(); ++index) {
                const auto action = SettingsAction{kinds[index], selectedCard};
                const auto active = card.content.widthSpan == spans[index];
                const auto rect = compactCardSizeRect(index);
                FillRounded(dc, rect,
                    pressed == action ? kAccentPressed
                        : active ? kAccent
                        : hovered == action ? RGB(54, 56, 62) : RGB(43, 45, 50), 7);
                DrawRoundedOutline(dc, rect,
                    active ? kAccentOutline
                        : hovered == action ? RGB(75, 78, 85) : RGB(58, 60, 66), 7);
                if (active) {
                    DrawLabelRaw(dc, labels[index], rect, RGB(255, 255, 255), 10,
                        FW_SEMIBOLD, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                } else {
                    DrawLabel(dc, labels[index], rect, RGB(238, 239, 242), 10,
                        FW_NORMAL, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }
            }
        }
        if (card.type == domain::CardType::Application || card.type == domain::CardType::Mapping) {
            paintFileCardSettings(dc, card);
        } else if (card.type == domain::CardType::Todo) {
            paintTodoSettings(dc, card);
        }
    }

    void paintCardMenu(HDC dc) noexcept {
        const auto panel = cardMenuPanelRect();
        FillRounded(dc, panel, RGB(45, 47, 52), 8);
        DrawRoundedOutline(dc, panel, RGB(68, 70, 77), 8);
        const auto fileCard = selectedCard < cards.size()
            && (cards[selectedCard].type == domain::CardType::Application
                || cards[selectedCard].type == domain::CardType::Mapping);
        if (fileCard) {
            const std::array<std::wstring, 5> labels{
                tr(L"自定义排序", L"Custom order"), tr(L"按名称", L"Name"),
                tr(L"按大小", L"Size"), tr(L"按类型", L"Item type"),
                tr(L"按修改日期", L"Modified date")};
            const std::array modes{
                domain::ApplicationItemSortMode::Custom,
                domain::ApplicationItemSortMode::Name,
                domain::ApplicationItemSortMode::Size,
                domain::ApplicationItemSortMode::ItemType,
                domain::ApplicationItemSortMode::ModifiedDate,
            };
            const auto activeIndex = std::ranges::find(modes,
                cards[selectedCard].applicationSortMode) - modes.begin();
            const auto sortAction = SettingsAction{
                SettingsActionKind::ToggleApplicationSortMenu, selectedCard};
            const auto row = cardMenuSortButtonRect();
            if (applicationSortMenuOpen || hovered == sortAction || pressed == sortAction) {
                FillRounded(dc, row,
                    pressed == sortAction ? RGB(48, 83, 132)
                        : applicationSortMenuOpen ? RGB(43, 65, 96) : RGB(57, 59, 65), 6);
            }
            DrawLabel(dc, tr(L"排序", L"Sort"),
                Rect(row.left + 12, row.top, row.right - 36, row.bottom),
                RGB(235, 237, 241), 11);
            DrawLabel(dc, labels[static_cast<std::size_t>(activeIndex)],
                Rect(row.left + 76, row.top, row.right - 28, row.bottom),
                RGB(165, 187, 218), 10, FW_NORMAL, DT_RIGHT | DT_VCENTER);
            DrawGlyph(dc, L"\uE76C", Rect(row.right - 28, row.top, row.right - 8, row.bottom),
                RGB(174, 181, 193), 10);
            if (applicationSortMenuOpen) {
                const auto subPanel = Rect(cardMenuSortRect(0).left - 4,
                    cardMenuSortRect(0).top - 4,
                    cardMenuSortRect(4).right + 4,
                    cardMenuSortRect(4).bottom + 4);
                FillRounded(dc, subPanel, RGB(45, 47, 52), 8);
                DrawRoundedOutline(dc, subPanel, RGB(68, 70, 77), 8);
                for (std::size_t index = 0; index < modes.size(); ++index) {
                    const auto action = SettingsAction{
                        SettingsActionKind::SelectApplicationSort, selectedCard, index};
                    const auto option = cardMenuSortRect(index);
                    const auto active = cards[selectedCard].applicationSortMode == modes[index];
                    if (active || hovered == action || pressed == action) {
                        FillRounded(dc, option,
                            pressed == action ? RGB(48, 83, 132)
                                : active ? RGB(43, 65, 96) : RGB(57, 59, 65), 6);
                    }
                    DrawGlyph(dc, active ? L"\uE73E" : L"",
                        Rect(option.left + 6, option.top, option.left + 32, option.bottom),
                        RGB(116, 174, 255), 10);
                    DrawLabel(dc, labels[index],
                        Rect(option.left + 36, option.top, option.right - 8, option.bottom),
                        active ? RGB(235, 242, 252) : RGB(229, 231, 235), 11);
                }
            }
        }
        const auto deleteAction = SettingsAction{SettingsActionKind::DeleteCard, selectedCard};
        const auto erase = cardMenuDeleteRect();
        if (hovered == deleteAction || pressed == deleteAction) {
            FillRounded(dc, erase, pressed == deleteAction ? RGB(91, 47, 52) : RGB(67, 45, 49), 6);
        }
        DrawGlyph(dc, L"\uE74D", Rect(erase.left + 6, erase.top, erase.left + 34, erase.bottom),
            RGB(236, 145, 151), 13);
        DrawLabel(dc, tr(L"删除", L"Delete"), Rect(erase.left + 38, erase.top, erase.right - 8, erase.bottom),
            RGB(241, 181, 185), 11);
    }

    void paintDeletionConfirmation(HDC dc) noexcept {
        if (!pendingDeletion.has_value()) return;
        RECT client{};
        GetClientRect(window, &client);
        Gdiplus::Graphics graphics(dc);
        Gdiplus::SolidBrush dimmer(Gdiplus::Color(190, 8, 9, 11));
        graphics.FillRectangle(&dimmer,
            static_cast<INT>(client.left), static_cast<INT>(client.top),
            static_cast<INT>(client.right - client.left),
            static_cast<INT>(client.bottom - client.top));
        const auto panel = confirmationPanelRect();
        FillRounded(dc, panel, RGB(38, 40, 45), 10);
        DrawRoundedOutline(dc, panel, RGB(72, 75, 82), 10);
        const auto mappingSwitch = pendingDeletion->kind
                == SettingsActionKind::SelectMappingReferences
            || pendingDeletion->kind == SettingsActionKind::SelectMappingFolder;
        DrawLabel(dc, mappingSwitch
                ? tr(L"切换来源形态", L"Change source mode")
                : tr(L"确认删除", L"Confirm deletion"),
            Rect(panel.left + 22, panel.top + 18, panel.right - 22, panel.top + 52),
            RGB(245, 246, 248), 17, FW_SEMIBOLD);
        std::wstring message;
        if (pendingDeletion->kind == SettingsActionKind::DeleteCard
            && pendingDeletion->index < cards.size()) {
            const auto type = cards[pendingDeletion->index].type;
            message = type == domain::CardType::Application
                ? tr(L"卡片内的文件将移回桌面；同名文件会自动重命名。",
                    L"Files in this card will return to the desktop. Name conflicts are renamed.")
                : type == domain::CardType::Mapping
                ? tr(L"只删除卡片，不会删除映射的源文件。",
                    L"Only the card is removed. Mapped source files are kept.")
                : tr(L"卡片及其中的待办数据将被删除，此操作无法撤销。",
                    L"The card and its tasks will be deleted. This cannot be undone.");
        } else if (mappingSwitch) {
            message = tr(
                L"当前映射关系将被移除，但不会移动或删除任何源文件。确定继续吗？",
                L"Current mappings will be removed, but no source files will be moved or deleted. Continue?");
        } else {
            message = tr(L"这条归档待办将被永久删除，此操作无法撤销。",
                L"This archived task will be permanently deleted. This cannot be undone.");
        }
        DrawMultilineLabel(dc, message,
            Rect(panel.left + 22, panel.top + 66, panel.right - 22, panel.bottom - 72),
            RGB(183, 186, 193), 12);
        const auto cancelAction = SettingsAction{SettingsActionKind::CancelDeletion};
        const auto confirmAction = SettingsAction{SettingsActionKind::ConfirmDeletion};
        const auto cancel = confirmationCancelRect();
        const auto confirm = confirmationConfirmRect();
        FillRounded(dc, cancel,
            pressed == cancelAction ? RGB(56, 58, 64)
                : hovered == cancelAction ? RGB(51, 53, 59) : RGB(45, 47, 52), 7);
        DrawRoundedOutline(dc, cancel, RGB(72, 75, 82), 7);
        FillRounded(dc, confirm,
            mappingSwitch
                ? (pressed == confirmAction ? kAccentPressed
                    : hovered == confirmAction ? kAccentHover : kAccent)
                : (pressed == confirmAction ? RGB(145, 48, 55)
                    : hovered == confirmAction ? RGB(187, 60, 68) : RGB(169, 53, 61)), 7);
        DrawLabel(dc, tr(L"取消", L"Cancel"), cancel, RGB(230, 232, 236), 11,
            FW_SEMIBOLD, DT_CENTER | DT_VCENTER);
        DrawLabelRaw(dc, mappingSwitch ? tr(L"切换", L"Change") : tr(L"删除", L"Delete"),
            confirm, RGB(255, 255, 255), 11,
            FW_SEMIBOLD, DT_CENTER | DT_VCENTER);
    }

    void paintStorageRootConfirmation(HDC dc) noexcept {
        if (!pendingStorageRoot.has_value()) return;
        RECT client{};
        GetClientRect(window, &client);
        Gdiplus::Graphics graphics(dc);
        Gdiplus::SolidBrush dimmer(Gdiplus::Color(190, 8, 9, 11));
        graphics.FillRectangle(&dimmer,
            static_cast<INT>(client.left), static_cast<INT>(client.top),
            static_cast<INT>(client.right - client.left),
            static_cast<INT>(client.bottom - client.top));
        const auto panel = confirmationPanelRect();
        FillRounded(dc, panel, RGB(38, 40, 45), 10);
        DrawRoundedOutline(dc, panel, RGB(72, 75, 82), 10);
        DrawLabel(dc, tr(L"迁移数据", L"Move data"),
            Rect(panel.left + 22, panel.top + 18, panel.right - 22, panel.top + 52),
            RGB(245, 246, 248), 17, FW_SEMIBOLD);
        DrawMultilineLabel(dc,
            tr(L"Desto 将把当前数据迁移到所选文件夹。迁移失败时会自动回滚。\n\n目标：",
                L"Desto will move current data to the selected folder. Changes are rolled back if migration fails.\n\nDestination: ")
                + pendingStorageRoot->wstring(),
            Rect(panel.left + 22, panel.top + 64, panel.right - 22, panel.bottom - 72),
            RGB(183, 186, 193), 12);
        const auto cancelAction = SettingsAction{SettingsActionKind::CancelStorageRootChange};
        const auto confirmAction = SettingsAction{SettingsActionKind::ConfirmStorageRootChange};
        const auto cancel = confirmationCancelRect();
        const auto confirm = confirmationConfirmRect();
        FillRounded(dc, cancel,
            pressed == cancelAction ? RGB(56, 58, 64)
                : hovered == cancelAction ? RGB(51, 53, 59) : RGB(45, 47, 52), 7);
        DrawRoundedOutline(dc, cancel, RGB(72, 75, 82), 7);
        FillRounded(dc, confirm,
            pressed == confirmAction ? kAccentPressed
                : hovered == confirmAction ? kAccentHover : kAccent, 7);
        DrawLabel(dc, tr(L"取消", L"Cancel"), cancel, RGB(230, 232, 236), 11,
            FW_SEMIBOLD, DT_CENTER | DT_VCENTER);
        DrawLabelRaw(dc, tr(L"迁移", L"Move"), confirm, RGB(255, 255, 255), 11,
            FW_SEMIBOLD, DT_CENTER | DT_VCENTER);
    }

    void paintFileCardSettings(HDC dc, const presentation::CardView& card) noexcept {
        const auto left = cardDetailLeft();
        const auto layout = ResolveFileCardSettingsLayout(
            card.type == domain::CardType::Mapping);
        DrawLabel(dc, tr(L"图标大小", L"Icon size"),
            Rect(itemSizeRect(0).left, 136, clientRight() - 26, 160),
            RGB(216, 218, 223), 12, FW_SEMIBOLD);
        const std::array<std::wstring, 4> labels{
            tr(L"小", L"Small"), tr(L"中", L"Medium"),
            tr(L"大", L"Large"), tr(L"特大", L"Extra large")};
        const std::array values{domain::CardItemSize::Small, domain::CardItemSize::Medium, domain::CardItemSize::Large, domain::CardItemSize::ExtraLarge};
        const std::array kinds{SettingsActionKind::SmallItems, SettingsActionKind::MediumItems, SettingsActionKind::LargeItems, SettingsActionKind::ExtraLargeItems};
        for (std::size_t index = 0; index < labels.size(); ++index) {
            const auto action = SettingsAction{kinds[index], selectedCard};
            const auto active = card.content.itemSize == values[index];
            const auto rect = itemSizeRect(index);
            FillRounded(dc, rect,
                pressed == action ? kAccentPressed
                    : active ? kAccent
                    : hovered == action ? RGB(54, 56, 62) : RGB(43, 45, 50), 7);
            DrawRoundedOutline(dc, rect,
                active ? kAccentOutline
                    : hovered == action ? RGB(75, 78, 85) : RGB(58, 60, 66), 7);
            if (active) {
                DrawLabelRaw(dc, labels[index], rect, RGB(255, 255, 255), 11,
                    FW_SEMIBOLD, DT_CENTER | DT_VCENTER);
            } else {
                DrawLabel(dc, labels[index], rect, RGB(238, 239, 242), 11,
                    FW_NORMAL, DT_CENTER | DT_VCENTER);
            }
        }
        if (card.type == domain::CardType::Mapping) {
            DrawLabel(dc, tr(L"来源形态", L"Source mode"),
                Rect(left, layout.sourceLabelTop,
                    clientRight() - 26, layout.sourceTop - 6),
                RGB(216, 218, 223), 12, FW_SEMIBOLD);
            const std::array<std::wstring, 2> labels{
                tr(L"引用集合", L"References"), tr(L"文件夹来源", L"Folder source")};
            const std::array modes{
                domain::MappingMode::References, domain::MappingMode::Folder};
            const std::array kinds{
                SettingsActionKind::SelectMappingReferences,
                SettingsActionKind::SelectMappingFolder};
            for (std::size_t index = 0; index < modes.size(); ++index) {
                const auto action = SettingsAction{kinds[index], selectedCard};
                const auto active = card.mappingMode == modes[index];
                const auto rect = mappingModeRect(index);
                FillRounded(dc, rect,
                    pressed == action ? kAccentPressed
                        : active ? kAccent
                        : hovered == action ? RGB(54, 56, 62) : RGB(43, 45, 50), 7);
                DrawRoundedOutline(dc, rect,
                    active ? kAccentOutline : RGB(61, 64, 70), 7);
                if (active) {
                    DrawLabelRaw(dc, labels[index], rect, RGB(255, 255, 255), 10,
                        FW_SEMIBOLD, DT_CENTER | DT_VCENTER);
                } else {
                    DrawLabel(dc, labels[index], rect, RGB(238, 239, 242), 10,
                        FW_NORMAL, DT_CENTER | DT_VCENTER);
                }
            }
        }
        DrawLabel(dc, tr(L"卡片工具栏", L"Card toolbar"),
            Rect(left, layout.toolbarLabelTop,
                clientRight() - 26, layout.toolbarTop - 6),
            RGB(216, 218, 223), 12, FW_SEMIBOLD);
        paintOptionButton(dc, filePresentationRect(), L"\uE71B",
            card.showPresentationControl, SettingsActionKind::TogglePresentationControl);
        paintOptionButton(dc, collapseRect(), L"\uE70E", card.showCollapseControl,
            SettingsActionKind::ToggleCollapseControl);
        paintOptionButton(dc, pinControlSettingRect(), L"\uE718", card.showPinControl,
            SettingsActionKind::TogglePinControl);
        DrawLabel(dc, tr(L"卡片选项", L"Card options"),
            Rect(left, layout.optionsLabelTop,
                clientRight() - 26, layout.optionsTop - 6),
            RGB(216, 218, 223), 12, FW_SEMIBOLD);
        paintOptionButton(dc, itemNamesRect(), L"\uE8D2", card.content.showItemNames,
            SettingsActionKind::ToggleItemNames);
        paintOptionButton(dc, sizeModeRect(), L"\uE740",
            card.content.sizeMode == domain::CardSizeMode::Adaptive,
            SettingsActionKind::ToggleSizeMode);
        paintOptionButton(dc, heightLimitRect(), L"\uE74B",
            card.content.maximumVisibleRows.has_value(),
            SettingsActionKind::ToggleHeightLimit);
        paintOptionButton(dc, positionLockRect(), L"\uE72E",
            card.positionLocked, SettingsActionKind::TogglePositionLock);
        if (card.content.sizeMode == domain::CardSizeMode::Fixed) {
            const auto paintStepper = [&](RECT rect, std::wstring label,
                                          std::uint32_t value,
                                          SettingsActionKind decreaseKind,
                                          SettingsActionKind increaseKind,
                                          bool canDecrease) {
                FillRounded(dc, rect, RGB(43, 45, 50), 7);
                DrawRoundedOutline(dc, rect, RGB(61, 64, 70), 7);
                const auto decreaseAction = SettingsAction{decreaseKind, selectedCard};
                const auto increaseAction = SettingsAction{increaseKind, selectedCard};
                const auto decrease = decrementRect(rect);
                const auto increase = incrementRect(rect);
                if (canDecrease && (hovered == decreaseAction || pressed == decreaseAction)) {
                    FillRounded(dc, decrease,
                        pressed == decreaseAction ? kAccentPressed : RGB(54, 56, 62), 6);
                }
                if (value < 64 && (hovered == increaseAction || pressed == increaseAction)) {
                    FillRounded(dc, increase,
                        pressed == increaseAction ? kAccentPressed : RGB(54, 56, 62), 6);
                }
                DrawLabel(dc, L"\u2212", decrease,
                    canDecrease ? RGB(213, 216, 222) : RGB(91, 94, 101),
                    13, FW_NORMAL, DT_CENTER | DT_VCENTER);
                DrawLabel(dc, L"+", increase,
                    value < 64 ? RGB(213, 216, 222) : RGB(91, 94, 101),
                    13, FW_NORMAL, DT_CENTER | DT_VCENTER);
                const auto valueRect = Rect(
                    decrease.right, rect.top, increase.left, rect.bottom);
                DrawLabel(dc, label + L" " + std::to_wstring(value), valueRect,
                    RGB(232, 234, 238), 10, FW_SEMIBOLD,
                    DT_CENTER | DT_VCENTER);
            };
            const auto canDecreaseColumns = card.content.widthSpan
                    > domain::MinimumCardWidthSpan(card.content.itemSize)
                && fixedGridFits(card, card.content.widthSpan - 1, card.content.fixedRows);
            const auto canDecreaseRows = card.content.fixedRows > 1
                && fixedGridFits(card, card.content.widthSpan, card.content.fixedRows - 1);
            paintStepper(
                fixedColumnsRect(), tr(L"宽", L"W"), card.content.widthSpan,
                SettingsActionKind::DecreaseFixedColumns,
                SettingsActionKind::IncreaseFixedColumns,
                canDecreaseColumns);
            paintStepper(
                fixedRowsRect(), tr(L"高", L"H"), card.content.fixedRows,
                SettingsActionKind::DecreaseFixedRows,
                SettingsActionKind::IncreaseFixedRows,
                canDecreaseRows);
        }
        if (card.content.maximumVisibleRows.has_value()) {
            const auto rect = maximumVisibleRowsRect();
            FillRounded(dc, rect, RGB(43, 45, 50), 7);
            DrawRoundedOutline(dc, rect, RGB(61, 64, 70), 7);
            const auto decrease = decrementRect(rect);
            const auto increase = incrementRect(rect);
            DrawLabel(dc, L"\u2212", decrease, RGB(213, 216, 222), 13,
                FW_NORMAL, DT_CENTER | DT_VCENTER);
            DrawLabel(dc, L"+", increase, RGB(213, 216, 222), 13,
                FW_NORMAL, DT_CENTER | DT_VCENTER);
            DrawLabel(dc,
                tr(L"最大 ", L"Max ") + std::to_wstring(*card.content.maximumVisibleRows),
                Rect(decrease.right, rect.top, increase.left, rect.bottom),
                RGB(232, 234, 238), 10, FW_SEMIBOLD,
                DT_CENTER | DT_VCENTER);
        }
        paintCardContentPreview(dc, card);
    }

    void paintTodoSettings(HDC dc, const presentation::CardView& card) noexcept {
        const auto left = cardDetailLeft();
        const auto layout = ResolveFileCardSettingsLayout(false);
        DrawLabel(dc, tr(L"卡片工具栏", L"Card toolbar"),
            Rect(left, layout.toolbarLabelTop,
                clientRight() - 26, layout.toolbarTop - 6),
            RGB(216, 218, 223), 12, FW_SEMIBOLD);
        paintOptionButton(dc, collapseRect(), L"\uE70E", card.showCollapseControl,
            SettingsActionKind::ToggleCollapseControl);
        paintOptionButton(dc, pinControlSettingRect(), L"\uE718", card.showPinControl,
            SettingsActionKind::TogglePinControl);
        DrawLabel(dc, tr(L"卡片选项", L"Card options"),
            Rect(left, layout.optionsLabelTop,
                clientRight() - 26, layout.optionsTop - 6),
            RGB(216, 218, 223), 12, FW_SEMIBOLD);
        paintOptionButton(dc, createdTimeRect(), L"\uE823",
            card.todoPreferences.showCreatedTime, SettingsActionKind::ToggleCreatedTime);
        paintOptionButton(dc, todoHeightLimitRect(), L"\uE74B",
            card.content.maximumVisibleRows.has_value(), SettingsActionKind::ToggleHeightLimit);
        paintOptionButton(dc, positionLockRect(), L"\uE72E",
            card.positionLocked, SettingsActionKind::TogglePositionLock);
        if (card.content.maximumVisibleRows.has_value()) {
            const auto rect = todoMaximumVisibleRowsRect();
            FillRounded(dc, rect, RGB(43, 45, 50), 7);
            DrawRoundedOutline(dc, rect, RGB(61, 64, 70), 7);
            DrawLabel(dc, L"\u2212", decrementRect(rect), RGB(213, 216, 222), 13,
                FW_NORMAL, DT_CENTER | DT_VCENTER);
            DrawLabel(dc, L"+", incrementRect(rect), RGB(213, 216, 222), 13,
                FW_NORMAL, DT_CENTER | DT_VCENTER);
            DrawLabel(dc,
                tr(L"最大 ", L"Max ") + std::to_wstring(*card.content.maximumVisibleRows),
                Rect(decrementRect(rect).right, rect.top, incrementRect(rect).left, rect.bottom),
                RGB(232, 234, 238), 10, FW_SEMIBOLD,
                DT_CENTER | DT_VCENTER);
        }
        const auto contentPanel = todoContentPreviewRect(card);
        paintCardContentPreview(dc, card, contentPanel);
        DrawLabel(dc, tr(L"已归档项目请在左侧“归档”中管理。",
            L"Manage archived items from Archive in the sidebar."),
            Rect(left, contentPanel.bottom + 8, clientRight() - 26,
                contentPanel.bottom + 38), RGB(137, 140, 148), 11);
    }

    void paintCardContentPreview(
        HDC dc,
        const presentation::CardView& card,
        RECT panel = {}) noexcept {
        if (panel.right <= panel.left || panel.bottom <= panel.top) {
            panel = contentPreviewRect(card);
        }
        paintSection(dc, panel);
        const auto indices = contentItemIndices(card);
        const auto total = indices.size();
        DrawGlyph(dc, CardTypeGlyph(card.type),
            Rect(panel.left + 12, panel.top + 7, panel.left + 34, panel.top + 31),
            RGB(104, 153, 224), 13);
        DrawLabel(dc, DefaultCardTitle(card, usesEnglish()),
            Rect(panel.left + 40, panel.top + 8, panel.right - 80, panel.top + 32),
            RGB(221, 223, 227), 11, FW_SEMIBOLD);
        DrawLabel(dc, std::to_wstring(total),
            Rect(panel.right - 64, panel.top + 8, panel.right - 14, panel.top + 32),
            RGB(143, 147, 156), 10, FW_NORMAL, DT_RIGHT | DT_VCENTER);
        if (total == 0) {
            DrawLabel(dc, tr(L"暂无内容", L"No contents"),
                Rect(panel.left + 14, panel.top + 35, panel.right - 14, panel.bottom - 8),
                RGB(133, 137, 145), 10);
            return;
        }
        if (card.type == domain::CardType::Application
            || card.type == domain::CardType::Mapping) {
            constexpr std::size_t columns = 8;
            const auto cellWidth = std::max<LONG>(48, (panel.right - panel.left - 24) / 8);
            for (std::size_t index = 0; index < total; ++index) {
                const auto sourceIndex = indices[index];
                const auto column = static_cast<LONG>(index % columns);
                const auto row = static_cast<LONG>(index / columns);
                const auto cellLeft = panel.left + 12 + column * cellWidth;
                const auto cellTop = panel.top + 40 + row * 58;
                const auto iconRect = Rect(cellLeft + 8, cellTop + 2,
                    cellLeft + cellWidth - 8, cellTop + 50);
                if (!DrawCardItemIcon(dc, card.items[sourceIndex].icon, iconRect)) {
                    DrawGlyph(dc, L"\uE8A5", iconRect, RGB(145, 165, 196), 18);
                }
            }
            return;
        }
        for (std::size_t index = 0; index < total; ++index) {
            const auto sourceIndex = indices[index];
            std::wstring label;
            if (card.type == domain::CardType::Todo) {
                label = Utf8ToWide(card.todoItems[sourceIndex].title);
            } else {
                label = card.items[sourceIndex].displayName;
            }
            const auto row = contentPreviewRowRect(panel, index);
            const auto iconRect = Rect(row.left, row.top + 4, row.left + 20, row.bottom - 4);
            const auto drewIcon = card.type != domain::CardType::Todo
                && DrawCardItemIcon(dc, card.items[sourceIndex].icon, iconRect);
            if (!drewIcon) {
                DrawGlyph(dc,
                    card.type == domain::CardType::Todo ? L"\uE73E" : L"\uE8A5",
                    Rect(row.left, row.top, row.left + 24, row.bottom),
                    RGB(145, 165, 196), 11);
            }
            DrawLabel(dc, label,
                Rect(row.left + 28, row.top, row.right, row.bottom),
                RGB(197, 200, 207), 10, FW_NORMAL,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }

    void paintOptionButton(
        HDC dc,
        RECT rect,
        std::wstring_view glyph,
        bool enabled,
        SettingsActionKind kind) noexcept {
        const auto action = SettingsAction{kind, selectedCard};
        const auto hot = hovered == action;
        const auto down = pressed == action;
        FillRounded(dc, rect,
            down ? kAccentPressed
                : enabled ? kAccent
                : hot ? RGB(46, 48, 54) : RGB(34, 36, 41), 7);
        DrawRoundedOutline(dc, rect,
            enabled ? RGB(105, 169, 255)
                : hot ? RGB(91, 95, 104) : RGB(62, 65, 72), 7);
        if (enabled) {
            DrawGlyphRaw(dc, glyph, rect, RGB(255, 255, 255), 15);
        } else {
            DrawGlyph(dc, glyph, rect, RGB(194, 198, 206), 15);
        }
    }

    void paintArchiveCalendar(HDC dc) noexcept {
        if (!archiveCalendarOpen) return;
        const auto panel = archiveCalendarPanelRect();
        FillRounded(dc, panel, RGB(29, 31, 35), 8);
        DrawRoundedOutline(dc, panel, RGB(68, 72, 80), 8);

        const auto previous = archiveCalendarPreviousRect();
        const auto previousAction = SettingsAction{SettingsActionKind::PreviousArchiveMonth};
        if (hovered == previousAction || pressed == previousAction) {
            FillRounded(dc, previous,
                pressed == previousAction ? RGB(48, 67, 94) : RGB(42, 48, 58), 6);
        }
        DrawGlyph(dc, L"\uE76B", previous, RGB(196, 200, 208), 13);
        const auto next = archiveCalendarNextRect();
        const auto nextAction = SettingsAction{SettingsActionKind::NextArchiveMonth};
        const auto canAdvance = archiveCalendarCanAdvance();
        if (canAdvance && (hovered == nextAction || pressed == nextAction)) {
            FillRounded(dc, next,
                pressed == nextAction ? RGB(48, 67, 94) : RGB(42, 48, 58), 6);
        }
        DrawGlyph(dc, L"\uE76C", next,
            canAdvance ? RGB(196, 200, 208) : RGB(91, 94, 101), 13);

        wchar_t title[48]{};
        if (usesEnglish()) {
            constexpr std::array<const wchar_t*, 12> months{
                L"January", L"February", L"March", L"April", L"May", L"June",
                L"July", L"August", L"September", L"October", L"November", L"December"};
            const auto month = std::clamp<int>(archiveCalendarMonth.month, 1, 12);
            swprintf_s(title, L"%s %d", months[month - 1], archiveCalendarMonth.year);
        } else {
            swprintf_s(title, L"%d年%u月", archiveCalendarMonth.year,
                static_cast<unsigned>(archiveCalendarMonth.month));
        }
        DrawLabel(dc, title,
            Rect(previous.right + 4, panel.top + 8, next.left - 4, panel.top + 42),
            RGB(234, 236, 240), 13, FW_SEMIBOLD,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        const std::array<std::wstring_view, 7> weekdays = usesEnglish()
            ? std::array<std::wstring_view, 7>{L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat", L"Sun"}
            : std::array<std::wstring_view, 7>{L"一", L"二", L"三", L"四", L"五", L"六", L"日"};
        for (std::size_t column = 0; column < weekdays.size(); ++column) {
            const auto left = panel.left + static_cast<int>(column) * 48;
            DrawLabel(dc, weekdays[column], Rect(left, 176, left + 48, 204),
                RGB(131, 135, 143), 10, FW_NORMAL,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        const auto selected = selectedArchiveDate();
        const auto today = domain::CurrentTodoDate(timeZoneOffsetMinutes);
        for (std::size_t index = 0; index < 42; ++index) {
            const auto date = archiveCalendarCellDate(index);
            const auto rect = archiveCalendarDayRect(index);
            const auto selectedCell = date == selected;
            const auto todayCell = date == today;
            const auto currentMonth = date.year == archiveCalendarMonth.year
                && date.month == archiveCalendarMonth.month;
            const auto future = domain::CompareTodoDates(date, today) > 0;
            const auto action = SettingsAction{SettingsActionKind::SelectArchiveDate, index};
            if (selectedCell) {
                FillRounded(dc, Rect(rect.left + 7, rect.top + 2, rect.right - 7, rect.bottom - 2),
                    kAccent, 6);
            } else if (!future && (hovered == action || pressed == action)) {
                FillRounded(dc, Rect(rect.left + 7, rect.top + 2, rect.right - 7, rect.bottom - 2),
                    pressed == action ? RGB(48, 67, 94) : RGB(43, 47, 54), 6);
            }
            if (todayCell && !selectedCell) {
                DrawRoundedOutline(dc,
                    Rect(rect.left + 7, rect.top + 2, rect.right - 7, rect.bottom - 2),
                    RGB(76, 139, 227), 6);
            }
            wchar_t day[4]{};
            swprintf_s(day, L"%u", static_cast<unsigned>(date.day));
            DrawLabel(dc, day, rect,
                selectedCell ? RGB(255, 255, 255)
                    : future ? RGB(76, 79, 86)
                    : currentMonth ? RGB(222, 224, 229) : RGB(105, 108, 116),
                11, selectedCell ? FW_SEMIBOLD : FW_NORMAL,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    void paintArchive(HDC dc) noexcept {
        paintPageTitle(dc, tr(L"归档", L"Archive"),
            tr(L"查看详细信息并逐条恢复已归档待办",
                L"Review details and restore archived tasks individually"));
        const auto addAction = SettingsAction{SettingsActionKind::AddHistoricalArchive};
        const auto add = archiveAddButtonRect();
        FillRounded(dc, add,
            pressed == addAction ? kAccentPressed
                : hovered == addAction ? kAccentHover : kAccent, 7);
        DrawGlyph(dc, L"\uE710", Rect(add.left + 8, add.top, add.left + 34, add.bottom),
            RGB(255, 255, 255), 12);
        DrawLabel(dc, tr(L"添加", L"Add"), Rect(add.left + 30, add.top, add.right - 8, add.bottom),
            RGB(255, 255, 255), 11, FW_SEMIBOLD, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        const auto exportAction = SettingsAction{SettingsActionKind::ExportArchive};
        const auto exportButton = archiveExportButtonRect();
        FillRounded(dc, exportButton,
            pressed == exportAction ? RGB(48, 67, 94)
                : hovered == exportAction ? RGB(42, 48, 58) : RGB(34, 36, 41), 7);
        DrawRoundedOutline(dc, exportButton, RGB(58, 61, 68), 7);
        DrawGlyph(dc, L"\uEDE1", Rect(exportButton.left + 7, exportButton.top,
            exportButton.left + 33, exportButton.bottom), RGB(203, 207, 216), 12);
        DrawLabel(dc, tr(L"导出", L"Export"), Rect(exportButton.left + 29,
            exportButton.top, exportButton.right - 7, exportButton.bottom),
            RGB(225, 228, 234), 11, FW_SEMIBOLD,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        const auto previous = archiveDatePreviousRect();
        const auto previousAction = SettingsAction{SettingsActionKind::PreviousArchiveDate};
        FillRounded(dc, previous,
            pressed == previousAction ? RGB(48, 67, 94)
                : hovered == previousAction ? RGB(42, 48, 58) : RGB(34, 36, 41), 7);
        DrawRoundedOutline(dc, previous, RGB(58, 61, 68), 7);
        DrawGlyph(dc, L"\uE76B", previous, RGB(196, 200, 208), 14);

        const auto dateLabel = archiveDateLabelRect();
        const auto dateAction = SettingsAction{SettingsActionKind::ToggleArchiveCalendar};
        FillRounded(dc, dateLabel,
            pressed == dateAction ? RGB(48, 67, 94)
                : hovered == dateAction || archiveCalendarOpen
                ? RGB(42, 48, 58) : RGB(34, 36, 41), 7);
        DrawRoundedOutline(dc, dateLabel, RGB(58, 61, 68), 7);
        DrawLabel(dc, archiveDateText(), dateLabel, RGB(232, 234, 238), 12,
            FW_SEMIBOLD, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawGlyph(dc, archiveCalendarOpen ? L"\uE70E" : L"\uE70D",
            Rect(dateLabel.right - 38, dateLabel.top, dateLabel.right - 8, dateLabel.bottom),
            RGB(154, 158, 166), 10);

        const auto next = archiveDateNextRect();
        const auto nextAction = SettingsAction{SettingsActionKind::NextArchiveDate};
        const auto canAdvance = archiveDateOffset < 0;
        FillRounded(dc, next,
            pressed == nextAction ? RGB(48, 67, 94)
                : hovered == nextAction && canAdvance ? RGB(42, 48, 58)
                : RGB(34, 36, 41), 7);
        DrawRoundedOutline(dc, next, RGB(58, 61, 68), 7);
        DrawGlyph(dc, L"\uE76C", next,
            canAdvance ? RGB(196, 200, 208) : RGB(91, 94, 101), 14);

        const auto entries = archivedEntries();
        if (entries.empty()) {
            const auto empty = Rect(kContentLeft, 186, clientRight() - 26, 274);
            paintSection(dc, empty);
            DrawGlyph(dc, L"\uE7B8", Rect(empty.left + 18, empty.top + 22,
                empty.left + 54, empty.top + 58), RGB(145, 148, 156), 20);
            DrawLabel(dc, tr(L"归档中没有内容", L"Archive is empty"),
                Rect(empty.left + 66, empty.top + 12, empty.right - 20, empty.top + 42),
                RGB(234, 235, 238), 14, FW_SEMIBOLD);
            DrawLabel(dc, tr(L"完成的待办归档后会显示在这里。",
                L"Archived completed tasks appear here."),
                Rect(empty.left + 66, empty.top + 42, empty.right - 20, empty.top + 70),
                RGB(145, 148, 156), 12);
            paintArchiveCalendar(dc);
            paintArchiveOverlay(dc);
            return;
        }
        const auto count = std::min(visibleArchiveRows(), entries.size() - std::min(entries.size(), archiveOffset));
        for (std::size_t visible = 0; visible < count; ++visible) {
            const auto& entry = entries[archiveOffset + visible];
            const auto& card = cards[entry.cardIndex];
            const auto& item = card.todoItems[entry.itemIndex];
            const auto row = archiveRow(visible);
            FillRounded(dc, row, RGB(36, 37, 41), 7);
            DrawLabel(dc, Utf8ToWide(item.title),
                Rect(row.left + 16, row.top + 8, row.right - 112, row.top + 34),
                RGB(238, 239, 242), 12, FW_SEMIBOLD);
            const auto timestamp = item.completedAtUnixMilliseconds > 0
                ? TodoTimeText(item.completedAtUnixMilliseconds,
                    timeZoneOffsetMinutes, true, usesEnglish())
                : TodoTimeText(item.createdAtUnixMilliseconds,
                    timeZoneOffsetMinutes, false, usesEnglish());
            const auto detail = DefaultCardTitle(card, usesEnglish()) + L"  ·  "
                + timestamp
                + L"  ·  " + TodoDateText(item, usesEnglish());
            DrawLabel(dc, detail,
                Rect(row.left + 16, row.top + 36, row.right - 112, row.bottom - 7),
                RGB(139, 142, 150), 10);
            const auto action = SettingsAction{SettingsActionKind::RestoreArchivedItem, entry.cardIndex, entry.itemIndex};
            const auto restore = archiveRestoreRect(visible);
            FillRounded(dc, restore,
                pressed == action ? RGB(49, 83, 132)
                    : hovered == action ? RGB(46, 61, 82) : RGB(42, 44, 49), 7);
            DrawGlyph(dc, L"\uE7A7", restore,
                hovered == action ? RGB(145, 188, 250) : RGB(190, 194, 202), 14);
            const auto deleteAction = SettingsAction{
                SettingsActionKind::DeleteArchivedItem, entry.cardIndex, entry.itemIndex};
            const auto erase = archiveDeleteRect(visible);
            FillRounded(dc, erase,
                pressed == deleteAction ? RGB(91, 47, 52)
                    : hovered == deleteAction ? RGB(67, 45, 49) : RGB(42, 44, 49), 7);
            DrawGlyph(dc, L"\uE74D", erase,
                hovered == deleteAction ? RGB(245, 157, 163) : RGB(190, 194, 202), 14);
        }
        paintArchiveCalendar(dc);
        paintArchiveOverlay(dc);
    }

    void paintArchiveOverlay(HDC dc) noexcept {
        if (archiveOverlay == ArchiveOverlay::None) return;
        const auto panel = archiveOverlayRect();
        FillRounded(dc, panel, RGB(30, 32, 37), 10);
        DrawRoundedOutline(dc, panel, RGB(71, 75, 84), 10);
        DrawLabel(dc,
            archiveOverlay == ArchiveOverlay::Add
                ? tr(L"补充历史归档", L"Add historical archive")
                : tr(L"导出归档", L"Export archive"),
            Rect(panel.left + 20, panel.top + 14, panel.right - 20, panel.top + 48),
            RGB(242, 244, 247), 16, FW_SEMIBOLD);
        if (archiveOverlay == ArchiveOverlay::Add) {
            const auto card = archiveOverlayCardRect();
            FillRounded(dc, card, RGB(39, 42, 48), 7);
            DrawRoundedOutline(dc, card, RGB(65, 69, 78), 7);
            const auto cardTitle = historicalArchiveCard < cards.size()
                ? DefaultCardTitle(cards[historicalArchiveCard], usesEnglish()) : L"";
            DrawLabel(dc, cardTitle, Rect(card.left + 12, card.top, card.right - 38, card.bottom),
                RGB(229, 231, 236), 12, FW_NORMAL, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            DrawGlyph(dc, L"\uE70D", Rect(card.right - 34, card.top, card.right - 8, card.bottom),
                RGB(153, 157, 166), 10);
            DrawLabel(dc, tr(L"归档日期：", L"Archive date: ") + archiveDateText(),
                Rect(panel.left + 20, panel.top + 166, panel.right - 20, panel.top + 200),
                RGB(165, 169, 178), 11);
        } else {
            const std::array values{archiveExportBegin, archiveExportEnd};
            const std::array labels{tr(L"开始日期", L"Start date"), tr(L"结束日期", L"End date")};
            for (std::size_t index = 0; index < values.size(); ++index) {
                const auto rect = archiveExportDateRect(index);
                const auto selected = (index == 0
                        && archiveExportDateField == ArchiveExportDateField::Begin)
                    || (index == 1
                        && archiveExportDateField == ArchiveExportDateField::End);
                FillRounded(dc, rect, selected ? RGB(43, 91, 153) : RGB(39, 42, 48), 7);
                DrawRoundedOutline(dc, rect, selected ? kAccent : RGB(65, 69, 78), 7);
                DrawLabel(dc, labels[index],
                    Rect(rect.left + 10, rect.top + 2, rect.right - 10, rect.top + 19),
                    RGB(145, 150, 160), 9, FW_NORMAL,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                DrawLabel(dc, archiveDateValueText(values[index]),
                    Rect(rect.left + 10, rect.top + 17, rect.right - 10, rect.bottom - 2),
                    RGB(231, 233, 238), 11, FW_SEMIBOLD,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }
            if (archiveExportDateField != ArchiveExportDateField::None) {
                const auto calendar = archiveExportCalendarRect();
                FillRounded(dc, calendar, RGB(35, 38, 44), 8);
                DrawRoundedOutline(dc, calendar, RGB(69, 74, 84), 8);
                const auto previous = archiveExportCalendarPreviousRect();
                const auto next = archiveExportCalendarNextRect();
                DrawGlyph(dc, L"\uE76B", previous, RGB(204, 208, 216), 11);
                DrawGlyph(dc, L"\uE76C", next,
                    archiveExportCalendarCanAdvance()
                        ? RGB(204, 208, 216) : RGB(104, 109, 119), 11);
                wchar_t month[32]{};
                swprintf_s(month, L"%04d-%02u", archiveExportCalendarMonth.year,
                    static_cast<unsigned>(archiveExportCalendarMonth.month));
                DrawLabel(dc, month,
                    Rect(previous.right, calendar.top + 5, next.left, calendar.top + 41),
                    RGB(236, 238, 242), 12, FW_SEMIBOLD,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                const std::array<std::wstring_view, 7> weekdays = usesEnglish()
                    ? std::array<std::wstring_view, 7>{L"M", L"T", L"W", L"T", L"F", L"S", L"S"}
                    : std::array<std::wstring_view, 7>{L"一", L"二", L"三", L"四", L"五", L"六", L"日"};
                const auto width = calendar.right - calendar.left;
                for (std::size_t index = 0; index < weekdays.size(); ++index) {
                    DrawLabel(dc, std::wstring(weekdays[index]),
                        Rect(calendar.left + static_cast<int>(index) * width / 7,
                            calendar.top + 40,
                            calendar.left + static_cast<int>(index + 1) * width / 7,
                            calendar.top + 64),
                        RGB(145, 150, 160), 9, FW_NORMAL,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }
                for (std::size_t index = 0; index < 42; ++index) {
                    const auto date = archiveExportCalendarCellDate(index);
                    const auto day = archiveExportCalendarDayRect(index);
                    const auto selected = date == archiveExportBegin || date == archiveExportEnd;
                    const auto future = domain::CompareTodoDates(
                        date, domain::CurrentTodoDate(timeZoneOffsetMinutes)) > 0;
                    if (selected) FillRounded(dc, day, RGB(43, 91, 153), 6);
                    DrawLabel(dc, std::to_wstring(date.day), day,
                        future ? RGB(85, 89, 98)
                        : date.month == archiveExportCalendarMonth.month
                            ? RGB(226, 229, 235) : RGB(112, 117, 127),
                        10, selected ? FW_SEMIBOLD : FW_NORMAL,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }
            } else {
                DrawLabel(dc, tr(L"点击日期框选择导出范围。", L"Select the start and end dates."),
                    Rect(panel.left + 20, panel.top + 128, panel.right - 20, panel.top + 164),
                    RGB(152, 156, 165), 11);
            }
        }
        const auto cancel = archiveOverlayCancelRect();
        const auto cancelAction = SettingsAction{SettingsActionKind::CancelArchiveOverlay};
        FillRounded(dc, cancel, hovered == cancelAction ? RGB(48, 51, 58) : RGB(40, 42, 47), 7);
        DrawLabel(dc, tr(L"取消", L"Cancel"), cancel, RGB(217, 220, 226), 11,
            FW_SEMIBOLD, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        const auto confirm = archiveOverlayConfirmRect();
        FillRounded(dc, confirm, RGB(51, 136, 255), 7);
        DrawLabel(dc, archiveOverlay == ArchiveOverlay::Add ? tr(L"添加", L"Add") : tr(L"导出", L"Export"),
            confirm, RGB(255, 255, 255), 11, FW_SEMIBOLD,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    void paintAbout(HDC dc) noexcept {
        paintPageTitle(dc, tr(L"关于", L"About"), L"");
        const auto center = (kContentLeft + clientRight() - 26) / 2;
        const auto icon = static_cast<HICON>(LoadImageW(
            module, MAKEINTRESOURCEW(kDestoIconResourceId), IMAGE_ICON,
            88, 88, LR_DEFAULTCOLOR));
        if (icon != nullptr) {
            DrawIconEx(dc, center - 44, 108, icon, 88, 88, 0, nullptr, DI_NORMAL);
            DestroyIcon(icon);
        }
        DrawLabel(dc, L"Desto", Rect(kContentLeft, 210, clientRight() - 26, 252),
            RGB(247, 248, 250), 26, FW_SEMIBOLD, DT_CENTER | DT_VCENTER);
        const auto versionText = std::wstring(usesEnglish()
            ? L"Version: " : L"当前版本：")
            + std::to_wstring(DESTO_VERSION_MAJOR) + L"."
            + std::to_wstring(DESTO_VERSION_MINOR) + L"."
            + std::to_wstring(DESTO_VERSION_PATCH)
            + (updateChannel == "development"
                ? L"." + std::to_wstring(DESTO_VERSION_BUILD)
                : L"");
        DrawLabel(dc, versionText,
            Rect(kContentLeft, 250, clientRight() - 26, 282),
            RGB(165, 169, 177), 12, FW_NORMAL, DT_CENTER | DT_VCENTER);
        const auto paintCommand = [&](RECT rect, const SettingsAction& action,
                                      std::wstring_view label, std::wstring_view glyph) {
            FillRounded(dc, rect, pressed == action ? kAccentPressed
                : hovered == action ? kAccentHover : kAccent, 7);
            DrawGlyphRaw(dc, glyph, Rect(rect.left + 12, rect.top,
                rect.left + 36, rect.bottom), RGB(255, 255, 255), 13);
            DrawLabelRaw(dc, label, Rect(rect.left + 40, rect.top,
                rect.right - 10, rect.bottom), RGB(255, 255, 255), 11,
                FW_SEMIBOLD, DT_CENTER | DT_VCENTER);
        };
        paintCommand(openProjectRect(), {SettingsActionKind::OpenProject},
            tr(L"查看项目", L"View project"), L"\uE8A7");
        paintCommand(checkForUpdatesRect(), {SettingsActionKind::CheckForUpdates},
            tr(L"检查更新", L"Check for updates"), L"\uE895");
        paintCommand(updateChannelRect(), {SettingsActionKind::ToggleUpdateChannel},
            updateChannel == "development"
                ? tr(L"开发版通道", L"Development channel")
                : tr(L"稳定版通道", L"Stable channel"), L"\uE7BA");
    }

    std::wstring title;
    ULONG_PTR gdiplusToken = 0;
    HINSTANCE module = nullptr;
    HWND window = nullptr;
    HWND renameEdit = nullptr;
    HWND archiveSearchEdit = nullptr;
    HWND archiveAddEdit = nullptr;
    std::optional<std::size_t> renameCardIndex;
    bool closingRename = false;
    std::vector<presentation::CardView> cards;
    SettingsPage page = SettingsPage::System;
    std::size_t selectedCard = 0;
    std::size_t archiveOffset = 0;
    std::int32_t archiveDateOffset = -1;
    bool archiveCalendarOpen = false;
    ArchiveOverlay archiveOverlay = ArchiveOverlay::None;
    std::size_t historicalArchiveCard = 0;
    domain::TodoDate archiveExportBegin{1970, 1, 1};
    domain::TodoDate archiveExportEnd{1970, 1, 1};
    domain::TodoDate archiveExportCalendarMonth{1970, 1, 1};
    ArchiveExportDateField archiveExportDateField = ArchiveExportDateField::None;
    domain::TodoDate archiveCalendarMonth{1970, 1, 1};
    std::unordered_map<domain::CardId, int> cardEditorOffsets;
    double globalCornerRadius = 16.0;
    std::optional<std::int32_t> timeZoneOffsetMinutes;
    std::string language = "system";
    std::filesystem::path storageRoot;
    std::optional<std::filesystem::path> pendingStorageRoot;
    bool runAtStartup = false;
    std::string desktopDoubleClickAction = "none";
    std::string taskbarDoubleClickAction = "none";
    bool pinnedCardsYieldToFullscreen = true;
    bool showIconBackgroundFrame = false;
    bool confirmFileDeletion = true;
    std::string updateChannel = "stable";
    SystemDropdown activeSystemDropdown = SystemDropdown::None;
    bool addMenuOpen = false;
    bool cardMenuOpen = false;
    bool applicationSortMenuOpen = false;
    std::optional<SettingsAction> pendingDeletion;
    std::optional<SettingsAction> keyboardFocus;
    SettingsAction hovered;
    SettingsAction pressed;
    std::optional<std::size_t> cardDragSource;
    std::optional<std::size_t> cardDragTarget;
    ULONGLONG cardDragStartedAt = 0;
    POINT cardDragStart{};
    bool cardDragActive = false;
    AppearanceChangedCallback appearanceChanged;
    ContentChangedCallback contentChanged;
    ApplicationSortChangedCallback applicationSortChanged;
    MappingSortChangedCallback mappingSortChanged;
    MappingModeChangedCallback mappingModeChanged;
    ChromeChangedCallback chromeChanged;
    TodoPreferencesChangedCallback todoPreferencesChanged;
    RestoreArchivedCallback restoreArchived;
    RestoreArchivedItemCallback restoreArchivedItem;
    DeleteArchivedItemCallback deleteArchivedItem;
    ArchiveTodoItemCallback archiveTodoItem;
    HistoricalArchiveAddedCallback historicalArchiveAdded;
    ArchiveExportCallback archiveExport;
    CardAddedCallback cardAdded;
    CardDeletedCallback cardDeleted;
    CardRenamedCallback cardRenamed;
    CardVisibilityChangedCallback cardVisibilityChanged;
    CardOrderChangedCallback cardOrderChanged;
    GlobalCornerRadiusChangedCallback globalCornerRadiusChanged;
    TimeZoneChangedCallback timeZoneChanged;
    LanguageChangedCallback languageChanged;
    StorageRootChangedCallback storageRootChanged;
    RunAtStartupChangedCallback runAtStartupChanged;
    DesktopDoubleClickActionChangedCallback desktopDoubleClickActionChanged;
    TaskbarDoubleClickActionChangedCallback taskbarDoubleClickActionChanged;
    PinnedCardsYieldToFullscreenChangedCallback pinnedCardsYieldToFullscreenChanged;
    IconBackgroundFrameChangedCallback iconBackgroundFrameChanged;
    FileDeletionConfirmationChangedCallback fileDeletionConfirmationChanged;
    UpdateChannelChangedCallback updateChannelChanged;
    static constexpr const wchar_t* className = L"DestoSettingsWindow";
};

WindowsSettingsHost::WindowsSettingsHost(std::wstring title)
    : impl_(std::make_unique<Impl>(std::move(title))) {}

WindowsSettingsHost::~WindowsSettingsHost() = default;

void WindowsSettingsHost::present(
    std::span<const presentation::CardView> cards,
    const WindowsSystemSettings& settings) {
    const auto selectedId = impl_->selectedCard < impl_->cards.size()
        ? std::optional<domain::CardId>(impl_->cards[impl_->selectedCard].id) : std::nullopt;
    impl_->cards.assign(cards.begin(), cards.end());
    impl_->globalCornerRadius = settings.globalCornerRadius;
    impl_->timeZoneOffsetMinutes = settings.timeZoneOffsetMinutes;
    impl_->language = settings.language;
    impl_->storageRoot = settings.storageRoot;
    impl_->runAtStartup = settings.runAtStartup;
    impl_->desktopDoubleClickAction = settings.desktopDoubleClickAction;
    impl_->taskbarDoubleClickAction = settings.taskbarDoubleClickAction;
    impl_->pinnedCardsYieldToFullscreen = settings.pinnedCardsYieldToFullscreen;
    impl_->showIconBackgroundFrame = settings.showIconBackgroundFrame;
    impl_->confirmFileDeletion = settings.confirmFileDeletion;
    impl_->updateChannel = settings.updateChannel;
    if (selectedId.has_value()) {
        const auto found = std::ranges::find(impl_->cards, *selectedId, &presentation::CardView::id);
        impl_->selectedCard = found == impl_->cards.end()
            ? 0 : static_cast<std::size_t>(std::distance(impl_->cards.begin(), found));
    } else if (impl_->selectedCard >= impl_->cards.size()) {
        impl_->selectedCard = 0;
    }
    impl_->clampArchiveOffset();
    impl_->clampCardEditorOffset();
    impl_->updateArchiveSearchEditor();
    impl_->updateArchiveAddEditor();
    if (impl_->keyboardFocus.has_value()) {
        const auto actions = impl_->keyboardActions();
        if (std::ranges::find(actions, *impl_->keyboardFocus) == actions.end()) {
            impl_->keyboardFocus.reset();
            impl_->hovered = {};
        }
    }
    InvalidateRect(impl_->window, nullptr, FALSE);
}

void WindowsSettingsHost::updateCard(presentation::CardView card) {
    const auto found = std::ranges::find(
        impl_->cards, card.id, &presentation::CardView::id);
    if (found == impl_->cards.end()) return;
    *found = std::move(card);
    impl_->clampArchiveOffset();
    impl_->clampCardEditorOffset();
    if (impl_->window != nullptr) InvalidateRect(impl_->window, nullptr, FALSE);
}

void WindowsSettingsHost::insertCard(presentation::CardView card) {
    const auto found = std::ranges::find(
        impl_->cards, card.id, &presentation::CardView::id);
    if (found != impl_->cards.end()) {
        *found = std::move(card);
    } else {
        impl_->cards.push_back(std::move(card));
        impl_->selectedCard = impl_->cards.size() - 1;
    }
    impl_->clampArchiveOffset();
    impl_->clampCardEditorOffset();
    if (impl_->window != nullptr) InvalidateRect(impl_->window, nullptr, FALSE);
}

void WindowsSettingsHost::show() {
    ShowWindow(impl_->window, SW_SHOWNORMAL);
    ShowWindow(impl_->window, SW_RESTORE);
    SetWindowPos(impl_->window, HWND_TOP, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    BringWindowToTop(impl_->window);
    SetForegroundWindow(impl_->window);
    SetActiveWindow(impl_->window);
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

void WindowsSettingsHost::setApplicationSortChangedCallback(
    ApplicationSortChangedCallback callback) {
    impl_->applicationSortChanged = std::move(callback);
}

void WindowsSettingsHost::setMappingSortChangedCallback(
    MappingSortChangedCallback callback) {
    impl_->mappingSortChanged = std::move(callback);
}

void WindowsSettingsHost::setMappingModeChangedCallback(
    MappingModeChangedCallback callback) {
    impl_->mappingModeChanged = std::move(callback);
}

void WindowsSettingsHost::setChromeChangedCallback(ChromeChangedCallback callback) {
    impl_->chromeChanged = std::move(callback);
}

void WindowsSettingsHost::setTodoPreferencesChangedCallback(TodoPreferencesChangedCallback callback) {
    impl_->todoPreferencesChanged = std::move(callback);
}

void WindowsSettingsHost::setRestoreArchivedCallback(RestoreArchivedCallback callback) {
    impl_->restoreArchived = std::move(callback);
}

void WindowsSettingsHost::setRestoreArchivedItemCallback(RestoreArchivedItemCallback callback) {
    impl_->restoreArchivedItem = std::move(callback);
}

void WindowsSettingsHost::setDeleteArchivedItemCallback(DeleteArchivedItemCallback callback) {
    impl_->deleteArchivedItem = std::move(callback);
}

void WindowsSettingsHost::setArchiveTodoItemCallback(ArchiveTodoItemCallback callback) {
    impl_->archiveTodoItem = std::move(callback);
}

void WindowsSettingsHost::setHistoricalArchiveAddedCallback(
    HistoricalArchiveAddedCallback callback) {
    impl_->historicalArchiveAdded = std::move(callback);
}

void WindowsSettingsHost::setArchiveExportCallback(ArchiveExportCallback callback) {
    impl_->archiveExport = std::move(callback);
}

void WindowsSettingsHost::setCardAddedCallback(CardAddedCallback callback) {
    impl_->cardAdded = std::move(callback);
}

void WindowsSettingsHost::setCardDeletedCallback(CardDeletedCallback callback) {
    impl_->cardDeleted = std::move(callback);
}

void WindowsSettingsHost::setCardRenamedCallback(CardRenamedCallback callback) {
    impl_->cardRenamed = std::move(callback);
}

void WindowsSettingsHost::setCardVisibilityChangedCallback(
    CardVisibilityChangedCallback callback) {
    impl_->cardVisibilityChanged = std::move(callback);
}

void WindowsSettingsHost::setCardOrderChangedCallback(CardOrderChangedCallback callback) {
    impl_->cardOrderChanged = std::move(callback);
}

void WindowsSettingsHost::setGlobalCornerRadiusChangedCallback(
    GlobalCornerRadiusChangedCallback callback) {
    impl_->globalCornerRadiusChanged = std::move(callback);
}

void WindowsSettingsHost::setTimeZoneChangedCallback(TimeZoneChangedCallback callback) {
    impl_->timeZoneChanged = std::move(callback);
}

void WindowsSettingsHost::setLanguageChangedCallback(LanguageChangedCallback callback) {
    impl_->languageChanged = std::move(callback);
}

void WindowsSettingsHost::setStorageRootChangedCallback(StorageRootChangedCallback callback) {
    impl_->storageRootChanged = std::move(callback);
}

void WindowsSettingsHost::setRunAtStartupChangedCallback(RunAtStartupChangedCallback callback) {
    impl_->runAtStartupChanged = std::move(callback);
}

void WindowsSettingsHost::setDesktopDoubleClickActionChangedCallback(
    DesktopDoubleClickActionChangedCallback callback) {
    impl_->desktopDoubleClickActionChanged = std::move(callback);
}

void WindowsSettingsHost::setTaskbarDoubleClickActionChangedCallback(
    TaskbarDoubleClickActionChangedCallback callback) {
    impl_->taskbarDoubleClickActionChanged = std::move(callback);
}

void WindowsSettingsHost::setRestoreWindowsOnNewWindowChangedCallback(
    RestoreWindowsOnNewWindowChangedCallback) {
    // Compatibility no-op: setting is intentionally no longer exposed.
}


void WindowsSettingsHost::setPinnedCardsYieldToFullscreenChangedCallback(
    PinnedCardsYieldToFullscreenChangedCallback callback) {
    impl_->pinnedCardsYieldToFullscreenChanged = std::move(callback);
}

void WindowsSettingsHost::setIconBackgroundFrameChangedCallback(
    IconBackgroundFrameChangedCallback callback) {
    impl_->iconBackgroundFrameChanged = std::move(callback);
}

void WindowsSettingsHost::setFileDeletionConfirmationChangedCallback(
    FileDeletionConfirmationChangedCallback callback) {
    impl_->fileDeletionConfirmationChanged = std::move(callback);
}

void WindowsSettingsHost::setUpdateChannelChangedCallback(UpdateChannelChangedCallback callback) {
    impl_->updateChannelChanged = std::move(callback);
}

} // namespace desto::platform::windows
