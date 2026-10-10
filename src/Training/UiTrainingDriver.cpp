#include "Training/UiTrainingDriver.h"

#include "App/Panel.h"
#include "App/Panels/ProcessDetailsPanel.h"
#include "App/Panels/ProcessesPanel.h"
#include "App/Panels/ServicesPanel.h"
#include "App/Panels/StartupPanel.h"
#include "App/Panels/SystemInfoPanel.h"
#include "App/Panels/SystemMetricsPanel.h"
#include "App/SelectOverride.h"
#include "App/ShellMetrics.h"
#include "App/SyntheticScenario.h"
#include "App/TabLabel.h"
#include "App/UserConfig.h"
#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/ConfigDirOverride.h"
#include "Core/Event.h"
#include "Core/Layer.h"
#include "Core/Utf8Path.h"
#include "Domain/ProcessSnapshot.h"
#include "Platform/CurrentProcess.h"
#include "Training/UiTrainingPlan.h"
#include "UI/AssetPath.h"
#include "UI/ChartWidgets.h"
#include "UI/FontFileCache.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/RenderMetrics.h"
#include "UI/Theme.h"
#include "UI/UILayer.h"

#include <SDL3/SDL_stdinc.h>
#include <imgui.h>
#include <implot.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace Training
{

namespace
{

/// The display the panels are laid out for: a common desktop size, at a display scale of 1.
constexpr float DISPLAY_WIDTH = 1600.0F;
constexpr float DISPLAY_HEIGHT = 1000.0F;
constexpr float DISPLAY_SCALE = 1.0F;
/// The frame delta when frames are not paced (--fps 0).
constexpr int UNPACED_DELTA_FPS = 60;
/// A non-null texture id for the null renderer below; nothing ever reads it.
constexpr ImTextureID NULL_RENDERER_TEXTURE_ID = 1;

// The main tabs' labels, as ShellLayer draws them (fixed "###" ids, TabLabel.h).
constexpr const char* PROCESSES_TAB_LABEL = ICON_FA_LIST "  Processes###ProcessesTab";
constexpr const char* SERVICES_TAB_LABEL = ICON_FA_GEARS "  Services###ServicesTab";
constexpr const char* STARTUP_TAB_LABEL = ICON_FA_POWER_OFF "  Startup###StartupTab";
constexpr const char* SYSTEM_INFO_TAB_LABEL = ICON_FA_SERVER "  System###SystemInfoTab";

[[nodiscard]] App::SelectOverride::DetailsTab toDetailsTab(DetailsView view) noexcept
{
    switch (view)
    {
    case DetailsView::Gpu:
        return App::SelectOverride::DetailsTab::Gpu;
    case DetailsView::Network:
        return App::SelectOverride::DetailsTab::Network;
    case DetailsView::None:
    case DetailsView::Overview:
        break;
    }
    return App::SelectOverride::DetailsTab::Overview;
}

/// Sets (or, with an empty @p value, removes) a variable in the environment SDL_getenv() reads.
void setEnvironment(const char* name, const std::string& value)
{
    SDL_Environment* environment = SDL_GetEnvironment();
    const bool ok = value.empty() ? SDL_UnsetEnvironmentVariable(environment, name)
                                  : SDL_SetEnvironmentVariable(environment, name, value.c_str(), /*overwrite=*/true);
    if (!ok)
    {
        throw std::runtime_error(std::format("could not set {} in the environment", name));
    }
}

/// The run's own config directory (TASKSMACK_CONFIG_DIR), empty at the start and removed at the end.
class ScratchConfigDir
{
  public:
    ScratchConfigDir()
        : m_Path(std::filesystem::temp_directory_path() / std::format("tasksmack-ui-training-{}", Platform::currentProcessId()))
    {
        std::filesystem::remove_all(m_Path);
        std::filesystem::create_directories(m_Path);
    }
    ~ScratchConfigDir()
    {
        // Best effort: a leftover temp directory is harmless, and a destructor must not throw.
        try
        {
            std::error_code ec;
            std::filesystem::remove_all(m_Path, ec);
        }
        catch (...) // NOLINT(bugprone-empty-catch) - see above
        {}
    }
    ScratchConfigDir(const ScratchConfigDir&) = delete;
    ScratchConfigDir& operator=(const ScratchConfigDir&) = delete;
    ScratchConfigDir(ScratchConfigDir&&) = delete;
    ScratchConfigDir& operator=(ScratchConfigDir&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return m_Path;
    }

  private:
    std::filesystem::path m_Path;
};

/// The renderer's half of ImGui's texture protocol (ImGuiBackendFlags_RendererHasTextures), with no
/// GPU: every texture ImGui asks for is "created" and "updated" at once, and destroyed when it asks.
/// The font atlas then grows and is rebuilt exactly as it is under the OpenGL backend.
void processTextureRequests()
{
    for (ImTextureData* texture : ImGui::GetPlatformIO().Textures)
    {
        switch (texture->Status)
        {
        case ImTextureStatus_WantCreate:
            texture->SetTexID(NULL_RENDERER_TEXTURE_ID);
            texture->SetStatus(ImTextureStatus_OK);
            break;
        case ImTextureStatus_WantUpdates:
            texture->SetStatus(ImTextureStatus_OK);
            break;
        case ImTextureStatus_WantDestroy:
            texture->SetTexID(ImTextureID_Invalid);
            texture->SetStatus(ImTextureStatus_Destroyed);
            break;
        case ImTextureStatus_OK:
        case ImTextureStatus_Destroyed:
            break;
        }
    }
}

/// ShellLayer without the window: the same six panels, attached, updated and drawn in a tab bar the
/// same way, and the same events forwarded to them. Keep it in step with ShellLayer's onAttach(),
/// onUpdate() and onRender() (the parts that do not touch the window, the status bar or input).
class TrainingShell : public Core::Layer
{
  public:
    explicit TrainingShell(Probes probes) : Layer("TrainingShell"), m_Probes(probes)
    {}

    void onAttach() override
    {
        const auto& config = App::UserConfig::get();
        for (App::Panel* panel : panels())
        {
            panel->onAttach();
        }
        App::ProcessesPanel::restoreTableLayout(config.settings().processTableLayout);
        if (const auto processModel = m_Processes.processModel(); processModel != nullptr)
        {
            m_SystemMetrics.setProcessModel(processModel);
            if (const auto gpuModel = m_SystemMetrics.gpuModel(); gpuModel != nullptr)
            {
                processModel->setGPUModel(gpuModel);
                m_Processes.setGpuModel(gpuModel);
            }
        }
        m_SystemTabLabel = App::TabLabel::make(ICON_FA_COMPUTER, m_SystemMetrics.hostname(), App::TabLabel::SYSTEM_TAB_ID);
        m_ProcessDetails.setProcessCapabilities(m_Processes.processCapabilities());

        // Real probes: this process, which is always there and is ours. Synthetic: chosen once the
        // synthetic machine shows CPU usage (selectSyntheticProcess()), its PIDs being its own.
        if (m_Probes == Probes::Real)
        {
            m_Processes.requestStartupSelection(App::SelectOverride::Target{.pid = Platform::currentProcessId(), .name = {}}, false);
            m_SelectionRequested = true;
        }
    }

    void onDetach() override
    {
        for (App::Panel* panel : std::views::reverse(panels()))
        {
            panel->onDetach();
        }
    }

    void onEvent(Core::Event& event) override
    {
        for (App::Panel* panel : panels())
        {
            panel->onEvent(event);
        }
    }

    /// The registered id of the tab drawn last frame (empty before the first).
    [[nodiscard]] std::string_view activeTab() const noexcept
    {
        return m_ActiveTab;
    }

    /// Makes @p workload the one on show from the next frame.
    void begin(const Workload& workload)
    {
        m_WantedTab = tabId(workload.tab);
        if (workload.tab == Tab::Processes && workload.treeView != m_Processes.treeViewEnabled())
        {
            m_Processes.toggleTreeView();
        }
        if (workload.tab == Tab::ProcessDetails)
        {
            selectSyntheticProcess(/*force=*/true);
        }
        if (workload.details != DetailsView::None)
        {
            m_ProcessDetails.requestTab(toDetailsTab(workload.details));
        }
    }

    void onUpdate(float deltaTime) override
    {
        if (m_PendingStartupSettings)
        {
            // As ShellLayer's first update: the configured interval and history, or the scenario's.
            m_PendingStartupSettings = false;
            const auto& settings = App::UserConfig::get().settings();
            const App::Synthetic::Scenario* scenario = App::Synthetic::activeScenario();
            Core::RefreshRateChangedEvent refreshEvent(App::Synthetic::startupRefreshIntervalMs(scenario, settings.refreshIntervalMs),
                                                       /*initial=*/true);
            Core::Application::get().raiseEvent(refreshEvent);
            Core::HistoryDurationChangedEvent historyEvent(App::Synthetic::startupHistorySeconds(scenario, settings.maxHistorySeconds),
                                                           /*initial=*/true);
            Core::Application::get().raiseEvent(historyEvent);
        }

        for (App::Panel* panel : panels())
        {
            panel->onUpdate(deltaTime);
        }
        selectSyntheticProcess(/*force=*/false);
        if (const std::optional<App::SelectOverride::DetailsTab> startupTab = m_Processes.takeStartupDetailsTab(); startupTab.has_value())
        {
            m_ProcessDetails.requestTab(*startupTab);
        }
        m_ProcessDetails.setProcessCapabilities(m_Processes.processCapabilities());

        // The selected process's new samples, as ShellLayer hands them over.
        const std::int32_t selectedPid = m_ProcessDetails.selectedPid();
        if (selectedPid != m_WatchedPid)
        {
            m_Processes.watchProcess(selectedPid);
            m_WatchedPid = selectedPid;
        }
        m_PendingSamples.clear();
        if (selectedPid != -1)
        {
            static_cast<void>(m_Processes.watchedSamplesSince(m_ProcessDetails.lastSampleVersion(), m_PendingSamples));
        }
        m_ProcessDetails.updateWithSamples(m_PendingSamples, deltaTime);
        m_PendingSamples.clear();
    }

    void onRender() override
    {
        UI::RenderMetrics::get().beginFrame(ImGui::GetFrameCount());
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const float statusBarHeight = ImGui::GetFrameHeight() + (ImGui::GetStyle().WindowPadding.y * 2.0F);
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, viewport->WorkSize.y - statusBarHeight));
        const ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                             ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoScrollbar |
                                             ImGuiWindowFlags_NoScrollWithMouse;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
        const bool open = ImGui::Begin("##MainWindow", nullptr, windowFlags);
        ImGui::PopStyleVar(3);
        if (open)
        {
            App::Panel* shown = renderTabBar();
            const float styleScale = UI::Theme::get().styleScale();
            ImGui::PushStyleVar(
                ImGuiStyleVar_WindowPadding,
                ImVec2(App::ShellMetrics::CONTENT_PADDING_H * styleScale, App::ShellMetrics::CONTENT_PADDING_V * styleScale));
            if (ImGui::BeginChild("##ContentArea", ImVec2(0.0F, 0.0F), ImGuiChildFlags_AlwaysUseWindowPadding) && shown != nullptr)
            {
                shown->renderContent();
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();
            // The shell's per-frame minimum-size measurements (applied to the window there).
            static_cast<void>(App::ProcessesPanel::measureToolbarMinimumWidth());
            static_cast<void>(m_SystemMetrics.overviewNowBarColumnWidth());
            static_cast<void>(m_SystemMetrics.firstChartNonPlotHeight(viewport->Size.x, ImGui::GetFontSize()));
        }
        ImGui::End();
        UI::RenderMetrics::get().renderOverlay(&m_ShowRenderMetrics);
        m_Processes.expireFrameRequests();
        m_ProcessDetails.expireFrameRequests();
    }

  private:
    [[nodiscard]] std::array<App::Panel*, 6> panels() noexcept
    {
        return {&m_SystemMetrics, &m_Processes, &m_ProcessDetails, &m_Services, &m_Startup, &m_SystemInfo};
    }

    /// The main tab bar, with the wanted tab brought forward; returns the panel of the tab on show.
    App::Panel* renderTabBar()
    {
        const float styleScale = UI::Theme::get().styleScale();
        ImGui::Dummy(ImVec2(0.0F, App::ShellMetrics::TOP_EDGE_PADDING * styleScale));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (App::ShellMetrics::CONTENT_PADDING_H * styleScale));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(App::ShellMetrics::TAB_PADDING_X * styleScale, App::ShellMetrics::MAIN_TAB_PADDING_Y * styleScale));
        App::Panel* shown = nullptr;
        if (ImGui::BeginTabBar("##MainTabBar",
                               ImGuiTabBarFlags_NoCloseWithMiddleMouseButton | ImGuiTabBarFlags_NoTooltip |
                                   ImGuiTabBarFlags_DrawSelectedOverline))
        {
            const std::string& detailsLabel =
                m_DetailsTabLabel.get(m_ProcessDetails.tabLabel(),
                                      [](std::string_view text)
                                      { return App::TabLabel::make(ICON_FA_CIRCLE_INFO, text, App::TabLabel::PROCESS_DETAILS_TAB_ID); });
            const std::array<std::pair<std::string_view, const char*>, 6> labels{
                {
                    {tabId(Tab::SystemOverview), m_SystemTabLabel.c_str()},
                    {tabId(Tab::Processes), PROCESSES_TAB_LABEL},
                    {tabId(Tab::ProcessDetails), detailsLabel.c_str()},
                    {tabId(Tab::Services), SERVICES_TAB_LABEL},
                    {tabId(Tab::Startup), STARTUP_TAB_LABEL},
                    {tabId(Tab::SystemInfo), SYSTEM_INFO_TAB_LABEL},
                },
            };
            const auto all = panels();
            std::string_view shownId;
            for (std::size_t i = 0; i < labels.size(); ++i)
            {
                ImGuiTabItemFlags flags = ImGuiTabItemFlags_NoCloseWithMiddleMouseButton;
                if (labels[i].first == m_WantedTab && m_ActiveTab != m_WantedTab)
                {
                    flags |= ImGuiTabItemFlags_SetSelected;
                }
                if (ImGui::BeginTabItem(labels[i].second, nullptr, flags))
                {
                    shown = all[i];
                    shownId = labels[i].first;
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
            if (!shownId.empty() && shownId != m_ActiveTab)
            {
                m_ActiveTab = shownId;
                Core::ActiveTabChangedEvent event{std::string(shownId)};
                Core::Application::get().raiseEvent(event);
            }
        }
        ImGui::PopStyleVar();
        return shown;
    }

    /// Synthetic probes: select the synthetic machine's busiest process (the lowest PID on a tie), so
    /// Process Details has a process whose charts move. Waits for a snapshot with CPU usage in it (the
    /// first one has none: usage is a delta), unless @p force, as the first Details workload begins.
    void selectSyntheticProcess(bool force)
    {
        if (m_SelectionRequested || m_Processes.processCount() == 0)
        {
            return;
        }
        const auto model = m_Processes.processModel();
        if (model == nullptr)
        {
            return;
        }
        const std::vector<Domain::ProcessSnapshot> snapshots = model->snapshots();
        const auto busiest =
            std::ranges::max_element(snapshots,
                                     [](const Domain::ProcessSnapshot& a, const Domain::ProcessSnapshot& b)
                                     { return a.cpuPercent < b.cpuPercent || (a.cpuPercent == b.cpuPercent && a.pid > b.pid); });
        if (busiest == snapshots.end() || (!force && busiest->cpuPercent <= 0.0))
        {
            return;
        }
        spdlog::info("UI training: selecting synthetic PID {} ({})", busiest->pid, busiest->name);
        m_Processes.requestStartupSelection(App::SelectOverride::Target{.pid = busiest->pid, .name = {}}, false);
        m_SelectionRequested = true;
    }

    Probes m_Probes;
    App::SystemMetricsPanel m_SystemMetrics;
    App::ProcessesPanel m_Processes;
    App::ProcessDetailsPanel m_ProcessDetails;
    App::ServicesPanel m_Services;
    App::StartupPanel m_Startup;
    App::SystemInfoPanel m_SystemInfo;

    bool m_PendingStartupSettings = true;
    bool m_ShowRenderMetrics = false; // ShellLayer's Ctrl+Shift+M overlay, off as it starts
    bool m_SelectionRequested = false;
    std::int32_t m_WatchedPid = -1;
    std::vector<Domain::ProcessSample> m_PendingSamples;
    std::string_view m_WantedTab = tabId(Tab::SystemOverview);
    std::string_view m_ActiveTab;
    std::string m_SystemTabLabel;
    App::TabLabel::CachedLabel m_DetailsTabLabel;
};

/// The ImGui and ImPlot contexts, set up as UILayer::onAttach() does minus the platform and renderer
/// backends, with the app's own fonts and themes; destroyed in reverse.
class HeadlessUi
{
  public:
    HeadlessUi()
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImPlot::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.DisplaySize = ImVec2(DISPLAY_WIDTH, DISPLAY_HEIGHT);
        // What ImGui_ImplOpenGL3 declares: incremental textures (served by processTextureRequests())
        // and 64K+ vertex meshes, which change how ImGui splits the big charts' and tables' draw lists.
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;

        auto& theme = UI::Theme::get();
        theme.setDisplayScale(DISPLAY_SCALE);
        const std::filesystem::path assetsDir = UI::findAssetsDir();
        UI::UILayer::loadAllFonts(m_FontFiles, assetsDir, theme.displayScale());
        theme.loadThemes(assetsDir / "themes");
        theme.applyImGuiStyle();
    }
    ~HeadlessUi()
    {
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
    }
    HeadlessUi(const HeadlessUi&) = delete;
    HeadlessUi& operator=(const HeadlessUi&) = delete;
    HeadlessUi(HeadlessUi&&) = delete;
    HeadlessUi& operator=(HeadlessUi&&) = delete;

    /// One frame: UILayer::beginFrame(), the shell's render, then UILayer::endFrame() up to the draw data.
    static void frame(TrainingShell& shell, float deltaTime)
    {
        UI::Theme::get().applyPendingStyleChanges();
        ImGui::GetIO().DeltaTime = deltaTime;
        ImGui::NewFrame();
        UI::Widgets::trimFrameCaches();
        ImFont* font = UI::Theme::get().regularFont();
        if (font != nullptr)
        {
            ImGui::PushFont(font, 0.0F);
        }
        shell.onRender();
        if (font != nullptr)
        {
            ImGui::PopFont();
        }
        ImGui::Render();
        processTextureRequests();
    }

  private:
    // Declared first, so destroyed last: the atlas reads these bytes until its context goes.
    UI::FontFileCache m_FontFiles;
};

} // namespace

