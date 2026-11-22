/// @file test_ProcessesPanelRenderColumns.cpp
/// @brief The Processes panel run headless (#1546): columns this system can't fill, the Signature and Services columns, the startup
/// selection and the primary row, the filter, list and tree, column layout that survives a restart, and collapsed parents. Split from
/// test_ProcessesPanelRender.cpp by feature (#3850); the fixture is in ProcessesPanelRenderFixture.h.

#include "App/Panels/AuthorizeWording.h"
#include "App/Panels/ExitingProcessRows.h"
#include "App/Panels/FindHandlesDialog.h"
#include "App/Panels/NiceStep.h"
#include "App/Panels/PriorityChangesInFlight.h"
#include "App/Panels/ProcessActionsView.h"
#include "App/Panels/ProcessColumnAvailability.h"
#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "App/Panels/ProcessDumpView.h"
#include "App/Panels/ProcessGrouping.h"
#include "App/Panels/ProcessPins.h"
#include "App/Panels/ProcessRowFormat.h"
#include "App/Panels/ProcessRowMeter.h"
#include "App/Panels/ProcessSignatureText.h"
#include "App/Panels/ProcessTreeIndent.h"
#include "App/Panels/ProcessWaitChainDialog.h"
#include "App/Panels/ProcessesPanel.h"
#include "App/Panels/StuckProcesses.h"
#include "App/Panels/UserGrouping.h"
#include "App/Panels/UserSessionsFeed.h"
#include "App/ProcessColumnConfig.h"
#include "App/SelectOverride.h"
#include "App/UdpCountingStatus.h"
#include "App/UserConfig.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Domain/GPUModel.h"
#include "Domain/HandleSearch.h"
#include "Domain/PriorityConfig.h"
#include "Domain/ProcessModel.h"
#include "Domain/ProcessSnapshot.h"
#include "Domain/SamplingConfig.h"
#include "Domain/ServiceModel.h"
#include "Mocks/FakeClipboard.h"
#include "Mocks/FakeHttpsClient.h"
#include "Mocks/FakeUserSessions.h"
#include "Mocks/MockGPUProbe.h"
#include "Mocks/MockProbes.h"
#include "Platform/CpuAffinity.h"
#include "Platform/GPUTypes.h"
#include "Platform/IHttpsClient.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/IServiceProbe.h"
#include "Platform/IUserSessions.h"
#include "Platform/ImageSignature.h"
#include "Platform/ProcessTypes.h"
#include "Platform/SchedPolicy.h"
#include "Platform/WaitChain.h"
#include "ProcessesPanelRenderFixture.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>
#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <latch>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace App
{
namespace
{

// #1395: a text column this system can't fill (the mock probe reports no publisher, type, container or
// per-process GPU) reads as the unavailable dash in every row when the user has chosen to show it.
TEST_F(ProcessesPanelRenderTest, ChosenColumnsThisSystemCantFillShowTheDash)
{
    ProcessColumnSettings& columns = UserConfig::get().settings().processColumns;
    for (const ProcessColumn col : {
             ProcessColumn::Publisher,
             ProcessColumn::Type,
             ProcessColumn::Architecture,
             ProcessColumn::Container,
             ProcessColumn::GpuEngine,
             ProcessColumn::GpuDevice,
         })
    {
        columns.setVisible(col, true);
    }
    const auto panel = attachedPanel();
    renderUntilLoaded(*panel);
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("Publisher")) << text;
    EXPECT_TRUE(text.contains("Container")) << text;
    EXPECT_TRUE(text.contains(ProcessRowFormat::UNAVAILABLE_CELL_TEXT)) << text;
    panel->onDetach();
}

// #1891: where the probe classifies processes (Windows), the Type column shows each one's type in its
// colour, and the dash for a process it couldn't classify.
TEST_F(ProcessesPanelRenderTest, TheTypeColumnShowsEachProcesssTypeOrTheDash)
{
    UserConfig::get().settings().processColumns.setVisible(ProcessColumn::Type, true);
    const auto panel = std::make_unique<ProcessesPanel>(
        []
        {
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            Platform::ProcessCounters app = TestMocks::makeProcessCounters(100, "server", 'R', 200, 50, 1000, std::uint64_t{8} << 20U, 1);
            app.processType = "Background Process";
            probe->withProcess(std::move(app))
                .withProcess(TestMocks::makeProcessCounters(101, "worker", 'S', 30, 10, 1100, std::uint64_t{2} << 20U, 100));
            Platform::ProcessCapabilities caps = probe->capabilities();
            caps.hasProcessType = true;
            probe->setCapabilities(caps);
            return ProcessesPanelPlatform{
                .probe = std::move(probe),
                .actions = std::make_unique<TestMocks::MockProcessActions>(),
                .icons = nullptr,
            };
        },
        std::function<void(Core::Event&)>{});
    attachAndSeed(*panel);
    Core::ActiveTabChangedEvent shown("Processes");
    panel->onEvent(shown);
    renderUntilLoaded(*panel);
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("Background Process")) << text;
    EXPECT_TRUE(text.contains(ProcessRowFormat::UNAVAILABLE_CELL_TEXT)) << "worker isn't classified:\n" << text;
    panel->onDetach();
}

// #2394: the Architecture column shows the architecture each process runs as, and the dash for one whose
// architecture couldn't be read.
TEST_F(ProcessesPanelRenderTest, TheArchitectureColumnShowsEachProcesssArchitectureOrTheDash)
{
    UserConfig::get().settings().processColumns.setVisible(ProcessColumn::Architecture, true);
    const auto panel = std::make_unique<ProcessesPanel>(
        []
        {
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            Platform::ProcessCounters emulated =
                TestMocks::makeProcessCounters(100, "server", 'R', 200, 50, 1000, std::uint64_t{8} << 20U, 1);
            emulated.architecture = "x64 (emulated)";
            probe->withProcess(std::move(emulated))
                .withProcess(TestMocks::makeProcessCounters(101, "worker", 'S', 30, 10, 1100, std::uint64_t{2} << 20U, 100));
            Platform::ProcessCapabilities caps = probe->capabilities();
            caps.hasArchitecture = true;
            probe->setCapabilities(caps);
            return ProcessesPanelPlatform{
                .probe = std::move(probe),
                .actions = std::make_unique<TestMocks::MockProcessActions>(),
                .icons = nullptr,
            };
        },
        std::function<void(Core::Event&)>{});
    attachAndSeed(*panel);
    Core::ActiveTabChangedEvent shown("Processes");
    panel->onEvent(shown);
    renderUntilLoaded(*panel);
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("Architecture")) << text;
    EXPECT_TRUE(text.contains("x64 (emulated)")) << text;
    EXPECT_TRUE(text.contains(ProcessRowFormat::UNAVAILABLE_CELL_TEXT)) << "worker's isn't read:\n" << text;
    panel->onDetach();
    UserConfig::get().settings().processColumns.setVisible(ProcessColumn::Architecture, false);
}

// #3086, #3244: the Policy column words each process's scheduling policy from the snapshot's policy and
// real-time priority where it draws the cell -- "Normal", "Batch", "FIFO 50", "RR 10" -- and shows the dash
// for one whose policy couldn't be read.
TEST_F(ProcessesPanelRenderTest, ThePolicyCellIsFormattedWhenDrawnFromTheStoredPolicy)
{
    UserConfig::get().settings().processColumns.setVisible(ProcessColumn::Policy, true);
    const auto panel = std::make_unique<ProcessesPanel>(
        []
        {
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            const auto withPolicy = [](std::int32_t pid, const char* name, Platform::SchedPolicy policy, std::uint32_t rtPriority)
            {
                Platform::ProcessCounters counters =
                    TestMocks::makeProcessCounters(pid, name, 'S', 30, 10, 1000, std::uint64_t{2} << 20U, 1);
                counters.schedPolicy = policy;
                counters.rtPriority = rtPriority;
                return counters;
            };
            probe->withProcess(withPolicy(100, "shell", Platform::SchedPolicy::Normal, 0))
                .withProcess(withPolicy(101, "indexer", Platform::SchedPolicy::Batch, 0))
                .withProcess(withPolicy(102, "pipewire", Platform::SchedPolicy::Fifo, 50))
                .withProcess(withPolicy(103, "jackd", Platform::SchedPolicy::RoundRobin, 10))
                .withProcess(withPolicy(104, "unread", Platform::SchedPolicy::Unknown, 0));
            Platform::ProcessCapabilities caps = probe->capabilities();
            caps.hasSchedPolicy = true;
            probe->setCapabilities(caps);
            return ProcessesPanelPlatform{
                .probe = std::move(probe),
                .actions = std::make_unique<TestMocks::MockProcessActions>(),
                .icons = nullptr,
            };
        },
        std::function<void(Core::Event&)>{});
    attachAndSeed(*panel);
    Core::ActiveTabChangedEvent shown("Processes");
    panel->onEvent(shown);
    renderUntilLoaded(*panel);
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("Policy")) << text;
    EXPECT_TRUE(text.contains("Normal")) << text;
    EXPECT_TRUE(text.contains("Batch")) << text;
    EXPECT_TRUE(text.contains("FIFO 50")) << text;
    EXPECT_TRUE(text.contains("RR 10")) << text;
    EXPECT_TRUE(text.contains(ProcessRowFormat::UNAVAILABLE_CELL_TEXT)) << "unread's isn't read:\n" << text;
    panel->onDetach();
    UserConfig::get().settings().processColumns.setVisible(ProcessColumn::Policy, false);
}

