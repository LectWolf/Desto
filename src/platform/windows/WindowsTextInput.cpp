#include "WindowsTextInput.h"
#include "WindowsIconFont.h"
#include "WindowsPopupMenu.h"
#include "TextInputModel.h"

#include <msctf.h>
#include <textstor.h>
#include <usp10.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#undef max
#undef min

namespace desto::platform::windows {
namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kTextInputClassName[] = L"DestoWindowsTextInput";
constexpr UINT_PTR kCaretTimerId = 1;
constexpr UINT kCaretIntervalMilliseconds = 530;
constexpr UINT kDeferredRedrawMessage = WM_APP + 0x31;
constexpr UINT kUndoCommand = 42'001;
constexpr UINT kCutCommand = 42'002;
constexpr UINT kCopyCommand = 42'003;
constexpr UINT kPasteCommand = 42'004;
constexpr UINT kSelectAllCommand = 42'005;
constexpr TsViewCookie kViewCookie = 1;

struct TsfThreadState {
    ComPtr<ITfThreadMgr> manager;
    ComPtr<ITfKeystrokeMgr> keystrokes;
    TfClientId clientId = TF_CLIENTID_NULL;
    bool initializedCom = false;

    TsfThreadState() noexcept {
        const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        initializedCom = initialized == S_OK || initialized == S_FALSE;
        if (FAILED(CoCreateInstance(
                CLSID_TF_ThreadMgr,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(manager.GetAddressOf())))) {
            return;
        }
        if (FAILED(manager->Activate(&clientId))) {
            manager.Reset();
            clientId = TF_CLIENTID_NULL;
            return;
        }
        (void)manager.As(&keystrokes);
    }

    ~TsfThreadState() {
        if (manager != nullptr && clientId != TF_CLIENTID_NULL) manager->Deactivate();
        keystrokes.Reset();
        manager.Reset();
        if (initializedCom) CoUninitialize();
    }
};

TsfThreadState& ThreadTsf() noexcept {
    thread_local TsfThreadState state;
    return state;
}

struct TextLayout {
    HDC dc = nullptr;
    HFONT font = nullptr;
    HGDIOBJ previousFont = nullptr;
    SCRIPT_STRING_ANALYSIS analysis = nullptr;
    SIZE size{};

