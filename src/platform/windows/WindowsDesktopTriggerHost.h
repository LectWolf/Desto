#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <Windows.h>

namespace desto::platform::windows {

[[nodiscard]] bool ShouldCaptureDesktopSessionWindow(
    HWND window, HMONITOR monitor = nullptr) noexcept;
[[nodiscard]] bool ShouldRestoreDesktopSessionOnForeground(
    bool allDisplaysSession,
    bool sessionActive,
    bool transitioning,
    bool restoreOnForeground,
    bool candidateWasCaptured,
    bool candidateIsIconic) noexcept;

enum class DesktopSessionToggleAction {
    BeginSession,
    HideExposedWindows,
    RestoreSession,
};

[[nodiscard]] DesktopSessionToggleAction ResolveDesktopSessionToggleAction(
    bool sessionActive,
    bool desktopVisible) noexcept;

[[nodiscard]] bool IsBlankTaskbarAccessibilityTarget(
    bool querySucceeded,
    long role,
    bool directTaskbarHit) noexcept;

// Restores the captured visible state while retaining maximized windows.
[[nodiscard]] bool RestoreCapturedWindowPlacement(
    HWND window, const WINDOWPLACEMENT& captured) noexcept;

class DesktopDoubleClickDetector final {
public:
    DesktopDoubleClickDetector(
        std::uint32_t maximumDelay,
        int maximumWidth,
        int maximumHeight) noexcept;

    [[nodiscard]] bool registerClick(
        int screenX,
        int screenY,
        std::uint32_t timestamp,
        bool desktopBackground) noexcept;
    void reset() noexcept;

private:
    std::uint32_t maximumDelay_ = 0;
    int maximumWidth_ = 0;
    int maximumHeight_ = 0;
    bool hasFirstClick_ = false;
    int lastScreenX_ = 0;
    int lastScreenY_ = 0;
    std::uint32_t lastTimestamp_ = 0;
};

class WindowsDesktopTriggerHost final {
public:
    using Callback = std::function<void()>;
    using TaskbarCallback = std::function<void(int screenX, int screenY)>;

    WindowsDesktopTriggerHost();
    ~WindowsDesktopTriggerHost();

    WindowsDesktopTriggerHost(const WindowsDesktopTriggerHost&) = delete;
    WindowsDesktopTriggerHost& operator=(const WindowsDesktopTriggerHost&) = delete;

    void setDoubleClickCallback(Callback callback);
    void setTaskbarDoubleClickCallback(TaskbarCallback callback);
    [[nodiscard]] std::uint32_t hookThreadId() const noexcept;
    [[nodiscard]] bool desktopIconsVisible() const noexcept;
    void setDesktopIconsVisible(bool visible);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class WindowsTaskbarWindowToggle final {
public:
    WindowsTaskbarWindowToggle();
    ~WindowsTaskbarWindowToggle();

    WindowsTaskbarWindowToggle(const WindowsTaskbarWindowToggle&) = delete;
    WindowsTaskbarWindowToggle& operator=(const WindowsTaskbarWindowToggle&) = delete;

    void setRestoreOnNewWindow(bool enabled) noexcept;
    void toggle(int screenX, int screenY, bool currentDisplayOnly);
    void showDesktop(int screenX, int screenY, bool currentDisplayOnly);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace desto::platform::windows
