#include "App/DialogGeometry.h"
#include "App/ProcessColumnConfig.h"
#include "UI/Format.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace App
{
namespace
{

// ========== Default Width Scaling (#913) ==========

// At the reference em the authored pixel widths must come back unchanged, so the table looks the
// same as before at the Medium preset on an unscaled display.
TEST(ProcessColumnConfigTest, ScaledDefaultWidthIsAuthoredWidthAtReferenceEm)
{
    for (const auto col : allProcessColumns())
    {
        const auto info = getColumnInfo(col);
        EXPECT_FLOAT_EQ(scaledDefaultWidth(info, REFERENCE_EM_PX), info.defaultWidth) << info.configKey;
    }
}

TEST(ProcessColumnConfigTest, ScaledDefaultWidthTracksTheFont)
{
    const auto name = getColumnInfo(ProcessColumn::Name);

    // Even Huger is twice the Medium body font, Small seven eighths of it.
    EXPECT_FLOAT_EQ(scaledDefaultWidth(name, REFERENCE_EM_PX * 2.0F), name.defaultWidth * 2.0F);
    EXPECT_FLOAT_EQ(scaledDefaultWidth(name, REFERENCE_EM_PX * 0.875F), name.defaultWidth * 0.875F);
}

// Every column keeps the same width in ems at every font, which is the property that stops a
// column sized for its content at one preset from clipping that content at another.
TEST(ProcessColumnConfigTest, ScaledDefaultWidthIsConstantInEms)
{
    for (const auto col : allProcessColumns())
    {
        const auto info = getColumnInfo(col);
        const float atReference = scaledDefaultWidth(info, REFERENCE_EM_PX) / REFERENCE_EM_PX;
        for (const float em : {8.0F, 13.5F, 21.5F, 43.0F})
        {
            EXPECT_NEAR(scaledDefaultWidth(info, em) / em, atReference, 1e-4F) << info.configKey << " em=" << em;
        }
    }
}

TEST(ProcessColumnConfigTest, ScaledDefaultWidthFallsBackOnUnusableEm)
{
    const auto name = getColumnInfo(ProcessColumn::Name);

    for (const float em : {0.0F, -8.0F, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()})
    {
        EXPECT_FLOAT_EQ(scaledDefaultWidth(name, em), name.defaultWidth);
    }
}

// Every column must start with a positive width: ProcessesPanel treats a non-positive default as a
// stretch column, which collapses under the table's horizontal scrolling.
TEST(ProcessColumnConfigTest, EveryColumnHasAPositiveDefaultWidth)
{
    for (const auto col : allProcessColumns())
    {
        const auto info = getColumnInfo(col);
        EXPECT_GT(info.defaultWidth, 0.0F) << info.configKey;
    }
}

// ========== Content-fitted width (#1280) ==========

TEST(ProcessColumnConfigTest, ContentFittedWidthFitsTheWidestLabelWithAMargin)
{
    const float em = REFERENCE_EM_PX;
    // A label wider than the default: the column grows to it plus the margin.
    EXPECT_FLOAT_EQ(contentFittedWidth(85.0F, 90.0F, em), 90.0F + (COLUMN_CONTENT_MARGIN_EM * em));
    // A narrow label leaves the authored default alone.
    EXPECT_FLOAT_EQ(contentFittedWidth(85.0F, 20.0F, em), 85.0F);
    // Always at least one em of room around the widest label.
    for (const float widest : {10.0F, 60.0F, 75.0F, 84.0F, 200.0F})
    {
        EXPECT_GE(contentFittedWidth(85.0F, widest, em), widest + em) << widest;
    }
}

TEST(ProcessColumnConfigTest, ContentFittedWidthFallsBackWhenNotMeasured)
{
    EXPECT_FLOAT_EQ(contentFittedWidth(85.0F, 0.0F, REFERENCE_EM_PX), 85.0F);
    EXPECT_FLOAT_EQ(contentFittedWidth(85.0F, std::numeric_limits<float>::quiet_NaN(), REFERENCE_EM_PX), 85.0F);
    EXPECT_FLOAT_EQ(contentFittedWidth(85.0F, 90.0F, 0.0F), 85.0F);
    EXPECT_FLOAT_EQ(contentFittedWidth(85.0F, 90.0F, std::numeric_limits<float>::infinity()), 85.0F);
}

// ========== Header Wording (#1203) ==========

// Headers are plain words: no htop-style single letters ("S") or "+" suffixes ("TIME+").
TEST(ProcessColumnConfigTest, NoHeaderIsASingleLetterOrContainsPlus)
{
    for (const auto col : allProcessColumns())
    {
        const auto info = getColumnInfo(col);
        EXPECT_GT(info.name.size(), 1U) << info.configKey << " header '" << info.name << "'";
        EXPECT_FALSE(info.name.contains('+')) << info.configKey << " header '" << info.name << "'";
    }
}

// Header renames must not touch the config keys: saved column layouts are keyed by them, so a
// changed key would silently drop the user's choice for that column.
TEST(ProcessColumnConfigTest, ConfigKeysAreUnchanged)
{
    constexpr auto expected = std::to_array<std::string_view>({
        "pid",        "name",         "user",        "ppid",        "publisher", "container", "services",    "state",         "status",
        "type",       "architecture", "cpu_percent", "mem_percent", "resident",  "virtual",   "shared",      "peak_resident", "private",
        "swap",       "nice",         "policy",      "affinity",    "threads",   "handles",   "gdi_objects", "cpu_time",      "start_time",
        "io_read",    "io_write",     "page_faults", "net_sent",    "net_recv",  "power",     "gpu_percent", "gpu_memory",    "gpu_engine",
        "gpu_device", "npu_percent",  "npu_memory",  "npu_engine",  "command",
    });
    ASSERT_EQ(expected.size(), processColumnCount());
    for (const auto col : allProcessColumns())
    {
        EXPECT_EQ(getColumnInfo(col).configKey, expected.at(toIndex(col))) << "column index " << toIndex(col);
    }
}

TEST(ProcessColumnConfigTest, HeadersUseThePlainLanguageNames)
{
    EXPECT_EQ(getColumnInfo(ProcessColumn::State).name, "State");
    EXPECT_EQ(getColumnInfo(ProcessColumn::MemPercent).name, "Mem %");
    EXPECT_EQ(getColumnInfo(ProcessColumn::Resident).name, "Memory");
    EXPECT_EQ(getColumnInfo(ProcessColumn::Virtual).name, "Virtual");
    EXPECT_EQ(getColumnInfo(ProcessColumn::Shared).name, "Shared");
    EXPECT_EQ(getColumnInfo(ProcessColumn::PeakResident).name, "Peak Mem");
    EXPECT_EQ(getColumnInfo(ProcessColumn::Threads).name, "Threads");
    EXPECT_EQ(getColumnInfo(ProcessColumn::CpuTime).name, "CPU Time");
    EXPECT_EQ(getColumnInfo(ProcessColumn::PageFaults).name, "Page Faults");
    EXPECT_EQ(getColumnInfo(ProcessColumn::NetReceived).name, "Net Received");
    EXPECT_EQ(getColumnInfo(ProcessColumn::GpuDevice).name, "GPU");
    EXPECT_EQ(getColumnInfo(ProcessColumn::NpuPercent).name, "NPU %");
    EXPECT_EQ(getColumnInfo(ProcessColumn::NpuMemory).name, "NPU Mem");
    EXPECT_EQ(getColumnInfo(ProcessColumn::NpuEngine).name, "NPU Engine");
}

// #2240: the CPU Time description (header tooltip and F1 Help) shows the format the cells use,
// UI::Format::formatDuration()'s two largest units, not the old "H:MM:SS".
TEST(ProcessColumnConfigTest, CpuTimeDescriptionMatchesTheCellFormat)
{
    const std::string_view description = getColumnInfo(ProcessColumn::CpuTime).description;
    EXPECT_EQ(description.find("H:MM:SS"), std::string_view::npos);
    EXPECT_EQ(description.find("M:SS"), std::string_view::npos);
    const auto examples = std::to_array<double>({45.0, 125.0, 3725.0, (3.0 * 86400.0) + (4.0 * 3600.0)});
    for (const double seconds : examples)
    {
        const std::string example = UI::Format::formatDuration(seconds);
        EXPECT_NE(description.find(example), std::string_view::npos) << example;
    }
}

// #1799: the NPU columns sit after the GPU ones, before Command, and are off by default (as Task
// Manager's are) but can be shown from the Columns menu.
TEST(ProcessColumnConfigTest, TheNpuColumnsFollowTheGpuOnesAndAreOffByDefault)
{
    EXPECT_EQ(toIndex(ProcessColumn::NpuPercent), toIndex(ProcessColumn::GpuDevice) + 1);
    EXPECT_EQ(toIndex(ProcessColumn::NpuMemory), toIndex(ProcessColumn::NpuPercent) + 1);
    EXPECT_EQ(toIndex(ProcessColumn::NpuEngine), toIndex(ProcessColumn::NpuMemory) + 1);
    EXPECT_EQ(toIndex(ProcessColumn::Command), toIndex(ProcessColumn::NpuEngine) + 1);
    const ProcessColumnSettings defaults;
    for (const ProcessColumn col : {ProcessColumn::NpuPercent, ProcessColumn::NpuMemory, ProcessColumn::NpuEngine})
    {
        const ProcessColumnInfo info = getColumnInfo(col);
        EXPECT_FALSE(info.defaultVisible) << info.configKey;
        EXPECT_TRUE(info.canHide) << info.configKey;
        EXPECT_FALSE(defaults.isVisible(col)) << info.configKey;
        EXPECT_TRUE(info.menuName.starts_with("NPU")) << info.configKey;
    }
}

// #1101: per-process network counts TCP only on both platforms; the column tooltips must say so
// rather than let a browser streaming over QUIC read as idle.
TEST(ProcessColumnConfigTest, NetworkColumnsNoteTheyAreTcpOnlyWithoutUdpCounters)
{
    for (const auto col : {ProcessColumn::NetSent, ProcessColumn::NetReceived})
    {
        const std::string_view note = columnCapabilityNote(col, false);
        EXPECT_TRUE(note.contains("TCP only")) << getColumnInfo(col).configKey;
        EXPECT_TRUE(note.contains("UDP")) << getColumnInfo(col).configKey;
        // With the UDP event trace delivering (#1492), the note says both are counted.
        const std::string_view withUdp = columnCapabilityNote(col, true);
        EXPECT_TRUE(withUdp.contains("TCP and UDP")) << getColumnInfo(col).configKey;
        EXPECT_FALSE(withUdp.contains("TCP only")) << getColumnInfo(col).configKey;
    }
}

TEST(ProcessColumnConfigTest, OtherColumnsHaveNoCapabilityNote)
{
    for (const auto col : allProcessColumns())
    {
        if (col == ProcessColumn::NetSent || col == ProcessColumn::NetReceived)
        {
            continue;
        }
        EXPECT_TRUE(columnCapabilityNote(col, false).empty()) << getColumnInfo(col).configKey;
        EXPECT_TRUE(columnCapabilityNote(col, true).empty()) << getColumnInfo(col).configKey;
    }
}

// ========== Column Count and Index Conversion ==========

TEST(ProcessColumnConfigTest, ColumnCountIsCorrect)
{
    constexpr auto count = processColumnCount();
    constexpr auto expected = static_cast<std::size_t>(ProcessColumn::Count);
    EXPECT_EQ(count, expected);
}

TEST(ProcessColumnConfigTest, AllColumnsArraySizeMatchesCount)
{
    constexpr auto columns = allProcessColumns();
    constexpr auto count = processColumnCount();
    EXPECT_EQ(columns.size(), count);
}

TEST(ProcessColumnConfigTest, ToIndexReturnsCorrectValues)
{
    // Verify the positions of stable anchor columns.
    // These indices are part of the public table-order contract: identity columns come
    // first, Windows-only feature columns (Publisher, Type, GdiObjects) follow in
    // documented order, and no accidental reorder should go undetected.
    EXPECT_EQ(toIndex(ProcessColumn::PID), 0);
    EXPECT_EQ(toIndex(ProcessColumn::Name), 1);
    EXPECT_EQ(toIndex(ProcessColumn::Publisher), 4);     // Windows publisher — after User(2) and PPID(3)
    EXPECT_EQ(toIndex(ProcessColumn::Container), 5);     // Linux container (#805)
    EXPECT_EQ(toIndex(ProcessColumn::Services), 6);      // Services (#1886) — last of the identity columns
    EXPECT_EQ(toIndex(ProcessColumn::Type), 9);          // Process type — after State(7) and Status(8)
    EXPECT_EQ(toIndex(ProcessColumn::Architecture), 10); // Architecture (#2394) — after Type
    EXPECT_EQ(toIndex(ProcessColumn::Policy), 20);       // Scheduling policy (#3086) — after Priority(19)
    EXPECT_EQ(toIndex(ProcessColumn::GdiObjects), 24);   // GDI count — after Handles(23); Private and Swap (#1885) sit before them
}

TEST(ProcessColumnConfigTest, ToIndexIsMonotonic)
{
    // Verify indices are sequential (no gaps)
    const auto count = processColumnCount();
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto col = static_cast<ProcessColumn>(i);
        EXPECT_EQ(toIndex(col), i);
    }
}

TEST(ProcessColumnConfigTest, AllColumnsContainsUniqueColumns)
{
    constexpr auto columns = allProcessColumns();
    std::vector<ProcessColumn> seen;
    seen.reserve(columns.size());

    for (const auto col : columns)
    {
        // Check not seen before
        EXPECT_EQ(std::ranges::find(seen, col), seen.end()) << "Duplicate column detected";
        seen.push_back(col);
    }
}

// ========== Column Settings ==========

TEST(ProcessColumnSettingsTest, DefaultConstructorHasDefaultVisibility)
{
    const ProcessColumnSettings settings;

    // Most columns should be visible by default
    EXPECT_TRUE(settings.isVisible(ProcessColumn::PID));
    EXPECT_TRUE(settings.isVisible(ProcessColumn::Name));
    EXPECT_TRUE(settings.isVisible(ProcessColumn::CpuPercent));
    EXPECT_TRUE(settings.isVisible(ProcessColumn::MemPercent));
}

TEST(ProcessColumnSettingsTest, SetVisibilityChangesState)
{
    ProcessColumnSettings settings;

    // Hide a column
    settings.setVisible(ProcessColumn::PID, false);
    EXPECT_FALSE(settings.isVisible(ProcessColumn::PID));

    // Show it again
    settings.setVisible(ProcessColumn::PID, true);
    EXPECT_TRUE(settings.isVisible(ProcessColumn::PID));
}

TEST(ProcessColumnSettingsTest, ToggleVisibilityFlipsState)
{
    ProcessColumnSettings settings;

    const bool initial = settings.isVisible(ProcessColumn::Name);
    settings.toggleVisible(ProcessColumn::Name);
    EXPECT_EQ(settings.isVisible(ProcessColumn::Name), !initial);

    // Toggle back
    settings.toggleVisible(ProcessColumn::Name);
    EXPECT_EQ(settings.isVisible(ProcessColumn::Name), initial);
}

TEST(ProcessColumnSettingsTest, ToggleVisibilityOnlyAffectsTargetColumn)
{
    ProcessColumnSettings settings;
    ProcessColumnSettings expected;
    expected.toggleVisible(ProcessColumn::CpuPercent);

    settings.toggleVisible(ProcessColumn::CpuPercent);

    // Toggling one column must not disturb any other column's visibility: the resulting state
    // must equal a fresh instance with only that one column flipped.
    EXPECT_EQ(settings.visible, expected.visible);
}

TEST(ProcessColumnSettingsTest, BoundaryConditions)
{
    ProcessColumnSettings settings;

    // Test all valid columns can be toggled
    const auto count = processColumnCount();
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto col = static_cast<ProcessColumn>(i);
        const bool before = settings.isVisible(col);
        settings.toggleVisible(col);
        EXPECT_NE(settings.isVisible(col), before);
        settings.toggleVisible(col);
        EXPECT_EQ(settings.isVisible(col), before);
    }
}

