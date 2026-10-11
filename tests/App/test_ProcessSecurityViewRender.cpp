/// @file test_ProcessSecurityViewRender.cpp
/// @brief The Security section (#1526): its read cadence against a mock reader, the rows it builds, and
/// its real ImGui render, headless. Reads run on a worker: a slow one never blocks update(), a read
/// for an earlier selection is dropped, and one that throws becomes a Failed status. The text it hands ImGui is captured through ImGui's
/// own logging (LogToBuffer()): nothing read while collapsed, the rows of a read, and a muted note instead of a table for a denied or
/// failed read.

#include "App/Panels/ProcessSecurityView.h"
#include "Mocks/MockProbes.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessSecurity.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <atomic>
#include <cstdint>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace App
{
namespace
{

using Platform::SecurityReadStatus;

constexpr Platform::ProcessTarget TARGET{.pid = 4242, .startTimeTicks = 777};

[[nodiscard]] Platform::SecurityReadResult sampleSecurity()
{
    const Platform::SecurityPrincipal matt{.id = 1000, .name = "matt"};
    Platform::ProcessSecurity security;
    security.users = Platform::SecurityIdSet{.real = matt, .effective = matt, .saved = matt, .filesystem = matt};
    security.groups = Platform::SecurityIdSet{.real = matt, .effective = matt, .saved = matt, .filesystem = matt};
    security.supplementaryGroups = {{.id = 27, .name = "sudo"}};
    security.capabilities = Platform::CapabilitySets{
        .effective = std::uint64_t{0},
        .permitted = std::uint64_t{0},
        .inheritable = std::uint64_t{0},
        .bounding = std::uint64_t{0x1ffffffffff},
        .ambient = std::nullopt, // an old kernel: no row
    };
    security.noNewPrivileges = false;
    security.seccomp = Platform::SeccompMode::Filter;
    security.securityLabel = "unconfined";
    security.controlGroup = "/user.slice/app.scope";
    return {.status = SecurityReadStatus::Ok, .security = security, .detail = {}};
}

TEST(ProcessSecurityViewTest, BuildsOneRowPerReportedField)
{
    const std::vector<Detail::SecurityRow> rows = Detail::securityRows(sampleSecurity().security);
    const std::vector<Detail::SecurityRow> expected{
        {.label = "User", .value = "1000 (matt)"},
        {.label = "Group", .value = "1000 (matt)"},
        {.label = "Supplementary groups", .value = "27 (sudo)"},
        {.label = "Effective capabilities", .value = "none"},
        {.label = "Permitted capabilities", .value = "none"},
        {.label = "Inheritable capabilities", .value = "none"},
        {.label = "Bounding set", .value = "all (41)"},
        {.label = "No new privileges", .value = "No"},
        {.label = "Seccomp", .value = "Filter"},
        {.label = "Security label", .value = "unconfined"},
        {.label = "Control group", .value = "/user.slice/app.scope"},
    };
    EXPECT_EQ(rows, expected);
    EXPECT_TRUE(Detail::securityRows({}).empty()); // nothing reported: no made-up rows
}

TEST(ProcessSecurityViewTest, BuildsTheWindowsTokenRows)
{
    Platform::ProcessSecurity security;
    security.tokenUser = Platform::SecurityAccount{.sid = "S-1-5-21-1-2-3-1001", .name = "DESKTOP\\matt"};
    security.sessionId = 1;
    security.integrityLevel = 0x2000;
    security.elevated = false;
    security.elevationType = Platform::TokenElevationKind::Limited;
    security.appContainer = false;
    security.virtualizationEnabled = true;
    security.privileges = std::vector<Platform::TokenPrivilege>{{.name = "SeShutdownPrivilege", .enabled = false},
                                                                {.name = "SeChangeNotifyPrivilege", .enabled = true}};
    security.tokenGroups = std::vector<Platform::TokenGroup>{
        {.account = {.sid = "S-1-1-0", .name = "Everyone"}, .enabled = true, .denyOnly = false},
        {.account = {.sid = "S-1-5-32-544", .name = "BUILTIN\\Administrators"}, .enabled = false, .denyOnly = true},
    };
    const std::vector<Detail::SecurityRow> expected{
        {.label = "User", .value = "DESKTOP\\matt (S-1-5-21-1-2-3-1001)"},
        {.label = "Session", .value = "1"},
        {.label = "Integrity level", .value = "Medium"},
        {.label = "Elevated", .value = "No"},
        {.label = "Elevation type", .value = "Limited (filtered)"},
        {.label = "AppContainer", .value = "No"},
        {.label = "Virtualization", .value = "Enabled"},
        {.label = "Privileges", .value = "SeShutdownPrivilege: Disabled\nSeChangeNotifyPrivilege: Enabled"},
        {.label = "Groups", .value = "Everyone\nBUILTIN\\Administrators (deny only)"},
    };
    EXPECT_EQ(Detail::securityRows(security), expected);

    // A token with no privileges or groups says so, rather than dropping the rows.
    Platform::ProcessSecurity empty;
    empty.privileges = std::vector<Platform::TokenPrivilege>{};
    empty.tokenGroups = std::vector<Platform::TokenGroup>{};
    const std::vector<Detail::SecurityRow> none{{.label = "Privileges", .value = "none"}, {.label = "Groups", .value = "none"}};
    EXPECT_EQ(Detail::securityRows(empty), none);
}

TEST(ProcessSecurityViewTest, ReadsOnlyWhileOpenAndAtTheRefreshCadence)
{
    ProcessSecurityView view;
    TestMocks::MockProcessSecurityReader reader;
    reader.setResult(sampleSecurity());

    EXPECT_FALSE(view.update(&reader, TARGET, 10.0F)); // never drawn open: never read
    view.markDrawnOpen();
    EXPECT_TRUE(view.update(&reader, TARGET, 0.0F));
    view.finishPendingRead(TARGET);
    EXPECT_TRUE(view.hasRead());
    EXPECT_EQ(view.rows().size(), 11U);

    view.markDrawnOpen();
    EXPECT_FALSE(view.update(&reader, TARGET, 1.0F)); // not yet due
    view.markDrawnOpen();
    EXPECT_TRUE(view.update(&reader, TARGET, 2.5F)); // PROCESS_SECURITY_REFRESH_MS since the last read
    view.finishPendingRead(TARGET);
    EXPECT_EQ(reader.readCount(), 2);

    // A new selection drops the rows and reads again as soon as it is shown.
    view.onSelectionChanged();
    EXPECT_FALSE(view.hasRead());
    EXPECT_TRUE(view.rows().empty());
    view.markDrawnOpen();
    EXPECT_TRUE(view.update(&reader, TARGET, 0.0F));
    view.finishPendingRead(TARGET);
    EXPECT_EQ(reader.readCount(), 3);

    EXPECT_FALSE(view.update(nullptr, TARGET, 10.0F)); // no reader: nothing read
}

/// A reader whose reads wait at a gate the test opens, or throw: a slow (an unreachable domain
/// controller) or broken platform read.
class GatedSecurityReader final : public Platform::IProcessSecurityReader
{
  public:
    explicit GatedSecurityReader(std::shared_future<void> gate, bool throws = false) : m_Gate(std::move(gate)), m_Throws(throws)
    {}

    [[nodiscard]] bool hasSecurity() const override
    {
        return true;
    }

    [[nodiscard]] Platform::SecurityReadResult readSecurity(const Platform::ProcessTarget& /*target*/) override
    {
        ++m_Reads;
        m_Gate.wait();
        if (m_Throws)
        {
            throw std::runtime_error("lookup failed");
        }
        return sampleSecurity();
    }

    [[nodiscard]] int reads() const noexcept
    {
        return m_Reads.load();
    }

  private:
    std::shared_future<void> m_Gate;
    bool m_Throws = false;
    std::atomic<int> m_Reads{0};
};

TEST(ProcessSecurityViewTest, ASlowReadNeverBlocksUpdateAndNoSecondReadStartsWhileItRuns)
{
    std::promise<void> gate;
    GatedSecurityReader reader(gate.get_future().share());
    ProcessSecurityView view;
    view.markDrawnOpen();
    ASSERT_TRUE(view.update(&reader, TARGET, 0.0F)); // returns with the read still waiting at the gate
    EXPECT_TRUE(view.readInFlight());
    EXPECT_FALSE(view.hasRead()); // the section shows "Reading..."

    // Frames go on, past the refresh interval, without waiting or starting another read.
    view.markDrawnOpen();
    EXPECT_FALSE(view.update(&reader, TARGET, 10.0F));
    EXPECT_FALSE(view.hasRead());

    gate.set_value();
    view.finishPendingRead(TARGET);
    EXPECT_TRUE(view.hasRead());
    EXPECT_EQ(view.status(), SecurityReadStatus::Ok);
    EXPECT_EQ(view.rows().size(), 11U);
    EXPECT_EQ(reader.reads(), 1);
}

TEST(ProcessSecurityViewTest, AReadForAnEarlierSelectionIsDropped)
{
    constexpr Platform::ProcessTarget OTHER{.pid = 5151, .startTimeTicks = 888};
    std::promise<void> gate;
    GatedSecurityReader reader(gate.get_future().share());
    ProcessSecurityView view;
    view.markDrawnOpen();
    ASSERT_TRUE(view.update(&reader, TARGET, 0.0F));

    view.onSelectionChanged(); // OTHER is selected while TARGET's read is still running
    gate.set_value();
    view.finishPendingRead(OTHER);
    EXPECT_FALSE(view.hasRead()); // TARGET's rows never show under OTHER
    EXPECT_TRUE(view.rows().empty());

    // The same PID with another start time is another process: its read is dropped too.
    view.markDrawnOpen();
    ASSERT_TRUE(view.update(&reader, TARGET, 0.0F));
    view.finishPendingRead({.pid = TARGET.pid, .startTimeTicks = TARGET.startTimeTicks + 1});
    EXPECT_FALSE(view.hasRead());

    view.markDrawnOpen();
    ASSERT_TRUE(view.update(&reader, OTHER, 0.0F)); // and OTHER's own read is due at once
    view.finishPendingRead(OTHER);
    EXPECT_TRUE(view.hasRead());
    EXPECT_EQ(reader.reads(), 3);
}

TEST(ProcessSecurityViewTest, AReadThatThrowsBecomesAFailedStatus)
{
    std::promise<void> gate;
    gate.set_value();
    GatedSecurityReader reader(gate.get_future().share(), true);
    ProcessSecurityView view;
    view.markDrawnOpen();
    ASSERT_TRUE(view.update(&reader, TARGET, 0.0F));
    view.finishPendingRead(TARGET);
    EXPECT_TRUE(view.hasRead());
    EXPECT_EQ(view.status(), SecurityReadStatus::Failed);
    EXPECT_EQ(view.detail(), "lookup failed");
    EXPECT_TRUE(view.rows().empty());
}

class ProcessSecurityViewRenderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1200.0F, 800.0F);
        io.DeltaTime = 1.0F / 60.0F;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // no renderer: ImGui owns the atlas
        io.Fonts->AddFontDefault();
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
    }

    /// Renders one frame of @p view with its header held @p open, and returns all the text it drew.
    static std::string renderAndCapture(ProcessSecurityView& view, bool hasSecurity, bool open = true)
    {
        std::string captured;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(1200.0F, 800.0F));
        ImGui::Begin("Details", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::LogToBuffer();
        ImGui::SetNextItemOpen(open, ImGuiCond_Always);
        view.render(hasSecurity);
        captured = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::Render();
        return captured;
    }

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(ProcessSecurityViewRenderTest, DrawsNothingWithoutSupportAndNothingReadWhileCollapsed)
{
    ProcessSecurityView view;
    EXPECT_FALSE(renderAndCapture(view, false).contains("Security"));

    TestMocks::MockProcessSecurityReader reader;
    for (int i = 0; i < 3; ++i)
    {
        static_cast<void>(view.update(&reader, TARGET, 10.0F));
        EXPECT_TRUE(renderAndCapture(view, true, false).contains("Security"));
    }
    EXPECT_EQ(reader.readCount(), 0);
}

