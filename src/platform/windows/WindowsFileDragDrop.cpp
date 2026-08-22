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

FORMATETC DestoInternalFileDropFormat() noexcept {
    static const auto format = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(
        L"Desto.InternalFileDrop"));
    return {
        .cfFormat = format,
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

FORMATETC PerformedEffectFormat() noexcept {
    static const auto format = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(
        L"Performed DropEffect"));
    return {
        .cfFormat = format,
        .ptd = nullptr,
        .dwAspect = DVASPECT_CONTENT,
        .lindex = -1,
        .tymed = TYMED_HGLOBAL,
    };
}

FORMATETC DestoSourceCardFormat() noexcept {
    static const auto format = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(
        L"Desto.ApplicationCardId"));
    return {
        .cfFormat = format,
        .ptd = nullptr,
        .dwAspect = DVASPECT_CONTENT,
        .lindex = -1,
        .tymed = TYMED_HGLOBAL,
    };
}

FORMATETC DestoDropCompletedFormat() noexcept {
    static const auto format = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(
        L"Desto.ApplicationCardDropCompleted"));
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
    auto format = SupportsFormat(data, FileDropFormat())
        ? FileDropFormat() : DestoInternalFileDropFormat();
    STGMEDIUM medium{};
    if (data == nullptr || FAILED(data->GetData(&format, &medium))) {
        return {};
    }
    std::vector<std::filesystem::path> result;
    if (format.cfFormat == FileDropFormat().cfFormat) {
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
    } else {
        const auto* value = static_cast<const wchar_t*>(GlobalLock(medium.hGlobal));
        if (value != nullptr) {
            const auto characterCount = GlobalSize(medium.hGlobal) / sizeof(wchar_t);
            std::size_t offset = 0;
            while (offset < characterCount && value[offset] != L'\0') {
                const auto begin = offset;
                while (offset < characterCount && value[offset] != L'\0') ++offset;
                if (offset > begin) {
                    result.emplace_back(std::wstring(value + begin, offset - begin));
                }
                ++offset;
            }
            GlobalUnlock(medium.hGlobal);
        }
    }
    ReleaseStgMedium(&medium);
    return result;
}

