#include "WindowsStartupImpact.h"

#include "WindowsElevation.h"
#include "WindowsHandles.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Platform::WindowsStartupImpact
{

namespace
{

/// %SystemRoot%\System32\wdi\LogFiles\StartupInfo\<SID>_StartupInfo<index>.xml.
[[nodiscard]] std::optional<std::wstring> startupInfoPath(int index, std::uint32_t& error)
{
    std::array<wchar_t, MAX_PATH> system{};
    const UINT length = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
    if (length == 0 || length >= system.size())
    {
        error = GetLastError();
        return std::nullopt;
    }
    // The current process's user SID as text ("S-1-5-21-...").
    const std::expected<std::wstring, DWORD> sid = Windows::currentUserSidText();
    if (!sid.has_value())
    {
        error = sid.error();
        return std::nullopt;
    }
    return std::format(L"{}\\wdi\\LogFiles\\StartupInfo\\{}_StartupInfo{}.xml", std::wstring_view(system.data(), length), *sid, index);
}

/// Reads one StartupInfo file whole. Opened for reading only, sharing everything, so Windows can go on
/// writing or replacing it.
[[nodiscard]] bool readFile(int index, std::vector<std::uint8_t>& bytes, std::uint32_t& error)
{
    const std::optional<std::wstring> path = startupInfoPath(index, error);
    if (!path.has_value())
    {
        return false;
    }
    const Windows::UniqueHandle file(CreateFileW(path->c_str(),
                                                 GENERIC_READ,
                                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                 nullptr,
                                                 OPEN_EXISTING,
                                                 FILE_ATTRIBUTE_NORMAL,
                                                 nullptr));
    if (!file)
    {
        error = GetLastError();
        return false;
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file.get(), &size) == FALSE)
    {
        error = GetLastError();
        return false;
    }
    if (size.QuadPart < 0 || std::cmp_greater(size.QuadPart, MAX_FILE_BYTES))
    {
        error = ERROR_FILE_TOO_LARGE;
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t total = 0;
    while (total < bytes.size())
    {
        DWORD read = 0;
        if (ReadFile(file.get(), bytes.data() + total, static_cast<DWORD>(bytes.size() - total), &read, nullptr) == FALSE)
        {
            error = GetLastError();
            return false;
        }
        if (read == 0)
        {
            break; // it shrank while being read: what was read is all there is
        }
        total += read;
    }
    bytes.resize(total);
    error = 0;
    return true;
}

} // namespace

Functions systemFunctions()
{
    return {.readFile = &readFile};
}

} // namespace Platform::WindowsStartupImpact
