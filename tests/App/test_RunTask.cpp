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
#include <string_view>
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

// #3427: Windows' Run box split, the program and then its parameters as typed.
TEST(CommandWordsTest, SplitProgramTakesTheProgramAndLeavesTheParametersAsTyped)
{
    using Platform::CommandWords::splitProgram;
    const auto split = [](std::string_view line)
    {
        const auto result = splitProgram(line);
        return result.has_value() ? std::pair{result->program, result->parameters} : std::pair{std::string("ERROR"), result.error()};
    };
    using Pair = std::pair<std::string, std::string>;
    EXPECT_EQ(split("notepad"), (Pair{"notepad", ""}));
    EXPECT_EQ(split("  \tnotepad  \t"), (Pair{"notepad", ""}));
    EXPECT_EQ(split("cmd /k echo  two  spaces"), (Pair{"cmd", "/k echo  two  spaces"}));                 // the parameters untouched
    EXPECT_EQ(split(R"(notepad "C:\My Files\a b.txt")"), (Pair{"notepad", R"("C:\My Files\a b.txt")"})); // quotes kept for it
    EXPECT_EQ(split(R"("C:\Program Files\App\app.exe" --flag "x y")"), (Pair{R"(C:\Program Files\App\app.exe)", R"(--flag "x y")"}));
    EXPECT_EQ(split(R"("C:\Program Files\App\app.exe")"), (Pair{R"(C:\Program Files\App\app.exe)", ""}));
    EXPECT_EQ(split("https://example.com/a?b=c"), (Pair{"https://example.com/a?b=c", ""}));
    EXPECT_EQ(split("shell:startup"), (Pair{"shell:startup", ""}));
    // UTF-8 stays whole: split on ASCII bytes only.
    EXPECT_EQ(split("\"C:\\Users\\Zo\xC3\xAB\\Caf\xC3\xA9 \xE2\x98\x95.txt\" \xE6\x97\xA5\xE6\x9C\xAC"),
              (Pair{"C:\\Users\\Zo\xC3\xAB\\Caf\xC3\xA9 \xE2\x98\x95.txt", "\xE6\x97\xA5\xE6\x9C\xAC"}));
    EXPECT_EQ(split("\xE3\x83\xA1\xE3\x83\xA2\xE5\xB8\xB3 x"), (Pair{"\xE3\x83\xA1\xE3\x83\xA2\xE5\xB8\xB3", "x"}));
}

TEST(CommandWordsTest, SplitProgramRefusesABlankProgramOrAnOpenQuote)
{
    using Platform::CommandWords::splitProgram;
    EXPECT_FALSE(splitProgram("").has_value());
    EXPECT_FALSE(splitProgram(" \t ").has_value());
    EXPECT_FALSE(splitProgram(R"("" --x)").has_value());
    EXPECT_FALSE(splitProgram(R"("  " --x)").has_value());
    const auto open = splitProgram(R"("C:\Program Files\app.exe --x)");
    ASSERT_FALSE(open.has_value());
    EXPECT_EQ(open.error(), "A \" quote isn't closed");
}

TEST(CommandWordsTest, QuoteProgramMakesAProgramThatSplitsBack)
{
    for (const std::string path : {R"(C:\Windows\notepad.exe)", R"(C:\Program Files\App\app.exe)", "C:\\Caf\xC3\xA9 \xE2\x98\x95\\x.exe"})
    {
        const std::string quoted = Platform::CommandWords::quoteProgram(path);
        const auto split = Platform::CommandWords::splitProgram(quoted);
        ASSERT_TRUE(split.has_value()) << quoted;
        EXPECT_EQ(split->program, path) << quoted;
        EXPECT_EQ(split->parameters, "") << quoted;
    }
    EXPECT_EQ(Platform::CommandWords::quoteProgram(R"(C:\Windows\notepad.exe)"), R"(C:\Windows\notepad.exe)"); // left alone when it can be
    EXPECT_EQ(Platform::CommandWords::quoteProgram(R"(C:\My App\a.exe)"), R"("C:\My App\a.exe")");
    EXPECT_EQ(Platform::CommandWords::quoteProgram(""), R"("")");
}

// #2728: an elevated session doesn't offer the commands a handed-over config folder holds: anything running as the
// launching user could have planted one there to be run as administrator with one click.
TEST(RunTaskHistoryTest, AHandedOverConfigOffersNoRecentCommands)
{
    const std::vector<std::string> stored{"notepad", "cmd /c evil.bat"};
    EXPECT_EQ(RunTaskHistory::offered(stored, false), stored);
    EXPECT_TRUE(RunTaskHistory::offered(stored, true).empty());
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
    // #3427: a dismissed UAC prompt says it was cancelled, not that it failed.
    const Detail::ActionResultMessage cancelled =
        runTaskResultMessage("notepad", true, Platform::ProcessActionResult::cancelledByUser("the administrator prompt was cancelled"));
    EXPECT_FALSE(cancelled.ok); // nothing started
    EXPECT_TRUE(cancelled.neutral);
    EXPECT_EQ(cancelled.text, "Didn't start notepad: the administrator prompt was cancelled");
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
#ifdef _WIN32
    answer(R"(C:\Program Files\My App\run.exe)"); // as the file dialog would, maybe from another thread
#else
    answer("/opt/My App/run"); // as the file dialog would, maybe from another thread
#endif
    static_cast<void>(frame(dialog)); // takes it in, and focuses the command line
    static_cast<void>(frame(dialog));

    const std::optional<Platform::RunProgramRequest> run = press(dialog, "Run", true);
    ASSERT_TRUE(run.has_value());
#ifdef _WIN32
    // Quoted as the program of a Windows command line (#3427).
    EXPECT_EQ(run.value_or(Platform::RunProgramRequest{}).commandLine, R"("C:\Program Files\My App\run.exe")");
#else
    EXPECT_EQ(run.value_or(Platform::RunProgramRequest{}).commandLine, "'/opt/My App/run'");
#endif
}

