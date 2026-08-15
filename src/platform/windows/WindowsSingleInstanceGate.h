#pragma once

#include <string>

#include "SingleInstance.h"

namespace desto::platform::windows {

class WindowsSingleInstanceGate final : public application::SingleInstanceGate {
public:
    explicit WindowsSingleInstanceGate(std::wstring name);
    ~WindowsSingleInstanceGate() override;

    [[nodiscard]] application::InstanceAcquireResult acquire() noexcept override;
    void release() noexcept override;

private:
    std::wstring name_;
    void* handle_ = nullptr;
};

} // namespace desto::platform::windows