TEST(ProcessColumnSettingsTest, AllColumnsCanBeHidden)
{
    ProcessColumnSettings settings;

    // Hide all columns
    const auto columns = allProcessColumns();
    for (const auto col : columns)
    {
        settings.setVisible(col, false);
    }

    // Verify all hidden
    for (const auto col : columns)
    {
        EXPECT_FALSE(settings.isVisible(col));
    }
}

TEST(ProcessColumnSettingsTest, AllColumnsCanBeShown)
{
    ProcessColumnSettings settings;

    // Show all columns
    const auto columns = allProcessColumns();
    for (const auto col : columns)
    {
        settings.setVisible(col, true);
    }

    // Verify all shown
    for (const auto col : columns)
    {
        EXPECT_TRUE(settings.isVisible(col));
    }
}

// ========== Columns menu (#1209) ==========

TEST(ProcessColumnSettingsTest, ResetRestoresTheDefaultColumns)
{
    ProcessColumnSettings settings;
    EXPECT_EQ(settings.visible, ProcessColumnSettings::defaults().visible);

    settings.requestVisible(ProcessColumn::Command, false);
    settings.requestVisible(ProcessColumn::Threads, true);
    EXPECT_NE(settings.visible, ProcessColumnSettings::defaults().visible);

    settings = ProcessColumnSettings::defaults();
    EXPECT_EQ(settings, ProcessColumnSettings{});
    for (const ProcessColumn col : allProcessColumns())
    {
        EXPECT_EQ(settings.isVisible(col), getColumnInfo(col).defaultVisible);
    }
}