// #3427: the Run as administrator tick is there only where the platform says it can work (Windows: hidden
// when TaskSmack is elevated already), and a tick left from before isn't sent once it's hidden.
TEST_F(RunTaskDialogTest, TheAdministratorTickIsOfferedOnlyWhereItCanWork)
{
    RunTaskDialog dialog([](const std::function<void(std::string)>&) {});
    dialog.open({"notepad"}, true);
    static_cast<void>(frame(dialog));
    static_cast<void>(frame(dialog)); // the modal is up from its second frame
    static_cast<void>(press(dialog, "Run as administrator"));
    std::optional<Platform::RunProgramRequest> run = press(dialog, "Run", true);
    ASSERT_TRUE(run.has_value());
    EXPECT_TRUE(run.value_or(Platform::RunProgramRequest{}).asAdministrator);
    static_cast<void>(frame(dialog));

    dialog.open({"notepad"}, false);
    static_cast<void>(frame(dialog));
    static_cast<void>(frame(dialog));
    static_cast<void>(press(dialog, "Run as administrator")); // no such item: nothing happens
    run = press(dialog, "Run", true);
    ASSERT_TRUE(run.has_value());
    EXPECT_FALSE(run.value_or(Platform::RunProgramRequest{.commandLine = {}, .asAdministrator = true}).asAdministrator);
}

// #3186: the arrow beside the command line lists the recent commands; picking one fills the line in.
TEST_F(RunTaskDialogTest, PickingARecentCommandFillsTheLineIn)
{
    RunTaskDialog dialog([](const std::function<void(std::string)>&) {});
    dialog.open({"htop", "foot --title Rescue"}, false);
    static_cast<void>(frame(dialog));
    static_cast<void>(frame(dialog)); // the modal is up from its second frame
    static_cast<void>(press(dialog, "##recentButton"));
    static_cast<void>(frame(dialog));
    ImGuiContext& g = *GImGui;
    ASSERT_GE(g.OpenPopupStack.Size, 2) << "the recent commands didn't open";
    const ImGuiWindow* recent = g.OpenPopupStack.back().Window;
    ASSERT_NE(recent, nullptr);
    ImGui::ActivateItemByID(ImHashStr("foot --title Rescue", 0, recent->ID));
    static_cast<void>(frame(dialog));
    static_cast<void>(frame(dialog));
    for (int i = 0; i < 4; ++i) // the picked command focuses the command line; let it settle
    {
        static_cast<void>(frame(dialog));
    }

    const std::optional<Platform::RunProgramRequest> run = press(dialog, "Run", true);
    ASSERT_TRUE(run.has_value());
    EXPECT_EQ(run.value_or(Platform::RunProgramRequest{}).commandLine, "foot --title Rescue");
}

// #3186: the command line has the keyboard when the dialog opens, and Enter there runs it.
TEST_F(RunTaskDialogTest, EnterInTheCommandLineRunsIt)
{
    RunTaskDialog dialog([](const std::function<void(std::string)>&) {});
    dialog.open({"htop"}, false);
    for (int i = 0; i < 3; ++i)
    {
        static_cast<void>(frame(dialog));
    }
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
    std::optional<Platform::RunProgramRequest> run = frame(dialog);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
    for (int i = 0; i < 3 && !run.has_value(); ++i)
    {
        run = frame(dialog);
    }
    ASSERT_TRUE(run.has_value());
    EXPECT_EQ(run.value_or(Platform::RunProgramRequest{}).commandLine, "htop");
    static_cast<void>(frame(dialog));
    EXPECT_FALSE(dialog.isOpen());
}

// #3186: Escape closes the dialog without running anything (the first one leaves the command line, which
// has the keyboard when the dialog opens).
TEST_F(RunTaskDialogTest, EscapeClosesWithoutRunning)
{
    RunTaskDialog dialog([](const std::function<void(std::string)>&) {});
    dialog.open({"htop"}, false);
    for (int i = 0; i < 3; ++i)
    {
        static_cast<void>(frame(dialog));
    }
    ASSERT_TRUE(dialog.isOpen());
    for (int press = 0; press < 2 && dialog.isOpen(); ++press)
    {
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
        EXPECT_FALSE(frame(dialog).has_value());
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
        EXPECT_FALSE(frame(dialog).has_value());
    }
    static_cast<void>(frame(dialog));
    EXPECT_FALSE(dialog.isOpen());
}

// #3186: the app's own Browse... with no Application (a headless run) chooses nothing, and the command
// line keeps what it had.
TEST_F(RunTaskDialogTest, TheAppsBrowseChoosesNothingWithoutAWindow)
{
    RunTaskDialog dialog;
    dialog.open({"htop"}, false);
    static_cast<void>(frame(dialog));
    static_cast<void>(frame(dialog));
    static_cast<void>(press(dialog, ICON_FA_FOLDER_OPEN " Browse..."));
    static_cast<void>(frame(dialog));
    const std::optional<Platform::RunProgramRequest> run = press(dialog, "Run", true);
    ASSERT_TRUE(run.has_value());
    EXPECT_EQ(run.value_or(Platform::RunProgramRequest{}).commandLine, "htop");
}

} // namespace
} // namespace App
