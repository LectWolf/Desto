#include "ApplicationLifecycle.h"

namespace desto::application {

ApplicationLifecycle::~ApplicationLifecycle() {
    releaseInstance();
}

LifecycleResult ApplicationLifecycle::begin() {
    if (state_ != LifecycleState::Created) {
        return invalid();
    }
    switch (instance_.acquire()) {
    case InstanceAcquireResult::Acquired:
        instanceAcquired_ = true;
        state_ = LifecycleState::Starting;
        diagnostics_.record(DiagnosticLevel::Info, "lifecycle.starting");
        return applied();
    case InstanceAcquireResult::AlreadyRunning:
        state_ = LifecycleState::Stopped;
        diagnostics_.record(DiagnosticLevel::Warning, "lifecycle.duplicate_instance");
        return {false, LifecycleError::AlreadyRunning, state_};
    case InstanceAcquireResult::Failed:
        state_ = LifecycleState::Failed;
        diagnostics_.record(DiagnosticLevel::Error, "lifecycle.instance_acquire_failed");
        return {false, LifecycleError::InstanceAcquireFailed, state_};
    }
    return {false, LifecycleError::InstanceAcquireFailed, state_};
}

LifecycleResult ApplicationLifecycle::configurationLoaded() {
    if (state_ != LifecycleState::Starting || configurationLoaded_) {
        return invalid();
    }
    configurationLoaded_ = true;
    diagnostics_.record(DiagnosticLevel::Info, "lifecycle.configuration_loaded");
    return applied();
}

LifecycleResult ApplicationLifecycle::runtimeReady() {
    if (state_ != LifecycleState::Starting || !configurationLoaded_) {
        return invalid();
    }
    state_ = LifecycleState::Running;
    diagnostics_.record(DiagnosticLevel::Info, "lifecycle.running");
    return applied();
}

LifecycleResult ApplicationLifecycle::requestShutdown(ShutdownReason reason) {
    if (state_ != LifecycleState::Starting && state_ != LifecycleState::Running) {
        return invalid();
    }
    state_ = LifecycleState::ShutdownRequested;
    switch (reason) {
    case ShutdownReason::User:
        diagnostics_.record(DiagnosticLevel::Info, "lifecycle.shutdown_user");
        break;
    case ShutdownReason::Tray:
        diagnostics_.record(DiagnosticLevel::Info, "lifecycle.shutdown_tray");
        break;
    case ShutdownReason::System:
        diagnostics_.record(DiagnosticLevel::Info, "lifecycle.shutdown_system");
        break;
    case ShutdownReason::StartupFailure:
        diagnostics_.record(DiagnosticLevel::Warning, "lifecycle.shutdown_startup_failure");
        break;
    }
    return applied();
}

LifecycleResult ApplicationLifecycle::completeShutdown() {
    if (state_ != LifecycleState::ShutdownRequested) {
        return invalid();
    }
    releaseInstance();
    state_ = LifecycleState::Stopped;
    diagnostics_.record(DiagnosticLevel::Info, "lifecycle.stopped");
    return applied();
}

LifecycleResult ApplicationLifecycle::fail() {
    if (state_ == LifecycleState::Stopped || state_ == LifecycleState::Failed) {
        return invalid();
    }
    releaseInstance();
    state_ = LifecycleState::Failed;
    diagnostics_.record(DiagnosticLevel::Error, "lifecycle.failed");
    return applied();
}

void ApplicationLifecycle::releaseInstance() noexcept {
    if (!instanceAcquired_) {
        return;
    }
    instance_.release();
    instanceAcquired_ = false;
}

LifecycleResult ApplicationLifecycle::invalid() const noexcept {
    return {false, LifecycleError::InvalidTransition, state_};
}

LifecycleResult ApplicationLifecycle::applied() {
    return {true, LifecycleError::None, state_};
}

} // namespace desto::application
