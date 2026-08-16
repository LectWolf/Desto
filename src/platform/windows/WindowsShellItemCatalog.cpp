#include "WindowsShellItemCatalog.h"

#include <Windows.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwchar>
#include <memory>
#include <system_error>

namespace desto::platform::windows {
namespace {

using Microsoft::WRL::ComPtr;

class ComApartment final {
public:
    ComApartment() noexcept
        : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {
    }

    ~ComApartment() {
        if (result_ == S_OK || result_ == S_FALSE) {
            CoUninitialize();
        }
    }

    [[nodiscard]] bool available() const noexcept {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }

private:
    HRESULT result_;
};

struct ShortcutMetadata {
    std::filesystem::path targetPath;
    std::wstring appUserModelId;
};

bool IsShortcut(const std::filesystem::path& path) noexcept {
    return _wcsicmp(path.extension().c_str(), L".lnk") == 0;
}

ShortcutMetadata ReadShortcut(const std::filesystem::path& path) {
    ShortcutMetadata result;
    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(
            CLSID_ShellLink,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&link)))) {
        return result;
    }
    ComPtr<IPersistFile> persisted;
    if (FAILED(link.As(&persisted)) || FAILED(persisted->Load(path.c_str(), STGM_READ))) {
        return result;
    }

    WIN32_FIND_DATAW targetData{};
    wchar_t target[MAX_PATH]{};
    if (SUCCEEDED(link->GetPath(target, MAX_PATH, &targetData, SLGP_RAWPATH)) && target[0] != L'\0') {
        result.targetPath = target;
    }

    ComPtr<IPropertyStore> properties;
    if (SUCCEEDED(link.As(&properties))) {
        PROPVARIANT value{};
        PropVariantInit(&value);
        if (SUCCEEDED(properties->GetValue(PKEY_AppUserModel_ID, &value))) {
            wchar_t* text = nullptr;
            if (SUCCEEDED(PropVariantToStringAlloc(value, &text)) && text != nullptr) {
                result.appUserModelId = text;
                CoTaskMemFree(text);
            }
        }
        PropVariantClear(&value);
    }
    if (result.appUserModelId.empty()) {
        PIDLIST_ABSOLUTE targetId = nullptr;
        if (SUCCEEDED(link->GetIDList(&targetId)) && targetId != nullptr) {
            ComPtr<IShellItem2> targetItem;
            if (SUCCEEDED(SHCreateItemFromIDList(targetId, IID_PPV_ARGS(&targetItem)))) {
                wchar_t* appId = nullptr;
                if (SUCCEEDED(targetItem->GetString(PKEY_AppUserModel_ID, &appId))
                    && appId != nullptr) {
                    result.appUserModelId = appId;
                    CoTaskMemFree(appId);
                }
            }
            CoTaskMemFree(targetId);
        }
    }
    return result;
}

std::wstring DisplayName(const std::filesystem::path& path) {
    if (IsShortcut(path)) {
        return path.stem().wstring();
    }
    SHFILEINFOW info{};
    if (SHGetFileInfoW(
            path.c_str(),
            0,
            &info,
            sizeof(info),
            SHGFI_DISPLAYNAME) != 0
        && info.szDisplayName[0] != L'\0') {
        return info.szDisplayName;
    }
    return path.filename().wstring();
}

