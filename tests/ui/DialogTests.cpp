#include "Dialog.h"
#include "RecoveryView.h"
#include "TestSupport.h"
#include "TextInputModel.h"

using namespace desto::ui;

namespace {

Dialog MakeDialog(bool cancellable = true) {
    return Dialog({L"Title", L"Message", L"Confirm", cancellable ? L"Cancel" : L""});
}

void RunTests() {
    auto dialog = MakeDialog();
    dialog.layout({470, 250});
    const auto initial = dialog.snapshot();
    DESTO_CHECK((initial.confirmButton.bounds == Rect{352, 194, 96, 36}));
    DESTO_CHECK(initial.cancelButton.has_value());
    DESTO_CHECK((initial.cancelButton->bounds == Rect{246, 194, 96, 36}));
    DESTO_CHECK(initial.confirmButton.focused);
    DESTO_CHECK(!initial.cancelButton->focused);
    DESTO_CHECK(dialog.pointerReleased({400, 210}) == DialogAction::Confirm);
    DESTO_CHECK(dialog.pointerReleased({280, 210}) == DialogAction::Cancel);
    DESTO_CHECK(!dialog.pointerReleased({20, 20}).has_value());

    DESTO_CHECK(!dialog.keyPressed(DialogKey::Tab).has_value());
    const auto cancelFocused = dialog.snapshot();
    DESTO_CHECK(cancelFocused.cancelButton->focused);
    DESTO_CHECK(dialog.keyPressed(DialogKey::Enter) == DialogAction::Cancel);
    DESTO_CHECK(dialog.keyPressed(DialogKey::Escape) == DialogAction::Cancel);

    auto alert = MakeDialog(false);
    alert.layout({470, 250});
    DESTO_CHECK(!alert.snapshot().cancelButton.has_value());
    DESTO_CHECK(!alert.keyPressed(DialogKey::Tab).has_value());
    DESTO_CHECK(alert.keyPressed(DialogKey::Enter) == DialogAction::Confirm);

    const auto backup = RecoveryView::backupAvailable(L"settings.json.bak1");
    DESTO_CHECK(backup.message.find(L"settings.json.bak1") != std::wstring::npos);
    DESTO_CHECK(!backup.cancelLabel.empty());
    const auto empty = RecoveryView::noUsableConfiguration();
    DESTO_CHECK(empty.confirmLabel == L"创建空 Workspace");

    TextInputModel input(7, L"A\U0001F680B");
    DESTO_CHECK(input.text() == L"A\U0001F680B");
    DESTO_CHECK((input.selection() == TextSelection{4, 4}));
    input.move(TextMovement::PreviousCharacter, false);
    DESTO_CHECK((input.selection() == TextSelection{3, 3}));
    input.move(TextMovement::PreviousCharacter, true);
    DESTO_CHECK((input.selection() == TextSelection{3, 1}));
    const auto replacement = input.replaceSelection(L"xy", true);
    DESTO_CHECK(input.text() == L"AxyB");
    DESTO_CHECK((input.selection() == TextSelection{3, 3}));
    DESTO_CHECK((replacement == TextChange{1, 3, 3}));
    DESTO_CHECK((input.undo() == TextChange{0, 4, 4}));
    DESTO_CHECK(input.text() == L"A\U0001F680B");
    DESTO_CHECK((input.selection() == TextSelection{3, 1}));

    input.setSelection(1, 3);
    DESTO_CHECK((input.replaceSelection(L"123456789", false)
        == TextChange{1, 3, 6}));
    DESTO_CHECK(input.text() == L"A12345B");
    DESTO_CHECK((input.selection() == TextSelection{6, 6}));

    (void)input.setText(L"0123456789");
    DESTO_CHECK(input.text() == L"0123456");
    DESTO_CHECK((input.selection() == TextSelection{7, 7}));

    TextInputModel words(32, L"one  two");
    words.move(TextMovement::PreviousWord, false);
    DESTO_CHECK((words.selection() == TextSelection{5, 5}));
    words.move(TextMovement::PreviousWord, false);
    DESTO_CHECK((words.selection() == TextSelection{0, 0}));
    words.move(TextMovement::NextWord, true);
    DESTO_CHECK((words.selection() == TextSelection{0, 5}));
    words.move(TextMovement::NextCharacter, false);
    DESTO_CHECK((words.selection() == TextSelection{5, 5}));
    DESTO_CHECK(!TextInputModel(4).undo().has_value());
}

} // namespace

int main() {
    return desto::test::Run(RunTests);
}
