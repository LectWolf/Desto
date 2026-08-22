#include "WindowsShellItemCatalog.h"

#include <Windows.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <system_error>
#include <unordered_map>

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

bool IsHiddenFile(const std::filesystem::path& path) noexcept {
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_HIDDEN) != 0;
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
    return path.filename().wstring();
}

presentation::CardItemIcon BitmapPixels(HBITMAP bitmap) {
    if (bitmap == nullptr) {
        return {};
    }

    // IShellItemImageFactory returns an HBITMAP whose alpha encoding is not
    // part of the API contract. Let WIC make the conversion explicit instead
    // of guessing from pixel values (which misclassifies dark translucent
    // edges and creates a halo after scaling).
    ComPtr<IWICImagingFactory> imagingFactory;
    if (FAILED(CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&imagingFactory)))) {
        return {};
    }
    ComPtr<IWICBitmap> sourceBitmap;
    if (FAILED(imagingFactory->CreateBitmapFromHBITMAP(
            bitmap,
            nullptr,
            WICBitmapUseAlpha,
            &sourceBitmap))) {
        return {};
    }
    UINT width = 0;
    UINT height = 0;
    if (FAILED(sourceBitmap->GetSize(&width, &height)) || width == 0 || height == 0) {
        return {};
    }
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(imagingFactory->CreateFormatConverter(&converter))
        || FAILED(converter->Initialize(
            sourceBitmap.Get(),
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom))) {
        return {};
    }
    auto pixels = std::make_shared<std::vector<std::uint32_t>>(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    if (FAILED(converter->CopyPixels(
            nullptr,
            width * sizeof(std::uint32_t),
            static_cast<UINT>(pixels->size() * sizeof(std::uint32_t)),
            reinterpret_cast<BYTE*>(pixels->data())))) {
        return {};
    }
    const auto hasVisiblePixel = std::any_of(
        pixels->begin(),
        pixels->end(),
        [](std::uint32_t pixel) { return ((pixel >> 24) & 0xFFu) != 0; });
    if (!hasVisiblePixel) {
        return {};
    }
    return {
        .width = static_cast<int>(width),
        .height = static_cast<int>(height),
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
    const auto flags = static_cast<SIIGBF>(
        SIIGBF_ICONONLY | SIIGBF_RESIZETOFIT | SIIGBF_BIGGERSIZEOK);
    if (FAILED(imageFactory->GetImage(size, flags, &bitmap)) || bitmap == nullptr) {
        return {};
    }
    auto result = BitmapPixels(bitmap);
    DeleteObject(bitmap);
    return result;
}

struct FileFingerprint {
    bool exists = false;
    bool directory = false;
    std::uintmax_t fileSize = 0;
    std::int64_t modifiedTime = 0;

    bool operator==(const FileFingerprint&) const = default;
};

FileFingerprint ReadFingerprint(const std::filesystem::path& path) noexcept {
    FileFingerprint result;
    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    if (error || !std::filesystem::exists(status)) return result;
    result.exists = true;
    result.directory = std::filesystem::is_directory(status);
    if (std::filesystem::is_regular_file(status)) {
        result.fileSize = std::filesystem::file_size(path, error);
        if (error) {
            result.fileSize = 0;
            error.clear();
        }
    }
    const auto modified = std::filesystem::last_write_time(path, error);
    if (!error) result.modifiedTime = modified.time_since_epoch().count();
    return result;
}

std::wstring CachePathKey(const std::filesystem::path& path) {
    auto result = path.lexically_normal().wstring();
    std::ranges::transform(result, result.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return result;
}

presentation::CardItemView InspectShellItem(
    const std::filesystem::path& normalized,
    ShellIconSourceSize iconSize,
    const FileFingerprint& fingerprint) {
    presentation::CardItemView result{
        .id = normalized.wstring(),
        .displayName = DisplayName(normalized),
        .sourcePath = normalized,
        .fileSize = fingerprint.fileSize,
        .modifiedTime = fingerprint.modifiedTime,
    };
    if (!fingerprint.exists) {
        result.state = presentation::CardItemState::Missing;
        return result;
    }

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

} // namespace

struct WindowsShellItemCatalog::Impl {
    struct CacheKey {
        std::wstring path;
        ShellIconSourceSize iconSize = ShellIconSourceSize::Medium;

        bool operator==(const CacheKey&) const = default;
    };

    struct CacheKeyHash {
        std::size_t operator()(const CacheKey& key) const noexcept {
            const auto pathHash = std::hash<std::wstring>{}(key.path);
            const auto sizeHash = std::hash<int>{}(static_cast<int>(key.iconSize));
            return pathHash ^ (sizeHash + 0x9e3779b9u + (pathHash << 6) + (pathHash >> 2));
        }
    };

    struct CacheEntry {
        FileFingerprint fingerprint;
        presentation::CardItemView item;
        std::size_t iconBytes = 0;
        std::uint64_t lastUsed = 0;
    };

    explicit Impl(std::size_t entryLimit, std::size_t byteLimit)
        : maximumEntries(entryLimit), maximumIconBytes(byteLimit) {
    }

    void erase(std::unordered_map<CacheKey, CacheEntry, CacheKeyHash>::iterator entry) {
        cachedIconBytes -= entry->second.iconBytes;
        cache.erase(entry);
    }

    void trim() {
        while (!cache.empty()
            && (cache.size() > maximumEntries || cachedIconBytes > maximumIconBytes)) {
            auto oldest = cache.begin();
            for (auto entry = std::next(cache.begin()); entry != cache.end(); ++entry) {
                if (entry->second.lastUsed < oldest->second.lastUsed) oldest = entry;
            }
            erase(oldest);
        }
    }

    std::size_t maximumEntries = 0;
    std::size_t maximumIconBytes = 0;
    mutable std::mutex mutex;
    std::unordered_map<CacheKey, CacheEntry, CacheKeyHash> cache;
    std::size_t cachedIconBytes = 0;
    std::uint64_t clock = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
};

WindowsShellItemCatalog::WindowsShellItemCatalog(
    std::size_t maximumEntries,
    std::size_t maximumIconBytes)
    : impl_(std::make_unique<Impl>(maximumEntries, maximumIconBytes)) {
}

WindowsShellItemCatalog::~WindowsShellItemCatalog() = default;

ShellIconSourceSize ResolveShellIconSourceSize(domain::CardItemSize itemSize) noexcept {
    switch (itemSize) {
    case domain::CardItemSize::Small:
        return ShellIconSourceSize::Small;
    case domain::CardItemSize::Medium:
        return ShellIconSourceSize::Small;
    case domain::CardItemSize::Large:
        return ShellIconSourceSize::Medium;
    case domain::CardItemSize::ExtraLarge:
        return ShellIconSourceSize::Medium;
    }
    return ShellIconSourceSize::Large;
}

presentation::CardItemView WindowsShellItemCatalog::inspect(
    const std::filesystem::path& sourcePath,
    ShellIconSourceSize iconSize) const {
    const auto normalized = sourcePath.lexically_normal();
    const auto fingerprint = ReadFingerprint(normalized);
    const Impl::CacheKey key{CachePathKey(normalized), iconSize};
    {
        std::lock_guard lock(impl_->mutex);
        const auto cached = impl_->cache.find(key);
        if (cached != impl_->cache.end() && cached->second.fingerprint == fingerprint) {
            cached->second.lastUsed = ++impl_->clock;
            ++impl_->hits;
            return cached->second.item;
        }
        if (cached != impl_->cache.end()) impl_->erase(cached);
        ++impl_->misses;
    }

    auto result = InspectShellItem(normalized, iconSize, fingerprint);
    const auto iconBytes = result.icon.empty()
        ? std::size_t{0}
        : result.icon.premultipliedPixels->size() * sizeof(std::uint32_t);
    if (impl_->maximumEntries == 0 || iconBytes > impl_->maximumIconBytes) return result;
    {
        std::lock_guard lock(impl_->mutex);
        const auto existing = impl_->cache.find(key);
        if (existing != impl_->cache.end()) impl_->erase(existing);
        impl_->cachedIconBytes += iconBytes;
        impl_->cache.emplace(key, Impl::CacheEntry{
            fingerprint, result, iconBytes, ++impl_->clock});
        impl_->trim();
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
        if (IsHiddenFile(iterator->path())) continue;
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

std::vector<presentation::CardItemView> WindowsShellItemCatalog::refreshIcons(
    std::span<const presentation::CardItemView> items,
    ShellIconSourceSize iconSize) const {
    std::vector<presentation::CardItemView> result;
    result.reserve(items.size());
    const auto expectedWidth = static_cast<int>(iconSize);
    for (const auto& item : items) {
        if (!item.icon.empty() && item.icon.width == expectedWidth
            && item.icon.height == expectedWidth) {
            result.push_back(item);
        } else {
            result.push_back(inspect(item.sourcePath, iconSize));
        }
    }
    return result;
}

std::vector<presentation::CardItemView> WindowsShellItemCatalog::refreshDirectoryEntries(
    const std::filesystem::path& directory,
    std::span<const presentation::CardItemView> currentItems,
    std::span<const std::filesystem::path> changedRelativePaths,
    ShellIconSourceSize iconSize) const {
    const auto normalizedDirectory = directory.lexically_normal();
    std::vector<presentation::CardItemView> result(currentItems.begin(), currentItems.end());
    for (const auto& relative : changedRelativePaths) {
        if (relative.empty() || relative.is_absolute()
            || std::ranges::any_of(relative, [](const auto& component) {
                return component == L"..";
            })) {
            return enumerate(normalizedDirectory, {}, iconSize);
        }
        const auto target = (normalizedDirectory / relative).lexically_normal();
        if (_wcsicmp(target.parent_path().c_str(), normalizedDirectory.c_str()) != 0) {
            return enumerate(normalizedDirectory, {}, iconSize);
        }
        invalidate(target);
        const auto targetKey = CachePathKey(target);
        std::erase_if(result, [&](const auto& item) {
            return CachePathKey(item.sourcePath) == targetKey;
        });
        std::error_code error;
        if (std::filesystem::exists(target, error) && !error
            && !IsHiddenFile(target)) {
            result.push_back(inspect(target, iconSize));
        }
    }
    std::ranges::stable_sort(result, [](const auto& left, const auto& right) {
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
    invalidate(source);
    invalidate(destination);
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

void WindowsShellItemCatalog::invalidate(
    const std::filesystem::path& sourcePath) const noexcept {
    try {
        const auto path = CachePathKey(sourcePath);
        std::lock_guard lock(impl_->mutex);
        for (auto entry = impl_->cache.begin(); entry != impl_->cache.end();) {
            if (entry->first.path == path) {
                const auto current = entry++;
                impl_->erase(current);
            } else {
                ++entry;
            }
        }
    } catch (...) {
    }
}

void WindowsShellItemCatalog::clearCache() const noexcept {
    std::lock_guard lock(impl_->mutex);
    impl_->cache.clear();
    impl_->cachedIconBytes = 0;
}

ShellItemCacheStats WindowsShellItemCatalog::cacheStats() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return {
        .entries = impl_->cache.size(),
        .iconBytes = impl_->cachedIconBytes,
        .hits = impl_->hits,
        .misses = impl_->misses,
    };
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
