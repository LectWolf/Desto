#pragma once

#include <functional>
#include <memory>
#include <cstdint>
#include <string>

namespace desto::platform::windows {

[[nodiscard]] std::uint32_t ResolveDestoTrayIconFlags() noexcept;

class WindowsTrayHost final {
public:
    using Callback = std::function<void()>;

    explicit WindowsTrayHost();
    ~WindowsTrayHost();

    WindowsTrayHost(const WindowsTrayHost&) = delete;
    WindowsTrayHost& operator=(const WindowsTrayHost&) = delete;

    void setOpenSettingsCallback(Callback callback);
    void setToggleDesktopCallback(Callback callback);
    void setExitCallback(Callback callback);
    void setDesktopVisible(bool visible) noexcept;
    void setLanguage(std::string language);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace desto::platform::windows
