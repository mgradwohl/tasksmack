#pragma once

// The Boot performance section's arithmetic (#1525): systemd's Manager timestamps turned into the
// phases `systemd-analyze` prints. Pure, so it is unit-tested on any host; the timestamps are read
// over D-Bus by SystemdBus.cpp.

#include "Platform/ISystemInfoProbe.h"

#include <cstdint>
#include <optional>

namespace Platform::LinuxBootTimes
{

/// The systemd Manager properties the phases come from, in microseconds of CLOCK_MONOTONIC. systemd
/// records the firmware and loader times as how long *before* the kernel started (from the boot
/// loader's LoaderTimeInitUSec/LoaderTimeExecUSec EFI variables), so they count back from 0; each is
/// 0 when it wasn't measured.
struct ManagerTimestamps
{
    std::uint64_t firmware = 0;  ///< FirmwareTimestampMonotonic
    std::uint64_t loader = 0;    ///< LoaderTimestampMonotonic
    std::uint64_t initrd = 0;    ///< InitRDTimestampMonotonic
    std::uint64_t userspace = 0; ///< UserspaceTimestampMonotonic
    std::uint64_t finish = 0;    ///< FinishTimestampMonotonic
};

/// The phases from @p timestamps into @p info, as systemd-analyze computes them: firmware = firmware -
/// loader, loader = loader, kernel = up to the initrd (or userspace without one), initrd = userspace -
/// initrd, userspace = finish - userspace. A phase whose timestamps are missing or out of order is left
/// out; the total is their sum, once startup has finished.
inline void computePhases(const ManagerTimestamps& timestamps, BootPerformanceInfo& info)
{
    info.finished = timestamps.finish != 0;
    if (timestamps.firmware != 0 && timestamps.firmware >= timestamps.loader)
    {
        info.firmwareUs = timestamps.firmware - timestamps.loader;
    }
    if (timestamps.loader != 0)
    {
        info.loaderUs = timestamps.loader;
    }
    const std::uint64_t kernelEnd = (timestamps.initrd != 0) ? timestamps.initrd : timestamps.userspace;
    if (kernelEnd != 0)
    {
        info.kernelUs = kernelEnd;
    }
    if (timestamps.initrd != 0 && timestamps.userspace >= timestamps.initrd)
    {
        info.initrdUs = timestamps.userspace - timestamps.initrd;
    }
    if (info.finished && timestamps.userspace != 0 && timestamps.finish >= timestamps.userspace)
    {
        info.userspaceUs = timestamps.finish - timestamps.userspace;
    }
    if (info.finished)
    {
        std::uint64_t total = 0;
        for (const std::optional<std::uint64_t>& phase : {info.firmwareUs, info.loaderUs, info.kernelUs, info.initrdUs, info.userspaceUs})
        {
            total += phase.value_or(0);
        }
        info.totalUs = total;
    }
}

} // namespace Platform::LinuxBootTimes