TEST(ProcessColumnSettingsTest, OnlyAnUnchosenColumnTakesANewDefault)
{
    ProcessColumnSettings settings;
    EXPECT_FALSE(settings.isChosen(ProcessColumn::Power));
    settings.setDefaultVisible(ProcessColumn::Power, false);
    EXPECT_FALSE(settings.isVisible(ProcessColumn::Power));
    EXPECT_FALSE(settings.isChosen(ProcessColumn::Power));

    settings.setVisible(ProcessColumn::Power, true); // The user's (or the config file's) choice
    EXPECT_TRUE(settings.isChosen(ProcessColumn::Power));
    settings.setDefaultVisible(ProcessColumn::Power, false);
    EXPECT_TRUE(settings.isVisible(ProcessColumn::Power));

    settings.toggleVisible(ProcessColumn::Threads);
    EXPECT_TRUE(settings.isChosen(ProcessColumn::Threads));
}

TEST(ProcessColumnSettingsTest, TableStateIsAChoiceOnlyWhenTheUserMadeIt)
{
    // #1210: before the table has been drawn with our settings, an enabled column comes from the
    // ImGui layout ShellLayer restored, not from the user, and must not pin the column as chosen.
    ProcessColumnSettings settings;
    settings.setDefaultVisible(ProcessColumn::Power, false); // e.g. no RAPL
    EXPECT_TRUE(settings.adoptTableVisibility(ProcessColumn::Power, true, /*userChange=*/false));
    EXPECT_TRUE(settings.isVisible(ProcessColumn::Power));
    EXPECT_FALSE(settings.isChosen(ProcessColumn::Power)); // Still follows the system's default
    settings.setDefaultVisible(ProcessColumn::Power, false);
    EXPECT_FALSE(settings.isVisible(ProcessColumn::Power));

    // Once the table shows our settings, a difference is the user's toggle in ImGui's header menu.
    EXPECT_TRUE(settings.adoptTableVisibility(ProcessColumn::Power, true, /*userChange=*/true));
    EXPECT_TRUE(settings.isChosen(ProcessColumn::Power));
    settings.setDefaultVisible(ProcessColumn::Power, false);
    EXPECT_TRUE(settings.isVisible(ProcessColumn::Power));

    // No difference: nothing changes, nothing is marked.
    EXPECT_FALSE(settings.adoptTableVisibility(ProcessColumn::Threads, settings.isVisible(ProcessColumn::Threads), true));
    EXPECT_FALSE(settings.isChosen(ProcessColumn::Threads));
}