int run(const Options& options)
{
    const std::vector<Segment> segments = allocateFrames(options.frames, WORKLOADS);
    spdlog::info("{}", describePlan(options, segments));

    // Before anything reads them: both are read once, on first use.
    const ScratchConfigDir configDir;
    setEnvironment(std::string(Core::ConfigDirOverride::ENV_VAR).c_str(), Core::pathToUtf8(configDir.path()));
    setEnvironment(std::string(App::Synthetic::ENV_VAR).c_str(),
                   options.probes == Probes::Synthetic ? options.syntheticSpec : std::string());
    if ((App::Synthetic::activeScenario() != nullptr) != (options.probes == Probes::Synthetic))
    {
        spdlog::error("UI training: the synthetic scenario '{}' was not accepted", options.syntheticSpec);
        return 1;
    }

    Core::ApplicationSpecification spec;
    spec.Name = "TaskSmack UI training";
    spec.Headless = true;
    Core::Application app(spec);

    int exitCode = 0;
    {
        const HeadlessUi ui;
        auto& config = App::UserConfig::get();
        config.load();
        config.applyToApplication();

        auto& shell = app.pushLayer<TrainingShell>(options.probes);
        const int deltaFps = options.fps > 0 ? options.fps : UNPACED_DELTA_FPS;
        const float deltaTime = 1.0F / static_cast<float>(deltaFps);
        const auto framePeriod =
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / deltaFps));
        const auto started = std::chrono::steady_clock::now();
        auto nextFrame = started;
        int framesRendered = 0;
        try
        {
            for (const Segment& segment : segments)
            {
                shell.begin(*segment.workload);
                const std::string_view wanted = tabId(segment.workload->tab);
                int shownFrames = 0;
                std::int64_t vertices = 0;
                for (int i = 0; i < segment.frames; ++i)
                {
                    shell.onUpdate(deltaTime);
                    HeadlessUi::frame(shell, deltaTime);
                    ++framesRendered;
                    if (shell.activeTab() == wanted)
                    {
                        ++shownFrames;
                        if (const ImDrawData* drawData = ImGui::GetDrawData(); drawData != nullptr)
                        {
                            vertices += drawData->TotalVtxCount;
                        }
                    }
                    if (options.fps > 0)
                    {
                        nextFrame += framePeriod;
                        std::this_thread::sleep_until(nextFrame);
                    }
                }
                spdlog::info("UI training: {:<18} {} frames, {} with {} on show, {} vertices a frame",
                             segment.workload->name,
                             segment.frames,
                             shownFrames,
                             wanted,
                             shownFrames > 0 ? vertices / shownFrames : 0);
                // The tab bar brings a tab forward one frame after it is asked to, so a workload of
                // two or more frames must have shown its tab at least once.
                if (segment.frames >= 2 && shownFrames == 0)
                {
                    spdlog::error("UI training: workload {} never showed tab {}", segment.workload->name, wanted);
                    exitCode = 1;
                }
            }
        }
        catch (const std::exception& e)
        {
            spdlog::error("UI training: frame {} failed: {}", framesRendered, e.what());
            exitCode = 1;
        }
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - started;
        spdlog::info("UI training: {} frames in {:.1f} s", framesRendered, elapsed.count());

        // The panels stop their samplers before the contexts they drew into go.
        app.detachAllLayers();
    }
    return exitCode;
}

} // namespace Training
