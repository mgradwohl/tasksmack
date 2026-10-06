#include "App/DialogGeometry.h"
#include "App/ProcessColumnConfig.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
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
        "pid",         "name",        "user",        "ppid",        "publisher",  "state",         "status",     "type",
        "cpu_percent", "mem_percent", "resident",    "virtual",     "shared",     "peak_resident", "nice",       "affinity",
        "threads",     "handles",     "gdi_objects", "cpu_time",    "start_time", "io_read",       "io_write",   "page_faults",
        "net_sent",    "net_recv",    "power",       "gpu_percent", "gpu_memory", "gpu_engine",    "gpu_device", "command",
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
        EXPECT_TRUE(columnCapabilityNote(col, true).empty()) << getColumnInfo(col).configKey;
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
    EXPECT_EQ(toIndex(ProcessColumn::Publisher), 4);   // Windows publisher — after User(2) and PPID(3)
    EXPECT_EQ(toIndex(ProcessColumn::Type), 7);        // Process type — after State(5) and Status(6)
    EXPECT_EQ(toIndex(ProcessColumn::GdiObjects), 18); // GDI count — after Handles(17)
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
        EXPECT_EQ(std::find(seen.begin(), seen.end(), col), seen.end()) << "Duplicate column detected";
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
    EXPECT_TRUE(settings.isDefault());

    settings.requestVisible(ProcessColumn::Command, false);
    settings.requestVisible(ProcessColumn::Threads, true);
    EXPECT_FALSE(settings.isDefault());

    settings = ProcessColumnSettings::defaults();
    EXPECT_TRUE(settings.isDefault());
    EXPECT_EQ(settings, ProcessColumnSettings{});
    for (const ProcessColumn col : allProcessColumns())
    {
        EXPECT_EQ(settings.isVisible(col), getColumnInfo(col).defaultVisible);
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
    for (const ProcessColumn col :
         {ProcessColumn::PID,        ProcessColumn::PPID,       ProcessColumn::CpuPercent, ProcessColumn::MemPercent,
          ProcessColumn::Resident,   ProcessColumn::Virtual,    ProcessColumn::Shared,     ProcessColumn::PeakResident,
          ProcessColumn::Priority,   ProcessColumn::Affinity,   ProcessColumn::Threads,    ProcessColumn::Handles,
          ProcessColumn::GdiObjects, ProcessColumn::CpuTime,    ProcessColumn::StartTime,  ProcessColumn::IoRead,
          ProcessColumn::IoWrite,    ProcessColumn::PageFaults, ProcessColumn::NetSent,    ProcessColumn::NetReceived,
          ProcessColumn::Power,      ProcessColumn::GpuPercent, ProcessColumn::GpuMemory})
    {
        EXPECT_EQ(columnAlignment(col), ColumnAlign::Right) << getColumnInfo(col).configKey;
    }
}

TEST(ProcessColumnConfigTest, TextColumnsAreLeftAlignedAndStateIsCentred)
{
    for (const ProcessColumn col : {ProcessColumn::Name,
                                    ProcessColumn::User,
                                    ProcessColumn::Publisher,
                                    ProcessColumn::Status,
                                    ProcessColumn::Type,
                                    ProcessColumn::GpuEngine,
                                    ProcessColumn::GpuDevice,
                                    ProcessColumn::Command})
    {
        EXPECT_EQ(columnAlignment(col), ColumnAlign::Left) << getColumnInfo(col).configKey;
    }
    EXPECT_EQ(columnAlignment(ProcessColumn::State), ColumnAlign::Center);
}

} // namespace
} // namespace App