TEST(ProcessColumnSettingsTest, KeepUnhideableColumnsVisibleShowsPidAndNameWhateverWasAsked)
{
    // #1209: a config may say "pid = false", and TableSetColumnEnabled(false) ignores the column's
    // NoHide flag, so loaded and requested visibility is normalized first.
    ProcessColumnSettings settings;
    settings.setVisible(ProcessColumn::PID, false);
    settings.setVisible(ProcessColumn::Name, false);
    settings.setVisible(ProcessColumn::User, false);
    settings.keepUnhideableColumnsVisible();
    EXPECT_TRUE(settings.isVisible(ProcessColumn::PID));
    EXPECT_TRUE(settings.isVisible(ProcessColumn::Name));
    EXPECT_FALSE(settings.isVisible(ProcessColumn::User)); // A hideable column keeps its choice
    for (const ProcessColumn col : allProcessColumns())
    {
        if (!getColumnInfo(col).canHide)
        {
            EXPECT_TRUE(settings.isVisible(col)) << getColumnInfo(col).configKey;
        }
    }
}

TEST(ProcessColumnSettingsTest, RequestVisibleKeepsUnhideableColumnsShown)
{
    ProcessColumnSettings settings;
    settings.requestVisible(ProcessColumn::PID, false);
    settings.requestVisible(ProcessColumn::Name, false);
    EXPECT_TRUE(settings.isVisible(ProcessColumn::PID));
    EXPECT_TRUE(settings.isVisible(ProcessColumn::Name));

    settings.requestVisible(ProcessColumn::User, false);
    EXPECT_FALSE(settings.isVisible(ProcessColumn::User));
    settings.requestVisible(ProcessColumn::User, true);
    EXPECT_TRUE(settings.isVisible(ProcessColumn::User));
}

