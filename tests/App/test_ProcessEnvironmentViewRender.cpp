/// @file test_ProcessEnvironmentViewRender.cpp
/// @brief The Environment section's real ImGui render, headless (#179). The text it hands ImGui is
/// captured through ImGui's own logging (LogToBuffer(), which also turns clipping off), so the tests
/// can say what was drawn: a masked value's text is never drawn until its row's reveal button is
/// clicked, the reveal ends with the selection, each failed read draws its status line instead of an
/// empty table, and nothing at all is drawn where the platform has no environment support.

#include "App/Panels/ProcessEnvironmentView.h"
#include "Mocks/MockProbes.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessEnvironment.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <cfloat>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace App
{
namespace
{

constexpr Platform::ProcessTarget TARGET{.pid = 4242, .startTimeTicks = 777};
constexpr float WINDOW_WIDTH = 1200.0F;
constexpr float WINDOW_HEIGHT = 800.0F;

[[nodiscard]] Platform::EnvironmentReadResult sampleEnvironment()
{
    return {.status = Platform::EnvironmentReadStatus::Ok,
            .variables = {{.name = "MY_API_TOKEN", .value = "supersecret"}, {.name = "FOO", .value = "bar"}}};
}

class ProcessEnvironmentViewRenderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(WINDOW_WIDTH, WINDOW_HEIGHT);
        io.DeltaTime = 1.0F / 60.0F;
        // No renderer: let ImGui build and own the font atlas itself.
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
    }

    /// One frame of a window like the Process Details pane, running @p body inside it.
    static void runFrame(const std::function<void()>& body)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(WINDOW_WIDTH, WINDOW_HEIGHT));
        ImGui::Begin("Details", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        body();
        ImGui::End();
        ImGui::Render();
    }

    /// Renders one frame of @p view with its header held @p open, and returns all the text it drew.
    static std::string renderAndCapture(ProcessEnvironmentView& view, bool hasEnvironment, bool open = true)
    {
        std::string captured;
        runFrame(
            [&]
            {
                ImGui::LogToBuffer();
                ImGui::SetNextItemOpen(open, ImGuiCond_Always);
                view.render(hasEnvironment);
                captured = GImGui->LogBuffer.c_str();
                ImGui::LogFinish();
            });
        return captured;
    }

    /// Runs frames as the panel does -- update, then render open -- until the view has read @p reader.
    static void openAndRead(ProcessEnvironmentView& view, TestMocks::MockProcessEnvironmentReader& reader)
    {
        for (int i = 0; i < 3 && !view.hasRead(); ++i)
        {
            static_cast<void>(view.update(&reader, TARGET, 1.0F / 60.0F));
            static_cast<void>(renderAndCapture(view, true));
        }
        ASSERT_TRUE(view.hasRead());
    }

    /// Where to click the reveal button of the table's first data row: the start of its VALUE cell.
    /// Found from the table ImGui keeps for "##EnvironmentTable" after a frame that drew it open.
    static std::optional<ImVec2> firstRevealButtonCentre(ProcessEnvironmentView& view)
    {
        std::optional<ImVec2> centre;
        runFrame(
            [&]
            {
                ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                view.render(true);
                const ImGuiTable* table = ImGui::TableFindByID(ImGui::GetID("##EnvironmentTable"));
                if (table != nullptr)
                {
                    // What TableGetHeaderRowHeight() returns for one-line headers (it needs a current table).
                    const float headerHeight = ImGui::GetTextLineHeight() + (ImGui::GetStyle().CellPadding.y * 2.0F);
                    const float rowHeight = ImGui::GetFrameHeight() + (ImGui::GetStyle().CellPadding.y * 2.0F);
                    centre = ImVec2(table->Columns[1].WorkMinX + (ImGui::GetFontSize() * 0.5F),
                                    table->OuterRect.Min.y + headerHeight + (rowHeight * 0.5F));
                }
            });
        return centre;
    }

    /// A left click at @p pos over two frames (press, release), the section drawn open throughout.
    static void clickAt(ProcessEnvironmentView& view, ImVec2 pos)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(pos.x, pos.y);
        static_cast<void>(renderAndCapture(view, true)); // hover first, so the press lands on the button
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        static_cast<void>(renderAndCapture(view, true));
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        static_cast<void>(renderAndCapture(view, true));
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(ProcessEnvironmentViewRenderTest, DrawsNothingWithoutEnvironmentSupport)
{
    ProcessEnvironmentView view;
    view.applyResult(sampleEnvironment());
    const std::string text = renderAndCapture(view, false);
    EXPECT_FALSE(text.contains("Environment"));
    EXPECT_FALSE(text.contains("FOO"));

    // And, never drawn open, it is never read.
    TestMocks::MockProcessEnvironmentReader reader;
    EXPECT_FALSE(view.update(&reader, TARGET, 10.0F));
    EXPECT_EQ(reader.readCount(), 0);
}

