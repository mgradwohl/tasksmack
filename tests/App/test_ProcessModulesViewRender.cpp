/// @file test_ProcessModulesViewRender.cpp
/// @brief The Modules section (#802): its read cadence, sort and filter against a mock reader, the
/// Domain formatting it shows, and its real ImGui render, headless. The text it hands ImGui is
/// captured through ImGui's own logging (LogToBuffer()): nothing read while collapsed, the count in
/// the header, the rows, the Version column only when some module has one, and a muted note instead
/// of a table for a denied or failed read.

#include "App/Panels/ProcessModulesView.h"
#include "Domain/ProcessModules.h"
#include "Mocks/MockProbes.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessModules.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>

namespace App
{
namespace
{

using Platform::ModulesReadStatus;
using Platform::ModuleVersion;
using Platform::ProcessModule;

constexpr Platform::ProcessTarget TARGET{.pid = 4242, .startTimeTicks = 777};

[[nodiscard]] Platform::ModulesReadResult sampleModules(bool withVersions = true)
{
    const auto version = [withVersions](std::uint16_t build) -> std::optional<ModuleVersion>
    {
        return withVersions ? std::optional(ModuleVersion{.major = 10, .minor = 0, .build = build, .revision = 1}) : std::nullopt;
    };
    return {
        .status = ModulesReadStatus::Ok,
        .modules = {ProcessModule{.path = R"(C:\Apps\App.exe)",
                                  .baseAddress = 0x7FF6A0000000,
                                  .sizeBytes = 0x52000,
                                  .version = std::nullopt,
                                  .deleted = false},
                    ProcessModule{.path = R"(C:\Windows\System32\ntdll.dll)",
                                  .baseAddress = 0x7FF8A1B20000,
                                  .sizeBytes = 0x1F8000,
                                  .version = version(26100),
                                  .deleted = false},
                    ProcessModule{
                        .path = "/usr/lib/libgone.so", .baseAddress = 0x1000, .sizeBytes = 0x4000, .version = version(9), .deleted = true}},
        .detail = {}};
}

TEST(ProcessModulesDomainTest, FormatsNameAddressAndVersion)
{
    EXPECT_EQ(Domain::Modules::fileName(R"(C:\Windows\System32\ntdll.dll)"), "ntdll.dll");
    EXPECT_EQ(Domain::Modules::fileName("/usr/lib/libc.so.6"), "libc.so.6");
    EXPECT_EQ(Domain::Modules::fileName("bare"), "bare");
    EXPECT_EQ(Domain::Modules::formatAddress(0x7FF8A1B20000), "0x7FF8A1B20000");
    EXPECT_EQ(Domain::Modules::formatAddress(0x1000), "0x000000001000");
    EXPECT_EQ(Domain::Modules::formatVersion(ModuleVersion{.major = 10, .minor = 0, .build = 26100, .revision = 4202}), "10.0.26100.4202");
    EXPECT_EQ(Domain::Modules::formatVersion(std::nullopt), "");
}

TEST(ProcessModulesViewTest, ReadsOnlyWhileOpenAndAtTheRefreshCadence)
{
    ProcessModulesView view;
    TestMocks::MockProcessModulesReader reader;
    reader.setResult(sampleModules());

    EXPECT_FALSE(view.update(&reader, TARGET, 10.0F)); // never drawn open: never read
    view.markDrawnOpen();
    EXPECT_TRUE(view.update(&reader, TARGET, 0.0F));
    view.finishPendingRead(TARGET);
    EXPECT_TRUE(view.hasRead());
    EXPECT_EQ(view.rows().size(), 3U);

    view.markDrawnOpen();
    EXPECT_FALSE(view.update(&reader, TARGET, 1.0F)); // not yet due
    view.markDrawnOpen();
    EXPECT_TRUE(view.update(&reader, TARGET, 2.5F)); // PROCESS_MODULES_REFRESH_MS since the last read
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
}

TEST(ProcessModulesViewTest, DropsAReadForAPreviousSelection)
{
    ProcessModulesView view;
    TestMocks::MockProcessModulesReader reader;
    reader.setResult(sampleModules());
    view.markDrawnOpen();
    ASSERT_TRUE(view.update(&reader, TARGET, 0.0F));
    view.onSelectionChanged();
    view.finishPendingRead(TARGET);
    EXPECT_FALSE(view.hasRead());
}

TEST(ProcessModulesViewTest, SortsByNameByDefaultAndByAnyColumn)
{
    ProcessModulesView view;
    view.applyResult(sampleModules());
    ASSERT_EQ(view.rows().size(), 3U);
    EXPECT_EQ(view.rows()[0].name, "App.exe"); // case-insensitive: "App" < "libgone" < "ntdll"
    EXPECT_EQ(view.rows()[1].name, "libgone.so (deleted)");
    EXPECT_EQ(view.rows()[2].name, "ntdll.dll");

    view.setSort(Detail::ModulesColumn::Size, false);
    EXPECT_EQ(view.rows()[0].name, "ntdll.dll");
    view.setSort(Detail::ModulesColumn::Base, true);
    EXPECT_EQ(view.rows()[0].name, "libgone.so (deleted)");
    view.setSort(Detail::ModulesColumn::Version, true); // no version first
    EXPECT_EQ(view.rows()[0].name, "App.exe");
    EXPECT_EQ(view.rows()[1].version, "10.0.9.1");
}

TEST(ProcessModulesViewTest, FiltersByNameOrPathIgnoringCase)
{
    ProcessModulesView view;
    view.applyResult(sampleModules());
    view.setFilter("SYSTEM32");
    std::span<const std::size_t> visible = view.filteredRows();
    ASSERT_EQ(visible.size(), 1U);
    EXPECT_EQ(view.rows()[visible[0]].name, "ntdll.dll");
    view.setFilter("");
    visible = view.filteredRows();
    EXPECT_EQ(visible.size(), 3U);
}

class ProcessModulesViewRenderTest : public ::testing::Test
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
    static std::string renderAndCapture(ProcessModulesView& view, bool hasModules, bool open = true)
    {
        std::string captured;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(1200.0F, 800.0F));
        ImGui::Begin("Details", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::LogToBuffer();
        ImGui::SetNextItemOpen(open, ImGuiCond_Always);
        view.render(hasModules);
        captured = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::Render();
        return captured;
    }

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(ProcessModulesViewRenderTest, DrawsNothingWithoutSupportAndNothingReadWhileCollapsed)
{
    ProcessModulesView view;
    EXPECT_FALSE(renderAndCapture(view, false).contains("Modules"));

    TestMocks::MockProcessModulesReader reader;
    for (int i = 0; i < 3; ++i)
    {
        static_cast<void>(view.update(&reader, TARGET, 10.0F));
        EXPECT_TRUE(renderAndCapture(view, true, false).contains("Modules"));
    }
    EXPECT_EQ(reader.readCount(), 0);
}

TEST_F(ProcessModulesViewRenderTest, EmptyState)
{
    ProcessModulesView view;
    EXPECT_TRUE(renderAndCapture(view, true).contains("Reading..."));
    view.applyResult({.status = ModulesReadStatus::Ok, .modules = {}, .detail = {}});
    const std::string text = renderAndCapture(view, true);
    EXPECT_TRUE(text.contains("Modules (0)"));
    EXPECT_TRUE(text.contains("No modules loaded"));
}

TEST_F(ProcessModulesViewRenderTest, AccessDeniedAndFailedShowANoteNotATable)
{
    ProcessModulesView view;
    view.applyResult({.status = ModulesReadStatus::PermissionDenied, .modules = {}, .detail = {}});
    std::string text = renderAndCapture(view, true);
    EXPECT_TRUE(text.contains("Access denied"));
    EXPECT_FALSE(text.contains("NAME"));
    EXPECT_FALSE(text.contains("Modules (")); // no count: nothing was listed

    view.applyResult({.status = ModulesReadStatus::Failed, .modules = {}, .detail = "Only part of a request was completed."});
    text = renderAndCapture(view, true);
    EXPECT_TRUE(text.contains("Could not be read: Only part of a request was completed."));
}

TEST_F(ProcessModulesViewRenderTest, PopulatedShowsTheCountColumnsAndRows)
{
    ProcessModulesView view;
    view.applyResult(sampleModules());
    std::string text = renderAndCapture(view, true);
    EXPECT_TRUE(text.contains("Modules (3)"));
    for (const char* expected :
         {"NAME", "VERSION", "BASE", "SIZE", "PATH", "ntdll.dll", "10.0.26100.1", "0x7FF8A1B20000", R"(C:\Apps\App.exe)"})
    {
        EXPECT_TRUE(text.contains(expected)) << expected;
    }

    // No module with a version (Linux): no Version column.
    view.applyResult(sampleModules(false));
    text = renderAndCapture(view, true);
    EXPECT_FALSE(text.contains("VERSION"));
    EXPECT_TRUE(text.contains("libgone.so (deleted)"));

    view.setFilter("no-such-module");
    EXPECT_TRUE(renderAndCapture(view, true).contains("No modules match the filter"));
}

} // namespace
} // namespace App
