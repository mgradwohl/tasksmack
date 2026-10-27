// Run new task (#3065): the command line's words, the recent commands, and the dialog.

#include "App/Panels/RunTaskDialog.h"
#include "App/Panels/RunTaskHistory.h"
#include "Platform/CommandWords.h"
#include "Platform/IProcessActions.h"
#include "UI/IconsFontAwesome6.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <cstddef>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace App
{
namespace
{

using Words = std::vector<std::string>;

TEST(CommandWordsTest, SplitsOnBlanksWithShellQuoting)
{
    EXPECT_EQ(Platform::CommandWords::split("konsole"), Words{"konsole"});
    EXPECT_EQ(Platform::CommandWords::split("  foot  --title   Rescue "), (Words{"foot", "--title", "Rescue"}));
    EXPECT_EQ(Platform::CommandWords::split("gedit '/home/z/My Notes.txt'"), (Words{"gedit", "/home/z/My Notes.txt"}));
    EXPECT_EQ(Platform::CommandWords::split(R"(echo "a \"b\" \$HOME \x")"), (Words{"echo", R"(a "b" $HOME \x)"}));
    EXPECT_EQ(Platform::CommandWords::split(R"(ls My\ Files)"), (Words{"ls", "My Files"}));
    EXPECT_EQ(Platform::CommandWords::split(R"(a''b "")"), (Words{"ab", ""}));                 // empty quotes are a word
    EXPECT_EQ(Platform::CommandWords::split("$HOME ~ *.txt"), (Words{"$HOME", "~", "*.txt"})); // nothing expanded
    EXPECT_EQ(Platform::CommandWords::split("   "), Words{});
}

TEST(CommandWordsTest, AnUnclosedQuoteOrTrailingBackslashIsAnError)
{
    EXPECT_FALSE(Platform::CommandWords::split("gedit 'notes").has_value());
    EXPECT_FALSE(Platform::CommandWords::split("echo \"hi").has_value());
    EXPECT_FALSE(Platform::CommandWords::split("echo hi\\").has_value());
}

TEST(CommandWordsTest, QuoteMakesOneWordThatSplitsBack)
{
    for (const std::string path : {"/usr/bin/foot", "/opt/My App/run", "/tmp/it's", "/x/$y", ""})
    {
        const std::string quoted = Platform::CommandWords::quote(path);
        EXPECT_EQ(Platform::CommandWords::split(quoted), Words{path}) << quoted;
    }
    EXPECT_EQ(Platform::CommandWords::quote("/usr/bin/foot"), "/usr/bin/foot"); // left alone when it can be
}

TEST(RunTaskHistoryTest, RememberPutsTheCommandFirstOnceAndCaps)
{
    std::vector<std::string> recent{"htop", "konsole"};
    recent = RunTaskHistory::remember(recent, "konsole");
    EXPECT_EQ(recent, (std::vector<std::string>{"konsole", "htop"}));
    recent = RunTaskHistory::remember(recent, "   ");
    EXPECT_EQ(recent.size(), 2U);
    for (std::size_t i = 0; i < RunTaskHistory::MAX_RECENT + 3; ++i)
    {
        recent = RunTaskHistory::remember(recent, std::format("cmd{}", i));
    }
    EXPECT_EQ(recent.size(), RunTaskHistory::MAX_RECENT);
    EXPECT_EQ(recent.front(), std::format("cmd{}", RunTaskHistory::MAX_RECENT + 2));
    EXPECT_EQ(RunTaskHistory::remember({}, std::string(RunTaskHistory::MAX_COMMAND_BYTES + 1, 'x')), std::vector<std::string>{});
}

TEST(RunTaskResultTest, ShortNameAndResultLines)
{
    EXPECT_EQ(runTaskShortName("htop"), "htop");
    const std::string longCommand = std::string(56, 'a') + "\xC3\xBC" + std::string(10, 'b'); // a 2-byte character at 56-57
    EXPECT_EQ(runTaskShortName(longCommand), std::string(56, 'a') + "...");                   // not cut inside it
    EXPECT_EQ(runTaskResultMessage("htop", false, Platform::ProcessActionResult::ok()).text, "Started htop");
    EXPECT_EQ(runTaskResultMessage("htop", true, Platform::ProcessActionResult::ok()).text, "Started htop as administrator");
    const Detail::ActionResultMessage failed =
        runTaskResultMessage("htop", false, Platform::ProcessActionResult::error("htop wasn't found"));
    EXPECT_FALSE(failed.ok);
    EXPECT_EQ(failed.text, "Could not start htop: htop wasn't found");
}

// The dialog, drawn in an ImGui context of its own.
class RunTaskDialogTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1280.0F, 720.0F);
        io.DeltaTime = 1.0F / 60.0F;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
    }

    /// One frame with the dialog drawn; what it returned.
    static std::optional<Platform::RunProgramRequest> frame(RunTaskDialog& dialog, const std::function<void()>& inside = {})
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("host", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        std::optional<Platform::RunProgramRequest> run = dialog.render();
        if (inside)
        {
            inside();
        }
        ImGui::End();
        ImGui::Render();
        return run;
    }

    /// Presses the dialog's item labelled @p label (in the modal's ID scope; a filled button, the primary
    /// action, has its own "##filled" ID under it), then draws frames until the dialog acts on it.
    static std::optional<Platform::RunProgramRequest> press(RunTaskDialog& dialog, const char* label, bool filled = false)
    {
        const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
        if (modal == nullptr)
        {
            ADD_FAILURE() << "no modal open";
            return std::nullopt;
        }
        const ImGuiID id = ImHashStr(label, 0, modal->ID);
        ImGui::ActivateItemByID(filled ? ImHashStr("##filled", 0, id) : id);
        std::optional<Platform::RunProgramRequest> run;
        for (int i = 0; i < 3 && !run.has_value(); ++i)
        {
            run = frame(dialog);
        }
        return run;
    }

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(RunTaskDialogTest, RunReturnsTheLastCommandAndCancelReturnsNothing)
{
    RunTaskDialog dialog([](const std::function<void(std::string)>&) {});
    dialog.open({"foot --title Rescue"}, true);
    EXPECT_TRUE(dialog.isOpen());
    static_cast<void>(frame(dialog));
    static_cast<void>(frame(dialog));

    // The last command is filled in; Run starts it.
    const std::optional<Platform::RunProgramRequest> run = press(dialog, "Run", true);
    ASSERT_TRUE(run.has_value());
    EXPECT_EQ(run.value_or(Platform::RunProgramRequest{}).commandLine, "foot --title Rescue");
    EXPECT_FALSE(run.value_or(Platform::RunProgramRequest{}).asAdministrator);
    static_cast<void>(frame(dialog));
    EXPECT_FALSE(dialog.isOpen());

    dialog.open({}, false);
    static_cast<void>(frame(dialog));
    static_cast<void>(frame(dialog)); // the modal is up from its second frame
    EXPECT_FALSE(press(dialog, "Cancel").has_value());
    EXPECT_FALSE(dialog.isOpen());
}

TEST_F(RunTaskDialogTest, BrowsePutsTheChosenFileInAsOneWord)
{
    std::function<void(std::string)> answer;
    RunTaskDialog dialog([&answer](std::function<void(std::string)> chosen) { answer = std::move(chosen); });
    dialog.open({}, false);
    static_cast<void>(frame(dialog));
    static_cast<void>(frame(dialog)); // the modal is up from its second frame
    static_cast<void>(press(dialog, ICON_FA_FOLDER_OPEN " Browse..."));
    ASSERT_TRUE(static_cast<bool>(answer)) << "Browse... didn't ask for a file";
    answer("/opt/My App/run");        // as the file dialog would, maybe from another thread
    static_cast<void>(frame(dialog)); // takes it in, and focuses the command line
    static_cast<void>(frame(dialog));

    const std::optional<Platform::RunProgramRequest> run = press(dialog, "Run", true);
    ASSERT_TRUE(run.has_value());
    EXPECT_EQ(run.value_or(Platform::RunProgramRequest{}).commandLine, "'/opt/My App/run'");
}

} // namespace
} // namespace App
