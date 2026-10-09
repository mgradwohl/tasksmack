/// @file test_ProcessSecurityViewRender.cpp
/// @brief The Security section (#1526): its read cadence against a mock reader, the rows it builds, and
/// its real ImGui render, headless. The text it hands ImGui is captured through ImGui's own logging
/// (LogToBuffer()): nothing read while collapsed, the rows of a read, and a muted note instead of a
/// table for a denied or failed read.

#include "App/Panels/ProcessSecurityView.h"
#include "Mocks/MockProbes.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessSecurity.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <cstdint>
#include <optional>
#include <string>
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

TEST(ProcessSecurityViewTest, ReadsOnlyWhileOpenAndAtTheRefreshCadence)
{
    ProcessSecurityView view;
    TestMocks::MockProcessSecurityReader reader;
    reader.setResult(sampleSecurity());

    EXPECT_FALSE(view.update(&reader, TARGET, 10.0F)); // never drawn open: never read
    view.markDrawnOpen();
    EXPECT_TRUE(view.update(&reader, TARGET, 0.0F));
    EXPECT_TRUE(view.hasRead());
    EXPECT_EQ(view.rows().size(), 11U);

    view.markDrawnOpen();
    EXPECT_FALSE(view.update(&reader, TARGET, 1.0F)); // not yet due
    view.markDrawnOpen();
    EXPECT_TRUE(view.update(&reader, TARGET, 2.5F)); // PROCESS_SECURITY_REFRESH_MS since the last read
    EXPECT_EQ(reader.readCount(), 2);

    // A new selection drops the rows and reads again as soon as it is shown.
    view.onSelectionChanged();
    EXPECT_FALSE(view.hasRead());
    EXPECT_TRUE(view.rows().empty());
    view.markDrawnOpen();
    EXPECT_TRUE(view.update(&reader, TARGET, 0.0F));
    EXPECT_EQ(reader.readCount(), 3);

    EXPECT_FALSE(view.update(nullptr, TARGET, 10.0F)); // no reader: nothing read
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
