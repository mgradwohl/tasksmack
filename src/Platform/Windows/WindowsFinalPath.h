#pragma once

// The paths a Win32 call writes into a caller's buffer, read in one place (#4368): this process's own image
// (GetModuleFileNameW) and a handle's final path (GetFinalPathNameByHandleW), which the elevated code reads to tell
// which file or folder a handle really holds.
//   - readGrowingPath(): a MAX_PATH buffer first, then the longest path's.
//   - moduleImagePath(): this process's image through an injected GetModuleFileNameW, by readGrowingPath().
//   - readFinalPath(): a handle's final path, with the buffer sizing (FinalPathBuffer) and the \\?\ prefix
//     (FinalPathPrefix) each caller asks for. Callers differ in both, and each keeps its own.
// Header-only, and with nothing of the shell or COM, so TaskSmackHelper.exe and the app share it as it is.

#ifdef _WIN32
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
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Platform::Windows
{

/// The longest path the wide Win32 calls return, in characters, with its terminator (an extended-length path).
inline constexpr DWORD LONGEST_PATH = 32'768;

/// A path a Win32 call writes into a buffer of the size it is given, @p read(buffer, size) returning the characters
/// written (0 on failure, or @p size or more when the buffer was too small): MAX_PATH first, then LONGEST_PATH.
/// nullopt when it fails or doesn't fit either.
template<typename Read> [[nodiscard]] std::optional<std::wstring> readGrowingPath(Read read)
{
    for (const DWORD size : {DWORD{MAX_PATH}, LONGEST_PATH})
    {
        std::wstring path(size, L'\0');
        const DWORD written = read(path.data(), size);
        if (written == 0)
        {
            return std::nullopt;
        }
        if (written < size)
        {
            path.resize(written);
            return path;
        }
    }
    return std::nullopt;
}

/// GetModuleFileNameW, or a test's stand-in.
using ModuleFileNameFn = DWORD(WINAPI*)(HMODULE module, LPWSTR path, DWORD size);

/// This process's own image, as a Win32 path, read through @p getModuleFileName by readGrowingPath(): a path that
/// doesn't fit comes back truncated, with the size tried, so the longest is tried next. nullopt when it fails, with
/// GetLastError() saying why.
[[nodiscard]] inline std::optional<std::wstring> moduleImagePath(ModuleFileNameFn getModuleFileName)
{
    return readGrowingPath([getModuleFileName](wchar_t* buffer, DWORD size) { return getModuleFileName(nullptr, buffer, size); });
}

/// GetFinalPathNameByHandleW, or a test's stand-in.
using FinalPathNameFn = DWORD(WINAPI*)(HANDLE handle, LPWSTR path, DWORD size, DWORD flags);

/// What readFinalPath() does with the \\?\ prefix GetFinalPathNameByHandleW gives a VOLUME_NAME_DOS path.
enum class FinalPathPrefix : std::uint8_t
{
    Kept,       ///< As Windows gives it: "\\?\C:\dir", "\\?\UNC\server\share"
    KeptForUnc, ///< "C:\dir" for "\\?\C:\dir", but "\\?\UNC\server\share" kept
    Removed,    ///< "C:\dir" for "\\?\C:\dir", and "\\server\share" for "\\?\UNC\server\share"
};

/// @p path, a final path as GetFinalPathNameByHandleW gives it, with its \\?\ prefix as @p prefix says. The prefixes
/// are matched exactly, case included, as Windows writes them.
[[nodiscard]] inline std::wstring withFinalPathPrefix(std::wstring path, FinalPathPrefix prefix)
{
    constexpr std::wstring_view EXTENDED = LR"(\\?\)";
    constexpr std::wstring_view UNC = LR"(\\?\UNC\)";
    switch (prefix)
    {
    case FinalPathPrefix::Kept:
        break;
    case FinalPathPrefix::KeptForUnc:
        if (path.starts_with(EXTENDED) && !path.starts_with(UNC))
        {
            path.erase(0, EXTENDED.size());
        }
        break;
    case FinalPathPrefix::Removed:
        if (path.starts_with(UNC))
        {
            return LR"(\\)" + path.substr(UNC.size());
        }
        if (path.starts_with(EXTENDED))
        {
            return path.substr(EXTENDED.size());
        }
        break;
    }
    return path;
}

/// How readFinalPath() sizes its buffer.
enum class FinalPathBuffer : std::uint8_t
{
    /// MAX_PATH, then once more at the size the first call answered it needs (its terminator included); a path that
    /// still doesn't fit is no path.
    AsAnswered,
    /// MAX_PATH, then LONGEST_PATH (readGrowingPath()).
    Longest,
};

/// The final path of what @p handle holds, read through @p getFinalPath with @p flags (FILE_NAME_NORMALIZED |
/// VOLUME_NAME_DOS, say), its buffer sized as @p buffer says and its \\?\ prefix as @p prefix says. nullopt when it
/// can't be read or doesn't fit.
[[nodiscard]] inline std::optional<std::wstring>
readFinalPath(HANDLE handle, FinalPathNameFn getFinalPath, DWORD flags, FinalPathBuffer buffer, FinalPathPrefix prefix)
{
    if (buffer == FinalPathBuffer::Longest)
    {
        auto path =
            readGrowingPath([handle, getFinalPath, flags](wchar_t* text, DWORD size) { return getFinalPath(handle, text, size, flags); });
        if (!path.has_value())
        {
            return std::nullopt;
        }
        return withFinalPathPrefix(std::move(*path), prefix);
    }
    std::wstring path(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        const DWORD length = getFinalPath(handle, path.data(), static_cast<DWORD>(path.size()), flags);
        if (length == 0)
        {
            return std::nullopt;
        }
        if (length < path.size())
        {
            path.resize(length);
            return withFinalPathPrefix(std::move(path), prefix);
        }
        path.assign(length, L'\0'); // too short: length is the size it needs, with its terminator
    }
    return std::nullopt;
}

} // namespace Platform::Windows

#endif // _WIN32
