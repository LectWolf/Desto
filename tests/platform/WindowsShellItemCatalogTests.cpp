#include "TestSupport.h"
#include "WindowsShellItemCatalog.h"

#include <Windows.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <fstream>

using Microsoft::WRL::ComPtr;
using namespace desto::domain;
using namespace desto::platform::windows;
using namespace desto::presentation;

namespace {

void CreateShortcut(
    const std::filesystem::path& shortcutPath,
    const std::filesystem::path& targetPath,
    const wchar_t* appUserModelId) {
    ComPtr<IShellLinkW> link;
    DESTO_CHECK(SUCCEEDED(CoCreateInstance(
        CLSID_ShellLink,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&link))));
    DESTO_CHECK(SUCCEEDED(link->SetPath(targetPath.c_str())));
    if (appUserModelId != nullptr) {
        ComPtr<IPropertyStore> properties;
        DESTO_CHECK(SUCCEEDED(link.As(&properties)));
        PROPVARIANT value{};
        DESTO_CHECK(SUCCEEDED(InitPropVariantFromString(appUserModelId, &value)));
        DESTO_CHECK(SUCCEEDED(properties->SetValue(PKEY_AppUserModel_ID, value)));
        DESTO_CHECK(SUCCEEDED(properties->Commit()));
        PropVariantClear(&value);
    }
    ComPtr<IPersistFile> persisted;
    DESTO_CHECK(SUCCEEDED(link.As(&persisted)));
    DESTO_CHECK(SUCCEEDED(persisted->Save(shortcutPath.c_str(), TRUE)));
}

