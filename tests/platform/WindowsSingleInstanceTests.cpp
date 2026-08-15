#include "WindowsSingleInstanceGate.h"
#include "TestSupport.h"

#include <Windows.h>

#include <string>

using namespace desto::application;
using namespace desto::platform::windows;

namespace {

void RunTests() {
    const auto name = std::wstring(L"Local\\DestoTestSingleInstance-")
        + std::to_wstring(GetCurrentProcessId());
    WindowsSingleInstanceGate first(name);
    WindowsSingleInstanceGate second(name);

    DESTO_CHECK(first.acquire() == InstanceAcquireResult::Acquired);
    DESTO_CHECK(second.acquire() == InstanceAcquireResult::AlreadyRunning);
    first.release();
    DESTO_CHECK(second.acquire() == InstanceAcquireResult::Acquired);
    second.release();
}

} // namespace

int main() {
    return desto::test::Run(RunTests);
}
