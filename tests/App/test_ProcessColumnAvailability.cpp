#include "App/Panels/ProcessColumnAvailability.h"
#include "App/Panels/ProcessRowFormat.h"
#include "App/ProcessColumnConfig.h"
#include "Domain/ProcessModel.h"
#include "Platform/ProcessTypes.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <optional>
#include <string_view>

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
    caps.hasArchitecture = true; // #2394
    caps.hasGdiObjects = true;
    caps.hasSharedMemory = true;  // The shareable working set (#2583)
    caps.hasPrivateBytes = false; // Private bytes are the commit size, Virtual (#1879)
    caps.hasSwapBytes = false;    // No per-process swap figure (#1879)
    return caps;
}

TEST(ProcessColumnAvailabilityTest, SharedMemoryIsSupportedOnWindowsAndStaysHiddenByDefault)
{
    // #2583: Windows fills Shared from the shareable working set; its column was all dashes before
    // (#1035, #1210). It stays off by default, as on Linux. Power is still unsupported (#1028).
    const Platform::ProcessCapabilities caps = windowsLikeCapabilities();
    EXPECT_TRUE(isSupported(ProcessColumn::Shared, caps));
    EXPECT_TRUE(unavailableValuesNote(ProcessColumn::Shared, caps).empty());
    EXPECT_TRUE(rowFormatOptions(caps).hasSharedMemory);
    EXPECT_FALSE(getColumnInfo(ProcessColumn::Shared).defaultVisible);
    EXPECT_FALSE(ProcessColumnAvailability::defaultColumns(caps).isVisible(ProcessColumn::Shared));
    EXPECT_FALSE(isSupported(ProcessColumn::Power, caps));

    // Without the capability the column is all dashes and its header says why.
    Platform::ProcessCapabilities without = caps;
    without.hasSharedMemory = false;
    EXPECT_FALSE(isSupported(ProcessColumn::Shared, without));
    EXPECT_EQ(unavailableValuesNote(ProcessColumn::Shared, without), ProcessColumnAvailability::UNSUPPORTED_COLUMN_NOTE);
}

TEST(ProcessColumnAvailabilityTest, WindowsHasNeitherPrivateNorSwap)
{
    // #1879: Windows' private bytes are the commit size, already the Virtual column, and it has no
    // per-process swap figure. Both columns are all dashes, and each header says why rather than the
    // generic note.
    const Platform::ProcessCapabilities caps = windowsLikeCapabilities();
    EXPECT_FALSE(isSupported(ProcessColumn::Private, caps));
    EXPECT_FALSE(isSupported(ProcessColumn::Swap, caps));
    const std::string_view privateNote = unavailableValuesNote(ProcessColumn::Private, caps);
    const std::string_view swapNote = unavailableValuesNote(ProcessColumn::Swap, caps);
    EXPECT_EQ(privateNote, ProcessColumnAvailability::PRIVATE_UNSUPPORTED_COLUMN_NOTE);
    EXPECT_EQ(swapNote, ProcessColumnAvailability::SWAP_UNSUPPORTED_COLUMN_NOTE);
    EXPECT_TRUE(privateNote.contains("Virtual")) << privateNote;
    EXPECT_TRUE(swapNote.contains("pagefile")) << swapNote;
    EXPECT_FALSE(rowFormatOptions(caps).hasPrivateBytes);
    EXPECT_FALSE(rowFormatOptions(caps).hasSwapBytes);
}

TEST(ProcessColumnAvailabilityTest, PrivateAndSwapFollowTheirCapabilities)
{
    // #1885: Linux reads RssAnon and VmSwap; a probe that doesn't gets dashes and the header's note, and
    // both columns stay hidden by default. Each note says why Windows lacks it (#1879).
    Platform::ProcessCapabilities caps;
    EXPECT_FALSE(isSupported(ProcessColumn::Private, caps));
    EXPECT_FALSE(isSupported(ProcessColumn::Swap, caps));
    EXPECT_EQ(unavailableValuesNote(ProcessColumn::Private, caps), ProcessColumnAvailability::PRIVATE_UNSUPPORTED_COLUMN_NOTE);
    EXPECT_EQ(unavailableValuesNote(ProcessColumn::Swap, caps), ProcessColumnAvailability::SWAP_UNSUPPORTED_COLUMN_NOTE);
    caps.hasPrivateBytes = true;
    caps.hasSwapBytes = true;
    EXPECT_TRUE(isSupported(ProcessColumn::Private, caps));
    EXPECT_TRUE(isSupported(ProcessColumn::Swap, caps));
    EXPECT_TRUE(rowFormatOptions(caps).hasPrivateBytes);
    EXPECT_TRUE(rowFormatOptions(caps).hasSwapBytes);
    EXPECT_FALSE(getColumnInfo(ProcessColumn::Private).defaultVisible);
    EXPECT_FALSE(getColumnInfo(ProcessColumn::Swap).defaultVisible);
}