// #3070: the Signature column shows a check mark and the signer for a verified image, Unsigned and
// Invalid for the rest, "Not checked" for a check that failed, and the dash while one is to come; hovering
// a cell says how it is signed. Where the probe checks no signatures (Linux) every row is the dash.
TEST_F(ProcessesPanelRenderTest, TheSignatureColumnShowsTheVerifiedSignerOrWhyNot)
{
    UserConfig::get().settings().processColumns.setVisible(ProcessColumn::Signature, true);
    const auto panel = std::make_unique<ProcessesPanel>(
        []
        {
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            Platform::ProcessCounters system =
                TestMocks::makeProcessCounters(100, "explorer", 'R', 200, 50, 1000, std::uint64_t{8} << 20U, 1);
            system.signature =
                Platform::SignatureCheck{.signature = Platform::ImageSignature::Catalog, .note = {}, .signer = "Microsoft Windows"};
            Platform::ProcessCounters homebrew =
                TestMocks::makeProcessCounters(101, "homebrew", 'S', 30, 10, 1100, std::uint64_t{2} << 20U, 100);
            homebrew.signature = Platform::SignatureCheck{.signature = Platform::ImageSignature::Unsigned, .note = {}, .signer = {}};
            Platform::ProcessCounters tampered =
                TestMocks::makeProcessCounters(102, "tampered", 'S', 30, 10, 1200, std::uint64_t{2} << 20U, 100);
            tampered.signature =
                Platform::SignatureCheck{.signature = Platform::ImageSignature::Untrusted, .note = "bad digest", .signer = {}};
            Platform::ProcessCounters locked =
                TestMocks::makeProcessCounters(103, "locked", 'S', 30, 10, 1300, std::uint64_t{2} << 20U, 100);
            locked.signature =
                Platform::SignatureCheck{.signature = Platform::ImageSignature::Unknown, .note = "Access is denied", .signer = {}};
            probe->withProcess(std::move(system))
                .withProcess(std::move(homebrew))
                .withProcess(std::move(tampered))
                .withProcess(std::move(locked))
                .withProcess(TestMocks::makeProcessCounters(104, "pending", 'S', 30, 10, 1400, std::uint64_t{2} << 20U, 100));
            Platform::ProcessCapabilities caps = probe->capabilities();
            caps.hasSignature = true;
            probe->setCapabilities(caps);
            return ProcessesPanelPlatform{
                .probe = std::move(probe),
                .actions = std::make_unique<TestMocks::MockProcessActions>(),
                .icons = nullptr,
            };
        },
        std::function<void(Core::Event&)>{});
    attachAndSeed(*panel);
    Core::ActiveTabChangedEvent shown("Processes");
    panel->onEvent(shown);
    renderUntilLoaded(*panel);
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("Signature")) << text;
    EXPECT_TRUE(text.contains(ProcessSignatureText::VERIFIED_MARK)) << text;
    EXPECT_TRUE(text.contains("Microsoft Windows")) << text;
    EXPECT_TRUE(text.contains("Unsigned")) << text;
    EXPECT_TRUE(text.contains("Invalid")) << text;
    EXPECT_TRUE(text.contains("Not checked")) << text;
    EXPECT_TRUE(text.contains(ProcessRowFormat::UNAVAILABLE_CELL_TEXT)) << "pending isn't checked yet:\n" << text;

    // Hovering the column's cells shows each one's detail (the tooltip waits for the mouse to rest, so the
    // hover delays are zeroed and the column swept).
    ImGuiStyle& style = ImGui::GetStyle();
    style.HoverStationaryDelay = 0.0F;
    style.HoverDelayShort = 0.0F;
    style.HoverDelayNormal = 0.0F;
    const ImGuiTable* table = nullptr;
    int signatureColumn = -1;
    for (int t = 0; t < GImGui->Tables.GetMapSize() && signatureColumn < 0; ++t)
    {
        const ImGuiTable* candidate = GImGui->Tables.TryGetMapData(t);
        for (int c = 0; candidate != nullptr && c < candidate->ColumnsCount; ++c)
        {
            if (std::string_view(ImGui::TableGetColumnName(candidate, c)) == "Signature")
            {
                table = candidate;
                signatureColumn = c;
                break;
            }
        }
    }
    ASSERT_NE(table, nullptr) << "no table with a Signature column";
    const ImGuiTableColumn& column = table->Columns[signatureColumn];
    const float x = column.MinX + 8.0F; // inside each cell's text, however short
    bool verifiedTip = false;
    bool invalidTip = false;
    ImGuiIO& io = ImGui::GetIO();
    const int steps = static_cast<int>(table->InnerRect.GetHeight() / 4.0F);
    for (int step = 0; step < steps && !(verifiedTip && invalidTip); ++step)
    {
        io.AddMousePosEvent(x, table->InnerRect.Min.y + 2.0F + (static_cast<float>(step) * 4.0F));
        for (int settle = 0; settle < 3; ++settle)
        {
            const std::string hovered = renderAndCapture(*panel);
            verifiedTip = verifiedTip || hovered.contains("Verified:");
            invalidTip = invalidTip || hovered.contains("bad digest");
        }
    }
    EXPECT_TRUE(verifiedTip) << "the verified cell's tooltip";
    EXPECT_TRUE(invalidTip) << "the invalid cell's tooltip, with its reason";
    panel->onDetach();
    UserConfig::get().settings().processColumns.setVisible(ProcessColumn::Signature, false);
}

TEST_F(ProcessesPanelRenderTest, TheSignatureColumnIsTheDashWhereSignaturesArentChecked)
{
    UserConfig::get().settings().processColumns.setVisible(ProcessColumn::Signature, true);
    const auto panel = attachedPanel(); // the mock probe checks none, as on Linux
    renderUntilLoaded(*panel);
    EXPECT_FALSE(panel->processCapabilities().hasSignature);
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("Signature")) << text;
    EXPECT_TRUE(text.contains(ProcessRowFormat::UNAVAILABLE_CELL_TEXT)) << text;
    EXPECT_FALSE(text.contains("Unsigned")) << text;
    panel->onDetach();
    UserConfig::get().settings().processColumns.setVisible(ProcessColumn::Signature, false);
}

// #1879: where the probe fills neither (Windows; the mock reports neither), the Private and Swap columns
// read as the dash in every row, and their headers and cells say why.
TEST_F(ProcessesPanelRenderTest, PrivateAndSwapWithoutTheirFiguresShowTheDashAndSayWhy)
{
    ProcessColumnSettings& columns = UserConfig::get().settings().processColumns;
    for (const ProcessColumn col : allProcessColumns())
    {
        if (getColumnInfo(col).canHide)
        {
            // PID, Name, Private and Swap: both headers are in reach
            columns.setVisible(col, col == ProcessColumn::Private || col == ProcessColumn::Swap);
        }
    }
    const auto panel = attachedPanel();
    renderUntilLoaded(*panel);
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("Private")) << text;
    EXPECT_TRUE(text.contains("Swap")) << text;
    EXPECT_TRUE(text.contains(ProcessRowFormat::UNAVAILABLE_CELL_TEXT)) << text;

    // Over each point down the header and the first rows, across the whole width (the last column
    // stretches, and its right-aligned dash sits at the far end). Tooltips show without a hover delay,
    // so a few frames at each point are enough.
    ImGuiStyle& style = ImGui::GetStyle();
    style.HoverDelayShort = 0.0F;
    style.HoverDelayNormal = 0.0F;
    style.HoverStationaryDelay = 0.0F;
    const auto hoverUntilShown = [&panel](std::string_view wanted)
    {
        ImGuiIO& io = ImGui::GetIO();
        for (int y = 20; y <= 140; y += 6)
        {
            for (int x = 10; x <= static_cast<int>(DISPLAY_WIDTH); x += 6)
            {
                io.AddMousePosEvent(static_cast<float>(x), static_cast<float>(y));
                for (int frame = 0; frame < 3; ++frame)
                {
                    if (renderAndCapture(*panel).contains(wanted))
                    {
                        return true;
                    }
                }
            }
        }
        return false;
    };
    EXPECT_TRUE(hoverUntilShown(ProcessColumnAvailability::PRIVATE_UNSUPPORTED_COLUMN_NOTE)) << "Private's header tooltip";
    EXPECT_TRUE(hoverUntilShown(ProcessRowFormat::PRIVATE_UNSUPPORTED_CELL_REASON)) << "a Private cell's tooltip";
    EXPECT_TRUE(hoverUntilShown(ProcessColumnAvailability::SWAP_UNSUPPORTED_COLUMN_NOTE)) << "Swap's header tooltip";
    EXPECT_TRUE(hoverUntilShown(ProcessRowFormat::SWAP_UNSUPPORTED_CELL_REASON)) << "a Swap cell's tooltip";
    panel->onDetach();
}