presentation::CardItemIcon BitmapPixels(HBITMAP bitmap) {
    BITMAP details{};
    if (bitmap == nullptr || GetObjectW(bitmap, sizeof(details), &details) == 0
        || details.bmWidth <= 0 || details.bmHeight == 0) {
        return {};
    }
    const auto width = details.bmWidth;
    const auto height = std::abs(details.bmHeight);
    auto pixels = std::make_shared<std::vector<std::uint32_t>>(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    const auto dc = GetDC(nullptr);
    if (dc == nullptr
        || GetDIBits(
            dc,
            bitmap,
            0,
            static_cast<UINT>(height),
            pixels->data(),
            &info,
            DIB_RGB_COLORS) != height) {
        if (dc != nullptr) {
            ReleaseDC(nullptr, dc);
        }
        return {};
    }
    ReleaseDC(nullptr, dc);
    const auto hasVisiblePixel = std::any_of(
        pixels->begin(),
        pixels->end(),
        [](std::uint32_t pixel) { return ((pixel >> 24) & 0xFFu) != 0; });
    if (!hasVisiblePixel) {
        return {};
    }
    const auto needsPremultiplication = std::any_of(
        pixels->begin(),
        pixels->end(),
        [](std::uint32_t pixel) {
            const auto alpha = (pixel >> 24) & 0xFFu;
            return alpha > 0 && alpha < 255
                && (((pixel >> 16) & 0xFFu) > alpha
                    || ((pixel >> 8) & 0xFFu) > alpha
                    || (pixel & 0xFFu) > alpha);
        });
    if (needsPremultiplication) {
        for (auto& pixel : *pixels) {
            const auto alpha = (pixel >> 24) & 0xFFu;
            const auto red = ((pixel >> 16) & 0xFFu) * alpha / 255u;
            const auto green = ((pixel >> 8) & 0xFFu) * alpha / 255u;
            const auto blue = (pixel & 0xFFu) * alpha / 255u;
            pixel = (alpha << 24) | (red << 16) | (green << 8) | blue;
        }
    }
    return {
        .width = width,
        .height = height,
        .premultipliedPixels = std::move(pixels),
    };
}

presentation::CardItemIcon LoadShellImage(
    const std::wstring& parsingName,
    ShellIconSourceSize sourceSize) {
    ComPtr<IShellItem> shellItem;
    if (FAILED(SHCreateItemFromParsingName(
            parsingName.c_str(),
            nullptr,
            IID_PPV_ARGS(&shellItem)))) {
        return {};
    }
    ComPtr<IShellItemImageFactory> imageFactory;
    if (FAILED(shellItem.As(&imageFactory))) {
        return {};
    }
    HBITMAP bitmap = nullptr;
    const auto pixels = static_cast<LONG>(sourceSize);
    const SIZE size{pixels, pixels};
    const auto flags = static_cast<SIIGBF>(SIIGBF_ICONONLY | SIIGBF_RESIZETOFIT);
    if (FAILED(imageFactory->GetImage(size, flags, &bitmap)) || bitmap == nullptr) {
        return {};
    }
    auto result = BitmapPixels(bitmap);
    DeleteObject(bitmap);
    return result;
}

} // namespace

ShellIconSourceSize ResolveShellIconSourceSize(domain::CardItemSize itemSize) noexcept {
    switch (itemSize) {
    case domain::CardItemSize::Small:
    case domain::CardItemSize::Medium:
        return ShellIconSourceSize::Small;
    case domain::CardItemSize::Large:
    case domain::CardItemSize::ExtraLarge:
        return ShellIconSourceSize::Medium;
    }
    return ShellIconSourceSize::Medium;
}

presentation::CardItemView WindowsShellItemCatalog::inspect(
    const std::filesystem::path& sourcePath,
    ShellIconSourceSize iconSize) const {
    const auto normalized = sourcePath.lexically_normal();
    presentation::CardItemView result{
        .id = normalized.wstring(),
        .displayName = DisplayName(normalized),
        .sourcePath = normalized,
    };
    std::error_code error;
    if (!std::filesystem::exists(normalized, error) || error) {
        result.state = presentation::CardItemState::Missing;
        return result;
    }

    if (std::filesystem::is_regular_file(normalized, error) && !error) {
        result.fileSize = std::filesystem::file_size(normalized, error);
        if (error) result.fileSize = 0;
    }
    error.clear();
    const auto modified = std::filesystem::last_write_time(normalized, error);
    if (!error) result.modifiedTime = modified.time_since_epoch().count();
    SHFILEINFOW fileInfo{};
    if (SHGetFileInfoW(
            normalized.c_str(),
            0,
            &fileInfo,
            sizeof(fileInfo),
            SHGFI_TYPENAME) != 0) {
        result.itemType = fileInfo.szTypeName;
    }

    ComApartment apartment;
    if (!apartment.available()) {
        result.state = presentation::CardItemState::IconUnavailable;
        return result;
    }

    const auto shortcutSource = IsShortcut(normalized);
    if (shortcutSource) {
        const auto shortcut = ReadShortcut(normalized);
        result.resolvedTargetPath = shortcut.targetPath;
        result.appUserModelId = shortcut.appUserModelId;
        if (result.resolvedTargetPath.empty() && result.appUserModelId.empty()) {
            result.state = presentation::CardItemState::UnresolvedShortcut;
        }
        if (!result.appUserModelId.empty()) {
            result.icon = LoadShellImage(
                L"shell:AppsFolder\\" + result.appUserModelId, iconSize);
        }
    }
    const auto canUseSourceImage = !shortcutSource
        || (result.appUserModelId.empty() && !result.resolvedTargetPath.empty());
    if (result.icon.empty() && canUseSourceImage) {
        result.icon = LoadShellImage(normalized.wstring(), iconSize);
    }
    if (result.state == presentation::CardItemState::Ready && result.icon.empty()) {
        result.state = presentation::CardItemState::IconUnavailable;
    }
    return result;
}

