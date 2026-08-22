#include "WindowsIconFont.h"
#include "TestSupport.h"

#include <Windows.h>

#include <array>

using namespace desto::platform::windows;

namespace {

void RunTests() {
    DESTO_CHECK(ResolveWindowsIconFontFamily(true, true) == WindowsFluentIconFont);
    DESTO_CHECK(ResolveWindowsIconFontFamily(false, true) == WindowsMdl2IconFont);
    DESTO_CHECK(WindowsFontFamilyAvailable(WindowsMdl2IconFont));
    DESTO_CHECK(!WindowsFontFamilyAvailable(L"Desto Definitely Missing Font"));

    const auto dc = CreateCompatibleDC(nullptr);
    DESTO_CHECK(dc != nullptr);
    const auto font = CreateFontW(
        -16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        ResolveWindowsIconFontFamily(false, true).data());
    DESTO_CHECK(font != nullptr);
    const auto previous = SelectObject(dc, font);
    const std::array<wchar_t, 4> criticalGlyphs{
        L'\uE70F', // Rename
        L'\uE712', // More
        L'\uE74D', // Delete
        L'\uE70E', // Collapse
    };
    std::array<WORD, criticalGlyphs.size()> indices{};
    DESTO_CHECK(GetGlyphIndicesW(
        dc,
        criticalGlyphs.data(),
        static_cast<int>(criticalGlyphs.size()),
        indices.data(),
        GGI_MARK_NONEXISTING_GLYPHS) != GDI_ERROR);
    for (const auto index : indices) DESTO_CHECK(index != 0xFFFFu);
    SelectObject(dc, previous);
    DeleteObject(font);
    DeleteDC(dc);
}

} // namespace

int main() {
    return desto::test::Run(RunTests);
}