TEST_F(ProcessSecurityViewRenderTest, OpenReadsAndShowsTheRows)
{
    ProcessSecurityView view;
    TestMocks::MockProcessSecurityReader reader;
    reader.setResult(sampleSecurity());
    EXPECT_TRUE(renderAndCapture(view, true).contains("Reading..."));
    ASSERT_TRUE(view.update(&reader, TARGET, 0.0F));
    view.finishPendingRead(TARGET);
    const std::string text = renderAndCapture(view, true);
    EXPECT_TRUE(text.contains("User")) << text;
    EXPECT_TRUE(text.contains("1000 (matt)")) << text;
    EXPECT_TRUE(text.contains("all (41)")) << text;
    EXPECT_TRUE(text.contains("/user.slice/app.scope")) << text;
    EXPECT_FALSE(text.contains("Ambient")) << text;
}

TEST_F(ProcessSecurityViewRenderTest, AccessDeniedAndFailedShowANoteNotATable)
{
    ProcessSecurityView view;
    view.applyResult({.status = SecurityReadStatus::PermissionDenied, .security = {}, .detail = {}});
    std::string text = renderAndCapture(view, true);
    EXPECT_TRUE(text.contains("permission denied")) << text;
    EXPECT_FALSE(text.contains("User")) << text;

    view.applyResult({.status = SecurityReadStatus::Failed, .security = {}, .detail = "Input/output error"});
    text = renderAndCapture(view, true);
    EXPECT_TRUE(text.contains("Could not be read: Input/output error")) << text;
}

} // namespace
} // namespace App
