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
        [] -> std::optional<std::filesystem::path>
        {
            // _wdupenv_s, not _dupenv_s: the narrow value is in the active code page, which can't
            // hold a user name outside it in a binary without the UTF-8 manifest (#1648).
            wchar_t* appData = nullptr;
            if (_wdupenv_s(&appData, nullptr, L"APPDATA") == 0 && appData != nullptr)
            {
                const std::unique_ptr<wchar_t, decltype(&std::free)> holder(appData, &std::free);
                if (appData[0] != L'\0')
                {
                    return std::filesystem::path(appData);
                }
            }
            return std::nullopt;
        },
        [](std::error_code& ec) { return std::filesystem::current_path(ec); });
}

} // namespace Platform
