#include "WindowsDirectoryChangeSource.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <condition_variable>
#include <cwchar>
#include <exception>
#include <mutex>
#include <string>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace desto::platform::windows {
namespace {

constexpr ULONG_PTR StopCompletionKey = 1;
constexpr DWORD ChangeFilter = FILE_NOTIFY_CHANGE_FILE_NAME
    | FILE_NOTIFY_CHANGE_DIR_NAME
    | FILE_NOTIFY_CHANGE_SIZE
    | FILE_NOTIFY_CHANGE_LAST_WRITE
    | FILE_NOTIFY_CHANGE_CREATION;
constexpr DWORD CoalesceMilliseconds = 60;
constexpr std::size_t MaximumIncrementalPathsPerCard = 512;

void ValidateWatches(std::vector<DirectoryMappingWatch>& watches) {
    std::unordered_set<domain::CardId> cardIds;
    for (auto& watch : watches) {
        if (watch.cardId.empty() || watch.sourceRoot.empty()
            || !watch.sourceRoot.is_absolute()) {
            throw std::invalid_argument("Directory watches require a Card id and absolute path.");
        }
        watch.sourceRoot = watch.sourceRoot.lexically_normal();
        if (!cardIds.insert(watch.cardId).second) {
            throw std::invalid_argument("A Mapping Card may have only one directory watch.");
        }
    }
}

} // namespace

struct WindowsDirectoryChangeSource::Impl {
    struct WatchState {
        DirectoryMappingWatch watch;
        HANDLE directory = INVALID_HANDLE_VALUE;
        OVERLAPPED overlapped{};
        std::array<std::byte, 4096> buffer{};
        bool active = false;
    };

    Impl(std::vector<DirectoryMappingWatch> watchValues, Callback callbackValue)
        : configuredWatches(std::move(watchValues)), callback(std::move(callbackValue)) {
    }

    std::vector<DirectoryMappingWatch> configuredWatches;
    Callback callback;
    std::vector<std::unique_ptr<WatchState>> watches;
    std::thread thread;
    mutable std::mutex mutex;
    std::condition_variable readyCondition;
    std::exception_ptr startupFailure;
    HANDLE completionPort = nullptr;
    bool ready = false;
    bool active = false;

    void closeHandles() noexcept {
        for (auto& state : watches) {
            if (state->directory != INVALID_HANDLE_VALUE) {
                CancelIoEx(state->directory, &state->overlapped);
                CloseHandle(state->directory);
                state->directory = INVALID_HANDLE_VALUE;
            }
        }
        watches.clear();
        if (completionPort != nullptr) {
            CloseHandle(completionPort);
            completionPort = nullptr;
        }
    }

    bool issueRead(WatchState& state) noexcept {
        state.overlapped = {};
        state.active = ReadDirectoryChangesW(
            state.directory,
            state.buffer.data(),
            static_cast<DWORD>(state.buffer.size()),
            FALSE,
            ChangeFilter,
            nullptr,
            &state.overlapped,
            nullptr) != FALSE;
        return state.active;
    }

    void initializeWatches() {
        completionPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
        if (completionPort == nullptr) {
            throw std::runtime_error("CreateIoCompletionPort failed for mapping watches.");
        }
        watches.reserve(configuredWatches.size());
        for (const auto& configured : configuredWatches) {
            auto state = std::make_unique<WatchState>();
            state->watch = configured;
            state->directory = CreateFileW(
                configured.sourceRoot.c_str(),
                FILE_LIST_DIRECTORY,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                nullptr);
            if (state->directory == INVALID_HANDLE_VALUE) {
                throw std::runtime_error("Unable to open a Mapping Card source directory.");
            }
            const auto key = reinterpret_cast<ULONG_PTR>(state.get());
            if (CreateIoCompletionPort(state->directory, completionPort, key, 0) == nullptr) {
                throw std::runtime_error("Unable to register a Mapping Card directory watch.");
            }
            if (!issueRead(*state)) {
                throw std::runtime_error("Unable to begin a Mapping Card directory watch.");
            }
            watches.push_back(std::move(state));
        }
    }

    using PendingChanges = std::unordered_map<domain::CardId, DirectoryMappingChange>;

    static void appendPath(
        DirectoryMappingChange& change,
        std::filesystem::path relativePath) {
        if (change.requiresFullRefresh || relativePath.empty()) return;
        relativePath = relativePath.lexically_normal();
        if (relativePath.is_absolute() || relativePath.native().starts_with(L"..")) {
            change.requiresFullRefresh = true;
            change.relativePaths.clear();
            return;
        }
        const auto duplicate = std::ranges::any_of(
            change.relativePaths, [&](const auto& existing) {
                return _wcsicmp(existing.c_str(), relativePath.c_str()) == 0;
            });
        if (!duplicate) change.relativePaths.push_back(std::move(relativePath));
        if (change.relativePaths.size() > MaximumIncrementalPathsPerCard) {
            change.requiresFullRefresh = true;
            change.relativePaths.clear();
        }
    }

    static void appendNotificationPaths(
        DirectoryMappingChange& change,
        const WatchState& state,
        DWORD bytes) {
        if (bytes == 0 || bytes > state.buffer.size()) {
            change.requiresFullRefresh = true;
            change.relativePaths.clear();
            return;
        }
        DWORD offset = 0;
        while (offset < bytes) {
            const auto* notification = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(
                state.buffer.data() + offset);
            if (notification->FileNameLength > 0
                && notification->FileNameLength % sizeof(wchar_t) == 0) {
                appendPath(change, std::filesystem::path(std::wstring(
                    notification->FileName,
                    notification->FileNameLength / sizeof(wchar_t))));
            }
            if (notification->NextEntryOffset == 0) break;
            if (notification->NextEntryOffset > bytes - offset) {
                change.requiresFullRefresh = true;
                change.relativePaths.clear();
                break;
            }
            offset += notification->NextEntryOffset;
        }
    }

