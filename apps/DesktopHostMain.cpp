#include "ApplicationLifecycle.h"
#include "ApplicationRuntime.h"
#include "Diagnostics.h"
#include "CardView.h"
#include "WindowsDesktopHost.h"
#include "WindowsDisplayTopology.h"
#include "WindowsSingleInstanceGate.h"

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace desto::application;
using namespace desto::domain;
using namespace desto::platform::windows;

namespace {

int DurationMilliseconds(std::wstring_view commandLine) {
    constexpr auto marker = std::wstring_view(L"--duration-ms ");
    const auto position = commandLine.find(marker);
    if (position == std::wstring_view::npos) {
        return 0;
    }
    try {
        return std::clamp(
            std::stoi(std::wstring(commandLine.substr(position + marker.size()))), 1000, 120000);
    } catch (...) {
        return 0;
    }
}

void SeedPreview(ApplicationRuntime& runtime, const std::vector<DisplaySnapshot>& displays) {
    const std::vector<CardId> ids{"preview-application", "preview-mapping", "preview-todo"};
    const auto application = runtime.execute(
        CreateApplicationCard{ids[0], std::filesystem::path("cards") / ids[0]});
    if (application.status == CommandStatus::Rejected) {
        throw std::runtime_error("Unable to create application preview Card.");
    }
    if (runtime.execute(CreateMappingCard{ids[1]}).status == CommandStatus::Rejected
        || runtime.execute(CreateTodoCard{ids[2]}).status == CommandStatus::Rejected) {
        throw std::runtime_error("Unable to create preview Card.");
    }
    for (std::size_t index = 0; index < ids.size(); ++index) {
        const auto placement = runtime.execute(SetPlacement{{
            .id = "preview-placement-" + ids[index],
            .cardId = ids[index],
            .target = DisplayTarget::all(),
            .rect = {48.0 + index * 360.0, 56.0 + index * 32.0, 320.0, 220.0},
            .zIndex = static_cast<std::int32_t>(index),
        }});
        if (placement.status == CommandStatus::Rejected) {
            throw std::runtime_error("Unable to create preview Placement.");
        }
    }
    auto cardSnapshots = runtime.cardSnapshots();
    for (auto& card : cardSnapshots) {
        switch (card.type) {
        case CardType::Application:
            card.appearance = {.preset = "white", .opacity = 0.96, .cornerRadius = 18.0};
            break;
        case CardType::Mapping:
            card.appearance = {.preset = "black", .opacity = 0.82, .cornerRadius = 24.0};
            break;
        case CardType::Todo:
            card.appearance = {.preset = "pearl-pink", .opacity = 0.92, .cornerRadius = 30.0};
            break;
        }
    }
    const auto workspace = runtime.workspace();
    runtime.restore(cardSnapshots, workspace);
    if (runtime.execute(UpdateDisplayTopology{displays}).status == CommandStatus::Rejected) {
        throw std::runtime_error("Unable to apply display topology.");
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR commandLine, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    DiagnosticRecorder diagnostics(DiagnosticLevel::Info, 64);
    WindowsSingleInstanceGate instance(L"Local\\Desto.DesktopHost");
    ApplicationLifecycle lifecycle(instance, diagnostics);
    try {
        const auto begin = lifecycle.begin();
        if (!begin.applied) {
            return begin.error == LifecycleError::AlreadyRunning ? 0 : 1;
        }
        if (!lifecycle.configurationLoaded().applied) {
            throw std::runtime_error("Configuration lifecycle transition failed.");
        }
        WindowsDisplayTopology topology;
        const auto displays = topology.snapshot();
        ApplicationRuntime runtime;
        SeedPreview(runtime, displays);
        std::vector<desto::presentation::CardView> cardViews;
        for (const auto* card : runtime.cards()) {
            cardViews.push_back(desto::presentation::MakeCardView(*card));
        }
        WindowsDesktopHost host;
        host.setPlacementChangedCallback(
            [&](const PlacementId& placementId, const CardId& cardId, const PlacementRect& rect) {
                const auto& placements = runtime.workspace().placements();
                const auto found = std::find_if(
                    placements.begin(), placements.end(), [&](const CardPlacement& placement) {
                        return placement.id == placementId && placement.cardId == cardId;
                    });
                if (found == placements.end()) {
                    diagnostics.record(DiagnosticLevel::Warning, "desktop.placement_missing");
                    return;
                }
                auto updated = *found;
                updated.rect = rect;
                if (runtime.execute(SetPlacement{std::move(updated)}).status
                    == CommandStatus::Rejected) {
                    diagnostics.record(DiagnosticLevel::Warning, "desktop.placement_rejected");
                }
            });
        host.present(runtime.projections(), displays, cardViews);
        if (!lifecycle.runtimeReady().applied) {
            throw std::runtime_error("Runtime lifecycle transition failed.");
        }
        host.run(DurationMilliseconds(commandLine));
        if (!lifecycle.requestShutdown(ShutdownReason::User).applied) {
            throw std::runtime_error("Shutdown request transition failed.");
        }
        if (!lifecycle.completeShutdown().applied) {
            throw std::runtime_error("Shutdown completion transition failed.");
        }
        return 0;
    } catch (...) {
        (void)lifecycle.fail();
        return 1;
    }
}
