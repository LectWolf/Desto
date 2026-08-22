#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

#include "Card.h"

namespace desto::platform::windows {

struct DirectoryMappingWatch {
    domain::CardId cardId;
    std::filesystem::path sourceRoot;
};

struct DirectoryMappingChange {
    domain::CardId cardId;
    std::vector<std::filesystem::path> relativePaths;
    bool requiresFullRefresh = false;
};

class WindowsDirectoryChangeSource final {
public:
    using Callback = std::function<void(std::vector<DirectoryMappingChange>)>;

    explicit WindowsDirectoryChangeSource(
        std::vector<DirectoryMappingWatch> watches,
        Callback callback);
    ~WindowsDirectoryChangeSource();

    WindowsDirectoryChangeSource(const WindowsDirectoryChangeSource&) = delete;
    WindowsDirectoryChangeSource& operator=(const WindowsDirectoryChangeSource&) = delete;

    void start();
    void stop() noexcept;
    void replaceWatches(std::vector<DirectoryMappingWatch> watches);
    [[nodiscard]] bool running() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace desto::platform::windows
