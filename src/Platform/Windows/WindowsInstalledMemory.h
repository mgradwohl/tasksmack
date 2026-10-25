#pragma once

// Installed against usable memory for the Memory modules section (#1515): the RAM the firmware says is
// installed (GetPhysicallyInstalledSystemMemory, from the SMBIOS table) and the RAM Windows can use
// (GlobalMemoryStatusEx's total); the difference is hardware-reserved. The calls go through an
// injectable table so tests can drive the failures without the hardware.

#include "Platform/ISystemInfoProbe.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include <cstdint>

namespace Platform
{

/// The kernel32 calls readInstalledMemory() makes. Defaults to the real exports; tests substitute fakes.
struct InstalledMemoryFunctions
{
    decltype(&GetPhysicallyInstalledSystemMemory) getPhysicallyInstalledSystemMemory = &GetPhysicallyInstalledSystemMemory;
    decltype(&GlobalMemoryStatusEx) globalMemoryStatusEx = &GlobalMemoryStatusEx;
};

/// Fills @p info's installedBytes and usableBytes; a call that fails leaves its value 0.
inline void readInstalledMemory(MemoryModulesInfo& info, const InstalledMemoryFunctions& fns = {}) noexcept
{
    constexpr std::uint64_t KIB = 1024;
    ULONGLONG installedKib = 0;
    if (fns.getPhysicallyInstalledSystemMemory(&installedKib) != FALSE)
    {
        info.installedBytes = static_cast<std::uint64_t>(installedKib) * KIB;
    }
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (fns.globalMemoryStatusEx(&status) != FALSE)
    {
        info.usableBytes = status.ullTotalPhys;
    }
}

} // namespace Platform