// #4754: a process no account could be read for (on Windows, another account's, without administrator rights) shows
// the dash in User, not a blank, and hovering it says why. The fixture's mock processes have no user.
TEST_F(ProcessesPanelRenderTest, AUserNoAccountWasReadForShowsTheDashAndSaysWhy)
{
    ProcessColumnSettings& columns = UserConfig::get().settings().processColumns;
    for (const ProcessColumn col : allProcessColumns())
    {
        if (getColumnInfo(col).canHide)
        {
            columns.setVisible(col, col == ProcessColumn::User);
        }
    }
    const auto panel = attachedPanel();
    renderUntilLoaded(*panel);
    EXPECT_TRUE(renderAndCapture(*panel).contains(ProcessRowFormat::UNAVAILABLE_CELL_TEXT));

    ImGuiStyle& style = ImGui::GetStyle();
    style.HoverDelayShort = 0.0F;
    style.HoverDelayNormal = 0.0F;
    style.HoverStationaryDelay = 0.0F;
    bool shown = false;
    ImGuiIO& io = ImGui::GetIO();
    for (int y = 20; y <= 140 && !shown; y += 6)
    {
        for (int x = 10; x <= static_cast<int>(DISPLAY_WIDTH) && !shown; x += 6)
        {
            io.AddMousePosEvent(static_cast<float>(x), static_cast<float>(y));
            for (int frame = 0; frame < 3 && !shown; ++frame)
            {
                shown = renderAndCapture(*panel).contains(ProcessRowFormat::USER_UNREADABLE_CELL_REASON);
            }
        }
    }
    EXPECT_TRUE(shown) << "a blank User cell's tooltip";
    panel->onDetach();
}

// #1891: with per-process GPU data supported but not read yet for a process (it was first listed after
// the GPU sampler's last read), the GPU Engine and GPU Device cells read as the dash, and hovering one
// says why -- not the generic "couldn't be read" an empty list would get.
TEST_F(ProcessesPanelRenderTest, GpuTextCellsNotReadYetSayWhyOnHover)
{
    ProcessColumnSettings& columns = UserConfig::get().settings().processColumns;
    columns.setVisible(ProcessColumn::GpuEngine, true);
    columns.setVisible(ProcessColumn::GpuDevice, true);
    // The GPU sampler reads before the panel lists any process, so every process is listed after the
    // read: it may have started since, so its GPU fields are unread (#1210).
    const std::shared_ptr<Domain::GPUModel> gpu = gpuModel(false);
    gpu->refresh();
    std::this_thread::sleep_for(std::chrono::milliseconds(5)); // listed strictly after the read
    const auto panel = attachedPanel({}, gpu);
    panel->processModel()->setGPUModel(gpu); // as ShellLayer wires both
    panel->processModel()->refresh();
    renderUntilLoaded(*panel);
    ASSERT_TRUE(renderAndCapture(*panel).contains(ProcessRowFormat::UNAVAILABLE_CELL_TEXT));

    ImGuiStyle& style = ImGui::GetStyle();
    style.HoverDelayShort = 0.0F;
    style.HoverDelayNormal = 0.0F;
    style.HoverStationaryDelay = 0.0F;
    bool shown = false;
    ImGuiIO& io = ImGui::GetIO();
    for (int y = 20; y <= 140 && !shown; y += 6)
    {
        for (int x = 10; x <= static_cast<int>(DISPLAY_WIDTH) && !shown; x += 6)
        {
            io.AddMousePosEvent(static_cast<float>(x), static_cast<float>(y));
            for (int frame = 0; frame < 3 && !shown; ++frame)
            {
                shown = renderAndCapture(*panel).contains(ProcessRowFormat::GPU_NOT_READ_YET_CELL_REASON);
            }
        }
    }
    EXPECT_TRUE(shown) << "a GPU text cell's not-read-yet reason";
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    panel->onDetach();
}

// #1880: the Services column, once a service list with PIDs is attached, lists each process's services
// comma-joined -- blank for one hosting none -- and sorts by that text.
TEST_F(ProcessesPanelRenderTest, TheServicesColumnListsEachProcesssServices)
{
    UserConfig::get().settings().processColumns.setVisible(ProcessColumn::Services, true);
    const auto panel = attachedPanel();
    renderUntilLoaded(*panel);
    // No service list yet: this system can't tell (the mock probe has no systemd units either).
    EXPECT_FALSE(panel->processCapabilities().hasServices);

    Platform::ServiceCapabilities listCaps;
    listCaps.canEnumerate = true;
    listCaps.hasPid = true;
    auto serviceProbe = std::make_unique<TestMocks::MockServiceProbe>(listCaps);
    Platform::ServiceInfo dhcp;
    dhcp.name = "Dhcp";
    dhcp.pid = 100;
    Platform::ServiceInfo eventLog;
    eventLog.name = "EventLog";
    eventLog.pid = 100;
    serviceProbe->setEnumeration(Platform::ServiceEnumeration::succeeded({eventLog, dhcp}));
    const auto services = std::make_shared<Domain::ServiceModel>(std::move(serviceProbe));
    services->sample();
    panel->processModel()->setServiceModel(services);
    panel->processModel()->refresh(); // the sampler's next refresh, now

    std::string text;
    int frame = 0;
    while (!text.contains("Dhcp, EventLog") && frame < LOAD_FRAMES)
    {
        text = renderAndCapture(*panel);
        ++frame;
    }
    EXPECT_TRUE(text.contains("Services")) << text;
    EXPECT_TRUE(text.contains("Dhcp, EventLog")) << text; // the list's name order
    EXPECT_TRUE(panel->processCapabilities().hasServices);

    // Hovering the cell lists them one per line under a count (#1891); the tooltip waits for the mouse
    // to rest, so the hover delays are zeroed and the rows swept.
    ImGuiStyle& style = ImGui::GetStyle();
    style.HoverStationaryDelay = 0.0F;
    style.HoverDelayShort = 0.0F;
    style.HoverDelayNormal = 0.0F;
    const std::string heading = ProcessRowFormat::servicesTooltipHeading(2);
    // The Services column's cells, from the table ImGui laid out: sweep down that column only, so the
    // row's position (it follows the sort) doesn't matter, and few frames are drawn (a slow ASan run).
    const ImGuiTable* table = nullptr;
    int servicesColumn = -1;
    for (int t = 0; t < GImGui->Tables.GetMapSize() && servicesColumn < 0; ++t)
    {
        const ImGuiTable* candidate = GImGui->Tables.TryGetMapData(t);
        for (int c = 0; candidate != nullptr && c < candidate->ColumnsCount; ++c)
        {
            if (std::string_view(ImGui::TableGetColumnName(candidate, c)) == "Services")
            {
                table = candidate;
                servicesColumn = c;
                break;
            }
        }
    }
    ASSERT_NE(table, nullptr) << "no table with a Services column";
    const ImGuiTableColumn& column = table->Columns[servicesColumn];
    const float x = (column.MinX + column.MaxX) * 0.5F;
    bool shown = false;
    ImGuiIO& io = ImGui::GetIO();
    const int steps = static_cast<int>(table->InnerRect.GetHeight() / 4.0F);
    for (int step = 0; step < steps && !shown; ++step)
    {
        io.AddMousePosEvent(x, table->InnerRect.Min.y + 2.0F + (static_cast<float>(step) * 4.0F));
        for (int settle = 0; settle < 3 && !shown; ++settle)
        {
            const std::string hovered = renderAndCapture(*panel);
            shown = hovered.contains(heading) && hovered.contains("\nDhcp") && hovered.contains("\nEventLog");
        }
    }
    EXPECT_TRUE(shown) << "the Services cell's tooltip: \"" << heading << "\", then each name";
    panel->onDetach();
}

// #1395: "Reset columns" brings back this system's default columns and their default order.
TEST_F(ProcessesPanelRenderTest, ResetColumnsBringsBackTheDefaults)
{
    UserConfig::get().settings().processColumns.setVisible(ProcessColumn::Threads, true);
    const auto panel = attachedPanel();
    renderUntilLoaded(*panel);
    ASSERT_TRUE(renderAndCapture(*panel).contains("Threads"));

    const ImGuiWindow* shell = ImGui::FindWindowByName("Shell");
    ASSERT_NE(shell, nullptr);
    ImGui::ActivateItemByID(ImHashStr(ICON_FA_TABLE_COLUMNS " Columns", 0, shell->ID));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    ASSERT_NE(topPopup(), nullptr);
    pressPopupItem(*panel, ICON_FA_ROTATE_LEFT " Reset columns");
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_FALSE(renderAndCapture(*panel).contains("Threads")); // hidden by default again
    EXPECT_FALSE(UserConfig::get().settings().processColumns.isVisible(ProcessColumn::Threads));
    panel->onDetach();
}

