/// @file test_UiScenariosOnDisplay.cpp
/// @brief The UI scenario suite (#2711): the real Core::Application, with the shell's real panels on a
/// synthetic machine (TASKSMACK_SYNTHETIC, set for this suite's process in tests/CMakeLists.txt), driven
/// through the #2700 sign-off's steps by input injected through ImGui: the Processes list, then its tree
/// and a collapsed branch, typing in the filter box, the row menu, a column resized and reordered by
/// dragging, Process Details' Overview and Network and I/O, the machine overview, System Information,
/// Help, About, and Settings' theme combo applied by Save.
///
/// Each step asserts behaviour (what the shell or ImGui says), never pixels. With TASKSMACK_UI_CAPTURE_DIR
/// set, each step's frame is also saved there as <NN>-<step>.png, read back before it is presented: the
/// frames tools/ui-compare.sh compares (#2712). The variable is a test aid, not one of the app's hooks.
///
/// Needs a display with a GL 3.3 core context (Xvfb in Linux CI, where TASKSMACK_REQUIRE_DISPLAY=1
/// turns a missing one into a failure); skipped elsewhere, like UiLayersOnDisplayTest.

#include "App/AboutDialog.h"
#include "App/AboutLayer.h"
#include "App/ElevationNoticeLayer.h"
#include "App/HelpLayer.h"
#include "App/HelpWindow.h"
#include "App/Panels/ProcessesPanel.h"
#include "App/ProcessColumnConfig.h"
#include "App/SettingsDialog.h"
#include "App/SettingsLayer.h"
#include "App/ShellLayer.h"
#include "App/SyntheticScenario.h"
#include "App/TitleBarButtons.h"
#include "App/TitleBarLayer.h"
#include "Core/AnimationRequest.h"
#include "Core/Application.h"
#include "Core/EnvRead.h"
#include "Core/FramebufferCapture.h"
#include "Core/HeadlessVideoDriverTestUtils.h"
#include "Core/Layer.h"
#include "Core/Utf8Path.h"
#include "Core/Window.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"
#include "UI/UILayer.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // FindWindowByName(), ImHashStr(), ActivateItemByID(), tables and tab bars
#include <spdlog/spdlog.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{

/// A display with a GL 3.3 core context, as UiLayersOnDisplayTest finds one.
bool hasDisplay()
{
#ifdef _WIN32
    const bool available = TestSupport::detectWindowsDisplay();
#else
    // NOLINTBEGIN(concurrency-mt-unsafe) - read-only environment access during single-threaded test setup
    const char* display = std::getenv("DISPLAY");
    const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    // NOLINTEND(concurrency-mt-unsafe)
    const bool named = (display != nullptr && *display != '\0') || (waylandDisplay != nullptr && *waylandDisplay != '\0');
    const bool available =
        (named && TestSupport::probeGLCapability()) || (TestSupport::tryEnableOffscreenVideoDriver() && TestSupport::probeGLCapability());
#endif
    return TestSupport::enforceDisplayRequirement(available);
}

