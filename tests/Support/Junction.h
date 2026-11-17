#pragma once

// A junction (a mount point) any user can make on a folder of their own, with no privilege (#4106): what a less-privileged
// side plants under an elevated TaskSmack's config folder. For tests in a temp folder of their own; test_PinnedFolder.cpp
// makes its mount points the same way.

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h> // FSCTL_SET_REPARSE_POINT
// clang-format on

#include "Platform/Windows/WindowsHandles.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
#include <string>

namespace TestSupport
{

/// A mount point's reparse data (ntifs.h's REPARSE_DATA_BUFFER, MountPointReparseBuffer), which user-mode headers
/// don't declare.
struct MountPointReparseData
{
    DWORD ReparseTag;
    WORD ReparseDataLength;
    WORD Reserved;
    WORD SubstituteNameOffset;
    WORD SubstituteNameLength;
    WORD PrintNameOffset;
    WORD PrintNameLength;
    std::array<wchar_t, MAX_PATH * 2> PathBuffer;
};

/// Opens folder @p link itself (not what it names) for writing its reparse point.
[[nodiscard]] inline Platform::Windows::UniqueHandle openForReparse(const std::filesystem::path& link)
{
    return Platform::Windows::UniqueHandle(CreateFileW(link.c_str(),
                                                       GENERIC_WRITE,
                                                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                                                       nullptr,
                                                       OPEN_EXISTING,
                                                       FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                                       nullptr));
}

/// Turns the empty folder @p link into a junction to folder @p target. Why not, when it can't.
[[nodiscard]] inline std::optional<std::string> makeJunction(const std::filesystem::path& link, const std::filesystem::path& target)
{
    const Platform::Windows::UniqueHandle folder = openForReparse(link);
    if (!folder)
    {
        return std::format("can't open {} to write: error {}", link.string(), GetLastError());
    }
    const std::wstring substitute = LR"(\??\)" + target.native();
    MountPointReparseData data{};
    if (substitute.size() + 2 > data.PathBuffer.size())
    {
        return std::string("the target's path is too long");
    }
    data.ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    data.SubstituteNameLength = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    data.PrintNameOffset = static_cast<WORD>(data.SubstituteNameLength + sizeof(wchar_t));
    std::ranges::copy(substitute, data.PathBuffer.begin());
    // The four name fields, the substitute name and its terminator, and an empty print name with its terminator.
    data.ReparseDataLength = static_cast<WORD>((4 * sizeof(WORD)) + data.SubstituteNameLength + (2 * sizeof(wchar_t)));
    const auto size = static_cast<DWORD>(offsetof(MountPointReparseData, SubstituteNameOffset) + data.ReparseDataLength);
    DWORD returned = 0;
    if (DeviceIoControl(folder.get(), FSCTL_SET_REPARSE_POINT, &data, size, nullptr, 0, &returned, nullptr) == FALSE)
    {
        return std::format("FSCTL_SET_REPARSE_POINT failed: error {}", GetLastError());
    }
    return std::nullopt;
}

/// Undoes makeJunction(): @p link is an empty folder again, so removing it removes nothing behind it. False when it
/// couldn't.
[[nodiscard]] inline bool removeJunction(const std::filesystem::path& link)
{
    const Platform::Windows::UniqueHandle folder = openForReparse(link);
    REPARSE_GUID_DATA_BUFFER header{};
    header.ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    DWORD returned = 0;
    return DeviceIoControl(
               folder.get(), FSCTL_DELETE_REPARSE_POINT, &header, REPARSE_GUID_DATA_BUFFER_HEADER_SIZE, nullptr, 0, &returned, nullptr) !=
           FALSE;
}

} // namespace TestSupport

#endif // _WIN32