TEST_F(ProcessesPanelRenderTest, CtrlAThenF9KillsEverySelectedProcessAfterOneConfirmation)
{
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = attachedPanelWithActions([](Core::Event&) {}, actions);
    ASSERT_NE(actions, nullptr);
    renderUntilLoaded(*panel);
    ASSERT_EQ(panel->processCount(), 3U);

    selectAll(*panel);
    EXPECT_EQ(panel->selectionCount(), 3U);

    panel->requestKillSelected(); // F9, as ShellLayer hands it to the tab on show
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
    ASSERT_NE(modal, nullptr);
    EXPECT_TRUE(std::string_view(modal->Name).contains('3')) << modal->Name; // one dialog for all three
    EXPECT_EQ(actions->killCount(), 0);

    pressModalFilledButton(*panel, "Kill");
    EXPECT_EQ(ImGui::GetTopMostPopupModal(), nullptr);
    // On the batch's worker (#2142): the toolbar's line comes on a later frame.
    ASSERT_TRUE(updateUntilBatchResult(*panel, "Kill sent to 3 processes"));
    EXPECT_EQ(actions->killCount(), 3); // each, by identity
    panel->onDetach();
}

TEST_F(ProcessesPanelRenderTest, TheRowMenuOnASelectionSetsOnePriorityForAllOfIt)
{
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = attachedPanelWithActions([](Core::Event&) {}, actions);
    ASSERT_NE(actions, nullptr);
    renderUntilLoaded(*panel);
    selectAll(*panel);
    ASSERT_EQ(panel->selectionCount(), 3U);

    // A right-click on a row of the selection keeps it, and the menu acts on all of it.
    ASSERT_TRUE(openARowMenu(*panel).has_value());
    EXPECT_EQ(panel->selectionCount(), 3U);
    pressPopupItem(*panel, std::format("{} Set priority for 3 processes...###SetPriority", ICON_FA_GAUGE_HIGH));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    ASSERT_NE(ImGui::GetTopMostPopupModal(), nullptr);
    EXPECT_EQ(actions->setPriorityCount(), 0);

    // The dialog lists them, so it is the confirmation (#1539): Apply sets the value on each.
    pressModalFilledButton(*panel, "Apply to 3 processes");
    EXPECT_EQ(ImGui::GetTopMostPopupModal(), nullptr);
    ASSERT_TRUE(updateUntilBatchResult(*panel, "Priority set to")); // from the batch's worker (#2142)
    EXPECT_EQ(actions->setPriorityCount(), 3);
    panel->onDetach();
}

TEST_F(ProcessesPanelRenderTest, TheStartupSelectionPicksItsProcessByPidAndOpensDetails)
{
    SelectionSink events;
    const auto panel = attachedPanel(events.sink());
    panel->requestStartupSelection(SelectOverride::Target{.pid = 101, .name = {}, .tab = SelectOverride::DetailsTab::Network}, true);
    EXPECT_FALSE(panel->takeStartupDetailsTab().has_value()); // nothing until it fires
    updateUntilLoaded(*panel);

    EXPECT_EQ(panel->selectedPid(), 101);
    EXPECT_EQ(panel->selectionCount(), 1U);
    ASSERT_FALSE(events.selected.empty());
    EXPECT_EQ(events.selected.back(), 101);
    EXPECT_EQ(events.showDetails, 1); // as the row menu's Details
    // The Details tab it asked for, handed over once (TASKSMACK_DETAILS_TAB).
    EXPECT_EQ(panel->takeStartupDetailsTab(), std::optional<SelectOverride::DetailsTab>(SelectOverride::DetailsTab::Network));
    EXPECT_FALSE(panel->takeStartupDetailsTab().has_value());
    panel->onDetach();
}

TEST_F(ProcessesPanelRenderTest, TheStartupSelectionFindsAProcessByName)
{
    SelectionSink events;
    const auto panel = attachedPanel(events.sink());
    panel->requestStartupSelection(SelectOverride::Target{.pid = std::nullopt, .name = "server", .tab = {}}, false);
    updateUntilLoaded(*panel);

    EXPECT_EQ(panel->selectedPid(), 100);
    EXPECT_EQ(events.showDetails, 0); // not asked to open Details
    panel->onDetach();
}

// #2731: the sampler runs at the refresh setting in every UI state. Settings is the one thing that sets
// it. A tab change (to a tab with process data or without), frames at any rate, and the slow frames of
// a minimized window change nothing. Nor can a resize or move: onUpdate() no longer asks whether one is
// on, so there is no state through which it could.
TEST_F(ProcessesPanelRenderTest, TheSamplerIntervalIsTheRefreshSettingInEveryUiState)
{
    const auto panel = attachedPanel();
    const auto setting = std::chrono::milliseconds(Domain::Sampling::REFRESH_INTERVAL_DEFAULT_MS);
    EXPECT_EQ(panel->samplingInterval(), setting);

    const auto expectThroughEveryUiState = [&panel](std::chrono::milliseconds expected)
    {
        // Each main tab shown in turn, with a frame on each: tabs with process data and tabs without.
        for (const char* tab : {"Processes", "SystemInfo", "SystemOverview", "Services", "ProcessDetails", "Startup", "Processes"})
        {
            Core::ActiveTabChangedEvent shown(tab);
            panel->onEvent(shown);
            panel->onUpdate(1.0F / 60.0F);
            EXPECT_EQ(panel->samplingInterval(), expected) << tab;
        }
        // Frames at the display rate of a resize or move, at 60 Hz, at the 5 Hz of a minimized window,
        // and after the gap of a window left minimized or covered.
        for (const float deltaTime : {0.002F, 1.0F / 60.0F, 0.2F, 2.0F})
        {
            for (int frame = 0; frame < 20; ++frame)
            {
                panel->onUpdate(deltaTime);
            }
            EXPECT_EQ(panel->samplingInterval(), expected) << deltaTime;
        }
    };
    expectThroughEveryUiState(setting);

    // Settings changes it, and the new value holds through the same states.
    Core::RefreshRateChangedEvent faster(250);
    panel->onEvent(faster);
    EXPECT_EQ(panel->samplingInterval(), std::chrono::milliseconds(250));
    expectThroughEveryUiState(std::chrono::milliseconds(250));

    Core::RefreshRateChangedEvent slower(Domain::Sampling::REFRESH_INTERVAL_MAX_MS);
    panel->onEvent(slower);
    EXPECT_EQ(panel->samplingInterval(), std::chrono::milliseconds(Domain::Sampling::REFRESH_INTERVAL_MAX_MS));
    expectThroughEveryUiState(std::chrono::milliseconds(Domain::Sampling::REFRESH_INTERVAL_MAX_MS));
    panel->onDetach();
}

TEST_F(ProcessesPanelRenderTest, PlainCtrlAndShiftClicksBuildTheSelection)
{
    SelectionSink events;
    const auto panel = attachedPanel(events.sink());
    renderUntilLoaded(*panel);
    ASSERT_EQ(panel->processCount(), 3U);
    const std::map<std::int32_t, float> heights = rowHeights(*panel, 3);
    ASSERT_EQ(heights.size(), 3U);
    // The rows top to bottom.
    std::vector<std::pair<float, std::int32_t>> byHeight;
    byHeight.reserve(heights.size());
    for (const auto& [pid, y] : heights)
    {
        byHeight.emplace_back(y, pid);
    }
    std::ranges::sort(byHeight);
    const auto [topY, top] = byHeight.front();
    const auto [middleY, middle] = byHeight[1];
    const auto [bottomY, bottom] = byHeight.back();

    // A plain click selects one row and announces it.
    clickAt(*panel, topY);
    EXPECT_EQ(panel->selectedPid(), top);
    EXPECT_EQ(panel->selectionCount(), 1U);
    ASSERT_FALSE(events.selected.empty());
    EXPECT_EQ(events.selected.back(), top);

    // Ctrl+click adds a row, which becomes the primary one.
    clickAt(*panel, middleY, /*ctrl=*/true);
    EXPECT_EQ(panel->selectionCount(), 2U);
    EXPECT_EQ(panel->selectedPid(), middle);

    // Ctrl+click on it again takes it out; the one left becomes the primary, so F9 still has a target.
    clickAt(*panel, middleY, /*ctrl=*/true);
    EXPECT_EQ(panel->selectionCount(), 1U);
    EXPECT_EQ(panel->selectedPid(), top);

    // Shift+click selects the range from the anchor down to the bottom row.
    clickAt(*panel, bottomY, /*ctrl=*/false, /*shift=*/true);
    EXPECT_GE(panel->selectionCount(), 2U);
    EXPECT_EQ(panel->selectedPid(), bottom);
    panel->onDetach();
}

