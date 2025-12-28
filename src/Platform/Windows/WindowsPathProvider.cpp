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

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

namespace Platform
{

std::filesystem::path WindowsPathProvider::getExecutableDir() const
{
    // Use wide string API and let filesystem handle conversion
    std::wstring buffer(MAX_PATH, L'\0');
    const DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (len > 0 && len < buffer.size())
    {
        buffer.resize(len);
        return std::filesystem::path(buffer).parent_path();
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
        std::unique_ptr<char, decltype(&std::free)> holder(appData, &std::free);
        if (appData[0] != '\0')
        {
            return std::filesystem::path(appData) / "TaskSmack";
        }
    }

    // Fallback to current directory if APPDATA not found
    return std::filesystem::current_path();
}

} // namespace Platform
