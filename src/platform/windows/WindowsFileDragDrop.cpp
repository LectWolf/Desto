#include "WindowsFileDragDrop.h"

#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
#include <cstring>
#include <new>
#include <utility>

namespace desto::platform::windows {
namespace {

FORMATETC FileDropFormat() noexcept {
    return {
        .cfFormat = CF_HDROP,
        .ptd = nullptr,
        .dwAspect = DVASPECT_CONTENT,
        .lindex = -1,
        .tymed = TYMED_HGLOBAL,
    };
}

FORMATETC PreferredEffectFormat() noexcept {
    static const auto format = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(
        L"Preferred DropEffect"));
    return {
        .cfFormat = format,
        .ptd = nullptr,
        .dwAspect = DVASPECT_CONTENT,
        .lindex = -1,
        .tymed = TYMED_HGLOBAL,
    };
}

bool SupportsFormat(IDataObject* data, const FORMATETC& format) noexcept {
    auto copy = format;
    return data != nullptr && SUCCEEDED(data->QueryGetData(&copy));
}

std::vector<std::filesystem::path> ReadFileDrop(IDataObject* data) {
    auto format = FileDropFormat();
    STGMEDIUM medium{};
    if (data == nullptr || FAILED(data->GetData(&format, &medium))) {
        return {};
    }
    std::vector<std::filesystem::path> result;
    const auto drop = static_cast<HDROP>(GlobalLock(medium.hGlobal));
    if (drop != nullptr) {
        const auto count = DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
        result.reserve(count);
        for (UINT index = 0; index < count; ++index) {
            const auto length = DragQueryFileW(drop, index, nullptr, 0);
            std::wstring path(length + 1, L'\0');
            if (DragQueryFileW(drop, index, path.data(), length + 1) != 0) {
                path.resize(length);
                result.emplace_back(std::move(path));
            }
        }
        GlobalUnlock(medium.hGlobal);
    }
    ReleaseStgMedium(&medium);
    return result;
}

HGLOBAL CreateFileDropMemory(const std::vector<std::filesystem::path>& paths) {
    std::vector<std::wstring> values;
    values.reserve(paths.size());
    std::size_t characterCount = 1;
    for (const auto& path : paths) {
        values.push_back(path.wstring());
        characterCount += values.back().size() + 1;
    }
    const auto byteCount = sizeof(DROPFILES) + characterCount * sizeof(wchar_t);
    auto memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, byteCount);
    if (memory == nullptr) {
        return nullptr;
    }
    auto* bytes = static_cast<std::byte*>(GlobalLock(memory));
    if (bytes == nullptr) {
        GlobalFree(memory);
        return nullptr;
    }
    auto* header = reinterpret_cast<DROPFILES*>(bytes);
    header->pFiles = sizeof(DROPFILES);
    header->fWide = TRUE;
    auto* destination = reinterpret_cast<wchar_t*>(bytes + sizeof(DROPFILES));
    for (const auto& value : values) {
        std::memcpy(destination, value.c_str(), (value.size() + 1) * sizeof(wchar_t));
        destination += value.size() + 1;
    }
    *destination = L'\0';
    GlobalUnlock(memory);
    return memory;
}

HGLOBAL CreateEffectMemory(DWORD effect) {
    auto memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (memory == nullptr) {
        return nullptr;
    }
    auto* value = static_cast<DWORD*>(GlobalLock(memory));
    if (value == nullptr) {
        GlobalFree(memory);
        return nullptr;
    }
    *value = effect;
    GlobalUnlock(memory);
    return memory;
}

