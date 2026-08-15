#pragma once

#include <functional>
#include <memory>

namespace desto::platform::windows {

class WindowsDisplayChangeSource final {
public:
    using Callback = std::function<void()>;

    // The callback runs on the source thread and must be short, non-blocking,
    // non-throwing, and must not call start() or stop().

    explicit WindowsDisplayChangeSource(Callback callback);
    ~WindowsDisplayChangeSource();

    WindowsDisplayChangeSource(const WindowsDisplayChangeSource&) = delete;
    WindowsDisplayChangeSource& operator=(const WindowsDisplayChangeSource&) = delete;

    void start();
    void stop() noexcept;
    [[nodiscard]] bool running() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace desto::platform::windows