TEST_F(ProcessEnvironmentViewRenderTest, CollapsedHeaderDrawsAndReadsNothing)
{
    ProcessEnvironmentView view;
    TestMocks::MockProcessEnvironmentReader reader;
    reader.setResult(sampleEnvironment());
    for (int i = 0; i < 3; ++i)
    {
        static_cast<void>(view.update(&reader, TARGET, 10.0F));
        const std::string text = renderAndCapture(view, true, false);
        EXPECT_TRUE(text.contains("Environment"));
        EXPECT_FALSE(text.contains("FOO"));
    }
    EXPECT_EQ(reader.readCount(), 0);
}

TEST_F(ProcessEnvironmentViewRenderTest, ShowsReadingUntilTheFirstRead)
{
    ProcessEnvironmentView view;
    EXPECT_TRUE(renderAndCapture(view, true).contains("Reading..."));
}

TEST_F(ProcessEnvironmentViewRenderTest, MaskedValueIsNotDrawnUntilRevealedAndRevealEndsWithTheSelection)
{
    ProcessEnvironmentView view;
    TestMocks::MockProcessEnvironmentReader reader;
    reader.setResult(sampleEnvironment());
    openAndRead(view, reader);

    std::string text = renderAndCapture(view, true);
    EXPECT_TRUE(text.contains("NAME"));
    EXPECT_TRUE(text.contains("VALUE"));
    EXPECT_TRUE(text.contains("FOO"));
    EXPECT_TRUE(text.contains("bar")); // a plain value is shown as it is
    EXPECT_TRUE(text.contains("MY_API_TOKEN"));
    EXPECT_TRUE(text.contains(Detail::MASKED_ENVIRONMENT_VALUE));
    EXPECT_FALSE(text.contains("supersecret"));

    // Click the eye button of the first row (sorted: FOO, MY_API_TOKEN -- so the masked row is second;
    // put it first with a filter on its name).
    view.setFilter("TOKEN");
    const std::optional<ImVec2> button = firstRevealButtonCentre(view);
    ASSERT_TRUE(button.has_value());
    if (button.has_value())
    {
        clickAt(view, *button);
    }
    ASSERT_EQ(view.rows()[1].name, "MY_API_TOKEN");
    EXPECT_TRUE(view.isRevealed(view.rows()[1]));
    view.setFilter("");

    text = renderAndCapture(view, true);
    EXPECT_TRUE(text.contains("supersecret"));
    EXPECT_FALSE(text.contains(Detail::MASKED_ENVIRONMENT_VALUE));

    // Re-read at the refresh cadence: still revealed.
    view.applyResult(reader.readEnvironment(TARGET));
    EXPECT_TRUE(renderAndCapture(view, true).contains("supersecret"));

    // A different selection -- even one whose environment has the same variable -- masks it again.
    view.onSelectionChanged();
    openAndRead(view, reader);
    text = renderAndCapture(view, true);
    EXPECT_FALSE(text.contains("supersecret"));
    EXPECT_TRUE(text.contains(Detail::MASKED_ENVIRONMENT_VALUE));
}

TEST_F(ProcessEnvironmentViewRenderTest, FailedReadsDrawTheirStatusInsteadOfAnEmptyTable)
{
    struct Case
    {
        Platform::EnvironmentReadStatus status;
        const char* text;
    };
    const std::vector<Case> cases{
        {.status = Platform::EnvironmentReadStatus::PermissionDenied, .text = "Not readable (permission denied)"},
        {.status = Platform::EnvironmentReadStatus::ProcessExited, .text = "Process exited"},
        {.status = Platform::EnvironmentReadStatus::Failed, .text = "Could not be read"},
    };
    for (const Case& c : cases)
    {
        SCOPED_TRACE(c.text);
        ProcessEnvironmentView view;
        TestMocks::MockProcessEnvironmentReader reader;
        reader.setResult({.status = c.status, .variables = {}});
        openAndRead(view, reader);
        const std::string text = renderAndCapture(view, true);
        EXPECT_TRUE(text.contains(c.text));
        EXPECT_FALSE(text.contains("NAME"));
    }

    ProcessEnvironmentView empty;
    TestMocks::MockProcessEnvironmentReader reader;
    openAndRead(empty, reader);
    EXPECT_TRUE(renderAndCapture(empty, true).contains("No environment variables"));
}

TEST_F(ProcessEnvironmentViewRenderTest, LongEnvironmentGetsAFilterAndEveryRowIsReachable)
{
    std::vector<Platform::EnvironmentVariable> variables;
    variables.reserve(40);
    for (int i = 0; i < 40; ++i)
    {
        variables.push_back({.name = "VAR_" + std::to_string(100 + i), .value = "value" + std::to_string(i)});
    }
    ProcessEnvironmentView view;
    view.applyResult({.status = Platform::EnvironmentReadStatus::Ok, .variables = std::move(variables)});

    std::string text = renderAndCapture(view, true);
    EXPECT_TRUE(text.contains("VAR_100"));
    EXPECT_TRUE(text.contains("VAR_139")); // logging lifts the clipper: every row is drawn

    view.setFilter("var_12");
    text = renderAndCapture(view, true);
    EXPECT_TRUE(text.contains("VAR_125"));
    EXPECT_FALSE(text.contains("VAR_135"));

    view.setFilter("nothing-matches");
    EXPECT_TRUE(renderAndCapture(view, true).contains("No variables match the filter"));
}

} // namespace
} // namespace App
