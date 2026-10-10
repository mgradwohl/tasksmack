/// @file test_ProcessOpenFilesViewRender.cpp
/// @brief The Open files section (#183): scans only on request (never on opening or a timer), sort and
/// filter against a mock reader, the Domain formatting it shows, and its real ImGui render, headless
/// (text captured with LogToBuffer()): the idle scan button, scanning, the empty, denied, failed and
/// populated states with Rescan and the scan time, the Mode column only when flags were read, HANDLE
/// in hex on Windows, and the truncation and unnamed-handle notes.

#include "App/Panels/ProcessOpenFilesView.h"
#include "Domain/ProcessOpenFiles.h"
#include "Mocks/MockProbes.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessOpenFiles.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <optional>
#include <string>
#include <utility>

namespace App
{
namespace
{

using Platform::OpenFile;
using Platform::OpenFileKind;
using Platform::OpenFilesReadResult;
using Platform::OpenFilesReadStatus;

constexpr Platform::ProcessTarget TARGET{.pid = 4242, .startTimeTicks = 777};

[[nodiscard]] OpenFilesReadResult statusOnly(OpenFilesReadStatus status, std::string detail = {})
{
    OpenFilesReadResult result;
    result.status = status;
    result.detail = std::move(detail);
    return result;
}

[[nodiscard]] OpenFilesReadResult linuxFiles()
{
    OpenFilesReadResult result = statusOnly(OpenFilesReadStatus::Ok);
    result.files = {
        OpenFile{.descriptor = 10, .kind = OpenFileKind::File, .path = "/var/log/app log.txt", .flags = 02002001, .deleted = true},
        OpenFile{.descriptor = 0, .kind = OpenFileKind::Device, .path = "/dev/pts/0", .flags = 02, .deleted = false},
        OpenFile{.descriptor = 3, .kind = OpenFileKind::Socket, .path = "socket:[9001]", .flags = 02, .deleted = false},
    };
    return result;
}

[[nodiscard]] OpenFilesReadResult windowsFiles()
{
    OpenFilesReadResult result = statusOnly(OpenFilesReadStatus::Ok);
    result.hexDescriptors = true;
    result.namesIncomplete = true;
    result.files = {
        OpenFile{.descriptor = 0x1A4, .kind = OpenFileKind::File, .path = R"(C:\Data\notes.txt)", .flags = std::nullopt, .deleted = false},
        OpenFile{.descriptor = 0x1B0, .kind = OpenFileKind::Other, .path = {}, .flags = std::nullopt, .deleted = false},
    };
    return result;
}

TEST(ProcessOpenFilesDomainTest, FormatsDescriptorKindAndMode)
{
    using namespace Domain::OpenFiles;
    EXPECT_EQ(formatDescriptor(12, false), "12");
    EXPECT_EQ(formatDescriptor(0x1A4, true), "0x1A4");
    EXPECT_EQ(kindLabel(OpenFileKind::AnonInode), "Anon inode");
    EXPECT_EQ(kindLabel(OpenFileKind::Other), "Other");
    EXPECT_EQ(formatMode(std::nullopt), "");
    EXPECT_EQ(formatMode(0), "r");
    EXPECT_EQ(formatMode(02000001), "w");
    EXPECT_EQ(formatMode(02002), "rw append");
}

TEST(ProcessOpenFilesViewTest, ScansOnlyWhenAskedAndNeverOnATimer)
{
    ProcessOpenFilesView view;
    TestMocks::MockProcessOpenFilesReader reader;
    reader.setResult(linuxFiles());

    view.markDrawnOpen();
    EXPECT_FALSE(view.update(&reader, TARGET, 0.0F)); // open, but not asked: nothing read
    view.requestScan();
    EXPECT_TRUE(view.scanning());
    view.markDrawnOpen();
    EXPECT_TRUE(view.update(&reader, TARGET, 0.0F));
    view.finishPendingRead(TARGET);
    EXPECT_FALSE(view.scanning());
    ASSERT_EQ(view.rows().size(), 3U);

    for (int i = 0; i < 5; ++i)
    {
        view.markDrawnOpen();
        EXPECT_FALSE(view.update(&reader, TARGET, 10.0F)); // no refresh, however long it stays open
    }
    view.requestScan(); // Rescan
    view.markDrawnOpen();
    EXPECT_TRUE(view.update(&reader, TARGET, 0.0F));
    view.finishPendingRead(TARGET);
    EXPECT_EQ(reader.readCount(), 2);

    // A new selection clears back to the button.
    view.onSelectionChanged();
    EXPECT_FALSE(view.hasRead());
    EXPECT_FALSE(view.scanning());
    EXPECT_TRUE(view.rows().empty());
}

TEST(ProcessOpenFilesViewTest, ASelectionChangeOrClosingMidScanDropsTheScan)
{
    ProcessOpenFilesView view;
    TestMocks::MockProcessOpenFilesReader reader;
    reader.setResult(linuxFiles());

    view.requestScan();
    view.markDrawnOpen();
    ASSERT_TRUE(view.update(&reader, TARGET, 0.0F));
    view.onSelectionChanged();
    view.finishPendingRead(TARGET);
    EXPECT_FALSE(view.hasRead());

    view.requestScan();
    view.markDrawnOpen();
    ASSERT_TRUE(view.update(&reader, TARGET, 0.0F));
    EXPECT_FALSE(view.update(&reader, TARGET, 0.0F)); // not drawn open this frame: closed mid-scan
    view.finishPendingRead(TARGET);
    EXPECT_FALSE(view.hasRead());
    EXPECT_FALSE(view.scanning());
}

TEST(ProcessOpenFilesViewTest, SortsByDescriptorByDefaultAndFilters)
{
    ProcessOpenFilesView view;
    view.applyResult(linuxFiles());
    ASSERT_EQ(view.rows().size(), 3U);
    EXPECT_EQ(view.rows()[0].descriptor, "0");
    EXPECT_EQ(view.rows()[2].path, "/var/log/app log.txt (deleted)");
    EXPECT_EQ(view.rows()[2].mode, "w append");

    view.setSort(Detail::OpenFilesColumn::Path, true);
    EXPECT_EQ(view.rows()[0].path, "/dev/pts/0");
    view.setSort(Detail::OpenFilesColumn::Descriptor, false);
    EXPECT_EQ(view.rows()[0].descriptor, "10");

    view.setFilter("SOCKET");
    EXPECT_EQ(view.filteredRows().size(), 1U); // the path and the type both match one row
}

class ProcessOpenFilesViewRenderTest : public ::testing::Test
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
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
    }

