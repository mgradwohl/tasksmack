#include "App/Panels/ProcessColumnAvailability.h"
#include "App/Panels/ProcessRowFormat.h"
#include "App/ProcessColumnConfig.h"
#include "Platform/ProcessTypes.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <optional>

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

TEST(ProcessColumnAvailabilityTest, WithdrawnCapabilityHidesOnlyUnchosenColumns)
{
    // #1210: Windows can withdraw its per-process network counters after the first EStats sample.
    Platform::ProcessCapabilities before = windowsLikeCapabilities();
    ProcessColumnSettings settings = ProcessColumnAvailability::defaultColumns(before);
    ASSERT_TRUE(settings.isVisible(ProcessColumn::NetSent));
    ASSERT_TRUE(settings.isVisible(ProcessColumn::NetReceived));
    settings.setVisible(ProcessColumn::NetReceived, true); // The user chose this one

    Platform::ProcessCapabilities after = before;
    after.hasNetworkCounters = false;
    const auto changed = ProcessColumnAvailability::capabilityDefaultChanges(settings, after);
    ASSERT_TRUE(changed.has_value());
    EXPECT_FALSE(changed.value().isVisible(ProcessColumn::NetSent));    // Not chosen: follows the system
    EXPECT_TRUE(changed.value().isVisible(ProcessColumn::NetReceived)); // Chosen: left alone
    EXPECT_FALSE(changed.value().isChosen(ProcessColumn::NetSent));     // Still the system's default, not a choice
    for (const ProcessColumn col : allProcessColumns())
    {
        if (col != ProcessColumn::NetSent)
        {
            EXPECT_EQ(changed.value().isVisible(col), settings.isVisible(col)) << getColumnInfo(col).configKey;
        }
    }
}

TEST(ProcessColumnAvailabilityTest, RegainedCapabilityRestoresAnUnchosenColumnsDefault)
{
    Platform::ProcessCapabilities without = linuxWithoutRaplCapabilities();
    ProcessColumnSettings settings = ProcessColumnAvailability::defaultColumns(without);
    ASSERT_FALSE(settings.isVisible(ProcessColumn::Power));

    Platform::ProcessCapabilities with = without;
    with.hasPowerUsage = true;
    const auto changed = ProcessColumnAvailability::capabilityDefaultChanges(settings, with);
    ASSERT_TRUE(changed.has_value());
    EXPECT_TRUE(changed.value().isVisible(ProcessColumn::Power));
}

TEST(ProcessColumnAvailabilityTest, CapabilityChangeThatMovesNoColumnQueuesNothing)
{
    const Platform::ProcessCapabilities caps = windowsLikeCapabilities();
    const ProcessColumnSettings settings = ProcessColumnAvailability::defaultColumns(caps);

    // Same capabilities, or a change no column depends on (reduced privileges).
    EXPECT_FALSE(ProcessColumnAvailability::capabilityDefaultChanges(settings, caps).has_value());
    Platform::ProcessCapabilities reduced = caps;
    reduced.hasReducedPrivileges = true;
    EXPECT_FALSE(ProcessColumnAvailability::capabilityDefaultChanges(settings, reduced).has_value());

    // A withdrawn capability whose columns were all chosen moves nothing either.
    ProcessColumnSettings chosen = settings;
    chosen.setVisible(ProcessColumn::NetSent, true);
    chosen.setVisible(ProcessColumn::NetReceived, true);
    Platform::ProcessCapabilities noNetwork = caps;
    noNetwork.hasNetworkCounters = false;
    EXPECT_FALSE(ProcessColumnAvailability::capabilityDefaultChanges(chosen, noNetwork).has_value());
}

TEST(ProcessColumnAvailabilityTest, GpuColumnsNeedPerProcessGpuMetrics)
{
    // #1210: DRM- or ROCm-only Linux (and no usable GPU probe) has no per-process GPU metrics, and
    // every process read a measured-looking 0.
    using ProcessColumnAvailability::perProcessGpuSupported;
    EXPECT_TRUE(perProcessGpuSupported(/*hasGpuModel=*/true, /*perProcessKnownUnsupported=*/false));
    EXPECT_FALSE(perProcessGpuSupported(true, true));
    EXPECT_FALSE(perProcessGpuSupported(false, false)); // No GPU model at all

    const Platform::ProcessCapabilities caps = linuxWithoutRaplCapabilities();
    for (const ProcessColumn col :
         {ProcessColumn::GpuPercent, ProcessColumn::GpuMemory, ProcessColumn::GpuEngine, ProcessColumn::GpuDevice})
    {
        EXPECT_TRUE(isSupported(col, caps, /*perProcessGpu=*/true)) << getColumnInfo(col).configKey;
        EXPECT_FALSE(isSupported(col, caps, /*perProcessGpu=*/false)) << getColumnInfo(col).configKey;
        EXPECT_EQ(unavailableValuesNote(col, caps, false), ProcessColumnAvailability::UNSUPPORTED_COLUMN_NOTE);
    }
    EXPECT_FALSE(rowFormatOptions(caps, false).hasPerProcessGpu);
    EXPECT_TRUE(rowFormatOptions(caps).hasPerProcessGpu);
}

TEST(ProcessColumnAvailabilityTest, GpuColumnsShownByTheUserAreHiddenOnlyIfUnchosen)
{
    const Platform::ProcessCapabilities caps = windowsLikeCapabilities();
    ProcessColumnSettings settings = ProcessColumnAvailability::defaultColumns(caps, /*perProcessGpu=*/true);
    settings.setDefaultVisible(ProcessColumn::GpuPercent, true); // An unchosen column that is shown
    settings.setVisible(ProcessColumn::GpuMemory, true);         // The user's choice

    const auto changed = ProcessColumnAvailability::capabilityDefaultChanges(settings, caps, /*perProcessGpu=*/false);
    ASSERT_TRUE(changed.has_value());
    EXPECT_FALSE(changed.value().isVisible(ProcessColumn::GpuPercent));
    EXPECT_TRUE(changed.value().isVisible(ProcessColumn::GpuMemory));
    EXPECT_FALSE(ProcessColumnAvailability::defaultColumns(caps, false).isVisible(ProcessColumn::GpuPercent));
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
