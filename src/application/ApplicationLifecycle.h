#pragma once

#include "Diagnostics.h"
#include "SingleInstance.h"

namespace desto::application {

enum class LifecycleState {
    Created,
    Starting,
    Running,
    ShutdownRequested,
    Stopped,
    Failed,
};

enum class LifecycleError {
    None,
    InvalidTransition,
    AlreadyRunning,
    InstanceAcquireFailed,
};

enum class ShutdownReason {
    User,
    Tray,
    System,
    StartupFailure,
};

struct LifecycleResult {
    bool applied = false;
    LifecycleError error = LifecycleError::None;
    LifecycleState state = LifecycleState::Created;
};

class ApplicationLifecycle {
public:
    ApplicationLifecycle(SingleInstanceGate& instance, DiagnosticRecorder& diagnostics) noexcept
        : instance_(instance), diagnostics_(diagnostics) {
    }
    ~ApplicationLifecycle();

    [[nodiscard]] LifecycleResult begin();
    [[nodiscard]] LifecycleResult configurationLoaded();
    [[nodiscard]] LifecycleResult runtimeReady();
    [[nodiscard]] LifecycleResult requestShutdown(ShutdownReason reason);
    [[nodiscard]] LifecycleResult completeShutdown();
    [[nodiscard]] LifecycleResult fail();

    [[nodiscard]] LifecycleState state() const noexcept { return state_; }

private:
    void releaseInstance() noexcept;
    [[nodiscard]] LifecycleResult invalid() const noexcept;
    [[nodiscard]] LifecycleResult applied();

    SingleInstanceGate& instance_;
    DiagnosticRecorder& diagnostics_;
    LifecycleState state_ = LifecycleState::Created;
    bool instanceAcquired_ = false;
    bool configurationLoaded_ = false;
};

} // namespace desto::application