/// The vertical extent of everything drawn in the primary row's marker colour (the theme stub's
/// selectionPrimary, used by nothing else) in the last frame, or nullopt if nothing was. Only opaque
/// vertices count, not the transparent fringe of anti-aliased strokes.
[[nodiscard]] std::optional<std::pair<float, float>> primaryMarkerExtent()
{
    const ImU32 marker = ImGui::ColorConvertFloat4ToU32(UI::Theme::get().scheme().selectionPrimary);
    const ImDrawData* drawData = ImGui::GetDrawData();
    std::optional<std::pair<float, float>> extent;
    if (drawData == nullptr)
    {
        return extent;
    }
    for (const ImDrawList* list : drawData->CmdLists)
    {
        for (const ImDrawVert& vert : list->VtxBuffer)
        {
            if (vert.col != marker)
            {
                continue;
            }
            extent = extent.has_value() ? std::pair{std::min(extent->first, vert.pos.y), std::max(extent->second, vert.pos.y)}
                                        : std::pair{vert.pos.y, vert.pos.y};
        }
    }
    return extent;
}

// #1926: with several rows selected, the primary one -- the process Process Details shows -- is marked
// (a bar and an outline in the theme's selectionPrimary), and the other selected rows are not; a lone
// selected row is not marked. In the list and in the tree.
TEST_F(ProcessesPanelRenderTest, ThePrimaryRowOfASelectionIsMarked)
{
    for (const bool tree : {false, true})
    {
        SCOPED_TRACE(tree ? "tree view" : "list view");
        SelectionSink events;
        const auto panel = attachedPanel(events.sink());
        if (panel->treeViewEnabled() != tree)
        {
            panel->toggleTreeView();
        }
        renderUntilLoaded(*panel);
        ASSERT_EQ(panel->processCount(), 3U);
        const std::map<std::int32_t, float> heights = rowHeights(*panel, 3);
        ASSERT_EQ(heights.size(), 3U);
        std::vector<std::pair<float, std::int32_t>> byHeight;
        byHeight.reserve(heights.size());
        for (const auto& [pid, y] : heights)
        {
            byHeight.emplace_back(y, pid);
        }
        std::ranges::sort(byHeight);
        const float topY = byHeight[0].first;
        const float middleY = byHeight[1].first;
        const float bottomY = byHeight[2].first;
        const float rowPitch = (bottomY - topY) / 2.0F;

        // One row selected: it is the only candidate, so nothing marks it.
        clickAt(*panel, topY);
        ASSERT_EQ(panel->selectionCount(), 1U);
        EXPECT_FALSE(primaryMarkerExtent().has_value());

        // Two more Ctrl+clicked: the last one is the primary, and the marker is on its row alone.
        clickAt(*panel, middleY, /*ctrl=*/true);
        clickAt(*panel, bottomY, /*ctrl=*/true);
        ASSERT_EQ(panel->selectionCount(), 3U);
        ASSERT_EQ(panel->selectedPid(), byHeight[2].second);
        auto extent = primaryMarkerExtent();
        ASSERT_TRUE(extent.has_value());
        EXPECT_LE(extent->first, bottomY);
        EXPECT_GE(extent->second, bottomY);
        // One row tall, no more (the row pitch is only known to within the scan's 4 px, and the outline's
        // anti-aliased stroke reaches a pixel past the row).
        EXPECT_LT(extent->second - extent->first, 1.5F * rowPitch);
        for (const float other : {topY, middleY})
        {
            EXPECT_TRUE(other < extent->first + 1.0F || other > extent->second - 1.0F) << other;
        }

        // The top row Ctrl+clicked out and back in becomes the primary: the marker follows it.
        clickAt(*panel, topY, /*ctrl=*/true);
        clickAt(*panel, topY, /*ctrl=*/true);
        ASSERT_EQ(panel->selectionCount(), 3U);
        ASSERT_EQ(panel->selectedPid(), byHeight[0].second);
        extent = primaryMarkerExtent();
        ASSERT_TRUE(extent.has_value());
        EXPECT_LE(extent->first, topY);
        EXPECT_GE(extent->second, topY);
        for (const float other : {middleY, bottomY})
        {
            EXPECT_TRUE(other < extent->first + 1.0F || other > extent->second - 1.0F) << other;
        }
        panel->onDetach();
    }
}

TEST_F(ProcessesPanelRenderTest, LeavingTheTreeViewShowsTheSortedListAgain)
{
    const auto panel = attachedPanel();
    renderUntilLoaded(*panel);
    const bool wasTree = panel->treeViewEnabled();
    panel->toggleTreeView();
    static_cast<void>(renderAndCapture(*panel));
    panel->toggleTreeView(); // back: the list is sorted again (#1174)
    EXPECT_EQ(panel->treeViewEnabled(), wasTree);
    static_cast<void>(renderAndCapture(*panel));
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("init")) << text;
    EXPECT_TRUE(text.contains("server")) << text;
    EXPECT_TRUE(text.contains("worker")) << text;
    panel->onDetach();
}

TEST_F(ProcessesPanelRenderTest, ArrowKeysWalkAndFoldTheTree)
{
    SelectionSink events;
    const auto panel = attachedPanel(events.sink());
    renderUntilLoaded(*panel);
    if (!panel->treeViewEnabled())
    {
        panel->toggleTreeView();
    }
    // The tree: init (1) > server (100) > worker (101).
    pressKey(*panel, ImGuiKey_Home);
    EXPECT_EQ(panel->selectedPid(), 1);
    pressKey(*panel, ImGuiKey_RightArrow); // expanded already: to its first child
    EXPECT_EQ(panel->selectedPid(), 100);
    pressKey(*panel, ImGuiKey_RightArrow);
    EXPECT_EQ(panel->selectedPid(), 101);
    pressKey(*panel, ImGuiKey_LeftArrow); // a leaf: to its parent
    EXPECT_EQ(panel->selectedPid(), 100);

    pressKey(*panel, ImGuiKey_LeftArrow); // expanded: collapses it, the selection stays
    EXPECT_EQ(panel->selectedPid(), 100);
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_FALSE(renderAndCapture(*panel).contains("worker"));

    pressKey(*panel, ImGuiKey_RightArrow); // collapsed: expands it
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_TRUE(renderAndCapture(*panel).contains("worker"));
    EXPECT_EQ(panel->selectedPid(), 100);
    panel->onDetach();
}

TEST_F(ProcessesPanelRenderTest, PageKeysJumpAPageAndStopAtTheEnds)
{
    SelectionSink events;
    const auto panel = attachedPanel(events.sink());
    renderUntilLoaded(*panel);
    pressKey(*panel, ImGuiKey_Home);
    const std::int32_t first = panel->selectedPid();
    pressKey(*panel, ImGuiKey_End);
    const std::int32_t last = panel->selectedPid();
    ASSERT_NE(first, last);
    // Three rows fit in a page: Page Up from the last row lands on the first, Page Down goes back.
    pressKey(*panel, ImGuiKey_PageUp);
    EXPECT_EQ(panel->selectedPid(), first);
    pressKey(*panel, ImGuiKey_PageDown);
    EXPECT_EQ(panel->selectedPid(), last);
    pressKey(*panel, ImGuiKey_PageDown); // already at the end: stays
    EXPECT_EQ(panel->selectedPid(), last);
    panel->onDetach();
}

TEST_F(ProcessesPanelRenderTest, TheFilterKeepsMatchingRowsAndClearingItBringsThemBack)
{
    SelectionSink events;
    const auto panel = attachedPanel(events.sink());
    renderUntilLoaded(*panel);
    const ImGuiWindow* shell = ImGui::FindWindowByName("Shell");
    ASSERT_NE(shell, nullptr);
    // Click into the filter and type, a character a frame as a keyboard would.
    ImGui::ActivateItemByID(ImHashStr("##search", 0, shell->ID));
    static_cast<void>(renderAndCapture(*panel));
    for (const char* character : {"w", "o", "r", "k"})
    {
        ImGui::GetIO().AddInputCharactersUTF8(character);
        static_cast<void>(renderAndCapture(*panel));
    }
    static_cast<void>(renderAndCapture(*panel));
    std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("worker")) << text;
    EXPECT_FALSE(text.contains("server")) << text; // only the matching row is left

    // The clear button beside the filter brings every row back. Enter first: a click on the button
    // takes the keyboard from the filter, which an active text field would otherwise write back over.
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
    static_cast<void>(renderAndCapture(*panel));
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
    static_cast<void>(renderAndCapture(*panel));
    ImGui::ActivateItemByID(ImHashStr(ICON_FA_XMARK, 0, shell->ID));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("server")) << text;
    EXPECT_TRUE(text.contains("worker")) << text;
    panel->onDetach();
}

// #3328: the User column writes DOMAIN\user for a name two accounts share, so PC\alice's and CORP\alice's
// rows differ, and the short name for every other owner, so the column stays narrow.
TEST_F(ProcessesPanelRenderTest, TheUserColumnWritesTheDomainOnlyForANameTwoAccountsShare)
{
    const auto panel = attachedPanelWithOwners();
    renderUntilLoaded(*panel);
    static_cast<void>(renderAndCapture(*panel));
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("PC\\alice")) << text;
    EXPECT_TRUE(text.contains("CORP\\alice")) << text;
    EXPECT_TRUE(text.contains("bob")) << text;
    EXPECT_FALSE(text.contains("PC\\bob")) << text; // One account has the name: no domain needed
    panel->onDetach();
}