std::optional<std::string> ReadSourceCardId(IDataObject* data) {
    auto format = DestoSourceCardFormat();
    STGMEDIUM medium{};
    if (data == nullptr || FAILED(data->GetData(&format, &medium))) return std::nullopt;
    std::optional<std::string> result;
    const auto size = GlobalSize(medium.hGlobal);
    const auto* value = static_cast<const char*>(GlobalLock(medium.hGlobal));
    if (value != nullptr) {
        if (size > 1) {
            const auto* terminator = static_cast<const char*>(std::memchr(value, '\0', size));
            if (terminator != nullptr && terminator != value
                && static_cast<std::size_t>(terminator - value) <= 256) {
                result = std::string(value, terminator);
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

HGLOBAL CreateInternalFileDropMemory(const std::vector<std::filesystem::path>& paths) {
    std::size_t characterCount = 1;
    for (const auto& path : paths) characterCount += path.wstring().size() + 1;
    const auto memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT,
        characterCount * sizeof(wchar_t));
    if (memory == nullptr) return nullptr;
    auto* destination = static_cast<wchar_t*>(GlobalLock(memory));
    if (destination == nullptr) {
        GlobalFree(memory);
        return nullptr;
    }
    for (const auto& path : paths) {
        const auto value = path.wstring();
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

std::optional<DWORD> ReadEffectMemory(const STGMEDIUM& medium) noexcept {
    if (medium.tymed != TYMED_HGLOBAL
        || medium.hGlobal == nullptr
        || GlobalSize(medium.hGlobal) < sizeof(DWORD)) {
        return std::nullopt;
    }
    const auto* value = static_cast<const DWORD*>(GlobalLock(medium.hGlobal));
    if (value == nullptr) return std::nullopt;
    const auto result = *value;
    GlobalUnlock(medium.hGlobal);
    return result;
}

bool SetEffectData(IDataObject* data, const FORMATETC& sourceFormat, DWORD effect) noexcept {
    if (data == nullptr) return false;
    auto format = sourceFormat;
    STGMEDIUM medium{
        .tymed = TYMED_HGLOBAL,
        .hGlobal = CreateEffectMemory(effect),
    };
    if (medium.hGlobal == nullptr) return false;
    if (SUCCEEDED(data->SetData(&format, &medium, TRUE))) return true;
    ReleaseStgMedium(&medium);
    return false;
}

HGLOBAL CreateStringMemory(const std::string& value) {
    auto memory = GlobalAlloc(GMEM_MOVEABLE, value.size() + 1);
    if (memory == nullptr) return nullptr;
    auto* destination = static_cast<char*>(GlobalLock(memory));
    if (destination == nullptr) {
        GlobalFree(memory);
        return nullptr;
    }
    std::memcpy(destination, value.c_str(), value.size() + 1);
    GlobalUnlock(memory);
    return memory;
}

class FileDataObject final : public IDataObject {
public:
    FileDataObject(
        std::vector<std::filesystem::path> paths,
        std::optional<std::string> sourceCardId,
        bool allowMove,
        bool exposeToShell)
        : paths_(std::move(paths)), sourceCardId_(std::move(sourceCardId)),
          allowMove_(allowMove), exposeToShell_(exposeToShell) {
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
            const auto performedFormat = PerformedEffectFormat();
            const auto sourceFormat = DestoSourceCardFormat();
            const auto completedFormat = DestoDropCompletedFormat();
            HGLOBAL memory = nullptr;
            if (exposeToShell_ && format->cfFormat == fileFormat.cfFormat
                && (format->tymed & TYMED_HGLOBAL) != 0) {
                memory = CreateFileDropMemory(paths_);
            } else if (!exposeToShell_
                       && format->cfFormat == DestoInternalFileDropFormat().cfFormat
                       && (format->tymed & TYMED_HGLOBAL) != 0) {
                memory = CreateInternalFileDropMemory(paths_);
            } else if (format->cfFormat == effectFormat.cfFormat
                       && (format->tymed & TYMED_HGLOBAL) != 0) {
                memory = CreateEffectMemory(allowMove_ ? DROPEFFECT_MOVE : DROPEFFECT_COPY);
            } else if (performedDropEffect_.has_value()
                       && format->cfFormat == performedFormat.cfFormat
                       && (format->tymed & TYMED_HGLOBAL) != 0) {
                memory = CreateEffectMemory(*performedDropEffect_);
            } else if (sourceCardId_.has_value()
                       && format->cfFormat == sourceFormat.cfFormat
                       && (format->tymed & TYMED_HGLOBAL) != 0) {
                memory = CreateStringMemory(*sourceCardId_);
            } else if (dropCompleted_
                       && format->cfFormat == completedFormat.cfFormat
                       && (format->tymed & TYMED_HGLOBAL) != 0) {
                memory = CreateEffectMemory(TRUE);
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
        const auto performedFormat = PerformedEffectFormat();
        const auto sourceFormat = DestoSourceCardFormat();
        const auto completedFormat = DestoDropCompletedFormat();
        return (format->dwAspect == DVASPECT_CONTENT
                && format->lindex == -1
                && (format->tymed & TYMED_HGLOBAL) != 0
                && ((exposeToShell_ && format->cfFormat == fileFormat.cfFormat)
                    || (!exposeToShell_
                        && format->cfFormat == DestoInternalFileDropFormat().cfFormat)
                    || format->cfFormat == effectFormat.cfFormat
                    || (performedDropEffect_.has_value()
                        && format->cfFormat == performedFormat.cfFormat)
                    || (sourceCardId_.has_value()
                        && format->cfFormat == sourceFormat.cfFormat)
                    || (dropCompleted_
                        && format->cfFormat == completedFormat.cfFormat)))
            ? S_OK
            : DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* output) override {
        if (output != nullptr) {
            output->ptd = nullptr;
        }
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetData(
        FORMATETC* format,
        STGMEDIUM* medium,
        BOOL release) override {
        if (format == nullptr || medium == nullptr) return E_POINTER;
        const auto effect = ReadEffectMemory(*medium);
        if (!effect.has_value()) return DV_E_TYMED;
        if (format->cfFormat == DestoDropCompletedFormat().cfFormat) {
            dropCompleted_ = *effect != 0;
        } else if (format->cfFormat == PerformedEffectFormat().cfFormat) {
            performedDropEffect_ = *effect;
        } else {
            return E_NOTIMPL;
        }
        if (release) ReleaseStgMedium(medium);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** output) override {
        if (output == nullptr) {
            return E_POINTER;
        }
        *output = nullptr;
        if (direction != DATADIR_GET) {
            return E_NOTIMPL;
        }
        std::vector<FORMATETC> formats{
            exposeToShell_ ? FileDropFormat() : DestoInternalFileDropFormat(),
            PreferredEffectFormat(),
        };
        if (sourceCardId_.has_value()) formats.push_back(DestoSourceCardFormat());
        if (dropCompleted_) formats.push_back(DestoDropCompletedFormat());
        if (performedDropEffect_.has_value()) formats.push_back(PerformedEffectFormat());
        return SHCreateStdEnumFmtEtc(
            static_cast<UINT>(formats.size()), formats.data(), output);
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
    std::optional<std::string> sourceCardId_;
    bool allowMove_ = true;
    bool exposeToShell_ = true;
    std::optional<DWORD> performedDropEffect_;
    bool dropCompleted_ = false;
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
        DWORD keyState,
        POINTL point,
        DWORD* effect) override {
        try {
            acceptsFiles_ = SupportsFormat(data, FileDropFormat())
                || SupportsFormat(data, DestoInternalFileDropFormat());
            sourceCardId_ = ReadSourceCardId(data);
            return Update(keyState, point, effect);
        } catch (...) {
            acceptsFiles_ = false;
            sourceCardId_.reset();
            if (effect != nullptr) *effect = DROPEFFECT_NONE;
            return E_UNEXPECTED;
        }
    }
    HRESULT STDMETHODCALLTYPE DragOver(
        DWORD keyState,
        POINTL point,
        DWORD* effect) override {
        try {
            return Update(keyState, point, effect);
        } catch (...) {
            if (effect != nullptr) *effect = DROPEFFECT_NONE;
            return E_UNEXPECTED;
        }
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        try {
            acceptsFiles_ = false;
            sourceCardId_.reset();
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
        DWORD keyState,
        POINTL point,
        DWORD* effect) override {
        if (effect == nullptr) {
            return E_POINTER;
        }
        try {
            const auto allowed = *effect;
            auto paths = acceptsFiles_ ? ReadFileDrop(data) : std::vector<std::filesystem::path>{};
            acceptsFiles_ = false;
            auto sourceCardId = ReadSourceCardId(data);
            sourceCardId_.reset();
            const auto internalSource = sourceCardId.has_value();
            *effect = !paths.empty() && callbacks_.drop
                ? callbacks_.drop(
                    std::move(paths), std::move(sourceCardId), point, allowed, keyState)
                : DROPEFFECT_NONE;
            if (*effect != DROPEFFECT_NONE) {
                (void)SetEffectData(data, PerformedEffectFormat(), *effect);
            }
            if (internalSource && *effect != DROPEFFECT_NONE) {
                (void)SetEffectData(data, DestoDropCompletedFormat(), TRUE);
            }
            return S_OK;
        } catch (...) {
            acceptsFiles_ = false;
            *effect = DROPEFFECT_NONE;
            return E_UNEXPECTED;
        }
    }

private:
    HRESULT Update(DWORD keyState, POINTL point, DWORD* effect) {
        if (effect == nullptr) {
            return E_POINTER;
        }
        const auto allowed = *effect;
        *effect = acceptsFiles_ && callbacks_.dragOver
            ? callbacks_.dragOver(point, allowed, keyState, sourceCardId_)
            : DROPEFFECT_NONE;
        return S_OK;
    }

    std::atomic<ULONG> references_{1};
    FileDropTargetCallbacks callbacks_;
    bool acceptsFiles_ = false;
    std::optional<std::string> sourceCardId_;
};

} // namespace

IDropTarget* CreateFileDropTarget(FileDropTargetCallbacks callbacks) {
    return new (std::nothrow) FileDropTarget(std::move(callbacks));
}

IDataObject* CreateFileDataObject(
    const std::vector<std::filesystem::path>& paths,
    std::optional<std::string> sourceCardId,
    bool allowMove,
    bool exposeToShell) {
    if (paths.empty()) {
        return nullptr;
    }
    try {
        return new (std::nothrow) FileDataObject(
            paths, std::move(sourceCardId), allowMove, exposeToShell);
    } catch (...) {
        return nullptr;
    }
}

FileDragResult BeginFileDrag(
    const std::vector<std::filesystem::path>& paths,
    std::optional<std::string> sourceCardId,
    bool allowMove,
    bool exposeToShell) {
    if (paths.empty()) {
        return {.status = E_INVALIDARG};
    }
    auto* data = CreateFileDataObject(
        paths, std::move(sourceCardId), allowMove, exposeToShell);
    auto* source = new (std::nothrow) FileDropSource();
    if (data == nullptr || source == nullptr) {
        if (data != nullptr) data->Release();
        if (source != nullptr) source->Release();
        return {.status = E_OUTOFMEMORY};
    }
    DWORD effect = DROPEFFECT_NONE;
    const auto allowedEffects = allowMove
        ? (DROPEFFECT_MOVE | DROPEFFECT_COPY) : DROPEFFECT_COPY;
    const auto status = DoDragDrop(data, source, allowedEffects, &effect);
    const auto completedInsideDesto = WasFileDropHandledByDesto(data);
    data->Release();
    source->Release();
    return {
        .status = status,
        .effect = effect,
        .completedInsideDesto = completedInsideDesto,
    };
}

bool WasFileDropHandledByDesto(IDataObject* data) noexcept {
    return SupportsFormat(data, DestoDropCompletedFormat());
}

std::optional<DWORD> PerformedFileDropEffect(IDataObject* data) noexcept {
    auto format = PerformedEffectFormat();
    STGMEDIUM medium{};
    if (data == nullptr || FAILED(data->GetData(&format, &medium))) return std::nullopt;
    const auto result = ReadEffectMemory(medium);
    ReleaseStgMedium(&medium);
    return result;
}

} // namespace desto::platform::windows
