#include "App/Panels/ProcessColumnAvailability.h"
#include "App/Panels/ProcessRowFormat.h"
#include "App/ProcessColumnConfig.h"
#include "Platform/ProcessTypes.h"

#include <gtest/gtest.h>

#include <initializer_list>

namespace App
{
namespace
{

using ProcessColumnAvailability::isSupported;
using ProcessColumnAvailability::rowFormatOptions;
using ProcessColumnAvailability::unavailableValuesNote;

/// What the Windows probe reports (WindowsProcessProbe::capabilities()), without network counters.
[[nodiscard]] Platform::ProcessCapabilities windowsLikeCapabilities()
{
    Platform::ProcessCapabilities caps;
    caps.hasIoCounters = true;
    caps.hasThreadCount = true;
    caps.hasHandleCount = true;
    caps.hasPageFaults = true;
    caps.hasCpuAffinity = true;
    caps.hasNetworkCounters = true;
    caps.hasPowerUsage = false;
    caps.hasStatus = true;
    caps.hasPublisher = true;
    caps.hasProcessType = true;
    caps.hasGdiObjects = true;
    caps.hasSharedMemory = false;
    return caps;
}

TEST(ProcessColumnAvailabilityTest, SharedMemoryIsUnsupportedOnWindows)
{
    // #1210: Windows has no shared memory figure, so its column is all dashes and its header says why.
    const Platform::ProcessCapabilities caps = windowsLikeCapabilities();
    EXPECT_FALSE(isSupported(ProcessColumn::Shared, caps));
    EXPECT_EQ(unavailableValuesNote(ProcessColumn::Shared, caps), ProcessColumnAvailability::UNSUPPORTED_COLUMN_NOTE);
    EXPECT_FALSE(isSupported(ProcessColumn::Power, caps));
}

TEST(ProcessColumnAvailabilityTest, StatusStaysSupportedWherePresent)
{
    // Status is sparse on Windows (Suspended, Efficiency Mode), not unsupported.
    EXPECT_TRUE(isSupported(ProcessColumn::Status, windowsLikeCapabilities()));
    EXPECT_TRUE(unavailableValuesNote(ProcessColumn::Status, windowsLikeCapabilities()).empty());
}

TEST(ProcessColumnAvailabilityTest, ColumnsEveryProbeFillsAreAlwaysSupported)
{
    const Platform::ProcessCapabilities none{};
    for (const ProcessColumn col : {ProcessColumn::PID,
                                    ProcessColumn::Name,
                                    ProcessColumn::CpuPercent,
                                    ProcessColumn::MemPercent,
                                    ProcessColumn::Resident,
                                    ProcessColumn::Virtual,
                                    ProcessColumn::PeakResident, // Tracked by the model where the OS has no peak
                                    ProcessColumn::Command})
    {
        EXPECT_TRUE(isSupported(col, none)) << getColumnInfo(col).configKey;
    }
}

TEST(ProcessColumnAvailabilityTest, ColumnsWithPerProcessGapsExplainTheDash)
{
    const Platform::ProcessCapabilities caps = windowsLikeCapabilities();
    for (const ProcessColumn col : {ProcessColumn::IoRead,
                                    ProcessColumn::IoWrite,
                                    ProcessColumn::NetSent,
                                    ProcessColumn::NetReceived,
                                    ProcessColumn::Handles,
                                    ProcessColumn::GdiObjects,
                                    ProcessColumn::Threads})
    {
        EXPECT_EQ(unavailableValuesNote(col, caps), ProcessColumnAvailability::UNREADABLE_VALUES_NOTE) << getColumnInfo(col).configKey;
    }
    EXPECT_TRUE(unavailableValuesNote(ProcessColumn::CpuPercent, caps).empty());
}

TEST(ProcessColumnAvailabilityTest, UnsupportedNoteWinsOverThePerProcessNote)
{
    Platform::ProcessCapabilities caps = windowsLikeCapabilities();
    caps.hasNetworkCounters = false;
    EXPECT_EQ(unavailableValuesNote(ProcessColumn::NetSent, caps), ProcessColumnAvailability::UNSUPPORTED_COLUMN_NOTE);
}

/// What the Linux probe reports on a machine without RAPL (LinuxProcessProbe::capabilities()).
[[nodiscard]] Platform::ProcessCapabilities linuxWithoutRaplCapabilities()
{
    Platform::ProcessCapabilities caps;
    caps.hasIoCounters = true;
    caps.hasThreadCount = true;
    caps.hasHandleCount = true;
    caps.hasPageFaults = true;
    caps.hasCpuAffinity = true;
    caps.hasNetworkCounters = true;
    caps.hasPowerUsage = false;
    caps.hasStatus = true;
    caps.hasSharedMemory = true;
    return caps;
}

TEST(ProcessColumnAvailabilityTest, NoDefaultColumnIsEntirelyUnavailable)
{
    // #1210: Power is shown by default, and on Linux without RAPL it would be a column of dashes.
    ASSERT_TRUE(getColumnInfo(ProcessColumn::Power).defaultVisible);
    for (const Platform::ProcessCapabilities& caps : {linuxWithoutRaplCapabilities(), windowsLikeCapabilities()})
    {
        const ProcessColumnSettings defaults = ProcessColumnAvailability::defaultColumns(caps);
        for (const ProcessColumn col : allProcessColumns())
        {
            if (defaults.isVisible(col))
            {
                EXPECT_TRUE(isSupported(col, caps)) << getColumnInfo(col).configKey;
            }
        }
    }
    EXPECT_FALSE(ProcessColumnAvailability::defaultColumns(linuxWithoutRaplCapabilities()).isVisible(ProcessColumn::Power));
}

TEST(ProcessColumnAvailabilityTest, SupportedColumnsKeepTheirDefaults)
{
    Platform::ProcessCapabilities everything = linuxWithoutRaplCapabilities();
    everything.hasPowerUsage = true;
    everything.hasPublisher = true;
    everything.hasProcessType = true;
    everything.hasGdiObjects = true;
    EXPECT_EQ(ProcessColumnAvailability::defaultColumns(everything).visible, ProcessColumnSettings::defaults().visible);
    EXPECT_TRUE(ProcessColumnAvailability::hasDefaultColumns(ProcessColumnSettings{}, everything));
}

TEST(ProcessColumnAvailabilityTest, CapabilityDefaultsLeaveAChosenColumnAlone)
{
    const Platform::ProcessCapabilities caps = linuxWithoutRaplCapabilities();

    // Not chosen (no saved value): hidden, since this system cannot fill it.
    ProcessColumnSettings fresh;
    ProcessColumnAvailability::applyCapabilityDefaults(fresh, caps);
    EXPECT_FALSE(fresh.isVisible(ProcessColumn::Power));
    EXPECT_TRUE(ProcessColumnAvailability::hasDefaultColumns(fresh, caps));

    // Chosen (the user turned it on, or the config file says so): kept.
    ProcessColumnSettings saved;
    saved.setVisible(ProcessColumn::Power, true);
    ProcessColumnAvailability::applyCapabilityDefaults(saved, caps);
    EXPECT_TRUE(saved.isVisible(ProcessColumn::Power));
    EXPECT_FALSE(ProcessColumnAvailability::hasDefaultColumns(saved, caps)); // "Reset columns" would hide it

    // The Columns menu can still show it.
    fresh.requestVisible(ProcessColumn::Power, true);
    EXPECT_TRUE(fresh.isVisible(ProcessColumn::Power));
}

TEST(ProcessColumnAvailabilityTest, EmptyTextInASupportedColumnIsBlankNotUnavailable)
{
    using ProcessColumnAvailability::TextCell;
    using ProcessColumnAvailability::textCell;
    // Windows' publisher lookup returns empty both for an executable without a CompanyName and for
    // one it could not read, so an empty publisher is not claimed to be unavailable.
    EXPECT_EQ(textCell(/*columnSupported=*/true, /*hasValue=*/false), TextCell::Blank);
    EXPECT_EQ(textCell(true, true), TextCell::Text);
    // Only a column the system cannot fill at all shows the dash, whatever the value.
    EXPECT_EQ(textCell(false, false), TextCell::Unavailable);
    EXPECT_EQ(textCell(false, true), TextCell::Unavailable);
}

TEST(ProcessColumnAvailabilityTest, RowFormatOptionsMirrorTheCapabilities)
{
    const Platform::ProcessCapabilities caps = windowsLikeCapabilities();
    const ProcessRowFormat::RowFormatOptions options = rowFormatOptions(caps);
    EXPECT_FALSE(options.hasPowerUsage);
    EXPECT_FALSE(options.hasSharedMemory);
    EXPECT_TRUE(options.hasIoCounters);
    EXPECT_TRUE(options.hasNetworkCounters);
    EXPECT_TRUE(options.hasThreadCount);
    EXPECT_TRUE(options.hasHandleCount);
    EXPECT_TRUE(options.hasPageFaults);
    EXPECT_TRUE(options.hasCpuAffinity);
    EXPECT_TRUE(options.hasGdiObjects);

    // And every column the options gate agrees with isSupported().
    const Platform::ProcessCapabilities none{};
    const ProcessRowFormat::RowFormatOptions noneOptions = rowFormatOptions(none);
    EXPECT_EQ(noneOptions.hasIoCounters, isSupported(ProcessColumn::IoRead, none));
    EXPECT_EQ(noneOptions.hasGdiObjects, isSupported(ProcessColumn::GdiObjects, none));
    EXPECT_EQ(noneOptions.hasSharedMemory, isSupported(ProcessColumn::Shared, none));
}

} // namespace
} // namespace App