TEST(ProcessColumnAvailabilityTest, ArchitectureFollowsItsCapabilityAndIsHiddenByDefault)
{
    // #2394: both probes read it; one that doesn't shows dashes and the header's note.
    Platform::ProcessCapabilities caps;
    EXPECT_FALSE(isSupported(ProcessColumn::Architecture, caps));
    EXPECT_EQ(unavailableValuesNote(ProcessColumn::Architecture, caps), ProcessColumnAvailability::UNSUPPORTED_COLUMN_NOTE);
    EXPECT_FALSE(rowFormatOptions(caps).hasArchitecture);
    caps.hasArchitecture = true;
    EXPECT_TRUE(isSupported(ProcessColumn::Architecture, caps));
    EXPECT_TRUE(rowFormatOptions(caps).hasArchitecture);
    EXPECT_FALSE(ProcessColumnAvailability::defaultVisible(ProcessColumn::Architecture, caps));
    EXPECT_EQ(getColumnInfo(ProcessColumn::Architecture).name, "Architecture");
    EXPECT_EQ(getColumnInfo(ProcessColumn::Architecture).configKey, "architecture");
    EXPECT_EQ(columnAlignment(ProcessColumn::Architecture), ColumnAlign::Left);
}

TEST(ProcessColumnAvailabilityTest, PolicyFollowsItsCapabilityAndIsHiddenByDefault)
{
    // #3086: Linux reads it from /proc/[pid]/stat; Windows shows dashes and the header's note.
    Platform::ProcessCapabilities caps;
    EXPECT_FALSE(isSupported(ProcessColumn::Policy, caps));
    EXPECT_FALSE(rowFormatOptions(caps).hasSchedPolicy);
    caps.hasSchedPolicy = true;
    EXPECT_TRUE(isSupported(ProcessColumn::Policy, caps));
    EXPECT_TRUE(rowFormatOptions(caps).hasSchedPolicy);
    EXPECT_FALSE(ProcessColumnAvailability::defaultVisible(ProcessColumn::Policy, caps));
    EXPECT_EQ(getColumnInfo(ProcessColumn::Policy).configKey, "policy");
    EXPECT_EQ(columnAlignment(ProcessColumn::Policy), ColumnAlign::Left);
}

