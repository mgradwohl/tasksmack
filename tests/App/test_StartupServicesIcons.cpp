/// @file test_StartupServicesIcons.cpp
/// @brief The icons beside the Startup and Services tabs' names (#1800), headless: which file each
/// row's icon is read from (a startup entry's program or packaged logo, a service's program but never
/// svchost's), that only the rows on screen look one up, that the textures are made once loaded
/// (through a fake backend), and that a table drawn without the icon view draws none.

#include "App/Panels/ProcessIconView.h"
#include "App/Panels/ServicesView.h"
#include "App/Panels/StartupView.h"
#include "Domain/ProcessIconCache.h"
#include "Domain/ServiceModel.h"
#include "Domain/StartupModel.h"
#include "Platform/IProcessIconSource.h"
#include "Platform/IServiceProbe.h"
#include "Platform/IStartupProbe.h"
#include "UI/IconTextureCache.h"

#include <gtest/gtest.h>
#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace App
{
namespace
{

/// What the fake source was asked for, and which files have an icon.
struct SourceLog
{
    std::vector<std::string> paths; ///< Every loadIcon() path, in order
    std::set<std::string> withIcon;
};

class RecordingIconSource final : public Platform::IProcessIconSource
{
  public:
    explicit RecordingIconSource(std::shared_ptr<SourceLog> log) : m_Log(std::move(log))
    {}

    [[nodiscard]] std::string executablePath(std::int32_t /*pid*/) override
    {
        return {};
    }

    [[nodiscard]] std::optional<Platform::IconPixels>
    loadIcon(std::string_view executablePath, std::string_view /*processName*/, int sizePx) override
    {
        m_Log->paths.emplace_back(executablePath);
        if (!m_Log->withIcon.contains(std::string(executablePath)))
        {
            return std::nullopt;
        }
        const auto side = static_cast<std::size_t>(sizePx);
        return Platform::IconPixels{.width = sizePx, .height = sizePx, .rgba = std::vector<std::uint8_t>(side * side * 4U, 255U)};
    }

    [[nodiscard]] bool caseInsensitivePaths() const noexcept override
    {
        return true;
    }

  private:
    std::shared_ptr<SourceLog> m_Log;
};

[[nodiscard]] Platform::StartupEntry startup(const std::string& name, Platform::StartupLocation location)
{
    Platform::StartupEntry entry;
    entry.name = name;
    entry.executablePath = R"(C:\Apps\)" + name + ".exe";
    entry.command = "\"" + entry.executablePath + "\"";
    entry.location = location;
    entry.target = Platform::StartupTargetState::Present;
    return entry;
}

[[nodiscard]] Platform::ServiceInfo service(const std::string& name, const std::string& executablePath)
{
    Platform::ServiceInfo info;
    info.name = name;
    info.displayName = name;
    info.state = Platform::ServiceState::Running;
    info.executablePath = executablePath;
    info.binaryPath = executablePath;
    return info;
}

class StartupServicesIconsTest : public ::testing::Test
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
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();

        m_Icons = std::make_unique<ProcessIconView>(
            UI::IconTextureBackend{.create = [this](int /*width*/, int /*height*/, std::span<const std::uint8_t> /*rgba*/)
                                   { return static_cast<std::uint32_t>(++m_Created); },
                                   .destroy = [](std::uint32_t /*texture*/) {}},
            Domain::ProcessIconCacheConfig{.startWorker = false});
        m_Icons->attach(std::make_unique<RecordingIconSource>(m_Log));
    }

    void TearDown() override
    {
        m_Icons.reset();
        ImGui::DestroyContext(m_Context);
    }

    /// One frame of @p draw in a window @p height tall, with the icons' size measured first, as the
    /// panels do.
    void frame(const std::function<void()>& draw, float height = 800.0F)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(1200.0F, height));
        ImGui::Begin("Tab", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        m_Icons->beginFrame();
        draw();
        ImGui::End();
        ImGui::Render();
    }

    [[nodiscard]] static Platform::StartupCapabilities startupCapabilities()
    {
        return {.canEnumerate = true,
                .hasEnabledState = true,
                .hasDisabledTime = false,
                .hasPublisher = false,
                .canResolveShortcuts = false,
                .unavailableReason = {}};
    }

    [[nodiscard]] static Platform::ServiceCapabilities serviceCapabilities()
    {
        Platform::ServiceCapabilities capabilities;
        capabilities.canEnumerate = true;
        return capabilities;
    }

    std::shared_ptr<SourceLog> m_Log = std::make_shared<SourceLog>();
    std::unique_ptr<ProcessIconView> m_Icons;
    int m_Created = 0;

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST(StartupIconPathTest, AnEntrysIconIsItsLogoElseItsProgram)
{
    Platform::StartupEntry entry = startup("Discord", Platform::StartupLocation::RunUser);
    EXPECT_EQ(startupIconPath(entry), R"(C:\Apps\Discord.exe)");

    entry.iconPath = R"(C:\Program Files\WindowsApps\Contoso\Assets\Square44x44Logo.png)";
    EXPECT_EQ(startupIconPath(entry), entry.iconPath) << "a packaged app's logo, before its program";

    Platform::StartupEntry missing = startup("Gone", Platform::StartupLocation::RunUser);
    missing.target = Platform::StartupTargetState::Missing;
    EXPECT_TRUE(startupIconPath(missing).empty()) << "nothing to read: the generic icon";

    Platform::StartupEntry unresolved;
    unresolved.name = "Shortcut";
    EXPECT_TRUE(startupIconPath(unresolved).empty());
}