    void deliver(PendingChanges& changed) noexcept {
        if (changed.empty()) {
            return;
        }
        std::vector<DirectoryMappingChange> batch;
        batch.reserve(changed.size());
        for (auto& [cardId, change] : changed) {
            (void)cardId;
            std::ranges::sort(change.relativePaths, [](const auto& left, const auto& right) {
                return _wcsicmp(left.c_str(), right.c_str()) < 0;
            });
            batch.push_back(std::move(change));
        }
        std::ranges::sort(batch, {}, &DirectoryMappingChange::cardId);
        changed.clear();
        try {
            callback(std::move(batch));
        } catch (...) {
            // The watcher must remain active when a consumer refresh fails.
        }
    }

    bool processCompletion(
        BOOL succeeded,
        DWORD error,
        DWORD bytes,
        ULONG_PTR key,
        PendingChanges& changed) noexcept {
        if (key == StopCompletionKey) {
            return false;
        }
        auto* state = reinterpret_cast<WatchState*>(key);
        if (state == nullptr) {
            return true;
        }
        auto [entry, inserted] = changed.try_emplace(
            state->watch.cardId,
            DirectoryMappingChange{.cardId = state->watch.cardId});
        (void)inserted;
        if (succeeded) {
            appendNotificationPaths(entry->second, *state, bytes);
        } else {
            entry->second.requiresFullRefresh = true;
            entry->second.relativePaths.clear();
        }
        state->active = false;
        if (succeeded || error == ERROR_NOTIFY_ENUM_DIR) {
            (void)issueRead(*state);
        }
        return true;
    }

    void run() noexcept {
        try {
            initializeWatches();
            {
                std::lock_guard lock(mutex);
                active = true;
                ready = true;
                readyCondition.notify_all();
            }

            bool continueRunning = true;
            while (continueRunning) {
                DWORD bytes = 0;
                ULONG_PTR key = 0;
                OVERLAPPED* overlapped = nullptr;
                const auto succeeded = GetQueuedCompletionStatus(
                    completionPort, &bytes, &key, &overlapped, INFINITE);
                const auto error = succeeded ? ERROR_SUCCESS : GetLastError();
                PendingChanges changed;
                continueRunning = processCompletion(
                    succeeded, error, bytes, key, changed);
                while (continueRunning && !changed.empty()) {
                    bytes = 0;
                    key = 0;
                    overlapped = nullptr;
                    const auto more = GetQueuedCompletionStatus(
                        completionPort,
                        &bytes,
                        &key,
                        &overlapped,
                        CoalesceMilliseconds);
                    const auto moreError = more ? ERROR_SUCCESS : GetLastError();
                    if (!more && overlapped == nullptr && moreError == WAIT_TIMEOUT) {
                        break;
                    }
                    continueRunning = processCompletion(
                        more, moreError, bytes, key, changed);
                }
                if (continueRunning) {
                    deliver(changed);
                }
            }
        } catch (...) {
            std::lock_guard lock(mutex);
            startupFailure = std::current_exception();
            ready = true;
            readyCondition.notify_all();
        }

        closeHandles();
        std::lock_guard lock(mutex);
        active = false;
    }
};

WindowsDirectoryChangeSource::WindowsDirectoryChangeSource(
    std::vector<DirectoryMappingWatch> watches,
    Callback callback)
    : impl_(std::make_unique<Impl>(std::move(watches), std::move(callback))) {
    if (!impl_->callback) {
        throw std::invalid_argument("Directory change callback must not be empty.");
    }
    ValidateWatches(impl_->configuredWatches);
}

WindowsDirectoryChangeSource::~WindowsDirectoryChangeSource() {
    stop();
}

void WindowsDirectoryChangeSource::start() {
    std::unique_lock lock(impl_->mutex);
    if (impl_->thread.joinable()) {
        throw std::logic_error("Directory change source is already started.");
    }
    impl_->ready = false;
    impl_->startupFailure = nullptr;
    impl_->thread = std::thread([this] { impl_->run(); });
    impl_->readyCondition.wait(lock, [&] { return impl_->ready; });
    if (impl_->startupFailure) {
        const auto failure = impl_->startupFailure;
        lock.unlock();
        impl_->thread.join();
        std::rethrow_exception(failure);
    }
}

void WindowsDirectoryChangeSource::stop() noexcept {
    std::thread worker;
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->thread.joinable()) {
            return;
        }
        if (impl_->completionPort != nullptr) {
            PostQueuedCompletionStatus(
                impl_->completionPort, 0, StopCompletionKey, nullptr);
        }
        worker = std::move(impl_->thread);
    }
    if (worker.joinable()) {
        worker.join();
    }
}

void WindowsDirectoryChangeSource::replaceWatches(
    std::vector<DirectoryMappingWatch> watches) {
    ValidateWatches(watches);
    const auto restart = impl_->thread.joinable();
    if (restart) stop();
    impl_->configuredWatches = std::move(watches);
    if (restart) start();
}

bool WindowsDirectoryChangeSource::running() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->active;
}

} // namespace desto::platform::windows
