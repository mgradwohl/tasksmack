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

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

namespace Platform
{

std::filesystem::path WindowsPathProvider::getExecutableDir() const
{
    // Use wide string API and let filesystem handle conversion.
    // Handle paths that may exceed MAX_PATH by growing the buffer as needed.
    constexpr DWORD kInitialSize = MAX_PATH;
    constexpr DWORD kMaxLongPath = 32767; // Windows long-path limit for Unicode paths

    std::wstring buffer(kInitialSize, L'\0');

    for (;;)
    {
        const DWORD bufferSize = static_cast<DWORD>(buffer.size());
        const DWORD len = GetModuleFileNameW(nullptr, buffer.data(), bufferSize);

        if (len == 0)
        {
            // Failure; break to fallback.
            break;
        }

        if (len < bufferSize)
        {
            // Successfully retrieved full path (len does not include terminating null).
            buffer.resize(len);
            return std::filesystem::path(buffer).parent_path();
        }

        // len >= bufferSize: the buffer may be too small and the path truncated.
        if (bufferSize >= kMaxLongPath)
        {
            // Already at or above the long-path limit; cannot grow further safely.
            break;
        }

        const DWORD newSize = std::min(bufferSize * 2, kMaxLongPath);

        buffer.assign(static_cast<std::size_t>(newSize), L'\0');
    }

    // Fallback to current directory if GetModuleFileName fails
    return std::filesystem::current_path();
}

std::filesystem::path WindowsPathProvider::getUserConfigDir() const
{
    // Use _dupenv_s (secure version) to get APPDATA
    char* appData = nullptr;
    if (_dupenv_s(&appData, nullptr, "APPDATA") == 0 && appData != nullptr)
    {
        const std::unique_ptr<char, decltype(&std::free)> holder(appData, &std::free);
        if (appData[0] != '\0')
        {
            return std::filesystem::path(appData) / "TaskSmack";
        }
    }

    // Fallback to current directory if APPDATA not found
    return std::filesystem::current_path();
}

} // namespace Platform