TEST(ProcessColumnAvailabilityTest, ServicesFollowTheirCapability)
{
    // #1886: Linux reads the systemd service; Windows (until #1880) shows dashes and the header's note.
    EXPECT_FALSE(isSupported(ProcessColumn::Services, windowsLikeCapabilities()));
    EXPECT_EQ(unavailableValuesNote(ProcessColumn::Services, windowsLikeCapabilities()),
              ProcessColumnAvailability::UNSUPPORTED_COLUMN_NOTE);
    Platform::ProcessCapabilities caps = windowsLikeCapabilities();
    caps.hasServices = true;
    EXPECT_TRUE(isSupported(ProcessColumn::Services, caps));
    EXPECT_TRUE(rowFormatOptions(caps).hasServices);
    EXPECT_FALSE(getColumnInfo(ProcessColumn::Services).defaultVisible);
    EXPECT_EQ(columnAlignment(ProcessColumn::Services), ColumnAlign::Left);
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
    caps.hasArchitecture = true; // #2394
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

/// No per-process GPU or NPU data at all: DRM- or ROCm-only Linux, or no usable GPU probe.
constexpr ProcessColumnAvailability::GpuSupport NO_PER_PROCESS_GPU{
    .perProcess = false, .utilization = false, .npuPerProcess = false, .npuUtilization = false};
/// Per-process memory, devices and engines, but no utilization: NVML's running-process lists.
constexpr ProcessColumnAvailability::GpuSupport NO_PER_PROCESS_GPU_UTILIZATION{.perProcess = true, .utilization = false};

/// A generation's published support (Domain::ProcessModel::GpuSupport).
[[nodiscard]] constexpr Domain::ProcessModel::GpuSupport generation(bool perProcess, bool utilization, bool readFailed = false)
{
    return {.perProcess = perProcess,
            .utilization = utilization,
            .npuPerProcess = perProcess,
            .npuUtilization = utilization,
            .readFailed = readFailed};
}

TEST(ProcessColumnAvailabilityTest, GpuSupportComesFromTheGpuModelsFlags)
{
    using ProcessColumnAvailability::gpuSupport;
    using Known = ProcessColumnAvailability::KnownUnsupported;
    EXPECT_EQ(gpuSupport(/*hasGpuModel=*/true, Known{}), (ProcessColumnAvailability::GpuSupport{.perProcess = true, .utilization = true}));
    EXPECT_EQ(gpuSupport(true, Known{.utilization = true, .npuUtilization = true}),
              (ProcessColumnAvailability::GpuSupport{.perProcess = true, .utilization = false, .npuUtilization = false}));
    // No metrics means no utilization either.
    EXPECT_EQ(gpuSupport(true, Known{.perProcess = true, .npuPerProcess = true}), NO_PER_PROCESS_GPU);
    EXPECT_EQ(gpuSupport(false, Known{}), NO_PER_PROCESS_GPU); // No GPU model at all
}

TEST(ProcessColumnAvailabilityTest, CellsFollowTheSupportTheirGenerationWasReadUnder)
{
    // #1210: the table formats a generation's GPU cells by the support published with it.
    using ProcessColumnAvailability::gpuSupportOfGeneration;
    EXPECT_EQ(gpuSupportOfGeneration(generation(true, true)),
              (ProcessColumnAvailability::GpuSupport{.perProcess = true, .utilization = true}));
    EXPECT_EQ(gpuSupportOfGeneration(generation(true, false)),
              (ProcessColumnAvailability::GpuSupport{.perProcess = true, .utilization = false, .npuUtilization = false}));
    EXPECT_EQ(gpuSupportOfGeneration(generation(false, true)), NO_PER_PROCESS_GPU); // No data means no utilization
    EXPECT_EQ(gpuSupportOfGeneration(generation(false, false)), NO_PER_PROCESS_GPU);

    // A failed read keeps the support and is passed on to the row formatter (#1210).
    const auto failed = gpuSupportOfGeneration(generation(true, true, /*readFailed=*/true));
    EXPECT_TRUE(failed.perProcess);
    EXPECT_TRUE(failed.readFailed);
    EXPECT_TRUE(rowFormatOptions(linuxWithoutRaplCapabilities(), failed).gpuReadFailed);
    EXPECT_TRUE(rowFormatOptions(linuxWithoutRaplCapabilities(), failed).hasPerProcessGpu);
    EXPECT_FALSE(gpuSupportOfGeneration(generation(false, false, true)).readFailed); // No support: nothing to fail
    // NPU data alone (#1827): a failed read is still a failed read.
    EXPECT_TRUE(gpuSupportOfGeneration({.npuPerProcess = true, .readFailed = true}).readFailed);
}

// #1827: per-process GPU and NPU data are separate. Linux with an Intel NPU listed and DRM per-process
// data for the iGPU, but no NPU fdinfo read, showed the NPU columns as measured zeros; an NPU read
// alone must not switch on the GPU columns either (#1210).
TEST(ProcessColumnAvailabilityTest, GpuAndNpuColumnsFollowTheirOwnPerProcessSupport)
{
    const Platform::ProcessCapabilities caps = linuxWithoutRaplCapabilities();
    constexpr ProcessColumnAvailability::GpuSupport GPU_ONLY{.npuPerProcess = false, .npuUtilization = false};
    constexpr ProcessColumnAvailability::GpuSupport NPU_ONLY{.perProcess = false, .utilization = false, .npuUtilization = false};
    for (const ProcessColumn col : {ProcessColumn::NpuPercent, ProcessColumn::NpuMemory, ProcessColumn::NpuEngine})
    {
        EXPECT_FALSE(isSupported(col, caps, GPU_ONLY)) << getColumnInfo(col).configKey;
    }
    EXPECT_TRUE(isSupported(ProcessColumn::GpuMemory, caps, GPU_ONLY));
    EXPECT_FALSE(rowFormatOptions(caps, GPU_ONLY).hasPerProcessNpu);

    // NPU memory without NPU utilization (an fdinfo read): NPU Memory and Engine, not NPU %.
    EXPECT_TRUE(isSupported(ProcessColumn::NpuMemory, caps, NPU_ONLY));
    EXPECT_TRUE(isSupported(ProcessColumn::NpuEngine, caps, NPU_ONLY));
    EXPECT_FALSE(isSupported(ProcessColumn::NpuPercent, caps, NPU_ONLY));
    for (const ProcessColumn col :
         {ProcessColumn::GpuPercent, ProcessColumn::GpuMemory, ProcessColumn::GpuEngine, ProcessColumn::GpuDevice})
    {
        EXPECT_FALSE(isSupported(col, caps, NPU_ONLY)) << getColumnInfo(col).configKey;
    }
    const ProcessRowFormat::RowFormatOptions npuOnly = rowFormatOptions(caps, NPU_ONLY);
    EXPECT_TRUE(npuOnly.hasPerProcessNpu);
    EXPECT_FALSE(npuOnly.hasPerProcessNpuUtilization);
    EXPECT_FALSE(npuOnly.hasPerProcessGpu);
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

// #1799: the NPU columns need an NPU or compute accelerator as well as per-process NPU data, and NPU %
// needs per-process NPU utilization too. Without them they are "not available on this system", as the
// GPU columns are without per-process data.
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
    constexpr ProcessColumnAvailability::GpuSupport NO_NPU_UTILIZATION{.npuUtilization = false};
    EXPECT_FALSE(isSupported(ProcessColumn::NpuPercent, caps, NO_NPU_UTILIZATION));
    EXPECT_TRUE(isSupported(ProcessColumn::NpuMemory, caps, NO_NPU_UTILIZATION));
    EXPECT_TRUE(isSupported(ProcessColumn::NpuEngine, caps, NO_NPU_UTILIZATION));

    // The GPU model's word on an accelerator reaches the support and the row formatter.
    EXPECT_FALSE(gpuSupport(true, {}, /*hasAccelerator=*/false).accelerator);
    EXPECT_TRUE(gpuSupport(true, {}, /*hasAccelerator=*/true).accelerator);
    EXPECT_FALSE(rowFormatOptions(caps, NO_ACCELERATOR).hasPerProcessNpu);
    EXPECT_TRUE(rowFormatOptions(caps, NO_ACCELERATOR).hasPerProcessGpu);
    EXPECT_FALSE(rowFormatOptions(caps, NO_PER_PROCESS_GPU).hasPerProcessNpu);
    EXPECT_TRUE(rowFormatOptions(caps).hasPerProcessNpu);
    EXPECT_FALSE(ProcessColumnAvailability::gpuSupportOfGeneration(generation(true, true), /*accelerator=*/false).accelerator);
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
    EXPECT_FALSE(defaultColumns(caps, {.perProcess = true, .npuUtilization = false, .accelerator = true, .acceleratorListed = true})
                     .isVisible(ProcessColumn::NpuPercent));

    // The GPU model's word: listed only once known, and never when it lists none.
    EXPECT_TRUE(gpuSupport(true, {}, /*hasAccelerator=*/true, /*acceleratorListed=*/true).acceleratorListed);
    EXPECT_FALSE(gpuSupport(true, {}, /*hasAccelerator=*/true).acceleratorListed);
    EXPECT_FALSE(gpuSupport(true, {}, /*hasAccelerator=*/false, /*acceleratorListed=*/true).acceleratorListed);
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
    EXPECT_TRUE(options.hasSharedMemory);
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
    EXPECT_TRUE(options.hasArchitecture);
    EXPECT_FALSE(rowFormatOptions(linuxWithoutRaplCapabilities()).hasPublisher);
    EXPECT_TRUE(rowFormatOptions(linuxWithoutRaplCapabilities()).hasContainer);

    // And every column the options gate agrees with isSupported().
    const Platform::ProcessCapabilities none{};
    const ProcessRowFormat::RowFormatOptions noneOptions = rowFormatOptions(none);
    EXPECT_EQ(noneOptions.hasIoCounters, isSupported(ProcessColumn::IoRead, none));
    EXPECT_EQ(noneOptions.hasGdiObjects, isSupported(ProcessColumn::GdiObjects, none));
    EXPECT_EQ(noneOptions.hasSharedMemory, isSupported(ProcessColumn::Shared, none));
    EXPECT_EQ(noneOptions.hasPrivateBytes, isSupported(ProcessColumn::Private, none));
    EXPECT_EQ(noneOptions.hasSwapBytes, isSupported(ProcessColumn::Swap, none));
}

} // namespace
} // namespace App