void RunTests() {
    DESTO_CHECK(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)));
    const auto token = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root = std::filesystem::temp_directory_path() / ("desto-shell-" + token);
    std::filesystem::create_directories(root);
    std::ofstream(root / "zeta.txt") << "content";
    std::ofstream(root / "Broken.lnk") << "not a shortcut";

    wchar_t systemDirectory[MAX_PATH]{};
    DESTO_CHECK(GetSystemDirectoryW(systemDirectory, MAX_PATH) > 0);
    const auto target = std::filesystem::path(systemDirectory) / "notepad.exe";
    CreateShortcut(root / "Alpha.lnk", target, L"Desto.Test.Application!App");

    WindowsShellItemCatalog catalog;
    DESTO_CHECK(ResolveShellIconSourceSize(CardItemSize::Small)
                == ShellIconSourceSize::Small);
    DESTO_CHECK(ResolveShellIconSourceSize(CardItemSize::Medium)
                == ShellIconSourceSize::Small);
    DESTO_CHECK(ResolveShellIconSourceSize(CardItemSize::Large)
                == ShellIconSourceSize::Medium);
    DESTO_CHECK(ResolveShellIconSourceSize(CardItemSize::ExtraLarge)
                == ShellIconSourceSize::Medium);
    const auto textFile = catalog.inspect(root / "zeta.txt");
    DESTO_CHECK(textFile.fileSize == 7);
    DESTO_CHECK(!textFile.itemType.empty());
    DESTO_CHECK(textFile.modifiedTime != 0);
    const auto afterFirstInspection = catalog.cacheStats();
    const auto cachedTextFile = catalog.inspect(root / "zeta.txt");
    const auto afterCachedInspection = catalog.cacheStats();
    DESTO_CHECK(afterCachedInspection.hits == afterFirstInspection.hits + 1);
    DESTO_CHECK(afterCachedInspection.misses == afterFirstInspection.misses);
    DESTO_CHECK(cachedTextFile.icon.premultipliedPixels
        == textFile.icon.premultipliedPixels);

    catalog.invalidate(root / "zeta.txt");
    const auto afterInvalidation = catalog.inspect(root / "zeta.txt");
    const auto afterInvalidatedInspection = catalog.cacheStats();
    DESTO_CHECK(afterInvalidatedInspection.misses == afterCachedInspection.misses + 1);
    DESTO_CHECK(afterInvalidation.fileSize == textFile.fileSize);
    std::ofstream(root / "zeta.txt", std::ios::app) << "-changed";
    const auto changedTextFile = catalog.inspect(root / "zeta.txt");
    const auto afterChangedInspection = catalog.cacheStats();
    DESTO_CHECK(afterChangedInspection.misses == afterInvalidatedInspection.misses + 1);
    DESTO_CHECK(changedTextFile.fileSize > afterInvalidation.fileSize);
    const auto smallTextFile = catalog.inspect(
        root / "zeta.txt", ShellIconSourceSize::Small);
    DESTO_CHECK(smallTextFile.icon.empty() || smallTextFile.icon.width == 32);
    DESTO_CHECK(textFile.icon.empty() || textFile.icon.width == 48);
    const auto refreshedSmall = catalog.refreshIcons(
        std::vector<CardItemView>{textFile}, ShellIconSourceSize::Small);
    DESTO_CHECK(refreshedSmall.size() == 1);
    DESTO_CHECK(refreshedSmall.front().icon.empty()
        || refreshedSmall.front().icon.width == 32);
    const auto reusedSmall = catalog.refreshIcons(
        refreshedSmall, ShellIconSourceSize::Small);
    DESTO_CHECK(reusedSmall.front().icon.empty()
        || reusedSmall.front().icon.premultipliedPixels
            == refreshedSmall.front().icon.premultipliedPixels);
    const auto refreshedMedium = catalog.refreshIcons(
        refreshedSmall, ShellIconSourceSize::Large);
    DESTO_CHECK(refreshedMedium.front().icon.empty()
        || refreshedMedium.front().icon.width == 64);
    const auto retargeted = catalog.retarget(textFile, root / "renamed.txt");
    DESTO_CHECK(retargeted.sourcePath == root / "renamed.txt");
    DESTO_CHECK(retargeted.displayName == L"renamed.txt");
    DESTO_CHECK(retargeted.fileSize == textFile.fileSize);
    DESTO_CHECK(retargeted.icon.premultipliedPixels == textFile.icon.premultipliedPixels);
    const auto shortcut = catalog.inspect(root / "Alpha.lnk");
    DESTO_CHECK(shortcut.displayName == L"Alpha");
    DESTO_CHECK(shortcut.sourcePath.extension() == L".lnk");
    DESTO_CHECK(shortcut.resolvedTargetPath.filename() == L"notepad.exe");
    DESTO_CHECK(shortcut.appUserModelId == L"Desto.Test.Application!App");
    DESTO_CHECK(shortcut.state != CardItemState::Missing);
    DESTO_CHECK(shortcut.state != CardItemState::UnresolvedShortcut);

    const auto broken = catalog.inspect(root / "Broken.lnk");
    DESTO_CHECK(broken.displayName == L"Broken");
    DESTO_CHECK(broken.state == CardItemState::UnresolvedShortcut);
    DESTO_CHECK(broken.icon.empty());

    constexpr auto kazumiId = L"com.flutter.kazumi_wbnnev551gwxy!kazumi";
    ComPtr<IShellItem> packagedApplication;
    if (SUCCEEDED(SHCreateItemFromParsingName(
            (std::wstring(L"shell:AppsFolder\\") + kazumiId).c_str(),
            nullptr,
            IID_PPV_ARGS(&packagedApplication)))) {
        CreateShortcut(root / "Kazumi.lnk", target, kazumiId);
        CreateShortcut(root / "Notepad.lnk", target, nullptr);
        const auto packaged = catalog.inspect(root / "Kazumi.lnk");
        const auto executable = catalog.inspect(root / "Notepad.lnk");
        DESTO_CHECK(packaged.appUserModelId == kazumiId);
        DESTO_CHECK(!packaged.icon.empty());
        DESTO_CHECK(!executable.icon.empty());
        DESTO_CHECK(*packaged.icon.premultipliedPixels
            != *executable.icon.premultipliedPixels);
    }

    const auto missing = catalog.inspect(root / "missing.exe");
    DESTO_CHECK(missing.state == CardItemState::Missing);
    DESTO_CHECK(missing.icon.empty());

    std::ofstream(root / "notes.txt") << "notes";
    const auto hiddenPath = root / "hidden.txt";
    std::ofstream(hiddenPath) << "hidden";
    DESTO_CHECK(SetFileAttributesW(hiddenPath.c_str(), FILE_ATTRIBUTE_HIDDEN));
    const auto namedFile = catalog.inspect(root / "notes.txt");
    DESTO_CHECK(namedFile.displayName == L"notes.txt");

    const auto items = catalog.enumerate(root);
    DESTO_CHECK(items.size() >= 2);
    DESTO_CHECK(items[0].displayName == L"Alpha");
    DESTO_CHECK(std::any_of(
        items.begin(),
        items.end(),
        [](const CardItemView& item) {
            return item.displayName.find(L"zeta") != std::wstring::npos;
        }));
    DESTO_CHECK(std::ranges::none_of(items, [](const auto& item) {
        return item.sourcePath.filename() == "hidden.txt";
    }));
    const std::vector<std::filesystem::path> preferredOrder{"zeta.txt"};
    const auto preferred = catalog.enumerate(root, preferredOrder);
    DESTO_CHECK(preferred.front().sourcePath.filename() == "zeta.txt");

    std::ofstream(root / "delta.txt") << "delta";
    const auto beforeIncrementalAdd = catalog.cacheStats();
    auto incremental = catalog.refreshDirectoryEntries(
        root,
        items,
        std::vector<std::filesystem::path>{"delta.txt"});
    DESTO_CHECK(std::ranges::any_of(incremental, [](const auto& item) {
        return item.sourcePath.filename() == "delta.txt";
    }));
    DESTO_CHECK(catalog.cacheStats().misses == beforeIncrementalAdd.misses + 1);
    const auto hiddenIncrementalPath = root / "hidden-incremental.txt";
    std::ofstream(hiddenIncrementalPath) << "hidden";
    DESTO_CHECK(SetFileAttributesW(
        hiddenIncrementalPath.c_str(), FILE_ATTRIBUTE_HIDDEN));
    incremental = catalog.refreshDirectoryEntries(
        root,
        incremental,
        std::vector<std::filesystem::path>{"hidden-incremental.txt"});
    DESTO_CHECK(std::ranges::none_of(incremental, [](const auto& item) {
        return item.sourcePath.filename() == "hidden-incremental.txt";
    }));
    std::filesystem::remove(root / "zeta.txt");
    const auto beforeIncrementalRemove = catalog.cacheStats();
    incremental = catalog.refreshDirectoryEntries(
        root,
        incremental,
        std::vector<std::filesystem::path>{"zeta.txt"});
    DESTO_CHECK(std::ranges::none_of(incremental, [](const auto& item) {
        return item.sourcePath.filename() == "zeta.txt";
    }));
    DESTO_CHECK(catalog.cacheStats().misses == beforeIncrementalRemove.misses);

    WindowsShellItemCatalog boundedCatalog(2, 1024 * 1024);
    (void)boundedCatalog.inspect(root / "zeta.txt");
    (void)boundedCatalog.inspect(root / "notes.txt");
    (void)boundedCatalog.inspect(root / "Alpha.lnk");
    DESTO_CHECK(boundedCatalog.cacheStats().entries <= 2);
    DESTO_CHECK(boundedCatalog.cacheStats().iconBytes <= 1024 * 1024);

    std::filesystem::remove_all(root);
    CoUninitialize();
}

} // namespace

int main() {
    return desto::test::Run(RunTests);
}
