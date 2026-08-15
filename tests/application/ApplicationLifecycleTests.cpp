#include "ApplicationLifecycle.h"
#include "SingleInstance.h"
#include "TestSupport.h"

using namespace desto::application;

namespace {

void RunTests() {
    DiagnosticRecorder diagnostics(DiagnosticLevel::Debug, 4);
    MemorySingleInstanceDomain domain;
    MemorySingleInstanceGate firstGate(domain);
    MemorySingleInstanceGate secondGate(domain);

    ApplicationLifecycle first(firstGate, diagnostics);
    DESTO_CHECK(first.begin().applied);
    DESTO_CHECK(first.state() == LifecycleState::Starting);
    DESTO_CHECK(!first.runtimeReady().applied);
    DESTO_CHECK(first.configurationLoaded().applied);
    DESTO_CHECK(first.runtimeReady().applied);
    DESTO_CHECK(first.state() == LifecycleState::Running);

    ApplicationLifecycle duplicate(secondGate, diagnostics);
    const auto duplicateResult = duplicate.begin();
    DESTO_CHECK(!duplicateResult.applied);
    DESTO_CHECK(duplicateResult.error == LifecycleError::AlreadyRunning);
    DESTO_CHECK(duplicate.state() == LifecycleState::Stopped);
    DESTO_CHECK(domain.held());

    DESTO_CHECK(first.requestShutdown(ShutdownReason::Tray).applied);
    DESTO_CHECK(first.completeShutdown().applied);
    DESTO_CHECK(first.state() == LifecycleState::Stopped);
    DESTO_CHECK(!domain.held());
    DESTO_CHECK(!first.completeShutdown().applied);

    ApplicationLifecycle restarted(secondGate, diagnostics);
    DESTO_CHECK(restarted.begin().applied);
    DESTO_CHECK(restarted.configurationLoaded().applied);
    DESTO_CHECK(restarted.runtimeReady().applied);
    DESTO_CHECK(restarted.fail().applied);
    DESTO_CHECK(restarted.state() == LifecycleState::Failed);
    DESTO_CHECK(!domain.held());
    DESTO_CHECK(!restarted.fail().applied);

    DESTO_CHECK(diagnostics.entries().size() == 4);
    DESTO_CHECK(diagnostics.droppedCount() > 0);
    DESTO_CHECK(diagnostics.entries().back().code == "lifecycle.failed");

    DiagnosticRecorder filtered(DiagnosticLevel::Warning, 2);
    filtered.record(DiagnosticLevel::Info, "ignored.info");
    filtered.record(DiagnosticLevel::Warning, "visible.warning");
    DESTO_CHECK(filtered.entries().size() == 1);
    DESTO_CHECK(filtered.entries().front().code == "visible.warning");
}

} // namespace

int main() {
    return desto::test::Run(RunTests);
}