// #3328: the filter finds a process by its owner, by the short name or by DOMAIN\user.
TEST_F(ProcessesPanelRenderTest, TheFilterFindsAProcessByItsOwnersAccount)
{
    const auto panel = attachedPanelWithOwners();
    renderUntilLoaded(*panel);
    const ImGuiWindow* shell = ImGui::FindWindowByName("Shell");
    ASSERT_NE(shell, nullptr);
    ImGui::ActivateItemByID(ImHashStr("##search", 0, shell->ID));
    static_cast<void>(renderAndCapture(*panel));
    for (const char* character : {"c", "o", "r", "p", "\\", "a"})
    {
        ImGui::GetIO().AddInputCharactersUTF8(character);
        static_cast<void>(renderAndCapture(*panel));
    }
    static_cast<void>(renderAndCapture(*panel));
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("worker")) << text;
    EXPECT_FALSE(text.contains("server")) << text; // PC\alice's
    panel->onDetach();
}

TEST_F(ProcessesPanelRenderTest, HoldingCtrlOverTheTablePausesItAndSaysSo)
{
    SelectionSink events;
    const auto panel = attachedPanel(events.sink());
    renderUntilLoaded(*panel);
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(DISPLAY_WIDTH * 0.5F, DISPLAY_HEIGHT * 0.5F); // the pane hovered
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    // The freeze engages once Ctrl has been held a moment (ProcessDisplayFreeze::ENGAGE_DELAY_SECONDS).
    std::string text;
    for (int i = 0; i < 30 && !text.contains("Paused (Ctrl)"); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        text = renderAndCapture(*panel);
    }
    EXPECT_TRUE(text.contains("Paused (Ctrl)")) << text;
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_FALSE(renderAndCapture(*panel).contains("Paused (Ctrl)"));
    panel->onDetach();
}

TEST_F(ProcessesPanelRenderTest, TheListAndTreeSegmentsSwitchTheView)
{
    SelectionSink events;
    const auto panel = attachedPanel(events.sink());
    renderUntilLoaded(*panel);
    const ImGuiWindow* shell = ImGui::FindWindowByName("Shell");
    ASSERT_NE(shell, nullptr);
    ImGui::ActivateItemByID(ImHashStr(ICON_FA_SITEMAP " Tree", 0, shell->ID));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_TRUE(panel->treeViewEnabled());
    ImGui::ActivateItemByID(ImHashStr(ICON_FA_LIST " List", 0, shell->ID));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_FALSE(panel->treeViewEnabled());
    panel->onDetach();
}

TEST_F(ProcessesPanelRenderTest, AColumnHeaderExplainsItsColumnOnHover)
{
    SelectionSink events;
    const auto panel = attachedPanel(events.sink());
    renderUntilLoaded(*panel);
    // The PID column's description, as its header's tooltip shows it (#1210).
    const std::string description(getColumnInfo(ProcessColumn::PID).description);
    ASSERT_FALSE(description.empty());
    ImGuiIO& io = ImGui::GetIO();
    // Down the header row's band, across the columns, holding still long enough for the tooltip.
    bool shown = false;
    for (int y = 20; y <= 140 && !shown; y += 6)
    {
        for (int x = 20; x <= 760 && !shown; x += 30)
        {
            io.AddMousePosEvent(static_cast<float>(x), static_cast<float>(y));
            for (int frame = 0; frame < 30 && !shown; ++frame) // past ImGuiHoveredFlags_DelayShort at 60 fps
            {
                shown = renderAndCapture(*panel).contains(description);
            }
        }
    }
    EXPECT_TRUE(shown);
    panel->onDetach();
}

// #838: tree view sorts each parent's children by the clicked column, and every child stays right
// under its parent; the list sorts the same processes flat.
TEST_F(ProcessesPanelRenderTest, TheTreeViewSortsSiblingsUnderTheirParents)
{
    using TestMocks::makeProcessCounters;
    constexpr std::uint64_t RSS = std::uint64_t{1} << 20U;
    const auto panel = loadedTreePanel({
        makeProcessCounters(1, "tree-root", 'S', 10, 5, 100, RSS, 0),
        makeProcessCounters(300, "alpha", 'S', 10, 5, 1000, RSS, 1),
        makeProcessCounters(200, "charlie", 'S', 10, 5, 1000, RSS, 1),
        makeProcessCounters(100, "bravo", 'S', 10, 5, 1000, RSS, 1),
        makeProcessCounters(400, "zulu", 'S', 10, 5, 2000, RSS, 300),
    });
    ASSERT_EQ(panel->processCount(), 5U);
    const ImGuiTable* table = processTable();
    ASSERT_NE(table, nullptr);
    const ImGuiTableColumn& name = table->Columns[static_cast<int>(toIndex(ProcessColumn::Name))];
    clickAtPoint(*panel, (name.MinX + name.MaxX) * 0.5F, table->OuterRect.Min.y + (ImGui::GetFrameHeight() * 0.5F));

    std::string text = renderAndCapture(*panel);
    const auto at = [&text](std::string_view label)
    {
        return text.find(label);
    };
    ASSERT_NE(at("zulu"), std::string::npos) << text;
    // tree-root, then its children by name, zulu under alpha though it sorts last by name.
    EXPECT_LT(at("tree-root"), at("alpha")) << text;
    EXPECT_LT(at("alpha"), at("zulu")) << text;
    EXPECT_LT(at("zulu"), at("bravo")) << text;
    EXPECT_LT(at("bravo"), at("charlie")) << text;

    panel->toggleTreeView(); // the list: the same sort, flat
    static_cast<void>(renderAndCapture(*panel));
    text = renderAndCapture(*panel);
    EXPECT_LT(at("alpha"), at("bravo")) << text;
    EXPECT_LT(at("bravo"), at("charlie")) << text;
    EXPECT_LT(at("charlie"), at("tree-root")) << text;
    EXPECT_LT(at("tree-root"), at("zulu")) << text;
    panel->onDetach();
}

// #838 audit (#906, #911): the expander is drawn in the Name column, so a click at the start of a
// parent's Name cell folds it, while a click in its PID cell only selects the row.
TEST_F(ProcessesPanelRenderTest, TheTreeExpanderSitsInTheNameColumn)
{
    SelectionSink events; // a click selects a row, which raises an event
    const auto panel = attachedPanel(events.sink());
    renderUntilLoaded(*panel);
    if (!panel->treeViewEnabled())
    {
        panel->toggleTreeView();
    }
    static_cast<void>(renderAndCapture(*panel));
    const ImGuiTable* table = processTable();
    ASSERT_NE(table, nullptr);
    const ImGuiTableColumn& pid = table->Columns[static_cast<int>(toIndex(ProcessColumn::PID))];
    const ImGuiTableColumn& name = table->Columns[static_cast<int>(toIndex(ProcessColumn::Name))];
    ASSERT_LT(pid.MaxX, name.MinX + 1.0F) << "PID before Name, as by default";

    // Every row is found by clicks in the PID column, none of which folds anything.
    const std::map<std::int32_t, float> heights = rowHeights(*panel, 3);
    ASSERT_EQ(heights.size(), 3U);
    EXPECT_TRUE(renderAndCapture(*panel).contains("worker"));

    // The root's expander: the start of its Name cell (no indent at depth 0), a text line high. The
    // height found for the row is the lowest point in it, so half a line up is about its middle.
    const float rootMiddle = heights.at(1) - (ImGui::GetTextLineHeight() * 0.5F);
    clickAtPoint(*panel, name.MinX + ImGui::GetStyle().CellPadding.x + (ImGui::GetFontSize() * 0.5F), rootMiddle);
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("init")) << text;
    EXPECT_FALSE(text.contains("server")) << text;
    EXPECT_FALSE(text.contains("worker")) << text;

    // #2713: a second click on the expander opens the branch again.
    clickAtPoint(*panel, name.MinX + ImGui::GetStyle().CellPadding.x + (ImGui::GetFontSize() * 0.5F), rootMiddle);
    const std::string reopened = renderAndCapture(*panel);
    EXPECT_TRUE(reopened.contains("server")) << reopened;
    EXPECT_TRUE(reopened.contains("worker")) << reopened;
    panel->onDetach();
}