TEST(ServiceIconPathTest, AServicesIconIsItsProgramButNeverTheSharedHosts)
{
    EXPECT_EQ(serviceIconPath(service("Spooler", R"(C:\Windows\System32\spoolsv.exe)")), R"(C:\Windows\System32\spoolsv.exe)");
    EXPECT_TRUE(serviceIconPath(service("EventLog", R"(C:\Windows\system32\svchost.exe)")).empty());
    EXPECT_TRUE(serviceIconPath(service("Dhcp", R"(C:\WINDOWS\System32\SVCHOST.EXE)")).empty()) << "any case";
    EXPECT_TRUE(serviceIconPath(service("Bare", "svchost.exe")).empty());
    EXPECT_TRUE(serviceIconPath(service("Unread", "")).empty());
    EXPECT_FALSE(serviceIconPath(service("Lookalike", R"(C:\Tools\mysvchost.exe)")).empty());
    // A systemd unit (and any service whose program the probe doesn't report) has no program path.
    EXPECT_TRUE(serviceIconPath(service("ssh.service", "")).empty());
}

TEST_F(StartupServicesIconsTest, StartupRowsLoadTheirProgramsAndLogosThenDrawThem)
{
    Domain::StartupPublication publication;
    publication.version = 2;
    publication.entries = {startup("Discord", Platform::StartupLocation::RunUser),
                           startup("Terminal", Platform::StartupLocation::PackagedApp),
                           startup("Gone", Platform::StartupLocation::RunMachine)};
    publication.entries[1].iconPath = R"(C:\Packages\Terminal\Assets\Square44x44Logo.png)";
    publication.entries[2].target = Platform::StartupTargetState::Missing;
    m_Log->withIcon = {R"(C:\Apps\Discord.exe)", publication.entries[1].iconPath};

    StartupViewState state;
    frame([&] { static_cast<void>(renderStartupView(&publication, startupCapabilities(), state, nullptr, m_Icons.get())); });
    ASSERT_EQ(state.iconKeys.size(), 3U);
    EXPECT_EQ(state.iconKeys[0], Domain::iconFileKey(R"(C:\Apps\Discord.exe)"));
    EXPECT_EQ(m_Created, 0) << "loading: the generic icon meanwhile";

    EXPECT_EQ(m_Icons->cache()->loadPending(), 2U) << "the missing program is never read";
    const std::set<std::string> read(m_Log->paths.begin(), m_Log->paths.end());
    EXPECT_EQ(read, (std::set<std::string>{R"(C:\Apps\Discord.exe)", publication.entries[1].iconPath}))
        << "the packaged app's logo, not its program";

    frame([&] { static_cast<void>(renderStartupView(&publication, startupCapabilities(), state, nullptr, m_Icons.get())); });
    EXPECT_EQ(m_Created, 2);
    frame([&] { static_cast<void>(renderStartupView(&publication, startupCapabilities(), state, nullptr, m_Icons.get())); });
    EXPECT_EQ(m_Created, 2) << "made once";
    EXPECT_EQ(m_Icons->cache()->stats().queued, 0U) << "nothing is read again";
}

