/// @file test_ProcessMemoryMapViewRender.cpp
/// @brief The Memory map section (#3089): its read cadence, totals, sort and filter against a mock reader,
/// and its real ImGui render, headless. The text it hands ImGui is captured through ImGui's own logging
/// (LogToBuffer()): nothing read while collapsed, the count in the header, Windows' rows (no smaps sizes, so
/// no Resident column, and a Reserved total) and Linux's (resident, PSS and the split, no Reserved), the
/// protection in Windows' words or maps' letters, a muted note instead of the tables for a denied or failed
/// read, and under a denied one the "Authenticate to read" button that reads through Windows' helper (#3665).

#include "App/Panels/AuthorizeWording.h"
#include "App/Panels/ProcessMemoryMapView.h"
#include "Mocks/MockProbes.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessMemoryMap.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <span>
#include <string>

namespace App
{
namespace
{

using Platform::MemoryMapping;
using Platform::MemoryMappingKind;
using Platform::MemoryMapReadResult;
using Platform::MemoryMapReadStatus;

constexpr Platform::ProcessTarget TARGET{.pid = 4242, .startTimeTicks = 777};

/// Regions as the Windows reader reports them: no smaps sizes, a reserved one, copy-on-write image data.
[[nodiscard]] MemoryMapReadResult windowsMap()
{
    return {
        .status = MemoryMapReadStatus::Ok,
        .mappings =
            {
                MemoryMapping{
                    .start = 0x7FF8A1B20000,
                    .end = 0x7FF8A1B21000,
                    .readable = true,
                    .shared = true,
                    .kind = MemoryMappingKind::Image,
                    .path = R"(C:\Windows\System32\ntdll.dll)",
                },
                MemoryMapping{
                    .start = 0x7FF8A1B21000,
                    .end = 0x7FF8A1C32000,
                    .readable = true,
                    .executable = true,
                    .shared = true,
                    .kind = MemoryMappingKind::Image,
                    .path = R"(C:\Windows\System32\ntdll.dll)",
                },
                MemoryMapping{
                    .start = 0x7FF8A1C32000,
                    .end = 0x7FF8A1C34000,
                    .readable = true,
                    .writable = true,
                    .kind = MemoryMappingKind::Image,
                    .path = R"(C:\Windows\System32\ntdll.dll)",
                },
                MemoryMapping{
                    .start = 0x2000000,
                    .end = 0x2010000,
                    .readable = true,
                    .writable = true,
                    .kind = MemoryMappingKind::Anonymous,
                },
                MemoryMapping{.start = 0x2010000, .end = 0x2100000, .reserved = true, .kind = MemoryMappingKind::Anonymous},
                MemoryMapping{
                    .start = 0x3000000,
                    .end = 0x3004000,
                    .readable = true,
                    .shared = true,
                    .kind = MemoryMappingKind::File,
                    .path = R"(D:\Data\cache.bin)",
                },
            },
        .detail = {},
        .truncated = false,
    };
}

/// Mappings as Linux's smaps gives them, with their sizes.
[[nodiscard]] MemoryMapReadResult linuxMap()
{
    return {
        .status = MemoryMapReadStatus::Ok,
        .mappings =
            {
                MemoryMapping{
                    .start = 0x7F0000000000,
                    .end = 0x7F0000020000,
                    .readable = true,
                    .executable = true,
                    .kind = MemoryMappingKind::Image,
                    .path = "/usr/lib/libc.so.6",
                    .residentBytes = 0x10000,
                    .pssBytes = 0x4000,
                    .sharedCleanBytes = 0x10000,
                    .sharedDirtyBytes = 0,
                    .privateCleanBytes = 0,
                    .privateDirtyBytes = 0,
                    .swapBytes = 0,
                },
                MemoryMapping{
                    .start = 0x5555000,
                    .end = 0x5576000,
                    .readable = true,
                    .writable = true,
                    .kind = MemoryMappingKind::Heap,
                    .path = "[heap]",
                    .residentBytes = 0x21000,
                    .pssBytes = 0x21000,
                    .privateDirtyBytes = 0x21000,
                    .swapBytes = 0x1000,
                },
            },
        .detail = {},
        .truncated = false,
    };
}

TEST(ProcessMemoryMapViewTest, ReadsOnlyWhileOpenAndAtTheRefreshCadence)
{
    ProcessMemoryMapView view;
    TestMocks::MockProcessMemoryMapReader reader;
    reader.setResult(windowsMap());

    EXPECT_FALSE(view.update(&reader, TARGET, 10.0F)); // never drawn open: never read
    view.markDrawnOpen();
    EXPECT_TRUE(view.update(&reader, TARGET, 0.0F));
    view.finishPendingRead(TARGET);
    EXPECT_TRUE(view.hasRead());
    EXPECT_EQ(view.rows().size(), 6U);

    view.markDrawnOpen();
    EXPECT_FALSE(view.update(&reader, TARGET, 1.0F)); // not yet due
    view.markDrawnOpen();
    EXPECT_TRUE(view.update(&reader, TARGET, 4.5F)); // PROCESS_MEMORY_MAP_REFRESH_MS since the last read
    view.finishPendingRead(TARGET);
    EXPECT_EQ(reader.readCount(), 2);

    // A new selection drops the rows and the totals and reads again as soon as it is shown.
    view.onSelectionChanged();
    EXPECT_FALSE(view.hasRead());
    EXPECT_TRUE(view.rows().empty());
    EXPECT_TRUE(view.totals().empty());
    view.markDrawnOpen();
    EXPECT_TRUE(view.update(&reader, TARGET, 0.0F));
    view.finishPendingRead(TARGET);
    EXPECT_EQ(reader.readCount(), 3);
}

TEST(ProcessMemoryMapViewTest, DropsAReadForAPreviousSelection)
{
    ProcessMemoryMapView view;
    TestMocks::MockProcessMemoryMapReader reader;
    reader.setResult(windowsMap());
    view.markDrawnOpen();
    ASSERT_TRUE(view.update(&reader, TARGET, 0.0F));
    view.onSelectionChanged();
    view.finishPendingRead(TARGET);
    EXPECT_FALSE(view.hasRead());
}

TEST(ProcessMemoryMapViewTest, TotalsByKindLargestFirstThenTheTotal)
{
    ProcessMemoryMapView view;
    view.applyResult(windowsMap(), true);
    const std::span<const ProcessMemoryMapView::TotalsRow> totals = view.totals();
    ASSERT_EQ(totals.size(), 4U); // Image, Private, Mapped file, then the total
    EXPECT_EQ(totals[0].label, "Image");
    EXPECT_EQ(totals[0].regions, "3");
    EXPECT_EQ(totals[0].totals.sizeBytes, 0x114000U);
    EXPECT_EQ(totals[1].label, "Private");
    EXPECT_EQ(totals[1].totals.sizeBytes, 0x10000U);
    EXPECT_EQ(totals[1].totals.reservedBytes, 0xF0000U);
    EXPECT_EQ(totals[2].label, "Mapped file");
    EXPECT_EQ(totals[3].label, "Total");
    EXPECT_EQ(totals[3].regions, "6");
    // Windows measured none of smaps' sizes: the cells are empty, not "0 B".
    EXPECT_TRUE(totals[3].resident.empty());
    EXPECT_TRUE(totals[3].pss.empty());
    EXPECT_TRUE(totals[3].privateBytes.empty());
    EXPECT_FALSE(view.hasResident());
}

TEST(ProcessMemoryMapViewTest, ProtectionInWordsOrLetters)
{
    ProcessMemoryMapView view;
    view.applyResult(windowsMap(), true);
    ASSERT_EQ(view.rows().size(), 6U);
    // In address order by default.
    EXPECT_EQ(view.rows()[0].protection, "Read/Write");
    EXPECT_EQ(view.rows()[1].protection, "Reserved");
    EXPECT_EQ(view.rows()[2].protection, "Read");
    EXPECT_EQ(view.rows()[4].protection, "Execute/Read");
    EXPECT_EQ(view.rows()[5].protection, "Copy on write");
    EXPECT_EQ(view.rows()[5].address, "0x7FF8A1C32000");

    view.applyResult(windowsMap(), false);
    EXPECT_EQ(view.rows()[4].protection, "r-xs");
    EXPECT_EQ(view.rows()[5].protection, "rw-p");
}

TEST(ProcessMemoryMapViewTest, SortsByAnyColumnAndFiltersIgnoringCase)
{
    ProcessMemoryMapView view;
    view.applyResult(windowsMap(), true);
    view.setSort(Detail::MemoryMapColumn::Size, false);
    EXPECT_EQ(view.rows()[0].mapping.start, 0x7FF8A1B21000U); // ntdll's code, the largest
    view.setSort(Detail::MemoryMapColumn::Kind, true);
    EXPECT_EQ(view.rows()[0].kind, "Image");
    view.setSort(Detail::MemoryMapColumn::Path, true);
    EXPECT_TRUE(view.rows()[0].path.empty()); // no path first, then C:\, then D:\ ...
    EXPECT_EQ(view.rows()[5].path, R"(D:\Data\cache.bin)");
    view.setSort(Detail::MemoryMapColumn::Address, true);
    EXPECT_EQ(view.rows()[0].mapping.start, 0x2000000U);

    view.setFilter("NTDLL");
    EXPECT_EQ(view.filteredRows().size(), 3U);
    view.setFilter("execute");
    ASSERT_EQ(view.filteredRows().size(), 1U);
    view.setFilter("reserved");
    ASSERT_EQ(view.filteredRows().size(), 1U);
    EXPECT_TRUE(view.rows()[view.filteredRows()[0]].mapping.reserved);
    view.setFilter("");
    EXPECT_EQ(view.filteredRows().size(), 6U);
}

TEST(ProcessMemoryMapViewTest, StatusLinesAndAFailedReadKeepsNoRows)
{
    EXPECT_TRUE(Detail::memoryMapStatusText(MemoryMapReadStatus::Ok).empty());
    EXPECT_EQ(Detail::memoryMapStatusText(MemoryMapReadStatus::ProcessExited), "Process exited");
    EXPECT_EQ(Detail::memoryMapStatusText(MemoryMapReadStatus::Unsupported), "Not available on this platform");
    EXPECT_EQ(Detail::memoryMapStatusText(MemoryMapReadStatus::IdentityUnknown), "Not available for this process");
    EXPECT_EQ(Detail::memoryMapStatusText(MemoryMapReadStatus::Failed), "Could not be read");

    ProcessMemoryMapView view;
    view.applyResult(windowsMap());
    view.applyResult({.status = MemoryMapReadStatus::PermissionDenied, .mappings = {}, .detail = {}, .truncated = false});
    EXPECT_EQ(view.status(), MemoryMapReadStatus::PermissionDenied);
    EXPECT_TRUE(view.rows().empty());
    EXPECT_TRUE(view.totals().empty());
    EXPECT_FALSE(view.offersAuthenticateToRead()); // no reader here can read as an administrator
}

// #3665: an elevated or another user's process: a refused read offers
// "Authenticate to read" where the reader can read as an administrator
// (Windows' helper), and the press makes the next update read that way, on the
// worker.
TEST(ProcessMemoryMapViewTest, ARefusedReadOffersAuthenticateToReadAndThePressReadsAsAdministrator)
{
    ProcessMemoryMapView view;
    TestMocks::MockProcessMemoryMapReader reader;
    reader.setResult({.status = MemoryMapReadStatus::PermissionDenied, .mappings = {}, .detail = {}, .truncated = false});

    view.markDrawnOpen();
    EXPECT_TRUE(view.update(&reader, TARGET, 0.0F));
    view.finishPendingRead(TARGET);
    EXPECT_EQ(view.status(), MemoryMapReadStatus::PermissionDenied);
    EXPECT_TRUE(view.readRefusedForPermission()) << "where no helper can read it, the restart is offered instead";
    EXPECT_FALSE(view.offersAuthenticateToRead()) << "this reader can't read as an administrator";

    reader.setAuthorizedResult(windowsMap());
    static_cast<void>(view.update(&reader, TARGET,
                                  0.0F)); // takes in what the reader can do now
    ASSERT_TRUE(view.offersAuthenticateToRead());
    EXPECT_EQ(reader.authorizedReadCount(), 0) << "nothing is read as an administrator until the press";

    view.requestAuthorizedRead();
    EXPECT_TRUE(view.update(&reader, TARGET, 0.0F));
    view.finishPendingRead(TARGET);
    EXPECT_EQ(reader.authorizedReadCount(), 1);
    EXPECT_EQ(view.status(), MemoryMapReadStatus::Ok);
    EXPECT_EQ(view.rows().size(), 6U);
    EXPECT_FALSE(view.offersAuthenticateToRead());
    EXPECT_FALSE(view.readRefusedForPermission());
}

class ProcessMemoryMapViewRenderTest : public ::testing::Test
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
    static std::string renderAndCapture(ProcessMemoryMapView& view, bool hasMemoryMap, bool open = true)
    {
        std::string captured;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(1200.0F, 800.0F));
        ImGui::Begin("Details", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::LogToBuffer();
        ImGui::SetNextItemOpen(open, ImGuiCond_Always);
        view.render(hasMemoryMap);
        captured = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::Render();
        return captured;
    }

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(ProcessMemoryMapViewRenderTest, DrawsNothingWithoutSupportAndNothingReadWhileCollapsed)
{
    ProcessMemoryMapView view;
    EXPECT_FALSE(renderAndCapture(view, false).contains("Memory map"));

    TestMocks::MockProcessMemoryMapReader reader;
    for (int i = 0; i < 3; ++i)
    {
        static_cast<void>(view.update(&reader, TARGET, 10.0F));
        EXPECT_TRUE(renderAndCapture(view, true, false).contains("Memory map"));
    }
    EXPECT_EQ(reader.readCount(), 0);
}

TEST_F(ProcessMemoryMapViewRenderTest, EmptyStateAndTheCap)
{
    ProcessMemoryMapView view;
    EXPECT_TRUE(renderAndCapture(view, true).contains("Reading..."));
    view.applyResult({.status = MemoryMapReadStatus::Ok, .mappings = {}, .detail = {}, .truncated = false});
    std::string text = renderAndCapture(view, true);
    EXPECT_TRUE(text.contains("Memory map (0)"));
    EXPECT_TRUE(text.contains("No memory mapped"));

    MemoryMapReadResult capped = windowsMap();
    capped.truncated = true;
    view.applyResult(capped);
    text = renderAndCapture(view, true);
    EXPECT_TRUE(text.contains("Showing the first 6 regions"));
}

TEST_F(ProcessMemoryMapViewRenderTest, AccessDeniedAndFailedShowANoteNotATable)
{
    ProcessMemoryMapView view;
    view.applyResult({.status = MemoryMapReadStatus::PermissionDenied, .mappings = {}, .detail = {}, .truncated = false});
    std::string text = renderAndCapture(view, true);
    EXPECT_TRUE(text.contains("Access denied"));
    EXPECT_FALSE(text.contains("ADDRESS"));
    EXPECT_FALSE(text.contains("Memory map (")); // no count: nothing was listed

    view.applyResult({.status = MemoryMapReadStatus::Failed, .mappings = {}, .detail = "Not enough memory.", .truncated = false});
    text = renderAndCapture(view, true);
    EXPECT_TRUE(text.contains("Could not be read: Not enough memory."));
}

// #3665: the shared "Authenticate to read" button
// (Detail::drawAuthenticateToReadButton) is drawn under a refused read's note
// only while the reader can read as an administrator, and pressing it reads
// that way: the regions show and the button is gone.
TEST_F(ProcessMemoryMapViewRenderTest, ARefusedReadDrawsTheAuthenticateButtonAndPressingItReadsAsTheAdministrator)
{
    ProcessMemoryMapView view;
    TestMocks::MockProcessMemoryMapReader reader;
    reader.setResult({.status = MemoryMapReadStatus::PermissionDenied, .mappings = {}, .detail = {}, .truncated = false});
    static_cast<void>(renderAndCapture(view, true)); // drawn open, so the first update reads
    ASSERT_TRUE(view.update(&reader, TARGET, 0.0F));
    view.finishPendingRead(TARGET);
    EXPECT_FALSE(renderAndCapture(view, true).contains(AuthorizeWording::READ_TEXT)) << "the reader can't read as an administrator";

    reader.setAuthorizedResult(windowsMap());
    static_cast<void>(view.update(&reader, TARGET,
                                  0.0F)); // takes in what the reader can do now
    ASSERT_TRUE(view.offersAuthenticateToRead());
    std::string text = renderAndCapture(view, true);
    EXPECT_TRUE(text.contains("Access denied"));
    EXPECT_TRUE(text.contains(AuthorizeWording::READ_TEXT));
    EXPECT_EQ(reader.authorizedReadCount(), 0) << "nothing is read until the button is pressed";

    const ImGuiWindow* details = ImGui::FindWindowByName("Details");
    ASSERT_NE(details, nullptr);
    ImGui::ActivateItemByID(ImHashStr(AuthorizeWording::READ_LABEL, 0, details->ID));
    static_cast<void>(renderAndCapture(view, true)); // the frame the press lands in
    EXPECT_TRUE(view.update(&reader, TARGET, 0.0F));
    view.finishPendingRead(TARGET);
    EXPECT_EQ(reader.authorizedReadCount(), 1);
    text = renderAndCapture(view, true);
    EXPECT_FALSE(text.contains(AuthorizeWording::READ_TEXT));
    EXPECT_TRUE(text.contains("Memory map (6)"));
}

TEST_F(ProcessMemoryMapViewRenderTest, WindowsShowsReservedButNoSmapsSizes)
{
    ProcessMemoryMapView view;
    view.applyResult(windowsMap(), true);
    const std::string text = renderAndCapture(view, true);
    EXPECT_TRUE(text.contains("Memory map (6)"));
    for (const char* expected : {
             "KIND",
             "REGIONS",
             "SIZE",
             "RESERVED",
             "Image",
             "Private",
             "Mapped file",
             "Total",
             "ADDRESS",
             "PROTECTION",
             "PATH",
             "0x7FF8A1B21000",
             "Execute/Read",
             R"(C:\Windows\System32\ntdll.dll)",
         })
    {
        EXPECT_TRUE(text.contains(expected)) << expected;
    }
    for (const char* absent : {"RESIDENT", "PSS", "SWAP"})
    {
        EXPECT_FALSE(text.contains(absent)) << absent;
    }
}

TEST_F(ProcessMemoryMapViewRenderTest, LinuxShowsTheSmapsSizesButNoReserved)
{
    ProcessMemoryMapView view;
    view.applyResult(linuxMap(), false);
    std::string text = renderAndCapture(view, true);
    for (const char* expected : {"RESIDENT", "PSS", "PRIVATE", "SHARED", "SWAP", "Heap", "[heap]", "r-xp", "/usr/lib/libc.so.6"})
    {
        EXPECT_TRUE(text.contains(expected)) << expected;
    }
    EXPECT_FALSE(text.contains("RESERVED"));

    view.setFilter("no-such-region");
    text = renderAndCapture(view, true);
    EXPECT_TRUE(text.contains("No regions match the filter"));
}

} // namespace
} // namespace App
