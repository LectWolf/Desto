#pragma once

#include <string_view>

namespace desto::platform::windows {

inline constexpr std::wstring_view WindowsFluentIconFont = L"Segoe Fluent Icons";
inline constexpr std::wstring_view WindowsMdl2IconFont = L"Segoe MDL2 Assets";

[[nodiscard]] std::wstring_view ResolveWindowsIconFontFamily(
    bool fluentAvailable,
    bool mdl2Available) noexcept;

[[nodiscard]] bool WindowsFontFamilyAvailable(std::wstring_view family) noexcept;
[[nodiscard]] std::wstring_view WindowsIconFontFamily() noexcept;

} // namespace desto::platform::windows