TEST_F(StartupServicesIconsTest, OnlyTheStartupRowsOnScreenLookUpIcons)
{
    Domain::StartupPublication publication;
    publication.version = 1;
    std::size_t index = 0;
    while (index < 300)
    {
        publication.entries.push_back(startup(std::format("App{:03}", index), Platform::StartupLocation::RunUser));
        ++index;
    }
    StartupViewState state;
    frame([&] { static_cast<void>(renderStartupView(&publication, startupCapabilities(), state, nullptr, m_Icons.get())); }, 200.0F);
    const std::size_t queued = m_Icons->cache()->stats().queued;
    EXPECT_GT(queued, 0U);
    EXPECT_LT(queued, 30U) << "the clipper's rows only";
}

TEST_F(StartupServicesIconsTest, AStartupTableWithoutTheIconViewLoadsNothing)
{
    Domain::StartupPublication publication;
    publication.version = 1;
    publication.entries = {startup("Discord", Platform::StartupLocation::RunUser)};
    StartupViewState state;
    frame([&] { static_cast<void>(renderStartupView(&publication, startupCapabilities(), state)); });
    EXPECT_EQ(m_Icons->cache()->stats().queued, 0U);
}

TEST_F(StartupServicesIconsTest, ServiceRowsLoadTheirProgramsButSvchostKeepsTheGear)
{
    Domain::ServicePublication publication;
    publication.version = 5;
    publication.services = {service("Spooler", R"(C:\Windows\System32\spoolsv.exe)"),
                            service("EventLog", R"(C:\Windows\System32\svchost.exe)"),
                            service("NoConfig", "")};
    m_Log->withIcon = {R"(C:\Windows\System32\spoolsv.exe)"};

    ServicesViewState state;
    frame([&] { static_cast<void>(renderServicesView(&publication, serviceCapabilities(), state, nullptr, m_Icons.get())); });
    ASSERT_EQ(state.iconKeys.size(), 3U);
    EXPECT_EQ(state.iconKeysVersion, 5U);
    EXPECT_EQ(m_Icons->cache()->loadPending(), 1U) << "svchost's and the unread program are never looked up";
    EXPECT_EQ(m_Log->paths, (std::vector<std::string>{R"(C:\Windows\System32\spoolsv.exe)"}));

    frame([&] { static_cast<void>(renderServicesView(&publication, serviceCapabilities(), state, nullptr, m_Icons.get())); });
    EXPECT_EQ(m_Created, 1);

    // A new sample rebuilds the keys once.
    publication.version = 6;
    publication.services.push_back(service("Agent", R"(C:\Tools\agent.exe)"));
    frame([&] { static_cast<void>(renderServicesView(&publication, serviceCapabilities(), state, nullptr, m_Icons.get())); });
    ASSERT_EQ(state.iconKeys.size(), 4U);
    EXPECT_EQ(state.iconKeys[3], Domain::iconFileKey(R"(C:\Tools\agent.exe)"));
}

TEST_F(StartupServicesIconsTest, OnlyTheServiceRowsOnScreenLookUpIcons)
{
    Domain::ServicePublication publication;
    publication.version = 1;
    std::size_t index = 0;
    while (index < 300)
    {
        publication.services.push_back(service(std::format("svc{:03}", index), std::format(R"(C:\Services\svc{:03}.exe)", index)));
        ++index;
    }
    ServicesViewState state;
    frame([&] { static_cast<void>(renderServicesView(&publication, serviceCapabilities(), state, nullptr, m_Icons.get())); }, 200.0F);
    const std::size_t queued = m_Icons->cache()->stats().queued;
    EXPECT_GT(queued, 0U);
    EXPECT_LT(queued, 30U) << "the clipper's rows only";
}

TEST_F(StartupServicesIconsTest, OffTheTablesDrawAndLoadNoIcons)
{
    m_Icons->setEnabled(false);
    Domain::ServicePublication services;
    services.version = 1;
    services.services = {service("Spooler", R"(C:\Windows\System32\spoolsv.exe)")};
    ServicesViewState serviceState;
    Domain::StartupPublication entries;
    entries.version = 1;
    entries.entries = {startup("Discord", Platform::StartupLocation::RunUser)};
    StartupViewState startupState;
    frame(
        [&]
        {
            static_cast<void>(renderServicesView(&services, serviceCapabilities(), serviceState, nullptr, m_Icons.get()));
            static_cast<void>(renderStartupView(&entries, startupCapabilities(), startupState, nullptr, m_Icons.get()));
        });
    EXPECT_EQ(m_Icons->cache()->stats().queued, 0U);
}

} // namespace
} // namespace App
