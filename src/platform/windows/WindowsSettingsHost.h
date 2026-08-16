#pragma once

#include <functional>
#include <memory>
#include <span>
#include <string>

#include "CardView.h"

namespace desto::platform::windows {

class WindowsSettingsHost final {
public:
    using AppearanceChangedCallback = std::function<bool(
        const domain::CardId&, const domain::CardAppearancePreferences&)>;
    using ContentChangedCallback = std::function<bool(
        const domain::CardId&, const domain::CardContentPreferences&)>;
    using ChromeChangedCallback = std::function<bool(
        const domain::CardId&, const domain::CardChromePreferences&)>;
    using TodoPreferencesChangedCallback = std::function<bool(
        const domain::CardId&, const domain::TodoCardPreferences&)>;
    using RestoreArchivedCallback = std::function<bool(const domain::CardId&)>;

    explicit WindowsSettingsHost(std::wstring title = L"Desto");
    ~WindowsSettingsHost();

    WindowsSettingsHost(const WindowsSettingsHost&) = delete;
    WindowsSettingsHost& operator=(const WindowsSettingsHost&) = delete;

    void present(std::span<const presentation::CardView> cards);
    void show();
    void hide() noexcept;
    [[nodiscard]] void* nativeHandle() const noexcept;

    void setAppearanceChangedCallback(AppearanceChangedCallback callback);
    void setContentChangedCallback(ContentChangedCallback callback);
    void setChromeChangedCallback(ChromeChangedCallback callback);
    void setTodoPreferencesChangedCallback(TodoPreferencesChangedCallback callback);
    void setRestoreArchivedCallback(RestoreArchivedCallback callback);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace desto::platform::windows