// #2713 (a #2700 manual check): a column dragged to a new place and one widened by its border, both with
// the mouse through the panel, come back the same once the layout has gone through config.toml into a
// fresh panel and a fresh ImGui context -- the sort with them.
TEST_F(ProcessesPanelRenderTest, ColumnsReorderedAndResizedByMouseSurviveARestart)
{
    // Frames in a window that keeps settings, as the app's does: a table in a NoSavedSettings window (the
    // other tests' "Shell") saves none.
    const auto frame = [](ProcessesPanel& shown)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(DISPLAY_WIDTH, DISPLAY_HEIGHT));
        ImGui::Begin("SavingShell", nullptr, ImGuiWindowFlags_NoDecoration);
        shown.renderContent();
        ImGui::End();
        ImGui::Render();
    };
    // Until it has read all three processes (the sampler runs on its own thread), then a frame to draw them.
    const auto load = [&frame](ProcessesPanel& shown)
    {
        for (int drawn = 0; drawn < LOAD_FRAMES && shown.processCount() < 3; ++drawn)
        {
            frame(shown);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        frame(shown);
        frame(shown);
    };
    auto panel = attachedPanel();
    load(*panel);
    const ImGuiTable* table = processTable();
    ASSERT_NE(table, nullptr);
    const auto pidIndex = static_cast<int>(toIndex(ProcessColumn::PID));
    const auto nameIndex = static_cast<int>(toIndex(ProcessColumn::Name));
    ASSERT_LT(table->Columns[pidIndex].DisplayOrder, table->Columns[nameIndex].DisplayOrder) << "PID before Name, as by default";
    const float headerY = table->OuterRect.Min.y + ImGui::GetStyle().CellPadding.y + (ImGui::GetTextLineHeight() * 0.5F);
    ImGuiIO& io = ImGui::GetIO();
    // Presses at (@p fromX, headerY), moves to @p toX in steps, and releases, a frame or two each.
    const auto dragHeader = [&](float fromX, float toX)
    {
        io.AddMousePosEvent(fromX, headerY);
        frame(*panel);
        frame(*panel);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame(*panel);
        for (int step = 1; step <= 10; ++step)
        {
            io.AddMousePosEvent(fromX + ((toX - fromX) * static_cast<float>(step) / 10.0F), headerY);
            frame(*panel);
        }
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame(*panel);
        frame(*panel);
    };

    // Name's header dragged onto PID's: Name goes first.
    const ImGuiTableColumn& pid = table->Columns[pidIndex];
    const ImGuiTableColumn& name = table->Columns[nameIndex];
    dragHeader((name.MinX + name.MaxX) * 0.5F, pid.MinX + 2.0F);
    ASSERT_LT(name.DisplayOrder, pid.DisplayOrder) << "the drag reordered them";

    // PID's right border dragged 40 px right: wider.
    const float widthBefore = pid.WidthGiven;
    dragHeader(pid.MaxX - 1.0F, pid.MaxX + 39.0F);
    EXPECT_GT(pid.WidthGiven, widthBefore + 20.0F) << "the border drag widened it";
    const int nameOrder = name.DisplayOrder;
    const int pidOrder = pid.DisplayOrder;
    const float pidWidth = pid.WidthGiven;

    const std::string layout = panel->captureTableLayout();
    ASSERT_FALSE(layout.empty());
    panel->onDetach();
    panel.reset();

    // Through config.toml, as the writer writes it and load() reads it.
    std::ostringstream file;
    file << toml::table{{"process_table", toml::table{{"layout", layout}}}};
    UserSettings loaded;
    ASSERT_TRUE(UserConfig::parseSettings(file.str(), loaded));
    ASSERT_EQ(loaded.processTableLayout, layout);

    // A fresh context remembers nothing of the table; only the stored layout can put it back.
    ImGuiContext* original = ImGui::GetCurrentContext();
    ImPlotContext* originalPlot = ImPlot::GetCurrentContext();
    ImGuiContext* fresh = ImGui::CreateContext();
    ImGui::SetCurrentContext(fresh);
    ImPlotContext* freshPlot = ImPlot::CreateContext();
    ImGuiIO& freshIo = ImGui::GetIO();
    freshIo.IniFilename = nullptr;
    freshIo.LogFilename = nullptr;
    freshIo.DisplaySize = ImVec2(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    freshIo.DeltaTime = 1.0F / 60.0F;
    freshIo.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
    freshIo.Fonts->AddFontDefault();
    ProcessesPanel::restoreTableLayout(loaded.processTableLayout);
    auto restarted = attachedPanel();
    load(*restarted);
    const ImGuiTable* restored = processTable();
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->Columns[nameIndex].DisplayOrder, nameOrder);
    EXPECT_EQ(restored->Columns[pidIndex].DisplayOrder, pidOrder);
    EXPECT_NEAR(restored->Columns[pidIndex].WidthGiven, pidWidth, 1.0F);
    EXPECT_EQ(restarted->captureTableLayout(), layout) << "the same layout, its sort included";
    restarted->onDetach();
    restarted.reset();
    ImPlot::DestroyContext(freshPlot);
    ImGui::DestroyContext(fresh);
    ImGui::SetCurrentContext(original);
    ImPlot::SetCurrentContext(originalPlot);
}

// #838 audit (#852, #1138): the tree view goes through ImGuiListClipper, so only the rows in view
// submit draw calls: a frame of 2000 processes draws about what a frame of 100 does, at the top and
// after End scrolls to the bottom. Measured in vertices, not logged text: ImGui turns clipping off
// while logging.
TEST_F(ProcessesPanelRenderTest, TheTreeViewDrawsOnlyTheRowsInView)
{
    // A root and @p children processes under it.
    const auto tree = [](int children)
    {
        constexpr std::uint64_t RSS = std::uint64_t{1} << 20U;
        std::vector<Platform::ProcessCounters> processes;
        processes.reserve(static_cast<std::size_t>(children) + 1);
        processes.push_back(TestMocks::makeProcessCounters(1, "tree-root", 'S', 10, 5, 100, RSS, 0));
        for (int i = 0; i < children; ++i)
        {
            const int pid = 1000 + i;
            processes.push_back(TestMocks::makeProcessCounters(pid, std::format("proc-{}", pid), 'S', 10, 5, 1000, RSS, 1));
        }
        return processes;
    };
    // The vertices one frame of @p panel draws, without logging.
    const auto frameVertices = [](ProcessesPanel& panel)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(DISPLAY_WIDTH, DISPLAY_HEIGHT));
        ImGui::Begin("Shell", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        panel.renderContent();
        ImGui::End();
        ImGui::Render();
        return ImGui::GetIO().MetricsRenderVertices;
    };

    // 100 children already overflow the 1200 px table, so its frame is a screenful of rows.
    const auto screenful = loadedTreePanel(tree(99));
    ASSERT_EQ(screenful->processCount(), 100U);
    const int screenfulVertices = frameVertices(*screenful);
    screenful->onDetach();

    const auto panel = loadedTreePanel(tree(1999));
    ASSERT_EQ(panel->processCount(), 2000U);
    const int atTop = frameVertices(*panel);
    EXPECT_GT(atTop, 0);
    EXPECT_LT(atTop, screenfulVertices * 3 / 2) << "20 times the rows, about the same drawing";

    pressKey(*panel, ImGuiKey_End); // the last row, scrolled into view
    static_cast<void>(frameVertices(*panel));
    const ImGuiTable* table = processTable();
    ASSERT_NE(table, nullptr);
    EXPECT_GT(table->InnerWindow->Scroll.y, 0.0F);
    EXPECT_FLOAT_EQ(table->InnerWindow->Scroll.y, table->InnerWindow->ScrollMax.y);
    EXPECT_LT(frameVertices(*panel), screenfulVertices * 3 / 2);
    panel->onDetach();
}

// #838: tree view's Collapse all folds every parent, leaving the roots, and Expand all opens them again.
TEST_F(ProcessesPanelRenderTest, TheTreeViewsCollapseAllAndExpandAllFoldEveryParent)
{
    SelectionSink events;
    const auto panel = attachedPanel(events.sink());
    renderUntilLoaded(*panel);
    if (!panel->treeViewEnabled())
    {
        panel->toggleTreeView();
    }
    static_cast<void>(renderAndCapture(*panel));
    const ImGuiWindow* shell = ImGui::FindWindowByName("Shell");
    ASSERT_NE(shell, nullptr);
    std::string text = renderAndCapture(*panel);
    ASSERT_TRUE(text.contains("worker")) << text;

    ImGui::ActivateItemByID(ImHashStr(ICON_FA_ANGLES_UP "##CollapseAll", 0, shell->ID));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("init")) << text;
    EXPECT_FALSE(text.contains("server")) << text;
    EXPECT_FALSE(text.contains("worker")) << text;

    ImGui::ActivateItemByID(ImHashStr(ICON_FA_ANGLES_DOWN "##ExpandAll", 0, shell->ID));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("server")) << text;
    EXPECT_TRUE(text.contains("worker")) << text;
    panel->onDetach();
}

