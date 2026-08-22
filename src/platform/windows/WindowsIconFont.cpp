#include "WindowsIconFont.h"

#include <Windows.h>

#include <cwchar>
#include <string>

namespace desto::platform::windows {
namespace {

struct FontFamilyQuery {
    const wchar_t* requested = nullptr;
    bool found = false;
};

int CALLBACK FindExactFontFamily(
    const LOGFONTW* font,
    const TEXTMETRICW*,
    DWORD,
    LPARAM parameter) noexcept {
    auto& query = *reinterpret_cast<FontFamilyQuery*>(parameter);
    query.found = _wcsicmp(font->lfFaceName, query.requested) == 0;
    return query.found ? 0 : 1;
}

} // namespace

std::wstring_view ResolveWindowsIconFontFamily(
    bool fluentAvailable,
    bool mdl2Available) noexcept {
    if (fluentAvailable) return WindowsFluentIconFont;
    if (mdl2Available) return WindowsMdl2IconFont;
    return L"Segoe UI Symbol";
}

bool WindowsFontFamilyAvailable(std::wstring_view family) noexcept {
    if (family.empty() || family.size() >= LF_FACESIZE) return false;
    const auto dc = GetDC(nullptr);
    if (dc == nullptr) return false;
    LOGFONTW query{};
    query.lfCharSet = DEFAULT_CHARSET;
    const std::wstring requested{family};
    wcsncpy_s(query.lfFaceName, requested.c_str(), _TRUNCATE);
    FontFamilyQuery result{requested.c_str(), false};
    EnumFontFamiliesExW(
        dc,
        &query,
        &FindExactFontFamily,
        reinterpret_cast<LPARAM>(&result),
        0);
    ReleaseDC(nullptr, dc);
    return result.found;
}

std::wstring_view WindowsIconFontFamily() noexcept {
    static const auto family = ResolveWindowsIconFontFamily(
        WindowsFontFamilyAvailable(WindowsFluentIconFont),
        WindowsFontFamilyAvailable(WindowsMdl2IconFont));
    return family;
}

} // namespace desto::platform::windows
