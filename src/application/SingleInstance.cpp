#include "SingleInstance.h"

namespace desto::application {

InstanceAcquireResult MemorySingleInstanceGate::acquire() noexcept {
    if (acquired_) {
        return InstanceAcquireResult::Acquired;
    }
    if (domain_.held_) {
        return InstanceAcquireResult::AlreadyRunning;
    }
    domain_.held_ = true;
    acquired_ = true;
    return InstanceAcquireResult::Acquired;
}

void MemorySingleInstanceGate::release() noexcept {
    if (!acquired_) {
        return;
    }
    domain_.held_ = false;
    acquired_ = false;
}

} // namespace desto::application
