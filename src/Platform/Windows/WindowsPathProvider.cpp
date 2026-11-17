#include "WindowsPathProvider.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h> // SHGetKnownFolderPath
// clang-format on

#include "WindowsPathProviderMath.h"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <system_error>

namespace Platform
{

std::filesystem::path WindowsPathProvider::getExecutableDir() const
{
    return resolveExecutableDir([](wchar_t* buffer, std::uint32_t size) -> std::uint32_t
                                { return GetModuleFileNameW(nullptr, buffer, size); },
                                [](std::error_code& ec) { return std::filesystem::current_path(ec); });
}

WindowsPathProvider::WindowsPathProvider() : m_KnownFolderPath(&::SHGetKnownFolderPath)
{}

WindowsPathProvider::WindowsPathProvider(PathProviderKnownFolderFn knownFolderPath) : m_KnownFolderPath(knownFolderPath)
{}

std::filesystem::path WindowsPathProvider::getUserConfigDir() const
{
    return resolveUserConfigDir(
        [this] -> std::optional<std::filesystem::path>
        {
            // The known folder, not %APPDATA%: config.toml's directory comes from the same call (UserConfig.cpp), and a
            // user's own environment variable must not move the themes folder away from it, elevated or not (#4107).
            PWSTR roaming = nullptr;
            const HRESULT result = m_KnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming);
            // Freed whether or not the call succeeded, as SHGetKnownFolderPath's contract asks.
            const std::unique_ptr<wchar_t, decltype(&::CoTaskMemFree)> owned(roaming, &::CoTaskMemFree);
            if (SUCCEEDED(result) && owned != nullptr && owned.get()[0] != L'\0')
            {
                return std::filesystem::path(owned.get());
            }
            spdlog::warn("WindowsPathProvider: SHGetKnownFolderPath(RoamingAppData) failed (HRESULT 0x{:08X}); "
                         "using the current directory for the user config folder",
                         static_cast<std::uint32_t>(result));
            return std::nullopt;
        },
        [](std::error_code& ec) { return std::filesystem::current_path(ec); });
}

} // namespace Platform
