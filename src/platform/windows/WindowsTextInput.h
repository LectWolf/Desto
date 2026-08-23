#pragma once

#include <Windows.h>

#include <cstddef>
#include <string>

namespace desto::platform::windows {

constexpr WORD WindowsTextInputCommitNotification = 0x501;
constexpr WORD WindowsTextInputCancelNotification = 0x502;
constexpr WORD WindowsTextInputTabForwardNotification = 0x503;
constexpr WORD WindowsTextInputTabBackwardNotification = 0x504;

struct WindowsTextInputStyle {
    COLORREF background = RGB(243, 243, 243);
    COLORREF outline = RGB(207, 210, 216);
    COLORREF focusedOutline = RGB(90, 153, 235);
    COLORREF text = RGB(45, 49, 57);
    COLORREF placeholder = RGB(112, 117, 126);
    COLORREF selection = RGB(70, 118, 196);
    COLORREF compositionUnderline = RGB(45, 110, 205);
    BYTE backgroundAlpha = 255;
    BYTE outlineAlpha = 255;
    float cornerRadius = 8.0F;
    float outlineWidth = 1.0F;
    float paddingLeft = 10.0F;
    float paddingRight = 10.0F;
    float fontSize = 13.0F;
    int fontWeight = 400;
    std::wstring fontFamily = L"Segoe UI Variable Text";
    std::wstring leadingGlyph;
    float leadingGlyphSize = 14.0F;
    float leadingGlyphWidth = 30.0F;
    bool centered = false;
};

struct WindowsTextInputCreateInfo {
    HWND notificationWindow = nullptr;
    UINT controlId = 0;
    RECT bounds{};
    bool popup = false;
    bool commitOnFocusLoss = true;
    std::size_t maximumLength = 512;
    std::wstring text;
    std::wstring placeholder;
    WindowsTextInputStyle style;
};

struct WindowsTextInputRenderStatistics {
    std::size_t nonTransparentPixels = 0;
    std::size_t coloredPixels = 0;
    std::size_t paints = 0;
    std::size_t backingStoreCreations = 0;
    std::size_t renderTargetCreations = 0;
};

// Creates one custom text-input HWND. It owns text, selection, TSF state and
// all visible rendering; no native EDIT or auxiliary paint host is created.
[[nodiscard]] HWND CreateWindowsTextInput(
    const WindowsTextInputCreateInfo& createInfo) noexcept;

[[nodiscard]] bool IsWindowsTextInput(HWND window) noexcept;
[[nodiscard]] std::wstring WindowsTextInputText(HWND window);
void SetWindowsTextInputText(HWND window, std::wstring text) noexcept;
void SetWindowsTextInputSelection(HWND window, std::size_t start, std::size_t end) noexcept;
[[nodiscard]] std::pair<std::size_t, std::size_t>
WindowsTextInputSelection(HWND window) noexcept;
[[nodiscard]] WindowsTextInputRenderStatistics
WindowsTextInputRenderStats(HWND window) noexcept;
void SetWindowsTextInputBounds(HWND window, RECT bounds) noexcept;
void SetWindowsTextInputStyle(HWND window, WindowsTextInputStyle style) noexcept;
void SetWindowsTextInputPlaceholder(HWND window, std::wstring placeholder) noexcept;
void FocusWindowsTextInput(HWND window) noexcept;

} // namespace desto::platform::windows