    TextLayout() = default;
    TextLayout(const TextLayout&) = delete;
    TextLayout& operator=(const TextLayout&) = delete;
    ~TextLayout() {
        if (analysis != nullptr) ScriptStringFree(&analysis);
        if (dc != nullptr && previousFont != nullptr) SelectObject(dc, previousFont);
        if (font != nullptr) DeleteObject(font);
        if (dc != nullptr) DeleteDC(dc);
    }
};

std::unique_ptr<TextLayout> CreateLayout(
    std::wstring_view text,
    const WindowsTextInputStyle& style,
    int width) noexcept {
    if (width <= 0 || text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return nullptr;
    }
    auto result = std::make_unique<TextLayout>();
    result->dc = CreateCompatibleDC(nullptr);
    result->font = CreateFontW(
        -std::max(1, static_cast<int>(std::lround(style.fontSize))),
        0, 0, 0, style.fontWeight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        style.fontFamily.c_str());
    if (result->dc == nullptr || result->font == nullptr) return nullptr;
    result->previousFont = SelectObject(result->dc, result->font);
    SCRIPT_CONTROL control{};
    SCRIPT_STATE state{};
    if (FAILED(ScriptStringAnalyse(
            result->dc,
            text.data(),
            static_cast<int>(text.size()),
            0,
            -1,
            SSA_GLYPHS | SSA_FALLBACK | SSA_LINK,
            width,
            &control,
            &state,
            nullptr,
            nullptr,
            nullptr,
            &result->analysis))) {
        return nullptr;
    }
    if (const auto* measured = ScriptString_pSize(result->analysis); measured != nullptr) {
        result->size = *measured;
    }
    return result;
}

std::size_t PreviousCodePoint(std::wstring_view text, std::size_t position) noexcept {
    if (position == 0) return 0;
    auto result = position - 1;
    if (result > 0 && text[result] >= 0xDC00 && text[result] <= 0xDFFF
        && text[result - 1] >= 0xD800 && text[result - 1] <= 0xDBFF) {
        --result;
    }
    return result;
}

std::size_t NextCodePoint(std::wstring_view text, std::size_t position) noexcept {
    if (position >= text.size()) return text.size();
    auto result = position + 1;
    if (result < text.size() && text[position] >= 0xD800 && text[position] <= 0xDBFF
        && text[result] >= 0xDC00 && text[result] <= 0xDFFF) {
        ++result;
    }
    return result;
}

class TextInputState final : public ITextStoreACP,
                             public ITfContextOwnerCompositionSink {
public:
    explicit TextInputState(WindowsTextInputCreateInfo createInfoValue)
        : createInfo(std::move(createInfoValue)),
          model(createInfo.maximumLength, createInfo.text) {}

    ~TextInputState() {
        releaseLayeredBackingStore();
        detachTsf();
    }

    void attach(HWND value) noexcept {
        window = value;
        attachTsf();
    }

    void detachFromWindow() noexcept {
        detachTsf();
        releaseLayeredBackingStore();
        window = nullptr;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }

    ULONG STDMETHODCALLTYPE Release() override {
        const auto next = --references;
        if (next == 0) delete this;
        return next;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** result) override {
        if (result == nullptr) return E_POINTER;
        *result = nullptr;
        if (id == IID_IUnknown || id == IID_ITextStoreACP) {
            *result = static_cast<ITextStoreACP*>(this);
        } else if (id == IID_ITfContextOwnerCompositionSink) {
            *result = static_cast<ITfContextOwnerCompositionSink*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE AdviseSink(
        REFIID id, IUnknown* unknown, DWORD) override {
        if (id != IID_ITextStoreACPSink || unknown == nullptr) return E_INVALIDARG;
        if (sink != nullptr) return E_UNEXPECTED;
        return unknown->QueryInterface(IID_PPV_ARGS(sink.GetAddressOf()));
    }

    HRESULT STDMETHODCALLTYPE UnadviseSink(IUnknown* unknown) override {
        if (sink == nullptr || unknown == nullptr) return E_UNEXPECTED;
        ComPtr<IUnknown> current;
        sink.As(&current);
        if (current.Get() != unknown) return E_UNEXPECTED;
        sink.Reset();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE RequestLock(DWORD flags, HRESULT* session) override {
        if (session == nullptr) return E_INVALIDARG;
        if (locked) {
            if ((flags & TS_LF_SYNC) != 0) {
                *session = TS_E_SYNCHRONOUS;
            } else {
                pendingLockFlags |= flags;
                *session = TS_S_ASYNC;
            }
            return S_OK;
        }
        if (sink == nullptr) {
            *session = E_FAIL;
            return S_OK;
        }
        locked = true;
        lockFlags = flags;
        *session = sink->OnLockGranted(flags);
        lockFlags = 0;
        locked = false;
        while (pendingLockFlags != 0 && sink != nullptr) {
            const auto pending = pendingLockFlags;
            pendingLockFlags = 0;
            locked = true;
            lockFlags = pending;
            (void)sink->OnLockGranted(pending);
            lockFlags = 0;
            locked = false;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetStatus(TS_STATUS* status) override {
        if (status == nullptr) return E_INVALIDARG;
        status->dwDynamicFlags = 0;
        status->dwStaticFlags = TS_SS_NOHIDDENTEXT;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryInsert(
        LONG start, LONG end, ULONG count, LONG* resultStart, LONG* resultEnd) override {
        if (resultStart == nullptr || resultEnd == nullptr) return E_INVALIDARG;
        const auto size = static_cast<LONG>(model.text().size());
        start = std::clamp(start, 0L, size);
        end = std::clamp(end, start, size);
        *resultStart = start;
        const auto available = createInfo.maximumLength > model.text().size() - (end - start)
            ? createInfo.maximumLength - (model.text().size() - (end - start)) : 0;
        *resultEnd = start + static_cast<LONG>((std::min)(
            static_cast<std::size_t>(count), available));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetSelection(
        ULONG index, ULONG count, TS_SELECTION_ACP* selection, ULONG* fetched) override {
        if (selection == nullptr || fetched == nullptr || count == 0) return E_INVALIDARG;
        if (index != TS_DEFAULT_SELECTION && index != 0) return TS_E_NOSELECTION;
        const auto current = model.selection();
        selection[0].acpStart = static_cast<LONG>((std::min)(current.anchor, current.caret));
        selection[0].acpEnd = static_cast<LONG>((std::max)(current.anchor, current.caret));
        selection[0].style.ase = current.caret < current.anchor ? TS_AE_START : TS_AE_END;
        selection[0].style.fInterimChar = FALSE;
        *fetched = 1;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetSelection(
        ULONG count, const TS_SELECTION_ACP* selection) override {
        if (selection == nullptr || count == 0) return E_INVALIDARG;
        const auto start = clampPosition(selection[0].acpStart);
        const auto end = clampPosition(selection[0].acpEnd);
        if (selection[0].style.ase == TS_AE_START) {
            model.setSelection(end, start);
        } else {
            model.setSelection(start, end);
        }
        selectionChanged(false);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetText(
        LONG start, LONG end,
        WCHAR* plain, ULONG plainCapacity, ULONG* plainCount,
        TS_RUNINFO* runs, ULONG runCapacity, ULONG* runCount,
        LONG* next) override {
        if (plainCount == nullptr || runCount == nullptr || next == nullptr) return E_INVALIDARG;
        const auto size = static_cast<LONG>(model.text().size());
        start = std::clamp(start, 0L, size);
        if (end == -1) end = size;
        end = std::clamp(end, start, size);
        const auto requested = static_cast<ULONG>(end - start);
        const auto copied = (std::min)(requested, plainCapacity);
        if (plain != nullptr && copied > 0) {
            std::memcpy(plain, model.text().data() + start, copied * sizeof(WCHAR));
        }
        *plainCount = copied;
        if (runs != nullptr && runCapacity > 0 && copied > 0) {
            runs[0].uCount = copied;
            runs[0].type = TS_RT_PLAIN;
            *runCount = 1;
        } else {
            *runCount = 0;
        }
        *next = start + static_cast<LONG>(copied);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetText(
        DWORD, LONG start, LONG end,
        const WCHAR* value, ULONG count, TS_TEXTCHANGE* change) override {
        return replaceRange(start, end, value, count, change, true);
    }

    HRESULT STDMETHODCALLTYPE GetFormattedText(LONG, LONG, IDataObject**) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetEmbedded(LONG, REFGUID, REFIID, IUnknown**) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE QueryInsertEmbedded(
        const GUID*, const FORMATETC*, BOOL* insertable) override {
        if (insertable == nullptr) return E_INVALIDARG;
        *insertable = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE InsertEmbedded(
        DWORD, LONG, LONG, IDataObject*, TS_TEXTCHANGE*) override {
        return TS_E_FORMAT;
    }

    HRESULT STDMETHODCALLTYPE InsertTextAtSelection(
        DWORD flags, const WCHAR* value, ULONG count,
        LONG* start, LONG* end, TS_TEXTCHANGE* change) override {
        const auto current = model.selection();
        const auto selectionStart = static_cast<LONG>((std::min)(current.anchor, current.caret));
        const auto selectionEnd = static_cast<LONG>((std::max)(current.anchor, current.caret));
        if ((flags & TS_IAS_QUERYONLY) != 0) {
            if (start != nullptr) *start = selectionStart;
            if (end != nullptr) *end = selectionStart + static_cast<LONG>(count);
            if (change != nullptr) {
                change->acpStart = selectionStart;
                change->acpOldEnd = selectionEnd;
                change->acpNewEnd = selectionStart + static_cast<LONG>(count);
            }
            return S_OK;
        }
        const auto result = replaceRange(
            selectionStart, selectionEnd, value, count, change, true);
        if (SUCCEEDED(result)) {
            if (start != nullptr) *start = selectionStart;
            if (end != nullptr) *end = static_cast<LONG>(model.selection().caret);
        }
        return result;
    }

    HRESULT STDMETHODCALLTYPE InsertEmbeddedAtSelection(
        DWORD, IDataObject*, LONG*, LONG*, TS_TEXTCHANGE*) override {
        return TS_E_FORMAT;
    }
    HRESULT STDMETHODCALLTYPE RequestSupportedAttrs(
        DWORD, ULONG, const TS_ATTRID*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE RequestAttrsAtPosition(
        LONG, ULONG, const TS_ATTRID*, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE RequestAttrsTransitioningAtPosition(
        LONG, ULONG, const TS_ATTRID*, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE FindNextAttrTransition(
        LONG start, LONG halt, ULONG, const TS_ATTRID*, DWORD,
        LONG* next, BOOL* found, LONG* offset) override {
        if (next == nullptr || found == nullptr || offset == nullptr) return E_INVALIDARG;
        *next = halt;
        *found = FALSE;
        *offset = start;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RetrieveRequestedAttrs(
        ULONG, TS_ATTRVAL*, ULONG* fetched) override {
        if (fetched == nullptr) return E_INVALIDARG;
        *fetched = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEndACP(LONG* end) override {
        if (end == nullptr) return E_INVALIDARG;
        *end = static_cast<LONG>(model.text().size());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetActiveView(TsViewCookie* view) override {
        if (view == nullptr) return E_INVALIDARG;
        *view = kViewCookie;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetACPFromPoint(
        TsViewCookie view, const POINT* point, DWORD, LONG* position) override {
        if (view != kViewCookie || point == nullptr || position == nullptr) return E_INVALIDARG;
        POINT client = *point;
        ScreenToClient(window, &client);
        *position = static_cast<LONG>(hitTest(client.x, client.y));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTextExt(
        TsViewCookie view, LONG start, LONG end, RECT* rect, BOOL* clipped) override {
        if (view != kViewCookie || rect == nullptr || clipped == nullptr) return E_INVALIDARG;
        const auto first = caretRect(clampPosition(start));
        const auto last = caretRect(clampPosition(end));
        *rect = first;
        rect->right = (std::max)(first.right, last.right);
        MapWindowPoints(window, HWND_DESKTOP, reinterpret_cast<POINT*>(rect), 2);
        *clipped = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetScreenExt(TsViewCookie view, RECT* rect) override {
        if (view != kViewCookie || rect == nullptr) return E_INVALIDARG;
        return GetWindowRect(window, rect) ? S_OK : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE GetWnd(TsViewCookie view, HWND* result) override {
        if (view != kViewCookie || result == nullptr) return E_INVALIDARG;
        *result = window;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnStartComposition(
        ITfCompositionView* composition, BOOL* accepted) override {
        if (accepted == nullptr) return E_INVALIDARG;
        *accepted = TRUE;
        updateCompositionRange(composition);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnUpdateComposition(
        ITfCompositionView* composition, ITfRange*) override {
        updateCompositionRange(composition);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnEndComposition(ITfCompositionView*) override {
        compositionRange.reset();
        redraw();
        return S_OK;
    }

    void setText(std::wstring value, bool notify = true) noexcept {
        try {
            const auto change = model.setText(std::move(value));
            invalidateTextLayout();
            if (notify) {
                notifyTextChange(change.start, change.oldEnd, change.newEnd);
                notifySelectionChange();
                notifyChanged();
            }
            redraw();
        } catch (...) {}
    }

    void setSelection(std::size_t start, std::size_t end) noexcept {
        model.setSelection(start, end);
        selectionChanged(true);
    }

    std::pair<std::size_t, std::size_t> selection() const noexcept {
        const auto current = model.selection();
        return {current.anchor, current.caret};
    }

    void performUndo() noexcept { undo(); }

    void setStyle(WindowsTextInputStyle value) noexcept {
        createInfo.style = std::move(value);
        invalidateAllLayouts();
        redraw();
    }

    bool handleKeyMessage(UINT message, WPARAM key, LPARAM lParam) noexcept {
        auto& tsf = ThreadTsf();
        if (tsf.keystrokes != nullptr) {
            BOOL eaten = FALSE;
            HRESULT result = message == WM_KEYDOWN
                ? tsf.keystrokes->TestKeyDown(static_cast<WPARAM>(key), lParam, &eaten)
                : tsf.keystrokes->TestKeyUp(static_cast<WPARAM>(key), lParam, &eaten);
            if (SUCCEEDED(result) && eaten) {
                eaten = FALSE;
                result = message == WM_KEYDOWN
                    ? tsf.keystrokes->KeyDown(static_cast<WPARAM>(key), lParam, &eaten)
                    : tsf.keystrokes->KeyUp(static_cast<WPARAM>(key), lParam, &eaten);
                if (SUCCEEDED(result) && eaten) {
                    suppressTranslatedCharacter = message == WM_KEYDOWN;
                    return true;
                }
            }
        }
        if (message == WM_KEYUP) {
            suppressTranslatedCharacter = false;
            return false;
        }
        const auto control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const auto shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (control && key == L'A') { model.setSelection(0, model.text().size()); selectionChanged(true); return true; }
        if (control && key == L'C') { copySelection(); return true; }
        if (control && key == L'X') { copySelection(); eraseSelection(true); return true; }
        if (control && key == L'V') { pasteClipboard(); return true; }
        if (control && key == L'Z') { undo(); return true; }
        if (key == VK_RETURN) { notify(WindowsTextInputCommitNotification); return true; }
        if (key == VK_ESCAPE) { notify(WindowsTextInputCancelNotification); return true; }
        if (key == VK_TAB) {
            notify(shift ? WindowsTextInputTabBackwardNotification
                         : WindowsTextInputTabForwardNotification);
            return true;
        }
        if (key == VK_HOME) { moveCaret(0, shift); return true; }
        if (key == VK_END) { moveCaret(model.text().size(), shift); return true; }
        if (key == VK_LEFT) {
            model.move(control ? desto::ui::TextMovement::PreviousWord
                               : desto::ui::TextMovement::PreviousCharacter, shift);
            selectionChanged(true); return true;
        }
        if (key == VK_RIGHT) {
            model.move(control ? desto::ui::TextMovement::NextWord
                               : desto::ui::TextMovement::NextCharacter, shift);
            selectionChanged(true); return true;
        }
        if (key == VK_BACK) {
            const auto current = model.selection();
            if (current.anchor != current.caret) eraseSelection(true);
            else if (current.caret > 0) {
                model.move(control ? desto::ui::TextMovement::PreviousWord
                                   : desto::ui::TextMovement::PreviousCharacter, true);
                eraseSelection(true);
            }
            return true;
        }
        if (key == VK_DELETE) {
            const auto current = model.selection();
            if (current.anchor != current.caret) eraseSelection(true);
            else if (current.caret < model.text().size()) {
                model.move(control ? desto::ui::TextMovement::NextWord
                                   : desto::ui::TextMovement::NextCharacter, true);
                eraseSelection(true);
            }
            return true;
        }
        return false;
    }

    void insertCharacter(wchar_t character) noexcept {
        if (suppressTranslatedCharacter) {
            suppressTranslatedCharacter = false;
            return;
        }
        if (character < 0x20 || character == 0x7F) return;
        const wchar_t value[1]{character};
        replaceSelection(std::wstring_view(value, 1), true);
    }

    void focusChanged(bool focused) noexcept {
        hasFocus = focused;
        caretVisible = true;
        auto& tsf = ThreadTsf();
        if (focused) {
            if (tsf.manager != nullptr && documentManager != nullptr) {
                tsf.manager->SetFocus(documentManager.Get());
            }
            SetTimer(window, kCaretTimerId, kCaretIntervalMilliseconds, nullptr);
        } else {
            KillTimer(window, kCaretTimerId);
            if (tsf.manager != nullptr) tsf.manager->SetFocus(nullptr);
            if (createInfo.commitOnFocusLoss && !contextMenuOpen) {
                notify(WindowsTextInputCommitNotification);
            }
        }
        redraw();
    }

    void tickCaret() noexcept {
        caretVisible = !caretVisible;
        redraw(false);
    }

    void beginMouseSelection(int x, int y, bool extend) noexcept {
        const auto position = hitTest(x, y);
        const auto current = model.selection();
        model.setSelection(extend ? current.anchor : position, position);
        dragging = true;
        SetCapture(window);
        selectionChanged(true);
    }

    void updateMouseSelection(int x, int y) noexcept {
        if (!dragging) return;
        model.setSelection(model.selection().anchor, hitTest(x, y));
        selectionChanged(true);
    }

    void endMouseSelection() noexcept {
        dragging = false;
        if (GetCapture() == window) ReleaseCapture();
    }

    void selectWordAt(int x, int y) noexcept {
        if (model.text().empty()) return;
        auto position = hitTest(x, y);
        if (position == model.text().size() && position > 0) {
            position = PreviousCodePoint(model.text(), position);
        }
        const auto whitespace = std::iswspace(model.text()[position]) != 0;
        auto start = position;
        auto end = NextCodePoint(model.text(), position);
        while (start > 0) {
            const auto previous = PreviousCodePoint(model.text(), start);
            if ((std::iswspace(model.text()[previous]) != 0) != whitespace) break;
            start = previous;
        }
        while (end < model.text().size()) {
            if ((std::iswspace(model.text()[end]) != 0) != whitespace) break;
            end = NextCodePoint(model.text(), end);
        }
        model.setSelection(start, end);
        selectionChanged(true);
    }

    void showContextMenu(LPARAM position) noexcept {
        const auto current = model.selection();
        POINT point{GET_X_LPARAM(position), GET_Y_LPARAM(position)};
        if (position == -1) {
            const auto rect = caretRect(current.caret);
            point = {rect.left, rect.bottom};
            ClientToScreen(window, &point);
        }
        const auto english = createInfo.placeholder.find(L"Search") != std::wstring::npos
            || createInfo.placeholder.find(L"Task") != std::wstring::npos;
        const std::array items{
            WindowsPopupMenuItem{kUndoCommand, english ? L"Undo" : L"撤销", L"", false, false, model.canUndo(), L"Ctrl+Z"},
            WindowsPopupMenuItem{kCutCommand, english ? L"Cut" : L"剪切", L"", true, false, current.anchor != current.caret, L"Ctrl+X"},
            WindowsPopupMenuItem{kCopyCommand, english ? L"Copy" : L"复制", L"", false, false, current.anchor != current.caret, L"Ctrl+C"},
            WindowsPopupMenuItem{kPasteCommand, english ? L"Paste" : L"粘贴", L"", false, false, IsClipboardFormatAvailable(CF_UNICODETEXT) != FALSE, L"Ctrl+V"},
            WindowsPopupMenuItem{kSelectAllCommand, english ? L"Select all" : L"全选", L"", true, false, !model.text().empty(), L"Ctrl+A"},
        };
        contextMenuOpen = true;
        const auto command = ShowWindowsPopupMenu(window, point, items);
        contextMenuOpen = false;
        if (IsWindow(window)) SetFocus(window);
        if (command == kUndoCommand) undo();
        else if (command == kCutCommand) { copySelection(); eraseSelection(true); }
        else if (command == kCopyCommand) copySelection();
        else if (command == kPasteCommand) pasteClipboard();
        else if (command == kSelectAllCommand) { model.setSelection(0, model.text().size()); selectionChanged(true); }
    }

    void paint(HDC suppliedDc = nullptr) noexcept {
        RECT client{};
        GetClientRect(window, &client);
        const auto width = client.right - client.left;
        const auto height = client.bottom - client.top;
        if (width <= 0 || height <= 0) return;
        if (createInfo.popup) paintLayered(width, height);
        else paintOpaque(suppliedDc, client);
    }

    void sizeChanged() noexcept {
        invalidateAllLayouts();
        applyWindowRegion();
        paint();
    }

    void flushDeferredRedraw() noexcept { performDeferredRedraw(); }

    std::size_t textPosition(int x, int y) noexcept { return hitTest(x, y); }
    RECT textPositionRect(std::size_t position) noexcept { return caretRect(position); }

    WindowsTextInputCreateInfo createInfo;
    desto::ui::TextInputModel model;
    HWND window = nullptr;
    WindowsTextInputRenderStatistics renderStatistics;

    const std::wstring& value() const noexcept { return model.text(); }

private:
    std::atomic<ULONG> references{1};
    float scrollX = 0.0F;
    bool hasFocus = false;
    bool caretVisible = true;
    bool dragging = false;
    bool contextMenuOpen = false;
    bool redrawPending = false;
    bool suppressTranslatedCharacter = false;
    bool locked = false;
    DWORD lockFlags = 0;
    DWORD pendingLockFlags = 0;
    std::optional<std::pair<std::size_t, std::size_t>> compositionRange;
    mutable std::unique_ptr<TextLayout> textLayoutCache;
    mutable std::unique_ptr<TextLayout> placeholderLayoutCache;
    HDC layeredDc = nullptr;
    HBITMAP layeredBitmap = nullptr;
    HGDIOBJ layeredPreviousBitmap = nullptr;
    std::uint32_t* layeredPixels = nullptr;
    int layeredWidth = 0;
    int layeredHeight = 0;
    ComPtr<ITextStoreACPSink> sink;
    ComPtr<ITfDocumentMgr> documentManager;
    ComPtr<ITfContext> context;
    ComPtr<ITfSource> contextSource;
    DWORD compositionSinkCookie = TF_INVALID_COOKIE;

    std::size_t clampPosition(LONG value) const noexcept {
        if (value <= 0) return 0;
        return (std::min)(static_cast<std::size_t>(value), model.text().size());
    }

    RECT contentRect() const noexcept {
        RECT client{};
        GetClientRect(window, &client);
        const auto glyphInset = createInfo.style.leadingGlyph.empty()
            ? 0.0F : createInfo.style.leadingGlyphWidth;
        client.left += static_cast<LONG>(std::lround(createInfo.style.paddingLeft + glyphInset));
        client.right -= static_cast<LONG>(std::lround(createInfo.style.paddingRight));
        return client;
    }

    void invalidateTextLayout() noexcept {
        textLayoutCache.reset();
    }

    void invalidateAllLayouts() noexcept {
        textLayoutCache.reset();
        placeholderLayoutCache.reset();
    }

    TextLayout* currentLayout(bool placeholder = false) const noexcept {
        const auto bounds = contentRect();
        const auto width = static_cast<int>((std::max)(1L, bounds.right - bounds.left));
        const auto& value = placeholder ? createInfo.placeholder : model.text();
        auto& cache = placeholder ? placeholderLayoutCache : textLayoutCache;
        if (cache == nullptr) {
            cache = CreateLayout(value, createInfo.style, (std::max)(width, 4096));
        }
        return cache.get();
    }

    void updateScroll(const TextLayout& layout) noexcept {
        if (createInfo.style.centered) { scrollX = 0.0F; return; }
        const auto bounds = contentRect();
        const auto available = static_cast<float>((std::max)(1L, bounds.right - bounds.left));
        int x = 0;
        if (caretX(layout, model.selection().caret, &x)) {
            if (x - scrollX > available - 2.0F) scrollX = x - available + 2.0F;
            if (x - scrollX < 0.0F) scrollX = x;
        }
        scrollX = (std::max)(0.0F, scrollX);
    }

    bool caretX(const TextLayout& layout, std::size_t position, int* x) const noexcept {
        if (x == nullptr || layout.analysis == nullptr) return false;
        const auto textSize = model.text().size();
        const auto clamped = (std::min)(position, textSize);
        if (clamped == textSize && textSize != 0) {
            const auto last = PreviousCodePoint(model.text(), textSize);
            const auto surrogatePair = last + 1 < textSize
                && model.text()[last] >= 0xD800 && model.text()[last] <= 0xDBFF
                && model.text()[last + 1] >= 0xDC00 && model.text()[last + 1] <= 0xDFFF;
            if (surrogatePair) {
                *x = layout.size.cx;
                return true;
            }
            return SUCCEEDED(ScriptStringCPtoX(
                layout.analysis, static_cast<int>(last), TRUE, x));
        }
        return SUCCEEDED(ScriptStringCPtoX(
            layout.analysis, static_cast<int>(clamped), FALSE, x));
    }

    std::size_t hitTest(int x, int y) noexcept {
        const auto layout = currentLayout();
        if (layout == nullptr || layout->analysis == nullptr || model.text().empty()) return 0;
        const auto bounds = contentRect();
        int position = 0;
        int trailing = 0;
        if (FAILED(ScriptStringXtoCP(layout->analysis,
                x - bounds.left + static_cast<int>(std::lround(scrollX)),
                &position, &trailing))) return model.selection().caret;
        const auto resolved = static_cast<std::size_t>((std::max)(0, position));
        if (trailing != 0) return NextCodePoint(model.text(), resolved);
        return (std::min)(resolved, model.text().size());
    }

    RECT caretRect(std::size_t position) noexcept {
        const auto bounds = contentRect();
        RECT result{bounds.left, bounds.top + 4, bounds.left + 1, bounds.bottom - 4};
        const auto layout = currentLayout();
        if (layout == nullptr || layout->analysis == nullptr) return result;
        updateScroll(*layout);
        int x = 0;
        if (caretX(*layout, position, &x)) {
            result.left = bounds.left + static_cast<LONG>(std::lround(x - scrollX));
            result.right = result.left + 1;
        }
        return result;
    }

    void notify(WORD code, bool posted = false) noexcept {
        if (createInfo.notificationWindow == nullptr) return;
        const auto wParam = MAKEWPARAM(createInfo.controlId, code);
        if (posted) PostMessageW(createInfo.notificationWindow, WM_COMMAND, wParam,
            reinterpret_cast<LPARAM>(window));
        else SendMessageW(createInfo.notificationWindow, WM_COMMAND, wParam,
            reinterpret_cast<LPARAM>(window));
    }

    void notifyChanged() noexcept { notify(EN_CHANGE); }

    void notifyTextChange(
        std::size_t start, std::size_t oldEnd, std::size_t newEnd) noexcept {
        if (sink == nullptr) return;
        TS_TEXTCHANGE change{
            static_cast<LONG>(start),
            static_cast<LONG>(oldEnd),
            static_cast<LONG>(newEnd)};
        sink->OnTextChange(0, &change);
    }

    void notifySelectionChange() noexcept {
        if (sink != nullptr) sink->OnSelectionChange();
    }

    void notifyLayoutChange() noexcept {
        if (sink != nullptr) sink->OnLayoutChange(TS_LC_CHANGE, kViewCookie);
    }

    void redraw(bool restartCaret = true) noexcept {
        if (restartCaret) {
            caretVisible = true;
            if (hasFocus && window != nullptr) {
                KillTimer(window, kCaretTimerId);
                SetTimer(window, kCaretTimerId, kCaretIntervalMilliseconds, nullptr);
            }
        }
        if (window == nullptr) return;
        if (createInfo.popup) {
            // TSF can report text, selection and composition changes as a
            // burst.  Coalesce them into one layered-window commit.
            if (!redrawPending) {
                redrawPending = true;
                PostMessageW(window, kDeferredRedrawMessage, 0, 0);
            }
        } else {
            InvalidateRect(window, nullptr, FALSE);
        }
        notifyLayoutChange();
    }

    void performDeferredRedraw() noexcept {
        redrawPending = false;
        if (window != nullptr && createInfo.popup) paint();
    }

    void selectionChanged(bool notifySink) noexcept {
        if (notifySink) notifySelectionChange();
        redraw();
    }

    void moveCaret(std::size_t value, bool extend) noexcept {
        const auto current = model.selection();
        const auto caret = (std::min)(value, model.text().size());
        model.setSelection(extend ? current.anchor : caret, caret);
        selectionChanged(true);
    }

    void replace(
        std::size_t start,
        std::size_t end,
        std::wstring_view value,
        bool remember) noexcept {
        try {
            const auto change = model.replace(start, end, value, remember);
            invalidateTextLayout();
            notifyTextChange(change.start, change.oldEnd, change.newEnd);
            notifySelectionChange();
            notifyChanged();
            redraw();
        } catch (...) {}
    }

    void replaceSelection(std::wstring_view value, bool remember) noexcept {
        const auto current = model.selection();
        replace((std::min)(current.anchor, current.caret),
            (std::max)(current.anchor, current.caret), value, remember);
    }

    HRESULT replaceRange(
        LONG start, LONG end,
        const WCHAR* value, ULONG count,
        TS_TEXTCHANGE* change,
        bool remember) noexcept {
        if (value == nullptr && count != 0) return E_INVALIDARG;
        const auto actualStart = clampPosition(start);
        const auto actualEnd = (std::max)(actualStart, clampPosition(end));
        const auto available = createInfo.maximumLength
            - (model.text().size() - (actualEnd - actualStart));
        const auto accepted = (std::min)(static_cast<std::size_t>(count), available);
        replace(actualStart, actualEnd, std::wstring_view(value, accepted), remember);
        if (change != nullptr) {
            change->acpStart = static_cast<LONG>(actualStart);
            change->acpOldEnd = static_cast<LONG>(actualEnd);
            change->acpNewEnd = static_cast<LONG>(actualStart + accepted);
        }
        return S_OK;
    }

    void eraseSelection(bool remember) noexcept {
        const auto current = model.selection();
        if (current.anchor == current.caret) return;
        replaceSelection(L"", remember);
    }

    void undo() noexcept {
        const auto change = model.undo();
        if (!change.has_value()) return;
        invalidateTextLayout();
        notifyTextChange(change->start, change->oldEnd, change->newEnd);
        notifySelectionChange();
        notifyChanged();
        redraw();
    }

    void copySelection() noexcept {
        const auto current = model.selection();
        if (current.anchor == current.caret || !OpenClipboard(window)) return;
        const auto start = (std::min)(current.anchor, current.caret);
        const auto end = (std::max)(current.anchor, current.caret);
        const auto bytes = (end - start + 1) * sizeof(wchar_t);
        const auto memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (memory != nullptr) {
            if (auto* destination = static_cast<wchar_t*>(GlobalLock(memory)); destination != nullptr) {
                std::memcpy(destination, model.text().data() + start,
                    (end - start) * sizeof(wchar_t));
                destination[end - start] = L'\0';
                GlobalUnlock(memory);
                EmptyClipboard();
                if (SetClipboardData(CF_UNICODETEXT, memory) != nullptr) {
                    CloseClipboard();
                    return;
                }
            }
            GlobalFree(memory);
        }
        CloseClipboard();
    }

    void pasteClipboard() noexcept {
        if (!OpenClipboard(window)) return;
        const auto memory = GetClipboardData(CF_UNICODETEXT);
        const auto* value = memory == nullptr
            ? nullptr : static_cast<const wchar_t*>(GlobalLock(memory));
        if (value != nullptr) {
            replaceSelection(value, true);
            GlobalUnlock(memory);
        }
        CloseClipboard();
    }

    void attachTsf() noexcept {
        auto& tsf = ThreadTsf();
        if (tsf.manager == nullptr || tsf.clientId == TF_CLIENTID_NULL) return;
        if (FAILED(tsf.manager->CreateDocumentMgr(documentManager.GetAddressOf()))) return;
        TfEditCookie cookie = TF_INVALID_COOKIE;
        if (FAILED(documentManager->CreateContext(
                tsf.clientId, 0, static_cast<ITextStoreACP*>(this),
                context.GetAddressOf(), &cookie))) {
            documentManager.Reset();
            return;
        }
        if (FAILED(documentManager->Push(context.Get()))) {
            context.Reset();
            documentManager.Reset();
            return;
        }
        if (SUCCEEDED(context.As(&contextSource))) {
            (void)contextSource->AdviseSink(
                IID_ITfContextOwnerCompositionSink,
                static_cast<ITfContextOwnerCompositionSink*>(this),
                &compositionSinkCookie);
        }
    }

    void detachTsf() noexcept {
        if (contextSource != nullptr && compositionSinkCookie != TF_INVALID_COOKIE) {
            contextSource->UnadviseSink(compositionSinkCookie);
        }
        compositionSinkCookie = TF_INVALID_COOKIE;
        contextSource.Reset();
        if (documentManager != nullptr) documentManager->Pop(TF_POPF_ALL);
        context.Reset();
        documentManager.Reset();
        sink.Reset();
    }

    void updateCompositionRange(ITfCompositionView* composition) noexcept {
        compositionRange.reset();
        if (composition != nullptr) {
            ComPtr<ITfRange> range;
            ComPtr<ITfRangeACP> acp;
            LONG start = 0;
            LONG count = 0;
            if (SUCCEEDED(composition->GetRange(range.GetAddressOf()))
                && SUCCEEDED(range.As(&acp))
                && SUCCEEDED(acp->GetExtent(&start, &count))) {
                compositionRange = std::pair{
                    clampPosition(start), clampPosition(start + count)};
            }
        }
        redraw();
    }

    void paintOpaque(HDC suppliedDc, RECT client) noexcept {
        PAINTSTRUCT paint{};
        const auto dc = suppliedDc != nullptr ? suppliedDc : BeginPaint(window, &paint);
        renderToDc(dc, client);
        if (suppliedDc == nullptr) EndPaint(window, &paint);
    }

    void applyWindowRegion() noexcept {
        if (window == nullptr) return;
        RECT bounds{};
        GetClientRect(window, &bounds);
        if (createInfo.style.cornerRadius <= 0.0F) {
            SetWindowRgn(window, nullptr, TRUE);
            return;
        }
        const auto diameter = static_cast<int>(std::lround(
            createInfo.style.cornerRadius * 2.0F));
        SetWindowRgn(window, CreateRoundRectRgn(
            bounds.left, bounds.top, bounds.right + 1, bounds.bottom + 1,
            diameter, diameter), TRUE);
    }

    void releaseLayeredBackingStore() noexcept {
        if (layeredDc != nullptr && layeredPreviousBitmap != nullptr) {
            SelectObject(layeredDc, layeredPreviousBitmap);
        }
        if (layeredBitmap != nullptr) DeleteObject(layeredBitmap);
        if (layeredDc != nullptr) DeleteDC(layeredDc);
        layeredDc = nullptr;
        layeredBitmap = nullptr;
        layeredPreviousBitmap = nullptr;
        layeredPixels = nullptr;
        layeredWidth = 0;
        layeredHeight = 0;
    }

    bool ensureLayeredBackingStore(int width, int height) noexcept {
        if (layeredDc != nullptr && layeredBitmap != nullptr
            && layeredPixels != nullptr && layeredWidth == width
            && layeredHeight == height) {
            return true;
        }
        releaseLayeredBackingStore();
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        layeredBitmap = CreateDIBSection(
            nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        layeredDc = CreateCompatibleDC(nullptr);
        if (layeredBitmap == nullptr || layeredDc == nullptr || pixels == nullptr) {
            releaseLayeredBackingStore();
            return false;
        }
        layeredPreviousBitmap = SelectObject(layeredDc, layeredBitmap);
        layeredPixels = static_cast<std::uint32_t*>(pixels);
        layeredWidth = width;
        layeredHeight = height;
        ++renderStatistics.backingStoreCreations;
        return true;
    }

    void paintLayered(int width, int height) noexcept {
        if (!ensureLayeredBackingStore(width, height)) return;
        std::memset(layeredPixels, 0, static_cast<std::size_t>(width) * height * 4);
        renderToDc(layeredDc, RECT{0, 0, width, height});
        for (std::size_t index = 0; index < static_cast<std::size_t>(width) * height; ++index) {
            if ((layeredPixels[index] & 0x00FFFFFFu) != 0) {
                layeredPixels[index] |= 0xFF000000u;
            }
        }
        ++renderStatistics.paints;
        renderStatistics.nonTransparentPixels = 0;
        renderStatistics.coloredPixels = 0;
        const auto* rendered = layeredPixels;
        const auto pixelCount = static_cast<std::size_t>(width) * height;
        for (std::size_t index = 0; index < pixelCount; ++index) {
            const auto pixel = rendered[index];
            const auto alpha = (pixel >> 24) & 0xFFu;
            if (alpha == 0) continue;
            ++renderStatistics.nonTransparentPixels;
            const auto blue = pixel & 0xFFu;
            const auto green = (pixel >> 8) & 0xFFu;
            const auto red = (pixel >> 16) & 0xFFu;
            if ((std::max)({red, green, blue}) - (std::min)({red, green, blue}) >= 20u) {
                ++renderStatistics.coloredPixels;
            }
        }
        POINT source{};
        RECT screen{};
        GetWindowRect(window, &screen);
        POINT destination{screen.left, screen.top};
        SIZE size{width, height};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(
            window, nullptr, &destination, &size, layeredDc, &source, 0, &blend, ULW_ALPHA);
    }

    void renderToDc(HDC dc, RECT client) noexcept {
        if (dc == nullptr) return;
        const auto fill = [&](COLORREF color, RECT bounds, int radius) {
            const auto brush = CreateSolidBrush(color);
            if (brush == nullptr) return;
            const auto previous = SelectObject(dc, brush);
            if (radius > 0) {
                RoundRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom,
                    radius * 2, radius * 2);
            } else {
                FillRect(dc, &bounds, brush);
            }
            SelectObject(dc, previous);
            DeleteObject(brush);
        };
        SetBkMode(dc, TRANSPARENT);
        const auto clientWidth = client.right - client.left;
        const auto clientHeight = client.bottom - client.top;
        if (!createInfo.popup || createInfo.style.backgroundAlpha != 0) {
            fill(createInfo.style.background, client,
                static_cast<int>(std::lround(createInfo.style.cornerRadius)));
        }
        if (createInfo.style.outlineWidth > 0.0F) {
            const auto pen = CreatePen(
                PS_SOLID,
                (std::max)(1, static_cast<int>(std::lround(createInfo.style.outlineWidth))),
                hasFocus ? createInfo.style.focusedOutline : createInfo.style.outline);
            if (pen != nullptr) {
                const auto previousPen = SelectObject(dc, pen);
                const auto previousBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
                const auto diameter = static_cast<int>(std::lround(
                    createInfo.style.cornerRadius * 2.0F));
                RoundRect(dc, client.left, client.top, client.right, client.bottom,
                    diameter, diameter);
                SelectObject(dc, previousBrush);
                SelectObject(dc, previousPen);
                DeleteObject(pen);
            }
        }

        const auto font = CreateFontW(
            -std::max(1, static_cast<int>(std::lround(createInfo.style.fontSize))),
            0, 0, 0, createInfo.style.fontWeight, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            createInfo.style.fontFamily.c_str());
        const auto previousFont = font == nullptr ? HGDIOBJ{} : SelectObject(dc, font);
        SetTextColor(dc, model.text().empty()
            ? createInfo.style.placeholder : createInfo.style.text);

        if (!createInfo.style.leadingGlyph.empty()) {
            const auto glyphFont = CreateFontW(
                -std::max(1, static_cast<int>(std::lround(createInfo.style.leadingGlyphSize))),
                0, 0, 0, createInfo.style.fontWeight, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                WindowsIconFontFamily().data());
            const auto previousGlyph = glyphFont == nullptr
                ? HGDIOBJ{} : SelectObject(dc, glyphFont);
            RECT glyph{
                static_cast<LONG>(std::lround(createInfo.style.paddingLeft)), client.top,
                static_cast<LONG>(std::lround(
                    createInfo.style.paddingLeft + createInfo.style.leadingGlyphWidth)),
                client.bottom,
            };
            DrawTextW(dc, createInfo.style.leadingGlyph.c_str(), -1, &glyph,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            if (previousGlyph != nullptr) SelectObject(dc, previousGlyph);
            if (glyphFont != nullptr) DeleteObject(glyphFont);
        }

        const auto bounds = contentRect();
        const auto placeholder = model.text().empty();
        const auto layout = currentLayout(placeholder);
        if (!placeholder && layout != nullptr) updateScroll(*layout);
        auto textBounds = bounds;
        textBounds.left -= static_cast<LONG>(std::lround(scrollX));
        const auto saved = SaveDC(dc);
        IntersectClipRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom);

        const auto current = model.selection();
        if (!placeholder && current.anchor != current.caret) {
            const auto first = caretRect((std::min)(current.anchor, current.caret));
            const auto last = caretRect((std::max)(current.anchor, current.caret));
            const RECT selection{
                first.left, bounds.top, (std::max)(first.left + 1, last.left), bounds.bottom};
            fill(createInfo.style.selection, selection, 0);
        }
        DrawTextW(
            dc,
            placeholder ? createInfo.placeholder.c_str() : model.text().c_str(),
            -1,
            &textBounds,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        if (compositionRange.has_value()
            && compositionRange->second > compositionRange->first) {
            const auto first = caretRect(compositionRange->first);
            const auto last = caretRect(compositionRange->second);
            const auto pen = CreatePen(PS_SOLID, 1, createInfo.style.compositionUnderline);
            if (pen != nullptr) {
                const auto previous = SelectObject(dc, pen);
                MoveToEx(dc, first.left, bounds.bottom - 3, nullptr);
                LineTo(dc, (std::max)(first.left + 1, last.left), bounds.bottom - 3);
                SelectObject(dc, previous);
                DeleteObject(pen);
            }
        }
        if (hasFocus && caretVisible && current.anchor == current.caret) {
            const auto caretBounds = caretRect(current.caret);
            const auto pen = CreatePen(PS_SOLID, 1, createInfo.style.focusedOutline);
            if (pen != nullptr) {
                const auto previous = SelectObject(dc, pen);
                MoveToEx(dc, caretBounds.left, caretBounds.top, nullptr);
                LineTo(dc, caretBounds.left, caretBounds.bottom);
                SelectObject(dc, previous);
                DeleteObject(pen);
            }
        }
        RestoreDC(dc, saved);
        if (previousFont != nullptr) SelectObject(dc, previousFont);
        if (font != nullptr) DeleteObject(font);
        (void)clientWidth;
        (void)clientHeight;
    }

};

TextInputState* State(HWND window) noexcept {
    return reinterpret_cast<TextInputState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

LRESULT CALLBACK TextInputProcedure(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept {
    auto* state = State(window);
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<TextInputState*>(create->lpCreateParams);
        state->AddRef();
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        state->attach(window);
    }
    if (state == nullptr) return DefWindowProcW(window, message, wParam, lParam);
    ComPtr<TextInputState> messageLifetime(state);
    switch (message) {
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS | DLGC_WANTTAB;
    case WM_SETFOCUS:
        state->focusChanged(true);
        return 0;
    case WM_KILLFOCUS:
        state->focusChanged(false);
        return 0;
    case WM_KEYDOWN:
    case WM_KEYUP:
        if (state->handleKeyMessage(message, wParam, lParam)) return 0;
        break;
    case WM_CHAR:
        state->insertCharacter(static_cast<wchar_t>(wParam));
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(window);
        state->beginMouseSelection(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam),
            (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        return 0;
    case WM_LBUTTONDBLCLK:
        SetFocus(window);
        state->selectWordAt(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_MOUSEMOVE:
        if ((wParam & MK_LBUTTON) != 0) {
            state->updateMouseSelection(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        state->endMouseSelection();
        return 0;
    case WM_CONTEXTMENU:
        state->showContextMenu(lParam);
        return 0;
    case WM_TIMER:
        if (wParam == kCaretTimerId) { state->tickCaret(); return 0; }
        break;
    case kDeferredRedrawMessage:
        state->flushDeferredRedraw();
        return 0;
    case WM_SIZE:
        state->sizeChanged();
        return 0;
    case WM_PAINT:
        state->paint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SETTEXT:
        state->setText(lParam == 0 ? L"" : reinterpret_cast<const wchar_t*>(lParam));
        return TRUE;
    case WM_GETTEXT: {
        if (lParam == 0 || wParam == 0) return 0;
        const auto count = (std::min)(static_cast<std::size_t>(wParam - 1), state->value().size());
        std::memcpy(reinterpret_cast<void*>(lParam), state->value().data(), count * sizeof(wchar_t));
        reinterpret_cast<wchar_t*>(lParam)[count] = L'\0';
        return static_cast<LRESULT>(count);
    }
    case WM_GETTEXTLENGTH:
        return static_cast<LRESULT>(state->value().size());
    case EM_GETSEL: {
        const auto [anchor, caret] = state->selection();
        const auto start = (std::min)(anchor, caret);
        const auto end = (std::max)(anchor, caret);
        if (wParam != 0) *reinterpret_cast<DWORD*>(wParam) = static_cast<DWORD>(start);
        if (lParam != 0) *reinterpret_cast<DWORD*>(lParam) = static_cast<DWORD>(end);
        return MAKELRESULT(static_cast<WORD>(start), static_cast<WORD>(end));
    }
    case EM_SETSEL:
        state->setSelection(
            wParam == static_cast<WPARAM>(-1) ? state->value().size() : static_cast<std::size_t>(wParam),
            lParam == -1 ? state->value().size() : static_cast<std::size_t>(lParam));
        return 0;
    case EM_POSFROMCHAR: {
        const auto rect = state->textPositionRect(static_cast<std::size_t>(wParam));
        return MAKELPARAM(rect.left, rect.top);
    }
    case EM_CHARFROMPOS:
        return static_cast<LRESULT>(state->textPosition(
            GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)));
    case WM_UNDO:
        state->performUndo();
        return TRUE;
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        state->detachFromWindow();
        state->Release();
        return DefWindowProcW(window, message, wParam, lParam);
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool EnsureTextInputClass() noexcept {
    static const bool registered = [] {
        WNDCLASSW descriptor{};
        descriptor.style = CS_DBLCLKS;
        descriptor.lpfnWndProc = &TextInputProcedure;
        descriptor.hInstance = GetModuleHandleW(nullptr);
        descriptor.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(IDC_IBEAM));
        descriptor.lpszClassName = kTextInputClassName;
        return RegisterClassW(&descriptor) != 0
            || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    return registered;
}

} // namespace

HWND CreateWindowsTextInput(const WindowsTextInputCreateInfo& createInfo) noexcept {
    try {
        if (!EnsureTextInputClass() || createInfo.notificationWindow == nullptr) return nullptr;
        auto* state = new TextInputState(createInfo);
        const auto width = createInfo.bounds.right - createInfo.bounds.left;
        const auto height = createInfo.bounds.bottom - createInfo.bounds.top;
        const auto extendedStyle = createInfo.popup ? WS_EX_TOOLWINDOW | WS_EX_LAYERED : 0;
        const auto style = createInfo.popup ? WS_POPUP : WS_CHILD | WS_TABSTOP;
        const auto parent = createInfo.popup ? nullptr : createInfo.notificationWindow;
        const auto window = CreateWindowExW(
            extendedStyle,
            kTextInputClassName,
            L"",
            style,
            createInfo.bounds.left,
            createInfo.bounds.top,
            width,
            height,
            parent,
            createInfo.popup ? nullptr
                : reinterpret_cast<HMENU>(static_cast<INT_PTR>(createInfo.controlId)),
            GetModuleHandleW(nullptr),
            state);
        state->Release();
        if (window == nullptr) return nullptr;
        ShowWindow(window, SW_SHOW);
        return window;
    } catch (...) {
        return nullptr;
    }
}

bool IsWindowsTextInput(HWND window) noexcept {
    if (window == nullptr) return false;
    wchar_t name[64]{};
    return GetClassNameW(window, name, static_cast<int>(std::size(name))) > 0
        && std::wstring_view(name) == kTextInputClassName;
}

std::wstring WindowsTextInputText(HWND window) {
    auto* state = State(window);
    return state == nullptr ? std::wstring{} : state->value();
}

void SetWindowsTextInputText(HWND window, std::wstring text) noexcept {
    if (auto* state = State(window); state != nullptr) state->setText(std::move(text));
}

void SetWindowsTextInputSelection(
    HWND window, std::size_t start, std::size_t end) noexcept {
    if (auto* state = State(window); state != nullptr) state->setSelection(start, end);
}

std::pair<std::size_t, std::size_t>
WindowsTextInputSelection(HWND window) noexcept {
    if (auto* state = State(window); state != nullptr) return state->selection();
    return {};
}

WindowsTextInputRenderStatistics WindowsTextInputRenderStats(HWND window) noexcept {
    if (auto* state = State(window); state != nullptr) return state->renderStatistics;
    return {};
}

void SetWindowsTextInputBounds(HWND window, RECT bounds) noexcept {
    if (window == nullptr) return;
    SetWindowPos(window, nullptr, bounds.left, bounds.top,
        bounds.right - bounds.left, bounds.bottom - bounds.top,
        SWP_NOACTIVATE | SWP_NOZORDER);
}

void SetWindowsTextInputStyle(HWND window, WindowsTextInputStyle style) noexcept {
    if (auto* state = State(window); state != nullptr) state->setStyle(std::move(style));
}

void FocusWindowsTextInput(HWND window) noexcept {
    if (window == nullptr) return;
    SetForegroundWindow(window);
    SetFocus(window);
}

} // namespace desto::platform::windows
