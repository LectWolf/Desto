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
    auto* data = CreateFileDataObject(paths);
    DESTO_CHECK(data != nullptr);

    bool entered = false;
    bool left = false;
    bool dropped = false;
    auto* target = CreateFileDropTarget({
        .dragOver = [&](POINTL point, DWORD allowed) {
            entered = true;
            DESTO_CHECK(point.x == 140);
            DESTO_CHECK(point.y == 220);
            DESTO_CHECK((allowed & DROPEFFECT_MOVE) != 0);
            return static_cast<DWORD>(DROPEFFECT_MOVE);
        },
        .dragLeave = [&] { left = true; },
        .drop = [&](std::vector<std::filesystem::path> received, POINTL, DWORD) {
            DESTO_CHECK(received == paths);
            dropped = true;
            return static_cast<DWORD>(DROPEFFECT_MOVE);
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
    DESTO_CHECK(target->Drop(data, 0, {140, 220}, &effect) == S_OK);
    DESTO_CHECK(dropped);
    DESTO_CHECK(effect == DROPEFFECT_MOVE);

    target->Release();
    data->Release();
    OleUninitialize();
}

} // namespace

int main() {
    return desto::test::Run(RunTests);
}
