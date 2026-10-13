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

/// What the Windows probe reports (WindowsProcessProbe::capabilities()) while its network counters
/// work. Tests that need them withdrawn (#1254, #1358) clear hasNetworkCounters themselves.
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

TEST(ProcessColumnAvailabilityTest, ContainerIsLinuxOnly)
{
    // #805: containers are read from cgroups, which Windows has none of.
    EXPECT_FALSE(isSupported(ProcessColumn::Container, windowsLikeCapabilities()));
    EXPECT_EQ(unavailableValuesNote(ProcessColumn::Container, windowsLikeCapabilities()),
              ProcessColumnAvailability::UNSUPPORTED_COLUMN_NOTE);
    EXPECT_FALSE(rowFormatOptions(windowsLikeCapabilities()).hasContainer);
}

TEST(ProcessColumnAvailabilityTest, ServicesFollowTheCapabilityAndStayHiddenByDefault)
{
    // #1880: Linux's probe fills it from cgroups; on Windows, ProcessModel sets it once a service list
    // with PIDs is attached. Supported or not, it is never shown by default.
    Platform::ProcessCapabilities caps = windowsLikeCapabilities();
    EXPECT_FALSE(isSupported(ProcessColumn::Services, caps));
    EXPECT_EQ(unavailableValuesNote(ProcessColumn::Services, caps), ProcessColumnAvailability::UNSUPPORTED_COLUMN_NOTE);
    EXPECT_FALSE(rowFormatOptions(caps).hasServices);
    caps.hasServices = true;
    EXPECT_TRUE(isSupported(ProcessColumn::Services, caps));
    EXPECT_TRUE(unavailableValuesNote(ProcessColumn::Services, caps).empty());
    EXPECT_TRUE(rowFormatOptions(caps).hasServices);
    EXPECT_FALSE(ProcessColumnAvailability::defaultVisible(ProcessColumn::Services, caps));
    EXPECT_FALSE(ProcessColumnAvailability::defaultColumns(caps).isVisible(ProcessColumn::Services));
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
    caps.hasContainer = true;
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

TEST(ProcessColumnAvailabilityTest, ResetStaysAvailableWhileAColumnIsChosen)
{
    // #1210: Power shown then hidden again matches the default visibility, but stays chosen, so a
    // later gain of power readings would not show it. Reset must stay available to clear that.
    const Platform::ProcessCapabilities caps = linuxWithoutRaplCapabilities();
    ProcessColumnSettings settings = ProcessColumnAvailability::defaultColumns(caps);
    settings.requestVisible(ProcessColumn::Power, true);
    settings.requestVisible(ProcessColumn::Power, false);
    EXPECT_EQ(settings.visible, ProcessColumnAvailability::defaultColumns(caps).visible);
    EXPECT_FALSE(ProcessColumnAvailability::hasDefaultColumns(settings, caps));

    // Reset clears the choice.
    settings = ProcessColumnAvailability::defaultColumns(caps);
    EXPECT_TRUE(ProcessColumnAvailability::hasDefaultColumns(settings, caps));
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
    EXPECT_FALSE(changed.value_or(ProcessColumnSettings{}).isVisible(ProcessColumn::NetSent));    // Not chosen: follows the system
    EXPECT_TRUE(changed.value_or(ProcessColumnSettings{}).isVisible(ProcessColumn::NetReceived)); // Chosen: left alone
    EXPECT_FALSE(changed.value_or(ProcessColumnSettings{}).isChosen(ProcessColumn::NetSent)); // Still the system's default, not a choice
    for (const ProcessColumn col : allProcessColumns())
    {
        if (col != ProcessColumn::NetSent)
        {
            EXPECT_EQ(changed.value_or(ProcessColumnSettings{}).isVisible(col), settings.isVisible(col)) << getColumnInfo(col).configKey;
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
    EXPECT_TRUE(changed.value_or(ProcessColumnSettings{}).isVisible(ProcessColumn::Power));
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

/// No per-process GPU data at all: DRM- or ROCm-only Linux, or no usable GPU probe.
constexpr ProcessColumnAvailability::GpuSupport NO_PER_PROCESS_GPU{.perProcess = false, .utilization = false};
/// Per-process memory, devices and engines, but no utilization: NVML's running-process lists.
constexpr ProcessColumnAvailability::GpuSupport NO_PER_PROCESS_GPU_UTILIZATION{.perProcess = true, .utilization = false};

TEST(ProcessColumnAvailabilityTest, GpuSupportComesFromTheGpuModelsFlags)
{
    using ProcessColumnAvailability::gpuSupport;
    EXPECT_EQ(gpuSupport(/*hasGpuModel=*/true, /*perProcessKnownUnsupported=*/false, /*perProcessUtilizationKnownUnsupported=*/false),
              (ProcessColumnAvailability::GpuSupport{.perProcess = true, .utilization = true}));
    EXPECT_EQ(gpuSupport(true, false, true), NO_PER_PROCESS_GPU_UTILIZATION);
    EXPECT_EQ(gpuSupport(true, true, false), NO_PER_PROCESS_GPU);   // No metrics means no utilization either
    EXPECT_EQ(gpuSupport(false, false, false), NO_PER_PROCESS_GPU); // No GPU model at all
}

TEST(ProcessColumnAvailabilityTest, CellsFollowTheSupportTheirGenerationWasReadUnder)
{
    // #1210: the table formats a generation's GPU cells by the support published with it.
    using ProcessColumnAvailability::gpuSupportOfGeneration;
    EXPECT_EQ(gpuSupportOfGeneration(true, true), (ProcessColumnAvailability::GpuSupport{.perProcess = true, .utilization = true}));
    EXPECT_EQ(gpuSupportOfGeneration(true, false), NO_PER_PROCESS_GPU_UTILIZATION);
    EXPECT_EQ(gpuSupportOfGeneration(false, true), NO_PER_PROCESS_GPU); // No data means no utilization
    EXPECT_EQ(gpuSupportOfGeneration(false, false), NO_PER_PROCESS_GPU);

    // A failed read keeps the support and is passed on to the row formatter (#1210).
    const auto failed = gpuSupportOfGeneration(true, true, /*readFailed=*/true);
    EXPECT_TRUE(failed.perProcess);
    EXPECT_TRUE(failed.readFailed);
    EXPECT_TRUE(rowFormatOptions(linuxWithoutRaplCapabilities(), failed).gpuReadFailed);
    EXPECT_TRUE(rowFormatOptions(linuxWithoutRaplCapabilities(), failed).hasPerProcessGpu);
    EXPECT_FALSE(gpuSupportOfGeneration(false, false, true).readFailed); // No support: nothing to fail
}

TEST(ProcessColumnAvailabilityTest, GpuColumnsNeedPerProcessGpuMetrics)
{
    // #1210: DRM- or ROCm-only Linux (and no usable GPU probe) has no per-process GPU metrics, and
    // every process read a measured-looking 0.
    const Platform::ProcessCapabilities caps = linuxWithoutRaplCapabilities();
    for (const ProcessColumn col :
         {ProcessColumn::GpuPercent, ProcessColumn::GpuMemory, ProcessColumn::GpuEngine, ProcessColumn::GpuDevice})
    {
        EXPECT_TRUE(isSupported(col, caps)) << getColumnInfo(col).configKey;
        EXPECT_FALSE(isSupported(col, caps, NO_PER_PROCESS_GPU)) << getColumnInfo(col).configKey;
        EXPECT_EQ(unavailableValuesNote(col, caps, NO_PER_PROCESS_GPU), ProcessColumnAvailability::UNSUPPORTED_COLUMN_NOTE);
    }
    EXPECT_FALSE(rowFormatOptions(caps, NO_PER_PROCESS_GPU).hasPerProcessGpu);
    EXPECT_TRUE(rowFormatOptions(caps).hasPerProcessGpu);
}

TEST(ProcessColumnAvailabilityTest, GpuPercentNeedsPerProcessUtilization)
{
    // #1210: Linux NVML reports each process's GPU memory and engines but not its utilization, so
    // GPU % read a measured 0.0% for every process while the other GPU columns were real.
    const Platform::ProcessCapabilities caps = linuxWithoutRaplCapabilities();
    EXPECT_FALSE(isSupported(ProcessColumn::GpuPercent, caps, NO_PER_PROCESS_GPU_UTILIZATION));
    for (const ProcessColumn col : {ProcessColumn::GpuMemory, ProcessColumn::GpuEngine, ProcessColumn::GpuDevice})
    {
        EXPECT_TRUE(isSupported(col, caps, NO_PER_PROCESS_GPU_UTILIZATION)) << getColumnInfo(col).configKey;
    }
    const ProcessRowFormat::RowFormatOptions options = rowFormatOptions(caps, NO_PER_PROCESS_GPU_UTILIZATION);
    EXPECT_TRUE(options.hasPerProcessGpu);
    EXPECT_FALSE(options.hasPerProcessGpuUtilization);
    EXPECT_TRUE(rowFormatOptions(caps).hasPerProcessGpuUtilization);
}

// #1799: the NPU columns need an NPU or compute accelerator as well as per-process GPU metrics, and
// NPU % needs per-process utilization too. Without one -- every Linux system until #1827 -- they are
// "not available on this system", as the GPU columns are without per-process data.
TEST(ProcessColumnAvailabilityTest, NpuColumnsNeedAnAcceleratorAndPerProcessMetrics)
{
    using ProcessColumnAvailability::gpuSupport;
    const Platform::ProcessCapabilities caps = windowsLikeCapabilities();
    constexpr ProcessColumnAvailability::GpuSupport NO_ACCELERATOR{.perProcess = true, .utilization = true, .accelerator = false};
    for (const ProcessColumn col : {ProcessColumn::NpuPercent, ProcessColumn::NpuMemory, ProcessColumn::NpuEngine})
    {
        EXPECT_TRUE(isSupported(col, caps)) << getColumnInfo(col).configKey;
        EXPECT_FALSE(isSupported(col, caps, NO_ACCELERATOR)) << getColumnInfo(col).configKey;
        EXPECT_FALSE(isSupported(col, caps, NO_PER_PROCESS_GPU)) << getColumnInfo(col).configKey;
        EXPECT_EQ(unavailableValuesNote(col, caps, NO_ACCELERATOR), ProcessColumnAvailability::UNSUPPORTED_COLUMN_NOTE);
        EXPECT_FALSE(ProcessColumnAvailability::defaultColumns(caps).isVisible(col)) << "Off by default";
    }
    // The GPU columns don't depend on an accelerator.
    EXPECT_TRUE(isSupported(ProcessColumn::GpuPercent, caps, NO_ACCELERATOR));
    // NPU % needs utilization; NPU Memory and NPU Engine don't.
    EXPECT_FALSE(isSupported(ProcessColumn::NpuPercent, caps, NO_PER_PROCESS_GPU_UTILIZATION));
    EXPECT_TRUE(isSupported(ProcessColumn::NpuMemory, caps, NO_PER_PROCESS_GPU_UTILIZATION));
    EXPECT_TRUE(isSupported(ProcessColumn::NpuEngine, caps, NO_PER_PROCESS_GPU_UTILIZATION));

    // The GPU model's word on an accelerator reaches the support and the row formatter.
    EXPECT_FALSE(gpuSupport(true, false, false, /*hasAccelerator=*/false).accelerator);
    EXPECT_TRUE(gpuSupport(true, false, false, /*hasAccelerator=*/true).accelerator);
    EXPECT_FALSE(rowFormatOptions(caps, NO_ACCELERATOR).hasPerProcessNpu);
    EXPECT_TRUE(rowFormatOptions(caps, NO_ACCELERATOR).hasPerProcessGpu);
    EXPECT_FALSE(rowFormatOptions(caps, NO_PER_PROCESS_GPU).hasPerProcessNpu);
    EXPECT_TRUE(rowFormatOptions(caps).hasPerProcessNpu);
    EXPECT_FALSE(ProcessColumnAvailability::gpuSupportOfGeneration(true, true, false, /*accelerator=*/false).accelerator);
}

/// Per-process GPU data on a system whose GPU model has listed an NPU (#1870).
constexpr ProcessColumnAvailability::GpuSupport NPU_LISTED{
    .perProcess = true,
    .utilization = true,
    .accelerator = true,
    .acceleratorListed = true,
};

// #1870: NPU % is shown by default once an NPU or compute accelerator is known to be listed, and not
// while that is unknown or on a system without one. NPU Memory and NPU Engine stay off either way.
TEST(ProcessColumnAvailabilityTest, NpuPercentIsShownByDefaultOnlyWithAKnownAccelerator)
{
    using ProcessColumnAvailability::defaultColumns;
    using ProcessColumnAvailability::gpuSupport;
    const Platform::ProcessCapabilities caps = windowsLikeCapabilities();

    const ProcessColumnSettings withNpu = defaultColumns(caps, NPU_LISTED);
    EXPECT_TRUE(withNpu.isVisible(ProcessColumn::NpuPercent));
    EXPECT_FALSE(withNpu.isChosen(ProcessColumn::NpuPercent)); // A default, not a choice: not saved
    EXPECT_FALSE(withNpu.isVisible(ProcessColumn::NpuMemory));
    EXPECT_FALSE(withNpu.isVisible(ProcessColumn::NpuEngine));
    EXPECT_TRUE(ProcessColumnAvailability::hasDefaultColumns(withNpu, caps, NPU_LISTED));
    // Every other column keeps the default it has without an NPU.
    for (const ProcessColumn col : allProcessColumns())
    {
        if (col != ProcessColumn::NpuPercent)
        {
            EXPECT_EQ(withNpu.isVisible(col), defaultColumns(caps).isVisible(col)) << getColumnInfo(col).configKey;
        }
    }

    // Unknown yet (the GPU model has not published), no accelerator, or no per-process utilization.
    EXPECT_FALSE(defaultColumns(caps).isVisible(ProcessColumn::NpuPercent));
    EXPECT_FALSE(
        defaultColumns(caps, {.perProcess = true, .utilization = true, .accelerator = false}).isVisible(ProcessColumn::NpuPercent));
    EXPECT_FALSE(defaultColumns(caps, {.perProcess = true, .utilization = false, .accelerator = true, .acceleratorListed = true})
                     .isVisible(ProcessColumn::NpuPercent));

    // The GPU model's word: listed only once known, and never when it lists none.
    EXPECT_TRUE(gpuSupport(true, false, false, /*hasAccelerator=*/true, /*acceleratorListed=*/true).acceleratorListed);
    EXPECT_FALSE(gpuSupport(true, false, false, /*hasAccelerator=*/true).acceleratorListed);
    EXPECT_FALSE(gpuSupport(true, false, false, /*hasAccelerator=*/false, /*acceleratorListed=*/true).acceleratorListed);
}

// #1870: the accelerator is known only once the GPU probe has enumerated, so NPU % takes its default
// then -- unless its visibility was chosen meanwhile, or in the config file, which always wins.
TEST(ProcessColumnAvailabilityTest, AnAcceleratorBecomingKnownShowsNpuPercentOnlyIfUnchosen)
{
    const Platform::ProcessCapabilities caps = windowsLikeCapabilities();
    const ProcessColumnSettings startup = ProcessColumnAvailability::defaultColumns(caps);
    ASSERT_FALSE(startup.isVisible(ProcessColumn::NpuPercent));

    const auto changed = ProcessColumnAvailability::capabilityDefaultChanges(startup, caps, NPU_LISTED);
    ASSERT_TRUE(changed.has_value());
    EXPECT_TRUE(changed.value_or(ProcessColumnSettings{}).isVisible(ProcessColumn::NpuPercent));
    EXPECT_FALSE(changed.value_or(ProcessColumnSettings{}).isChosen(ProcessColumn::NpuPercent));

    // Turned off by the user (or "npu_percent = false" in the file): stays off.
    ProcessColumnSettings off = startup;
    off.requestVisible(ProcessColumn::NpuPercent, false);
    EXPECT_FALSE(ProcessColumnAvailability::capabilityDefaultChanges(off, caps, NPU_LISTED).has_value());
    ProcessColumnAvailability::applyCapabilityDefaults(off, caps, NPU_LISTED);
    EXPECT_FALSE(off.isVisible(ProcessColumn::NpuPercent));
    EXPECT_FALSE(ProcessColumnAvailability::hasDefaultColumns(off, caps, NPU_LISTED)); // Reset would show it

    // Turned on with no accelerator: stays on, its cells the unavailable dash.
    ProcessColumnSettings on = startup;
    on.setVisible(ProcessColumn::NpuPercent, true);
    constexpr ProcessColumnAvailability::GpuSupport NO_ACCELERATOR{.perProcess = true, .utilization = true, .accelerator = false};
    ProcessColumnAvailability::applyCapabilityDefaults(on, caps, NO_ACCELERATOR);
    EXPECT_TRUE(on.isVisible(ProcessColumn::NpuPercent));
    EXPECT_FALSE(rowFormatOptions(caps, NO_ACCELERATOR).hasPerProcessNpu);
}

TEST(ProcessColumnAvailabilityTest, GpuColumnsShownByTheUserAreHiddenOnlyIfUnchosen)
{
    const Platform::ProcessCapabilities caps = windowsLikeCapabilities();
    ProcessColumnSettings settings = ProcessColumnAvailability::defaultColumns(caps);
    settings.setDefaultVisible(ProcessColumn::GpuPercent, true); // An unchosen column that is shown
    settings.setVisible(ProcessColumn::GpuMemory, true);         // The user's choice

    const auto changed = ProcessColumnAvailability::capabilityDefaultChanges(settings, caps, NO_PER_PROCESS_GPU);
    ASSERT_TRUE(changed.has_value());
    EXPECT_FALSE(changed.value_or(ProcessColumnSettings{}).isVisible(ProcessColumn::GpuPercent));
    EXPECT_TRUE(changed.value_or(ProcessColumnSettings{}).isVisible(ProcessColumn::GpuMemory));
    EXPECT_FALSE(ProcessColumnAvailability::defaultColumns(caps, NO_PER_PROCESS_GPU).isVisible(ProcessColumn::GpuPercent));

    // Losing only utilization hides only GPU %, unless chosen.
    const auto utilOnly = ProcessColumnAvailability::capabilityDefaultChanges(settings, caps, NO_PER_PROCESS_GPU_UTILIZATION);
    ASSERT_TRUE(utilOnly.has_value());
    EXPECT_FALSE(utilOnly.value_or(ProcessColumnSettings{}).isVisible(ProcessColumn::GpuPercent));
    EXPECT_TRUE(utilOnly.value_or(ProcessColumnSettings{}).isVisible(ProcessColumn::GpuMemory));
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
    EXPECT_TRUE(options.hasStatus);
    EXPECT_TRUE(options.hasPublisher);
    EXPECT_TRUE(options.hasProcessType);
    EXPECT_FALSE(rowFormatOptions(linuxWithoutRaplCapabilities()).hasPublisher);
    EXPECT_TRUE(rowFormatOptions(linuxWithoutRaplCapabilities()).hasContainer);

    // And every column the options gate agrees with isSupported().
    const Platform::ProcessCapabilities none{};
    const ProcessRowFormat::RowFormatOptions noneOptions = rowFormatOptions(none);
    EXPECT_EQ(noneOptions.hasIoCounters, isSupported(ProcessColumn::IoRead, none));
    EXPECT_EQ(noneOptions.hasGdiObjects, isSupported(ProcessColumn::GdiObjects, none));
    EXPECT_EQ(noneOptions.hasSharedMemory, isSupported(ProcessColumn::Shared, none));
}

} // namespace
} // namespace App
