#include "WindowsDisplayChangeSource.h"

#include <Windows.h>

#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace desto::platform::windows {

struct WindowsDisplayChangeSource::Impl {
    explicit Impl(Callback callbackValue)
        : callback(std::move(callbackValue)) {
    }

    Callback callback;
    std::thread thread;
    mutable std::mutex mutex;
    std::condition_variable readyCondition;
    std::exception_ptr startupFailure;
    DWORD threadId = 0;
    bool ready = false;
    bool active = false;
    HWND window = nullptr;

    static LRESULT CALLBACK WindowProcedure(
        HWND windowHandle,
        UINT message,
        WPARAM wParam,
        LPARAM lParam) {
        auto* instance = reinterpret_cast<Impl*>(
            GetWindowLongPtrW(windowHandle, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            instance = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(
                windowHandle,
                GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(instance));
        }

        if (instance != nullptr) {
            switch (message) {
            case WM_DISPLAYCHANGE:
            case WM_DEVICECHANGE:
            case WM_SETTINGCHANGE:
            case WM_DPICHANGED:
                try {
                    instance->callback();
                } catch (...) {
                    // A system message thread must not terminate because a consumer failed.
                }
                break;
            default:
                break;
            }
        }
        return DefWindowProcW(windowHandle, message, wParam, lParam);
    }

    void run() noexcept {
        const auto module = GetModuleHandleW(nullptr);
        threadId = GetCurrentThreadId();
        MSG queueProbe{};
        PeekMessageW(&queueProbe, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

        const auto className = L"DestoDisplayChangeSource-" + std::to_wstring(threadId);
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = &WindowProcedure;
        windowClass.hInstance = module;
        windowClass.lpszClassName = className.c_str();
        if (RegisterClassW(&windowClass) == 0) {
            std::lock_guard lock(mutex);
            startupFailure = std::make_exception_ptr(
                std::runtime_error("RegisterClassW failed for display change source."));
            ready = true;
            readyCondition.notify_all();
            return;
        }

        window = CreateWindowExW(
            WS_EX_TOOLWINDOW,
            className.c_str(),
            L"DestoDisplayChangeSource",
            WS_POPUP,
            0,
            0,
            0,
            0,
            nullptr,
            nullptr,
            module,
            this);
        if (window == nullptr) {
            UnregisterClassW(className.c_str(), module);
            std::lock_guard lock(mutex);
            startupFailure = std::make_exception_ptr(
                std::runtime_error("CreateWindowExW failed for display change source."));
            ready = true;
            readyCondition.notify_all();
            return;
        }

        {
            std::lock_guard lock(mutex);
            active = true;
            ready = true;
            readyCondition.notify_all();
        }

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        DestroyWindow(window);
        window = nullptr;
        UnregisterClassW(className.c_str(), module);
        std::lock_guard lock(mutex);
        active = false;
    }
};

WindowsDisplayChangeSource::WindowsDisplayChangeSource(Callback callback)
    : impl_(std::make_unique<Impl>(std::move(callback))) {
    if (!impl_->callback) {
        throw std::invalid_argument("Display change callback must not be empty.");
    }
}

WindowsDisplayChangeSource::~WindowsDisplayChangeSource() {
    stop();
}

void WindowsDisplayChangeSource::start() {
    std::unique_lock lock(impl_->mutex);
    if (impl_->thread.joinable()) {
        throw std::logic_error("Display change source is already started.");
    }
    impl_->ready = false;
    impl_->startupFailure = nullptr;
    impl_->thread = std::thread([this] { impl_->run(); });

    impl_->readyCondition.wait(lock, [&] { return impl_->ready; });
    if (impl_->startupFailure) {
        auto failure = impl_->startupFailure;
        lock.unlock();
        // The worker has already returned after reporting startup failure.
        impl_->thread.join();
        std::rethrow_exception(failure);
    }
}

void WindowsDisplayChangeSource::stop() noexcept {
    std::thread worker;
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->thread.joinable()) {
            return;
        }
        if (impl_->threadId != 0) {
            PostThreadMessageW(impl_->threadId, WM_QUIT, 0, 0);
        }
        worker = std::move(impl_->thread);
    }
    if (worker.joinable()) {
        worker.join();
    }
}

bool WindowsDisplayChangeSource::running() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->active;
}

} // namespace desto::platform::windows