std::vector<presentation::CardItemView> WindowsShellItemCatalog::enumerate(
    const std::filesystem::path& directory,
    std::span<const std::filesystem::path> preferredOrder,
    ShellIconSourceSize iconSize) const {
    std::vector<presentation::CardItemView> result;
    std::error_code error;
    if (!std::filesystem::exists(directory, error)) {
        return result;
    }
    for (std::filesystem::directory_iterator iterator(directory, error), end;
         !error && iterator != end;
         iterator.increment(error)) {
        result.push_back(inspect(iterator->path(), iconSize));
    }
    const auto rank = [&](const presentation::CardItemView& item) {
        const auto name = item.sourcePath.filename().wstring();
        const auto found = std::find_if(
            preferredOrder.begin(), preferredOrder.end(), [&](const std::filesystem::path& preferred) {
                return _wcsicmp(name.c_str(), preferred.filename().c_str()) == 0;
            });
        return found == preferredOrder.end()
            ? preferredOrder.size()
            : static_cast<std::size_t>(std::distance(preferredOrder.begin(), found));
    };
    std::stable_sort(
        result.begin(),
        result.end(),
        [&](const presentation::CardItemView& left, const presentation::CardItemView& right) {
            const auto leftRank = rank(left);
            const auto rightRank = rank(right);
            if (leftRank != rightRank) {
                return leftRank < rightRank;
            }
            return _wcsicmp(left.displayName.c_str(), right.displayName.c_str()) < 0;
        });
    return result;
}

presentation::CardItemView WindowsShellItemCatalog::retarget(
    presentation::CardItemView preparedItem,
    const std::filesystem::path& destinationPath) const {
    const auto destination = destinationPath.lexically_normal();
    if (_wcsicmp(
            preparedItem.sourcePath.filename().c_str(),
            destination.filename().c_str()) != 0) {
        preparedItem.displayName = IsShortcut(destination)
            ? destination.stem().wstring()
            : destination.filename().wstring();
    }
    preparedItem.id = destination.wstring();
    preparedItem.sourcePath = destination;
    return preparedItem;
}

void WindowsShellItemCatalog::notifyMoved(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& destinationPath) const noexcept {
    const auto source = sourcePath.lexically_normal();
    const auto destination = destinationPath.lexically_normal();
    std::error_code error;
    const auto event = std::filesystem::is_directory(destination, error)
        ? SHCNE_RENAMEFOLDER
        : SHCNE_RENAMEITEM;
    constexpr auto flags = SHCNF_PATHW | SHCNF_FLUSHNOWAIT;
    SHChangeNotify(event, flags, source.c_str(), destination.c_str());

    const auto sourceDirectory = source.parent_path();
    const auto destinationDirectory = destination.parent_path();
    if (!sourceDirectory.empty()) {
        SHChangeNotify(SHCNE_UPDATEDIR, flags, sourceDirectory.c_str(), nullptr);
    }
    if (!destinationDirectory.empty()
        && _wcsicmp(sourceDirectory.c_str(), destinationDirectory.c_str()) != 0) {
        SHChangeNotify(SHCNE_UPDATEDIR, flags, destinationDirectory.c_str(), nullptr);
    }
}

bool WindowsShellItemCatalog::launch(const presentation::CardItemView& item) const noexcept {
    if (item.sourcePath.empty()
        || item.state == presentation::CardItemState::Missing
        || item.state == presentation::CardItemState::UnresolvedShortcut) {
        return false;
    }
    const auto result = ShellExecuteW(
        nullptr,
        L"open",
        item.sourcePath.c_str(),
        nullptr,
        item.sourcePath.parent_path().c_str(),
        SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

} // namespace desto::platform::windows