// #838: a filtered tree keeps a match under its ancestors, which do not match and are drawn dimmed,
// rather than promoting the match to a root; the list still shows the match alone.
TEST_F(ProcessesPanelRenderTest, AFilteredTreeShowsTheMatchUnderItsDimmedAncestors)
{
    SelectionSink events;
    const auto panel = attachedPanel(events.sink());
    renderUntilLoaded(*panel);
    if (!panel->treeViewEnabled())
    {
        panel->toggleTreeView();
    }
    static_cast<void>(renderAndCapture(*panel));
    // Where each row is, found before filtering: the filtered tree has the same rows in the same places.
    const std::map<std::int32_t, float> heights = rowHeights(*panel, 3);
    ASSERT_EQ(heights.size(), 3U);

    const ImGuiWindow* shell = ImGui::FindWindowByName("Shell");
    ASSERT_NE(shell, nullptr);
    ImGui::ActivateItemByID(ImHashStr("##search", 0, shell->ID));
    static_cast<void>(renderAndCapture(*panel));
    for (const char* character : {"w", "o", "r", "k"})
    {
        ImGui::GetIO().AddInputCharactersUTF8(character);
        static_cast<void>(renderAndCapture(*panel));
    }
    static_cast<void>(renderAndCapture(*panel));
    const std::string text = renderAndCapture(*panel);
    ASSERT_TRUE(text.contains("worker")) << text;
    EXPECT_LT(text.find("init"), text.find("server")) << text;
    EXPECT_LT(text.find("server"), text.find("worker")) << text;

    // The brightest vertex in each row's Name cell: the ancestors' are about half the match's.
    const ImGuiTable* table = processTable();
    ASSERT_NE(table, nullptr);
    const ImGuiTableColumn& name = table->Columns[static_cast<int>(toIndex(ProcessColumn::Name))];
    const ImDrawList* drawList = table->InnerWindow->DrawList;
    const float rowHeight = heights.at(101) - heights.at(100);
    ASSERT_GT(rowHeight, 0.0F);
    const auto brightest = [&](std::int32_t pid)
    {
        const float bottom = heights.at(pid);
        unsigned int alpha = 0;
        for (const ImDrawVert& vert : drawList->VtxBuffer)
        {
            if (vert.pos.x >= name.MinX && vert.pos.x <= name.MaxX && vert.pos.y > bottom - rowHeight + 4.0F && vert.pos.y <= bottom)
            {
                alpha = std::max(alpha, (vert.col >> IM_COL32_A_SHIFT) & 0xFFU);
            }
        }
        return alpha;
    };
    const unsigned int match = brightest(101);
    EXPECT_GT(match, 200U);
    EXPECT_LT(brightest(1), match * 3 / 4);
    EXPECT_GT(brightest(1), 0U);
    EXPECT_LT(brightest(100), match * 3 / 4);

    panel->toggleTreeView(); // the list: the match alone
    static_cast<void>(renderAndCapture(*panel));
    const std::string list = renderAndCapture(*panel);
    EXPECT_TRUE(list.contains("worker")) << list;
    EXPECT_FALSE(list.contains("server")) << list;
    panel->onDetach();
}

// #838: a collapsed parent shows how many children it has after its name, "(1)"; an expanded one does not.
TEST_F(ProcessesPanelRenderTest, ACollapsedParentShowsItsChildCountAndAnExpandedOneDoesNot)
{
    SelectionSink events;
    const auto panel = attachedPanel(events.sink());
    renderUntilLoaded(*panel);
    if (!panel->treeViewEnabled())
    {
        panel->toggleTreeView();
    }
    static_cast<void>(renderAndCapture(*panel));
    std::string text = renderAndCapture(*panel);
    ASSERT_TRUE(text.contains("worker")) << text;
    EXPECT_FALSE(text.contains("(1)")) << text;

    const ImGuiWindow* shell = ImGui::FindWindowByName("Shell");
    ASSERT_NE(shell, nullptr);
    ImGui::ActivateItemByID(ImHashStr(ICON_FA_ANGLES_UP "##CollapseAll", 0, shell->ID));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    text = renderAndCapture(*panel);
    ASSERT_FALSE(text.contains("server")) << text;
    EXPECT_GT(text.find("(1)"), text.find("init")) << text; // init's one child, after its name
    EXPECT_NE(text.find("(1)"), std::string::npos) << text;

    ImGui::ActivateItemByID(ImHashStr(ICON_FA_ANGLES_DOWN "##ExpandAll", 0, shell->ID));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("worker")) << text;
    EXPECT_FALSE(text.contains("(1)")) << text;
    panel->onDetach();
}

/// The x positions of the tree's guide lines (the theme stub's treeLines, used by nothing else) in the
/// last frame: one per line, the middle of each run of opaque vertices less than a few pixels apart.
[[nodiscard]] std::vector<float> guideLineXs()
{
    const ImU32 lineColor = ImGui::ColorConvertFloat4ToU32(UI::Theme::get().scheme().treeLines);
    std::vector<float> xs;
    if (const ImDrawData* drawData = ImGui::GetDrawData(); drawData != nullptr)
    {
        for (const ImDrawList* list : drawData->CmdLists)
        {
            for (const ImDrawVert& vert : list->VtxBuffer)
            {
                if (vert.col == lineColor)
                {
                    xs.push_back(vert.pos.x);
                }
            }
        }
    }
    std::ranges::sort(xs);
    std::vector<float> lines;
    float runStart = 0.0F;
    for (std::size_t i = 0; i < xs.size(); ++i)
    {
        if (i == 0 || xs[i] - xs[i - 1] > 3.0F)
        {
            runStart = xs[i];
            lines.push_back(xs[i]);
        }
        lines.back() = (runStart + xs[i]) * 0.5F;
    }
    return lines;
}

// #838: each tree row draws a guide line for every level above it, through its ancestors' carets: worker
// (depth 2) shares init's line with server and adds server's. The roots and the list draw none.
TEST_F(ProcessesPanelRenderTest, TheTreeDrawsAGuideLineForEachLevelAboveARow)
{
    SelectionSink events;
    const auto panel = attachedPanel(events.sink());
    renderUntilLoaded(*panel);
    if (!panel->treeViewEnabled())
    {
        panel->toggleTreeView();
    }
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    const std::vector<float> lines = guideLineXs();
    ASSERT_EQ(lines.size(), 2U);
    const ImGuiTable* table = processTable();
    ASSERT_NE(table, nullptr);
    const ImGuiTableColumn& name = table->Columns[static_cast<int>(toIndex(ProcessColumn::Name))];
    const float step = ProcessTreeIndent::INDENT_PER_LEVEL_EM * ImGui::GetFontSize();
    EXPECT_NEAR(lines[1] - lines[0], step, 1.5F); // one level apart, at the font's indent step
    EXPECT_GT(lines[0], name.MinX);
    EXPECT_LT(lines[0], name.MinX + ImGui::GetStyle().CellPadding.x + ImGui::GetFrameHeight()); // in init's caret slot

    panel->toggleTreeView(); // the list: no tree, no lines
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_TRUE(guideLineXs().empty());
    panel->onDetach();
}

// #838: a name too long for its row at depth 12 is cut short with an ellipsis, with the whole name as
// its tooltip, after every level's indent has shrunk alike to fit the deepest row.
TEST_F(ProcessesPanelRenderTest, ADeepLongNameIsCutShortWithItsWholeNameAsTheTooltip)
{
    constexpr std::uint64_t RSS = std::uint64_t{1} << 20U;
    constexpr int DEPTH = 12;
    const std::string longName = "a-process-name-far-too-long-for-the-name-column-at-this-depth.exe";
    std::vector<Platform::ProcessCounters> chain;
    for (int level = 0; level <= DEPTH; ++level)
    {
        const std::string processName = level == DEPTH ? longName : std::format("level-{}", level);
        chain.push_back(
            TestMocks::makeProcessCounters(level + 1, processName, 'S', 10, 5, 100U + static_cast<std::uint64_t>(level), RSS, level));
    }
    const auto panel = loadedTreePanel(std::move(chain));
    ASSERT_EQ(panel->processCount(), static_cast<std::size_t>(DEPTH + 1));
    const std::string text = renderAndCapture(*panel);
    ASSERT_TRUE(text.contains(longName)) << text; // logged whole, as a clipped cell logs its text
    ASSERT_TRUE(text.contains("level-0")) << text;

    // Down the right edge of the Name column, holding still long enough for a tooltip: only the long
    // name is clipped, so the only tooltip is its own, which logs the name once more.
    const ImGuiTable* table = processTable();
    ASSERT_NE(table, nullptr);
    const ImGuiTableColumn& name = table->Columns[static_cast<int>(toIndex(ProcessColumn::Name))];
    ImGuiIO& io = ImGui::GetIO();
    const auto occurrences = [&longName](const std::string& log)
    {
        std::size_t count = 0;
        for (std::size_t at = log.find(longName); at != std::string::npos; at = log.find(longName, at + 1))
        {
            ++count;
        }
        return count;
    };
    io.AddMousePosEvent(-100.0F, -100.0F); // hovering nothing: only the cell logs the name
    static_cast<void>(renderAndCapture(*panel));
    const std::size_t resting = occurrences(renderAndCapture(*panel));
    ASSERT_GE(resting, 1U);
    bool shown = false;
    for (int y = 20; y <= 400 && !shown; y += 4)
    {
        io.AddMousePosEvent(name.MaxX - 12.0F, static_cast<float>(y));
        for (int frame = 0; frame < 30 && !shown; ++frame) // past ImGuiHoveredFlags_DelayShort at 60 fps
        {
            shown = occurrences(renderAndCapture(*panel)) == resting + 1;
        }
    }
    EXPECT_TRUE(shown);
    panel->onDetach();
}

} // namespace
} // namespace App
