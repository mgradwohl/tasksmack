#pragma once

// A held file's version resource, read through its handle and never by a name (#4057, #4373): a read-only data
// mapping of the file, its PE resource directory walked to RT_VERSION by VersionResourceMath::findVersionResource()
// (every offset checked against the file), and the VS_VERSIONINFO block copied out. The Publisher column reads the
// CompanyName from it (Windows::heldFilePublisher()) and the Modules section the file version (heldFileVersion()).
// GetFileVersionInfoW isn't used for either: it opens the file by its name, so a drive letter or subst that points
// elsewhere by then, a language .mui beside the file, or a network path a process planted in its own loader data
// would supply the answer, or get a connection from TaskSmack (or its elevated helper) for it.
//
// Its own file, so TaskSmackHelper.exe, which reads modules for an administrator (#2309), builds it in without the
// image file and signature code around the publisher read.

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // NOLINT(misc-include-cleaner) - umbrella header; symbols reside in sub-headers
// clang-format on

#include "WindowsVersionResourceMath.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace Platform::Windows
{

// NOLINTBEGIN(misc-include-cleaner) - Win32 types come from windows.h sub-headers

/// The VS_VERSIONINFO block of a held file, copied out of it.
struct HeldVersionBlock
{
    /// The block, then zeroed room after it: VerQueryValueW is given twice the largest block's room, as
    /// GetFileVersionInfoSizeW asks for, since it may write after the block. Empty when the file has none.
    std::vector<BYTE> buffer;
    /// The block's own length (its wLength), at the start of buffer; 0 when the file has none.
    std::size_t length = 0;
};

/// The version info block of the file @p file is open on (GENERIC_READ), read through that handle (see the top of
/// this file). None when the file has none, isn't a PE file, is empty, or can't be mapped or read (an in-page error,
/// such as a network file that went away, is none rather than a crash).
[[nodiscard]] HeldVersionBlock heldVersionBlock(HANDLE file);

/// The file version (VS_FIXEDFILEINFO's dwFileVersionMS and dwFileVersionLS) of the file @p file is open on, read
/// through that handle by heldVersionBlock(); nullopt when it has none.
[[nodiscard]] std::optional<VersionResourceMath::FixedFileVersion> heldFileVersion(HANDLE file);

// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows

#endif // _WIN32
