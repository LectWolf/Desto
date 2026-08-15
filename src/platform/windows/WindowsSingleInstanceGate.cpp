#include "WindowsSingleInstanceGate.h"

#include <Windows.h>

#include <stdexcept>
#include <utility>

namespace desto::platform::windows {

WindowsSingleInstanceGate::WindowsSingleInstanceGate(std::wstring name)
    : name_(std::move(name)) {
    if (name_.empty()) {
        throw std::invalid_argument("Single-instance mutex name must not be empty.");
    }
}

WindowsSingleInstanceGate::~WindowsSingleInstanceGate() {
    release();
}

application::InstanceAcquireResult WindowsSingleInstanceGate::acquire() noexcept {
    if (handle_ != nullptr) {
        return application::InstanceAcquireResult::Acquired;
    }
    const auto handle = CreateMutexW(nullptr, TRUE, name_.c_str());
    if (handle == nullptr) {
        return application::InstanceAcquireResult::Failed;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(handle);
        return application::InstanceAcquireResult::AlreadyRunning;
    }
    handle_ = handle;
    return application::InstanceAcquireResult::Acquired;
}

void WindowsSingleInstanceGate::release() noexcept {
    if (handle_ == nullptr) {
        return;
    }
    ReleaseMutex(static_cast<HANDLE>(handle_));
    CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
}

} // namespace desto::platform::windows