class FileDataObject final : public IDataObject {
public:
    explicit FileDataObject(std::vector<std::filesystem::path> paths)
        : paths_(std::move(paths)) {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** result) override {
        if (result == nullptr) {
            return E_POINTER;
        }
        *result = nullptr;
        if (id == IID_IUnknown || id == IID_IDataObject) {
            *result = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining = --references_;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override {
        if (format == nullptr || medium == nullptr) {
            return E_POINTER;
        }
        std::memset(medium, 0, sizeof(*medium));
        try {
            const auto fileFormat = FileDropFormat();
            const auto effectFormat = PreferredEffectFormat();
            HGLOBAL memory = nullptr;
            if (format->cfFormat == fileFormat.cfFormat
                && (format->tymed & TYMED_HGLOBAL) != 0) {
                memory = CreateFileDropMemory(paths_);
            } else if (format->cfFormat == effectFormat.cfFormat
                       && (format->tymed & TYMED_HGLOBAL) != 0) {
                memory = CreateEffectMemory(DROPEFFECT_MOVE);
            } else {
                return DV_E_FORMATETC;
            }
            if (memory == nullptr) {
                return STG_E_MEDIUMFULL;
            }
            medium->tymed = TYMED_HGLOBAL;
            medium->hGlobal = memory;
            return S_OK;
        } catch (...) {
            return E_UNEXPECTED;
        }
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override {
        return DATA_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
        if (format == nullptr) {
            return E_POINTER;
        }
        const auto fileFormat = FileDropFormat();
        const auto effectFormat = PreferredEffectFormat();
        return (format->dwAspect == DVASPECT_CONTENT
                && format->lindex == -1
                && (format->tymed & TYMED_HGLOBAL) != 0
                && (format->cfFormat == fileFormat.cfFormat
                    || format->cfFormat == effectFormat.cfFormat))
            ? S_OK
            : DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* output) override {
        if (output != nullptr) {
            output->ptd = nullptr;
        }
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** output) override {
        if (output == nullptr) {
            return E_POINTER;
        }
        *output = nullptr;
        if (direction != DATADIR_GET) {
            return E_NOTIMPL;
        }
        FORMATETC formats[]{FileDropFormat(), PreferredEffectFormat()};
        return SHCreateStdEnumFmtEtc(2, formats, output);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }

private:
    std::atomic<ULONG> references_{1};
    std::vector<std::filesystem::path> paths_;
};

class FileDropSource final : public IDropSource {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** result) override {
        if (result == nullptr) {
            return E_POINTER;
        }
        *result = nullptr;
        if (id == IID_IUnknown || id == IID_IDropSource) {
            *result = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining = --references_;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escapePressed, DWORD keyState) override {
        if (escapePressed) {
            return DRAGDROP_S_CANCEL;
        }
        return (keyState & MK_LBUTTON) == 0 ? DRAGDROP_S_DROP : S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override {
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

private:
    std::atomic<ULONG> references_{1};
};

class FileDropTarget final : public IDropTarget {
public:
    explicit FileDropTarget(FileDropTargetCallbacks callbacks)
        : callbacks_(std::move(callbacks)) {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** result) override {
        if (result == nullptr) {
            return E_POINTER;
        }
        *result = nullptr;
        if (id == IID_IUnknown || id == IID_IDropTarget) {
            *result = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining = --references_;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(
        IDataObject* data,
        DWORD,
        POINTL point,
        DWORD* effect) override {
        try {
            acceptsFiles_ = SupportsFormat(data, FileDropFormat());
            return Update(point, effect);
        } catch (...) {
            acceptsFiles_ = false;
            if (effect != nullptr) *effect = DROPEFFECT_NONE;
            return E_UNEXPECTED;
        }
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL point, DWORD* effect) override {
        try {
            return Update(point, effect);
        } catch (...) {
            if (effect != nullptr) *effect = DROPEFFECT_NONE;
            return E_UNEXPECTED;
        }
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        try {
            acceptsFiles_ = false;
            if (callbacks_.dragLeave) {
                callbacks_.dragLeave();
            }
            return S_OK;
        } catch (...) {
            return E_UNEXPECTED;
        }
    }
    HRESULT STDMETHODCALLTYPE Drop(
        IDataObject* data,
        DWORD,
        POINTL point,
        DWORD* effect) override {
        if (effect == nullptr) {
            return E_POINTER;
        }
        try {
            const auto allowed = *effect;
            auto paths = acceptsFiles_ ? ReadFileDrop(data) : std::vector<std::filesystem::path>{};
            acceptsFiles_ = false;
            *effect = !paths.empty() && callbacks_.drop
                ? callbacks_.drop(std::move(paths), point, allowed)
                : DROPEFFECT_NONE;
            return S_OK;
        } catch (...) {
            acceptsFiles_ = false;
            *effect = DROPEFFECT_NONE;
            return E_UNEXPECTED;
        }
    }

private:
    HRESULT Update(POINTL point, DWORD* effect) {
        if (effect == nullptr) {
            return E_POINTER;
        }
        const auto allowed = *effect;
        *effect = acceptsFiles_ && callbacks_.dragOver
            ? callbacks_.dragOver(point, allowed)
            : DROPEFFECT_NONE;
        return S_OK;
    }

    std::atomic<ULONG> references_{1};
    FileDropTargetCallbacks callbacks_;
    bool acceptsFiles_ = false;
};

} // namespace

IDropTarget* CreateFileDropTarget(FileDropTargetCallbacks callbacks) {
    return new (std::nothrow) FileDropTarget(std::move(callbacks));
}

IDataObject* CreateFileDataObject(const std::vector<std::filesystem::path>& paths) {
    if (paths.empty()) {
        return nullptr;
    }
    try {
        return new (std::nothrow) FileDataObject(paths);
    } catch (...) {
        return nullptr;
    }
}

FileDragResult BeginFileDrag(const std::vector<std::filesystem::path>& paths) {
    if (paths.empty()) {
        return {.status = E_INVALIDARG};
    }
    auto* data = CreateFileDataObject(paths);
    auto* source = new (std::nothrow) FileDropSource();
    if (data == nullptr || source == nullptr) {
        if (data != nullptr) data->Release();
        if (source != nullptr) source->Release();
        return {.status = E_OUTOFMEMORY};
    }
    DWORD effect = DROPEFFECT_NONE;
    const auto status = DoDragDrop(data, source, DROPEFFECT_MOVE | DROPEFFECT_COPY, &effect);
    data->Release();
    source->Release();
    return {.status = status, .effect = effect};
}

} // namespace desto::platform::windows
