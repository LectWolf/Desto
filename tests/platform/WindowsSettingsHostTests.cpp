#include "TestSupport.h"
#include "WindowsSettingsHost.h"

#include <Windows.h>
#include <windowsx.h>

#include <vector>

using namespace desto::domain;
using namespace desto::platform::windows;
using namespace desto::presentation;

namespace {

void Click(HWND window, int x, int y) {
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
}

void RunTests() {
    WindowsSettingsHost host(L"Desto Settings Test");
    std::vector<CardView> cards{
        {.id = "application-card", .type = CardType::Application,
         .appearancePreset = "white"},
        {.id = "todo-card", .type = CardType::Todo,
         .appearancePreset = "transparent-black",
         .todoItems = {{.id = "archived", .title = "Archived", .archived = true}}},
    };
    bool appearanceChanged = false;
    bool contentChanged = false;
    bool todoPreferencesChanged = false;
    bool restored = false;
    host.setAppearanceChangedCallback(
        [&](const CardId& id, const CardAppearancePreferences& preferences) {
            DESTO_CHECK(id == "application-card");
            appearanceChanged = preferences.preset == "jewel";
            return appearanceChanged;
        });
    host.setContentChangedCallback(
        [&](const CardId& id, const CardContentPreferences& preferences) {
            DESTO_CHECK(id == "application-card");
            contentChanged = preferences.itemSize == CardItemSize::ExtraLarge;
            return contentChanged;
        });
    host.setTodoPreferencesChangedCallback(
        [&](const CardId& id, const TodoCardPreferences& preferences) {
            DESTO_CHECK(id == "todo-card");
            todoPreferencesChanged = preferences.showCreatedTime;
            return todoPreferencesChanged;
        });
    host.setRestoreArchivedCallback([&](const CardId& id) {
        DESTO_CHECK(id == "todo-card");
        restored = true;
        return true;
    });
    host.present(cards);
    host.show();
    const auto window = static_cast<HWND>(host.nativeHandle());
    DESTO_CHECK(window != nullptr);
    DESTO_CHECK(IsWindowVisible(window));

    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(420, 190));
    DESTO_CHECK(!appearanceChanged);
    SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(520, 190));
    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(520, 190));
    DESTO_CHECK(!appearanceChanged);
    Click(window, 420, 190);
    DESTO_CHECK(appearanceChanged);

    Click(window, 510, 312);
    DESTO_CHECK(contentChanged);

    Click(window, 90, 182);
    Click(window, 500, 316);
    DESTO_CHECK(todoPreferencesChanged);
    Click(window, 330, 386);
    DESTO_CHECK(restored);

    SendMessageW(window, WM_CLOSE, 0, 0);
    DESTO_CHECK(!IsWindowVisible(window));
}

} // namespace

int main() {
    return desto::test::Run(RunTests);
}
