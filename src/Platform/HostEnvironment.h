#pragma once

/// @file HostEnvironment.h
/// @brief Facts about the machine TaskSmack runs on that more than one layer needs, cheap enough to ask once at startup.

#ifndef _WIN32
#include <cstdlib>
#include <filesystem>
#include <system_error>
#endif

namespace Platform
{

/// Whether TaskSmack runs under WSL: its sessions have no polkit agent (#3380), and WSLg reports every display at
/// 100% whatever Windows uses (#3376). Always false on Windows. Reads the environment and one /proc path, so ask it
/// once (at construction or attach), not per frame.
[[nodiscard]] inline bool runningUnderWsl()
{
#ifdef _WIN32
    return false;
#else
    if (std::getenv("WSL_DISTRO_NAME") != nullptr) // NOLINT(concurrency-mt-unsafe) - read once, at startup
    {
        return true;
    }
    std::error_code error;
    return std::filesystem::exists("/proc/sys/fs/binfmt_misc/WSLInterop", error);
#endif
}

} // namespace Platform