    static std::string renderAndCapture(ProcessOpenFilesView& view, bool hasOpenFiles, bool open = true)
    {
        std::string captured;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(1200.0F, 800.0F));
        ImGui::Begin("Details", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::LogToBuffer();
        ImGui::SetNextItemOpen(open, ImGuiCond_Always);
        view.render(hasOpenFiles);
        captured = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::Render();
        return captured;
    }

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(ProcessOpenFilesViewRenderTest, HiddenWithoutSupportAndNothingReadOpenOrCollapsed)
{
    ProcessOpenFilesView view;
    EXPECT_FALSE(renderAndCapture(view, false).contains("Open files"));

    TestMocks::MockProcessOpenFilesReader reader;
    for (int i = 0; i < 3; ++i)
    {
        EXPECT_TRUE(renderAndCapture(view, true, i % 2 == 0).contains("Open files"));
        static_cast<void>(view.update(&reader, TARGET, 10.0F));
    }
    EXPECT_EQ(reader.readCount(), 0);
}

TEST_F(ProcessOpenFilesViewRenderTest, IdleShowsTheScanButtonAndScanningShowsProgress)
{
    ProcessOpenFilesView view;
    std::string text = renderAndCapture(view, true);
    EXPECT_TRUE(text.contains("Scan open files"));
    EXPECT_TRUE(text.contains("runs only when asked"));
    EXPECT_FALSE(text.contains("PATH"));

    view.requestScan();
    text = renderAndCapture(view, true);
    EXPECT_TRUE(text.contains("Scanning..."));
    EXPECT_FALSE(text.contains("Scan open files"));
}

TEST_F(ProcessOpenFilesViewRenderTest, EmptyStateHasRescanAndTheScanTime)
{
    ProcessOpenFilesView view;
    view.applyResult(statusOnly(OpenFilesReadStatus::Ok));
    const std::string text = renderAndCapture(view, true);
    EXPECT_TRUE(text.contains("Open files (0)"));
    EXPECT_TRUE(text.contains("No open files"));
    EXPECT_TRUE(text.contains("Rescan"));
    EXPECT_TRUE(text.contains("Scanned "));
}

TEST_F(ProcessOpenFilesViewRenderTest, DeniedAndFailedShowANoteNotATable)
{
    ProcessOpenFilesView view;
    view.applyResult(statusOnly(OpenFilesReadStatus::PermissionDenied));
    std::string text = renderAndCapture(view, true);
    EXPECT_TRUE(text.contains("Access denied"));
    EXPECT_TRUE(text.contains("Rescan"));
    EXPECT_FALSE(text.contains("PATH"));
    EXPECT_FALSE(text.contains("Open files ("));

    view.applyResult(statusOnly(OpenFilesReadStatus::Failed, "NtQuerySystemInformation failed (0xC0000022)"));
    text = renderAndCapture(view, true);
    EXPECT_TRUE(text.contains("Could not be read: NtQuerySystemInformation failed (0xC0000022)"));
}

TEST_F(ProcessOpenFilesViewRenderTest, PopulatedLinuxShowsModes)
{
    ProcessOpenFilesView view;
    view.applyResult(linuxFiles());
    const std::string text = renderAndCapture(view, true);
    for (const char* expected :
         {"Open files (3)", "FD", "TYPE", "MODE", "PATH", "socket:[9001]", "Device", "/var/log/app log.txt (deleted)"})
    {
        EXPECT_TRUE(text.contains(expected)) << expected;
    }
    view.setFilter("no-such-file");
    EXPECT_TRUE(renderAndCapture(view, true).contains("No open files match the filter"));
}

TEST_F(ProcessOpenFilesViewRenderTest, PopulatedWindowsShowsHexHandlesAndNotes)
{
    ProcessOpenFilesView view;
    OpenFilesReadResult result = windowsFiles();
    result.truncated = true;
    view.applyResult(result);
    const std::string text = renderAndCapture(view, true);
    for (const char* expected :
         {"HANDLE", "0x1A4", R"(C:\Data\notes.txt)", "(name not read)", "Showing the first 2 open files", "not named"})
    {
        EXPECT_TRUE(text.contains(expected)) << expected;
    }
    EXPECT_FALSE(text.contains("MODE"));
}

} // namespace
} // namespace App
