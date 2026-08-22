#include "TestSupport.h"
#include "WindowsFileDragDrop.h"

#include <Windows.h>

#include <filesystem>
#include <vector>

using namespace desto::platform::windows;

namespace {

void RunTests() {
    DESTO_CHECK(SUCCEEDED(OleInitialize(nullptr)));
    const std::vector<std::filesystem::path> paths{
        L"C:\\Desto Drag Test\\First.lnk",
        L"C:\\Desto Drag Test\\Second.txt",
    };
    auto* data = CreateFileDataObject(paths, "application-1");
    DESTO_CHECK(data != nullptr);

    bool entered = false;
    bool left = false;
    bool dropped = false;
    DWORD expectedKeyState = MK_LBUTTON;
    std::optional<std::string> expectedSourceCardId = "application-1";
    auto* target = CreateFileDropTarget({
        .dragOver = [&] (POINTL point,
                        DWORD allowed,
                        DWORD keyState,
                        const std::optional<std::string>& sourceCardId) {
            entered = true;
            DESTO_CHECK(point.x == 140);
            DESTO_CHECK(point.y == 220);
            DESTO_CHECK((allowed & DROPEFFECT_MOVE) != 0);
            DESTO_CHECK(keyState == expectedKeyState);
            DESTO_CHECK(sourceCardId == expectedSourceCardId);
            return static_cast<DWORD>((keyState & MK_CONTROL) != 0
                ? DROPEFFECT_COPY
                : DROPEFFECT_MOVE);
        },
        .dragLeave = [&] { left = true; },
        .drop = [&](std::vector<std::filesystem::path> received,
                    std::optional<std::string> sourceCardId,
                    POINTL,
                    DWORD,
                    DWORD keyState) {
            DESTO_CHECK(received == paths);
            DESTO_CHECK(sourceCardId == expectedSourceCardId);
            DESTO_CHECK(keyState == expectedKeyState);
            dropped = true;
            return static_cast<DWORD>((keyState & MK_CONTROL) != 0
                ? DROPEFFECT_COPY
                : DROPEFFECT_MOVE);
        },
    });
    DESTO_CHECK(target != nullptr);

    DWORD effect = DROPEFFECT_MOVE | DROPEFFECT_COPY;
    DESTO_CHECK(target->DragEnter(data, MK_LBUTTON, {140, 220}, &effect) == S_OK);
    DESTO_CHECK(entered);
    DESTO_CHECK(effect == DROPEFFECT_MOVE);
    DESTO_CHECK(target->DragLeave() == S_OK);
    DESTO_CHECK(left);

    effect = DROPEFFECT_MOVE | DROPEFFECT_COPY;
    DESTO_CHECK(target->DragEnter(data, MK_LBUTTON, {140, 220}, &effect) == S_OK);
    DESTO_CHECK(target->Drop(data, MK_LBUTTON, {140, 220}, &effect) == S_OK);
    DESTO_CHECK(dropped);
    DESTO_CHECK(effect == DROPEFFECT_MOVE);
    DESTO_CHECK(WasFileDropHandledByDesto(data));
    DESTO_CHECK(PerformedFileDropEffect(data) == DROPEFFECT_MOVE);

    auto* externalData = CreateFileDataObject(paths);
    DESTO_CHECK(externalData != nullptr);
    expectedSourceCardId.reset();
    expectedKeyState = MK_LBUTTON | MK_CONTROL;
    effect = DROPEFFECT_MOVE | DROPEFFECT_COPY;
    DESTO_CHECK(target->DragEnter(
        externalData, expectedKeyState, {140, 220}, &effect) == S_OK);
    DESTO_CHECK(target->Drop(
        externalData, expectedKeyState, {140, 220}, &effect) == S_OK);
    DESTO_CHECK(!WasFileDropHandledByDesto(externalData));
    DESTO_CHECK(effect == DROPEFFECT_COPY);
    DESTO_CHECK(PerformedFileDropEffect(externalData) == DROPEFFECT_COPY);

    auto* copyOnlyData = CreateFileDataObject(paths, "mapping-references", false);
    DESTO_CHECK(copyOnlyData != nullptr);
    auto preferred = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(
        L"Preferred DropEffect"));
    FORMATETC preferredFormat{
        .cfFormat = preferred,
        .ptd = nullptr,
        .dwAspect = DVASPECT_CONTENT,
        .lindex = -1,
        .tymed = TYMED_HGLOBAL,
    };
    STGMEDIUM preferredMedium{};
    DESTO_CHECK(copyOnlyData->GetData(&preferredFormat, &preferredMedium) == S_OK);
    const auto* preferredEffect = static_cast<const DWORD*>(
        GlobalLock(preferredMedium.hGlobal));
    DESTO_CHECK(preferredEffect != nullptr);
    if (preferredEffect != nullptr) {
        DESTO_CHECK(*preferredEffect == DROPEFFECT_COPY);
        GlobalUnlock(preferredMedium.hGlobal);
    }
    ReleaseStgMedium(&preferredMedium);
    copyOnlyData->Release();

    auto* internalOnlyData = CreateFileDataObject(
        paths, "mapping-reference-root", true, false);
    DESTO_CHECK(internalOnlyData != nullptr);
    FORMATETC shellFileFormat{
        .cfFormat = static_cast<CLIPFORMAT>(CF_HDROP),
        .ptd = nullptr,
        .dwAspect = DVASPECT_CONTENT,
        .lindex = -1,
        .tymed = TYMED_HGLOBAL,
    };
    DESTO_CHECK(internalOnlyData->QueryGetData(&shellFileFormat) != S_OK);
    expectedSourceCardId = "mapping-reference-root";
    expectedKeyState = MK_LBUTTON;
    effect = DROPEFFECT_MOVE | DROPEFFECT_COPY;
    DESTO_CHECK(target->DragEnter(
        internalOnlyData, MK_LBUTTON, {140, 220}, &effect) == S_OK);
    DESTO_CHECK(target->Drop(
        internalOnlyData, MK_LBUTTON, {140, 220}, &effect) == S_OK);
    DESTO_CHECK(dropped);
    internalOnlyData->Release();

    target->Release();
    externalData->Release();
    data->Release();
    OleUninitialize();
}

} // namespace

int main() {
    return desto::test::Run(RunTests);
}