/// Where each step's frame is saved: TASKSMACK_UI_CAPTURE_DIR, created if need be; nullopt when unset.
std::optional<std::filesystem::path> captureDir()
{
    // SDL_getenv(): UTF-8 on Windows too, and no CRT deprecation there.
    const char* value = Core::readEnv("TASKSMACK_UI_CAPTURE_DIR");
    if (value == nullptr || *value == '\0')
    {
        return std::nullopt;
    }
    std::filesystem::path dir = Core::utf8ToPath(value);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// The shell's widgets, as the shell and its panels name them.
constexpr const char* PROCESSES_TAB = "###ProcessesTab";
constexpr const char* DETAILS_TAB = "###ProcessDetailsTab";
constexpr const char* SYSTEM_TAB = "###SystemTab";
constexpr const char* SYSTEM_INFO_TAB = "###SystemInfoTab";
constexpr const char* DETAILS_OVERVIEW_TAB = ICON_FA_CIRCLE_INFO "  Overview";
constexpr const char* DETAILS_NETWORK_TAB = ICON_FA_NETWORK_WIRED "  Network and I/O";
constexpr const char* FILTER_TEXT = "systemd"; // the synthetic machine's init and user managers

[[nodiscard]] ImGuiID mainTabBarId()
{
    const ImGuiWindow* main = ImGui::FindWindowByName("##MainWindow");
    return main != nullptr ? ImHashStr("##MainTabBar", 0, main->ID) : 0;
}

[[nodiscard]] ImGuiID mainTabId(const char* label)
{
    return ImHashStr(label, 0, mainTabBarId());
}

[[nodiscard]] ImGuiID selectedTab(ImGuiID tabBar)
{
    const ImGuiTabBar* bar = GImGui->TabBars.GetByKey(tabBar);
    return bar != nullptr ? bar->SelectedTabId : 0;
}

/// The tab bar that has a tab labelled @p label (tab IDs hash the label under the bar's), or 0.
[[nodiscard]] ImGuiID tabBarWithTab(const char* label)
{
    for (int n = 0; n < GImGui->TabBars.GetMapSize(); ++n)
    {
        const ImGuiTabBar* bar = GImGui->TabBars.TryGetMapData(n);
        if (bar == nullptr)
        {
            continue;
        }
        const ImGuiID tab = ImHashStr(label, 0, bar->ID);
        for (const ImGuiTabItem& item : bar->Tabs)
        {
            if (item.ID == tab)
            {
                return bar->ID;
            }
        }
    }
    return 0;
}

/// The Processes table: the one with a column for every ProcessColumn.
[[nodiscard]] const ImGuiTable* processTable()
{
    for (int n = 0; n < GImGui->Tables.GetMapSize(); ++n)
    {
        const ImGuiTable* table = GImGui->Tables.TryGetMapData(n);
        if (table != nullptr && table->ColumnsCount == static_cast<int>(App::ProcessColumn::Count) && table->OuterWindow != nullptr &&
            table->OuterWindow->WasActive)
        {
            return table;
        }
    }
    return nullptr;
}

/// The popup open on top, if any.
[[nodiscard]] const ImGuiWindow* topPopup()
{
    return GImGui->OpenPopupStack.empty() ? nullptr : GImGui->OpenPopupStack.back().Window;
}

/// Whether the window named @p name was drawn this frame.
[[nodiscard]] bool windowShown(const char* name)
{
    const ImGuiWindow* window = ImGui::FindWindowByName(name);
    return window != nullptr && window->WasActive;
}

/// A key pressed on frame @p at and released on the next; true once both have gone in.
bool tapKey(int frame, int at, ImGuiKey key)
{
    if (frame == at || frame == at + 1)
    {
        ImGui::GetIO().AddKeyEvent(key, frame == at);
    }
    return frame > at + 1;
}

/// A click of @p button at @p pos starting on frame @p at: the move, then a press and a release a frame
/// apart, so a table sees the hover first. True once the release has gone in.
bool clickAt(int frame, int at, ImVec2 pos, ImGuiMouseButton button = ImGuiMouseButton_Left)
{
    ImGuiIO& io = ImGui::GetIO();
    if (frame == at)
    {
        io.AddMousePosEvent(pos.x, pos.y);
    }
    else if (frame == at + 2 || frame == at + 3)
    {
        io.AddMouseButtonEvent(button, frame == at + 2);
    }
    return frame > at + 3;
}

/// A left-button drag from @p from to @p to starting on frame @p at, in DRAG_STEPS moves. True once
/// the release has gone in.
bool dragFrom(int frame, int at, ImVec2 from, ImVec2 to)
{
    constexpr int DRAG_STEPS = 10;
    ImGuiIO& io = ImGui::GetIO();
    const int step = frame - at;
    if (step == 0)
    {
        io.AddMousePosEvent(from.x, from.y);
    }
    else if (step == 2)
    {
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    }
    else if (step > 2 && step <= 2 + DRAG_STEPS)
    {
        const float t = static_cast<float>(step - 2) / static_cast<float>(DRAG_STEPS);
        io.AddMousePosEvent(from.x + ((to.x - from.x) * t), from.y + ((to.y - from.y) * t));
    }
    else if (step == 3 + DRAG_STEPS)
    {
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    }
    return step > 3 + DRAG_STEPS;
}

/// Frames between retries of an input whose effect hasn't shown yet: a press can land on a frame that
/// doesn't take it (a window still taking the focus, a popup still opening).
constexpr int RETRY_FRAMES = 10;

/// What a step is handed each frame it runs.
struct StepContext
{
    const App::ShellLayer* shell = nullptr; ///< Never null
    int frame = 0;                          ///< Frames since the step began, 0 on its first
};

/// One scripted step: tick() runs each frame from the step's first, injects its input on the frames it
/// chooses, and returns true once what the step does is seen. A step not done within maxFrames fails
/// the scenario, and the steps after it don't run: each starts where the last left the shell.
struct Step
{
    std::string name; ///< Also the captured frame's name
    std::function<bool(StepContext&)> tick;
    int maxFrames = 240;
};

/// What the scenario driver saw.
struct ScenarioOutcome
{
    std::vector<std::string> passed;
    std::optional<std::string> failed; ///< The step that wasn't done in time
    std::vector<std::string> captureErrors;
    int captured = 0;
    bool timedOut = false;
};

/// Pushed last over the whole shell: waits for the panels' first samples, runs the steps in order,
/// saves each finished step's frame when capturing, then closes the window.
class ScenarioDriverLayer : public Core::Layer
{
  public:
    ScenarioDriverLayer(std::vector<Step> steps, ScenarioOutcome& outcome, const App::ShellLayer& shell)
        : Layer("ScenarioDriver"), m_Steps(std::move(steps)), m_Outcome(outcome), m_Shell(shell), m_CaptureDir(captureDir())
    {}

    void onAttach() override
    {
        // Presses come from here, not the keyboard: a window ctest -j shows beside this one mustn't drop
        // them by taking the X input focus (#2268).
        ImGui::GetIO().ConfigDebugIgnoreFocusLoss = true;
    }

    void onRender() override
    {
        Core::AnimationRequest::request(Core::AnimationRequest::FULL_RATE); // keep frames coming
        if (++m_Frames > MAX_FRAMES)
        {
            m_Outcome.timedOut = true;
            Core::Application::get().stop();
            return;
        }
        if (m_Done || m_Frames < SETTLE || m_Shell.processesPanelForTesting().adoptedProcessCount() == 0)
        {
            return;
        }
        if (m_SettleLeft > 0)
        {
            // A finished step's frame is captured once it has settled: a popup is hidden on the frame it
            // opens, and a tab's content draws a frame after the tab is selected.
            if (--m_SettleLeft == 0)
            {
                m_CaptureName = std::format("{:02}-{}", m_Current + 1, m_Steps[m_Current].name);
                m_StepFrame = 0;
                if (++m_Current == m_Steps.size())
                {
                    finish();
                }
            }
            return;
        }
        const Step& step = m_Steps[m_Current];
        StepContext context{.shell = &m_Shell, .frame = m_StepFrame++};
        if (step.tick(context))
        {
            m_Outcome.passed.push_back(step.name);
            m_SettleLeft = SETTLE_AFTER_STEP;
            return;
        }
        if (m_StepFrame > step.maxFrames)
        {
            m_Outcome.failed = step.name;
            finish();
        }
    }

    // After UILayer has drawn the frame and before it is presented: the frame a finished step left.
    void onPostRender() override
    {
        if (m_CaptureName.empty())
        {
            return;
        }
        const std::string name = std::exchange(m_CaptureName, {});
        if (!m_CaptureDir.has_value())
        {
            return;
        }
        const auto [width, height] = Core::Application::get().getWindow().getSizeInPixels();
        const std::filesystem::path file = *m_CaptureDir / (name + ".png");
        if (const auto read = Core::FramebufferCapture::readDefaultFramebuffer(width, height, m_Pixels); !read.has_value())
        {
            m_Outcome.captureErrors.push_back(name + ": " + read.error());
            return;
        }
        if (const auto written = Core::FramebufferCapture::writePng(file, width, height, m_Pixels); !written.has_value())
        {
            m_Outcome.captureErrors.push_back(name + ": " + written.error());
            return;
        }
        ++m_Outcome.captured;
    }

  private:
    void finish()
    {
        m_Done = true;
        // The close the title bar's Close makes: every layer's onDetach() runs, ShellLayer's saving config.toml.
        Core::Application::get().getWindow().requestClose();
    }

    static constexpr int SETTLE = 20;           // the first layout
    static constexpr int SETTLE_AFTER_STEP = 3; // frames between a step's effect and its capture
    static constexpr int MAX_FRAMES = 12000;    // every step's maxFrames, and then some

    std::vector<Step> m_Steps;
    ScenarioOutcome& m_Outcome;
    const App::ShellLayer& m_Shell;
    std::optional<std::filesystem::path> m_CaptureDir;
    std::vector<std::uint8_t> m_Pixels;
    std::string m_CaptureName; ///< Set by the step that finished this frame, taken by onPostRender()
    std::size_t m_Current = 0;
    int m_StepFrame = 0;
    int m_SettleLeft = 0; ///< Frames until the finished step's frame is captured
    int m_Frames = 0;
    bool m_Done = false;
};

/// A step that presses the main tab @p label and is done once the tab bar shows it.
Step mainTabStep(std::string name, const char* label)
{
    return Step{.name = std::move(name),
                .tick = [label](StepContext& step)
                {
                    if (selectedTab(mainTabBarId()) == mainTabId(label))
                    {
                        return true;
                    }
                    if (step.frame % RETRY_FRAMES == 0) // again until it takes
                    {
                        ImGui::ActivateItemByID(mainTabId(label));
                    }
                    return false;
                }};
}

/// The #2700 sign-off's steps, in order.
std::vector<Step> signOffSteps()
{
    std::vector<Step> steps;
    steps.push_back(mainTabStep("processes-list",
                                PROCESSES_TAB)); // the list: no tree, rows listed (the driver waited for them)

    steps.push_back({.name = "processes-tree",
                     .tick = [](StepContext& step)
                     {
                         if (step.shell->processesPanelForTesting().treeViewEnabled())
                         {
                             return true;
                         }
                         // The List | Tree toggle's key, again only once a press has had RETRY_FRAMES to show.
                         static_cast<void>(tapKey(step.frame % RETRY_FRAMES, 0, ImGuiKey_F5));
                         return false;
                     }});

    steps.push_back({.name = "processes-tree-collapsed",
                     .tick = [before = std::size_t{0}](StepContext& step) mutable
                     {
                         const App::ProcessesPanel& panel = step.shell->processesPanelForTesting();
                         const ImGuiTable* table = processTable();
                         if (step.frame == 0)
                         {
                             before = panel.collapsedBranchCountForTesting();
                         }
                         else if (panel.collapsedBranchCountForTesting() > before)
                         {
                             return true;
                         }
                         // Hovered, so the table takes the navigation keys; Home selects the first row, a root
                         // with children, and Left collapses it. Again every RETRY_FRAMES until it has.
                         const int cycle = step.frame % RETRY_FRAMES;
                         if (cycle == 0 && table != nullptr)
                         {
                             ImGui::GetIO().AddMousePosEvent(table->OuterRect.GetCenter().x, table->OuterRect.GetCenter().y);
                         }
                         static_cast<void>(tapKey(cycle, 2, ImGuiKey_Home));
                         static_cast<void>(tapKey(cycle, 6, ImGuiKey_LeftArrow));
                         return false;
                     }});

    steps.push_back({.name = "filter-typed",
                     .tick =
                         [probe = 0](StepContext& step) mutable
                     {
                         const App::ProcessesPanel& panel = step.shell->processesPanelForTesting();
                         if (panel.searchTextForTesting() == FILTER_TEXT)
                         {
                             return true;
                         }
                         const ImGuiContext& g = *GImGui;
                         const bool typing = g.ActiveId != 0 && g.InputTextState.ID == g.ActiveId;
                         if (typing)
                         {
                             if (step.frame % 4 == 0)
                             {
                                 ImGui::GetIO().AddInputCharactersUTF8(FILTER_TEXT);
                             }
                             return false;
                         }
                         // The filter box is on the toolbar row above the table: clicked along it, every 40 px,
                         // until a text field takes the keyboard.
                         const ImGuiTable* table = processTable();
                         if (table == nullptr || step.frame % 6 != 0)
                         {
                             return false;
                         }
                         const float y = table->OuterRect.Min.y - (ImGui::GetFrameHeight() * 0.5F) - ImGui::GetStyle().ItemSpacing.y;
                         const float x = table->OuterRect.Min.x + 20.0F + (40.0F * static_cast<float>(probe++));
                         ImGuiIO& io = ImGui::GetIO();
                         io.AddMousePosEvent(x, y);
                         io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
                         io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
                         return false;
                     },
                     .maxFrames = 600});

    steps.push_back({.name = "row-menu",
                     .tick =
                         [row = 0](StepContext& step) mutable
                     {
                         tapKey(step.frame, 0, ImGuiKey_Escape); // the filter box lets go of the keyboard
                         if (topPopup() != nullptr)
                         {
                             const App::ProcessesPanel& panel = step.shell->processesPanelForTesting();
                             return GImGui->OpenPopupStack.back().PopupId == panel.rowMenuPopupIdForTesting() && panel.selectedPid() != 0;
                         }
                         // Right-clicked down the rows under the header, every 6 px, until a row's menu opens.
                         const ImGuiTable* table = processTable();
                         const int at = 4 + (row * 6);
                         if (table != nullptr && step.frame >= at)
                         {
                             const float y = table->OuterRect.Min.y + (ImGui::GetFrameHeight() * 1.5F) + (6.0F * static_cast<float>(row));
                             if (clickAt(step.frame, at, ImVec2(table->OuterRect.Min.x + 60.0F, y), ImGuiMouseButton_Right))
                             {
                                 ++row;
                             }
                         }
                         return false;
                     },
                     .maxFrames = 600});

    steps.push_back({.name = "column-reordered",
                     .tick = [pidFirst = false, dragStart = -1, from = ImVec2{}, to = ImVec2{}](StepContext& step) mutable
                     {
                         const ImGuiTable* table = processTable();
                         if (dragStart < 0)
                         {
                             // Escape until the row menu has closed (again every 10 frames, in case a press was
                             // taken by something else), then the drag starts.
                             if (topPopup() != nullptr || table == nullptr)
                             {
                                 static_cast<void>(tapKey(step.frame % 10, 0, ImGuiKey_Escape));
                                 return false;
                             }
                             const ImGuiTableColumn& pid = table->Columns[static_cast<int>(App::toIndex(App::ProcessColumn::PID))];
                             const ImGuiTableColumn& name = table->Columns[static_cast<int>(App::toIndex(App::ProcessColumn::Name))];
                             // The later of PID and Name has its header dragged onto the earlier one's.
                             pidFirst = pid.DisplayOrder < name.DisplayOrder;
                             const ImGuiTableColumn& later = pidFirst ? name : pid;
                             const ImGuiTableColumn& earlier = pidFirst ? pid : name;
                             const float y = table->OuterRect.Min.y + ImGui::GetStyle().CellPadding.y + (ImGui::GetTextLineHeight() * 0.5F);
                             from = ImVec2((later.MinX + later.MaxX) * 0.5F, y);
                             to = ImVec2(earlier.MinX + 2.0F, y);
                             dragStart = step.frame + 2; // the key's release lands first
                         }
                         if (table == nullptr || step.frame < dragStart || !dragFrom(step.frame, dragStart, from, to))
                         {
                             return false;
                         }
                         const ImGuiTableColumn& pid = table->Columns[static_cast<int>(App::toIndex(App::ProcessColumn::PID))];
                         const ImGuiTableColumn& name = table->Columns[static_cast<int>(App::toIndex(App::ProcessColumn::Name))];
                         return (pid.DisplayOrder < name.DisplayOrder) != pidFirst;
                     }});

    steps.push_back({.name = "column-resized",
                     .tick = [widthBefore = 0.0F, border = 0.0F, y = 0.0F](StepContext& step) mutable
                     {
                         const ImGuiTable* table = processTable();
                         if (table == nullptr)
                         {
                             return false;
                         }
                         const ImGuiTableColumn& pid = table->Columns[static_cast<int>(App::toIndex(App::ProcessColumn::PID))];
                         if (step.frame == 0)
                         {
                             widthBefore = pid.WidthGiven;
                             border = pid.MaxX;
                             y = table->OuterRect.Min.y + ImGui::GetStyle().CellPadding.y + (ImGui::GetTextLineHeight() * 0.5F);
                         }
                         // PID's right border dragged 40 px right.
                         const bool released = dragFrom(step.frame, 0, ImVec2(border - 1.0F, y), ImVec2(border + 39.0F, y));
                         return released && pid.WidthGiven > widthBefore + 20.0F;
                     }});

    steps.push_back({.name = "details-overview",
                     .tick = [](StepContext& step)
                     {
                         const ImGuiID bar = tabBarWithTab(DETAILS_OVERVIEW_TAB);
                         if (selectedTab(mainTabBarId()) == mainTabId(DETAILS_TAB) && bar != 0 &&
                             selectedTab(bar) == ImHashStr(DETAILS_OVERVIEW_TAB, 0, bar))
                         {
                             return true;
                         }
                         if (step.frame % RETRY_FRAMES == 0) // the row menu's process, again until it takes
                         {
                             ImGui::ActivateItemByID(mainTabId(DETAILS_TAB));
                         }
                         return false;
                     }});

    steps.push_back({.name = "details-network",
                     .tick = [](StepContext& step)
                     {
                         const ImGuiID bar = tabBarWithTab(DETAILS_NETWORK_TAB);
                         if (bar == 0)
                         {
                             return false;
                         }
                         const ImGuiID network = ImHashStr(DETAILS_NETWORK_TAB, 0, bar);
                         if (selectedTab(bar) == network)
                         {
                             return true;
                         }
                         if (step.frame % 10 == 0) // pressed again until it takes: the bar may appear a frame late
                         {
                             ImGui::ActivateItemByID(network);
                         }
                         return false;
                     }});

    steps.push_back(mainTabStep("machine-overview", SYSTEM_TAB));
    steps.push_back(mainTabStep("system-information", SYSTEM_INFO_TAB));

    steps.push_back({.name = "help",
                     .tick = [](StepContext& step)
                     {
                         if (windowShown(App::HelpWindow::WINDOW_ID))
                         {
                             return true;
                         }
                         static_cast<void>(tapKey(step.frame % RETRY_FRAMES, 0, ImGuiKey_F1)); // again until it opens
                         return false;
                     }});

    steps.push_back({.name = "about",
                     .tick = [](StepContext& step)
                     {
                         if (windowShown(App::HelpWindow::WINDOW_ID))
                         {
                             // Escape closes Help while it has the focus; pressed again until it has.
                             static_cast<void>(tapKey(step.frame % RETRY_FRAMES, 0, ImGuiKey_Escape));
                             return false;
                         }
                         if (ImGui::IsPopupOpen(App::AboutDialog::POPUP_ID))
                         {
                             return true;
                         }
                         if (step.frame % RETRY_FRAMES == 0)
                         {
                             if (const ImGuiWindow* bar = ImGui::FindWindowByName("##TitleBar"); bar != nullptr)
                             {
                                 ImGui::ActivateItemByID(ImHashStr(App::TitleBarButtons::ABOUT_LABEL, 0, bar->ID));
                             }
                         }
                         return false;
                     }});

    steps.push_back({.name = "settings-theme-saved",
                     .tick =
                         [target = std::string{}, phase = 0](StepContext& step) mutable
                     {
                         // 0: About closes and Settings opens; 1: the theme combo opens; 2: the target theme is
                         // picked, which closes the combo; 3: Save closes the dialog; then the theme applies.
                         const UI::Theme& theme = UI::Theme::get();
                         if (target.empty())
                         {
                             target = theme.currentThemeId() == "dracula" ? "mocha" : "dracula";
                         }
                         const bool settingsOpen = ImGui::IsPopupOpen(App::SettingsDialog::POPUP_ID);
                         const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
                         const ImGuiWindow* top = topPopup();
                         const bool comboOpen = settingsOpen && top != nullptr && top != modal;
                         const bool retry = step.frame % RETRY_FRAMES == 0;
                         switch (phase)
                         {
                         case 0:
                             if (settingsOpen)
                             {
                                 phase = 1;
                             }
                             else if (ImGui::IsPopupOpen(App::AboutDialog::POPUP_ID))
                             {
                                 static_cast<void>(tapKey(step.frame % RETRY_FRAMES, 0, ImGuiKey_Escape));
                             }
                             else
                             {
                                 static_cast<void>(tapKey(step.frame % RETRY_FRAMES, 0, ImGuiKey_F2));
                             }
                             return false;
                         case 1:
                             if (comboOpen)
                             {
                                 phase = 2;
                             }
                             else if (retry && modal != nullptr)
                             {
                                 for (const ImGuiWindow* window : GImGui->Windows)
                                 {
                                     if (window->ParentWindow == modal && std::string_view{window->Name}.contains("##SettingsBody"))
                                     {
                                         ImGui::ActivateItemByID(ImHashStr("##Theme", 0, window->ID));
                                     }
                                 }
                             }
                             return false;
                         case 2:
                             if (!comboOpen)
                             {
                                 phase = 3;
                             }
                             else if (retry)
                             {
                                 // Its entries are scoped by theme id.
                                 const std::string name = target == "dracula" ? "Dracula" : "Mocha";
                                 ImGui::ActivateItemByID(ImHashStr(name.c_str(), 0, ImHashStr(target.c_str(), 0, top->ID)));
                             }
                             return false;
                         case 3:
                             if (!settingsOpen)
                             {
                                 phase = 4;
                             }
                             else if (retry && modal != nullptr)
                             {
                                 // Save: a filled button, "##filled" under its label.
                                 ImGui::ActivateItemByID(
                                     ImHashStr("##filled", 0, ImHashStr(App::SettingsDialog::SAVE_LABEL, 0, modal->ID)));
                             }
                             return false;
                         default:
                             return theme.currentThemeId() == target;
                         }
                     },
                     .maxFrames = 600});
    return steps;
}

TEST(UiScenarioTest, TheSignOffStepsRunThroughTheWholeShell)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }
    // The same machine on every runner: tests/CMakeLists.txt sets TASKSMACK_SYNTHETIC for this suite.
    ASSERT_NE(App::Synthetic::activeScenario(), nullptr) << "TASKSMACK_SYNTHETIC isn't set for this suite's process";
    Core::ApplicationSpecification spec;
    spec.Name = "UiScenarioTest";
    spec.Width = 1280;
    spec.Height = 800;
    spec.VSync = false;
    std::vector<Step> steps = signOffSteps();
    const std::size_t stepCount = steps.size();
    ScenarioOutcome outcome;
    try
    {
        Core::Application app(spec);
        app.pushLayer<UI::UILayer>();
        auto& titleBar = app.pushLayer<App::TitleBarLayer>();
        auto& shell = app.pushLayer<App::ShellLayer>();
        shell.setTitleBar(&titleBar);
        app.pushLayer<App::HelpLayer>();
        app.pushLayer<App::AboutLayer>();
        app.pushLayer<App::SettingsLayer>();
        app.pushLayer<App::ElevationNoticeLayer>();
        app.pushLayer<ScenarioDriverLayer>(std::move(steps), outcome, shell);
        app.run();
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application failed after the display probe passed: " << e.what();
    }

    EXPECT_FALSE(outcome.timedOut);
    EXPECT_FALSE(outcome.failed.has_value()) << "step not done in time: " << outcome.failed.value_or("")
                                             << " (after: " << (outcome.passed.empty() ? "none" : outcome.passed.back()) << ")";
    EXPECT_EQ(outcome.passed.size(), stepCount);
    for (const std::string& error : outcome.captureErrors)
    {
        ADD_FAILURE() << "capture failed: " << error;
    }
    if (captureDir().has_value())
    {
        EXPECT_EQ(outcome.captured, static_cast<int>(outcome.passed.size()));
    }
}

} // namespace