// ========== Header alignment (#1209) ==========

TEST(ProcessColumnConfigTest, NumericColumnsAreRightAligned)
{
    for (const ProcessColumn col : {
             ProcessColumn::PID,        ProcessColumn::PPID,       ProcessColumn::CpuPercent, ProcessColumn::MemPercent,
             ProcessColumn::Resident,   ProcessColumn::Virtual,    ProcessColumn::Shared,     ProcessColumn::PeakResident,
             ProcessColumn::Priority,   ProcessColumn::Affinity,   ProcessColumn::Threads,    ProcessColumn::Handles,
             ProcessColumn::GdiObjects, ProcessColumn::CpuTime,    ProcessColumn::StartTime,  ProcessColumn::IoRead,
             ProcessColumn::IoWrite,    ProcessColumn::PageFaults, ProcessColumn::NetSent,    ProcessColumn::NetReceived,
             ProcessColumn::Power,      ProcessColumn::GpuPercent, ProcessColumn::GpuMemory,  ProcessColumn::NpuPercent,
             ProcessColumn::NpuMemory,  ProcessColumn::Private,    ProcessColumn::Swap,
         })
    {
        EXPECT_EQ(columnAlignment(col), ColumnAlign::Right) << getColumnInfo(col).configKey;
    }
}

