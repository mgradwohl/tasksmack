#include "WindowsPathProvider.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include "WindowsPathProviderMath.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>

namespace Platform
{

std::filesystem::path WindowsPathProvider::getExecutableDir() const
{
    return resolveExecutableDir([](wchar_t* buffer, std::uint32_t size) -> std::uint32_t
                                { return GetModuleFileNameW(nullptr, buffer, size); },
                                [](std::error_code& ec) { return std::filesystem::current_path(ec); });
}

std::filesystem::path WindowsPathProvider::getUserConfigDir() const
{
    return resolveUserConfigDir(
        []() -> std::optional<std::string>
        {
            // Use _dupenv_s (secure version) to get APPDATA
            char* appData = nullptr;
            if (_dupenv_s(&appData, nullptr, "APPDATA") == 0 && appData != nullptr)
            {
                const std::unique_ptr<char, decltype(&std::free)> holder(appData, &std::free);
                if (appData[0] != '\0')
                {
                    return std::string(appData);
                }
            }
            return std::nullopt;
        },
        [](std::error_code& ec) { return std::filesystem::current_path(ec); });
}

} // namespace Platform