// #1880: the Services column is off by default (keeping it current reads the service list), and can be
// shown and saved like any other.
TEST(ProcessColumnConfigTest, TheServicesColumnIsHiddenByDefault)
{
    const ProcessColumnInfo info = getColumnInfo(ProcessColumn::Services);
    EXPECT_EQ(info.name, "Services");
    EXPECT_EQ(info.configKey, "services");
    EXPECT_FALSE(info.defaultVisible);
    EXPECT_TRUE(info.canHide);
    EXPECT_FALSE(ProcessColumnSettings{}.isVisible(ProcessColumn::Services));
}

TEST(ProcessColumnConfigTest, TextColumnsAreLeftAlignedAndStateIsCentred)
{
    for (const ProcessColumn col : {ProcessColumn::Name,
                                    ProcessColumn::User,
                                    ProcessColumn::Publisher,
                                    ProcessColumn::Services,
                                    ProcessColumn::Status,
                                    ProcessColumn::Type,
                                    ProcessColumn::Architecture,
                                    ProcessColumn::GpuEngine,
                                    ProcessColumn::GpuDevice,
                                    ProcessColumn::NpuEngine,
                                    ProcessColumn::Command})
    {
        EXPECT_EQ(columnAlignment(col), ColumnAlign::Left) << getColumnInfo(col).configKey;
    }
    EXPECT_EQ(columnAlignment(ProcessColumn::State), ColumnAlign::Center);
}

} // namespace
} // namespace App
